// STFS reader. See stfs.h; the block arithmetic mirrors port/tools/stfs.py line for line.
#include "setup/stfs.h"

#include <bcrypt.h>
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
    uint8_t* hdr = header_;
    if (!ReadAt(0, hdr, sizeof header_) ||
        (memcmp(hdr, "LIVE", 4) != 0 && memcmp(hdr, "PIRS", 4) != 0 && memcmp(hdr, "CON ", 4) != 0)) {
        err = "That file is not an Xbox 360 title update package.";
        Close(); return false;
    }
    headerSize_ = Be32(hdr + 0x340);
    contentType_ = Be32(hdr + 0x344);
    mediaId_ = Be32(hdr + 0x354);
    titleId_ = Be32(hdr + 0x360);
    // Volume descriptor at 0x379: size, reserved, block separation, then the file table's block count (16 bits, little
    // endian) at 0x37C and its first block (24 bits) at 0x37E. MEASURED 2026-10-31 on the DLC packages: 0x37B is the block
    // separation byte (1), so reading the count from there gave 257.
    tableBlocks_ = uint32_t(hdr[0x37C]) | uint32_t(hdr[0x37D]) << 8;
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

// The tables sit just below their group's first data block: one block (shift 0) or two copies (shift 1). Signed: the
// first group's table is below 0xC000 (stfs.py gets this from Python's negative ints; unsigned arithmetic wrapped and
// lost every chain that starts in blocks 0..0xA9).
int64_t StfsPackage::HashTableBlock(uint32_t block, int copy) const {
    return int64_t(FixBlock(block)) - int64_t(block % 0xAA) - (int64_t(1) << tableShift_) + copy;
}

// The next-block pointer of `block`, from its level-0 hash table entry (3 bytes at +0x15).
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
            e.consecutive = (r[0x28] & 0x40) != 0;
            e.blocks = Le24(r + 0x29);
            e.start = Le24(r + 0x2F);
            e.parent = int16_t(uint16_t(r[0x32]) << 8 | r[0x33]);
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

// "dir\\name" from the parent chain (-1 = the package's root); empty for a broken chain or an unsafe name.
std::string StfsPackage::PathOf(size_t index) const {
    std::string path;
    for (size_t i = index, depth = 0; depth < 16; ++depth) {
        const std::string& n = entries_[i].name;
        if (n.empty() || n == "." || n == ".." || n.find_first_of("\\/:*?\"<>|") != std::string::npos) return {};
        path = path.empty() ? n : n + "\\" + path;
        const int16_t parent = entries_[i].parent;
        if (parent == -1) return path;
        if (parent < 0 || size_t(parent) >= entries_.size() || !entries_[size_t(parent)].dir) return {};
        i = size_t(parent);
    }
    return {};
}

std::vector<StfsPackage::FileInfo> StfsPackage::Files() const {
    std::vector<FileInfo> out;
    for (size_t i = 0; i < entries_.size(); ++i)
        if (!entries_[i].dir) out.push_back({ PathOf(i), entries_[i].size });
    return out;
}

bool StfsPackage::ExtractFile(const std::string& path, const Sink& sink, std::string& err) {
    const Entry* e = nullptr;
    for (size_t i = 0; i < entries_.size(); ++i)
        if (!entries_[i].dir && _stricmp(PathOf(i).c_str(), path.c_str()) == 0) e = &entries_[i];
    if (!e) { err = "a file is missing from the package"; return false; }
    const uint32_t count = uint32_t((uint64_t(e->size) + 0xFFF) >> 12);
    std::vector<uint8_t> run(0xAA * 0x1000), tables[2] = { std::vector<uint8_t>(0x1000), std::vector<uint8_t>(0x1000) };
    const int copies = tableShift_ ? 2 : 1;
    uint32_t group = 0xFFFFFFFF, block = e->start;
    uint64_t left = e->size;
    for (uint32_t index = 0; index < count;) {
        if (block == 0xFFFFFF || block >= 0x1000000) { err = "the package is damaged (a broken block chain)"; return false; }
        // A consecutive file's blocks run unbroken up to the end of their hash group (0xAA blocks); others go block by block.
        const uint32_t n = e->consecutive ? (std::min)(0xAAu - block % 0xAA, count - index) : 1;
        if (!ReadAt(BlockOffset(block), run.data(), n * 0x1000)) { err = "the package is incomplete (it ends early)"; return false; }
        if (block / 0xAA != group) {
            group = block / 0xAA;
            for (int c = 0; c < copies; ++c) {
                const int64_t t = HashTableBlock(block, c);
                if (t * 0x1000 + 0xC000 < 0 || !ReadAt(uint64_t(t * 0x1000 + 0xC000), tables[c].data(), 0x1000))
                    { err = "the package is incomplete (it ends early)"; return false; }
            }
        }
        int matched = -1;
        for (uint32_t k = 0; k < n; ++k) {
            uint8_t digest[20] = {};
            BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, run.data() + k * 0x1000, 0x1000, digest, sizeof digest);
            const uint32_t slot = (block + k) % 0xAA * 0x18;
            matched = -1;
            for (int c = 0; c < copies && matched < 0; ++c)
                if (!memcmp(tables[c].data() + slot, digest, 20)) matched = c;
            if (matched < 0) { err = "the package is damaged (its data does not match its own checksums)"; return false; }
        }
        const uint32_t bytes = uint32_t((std::min<uint64_t>)(uint64_t(n) * 0x1000, left));
        if (!sink(run.data(), bytes)) { err = "the file could not be written"; return false; }
        left -= bytes;
        index += n;
        block = e->consecutive ? block + n : Be24(tables[matched].data() + block % 0xAA * 0x18 + 0x15);
    }
    return true;
}

} // namespace ttt2::setup
