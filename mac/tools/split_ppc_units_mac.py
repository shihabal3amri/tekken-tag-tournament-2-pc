#!/usr/bin/env python3
"""Split oversized generated PPC units at complete function boundaries (macOS form).

Same job and output names as port/tools/split_ppc_units.py, which only understands the
alias layout XenonRecomp writes when it was built off Apple:

    __attribute__((alias("__imp__sub_X"))) PPC_WEAK_FUNC(sub_X);
    PPC_FUNC_IMPL(__imp__sub_X) { ... }

XenonRecomp built on Apple (its CMakeLists drops XENON_RECOMP_USE_ALIAS, because Mach-O has
no symbol aliases) writes each function followed by a weak wrapper instead:

    PPC_FUNC_IMPL(__imp__sub_X) { ... }

    PPC_WEAK_FUNC(sub_X) {
    	__imp__sub_X(ctx, base);
    }

A function and its wrapper are kept together. Both layouts are accepted.
"""

import argparse
import os
from pathlib import Path
import re
import sys

PROLOGUE = b'#include "ppc_recomp_shared.h"\n\n'
ALIAS = re.compile(rb'^__attribute__\(\(alias\("[^\n"]+"\)\)\) PPC_WEAK_FUNC\([^\n]+\);', re.MULTILINE)
FUNCTION = re.compile(rb'^PPC_FUNC_IMPL\([^\n]+\) \{', re.MULTILINE)
WRAPPER = re.compile(rb'^PPC_WEAK_FUNC\(([^\n)]+)\) \{\n\t__imp__\1\(ctx, base\);\n\}', re.MULTILINE)
NUMBERED_UNIT = re.compile(r'ppc_recomp\.[0-9]+\.cpp\Z')


def write_if_changed(path: Path, content: bytes) -> None:
    if path.is_file() and path.read_bytes() == content:
        return
    temporary = path.with_name(f'{path.name}.{os.getpid()}.tmp')
    try:
        temporary.write_bytes(content)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def split_unit(source: Path, output_dir: Path, chunk_bytes: int) -> list[Path]:
    data = source.read_bytes()
    functions = len(FUNCTION.findall(data))
    alias_starts = [m.start() for m in ALIAS.finditer(data)]
    if alias_starts:
        starts = alias_starts
    else:
        starts = [m.start() for m in FUNCTION.finditer(data)]
        if len(WRAPPER.findall(data)) != functions:
            raise ValueError(f'{source}: every function needs exactly one weak wrapper')
    if not starts or data[:starts[0]] != PROLOGUE or len(starts) != functions:
        raise ValueError(f'{source}: unexpected generated PPC function layout')

    outputs: list[Path] = []
    chunk = bytearray(PROLOGUE)

    def emit() -> None:
        path = output_dir / f'{source.stem}.part.{len(outputs):03d}.cpp'
        write_if_changed(path, bytes(chunk))
        outputs.append(path)

    for index, start in enumerate(starts):
        end = starts[index + 1] if index + 1 < len(starts) else len(data)
        segment = data[start:end]
        if len(FUNCTION.findall(segment)) != 1 or not segment.rstrip().endswith(b'}'):
            raise ValueError(f'{source}: incomplete generated function {index}')
        if not alias_starts and len(WRAPPER.findall(segment)) != 1:
            raise ValueError(f'{source}: function {index} lost its weak wrapper')
        if len(PROLOGUE) + len(segment) > chunk_bytes:
            raise ValueError(f'{source}: generated function {index} exceeds chunk limit')
        if len(chunk) > len(PROLOGUE) and len(chunk) + len(segment) > chunk_bytes:
            emit()
            chunk = bytearray(PROLOGUE)
        chunk.extend(segment)
    emit()

    for stale in output_dir.glob(f'{source.stem}.part.*.cpp'):
        if stale not in outputs:
            stale.unlink()
    return outputs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--threshold-bytes', required=True, type=int)
    parser.add_argument('--chunk-bytes', required=True, type=int)
    parser.add_argument('sources', nargs='+', type=Path)
    args = parser.parse_args()
    if args.chunk_bytes <= len(PROLOGUE) or args.threshold_bytes <= args.chunk_bytes:
        parser.error('chunk size must be positive and smaller than the split threshold')
    args.output_dir.mkdir(parents=True, exist_ok=True)
    try:
        for source in args.sources:
            source = source.resolve(strict=True)
            if source.stat().st_size <= args.threshold_bytes or not NUMBERED_UNIT.fullmatch(source.name):
                print(source)
            else:
                for chunk in split_unit(source, args.output_dir.resolve(), args.chunk_bytes):
                    print(chunk)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
