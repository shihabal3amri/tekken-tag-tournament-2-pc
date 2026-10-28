#!/bin/bash
# Build the generated guest code (re/ppc) as a macOS static archive.
#   mac/build_ppc.sh [O1|O2|O3]          arm64 (default O1, as their Windows/Linux/Android builds)
#   ARCH=x86_64 mac/build_ppc.sh [O1]    the Intel slice: mac/out/ppc-x86_64/libttt2_mac_ppc.a
# Output (arm64): mac/out/ppc/libttt2_mac_ppc.a and the header overlay mac/out/ppc/headers/
#   consumers: -I mac/out/ppc/headers (before re/ppc) and -include mac/out/ppc/headers/ppc_arm64_compat.h
# Needs: re/ppc (mac/gen_ppc.sh), tools/XenonRecomp (XenonUtils + simde), ninja, Xcode clang.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OPT="${1:-O1}"
ARCH="${ARCH:-arm64}"
SUFFIX=""; [ "$ARCH" = arm64 ] || SUFFIX="-$ARCH"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
exec python3 "$ROOT/mac/tools/build_mac_ppc_archive.py" \
    --ppc-dir "$ROOT/re/ppc" \
    --build-dir "$ROOT/mac/build/ppc$SUFFIX" \
    --out-dir "$ROOT/mac/out/ppc$SUFFIX" \
    --jobs "$JOBS" --opt-level "$OPT" --arch "$ARCH"
