cmake_minimum_required(VERSION 3.20)
project(tekken_recomp C CXX)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ShaderBuildId.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/KernelStubs.cmake")
if(WIN32)
    add_subdirectory(src/launcher)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Native audio arithmetic retains the generated PPC rounding points.
set(_ttt2_native_fp_option -ffp-contract=off)
set(_ttt2_native_simd_option -mssse3)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(_ttt2_native_fp_option /clang:-ffp-contract=off)
    set(_ttt2_native_simd_option /clang:-mssse3)
endif()
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/kernel/audio_filter_native.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/kernel/audio_mixer_native.cpp"
    PROPERTIES COMPILE_OPTIONS "${_ttt2_native_fp_option}")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/kernel/audio_mixer_native.cpp"
        APPEND PROPERTY COMPILE_OPTIONS "${_ttt2_native_simd_option}")
endif()

if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
    add_compile_options(
        "/clang:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=port"
        "/clang:-ffile-prefix-map=${CMAKE_BINARY_DIR}=build")
endif()

# Keep the shared SDL window and other host components independently buildable
# on Linux alongside the complete game target.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(SDL3 3.4.0 REQUIRED CONFIG)
    find_package(OpenSSL 3.0 REQUIRED)
    find_package(Threads REQUIRED)
    option(TTT2_LINUX_STEAM "Enable native Linux Steam matchmaking and relay transport" OFF)
    set(TTT2_LINUX_STEAMWORKS "" CACHE PATH "Steamworks SDK sdk directory for Linux")
    set(_ttt2_linux_have_steam 0)
    if(TTT2_LINUX_STEAM)
        set(_ttt2_linux_steam_api "${TTT2_LINUX_STEAMWORKS}/redistributable_bin/linux64/libsteam_api.so")
        if(NOT EXISTS "${TTT2_LINUX_STEAMWORKS}/public/steam/steam_api.h"
           OR NOT EXISTS "${_ttt2_linux_steam_api}")
            message(FATAL_ERROR "Linux Steam requires Steamworks public headers and linux64/libsteam_api.so")
        endif()
        set(_ttt2_linux_have_steam 1)
    endif()
    add_library(ttt2_host_window_sdl STATIC src/runtime/host_window_sdl.cpp)
    target_include_directories(ttt2_host_window_sdl PUBLIC src/runtime)
    target_link_libraries(ttt2_host_window_sdl PUBLIC SDL3::SDL3)
    add_library(ttt2_host_window_game_sdl STATIC src/runtime/host_window_sdl.cpp)
    target_include_directories(ttt2_host_window_game_sdl PUBLIC src/runtime)
    target_compile_definitions(ttt2_host_window_game_sdl PRIVATE TTT2_HOST_GAME_UI=1)
    target_link_libraries(ttt2_host_window_game_sdl PUBLIC SDL3::SDL3)
    add_library(ttt2_host_audio_sdl STATIC src/runtime/audio_output.cpp)
    target_include_directories(ttt2_host_audio_sdl PUBLIC src/runtime)
    target_link_libraries(ttt2_host_audio_sdl PUBLIC SDL3::SDL3)
    add_library(ttt2_host_input_sdl STATIC src/runtime/host_input_sdl.cpp)
    target_include_directories(ttt2_host_input_sdl PUBLIC src/runtime)
    target_link_libraries(ttt2_host_input_sdl PUBLIC SDL3::SDL3)
    target_link_libraries(ttt2_host_window_sdl PUBLIC ttt2_host_input_sdl)

    set(TTT2_RENDERER_PPC_DIR "${CMAKE_SOURCE_DIR}/../re/ppc" CACHE PATH "Generated PPC headers for Linux components")
    set(TTT2_RENDERER_XENON_UTILS "${CMAKE_SOURCE_DIR}/../tools/XenonRecomp/XenonUtils" CACHE PATH "XenonUtils headers")
    set(TTT2_RENDERER_SIMDE_DIR "${CMAKE_SOURCE_DIR}/../tools/XenonRecomp/thirdparty/simde" CACHE PATH "SIMDe headers")
    add_library(ttt2_shared_threads_linux OBJECT
        src/runtime/host_thread_sdl.cpp
        src/runtime/host_sleep.cpp
        src/runtime/host_status.cpp
        src/runtime/host_process.cpp
        src/runtime/kernel/kernel_objects.cpp
        src/runtime/kernel/kernel_thread.cpp
        src/runtime/kernel/kernel_misc.cpp
        src/runtime/kernel/kernel_sys.cpp
        src/runtime/guest_time.cpp)
    target_include_directories(ttt2_shared_threads_linux PRIVATE
        "${TTT2_RENDERER_PPC_DIR}" "${TTT2_RENDERER_XENON_UTILS}"
        "${TTT2_RENDERER_SIMDE_DIR}" src/runtime src/runtime/kernel)
    target_link_libraries(ttt2_shared_threads_linux PRIVATE SDL3::SDL3)
    add_library(ttt2_guest_vm_linux STATIC src/runtime/host_vm_linux.cpp)
    target_include_directories(ttt2_guest_vm_linux PUBLIC src/runtime)
    add_library(ttt2_shared_physical_mirror_linux OBJECT
        src/runtime/kernel/phys_mirror.cpp src/runtime/phys_mirror_linux.cpp)
    target_include_directories(ttt2_shared_physical_mirror_linux PRIVATE
        src/runtime src/runtime/kernel)
    add_library(ttt2_host_file_linux STATIC src/runtime/host_file_common.cpp
        src/runtime/host_file_linux.cpp)
    target_include_directories(ttt2_host_file_linux PUBLIC src/runtime)
    add_library(ttt2_shared_files_linux OBJECT src/runtime/kernel/kernel_io.cpp)
    target_include_directories(ttt2_shared_files_linux PRIVATE
        "${TTT2_RENDERER_PPC_DIR}" "${TTT2_RENDERER_XENON_UTILS}"
        "${TTT2_RENDERER_SIMDE_DIR}" src/runtime src/runtime/kernel)
    add_library(ttt2_host_storage_linux STATIC src/runtime/host_storage_linux.cpp)
    target_include_directories(ttt2_host_storage_linux PUBLIC src/runtime)
    add_library(ttt2_host_random_linux STATIC src/runtime/host_random_linux.cpp)
    target_include_directories(ttt2_host_random_linux PUBLIC src/runtime)
    add_library(ttt2_shared_storage_linux OBJECT src/runtime/kernel/profiles.cpp
        src/runtime/kernel/save_store.cpp)
    target_include_directories(ttt2_shared_storage_linux PRIVATE src/runtime src/runtime/kernel)
    target_compile_definitions(ttt2_shared_storage_linux PRIVATE TTT2_LOCAL_SAVES=1)
    add_library(ttt2_shared_udp_linux OBJECT src/runtime/kernel/net_transport.cpp)
    target_include_directories(ttt2_shared_udp_linux PRIVATE src/runtime src/runtime/kernel)
    add_library(ttt2_shared_random_guest_linux OBJECT src/runtime/kernel/xlive.cpp)
    target_include_directories(ttt2_shared_random_guest_linux PRIVATE
        "${TTT2_RENDERER_PPC_DIR}" "${TTT2_RENDERER_XENON_UTILS}"
        "${TTT2_RENDERER_SIMDE_DIR}" src/runtime src/runtime/kernel)
    add_library(ttt2_shared_entry_linux OBJECT
        src/runtime/main.cpp src/runtime/host_runtime_linux.cpp)
    target_include_directories(ttt2_shared_entry_linux PRIVATE
        "${TTT2_RENDERER_PPC_DIR}" "${TTT2_RENDERER_XENON_UTILS}"
        "${TTT2_RENDERER_SIMDE_DIR}" src/runtime src/runtime/kernel)
    file(GLOB TTT2_KERNEL_LINUX "${CMAKE_SOURCE_DIR}/src/runtime/kernel/*.cpp")
    foreach(_excluded data_watch.cpp diag_hooks.cpp steam_bridge.cpp
                      host_settings_windows.cpp gpu_render.cpp xma.cpp)
        list(REMOVE_ITEM TTT2_KERNEL_LINUX
            "${CMAKE_SOURCE_DIR}/src/runtime/kernel/${_excluded}")
    endforeach()
    if(TTT2_LINUX_STEAM)
        list(REMOVE_ITEM TTT2_KERNEL_LINUX
            "${CMAKE_SOURCE_DIR}/src/runtime/kernel/steam_bridge_linux_stub.cpp")
    else()
        list(REMOVE_ITEM TTT2_KERNEL_LINUX
            "${CMAKE_SOURCE_DIR}/src/runtime/kernel/steam_bridge_linux.cpp")
    endif()
    add_library(ttt2_shared_kernel_linux OBJECT ${TTT2_KERNEL_LINUX})
    target_include_directories(ttt2_shared_kernel_linux PRIVATE
        "${TTT2_RENDERER_PPC_DIR}" "${TTT2_RENDERER_XENON_UTILS}"
        "${TTT2_RENDERER_SIMDE_DIR}" src/runtime src/runtime/kernel)
    target_compile_definitions(ttt2_shared_kernel_linux PRIVATE
        TTT2_LOCAL_SAVES=1 TTT2_HAVE_STEAM=${_ttt2_linux_have_steam} TTT2_GPU_HW_BACKEND=1
        TTT2_GPU_HW_D3D11=1 TTT2_D3D11_MOVIE_BACKEND=1)
    target_link_libraries(ttt2_shared_kernel_linux PRIVATE SDL3::SDL3 OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
    if(TTT2_LINUX_STEAM)
        target_include_directories(ttt2_shared_kernel_linux PRIVATE
            "${TTT2_LINUX_STEAMWORKS}/public")
    endif()

    # Compile the same GPU frontend and D3D11 backend source used by Windows.
    # The component probes remain available independently of the game target.
    set(TTT2_DXVK_NATIVE_ROOT "" CACHE PATH "DXVK Native installation prefix")
    if(TTT2_DXVK_NATIVE_ROOT)
        find_path(TTT2_DXVK_NATIVE_INCLUDE d3d11.h
            HINTS "${TTT2_DXVK_NATIVE_ROOT}/usr/include/dxvk"
                  "${TTT2_DXVK_NATIVE_ROOT}/include/dxvk")
        find_library(TTT2_DXVK_NATIVE_D3D11 dxvk_d3d11
            HINTS "${TTT2_DXVK_NATIVE_ROOT}/usr/lib" "${TTT2_DXVK_NATIVE_ROOT}/lib")
        find_library(TTT2_DXVK_NATIVE_DXGI dxvk_dxgi
            HINTS "${TTT2_DXVK_NATIVE_ROOT}/usr/lib" "${TTT2_DXVK_NATIVE_ROOT}/lib")
        set(TTT2_VKD3D_ROOT "" CACHE PATH "vkd3d-shader installation prefix")
        find_path(TTT2_VKD3D_INCLUDE vkd3d_shader.h
            HINTS "${TTT2_VKD3D_ROOT}/usr/include/vkd3d" "${TTT2_VKD3D_ROOT}/include/vkd3d")
        find_library(TTT2_VKD3D_SHADER vkd3d-shader
            HINTS "${TTT2_VKD3D_ROOT}/usr/lib" "${TTT2_VKD3D_ROOT}/lib")
        set(TTT2_RENDERER_PPC_DIR "${CMAKE_SOURCE_DIR}/../re/ppc" CACHE PATH "Generated PPC headers for the renderer component")
        set(TTT2_RENDERER_XENON_UTILS "${CMAKE_SOURCE_DIR}/../tools/XenonRecomp/XenonUtils" CACHE PATH "XenonUtils headers")
        set(TTT2_RENDERER_SIMDE_DIR "${CMAKE_SOURCE_DIR}/../tools/XenonRecomp/thirdparty/simde" CACHE PATH "SIMDe headers")
        if(NOT TTT2_DXVK_NATIVE_INCLUDE OR NOT TTT2_DXVK_NATIVE_D3D11
           OR NOT TTT2_DXVK_NATIVE_DXGI OR NOT TTT2_VKD3D_INCLUDE
           OR NOT TTT2_VKD3D_SHADER)
            message(FATAL_ERROR "Shared Linux renderer needs DXVK Native and vkd3d-shader development files")
        endif()
        # Old native DXVK keeps its Windows/DirectX ABI headers in subdirectories.
        # Use the same headers as Android, without its ARM-only intrinsic shims.
        if(EXISTS "${TTT2_DXVK_NATIVE_INCLUDE}/directx")
            include_directories("${TTT2_DXVK_NATIVE_INCLUDE}/directx"
                "${TTT2_DXVK_NATIVE_INCLUDE}/windows" "${CMAKE_SOURCE_DIR}/native/include")
        endif()
        add_library(ttt2_shared_renderer_linux OBJECT
            src/runtime/kernel/gpu_render.cpp src/runtime/native_d3dcompiler.cpp)
        ttt2_shader_build_id(ttt2_shared_renderer_linux "${CMAKE_SOURCE_DIR}/src/runtime")
        target_include_directories(ttt2_shared_renderer_linux PRIVATE
            "${TTT2_DXVK_NATIVE_INCLUDE}" "${TTT2_VKD3D_INCLUDE}"
            "${TTT2_RENDERER_PPC_DIR}"
            "${TTT2_RENDERER_XENON_UTILS}" "${TTT2_RENDERER_SIMDE_DIR}"
            src/runtime src/runtime/kernel)
        target_compile_definitions(ttt2_shared_renderer_linux PRIVATE
            TTT2_GPU_HW_BACKEND=1 TTT2_GPU_HW_D3D11=1
            TTT2_D3D11_MOVIE_BACKEND=1)
        target_link_libraries(ttt2_shared_renderer_linux PRIVATE SDL3::SDL3
            "${TTT2_DXVK_NATIVE_D3D11}" "${TTT2_DXVK_NATIVE_DXGI}"
            "${TTT2_VKD3D_SHADER}")
        option(TTT2_GEOMETRY_AB_PROBE "Include live reference geometry paths for Linux benchmarks" OFF)
        if(TTT2_GEOMETRY_AB_PROBE)
            target_compile_definitions(ttt2_shared_renderer_linux PRIVATE TTT2_GEOMETRY_AB_PROBE=1)
        endif()
        # A small ELF target proves the Linux D3D11, DXGI, and shader compiler
        # symbols resolve without linking the incomplete game host services.
    endif()
    set(FFMPEG_SRC "${CMAKE_SOURCE_DIR}/../tools/ffmpeg_dl/FFmpeg-n7.1"
        CACHE PATH "FFmpeg n7.1 source for the Linux game")
    set(FFMPEG_BUILD "${CMAKE_SOURCE_DIR}/../tools/ffmpeg_dl/build-linux-native"
        CACHE PATH "Configured native FFmpeg headers")
    # FFmpeg assertions use __FILE__; keep local source and build paths out of
    # the distributed ELF even when dependencies live outside this tree.
    add_compile_options(
        "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=port"
        "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build"
        "-ffile-prefix-map=${FFMPEG_SRC}=ffmpeg"
        "-ffile-prefix-map=${FFMPEG_BUILD}=ffmpeg-build")
    if(TTT2_DXVK_NATIVE_ROOT AND EXISTS "${FFMPEG_SRC}/configure"
       AND EXISTS "${FFMPEG_BUILD}/config.h")
        add_subdirectory(third_party/ffmpeg_xma)
        set(TTT2_LINUX_PPC_PREBUILT "" CACHE FILEPATH
            "Native PPC archive built from the generated C++ sources")
        if(TTT2_LINUX_PPC_PREBUILT)
            if(NOT EXISTS "${TTT2_LINUX_PPC_PREBUILT}")
                message(FATAL_ERROR "Missing native PPC archive: ${TTT2_LINUX_PPC_PREBUILT}")
            endif()
            add_library(ttt2_linux_ppc STATIC IMPORTED GLOBAL)
            set_target_properties(ttt2_linux_ppc PROPERTIES
                IMPORTED_LOCATION "${TTT2_LINUX_PPC_PREBUILT}")
        else()
            file(GLOB TTT2_LINUX_PPC_SOURCES CONFIGURE_DEPENDS
                "${TTT2_RENDERER_PPC_DIR}/ppc_recomp.*.cpp"
                "${TTT2_RENDERER_PPC_DIR}/ppc_func_mapping.cpp")
            if(NOT TTT2_LINUX_PPC_SOURCES)
                message(FATAL_ERROR "Generated PPC sources are missing")
            endif()
            # The generated units can exceed 50 MB. Split only large units at
            # function boundaries so optimizing them does not exhaust compiler
            # memory. Preserve the original sources outside the build tree.
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            set(_ttt2_ppc_splitter "${CMAKE_SOURCE_DIR}/tools/split_ppc_units.py")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${_ttt2_ppc_splitter}" ${TTT2_LINUX_PPC_SOURCES})
            execute_process(
                COMMAND "${Python3_EXECUTABLE}" "${_ttt2_ppc_splitter}"
                    --output-dir "${CMAKE_CURRENT_BINARY_DIR}/ppc_split"
                    --threshold-bytes 4194304 --chunk-bytes 2097152
                    ${TTT2_LINUX_PPC_SOURCES}
                OUTPUT_VARIABLE _ttt2_ppc_compile_lines
                ERROR_VARIABLE _ttt2_ppc_split_error
                RESULT_VARIABLE _ttt2_ppc_split_result
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT _ttt2_ppc_split_result EQUAL 0 OR _ttt2_ppc_compile_lines STREQUAL "")
                message(FATAL_ERROR "Cannot split generated PPC units: ${_ttt2_ppc_split_error}")
            endif()
            string(REPLACE "\n" ";" TTT2_LINUX_PPC_COMPILE_SOURCES
                "${_ttt2_ppc_compile_lines}")
            add_library(ttt2_linux_ppc STATIC ${TTT2_LINUX_PPC_COMPILE_SOURCES})
            target_include_directories(ttt2_linux_ppc PRIVATE
                "${TTT2_RENDERER_PPC_DIR}" "${TTT2_RENDERER_XENON_UTILS}"
                "${TTT2_RENDERER_SIMDE_DIR}")
            set(TTT2_LINUX_PPC_OPT_LEVEL "O1" CACHE STRING
                "Optimization level for the generated Linux PPC archive")
            set_property(CACHE TTT2_LINUX_PPC_OPT_LEVEL PROPERTY STRINGS O0 O1 O2 O3)
            if(NOT TTT2_LINUX_PPC_OPT_LEVEL MATCHES "^O[0-3]$")
                message(FATAL_ERROR "Invalid TTT2_LINUX_PPC_OPT_LEVEL: ${TTT2_LINUX_PPC_OPT_LEVEL}")
            endif()
            set(TTT2_LINUX_PPC_MARCH "x86-64-v2" CACHE STRING
                "CPU architecture for the generated Linux PPC archive")
            target_compile_options(ttt2_linux_ppc PRIVATE
                "-${TTT2_LINUX_PPC_OPT_LEVEL}"
                "-march=${TTT2_LINUX_PPC_MARCH}"
                -ffp-contract=off)
            if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
                set_target_properties(ttt2_linux_ppc PROPERTIES CXX_EXTENSIONS OFF)
                target_compile_options(ttt2_linux_ppc PRIVATE -finline -Wno-everything)
            endif()
            if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                target_compile_options(ttt2_linux_ppc PRIVATE
                    "-include${CMAKE_SOURCE_DIR}/src/runtime/ppc_compiler_compat.h")
            endif()
            target_compile_options(ttt2_linux_ppc PRIVATE
                "-include${CMAKE_SOURCE_DIR}/src/runtime/ppc_store_guard_linux.h")
        endif()
        ttt2_generate_kernel_stubs(TTT2_LINUX_KERNEL_STUBS
            "${TTT2_RENDERER_PPC_DIR}" "${CMAKE_SOURCE_DIR}/src/runtime")
        add_executable(tekken_recomp_linux EXCLUDE_FROM_ALL
            src/runtime/runtime_math.cpp src/runtime/guest_time.cpp
            src/runtime/skin_profile.cpp
            src/runtime/audio_output.cpp src/runtime/host_thread_sdl.cpp
            src/runtime/host_sleep.cpp src/runtime/host_status.cpp
            src/runtime/host_process.cpp src/runtime/host_vm_linux.cpp
            src/runtime/host_fault_linux.cpp src/runtime/host_file_common.cpp
            src/runtime/host_file_linux.cpp src/runtime/host_storage_linux.cpp
            src/runtime/host_udp_linux.cpp src/runtime/host_random_linux.cpp
            src/runtime/phys_mirror_linux.cpp src/runtime/kernel/xma.cpp
            "${TTT2_LINUX_KERNEL_STUBS}"
            $<TARGET_OBJECTS:ttt2_shared_entry_linux>
            $<TARGET_OBJECTS:ttt2_shared_kernel_linux>
            $<TARGET_OBJECTS:ttt2_shared_renderer_linux>)
        target_include_directories(tekken_recomp_linux PRIVATE
            "${TTT2_RENDERER_PPC_DIR}" "${TTT2_RENDERER_XENON_UTILS}"
            "${TTT2_RENDERER_SIMDE_DIR}" src/runtime src/runtime/kernel)
        if(TTT2_GEOMETRY_AB_PROBE)
            target_compile_definitions(tekken_recomp_linux PRIVATE TTT2_GEOMETRY_AB_PROBE=1)
        endif()
        target_compile_definitions(tekken_recomp_linux PRIVATE
            TTT2_LOCAL_SAVES=1 TTT2_HAVE_STEAM=${_ttt2_linux_have_steam} TTT2_GPU_HW_BACKEND=1
            TTT2_GPU_HW_D3D11=1 TTT2_D3D11_MOVIE_BACKEND=1)
        target_compile_options(tekken_recomp_linux PRIVATE -O2)
        # Keep the native skinning arithmetic identical to the generated PPC.
        set_source_files_properties(src/runtime/skin_profile.cpp PROPERTIES
            COMPILE_OPTIONS "-ffp-contract=off")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
            set_property(SOURCE src/runtime/skin_profile.cpp APPEND PROPERTY
                COMPILE_OPTIONS -mssse3)
        endif()
        target_link_libraries(tekken_recomp_linux PRIVATE
            ttt2_host_window_game_sdl ttt2_host_input_sdl
            SDL3::SDL3 ttt2_ffmpeg ttt2_linux_ppc
            "${TTT2_DXVK_NATIVE_D3D11}" "${TTT2_DXVK_NATIVE_DXGI}"
            "${TTT2_VKD3D_SHADER}" OpenSSL::SSL OpenSSL::Crypto Threads::Threads dl)
        if(TTT2_LINUX_STEAM)
            target_link_libraries(tekken_recomp_linux PRIVATE "${_ttt2_linux_steam_api}")
        endif()
        set_target_properties(tekken_recomp_linux PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN/lib"
            INSTALL_RPATH_USE_LINK_PATH FALSE)
    endif()
    add_executable(community_net_test
        tools/community_net_test.cpp src/runtime/kernel/community_net.cpp
        src/runtime/kernel/server_list.cpp src/runtime/kernel/community_tls.cpp)
    target_include_directories(community_net_test PRIVATE src/runtime/kernel)
    target_compile_options(community_net_test PRIVATE -O2 -Wall -Wextra)
    target_link_libraries(community_net_test PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
    return()
endif()

if(NOT WIN32)
    message(FATAL_ERROR "Unsupported target system: ${CMAKE_SYSTEM_NAME}")
endif()

find_package(SDL3 3.4.0 REQUIRED CONFIG COMPONENTS SDL3-shared)
get_target_property(TTT2_SDL_DLL SDL3::SDL3-shared IMPORTED_LOCATION)
if(NOT TTT2_SDL_DLL)
    message(FATAL_ERROR "SDL3 shared library must have an importable DLL for the installer")
endif()
option(TTT2_LOCAL_SAVES "Enable the persistent local save container" ON)

# Recompiler output + XenonRecomp runtime headers (generated / third-party, not in repo).
set(PPC_DIR      "${CMAKE_SOURCE_DIR}/../re/ppc"                       CACHE PATH "generated PPC C++")
ttt2_generate_kernel_stubs(TTT2_KERNEL_STUBS "${PPC_DIR}" "${CMAKE_SOURCE_DIR}/src/runtime")
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

# Reuse an archive made from the same generated game during host-only builds.
set(TTT2_PPC_PREBUILT "" CACHE FILEPATH "Existing Windows PPC archive for host-only checks")
if(TTT2_PPC_PREBUILT)
    if(NOT EXISTS "${TTT2_PPC_PREBUILT}")
        message(FATAL_ERROR "Missing prebuilt PPC archive: ${TTT2_PPC_PREBUILT}")
    endif()
    add_library(tekken_ppc STATIC IMPORTED GLOBAL)
    set_target_properties(tekken_ppc PROPERTIES IMPORTED_LOCATION "${TTT2_PPC_PREBUILT}")
else()
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
endif()

# XMA audio decoding: a minimal static FFmpeg (C) built from the local download of its source.
add_subdirectory(third_party/ffmpeg_xma)

# The runtime executable: minimal host (memory, function table, entry) + kernel import stubs.
file(GLOB RUNTIME_KERNEL "${CMAKE_SOURCE_DIR}/src/runtime/kernel/*.cpp")
foreach(_excluded diag_hooks_linux.cpp host_settings_linux.cpp
                  steam_bridge_linux.cpp steam_bridge_linux_stub.cpp)
    list(REMOVE_ITEM RUNTIME_KERNEL "${CMAKE_SOURCE_DIR}/src/runtime/kernel/${_excluded}")
endforeach()
add_executable(tekken_recomp
    "${CMAKE_SOURCE_DIR}/src/runtime/main.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/runtime_math.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/guest_time.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/skin_profile.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/audio_output.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_input_sdl.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_thread_sdl.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_sleep.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_status.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_process.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_vm_windows.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_fault_windows.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_file_common.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_file_windows.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_storage_windows.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_udp_windows.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_random_windows.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_runtime_windows.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/phys_mirror_windows.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_window_sdl.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/host_window_win32.cpp"
    "${TTT2_KERNEL_STUBS}"
    ${RUNTIME_KERNEL})
set_source_files_properties(src/runtime/skin_profile.cpp PROPERTIES
    COMPILE_OPTIONS "${_ttt2_native_fp_option};${_ttt2_native_simd_option}")
target_include_directories(tekken_recomp PRIVATE
    "${PPC_DIR}" "${XENON_UTILS}" "${SIMDE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/runtime" "${CMAKE_SOURCE_DIR}/src/runtime/kernel")
target_compile_definitions(tekken_recomp PRIVATE TTT2_LOCAL_SAVES=$<BOOL:${TTT2_LOCAL_SAVES}>)
target_compile_definitions(tekken_recomp PRIVATE TTT2_DRAW_CAPTURE_DIAGNOSTICS=1)
target_compile_definitions(tekken_recomp PRIVATE TTT2_D3D11_MOVIE_BACKEND=1)
# Hardware renderer (gpu_hw_d3d11.h); still opt-in at run time with TTT2_GPU_HW=1 / compare.
target_compile_definitions(tekken_recomp PRIVATE TTT2_GPU_HW_BACKEND=1)
target_compile_definitions(tekken_recomp PRIVATE TTT2_GPU_HW_D3D11=1)
target_link_libraries(tekken_recomp PRIVATE SDL3::SDL3 d3d11 dxgi d3dcompiler)
add_custom_command(TARGET tekken_recomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>"
            "$<TARGET_FILE_DIR:tekken_recomp>")
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
target_link_libraries(tekken_recomp PRIVATE tekken_ppc ole32 winmm ttt2_ffmpeg bcrypt ws2_32)
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
            "${TTT2_GAME_EXE};${TTT2_SETUP_ICON};${TTT2_README_TXT};${TTT2_STEAM_DLL};${TTT2_SDL_DLL};${TTT2_SETUP_MANIFEST}")
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
