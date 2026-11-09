// Downloadable content packages. See dlc_install.h.
#include "setup/dlc_install.h"
#include "setup/game_data.h"
#include "setup/stfs.h"

#include <windows.h>
#include <algorithm>
#include <cstring>

namespace ttt2::setup {

namespace {

constexpr uint32_t kDlcContentType = 2;
constexpr int kMaxDepth = 6;
constexpr size_t kMaxVisited = 200000;   // a whole drive chosen by mistake: stop looking instead of hanging

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::wstring Widen(const std::string& s) {   // package names are ASCII; any other byte stays one character
    std::wstring w;
    for (unsigned char c : s) w += wchar_t(c);
    return w;
}

bool SafeName(const std::string& n) {
    if (n.empty() || n.size() > 42) return false;
    for (char c : n)
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    return true;
}

// The record's file name: the package's own (Xbox content files are named by their content id), else the content id.
std::string NameOf(const std::wstring& path, const uint8_t* hdr) {
    const size_t slash = path.find_last_of(L"\\/");
    std::string n;
    for (wchar_t c : path.substr(slash == std::wstring::npos ? 0 : slash + 1)) {
        if (c > 0x7F) { n.clear(); break; }
        n += char(c);
    }
    if (SafeName(n)) return n;
    static const char hex[] = "0123456789ABCDEF";
    n.clear();
    for (int i = 0; i < 20; ++i) { n += hex[hdr[0x32C + i] >> 4]; n += hex[hdr[0x32C + i] & 15]; }
    return n;
}

std::wstring DisplayOf(const uint8_t* hdr) {
    std::wstring s;
    for (int i = 0; i < 128; ++i) {
        const wchar_t c = wchar_t(hdr[0x411 + 2 * i] << 8 | hdr[0x412 + 2 * i]);
        if (!c) break;
        s += c;
    }
    return s;
}

bool ReadHeader(const std::wstring& path, uint8_t (&hdr)[0x1000]) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    const bool ok = ReadFile(h, hdr, sizeof hdr, &got, nullptr) && got == sizeof hdr;
    CloseHandle(h);
    return ok;
}

void Walk(const std::wstring& dir, int depth, std::vector<std::wstring>& files, size_t& visited) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || ++visited > kMaxVisited) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // no junction loops
        const std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < kMaxDepth) Walk(p, depth + 1, files, visited);
        } else if ((uint64_t(fd.nFileSizeHigh) << 32 | fd.nFileSizeLow) > 0xC000) {
            files.push_back(p);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::string Lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32; return s; }

// A package folder and everything in it (only ever <game>\dlc\<name> or <name>.partial).
void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            const std::wstring p = dir + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) RemoveTree(p);
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); if (!DeleteFileW(p.c_str())) RemoveDirectoryW(p.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

} // namespace

std::vector<DlcPackage> FindDlc(const std::wstring& root, int* damaged) {
    if (damaged) *damaged = 0;
    std::vector<std::wstring> files;
    const DWORD attr = GetFileAttributesW(root.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return {};
    size_t visited = 0;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) Walk(root, 0, files, visited);
    else files.push_back(root);

    std::vector<DlcPackage> out;
    for (const std::wstring& f : files) {
        uint8_t hdr[0x1000];
        if (!ReadHeader(f, hdr) || (memcmp(hdr, "LIVE", 4) && memcmp(hdr, "PIRS", 4) && memcmp(hdr, "CON ", 4)) ||
            Be32(hdr + 0x360) != kTitleId || Be32(hdr + 0x344) != kDlcContentType)
            continue;
        DlcPackage p{ f, NameOf(f, hdr), DisplayOf(hdr), 0 };
        StfsPackage pkg;
        std::string err;
        bool ok = pkg.Open(f.c_str(), err);
        if (ok) {
            const auto list = pkg.Files();
            ok = !list.empty();
            for (const auto& file : list) {
                if (file.path.empty()) ok = false;   // an unsafe or broken name
                p.bytes += file.size;
            }
        }
        if (!ok) { if (damaged) ++*damaged; continue; }
        const bool dup = std::any_of(out.begin(), out.end(), [&](const DlcPackage& o) { return Lower(o.name) == Lower(p.name); });
        if (!dup) out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const DlcPackage& a, const DlcPackage& b) { return _wcsicmp(a.display.c_str(), b.display.c_str()) < 0; });
    return out;
}

bool InstallDlc(const DlcPackage& p, const std::wstring& gameDir, const std::function<bool(uint64_t)>& progress,
                std::vector<std::wstring>& written, std::string& err) {
    StfsPackage pkg;
    if (!pkg.Open(p.path.c_str(), err)) { err = "the package could not be read"; return false; }
    // Extracted into <name>.partial first: a package installed before stays as it was until this one is complete
    // and checked, then the folders swap.
    const std::wstring root = gameDir + L"\\dlc", dir = root + L"\\" + Widen(p.name), sidecar = dir + L".ttt2-dlc",
                       temp = dir + L".partial";
    CreateDirectoryW(root.c_str(), nullptr);
    RemoveTree(temp);   // left by an interrupted install
    if (!CreateDirectoryW(temp.c_str(), nullptr)) { err = "could not write to the install folder"; return false; }
    auto fail = [&] { RemoveTree(temp); return false; };

    std::vector<std::wstring> mine = { dir };   // the final paths, for the install's undo
    uint64_t done = 0;
    for (const auto& f : pkg.Files()) {
        if (f.path.empty()) { err = "the package is damaged (a file name it lists is not valid)"; return fail(); }
        const std::wstring rel = Widen(f.path);
        for (size_t at = rel.find(L'\\'); at != std::wstring::npos; at = rel.find(L'\\', at + 1)) {
            if (CreateDirectoryW((temp + L"\\" + rel.substr(0, at)).c_str(), nullptr)) mine.push_back(dir + L"\\" + rel.substr(0, at));
        }
        HANDLE h = CreateFileW((temp + L"\\" + rel).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { err = "could not write to the install folder"; return fail(); }
        mine.push_back(dir + L"\\" + rel);
        bool cancelled = false;
        const bool ok = pkg.ExtractFile(f.path, [&](const uint8_t* data, uint32_t n) {
            DWORD put = 0;
            if (!WriteFile(h, data, n, &put, nullptr) || put != n) return false;
            done += n;
            if (!progress(done)) { cancelled = true; return false; }
            return true;
        }, err);
        CloseHandle(h);
        if (!ok) { if (cancelled) err = "Installation cancelled."; return fail(); }
    }

    // Unlisted first (the .ttt2-dlc file), so the game never lists a package while its folder is being replaced.
    SetFileAttributesW(sidecar.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(sidecar.c_str());
    RemoveTree(dir);
    if (!MoveFileExW(temp.c_str(), dir.c_str(), 0)) { err = "could not write to the install folder"; return fail(); }
    auto undo = [&] { RemoveTree(dir); return false; };

    uint8_t rec[8 + 308] = { 'T', 'T', 'T', '2', 'D', 'L', 'C', '1' };
    rec[8 + 3] = 1;                                                  // device 1 (the hard drive)
    rec[8 + 7] = uint8_t(kDlcContentType);
    memcpy(rec + 8 + 8, pkg.Header() + 0x411, 256);                  // display name, UTF-16BE
    memcpy(rec + 8 + 264, p.name.data(), p.name.size());             // file name (<= 42)
    HANDLE h = CreateFileW(sidecar.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD put = 0;
    const bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, rec, sizeof rec, &put, nullptr) && put == sizeof rec;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) { DeleteFileW(sidecar.c_str()); err = "could not write to the install folder"; return undo(); }
    mine.push_back(sidecar);
    written.insert(written.end(), mine.begin(), mine.end());
    return true;
}

} // namespace ttt2::setup
