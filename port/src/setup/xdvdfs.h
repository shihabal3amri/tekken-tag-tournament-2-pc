// XDVDFS (Xbox / Xbox 360 disc filesystem) reader for the installer. Adapted from the Tom & Jerry
// port's installer (same project family, same rules).
//
// The installer ships NO game data - the player supplies their own disc image and we extract from
// it. That is what makes publishing it legal, and it means this reader has to cope with whatever
// the player actually points at: a redump XGD3 image (game partition at 0x2080000), an XGD2 image
// (0xFD90000), or a rebuilt / trimmed one (0).
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ttt2::setup {

struct IsoEntry {
    std::string path;      // relative, backslash-separated, e.g. "data000.bin"
    uint64_t    offset;    // absolute byte offset in the image
    uint32_t    size;
    bool        dir;
};

class XdvdfsImage {
public:
    ~XdvdfsImage() { Close(); }

    // Opens the image and enumerates the whole tree. `err` gets a user-facing reason on failure.
    bool Open(const wchar_t* isoPath, std::string& err);
    void Close();

    uint64_t PartitionBase() const { return base_; }
    const std::vector<IsoEntry>& Entries() const { return entries_; }
    const IsoEntry* Find(const char* path) const;          // case-insensitive, files only

    // Reads [offset, offset + n) of the image (absolute offsets, as in IsoEntry::offset).
    bool ReadAt(uint64_t off, void* buf, uint32_t n);
    bool ReadEntry(const IsoEntry& e, std::vector<uint8_t>& out);

private:
    bool WalkDir(uint32_t sector, uint32_t bytes, const std::string& prefix, int depth, std::string& err);

    HANDLE   h_ = INVALID_HANDLE_VALUE;
    uint64_t imageSize_ = 0;
    uint64_t base_ = 0;
    std::vector<IsoEntry> entries_;
};

} // namespace ttt2::setup
