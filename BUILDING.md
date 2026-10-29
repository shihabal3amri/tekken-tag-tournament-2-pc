# Building from source

Most players just want the installer from [Releases](https://github.com/shihabal3amri/tekken-tag-tournament-2-pc/releases/latest).
This page is for developers.

This is a static recompilation, so building needs your own copy of the game too: the recompiled
C++ is generated on your machine from your `Default.xex` + Title Update 3, and is never stored in
this repository (neither is anything else from the disc or the updates).

## Requirements

- Visual Studio 2022 Build Tools (MSVC x64)
- LLVM/Clang (clang-cl)
- CMake + Ninja (the copies bundled with Visual Studio are fine)
- Python 3.10+ (Pillow for the image tools)

The scripts expect the checkout at `C:\ttt2` (a junction is fine: `mklink /J C:\ttt2 <checkout>`),
because some tools choke on spaces and parentheses in paths.

## Steps

1. **Your game files.** Put your disc's `Default.xex` at `re\Default.xex`, put your title updates in
   `Updates\`, and extract them to `re\extracted_tu\` with `python port\tools\stfs.py`.
2. **The recompiler.** Clone [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (`--recursive`)
   into `tools\XenonRecomp`, check out `ddd128b`, apply `tools\patches\XenonRecomp.patch`
   (`git apply`), and run `tools\build_xenonrecomp.cmd`.
3. **Generate the C++** (into `re\ppc\`):
   1. XenonAnalyse on the TU3-patched image, then `python port\tools\gen_missing_switches.py`
   2. `XenonRecomp re\recomp\tekken.toml tools\XenonRecomp\XenonUtils\ppc_context.h`
   3. `python port\tools\gen_missing_functions.py`, and re-run XenonRecomp until no `// ERROR`
      sites remain
   4. `python port\tools\debug_probes.py` (applies the runtime's generated-code patches)
   5. `python port\tools\check_recomp_acceptance.py`
   6. `python port\tools\gen_kernel_stubs.py`
4. **FFmpeg** n7.1 source in `tools\ffmpeg_dl\FFmpeg-n7.1`, configured as described at the top of
   `port\third_party\ffmpeg_xma\CMakeLists.txt` (XMA decoding only).
5. **Steamworks SDK** 1.65 in `tools\steamworks_sdk_165\sdk` (optional; without it the build has
   no Steam online).
6. **Build**: `tools\build_port.cmd build-o1 1 8 ON` (the game, `port\build-o1\tekken_recomp.exe`),
   then `tools\build_setup.cmd build-o1 8` (the installer, `port\build-o1\setup\ttt2_setup.exe`).
   How the installer works: [port/INSTALLER.md](port/INSTALLER.md).

## Running a development build

The optional native launcher builds independently of the generated game code:
`tools\build_launcher.cmd`. Its output is `port\build-launcher\TTT2 Launcher.exe`.
Select the game executable with **Browse**, or place the launcher beside an installed
`Tekken Tag Tournament 2.exe`. It edits the same settings used by the game. The
separate-player option requires rebuilding the game from this source as well; older
executables ignore it. The launcher is not yet included in the installer payload.

Run `python port\tools\test_separate_players.py` for the device-routing regression.
It uses simulated XInput devices and temporary settings, without launching the game.

To run a build without regenerating the game files, install a release once and point the build
at it:

```
set TTT2_VFS_ROOT=<install folder>\game
port\build-o1\tekken_recomp.exe "<install folder>\game\tekken.img"
```

The development exe is a console program and logs to stderr; redirect it to a file
(`... > run.txt 2>&1`) for long runs. `TTT2_NET=loopback` keeps online play on this PC (two
instances, no Steam).

Keyboard SOCD cleaning is available in the launcher (restart the game after saving):
Original preserves input, Neutral cancels both opposite directions, Up Priority resolves
Up+Down to Up and Left+Right to neutral, and Last Input Priority selects the most recently
pressed direction (simultaneous presses resolve to neutral). The INI key is `[Keyboard] SOCD=0..3`.

Controller input options (restart required): `[Input] DPadOnly=1` ignores the left
analog stick while keeping digital D-pad controls available. `StickDeadzone=0..95`
sets an axis deadzone percentage; 0 preserves the existing range. Right-stick inputs
and button bindings are unchanged. Analog-only mode is pending menu-context validation.


The launcher has Graphics and Input tabs. Graphics presets stage settings until Save:
Original selects 720p with original image options; Performance selects 540p and disables
shadows; Enhanced selects 1080p, FXAA, sharpness 3 and 16x anisotropic filtering.
Display mode, VSync, input bindings and online netcode choices are preserved by presets.
Manual graphics changes update the preset selection. Restore original graphics also
stages changes; closing without saving discards them. These profiles are explicit
setting combinations, not hardware recommendations or benchmark guarantees.

The Steam status message checks whether Steam is running when the launcher opens.
Online play also needs a Steam-enabled game build and its runtime DLL; a running Steam
process alone does not verify connectivity. The local build without the user-supplied
Steamworks SDK cannot verify Steam online play.


Anti-aliasing now supports Off, FXAA and SMAA 1x (High). SMAA is a three-pass
spatial post-process using the original implementation and lookup textures in
port/third_party/smaa (license and pinned upstream revision included). No motion
vectors, temporal jitter or simulation changes are involved. Select it in the
launcher Graphics tab or in-game Display Settings; `[Display] AntiAliasing=2`.
The first use compiles its shaders. If initialization fails, the renderer logs
why and restores FXAA in settings. Existing AA values 0 and 1 retain their meaning.

Run `python port/tools/test_smaa_d3d11.py` for production shader checks on D3D11 WARP,
and `python port/tools/test_antialias_settings.py` for persistence validation.
A copied test game with SMAA enabled runs under .development/smaa-playtest using
its own settings and copied sandbox saves. These files are local and excluded from Git.


Offline tools overlay: enable it in the launcher's Offline Tools page and restart.
Press L3+R3 together on one controller. `[Mods] Overlay=1` is the corresponding INI
key. Current working actions are input-delay and shadow settings; health, money and
unlock cheats are still unavailable. The overlay blocks itself in online menus and
active sessions. This guard does not validate future fighter/save memory patches.

In-game display mode/resolution changes now offer Keep/Revert and automatically
restore the accepted selection after 15 seconds. State-machine regression:
`python port/tools/test_display_change_guard.py`. Live monitor recovery testing remains.
Overlay regression: `python port/tools/test_cheat_overlay.py`.


## Optional offline preview update archive (Windows)

`tools\build_launcher.cmd` also builds `TTT2 Offline Update.exe`, a static-CRT
bootstrap. It verifies an existing USA/Europe TU3 installation and payload checksums,
copies settings/saves to a separate preview, reuses the installed asset directory,
and starts the launcher. It never replaces the original installed executable.

Package existing local builds with:

    python port/tools/package_community_update.py --output dist/ttt2-v0.1.7-community.1-windows-offline-preview

The ZIP allowlist contains the host executables, original launcher artwork embedded
in the launcher, SMAA notices, README, build metadata and checksums. It contains no
ISO/XEX/TU files, game image/assets, personal saves/settings or Steamworks SDK files.
The package is explicitly an experimental offline preview; it is not a Steam update.
Do not publish generated game input files with the archive or source contribution.

Prepare-only regression (uses local installed assets but never launches a game):

    python port/tools/test_community_update.py --package dist/ttt2-v0.1.7-community.1-windows-offline-preview --installed-game-dir "C:/Games/Tekken Tag Tournament 2"
