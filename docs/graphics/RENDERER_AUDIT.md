# Renderer audit — 2026-10-28

This is an initial source audit, not completed graphics acceptance testing.

- Target: native Windows x64, C++20, custom Xbox 360 guest runtime. No conventional engine version is declared.
- Backend: Direct3D 11 / DXGI, raster rendering of translated guest shaders and GPU packets. Pipeline is guest-driven; do not assume a modern deferred/temporal renderer.
- Core sources: port/src/runtime/kernel/gpu_render.cpp, gpu_hw_d3d11.h, gpu_hw_translate.h, gpu_adapter.h.
- Existing presentation passes: display/color transform, FXAA, scaling and contrast-adaptive sharpening. Existing texture sampler overrides provide anisotropic filtering. Shadow rendering has a binary switch.
- Settings: display_settings.cpp/.h loads and saves settings.ini; menu_doc.cpp adds BX01 option rows and menu_ext.cpp registers native handlers. Native launcher shares the settings implementation.
- Resolutions: fixed 16:9 list, 640x360 through 3840x2160. Windowed, borderless and fullscreen modes exist. Present code letterboxes/pillarboxes to preserve 16:9.
- VSync: existing windowed/borderless path uses sync interval 0 and conditionally allows tearing; exclusive fullscreen uses interval 1 when VSync is enabled. This needs display-mode validation before treating it as disabled or replacing it.
- Vendor reconstruction, frame generation, HDR and temporal AA: no verified integrations in the inspected presentation path. Do not expose functional controls without their required actual pipeline.
- GPU detection: gpu_adapter.h. Full monitor/refresh-rate enumeration and user selection are not yet audited.
- Simulation: keep the existing 60 Hz logic and rollback protocol unchanged. Graphics scheduling does not authorize changes to game timing.
- Baseline build evidence: .development/game-build.log and .development/socd-build.log. Title/menu rendering inspected through Computer Use. Controlled fight CPU/GPU timings, VRAM baseline, startup benchmark and repeatable image comparison remain pending.
- Complete 153-ID tracking inventory: FEATURE_MATRIX.md. Entries explicitly distinguish existing source paths from features still requiring investigation.

Current limitation: the local build has TTT2_HAVE_STEAM=0 because the user-supplied Steamworks SDK is absent. This cannot verify live Steam online play. Saves are tested in a separate physical-path sandbox; save roots containing junctions are rejected by the existing save protection.


SMAA follow-up: the presentation path now supports original SMAA 1x HIGH in addition
to FXAA. See port/third_party/smaa/README.md for source provenance and the explicit
gamma-space blending limitation. Production WARP fixtures and AA settings persistence
pass, and all three passes ran in an isolated 1280x720 intro/attract session. This does
not establish fight performance across resolutions or online regression acceptance.


## Expanded source audit

The host translates guest vertex/pixel programs and executes guest GPU commands. It
has no material editor, named SSAO/GI/reflection/weather quality graph, mesh LOD
policy, or modern ray scene representation. These controls require validated guest
semantic mappings or genuine new passes/assets. The matrix records those dependencies
individually; it does not relabel generic blur, tint or sampler changes as those effects.
D3D11 raster is the supported hardware backend; the CPU renderer is a parity diagnostic.

New real controls use the existing architecture: SDR calibration extends the display
pixel shader's constant buffer, after guest gamma; mip detail alters eligible sampler
MipLODBias and includes the value in the sampler cache key; explicit internal scale
uses existing Rescale target reallocation independently of output dimensions. The
original spatial scaler uses Catmull-Rom upsampling and area downsampling.

Monitor device names come from EnumDisplayMonitors/GetMonitorInfo. The window thread
places the window on the selected monitor. Adapter choices come from DXGI and affect
initial device creation, with automatic high-performance fallback. Exclusive refresh
choices come from EnumDisplaySettings for the selected monitor and dimensions;
windowed/borderless continue using the desktop refresh. Refused exclusive refresh
selection persists automatic fallback. Fractional-Hz modes and multi-monitor recovery
still require acceptance testing.

Performance readout uses accepted DXGI submissions, GetThreadTimes for the command
processor CPU, wall time for present preparation, asynchronous timestamp queries for
GPU present intervals, and IDXGIAdapter3::QueryVideoMemoryInfo for this process's local
video memory usage and budget. GPU intervals include command gaps, not just busy time.
The window title is visible in windowed mode; detailed values also go to the log.
No generated-frame counts or invented VRAM totals are displayed.

Original motion blur is mapped through DisplaySettingSetMotionBlur (823749C0), which
updates the guest options object and live renderer option. Launcher overrides apply
only after a signed-in profile reaches the main menu; in-game changes sync the host
preference. No motion-blur strength/sample-quality setting is inferred from this flag.

Current evidence: .development/cheats-graphics-build.log, advanced-launcher-build.log,
sdr-tests.log, mip-tests.log, graphics-settings-tests.log and title-regression.log.
The expanded build passes. Runtime fight captures, multi-GPU/monitor acceptance,
exclusive-refresh fallback, preset balancing and live Steam regressions are pending.

## Source continuation — 2026-10-29 (no builds or tests run)

Added transient SDR reference panels in the display pixel-shader variants, after
guest gamma and before the existing SDR transform. The reference follows the same
AA/scaling/sharpening presentation path as the game. It leaves the middle image
visible for an owned system dialog with live brightness/gamma/contrast actions.
The tools overlay permits calibration only in the main/options menu, rechecks the
session gate in callbacks, and clears the reference on exit or a 60-second timeout.
It starts disabled and is not serialized. The prior launcher-only reference remains.

The production shader readback, overlay transition and configuration fixtures have
been extended, but have not been executed. No new build, runtime, screenshot or
release evidence exists for this continuation. Earlier build evidence does not
validate these changes.
