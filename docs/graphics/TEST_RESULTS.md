# Test results — 2026-10-28

## Environment and boundaries

Windows x64; Visual Studio 2022 Build Tools 17.14; Windows SDK 10.0.26100;
Clang 23.1.4; CMake/Ninja; Python 3.12.10. Runtime adapter selection lists an
NVIDIA GeForce RTX 3060. Shader regression fixtures use D3D11 WARP for deterministic
readback and do not establish real-GPU performance. The game’s local build currently
has TTT2_HAVE_STEAM=0; online acceptance cannot be claimed.

QA game/settings/copied saves: `.development/cheats-graphics-qa`. Original installed
save containers are untouched. The game-data folder is a junction to the existing
installation; save writes use a separate physical path. No disc or derived game code
is included in the contribution.

## Automated evidence

| Test | Result / scope |
|---|---|
| test_offline_cheats.py | 238 checks: real translated damage routine including static arena (100), mocked wallet resolver (18), bounded ownership/progression guards (120); old-guard negative control fails correctly |
| test_cheat_overlay.py | 20 checks: chord edges, busy UI, online closure, stale callback revalidation |
| test_separate_players.py | 135 checks per dynamic configuration; both Start orders, neutral availability, explicit join, isolation, reconnect, focus, two pads; legacy passes |
| test_shared_menu_input.py | 18 checks: joined P2 navigation, packet updates, isolated gameplay/character setup |
| test_sdr_calibration.py | 8192 production-transform GPU readbacks, neutral/range/combined controls/clipping/alpha |
| test_mip_detail.py | 1024 GPU samples select actual mip levels 0–3; unmipped bias unchanged |
| test_smaa_d3d11.py | Production three-pass SMAA WARP edge/flat-color/alpha/clear/resize fixture passes |
| test_graphics_settings.py | Default/bounds/persist/new-process restart; existing online/custom keyboard values preserved |
| test_display_change_guard.py | 20 pure transaction checks; live timeout acceptance still pending |
| test_title_server.py | Original game CRC/parser accepts level-3 payload; corrupt-byte negative control fails correctly |
| test_save_persistence.py | Earlier 302 create + 48 reopen checks; false-write and path negative controls pass; new cheat-session save reload still pending |

Build evidence: `.development/cheats-graphics-build.log` (PORT_BUILD_EXIT=0),
`.development/advanced-launcher-build.log` (launcher link succeeds).
Shader/config logs: `sdr-tests.log`, `mip-tests.log`, `graphics-settings-tests.log`.
Title-storage test harness was narrowed to its actual production helper dependencies;
production title_server.cpp was not modified.

## Launcher acceptance

Five pages visually inspected: Graphics/Input/Cheats/Display/Quality. New controls fit
without overlap in the tested window. Cheats expose real health/money/unlock selections.
Save wrote Overlay=1, InfiniteHealth=1, InfiniteMoney=1, UnlockContent=1,
AntiAliasing=2 and PerformanceStats=2 in the isolated fixture. All custom keyboard
bindings, including Start=0x4E, remained intact. Changes were staged until Save.
An accessibility selection exposed a stale preset label; the new combo subclass now
forwards selection notifications, excluding initialization/internal changes. Rebuild
passes; follow-up UI acceptance is still in progress.

## Runtime evidence so far

The expanded game build boots, renders with SMAA, and reports actual submitted FPS,
CPU/submit/GPU intervals and local video-memory usage/budget in its title and log.
Observed logo/boot readouts are around 60 submitted FPS; they are not a fight benchmark.
The wallet hook logs its first successful refill during boot. Live menu availability,
money display/purchase, content browsing, health damage prevention, overlay behavior,
relaunch/save reload, and graphics comparisons are in progress.

## Remaining acceptance

- Identical-scene screenshots and before/after fight CPU/GPU/memory measurements.
- Newest VS/Pair availability and actual two-device play/reconnect checks.
- Runtime cheat effects, toggle off behavior and copied-save reload.
- Exclusive refresh, missing-monitor recovery and alternate-GPU fallback on hardware.
- Internal scale/resource resize stress; mip/AA/SDR artifact coverage in multiple stages.
- Display timeout and failure/device-loss recovery; preset balancing.
- Live Steam/rollback regressions after the required Steamworks SDK is available.

No unmeasured performance improvement or completed 153-feature implementation is claimed.


## Health acceptance correction

The user confirmed the other tested features work, but health decreased in the prior
runtime candidate. The health address filter was narrower than the game's allocations.
It now handles static/image-arena health objects; a new regression and negative control
cover this exact rejection. The corrected build was also missing from both active
launcher targets: it had been built but not copied before the user stopped Computer Use.
Both game/launcher copies are now replaced and SHA256-verified in the three development
folders. This follow-up did not launch the game; corrected-build fight acceptance remains
pending. See .development/health-deployment.json and health-deployment-tests.log.


## Offline preview update package

The native updater's prepare-only regression passes against a local TU3 image.
It verifies payload SHA256 and the existing image SHA1; copies opaque save/profile
bytes and custom settings; preserves preview progress on repeat preparation; rejects
an incompatible image, corrupt payload and unrelated nonempty destination. Original
source settings/saves and installed game image hashes remain unchanged. No launcher
or game is opened by this automated test. The ZIP is generated from an explicit
8-entry allowlist and verified for CRC/inventory; no game assets or personal files
are included. Evidence: .development/updater-smoke.log and preview-updater-build.log.

## Final local build and updater - 2026-10-29

Implementation was completed for diagonal tuning, in-game SDR references, and a
scoped one-shot customization grant before final builds/tests began. Analog-only,
full content coverage and matrix renderer dependencies remain unresolved; this is
an experimental partial update, not completion of the entire master checklist.

- Release game, native launcher, plain updater and embedded standalone updater built.
- 15 focused regression scripts passed, plus the new 657-check permanent-grant
  fixture. Direction filter: 196768 checks; SDR GPU readback: 1572864; physical
  polling harness: 503; overlay: 40; health: 238 plus negative control; save store:
  302 create + 48 new-process reopen checks and both negative controls passed.
- Permanent-grant fixture uses locally extracted original TU3 bulk, query,
  serializer and loader routines. Backup/preview/menu/session/schema/save-busy
  guards pass; the live guest save scheduler is mocked. Actual save/restart and
  complete category coverage are not established by this fixture.
- ZIP updater and embedded standalone updater passed TU3 preparation, payload
  hashes, progress-preserving reruns and original-file preservation. Standalone
  hash equality confirms the embedded game/launcher match the latest local builds;
  original-launch separation uses a mock executable, not a Steam match.
- A simultaneous test run was rejected by the new updater mutex; sequential
  rerun passed. Accidental hard-link replacement preserves its original target.
- Snapshot verification confirms 54 original executables/DLLs/boot image/settings/
  save files unchanged. No game was launched or screen controlled.
- Logs and machine-readable results: .development/finish-2026-10-29/.

The latest game still needs the user's real fight/controller/display/restart
acceptance. No upstream merge, PR update or GitHub release was performed.

## Upstream-integrated candidate - 2026-10-29

Upstream main 007c523 is merged with conflicts resolved, retaining its SDL and
shared settings paths. Full Windows Release game, launcher and updater builds
passed. The integrated candidate includes the host flush-success mapping fix.
The updated save fixture passed 302 create + 48 fresh-process reopen checks,
including a native write oracle and both negative controls. Fourteen other
focused suites passed, including SDL virtual A/Start events, dynamic ownership,
SDR GPU readbacks, health guards and overlay transitions. The separate 657-check
permanent-grant fixture also passed; its live save scheduler remains mocked.

Standalone and ZIP updater tests passed with the new SDL3.dll payload/licence:
embedded game/launcher hashes match current builds; repeated updates preserve
preview progress; original launch is checked with a mock executable. The
54-file original-install/settings/save baseline still matches. No game session
or screen control was used. Linux/Android runtime, physical controllers, live
save/restart, Steam, analog-only and dependency-blocked graphics remain open.

Earlier evidence records describe earlier binaries. This candidate integrates
current upstream and must still receive physical gameplay acceptance.

## Install once, launch from desktop - 2026-10-29

The updater now installs the preview and writes a per-user TTT2 Offline Launcher
shortcut. That link targets the installed native launcher directly with its app
folder as the working directory. Launcher startup recognizes the preview marker
and restores its own settings/save/assets environment before reading settings.
The original game shortcut is unchanged. The updater is only needed for future
updates; the original game-data folder remains required.

The plain-package regression installs into temporary state/desktop directories,
reads the real Shell Link COM properties, verifies the production startup helper
with inherited wrong environment values, checks rerun progress/shortcut retention,
and rejects missing original assets without falling back to original settings.
No game or graphical launcher is opened by the fixture; no real desktop is edited.
