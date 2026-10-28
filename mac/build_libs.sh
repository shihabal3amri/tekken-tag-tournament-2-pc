#!/usr/bin/env bash
# The game's libraries, built from source for one architecture with an explicit deployment target
# (Homebrew bottles target the macOS they were built on, and Homebrew on Apple Silicon is arm64 only):
#   SDL 3.4.18, OpenSSL 3.6.5, Vulkan-Headers + Vulkan-Loader 1.4.363 (Homebrew's versions),
#   and MoltenVK 1.4.2 (Khronos' release build, universal, minos 12.0).
#
#   mac/build_libs.sh                  arm64, macOS 14.4 (os_sync_wait_on_address, host_posix_compat.h)
#   ARCH=x86_64 mac/build_libs.sh      the Intel slice, macOS 13.0 (Intel Macs on Ventura); native on
#                                      an Intel Mac or cross from arm64
# Output: mac/deps/install-$ARCH/{sdl3,openssl,vulkan,moltenvk}; then, with the same ARCH,
# mac/build_vkd3d.sh, mac/build_dxvk.sh, mac/build_ffmpeg.sh, mac/build_ppc.sh, mac/build_game.sh.
# MACOSX_DEPLOYMENT_TARGET overrides the deployment target.
set -euo pipefail

mac_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
arch=${ARCH:-arm64}
default_minos=13.0; [[ $arch == arm64 ]] && default_minos=14.4
minos=${MACOSX_DEPLOYMENT_TARGET:-$default_minos}
export MACOSX_DEPLOYMENT_TARGET=$minos
deps=$mac_root/deps
downloads=$deps/downloads
sources=$deps/src-libs
prefix=$deps/install-$arch
build=$mac_root/build/libs-$arch
jobs=${TTT2_BUILD_JOBS:-$(sysctl -n hw.ncpu)}
mkdir -p "$downloads" "$sources" "$prefix" "$build"

# name, URL, SHA-256 (checked on download)
fetch() {
    local file=$downloads/$1
    [[ -f $file ]] || curl -fL -o "$file" "$2"
    echo "$3  $file" | shasum -a 256 -c - > /dev/null || { echo "checksum mismatch: $file" >&2; exit 1; }
}
unpack() { # archive, directory it makes
    [[ -d $sources/$2 ]] || tar -xf "$downloads/$1" -C "$sources"
}
cmake_build() { # name, source dir, extra args...
    local name=$1 src=$2; shift 2
    cmake -S "$src" -B "$build/$name" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$minos" \
        -DCMAKE_INSTALL_PREFIX="$prefix/$name" "$@" > "$build/$name.configure.log" 2>&1 \
        || { tail -30 "$build/$name.configure.log"; exit 1; }
    cmake --build "$build/$name" -j "$jobs" > "$build/$name.build.log" 2>&1 \
        || { tail -30 "$build/$name.build.log"; exit 1; }
    cmake --install "$build/$name" > /dev/null
}

fetch SDL3-3.4.18.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz \
    9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
fetch openssl-3.6.5.tar.gz https://github.com/openssl/openssl/releases/download/openssl-3.6.5/openssl-3.6.5.tar.gz \
    a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98
fetch Vulkan-Headers-1.4.363.tar.gz https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.363.tar.gz \
    cbaf687d3c59b9666fe080e7f8396b6b4f4d344a768ee6b337c59852f4526b68
fetch Vulkan-Loader-1.4.363.tar.gz https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/v1.4.363.tar.gz \
    cc520304a4d8de9a6a1699553d6090aef0c25f87ef6cbc16b7c84ebd99093264
fetch MoltenVK-macos-1.4.2.tar https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar \
    f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e

# SDL3: shared, with the Vulkan window support DXVK presents through.
unpack SDL3-3.4.18.tar.gz SDL3-3.4.18
cmake_build sdl3 "$sources/SDL3-3.4.18" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF \
    -DSDL_EXAMPLES=OFF -DSDL_VULKAN=ON
echo "SDL3 3.4.18 ($arch): $prefix/sdl3"

# OpenSSL: shared libssl/libcrypto only (community_tls.cpp).
unpack openssl-3.6.5.tar.gz openssl-3.6.5
target=darwin64-$arch-cc; [[ $arch == arm64 ]] && target=darwin64-arm64-cc
mkdir -p "$build/openssl"
(
    cd "$build/openssl"
    if [[ ! -f Makefile ]]; then
        "$sources/openssl-3.6.5/Configure" "$target" no-tests shared "-mmacosx-version-min=$minos" \
            --prefix="$prefix/openssl" --openssldir="$prefix/openssl/ssl" --libdir=lib \
            > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    fi
    make -j "$jobs" > build.log 2>&1 || { tail -30 build.log; exit 1; }
    make install_sw > install.log 2>&1 || { tail -30 install.log; exit 1; }
)
echo "OpenSSL 3.6.5 ($arch): $prefix/openssl"

# Vulkan loader (SDL loads libvulkan.1.dylib; the .app points it at the bundled MoltenVK).
unpack Vulkan-Headers-1.4.363.tar.gz Vulkan-Headers-1.4.363
unpack Vulkan-Loader-1.4.363.tar.gz Vulkan-Loader-1.4.363
cmake_build vulkan-headers "$sources/Vulkan-Headers-1.4.363" -DVULKAN_HEADERS_ENABLE_MODULE=OFF \
    -DCMAKE_INSTALL_PREFIX="$prefix/vulkan"
cmake_build vulkan "$sources/Vulkan-Loader-1.4.363" -DCMAKE_PREFIX_PATH="$prefix/vulkan" \
    -DUPDATE_DEPS=OFF -DBUILD_TESTS=OFF
echo "Vulkan-Loader 1.4.363 ($arch): $prefix/vulkan"

# MoltenVK: Khronos' universal release build, as shipped.
unpack MoltenVK-macos-1.4.2.tar MoltenVK
mkdir -p "$prefix/moltenvk/lib"
cp -f "$sources/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" \
      "$sources/MoltenVK/MoltenVK/dynamic/dylib/macOS/MoltenVK_icd.json" "$prefix/moltenvk/lib/"
echo "MoltenVK 1.4.2 (universal): $prefix/moltenvk/lib/libMoltenVK.dylib"
