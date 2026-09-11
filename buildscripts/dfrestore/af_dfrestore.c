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

struct f_opts {
    float atten_lim;
    float pf_beta;
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
};

static void unload(struct priv *s)
{
    if (s->df && s->df_free)
        s->df_free(s->df);
    s->df = NULL;
    if (s->lib)
        dlclose(s->lib);
    s->lib = NULL;
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
    return true;
}

static void process(struct mp_filter *f)
{
    struct priv *s = f->priv;

    if (!mp_pin_in_needs_data(f->ppins[1]))
        return;

    struct mp_frame frame = mp_pin_out_read(s->in_pin);
    if (!frame.type)
        return; // no input yet

    if (frame.type != MP_FRAME_AUDIO && frame.type != MP_FRAME_EOF) {
        MP_ERR(f, "unexpected frame type\n");
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }

    if (frame.type == MP_FRAME_EOF) {
        mp_pin_in_write(f->ppins[1], frame);
        mp_pin_out_repeat_eof(s->in_pin);
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

    int nch = mp_aframe_get_channels(s->in);
    int samples = mp_aframe_get_size(s->in);

    if (s->df && samples > 0) {
        uint8_t **planes = mp_aframe_get_data_rw(s->in);
        if (planes && planes[0]) {
            // interleaved 处理（保证多声道 FIFO 时序一致）
            float *tmp = malloc((size_t)samples * nch * sizeof(float));
            if (tmp) {
                for (int ch = 0; ch < nch; ch++) {
                    float *src = (float *)planes[ch];
                    for (int i = 0; i < samples; i++)
                        tmp[i * nch + ch] = src[i];
                }
                if (s->df_process(s->df, tmp, samples) != 0)
                    MP_WARN(f, "dfrestore: process error -> passthrough\n");
                for (int ch = 0; ch < nch; ch++) {
                    float *dst = (float *)planes[ch];
                    for (int i = 0; i < samples; i++)
                        dst[i] = tmp[i * nch + ch];
                }
                free(tmp);
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
    mp_filter_free_children(f);
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
        MP_ERR(f, "dfrestore: cannot load libdfrestore.so\n");
        unload(s);
        talloc_free(f);
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
        },
        .options = (const struct m_option[]) {
            {"atten_lim", OPT_FLOAT(atten_lim), M_RANGE(0, 100)},
            {"pf_beta", OPT_FLOAT(pf_beta), M_RANGE(0, 1)},
            {0}
        },
    },
    .create = af_dfrestore_create,
};
