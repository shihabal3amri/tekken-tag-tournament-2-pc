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

To run a build without regenerating the game files, install a release once and point the build
at it:

```
set TTT2_VFS_ROOT=<install folder>\game
port\build-o1\tekken_recomp.exe "<install folder>\game\tekken.img"
```

The development exe is a console program and logs to stderr; redirect it to a file
(`... > run.txt 2>&1`) for long runs. `TTT2_NET=loopback` keeps online play on this PC (two
instances, no Steam).

## macOS

One universal app: native on Apple Silicon (arm64, macOS 14.4+) and on Intel Macs (x86_64, macOS 13+).
`mac/README.md` explains what changed for the Mac.

Requirements: Xcode command line tools, CMake, Ninja, Meson, Python 3.10+, Homebrew `bison` and
`spirv-headers` (used while building only), your disc image and the Title Update 3 package.

1. **Generate the C++** (steps 1-3 above in one script): `mac/gen_ppc.sh <disc.iso> <TU3 file>`. It
   builds XenonRecomp, extracts and checks `Default.xex`, `Default.xexp` and the patched image with the
   installer's own readers, then runs the recompiler, the fix-up scripts and the acceptance check.
2. **Libraries**, built from source for each CPU and its minimum macOS (arm64 is the default `ARCH`):

   ```
   mac/build_libs.sh && mac/build_vkd3d.sh && mac/build_dxvk.sh && mac/build_ffmpeg.sh && mac/build_ppc.sh
   ARCH=x86_64 mac/build_libs.sh   # and the same four with ARCH=x86_64
   ```

   `build_libs.sh` builds SDL3, OpenSSL and the Vulkan loader and fetches MoltenVK's universal release;
   `build_dxvk.sh` applies the repo's DXVK patches plus `mac/patches`. Everything goes to
   `mac/deps/install-<arch>`; no Homebrew library is linked.
3. **Steamworks SDK** 1.65 in `tools/steamworks_sdk_165/sdk` (optional, as on Windows).
4. **Build**: `mac/build_game.sh` and `ARCH=x86_64 mac/build_game.sh`.
5. **App**:

   ```
   mac/make_app.sh --binary mac/build/game/tekken_recomp_mac \
       --binary-x86 mac/build/game-x86_64/tekken_recomp_mac --release [--notarize <profile>]
   ```

   gives `mac/out/Tekken Tag Tournament 2.app` and `mac/out/TTT2-macOS-<version>.zip`, signed with your
   Developer ID. It has no game data: on first launch it asks for the disc image and Title Update 3
   (`port/src/runtime/apple_import.mm`).

To run a build against game data you already have:

```
TTT2_VFS_ROOT=<game>/game_files_x mac/build/game/tekken_recomp_mac <game>/tekken.img
```
