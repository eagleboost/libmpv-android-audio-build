#!/usr/bin/env python3
"""把 af_dfrestore（DeepFilterNet3 Restore 滤镜）注入 mpv 源码树。

注入内容：
1. 复制 af_dfrestore.c 到 mpv 的 audio/filter/
2. wscript_build.py 注册源文件
3. filters/user_filters.c 注册滤镜条目（af 数组）

配合 bundle.sh 里前置的 Rust 步骤：android_libs/<abi>/libdfrestore.so
已复制到 libmpv/src/main/jniLibs/<abi>/，运行时由 af 用 dlopen 加载。
"""
import os, re, sys

MPV_DIR = os.path.abspath(os.path.join(os.getcwd(), 'deps', 'mpv'))
if not os.path.isdir(MPV_DIR):
    print(f"[dfrestore] ERROR: {MPV_DIR} not found")
    sys.exit(1)

src = os.path.join(os.path.dirname(__file__), 'af_dfrestore.c')
dst = os.path.join(MPV_DIR, 'audio', 'filter', 'af_dfrestore.c')
with open(src, 'r') as fh:
    content = fh.read()
with open(dst, 'w') as fh:
    fh.write(content)
print("[dfrestore] Copied audio/filter/af_dfrestore.c")

# 2. wscript_build.py
ws = os.path.join(MPV_DIR, 'wscript_build.py')
with open(ws, 'r') as fh:
    content = fh.read()
if 'af_dfrestore' not in content:
    marker = '( "audio/filter/af_format.c" ),'
    if marker not in content:
        # 兜底：找任意 audio/filter 条目
        m = re.search(r'\(\s*"audio/filter/[a-z0-9_]+\.c"\s*\),', content)
        if not m:
            print("[dfrestore] ERROR: no audio/filter entry in wscript_build.py")
            sys.exit(1)
        marker = m.group(0)
    content = content.replace(marker, marker + f'\n        ( "audio/filter/af_dfrestore.c" ),', 1)
    with open(ws, 'w') as fh:
        fh.write(content)
    print("[dfrestore] Added to wscript_build.py")

# 3. filters/user_filters.c 注册
uw = os.path.join(MPV_DIR, 'filters', 'user_filters.c')
with open(uw, 'r') as fh:
    content = fh.read()
if 'af_dfrestore' not in content:
    # extern 声明：挂在 scaletempo 的 extern 之后
    m = re.search(r'extern const struct mp_user_filter_entry af_scaletempo;', content)
    if not m:
        # 兜底：任意 af_* extern
        m = re.search(r'extern const struct mp_user_filter_entry af_[a-z0-9_]+;', content)
    if not m:
        print("[dfrestore] ERROR: no af extern found in user_filters.c")
        sys.exit(1)
    anchor = m.group(0)
    content = content.replace(
        anchor,
        anchor + '\nextern const struct mp_user_filter_entry af_dfrestore;',
        1,
    )
    # 数组条目：挂在 &af_scaletempo 之后
    m2 = re.search(r'&af_scaletempo,', content)
    if not m2:
        m2 = re.search(r'&af_[a-z0-9_]+,', content)
    if not m2:
        print("[dfrestore] ERROR: no af array entry found in user_filters.c")
        sys.exit(1)
    a2 = m2.group(0)
    content = content.replace(a2, a2 + '\n    &af_dfrestore,', 1)
    with open(uw, 'w') as fh:
        fh.write(content)
    print("[dfrestore] Registered in filters/user_filters.c")

print("[dfrestore] done")
