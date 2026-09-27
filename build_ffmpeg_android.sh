#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
FFMPEG_VERSION="7.1.1"
SOURCE="$ROOT/.build/ffmpeg-$FFMPEG_VERSION"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
NDK_VERSION="27.2.12479018"
NDK="$SDK/ndk/$NDK_VERSION"
HOST="linux-x86_64"

test -n "$SDK" || { echo 'ANDROID_HOME or ANDROID_SDK_ROOT is required' >&2; exit 1; }
test -x "$NDK/toolchains/llvm/prebuilt/$HOST/bin/clang" || { echo "NDK $NDK_VERSION missing" >&2; exit 1; }
command -v make >/dev/null
command -v nasm >/dev/null
mkdir -p "$ROOT/decoder/src/main/jniLibs" "$ROOT/decoder/src/main/ffmpeg-headers" "$ROOT/.build"

if [[ ! -d "$SOURCE" ]]; then
  git clone --depth 1 --branch "n$FFMPEG_VERSION" https://github.com/FFmpeg/FFmpeg.git "$SOURCE"
fi
for ABI in arm64-v8a armeabi-v7a x86 x86_64; do
  case "$ABI" in
    arm64-v8a) ARCH=aarch64; CPU=armv8-a; PREFIX=aarch64-linux-android ;;
    armeabi-v7a) ARCH=arm; CPU=armv7-a; PREFIX=armv7a-linux-androideabi ;;
    x86) ARCH=x86; CPU=i686; PREFIX=i686-linux-android ;;
    x86_64) ARCH=x86_64; CPU=x86-64; PREFIX=x86_64-linux-android ;;
  esac
  BUILD="$ROOT/.build/$ABI"
  mkdir -p "$BUILD" "$ROOT/decoder/src/main/jniLibs/$ABI"
  cd "$SOURCE"
  make distclean >/dev/null 2>&1 || true
  PATH="$NDK/toolchains/llvm/prebuilt/$HOST/bin:$PATH" \
  ./configure \
    --prefix="$BUILD/install" --target-os=android --arch="$ARCH" --cpu="$CPU" \
    --cross-prefix="$NDK/toolchains/llvm/prebuilt/$HOST/bin/$PREFIX-" \
    --sysroot="$NDK/toolchains/llvm/prebuilt/$HOST/sysroot" \
    --cc="$NDK/toolchains/llvm/prebuilt/$HOST/bin/${PREFIX}${ANDROID_API:-24}-clang" \
    --cxx="$NDK/toolchains/llvm/prebuilt/$HOST/bin/${PREFIX}${ANDROID_API:-24}-clang++" \
    --nm="$NDK/toolchains/llvm/prebuilt/$HOST/bin/llvm-nm" \
    --ar="$NDK/toolchains/llvm/prebuilt/$HOST/bin/llvm-ar" \
    --ranlib="$NDK/toolchains/llvm/prebuilt/$HOST/bin/llvm-ranlib" \
    --strip="$NDK/toolchains/llvm/prebuilt/$HOST/bin/llvm-strip" \
    --enable-cross-compile --enable-pic --enable-shared --disable-static \
    --disable-programs --disable-doc --disable-debug --disable-everything \
    --disable-avdevice --disable-avfilter --disable-network --disable-autodetect \
    --disable-gpl --disable-nonfree --disable-symver --enable-small \
    --enable-decoder=mpeg4 --enable-parser=mpeg4video --enable-swscale \
    --extra-cflags='-O2 -fPIC -DANDROID' --extra-ldflags='-Wl,-z,max-page-size=16384'
  make -j2
  make install
  rm -rf "$ROOT/decoder/src/main/ffmpeg-headers"/*
  cp -R "$BUILD/install/include/." "$ROOT/decoder/src/main/ffmpeg-headers/"
  cp -L "$BUILD/install/lib/libavutil.so" "$ROOT/decoder/src/main/jniLibs/$ABI/libavutil.so"
  cp -L "$BUILD/install/lib/libavcodec.so" "$ROOT/decoder/src/main/jniLibs/$ABI/libavcodec.so"
  cp -L "$BUILD/install/lib/libswscale.so" "$ROOT/decoder/src/main/jniLibs/$ABI/libswscale.so"
  for lib in "$ROOT/decoder/src/main/jniLibs/$ABI"/*.so; do
    patchelf --set-soname "$(basename "$lib")" "$lib"
    while IFS= read -r needed; do
      case "$needed" in
        libavutil.so.*) patchelf --replace-needed "$needed" libavutil.so "$lib" ;;
        libavcodec.so.*) patchelf --replace-needed "$needed" libavcodec.so "$lib" ;;
        libswscale.so.*) patchelf --replace-needed "$needed" libswscale.so "$lib" ;;
      esac
    done < <(patchelf --print-needed "$lib")
  done
done

echo "FFmpeg $FFMPEG_VERSION MPEG-4 Part 2 binaries built for all four Android ABIs."
