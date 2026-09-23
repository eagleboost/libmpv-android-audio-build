/* af_dfrestore.c —— VoyTrace Restore：DeepFilterNet3 实时语音增强 mpv af。
 *
 * 通过 dlopen 加载 libdfrestore.so（Rust，内嵌 DFN3 模型，见
 * buildscripts/dfrestore/）。加载失败或初始化失败时滤镜创建失败，
 * mpv 自动旁路，绝不影响播放。
 *
 * 挂载：--af=dfrestore=atten_lim=12:pf_beta=0.05
 * 设计文档：VoyTrace tasks/audio-restoration-voice-restore.md §11
 */

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>

#include "audio/aframe.h"
#include "audio/format.h"
#include "common/common.h"
#include "filters/f_autoconvert.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "options/m_option.h"

/* libdfrestore.so 的 C ABI（见 buildscripts/dfrestore/src/lib.rs） */
typedef void *dfrestore_t;
typedef dfrestore_t (*dfrestore_init_fn)(int channels, float atten_lim_db, float pf_beta);
typedef int (*dfrestore_process_fn)(dfrestore_t, float *samples, int frames);
typedef size_t (*dfrestore_hop_fn)(dfrestore_t);
typedef void (*dfrestore_reset_fn)(dfrestore_t);
typedef void (*dfrestore_free_fn)(dfrestore_t);

/* ------------------------------------------------------------------ */
/* Voice Clarity EQ —— DFN3 降噪后的第二级补偿（tasks/voice-clarity-eq.md）
 *
 * DFN3 消除 MP3 高频伪影的同时把人声高频谐波也压掉了（人声变闷）。
 * 8 段 peaking biquad 串联：低频去浑浊 + 2-4 kHz presence boost 恢复
 * 清晰度（人声可懂度核心频段），7.5 kHz 轻衰减防伪影回放。
 * 末端 soft limiter 防 EQ 提升后削波。
 */

typedef struct {
    double b0, b1, b2, a1, a2;   // 归一化系数
    double x1, x2, y1, y2;       // 状态（上一/上两帧输入输出）
} biquad_t;

typedef struct {
    double fc;     // 中心频率 Hz
    double gdb;    // 增益 dB
    double Q;
} eq_band_t;

/* 曲线设计：重点 2-4 kHz presence，7.5 kHz 微降防伪影回放 */
static const eq_band_t voice_clarity_eq[] = {
    {  80.0, -2.0, 0.7 },   // 低频隆隆
    { 150.0, -1.0, 0.8 },   // 浑浊
    { 300.0, -2.0, 0.9 },   // 闷/箱子声
    { 800.0, -1.0, 1.0 },   // 鼻音/厚重
    { 2000.0, +1.5, 0.9 },  // 人声存在感
    { 3000.0, +2.0, 1.0 },  // 主要清晰度
    { 4500.0, +1.0, 1.0 },  // 辅音清晰度
    { 7500.0, -0.5, 1.0 },  // 防伪影回放
};
#define EQ_BANDS (sizeof(voice_clarity_eq) / sizeof(voice_clarity_eq[0]))
#define EQ_PREAMP_DB (-3.0)  // 前置增益：为 EQ 提升留 headroom

/* RBJ Audio EQ Cookbook — peaking biquad 系数 */
static void biquad_peaking(biquad_t *f, double fc, double gdb, double Q, double fs)
{
    double A = pow(10.0, gdb / 40.0);
    double w0 = 2.0 * M_PI * fc / fs;
    double cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * Q);
    double a0 = 1.0 + alpha / A;
    f->b0 = (1.0 + alpha * A) / a0;
    f->b1 = (-2.0 * cw) / a0;
    f->b2 = (1.0 - alpha * A) / a0;
    f->a1 = (-2.0 * cw) / a0;
    f->a2 = (1.0 - alpha / A) / a0;
}

/* 初始化 EQ 链（reinit 时按实际 fs 计算） */
static void eq_init(biquad_t *chain, int bands, double fs, double preamp_db)
{
    for (int i = 0; i < bands; i++) {
        biquad_peaking(&chain[i], voice_clarity_eq[i].fc,
                       voice_clarity_eq[i].gdb, voice_clarity_eq[i].Q, fs);
        chain[i].x1 = chain[i].x2 = chain[i].y1 = chain[i].y2 = 0.0;
    }
    // 第 0 个 band 前串一个整体 preamp（合并进 b0/b2 的 DC 增益）
    (void)preamp_db; // preamp 在 eq_process 入口做标量乘，更清晰
}

static inline float biquad_process(biquad_t *f, double x)
{
    double y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
             - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return (float)y;
}

/* EQ 链处理 interleaved float — 每声道独立 biquad 链
 *（共用状态会让 L,R,L,R 交替通过同一滤波器 → 声道间串扰 +
 *  滤波器状态异常 → 可能产生溢出值） */
static void eq_process(biquad_t chains[][EQ_BANDS], int bands, int nch,
                       float *buf, int total_samples, double preamp_linear)
{
    for (int i = 0; i < total_samples; i++) {
        int ch = i % nch; // 该样本属于哪个声道
        biquad_t *chain = chains[ch];
        double x = buf[i] * preamp_linear;
        for (int b = 0; b < bands; b++)
            x = biquad_process(&chain[b], x);
        // NaN/Inf 防护（理论上不会发生，防御式编程）
        if (!(x > -100.0 && x < 100.0)) x = 0.0;
        buf[i] = (float)x;
    }
}

/* Soft limiter：|x| > 0.9 后 tanh 软限幅，防削波且几乎无失真 */
static inline float soft_limit(float x)
{
    if (x > 0.9f)  return 0.9f + tanhf((x - 0.9f) * 5.0f) * 0.1f;
    if (x < -0.9f) return -0.9f + tanhf((x + 0.9f) * 5.0f) * 0.1f;
    return x;
}
/* ------------------------------------------------------------------ */

struct f_opts {
    float atten_lim;
    float pf_beta;
    int eq_enabled;   // 1 = DFN3 后串 Voice Clarity EQ
};

struct priv {
    struct f_opts *opts;

    struct mp_pin *in_pin;
    struct mp_aframe *cur_format;
    struct mp_aframe_pool *out_pool;
    struct mp_aframe *in;

    void *lib;
    dfrestore_t df;
    dfrestore_init_fn df_init;
    dfrestore_process_fn df_process;
    dfrestore_hop_fn df_hop_size;
    dfrestore_reset_fn df_reset;
    dfrestore_free_fn df_free;

    /* Voice Clarity EQ — 每声道独立 biquad 链（L/R 状态不互相污染） */
#define EQ_MAX_CH 2
    biquad_t eq_chain[EQ_MAX_CH][EQ_BANDS];
    int eq_nch;
    double eq_preamp;
};

static void unload(struct priv *s)
{
    if (s->df && s->df_free)
        s->df_free(s->df);
    s->df = NULL;
    // 不 dlclose：Rust/tract 注册的 TLS 析构器（pthread_key destructor）
    // 在 mpv core 等线程退出时仍会被 pthread_key_clean_all 调用——
    // dlclose 解除代码映射后指向 non-executable 内存 → SIGSEGV
    //（stack: pthread_exit → pthread_key_clean_all → unmapped code）。
    // 库 ~15MB，保驻代价可接受；下次 create 的 dlopen 引用计数+1 复用。
    s->lib = NULL; // 只清引用，不 dlclose
    s->df_init = NULL;
    s->df_process = NULL;
    s->df_hop_size = NULL;
    s->df_reset = NULL;
    s->df_free = NULL;
}

static bool load_lib(struct priv *s, struct mp_filter *f)
{
    MP_INFO(f, "[dfrestore] dlopen(libdfrestore.so)...\n"); // 见下方说明
    s->lib = dlopen("libdfrestore.so", RTLD_NOW | RTLD_LOCAL);
    if (!s->lib) {
        MP_ERR(f, "[dfrestore] dlopen failed: %s\n", dlerror());
        // Android 上同 APK 的 native lib 目录取决于调用进程 classloader
        // namespace，通常可直接按名加载；失败则再试全路径变体。
        s->lib = dlopen("libdfrestore.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!s->lib) {
        MP_ERR(f, "[dfrestore] dlopen failed again: %s\n", dlerror());
        return false;
    }
    MP_INFO(f, "[dfrestore] dlopen ok, resolving symbols...\n");
    s->df_init = (dfrestore_init_fn)(uintptr_t)dlsym(s->lib, "dfrestore_init");
    s->df_process = (dfrestore_process_fn)(uintptr_t)dlsym(s->lib, "dfrestore_process");
    s->df_hop_size = (dfrestore_hop_fn)(uintptr_t)dlsym(s->lib, "dfrestore_hop_size");
    s->df_reset = (dfrestore_reset_fn)(uintptr_t)dlsym(s->lib, "dfrestore_reset");
    s->df_free = (dfrestore_free_fn)(uintptr_t)dlsym(s->lib, "dfrestore_free");
    bool ok = s->df_init && s->df_process && s->df_reset && s->df_free;
    MP_INFO(f, "[dfrestore] symbols %s (hop_fn=%d)\n", ok ? "ok" : "MISSING", s->df_hop_size != NULL);
    return ok;
}

static bool reinit(struct mp_filter *f, struct mp_aframe *fmt)
{
    struct priv *s = f->priv;

    int rate = mp_aframe_get_rate(fmt);
    int nch = mp_aframe_get_channels(fmt);
    MP_INFO(f, "[dfrestore] reinit rate=%d ch=%d\n", rate, nch);

    if (rate != 48000) {
        // mp_autoconvert 已请求 48k；防御性兜底
        MP_ERR(f, "[dfrestore] expected 48kHz, got %d\n", rate);
        return false;
    }

    if (s->df)
        s->df_free(s->df);
    MP_INFO(f, "[dfrestore] calling dfrestore_init(ch=%d, atten=%.1f, beta=%.2f)...\n",
            nch, s->opts->atten_lim, s->opts->pf_beta);
    s->df = s->df_init(nch, s->opts->atten_lim, s->opts->pf_beta);
    if (!s->df) {
        MP_ERR(f, "[dfrestore] init failed (channels=%d)\n", nch);
        return false;
    }
    MP_INFO(f, "[dfrestore] init ok, hop=%zu\n", s->df_hop_size(s->df));

    mp_aframe_reset(s->cur_format);
    mp_aframe_config_copy(s->cur_format, fmt);

    // Voice Clarity EQ：按实际采样率初始化 biquad 系数（每声道一条链）
    if (s->opts->eq_enabled) {
        int ch = nch > EQ_MAX_CH ? EQ_MAX_CH : nch;
        s->eq_nch = ch;
        for (int c = 0; c < ch; c++)
            eq_init(s->eq_chain[c], EQ_BANDS, (double)rate, EQ_PREAMP_DB);
        s->eq_preamp = pow(10.0, EQ_PREAMP_DB / 20.0);
        MP_INFO(f, "[dfrestore] voice clarity EQ on (%d bands × %d ch, preamp %.1f dB)\n",
                 (int)EQ_BANDS, ch, EQ_PREAMP_DB);
    }
    return true;
}

static void process(struct mp_filter *f)
{
    struct priv *s = f->priv;

    if (!mp_pin_in_needs_data(f->ppins[1]))
        return;

    struct mp_frame frame = mp_pin_out_read(s->in_pin);
    if (!frame.type) {
        // 关键：经 autoconvert 中间层读数据必须显式请求下一帧
        // （与 scaletempo 相同），否则违反 filter graph 前置条件。
        mp_pin_out_request_data_next(s->in_pin);
        return; // no input yet
    }

    if (frame.type != MP_FRAME_AUDIO && frame.type != MP_FRAME_EOF) {
        MP_ERR(f, "unexpected frame type\n");
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }

    if (frame.type == MP_FRAME_EOF) {
        // 无缓冲数据：直接把 EOF 传下去（不 repeat）
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    s->in = frame.data;

    if (!mp_aframe_config_equals(s->in, s->cur_format)) {
        if (!reinit(f, s->in)) {
            // 直通（格式无法处理时保持原始数据）
            mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, s->in));
            s->in = NULL;
            return;
        }
    }

    int samples = mp_aframe_get_size(s->in);

    if (s->df && samples > 0) {
        uint8_t **planes = mp_aframe_get_data_rw(s->in);
        if (planes && planes[0]) {
            // AF_FORMAT_FLOAT 是打包（interleaved）格式：planes[0] 即
            // interleaved float 数据，直接交给 dfrestore（其原生格式）。
            // 注意 planes[1..] 对打包格式是 NULL，绝不可逐声道取。
            if (s->df_process(s->df, (float *)planes[0], samples) != 0)
                MP_WARN(f, "[dfrestore] process error -> passthrough\n");

            // 第二级：Voice Clarity EQ + soft limiter（DFN3 降噪后）
            if (s->opts->eq_enabled) {
                int nch = mp_aframe_get_channels(s->in);
                if (nch > EQ_MAX_CH) nch = EQ_MAX_CH; // 防 monac>2 溢出
                int total = samples * nch;
                float *buf = (float *)planes[0];
                eq_process(s->eq_chain, EQ_BANDS, nch, buf, total, s->eq_preamp);
                for (int i = 0; i < total; i++)
                    buf[i] = soft_limit(buf[i]);
            }
        }
    }

    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, s->in));
    s->in = NULL;
}

static void reset(struct mp_filter *f)
{
    struct priv *s = f->priv;
    if (s->df && s->df_reset)
        s->df_reset(s->df);
    TA_FREEP(&s->in);
}

static void destroy(struct mp_filter *f)
{
    struct priv *s = f->priv;
    unload(s);
    TA_FREEP(&s->in);
    // 注：不要在这里调 mp_filter_free_children —— filter_destructor 会
    // 在 destroy 之后自己调用它，重复释放会导致 filter graph 状态损坏
    // （上游 af_rubberband/scaletempo 同样只在 destroy 清理自有资源）。
}

static const struct mp_filter_info af_dfrestore_filter = {
    .name = "dfrestore",
    .priv_size = sizeof(struct priv),
    .process = process,
    .reset = reset,
    .destroy = destroy,
};

static struct mp_filter *af_dfrestore_create(struct mp_filter *parent,
                                             void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &af_dfrestore_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *s = f->priv;
    s->opts = talloc_steal(s, options);
    s->cur_format = talloc_steal(s, mp_aframe_create());
    s->out_pool = mp_aframe_pool_create(s);

    if (!load_lib(s, f)) {
        // 上游范式（af_rubberband）：create 失败直接返回 NULL，不手动
        // talloc_free(f)——filter_destructor 会正确清理，手动提前释放
        // 会与析构路径冲突。dlopen 失败时无资源需要 unload。
        MP_ERR(f, "[dfrestore] cannot load libdfrestore.so, filter disabled\n");
        return NULL;
    }

    struct mp_autoconvert *conv = mp_autoconvert_create(f);
    if (!conv)
        abort();

    // DeepFilterNet3 要求 float / 48kHz
    mp_autoconvert_add_afmt(conv, AF_FORMAT_FLOAT);
    mp_autoconvert_add_srate(conv, 48000);

    mp_pin_connect(conv->f->pins[0], f->ppins[0]);
    s->in_pin = conv->f->pins[1];

    return f;
}

#define OPT_BASE_STRUCT struct f_opts

const struct mp_user_filter_entry af_dfrestore = {
    .desc = {
        .description = "VoyTrace Restore: DeepFilterNet3 speech enhancement "
                       "(compressed audio artifacts)",
        .name = "dfrestore",
        .priv_size = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &(const OPT_BASE_STRUCT) {
            .atten_lim = 12.0,
            .pf_beta = 0.05,
            .eq_enabled = 1,   // Voice Clarity EQ 默认开（DFN3 后人声清晰度补偿）
        },
        .options = (const struct m_option[]) {
            {"atten_lim", OPT_FLOAT(atten_lim), M_RANGE(0, 100)},
            {"pf_beta", OPT_FLOAT(pf_beta), M_RANGE(0, 1)},
            {"eq", OPT_CHOICE(eq_enabled,
                {"off", 0}, {"on", 1}, {"no", 0}, {"yes", 1})},
            {0}
        },
    },
    .create = af_dfrestore_create,
};
