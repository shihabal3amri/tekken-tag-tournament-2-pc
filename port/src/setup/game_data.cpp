// See game_data.h. The LZX container code is tools/x_decompress/x_decompress.cpp's tabled path
// (verified there on all 26 compressed archives), reading chunk by chunk from the disc image
// instead of loading whole archives (the largest expands to 850 MB).
#include "setup/game_data.h"

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "xex_patcher.h"

extern "C" {
#include <mspack.h>
#include <lzx.h>
}

namespace ttt2::setup {

const char kBaseXexSha1[] = "a942090b7c293d0ce63346ed8a2974448135d0dd";
const char kImageSha1[] = "66adbc1a7744ec840a3ae8e51f8aeb677140d789";

namespace {

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

// ---- nrfo boxes: {u32 size, char[4] kind} ...; 'ofsd' holds u32 offsets after an 8-byte head.
uint64_t ExpandedFromRecord(const uint8_t* rec, size_t size)
{
    size_t off = 0;
    while (off + 8 <= size) {
        const uint32_t boxSize = Be32(rec + off);
        if (memcmp(rec + off + 4, "ofsd", 4) == 0 && boxSize >= 8 && off + boxSize <= size) {
            uint32_t best = 0;
            for (size_t k = 8; k + 4 <= boxSize; k += 4) best = (std::max)(best, Be32(rec + off + k));
            return uint64_t(best) + 256;
        }
        if (boxSize < 8) break;
        off += boxSize;
    }
    return 0;
}

// ---- libmspack plumbing (x_decompress.cpp)
struct ReadStream { const uint8_t* data; const uint8_t* end; int size; int pad; };
struct WriteStream { uint8_t* data; size_t size; };

int MspackRead(mspack_file* file, void* buffer, int bytes)
{
    auto* s = reinterpret_cast<ReadStream*>(file);
    if (s->size == 0) {
        // Frames start on even bytes: an odd frame owes one synthesised pad byte.
        if (s->pad) {
            s->pad = 0;
            if (bytes < 1) return 0;
            *reinterpret_cast<uint8_t*>(buffer) = 0;
            return 1;
        }
        if (s->data + 2 > s->end) return 0;
        uint16_t size = Be16(s->data);
        s->data += 2;
        if ((size & 0xFF00) == 0xFF00) {
            if (s->data + 3 > s->end) return 0;
            s->data += 1;
            size = Be16(s->data);
            s->data += 2;
        }
        s->size = size;
        s->pad = size & 1;
    }
    const int n = (int)(std::min<ptrdiff_t>)((std::min)(s->size, bytes), s->end - s->data);
    if (n <= 0) return 0;
    memcpy(buffer, s->data, n);
    s->data += n;
    s->size -= n;
    return n;
}
int MspackWrite(mspack_file* file, void* buffer, int bytes)
{
    auto* s = reinterpret_cast<WriteStream*>(file);
    const size_t n = (std::min)(s->size, (size_t)bytes);
    memcpy(s->data, buffer, n);
    s->data += n;
    s->size -= n;
    return (int)n;
}
void* MspackAlloc(mspack_system*, size_t n) { return operator new(n); }
void MspackFree(void* p) { operator delete(p); }
void MspackCopy(void* src, void* dst, size_t n) { memcpy(dst, src, n); }
mspack_system g_sys = { nullptr, nullptr, MspackRead, MspackWrite, nullptr, nullptr, nullptr,
                        MspackAlloc, MspackFree, MspackCopy };

size_t DecodeChunk(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen)
{
    ReadStream rs{ src, src + srcLen, 0, 0 };
    WriteStream ws{ dst, dstLen };
    lzxd_stream* lzx = lzxd_init(&g_sys, reinterpret_cast<mspack_file*>(&rs), reinterpret_cast<mspack_file*>(&ws),
                                 17, 0, (int)(std::min<size_t>)(srcLen, 0x10000), (off_t)dstLen, 0);
    if (!lzx) return 0;
    lzxd_decompress(lzx, (off_t)dstLen);
    lzxd_free(lzx);
    return dstLen - ws.size;
}

constexpr uint32_t kChunkAlign = 0x20000;

} // namespace

std::string Sha1Hex(const void* data, size_t size)
{
    uint8_t digest[20] = {};
    BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, (PUCHAR)data, (ULONG)size, digest, sizeof digest);
    char hex[41];
    for (int i = 0; i < 20; ++i) sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
    return std::string(hex, 40);
}

std::string XexpTargetVersion(const std::vector<uint8_t>& d)
{
    if (d.size() < 0x18 || memcmp(d.data(), "XEX2", 4) != 0) return {};
    const uint32_t count = Be32(d.data() + 0x14);
    for (uint32_t i = 0; i < count && 0x18 + i * 8 + 8 <= d.size(); ++i) {
        const uint32_t key = Be32(d.data() + 0x18 + i * 8), value = Be32(d.data() + 0x1C + i * 8);
        if (key == 0x000005FF && value + 12 <= d.size()) {      // DELTA_PATCH_DESCRIPTOR
            const uint32_t v = Be32(d.data() + value + 4);
            char s[32];
            sprintf_s(s, "%u.%u.%u.%u", (v >> 28) & 0xF, (v >> 24) & 0xF, (v >> 8) & 0xFFFF, v & 0xFF);
            return s;
        }
    }
    return {};
}

bool BuildGameImage(const std::vector<uint8_t>& baseXex, const std::vector<uint8_t>& xexp,
                    std::vector<uint8_t>& image, std::string& err)
{
    std::vector<uint8_t> patched;
    const XexPatcher::Result r = XexPatcher::apply(baseXex.data(), baseXex.size(), xexp.data(), xexp.size(), patched, false);
    if (r != XexPatcher::Result::Success) {
        err = "The title update could not be applied to the game (error " + std::to_string(int(r)) + ").";
        return false;
    }
    // A decrypted, uncompressed XEX2: the image follows the header (header size at +0x08, image
    // size in the security info at [+0x10] + 4).
    if (patched.size() < 0x18) { err = "The patched game is invalid."; return false; }
    const uint32_t headerSize = Be32(patched.data() + 0x08), securityOffset = Be32(patched.data() + 0x10);
    if (securityOffset + 8 > patched.size()) { err = "The patched game is invalid."; return false; }
    const uint32_t imageSize = Be32(patched.data() + securityOffset + 4);
    if (uint64_t(headerSize) + imageSize > patched.size()) { err = "The patched game is truncated."; return false; }
    image.assign(patched.begin() + headerSize, patched.begin() + headerSize + imageSize);
    if (Sha1Hex(image.data(), image.size()) != kImageSha1) {
        err = "The patched game does not match the one this port was built for. "
              "Use the retail disc and Title Update 3 (version 0.0.3.2).";
        return false;
    }
    return true;
}

uint64_t RecordExpandedSize(const std::vector<uint8_t>& record)
{
    return ExpandedFromRecord(record.data(), record.size());
}

std::map<std::string, uint64_t> ArchiveSizes(const std::vector<uint8_t>& nrfo, const std::vector<std::string>& names)
{
    std::map<std::string, uint64_t> out;
    if (nrfo.size() < 4) return out;
    const uint32_t count = Be32(nrfo.data());
    for (uint32_t i = 0; i < count && i < names.size() && 4 + i * 8 + 8 <= nrfo.size(); ++i) {
        const uint32_t off = Be32(nrfo.data() + 4 + i * 8), size = Be32(nrfo.data() + 8 + i * 8);
        out[names[i]] = (size && uint64_t(off) + size <= nrfo.size()) ? ExpandedFromRecord(nrfo.data() + off, size) : 0;
    }
    return out;
}

bool IsLzxContainer(const uint8_t* p)
{
    const uint32_t sig = Be32(p);
    return sig == 0x0FF512EDu || sig == 0x0FF512EEu;
}

bool ExpandContainer(const SourceReader& read, uint64_t sourceSize, uint64_t expected,
                     const std::wstring& outPath, const ByteProgress& progress, std::string& err)
{
    // Header: magic, version, hash, flags (bit 22: u32 sizes, else packed 20-bit sizes), then
    // one UNCOMPRESSED size per 0x20000-aligned chunk of the container.
    const uint32_t chunks = uint32_t((sourceSize + kChunkAlign - 1) / kChunkAlign);
    std::vector<uint8_t> first((size_t)(std::min<uint64_t>)(sourceSize, kChunkAlign));
    if (sourceSize < 16 || !read(0, first.data(), (uint32_t)first.size())) { err = "an archive could not be read"; return false; }
    const uint32_t flags = Be32(first.data() + 12);
    std::vector<uint32_t> table(chunks);
    size_t headerEnd;
    if (flags & (1u << 22)) {
        headerEnd = 16 + 4 * (size_t)chunks;
        if (headerEnd >= first.size()) { err = "an archive header is invalid"; return false; }
        for (uint32_t i = 0; i < chunks; ++i) table[i] = Be32(first.data() + 16 + 4 * i);
    } else {
        headerEnd = (16 + (20 * (size_t)chunks + 7) / 8 + 3) & ~(size_t)3;
        if (headerEnd >= first.size()) { err = "an archive header is invalid"; return false; }
        for (uint32_t i = 0; i < chunks; ++i) {
            const size_t bit = (size_t)i * 20;
            const uint8_t* p = first.data() + 16 + bit / 8;
            const uint32_t w = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
            table[i] = (w >> (4 - bit % 8)) & 0xFFFFFu;
        }
    }
    uint64_t sum = 0;
    for (uint32_t v : table) sum += v;
    if (sum != expected) { err = "an archive does not have the expected size"; return false; }

    HANDLE out = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { err = "the install folder is not writable"; return false; }
    std::vector<uint8_t> in(kChunkAlign), chunk;
    uint64_t written = 0;
    bool ok = true;
    for (uint32_t k = 0; k < chunks && ok; ++k) {
        const uint64_t begin = (std::max<uint64_t>)(headerEnd, (uint64_t)k * kChunkAlign);
        const uint64_t end = (std::min<uint64_t>)((uint64_t)(k + 1) * kChunkAlign, sourceSize);
        if (begin >= end || !read(begin, in.data(), uint32_t(end - begin))) { err = "an archive could not be read"; ok = false; break; }
        const size_t length = size_t(end - begin);
        chunk.assign(table[k], 0);
        size_t got;
        if (table[k] + 2 == length && Be16(in.data()) == 0) {      // stored chunk
            memcpy(chunk.data(), in.data() + 2, table[k]);
            got = table[k];
        } else
            got = DecodeChunk(in.data(), length, chunk.data(), table[k]);
        if (got != table[k]) { err = "an archive could not be decompressed (the disc image may be damaged)"; ok = false; break; }
        if (k == 0 && (chunk.size() < 12 || memcmp(chunk.data() + 4, "ftypnrfi", 8) != 0)) {
            err = "an archive decompressed to unexpected data"; ok = false; break;
        }
        DWORD put = 0;
        if (!WriteFile(out, chunk.data(), (DWORD)chunk.size(), &put, nullptr) || put != chunk.size()) {
            err = "the disk is full or the install folder is not writable"; ok = false; break;
        }
        written += put;
        if (progress && !progress(written)) { err = "Installation cancelled."; ok = false; break; }
    }
    CloseHandle(out);
    if (!ok || written != expected) {
        DeleteFileW(outPath.c_str());
        if (ok) err = "an archive came out the wrong size";
        return false;
    }
    return true;
}

} // namespace ttt2::setup
