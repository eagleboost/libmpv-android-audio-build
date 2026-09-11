#!/bin/bash
set -e

if [ -d "deps" ]; then
  sudo rm -r deps
fi
if [ -d "prefix" ]; then
  sudo rm -r prefix
fi

./download.sh
./patch.sh

# --- Inject audio_metrics into mpv source ---
python3 inject_audio_metrics.py
# --- End inject ---

# --- Build dfrestore (DeepFilterNet3 Restore, Rust) and inject af ---
export ANDROID_NDK_HOME="$PWD/sdk/android-sdk-linux/ndk/27.1.12297006"
pushd dfrestore
cargo ndk -t arm64-v8a -t armeabi-v7a -t x86 -t x86_64 -o android_libs build --release
popd
python3 inject_dfrestore.py
# 复制到打包暂存目录（bundle 末尾随 libmpv 一起打入 jar）
mkdir -p dfrestore_jni
for abi in arm64-v8a armeabi-v7a x86 x86_64; do
  mkdir -p dfrestore_jni/$abi
  cp -f dfrestore/android_libs/$abi/libdfrestore.so dfrestore_jni/$abi/
done
# --- End dfrestore ---

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

# dfrestore（DeepFilterNet3 Restore）随包
for abi in arm64-v8a armeabi-v7a x86 x86_64; do
  cp -f ../../dfrestore_jni/$abi/libdfrestore.so app/build/outputs/apk/release/lib/$abi/
done

cd app/build/outputs/apk/release

zip -r default-arm64-v8a.jar      lib/arm64-v8a/*.so
zip -r default-armeabi-v7a.jar    lib/armeabi-v7a/*.so
zip -r default-x86.jar            lib/x86/*.so
zip -r default-x86_64.jar         lib/x86_64/*.so

md5sum *.jar
