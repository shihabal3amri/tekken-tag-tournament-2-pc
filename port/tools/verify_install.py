"""Compare an installed game folder (ttt2_setup output) with the dev reference tree.

    python -I port/tools/verify_install.py "D:\\ttt2_install_test\\Tekken Tag Tournament 2"

Reference: re/game_files_x (expanded disc + update\\) and re/decrypted/Default_patched_tu3.bin.
Every file of <install>\\game must exist in the reference and match byte for byte; the reference
files the installer is expected to produce must all be present. Exit 0 = identical.
"""
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REF = os.path.join(ROOT, "re", "game_files_x")
REF_IMAGE = os.path.join(ROOT, "re", "decrypted", "Default_patched_tu3.bin")
# Files in the dev tree that are not disc / installer output (the raw TU archive left in the root).
REF_EXTRA = {"data100.bin", "data100.ofs"}


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 22)
            if not b:
                return h.hexdigest()
            h.update(b)


def files_under(base):
    out = {}
    for dirpath, _, names in os.walk(base):
        for n in names:
            full = os.path.join(dirpath, n)
            out[os.path.relpath(full, base).lower()] = full
    return out


def main():
    install = sys.argv[1]
    game = os.path.join(install, "game")
    got = files_under(game)
    want = {k: v for k, v in files_under(REF).items() if k not in REF_EXTRA}
    want["tekken.img"] = REF_IMAGE
    bad = 0
    for rel in sorted(set(got) | set(want)):
        if rel not in got:
            print(f"MISSING  {rel}"); bad += 1; continue
        if rel not in want:
            print(f"EXTRA    {rel}"); bad += 1; continue
        a, b = got[rel], want[rel]
        if os.path.getsize(a) != os.path.getsize(b):
            print(f"SIZE     {rel}: {os.path.getsize(a)} != {os.path.getsize(b)}"); bad += 1; continue
        ha, hb = sha1(a), sha1(b)
        print(f"{'OK      ' if ha == hb else 'DIFFER  '} {rel}  {ha}")
        bad += ha != hb
    for n in ("Tekken Tag Tournament 2.exe", "steam_api64.dll", "msvcp140.dll", "msvcp140_atomic_wait.dll",
              "vcruntime140.dll", "vcruntime140_1.dll", "README.txt"):
        if not os.path.isfile(os.path.join(install, n)):
            print(f"MISSING  {n}"); bad += 1
    print(f"{len(got)} game files checked, {bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
