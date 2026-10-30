// XDVDFS reader. See xdvdfs.h.
//
// Carried over from the Tom & Jerry installer, where both rules were MEASURED:
//  1. The partition base is probed, and the volume descriptor magic is checked at BOTH +0x00
//     and +0x7EC before a base is accepted. Xbox 360 images add the XGD2 / XGD3 bases.
//  2. The directory table is scanned LINEARLY, not walked as a tree: following the left/right
//     links silently dropped files on that disc. A linear walk (4-byte aligned entries,
//     0xFFFFFFFF padding skipping to the next 2048-byte sector) finds every entry.
#include "setup/xdvdfs.h"

#include <cstring>

namespace ttt2::setup {

static const uint32_t kSector = 2048;
static const char kMagic[20] = { 'M','I','C','R','O','S','O','F','T','*','X','B','O','X',
                                 '*','M','E','D','I','A' };
static const int    kMaxDepth = 16;          // a malformed or hostile image must not spin us
static const size_t kMaxEntries = 100000;

bool XdvdfsImage::ReadAt(uint64_t off, void* buf, uint32_t n) {
    if (h_ == INVALID_HANDLE_VALUE) return false;
    if (off + n > imageSize_) return false;
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) return false;
    uint8_t* p = (uint8_t*)buf;
    uint32_t left = n;
    while (left) {
        DWORD got = 0;
        if (!ReadFile(h_, p, left, &got, nullptr) || got == 0) return false;
        p += got; left -= got;
    }
    return true;
}

void XdvdfsImage::Close() {
    if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
    entries_.clear();
}

bool XdvdfsImage::Open(const wchar_t* isoPath, std::string& err) {
    Close();
    h_ = CreateFileW(isoPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) { err = "The disc image could not be opened."; return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h_, &sz)) { err = "The disc image could not be read."; Close(); return false; }
    imageSize_ = (uint64_t)sz.QuadPart;

    // XGD3 (redump), XGD2, then a rebuilt image whose game partition starts at 0.
    static const uint64_t kBases[] = { 0x2080000ull, 0xFD90000ull, 0x0ull, 0x18300000ull };
    uint8_t desc[kSector];
    bool found = false;
    for (uint64_t cand : kBases) {
        if (!ReadAt(cand + 0x10000, desc, kSector)) continue;
        if (memcmp(desc, kMagic, 20) == 0 && memcmp(desc + 0x7EC, kMagic, 20) == 0) {
            base_ = cand; found = true; break;
        }
    }
    if (!found) {
        err = "That file is not an Xbox 360 disc image (no game partition was found).";
        Close(); return false;
    }

    uint32_t rootSector = 0, rootSize = 0;
    memcpy(&rootSector, desc + 0x14, 4);
    memcpy(&rootSize,   desc + 0x18, 4);
    entries_.clear();
    if (!WalkDir(rootSector, rootSize, "", 0, err)) { Close(); return false; }
    return true;
}

bool XdvdfsImage::WalkDir(uint32_t sector, uint32_t bytes, const std::string& prefix,
                          int depth, std::string& err) {
    if (depth > kMaxDepth || bytes == 0) return true;
    if (bytes > 8u * 1024u * 1024u) { err = "The disc image's directory table is corrupt."; return false; }

    std::vector<uint8_t> tbl(bytes);
    if (!ReadAt(base_ + (uint64_t)sector * kSector, tbl.data(), bytes)) {
        err = "The disc image is truncated or unreadable.";
        return false;
    }

    struct Sub { uint32_t sector, size; std::string path; };
    std::vector<Sub> subdirs;

    uint32_t pos = 0;
    while (pos + 14 <= bytes) {
        uint32_t marker = 0;
        memcpy(&marker, tbl.data() + pos, 4);
        if (marker == 0xFFFFFFFFu) {
            pos = (pos / kSector + 1) * kSector;      // padding: jump to the next sector
            continue;
        }
        uint32_t start = 0, fsize = 0;
        memcpy(&start, tbl.data() + pos + 4, 4);
        memcpy(&fsize, tbl.data() + pos + 8, 4);
        uint8_t attr = tbl[pos + 12];
        uint8_t nlen = tbl[pos + 13];
        if (nlen == 0 || pos + 14 + nlen > bytes) {
            pos = (pos / kSector + 1) * kSector;
            continue;
        }
        std::string name((const char*)tbl.data() + pos + 14, nlen);
        std::string full = prefix.empty() ? name : prefix + "\\" + name;
        if (entries_.size() >= kMaxEntries) { err = "The disc image's directory table is corrupt."; return false; }

        IsoEntry e;
        e.path = full;
        e.size = fsize;
        e.dir = (attr & 0x10) != 0;
        e.offset = base_ + (uint64_t)start * kSector;
        entries_.push_back(e);
        if (e.dir) subdirs.push_back({ start, fsize, full });
        pos += (14u + nlen + 3u) & ~3u;
    }
    for (const Sub& s : subdirs)
        if (!WalkDir(s.sector, s.size, s.path, depth + 1, err)) return false;
    return true;
}

const IsoEntry* XdvdfsImage::Find(const char* path) const {
    for (const IsoEntry& e : entries_)
        if (!e.dir && _stricmp(e.path.c_str(), path) == 0) return &e;
    return nullptr;
}

bool XdvdfsImage::ReadEntry(const IsoEntry& e, std::vector<uint8_t>& out) {
    if (e.dir) return false;
    out.resize(e.size);
    return e.size == 0 || ReadAt(e.offset, out.data(), e.size);
}

} // namespace ttt2::setup
