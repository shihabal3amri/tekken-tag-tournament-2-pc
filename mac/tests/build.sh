#!/usr/bin/env bash
# Build the renderer tests against mac/deps/install/{dxvk,vkd3d} into mac/build/renderer/tests.
#   mac/tests/build.sh            then: mac/build/renderer/tests/d3d11_smoke [--window N [--no-vsync]]
# ARCH=x86_64 works on an Intel Mac (or with x86_64 SDL3 in TTT2_MAC_LIBDIR/TTT2_MAC_INCDIR).
set -euo pipefail
mac_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
repo=$(cd -- "$mac_root/.." && pwd)
arch=${ARCH:-$(uname -m)}
brew_prefix=$(brew --prefix 2>/dev/null || echo /opt/homebrew)
libdir=${TTT2_MAC_LIBDIR:-$brew_prefix/lib}
incdir=${TTT2_MAC_INCDIR:-$brew_prefix/include}
dxvk=${TTT2_DXVK_PREFIX:-$mac_root/deps/install/dxvk}
vkd3d=${TTT2_VKD3D_PREFIX:-$mac_root/deps/install/vkd3d}
out=$mac_root/build/renderer/tests
mkdir -p "$out"

# The same include order and defines the game's renderer object uses (port/CMakeLists.txt, Linux branch).
flags=(-arch "$arch" -std=c++20 -O2 -g
    -I"$dxvk/include/dxvk" -I"$dxvk/include/dxvk/directx" -I"$dxvk/include/dxvk/windows"
    -I"$repo/port/native/include" -I"$vkd3d/include/vkd3d"
    -I"$repo/port/src/runtime" -I"$repo/port/src/runtime/kernel"
    -isystem "$incdir")
libs=(-L"$dxvk/lib" -L"$vkd3d/lib" -L"$libdir" -ldxvk_d3d11 -ldxvk_dxgi -lvkd3d-shader -lSDL3
    -Wl,-rpath,"$dxvk/lib" -Wl,-rpath,"$vkd3d/lib")

clang++ "${flags[@]}" "$mac_root/tests/d3d11_smoke.cpp" "$repo/port/src/runtime/native_d3dcompiler.cpp" \
    "${libs[@]}" -o "$out/d3d11_smoke"
# shihab's own framebuffer-reuse test (port/tools), unchanged.
clang++ "${flags[@]}" "$repo/port/tools/d3d11_framebuffer_reuse_test.cpp" "$repo/port/src/runtime/native_d3dcompiler.cpp" \
    "${libs[@]}" -o "$out/d3d11_framebuffer_reuse_test"
echo "built: $out/d3d11_smoke $out/d3d11_framebuffer_reuse_test"
