# --------------------------------------------------

if [ ! -f "deps" ]; then
  sudo rm -r deps
fi
if [ ! -f "prefix" ]; then
  sudo rm -r prefix
fi

./download.sh
./patch.sh

# --- Inject audio_metrics into mpv source ---
MPV_DIR="deps/mpv"
if [ -d "$MPV_DIR" ]; then
  echo "[audio_metrics] Injecting into mpv source..."
  cp audio_metrics/audio_metrics.h "$MPV_DIR/audio/audio_metrics.h"
  cp audio_metrics/audio_metrics.c "$MPV_DIR/audio/audio_metrics.c"

  # Add audio_metrics.c to wscript_build.py sources
  sed -i '/"( "audio\/format.c" )"/a\        ( "audio/audio_metrics.c" ),' "$MPV_DIR/wscript_build.py"

  # Add include to ao.c
  sed -i 's/#include <stdio.h>/#include <stdio.h>\n#include "audio\/audio_metrics.h"/' "$MPV_DIR/audio/out/ao.c"

  # Add feed call at the start of ao_post_process_data
  sed -i '/^void ao_post_process_data(struct ao \*ao, void \*\*data, int num_samples)$/,/^{/{
    /^{/a\
    if (data \\&\\& data[0] \\&\\& num_samples > 0 \\&\\& !af_fmt_is_planar(ao->format)) {\
        int fmt = af_fmt_from_planar(ao->format);\
        if (fmt == AF_FORMAT_FLOAT) {\
            audio_metrics_feed_float((const float *)data[0], num_samples, ao->channels.num);\
        } else if (fmt == AF_FORMAT_S16) {\
            audio_metrics_feed_s16((const int16_t *)data[0], num_samples, ao->channels.num);\
        }\
    }
  }' "$MPV_DIR/audio/out/ao.c"

  # Add declarations to client.h
  sed -i '/MPV_EXPORT void mpv_wakeup(mpv_handle \*ctx);/a\
\
MPV_EXPORT void audio_metrics_init(int fft_size, int sample_rate);\
MPV_EXPORT const audio_metrics_t *audio_metrics_get(void);\
MPV_EXPORT void audio_metrics_reset(void);\
MPV_EXPORT void audio_metrics_destroy(void);' "$MPV_DIR/libmpv/client.h"

  # Add to mpv.def
  echo "audio_metrics_init" >> "$MPV_DIR/libmpv/mpv.def"
  echo "audio_metrics_get" >> "$MPV_DIR/libmpv/mpv.def"
  echo "audio_metrics_reset" >> "$MPV_DIR/libmpv/mpv.def"
  echo "audio_metrics_destroy" >> "$MPV_DIR/libmpv/mpv.def"

  echo "[audio_metrics] Injection complete."
else
  echo "[audio_metrics] WARNING: mpv source dir not found, skipping."
fi
# --- End inject ---

./build.sh

zip -r debug-symbols-default.zip prefix/*/lib

./sdk/android-sdk-linux/ndk/27.1.12297006/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-all prefix/arm64-v8a/usr/local/lib/libmpv.so
./sdk/android-sdk-linux/ndk/27.1.12297006/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-all prefix/armeabi-v7a/usr/local/lib/libmpv.so
./sdk/android-sdk-linux/ndk/27.1.12297006/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-all prefix/x86/usr/local/lib/libmpv.so
./sdk/android-sdk-linux/ndk/27.1.12297006/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-all prefix/x86_64/usr/local/lib/libmpv.so

# --------------------------------------------------

cd deps/media-kit-android-helper

sudo chmod +x gradlew
./gradlew assembleRelease

unzip -o app/build/outputs/apk/release/app-release.apk -d app/build/outputs/apk/release

cp ../../prefix/arm64-v8a/usr/local/lib/libmpv.so      app/build/outputs/apk/release/lib/arm64-v8a
cp ../../prefix/armeabi-v7a/usr/local/lib/libmpv.so    app/build/outputs/apk/release/lib/armeabi-v7a
cp ../../prefix/x86/usr/local/lib/libmpv.so            app/build/outputs/apk/release/lib/x86
cp ../../prefix/x86_64/usr/local/lib/libmpv.so         app/build/outputs/apk/release/lib/x86_64

cd app/build/outputs/apk/release

zip -r default-arm64-v8a.jar      lib/arm64-v8a/*.so
zip -r default-armeabi-v7a.jar    lib/armeabi-v7a/*.so
zip -r default-x86.jar            lib/x86/*.so
zip -r default-x86_64.jar         lib/x86_64/*.so

md5sum *.jar
