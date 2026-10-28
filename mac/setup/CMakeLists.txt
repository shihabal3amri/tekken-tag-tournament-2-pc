# macOS build of shihab's POSIX setup tools (the Android importer's desktop CLI) plus a small
# inputs tool for the code generator. Configure from the checkout root:
#   cmake -S mac/setup -B mac/build/setup -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build mac/build/setup
cmake_minimum_required(VERSION 3.22)
project(ttt2_setup_mac LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(checkout "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(TTT2_GAME_SOURCE "${checkout}" CACHE PATH "Checkout with tools/XenonRecomp")
# port/android/setup builds ttt2_setup_core and, off Android, ttt2_extract (ISO + TU3 -> game folder).
add_subdirectory("${checkout}/port/android/setup" android_setup)
# ttt2_inputs: ISO + TU3 -> re/Default.xex, re/extracted_tu/<TU>/Default.xexp, re/decrypted image.
add_executable(ttt2_inputs inputs_cli.cpp)
target_link_libraries(ttt2_inputs PRIVATE ttt2_setup_core)
