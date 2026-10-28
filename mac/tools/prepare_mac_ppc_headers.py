#!/usr/bin/env python3
"""Prepare the arm64 overlay for the generated PPC headers (macOS and Android).

Same three edits as port/tools/prepare_android_ppc_headers.py, applied to re/ppc/ppc_context.h
and written outside the generated tree:
  - simde_mm_vsl: variable shift through the _sll/_srl forms (NEON's immediate shifts need
    constants);
  - FPCR write through a 64-bit register (msr fpcr needs an X register);
  - FPCR RMode -> PPC rounding table (ARM encodes 01 = +Inf, 10 = -Inf).
The compat header accepts __APPLE__ as well as __ANDROID__ (theirs #errors off Android), and
maps three scalar float->int conversions to x86-exact versions (see the header text below).
Consumers: -I <out> before re/ppc, and -include <out>/ppc_arm64_compat.h.
"""

from pathlib import Path
import argparse


def write_if_changed(path: Path, content: str) -> None:
    if not path.is_file() or path.read_text() != content:
        path.write_text(content)


REPLACEMENTS = (
    ("variable shift",
     "return simde_mm_or_si128(simde_mm_slli_epi64(a, shift), simde_mm_srli_epi64(simde_mm_slli_si128(a, 8), 64 - shift));",
     """const simde__m128i left = simde_mm_cvtsi64_si128(shift);
    const simde__m128i right = simde_mm_cvtsi64_si128(64 - shift);
    return simde_mm_or_si128(simde_mm_sll_epi64(a, left),
                             simde_mm_srl_epi64(simde_mm_slli_si128(a, 8), right));"""),
    ("ARM64 FPCR",
     '__asm__ __volatile__("msr fpcr, %0" : : "r"(csr));',
     'uint64_t nativeCsr = csr;\n        __asm__ __volatile__("msr fpcr, %0" : : "r"(nativeCsr));'),
    ("ARM64 rounding modes",
     'static constexpr size_t HostToGuest[] = { PPC_ROUND_NEAREST, PPC_ROUND_DOWN, PPC_ROUND_UP, PPC_ROUND_TOWARD_ZERO };',
     '''#if defined(__aarch64__) || defined(_M_ARM64)
    // ARM FPCR encodes 01 as +Infinity and 10 as -Infinity.
    static constexpr size_t HostToGuest[] = { PPC_ROUND_NEAREST, PPC_ROUND_UP, PPC_ROUND_DOWN, PPC_ROUND_TOWARD_ZERO };
#else
    static constexpr size_t HostToGuest[] = { PPC_ROUND_NEAREST, PPC_ROUND_DOWN, PPC_ROUND_UP, PPC_ROUND_TOWARD_ZERO };
#endif'''),
)

COMPAT = '''#pragma once
// arm64 overlay for the generated PPC code (mac/tools/prepare_mac_ppc_headers.py).
#if !defined(__aarch64__) || !(defined(__APPLE__) || defined(__ANDROID__))
#error "arm64 PPC overlay requires arm64 macOS or Android"
#endif
#include "ppc_config.h"
#include "ppc_context_arm64.h"
#ifndef _mm_pause
#define _mm_pause() __asm__ __volatile__("yield")
#endif

// Scalar float->int conversions with x86 results (their Windows/Linux builds run CVTTSD2SI /
// CVTSD2SI natively). simde's portable versions differ (mac/DETERMINISM.md):
//   cvttsd_si32: returns INT32_MIN for exactly 2147483647.0 (range test uses `< INT32_MAX`);
//   cvttsd_si64: plain cast, so NaN -> 0 on arm64 (x86: INT64_MIN);
//   cvtsd_si64:  round() = ties away from zero and ignores the rounding mode (x86: MXCSR mode,
//                nearest-even by default), NaN -> 0.
// x86 returns the "integer indefinite" (INT_MIN of the width) for NaN and out-of-range input.
// rint() uses the current FPCR rounding mode, which ctx.fpscr.storeFromGuest sets (mtfsf).
namespace ttt2_arm64 {
inline int32_t cvttsd_si32(simde__m128d a) {
    const double v = simde_mm_cvtsd_f64(a);
    return (v > -2147483649.0 && v < 2147483648.0) ? int32_t(v) : INT32_MIN;
}
inline int64_t cvttsd_si64(simde__m128d a) {
    const double v = simde_mm_cvtsd_f64(a);
    return (v >= -9223372036854775808.0 && v < 9223372036854775808.0) ? int64_t(v) : INT64_MIN;
}
inline int64_t cvtsd_si64(simde__m128d a) {
    const double v = __builtin_rint(simde_mm_cvtsd_f64(a));
    return (v >= -9223372036854775808.0 && v < 9223372036854775808.0) ? int64_t(v) : INT64_MIN;
}
} // namespace ttt2_arm64
#undef simde_mm_cvttsd_si32
#undef simde_mm_cvttsd_si64
#undef simde_mm_cvtsd_si64
#define simde_mm_cvttsd_si32(a) ttt2_arm64::cvttsd_si32(a)
#define simde_mm_cvttsd_si64(a) ttt2_arm64::cvttsd_si64(a)
#define simde_mm_cvtsd_si64(a) ttt2_arm64::cvtsd_si64(a)
'''


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ppc-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    patched = (args.ppc_dir / "ppc_context.h").read_text()
    for label, old, new in REPLACEMENTS:
        if patched.count(old) != 1:
            raise SystemExit(f"expected exactly one {label} in {args.ppc_dir / 'ppc_context.h'}")
        patched = patched.replace(old, new)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    write_if_changed(args.output_dir / "ppc_context_arm64.h", patched)
    write_if_changed(args.output_dir / "ppc_arm64_compat.h", COMPAT)


if __name__ == "__main__":
    main()
