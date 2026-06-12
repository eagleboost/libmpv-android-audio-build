#!/usr/bin/env python3
import os, sys

MPV_DIR = os.path.join(os.path.dirname(__file__), '..', 'deps', 'mpv')
MPV_DIR = os.path.abspath(MPV_DIR)

if not os.path.isdir(MPV_DIR):
    print(f"[audio_metrics] ERROR: {MPV_DIR} not found")
    sys.exit(1)

src_dir = os.path.join(os.path.dirname(__file__), 'audio_metrics')

# 1. Copy source files
for f in ['audio_metrics.h', 'audio_metrics.c']:
    src = os.path.join(src_dir, f)
    dst = os.path.join(MPV_DIR, 'audio', f)
    with open(src, 'r') as fh:
        content = fh.read()
    with open(dst, 'w') as fh:
        fh.write(content)
    print(f"[audio_metrics] Copied audio/{f}")

# 2. Add to wscript_build.py
ws = os.path.join(MPV_DIR, 'wscript_build.py')
with open(ws, 'r') as fh:
    content = fh.read()
marker = '( "audio/format.c" ),'
if 'audio_metrics' not in content:
    content = content.replace(marker, marker + '\n        ( "audio/audio_metrics.c" ),')
    with open(ws, 'w') as fh:
        fh.write(content)
    print("[audio_metrics] Added to wscript_build.py")

# 3. Patch ao.c
ao = os.path.join(MPV_DIR, 'audio', 'out', 'ao.c')
with open(ao, 'r') as fh:
    content = fh.read()

if 'audio_metrics' not in content:
    content = content.replace(
        '#include <stdio.h>',
        '#include <stdio.h>\n#include "audio/audio_metrics.h"'
    )

    hook = """    if (data && data[0] && num_samples > 0) {
        audio_metrics_feed(data[0], num_samples, ao->channels.num, af_fmt_from_planar(ao->format));
    }
"""

    old = 'void ao_post_process_data(struct ao *ao, void **data, int num_samples)\n{\n'
    new = 'void ao_post_process_data(struct ao *ao, void **data, int num_samples)\n{\n' + hook
    if old in content:
        content = content.replace(old, new, 1)
    else:
        old2 = 'void ao_post_process_data(struct ao *ao, void **data, int num_samples)\n{'
        new2 = 'void ao_post_process_data(struct ao *ao, void **data, int num_samples)\n{\n' + hook
        if old2 in content:
            content = content.replace(old2, new2, 1)
        else:
            print("[audio_metrics] WARNING: Could not find ao_post_process_data in ao.c")

    with open(ao, 'w') as fh:
        fh.write(content)
    print("[audio_metrics] Patched audio/out/ao.c")

# 4. Patch client.h
client_h = os.path.join(MPV_DIR, 'libmpv', 'client.h')
with open(client_h, 'r') as fh:
    content = fh.read()

if 'audio_metrics_init' not in content:
    content = content.replace(
        'MPV_EXPORT void mpv_wakeup(mpv_handle *ctx);',
        """MPV_EXPORT void mpv_wakeup(mpv_handle *ctx);

typedef struct {
    double bass;
    double mid;
    double treble;
    double volume;
    bool beat;
    uint64_t frame_count;
} mpv_audio_metrics_t;

MPV_EXPORT void audio_metrics_init(int fft_size, int sample_rate);
MPV_EXPORT const mpv_audio_metrics_t *audio_metrics_get(void);
MPV_EXPORT void audio_metrics_reset(void);
MPV_EXPORT void audio_metrics_destroy(void);
"""
    )
    with open(client_h, 'w') as fh:
        fh.write(content)
    print("[audio_metrics] Patched libmpv/client.h")

# 5. Add to mpv.def
mpv_def = os.path.join(MPV_DIR, 'libmpv', 'mpv.def')
with open(mpv_def, 'r') as fh:
    content = fh.read()
if 'audio_metrics_init' not in content:
    content += "audio_metrics_init\naudio_metrics_get\naudio_metrics_reset\naudio_metrics_destroy\n"
    with open(mpv_def, 'w') as fh:
        fh.write(content)
    print("[audio_metrics] Patched libmpv/mpv.def")

print("[audio_metrics] Injection complete.")
