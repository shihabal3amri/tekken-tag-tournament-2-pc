#!/usr/bin/env bash
# FFmpeg n7.1 for the game's XMA audio on macOS (arm64; ARCH=x86_64 for the Intel slice): download the official tag and run configure
# the way port/third_party/ffmpeg_xma/CMakeLists.txt describes (XMA decoding only), with the
# Android arm64 build's extra --disable-inline-asm (port/tools/build_android_ffmpeg_xma.sh).
# Nothing is compiled here: CMake (port/third_party/ffmpeg_xma) builds the C files that
# config.mak selects, and patches wmaprodec.c at build time (the source tree stays as downloaded).
#   mac/deps/FFmpeg-n7.1       FFMPEG_SRC   (git tag n7.1 = b08d7969c550a804a59511c7b83f2dd8cc0499b8)
#   mac/deps/ffmpeg-build-mac-$ARCH  FFMPEG_BUILD (config.h, config.mak, avconfig.h, codec lists);
#                              arm64 targets macOS 14.4, x86_64 13.0
# --verify also builds the ttt2_ffmpeg archive and the XMA decode tests in mac/build/platform.
set -euo pipefail
mac=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(dirname "$mac")
src=$mac/deps/FFmpeg-n7.1
arch=${ARCH:-arm64}
build=$mac/deps/ffmpeg-build-mac-$arch
ff_arch=aarch64 minos=14.4 cross=()
if [[ $arch != arm64 ]]; then
    ff_arch=$arch minos=13.0
fi
[[ $(uname -m) == "$arch" ]] || cross=(--enable-cross-compile "--extra-cflags=-arch $arch" "--extra-ldflags=-arch $arch")
expected=b08d7969c550a804a59511c7b83f2dd8cc0499b8

if [[ ! -d $src/.git ]]; then
    mkdir -p "$mac/deps"
    git clone --depth 1 --branch n7.1 https://git.ffmpeg.org/ffmpeg.git "$src" ||
        git clone --depth 1 --branch n7.1 https://github.com/FFmpeg/FFmpeg.git "$src"
fi
if [[ $(git -C "$src" rev-parse HEAD) != "$expected" ]]; then
    echo "FFmpeg source at $src is not n7.1 ($expected)" >&2
    exit 1
fi
if [[ -n $(git -C "$src" status --porcelain) ]]; then
    echo "FFmpeg source at $src has local changes; CMake patches wmaprodec.c itself" >&2
    exit 1
fi

if [[ ! -f $build/ffbuild/config.mak || ${1:-} == --reconfigure ]]; then
    rm -rf "$build"
    mkdir -p "$build"
    (
        cd "$build"
        "$src/configure" --target-os=darwin --arch=$ff_arch --cc=clang --cxx=clang++ \
            --extra-cflags="-mmacosx-version-min=$minos" --extra-ldflags="-mmacosx-version-min=$minos" \
            ${cross[@]+"${cross[@]}"} \
            --enable-static --disable-shared --disable-everything \
            --disable-programs --disable-doc --disable-avdevice \
            --disable-avformat --disable-swresample --disable-swscale \
            --disable-postproc --disable-avfilter --disable-network \
            --disable-asm --disable-inline-asm --disable-debug \
            --disable-autodetect --enable-decoder=wmapro,xma1,xma2 \
            --enable-parser=xma > "$build/configure.log" 2>&1
    ) || { tail -20 "$build/configure.log" >&2; exit 1; }
fi
for feature in CONFIG_WMAPRO_DECODER CONFIG_XMA1_DECODER CONFIG_XMA2_DECODER CONFIG_XMA_PARSER; do
    grep -q "^$feature=yes$" "$build/ffbuild/config.mak" ||
        { echo "FFmpeg configuration is missing $feature" >&2; exit 1; }
done
echo "FFMPEG_SRC=$src"
echo "FFMPEG_BUILD=$build"

if [[ ${1:-} == --verify ]]; then
    out=$mac/build/platform
    cmake -S "$repo/port/third_party/ffmpeg_xma/test" -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DFFMPEG_SRC="$src" -DFFMPEG_BUILD="$build" -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
    cmake --build "$out" -j 18
fi
