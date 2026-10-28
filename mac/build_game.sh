#!/bin/bash
# Configure and build the macOS game executable: mac/build/game/tekken_recomp_mac.
#   mac/build_game.sh            build the game, arm64, macOS 14.4 (needs mac/out/ppc and, from
#                                mac/build_libs, build_vkd3d, build_dxvk, build_ffmpeg,
#                                mac/deps/install-arm64 and mac/deps/ffmpeg-build-mac-arm64)
#   mac/build_game.sh --tests    build and run the headless VM test (ttt2_host_vm_test) only
#   mac/build_game.sh --runtime  compile the runtime objects only (no PPC archive needed)
#   ARCH=x86_64 mac/build_game.sh   the Intel slice (macOS 13.0) in mac/build/game-x86_64; needs
#                                ARCH=x86_64 runs of build_libs, build_vkd3d, build_dxvk, build_ffmpeg and
#                                build_ppc first
# No Homebrew library is linked: its bottles target the macOS they were built on.
# Then mac/make_app.sh makes the .app.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${ARCH:-arm64}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
if [ "$ARCH" = arm64 ]; then
    BUILD="$ROOT/mac/build/game"
    cmake -S "$ROOT/port" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=14.4 >/dev/null
else
    BUILD="$ROOT/mac/build/game-$ARCH"
    cmake -S "$ROOT/port" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 >/dev/null
fi
case "${1:-}" in
    --tests)
        cmake --build "$BUILD" -j "$JOBS" --target ttt2_host_vm_test
        "$BUILD/ttt2_host_vm_test" ;;
    --runtime)
        cmake --build "$BUILD" -j "$JOBS" --target ttt2_mac_runtime ;;
    *)
        cmake --build "$BUILD" -j "$JOBS" --target tekken_recomp_mac
        echo "$BUILD/tekken_recomp_mac" ;;
esac
