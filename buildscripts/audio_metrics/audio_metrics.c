#include "audio_metrics.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static mpv_audio_metrics_t g_metrics = {0};
static float *g_fft_re = NULL;
static float *g_fft_im = NULL;
static float *g_window = NULL;
static float *g_ring = NULL;
static int g_fft_size = 0;
static int g_ring_pos = 0;
static int g_ring_filled = 0;
static bool g_initialized = false;

/* PCM tap 环形缓冲：约 4 秒 @ 48k，单声道 float = 768 KB。
 * 单生产者（mpv 音频线程）单消费者（Dart 轮询）。与 g_metrics 一样不加锁，
 * 偶发竞争最多多读/少读几个样本，对 ASR 无害。 */
#define PCM_TAP_CAP 192000
static float *g_tap = NULL;
static int g_tap_w = 0;     /* 写入位置 */
static int g_tap_count = 0; /* 当前可读样本数 */
static int g_tap_sr = 0;    /* 源采样率 */

#define BEAT_HISTORY 43
static double g_beat_history[BEAT_HISTORY];
static int g_beat_pos = 0;
static int g_beat_count = 0;

static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        while (j & bit) { j ^= bit; bit >>= 1; }
        j ^= bit;
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * M_PI / len;
        float wRe = (float)cos(ang);
        float wIm = (float)-sin(ang);
        for (int i = 0; i < n; i += len) {
            float curRe = 1.0f, curIm = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                float tRe = curRe * re[i+j+len/2] - curIm * im[i+j+len/2];
                float tIm = curRe * im[i+j+len/2] + curIm * re[i+j+len/2];
                re[i+j+len/2] = re[i+j] - tRe;
                im[i+j+len/2] = im[i+j] - tIm;
                re[i+j] += tRe;
                im[i+j] += tIm;
                float nr = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = nr;
            }
        }
    }
}

static void compute_metrics(void) {
    if (!g_initialized) return;
    int n = g_fft_size;
    int capacity = n * 2;

    for (int i = 0; i < n; i++) {
        int idx = (g_ring_pos - n + i + capacity) % capacity;
        g_fft_re[i] = g_ring[idx] * g_window[i];
        g_fft_im[i] = 0.0f;
    }

    fft(g_fft_re, g_fft_im, n);

    double sum = 0;
    for (int i = 0; i < n; i++)
        sum += g_fft_re[i] * g_fft_re[i] + g_fft_im[i] * g_fft_im[i];
    g_metrics.volume = sqrt(sum / ((double)n * n));

    int halfN = n / 2;
    int bassEnd = (int)(halfN * 0.07);
    int midEnd = (int)(halfN * 0.4);
    if (bassEnd < 1) bassEnd = 1;
    if (midEnd <= bassEnd) midEnd = bassEnd + 1;

    double bassSum = 0, midSum = 0, trebSum = 0;
    for (int i = 1; i <= bassEnd && i < halfN; i++) {
        bassSum += sqrt((double)(g_fft_re[i]*g_fft_re[i] + g_fft_im[i]*g_fft_im[i]));
    }
    for (int i = bassEnd+1; i <= midEnd && i < halfN; i++) {
        midSum += sqrt((double)(g_fft_re[i]*g_fft_re[i] + g_fft_im[i]*g_fft_im[i]));
    }
    for (int i = midEnd+1; i < halfN; i++) {
        trebSum += sqrt((double)(g_fft_re[i]*g_fft_re[i] + g_fft_im[i]*g_fft_im[i]));
    }

    g_metrics.bass = bassEnd > 0 ? (bassSum / bassEnd) / n : 0;
    g_metrics.mid = midEnd > bassEnd ? (midSum / (midEnd - bassEnd)) / n : 0;
    g_metrics.treble = halfN > midEnd+1 ? (trebSum / (halfN - midEnd - 1)) / n : 0;

    /* Downsample halfN magnitude bins into AUDIO_METRICS_SPECTRUM_BINS linear
       buckets. Each bucket = average magnitude of its source bins, normalized
       by n (same scale as bass/mid/treble). */
    for (int i = 0; i < AUDIO_METRICS_SPECTRUM_BINS; i++) {
        int lo = (int)((long)halfN * i / AUDIO_METRICS_SPECTRUM_BINS);
        int hi = (int)((long)halfN * (i + 1) / AUDIO_METRICS_SPECTRUM_BINS);
        if (hi <= lo) hi = lo + 1;
        if (hi > halfN) hi = halfN;
        double mag = 0.0;
        int cnt = 0;
        for (int k = lo; k < hi; k++) {
            mag += sqrt((double)(g_fft_re[k] * g_fft_re[k] + g_fft_im[k] * g_fft_im[k]));
            cnt++;
        }
        g_metrics.spectrum[i] = cnt > 0 ? (float)(mag / cnt) / n : 0.0f;
    }

    double bassVal = g_metrics.bass;
    g_beat_history[g_beat_pos] = bassVal;
    g_beat_pos = (g_beat_pos + 1) % BEAT_HISTORY;
    if (g_beat_count < BEAT_HISTORY) g_beat_count++;

    double avg = 0;
    for (int i = 0; i < g_beat_count; i++) avg += g_beat_history[i];
    avg /= g_beat_count;
    g_metrics.beat = (g_beat_count >= 4) && (bassVal > avg * 1.4) && (bassVal > 0.05);
}

void audio_metrics_init(int fft_size, int sample_rate) {
    audio_metrics_destroy();
    g_fft_size = fft_size;
    g_ring_pos = 0;
    g_ring_filled = 0;
    g_beat_pos = 0;
    g_beat_count = 0;
    memset(g_beat_history, 0, sizeof(g_beat_history));
    memset(&g_metrics, 0, sizeof(g_metrics));
    g_fft_re = (float *)calloc(fft_size, sizeof(float));
    g_fft_im = (float *)calloc(fft_size, sizeof(float));
    g_window = (float *)calloc(fft_size, sizeof(float));
    g_ring = (float *)calloc(fft_size * 2, sizeof(float));
    for (int i = 0; i < fft_size; i++)
        g_window[i] = (float)(0.5 * (1.0 - cos(2.0 * M_PI * i / (fft_size - 1))));
    g_initialized = (g_fft_re && g_fft_im && g_window && g_ring);

    g_tap = (float *)calloc(PCM_TAP_CAP, sizeof(float));
    g_tap_w = 0;
    g_tap_count = 0;
    g_tap_sr = sample_rate;
}

static void feed_mono(float mono) {
    if (!g_initialized) return;
    int capacity = g_fft_size * 2;
    g_ring[g_ring_pos] = mono;
    g_ring_pos = (g_ring_pos + 1) % capacity;
    if (g_ring_filled < capacity) g_ring_filled++;
}

static void tap_write(float mono) {
    if (!g_tap) return;
    g_tap[g_tap_w] = mono;
    g_tap_w = (g_tap_w + 1) % PCM_TAP_CAP;
    /* 满了则隐式覆盖最旧样本（count 维持上限，读起点随 g_tap_w 前移）。 */
    if (g_tap_count < PCM_TAP_CAP) g_tap_count++;
}

void audio_metrics_feed(const void *samples, int frame_count, int channels, int bytes_per_sample, int sample_rate) {
    if (!g_initialized) return;
    g_tap_sr = sample_rate;
    for (int f = 0; f < frame_count; f++) {
        float mono = 0;
        switch (bytes_per_sample) {
        case 4: {
            const float *p = (const float *)samples;
            for (int c = 0; c < channels; c++) mono += p[f * channels + c];
            mono /= channels;
            break;
        }
        case 2: {
            const int16_t *p = (const int16_t *)samples;
            for (int c = 0; c < channels; c++) mono += p[f * channels + c];
            mono = (mono / channels) / 32768.0f;
            break;
        }
        case 3: {
            const uint8_t *p = (const uint8_t *)samples;
            for (int c = 0; c < channels; c++) {
                int32_t val = (int32_t)(p[(f * channels + c) * 3] |
                    (p[(f * channels + c) * 3 + 1] << 8) |
                    (p[(f * channels + c) * 3 + 2] << 16));
                if (val & 0x800000) val |= 0xFF000000;
                mono += (float)val / 8388608.0f;
            }
            mono /= channels;
            break;
        }
        case 1: {
            const uint8_t *p = (const uint8_t *)samples;
            for (int c = 0; c < channels; c++) mono += (float)(p[f * channels + c] - 128);
            mono = (mono / channels) / 128.0f;
            break;
        }
        default:
            return;
        }
        feed_mono(mono);
        tap_write(mono);
    }
    g_metrics.frame_count += frame_count;
    if (g_ring_filled >= g_fft_size) compute_metrics();
}

const mpv_audio_metrics_t *audio_metrics_get(void) { return &g_metrics; }

void audio_metrics_reset(void) {
    g_ring_pos = 0;
    g_ring_filled = 0;
    g_beat_pos = 0;
    g_beat_count = 0;
    g_metrics.frame_count = 0;
    g_metrics.beat = false;
    memset(g_beat_history, 0, sizeof(g_beat_history));
    memset(g_metrics.spectrum, 0, sizeof(g_metrics.spectrum));
    g_tap_w = 0;
    g_tap_count = 0;
}

void audio_metrics_destroy(void) {
    free(g_fft_re); g_fft_re = NULL;
    free(g_fft_im); g_fft_im = NULL;
    free(g_window); g_window = NULL;
    free(g_ring); g_ring = NULL;
    free(g_tap); g_tap = NULL;
    g_tap_w = 0;
    g_tap_count = 0;
    g_tap_sr = 0;
    g_fft_size = 0;
    g_initialized = false;
    memset(&g_metrics, 0, sizeof(g_metrics));
}

int audio_pcm_tap_read(float *out, int max_samples) {
    if (!g_tap || !out || max_samples <= 0) return 0;
    int count = g_tap_count;            /* 快照，降低与生产者的竞争窗口 */
    int n = count < max_samples ? count : max_samples;
    int start = (g_tap_w - count + PCM_TAP_CAP) % PCM_TAP_CAP;
    for (int i = 0; i < n; i++) {
        out[i] = g_tap[(start + i) % PCM_TAP_CAP];
    }
    g_tap_count -= n;                   /* 消费已读样本 */
    return n;
}

int audio_pcm_tap_samplerate(void) {
    return g_tap_sr;
}

void audio_pcm_tap_reset(void) {
    g_tap_w = 0;
    g_tap_count = 0;
}
