// A local, data-only container store. Never maps save paths into extracted disc
// files. Metadata is versioned and deterministic; all guest path components are
// constrained and existing reparse points are rejected before file operations.
#include "save_store.h"
#include "profiles.h"
#include <cwchar>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifndef TTT2_LOCAL_SAVES
#define TTT2_LOCAL_SAVES 0
#endif

namespace {
// The signed-in profile owns the saves (profiles.cpp); its XUID names the save folder.
uint64_t LocalXuid() { return profiles::ActiveXuid(); }
constexpr wchar_t META[] = L".ttt2-content-v1";
std::mutex g_saveMutex;
std::unordered_map<std::string, std::filesystem::path> g_saveMounts;
std::string Lower(std::string s) { for(auto& c:s) if(c>='A'&&c<='Z')c+=32; return s; }
bool Component(const std::string& s)
{
    if (s.empty() || s.size()>42 || s.front()=='.' || s.back()=='.') return false;
    for(unsigned char c:s) if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
        (c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')) return false;
    const auto n=Lower(s.substr(0,s.find('.')));
    if(n=="con"||n=="prn"||n=="aux"||n=="nul")return false;
    if(n.size()==4&&(n.rfind("com",0)==0||n.rfind("lpt",0)==0)&&n[3]>='1'&&n[3]<='9')return false;
    return true;
}
uint32_t NoReparse(const std::filesystem::path& path)
{
    std::filesystem::path part;
    for(const auto& component:path) {
        part/=component;
        if(part==path.root_name())continue; // C: alone is drive-relative.
        DWORD a=GetFileAttributesW(part.c_str());
        if(a==INVALID_FILE_ATTRIBUTES) {
            const auto e=GetLastError();
            if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)continue;
            return e;
        }
        if(a&FILE_ATTRIBUTE_REPARSE_POINT)return ERROR_ACCESS_DENIED;
    }
    return 0;
}
uint32_t Root(std::filesystem::path& root)
{
    // An override is useful for repeatable tests and portable installations.
    const wchar_t* overridePath=_wgetenv(L"TTT2_SAVE_ROOT");
    const wchar_t* local=_wgetenv(L"LOCALAPPDATA");
    if(overridePath&&*overridePath)root=overridePath;
    else if(local&&*local)root=std::filesystem::path(local)/L"TTT2Recomp"/L"saves";
    else return ERROR_PATH_NOT_FOUND;
    if(!root.is_absolute())return ERROR_INVALID_NAME;
    const uint64_t xuid=LocalXuid();
    if(!xuid)return ERROR_NOT_LOGGED_ON;          // nobody signed in: no save device
    wchar_t folder[17];swprintf(folder,17,L"%016llX",static_cast<unsigned long long>(xuid));
    root=root.lexically_normal()/folder;
    return NoReparse(root);
}
uint32_t RecordPath(const SaveContentRecord& record,std::filesystem::path& path)
{
    auto be=[&](size_t n){return uint32_t(record[n])<<24|uint32_t(record[n+1])<<16|uint32_t(record[n+2])<<8|record[n+3];};
    if(be(0)!=1 || be(4)!=1)return ERROR_INVALID_PARAMETER;
    size_t len=0;while(len<42&&record[264+len])++len;
    if(len==42)return ERROR_INVALID_NAME;
    std::string name(reinterpret_cast<const char*>(record.data()+264),len);
    if(!Component(name))return ERROR_INVALID_NAME;
    uint32_t e=Root(path);if(e)return e;
    path/=Lower(name);return NoReparse(path);
}
uint32_t ReadMetadata(const std::filesystem::path& path,SaveContentRecord& record,uint64_t& creator)
{
    if(uint32_t e=NoReparse(path/META))return e;
    HANDLE h=CreateFileW((path/META).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE){auto e=GetLastError();return e==ERROR_FILE_NOT_FOUND?ERROR_PATH_NOT_FOUND:e;}
    std::array<uint8_t,324> data{};DWORD n=0;LARGE_INTEGER size{};
    bool ok=GetFileSizeEx(h,&size)&&size.QuadPart==data.size()&&ReadFile(h,data.data(),DWORD(data.size()),&n,nullptr)&&n==data.size();
    CloseHandle(h);
    if(!ok || std::memcmp(data.data(),"TTT2SAV1",8))return ERROR_FILE_CORRUPT;
    creator=0;for(unsigned i=8;i<16;++i)creator=creator<<8|data[i];
    std::copy(data.begin()+16,data.end(),record.begin());
    std::filesystem::path expected;
    if(RecordPath(record,expected) || expected!=path)return ERROR_FILE_CORRUPT;
    return 0;
}
uint32_t WriteMetadata(const std::filesystem::path& path,const SaveContentRecord& record)
{
    std::array<uint8_t,324> data{};std::memcpy(data.data(),"TTT2SAV1",8);
    for(unsigned i=0;i<8;++i)data[8+i]=uint8_t(LocalXuid()>>(56-8*i));
    std::copy(record.begin(),record.end(),data.begin()+16);
    const auto temp=path/L".ttt2-content-tmp";
    if(uint32_t e=NoReparse(temp))return e;
    HANDLE h=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return GetLastError();
    DWORD n=0;uint32_t e=0;
    if(!WriteFile(h,data.data(),DWORD(data.size()),&n,nullptr)||n!=data.size()||!FlushFileBuffers(h))e=GetLastError()?GetLastError():ERROR_WRITE_FAULT;
    CloseHandle(h);if(e)return e;
    if(!MoveFileExW(temp.c_str(),(path/META).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return GetLastError();
    return 0;
}
uint32_t DeleteTree(const std::filesystem::path& path)
{
    std::filesystem::path root;uint32_t e=Root(root);if(e)return e;
    // Content paths are exactly one validated component below this root.
    if(path.parent_path()!=root || !Component(path.filename().string()))return ERROR_ACCESS_DENIED;
    if((e=NoReparse(path)))return e;
    if((e=FlushSaveFiles(path,true)))return e;
    std::error_code ec;
    for(auto it=std::filesystem::recursive_directory_iterator(path,ec);!ec&&it!=std::filesystem::recursive_directory_iterator();it.increment(ec))
        if((e=NoReparse(it->path())))return e;
    if(ec)return uint32_t(ec.value());
    std::filesystem::remove_all(path,ec);return ec?uint32_t(ec.value()):0;
}
}

uint32_t SaveUserRoot(std::filesystem::path& root) { return Root(root); }

// ---- the save journal (online rollback, rollback.cpp) ---------------------------------------------
// The game saves the profile at the end of every battle, on a worker, in the frame of the final KO.
// Online that frame can still be a prediction: a rewind across it puts the game's memory back to
// before the save while the files stay as the save left them, and the re-simulated KO saves again
// into files its memory does not expect - each record type alternates between two files (write
// NN_000_0, delete NN_000_1 or the other way round), so the second save found its file gone and the
// game showed "Save Error" (MEASURED 2026-10-21: three saves of one KO on the host of a three-player
// room, the last rewind in the middle of an open save, the heap broken; the next match froze in
// RtlAllocateHeap's free-list walk; in two-instance tests 2-6 "Save Error" boxes per run). So a
// container opened in a frame a rewind may still re-simulate is copied first (every file, its
// bytes); a rewind to that frame or before puts the copy back; a confirmed frame drops it.
namespace {
struct JournalFile
{
    std::wstring name;
    std::vector<uint8_t> bytes;
    FILETIME written{};                        // as copied: an unchanged file is left alone on undo
};
struct JournalEntry
{
    int32_t frame = 0;
    std::filesystem::path dir;
    bool existed = false;
    std::vector<JournalFile> files;
};
bool WrittenAt(const std::filesystem::path& p, FILETIME& t, uint64_t& size)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return false;
    t = a.ftLastWriteTime;
    size = uint64_t(a.nFileSizeHigh) << 32 | a.nFileSizeLow;
    return true;
}
std::vector<JournalEntry> g_journal;           // under g_saveMutex, oldest first
bool ReadWhole(const std::filesystem::path& p, std::vector<uint8_t>& out)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (64ll << 20);
    if (ok)
    {
        out.resize(size_t(size.QuadPart));
        DWORD n = 0;
        ok = out.empty() || (ReadFile(h, out.data(), DWORD(out.size()), &n, nullptr) && n == out.size());
    }
    CloseHandle(h);
    return ok;
}
bool WriteWhole(const std::filesystem::path& p, const std::vector<uint8_t>& data)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const bool ok = data.empty() || (WriteFile(h, data.data(), DWORD(data.size()), &n, nullptr) && n == data.size());
    CloseHandle(h);
    return ok;
}
}

void SaveJournalCapture(const SaveContentRecord& record, int32_t frame)
{
    if constexpr (!TTT2_LOCAL_SAVES) return;
    std::lock_guard lock(g_saveMutex);
    std::filesystem::path dir;
    if (RecordPath(record, dir)) return;
    for (const JournalEntry& e : g_journal)
        if (e.dir == dir && e.frame == frame) return;       // this frame's earlier state is kept already
    JournalEntry e;
    e.frame = frame;
    e.dir = dir;
    std::error_code ec;
    e.existed = std::filesystem::is_directory(dir, ec);
    size_t bytes = 0;
    if (e.existed)
        for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file(ec) || NoReparse(it->path())) continue;
            JournalFile f;
            f.name = it->path().filename().wstring();
            uint64_t size = 0;
            if (!WrittenAt(it->path(), f.written, size) || !ReadWhole(it->path(), f.bytes))
            {
                std::fprintf(stderr, "[save-journal] could not copy %ls - no copy of this save\n", it->path().filename().c_str());
                return;
            }
            bytes += f.bytes.size();
            e.files.push_back(std::move(f));
        }
    std::fprintf(stderr, "[save-journal] save in frame %d (not confirmed yet): %zu files (%zu bytes) of %ls copied\n", frame,
                 e.files.size(), bytes, dir.filename().c_str());
    g_journal.push_back(std::move(e));
}

int SaveJournalUndo(int32_t fromFrame)
{
    std::lock_guard lock(g_saveMutex);
    int restored = 0;
    while (!g_journal.empty() && g_journal.back().frame >= fromFrame)
    {
        JournalEntry e = std::move(g_journal.back());
        g_journal.pop_back();
        bool mounted = false;
        for (const auto& [alias, mount] : g_saveMounts) mounted |= mount == e.dir;
        std::filesystem::path root;
        if (mounted || Root(root) || e.dir.parent_path() != root || !Component(e.dir.filename().string()) || NoReparse(e.dir) ||
            FlushSaveFiles(e.dir, true))
        {
            std::fprintf(stderr, "[save-journal] the save of frame %d is still open - its files are NOT put back\n", e.frame);
            continue;
        }
        std::error_code ec;
        if (!e.existed)
        {
            std::filesystem::remove_all(e.dir, ec);
            std::fprintf(stderr, "[save-journal] rewind to frame %d: the save of frame %d removed (it was new)%s\n", fromFrame, e.frame,
                         ec ? " - FAILED" : "");
            ++restored;
            continue;
        }
        std::filesystem::create_directories(e.dir, ec);
        bool ok = !ec;
        // Files the save added go; the ones it changed or deleted come back byte for byte; the rest (the
        // container holds ~100 files - replays, ghosts - and a save touches a few) stay as they are.
        // MEASURED: rewriting all of them (with flushes) took one rewind ~250 ms.
        std::vector<std::filesystem::path> present;
        for (auto it = std::filesystem::directory_iterator(e.dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) present.push_back(it->path());
        size_t removed = 0, put = 0;
        for (const auto& p : present)
        {
            bool keep = false;
            for (const JournalFile& f : e.files) keep |= p.filename().wstring() == f.name;
            if (keep) continue;
            if (DeleteFileW(p.c_str())) ++removed;
            else ok = false;
        }
        for (const JournalFile& f : e.files)
        {
            const auto p = e.dir / f.name;
            FILETIME t{};
            uint64_t size = 0;
            if (WrittenAt(p, t, size) && size == f.bytes.size() && CompareFileTime(&t, &f.written) == 0) continue;
            if (WriteWhole(p, f.bytes)) ++put;
            else ok = false;
        }
        std::fprintf(stderr, "[save-journal] rewind to frame %d: the save of frame %d undone (%zu files put back, %zu removed)%s\n",
                     fromFrame, e.frame, put, removed, ok ? "" : " - SOME FILES FAILED");
        ++restored;
    }
    return restored;
}

void SaveJournalPrune(int32_t confirmedFrame)
{
    std::lock_guard lock(g_saveMutex);
    g_journal.erase(std::remove_if(g_journal.begin(), g_journal.end(), [&](const JournalEntry& e) { return e.frame <= confirmedFrame; }),
                    g_journal.end());
}

void SaveJournalClear()
{
    std::lock_guard lock(g_saveMutex);
    g_journal.clear();
}

uint32_t SaveListContent(std::vector<SaveContentRecord>& records)
{
    if constexpr (!TTT2_LOCAL_SAVES) { records.clear(); return 0; }
    std::lock_guard lock(g_saveMutex);records.clear();std::filesystem::path root;
    if(uint32_t e=Root(root))return e;
    std::error_code ec;if(!std::filesystem::exists(root,ec))return ec?uint32_t(ec.value()):0;
    std::vector<std::filesystem::path> paths;
    for(auto it=std::filesystem::directory_iterator(root,ec);!ec&&it!=std::filesystem::directory_iterator();it.increment(ec)) {
        if(uint32_t e=NoReparse(it->path()))return e;
        if(it->is_directory(ec))paths.push_back(it->path());
    }
    if(ec)return uint32_t(ec.value());std::sort(paths.begin(),paths.end());
    for(const auto& p:paths){SaveContentRecord r{};uint64_t owner=0;uint32_t e=ReadMetadata(p,r,owner);if(e)return e;if(owner==LocalXuid())records.push_back(r);}
    return 0;
}
uint32_t SaveCreateContent(const std::string& rootName,const SaveContentRecord& record,uint32_t mode,uint32_t& disposition)
{
    disposition=0;const auto name=Lower(rootName);
    if(mode<1||mode>5)return ERROR_INVALID_PARAMETER;
    if constexpr (!TTT2_LOCAL_SAVES) return mode==3||mode==5?ERROR_PATH_NOT_FOUND:ERROR_ACCESS_DENIED;
    if(name!="tag25save"&&name!="dummysave")return ERROR_INVALID_NAME;
    std::lock_guard lock(g_saveMutex);if(g_saveMounts.count(name))return ERROR_ALREADY_EXISTS;
    std::filesystem::path path;if(uint32_t e=RecordPath(record,path))return e;
    SaveContentRecord old{};uint64_t owner=0;uint32_t state=ReadMetadata(path,old,owner);
    bool exists=state==0;
    if(state && state!=ERROR_PATH_NOT_FOUND)return state;
    // Never adopt or erase an unrelated folder whose metadata is missing.
    std::error_code existsError;
    if(!exists && std::filesystem::exists(path,existsError))return ERROR_FILE_CORRUPT;
    if(existsError)return uint32_t(existsError.value());
    if(exists&&owner!=LocalXuid())return ERROR_ACCESS_DENIED;
    if(mode==1&&exists)return ERROR_ALREADY_EXISTS;
    if((mode==3||mode==5)&&!exists)return ERROR_PATH_NOT_FOUND;
    bool create=!exists||mode==2||mode==5;
    if(create) {
        for(const auto& [alias,mount]:g_saveMounts)if(mount==path)return ERROR_BUSY;
        if(exists){if(uint32_t e=DeleteTree(path))return e;}
        std::error_code ec;std::filesystem::create_directories(path,ec);if(ec)return uint32_t(ec.value());
        if(uint32_t e=NoReparse(path))return e;
        if(uint32_t e=WriteMetadata(path,record))return e;
    }
    g_saveMounts[name]=path;disposition=create?1:2;return 0;
}
uint32_t SaveCloseContent(const std::string& rootName,bool close)
{
    std::lock_guard lock(g_saveMutex);auto it=g_saveMounts.find(Lower(rootName));
    if(it==g_saveMounts.end())return ERROR_PATH_NOT_FOUND;
    if(uint32_t e=FlushSaveFiles(it->second,false))return e;
    if(close)g_saveMounts.erase(it);return 0;
}
uint32_t SaveDeleteContent(const SaveContentRecord& record)
{
    if constexpr (!TTT2_LOCAL_SAVES) return ERROR_ACCESS_DENIED;
    std::lock_guard lock(g_saveMutex);std::filesystem::path path;if(uint32_t e=RecordPath(record,path))return e;
    SaveContentRecord old{};uint64_t owner;if(uint32_t e=ReadMetadata(path,old,owner))return e;
    if(owner!=LocalXuid())return ERROR_ACCESS_DENIED;
    for(const auto& [name,mount]:g_saveMounts)if(mount==path)return ERROR_BUSY;
    return DeleteTree(path);
}
uint32_t SaveContentCreator(const SaveContentRecord& record,uint64_t& creator)
{
    if constexpr (!TTT2_LOCAL_SAVES) { creator=0;return ERROR_PATH_NOT_FOUND; }
    std::lock_guard lock(g_saveMutex);std::filesystem::path path;if(uint32_t e=RecordPath(record,path))return e;
    SaveContentRecord old{};return ReadMetadata(path,old,creator);
}
bool ResolveSaveFile(const std::string& guest,std::filesystem::path& host,std::filesystem::path& container,uint32_t& error)
{
    std::string p=guest;std::replace(p.begin(),p.end(),'\\','/');if(p.rfind("/??/",0)==0)p.erase(0,4);
    const auto colon=p.find(':');if(colon==std::string::npos)return false;
    const auto name=Lower(p.substr(0,colon));if(name!="tag25save"&&name!="dummysave")return false;
    std::lock_guard lock(g_saveMutex);error=ERROR_PATH_NOT_FOUND;
    auto it=g_saveMounts.find(name);if(it==g_saveMounts.end())return true;
    auto tail=p.substr(colon+1);if(!tail.empty()&&tail[0]=='/')tail.erase(0,1);
    // TTT2 saves use a flat namespace. Reject nested paths, traversal, ADS and
    // metadata names instead of normalizing them into a different file.
    if(!tail.empty()&&!Component(tail)){error=ERROR_INVALID_NAME;return true;}
    container=it->second;host=tail.empty()?container:container/tail;error=NoReparse(host);return true;
}
