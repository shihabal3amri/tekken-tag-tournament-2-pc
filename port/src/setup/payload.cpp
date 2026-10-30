// Unpack the embedded runtime. See payload.h.
#include "setup/payload.h"
#include "setup/payload_ids.h"

#include <windows.h>

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
    CloseHandle(h);
    return true;
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
        const std::wstring path = std::wstring(destDir) + L"\\" + it.name;
        if (written) written->push_back(path);
        if (!WriteFileBytes(path, d, n, err)) return false;
    }
    return true;
}

} // namespace ttt2::setup
