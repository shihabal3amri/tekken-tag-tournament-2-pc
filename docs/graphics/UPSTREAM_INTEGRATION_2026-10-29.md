# Upstream integration - 2026-10-29

Integrated upstream main `007c523` without replacing its SDL input, platform
settings, native audio, renderer optimizations or rollback implementation.
The Windows launcher and preview controls use the shared settings backend.
Joined-player ownership and directional filters operate on the SDL snapshot.
Monitor positioning uses SDL before the existing shared window-mode code.
The permanent grant action is limited to the marked Windows offline preview;
other hosts have no permanent save-write action from this feature.

Fixed `host_file::DosErrorStatus(0)` so a successful host flush returns success
to the guest. The original production save fixture exposed this regression;
its native write oracle and process-reopen checks now pass with the platform
file adapters. Fixture buffers now live above upstream's protected low-memory
region. Added the missing SDL component probe and a snapshot ownership harness.

Validation on Windows x64:

- Full Release game and native launcher/updater targets build against SDL 3.4.18.
- Dynamic ownership fixtures pass both startup orders, preferences, separate
  joins, reconnect simulation, keyboard focus release, deadzone and diagonals.
- SDL component probe passes keyboard/focus and virtual A/Start press/release.
- Overlay, SDR shader, SMAA, mip detail, graphics persistence, display guard,
  keystroke ABI, health and health negative-control fixtures pass.
- Save fixtures: 302 creation checks, 48 new-process reopen checks, native write
  oracle and two negative controls pass. Permanent grants pass 657 checks with
  original bulk/query/serializer/loader code; the live scheduler is mocked.
- New updater payload includes SDL3.dll and its licence, and retains asset-free
  packaging. Updater-specific validation is recorded in TEST_RESULTS.md.

Remaining: physical controller/VS/Pair/reconnect acceptance, live save/restart,
Linux/Android compile/runtime coverage, live Steam validation, analog-only
source selection and renderer features requiring missing pipeline inputs.
This contribution remains draft. No game/disc data, saves, SDK or generated PPC
source belongs in Git. The offline preview remains separate from the original.
