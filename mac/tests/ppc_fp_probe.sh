#!/bin/bash
# Builds mac/tests/ppc_fp_probe.cpp for arm64 (mac overlay, as mac/build_ppc.sh) and for x86_64
# -march=x86-64-v2 (as their Windows/Linux PPC builds), runs arm64 here, x86_64 on a real Intel
# Mac over ssh (PROBE_HOST=<ssh host>; without it, only arm64 and Rosetta) and under Rosetta, and diffs.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/mac/build/codegen/fp_probe"; mkdir -p "$OUT"
HOST="${PROBE_HOST-}"
python3 "$ROOT/mac/tools/prepare_mac_ppc_headers.py" --ppc-dir "$ROOT/re/ppc" --output-dir "$OUT/headers"
COMMON=(-std=c++20 -O1 -ffp-contract=off -fno-exceptions -fno-rtti -Wno-everything
        -I "$ROOT/re/ppc" -I "$ROOT/tools/XenonRecomp/XenonUtils" -I "$ROOT/tools/XenonRecomp/thirdparty/simde")
clang++ "${COMMON[@]}" -arch arm64 -mmacosx-version-min=13.0 -I "$OUT/headers" -include "$OUT/headers/ppc_arm64_compat.h" \
    "$ROOT/mac/tests/ppc_fp_probe.cpp" -o "$OUT/probe_arm64"
clang++ "${COMMON[@]}" -arch x86_64 -march=x86-64-v2 -mmacosx-version-min=13.0 \
    "$ROOT/mac/tests/ppc_fp_probe.cpp" -o "$OUT/probe_x86_64"
"$OUT/probe_arm64" > "$OUT/arm64.txt"
# Variant: FPCR.AH=1 (FEAT_AFP, M2 and later) and FIZ added to the VMX flush mask.
mkdir -p "$OUT/headers_ah" && cp "$OUT/headers/"*.h "$OUT/headers_ah/"
sed -i '' 's/FlushMask = (1 << 19) | (1 << 24);/FlushMask = (1 << 19) | (1 << 24) | (1 << 0);/' "$OUT/headers_ah/ppc_context_arm64.h"
clang++ "${COMMON[@]}" -arch arm64 -DPROBE_FPCR_AH -I "$OUT/headers_ah" -include "$OUT/headers_ah/ppc_arm64_compat.h" \
    "$ROOT/mac/tests/ppc_fp_probe.cpp" -o "$OUT/probe_arm64_ah"
"$OUT/probe_arm64_ah" > "$OUT/arm64_ah.txt"
arch -x86_64 "$OUT/probe_x86_64" > "$OUT/rosetta.txt" || echo "(no Rosetta)"
if [ -n "$HOST" ]; then
    scp -q "$OUT/probe_x86_64" "$HOST:/tmp/ttt2_fp_probe" && ssh "$HOST" /tmp/ttt2_fp_probe > "$OUT/intel.txt"
    echo "== arm64 vs real Intel ($HOST) =="; diff "$OUT/arm64.txt" "$OUT/intel.txt" || true
    echo "== arm64 with FPCR.AH=1 + FIZ vs real Intel =="; diff "$OUT/arm64_ah.txt" "$OUT/intel.txt" || true
    echo "== Rosetta vs real Intel =="; diff "$OUT/rosetta.txt" "$OUT/intel.txt" && echo "(identical)"
else
    echo "== arm64 vs Rosetta =="; diff "$OUT/arm64.txt" "$OUT/rosetta.txt" || true
fi
