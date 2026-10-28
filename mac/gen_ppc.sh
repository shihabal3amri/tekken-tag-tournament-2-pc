#!/bin/bash
# Generate the guest C++ (re/ppc) on macOS, as BUILDING.md steps 1-3 do on Windows.
#   mac/gen_ppc.sh disc.iso TU3_package
# Takes a few minutes; overwrites re/ppc.
#
#  0. builds tools/XenonRecomp (build-mac) and mac/build/setup if they are missing
#  1. ttt2_inputs: ISO + TU3 -> re/Default.xex, re/extracted_tu/<TU>/Default.xexp,
#     re/decrypted/Default_patched_tu3.bin (all SHA-1 checked; the image is the runtime's tekken.img)
#  2. XenonRecomp once (writes re/recomp/Default_patched_tu3.xex), XenonAnalyse on that image,
#     gen_missing_switches.py, XenonRecomp again
#  3. gen_missing_functions.py + XenonRecomp until no `// ERROR` site is left
#  4. debug_probes.py, check_recomp_acceptance.py (with the Apple weak-wrapper fix, see below),
#     gen_kernel_stubs.py (into mac/build/codegen; the game build makes its own copy)
#
# XenonRecomp built on Apple writes `PPC_WEAK_FUNC(sub_X) { __imp__sub_X(ctx, base); }` after each
# function instead of an alias line before it (its CMakeLists drops XENON_RECOMP_USE_ALIAS on
# Apple; Mach-O has no aliases). check_recomp_acceptance.py takes "\n__attribute__" as a
# function's end, so a copy that also stops at "\nPPC_WEAK_FUNC(" is run here.
# The tracked re/recomp/*.toml are rewritten by steps 2-3 (LF instead of CRLF); when only line
# endings changed they are restored from git.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -eq 2 ] || { echo "Usage: mac/gen_ppc.sh disc.iso TU3_package (TU_174Q22P_0000008000000.00000000000O2)" >&2; exit 2; }
ISO="$1"
TU="$2"
XR="$ROOT/tools/XenonRecomp"
BUILD="$ROOT/mac/build/codegen"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
mkdir -p "$BUILD"
cd "$ROOT"

step() { printf '\n=== %s ===\n' "$*"; }

step "0. tools"
if [ ! -x "$XR/build-mac/XenonRecomp/XenonRecomp" ] || [ ! -x "$XR/build-mac/XenonAnalyse/XenonAnalyse" ]; then
    cmake -S "$XR" -B "$XR/build-mac" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build "$XR/build-mac" -j "$JOBS" --target XenonRecomp XenonAnalyse
fi
if [ ! -x "$ROOT/mac/build/setup/ttt2_inputs" ]; then
    cmake -S "$ROOT/mac/setup" -B "$ROOT/mac/build/setup" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$ROOT/mac/build/setup" -j "$JOBS"
fi
RECOMP="$XR/build-mac/XenonRecomp/XenonRecomp"
ANALYSE="$XR/build-mac/XenonAnalyse/XenonAnalyse"
TOML="$ROOT/re/recomp/tekken.toml"
HEADER="$XR/XenonUtils/ppc_context.h"

step "1. inputs from the disc and Title Update 3"
"$ROOT/mac/build/setup/ttt2_inputs" "$ISO" "$TU" "$ROOT/re"
TU_NAME="$(basename "$TU")"
if ! grep -q "extracted_tu/$TU_NAME/Default.xexp" "$TOML"; then
    echo "tekken.toml expects a different TU file name than $TU_NAME" >&2; exit 1
fi

recompile() {
    # XenonRecomp prints one line per function; keep the log, show the tail.
    "$RECOMP" "$TOML" "$HEADER" > "$BUILD/xenonrecomp.$1.log" 2>&1 || { tail -20 "$BUILD/xenonrecomp.$1.log"; exit 1; }
    tail -2 "$BUILD/xenonrecomp.$1.log"
}
error_sites() { cat "$ROOT"/re/ppc/ppc_recomp.*.cpp | grep -c '// ERROR' || true; }

step "2. XenonAnalyse + measured switches"
mkdir -p "$ROOT/re/ppc"
rm -f "$ROOT/re/recomp/Default_patched_tu3.xex"
recompile first          # applies TU3 and writes re/recomp/Default_patched_tu3.xex
"$ANALYSE" "$ROOT/re/recomp/Default_patched_tu3.xex" "$ROOT/re/recomp/jump_tables.toml"
python3 "$ROOT/port/tools/gen_missing_switches.py"
recompile switches

step "3. missing functions until no ERROR site is left"
for pass in 1 2 3 4 5 6 7 8; do
    n="$(error_sites)"
    echo "pass $pass: $n ERROR sites"
    [ "$n" = 0 ] && break
    python3 "$ROOT/port/tools/gen_missing_functions.py"
    recompile "functions$pass"
done
[ "$(error_sites)" = 0 ] || { echo "ERROR sites remain" >&2; exit 1; }

step "4. probes, acceptance, kernel stubs"
python3 "$ROOT/port/tools/debug_probes.py" > "$BUILD/debug_probes.log"; tail -1 "$BUILD/debug_probes.log"
python3 - "$ROOT/port/tools/check_recomp_acceptance.py" "$BUILD/check_recomp_acceptance_mac.py" <<'PY'
import sys
src = open(sys.argv[1]).read()
old = '            end = text.find("\\n__attribute__", match.end())\n'
new = ('            ends = [e for e in (text.find("\\n__attribute__", match.end()),\n'
       '                                text.find("\\nPPC_WEAK_FUNC(", match.end())) if e >= 0]\n'
       '            end = min(ends) if ends else -1\n')
if src.count(old) != 1:
    sys.exit("check_recomp_acceptance.py changed; update the Apple fix in mac/gen_ppc.sh")
open(sys.argv[2], "w").write(src.replace(old, new))
PY
PYTHONPATH="$ROOT/port/tools" python3 "$BUILD/check_recomp_acceptance_mac.py"
python3 "$ROOT/port/tools/gen_kernel_stubs.py" --output "$BUILD/kernel_stubs.gen.cpp"

# Steps 2-3 rewrite the tracked configs with LF line endings; restore them when nothing else changed.
if git -C "$ROOT" rev-parse --git-dir > /dev/null 2>&1; then
    for f in re/recomp/tekken.toml re/recomp/jump_tables.toml; do
        if git -C "$ROOT" diff --quiet --ignore-cr-at-eol -- "$f"; then
            git -C "$ROOT" checkout -q -- "$f"
        else
            echo "NOTE: $f changed (not only line endings): review git diff" >&2
        fi
    done
fi
step "done: $(ls "$ROOT"/re/ppc/ppc_recomp.*.cpp | wc -l | tr -d ' ') units in re/ppc"
