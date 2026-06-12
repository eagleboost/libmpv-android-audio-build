#include "audio_metrics.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static audio_metrics_t g_metrics = {0};
static float *g_fft_re = NULL;
static float *g_fft_im = NULL;
static float *g_window = NULL;
static float *g_ring = NULL;
static int g_fft_size = 0;
static int g_sample_rate = 0;
static int g_ring_pos = 0;
static int g_ring_filled = 0;
static bool g_initialized = false;

#define BEAT_HISTORY 43
static double g_beat_history[BEAT_HISTORY];
static int g_beat_pos = 0;
static int g_beat_count = 0;

static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
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
                float tRe = curRe * re[i + j + len / 2] - curIm * im[i + j + len / 2];
                float tIm = curRe * im[i + j + len / 2] + curIm * re[i + j + len / 2];
                re[i + j + len / 2] = re[i + j] - tRe;
                im[i + j + len / 2] = im[i + j] - tIm;
                re[i + j] += tRe;
                im[i + j] += tIm;
                float newRe = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = newRe;
            }
        }
    }
}

static void compute_metrics(void) {
    if (!g_initialized) return;

    int n = g_fft_size;

    for (int i = 0; i < n; i++) {
        int idx = (g_ring_pos - n + i + g_ring_filled > n ? n : g_ring_filled) + i;
        if (idx >= g_ring_filled) idx -= g_ring_filled;
        idx = (g_ring_pos - n + i + n * 2) % (n * 2);
        float s = g_ring[idx] * g_window[i];
        g_fft_re[i] = s;
        g_fft_im[i] = 0.0f;
    }

    fft(g_fft_re, g_fft_im, n);

    double sum = 0;
    for (int i = 0; i < n; i++)
        sum += g_fft_re[i] * g_fft_re[i] + g_fft_im[i] * g_fft_im[i];
    g_metrics.volume = sqrt(sum / (n * n));

    int halfN = n / 2;
    int bassEnd = (int)(halfN * 0.07);
    int midEnd = (int)(halfN * 0.4);

    double bassSum = 0, midSum = 0, trebSum = 0;
    for (int i = 1; i <= bassEnd && i < halfN; i++) {
        double mag = sqrt((double)(g_fft_re[i] * g_fft_re[i] + g_fft_im[i] * g_fft_im[i]));
        bassSum += mag;
    }
    for (int i = bassEnd + 1; i <= midEnd && i < halfN; i++) {
        double mag = sqrt((double)(g_fft_re[i] * g_fft_re[i] + g_fft_im[i] * g_fft_im[i]));
        midSum += mag;
    }
    for (int i = midEnd + 1; i < halfN; i++) {
        double mag = sqrt((double)(g_fft_re[i] * g_fft_re[i] + g_fft_im[i] * g_fft_im[i]));
        trebSum += mag;
    }

    g_metrics.bass = bassEnd > 0 ? (bassSum / bassEnd) / n : 0;
    g_metrics.mid = (midEnd > bassEnd) ? (midSum / (midEnd - bassEnd)) / n : 0;
    g_metrics.treble = (halfN > midEnd) ? (trebSum / (halfN - midEnd - 1)) / n : 0;

    double bassSmoothed = g_metrics.bass * 0.7 + g_metrics.bass * 0.3;

    g_beat_history[g_beat_pos] = bassSmoothed;
    g_beat_pos = (g_beat_pos + 1) % BEAT_HISTORY;
    if (g_beat_count < BEAT_HISTORY) g_beat_count++;

    double avg = 0;
    for (int i = 0; i < g_beat_count; i++) avg += g_beat_history[i];
    avg /= g_beat_count;

    g_metrics.beat = (g_beat_count >= 4) && (bassSmoothed > avg * 1.4) && (bassSmoothed > 0.05);
}

void audio_metrics_init(int fft_size, int sample_rate) {
    audio_metrics_destroy();
    g_fft_size = fft_size;
    g_sample_rate = sample_rate;
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

    for (int i = 0; i < fft_size; i++) {
        g_window[i] = (float)(0.5 * (1.0 - cos(2.0 * M_PI * i / (fft_size - 1))));
    }

    g_initialized = (g_fft_re && g_fft_im && g_window && g_ring);
}

void audio_metrics_feed(const float *samples, int frame_count, int channels) {
    if (!g_initialized) return;

    int capacity = g_fft_size * 2;

    for (int f = 0; f < frame_count; f++) {
        float mono = 0;
        for (int c = 0; c < channels; c++) {
            mono += samples[f * channels + c];
        }
        mono /= channels;

        g_ring[g_ring_pos] = mono;
        g_ring_pos = (g_ring_pos + 1) % capacity;
        if (g_ring_filled < capacity) g_ring_filled++;
    }

    g_metrics.frame_count += frame_count;

    if (g_ring_filled >= g_fft_size) {
        compute_metrics();
    }
}

const audio_metrics_t *audio_metrics_get(void) {
    return &g_metrics;
}

void audio_metrics_reset(void) {
    g_ring_pos = 0;
    g_ring_filled = 0;
    g_beat_pos = 0;
    g_beat_count = 0;
    g_metrics.frame_count = 0;
    g_metrics.beat = false;
    memset(g_beat_history, 0, sizeof(g_beat_history));
}

void audio_metrics_destroy(void) {
    free(g_fft_re); g_fft_re = NULL;
    free(g_fft_im); g_fft_im = NULL;
    free(g_window); g_window = NULL;
    free(g_ring); g_ring = NULL;
    g_fft_size = 0;
    g_initialized = false;
    memset(&g_metrics, 0, sizeof(g_metrics));
}
