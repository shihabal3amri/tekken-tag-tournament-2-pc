// Namco title-storage emulation (on by default; TTT2_TITLE_SERVER=0 turns it off).
//
// TTT2 gates ten fighters behind a per-title file it downloads from Xbox Live title storage at
// the main menu (MainMenuMode::state_title_storage_init): "title_storage.bin", facility 2,
// fetched by the boot download task sub_826D9F60 through the statically linked XDK function
// XStorageDownloadToMemory (sub_82853C28). Block 1's level feeds sub_826DD7E8: level 1 unlocks
// Miharu, Sebastian and Slim Bob; 2 adds Ancient Ogre, Angel, Kunimitsu and Michelle (also
// sold as pre-order DLC keys); 3 adds Violet, Dr. Bosconovitch and Unknown. The unlock items
// are then written to the save (KsReleV100), so one successful download is enough. The file
// is protected by a CRC32 only (sub_826DA208 over 0xFFF0 bytes, checked by sub_826DC518); the
// layout is the one the parser sub_826DCAA8 accepts.
//
// With the option on (every session, so the roster is complete even if a save never stored
// the unlock), user 0 reports a Live sign-in (the task requires sign-in state 2) and
// this file answers XStorageDownloadToMemory: title_storage.bin gets the final state of the
// service (level 3) with Namco's own online services switched off (block 1 +0x0C GIN / Team and
// +0x14 World Arena = 0: the game shows its "service unavailable" notice and skips them - there
// is no Namco server any more; port/ONLINE_PLAN.md). The per-user items (licence.dat /
// penalty.dat, uploaded by the online menus) live in <save root>/xstorage/<XUID>/. No game data
// is involved - the image is built here. Paid DLC (costumes, music) is untouched. With the
// option off the original functions run unchanged.
//
// licence.dat is also what other players see: the game views a player's license (ranks, ranked
// record) by downloading their licence.dat by XUID (the path names its owner, kernel_xam.cpp's
// XStorageBuildServerPath). Another player's comes from the community server, which keeps each
// player's last upload (server/PROTOCOL.md "Player files"), else from that player's game while it
// is connected to this one (xsocket.cpp) - never this player's own file (MEASURED 2026-10-23, the
// user: the opponent's license showed "mine instead of the opponent", under their name).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "gin_service.h"
#include "kernel_shared.h"
#include "kernel_content.h"
#include "title_server.h"
#include "save_store.h"
#include "community_net.h"
#include "net_transport.h"
#include "xlive.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <string>

bool XamLocalUserXuid(uint64_t xuid);   // kernel_xam.cpp: one of this PC's signed-in users' XUIDs
// xsocket.cpp: the item of `owner` from the game of a player connected now: 1 found, 0 none has it
int XSocketAskPeersForFile(uint64_t owner, const std::string& item, std::vector<uint8_t>& data, int timeoutMs);

extern "C" PPC_FUNC(__imp__sub_82853C28);
extern "C" PPC_FUNC(__imp__sub_82853F08);
extern "C" PPC_FUNC(__imp__sub_828539C8);

namespace {
constexpr uint32_t ERROR_SUCCESS_ = 0, ERROR_INSUFFICIENT_BUFFER_ = 122, ERROR_FUNCTION_FAILED_ = 1627, ERROR_IO_PENDING_ = 997;
constexpr uint32_t XONLINE_E_STORAGE_FILE_NOT_FOUND = 0x8015C003u;
constexpr uint32_t SERVED_LEVEL = 3;

uint32_t Crc32NoFinalXor(const uint8_t* p, size_t n)        // sub_826DA208
{
    static uint32_t table[256] = {};
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            table[i] = c;
        }
    uint32_t crc = 0;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x)
{
    v[at] = uint8_t(x >> 24); v[at + 1] = uint8_t(x >> 16); v[at + 2] = uint8_t(x >> 8); v[at + 3] = uint8_t(x);
}

std::string ReadWideAscii(uint8_t* base, uint32_t address, uint32_t maxChars)
{
    std::string s;
    for (uint32_t i = 0; address && i < maxChars; ++i)
    {
        const uint16_t c = GRead16(base, address + i * 2);
        if (!c) break;
        s += c < 0x80 ? char(c) : '?';
    }
    return s;
}

// <save root>/xstorage/<XUID>/<item> for a per-user storage path (the item is the last path part).
// Not inside <save root>/<XUID>/: every folder there is a save container (save_store.cpp).
bool UserStorageFile(const std::string& serverPath, std::filesystem::path& file)
{
    std::filesystem::path root;
    if (SaveUserRoot(root)) return false;
    const std::string item = serverPath.substr(serverPath.find_last_of('/') + 1);
    if (item.empty() || item.size() > 64 || item.find_first_of("\\:*?\"<>|") != std::string::npos || item[0] == '.')
        return false;
    file = root.parent_path() / L"xstorage" / root.filename() / std::filesystem::path(item);
    return true;
}

// The owner a per-user path names (".../3/<XUID>/<item>"), 0 when it names none.
uint64_t PathOwner(const std::string& path)
{
    const size_t end = path.find_last_of('/');
    if (end == std::string::npos || end < 17 || path[end - 17] != '/') return 0;
    uint64_t xuid = 0;
    for (size_t i = end - 16; i < end; ++i)
    {
        const char c = path[i];
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return 0;
        xuid = xuid << 4 | uint64_t(v);
    }
    return xuid;
}

std::string ItemOf(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }

// The items other players may read: the license (penalty.dat stays the player's own).
bool SharedItem(const std::string& item) { return item == "licence.dat"; }

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Another player's item: the community server's copy, else their game's (a player connected now). Kept a while:
// a room's license screen asks for the same players again and again. 1 found, 0 not found.
int RemoteUserItem(uint64_t owner, const std::string& item, std::vector<uint8_t>& data)
{
    struct Known { int found; std::vector<uint8_t> data; ULONGLONG at; };
    static std::mutex m;
    static std::map<std::pair<uint64_t, std::string>, Known> known;
    constexpr ULONGLONG kKeepMs = 20 * 1000, kKeepNoneMs = 5 * 1000;
    const auto key = std::make_pair(owner, item);
    {
        std::lock_guard l(m);
        const auto it = known.find(key);
        if (it != known.end() && GetTickCount64() - it->second.at < (it->second.found ? kKeepMs : kKeepNoneMs))
        {
            data = it->second.data;
            return it->second.found;
        }
    }
    const char* from = "nobody";
    int found = 0;
    if (const std::shared_ptr<community::Client> server = net::Community(); server && server->KeepsFiles())
    {
        found = server->GetFile(owner, item, data, 3000) == 1;
        if (found) from = "the server";
    }
    if (!found)
    {
        found = XSocketAskPeersForFile(owner, item, data, 2000) == 1;
        if (found) from = "the player's game";
    }
    if (!found) data.clear();
    std::fprintf(stderr, "[title-server] %016llX's %s: %s (%zu bytes)\n", (unsigned long long)owner, item.c_str(),
                 found ? from : "nobody has it", data.size());
    std::lock_guard l(m);
    if (known.size() > 256) known.clear();
    known[key] = { found, data, GetTickCount64() };
    return found;
}

// Copies a downloaded item into the game's buffer: the XStorageDownloadToMemory result and its extended error.
uint32_t Deliver(uint8_t* base, const std::vector<uint8_t>& data, uint32_t buffer, uint32_t cbBuffer, uint32_t& bytes,
                 uint32_t& extended)
{
    bytes = 0;
    extended = 0;
    if (data.empty()) { extended = XONLINE_E_STORAGE_FILE_NOT_FOUND; return ERROR_FUNCTION_FAILED_; }
    if (!buffer || data.size() > cbBuffer || !XamGuestRange(buffer, uint32_t(data.size())))
    {
        extended = 0x8007007Au;
        return ERROR_INSUFFICIENT_BUFFER_;
    }
    std::memcpy(base + buffer, data.data(), data.size());
    bytes = uint32_t(data.size());
    return ERROR_SUCCESS_;
}

// The results block {DWORD dwBytesTotal; XUID xuidOwner; FILETIME ftCreated}.
void WriteResults(uint8_t* base, uint32_t results, uint32_t cbResults, uint32_t bytes, uint64_t owner)
{
    if (!results || cbResults < 20 || !XamGuestRange(results, 20)) return;
    std::memset(base + results, 0, 20);
    GWrite32(base, results, bytes);
    GWrite32(base, results + 4, uint32_t(owner >> 32));
    GWrite32(base, results + 8, uint32_t(owner));
}
}

bool TitleServerOwnItem(uint64_t owner, const std::string& item, std::vector<uint8_t>& data)
{
    std::filesystem::path file;
    if (!SharedItem(item) || !owner || !XamLocalUserXuid(owner) || !UserStorageFile("/" + item, file)) return false;
    data = ReadFileBytes(file);
    return !data.empty() && data.size() <= community::kMaxPlayerFile;
}

void TitleServerShareOwnFiles(const std::shared_ptr<community::Client>& server)
{
    const uint64_t own = xlive::OnlineXuid(0);
    std::vector<uint8_t> data;
    if (!server || !server->KeepsFiles() || !own || !TitleServerOwnItem(own, "licence.dat", data)) return;
    server->PutFile(own, "licence.dat", data);
    std::fprintf(stderr, "[title-server] licence.dat (%zu bytes) given to the server\n", data.size());
}

bool TitleServerEnabled()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_TITLE_SERVER"); return !(e && std::strcmp(e, "0") == 0); }();
    return on;
}

std::vector<uint8_t> TitleStorageImage(uint32_t level, uint32_t bufferBytes)
{
    // Payload (sub_826DCAA8): 12-byte header (word 0 = 3), then four blocks, each behind a
    // 16-byte {type, count, size, 0} header. Bytes consumed must equal the size field.
    constexpr size_t PAYLOAD = 0x0C + (0x10 + 0x80) + (0x10 + 0x30) + 0x10 + 0x10;
    constexpr size_t HEADER = 0x14;                          // crc, u16, s16, size, 2 unchecked words
    std::vector<uint8_t> f((std::max<size_t>)(HEADER + PAYLOAD, bufferBytes), 0);
    Put32(f, 4, 0x00010000u);                                // +4 u16 = 1, +6 s16 = 0 (unchecked)
    Put32(f, 8, uint32_t(PAYLOAD));
    size_t o = HEADER;
    Put32(f, o, 3); o += 0x0C;
    // Block 1: +0 / +8 extra feature flags (left off), +4 level, online switches: +0xC GIN / Team
    // and +0x14 World Arena off (no Namco server; the game says so and skips them), +0x10 at the
    // game's default (1).
    Put32(f, o, 1); Put32(f, o + 4, 1); Put32(f, o + 8, 0x80); o += 0x10;
    // GIN / Team on with the community servers (gin_service.h: a server answers, or the port itself).
    Put32(f, o + 4, level); Put32(f, o + 0xC, gin_service::Enabled() ? 1 : 0); Put32(f, o + 0x10, 1); Put32(f, o + 0x14, 0); o += 0x80;
    // Block 2: the defaults sub_82698300 installs before parsing.
    Put32(f, o, 2); Put32(f, o + 4, 1); Put32(f, o + 8, 0x30); o += 0x10;
    const uint32_t defaults[9] = { 0, 1, 5, 1, 6, 4, 1, 1, 1 };
    for (uint32_t i = 0; i < 9; ++i) Put32(f, o + i * 4, defaults[i]);
    o += 0x30;
    Put32(f, o, 3); Put32(f, o + 8, 0x1D0); o += 0x10;      // block 3: no records
    Put32(f, o, 4); Put32(f, o + 8, 8); o += 0x10;          // block 4: no entries
    // CRC over the 0xFFF0 bytes after the CRC word of the zero-padded 0xFFF4 buffer.
    std::vector<uint8_t> padded(f);
    padded.resize((std::max<size_t>)(padded.size(), 0xFFF4), 0);
    Put32(f, 0, Crc32NoFinalXor(padded.data() + 4, 0xFFF0));
    return f;
}

// XStorageDownloadToMemory(user, wszServerPath, cbBuffer, pbBuffer, cbResults, pResults,
// pXOverlapped) - XDK ABI; results = {DWORD dwBytesTotal; XUID xuidOwner; FILETIME ftCreated}.
PPC_FUNC(sub_82853C28)
{
    if (!TitleServerEnabled()) { __imp__sub_82853C28(ctx, base); return; }
    const uint32_t user = ctx.r3.u32, path = ctx.r4.u32, cbBuffer = ctx.r5.u32, buffer = ctx.r6.u32;
    const uint32_t cbResults = ctx.r7.u32, results = ctx.r8.u32, overlapped = ctx.r9.u32;
    const std::string name = ReadWideAscii(base, path, 256);
    const std::string item = ItemOf(name);
    const uint64_t owner = PathOwner(name);
    uint32_t result = ERROR_SUCCESS_, extended = 0, bytes = 0;
    if (owner && !XamLocalUserXuid(owner))
    {
        // Another player's: asked of the server or their game, which takes a moment - an overlapped request completes
        // later from a worker, as XAM's own would.
        if (!SharedItem(item))
        {
            result = ERROR_FUNCTION_FAILED_;
            extended = XONLINE_E_STORAGE_FILE_NOT_FOUND;
        }
        else if (overlapped && !XamValidateOverlapped(base, overlapped))
        {
            GWrite32(base, overlapped + 4, 0);
            GWrite32(base, overlapped + 24, 0);
            std::atomic_thread_fence(std::memory_order_release);
            GWrite32(base, overlapped, ERROR_IO_PENDING_);
            std::thread([=] {
                std::vector<uint8_t> data;
                RemoteUserItem(owner, item, data);
                uint32_t got = 0, ext = 0;
                const uint32_t r = Deliver(base, data, buffer, cbBuffer, got, ext);
                WriteResults(base, results, cbResults, got, owner);
                std::fprintf(stderr, "[title-server] user %u download '%s' -> %s (%u bytes)\n", user, name.c_str(),
                             r ? "not served" : "served", got);
                XamCompleteOverlapped(base, overlapped, r, 0, ext);
            }).detach();
            ctx.r3.u32 = ERROR_IO_PENDING_;
            return;
        }
        else
        {
            std::vector<uint8_t> data;
            RemoteUserItem(owner, item, data);
            result = Deliver(base, data, buffer, cbBuffer, bytes, extended);
        }
    }
    else if (item == "title_storage.bin")
    {
        std::vector<uint8_t> image = TitleStorageImage(SERVED_LEVEL, cbBuffer);
        if (!buffer || image.size() > cbBuffer || !XamGuestRange(buffer, image.size()))
        {
            result = ERROR_INSUFFICIENT_BUFFER_;
            extended = 0x8007007Au;
        }
        else
        {
            std::memcpy(base + buffer, image.data(), image.size());
            bytes = uint32_t(image.size());
        }
    }
    else
    {
        // One of this PC's players' items: the copy the game uploaded before, else "not found".
        std::filesystem::path file;
        std::vector<uint8_t> data;
        if (UserStorageFile(name, file)) data = ReadFileBytes(file);
        result = Deliver(base, data, buffer, cbBuffer, bytes, extended);
    }
    WriteResults(base, results, cbResults, bytes, owner);
    std::fprintf(stderr, "[title-server] user %u download '%s' -> %s (%u bytes, level %u)\n", user, name.c_str(),
                 result ? "not served" : "served", bytes, result ? 0u : SERVED_LEVEL);
    // Overlapped requests complete inline and still report pending, like real XAM.
    ctx.r3.u32 = overlapped ? XamCompleteOverlapped(base, overlapped, result, 0, extended) : result;
}

// XStorageUploadFromMemory(user, wszServerPath, cbBuffer, pbBuffer, pXOverlapped): per-user items
// (licence.dat / penalty.dat) are kept in the profile's save folder.
PPC_FUNC(sub_82853F08)
{
    if (!TitleServerEnabled()) { __imp__sub_82853F08(ctx, base); return; }
    const uint32_t user = ctx.r3.u32, path = ctx.r4.u32, size = ctx.r5.u32, buffer = ctx.r6.u32;
    const uint32_t overlapped = ctx.r7.u32;
    const std::string name = ReadWideAscii(base, path, 256);
    const uint64_t owner = PathOwner(name);
    uint32_t result = ERROR_FUNCTION_FAILED_, extended = 0x80004005u;
    std::filesystem::path file;
    if ((!owner || XamLocalUserXuid(owner)) && UserStorageFile(name, file) && (size == 0 || XamGuestRange(buffer, size, 1)) &&
        size <= (16u << 20))
    {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        const std::filesystem::path temp = file.wstring() + L".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (out && size) out.write(reinterpret_cast<const char*>(base + buffer), size);
        }
        std::filesystem::rename(temp, file, ec);
        if (!ec) { result = ERROR_SUCCESS_; extended = 0; }
        // the license other players view: the community server keeps the last one
        if (!ec && user == 0 && SharedItem(ItemOf(name)))
            if (const std::shared_ptr<community::Client> server = net::Community(); server && server->KeepsFiles() && size &&
                size <= community::kMaxPlayerFile)
                server->PutFile(xlive::OnlineXuid(0), ItemOf(name), std::vector<uint8_t>(base + buffer, base + buffer + size));
    }
    std::fprintf(stderr, "[title-server] user %u upload '%s' (%u bytes) -> %s\n", user, name.c_str(), size,
                 result ? "failed" : "stored");
    ctx.r3.u32 = overlapped ? XamCompleteOverlapped(base, overlapped, result, 0, extended) : result;
}

// XStorageDelete(user, wszServerPath, pXOverlapped)
PPC_FUNC(sub_828539C8)
{
    if (!TitleServerEnabled()) { __imp__sub_828539C8(ctx, base); return; }
    const uint32_t user = ctx.r3.u32, path = ctx.r4.u32, overlapped = ctx.r5.u32;
    const std::string name = ReadWideAscii(base, path, 256);
    std::filesystem::path file;
    std::error_code ec;
    const uint64_t owner = PathOwner(name);
    const bool removed = (!owner || XamLocalUserXuid(owner)) && UserStorageFile(name, file) && std::filesystem::remove(file, ec);
    const uint32_t result = removed ? ERROR_SUCCESS_ : ERROR_FUNCTION_FAILED_;
    std::fprintf(stderr, "[title-server] user %u delete '%s' -> %s\n", user, name.c_str(), removed ? "deleted" : "not found");
    ctx.r3.u32 = overlapped ? XamCompleteOverlapped(base, overlapped, result, 0, removed ? 0 : XONLINE_E_STORAGE_FILE_NOT_FOUND)
                            : result;
}
