#!/usr/bin/env bash
# vkd3d-shader 1.17 (HLSL -> DXBC at run time, used by port/src/runtime/native_d3dcompiler.cpp)
# for macOS, same version and configure switches as port/tools/build_android_vkd3d_shader.sh.
#
# The release tarball is tag vkd3d-1.17 (f732fb78) plus the files autoreconf/widl/the SPIR-V
# grammar generator make; every source file is identical to the git tag (checked with diff -r),
# so macOS needs neither widl (Wine) nor perl JSON. bison >= 3 comes from Homebrew (keg-only).
#
#   mac/build_vkd3d.sh                 arm64 (macOS 14.4)
#   ARCH=x86_64 mac/build_vkd3d.sh     the Intel slice (macOS 13.0; Intel Mac, or cross from arm64)
# Output: mac/deps/install-$ARCH/vkd3d/{include/vkd3d/vkd3d_shader.h, lib/libvkd3d-shader.dylib}
set -euo pipefail

mac_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
arch=${ARCH:-arm64}
deps=$mac_root/deps
version=1.17
tarball_sha256=bc61cb9e84d5045cbcaffbdd707940d399d8bf62874663dfe5809a0bfb87e9b6
src=$deps/vkd3d-$version
build=$mac_root/build/renderer/vkd3d-$arch
prefix=${TTT2_VKD3D_PREFIX:-$deps/install-$arch/vkd3d}
default_minos=13.0; [[ $arch == arm64 ]] && default_minos=14.4
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-$default_minos}
brew_prefix=$(brew --prefix 2>/dev/null || echo /opt/homebrew)
jobs=${TTT2_BUILD_JOBS:-$(sysctl -n hw.ncpu)}

if [[ ! -f $src/configure ]]; then
    mkdir -p "$deps/vkd3d-tarball"
    tarball=$deps/vkd3d-tarball/vkd3d-$version.tar.xz
    [[ -f $tarball ]] || curl -fL -o "$tarball" "https://dl.winehq.org/vkd3d/source/vkd3d-$version.tar.xz"
    echo "$tarball_sha256  $tarball" | shasum -a 256 -c -
    tar -xf "$tarball" -C "$deps"
fi

bison_dir=$brew_prefix/opt/bison/bin
[[ -x $bison_dir/bison ]] || { echo "Need Homebrew bison (brew install bison)" >&2; exit 1; }
spirv_include=$brew_prefix/include
[[ -f $spirv_include/spirv/unified1/spirv.h ]] || { echo "Need Homebrew spirv-headers" >&2; exit 1; }

host=$arch-apple-darwin
[[ $arch == arm64 ]] && host=aarch64-apple-darwin
mkdir -p "$build"
cd "$build"
if [[ ! -f Makefile ]]; then
    PATH="$bison_dir:$PATH" \
    CC="clang -arch $arch" \
    CFLAGS="${CFLAGS:--O2 -g} -ffile-prefix-map=$src=. -ffile-prefix-map=$build=build" \
    CPPFLAGS="-I$spirv_include" SONAME_LIBVULKAN=libvulkan.1.dylib \
        "$src/configure" --host="$host" --prefix="$prefix" \
        --disable-maintainer-mode --disable-tests --disable-demos --disable-doxygen-doc \
        --with-xcb=no --with-opengl=no --with-ncurses=no \
        > "$build/configure.log" 2>&1 || { tail -30 "$build/configure.log"; exit 1; }
fi
PATH="$bison_dir:$PATH" make include/private/vkd3d_version.h > "$build/generate.log" 2>&1 \
    || { tail -20 "$build/generate.log"; exit 1; }
PATH="$bison_dir:$PATH" make -j "$jobs" libvkd3d-shader.la > "$build/build-shader.log" 2>&1 \
    || { tail -40 "$build/build-shader.log"; exit 1; }

# Install the shader library and its public headers only (what the game links).
mkdir -p "$prefix/lib" "$prefix/include/vkd3d"
cp -f "$build"/.libs/libvkd3d-shader.*.dylib "$prefix/lib/"
dylib=$(cd "$prefix/lib" && ls libvkd3d-shader.*.dylib | head -1)
ln -sf "$dylib" "$prefix/lib/libvkd3d-shader.dylib"
install_name_tool -id "@rpath/$dylib" "$prefix/lib/$dylib"
cp -f "$src/include/vkd3d_shader.h" "$src/include/vkd3d_types.h" "$prefix/include/vkd3d/"
printf 'vkd3d-shader %s (%s): %s\n' "$version" "$arch" "$prefix/lib/$dylib"
