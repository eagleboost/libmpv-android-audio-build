/*
 * af_soundlab.c — VoyTrace Sound Lab 参数均衡器（原生 mpv audio filter）
 *
 * 设计（tasks/均衡器实现调研.md V2）：
 * - 纯 C 的 peaking biquad 串联（RBJ Audio EQ Cookbook），与人声增强
 *   （af_dfrestore 内置 Voice Clarity EQ）共用同一套数学，一套实现
 *   服务所有 EQ 场景；不依赖 FFmpeg 滤镜编译清单（lavfi equalizer
 *   曾因缺 aresample 静默失败）。
 * - 曲线由 App 侧（Dart sound_goal.dart）解析为 cut-only 后下发：
 *   所有段增益 ≤ 0 dB，物理上不可能削波，无需 preamp/limiter。
 * - 每声道独立 biquad 链（L/R 状态不串扰）。
 *
 * 用法（mpv af 串）：
 *   sl=b=<fc>/<width_oct>/<gain_db>[;<fc>/<w>/<g>...]
 * 例：
 *   sl=b=300/0.90/-3.15
 *   sl=b=110/1.20/-4.50;350/1.00/-2.00;2600/1.00/0.00
 * 分隔符只用 ';' 和 '/'，与 mpv af 列表的 ',' ':' '=' '[' ']' 不冲突，
 * 无需转义。最多 SL_MAX_BANDS 段。
 *
 * 格式协商：autoconvert 请求 AF_FORMAT_FLOAT（打包 interleaved），
 * 采样率任意（biquad 系数按实际 fs 计算）。
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio/aframe.h"
#include "audio/format.h"
#include "filters/f_autoconvert.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "options/m_option.h"
#include "common/common.h"

#define SL_MAX_BANDS 8
#define SL_MAX_CH 8

/* ---- biquad（与 af_dfrestore 的 Voice Clarity EQ 同一套 RBJ 数学） ---- */

typedef struct biquad_t {
    double b0, b1, b2, a1, a2;
    double z1, z2;
} biquad_t;

/* RBJ Audio EQ Cookbook — peaking biquad 系数（width 为倍频程） */
static void biquad_peaking(biquad_t *f, double fc, double gdb, double width_oct,
                           double fs)
{
    double A = pow(10.0, gdb / 40.0);          // sqrt of gain
    double w0 = 2.0 * M_PI * fc / fs;
    double alpha = sin(w0) * sinh(log(2.0) / 2.0 * width_oct * w0 / sin(w0));

    double a0 = 1.0 + alpha / A;
    f->b0 = (1.0 + alpha * A) / a0;
    f->b1 = (-2.0 * cos(w0)) / a0;
    f->b2 = (1.0 - alpha * A) / a0;
    f->a1 = (-2.0 * cos(w0)) / a0;
    f->a2 = (1.0 - alpha / A) / a0;   // ← 符号修复：RBJ 是 1−alpha/A（写成 + 会不稳定→杂音）
    f->z1 = f->z2 = 0.0;
}

/* Transposed Direct Form II（数值稳定） */
static inline double biquad_process(biquad_t *f, double x)
{
    double y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* ------------------------------------------------------------------ */

struct sl_opts {
    char *bands;   // "fc/w/g;fc/w/g;..."（App 侧已 cut-only）
};

struct band {
    double fc, width, gdb;
};

struct priv {
    struct sl_opts *opts;

    struct mp_pin *in_pin;
    struct mp_aframe *cur_format;

    struct band bands[SL_MAX_BANDS];
    int nbands;
    biquad_t chain[SL_MAX_CH][SL_MAX_BANDS];
    int nch;
};

/* 解析 "fc/w/g;fc/w/g" → bands[]。返回段数，非法返回 -1。 */
static int sl_parse(const char *spec, struct band *out, int max)
{
    if (!spec || !*spec)
        return 0;
    char *tmp = talloc_strdup(NULL, spec);
    int n = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(tmp, ";", &saveptr); tok;
         tok = strtok_r(NULL, ";", &saveptr)) {
        double fc, w, g;
        if (sscanf(tok, "%lf/%lf/%lf", &fc, &w, &g) != 3) {
            n = -1;
            break;
        }
        if (fc < 10.0 || fc > 20000.0 || w <= 0.0 || w > 8.0) {
            n = -1;
            break;
        }
        if (fabs(g) < 1e-6)
            continue;   // 0 dB 段 no-op，直接跳过
        if (n >= max) {
            n = -1;
            break;
        }
        out[n].fc = fc;
        out[n].width = w;
        out[n].gdb = g;
        n++;
    }
    talloc_free(tmp);
    return n;
}

static bool reinit(struct mp_filter *f, struct mp_aframe *fmt)
{
    struct priv *s = f->priv;

    int rate = mp_aframe_get_rate(fmt);
    int nch = mp_aframe_get_channels(fmt);

    s->nbands = sl_parse(s->opts->bands, s->bands, SL_MAX_BANDS);
    if (s->nbands < 0) {
        MP_ERR(f, "[sl] invalid bands spec: '%s'\n", s->opts->bands);
        return false;
    }
    s->nch = nch > SL_MAX_CH ? SL_MAX_CH : nch;
    for (int c = 0; c < s->nch; c++)
        for (int b = 0; b < s->nbands; b++)
            biquad_peaking(&s->chain[c][b], s->bands[b].fc, s->bands[b].gdb,
                           s->bands[b].width, (double)rate);

    mp_aframe_reset(s->cur_format);
    mp_aframe_config_copy(s->cur_format, fmt);
    MP_INFO(f, "[sl] reinit rate=%d ch=%d bands=%d\n", rate, nch, s->nbands);
    return true;
}

static void process(struct mp_filter *f)
{
    struct priv *s = f->priv;

    if (!mp_pin_in_needs_data(f->ppins[1]))
        return;

    struct mp_frame frame = mp_pin_out_read(s->in_pin);
    if (!frame.type) {
        mp_pin_out_request_data_next(s->in_pin);
        return;
    }

    if (frame.type != MP_FRAME_AUDIO && frame.type != MP_FRAME_EOF) {
        MP_ERR(f, "[sl] unexpected frame type\n");
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }

    if (frame.type == MP_FRAME_EOF) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    struct mp_aframe *in = frame.data;

    if (!mp_aframe_config_equals(in, s->cur_format)) {
        if (!reinit(f, in)) {
            mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, in));
            return;
        }
    }

    int samples = mp_aframe_get_size(in);
    if (s->nbands > 0 && samples > 0) {
        uint8_t **planes = mp_aframe_get_data_rw(in);
        if (planes && planes[0]) {
            // AF_FORMAT_FLOAT 打包格式：interleaved float
            float *buf = (float *)planes[0];
            for (int i = 0; i < samples; i++)
                for (int c = 0; c < s->nch; c++) {
                    double x = buf[i * s->nch + c];
                    for (int b = 0; b < s->nbands; b++)
                        x = biquad_process(&s->chain[c][b], x);
                    buf[i * s->nch + c] = (float)x;
                }
        }
    }

    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, in));
}

static void reset(struct mp_filter *f)
{
    struct priv *s = f->priv;
    // 清 biquad 状态（seek 后防止瞬态）
    for (int c = 0; c < SL_MAX_CH; c++)
        for (int b = 0; b < SL_MAX_BANDS; b++) {
            s->chain[c][b].z1 = 0.0;
            s->chain[c][b].z2 = 0.0;
        }
}

static const struct mp_filter_info af_soundlab_filter = {
    .name = "sl",
    .priv_size = sizeof(struct priv),
    .process = process,
    .reset = reset,
};

static struct mp_filter *af_soundlab_create(struct mp_filter *parent,
                                            void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &af_soundlab_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *s = f->priv;
    s->opts = talloc_steal(s, options);
    s->cur_format = talloc_steal(s, mp_aframe_create());

    // 预检 bands 语法（快速失败，不进滤镜链）
    struct band probe[SL_MAX_BANDS];
    if (sl_parse(s->opts->bands, probe, SL_MAX_BANDS) < 0) {
        MP_ERR(f, "[sl] invalid bands spec: '%s'\n", s->opts->bands);
        return NULL;
    }

    struct mp_autoconvert *conv = mp_autoconvert_create(f);
    if (!conv)
        abort();

    mp_autoconvert_add_afmt(conv, AF_FORMAT_FLOAT);

    mp_pin_connect(conv->f->pins[0], f->ppins[0]);
    s->in_pin = conv->f->pins[1];

    return f;
}

#define OPT_BASE_STRUCT struct sl_opts

const struct mp_user_filter_entry af_soundlab = {
    .desc = {
        .description = "VoyTrace Sound Lab parametric EQ "
                       "(RBJ peaking biquads, cut-only from app)",
        .name = "sl",
        .priv_size = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &(const OPT_BASE_STRUCT) {
            .bands = "",
        },
        .options = (const struct m_option[]) {
            {"b", OPT_STRING(bands)},
            {"bands", OPT_STRING(bands)},
            {0}
        },
    },
    .create = af_soundlab_create,
};
