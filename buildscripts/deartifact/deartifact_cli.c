/* deartifact_cli.c —— 离线 A/B 验证工具（Phase 1）。
 *
 * 用法：deartifact_cli <in.wav> <out.wav> <off|low|high>
 * 输入：RIFF WAV，PCM 16-bit 或 IEEE float32，任意声道数（≤8）。
 * 输出：同格式 16-bit PCM WAV。
 *
 * 例（先 ffmpeg 解码 MP3）：
 *   ffmpeg -i sample.mp3 -ar 44100 -ac 2 in.wav
 *   deartifact_cli in.wav out_low.wav low
 *   deartifact_cli in.wav out_high.wav high
 *   deartifact_cli in.wav out_off.wav off   # bit-exact 验证
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "deartifact.h"

typedef struct {
    FILE *f;
    unsigned data_offset;
    unsigned data_bytes;
    int channels;
    int sample_rate;
    int bits;        /* 16 或 32(float) */
    int is_float;
} wav_t;

static unsigned rd32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int wav_open(wav_t *w, const char *path) {
    memset(w, 0, sizeof(*w));
    w->f = fopen(path, "rb");
    if (!w->f) return -1;
    unsigned char h[12];
    if (fread(h, 1, 12, w->f) != 12) return -1;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;

    unsigned char ch[8];
    int fmt_seen = 0;
    while (fread(ch, 1, 8, w->f) == 8) {
        unsigned size = rd32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            unsigned char f[40];
            unsigned want = size < 40 ? size : 40;
            if (fread(f, 1, want, w->f) != want) return -1;
            if (size > want) fseek(w->f, size - want, SEEK_CUR);
            unsigned fmt = f[0] | (f[1] << 8);
            w->channels = f[2] | (f[3] << 8);
            w->sample_rate = rd32(f + 4);
            w->bits = f[14] | (f[15] << 8);
            w->is_float = (fmt == 3);
            if (w->channels < 1 || w->channels > DEARTIFACT_MAX_CH) return -1;
            if (!(w->is_float && w->bits == 32) && w->bits != 16) return -1;
            fmt_seen = 1;
        } else if (!memcmp(ch, "data", 4)) {
            w->data_offset = (unsigned)ftell(w->f);
            w->data_bytes = size;
            if (fmt_seen) return 0;
            fseek(w->f, size, SEEK_CUR);
        } else {
            fseek(w->f, size + (size & 1), SEEK_CUR);
        }
    }
    return -1;
}

static int wav_write_header(FILE *f, int channels, int sample_rate, unsigned data_bytes) {
    unsigned byte_rate = sample_rate * channels * 2;
    unsigned char h[44] = {0};
    memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVE", 4);
    unsigned riff = 36 + data_bytes;
    h[4]=riff; h[5]=riff>>8; h[6]=riff>>16; h[7]=riff>>24;
    memcpy(h + 12, "fmt ", 4);
    h[16]=16; h[20]=1; /* PCM */
    h[22]=channels; h[23]=0;
    h[24]=sample_rate; h[25]=sample_rate>>8; h[26]=sample_rate>>16; h[27]=sample_rate>>24;
    h[28]=byte_rate; h[29]=byte_rate>>8; h[30]=byte_rate>>16; h[31]=byte_rate>>24;
    h[32]=channels*2; h[34]=16;
    memcpy(h + 36, "data", 4);
    h[40]=data_bytes; h[41]=data_bytes>>8; h[42]=data_bytes>>16; h[43]=data_bytes>>24;
    return fwrite(h, 1, 44, f) == 44 ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <in.wav> <out.wav> <off|low|high>\n", argv[0]);
        return 2;
    }
    int strength = DEARTIFACT_OFF;
    if (!strcmp(argv[3], "low")) strength = DEARTIFACT_LOW;
    else if (!strcmp(argv[3], "high")) strength = DEARTIFACT_HIGH;
    else if (strcmp(argv[3], "off")) { fprintf(stderr, "bad strength\n"); return 2; }

    wav_t w;
    if (wav_open(&w, argv[1])) {
        fprintf(stderr, "cannot read wav: %s\n", argv[1]);
        return 1;
    }
    fprintf(stderr, "input: %d Hz, %d ch, %d-bit %s, %u bytes data\n",
            w.sample_rate, w.channels, w.bits, w.is_float ? "float" : "pcm",
            w.data_bytes);

    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror("fopen out"); return 1; }
    if (wav_write_header(out, w.channels, w.sample_rate, 0)) return 1;

    void *ctx = deartifact_create(w.sample_rate, w.channels, strength);
    if (!ctx) { fprintf(stderr, "create failed\n"); return 1; }

    int bytes_per_frame = w.channels * w.bits / 8;
    long total_frames = (long)w.data_bytes / bytes_per_frame;
    unsigned char raw[4096];
    float pcm[2048], proc[2048];
    short out16[1024];
    long written = 0, in_count = 0;
    unsigned written_bytes = 0;

    fseek(w.f, (long)w.data_offset, SEEK_SET);
    int chunk_frames = 512;
    while (in_count < total_frames) {
        int want = chunk_frames;
        if (in_count + want > total_frames) want = (int)(total_frames - in_count);
        unsigned want_bytes = (unsigned)want * bytes_per_frame;
        if (fread(raw, 1, want_bytes, w.f) != want_bytes) break;
        /* 解交错为 float（WAV data 本身 interleaved，直接转标量即可） */
        for (int i = 0; i < want * w.channels; i++) {
            if (w.is_float) {
                unsigned char *p = raw + 4 * i;
                unsigned u = rd32(p);
                float fv; memcpy(&fv, &u, 4);
                pcm[i] = fv;
            } else {
                short s = (short)(raw[2 * i] | (raw[2 * i + 1] << 8));
                pcm[i] = s / 32768.0f;
            }
        }
        if (deartifact_process(ctx, pcm, proc, want)) {
            fprintf(stderr, "process error -> passthrough\n");
            memcpy(proc, pcm, (size_t)want * w.channels * sizeof(float));
        }
        for (int i = 0; i < want * w.channels; i++) {
            float v = proc[i];
            long q = lrintf(v * 32768.0f);
            if (q > 32767) q = 32767;
            if (q < -32768) q = -32768;
            out16[i] = (short)q;
        }
        if (fwrite(out16, 2, (size_t)want * w.channels, out) != (size_t)want * w.channels) {
            perror("fwrite"); return 1;
        }
        written += want;
        written_bytes += (unsigned)(want * w.channels * 2);
        in_count += want;
    }

    /* 冲刷尾部 FIFO */
    float tail[DEARTIFACT_FRAME * DEARTIFACT_MAX_CH];
    int n = deartifact_flush(ctx, tail, DEARTIFACT_FRAME);
    for (int i = 0; i < n * w.channels; i++) {
        float v = tail[i];
        long q = lrintf(v * 32768.0f);
        if (q > 32767) q = 32767;
        if (q < -32768) q = -32768;
        out16[i % 1024] = (short)q; /* 分块写 */
        if (i % 1024 == 1023 || i == n * w.channels - 1) {
            int cnt = (i % 1024) + 1;
            fwrite(out16, 2, cnt, out);
            written_bytes += (unsigned)(cnt * 2);
        }
    }
    (void)written;

    /* 回填 data size */
    fseek(out, 0, SEEK_SET);
    wav_write_header(out, w.channels, w.sample_rate, written_bytes);

    fprintf(stderr, "done: %ld frames in, +%d flushed\n", in_count, n);
    {
        uint64_t st[3] = {0, 0, 0};
        deartifact_debug_stats(ctx, st);
        fprintf(stderr, "stats: %llu frames, artifact active %.1f%%, de-esser active %.1f%%\n",
                (unsigned long long)st[0],
                st[0] ? 100.0 * st[1] / st[0] : 0.0,
                st[0] ? 100.0 * st[2] / st[0] : 0.0);
    }
    deartifact_destroy(ctx);
    fclose(w.f);
    fclose(out);
    return 0;
}
