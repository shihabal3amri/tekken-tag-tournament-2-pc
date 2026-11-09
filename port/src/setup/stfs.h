// Read-only STFS (LIVE / PIRS / CON) package reader: pulls the title update's files (the
// Default.xexp delta patch and data100.bin / .ofs) out of the player's own TU file, and extracts the
// player's downloadable content packages (dlc_install.cpp). C++ port of port/tools/stfs.py (which
// extracted the project's reference copy), same block addressing.
#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>
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
    const uint8_t* Header() const { return header_; }   // the first 0x1000 bytes (display name at 0x411, ...)
    // Reads a file (top level of the package) by name; false when absent or unreadable.
    bool ReadFile(const char* name, std::vector<uint8_t>& out);

    // The package's files, with their folders ("dir\\name").
    struct FileInfo { std::string path; uint64_t size; };
    std::vector<FileInfo> Files() const;
    // Streams a file (a path from Files()) to `sink` in pieces, every 4 KB block checked against the package's own
    // level-0 hash table (SHA-1). False with `err` set when a block does not match (a damaged package), a read fails,
    // or `sink` returns false.
    using Sink = std::function<bool(const uint8_t* data, uint32_t n)>;
    bool ExtractFile(const std::string& path, const Sink& sink, std::string& err);

private:
    struct Entry { std::string name; uint32_t size, start, blocks; bool dir, consecutive; int16_t parent; };
    bool ReadAt(uint64_t off, void* buf, uint32_t n);
    uint32_t FixBlock(uint32_t block) const;
    uint64_t BlockOffset(uint32_t block) const { return 0xC000ull + ((uint64_t)FixBlock(block) << 12); }
    // The backing block of `block`'s level-0 hash table (copy 0 or 1; read-only packages of shift 1 carry two).
    int64_t HashTableBlock(uint32_t block, int copy) const;
    uint32_t NextBlock(uint32_t block);
    bool ReadTable(std::string& err);
    std::string PathOf(size_t index) const;

    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
    uint8_t header_[0x1000] = {};
    uint32_t titleId_ = 0, mediaId_ = 0, contentType_ = 0, headerSize_ = 0;
    uint32_t tableBlock0_ = 0, tableBlocks_ = 0, tableShift_ = 1;
    std::vector<Entry> entries_;
};

} // namespace ttt2::setup
