#ifndef AUDIO_METRICS_H
#define AUDIO_METRICS_H

#include <stdint.h>
#include <stdbool.h>

#define AUDIO_METRICS_SPECTRUM_BINS 256

typedef struct {
    double bass;
    double mid;
    double treble;
    double volume;
    int beat;
    float spectrum[AUDIO_METRICS_SPECTRUM_BINS];
    uint64_t frame_count;
} mpv_audio_metrics_t;

void audio_metrics_init(int fft_size, int sample_rate);
void audio_metrics_feed(const void *samples, int frame_count, int channels, int bytes_per_sample, int sample_rate);
const mpv_audio_metrics_t *audio_metrics_get(void);
void audio_metrics_reset(void);
void audio_metrics_destroy(void);

/* ---------------------------------------------------------------------------
 * PCM tap —— 旁路导出原始单声道 PCM（float，源采样率），供离线 ASR 使用。
 *
 * 与 FFT 指标共用 audio_metrics_feed 的下混结果：feed 每解析出一个 mono 源
 * 样本，除了喂 FFT 环形缓冲，再写入一个独立、更大的 tap 环形缓冲。消费端
 * （Dart 侧 isolate）按需轮询读出。
 *
 * 不在 C 层重采样：tap 输出保持源采样率，由 audio_pcm_tap_samplerate() 报告。
 * sherpa-onnx 的 acceptWaveform(samples, sampleRate) 会在内部重采样到 16k，
 * 因此 C 层无需引入重采样逻辑（更简单、更不易出错）。
 * ------------------------------------------------------------------------ */

/* 读出最多 max_samples 个单声道 float 样本到 out，返回实际写入的样本数。
 * 读出即消费（从环形缓冲移除）。out 由调用方分配。 */
int audio_pcm_tap_read(float *out, int max_samples);

/* 当前 tap 数据的源采样率（Hz）；尚无数据时返回 0。 */
int audio_pcm_tap_samplerate(void);

/* 丢弃当前缓冲的全部样本（seek / 切歌时调用，避免跨段串扰）。 */
void audio_pcm_tap_reset(void);

#endif
