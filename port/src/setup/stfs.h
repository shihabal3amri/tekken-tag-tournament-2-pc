// Read-only STFS (LIVE / PIRS / CON) package reader: pulls the title update's files (the
// Default.xexp delta patch and data100.bin / .ofs) out of the player's own TU file. C++ port of
// port/tools/stfs.py (which extracted the project's reference copy), same block addressing.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ttt2::setup {

class StfsPackage {
public:
    ~StfsPackage() { Close(); }
    bool Open(const wchar_t* path, std::string& err);
    void Close();

    uint32_t TitleId() const { return titleId_; }
    uint32_t MediaId() const { return mediaId_; }
    uint32_t ContentType() const { return contentType_; }
    // Reads a file (top level of the package) by name; false when absent or unreadable.
    bool ReadFile(const char* name, std::vector<uint8_t>& out);

private:
    struct Entry { std::string name; uint32_t size, start, blocks; bool dir; };
    bool ReadAt(uint64_t off, void* buf, uint32_t n);
    uint32_t FixBlock(uint32_t block) const;
    uint64_t BlockOffset(uint32_t block) const { return 0xC000ull + ((uint64_t)FixBlock(block) << 12); }
    uint32_t NextBlock(uint32_t block);
    bool ReadTable(std::string& err);

    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
    uint32_t titleId_ = 0, mediaId_ = 0, contentType_ = 0, headerSize_ = 0;
    uint32_t tableBlock0_ = 0, tableBlocks_ = 0, tableShift_ = 1;
    std::vector<Entry> entries_;
};

} // namespace ttt2::setup
