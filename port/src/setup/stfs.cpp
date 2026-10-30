// STFS reader. See stfs.h; the block arithmetic mirrors port/tools/stfs.py line for line.
#include "setup/stfs.h"

#include <cstring>

namespace ttt2::setup {

static uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
static uint32_t Le24(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }
static uint32_t Be24(const uint8_t* p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }

bool StfsPackage::ReadAt(uint64_t off, void* buf, uint32_t n) {
    if (h_ == INVALID_HANDLE_VALUE || off + n > size_) return false;
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ::ReadFile(h_, buf, n, &got, nullptr) && got == n;
}

void StfsPackage::Close() {
    if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
    entries_.clear();
}

bool StfsPackage::Open(const wchar_t* path, std::string& err) {
    Close();
    h_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) { err = "The title update file could not be opened."; return false; }
    LARGE_INTEGER sz;
    GetFileSizeEx(h_, &sz);
    size_ = (uint64_t)sz.QuadPart;
    uint8_t hdr[0x1000];
    if (!ReadAt(0, hdr, sizeof hdr) ||
        (memcmp(hdr, "LIVE", 4) != 0 && memcmp(hdr, "PIRS", 4) != 0 && memcmp(hdr, "CON ", 4) != 0)) {
        err = "That file is not an Xbox 360 title update package.";
        Close(); return false;
    }
    headerSize_ = Be32(hdr + 0x340);
    contentType_ = Be32(hdr + 0x344);
    mediaId_ = Be32(hdr + 0x354);
    titleId_ = Be32(hdr + 0x360);
    tableBlocks_ = uint32_t(hdr[0x37B]) | uint32_t(hdr[0x37C]) << 8;
    tableBlock0_ = Le24(hdr + 0x37E);
    tableShift_ = (((headerSize_ + 0xFFF) & 0xF000) >> 0xC) == 0xB ? 0 : 1;
    if (!ReadTable(err)) { Close(); return false; }
    return true;
}

uint32_t StfsPackage::FixBlock(uint32_t block) const {
    uint32_t adj = 0;
    if (block >= 0xAA) adj += ((block / 0xAA) + 1) << tableShift_;
    if (block >= 0x70E4) adj += ((block / 0x70E4) + 1) << tableShift_;
    return block + adj;
}

// The next-block pointer of `block`, from its level-0 hash table entry (3 bytes at +0x15).
// Signed: the first group's table is backing block -1, i.e. the block just below 0xC000 (stfs.py
// gets this from Python's negative ints; unsigned arithmetic wrapped and lost every chain that
// starts in blocks 0..0xA9).
uint32_t StfsPackage::NextBlock(uint32_t block) {
    const int64_t table = int64_t(FixBlock(block)) - int64_t(block % 0xAA) - 1;
    const int64_t off = table * 0x1000 + 0xC000 + int64_t(block % 0xAA) * 0x18 + 0x15;
    uint8_t nb[3];
    if (off < 0 || !ReadAt(uint64_t(off), nb, 3)) return 0xFFFFFF;
    return Be24(nb);
}

bool StfsPackage::ReadTable(std::string& err) {
    uint32_t block = tableBlock0_;
    for (uint32_t i = 0; i < tableBlocks_ && i < 64; ++i) {
        uint8_t blk[0x1000];
        if (!ReadAt(BlockOffset(block), blk, sizeof blk)) { err = "The title update file is damaged."; return false; }
        for (uint32_t at = 0; at < 0x1000; at += 0x40) {
            const uint8_t* r = blk + at;
            const uint32_t nameLen = r[0x28] & 0x3F;
            if (!nameLen) continue;
            Entry e;
            e.name.assign((const char*)r, nameLen);
            e.dir = (r[0x28] & 0x80) != 0;
            e.blocks = Le24(r + 0x29);
            e.start = Le24(r + 0x2F);
            e.size = Be32(r + 0x34);
            entries_.push_back(e);
        }
        block = NextBlock(block);
        if (block == 0xFFFFFF || block == 0) break;
    }
    if (entries_.empty()) { err = "The title update file contains no files."; return false; }
    return true;
}

bool StfsPackage::ReadFile(const char* name, std::vector<uint8_t>& out) {
    for (const Entry& e : entries_) {
        if (e.dir || _stricmp(e.name.c_str(), name) != 0) continue;
        out.clear();
        out.reserve(e.size);
        uint32_t block = e.start, remaining = e.size, count = 0;
        while (remaining > 0 && block != 0xFFFFFF && block != 0 && count <= e.blocks + 1) {
            uint8_t blk[0x1000];
            if (!ReadAt(BlockOffset(block), blk, sizeof blk)) return false;
            const uint32_t take = remaining < 0x1000 ? remaining : 0x1000;
            out.insert(out.end(), blk, blk + take);
            remaining -= take;
            block = NextBlock(block);
            ++count;
        }
        return remaining == 0;
    }
    return false;
}

} // namespace ttt2::setup
