// The TTT2-specific install steps, all from the player's own files:
//  - the game image: base Default.xex (disc) + TU3 Default.xexp (title update) -> XenonUtils'
//    XexPatcher -> the decrypted, uncompressed image the port loads (game\tekken.img), checked
//    against the SHA-1 of the image the port was recompiled from;
//  - the archives: 26 of the 42 dataNNN.bin (and TU3's data100.bin) are Xbox LZX containers
//    (magic 0x0FF512ED) that the game can only read expanded; sizes come from nrfo.bin / .ofs.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ttt2::setup {

// The disc this port was built from (re/disc_manifest.json) and the image it was recompiled from.
constexpr uint32_t kTitleId = 0x4E4D0859u;
constexpr uint32_t kMediaId = 0x5D762D06u;
extern const char kBaseXexSha1[];            // Default.xex 0.0.0.2
extern const char kImageSha1[];              // base + TU3 (0.0.3.2), decrypted image

std::string Sha1Hex(const void* data, size_t size);

// Target version of a title update's Default.xexp (e.g. "0.0.3.2"); empty if it is not one.
std::string XexpTargetVersion(const std::vector<uint8_t>& xexp);

// Base XEX + delta patch -> the loaded image. `err` is user-facing.
bool BuildGameImage(const std::vector<uint8_t>& baseXex, const std::vector<uint8_t>& xexp,
                    std::vector<uint8_t>& image, std::string& err);

// Expanded size of each archive the master table names: record i of nrfo.bin describes
// `names[i]` (the archives sorted by name); 0 when a record has no offset table.
std::map<std::string, uint64_t> ArchiveSizes(const std::vector<uint8_t>& nrfo, const std::vector<std::string>& names);
// The same rule for one record on its own (TU3's data100.ofs).
uint64_t RecordExpandedSize(const std::vector<uint8_t>& record);

bool IsLzxContainer(const uint8_t* first4);

// Reads `n` bytes at `offset` of the source (an archive inside the disc image, or a buffer).
using SourceReader = std::function<bool(uint64_t offset, void* buf, uint32_t n)>;
// Progress: bytes written so far for this archive; return false to cancel.
using ByteProgress = std::function<bool(uint64_t done)>;

// Expands an LZX container of `sourceSize` bytes into `outPath`, which must come out exactly
// `expected` bytes long with an 'nrfi' archive header.
bool ExpandContainer(const SourceReader& read, uint64_t sourceSize, uint64_t expected,
                     const std::wstring& outPath, const ByteProgress& progress, std::string& err);

} // namespace ttt2::setup
