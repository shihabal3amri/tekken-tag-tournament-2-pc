cmake_minimum_required(VERSION 3.20)
project(tekken_recomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(TTT2_LOCAL_SAVES "Enable the experimental persistent local save container" OFF)

# Recompiler output + XenonRecomp runtime headers (generated / third-party, not in repo).
set(PPC_DIR      "${CMAKE_SOURCE_DIR}/../re/ppc"                       CACHE PATH "generated PPC C++")
set(XENON_UTILS  "${CMAKE_SOURCE_DIR}/../tools/XenonRecomp/XenonUtils" CACHE PATH "ppc_context.h")
set(SIMDE_DIR    "${CMAKE_SOURCE_DIR}/../tools/XenonRecomp/thirdparty/simde" CACHE PATH "simde")
set(TTT2_PPC_OPT_LEVEL "0" CACHE STRING "Optimization level for recompiled PPC code (0, 1, or 2)")
set_property(CACHE TTT2_PPC_OPT_LEVEL PROPERTY STRINGS 0 1 2)
if(NOT TTT2_PPC_OPT_LEVEL MATCHES "^[012]$")
    message(FATAL_ERROR "TTT2_PPC_OPT_LEVEL must be 0, 1, or 2")
endif()

file(GLOB PPC_SOURCES CONFIGURE_DEPENDS
    "${PPC_DIR}/ppc_recomp.*.cpp"
    "${PPC_DIR}/ppc_func_mapping.cpp")

# The recompiled game code, as a static library (Gate 1: it must compile & link).
add_library(tekken_ppc STATIC ${PPC_SOURCES})
target_include_directories(tekken_ppc PRIVATE "${PPC_DIR}" "${XENON_UTILS}" "${SIMDE_DIR}")
# Keep the quick bring-up build at O0; use a separate build directory for performance runs.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # clang-cl parses bare -O1 as MSVC /O1 (size optimization). Forward the actual
    # Clang level, and undo Debug's /Ob0 explicitly: /Ob2 alone does not clear it.
    if(TTT2_PPC_OPT_LEVEL STREQUAL "0")
        target_compile_options(tekken_ppc PRIVATE -O0)
    else()
        target_compile_options(tekken_ppc PRIVATE "/clang:-O${TTT2_PPC_OPT_LEVEL}" /clang:-finline)
    endif()
else()
    target_compile_options(tekken_ppc PRIVATE "-O${TTT2_PPC_OPT_LEVEL}")
endif()
target_compile_options(tekken_ppc PRIVATE -Wno-everything)
# Host ISA for the recompiled code. With no -march, clang-cl targets baseline x86-64
# (SSE2), so SIMDe emulates every SSSE3/SSE4.1 op the VMX128 translation uses in scalar
# loops - including the pshufb byte swap in every lvx/stvx (191,722 sites) and vsldoi
# (alignr). Measured 2026-10-10: the WMV decoder's hottest function was ~2,800 x86
# instructions, mostly scalar lane loops. x86-64-v2 (SSSE3, SSE4.1/4.2, POPCNT) is the
# Windows 11 24H2 minimum, so it adds no new hardware requirement. The only result-visible
# difference from the portable path: simde_mm_dp_ps (vmsum3/4fp128) becomes native DPPS,
# which sums products pairwise rather than sequentially (last-bit differences possible).
# -ffp-contract=off keeps separate PPC multiply/add instructions unfused on any target.
set(TTT2_PPC_MARCH "x86-64-v2" CACHE STRING "clang -march for recompiled PPC code (empty = compiler default)")
if(TTT2_PPC_MARCH)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(tekken_ppc PRIVATE "/clang:-march=${TTT2_PPC_MARCH}" /clang:-ffp-contract=off)
    else()
        target_compile_options(tekken_ppc PRIVATE "-march=${TTT2_PPC_MARCH}" -ffp-contract=off)
    endif()
endif()

# XMA audio decoding: a minimal static FFmpeg (C) built from the local download of its source.
enable_language(C)
add_subdirectory(third_party/ffmpeg_xma)

# The runtime executable: minimal host (memory, function table, entry) + kernel import stubs.
file(GLOB RUNTIME_KERNEL "${CMAKE_SOURCE_DIR}/src/runtime/kernel/*.cpp")
add_executable(tekken_recomp
    "${CMAKE_SOURCE_DIR}/src/runtime/main.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/runtime_math.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/guest_time.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/audio_output.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/kernel_stubs.gen.cpp"
    ${RUNTIME_KERNEL})
target_include_directories(tekken_recomp PRIVATE
    "${PPC_DIR}" "${XENON_UTILS}" "${SIMDE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/runtime" "${CMAKE_SOURCE_DIR}/src/runtime/kernel")
target_compile_definitions(tekken_recomp PRIVATE TTT2_LOCAL_SAVES=$<BOOL:${TTT2_LOCAL_SAVES}>)
target_compile_definitions(tekken_recomp PRIVATE TTT2_DRAW_CAPTURE_DIAGNOSTICS=1)
target_compile_definitions(tekken_recomp PRIVATE TTT2_D3D11_MOVIE_BACKEND=1)
# Hardware renderer (gpu_hw_d3d11.h); still opt-in at run time with TTT2_GPU_HW=1 / compare.
target_compile_definitions(tekken_recomp PRIVATE TTT2_GPU_HW_BACKEND=1)
target_link_libraries(tekken_recomp PRIVATE d3d11 dxgi d3dcompiler)
# The runtime is ~10 hand-written files, so optimising it costs seconds of build time - and it
# carries the software rasterizer, which interprets a pixel shader PER PIXEL and shades about
# 2.5 M pixels a frame. At -O0 that was slow enough that the opening video's white fade-in
# (alpha stepping down ~0.005 per rendered frame, so ~200 frames) never finished inside a run.
# The recompiled PPC library has its own configurable optimization level above.
# (C++ only: the target also compiles game.rc, and rc.exe rejects compiler flags.)
target_compile_options(tekken_recomp PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-O2;-Wno-everything>")
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # Allow optimized renderer/runtime helpers to inline even in the Debug configuration.
    target_compile_options(tekken_recomp PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/clang:-finline>")
endif()
target_link_libraries(tekken_recomp PRIVATE tekken_ppc xaudio2 ole32 winmm ttt2_ffmpeg bcrypt ws2_32)
# Steam online (kernel/steam_net.cpp, port/ONLINE_PLAN.md): built when the Steamworks SDK sits in
# tools/steamworks_sdk_165 (user-supplied, not part of the repository). steam_api64.dll is
# delay-loaded and copied next to the executable; without it (or without a running Steam
# client) the game falls back to the loopback transport.
set(TTT2_STEAMWORKS "${CMAKE_SOURCE_DIR}/../tools/steamworks_sdk_165/sdk" CACHE PATH "Steamworks SDK (sdk folder)")
if(EXISTS "${TTT2_STEAMWORKS}/public/steam/steam_api.h" AND EXISTS "${TTT2_STEAMWORKS}/redistributable_bin/win64/steam_api64.lib")
    target_include_directories(tekken_recomp PRIVATE "${TTT2_STEAMWORKS}/public")
    target_compile_definitions(tekken_recomp PRIVATE TTT2_HAVE_STEAM=1)
    target_link_libraries(tekken_recomp PRIVATE "${TTT2_STEAMWORKS}/redistributable_bin/win64/steam_api64.lib" delayimp)
    target_link_options(tekken_recomp PRIVATE "/DELAYLOAD:steam_api64.dll")
    add_custom_command(TARGET tekken_recomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TTT2_STEAMWORKS}/redistributable_bin/win64/steam_api64.dll"
                "$<TARGET_FILE_DIR:tekken_recomp>/steam_api64.dll")
    message(STATUS "Steamworks SDK: ${TTT2_STEAMWORKS} (Steam online enabled)")
else()
    target_compile_definitions(tekken_recomp PRIVATE TTT2_HAVE_STEAM=0)
    message(STATUS "Steamworks SDK not found - Steam online disabled")
endif()
# Emit a linker map so faulting PC RVAs can be resolved to guest functions during bring-up.
target_link_options(tekken_recomp PRIVATE "/MAP:tekken_recomp.map")
# Community servers (kernel/community_*.cpp): TLS through SChannel, the server key's hash through CNG.
target_link_libraries(tekken_recomp PRIVATE secur32 crypt32)

# community_net_test: the community-server client (TLS + pinning, sessions, invites, GIN, the server list) against a
# running ttt2-server - port/tools/community_net_test.cpp. Built with warnings on; no game code.
add_executable(community_net_test
    "${CMAKE_SOURCE_DIR}/tools/community_net_test.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/kernel/community_net.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/kernel/server_list.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/kernel/community_tls.cpp")
target_include_directories(community_net_test PRIVATE "${CMAKE_SOURCE_DIR}/src/runtime/kernel")
target_compile_options(community_net_test PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-O2;-W4>")
target_link_libraries(community_net_test PRIVATE ws2_32 secur32 crypt32 bcrypt)
target_compile_definitions(community_net_test PRIVATE _CRT_SECURE_NO_WARNINGS)

# --- Version, icon -------------------------------------------------------------------------
# One version for the game executable's VERSIONINFO, the installer and Add or remove programs.
set(TTT2_VERSION "0.1.7")
string(REPLACE "." "," TTT2_VERSION_COMMAS "${TTT2_VERSION},0")
# Original art (port/tools/make_icon.py), not the game's.
set(TTT2_SETUP_ICON "${CMAKE_SOURCE_DIR}/src/setup/ttt2.ico")
configure_file(src/runtime/game.rc.in "${CMAKE_BINARY_DIR}/game.rc" @ONLY)
set_source_files_properties("${CMAKE_BINARY_DIR}/game.rc" PROPERTIES OBJECT_DEPENDS "${TTT2_SETUP_ICON}")
target_sources(tekken_recomp PRIVATE "${CMAKE_BINARY_DIR}/game.rc")

# --- Installer (ttt2_setup) ------------------------------------------------------------------
# Modeled on the Tom & Jerry port's tj_setup. It ships NO game data: the player supplies their
# own disc image + Title Update 3 and the installer builds game\ from them (src/setup/*). /MT so
# the installer itself needs no VC++ redist. Build: tools\build_setup.cmd (after build_port.cmd).
#
# The shipped game exe is tekken_recomp.exe with its PE subsystem switched to WINDOWS (no console
# window; installed mode logs to %LOCALAPPDATA%\TTT2Recomp\log.txt, see main.cpp InstalledLayout).
# The dev exe stays a console program so run_port.cmd / the test scripts keep their stderr.
set(TTT2_PAYLOAD_DIR "${CMAKE_BINARY_DIR}/payload")
set(TTT2_GAME_EXE "${TTT2_PAYLOAD_DIR}/Tekken Tag Tournament 2.exe")
set(TTT2_README_TXT "${CMAKE_SOURCE_DIR}/tools/dist_README.txt")
set(TTT2_STEAM_DLL "${TTT2_STEAMWORKS}/redistributable_bin/win64/steam_api64.dll")
set(XENON_THIRDPARTY "${XENON_UTILS}/../thirdparty")
find_program(TTT2_EDITBIN editbin)
file(GLOB TTT2_CRT_DIRS "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT")
list(SORT TTT2_CRT_DIRS)
list(REVERSE TTT2_CRT_DIRS)
if(TTT2_CRT_DIRS AND TTT2_EDITBIN AND EXISTS "${TTT2_STEAM_DLL}" AND EXISTS "${XENON_UTILS}/xex_patcher.cpp")
    list(GET TTT2_CRT_DIRS 0 TTT2_CRT_DIR)
    set(CRT_MSVCP140        "${TTT2_CRT_DIR}/msvcp140.dll")
    set(CRT_MSVCP140_ATOMIC "${TTT2_CRT_DIR}/msvcp140_atomic_wait.dll")
    set(CRT_VCRUNTIME140    "${TTT2_CRT_DIR}/vcruntime140.dll")
    set(CRT_VCRUNTIME140_1  "${TTT2_CRT_DIR}/vcruntime140_1.dll")

    add_custom_command(OUTPUT "${TTT2_GAME_EXE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TTT2_PAYLOAD_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:tekken_recomp>" "${TTT2_GAME_EXE}"
        COMMAND "${TTT2_EDITBIN}" /nologo /SUBSYSTEM:WINDOWS "${TTT2_GAME_EXE}"
        DEPENDS tekken_recomp
        COMMENT "Game executable for the installer (GUI subsystem)"
        VERBATIM)
    add_custom_target(ttt2_game_exe DEPENDS "${TTT2_GAME_EXE}")

    # Disc / title-update readers + XenonUtils' XEX delta patcher (MIT) and its dependencies.
    add_library(ttt2_setup_core STATIC
        src/setup/xdvdfs.cpp
        src/setup/stfs.cpp
        src/setup/game_data.cpp
        "${XENON_UTILS}/xex_patcher.cpp"
        "${XENON_UTILS}/memory_mapped_file.cpp"
        "${XENON_THIRDPARTY}/libmspack/libmspack/mspack/lzxd.c"
        "${XENON_THIRDPARTY}/tiny-AES-c/aes.c")
    target_include_directories(ttt2_setup_core PUBLIC src PRIVATE
        "${XENON_UTILS}"
        "${XENON_THIRDPARTY}/libmspack/libmspack/mspack"
        "${XENON_THIRDPARTY}/tiny-AES-c"
        "${XENON_THIRDPARTY}/TinySHA1")
    target_compile_definitions(ttt2_setup_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(ttt2_setup_core PRIVATE -O2 -Wno-everything)
    set_property(TARGET ttt2_setup_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

    # ttt2_setup: the shipped installer (requireAdministrator). ttt2_setup_probe: the same
    # program as asInvoker, so `/silent <iso> <tu> <folder>` can be scripted into a test folder.
    foreach(variant ttt2_setup ttt2_setup_probe)
        if(variant STREQUAL "ttt2_setup")
            set(TTT2_UAC_LEVEL "requireAdministrator")
        else()
            set(TTT2_UAC_LEVEL "asInvoker")
        endif()
        set(TTT2_SETUP_MANIFEST "${CMAKE_BINARY_DIR}/${variant}.manifest")
        configure_file(src/setup/setup.manifest.in "${TTT2_SETUP_MANIFEST}" @ONLY)
        configure_file(src/setup/payload.rc.in "${CMAKE_BINARY_DIR}/${variant}.rc" @ONLY)
        # The .rc pulls these in by PATH: without OBJECT_DEPENDS a rebuilt game exe would not
        # relink the installer, which would silently ship the previous build (T&J lesson).
        set_source_files_properties("${CMAKE_BINARY_DIR}/${variant}.rc" PROPERTIES OBJECT_DEPENDS
            "${TTT2_GAME_EXE};${TTT2_SETUP_ICON};${TTT2_README_TXT};${TTT2_STEAM_DLL};${TTT2_SETUP_MANIFEST}")
        add_executable(${variant} WIN32
            src/setup/setup_main.cpp
            src/setup/payload.cpp
            "${CMAKE_BINARY_DIR}/${variant}.rc")
        add_dependencies(${variant} ttt2_game_exe)
        target_link_libraries(${variant} PRIVATE ttt2_setup_core comctl32 shell32 ole32 shlwapi advapi32 uuid bcrypt)
        target_compile_definitions(${variant} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
                                   TTT2_VERSION_STR="${TTT2_VERSION}")
        target_compile_options(${variant} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-O2>")
        target_link_options(${variant} PRIVATE /MANIFEST:NO)   # the manifest is in the .rc
        set_property(TARGET ${variant} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        set_target_properties(${variant} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/setup")
    endforeach()
    message(STATUS "installer: ttt2_setup available (CRT ${TTT2_CRT_DIR})")
else()
    message(STATUS "installer: unavailable (needs the x64 VC++ redist, editbin, the Steamworks SDK and XenonUtils)")
endif()
