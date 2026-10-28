# macOS

The port built natively for macOS: SDL3 for the window, input and audio, and the D3D11 renderer through
DXVK Native on MoltenVK. Players get one app; `BUILDING.md` has the build steps.

## Status (M5 Max, macOS 26)

- Menus, Arcade and Practice. Fights run at about 60 fps once the shaders are prepared (5 s windows of
  56.9-62.0 presents per second, muted real audio device). On-screen frame pacing and input latency are not
  measured yet.
- First launch without game data: the import window (`port/src/runtime/apple_import.mm`) takes the disc
  image and Title Update 3, writes `~/Library/Application Support/TTT2Recomp/game` (about a minute from a disc
  image on the internal SSD) and prepares the 6,371 shaders once (about 7 minutes).
- Sound is untested by ear: test runs are muted. The XMA decoder is within 1 LSB of FFmpeg's reference.
- Shader hitches: Metal pipelines compile in the background
  (`mac/patches/dxvk-1.9.2b-async-pipelines-macos.patch`; it never skips a draw into an image the CPU reads
  back). With the shader cache warm, fights showed no gap over 50 ms; a stage seen for the first time can
  still hitch once for about half a second.
- Online is experimental. Two copies on one Mac (`TTT2_NET=loopback`) stay in sync, also at 40 ms delay,
  10 ms jitter and 2 % loss. Over Steam against an Intel Mac, a full Player Match played to the end, but the
  fighter state drifts from the x86 build's partway through a match (see below), so arm64 builds use their own
  game protocol and only meet each other.

## Apple Silicon against x86

In two Steam matches against the x86_64 build, the fight RNG was equal on every frame, but `FighterHash`
(rollback_session.cpp) differed from frame 142 in one and from frame 600 in the other. Two x86 builds match
each other. The four fighter objects were word-for-word identical on frames 100-400 of the second match. So
the split comes from a rare operation, not everyday arithmetic. `DETERMINISM.md` lists every place arm64
and x86-64 round differently and what was checked. Until the cause is found, `net::GameProtocol()` adds
`-arm64` on arm64. `TTT2_NET_PROTO` overrides the protocol string for tests.

## What changed

| Area | Files | What |
|---|---|---|
| Guest code | `mac/gen_ppc.sh`, `mac/build_ppc.sh`, `mac/tools/`, `mac/setup/` | XenonRecomp `ddd128b` with the repo's patch builds unchanged. On Apple it emits `PPC_WEAK_FUNC` wrappers instead of aliases (Mach-O has none); the splitter and the acceptance check handle that. arm64 uses Android's header overlay plus fixes so `fctiwz`/`fctidz`/`fctid` match x86. |
| Game data | `mac/setup/`, `apple_import.mm` | The Android importer (`mobile_install.cpp`) builds the game folder from the ISO + TU3, checking the SHA-1s. |
| Memory | `host_vm_mac.cpp`, `host_posix_compat.h` | Guest 4 KB pages over 16 KB host pages: 4 KB bookkeeping, 16 KB `mprotect`. A write fault marks every armed 4 KB page of its host page, so rollback's first-write tracking sees all of them. SIGBUS and SIGSEGV, Darwin's ucontext, `shm_open` for the mirror, `os_sync_wait_on_address`. |
| Runtime | `host_runtime_mac.cpp`, `host_fault_mac.cpp`, `#if __APPLE__` in `*_linux.cpp` | Saves and settings in `~/Library/Application Support/TTT2Recomp`, shaders in `~/Library/Caches/TTT2Recomp`, the log in `~/Library/Logs/TTT2Recomp`; game data in Application Support (or `TTT2_VFS_ROOT`); `posix_spawn` for the shader prebuild and the Steam helper. |
| Main thread | `apple_game_entry.cpp`, `host_window_sdl.cpp` | Cocoa windows belong to the main thread: it runs SDL's event loop, the game's `main()` runs on a 16 MB worker, and window calls go through `SDL_RunOnMainThread`. |
| Renderer | `mac/patches/`, `mac/build_dxvk.sh`, `mac/build_vkd3d.sh`, `gpu_*` guards | DXVK Native 1.9.2b with the repo's 11 patches, plus one for macOS (Gcenx's DXVK-macOS relaxations, portability enumeration, the surface on the main thread) and the async pipeline patch. Feature level 11_0 on MoltenVK 1.4.2. vkd3d-shader 1.17. |
| Audio | `mac/build_ffmpeg.sh`, `port/third_party/ffmpeg_xma/` | FFmpeg n7.1, XMA decoder only, C code only, `-ffp-contract=off`. |
| Floats | `port/CMakeLists.txt` (APPLE block) | `-ffp-contract=off` on the whole runtime: Apple clang fuses `a*b+c` into FMA on arm64 by default, and x86-64-v2 has no FMA. |
| App | `mac/make_app.sh` | Every dylib in `Contents/Frameworks`, MoltenVK with its ICD manifest, the icon from `port/src/setup/ttt2.ico`. `--binary-x86` makes one universal app, `--release` signs it with a Developer ID under the hardened runtime and zips it, `--notarize` notarizes and staples it. |

## Running a build

```
TTT2_VFS_ROOT=<game>/game_files_x mac/build/game/tekken_recomp_mac <game>/tekken.img
```

- `TTT2_FRAMETIME=1`: frame and pipeline timing in the log.
- `TTT2_NET=loopback`: online between two copies on one Mac. The second one runs with `TTT2_INSTANCE=B` and
  its own `TTT2_SAVE_ROOT`. Add `TTT2_NET_DELAY`, `_JITTER`, `_LOSS` to simulate a network.
- `TTT2_AUDIO_MUTE=1`: the real audio device at zero gain. Pacing stays as in an audible run;
  `TTT2_AUDIO_SILENT=1` swaps the device clock for a timer instead.
- `TTT2_IMPORT_DISC` and `TTT2_IMPORT_UPDATE`: start the import window's install without asking.
- Pad injection reads `C:/ttt2/port/pad_inject<instance>.txt` relative to the working directory (lines
  `BUTTONS hold_ms gap_ms`).

## Steam

Online runs over Steam P2P as app 480 (Spacewar), from a helper process (`--steam-bridge`). With the
Steamworks SDK 1.65 in `tools/steamworks_sdk_165/sdk`, the build links the SDK's universal
`libsteam_api.dylib` and `mac/make_app.sh` bundles it. The Steam app has to be running and signed in.
