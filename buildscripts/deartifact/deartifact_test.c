/* deartifact_test.c —— 最小重建单测：常数信号 x=1.0 过 LOW 档，
 * 逐样本检查 |out - 1.0|。定位 WOLA 重建问题。 */
#include <stdio.h>
#include <math.h>
#include "deartifact.h"

int main(void) {
    int sr = 44100, ch = 1;
    void *ctx = deartifact_create(sr, ch, DEARTIFACT_LOW);
    if (!ctx) { printf("create failed\n"); return 1; }

    enum { T = 4096 };
    float in[T], out[T];
    for (int i = 0; i < T; i++) in[i] = 1.0f;

    if (deartifact_process(ctx, in, out, T)) { printf("process err\n"); return 1; }

    double max_err = 0; int bad_at = -1;
    for (int i = 0; i < T; i++) {
        double e = fabs(out[i] - 1.0);
        if (e > max_err) { max_err = e; bad_at = i; }
    }
    printf("max_err=%.4f at %d\n", max_err, bad_at);
    printf("first 16 out:");
    for (int i = 0; i < 16; i++) printf(" %.3f", out[i]);
    printf("\naround 1024:");
    for (int i = 1020; i < 1032; i++) printf(" %.3f", out[i]);
    printf("\nlast 8:");
    for (int i = T - 8; i < T; i++) printf(" %.3f", out[i]);
    printf("\n");

    float tail[DEARTIFACT_FRAME];
    int n = deartifact_flush(ctx, tail, DEARTIFACT_FRAME);
    printf("flushed=%d:", n);
    for (int i = 0; i < n && i < 8; i++) printf(" %.3f", tail[i]);
    printf("\n");

    deartifact_destroy(ctx);

    /* 第二阶段：恰好一帧，看 FIFO 头部内容 */
    ctx = deartifact_create(sr, ch, DEARTIFACT_LOW);
    enum { T1 = 1030 };
    float in1[T1], out1[T1];
    for (int i = 0; i < T1; i++) in1[i] = 1.0f;
    deartifact_process(ctx, in1, out1, T1);
    printf("phase2 out[1018..1029]:");
    for (int i = 1018; i < T1; i++) printf(" %.3f", out1[i]);
    printf("\n");
    float tail1[DEARTIFACT_FRAME];
    int n1 = deartifact_flush(ctx, tail1, DEARTIFACT_FRAME);
    printf("phase2 flushed=%d first8:", n1);
    for (int i = 0; i < n1 && i < 8; i++) printf(" %.3f", tail1[i]);
    printf("\n");
    deartifact_destroy(ctx);
    return 0;
}
