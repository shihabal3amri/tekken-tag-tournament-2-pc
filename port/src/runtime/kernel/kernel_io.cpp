// Disc files remain read-only. Writes are restricted to a mounted save container
// provided by save_store; the optional store is isolated from extracted archives.
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "kernel_objects.h"
#include "save_store.h"
#include "kernel_content.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <filesystem>
#include <chrono>
#include <vector>
#include <algorithm>

void RollbackBeforeHostWrite(uint32_t addr, uint32_t size);   // rollback.cpp
void RollbackNoteIo(bool frameOnly, const char* op = "", const PPCContext* ctx = nullptr);   // rollback.cpp: no rewind across I/O

// Where the extracted disc lives. TTT2_VFS_ROOT overrides it, which is how a tree of
// EXPANDED archives is selected (26 of the 42 dataNNN.bin are LZX containers on the disc and
// have to be expanded before the game can read them - see PROJECT_STATUS session 8).
static const char* VfsRoot()
{
    static const std::string root = [] {
        const char* e = std::getenv("TTT2_VFS_ROOT");
        return std::string(e && *e ? e : "C:/ttt2/re/game_files");
    }();
    return root.c_str();
}
#define VFS_ROOT VfsRoot()

struct FileRecord
{
    HANDLE handle;
    const std::string path;
    std::filesystem::path saveContainer;
    bool writable=false, directory=false;
    bool deleteAccess=false;          // guest DELETE / GENERIC_ALL: delete-disposition and rename
    std::string guestPath;            // as opened; the base for RootDirectory-relative opens
    bool directoryScanned=false;
    size_t directoryCursor=0;
    struct DirectoryItem { std::string name; uint64_t size; };
    std::vector<DirectoryItem> directoryItems;
    std::mutex mutex;
    FileRecord(HANDLE h, std::string name) : handle(h), path(std::move(name)) {}
    ~FileRecord() { CloseHandle(handle); }
};

static std::mutex g_fmutex;
static std::unordered_map<uint32_t, std::shared_ptr<FileRecord>> g_files;
static uint32_t g_nextFile = 0xE0000000;   // file-handle range, below the kernel-object range (0xF...)
static std::vector<std::weak_ptr<FileRecord>> g_saveFiles;

uint32_t FlushSaveFiles(const std::filesystem::path& container,bool requireClosed)
{
    std::vector<std::shared_ptr<FileRecord>> files;
    { std::lock_guard lock(g_fmutex);
      for(auto it=g_saveFiles.begin();it!=g_saveFiles.end();) {
          if(auto f=it->lock()){if(f->saveContainer==container)files.push_back(f);++it;}
          else it=g_saveFiles.erase(it);
      }
    }
    if(requireClosed && !files.empty())return ERROR_BUSY;
    for(auto& f:files){std::lock_guard lock(f->mutex);if(f->writable && !FlushFileBuffers(f->handle))return GetLastError();}
    return 0;
}

static std::shared_ptr<FileRecord> GetFile(uint32_t handle)
{
    std::lock_guard l(g_fmutex);
    auto it = g_files.find(handle);
    return it == g_files.end() ? nullptr : it->second;
}

static constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011u;
static constexpr uint32_t IO_STATUS_NAME_NOT_FOUND = 0xC0000034u;
static constexpr uint32_t IO_STATUS_INVALID_HANDLE        = 0xC0000008u;
static constexpr uint32_t IO_STATUS_INVALID_PARAMETER = 0xC000000Du;

static uint32_t FileErrorStatus(DWORD error)
{
    switch (error)
    {
    case ERROR_HANDLE_EOF: return STATUS_END_OF_FILE;
    case ERROR_INVALID_HANDLE: return IO_STATUS_INVALID_HANDLE;
    case ERROR_INVALID_PARAMETER:
    case ERROR_NEGATIVE_SEEK: return IO_STATUS_INVALID_PARAMETER;
    case ERROR_ACCESS_DENIED: return 0xC0000022u; // STATUS_ACCESS_DENIED
    case ERROR_FILE_NOT_FOUND: return 0xC0000034u;
    case ERROR_PATH_NOT_FOUND: return 0xC000003Au;
    case ERROR_INVALID_NAME: return 0xC0000033u;
    case ERROR_SHARING_VIOLATION: return 0xC0000043u;
    case ERROR_DISK_FULL: return 0xC000007Fu;
    case ERROR_LOCK_VIOLATION: return 0xC0000054u; // STATUS_FILE_LOCK_CONFLICT
    case ERROR_CRC: return 0xC000009Cu; // STATUS_DEVICE_DATA_ERROR
    case ERROR_IO_DEVICE: return 0xC0000185u; // STATUS_IO_DEVICE_ERROR
    case ERROR_OPERATION_ABORTED: return 0xC0000120u; // STATUS_CANCELLED
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return 0xC000009Au; // STATUS_INSUFFICIENT_RESOURCES
    default: return STATUS_UNSUCCESSFUL;
    }
}

static std::string ReadObjAttrPath(uint8_t* base, uint32_t objAttrPtr)
{
    // XOBJECT_ATTRIBUTES: +0 RootDirectory, +4 ObjectName(ptr to ANSI_STRING), +8 Attributes.
    // XANSI_STRING: +0 Length(u16), +2 MaximumLength(u16), +4 Buffer(u32 guest ptr).
    if (!objAttrPtr) return {};
    uint32_t namePtr = GRead32(base, objAttrPtr + 4);
    if (!namePtr) return {};
    uint16_t len = GRead16(base, namePtr + 0);
    uint32_t buf = GRead32(base, namePtr + 4);
    std::string s;
    for (uint16_t i = 0; i < len && i < 1024; ++i) s += (char)GRead8(base, buf + i);
    // RootDirectory naming one of our file handles: the name is relative to it. TTT2 loads a
    // save by opening "tag25save:\\" as a directory and then "00_000_0.BIN" relative to it;
    // resolving that bare name against the disc made every save look damaged.
    const uint32_t root = GRead32(base, objAttrPtr + 0);
    if (root >= 0xE0000000u && root < 0xF0000000u && !s.empty())
        if (auto parent = GetFile(root); parent && parent->directory && !parent->guestPath.empty())
        {
            const std::string& p = parent->guestPath;
            const char last = p.back();
            s = p + (last == '\\' || last == '/' || last == ':' ? "" : "\\") + s;
        }
    return s;
}

static std::string ResolvePath(std::string p)
{
    // Strip a drive ("game:\", "d:\") or device ("\Device\Cdrom0\") prefix, then look under VFS_ROOT.
    auto colon = p.find(':');
    // "update:" is the mounted title update, NOT the disc: only <VFS_ROOT>/update (TU3's
    // data100.bin - expanded - and data100.ofs). TTT2 probes update:\nrfo.bin first; serving the
    // disc's nrfo there made it index data100.bin with the disc table and never read
    // data100.ofs, so every TU3-overridden asset read garbage (the main menu never built).
    if (colon != std::string::npos && _strnicmp(p.c_str(), "update", colon) == 0 && colon == 6)
    {
        std::string rest = p.substr(colon + 1);
        for (auto& c : rest) if (c == '\\') c = '/';
        while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
        return std::string(VFS_ROOT) + "/update" + (rest.empty() ? "/" : "/" + rest);
    }
    if (colon != std::string::npos)
        p = p.substr(colon + 1);
    else if (p.size() > 8 && (p.rfind("\\Device\\", 0) == 0))
    {
        int slashes = 0; size_t pos = 0;
        for (size_t i = 1; i < p.size(); ++i) if (p[i] == '\\' && ++slashes == 2) { pos = i + 1; break; }
        p = p.substr(pos);
    }
    for (auto& c : p) if (c == '\\') c = '/';
    while (!p.empty() && p[0] == '/') p.erase(0, 1);

    std::error_code ec;
    std::string full = std::string(VFS_ROOT) + "/" + p;
    if (std::filesystem::exists(full, ec)) return full;

    // Fall back to the basename, case-insensitively (the game's data*.bin live at the root).
    auto slash = p.find_last_of('/');
    std::string bn = (slash == std::string::npos) ? p : p.substr(slash + 1);
    for (auto& e : std::filesystem::directory_iterator(VFS_ROOT, ec))
        if (_stricmp(e.path().filename().string().c_str(), bn.c_str()) == 0)
            return e.path().string();
    return full;
}

// Final (OS-resolved) path of an open handle, without the \\\\?\\ prefix; empty on failure.
static std::wstring FinalPath(HANDLE h)
{
    std::wstring path(32768, L'\0');
    const DWORD n=GetFinalPathNameByHandleW(h,path.data(),DWORD(path.size()),FILE_NAME_NORMALIZED);
    if(!n||n>=path.size())return L"";
    path.resize(n);
    if(path.rfind(L"\\\\?\\",0)==0)path.erase(0,4);
    return path;
}
static std::wstring FinalPathOf(const std::filesystem::path& directory)
{
    HANDLE h=CreateFileW(directory.c_str(),FILE_TRAVERSE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(h==INVALID_HANDLE_VALUE)return L"";
    std::wstring path=FinalPath(h);
    CloseHandle(h);
    return path;
}

static void DoOpen(PPCContext& ctx, uint8_t* base,bool create)
{
    // NtCreateFile: r3=FileHandle*, r5=ObjectAttributes*, r6=IoStatusBlock*  (NtOpenFile matches these).
    uint32_t handleOut = ctx.r3.u32, objAttr = ctx.r5.u32, ioStatus = ctx.r6.u32;
    std::string guestPath = ReadObjAttrPath(base, objAttr);
    if (guestPath.empty() && objAttr && XamGuestRange(objAttr, 12))
    {
        // Diagnostics: the menu load issues a few opens whose name reads as empty.
        const uint32_t root = GRead32(base, objAttr), name = GRead32(base, objAttr + 4);
        std::fprintf(stderr, "[io] empty-name open: objattr=%08X root=%08X name=%08X attr=%08X len=%u max=%u buf=%08X access=%08X%s\n",
                     objAttr, root, name, GRead32(base, objAttr + 8),
                     name && XamGuestRange(name, 8) ? GRead16(base, name) : 0u,
                     name && XamGuestRange(name, 8) ? GRead16(base, name + 2) : 0u,
                     name && XamGuestRange(name, 8) ? GRead32(base, name + 4) : 0u, ctx.r4.u32,
                     create ? " (NtCreateFile)" : " (NtOpenFile)");
        // Guest backtrace: [r1] = caller frame, saved LR at [callerFrame - 8].
        char line[512];
        int n = std::snprintf(line, sizeof(line), "[io] empty-name open backtrace:");
        uint32_t frame = ctx.r1.u32;
        for (int i = 0; i < 16 && frame && frame < 0xFFFFFFF0u && n < 460; ++i)
        {
            const uint32_t prev = GRead32(base, frame);
            if (!prev || prev <= frame || prev - frame > 0x10000) break;
            n += std::snprintf(line + n, sizeof(line) - n, " %08X", GRead32(base, prev - 8));
            frame = prev;
        }
        std::snprintf(line + n, sizeof(line) - n, "\n");
        std::fputs(line, stderr);
    }
    std::filesystem::path savePath,container;uint32_t saveError=0;
    bool save=ResolveSaveFile(guestPath,savePath,container,saveError);
    std::string host = save?savePath.string():ResolvePath(guestPath);
    HANDLE h=INVALID_HANDLE_VALUE;uint32_t status=STATUS_SUCCESS,information=1;
    const uint32_t writeMask=0x50010116u; // generic write/all, delete, write/append/EA/attributes
    const bool writable=(ctx.r4.u32&writeMask)!=0;
    bool directory=false;
    const char* stage="";                                  // save-path diagnostics: failing check
    if(saveError){status=FileErrorStatus(saveError);stage="resolve";}
    else if(!save) {
        if(writable || (create && ctx.r10.u32!=1))status=0xC0000022u;
        else {h=CreateFileA(host.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
              if(h==INVALID_HANDLE_VALUE)status=IO_STATUS_NAME_NOT_FOUND;}
    } else {
        // TTT2's actual NtOpenFile caller supplies r7=share/r8=options. Create
        // supplies r9=share/r10=disposition and its ninth argument at SP+0x54.
        if(create&&(!XamGuestRange(ctx.r1.u32,0x58)||(ctx.r7.u32&&!XamGuestRange(ctx.r7.u32,8)))) {
            ctx.r3.u32=IO_STATUS_INVALID_PARAMETER;return;
        }
        uint32_t options=create?GRead32(base,ctx.r1.u32+0x54):ctx.r8.u32;
        const uint32_t disposition=create?ctx.r10.u32:1;
        directory=(options&1)!=0;
        if(disposition>5 || (options&0x1000)){status=0xC0000002u;stage="options";} // delete-on-close not implemented
        else {
            // Open a verified container handle first; the flat child lookup is
            // relative to that handle, not a reparsable parent path.
            HANDLE parent=CreateFileW(container.c_str(),FILE_LIST_DIRECTORY|FILE_TRAVERSE,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(parent==INVALID_HANDLE_VALUE){status=FileErrorStatus(GetLastError());stage="container";}
            else {
                BY_HANDLE_FILE_INFORMATION pi{};
                const std::wstring actual=FinalPath(parent);
                // The container must be exactly <save root>\\<name> as the OS resolves the root. The
                // root's own final path can legitimately differ from its logical one: a packaged
                // (MSIX) launcher, e.g. the Claude desktop app, redirects %LOCALAPPDATA% writes to
                // ...\\Packages\\<app>\\LocalCache\\Local. Reparse points in the logical root were
                // already rejected (save_store NoReparse); the container itself must be none.
                const std::wstring expected=FinalPathOf(container.parent_path())+L"\\"+container.filename().wstring();
                if(!GetFileInformationByHandle(parent,&pi) || (pi.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
                   actual.empty() || _wcsicmp(actual.c_str(),expected.c_str())) {
                    status=0xC0000022u;stage="final-path";
                    std::fprintf(stderr,"[io] save container final path '%ls' != '%ls'\n",actual.c_str(),expected.c_str());
                }
                else if(savePath==container) {
                    if(!directory||disposition!=1||writable){status=0xC0000022u;stage="root-access";}
                    else {h=parent;parent=INVALID_HANDLE_VALUE;}
                } else if(directory){status=0xC0000002u;stage="nested";} // TTT2 save files are flat.
                else {
                    struct NativeString {USHORT length,maximum;PWSTR buffer;};
                    struct NativeAttributes {ULONG length;HANDLE root;NativeString* name;ULONG attributes;void* security;void* qos;};
                    struct NativeStatus {union {LONG status;void* pointer;};ULONG_PTR information;};
                    using OpenNative=LONG (NTAPI*)(PHANDLE,ACCESS_MASK,NativeAttributes*,NativeStatus*,LARGE_INTEGER*,ULONG,ULONG,ULONG,ULONG,void*,ULONG);
                    static auto ntCreate=reinterpret_cast<OpenNative>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtCreateFile"));
                    auto name=savePath.filename().wstring();NativeString text{USHORT(name.size()*2),USHORT(name.size()*2),name.data()};
                    NativeAttributes oa{sizeof(NativeAttributes),parent,&text,0x40,nullptr,nullptr};NativeStatus ios{};
                    LARGE_INTEGER allocation{};LARGE_INTEGER* allocationPtr=nullptr;
                    if(create&&ctx.r7.u32){allocation.QuadPart=(uint64_t(GRead32(base,ctx.r7.u32))<<32)|GRead32(base,ctx.r7.u32+4);allocationPtr=&allocation;}
                    // Host reads/writes use synchronous handles; guest options and
                    // access still control permission, sharing and disposition. The guest's
                    // FILE_NO_INTERMEDIATE_BUFFERING (0x8, TTT2's save writes use 0x48) is dropped:
                    // on the host it demands sector-aligned sizes and buffers, failing small writes.
                    ULONG hostOptions=(options&~0x18u)|0x20u|0x00200000u;
                    if(!ntCreate)status=0xC0000002u;
                    // FILE_READ_ATTRIBUTES lets the reparse check below query a handle the guest
                    // opened for DELETE only (TTT2's safe-save probes the old slot that way).
                    else status=uint32_t(ntCreate(&h,ctx.r4.u32|SYNCHRONIZE|FILE_READ_ATTRIBUTES,&oa,&ios,allocationPtr,
                        create?ctx.r8.u32:FILE_ATTRIBUTE_NORMAL,create?ctx.r9.u32:ctx.r7.u32,disposition,hostOptions,nullptr,0));
                    information=uint32_t(ios.information);
                    if(status&0x80000000u)stage="host-NtCreateFile";
                    if(!(status&0x80000000u)) {
                        BY_HANDLE_FILE_INFORMATION fi{};
                        if(!GetFileInformationByHandle(h,&fi)||(fi.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {CloseHandle(h);h=INVALID_HANDLE_VALUE;status=0xC0000022u;}
                    }
                }
                if(parent!=INVALID_HANDLE_VALUE)CloseHandle(parent);
            }
        }
    }
    {
        char line[1200];
        if (save)   // save paths: the reason matters (mount state, access, disposition, options)
            std::snprintf(line, sizeof(line), "[io] open '%s' -> %s status=%08X stage=%s access=%08X disp=%u opts=%08X%s\n",
                          guestPath.c_str(), h == INVALID_HANDLE_VALUE ? "FAILED" : host.c_str(), status, stage,
                          ctx.r4.u32, create ? ctx.r10.u32 : 1u,
                          create ? (XamGuestRange(ctx.r1.u32, 0x58) ? GRead32(base, ctx.r1.u32 + 0x54) : 0u) : ctx.r8.u32,
                          saveError ? " (not mounted / invalid)" : "");
        else
            std::snprintf(line, sizeof(line), "[io] open '%s' -> %s\n", guestPath.c_str(),
                          h == INVALID_HANDLE_VALUE ? "NOT FOUND" : host.c_str());
        std::fputs(line, stderr);
    }
    if (status&0x80000000u)
    {
        if (ioStatus) { GWrite32(base, ioStatus + 0, status); GWrite32(base, ioStatus + 4, 0); }
        ctx.r3.u32 = status;
        return;
    }
    uint32_t handle;
    {
        std::lock_guard l(g_fmutex);
        handle = g_nextFile++;
        auto slash = host.find_last_of("/\\");
        g_files[handle] = std::make_shared<FileRecord>(h,
            (slash == std::string::npos) ? host : host.substr(slash + 1));
        auto& file=g_files[handle];file->writable=save&&(ctx.r4.u32&0x50000006u);file->directory=directory;
        file->guestPath=guestPath;
        file->deleteAccess=save&&(ctx.r4.u32&0x10010000u);
        if(save){file->saveContainer=container;g_saveFiles.push_back(file);}
    }
    GWrite32(base, handleOut, handle);
    if (ioStatus) { GWrite32(base, ioStatus + 0, STATUS_SUCCESS); GWrite32(base, ioStatus + 4, information); }
    ctx.r3.u32 = STATUS_SUCCESS;
}

// Called from NtClose (kernel_objects.cpp) for handles in the file range.
bool TryCloseGuestFile(uint32_t handle)
{
    if (handle < 0xE0000000 || handle >= 0xF0000000) return false;
    std::lock_guard l(g_fmutex);
    auto it = g_files.find(handle);
    // An operation that already looked up this file retains the host handle until it
    // completes. Otherwise CloseHandle could race a read, or the HANDLE could be reused.
    if (it != g_files.end()) g_files.erase(it);
    return true;
}

// TTT2_FRAMETIME=1: host file calls over 5 ms (the caller waits for them: reads are synchronous).
// Per-thread totals of the host file calls (TTT2_FRAMETIME): diag_hooks.cpp reads them around slow tasks.
struct ThreadIo { uint64_t calls = 0, bytes = 0; double ms = 0; };
thread_local ThreadIo g_threadIo;
void ThreadIoTotals(uint64_t& calls, uint64_t& bytes, double& ms) { calls = g_threadIo.calls; bytes = g_threadIo.bytes; ms = g_threadIo.ms; }
static void LogSlowIo(const char* what, std::chrono::steady_clock::time_point start, uint32_t length)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_FRAMETIME"); return e && *e == '1'; }();
    if (!on) return;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    g_threadIo.calls += 1;
    g_threadIo.bytes += length;
    g_threadIo.ms += ms;
    static std::atomic<int> logged{ 0 };
    if (ms > 5.0 && logged++ < 300) std::fprintf(stderr, "[frame] slow %s %.1f ms (%u bytes) thread %lu\n", what, ms, length, GetCurrentThreadId());
}
PPC_FUNC(__imp__NtCreateFile) { KTrace("NtCreateFile"); RollbackNoteIo(true, "NtCreateFile", &ctx); const auto t = std::chrono::steady_clock::now(); DoOpen(ctx, base,true); LogSlowIo("open", t, 0); }
PPC_FUNC(__imp__NtOpenFile)   { KTrace("NtOpenFile");   RollbackNoteIo(true, "NtOpenFile", &ctx); const auto t = std::chrono::steady_clock::now(); DoOpen(ctx, base,false); LogSlowIo("open", t, 0); }

PPC_FUNC(__imp__NtReadFile)
{
    KTrace("NtReadFile");
    RollbackNoteIo(true, "NtReadFile", &ctx);
    // r3=FileHandle, r4=Event, r5=ApcRoutine, r6=ApcContext,
    // r7=IoStatusBlock, r8=Buffer, r9=Length, r10=ByteOffset* (LARGE_INTEGER, optional)
    auto file = GetFile(ctx.r3.u32);
    uint32_t event = ctx.r4.u32, apcRoutine = ctx.r5.u32, apcContext = ctx.r6.u32;
    uint32_t ioStatus = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, byteOffPtr = ctx.r10.u32;
    if (!file) { ctx.r3.u32 = IO_STATUS_INVALID_HANDLE; return; }
    int64_t effOff = -1;
    if (byteOffPtr)
    {
        uint64_t off = ((uint64_t)GRead32(base, byteOffPtr) << 32) | GRead32(base, byteOffPtr + 4);
        effOff = (int64_t)off;
        // FILE_USE_FILE_POINTER_POSITION (-2) and a null offset both use the current
        // position. Other negative offsets fail before an I/O is submitted.
        if (effOff < 0 && effOff != -2) { ctx.r3.u32 = IO_STATUS_INVALID_PARAMETER; return; }
    }
    DWORD read = 0;
    uint32_t status = STATUS_SUCCESS;
    const auto ioStart = std::chrono::steady_clock::now();
    {
        // The host handle is synchronous. Its shared file pointer needs one lock across
        // the entire seek/read sequence, including NtSet/QueryInformationFile calls.
        std::lock_guard fileLock(file->mutex);
        if (length != 0)
        {
            LARGE_INTEGER zero{}, original{};
            if (!SetFilePointerEx(file->handle, zero, &original, FILE_CURRENT))
                status = FileErrorStatus(GetLastError());
            else
            {
                const bool explicitOffset = byteOffPtr && effOff != -2;
                if (explicitOffset)
                {
                    LARGE_INTEGER offset; offset.QuadPart = effOff;
                    if (!SetFilePointerEx(file->handle, offset, nullptr, FILE_BEGIN))
                        status = FileErrorStatus(GetLastError());
                }
                else effOff = original.QuadPart;
                if (status == STATUS_SUCCESS)
                {
                    RollbackBeforeHostWrite(buffer, length);   // rollback.cpp: write-protected pages
                    if (!ReadFile(file->handle, base + buffer, length, &read, nullptr))
                        status = FileErrorStatus(GetLastError());
                    else if (read == 0)
                        status = STATUS_END_OF_FILE;
                    // Native NtReadFile preserves the old position on EOF (including a
                    // request beyond EOF) and failed reads. Partial reads still succeed.
                    if (explicitOffset && (read == 0 || status != STATUS_SUCCESS) &&
                        !SetFilePointerEx(file->handle, original, nullptr, FILE_BEGIN))
                        status = FileErrorStatus(GetLastError());
                }
            }
        }
        // A valid zero-length read succeeds without changing the file position.
    }
    LogSlowIo("read", ioStart, length);
    {
        static std::atomic<uint64_t> dlog{0};
        uint64_t dn = ++dlog;
        if (dn <= 300 || dn % 200 == 0)
            std::fprintf(stderr, "[io] read h=%08X '%s' off=%lld len=%u got=%lu status=%08X\n",
                         ctx.r3.u32, file->path.c_str(), (long long)effOff, length, (unsigned long)read, status);
    }
    if (status == STATUS_END_OF_FILE)
    {
        static std::atomic<uint32_t> eoflog{0};
        if (++eoflog <= 40)
            std::fprintf(stderr, "[io] EOF read '%s' off=%lld len=%u\n",
                         file->path.c_str(), (long long)effOff, length);
    }
    if (ioStatus) { GWrite32(base, ioStatus + 0, status); GWrite32(base, ioStatus + 4, read); }
    // The NT kernel signals the caller's event when an I/O completes (even synchronously);
    // async loaders wait on it after issuing the read.
    if (event) KernelSignalEventHandle(event);
    // Deliver the I/O completion APC: ApcRoutine(ApcContext, IoStatusBlock, 0) - on the
    // dispatcher thread (inline delivery would reuse the caller's in-use guest stack).
    if (apcRoutine)
    {
        static std::atomic<unsigned> alog{0};
        if (++alog <= 8)
        {
            std::fprintf(stderr, "[io] read APC -> %08X(ctx=%08X, iosb=%08X)\n",
                         apcRoutine, apcContext, ioStatus);
        }
        KernelQueueGuestCall(apcRoutine, apcContext, ioStatus, 0, 0);
    }
    static std::atomic<uint64_t> reads{0}, bytes{0};
    bytes += read;
    uint64_t n = ++reads;
    if (n <= 5 || n % 200 == 0)
    {
        char line[160];
        std::snprintf(line, sizeof(line), "[io] read #%llu h=%08X len=%u got=%lu total=%lluKB\n",
                      (unsigned long long)n, ctx.r3.u32, length, read, (unsigned long long)(bytes / 1024));
        std::fputs(line, stderr);
    }
    ctx.r3.u32 = status;
}

PPC_FUNC(__imp__NtSetInformationFile)
{
    KTrace("NtSetInformationFile");
    RollbackNoteIo(true, "NtSetInformationFile", &ctx);     // seeks by streaming threads are theirs; deletes come from the frame
    // r3=handle, r4=IoStatusBlock, r5=FileInformation, r6=Length, r7=FileInformationClass
    auto file = GetFile(ctx.r3.u32);
    if (!file) { ctx.r3.u32 = IO_STATUS_INVALID_HANDLE; return; }
    uint32_t status = STATUS_SUCCESS;
    if (ctx.r7.u32 == 14 /*FilePositionInformation*/)
    {
        if (!ctx.r5.u32 || ctx.r6.u32 < 8) { ctx.r3.u32 = IO_STATUS_INVALID_PARAMETER; return; }
        uint64_t pos = ((uint64_t)GRead32(base, ctx.r5.u32) << 32) | GRead32(base, ctx.r5.u32 + 4);
        if ((int64_t)pos < 0) { ctx.r3.u32 = IO_STATUS_INVALID_PARAMETER; return; }
        std::lock_guard fileLock(file->mutex);
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)pos;
        if (!SetFilePointerEx(file->handle, li, nullptr, FILE_BEGIN))
            status = FileErrorStatus(GetLastError());
    }
    else if(ctx.r7.u32==20 && file->writable && !file->saveContainer.empty()) {
        if(!XamGuestRange(ctx.r5.u32,8)||ctx.r6.u32<8){ctx.r3.u32=IO_STATUS_INVALID_PARAMETER;return;}
        FILE_END_OF_FILE_INFO info{};info.EndOfFile.QuadPart=(uint64_t(GRead32(base,ctx.r5.u32))<<32)|GRead32(base,ctx.r5.u32+4);
        if(info.EndOfFile.QuadPart<0){ctx.r3.u32=IO_STATUS_INVALID_PARAMETER;return;}
        std::lock_guard fileLock(file->mutex);
        if(!SetFileInformationByHandle(file->handle,FileEndOfFileInfo,&info,sizeof(info)))status=FileErrorStatus(GetLastError());
    }
    else if(ctx.r7.u32==13 && !file->saveContainer.empty()) {
        // FileDispositionInformation { BOOLEAN DeleteFile }: removed when the last handle closes.
        if(!XamGuestRange(ctx.r5.u32,1)||ctx.r6.u32<1){ctx.r3.u32=IO_STATUS_INVALID_PARAMETER;return;}
        if(!file->deleteAccess||file->directory)status=0xC0000022u;
        else {
            FILE_DISPOSITION_INFO info{};info.DeleteFile=GRead8(base,ctx.r5.u32)!=0;
            std::lock_guard fileLock(file->mutex);
            if(!SetFileInformationByHandle(file->handle,FileDispositionInfo,&info,sizeof(info)))status=FileErrorStatus(GetLastError());
        }
        std::fprintf(stderr,"[io] delete-disposition '%s' -> %08X\n",file->path.c_str(),status);
    }
    else if(ctx.r7.u32==10 && !file->saveContainer.empty()) {
        // FileRenameInformation (Xenia X_FILE_RENAME_INFORMATION): +0 ReplaceIfExists, +4 root
        // directory handle, +8 ANSI_STRING {length, maximum, buffer}. The target must be a flat
        // name in the SAME save container (TTT2 writes NN_000_0.BIN, then renames it to _1).
        if(!XamGuestRange(ctx.r5.u32,16)||ctx.r6.u32<16){ctx.r3.u32=IO_STATUS_INVALID_PARAMETER;return;}
        const bool replace=GRead32(base,ctx.r5.u32)!=0;
        const uint32_t root=GRead32(base,ctx.r5.u32+4),length=GRead16(base,ctx.r5.u32+8),buffer=GRead32(base,ctx.r5.u32+12);
        std::string target;
        if(length&&XamGuestRange(buffer,length,1))for(uint32_t i=0;i<length;++i)target+=char(GRead8(base,buffer+i));
        std::filesystem::path host,container;uint32_t error=0;
        if(root)status=0xC00000BBu;                                         // STATUS_NOT_SUPPORTED
        else if(!file->deleteAccess||file->directory)status=0xC0000022u;
        else if(target.empty()||!ResolveSaveFile(target,host,container,error)||error||
                container!=file->saveContainer||host==container)status=error?FileErrorStatus(error):0xC0000033u;
        else {
            const std::wstring name=host.wstring();
            std::vector<uint8_t> buf(sizeof(FILE_RENAME_INFO)+name.size()*sizeof(wchar_t));
            auto* info=reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
            info->ReplaceIfExists=replace;info->RootDirectory=nullptr;
            info->FileNameLength=DWORD(name.size()*sizeof(wchar_t));
            std::memcpy(info->FileName,name.data(),name.size()*sizeof(wchar_t));
            std::lock_guard fileLock(file->mutex);
            if(!SetFileInformationByHandle(file->handle,FileRenameInfo,info,DWORD(buf.size())))status=FileErrorStatus(GetLastError());
        }
        std::fprintf(stderr,"[io] rename '%s' -> '%s' replace=%d -> %08X\n",file->path.c_str(),target.c_str(),replace?1:0,status);
    }
    else if(!file->saveContainer.empty())status=0xC0000002u;
    if (ctx.r4.u32) { GWrite32(base, ctx.r4.u32 + 0, status); GWrite32(base, ctx.r4.u32 + 4, 0); }
    ctx.r3.u32 = status;
}

PPC_FUNC(__imp__NtQueryInformationFile)
{
    KTrace("NtQueryInformationFile");
    // r3=handle, r4=IoStatusBlock, r5=FileInformation OUT, r6=Length, r7=FileInformationClass
    auto file = GetFile(ctx.r3.u32);
    if (!file) { ctx.r3.u32 = IO_STATUS_INVALID_HANDLE; return; }
    uint32_t out = ctx.r5.u32, cls = ctx.r7.u32;
    uint32_t status = STATUS_SUCCESS;
    bool handled = false;
    LARGE_INTEGER size{};
    std::lock_guard fileLock(file->mutex);
    if (!file->directory && !GetFileSizeEx(file->handle, &size)) status = FileErrorStatus(GetLastError());
    if (status == STATUS_SUCCESS && out)
    {
        switch (cls)
        {
        case 4:  // FileBasicInformation: 4 timestamps(8) + FileAttributes(4)
            for (int i = 0; i < 0x20; i += 4) GWrite32(base, out + i, 0);
            GWrite32(base, out + 0x20, file->directory?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_NORMAL);
            handled = true;
            break;
        case 5:  // FileStandardInformation: AllocationSize @0, EndOfFile @8, links/delete/dir
            GWrite32(base, out + 0, (uint32_t)(size.QuadPart >> 32)); GWrite32(base, out + 4, (uint32_t)size.QuadPart);
            GWrite32(base, out + 8, (uint32_t)(size.QuadPart >> 32)); GWrite32(base, out + 12, (uint32_t)size.QuadPart);
            GWrite32(base, out + 16, 1); GWrite32(base, out + 20, 0);
            if(file->directory)GWrite8(base,out+21,1);
            handled = true;
            break;
        case 6:  // FileInternalInformation: 8-byte file index
            GWrite32(base, out + 0, 0); GWrite32(base, out + 4, ctx.r3.u32);
            handled = true;
            break;
        case 7:  // FileEaInformation: EaSize
            GWrite32(base, out + 0, 0);
            handled = true;
            break;
        case 27: // FileModeInformation: no FILE_SYNCHRONOUS_IO_* flags (handle opened async)
            GWrite32(base, out + 0, 0);
            handled = true;
            break;
        case 14: // FilePositionInformation
        {
            if (ctx.r6.u32 < 8) { status = IO_STATUS_INVALID_PARAMETER; break; }
            LARGE_INTEGER z{}, cur{};
            if (!SetFilePointerEx(file->handle, z, &cur, FILE_CURRENT))
            { status = FileErrorStatus(GetLastError()); break; }
            GWrite32(base, out + 0, (uint32_t)(cur.QuadPart >> 32)); GWrite32(base, out + 4, (uint32_t)cur.QuadPart);
            handled = true;
            break;
        }
        case 34: // FileNetworkOpenInformation: times @0..0x18, AllocationSize @0x20, EndOfFile @0x28, Attributes @0x30
            for (int i = 0; i < 0x20; i += 4) GWrite32(base, out + i, 0);
            GWrite32(base, out + 0x20, (uint32_t)(size.QuadPart >> 32)); GWrite32(base, out + 0x24, (uint32_t)size.QuadPart);
            GWrite32(base, out + 0x28, (uint32_t)(size.QuadPart >> 32)); GWrite32(base, out + 0x2C, (uint32_t)size.QuadPart);
            GWrite32(base, out + 0x30, file->directory?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_NORMAL); GWrite32(base, out + 0x34, 0);
            handled = true;
            break;
        }
    }
    {
        static std::atomic<uint32_t> qlog{0};
        if (++qlog <= 200 || !handled)
            std::fprintf(stderr, "[io] qinfo h=%08X '%s' class=%u len=%u size=%lld%s\n",
                         ctx.r3.u32, file->path.c_str(), cls, ctx.r6.u32,
                         (long long)size.QuadPart, handled ? "" : " UNHANDLED");
    }
    if (ctx.r4.u32) { GWrite32(base, ctx.r4.u32 + 0, status); GWrite32(base, ctx.r4.u32 + 4, status == STATUS_SUCCESS ? ctx.r6.u32 : 0); }
    ctx.r3.u32 = status;
}

// Query attributes (used to test existence / get size) by path in the object attributes.
PPC_FUNC(__imp__NtQueryFullAttributesFile)
{
    KTrace("NtQueryFullAttributesFile");
    // r3=ObjectAttributes*, r4=FILE_NETWORK_OPEN_INFORMATION*
    std::string guestPath = ReadObjAttrPath(base, ctx.r3.u32);
    std::filesystem::path savePath,container;uint32_t saveError=0;
    bool save=ResolveSaveFile(guestPath,savePath,container,saveError);
    if(saveError){ctx.r3.u32=FileErrorStatus(saveError);return;}
    std::string host = save?savePath.string():ResolvePath(guestPath);
    std::error_code ec;
    {
        char line[1200];
        std::snprintf(line, sizeof(line), "[io] query '%s' -> %s\n", guestPath.c_str(),
                      std::filesystem::exists(host, ec) ? host.c_str() : "NOT FOUND");
        std::fputs(line, stderr);
    }
    if (std::filesystem::exists(host, ec))
    {
        const bool directory=std::filesystem::is_directory(host,ec);
        uint64_t sz=directory?0:std::filesystem::file_size(host,ec);
        if(ec){ctx.r3.u32=FileErrorStatus(DWORD(ec.value()));return;}
        uint32_t o = ctx.r4.u32;
        // FILE_NETWORK_OPEN_INFORMATION: times @0..0x18, AllocationSize @0x20, EndOfFile @0x28, Attributes @0x30
        if (o) { for (int i = 0; i < 0x38; i += 4) GWrite32(base, o + i, 0);
                 GWrite32(base, o + 0x20, (uint32_t)(sz >> 32)); GWrite32(base, o + 0x24, (uint32_t)sz);   // AllocationSize
                 GWrite32(base, o + 0x28, (uint32_t)(sz >> 32)); GWrite32(base, o + 0x2C, (uint32_t)sz);   // EndOfFile
                 GWrite32(base, o + 0x30, directory?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_NORMAL); }
        ctx.r3.u32 = STATUS_SUCCESS;
    }
    else ctx.r3.u32 = IO_STATUS_NAME_NOT_FOUND;
}

PPC_FUNC(__imp__NtWriteFile)
{
    KTrace("NtWriteFile");RollbackNoteIo(false, "NtWriteFile", &ctx);auto file=GetFile(ctx.r3.u32);
    if(!file){ctx.r3.u32=IO_STATUS_INVALID_HANDLE;return;}
    if(ctx.r5.u32){ctx.r3.u32=0xC0000002u;return;} // initiating-thread APC delivery unavailable
    const uint32_t ios=ctx.r7.u32,buffer=ctx.r8.u32,length=ctx.r9.u32,offsetPtr=ctx.r10.u32;
    if(!XamGuestRange(ios,8)||(length&&!XamGuestRange(buffer,length,1))||(offsetPtr&&!XamGuestRange(offsetPtr,8))) {ctx.r3.u32=IO_STATUS_INVALID_PARAMETER;return;}
    int64_t offset=-2;if(offsetPtr)offset=int64_t((uint64_t(GRead32(base,offsetPtr))<<32)|GRead32(base,offsetPtr+4));
    if(offset<0 && offset!=-1 && offset!=-2){ctx.r3.u32=IO_STATUS_INVALID_PARAMETER;return;}
    DWORD written=0;uint32_t status=0;
    {std::lock_guard fileLock(file->mutex);
     if(!file->writable || file->saveContainer.empty() || file->directory)status=0xC0000022u;
     else if(length) {
        LARGE_INTEGER original{},zero{},target{};
        if(!SetFilePointerEx(file->handle,zero,&original,FILE_CURRENT))status=FileErrorStatus(GetLastError());
        else {
            if(offset!=-2){target.QuadPart=offset==-1?0:offset;if(!SetFilePointerEx(file->handle,target,nullptr,offset==-1?FILE_END:FILE_BEGIN))status=FileErrorStatus(GetLastError());}
            if(!status && !WriteFile(file->handle,base+buffer,length,&written,nullptr))status=FileErrorStatus(GetLastError());
            if(status && offset!=-2)SetFilePointerEx(file->handle,original,nullptr,FILE_BEGIN);
        }
     }}
    GWrite32(base,ios,status);GWrite32(base,ios+4,written);
    if(ctx.r4.u32)KernelSignalEventHandle(ctx.r4.u32);ctx.r3.u32=status;
}
PPC_FUNC(__imp__NtWriteFileGather) { KTrace("NtWriteFileGather");ctx.r3.u32=0xC0000002u; }
PPC_FUNC(__imp__NtFlushBuffersFile)
{
    KTrace("NtFlushBuffersFile");auto file=GetFile(ctx.r3.u32);
    if(!file){ctx.r3.u32=IO_STATUS_INVALID_HANDLE;return;}
    uint32_t status=0;{std::lock_guard fileLock(file->mutex);if(file->writable&&!FlushFileBuffers(file->handle))status=FileErrorStatus(GetLastError());}
    if(ctx.r4.u32){GWrite32(base,ctx.r4.u32,status);GWrite32(base,ctx.r4.u32+4,0);}ctx.r3.u32=status;
}
PPC_FUNC(__imp__NtQueryDirectoryFile)
{
    KTrace("NtQueryDirectoryFile");auto file=GetFile(ctx.r3.u32);
    if(!file){ctx.r3.u32=IO_STATUS_INVALID_HANDLE;return;}
    const auto ios=ctx.r7.u32,out=ctx.r8.u32,bytes=ctx.r9.u32,patternPtr=ctx.r10.u32;
    if(ctx.r5.u32 || file->saveContainer.empty()){ctx.r3.u32=0xC0000002u;return;}
    if(!file->directory){ctx.r3.u32=0xC0000103u;return;}
    if(!XamGuestRange(ios,8)||!XamGuestRange(out,bytes,1)||!XamGuestRange(ctx.r1.u32,0x58)){ctx.r3.u32=IO_STATUS_INVALID_PARAMETER;return;}
    std::string pattern="*";
    if(patternPtr){
        if(!XamGuestRange(patternPtr,8)){ctx.r3.u32=IO_STATUS_INVALID_PARAMETER;return;}
        uint32_t len=GRead16(base,patternPtr),p=GRead32(base,patternPtr+4);
        if(len>42 || (len&&!XamGuestRange(p,len,1))){ctx.r3.u32=IO_STATUS_INVALID_PARAMETER;return;}
        pattern.assign(reinterpret_cast<const char*>(base+p),len);
    }
    const bool wildcard=pattern.empty()||pattern=="*"||pattern=="*.*";
    if(!wildcard&&pattern.find_first_of("*?\\/:")!=std::string::npos){ctx.r3.u32=0xC0000002u;return;}
    uint32_t status=0,returned=0;
    {std::lock_guard fileLock(file->mutex);
     if(GRead32(base,ctx.r1.u32+0x54)){file->directoryScanned=false;file->directoryCursor=0;file->directoryItems.clear();}
     if(!file->directoryScanned){
        std::error_code ec;
        file->directoryItems.clear();
        for(auto it=std::filesystem::directory_iterator(file->saveContainer,ec);!ec&&it!=std::filesystem::directory_iterator();it.increment(ec)) {
            const auto& d=*it;
            auto name=d.path().filename().string();if(name.empty()||name[0]=='.')continue;
            DWORD attr=GetFileAttributesW(d.path().c_str());
            if(attr==INVALID_FILE_ATTRIBUTES){status=FileErrorStatus(GetLastError());break;}
            if(attr&FILE_ATTRIBUTE_REPARSE_POINT){status=0xC0000022u;break;}
            if(attr&FILE_ATTRIBUTE_DIRECTORY)continue;
            if(wildcard || _stricmp(name.c_str(),pattern.c_str())==0){
                uint64_t size=d.file_size(ec);if(ec)break;file->directoryItems.push_back({name,size});
            }
        }
        if(ec)status=FileErrorStatus(uint32_t(ec.value()));
        if(!status){std::sort(file->directoryItems.begin(),file->directoryItems.end(),[](auto& a,auto& b){return a.name<b.name;});file->directoryScanned=true;}
     }
     if(!status){
        if(file->directoryCursor==file->directoryItems.size())status=0x80000006u;
        else {
            const auto& item=file->directoryItems[file->directoryCursor];uint32_t required=64+uint32_t(item.name.size());
            if(bytes<required)status=0xC0000023u;
            else {
                std::memset(base+out,0,required);GWrite32(base,out+4,uint32_t(file->directoryCursor));
                GWrite32(base,out+40,uint32_t(item.size>>32));GWrite32(base,out+44,uint32_t(item.size));
                uint64_t allocated=(item.size+4095)&~uint64_t(4095);GWrite32(base,out+48,uint32_t(allocated>>32));GWrite32(base,out+52,uint32_t(allocated));
                GWrite32(base,out+56,FILE_ATTRIBUTE_NORMAL);GWrite32(base,out+60,uint32_t(item.name.size()));
                std::memcpy(base+out+64,item.name.data(),item.name.size());returned=required;++file->directoryCursor;
            }
        }
     }}
    GWrite32(base,ios,status);GWrite32(base,ios+4,returned);if(ctx.r4.u32)KernelSignalEventHandle(ctx.r4.u32);ctx.r3.u32=status;
}
PPC_FUNC(__imp__NtQueryVolumeInformationFile)
{
    KTrace("NtQueryVolumeInformationFile");
    // r3=handle, r4=IoStatusBlock, r5=FsInformation OUT, r6=Length, r7=FsInformationClass
    uint32_t out = ctx.r5.u32, cls = ctx.r7.u32;
    bool handled = false;
    if (out)
    {
        switch (cls)
        {
        case 3: // FileFsSizeInformation: TotalAllocationUnits(8), AvailableAllocationUnits(8), SectorsPerAllocationUnit(4), BytesPerSector(4)
            GWrite32(base, out + 0, 0); GWrite32(base, out + 4, 0x00400000);
            GWrite32(base, out + 8, 0); GWrite32(base, out + 12, 0);
            GWrite32(base, out + 16, 1); GWrite32(base, out + 20, 2048);
            handled = true;
            break;
        case 4: // FileFsDeviceInformation: DeviceType(4), Characteristics(4)
            GWrite32(base, out + 0, 0x2 /*FILE_DEVICE_CD_ROM*/); GWrite32(base, out + 4, 0x1 /*REMOVABLE_MEDIA*/);
            handled = true;
            break;
        }
    }
    std::fprintf(stderr, "[io] qvol h=%08X class=%u len=%u%s\n", ctx.r3.u32, cls, ctx.r6.u32,
                 handled ? "" : " UNHANDLED");
    if (ctx.r4.u32) { GWrite32(base, ctx.r4.u32, STATUS_SUCCESS); GWrite32(base, ctx.r4.u32 + 4, ctx.r6.u32); }
    ctx.r3.u32 = STATUS_SUCCESS;
}
