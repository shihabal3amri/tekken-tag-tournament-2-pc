#!/usr/bin/env python3
"""Build the generated game PPC code as a macOS static archive (arm64 or x86_64).

Modelled on port/tools/build_android_ppc_archive.py: split large units at function
boundaries, force-include the arm64 header overlay (arm64 only) and the generated-code
store guard, compile every unit with Ninja, archive with libtool/ar. x86_64 compiles the
generated code as their Linux build does: native SSE, -march=x86-64-v2, no overlay.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def ninja_path(path: Path) -> str:
    return str(path).replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def write_if_changed(path: Path, content: str) -> None:
    if not path.is_file() or path.read_text() != content:
        path.write_text(content)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ppc-dir", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True, help="objects, split units, build.ninja")
    parser.add_argument("--out-dir", type=Path, required=True, help="archive + headers/")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--opt-level", choices=("O0", "O1", "O2", "O3"), default="O1")
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--arch", choices=("arm64", "x86_64"), default="arm64")
    parser.add_argument("--extra-flag", action="append", default=[])
    args = parser.parse_args()

    mac_root = Path(__file__).resolve().parents[1]
    source_root = mac_root.parent
    ppc_dir = args.ppc_dir.resolve(strict=True)
    build_dir = args.build_dir.resolve()
    out_dir = args.out_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    header_dir = out_dir / "headers"
    arm64 = args.arch == "arm64"
    if arm64:
        subprocess.run([sys.executable, str(mac_root / "tools/prepare_mac_ppc_headers.py"),
                        "--ppc-dir", str(ppc_dir), "--output-dir", str(header_dir)], check=True)

    sources = sorted(ppc_dir.glob("ppc_recomp.*.cpp"))
    mapping = ppc_dir / "ppc_func_mapping.cpp"
    if not sources or not mapping.is_file():
        parser.error(f"generated PPC sources are missing from {ppc_dir}")
    split = subprocess.run(
        [sys.executable, str(mac_root / "tools/split_ppc_units_mac.py"),
         "--output-dir", str(build_dir / "split"), "--threshold-bytes", "4194304",
         "--chunk-bytes", "2097152", *(str(path) for path in sources), str(mapping)],
        check=True, text=True, capture_output=True)
    compile_sources = [Path(line) for line in split.stdout.splitlines()]

    utils_dir = source_root / "tools/XenonRecomp/XenonUtils"
    simde_dir = source_root / "tools/XenonRecomp/thirdparty/simde"
    runtime_dir = source_root / "port/src/runtime"
    guard = runtime_dir / "ppc_store_guard_linux.h"
    for directory in (utils_dir, simde_dir, runtime_dir):
        if not directory.is_dir():
            parser.error(f"missing generated PPC dependency: {directory}")

    compiler = shutil.which(args.cxx) or args.cxx
    # -ffp-contract=off: no fused multiply-add the guest did not ask for (their Windows/Linux
    # builds use the same); the explicit fmadd sites use std::fma (see mac/DETERMINISM.md).
    # x86_64: their Linux archive's TTT2_LINUX_PPC_MARCH, so the generated code runs the same
    # SSE instructions as the Windows and Linux builds (Rosetta translates x86-64-v2 as well).
    flags = ["-std=c++20", f"-{args.opt_level}", "-arch", args.arch, "-mmacosx-version-min=13.0",
             *([] if arm64 else ["-march=x86-64-v2"]),
             "-ffp-contract=off", "-fno-exceptions", "-fno-rtti", "-finline", "-Wno-everything",
             *args.extra_flag]
    for directory in ((header_dir,) if arm64 else ()) + (ppc_dir, utils_dir, simde_dir, runtime_dir):
        flags.extend(["-I", str(directory)])
    if arm64:
        flags.extend(["-include", str(header_dir / "ppc_arm64_compat.h")])
    flags.extend(["-include", str(guard)])

    objects_dir = build_dir / "objects"
    objects_dir.mkdir(exist_ok=True)
    objects = [objects_dir / f"{index:04d}-{source.stem}.o" for index, source in enumerate(compile_sources)]
    for stale in objects_dir.glob("*.o"):
        if stale not in objects:
            stale.unlink()
    archive = out_dir / "libttt2_mac_ppc.a"

    flag_file = build_dir / "compile_flags.rsp"
    write_if_changed(flag_file, "\n".join(flags) + "\n")
    # libtool -static takes a file list; ar would need one huge command line.
    list_file = build_dir / "objects.list"
    write_if_changed(list_file, "\n".join(map(str, objects)) + "\n")
    ninja = ["rule cxx",
             f"  command = {ninja_path(Path(compiler))} @{ninja_path(flag_file)} -MMD -MT $out -MF $out.d -c $in -o $out",
             "  depfile = $out.d",
             "  deps = gcc",
             "  description = CXX $in",
             "rule lib",
             f"  command = rm -f $out && libtool -static -no_warning_for_no_symbols -o $out -filelist {ninja_path(list_file)}",
             "  description = LIBTOOL $out"]
    overlay = (header_dir / "ppc_arm64_compat.h", header_dir / "ppc_context_arm64.h") if arm64 else ()
    implicit = " ".join(ninja_path(p) for p in (*overlay, guard, flag_file))
    for source, obj in zip(compile_sources, objects):
        ninja.append(f"build {ninja_path(obj)}: cxx {ninja_path(source)} | {implicit}")
    ninja.append(f"build {ninja_path(archive)}: lib " + " ".join(map(ninja_path, objects))
                 + f" | {ninja_path(list_file)}")
    ninja.append(f"default {ninja_path(archive)}")
    write_if_changed(build_dir / "build.ninja", "\n".join(ninja) + "\n")
    print(f"Compiling {len(compile_sources)} generated PPC units at -{args.opt_level} for macOS {args.arch}", flush=True)
    subprocess.run(["ninja", "-C", str(build_dir), "-f", str(build_dir / "build.ninja"), "-j", str(args.jobs)],
                   check=True)
    print(f"Archive: {archive}")


if __name__ == "__main__":
    main()
