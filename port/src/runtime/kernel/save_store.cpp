// A local, data-only container store. Never maps save paths into extracted disc
// files. Metadata is versioned and deterministic; all guest path components are
// constrained and existing reparse points are rejected before file operations.
#include "save_store.h"
#include "log_utf8.h"
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
void JournalCopyAllLocked(const std::filesystem::path& dir);   // the save journal, below (g_saveMutex held)
uint32_t DeleteTree(const std::filesystem::path& path)
{
    std::filesystem::path root;uint32_t e=Root(root);if(e)return e;
    // Content paths are exactly one validated component below this root.
    if(path.parent_path()!=root || !Component(path.filename().string()))return ERROR_ACCESS_DENIED;
    if((e=NoReparse(path)))return e;
    if((e=FlushSaveFiles(path,true)))return e;
    JournalCopyAllLocked(path);
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
// container opened in a frame a rewind may still re-simulate is journaled first; a rewind to that
// frame or before puts it back; a confirmed frame drops it.
// The journal lists the container's files (names, sizes, write times) when it opens and copies a file's
// bytes only right before the save changes it - opened to write or delete, the target of a rename, the
// whole container deleted (kernel_io.cpp calls SaveJournalBeforeChange). MEASURED 2026-11-09 (m174): copying
// all of them at the open (143 files, ~460 KB) took 49-72 ms of the save job's 66-95 ms, under the save
// lock the main thread takes every update - a ~50 ms hitch at every online KO; a save changes 4 of them
// (writes NN_000_x, deletes the other slot).
namespace {
struct JournalFile
{
    std::wstring name;
    uint64_t size = 0;                         // as listed when the container opened
    FILETIME listed{};
    bool copied = false;                       // `bytes` hold it as it was before the save changed it
    bool failed = false;                       // that copy failed: left as it is on undo (logged)
    unsigned long error = 0;                   // its Win32 error
    std::vector<uint8_t> bytes;
    FILETIME written{};                        // as copied: an unchanged file is left alone on undo
};
struct JournalEntry
{
    int32_t frame = 0;
    uint64_t id = 0;
    std::filesystem::path dir;
    bool existed = false;
    bool broken = false;                       // a copy failed: undo leaves every file as the save left them
    std::vector<JournalFile> files;            // every regular file when it opened (`copied`: its bytes too)
};
uint64_t g_journalIds = 0;                     // under g_saveMutex
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
// The regular files of a container from its directory entries (no file opened): name, size, write time.
// Reparse points are no save file: never listed, never removed on undo.
bool ListFiles(const std::filesystem::path& dir, std::vector<JournalFile>& out)
{
    WIN32_FIND_DATAW d{};
    HANDLE f = FindFirstFileExW((dir / L"*").c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do
    {
        if (d.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        JournalFile j;
        j.name = d.cFileName;
        j.size = uint64_t(d.nFileSizeHigh) << 32 | d.nFileSizeLow;
        j.listed = d.ftLastWriteTime;
        out.push_back(std::move(j));
    } while (FindNextFileW(f, &d));
    const bool ok = GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(f);
    return ok;
}
JournalFile* FindFile(std::vector<JournalFile>& files, const std::wstring& name)
{
    for (JournalFile& f : files)
        if (_wcsicmp(f.name.c_str(), name.c_str()) == 0) return &f;
    return nullptr;
}
// The newest journal entry of a container: the save that changes it now (an older one of the same container
// is undone only after the newer ones, which put back what they changed).
JournalEntry* NewestEntry(const std::filesystem::path& dir)
{
    for (auto it = g_journal.rbegin(); it != g_journal.rend(); ++it)
        if (it->dir == dir) return &*it;
    return nullptr;
}
bool CopyBeforeChange(const std::filesystem::path& p, JournalFile& f)
{
    uint64_t size = 0;
    f.copied = !NoReparse(p) && WrittenAt(p, f.written, size) && ReadWhole(p, f.bytes);
    f.failed = !f.copied;
    if (f.failed) { f.error = GetLastError(); f.bytes.clear(); }
    return f.copied;
}
// A newer save of a container that is not put back: its older ones are not either (review wf_fd6326af-ace). An older
// one's undo counts on the newer ones having put back what they changed (it removes what its listing lacks and has no
// copy of a file only a newer save changed): undone over a newer one left as it is, a record could keep neither slot.
void PoisonOlder(const std::filesystem::path& dir)
{
    for (JournalEntry& o : g_journal)
        if (o.dir == dir) o.broken = true;
}
// DeleteTree (g_saveMutex held): a journaled container deleted as a whole - the files not copied yet first.
void JournalCopyAllLocked(const std::filesystem::path& dir)
{
    if constexpr (!TTT2_LOCAL_SAVES) return;
    JournalEntry* e = NewestEntry(dir);
    if (!e || !e->existed || e->broken) return;
    size_t copied = 0, failed = 0;
    for (JournalFile& f : e->files)
        if (!f.copied && !f.failed) (CopyBeforeChange(dir / f.name, f) ? copied : failed)++;
    e->broken |= failed != 0;
    std::fprintf(stderr, "[save-journal] the container of the frame %d save is deleted: %zu files copied first%s\n", e->frame, copied,
                 failed ? " - SOME COULD NOT BE COPIED" : "");
}
}

void SaveJournalCapture(const SaveContentRecord& record, int32_t frame)
{
    if constexpr (!TTT2_LOCAL_SAVES) return;
    std::filesystem::path dir;
    {
        std::lock_guard lock(g_saveMutex);
        if (RecordPath(record, dir)) return;
        for (const JournalEntry& e : g_journal)
            if (e.dir == dir && e.frame == frame) return;   // this frame's earlier state is kept already
    }
    // The listing without the lock (the main thread takes it every update); nothing changes the container
    // here - the save job lists it before opening it.
    JournalEntry e;
    e.frame = frame;
    e.dir = dir;
    std::error_code ec;
    e.existed = std::filesystem::is_directory(dir, ec);
    if (e.existed && !ListFiles(dir, e.files))
    {
        const unsigned long error = GetLastError();
        std::fprintf(stderr, "[save-journal] could not list %s (error %lu) - no copy of this save\n", LogUtf8(dir.filename()).c_str(),
                     error);
        return;
    }
    std::lock_guard lock(g_saveMutex);
    for (const JournalEntry& o : g_journal)
        if (o.dir == dir && o.frame == frame) return;
    e.id = ++g_journalIds;
    std::fprintf(stderr, "[save-journal] save in frame %d (not confirmed yet): %zu files of %s listed (a file is copied before the save changes it)\n",
                 frame, e.files.size(), LogUtf8(dir.filename()).c_str());
    g_journal.push_back(std::move(e));
}

void SaveJournalBeforeChange(const std::filesystem::path& container, const std::filesystem::path& file)
{
    if constexpr (!TTT2_LOCAL_SAVES) return;
    const std::wstring name = file.filename().wstring();
    uint64_t id = 0;
    {
        std::lock_guard lock(g_saveMutex);
        JournalEntry* e = NewestEntry(container);
        if (!e || !e->existed || e->broken) return;        // no save journaled, a new container (removed on undo), or broken
        const JournalFile* f = FindFile(e->files, name);
        if (!f || f->copied || f->failed) return;          // new since the listing (removed on undo), or copied already
        id = e->id;
    }
    // The copy without the lock; only this save job changes the container while it is open.
    JournalFile copy;
    CopyBeforeChange(file, copy);
    std::lock_guard lock(g_saveMutex);
    for (JournalEntry& e : g_journal)
    {
        if (e.id != id) continue;
        JournalFile* f = FindFile(e.files, name);
        if (!f || f->copied || f->failed) return;
        f->copied = copy.copied;
        f->failed = copy.failed;
        f->bytes = std::move(copy.bytes);
        f->written = copy.written;
        e.broken |= f->failed;
        if (f->failed)
            std::fprintf(stderr, "[save-journal] frame %d: could not copy %s (error %lu) - this save is NOT put back on a rewind\n", e.frame,
                         LogUtf8(file.filename()).c_str(), copy.error);
        else
            std::fprintf(stderr, "[save-journal] frame %d: %s copied before the save changes it (%zu bytes)\n", e.frame,
                         LogUtf8(file.filename()).c_str(), f->bytes.size());
        return;
    }
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
            PoisonOlder(e.dir);
            continue;
        }
        if (e.broken)
        {
            PoisonOlder(e.dir);
            // All or nothing (review wf_f788ef00-b3b): the save's new slot removed while the slot it deleted cannot come
            // back would leave neither - the files stay as the save left them (as when the journal copied everything first).
            std::fprintf(stderr, "[save-journal] rewind to frame %d: the save of frame %d is NOT put back (a file could not be copied first, or a newer save of it was not put back) - its files stay as the saves left them\n",
                         fromFrame, e.frame);
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
        std::vector<JournalFile> present;
        ok &= ListFiles(e.dir, present);
        size_t removed = 0, put = 0, copied = 0, unexpected = 0, failed = 0;
        std::string firstUnexpected;
        for (const JournalFile& p : present)
        {
            if (FindFile(e.files, p.name)) continue;
            if (DeleteFileW((e.dir / p.name).c_str())) ++removed;
            else ok = false;
        }
        for (JournalFile& f : e.files)
        {
            const auto p = e.dir / f.name;
            copied += f.copied;
            if (f.failed) { ++failed; continue; }
            if (!f.copied)
            {
                // Never opened to be changed: as listed (a difference is logged, the file left as it is).
                const JournalFile* now = FindFile(present, f.name);
                if (now && now->size == f.size && CompareFileTime(&now->listed, &f.listed) == 0) continue;
                if (!unexpected++) firstUnexpected = LogUtf8(f.name);
                continue;
            }
            FILETIME t{};
            uint64_t size = 0;
            if (WrittenAt(p, t, size) && size == f.bytes.size() && CompareFileTime(&t, &f.written) == 0) continue;
            if (WriteWhole(p, f.bytes)) ++put;
            else ok = false;
        }
        std::fprintf(stderr, "[save-journal] rewind to frame %d: the save of frame %d undone (%zu files put back, %zu removed; %zu of %zu copied before the save changed them)%s\n",
                     fromFrame, e.frame, put, removed, copied, e.files.size(), ok ? "" : " - SOME FILES FAILED");
        if (unexpected || failed)
            std::fprintf(stderr, "[save-journal]   %zu files changed without a copy (first: %s), %zu could not be copied - left as they are\n",
                         unexpected, firstUnexpected.c_str(), failed);
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

bool SaveJournalHas(int32_t frame)
{
    std::lock_guard lock(g_saveMutex);
    for (const JournalEntry& e : g_journal)
        if (e.frame == frame) return true;
    return false;
}

// TEST 17: no save, once a session - with the unlock backups (unlock_all.cpp, <save root>\..\save_backups\<XUID>) that
// say whether this profile had saves before. Tester logs 46 / 48 / 50 began with a new save while the shader cache in
// the same folder was intact: saves deleted outside the game, which the log could not show.
static void NoteNoSave(const std::filesystem::path& root,size_t containers=0)
{
    static bool noted=false;if(noted)return;noted=true;
    std::error_code ec;size_t n=0;std::string newest;
    const auto backups=root.parent_path().parent_path()/L"save_backups"/root.filename();
    for(auto it=std::filesystem::directory_iterator(backups,ec);!ec&&it!=std::filesystem::directory_iterator();it.increment(ec))
        if(it->is_directory(ec)){++n;newest=(std::max)(newest,LogUtf8(it->path().filename()));}
    const bool folder=std::filesystem::exists(root,ec);
    char state[64];
    if(!folder)std::snprintf(state,sizeof(state),"no folder");
    else if(!containers)std::snprintf(state,sizeof(state),"the folder is empty");
    else std::snprintf(state,sizeof(state),"%zu save folder(s), none made by this profile",containers);
    std::fprintf(stderr,"[content] no save in %s (%s); %zu unlock backup(s)%s%s\n",LogUtf8(root).c_str(),
                 state,n,n?", the newest ":"",newest.c_str());
}

uint32_t SaveListContent(std::vector<SaveContentRecord>& records)
{
    if constexpr (!TTT2_LOCAL_SAVES) { records.clear(); return 0; }
    std::lock_guard lock(g_saveMutex);records.clear();std::filesystem::path root;
    if(uint32_t e=Root(root))return e;
    std::error_code ec;if(!std::filesystem::exists(root,ec)){if(!ec)NoteNoSave(root);return ec?uint32_t(ec.value()):0;}
    std::vector<std::filesystem::path> paths;
    for(auto it=std::filesystem::directory_iterator(root,ec);!ec&&it!=std::filesystem::directory_iterator();it.increment(ec)) {
        if(uint32_t e=NoReparse(it->path()))return e;
        if(it->is_directory(ec))paths.push_back(it->path());
    }
    if(ec)return uint32_t(ec.value());std::sort(paths.begin(),paths.end());
    for(const auto& p:paths){SaveContentRecord r{};uint64_t owner=0;uint32_t e=ReadMetadata(p,r,owner);if(e)return e;if(owner==LocalXuid())records.push_back(r);}
    if(records.empty())NoteNoSave(root,paths.size());
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
