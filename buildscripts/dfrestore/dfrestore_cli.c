/* dfrestore_cli.c —— Windows 本机验证：WAV 过 dfrestore DLL，输出 WAV。
 * 与 Python deepFilter（DFN3 + atten-lim 12 --pf）的结果做一致性对比。
 * 用法：dfrestore_cli <in_48k.wav> <out.wav> [atten_db] [pf_beta]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef void *dfrestore_t;

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.wav out.wav [atten_db] [pf_beta]\n", argv[0]); return 2; }
    float atten = argc > 3 ? (float)atof(argv[3]) : 12.0f;
    float beta = argc > 4 ? (float)atof(argv[4]) : 0.05f;

#if defined(_WIN32)
    HMODULE lib = LoadLibraryA("dfrestore.dll");
    if (!lib) { fprintf(stderr, "cannot load dfrestore.dll\n"); return 1; }
    dfrestore_t (*init)(int, float, float) = (dfrestore_t (*)(int, float, float))(void *)GetProcAddress(lib, "dfrestore_init");
    int (*proc)(dfrestore_t, float *, int) = (int (*)(dfrestore_t, float *, int))(void *)GetProcAddress(lib, "dfrestore_process");
    void (*free_fn)(dfrestore_t) = (void (*)(dfrestore_t))(void *)GetProcAddress(lib, "dfrestore_free");
#else
    void *lib = dlopen("libdfrestore.so", RTLD_NOW);
    if (!lib) { fprintf(stderr, "cannot load libdfrestore.so\n"); return 1; }
    dfrestore_t (*init)(int, float, float) = (dfrestore_t (*)(int, float, float))dlsym(lib, "dfrestore_init");
    int (*proc)(dfrestore_t, float *, int) = (int (*)(dfrestore_t, float *, int))dlsym(lib, "dfrestore_process");
    void (*free_fn)(dfrestore_t) = (void (*)(dfrestore_t))dlsym(lib, "dfrestore_free");
#endif
    if (!init || !proc || !free_fn) { fprintf(stderr, "missing exports\n"); return 1; }

    /* 极简 WAV 解析（16-bit PCM）*/
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("in.wav"); return 1; }
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12) return 1;
    int channels = 0, sample_rate = 0, bits = 0;
    unsigned data_off = 0, data_bytes = 0;
    unsigned char ch[8];
    while (fread(ch, 1, 8, f) == 8) {
        unsigned size = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((unsigned)ch[7] << 24);
        if (!memcmp(ch, "fmt ", 4)) {
            unsigned char fm[16];
            if (fread(fm, 1, 16, f) != 16) return 1;
            channels = fm[2] | (fm[3] << 8);
            sample_rate = fm[4] | (fm[5] << 8) | (fm[6] << 16) | (fm[7] << 24);
            bits = fm[14] | (fm[15] << 8);
            if (size > 16) fseek(f, (long)(size - 16), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            data_off = (unsigned)ftell(f);
            data_bytes = size;
            break;
        } else fseek(f, (long)(size + (size & 1)), SEEK_CUR);
    }
    fprintf(stderr, "in: %d Hz %d ch %d-bit\n", sample_rate, channels, bits);
    if (sample_rate != 48000) { fprintf(stderr, "need 48kHz input\n"); return 1; }
    if (bits != 16) { fprintf(stderr, "need 16-bit PCM\n"); return 1; }

    long total_frames = (long)data_bytes / (channels * 2);
    short *raw = malloc((size_t)total_frames * channels * 2);
    fseek(f, (long)data_off, SEEK_SET);
    if (fread(raw, 2, (size_t)total_frames * channels, f) != (size_t)total_frames * channels) return 1;
    fclose(f);

    dfrestore_t df = init(channels, atten, beta);
    if (!df) { fprintf(stderr, "dfrestore_init failed\n"); return 1; }

    float *pcm = malloc((size_t)total_frames * channels * sizeof(float));
    for (long i = 0; i < total_frames * channels; i++)
        pcm[i] = raw[i] / 32768.0f;

    const int CHUNK = 4096; /* 模拟 mpv af 的不定长块 */
    for (long off = 0; off < total_frames; off += CHUNK) {
        int n = (int)((total_frames - off) < CHUNK ? (total_frames - off) : CHUNK);
        float *p = pcm + (size_t)off * channels;
        if (proc(df, p, n) != 0) { fprintf(stderr, "process error -> passthrough\n"); }
    }

    FILE *o = fopen(argv[2], "wb");
    unsigned byte_rate = (unsigned)sample_rate * channels * 2;
    unsigned char oh[44] = {0};
    memcpy(oh, "RIFF", 4); memcpy(oh + 8, "WAVE", 4);
    unsigned riff = 36 + data_bytes;
    oh[4]=riff; oh[5]=riff>>8; oh[6]=riff>>16; oh[7]=riff>>24;
    memcpy(oh + 12, "fmt ", 4);
    oh[16]=16; oh[20]=1; oh[22]=channels; oh[23]=0;
    oh[24]=sample_rate; oh[25]=sample_rate>>8; oh[26]=sample_rate>>16; oh[27]=sample_rate>>24;
    oh[28]=byte_rate; oh[29]=byte_rate>>8; oh[30]=byte_rate>>16; oh[31]=byte_rate>>24;
    oh[32]=channels*2; oh[34]=16;
    memcpy(oh + 36, "data", 4);
    oh[40]=data_bytes; oh[41]=data_bytes>>8; oh[42]=data_bytes>>16; oh[43]=data_bytes>>24;
    fwrite(oh, 1, 44, o);
    for (long i = 0; i < total_frames * channels; i++) {
        float v = pcm[i];
        long q = lrintf(v * 32768.0f);
        if (q > 32767) q = 32767;
        if (q < -32768) q = -32768;
        raw[i] = (short)q;
    }
    fwrite(raw, 2, (size_t)total_frames * channels, o);
    fclose(o);
    fprintf(stderr, "done: %ld frames\n", total_frames);
    free_fn(df);
    return 0;
}
