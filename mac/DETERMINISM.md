# Determinism: where macOS arm64 can differ from the Windows x86-64-v2 build

One bullet per risk: file:line, what differs, why. Sections by area.

## Platform (audio, XMA, host glue)

Audio does not feed the simulation. The sound engine (NUSound sub_82D032D0), the audio library's threads and
the runtime's audio output run on their own timelines and are never rewound
(port/src/runtime/kernel/rollback.cpp:460). During re-simulated frames the game's calls into the sound
middleware return what the original frame recorded (port/src/runtime/kernel/sound_rollback.cpp:1-10). What the
fight logic can read back from the engine (~216 state queries per frame) depends on decode and output timing,
which already varies between Windows PCs. The differences below can change what you hear, but not a fight.

- port/src/runtime/audio_mixer_native.h (whole file) and port/CMakeLists.txt:18-23: on x86 the mixer is
  built with `-mssse3`, and simde maps its `_mm_*` calls to SSE. On arm64 simde lowers the same calls to
  NEON. The arithmetic used is `add_ps`/`sub_ps`/`mul_ps` (IEEE-exact on both), plus integer shuffles
  (`shuffle_epi8` becomes `vqtbl1q` on the masked index, exact). There is no rcp/rsqrt/dp/hadd, so the
  results match bit for bit, except in the two cases below. `-ffp-contract=off` is set on these files for
  every compiler; Apple clang's default (`on`) would otherwise fuse mul+add into FMA on arm64.
- Same file, every `ctx.fpscr.enableFlushMode()`: tools/XenonRecomp/XenonUtils/ppc_context.h:253-288. On x86
  "flush" sets MXCSR FTZ|DAZ; on arm64 it sets FPCR FZ (bit 24, plus FZ16). Both flush denormal inputs
  and outputs to zero for add/sub/mul. Whether a result that is tiny before rounding but normal after it
  is flushed the same way on both hasn't been checked (not verified).
- NaN propagation (mixer and all host-side float SIMD): when both operands are NaN, x86 SSE returns the
  first operand quieted. AArch64 (FPCR.DN=0) gives a signalling NaN priority over a quiet one. Payloads
  can differ only for sNaN/qNaN mixes. ConvertAudioPcm (port/src/runtime/audio_output.cpp:24) turns
  every NaN into silence before the device.
- tools/XenonRecomp/XenonUtils/ppc_context.h:250 (HostToGuest) used by `loadFromHost()` on arm64: the table
  is indexed by the host rounding field. x86 RC is 0 nearest, 1 down, 2 up, 3 zero. AArch64 RMode is
  0 RN, 1 RP (up), 2 RM (down), 3 RZ, so on arm64 an "up" host mode would read back as guest "down" and
  the other way round. main.cpp calls `loadFromHost()` once while the host is round-to-nearest, so
  nothing should be affected today. It is a codegen/runtime file, noted here because the mixer depends on
  the FPSCR helpers.
- port/third_party/ffmpeg_xma (XMA decode, float): the Windows build uses clang-cl `/O2` with no FMA on
  x86-64-v2; Linux uses `-O2`. On Apple the library is built with `-O2 -ffp-contract=off` (port/third_party/
  ffmpeg_xma/CMakeLists.txt, APPLE branch), and every build uses `--disable-asm`, so all of them run the
  same C code. Checked on an M5 Max: xma_core_test against a full FFmpeg 9.0.2 decode, 0 samples off by
  more than 1 LSB on a mono and a stereo XMA2 stream from the game's own data027.bin.
- Audio output pacing (port/src/runtime/audio_output.cpp:205-225): CoreAudio gives SDL a 1024-frame device
  buffer (21 ms at 48 kHz, measured on an M5 Max). Render credits come back in bursts of 4 guest frames
  (256 samples each), so the guest audio pump runs in bursts. That changes timing only (the sound
  engine's own timeline), not the simulation.

## Runtime (host VM, rollback, host math)

- port/CMakeLists.txt (APPLE branch, `ttt2_mac_runtime`): the whole runtime is built with `-ffp-contract=off`,
  not only skin_profile.cpp and the audio files as on Linux. Apple clang's arm64 default is `on` (fuses
  `a*b+c` inside an expression into FMADD, one rounding instead of two). The Windows/Linux x86-64-v2 builds have
  no FMA instruction, so nothing fuses there. Without this flag every float expression in the runtime
  (GPU translation, guest_time, skinning, packed fields) could differ in the last bit.
- port/src/runtime/skin_profile.cpp + skin_four_bone_native.h: on x86 the file is built with `-mssse3`, and
  simde maps its calls to SSE; on arm64 simde lowers the same calls to NEON. The calls used are `add_ps`,
  `mul_ps` (IEEE-exact on both, unfused with the flag above) and integer shuffles/loads (`shuffle_epi8` ->
  `vqtbl1q`, exact). There is no rcp/rsqrt/dp/hadd here, so the results should match bit for bit. Not
  checked against a Windows run.
- port/src/runtime/runtime_math.cpp:5-6: `roundevenf`/`roundeven` (the recompiler's frin/vrfin) use
  `rintf`/`rint`, which round in the current dynamic mode. On x86 that mode is MXCSR.RC, on arm64 FPCR.RMode,
  and both are set from the guest FPSCR. The result is the same only while the overlay's HostToGuest/GuestToHost
  tables map the modes correctly (mac/out/ppc/headers/ppc_context_arm64.h:255 has the arm64 order, checked).
  The macOS 26 SDK's math.h declares no `roundeven`, so the runtime's definitions are the ones linked.
- port/src/runtime/host_vm_mac.cpp (whole file), with port/src/runtime/kernel/rollback.cpp:577-603 (CowWrite)
  and :525-563 (OpenPageLocked): Apple Silicon pages are 16 KiB, and rollback's first-write observer works in
  4 KiB pages. A write fault opens the whole host page, so every rollback-protected 4 KiB page in it is handed
  to the observer at once, attributed to the thread that made the first write. This differs from 4 KiB hosts
  in three ways:
  (a) During calibration (`g_attributing`), the three neighbours are recorded as written by that thread
  (`g_frameWritten`/`g_foreignWritten`), so the hot set can grow by up to 3 pages per page the game really
  writes. That costs more snapshot work but rewinds the same data.
  (b) During a session, a neighbour opened by a frame-thread write is snapshotted ("before") and restored on
  rewind, even if only a foreign thread (sound, network) writes it later in that frame. On a 4 KiB host that
  page would have faulted on its own and been classed foreign (not rewound when ExcludeForeign is on). Here
  the rewind can undo a foreign thread's write to that neighbour, which is a real behaviour difference from
  Windows. How often it happens depends on whether frame-owned and foreign-owned data share 16 KiB blocks,
  and has not been measured.
  (c) A neighbour opened by a foreign write is classed foreign, so later frame-thread writes to it in that frame
  are not captured. Windows (MEM_WRITE_WATCH plus PAGE_READONLY per 4 KiB page) would capture them, so this
  is the risky direction: a rewind could miss that page's frame state. This is only possible while
  `ExcludeForeign()` holds and the page is not already in `g_isHot`. Calibration on the Mac makes the hot set
  from 16 KiB-wide observations, which should mostly cover it, but that is not verified.
  A precise fix would need the faulting store emulated or single-stepped per 4 KiB page. AArch64 user mode has
  no trace flag, so this backend does not do it.
- port/src/runtime/host_vm_mac.cpp: write-watch dirty bits are per 16 KiB. kernel/rollback.cpp:188
  (TakeWriteWatch over tracked ranges) and kernel/rollback_probe.cpp:107 see extra dirty pages; both treat
  dirty as "maybe changed" and compare contents (rollback.cpp:381-384, :437-438), so only cost changes, not
  state. kernel/phys_mirror.cpp:76-94 (Close) copies every reported 4 KiB page of a cached alias back to the A
  view. A neighbour reported dirty but not written copies its stale alias bytes over A. On a 4 KiB host this can
  happen only inside one page; here it spans 16 KiB. It only matters if the CPU writes the A view while the same
  chunk is open in C/E, which the frame-boundary close (PhysMirrorInvalidateAll) bounds.
- Guards: Protect(None/ReadOnly) on a 4 KiB page that shares a host page with a writable one is not enforced
  (most permissive wins). This affects only the diagnostic TTT2_GUARD_IMAGE ranges (main.cpp:249-250:
  0x82001000 and the end at 0x82203000 are not 16 KiB aligned) and guest NoAccess pages next to live ones.
  Correct game code never touches those, so no state difference is expected. A buggy write that faults on
  Windows passes silently here.
- Null page (guest 0-0xFFFF): as on Android ARM64, a direct host store there is fatal. On Linux x86-64 it is
  single-stepped and discarded. The generated stores (ppc_store_guard_linux.h) and guest_memory.h helpers
  drop them on every platform, so normal play behaves the same.

## Codegen (generated guest code, re/ppc + mac/out/ppc)

Reference: their Windows build compiles re/ppc with clang-cl `-O1 -march=x86-64-v2 -ffp-contract=off`
(port/CMakeLists.txt:338-371, BUILDING.md step 6), so simde maps every `simde_mm_*` call to the native SSE4.1
instruction. mac/build_ppc.sh compiles the same files with Apple clang `-O1 -ffp-contract=off`, simde lowering to
NEON. Evidence: `mac/tests/ppc_fp_probe.sh` builds mac/tests/ppc_fp_probe.cpp (each line is an expression copied
from XenonRecomp's emitter) for arm64 and for x86_64 `-march=x86-64-v2`, runs arm64 on the M5 and the M1 (same
output) and x86_64 on the real Intel iMac (i7-7700K), and diffs. Counts are `grep -c` over re/ppc/ppc_recomp.*.cpp.
"Fixed" = fixed in the arm64 overlay (mac/tools/prepare_mac_ppc_headers.py -> mac/out/ppc/headers/), checked by
the probe. Line numbers: tools/XenonRecomp/XenonRecomp/recompiler.cpp (patched), tools/XenonRecomp/XenonUtils/
ppc_context.h, tools/XenonRecomp/thirdparty/simde/x86/*.h.

- **fmadd/fmsub/fnmsub(s), vmaddfp, vnmsubfp: same on both, no FMA.** The recompiler emits a separate multiply
  and add (recompiler.cpp:984-1042 scalar, `f64 * f64 + f64`; :1968-1972 and :2032-2035 vector,
  `simde_mm_add_ps(simde_mm_mul_ps(..))`). NEON `vmulq/vaddq` lower to plain IR fmul/fadd, so only
  contraction could fuse them; `-ffp-contract=off` forbids it. Checked: `objdump -d` of libttt2_mac_ppc.a has 0
  fmadd/fmsub/fnmadd/fnmsub/fmla/fmls. (Both builds differ from the real Xenon, which fuses: one rounding vs two.)
  Keep `-ffp-contract=off`: Apple clang's default is `on`.
- **vmsum3fp128/vmsum4fp128 = `simde_mm_dp_ps(.., 0xEF / 0xFF)` (recompiler.cpp:2016-2025; 10,974 sites): same
  for finite values, signed zeros and FTZ.** x86 runs DPPS: masked products (unselected lanes = +0.0), then
  (p0+p1)+(p2+p3), broadcast. simde's A64 path (sse4.1.h:1261-1297) does `vmulq_f32`, zeroes masked lanes with an
  AND (+0.0), then `vaddvq_f32`, which clang emits as `faddp.4s` + `faddp.2s` (disassembled), i.e.
  (p0+p1)+(p2+p3): the same pairs in the same order. Probe: catastrophic-cancellation orders, -0 products and
  0xEF with lane 0 masked all match the i7 bit for bit. **Differs only with NaN inputs**: the i7's DPPS gave
  `7FC00001 7FC00001 FFC00002 FFC00002` for products (qNaN1, 1, qNaN2, 1), i.e. not one value broadcast; arm64
  gives qNaN1 in all four lanes. (Rosetta gives arm64's answer, so Rosetta is not a reference for this.)
- **Flush-to-zero tininess: differs (verified).** VMX ops run with `enableFlushMode()` (recompiler.cpp:471-482
  emits it before every VMX FP instruction, 13,473 sites), which sets MXCSR FTZ|DAZ on x86 and FPCR.FZ (bit 24,
  plus FZ16) on arm64 (ppc_context.h:256, :273). AArch64 (FPCR.AH=0) flushes a result that is tiny *before*
  rounding; x86 FTZ flushes only when it is tiny *after* rounding. Probe: `vmulfp (FLT_MIN+ulp)*(1-2^-23)` (exact
  FLT_MIN*(1-2^-46)) gives 0x00000000 on arm64 and 0x00800000 (FLT_MIN) on the i7. Any VMX result in that sliver
  under FLT_MIN differs. Denormal inputs are flushed the same on both (`vaddfp denorm+0` = 0 on both).
- **Default NaN sign: differs (verified).** An invalid operation (inf-inf, 0*inf, sqrt(-x), 0/0) makes x86's
  "indefinite" `FFC00000` / `FFF8000000000000`; AArch64 (FPCR.DN=0, AH=0) makes `7FC00000` / `7FF8000000000000`.
  Affects every scalar and VMX add/sub/mul/div/sqrt that produces a fresh NaN (the Xenon itself makes the positive
  one, so arm64 is closer to the console but not to Windows). Matters only if game state stores and later
  compares/hashes NaN bits (e.g. a desync checksum over memory).
- **NaN propagation with a signalling NaN: differs (verified).** With two NaN operands x86 returns the first one
  (quieted); AArch64 (AH=0) gives an sNaN operand priority over a qNaN wherever it is. Probe (inputs hidden from
  the optimiser): vector `qNaN1 + sNaN2` and `qNaN1 * sNaN2` give FFC00002 (sNaN2 quieted) on arm64, 7FC00001 on
  the i7; scalar `fadd qNaN1 + sNaN2` gives FFF8000000000002 vs 7FF8000000000001. qNaN+qNaN, sNaN+qNaN and 1+sNaN
  match. Guest data rarely holds sNaNs; it matters only where NaN bits are stored and compared. The compiler may
  commute `a+b` on both targets, so even the x86 order is not guaranteed by the source.
- **FPCR.AH experiment (not applied).** The M5 has FEAT_AFP (`hw.optional.arm.FEAT_AFP: 1`); an M1 Max
  does not (`0`). Setting FPCR.AH=1 (bit 1) and adding FIZ (bit 0) to the VMX flush mask made the three differences
  above (tininess, default-NaN sign, sNaN priority) match the i7 exactly (probe variant `arm64_ah`). It would have to be set on
  every guest thread, it changes host code on those threads too (checked on the M5 with AH=1: FNEG/FABS leave a
  NaN's sign alone, FMAX returns the second operand for a NaN or for +-0, as MAXPS does), and the M1 cannot do it, so Macs would then disagree with each other. Runtime's
  decision; noted here as the only way found to close those three.
- **fctiwz (`simde_mm_cvttsd_si32`, recompiler.cpp:969-972; 1,507 sites): fixed.** simde's portable version
  (sse2.h:3534-3550) returns INT32_MIN unless `v < INT32_MAX`, so exactly 2147483647.0 became 0x80000000 on
  arm64 (x86: 0x7FFFFFFF; the emitted `> double(INT_MAX)` guard doesn't catch it). The overlay replaces it with an
  x86-exact range test (NaN/out of range -> INT32_MIN).
- **fctidz (`simde_mm_cvttsd_si64`, recompiler.cpp:964-967; 358 sites): fixed.** simde casts directly
  (sse2.h:3557-3571): `fcvtzs` gives 0 for NaN, x86 gives 0x8000000000000000. Overlay: NaN/out of range ->
  INT64_MIN.
- **fctid (`simde_mm_cvtsd_si64`, recompiler.cpp:959-962; 10 sites): fixed.** simde uses `round()` (sse2.h:3075-
  3089): ties away from zero and blind to the rounding mode, and NaN -> 0. x86 CVTSD2SI rounds by MXCSR
  (nearest-even by default, or whatever mtfsf set). Probe before the fix: 2.5 -> 3 vs 2, -0.5 -> -1 vs 0, 2.7 with
  guest mode "toward zero" -> 3 vs 2. Overlay: `__builtin_rint` (emits `frintx`, current FPCR mode) + x86 range
  rule. The archive now has 10 frintx and 0 fcvtas.
- **vctsxs / vctuxs / vcfux (ppc_context.h:707-750, :676-689; 223 / 104 / 472 sites): same.** They build on
  `simde_mm_cvttps_epi32`, whose A32V7 NEON path (sse2.h:3430-3460) adds the x86 range/NaN fix-up because
  SIMDE_FAST_CONVERSION_RANGE/FAST_NANS are off (simde-common.h:76-125: they turn on only with -ffast-math or
  -ffinite-math-only; keep both off). Probe matched the i7 for NaN, +-inf, 3e9, -3e9, 5e9, 2.5 and the uint
  edge values. Note clang turns vctuxs' `simde_mm_max_ps(x, 0)` select into `fmaxnm.4s` (104 in the archive);
  for an sNaN input fmaxnm returns a qNaN where the select returns 0. The probe's sNaN case still matched (the
  following saturate mask makes both 0xFFFFFFFF), so no visible difference found.
- **vmaxfp/vminfp with a denormal: differs (verified).** simde's NEON max/min (sse.h:3099-3127, :3230-3262) is
  `vbslq(vcgtq(a, b), a, b)`: the compare sees the flushed value, but the bit-select returns the original
  denormal bits. x86 MAXPS under DAZ returns the flushed zero. Probe: `vmaxfp(0x00000100, -1)` = 0x00000100 on
  arm64, 0 on the i7 (vminfp(denorm, 1) = 0 on both). The value is flushed again by the next FP op on both, so
  it differs only if stored or compared bitwise. NaN cases match (x86's "return the second operand" is emulated).
  355 vmaxfp + 277 vminfp sites.
- **vexptefp / vlogefp (recompiler.cpp:1951-1965; 8 exp2f + 4 log2f calls): host libm, not verified against
  Windows.** These call `exp2f`/`log2f` per lane; Windows links the UCRT's versions, macOS its own libm. Apple's
  arm64 and x86_64 libm agreed on the sampled inputs, but UCRT was not tested (no Windows x64 here). With the VMX
  flush mode on, Apple arm64 `log2f(denormal)` returns -inf (FF800000) and Apple x86 returns -141 (C30D0000): the
  flush mode reaches libm, differently per host library.
- **vrefp, vrsqrtefp, fres, fsqrt(s): same.** Emitted as exact IEEE `1/x`, `1/sqrt(x)`, `float(1.0/x)`, `sqrt`
  (recompiler.cpp:1044-1047, :1059-1067, :2137-2142, :2169-2174): correctly rounded on both, no estimate
  instructions (archive has 0 frecpe/frsqrte). frsqrte is not emitted by this XenonRecomp. (Both differ from the
  Xenon's estimates, the same way.)
- **vrfin/vrfiz/vrfim (`simde_mm_round_ps`, recompiler.cpp:2144-2160; 19 sites): same.** NEON frintn/frintz/frintm
  (sse.h:631-758). Probe: ties to even match.
- **vcfsx/vcsxwfp, vcfux, fcfid (int -> float): same.** scvtf/ucvtf and CVTDQ2PS/CVTSI2SD round by the current
  mode on both; probe matched for 0x7FFFFFFF, 16777217, -16777217.
- **Rounding mode (mffs/mtfsf, recompiler.cpp:1307-1309, :1348-1350; 5 + 5 sites): fixed in the overlay** (as
  Android): ppc_context.h:250 HostToGuest is in x86 RC order; the overlay swaps up/down for AArch64 RMode. Probe:
  mtfsf 1 / 2 read back 1 / 2, and `fdivs 1/3` under "+inf" gives the same bits as the i7.
- **Flush-mode bookkeeping:** `enableFlushMode()`/`disableFlushMode()` (ppc_context.h:317-333) compare against the
  cached `ctx.fpscr.csr`, not the register; the runtime loads it per thread (main.cpp:225,
  kernel/kernel_thread.cpp:181/218). New macOS threads start with FPCR = 0 (checked), so a stale cache can only
  leave FZ wrong in the same ways as on x86. Same logic on both.
- **Compiler identity folds:** LLVM folds `x*1.0 -> x` and `x+(-0.0) -> x` assuming no flush-to-zero (probe:
  `vmulfp denorm*1` keeps the denormal on both). Same LLVM rules on both targets at -O1, so not a cross-platform
  difference, but -O2/-O3 could fold differently from their -O1; keep O1 (mac/build_ppc.sh default).
- **Generated code itself is host independent:** re/ppc regenerated on an Apple Silicon Mac by mac/gen_ppc.sh is byte-identical
  to a tree generated on an Intel Mac. The only Apple-specific form is the `PPC_WEAK_FUNC` wrapper instead of an alias
  (XenonRecomp's CMakeLists drops XENON_RECOMP_USE_ALIAS on Apple); calls still bind to runtime overrides (link
  test: generated `bl` sites go to the strong `sub_X` in the runtime) and the wrapper is a tail call (`b`).
- **Async pipeline compilation skips draws** (mac/patches/dxvk-1.9.2b-async-pipelines-macos.patch, on by default
  on Apple, `DXVK_ASYNC=0` turns it off): while a new Vulkan pipeline compiles, draws that need it are dropped for a
  few frames. Targets the CPU reads back (sources of copies into staging resources, followed through image copies
  and resolves; in fights: the 32x1 auto-exposure readback, 128x64 ones and a 1024x1024 one) compile synchronously,
  but images that only feed them by sampling (the scene the exposure is averaged from) are not protected, so the
  guest's exposure value can briefly differ from a sync build. Rendering only; no gameplay state.
