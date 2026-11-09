// Unpack the embedded runtime. See payload.h.
#include "setup/payload.h"
#include "setup/payload_ids.h"

#include <windows.h>
#include <bcrypt.h>

namespace ttt2::setup {

namespace {

struct Item { int id; const wchar_t* name; };

// The VC++ runtime is not part of Windows: app-local copies of exactly the four DLLs the game
// imports (llvm-readobj --coff-imports) spare the player a redist install.
const Item kItems[] = {
    { IDR_GAME_EXE,        L"Tekken Tag Tournament 2.exe" },
    { IDR_STEAM_API,       L"steam_api64.dll" },
    { IDR_MSVCP140,        L"msvcp140.dll" },
    { IDR_MSVCP140_ATOMIC, L"msvcp140_atomic_wait.dll" },
    { IDR_VCRUNTIME140,    L"vcruntime140.dll" },
    { IDR_VCRUNTIME140_1,  L"vcruntime140_1.dll" },
    { IDR_README,          L"README.txt" },
};

bool FindRes(int id, const void*& data, DWORD& size)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return false;
    data = LockResource(g);
    size = SizeofResource(nullptr, r);
    return data && size;
}

bool WriteFileBytes(const std::wstring& path, const void* data, size_t size, std::string& err)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "Could not write to the install folder. If the game is running, close it; otherwise "
              "choose a different location.";
        return false;
    }
    const uint8_t* p = (const uint8_t*)data;
    size_t done = 0;
    while (done < size) {
        DWORD put = 0;
        const DWORD want = (DWORD)((size - done) > (1u << 20) ? (1u << 20) : (size - done));
        if (!WriteFile(h, p + done, want, &put, nullptr) || put != want) {
            CloseHandle(h);
            err = "Could not write to the install folder (the disk may be full).";
            return false;
        }
        done += put;
    }
    FlushFileBuffers(h);   // on the disk now: WritePayload reads it back from there
    CloseHandle(h);
    return true;
}

std::string Narrow(const wchar_t* w) { std::string s; for (; *w; ++w) s += char(*w); return s; }   // our names are ASCII

std::string Hex(const uint8_t* p, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += digits[p[i] >> 4]; s += digits[p[i] & 15]; }
    return s;
}

// The build's checksum of resource `id` (IDR_PAYLOAD_SHA256: "<id> <sha256>" lines); empty when it has none.
std::string ExpectedSha256(int id)
{
    const void* d = nullptr; DWORD n = 0;
    if (!FindRes(IDR_PAYLOAD_SHA256, d, n)) return {};
    const std::string text((const char*)d, n), key = std::to_string(id) + " ";
    for (size_t at = 0; at < text.size();) {
        size_t eol = text.find('\n', at);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(at, eol - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, key.size(), key) == 0 && line.size() == key.size() + 64) return line.substr(key.size());
        at = eol + 1;
    }
    return {};
}

std::string Sha256Hex(const void* data, DWORD size)
{
    uint8_t digest[32] = {};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data, size, digest, sizeof digest) != 0) return {};
    return Hex(digest, sizeof digest);
}

// A file's SHA-256; fromDisk: read past the system's file cache (FILE_FLAG_NO_BUFFERING), so a file that changed on
// its way to the disk shows. Empty when it cannot be read.
std::string FileSha256Hex(const std::wstring& path, bool fromDisk)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN | (fromDisk ? FILE_FLAG_NO_BUFFERING : 0), nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    const DWORD chunk = 1u << 20;
    uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   // page aligned
    bool ok = buf && BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    for (DWORD got = 0; ok;) {
        ok = ReadFile(h, buf, chunk, &got, nullptr) != 0;
        if (!ok || !got) break;
        ok = BCryptHashData(hash, buf, got, 0) == 0;
    }
    uint8_t digest[32] = {};
    ok = ok && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    CloseHandle(h);
    return ok ? Hex(digest, sizeof digest) : std::string();
}

} // namespace

const wchar_t kGameExeName[] = L"Tekken Tag Tournament 2.exe";

uint64_t PayloadBytes()
{
    uint64_t t = 0;
    for (const Item& it : kItems) {
        const void* d = nullptr; DWORD n = 0;
        if (FindRes(it.id, d, n)) t += n;
    }
    return t;
}

bool WritePayload(const wchar_t* destDir, std::string& err, std::vector<std::wstring>* written)
{
    for (const Item& it : kItems) {
        const void* d = nullptr; DWORD n = 0;
        if (!FindRes(it.id, d, n)) {
            err = "The installer is damaged (a required file is missing from it). Please download it again.";
            return false;
        }
        // MEASURED 2026-10-31 (TEST 11, tester log 42): a game exe with a run of zeros where code should be crashed at
        // start; the installed file did not match the build. So: the carried copy is checked against the build's
        // checksum before it is written, and the written file again as it is on the disk.
        const std::string want = ExpectedSha256(it.id);
        if (want.empty() || Sha256Hex(d, n) != want) {
            err = "The installer is damaged (" + Narrow(it.name) + " inside it does not match its checksum). Please "
                  "download the installer again.";
            return false;
        }
        const std::wstring path = std::wstring(destDir) + L"\\" + it.name;
        if (written) written->push_back(path);
        if (!WriteFileBytes(path, d, n, err)) return false;
        if (FileSha256Hex(path, true) != want) {
            err = Narrow(it.name) + " was changed on its way to the disk (by the disk itself, or by a security program). "
                  "Check the drive, or allow the install folder in your antivirus, and install again.";
            return false;
        }
    }
    return true;
}

bool InstalledExeMatches(const wchar_t* destDir)
{
    const std::string want = ExpectedSha256(IDR_GAME_EXE);
    return !want.empty() && FileSha256Hex(std::wstring(destDir) + L"\\" + kGameExeName, false) == want;
}

} // namespace ttt2::setup
