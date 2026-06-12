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
} audio_metrics_t;

void audio_metrics_init(int fft_size, int sample_rate);
void audio_metrics_feed(const float *samples, int frame_count, int channels);
const audio_metrics_t *audio_metrics_get(void);
void audio_metrics_reset(void);
void audio_metrics_destroy(void);

#endif
