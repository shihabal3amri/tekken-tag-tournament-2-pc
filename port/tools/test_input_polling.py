"""Build the production SDL input component and exercise its event path.

The probe feeds keyboard events and a real SDL virtual gamepad through the
snapshot used by the guest poll thread. It needs no game data or physical pad.
Set CMAKE_PREFIX_PATH or SDL3_DIR where SDL3 is not installed system-wide.
"""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
RUNTIME = (ROOT / "port/src/runtime").as_posix()
PROBE = (ROOT / "port/tools/sdl_input_probe.cpp").as_posix()


def main():
    with tempfile.TemporaryDirectory(prefix="ttt2-sdl-input-") as temp:
        folder = Path(temp)
        source = folder / "source"
        build = folder / "build"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(ttt2_sdl_input_probe CXX)\n"
            "set(CMAKE_CXX_STANDARD 20)\n"
            "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
            "find_package(SDL3 3.4.0 REQUIRED CONFIG)\n"
            f"add_executable(ttt2_sdl_input_probe \"{RUNTIME + '/host_input_sdl.cpp'}\" \"{PROBE}\")\n"
            f"target_include_directories(ttt2_sdl_input_probe PRIVATE \"{RUNTIME}\")\n"
            "target_link_libraries(ttt2_sdl_input_probe PRIVATE SDL3::SDL3)\n"
            "if(WIN32)\nadd_custom_command(TARGET ttt2_sdl_input_probe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:ttt2_sdl_input_probe>)\nendif()\n"
        )
        configure = ["cmake", "-S", str(source), "-B", str(build)]
        if shutil.which("ninja"):
            configure += ["-G", "Ninja"]
        subprocess.run(configure, check=True)
        subprocess.run(["cmake", "--build", str(build), "--parallel", "2"], check=True)
        exe = build / ("ttt2_sdl_input_probe.exe" if os.name == "nt" else "ttt2_sdl_input_probe")
        env = dict(os.environ, SDL_VIDEODRIVER="dummy")
        subprocess.run([str(exe)], env=env, check=True, timeout=15)
        subprocess.run([str(exe)], env=dict(env, TTT2_SDL_TEST_PAD_SCRIPT="10:A:40"),
                       check=True, timeout=15)
        subprocess.run([str(exe)], env=dict(env, TTT2_SDL_TEST_PAD_SCRIPT="10:START:40"),
                       check=True, timeout=15)


if __name__ == "__main__":
    main()
