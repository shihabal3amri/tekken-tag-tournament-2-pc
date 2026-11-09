// Downloadable content (see dlc_content.h). TTT2 enumerates content type 2 at boot (sub_8258ED98, up to 53 records,
// per user index), checks each package by mounting it as DLCDATTMP, reading DLCDATTMP:\index.dat (encrypted, ~50
// bytes: which dataNNN.dat archives the package adds) and closing it (sub_8258F3B0), then mounts the ones it uses.
#include "dlc_content.h"
#include "log_utf8.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>

const char* GameFilesRoot();   // kernel_io.cpp: the extracted disc's folder (<install>\game)

namespace dlc {
namespace {
constexpr char kMagic[8] = { 'T', 'T', 'T', '2', 'D', 'L', 'C', '1' };
constexpr size_t kSidecarBytes = sizeof(kMagic) + std::tuple_size_v<SaveContentRecord>;
std::mutex g_mutex;
std::unordered_map<std::string, std::filesystem::path> g_mounts;   // lower-case root -> package folder

std::string Lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32; return s; }
uint32_t Be32(const SaveContentRecord& r, size_t at) { return uint32_t(r[at]) << 24 | uint32_t(r[at + 1]) << 16 | uint32_t(r[at + 2]) << 8 | r[at + 3]; }
std::string FileName(const SaveContentRecord& r)
{
    size_t n = 0;
    while (n < 42 && r[264 + n]) ++n;
    std::string name(reinterpret_cast<const char*>(r.data() + 264), n);
    for (char c : name)
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return {};
    return name;
}
std::filesystem::path Folder() { return std::filesystem::path(GameFilesRoot()) / "dlc"; }

// The packages the installer put there: <name>.ttt2-dlc = "TTT2DLC1" + the 308-byte record (device 1, type 2, display
// name, file name = <name>), next to the folder <name>.
std::vector<std::pair<std::string, SaveContentRecord>> Installed()
{
    std::vector<std::pair<std::string, SaveContentRecord>> out;
    std::error_code ec;
    const std::filesystem::path dir = Folder();
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        const std::filesystem::path& p = it->path();
        // Names are compared wide: string() throws at a name outside the ANSI code page (log_utf8.h), such as a copy
        // the player made (Explorer names it "<name> - Copy" in the display language).
        if (_wcsicmp(p.extension().c_str(), L".ttt2-dlc") != 0 || !it->is_regular_file(ec)) continue;
        uint8_t data[kSidecarBytes] = {};
        FILE* f = _wfopen(p.c_str(), L"rb");
        if (!f) continue;
        const size_t got = std::fread(data, 1, sizeof(data), f);
        const bool whole = got == sizeof(data) && std::fgetc(f) == EOF;
        std::fclose(f);
        if (!whole || std::memcmp(data, kMagic, sizeof(kMagic))) continue;
        SaveContentRecord r{};
        std::memcpy(r.data(), data + sizeof(kMagic), r.size());
        const std::string name = FileName(r);
        if (Be32(r, 4) != kContentType || name.empty() || _wcsicmp(p.stem().c_str(), std::wstring(name.begin(), name.end()).c_str()) ||
            !std::filesystem::is_directory(dir / name, ec))
            continue;
        out.emplace_back(name, r);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return Lower(a.first) < Lower(b.first); });
    return out;
}
}

std::vector<SaveContentRecord> List()
{
    std::vector<SaveContentRecord> records;
    for (auto& [name, r] : Installed()) records.push_back(r);
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        std::fprintf(stderr, "[dlc] %zu package(s) installed in %s\n", records.size(), LogUtf8(Folder()).c_str());
        for (const auto& r : records)
        {
            std::wstring display;   // the package's display name, UTF-16 big-endian in the record
            for (size_t i = 0; i < 128; ++i)
            {
                const wchar_t c = wchar_t(r[8 + 2 * i] << 8 | r[9 + 2 * i]);
                if (!c) break;
                display += c;
            }
            std::fprintf(stderr, "[dlc]   %s  %s\n", FileName(r).c_str(), LogUtf8(display).c_str());
        }
    }
    return records;
}

uint32_t Open(const std::string& root, const SaveContentRecord& record, uint32_t mode, uint32_t& disposition)
{
    disposition = 0;
    if (mode != 3 && mode != 4) return 5;                     // ERROR_ACCESS_DENIED: the content is read-only
    const std::string name = FileName(record);
    std::filesystem::path folder;
    for (auto& [installed, r] : Installed())
        if (!name.empty() && Lower(installed) == Lower(name)) folder = Folder() / installed;
    if (folder.empty()) return 3;                             // ERROR_PATH_NOT_FOUND
    std::lock_guard lock(g_mutex);
    auto [it, added] = g_mounts.emplace(Lower(root), folder);
    // The same package mounted again on its root (a rewind re-running the frame that mounted it): already open.
    if (!added && it->second != folder) return 183;            // ERROR_ALREADY_EXISTS
    disposition = 2;
    return 0;
}

bool Close(const std::string& root, bool close)
{
    std::lock_guard lock(g_mutex);
    const auto it = g_mounts.find(Lower(root));
    if (it == g_mounts.end()) return false;
    if (close) g_mounts.erase(it);
    return true;
}

bool Resolve(const std::string& guest, std::string& host)
{
    std::string p = guest;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.rfind("/??/", 0) == 0) p.erase(0, 4);
    const size_t colon = p.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    std::filesystem::path folder;
    {
        std::lock_guard lock(g_mutex);
        if (g_mounts.empty()) return false;
        const auto it = g_mounts.find(Lower(p.substr(0, colon)));
        if (it == g_mounts.end()) return false;
        folder = it->second;
    }
    std::string tail = p.substr(colon + 1);
    while (!tail.empty() && tail[0] == '/') tail.erase(0, 1);
    // Only names inside the package: no parent references, drives or streams.
    if (tail.find(':') != std::string::npos || tail == ".." || tail.rfind("../", 0) == 0 ||
        tail.find("/../") != std::string::npos || (tail.size() >= 3 && tail.compare(tail.size() - 3, 3, "/..") == 0))
        tail.clear();
    host = (tail.empty() ? folder : folder / tail).string();
    return true;
}
}
