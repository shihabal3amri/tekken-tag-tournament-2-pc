"""title_storage.bin emulation regression (TTT2_TITLE_SERVER); no game run required.

Builds the production TitleStorageImage (port/src/runtime/kernel/title_server.cpp) and checks
it with the GAME'S OWN integrity check: sub_826DC518 -> sub_826DA208 (CRC32 over 0xFFF0 bytes),
extracted from re/ppc and run against the CRC table read from the local patched image. The
payload layout is then walked with the rules of the parser sub_826DCAA8 (header word 3; block
1 x 0x80 with the level at +4; block 2 x 0x30; blocks 3/4; consumed bytes == size field).
Negative control: one flipped payload byte must fail the game's CRC check.
No game data is copied into this test; the image file is only read at run time.
"""

from pathlib import Path
import struct
import subprocess
import tempfile

from test_vcmpgtuh import compile_one


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "port/src/runtime/kernel/title_server.cpp"
IMAGE = ROOT / "re/decrypted/Default_patched_tu3.bin"
CRC_TABLE = 0x820EA260

HARNESS = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ppc_config.h"
#include "ppc_context.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>
inline uint16_t GRead16(uint8_t* base, uint32_t ga) { return __builtin_bswap16(*reinterpret_cast<uint16_t*>(base + ga)); }
@HELPERS@
PPC_FUNC(sub_826DA208);
@GUEST@
int main(int argc, char** argv)
{
    uint8_t* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_READWRITE));
    if (!base || !VirtualAlloc(base + 0x00100000, 0x200000, MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc(base + 0x820EA000, 0x2000, MEM_COMMIT, PAGE_READWRITE)) return 2;
    // CRC table bytes from the local image (argv[1] = 1024 hex-free raw bytes file).
    FILE* f = std::fopen(argv[1], "rb");
    if (!f || std::fread(base + 0x820EA260, 1, 1024, f) != 1024) return 3;
    std::fclose(f);
    const int flip = argc > 2 ? std::atoi(argv[2]) : -1;
    std::vector<uint8_t> image = TitleStorageImage(3, 0xFFF4);
    if (image.size() != 0xFFF4) { std::fprintf(stderr, "FAIL size %zu\n", image.size()); return 1; }
    if (flip >= 0) image[flip] ^= 0x01;
    std::memcpy(base + 0x00100000, image.data(), image.size());
    PPCContext ctx{};
    ctx.r1.u64 = 0x00300000; ctx.r3.u64 = 0x00100000;
    sub_826DC518(ctx, base);
    if (ctx.r3.u32 != 1) { std::fprintf(stderr, "FAIL game CRC check returned %u\n", ctx.r3.u32); return 1; }
    _setmode(_fileno(stdout), _O_BINARY);
    fwrite(image.data(), 1, image.size(), stdout);
    return 0;
}
'''


def body(text, marker):
    start = text.index(marker)
    return text[start:text.index('\n}', start) + 2]


def parse(image):
    size = struct.unpack(">I", image[8:12])[0]
    src = image[0x14:]
    assert struct.unpack(">I", src[:4])[0] == 3, "payload header word"
    o = 0xC

    def header(kind, element, count=None):
        nonlocal o
        t, c, s, _ = struct.unpack(">IIII", src[o:o + 16])
        o += 16
        assert t == kind and s == element and (count is None or c == count), (t, c, s)
        return c
    header(1, 0x80, 1)
    level = struct.unpack(">I", src[o + 4:o + 8])[0]
    o += 0x80
    header(2, 0x30, 1)
    o += 0x30
    records = header(3, 0x1D0)          # (header() advances o: read the count first)
    o += 0x1D0 * records
    entries = header(4, 8)
    o += 8 * entries
    assert o == size, (hex(o), hex(size))
    return level


def main():
    source = SOURCE.read_text(encoding="utf-8")
    helpers = "namespace gin_service { bool Enabled(){return true;} }\n"
    helpers += body(source,"uint32_t Crc32NoFinalXor")+"\n"+body(source,"void Put32")+"\n"
    helpers += body(source, "std::vector<uint8_t> TitleStorageImage(uint32_t level, uint32_t bufferBytes)")
    guest = []
    for path in (ROOT / "re/ppc").glob("ppc_recomp.*.cpp"):
        text = path.read_text(encoding="utf-8-sig")
        for name in ("826DC518", "826DA208"):
            marker = "PPC_FUNC_IMPL(__imp__sub_" + name + ")"
            if marker in text:
                guest.append(body(text, marker).replace(marker, "PPC_FUNC(sub_" + name + ")"))
    assert len(guest) == 2, len(guest)
    harness = HARNESS.replace("@HELPERS@", helpers).replace("@GUEST@", "\n".join(guest))
    includes = [ROOT / "re/ppc", ROOT / "tools/XenonRecomp/XenonUtils", ROOT / "tools/XenonRecomp/thirdparty/simde"]
    with tempfile.TemporaryDirectory(prefix="ttt2-title-") as temp:
        folder = Path(temp)
        table = folder / "crc_table.bin"
        data = IMAGE.read_bytes()
        table.write_bytes(data[CRC_TABLE - 0x82000000:CRC_TABLE - 0x82000000 + 1024])
        exe = compile_one(folder, "title", harness, includes)
        result = subprocess.run([str(exe), str(table)], capture_output=True, timeout=30)
        if result.returncode:
            raise AssertionError(result.stderr.decode(errors="replace"))
        level = parse(result.stdout)
        assert level == 3, level
        print("PASS game CRC check (sub_826DC518) + parser layout: title_storage.bin level %d, %d bytes"
              % (level, len(result.stdout)))
        bad = subprocess.run([str(exe), str(table), "0x40"], capture_output=True, timeout=30)
        if bad.returncode != 1 or b"FAIL game CRC check" not in bad.stderr:
            raise AssertionError("corrupted image passed the game's CRC check")
        print("PASS negative control: one flipped payload byte fails the game's CRC check")


if __name__ == "__main__":
    main()
