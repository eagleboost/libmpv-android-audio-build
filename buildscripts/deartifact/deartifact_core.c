/* deartifact_core.c —— De-Artifact Filter 核心 DSP（无 mpv 依赖）。
 * 接口与设计说明见 deartifact.h。 */

#include "deartifact.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- 参数表（依据 VoyTrace tasks/audio-restoration-voice-restore.md §8.3）---- */
typedef struct {
    double flux_threshold;   /* ① 检测阈值（相对帧间 flux） */
    double max_atten_db;     /* ③ 最大衰减 */
    double temporal_coef;    /* ③ 时间平滑：当前帧权重 */
    double lp_normal_hz;     /* ⑤ 正常 cutoff */
    double lp_artifact_hz;   /* ⑤ artifact 帧 cutoff */
    double mask_lo_hz;       /* ② 掩码下限（Hz） */
    int    de_esser;         /* ④ 是否启用 De-Esser */
} params_t;

static const params_t k_params[3] = {
    [DEARTIFACT_OFF]  = { .flux_threshold = 1e9, .max_atten_db = 0,
                          .temporal_coef = 1.0, .lp_normal_hz = 1e9,
                          .lp_artifact_hz = 1e9, .mask_lo_hz = 4000.0,
                          .de_esser = 0 },
    [DEARTIFACT_LOW]  = { .flux_threshold = 0.35, .max_atten_db = 6,
                          .temporal_coef = 0.50, .lp_normal_hz = 18000,
                          .lp_artifact_hz = 13000, .mask_lo_hz = 4000.0,
                          .de_esser = 0 },
    [DEARTIFACT_HIGH] = { .flux_threshold = 0.20, .max_atten_db = 18,
                          .temporal_coef = 0.20, .lp_normal_hz = 18000,
                          .lp_artifact_hz = 8500, .mask_lo_hz = 3000.0,
                          .de_esser = 1 },
};

/* 检测 / 处理频段（Hz） */
#define BAND_DET_LO   5000.0
#define BAND_DET_HI   9000.0
#define MASK_LO       4000.0
#define MASK_HI      11000.0
#define MASK_CORE_LO  5000.0
#define MASK_CORE_HI  9000.0
#define DEESS_LO      5000.0
#define DEESS_HI      8000.0

/* 触发防抖：连续 2 帧超阈值才触发；触发后保持 4 帧（~46ms @ hop 11.6ms） */
#define TRIG_FRAMES     2
#define HOLD_FRAMES     4

#define FIFO_CAP (DEARTIFACT_FRAME * 4)

typedef struct {
    int sample_rate;
    int channels;
    int strength;
    const params_t *p;

    float win[DEARTIFACT_FRAME];          /* Hann 窗 */

    /* 输入历史（最近 N 个样本/声道）；WOLA 输出累积器（长度 N，每帧滑动 H） */
    float hist[DEARTIFACT_MAX_CH][DEARTIFACT_FRAME];
    float out_acc[DEARTIFACT_MAX_CH][DEARTIFACT_FRAME];
    float winsq_acc[DEARTIFACT_FRAME];
    int   hist_fill;                      /* hist 中有效样本数（< N 时在预热） */

    /* 输出 FIFO（环形）：处理完的样本按序排队（STFT 延迟 512 样本） */
    float fifo[DEARTIFACT_MAX_CH][FIFO_CAP];
    int   fifo_head, fifo_count;

    /* FFT 工作区 */
    double re[DEARTIFACT_FRAME];
    double im[DEARTIFACT_FRAME];
    double mag_prev[DEARTIFACT_FRAME / 2];  /* 上一帧幅度谱（时间平滑） */
    int    have_prev;

    /* 检测 / 控制状态 */
    double det_energy_prev;               /* 上一帧 5–9k 带内能量（mid 通道） */
    int    over_cnt;                      /* 连续超阈值帧计数 */
    int    hold_cnt;                      /* 触发后剩余保持帧数 */
    double cutoff_hz;                     /* 当前平滑后 cutoff */
    double deess_gain;                    /* De-Esser 平滑增益 */

    /* bin 索引缓存 */
    int det_lo_bin, det_hi_bin, mask_lo_bin, mask_hi_bin;
    int core_lo_bin, core_hi_bin, deess_lo_bin, deess_hi_bin;

    /* 诊断统计 */
    uint64_t stat_frames, stat_active_frames, stat_deess_frames;

    int ready;                            /* 初始化成功 */
} deartifact_t;

/* ---------------- 基础 FFT（原位迭代 radix-2） ---------------- */
static void fft(double *re, double *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; k++) {
                double ur = re[i + k], ui = im[i + k];
                double vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                double vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

static void ifft(double *re, double *im, int n) {
    for (int i = 0; i < n; i++) im[i] = -im[i];
    fft(re, im, n);
    double inv = 1.0 / n;
    for (int i = 0; i < n; i++) { re[i] *= inv; im[i] *= -inv; }
}

/* ---------------- 生命周期 ---------------- */

/* ---------------- FIFO ---------------- */
static void fifo_push(deartifact_t *d, const float *vals) {
    int tail = (d->fifo_head + d->fifo_count) % FIFO_CAP;
    for (int ch = 0; ch < d->channels; ch++) d->fifo[ch][tail] = vals[ch];
    d->fifo_count++;
}

static void fifo_pop(deartifact_t *d, float *out) {
    for (int ch = 0; ch < d->channels; ch++) out[ch] = d->fifo[ch][d->fifo_head];
    d->fifo_head = (d->fifo_head + 1) % FIFO_CAP;
    d->fifo_count--;
}

void *deartifact_create(int sample_rate, int channels, int strength) {
    if (sample_rate <= 0 || channels <= 0 || channels > DEARTIFACT_MAX_CH)
        return NULL;
    if (strength < DEARTIFACT_OFF || strength > DEARTIFACT_HIGH)
        return NULL;
    deartifact_t *d = calloc(1, sizeof(deartifact_t));
    if (!d) return NULL;
    d->sample_rate = sample_rate;
    d->channels = channels;
    d->strength = strength;
    d->p = &k_params[strength];

    for (int i = 0; i < DEARTIFACT_FRAME; i++)
        d->win[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / DEARTIFACT_FRAME));

    double bin_hz = (double)sample_rate / DEARTIFACT_FRAME;
    d->det_lo_bin  = (int)(BAND_DET_LO  / bin_hz);
    d->det_hi_bin  = (int)(BAND_DET_HI  / bin_hz);
    d->mask_lo_bin = (int)(d->p->mask_lo_hz / bin_hz);
    d->mask_hi_bin = (int)(MASK_HI      / bin_hz);
    d->core_lo_bin = (int)(MASK_CORE_LO / bin_hz);
    d->core_hi_bin = (int)(MASK_CORE_HI / bin_hz);
    d->deess_lo_bin= (int)(DEESS_LO     / bin_hz);
    d->deess_hi_bin= (int)(DEESS_HI     / bin_hz);
    if (d->det_hi_bin > DEARTIFACT_FRAME / 2) d->det_hi_bin = DEARTIFACT_FRAME / 2;
    if (d->mask_hi_bin > DEARTIFACT_FRAME / 2) d->mask_hi_bin = DEARTIFACT_FRAME / 2;

    d->cutoff_hz = d->p->lp_normal_hz;
    d->deess_gain = 1.0;
    d->ready = 1;
    return d;
}

void deartifact_destroy(void *ctx) { free(ctx); }

void deartifact_reset(void *ctx) {
    deartifact_t *d = ctx;
    if (!d) return;
    memset(d->hist, 0, sizeof(d->hist));
    memset(d->out_acc, 0, sizeof(d->out_acc));
    memset(d->winsq_acc, 0, sizeof(d->winsq_acc));
    memset(d->mag_prev, 0, sizeof(d->mag_prev));
    d->hist_fill = 0;
    d->fifo_head = d->fifo_count = 0;
    d->have_prev = 0;
    d->det_energy_prev = 0;
    d->over_cnt = 0;
    d->hold_cnt = 0;
    d->cutoff_hz = d->p->lp_normal_hz;
    d->deess_gain = 1.0;
}

/* ---------------- 单帧处理 ---------------- */

/* 频段权重：core 区 1.0，向 MASK_LO/HI 两侧线性降到 0.3 */
static double band_weight(const deartifact_t *d, int bin) {
    double bw = 1.0;
    if (bin < d->core_lo_bin) {
        int span = d->core_lo_bin - d->mask_lo_bin;
        bw = span > 0 ? 0.3 + 0.7 * (double)(bin - d->mask_lo_bin) / span : 0.3;
    } else if (bin > d->core_hi_bin) {
        int span = d->mask_hi_bin - d->core_hi_bin;
        bw = span > 0 ? 0.3 + 0.7 * (double)(d->mask_hi_bin - bin) / span : 0.3;
    }
    if (bw < 0.3) bw = 0.3;
    if (bw > 1.0) bw = 1.0;
    return bw;
}

static void process_frame(deartifact_t *d) {
    const int N = DEARTIFACT_FRAME, halfN = N / 2;
    const params_t *p = d->p;

    /* 每个声道：加窗 → FFT；同时用 ch0/ch1 平均近似 mid 做检测 */
    double mag[DEARTIFACT_MAX_CH][DEARTIFACT_FRAME / 2];
    double det_energy = 0.0;
    int det_cnt = 0;

    for (int ch = 0; ch < d->channels; ch++) {
        for (int i = 0; i < N; i++) {
            d->re[i] = (double)d->hist[ch][i] * d->win[i];
            d->im[i] = 0.0;
        }
        fft(d->re, d->im, N);
        for (int k = 0; k < halfN; k++)
            mag[ch][k] = sqrt(d->re[k] * d->re[k] + d->im[k] * d->im[k]);
        if (ch < 2) {
            for (int k = d->det_lo_bin; k < d->det_hi_bin; k++) {
                det_energy += mag[ch][k];
                det_cnt++;
            }
        }
    }
    if (det_cnt > 0) det_energy /= det_cnt;
    if (d->channels == 1) det_energy *= 2.0; /* 单声道时 mag[1] 全 0，补偿 */

    /* ① 检测：相对帧间 flux */
    double denom = det_energy > d->det_energy_prev ? det_energy : d->det_energy_prev;
    double flux = 0.0;
    if (denom > 1e-12)
        flux = fabs(det_energy - d->det_energy_prev) / denom;
    d->det_energy_prev = det_energy;

    if (flux > p->flux_threshold) d->over_cnt++;
    else d->over_cnt = 0;
    if (d->over_cnt >= TRIG_FRAMES) {
        d->hold_cnt = HOLD_FRAMES;
        d->over_cnt = TRIG_FRAMES;
    }
    int artifact_active = d->hold_cnt > 0;
    if (d->hold_cnt > 0) d->hold_cnt--;
    d->stat_frames++;
    if (artifact_active) d->stat_active_frames++;
    if (p->de_esser && d->deess_gain < 0.9) d->stat_deess_frames++;

    /* ⑤ 动态低通 cutoff 平滑（artifact 时收到 lp_artifact_hz） */
    double cutoff_target = artifact_active ? p->lp_artifact_hz : p->lp_normal_hz;
    d->cutoff_hz = 0.7 * d->cutoff_hz + 0.3 * cutoff_target;

    /* ④ De-Esser（仅 HIGH）：5–8k 能量相对全带过高时压低，帧间平滑增益 */
    if (p->de_esser) {
        double band_e = 0, all_e = 0;
        for (int k = 0; k < halfN; k++) {
            double m = 0.5 * (mag[0][k] + (d->channels > 1 ? mag[1][k] : mag[0][k]));
            all_e += m;
            if (k >= d->deess_lo_bin && k < d->deess_hi_bin) band_e += m;
        }
        double ratio = all_e > 1e-12 ? band_e / all_e : 0.0;
        /* 5–8k 占全带 >18%（经验值：齿音/伪影突出）→ 目标增益 0.5（-6dB） */
        double target = ratio > 0.18 ? 0.5 : 1.0;
        /* 快攻慢放 */
        double coef = target < d->deess_gain ? 0.7 : 0.95;
        d->deess_gain = coef * d->deess_gain + (1.0 - coef) * target;
    }

    double max_atten_lin = pow(10.0, -p->max_atten_db / 20.0);

    for (int ch = 0; ch < d->channels; ch++) {
        /* 重新对该声道做加窗 FFT（上面的循环只留下了最后一个声道的频谱） */
        for (int i = 0; i < N; i++) {
            d->re[i] = (double)d->hist[ch][i] * d->win[i];
            d->im[i] = 0.0;
        }
        fft(d->re, d->im, N);

        for (int k = 0; k < halfN; k++) {
            double m = mag[ch][k];
            double out_m = m;

            if (k >= d->mask_lo_bin && k < d->mask_hi_bin && d->have_prev) {
                /* ② Artifact Mask（衰减门控） */
                double over = (flux - p->flux_threshold) / 0.20; /* 余量 0.20 */
                double mask = over;
                if (mask < 0) mask = 0;
                if (mask > 1) mask = 1;
                if (!artifact_active) mask = 0;
                mask *= band_weight(d, k);

                /* ③ 时间平滑（4–11k 常开——伪影闪烁的主压制手段）：
                 * 0.7×当前 + 0.3×上一帧 */
                double smoothed = p->temporal_coef * (double)m
                                + (1.0 - p->temporal_coef) * d->mag_prev[k];
                /* 频率平滑：smoothed 的 3-tap（首尾用自身） */
                double prev_k = k > d->mask_lo_bin ? d->mag_prev[k - 1] : smoothed;
                double next_k = k + 1 < d->mask_hi_bin ? d->mag_prev[k + 1] : smoothed;
                double freq_avg = (prev_k + smoothed + next_k) / 3.0;
                out_m = 0.5 * smoothed + 0.5 * freq_avg;
                /* 增益衰减：仅 artifact 帧 */
                if (mask > 0) {
                    double g = 1.0 - mask * (1.0 - max_atten_lin);
                    out_m *= g;
                }

                /* ④ De-Esser 频段额外压低 */
                if (p->de_esser && k >= d->deess_lo_bin && k < d->deess_hi_bin)
                    out_m *= d->deess_gain;
            }

            /* ⑤ 动态低通（软倾斜，非砖墙） */
            if (d->cutoff_hz < 17000.0) {
                double f = (double)k * d->sample_rate / N;
                double x = f / d->cutoff_hz;
                out_m *= 1.0 / sqrt(1.0 + x * x * x * x);
            }

            double scale = (m > 1e-15) ? out_m / m : 1.0;
            d->re[k] *= scale; d->im[k] *= scale;
            if (k > 0 && k < halfN) {
                d->re[N - k] *= scale; d->im[N - k] *= scale;
            }
        }
        /* k=0 与 k=halfN 保持共轭对称（halfN 处需同为实数，已满足） */
        d->im[0] = 0.0;
        d->im[halfN] = 0.0;

        ifft(d->re, d->im, N);

        /* WOLA：加合成窗（分析已加窗，此处直接累积并按 window² 归一化） */
        for (int i = 0; i < N; i++) {
            d->out_acc[ch][i] += (float)(d->re[i] * d->win[i]);
        }
    }
    /* 记录 mid 幅度谱供下帧时间平滑（递归 EMA：存平滑后结果，
     * 平滑深度随时间累积，比 2 帧平均强得多） */
    for (int k = 0; k < halfN; k++) {
        double mid_m = 0.5 * (mag[0][k]
                    + (d->channels > 1 ? mag[1][k] : mag[0][k]));
        d->mag_prev[k] = (float)(p->temporal_coef * mid_m
                        + (1.0 - p->temporal_coef) * d->mag_prev[k]);
    }

    /* window² 累积（每帧相同，与音频同步累积/滑动） */
    for (int i = 0; i < N; i++)
        d->winsq_acc[i] += d->win[i] * d->win[i];
    d->have_prev = 1;

    /* 产出前 hop 个输出样本（归一化后推入 FIFO），随后整体滑动 H */
    {
        const int H = DEARTIFACT_HOP;
        for (int n = 0; n < H; n++) {
            float vals[DEARTIFACT_MAX_CH];
            float w2 = d->winsq_acc[n];
            for (int ch = 0; ch < d->channels; ch++) {
                float v;
                if (w2 < 1e-7f) {
                    v = d->hist[ch][n]; /* 边缘窗能量过小 → 用原始样本 */
                } else {
                    v = d->out_acc[ch][n] / w2;
                }
                if (!isfinite(v) || v > 4.0f || v < -4.0f)
                    v = d->hist[ch][n]; /* 异常回退直通，绝不静音 */
                vals[ch] = v;
            }
            fifo_push(d, vals);
        }
        for (int ch = 0; ch < d->channels; ch++) {
            memmove(d->out_acc[ch], d->out_acc[ch] + H, (N - H) * sizeof(float));
            memset(d->out_acc[ch] + N - H, 0, H * sizeof(float));
        }
        memmove(d->winsq_acc, d->winsq_acc + H, (N - H) * sizeof(float));
        memset(d->winsq_acc + N - H, 0, H * sizeof(float));

        /* hist 滑动：保留后 N-H 个样本等待新数据 */
        for (int ch = 0; ch < d->channels; ch++)
            memmove(d->hist[ch], d->hist[ch] + H, (N - H) * sizeof(float));
        d->hist_fill = N - H;
    }
}

int deartifact_process(void *ctx, const float *in, float *out, int frames) {
    deartifact_t *d = ctx;
    if (!d || !d->ready || !in || !out || frames < 0)
        return -1;
    if (d->strength == DEARTIFACT_OFF) {
        if (in != out) memmove(out, in, (size_t)frames * d->channels * sizeof(float));
        return 0;
    }

    for (int i = 0; i < frames; i++) {
        for (int ch = 0; ch < d->channels; ch++) {
            float s = in[i * d->channels + ch];
            if (!isfinite(s)) s = 0.0f; /* NaN 防护 */
            d->hist[ch][d->hist_fill] = s;
        }
        d->hist_fill++;

        if (d->hist_fill == DEARTIFACT_FRAME)
            process_frame(d);

        /* 输出：预热期（FIFO 空）直通，之后消费 FIFO（延迟 512 样本） */
        if (d->fifo_count > 0) {
            float vals[DEARTIFACT_MAX_CH];
            fifo_pop(d, vals);
            for (int ch = 0; ch < d->channels; ch++)
                out[i * d->channels + ch] = vals[ch];
        } else {
            for (int ch = 0; ch < d->channels; ch++)
                out[i * d->channels + ch] = in[i * d->channels + ch];
        }
    }
    return 0;
}

int deartifact_flush(void *ctx, float *out, int max) {
    deartifact_t *d = ctx;
    if (!d || !d->ready || !out || max <= 0) return 0;
    int n = d->fifo_count < max ? d->fifo_count : max;
    for (int i = 0; i < n; i++) {
        float vals[DEARTIFACT_MAX_CH];
        fifo_pop(d, vals);
        for (int ch = 0; ch < d->channels; ch++)
            out[i * d->channels + ch] = vals[ch];
    }
    return n;
}

int deartifact_pending(void *ctx) {
    deartifact_t *d = ctx;
    return d ? d->fifo_count : 0;
}

/* 诊断：输出 [总帧数, artifact 激活帧数, de-esser 激活帧数] */
void deartifact_debug_stats(void *ctx, uint64_t *out3) {
    deartifact_t *d = ctx;
    if (!d || !out3) return;
    out3[0] = d->stat_frames;
    out3[1] = d->stat_active_frames;
    out3[2] = d->stat_deess_frames;
}
