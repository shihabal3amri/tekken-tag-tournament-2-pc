# Platform glue check for macOS (mac/tests/platform): the port's SDL window, input, audio and
# thread hosts plus apple_game_entry.cpp, linked against a small test "game" instead of the real
# one. Build: cmake -S mac/tests/platform -B mac/build/platform-check -G Ninja && cmake --build ...
cmake_minimum_required(VERSION 3.20)
project(ttt2_platform_check C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(SDL3 3.4.0 REQUIRED CONFIG)
find_package(Vulkan REQUIRED)   # headers only; the loader comes from SDL_Vulkan_GetVkGetInstanceProcAddr
set(runtime ${CMAKE_CURRENT_LIST_DIR}/../../../port/src/runtime)
add_executable(platform_check
    platform_check.cpp
    ${runtime}/apple_game_entry.cpp
    ${runtime}/host_window_sdl.cpp
    ${runtime}/host_input_sdl.cpp
    ${runtime}/audio_output.cpp
    ${runtime}/host_thread_sdl.cpp)
target_include_directories(platform_check PRIVATE ${runtime})
target_compile_definitions(platform_check PRIVATE TTT2_HOST_GAME_UI=1)
target_link_libraries(platform_check PRIVATE SDL3::SDL3 Vulkan::Headers)
