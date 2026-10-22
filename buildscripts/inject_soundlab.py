#!/usr/bin/env python3
"""把 af_soundlab（Sound Lab 参数均衡器滤镜）注入 mpv 源码树。

注入内容：
1. 复制 af_soundlab.c 到 mpv 的 audio/filter/
2. wscript_build.py 注册源文件
3. filters/user_filters.c 注册滤镜条目（af 数组）

与 inject_dfrestore.py 同范式；af_soundlab 是纯 C 滤镜，无 Rust 依赖。
"""
import os, re, sys

MPV_DIR = os.path.abspath(os.path.join(os.getcwd(), 'deps', 'mpv'))
if not os.path.isdir(MPV_DIR):
    print(f"[soundlab] ERROR: {MPV_DIR} not found")
    sys.exit(1)

src = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'soundlab', 'af_soundlab.c')
dst = os.path.join(MPV_DIR, 'audio', 'filter', 'af_soundlab.c')
with open(src, 'r') as fh:
    content = fh.read()
with open(dst, 'w') as fh:
    fh.write(content)
print("[soundlab] Copied audio/filter/af_soundlab.c")

# 2. wscript_build.py
ws = os.path.join(MPV_DIR, 'wscript_build.py')
with open(ws, 'r') as fh:
    content = fh.read()
if 'af_soundlab' not in content:
    marker = '( "audio/filter/af_format.c" ),'
    if marker not in content:
        m = re.search(r'\(\s*"audio/filter/[a-z0-9_]+\.c"\s*\),', content)
        if not m:
            print("[soundlab] ERROR: no audio/filter entry in wscript_build.py")
            sys.exit(1)
        marker = m.group(0)
    content = content.replace(marker, marker + f'\n        ( "audio/filter/af_soundlab.c" ),', 1)
    with open(ws, 'w') as fh:
        fh.write(content)
    print("[soundlab] Added to wscript_build.py")

# 3. 注册
uh = os.path.join(MPV_DIR, 'filters', 'user_filters.h')
with open(uh, 'r') as fh:
    hcontent = fh.read()
if 'af_soundlab' not in hcontent:
    m = re.search(r'extern const struct mp_user_filter_entry af_dfrestore;', hcontent)
    if not m:
        m = re.search(r'extern const struct mp_user_filter_entry af_[a-z0-9_]+;', hcontent)
    if not m:
        print("[soundlab] ERROR: no af extern found in user_filters.h")
        sys.exit(1)
    anchor = m.group(0)
    hcontent = hcontent.replace(
        anchor,
        anchor + '\nextern const struct mp_user_filter_entry af_soundlab;',
        1,
    )
    with open(uh, 'w') as fh:
        fh.write(hcontent)
    print("[soundlab] Added extern to filters/user_filters.h")

uw = os.path.join(MPV_DIR, 'filters', 'user_filters.c')
with open(uw, 'r') as fh:
    content = fh.read()
if 'af_soundlab' not in content:
    m2 = re.search(r'&af_dfrestore,', content)
    if not m2:
        m2 = re.search(r'&af_[a-z0-9_]+,', content)
    if not m2:
        print("[soundlab] ERROR: no af array entry found in user_filters.c")
        sys.exit(1)
    a2 = m2.group(0)
    content = content.replace(a2, a2 + '\n    &af_soundlab,', 1)
    with open(uw, 'w') as fh:
        fh.write(content)
    print("[soundlab] Registered in af_list (user_filters.c)")
