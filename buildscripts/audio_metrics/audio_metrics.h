#ifndef AUDIO_METRICS_H
#define AUDIO_METRICS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    double bass;
    double mid;
    double treble;
    double volume;
    bool beat;
    uint64_t frame_count;
} mpv_audio_metrics_t;

void audio_metrics_init(int fft_size, int sample_rate);
void audio_metrics_feed(const void *samples, int frame_count, int channels, int af_format);
const mpv_audio_metrics_t *audio_metrics_get(void);
void audio_metrics_reset(void);
void audio_metrics_destroy(void);

#endif
