// TTT2 native port -- minimal runtime skeleton (Phase 1: reach a link; Phase 2: make it run).
// Reserves the 32-bit guest address space, loads the decrypted TU3-patched image, populates
// the indirect-call function table, and jumps to the guest entry point.
#include "ppc_recomp_shared.h"
#include "kernel/guest_heap.h"
#include "kernel/kernel_shared.h"
#include "kernel/phys_mirror.h"
#include "kernel/xma.h"
#include "kernel/steam_net.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <timeapi.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <filesystem>

// 4 GiB reserved so (base + any 32-bit guest address) is always valid.
// 4 GiB of guest space plus a 16 MiB zero tail: a garbage guest pointer near the top of the
// address space (measured: 0xFFFFFFFD, an error code used as a pointer once the menus began
// building) makes 4-byte reads that CROSS the 4 GiB edge. Real hardware wraps or reads kernel
// space and survives; with an exact-size reservation the host faults instead. The committed
// zero tail turns those into benign zero reads, the same philosophy as the committed null page.
static constexpr uint64_t GUEST_RESERVE = 0x100000000ull + 0x1000000ull;

// g_base is the shared guest memory base (declared in kernel_shared.h, defined in kernel_thread.cpp).

// --- Null-page guard: guest reads through (near-)null pointers must return zero (the
// game relies on it), but WRITES through null must not stick - polluted low memory flips
// dormant TU3 guards that read e.g. *(0x59) and call through garbage. Keep the first
// 64 KiB write-protected; when guest code writes there, let the instruction execute once
// (single-step) then erase the written bytes and re-protect. Each write is an upstream
// divergence worth studying (on hardware the object pointer would be non-null).
static thread_local bool t_npStep = false;
static thread_local uint64_t t_npAddr = 0;

static void NullPageProtect(bool on)
{
    DWORD old;
    VirtualProtect(g_base, 0x10000, on ? PAGE_READONLY : PAGE_READWRITE, &old);
}

static LONG CALLBACK NullPageVeh(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2
        && r->ExceptionInformation[0] == 1)
    {
        uint64_t fa = r->ExceptionInformation[1];
        uint64_t lo = reinterpret_cast<uint64_t>(g_base);
        if (fa >= lo && fa < lo + 0x10000)
        {
            static int logged = 0;
            if (logged < 20)
            {
                ++logged;
                uint64_t modBase = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
                std::fprintf(stderr, "[nullpage] write@0x%llX pc RVA=0x%llX (ignored)\n",
                    (unsigned long long)(fa - lo),
                    (unsigned long long)(reinterpret_cast<uint64_t>(r->ExceptionAddress) - modBase));
            }
            t_npAddr = fa;
            NullPageProtect(false);
            ep->ContextRecord->EFlags |= 0x100;
            t_npStep = true;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    else if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_npStep)
    {
        t_npStep = false;
        uint64_t a = t_npAddr & ~0xFull;
        std::memset(reinterpret_cast<void*>(a), 0, 32);          // erase the write
        NullPageProtect(true);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Suspend every other thread and print each one's RIP plus in-module stack return addresses
// (resolve offline via the linker map: tools/stackdump.ps1). `why` heads the dump.
void DumpAllStacks(const char* why)
{
    uint64_t modBase = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof(te) };
    // Capture with threads suspended, but PRINT only after resuming them: a suspended
    // thread may hold the CRT stderr lock and printing would deadlock the dump.
    struct Cap { DWORD tid; uint64_t rip; uint64_t rets[24]; int nret; };
    Cap caps[64]; int ncap = 0;
    for (BOOL ok = Thread32First(snap, &te); ok && ncap < 64; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (SuspendThread(h) != (DWORD)-1)
        {
            CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(h, &c))
            {
                Cap& cap = caps[ncap++];
                cap.tid = te.th32ThreadID;
                cap.rip = c.Rip - modBase;
                cap.nret = 0;
                for (int i = 0; i < 2048 && cap.nret < 24; ++i)
                {
                    uint64_t v = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(),
                        reinterpret_cast<void*>(c.Rsp + i * 8), &v, 8, nullptr)) break;
                    if (v >= modBase && v < modBase + 0x20000000ull)
                        cap.rets[cap.nret++] = v - modBase;
                }
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    extern void KernelDumpGuestThreads();
    KernelDumpGuestThreads();
    std::fprintf(stderr, "[stacks] ==== thread dump (%s) ====\n", why);
    for (int i = 0; i < ncap; ++i)
    {
        std::fprintf(stderr, "[stacks] tid=%lu rip=0x%llX", caps[i].tid,
                     (unsigned long long)caps[i].rip);
        for (int j = 0; j < caps[i].nret; ++j)
            std::fprintf(stderr, " 0x%llX", (unsigned long long)caps[i].rets[j]);
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "[stacks] ==== end dump ====\n");
}

// TTT2_STACKDUMP=<seconds>: after that delay, the dump above, then the process ends.
static void StackDumpAfter(int seconds)
{
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    DumpAllStacks("timer");
    LogDrain(); TerminateProcess(GetCurrentProcess(), 3);
}

// Called through the lookup table when guest code indirect-calls an address with no
// compiled function. Logs the target and acts as a no-op callee. g_unmappedReporter lets
// kernel code (XexGetProcedureAddress) recognize these slots as still-free.
static void UnmappedCallReporter(PPCContext& ctx, uint8_t* base)
{
    static const bool strict = std::getenv("TTT2_STRICT_INDIRECT") != nullptr;
    static std::atomic<unsigned> logged{0};
    if (strict || logged.fetch_add(1, std::memory_order_relaxed) < 100)
    {
        // The guest's call chain (back chain at r1, saved lr at frame - 8), for the first reports.
        char chain[160] = "";
        if (logged.load(std::memory_order_relaxed) <= 20)
        {
            int k = 0;
            uint32_t sp = ctx.r1.u32;
            for (int i = 0; i < 10 && sp && sp < 0xFFFFFFF0u && k < int(sizeof(chain)) - 10; ++i)
            {
                const uint32_t prev = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + sp));
                if (!prev || prev <= sp || prev - sp > 0x10000) break;
                k += std::snprintf(chain + k, sizeof(chain) - k, " %08X", __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + prev - 8)));
                sp = prev;
            }
        }
        std::fprintf(stderr, "[runtime] UNMAPPED indirect call: ctr=0x%08X lr=0x%08X r3=0x%08X r1=0x%08X%s%s\n",
            static_cast<uint32_t>(ctx.ctr.u32), static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r1.u32, *chain ? ", guest callers" : "", chain);
    }
    if (strict)
    {
        // Returning from a missing switch target can leave the guest's frame
        // allocated and corrupt every later call. Validation runs must stop at
        // this first divergence, rather than reporting a later corrupt pointer.
        std::fprintf(stderr, "[runtime] strict indirect-call gate failed; stopping before further guest execution\n");
        std::fflush(stderr);
        LogDrain(); TerminateProcess(GetCurrentProcess(), 5);
    }
}
PPCFunc* const g_unmappedReporter = UnmappedCallReporter;

// --- optional write-watch (env TTT2_WATCH=<hex guest addr>): protects the page and logs the
// PC of every write overlapping the watched word via a vectored handler + single-step. ---
// The address can also be armed while the game runs: write "<hex addr>" to
// C:/ttt2/port/watch.txt (consumed) - for buffers whose address is only known in that run.
static uint32_t g_watchAddr = 0;
static int g_watchLogged = 0;
static thread_local bool t_watchStep = false;    // this thread is stepping past the page
static thread_local uint32_t t_watchPre = 0;     // watched-word value before the write
static thread_local uint64_t t_watchPc  = 0;     // PC of the writing instruction
static thread_local uint64_t t_watchStack[12];   // its callers (return-address RVAs)
static thread_local int t_watchFrames = 0;

// Callers of the faulting instruction, unwound from its context (x64 unwind data).
static void WatchCaptureStack(const CONTEXT& at, uint64_t modBase)
{
    CONTEXT c = at;
    t_watchFrames = 0;
    for (int i = 0; i < 12; ++i)
    {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (f)
        {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, f, &c, &handlerData, &establisher, nullptr);
        }
        else
        {
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);   // leaf: return address on top
            c.Rsp += 8;
        }
        if (!c.Rip) break;
        t_watchStack[t_watchFrames++] = c.Rip - modBase;
    }
}

static void WatchProtect(bool on)
{
    DWORD old;
    VirtualProtect(g_base + (g_watchAddr & ~0xFFFu), 0x1000, on ? PAGE_READONLY : PAGE_READWRITE, &old);
}

static LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (!g_watchAddr) return EXCEPTION_CONTINUE_SEARCH;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2
        && r->ExceptionInformation[0] == 1)      // write fault
    {
        uint64_t fa = r->ExceptionInformation[1];
        uint64_t page = reinterpret_cast<uint64_t>(g_base) + (g_watchAddr & ~0xFFFu);
        if (fa >= page && fa < page + 0x1000)
        {
            t_watchPre = __builtin_bswap32(*reinterpret_cast<uint32_t*>(g_base + g_watchAddr));
            t_watchPc  = reinterpret_cast<uint64_t>(r->ExceptionAddress);
            WatchCaptureStack(*ep->ContextRecord, reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr)));
            WatchProtect(false);
            ep->ContextRecord->EFlags |= 0x100;  // TF: re-protect after this one instruction
            t_watchStep = true;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    else if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_watchStep)
    {
        t_watchStep = false;
        uint32_t now = __builtin_bswap32(*reinterpret_cast<uint32_t*>(g_base + g_watchAddr));
        if (now != t_watchPre && g_watchLogged < 300)
        {
            ++g_watchLogged;
            uint64_t modBase = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
            char stack[12 * 12 + 1] = {};
            int at = 0;
            for (int i = 0; i < t_watchFrames; ++i)
                at += std::snprintf(stack + at, sizeof(stack) - at, " %llX", (unsigned long long)t_watchStack[i]);
            std::fprintf(stderr, "[watch] 0x%08X: %08X -> %08X pc RVA=0x%llX callers:%s\n",
                g_watchAddr, t_watchPre, now, (unsigned long long)(t_watchPc - modBase), stack);
        }
        WatchProtect(true);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int FaultFilter(EXCEPTION_POINTERS* ep);

// Shared with kernel threads (GuestCall wrappers) so their faults get reported too.
int GuestFaultReport(EXCEPTION_POINTERS* ep) { return FaultFilter(ep); }

// Turn a crash into a diagnostic: report the faulting guest address (Phase-2 work-list).
static int FaultFilter(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
    {
        uint64_t fault = r->ExceptionInformation[1];
        long long guest = (long long)(fault - (uint64_t)g_base);
        std::fprintf(stderr, "[fault] access violation %s host=0x%llX guest=0x%llX pc=%p\n",
            r->ExceptionInformation[0] ? "write" : "read",
            (unsigned long long)fault, (unsigned long long)guest, r->ExceptionAddress);
    }
    else
    {
        std::fprintf(stderr, "[fault] exception 0x%lX pc=%p\n",
            (unsigned long)r->ExceptionCode, r->ExceptionAddress);
    }
    uint64_t modBase = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    std::fprintf(stderr, "[fault] module base=0x%llX  pc RVA=0x%llX\n",
        (unsigned long long)modBase, (unsigned long long)(reinterpret_cast<uint64_t>(r->ExceptionAddress) - modBase));
    // Optional fault-time guest dump: TTT2_DUMP=<hexaddr>,<hexlen> prints that guest region.
    if (const char* d = std::getenv("TTT2_DUMP"))
    {
        uint32_t da = 0, dl = 0x40;
        if (std::sscanf(d, "%x,%x", &da, &dl) >= 1 && da)
        {
            std::fprintf(stderr, "[dump] guest 0x%08X (%u bytes):\n", da, dl);
            for (uint32_t i = 0; i < dl; i += 16)
            {
                std::fprintf(stderr, "  %08X:", da + i);
                for (uint32_t j = 0; j < 16 && i + j < dl; ++j)
                    std::fprintf(stderr, " %02X", g_base[da + i + j]);
                std::fprintf(stderr, "  |");
                for (uint32_t j = 0; j < 16 && i + j < dl; ++j)
                {
                    unsigned char c = g_base[da + i + j];
                    std::fputc((c >= 0x20 && c < 0x7F) ? c : '.', stderr);
                }
                std::fprintf(stderr, "|\n");
            }
        }
    }
    // Host registers + code bytes at RIP: enough to identify the exact faulting expression.
    if (CONTEXT* c = ep->ContextRecord)
    {
        std::fprintf(stderr, "[fault] rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX\n", c->Rax, c->Rbx, c->Rcx, c->Rdx);
        std::fprintf(stderr, "[fault] rsi=%016llX rdi=%016llX rbp=%016llX rsp=%016llX\n", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
        std::fprintf(stderr, "[fault] r8 =%016llX r9 =%016llX r10=%016llX r11=%016llX\n", c->R8, c->R9, c->R10, c->R11);
        std::fprintf(stderr, "[fault] r12=%016llX r13=%016llX r14=%016llX r15=%016llX\n", c->R12, c->R13, c->R14, c->R15);
        // A wild jump makes ExceptionAddress itself unreadable; probe before dumping code bytes.
        const unsigned char* ip = static_cast<const unsigned char*>(r->ExceptionAddress);
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(ip, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            std::fprintf(stderr, "[fault] code:");
            for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %02X", ip[i]);
            std::fprintf(stderr, "\n");
        }
        else
        {
            std::fprintf(stderr, "[fault] code: <pc unreadable - wild jump>\n");
        }
    }
    // If we jumped to a null/wild address, the caller's return address is on top of the stack.
    if (ep->ContextRecord)
    {
        uint64_t rsp = ep->ContextRecord->Rsp;
        for (int i = 0; i < 8; ++i)   // scan a few slots for the first in-module return address
        {
            uint64_t v = *reinterpret_cast<uint64_t*>(rsp + i * 8);
            if (v >= modBase && v < modBase + 0x20000000ull)
            {
                std::fprintf(stderr, "[fault] caller ret[+%d]=0x%llX  RVA=0x%llX\n",
                    i * 8, (unsigned long long)v, (unsigned long long)(v - modBase));
                break;
            }
        }
    }
    LogDrain();
    return EXCEPTION_EXECUTE_HANDLER;
}

// INSTALLED COPY (ttt2_setup.exe, port/src/setup): the exe sits next to game\ (the files the
// installer extracted from the player's own disc and title update) and is started with no
// arguments. Everything the development launcher passes through the environment gets its
// player default here, unless already set: the game data, the hardware renderer, no debug frame
// dumps, and - with no console attached (GUI subsystem, double-clicked) - the log in
// %LOCALAPPDATA%\TTT2Recomp\log.txt. Returns the image path, or empty when not installed.
static std::string InstalledLayout(int argc)
{
    if (argc > 1) return {};
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    dir.resize(dir.find_last_of(L'\\') + 1);
    const std::wstring image = dir + L"game\\tekken.img";
    if (GetFileAttributesW(image.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    auto narrow = [](const std::wstring& w) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    };
    auto setDefault = [](const char* name, const std::string& value) {
        if (!std::getenv(name)) _putenv_s(name, value.c_str());
    };
    setDefault("TTT2_VFS_ROOT", narrow(dir + L"game"));
    setDefault("TTT2_GPU_HW", "1");
    setDefault("TTT2_FBDUMP", "0");
    // The log stays where it goes only for a writable file (a launcher's redirect, as "Start TTT2 with
    // test logging.bat" does); anything else - nothing, a console, a pipe, a handle value that is not
    // this process's file - gets log.txt. MEASURED 2026-10-20: a copy started from its shortcut on one
    // PC wrote no log at all since 2026-10-12 (the same build started from a script did).
    // MEASURED 2026-10-21: a game started elevated by the installer held log.txt open for writing yet never
    // truncated or wrote it - its standard error was already a handle to that file. A redirect to our own
    // log.txt is not one to keep, and log_start.txt (next to it) says what each start found.
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    const DWORD type = err && err != INVALID_HANDLE_VALUE ? GetFileType(err) : FILE_TYPE_UNKNOWN;
    wchar_t errPath[MAX_PATH] = L"";
    if (type == FILE_TYPE_DISK && !GetFinalPathNameByHandleW(err, errPath, MAX_PATH, 0)) errPath[0] = 0;
    const std::wstring errName(errPath);
    static const wchar_t kOwnLog[] = L"\\TTT2Recomp\\log.txt";
    const size_t ownLen = std::wcslen(kOwnLog);
    const bool ownLog = errName.size() >= ownLen && _wcsicmp(errName.c_str() + errName.size() - ownLen, kOwnLog) == 0;
    DWORD none = 0;
    const bool redirected = type == FILE_TYPE_DISK && !ownLog && WriteFile(err, "", 0, &none, nullptr);
    wchar_t local[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
    {
        const std::wstring folder = std::wstring(local) + L"\\TTT2Recomp";
        CreateDirectoryW(folder.c_str(), nullptr);
        bool reopened = false;
        // _wfreopen, not _wfreopen_s: the _s variant opens without sharing, so the log could
        // not be opened (or sent) while the game runs.
        if (!redirected) reopened = _wfreopen((folder + L"\\log.txt").c_str(), L"w", stderr) != nullptr;
        if (FILE* f = _wfopen((folder + L"\\log_start.txt").c_str(), L"w"))
        {
            std::fprintf(f, "standard error %p, type %lu, file \"%ls\"; %s\n", static_cast<void*>(err), type, errPath,
                         redirected ? "kept (a launcher's redirect)" : reopened ? "log.txt opened" : "log.txt could not be opened");
            std::fclose(f);
        }
        if (reopened)
            std::fprintf(stderr, "[runtime] log: %%LOCALAPPDATA%%\\TTT2Recomp\\log.txt (standard error was %s, type %lu)\n",
                         !err ? "none" : err == INVALID_HANDLE_VALUE ? "invalid" : ownLog ? "log.txt itself" : "another handle", type);
    }
    return narrow(image);
}

// The log goes through a pipe to a writer thread when stderr is a file (log.txt, or a
// launcher's redirect): a guest or GPU thread that logs only copies into the pipe, and a slow
// WriteFile (antivirus scan, busy disk) stalls only the writer - measured 2026-10-15: a 115 ms
// boot frame with the command processor inside fprintf -> WriteFile. TTT2_LOG_ASYNC=0: write
// directly, as before.
static HANDLE g_logPipeRead = nullptr;
static std::atomic<bool> g_logWriting{ false };

static void StartLogWriter()
{
    if (const char* e = std::getenv("TTT2_LOG_ASYNC"); e && *e == '0') return;
    const int fd = _fileno(stderr);
    const HANDLE file = fd >= 0 ? reinterpret_cast<HANDLE>(_get_osfhandle(fd)) : INVALID_HANDLE_VALUE;
    if (file == INVALID_HANDLE_VALUE || !file || GetFileType(file) != FILE_TYPE_DISK) return;
    HANDLE target = nullptr, read = nullptr, write = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &target, 0, FALSE, DUPLICATE_SAME_ACCESS)) return;
    if (!CreatePipe(&read, &write, nullptr, 1 << 20)) { CloseHandle(target); return; }
    const int pipeFd = _open_osfhandle(reinterpret_cast<intptr_t>(write), _O_WRONLY | _O_TEXT);
    if (pipeFd < 0 || _dup2(pipeFd, fd) != 0)
    {
        if (pipeFd >= 0) _close(pipeFd); else CloseHandle(write);
        CloseHandle(read);
        CloseHandle(target);
        return;
    }
    _close(pipeFd);                     // fd 2 keeps its own duplicate of the write end
    SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(fd)));
    g_logPipeRead = read;
    std::thread([read, target] {
        static char buffer[1 << 16];
        for (;;)
        {
            DWORD n = 0;
            if (!ReadFile(read, buffer, sizeof(buffer), &n, nullptr) || !n) return;
            g_logWriting = true;
            DWORD written = 0;
            WriteFile(target, buffer, n, &written, nullptr);
            g_logWriting = false;
        }
    }).detach();
}

void LogDrain(unsigned timeoutMs)
{
    if (!g_logPipeRead) return;
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    int quiet = 0;                      // two empty checks in a row: the writer has caught up
    while (quiet < 2 && GetTickCount64() < until)
    {
        DWORD queued = 0;
        PeekNamedPipe(g_logPipeRead, nullptr, 0, nullptr, &queued, nullptr);
        quiet = queued == 0 && !g_logWriting ? quiet + 1 : 0;
        Sleep(quiet ? 5 : 1);
    }
}

// Guest entry call isolated in its own frame (no C++ unwinding objects) for __try/__except.
static void RunGuest(PPCContext& ctx, uint8_t* base)
{
    __try { _xstart(ctx, base); std::fprintf(stderr, "[runtime] _xstart returned\n"); }
    __except (FaultFilter(GetExceptionInformation())) { std::fprintf(stderr, "[runtime] guest faulted\n"); }
}

// The game's main thread (the guest's _xstart), for per-frame CPU cost reports (gpu_hw_d3d11.h).
HANDLE g_mainThreadHandle = nullptr;

// Switchable-graphics laptops: the NVIDIA / AMD drivers run an exe that exports these on the dedicated
// GPU (MEASURED: a friend's PC ran the port on its integrated Intel UHD Graphics). gpu_adapter.h also
// picks the high-performance adapter itself.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

int main(int argc, char** argv)
{
    // The Steam helper process (kernel/steam_bridge.cpp): Steam lives there so Steam Input does
    // not hide the controllers from the game. Nothing of the game starts in it.
    if (argc >= 2 && std::strcmp(argv[1], "--steam-bridge") == 0) return steamnet::RunBridge(argc, argv);
    // --build-shaders [game folder] [game image] [threads]: the game's pixel shaders into the disk cache
    // ahead of time (the installer runs it; gpu_hw_d3d11.h PrebuildShaders). Default: the installed
    // layout next to the exe (game\data002.bin, data003.bin, tekken.img), else the development tree.
    if (argc >= 4 && std::strcmp(argv[1], "--translate") == 0)
    {
        extern int GpuTranslateFile(const char* kind, const char* path);
        return GpuTranslateFile(argv[2], argv[3]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--build-shaders") == 0)
    {
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::filesystem::path exeDir = std::filesystem::path(self).parent_path();
        std::filesystem::path game = argc > 2 && *argv[2] ? std::filesystem::path(argv[2]) : exeDir / "game";
        std::filesystem::path image = argc > 3 && *argv[3] ? std::filesystem::path(argv[3]) : game / "tekken.img";
        std::error_code ec;
        if ((argc <= 2 || !*argv[2]) && !std::filesystem::exists(game / "data002.bin", ec))
        {
            game = "C:/ttt2/re/game_files_x";
            image = "C:/ttt2/re/decrypted/Default_patched_tu3.bin";
        }
        const unsigned threads = argc > 4 ? unsigned(std::atoi(argv[4])) : 0u;
        extern int GpuPrebuildShaders(const std::vector<std::filesystem::path>& files, unsigned threads);
        return GpuPrebuildShaders({ game / "data002.bin", game / "data003.bin", image }, threads);
    }
    const std::string installedImage = InstalledLayout(argc);
    std::setvbuf(stderr, nullptr, _IONBF, 0);   // unbuffered: trace survives a hard crash
    StartLogWriter();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThreadHandle,
                    THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
    // An installed game whose shader prebuild is missing or from another build (the installer ran it
    // under another account, the cache was deleted, the port was updated): it runs again now, as a
    // separate low-priority process on half the cores, while the game plays as before (shaders it
    // meets first are built in game until then). Work already in the cache is skipped.
    extern bool GpuShaderPrebuildCurrent();
    if (!installedImage.empty() && !GpuShaderPrebuildCurrent())
    {
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const unsigned cores = std::thread::hardware_concurrency();
        std::wstring line = L"\"" + std::wstring(self) + L"\" --build-shaders \"\" \"\" " + std::to_wstring(cores > 3 ? cores / 2 : 1);
        STARTUPINFOW si = { sizeof si };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(self, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi))
        {
            std::fprintf(stderr, "[shaders] the shader prebuild is not current - building in the background (process %lu)\n", pi.dwProcessId);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    // Timer resolution. Since Windows 10 2004 a process that never calls timeBeginPeriod
    // gets the default ~15.6 ms timer even if other processes asked for more, so every
    // short runtime sleep (CP idle poll, WAIT_REG_MEM back-off, vblank sleep_until, guest
    // KeDelayExecutionThread) was quantised to 15.6 ms. Measured before this change
    // (PID71952): the main thread spun 73% of its time in the D3D GPU wait sub_8273D4A0
    // while the CP slept, and the game advanced ~16.7 ticks/s. Also opt out of Windows 11
    // power throttling, which may ignore the request for occluded windows.
    // TTT2_TIMER_RES=0 keeps the old default resolution for A/B measurement.
    if (const char* tr = std::getenv("TTT2_TIMER_RES"); !tr || std::strcmp(tr, "0") != 0)
    {
        PROCESS_POWER_THROTTLING_STATE throttling{};
        throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                                 PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        throttling.StateMask = 0;   // controlled + not set = never throttle
        const BOOL throttleOk = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                                                      &throttling, sizeof(throttling));
        const MMRESULT period = timeBeginPeriod(1);
        std::fprintf(stderr, "[runtime] timer resolution: timeBeginPeriod(1)=%s power-throttling-optout=%s\n",
                     period == TIMERR_NOERROR ? "ok" : "FAILED", throttleOk ? "ok" : "FAILED");
    }
    else
        std::fputs("[runtime] timer resolution: default (TTT2_TIMER_RES=0)\n", stderr);

    // 1) Reserve AND commit the whole guest address space up front (as Unleashed does).
    //    The game's init code reads through would-be-null pointers and relies on those reads
    //    returning zero (then takes a fallback path) rather than faulting. Committing everything
    //    to a zero-backed region makes those defensive paths work.
    // Last-resort logger: anything that slips past every SEH/VEH net still says WHERE it
    // died before the process goes (run_user1 vanished with an empty log).
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        auto* r = ep->ExceptionRecord;
        std::fprintf(stderr, "[fatal] unhandled exception %08lX at %p (tid %lu)\n",
                     r->ExceptionCode, r->ExceptionAddress, GetCurrentThreadId());
        std::fprintf(stderr, "[fatal] module base %p, RVA %llX\n", GetModuleHandleW(nullptr),
                     (unsigned long long)(reinterpret_cast<uint64_t>(r->ExceptionAddress) -
                                          reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr))));
        HMODULE mod = nullptr;
        wchar_t modName[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(r->ExceptionAddress), &mod) && mod)
            GetModuleFileNameW(mod, modName, MAX_PATH);
        std::fprintf(stderr, "[fatal] in %ls +%llX; access %s %p\n", modName,
                     (unsigned long long)(reinterpret_cast<uint64_t>(r->ExceptionAddress) - reinterpret_cast<uint64_t>(mod)),
                     r->NumberParameters >= 2 ? (r->ExceptionInformation[0] == 1 ? "write" : r->ExceptionInformation[0] == 8 ? "execute" : "read") : "?",
                     r->NumberParameters >= 2 ? reinterpret_cast<void*>(r->ExceptionInformation[1]) : nullptr);
        std::fflush(stderr);
        LogDrain();
        return EXCEPTION_EXECUTE_HANDLER;
    });

    // MEM_WRITE_WATCH lets the hardware GPU backend ask which guest pages the CPU wrote since it
    // last uploaded a texture (GetWriteWatch). It costs nothing until a range is reset; after a
    // reset only the first write to each page of that range takes a soft fault (~0.45 us).
    uint8_t* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, GUEST_RESERVE,
        MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
    if (!base) { std::fprintf(stderr, "guest alloc failed (%lu)\n", GetLastError()); return 1; }
    g_base = base;   // let kernel code (threads, waits) reach guest memory without a ctx arg
    // 0xC0000000-0xDFFFFFFF is the console's cached alias of physical memory; it is filled
    // on demand from the 0xA0000000 view the port allocates in (kernel/phys_mirror.cpp).
    PhysMirrorInit(base);

    // 3) Load the decrypted, TU3-patched PPC image at its base address.
    const char* imgPath = (argc > 1) ? argv[1]
                        : !installedImage.empty() ? installedImage.c_str()
                        : "C:/ttt2/re/decrypted/Default_patched_tu3.bin";
    std::ifstream img(imgPath, std::ios::binary);
    if (!img) { std::fprintf(stderr, "cannot open image: %s\n", imgPath); return 1; }
    img.read(reinterpret_cast<char*>(base + PPC_IMAGE_BASE), PPC_IMAGE_SIZE);
    std::fprintf(stderr, "[runtime] loaded %s (%llu bytes) at guest 0x%llX\n",
                 imgPath, (unsigned long long)img.gcount(), (unsigned long long)PPC_IMAGE_BASE);

    // 4) Keep host function pointers outside the guest address space. Guest writes
    // used to corrupt the table at IMAGE_BASE + IMAGE_SIZE during the title transition.
    static_assert((PPC_CODE_SIZE & 3) == 0);
    g_ppcFunctionTable = static_cast<PPCFunc**>(
        std::calloc(PPC_CODE_SIZE / 4, sizeof(PPCFunc*)));
    if (!g_ppcFunctionTable) {
        std::fprintf(stderr, "host function table allocation failed\n");
        return 1;
    }
    size_t mapped = 0;
    for (size_t i = 0; PPCFuncMappings[i].guest != 0; ++i) {
        const uint32_t target = static_cast<uint32_t>(PPCFuncMappings[i].guest);
        if (PPCFuncMappings[i].guest != target ||
            uint32_t(target - uint32_t(PPC_CODE_BASE)) >= PPC_CODE_SIZE || (target & 3)) {
            std::fprintf(stderr, "invalid guest function mapping: 0x%llX\n",
                         (unsigned long long)PPCFuncMappings[i].guest);
            return 1;
        }
        PPC_LOOKUP_FUNC(base, target) = PPCFuncMappings[i].host;
        ++mapped;
    }
    std::fprintf(stderr, "[runtime] mapped %zu guest functions\n", mapped);

    // 4b) Fill every EMPTY slot with a reporter: an indirect call to an unmapped guest
    // address then logs the target (ctr) + caller (lr) and returns as a no-op, instead of
    // jumping to null. Reported addresses go into tekken.toml's functions list (regen).
    size_t empty = 0;
    for (uint32_t a = PPC_CODE_BASE; a < PPC_CODE_BASE + (uint32_t)PPC_CODE_SIZE; a += 4)
    {
        PPCFunc*& slot = PPC_LOOKUP_FUNC(base, a);
        if (!slot) { slot = UnmappedCallReporter; ++empty; }
    }
    std::fprintf(stderr, "[runtime] %zu unmapped slots -> reporter\n", empty);

    // Init guest heaps (regions clear of the thread block, image, and function table).
    g_virtualHeap.Init(base, VHEAP_BEGIN, VHEAP_END);
    g_physicalHeap.Init(base, PHEAP_BEGIN, PHEAP_END);
    XmaInit(base);   // XMA context array + its register, read by the title's sound init

    // 5) Guest thread block: PCR + TLS + TEB + stack (layout ported from Unleashed GuestThreadContext).
    //    The entry code dereferences the thread pointer (r13 -> PCR); without this it faults at guest 0x193.
    constexpr uint32_t PCR_SIZE = 0xAB0, TLS_SIZE = 0x100, TEB_SIZE = 0x2E0, STACK_SIZE = 0x40000;
    constexpr uint32_t THREAD_TOTAL = PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE;
    const uint32_t threadGuest = 0x40000000;   // already committed (whole space is committed)
    uint8_t* thr = base + threadGuest;
    auto put32be = [](uint8_t* p, uint32_t v) { *reinterpret_cast<uint32_t*>(p) = __builtin_bswap32(v); };
    put32be(thr + 0x00,  threadGuest + PCR_SIZE);                     // PCR: TLS pointer
    put32be(thr + 0x100, threadGuest + PCR_SIZE + TLS_SIZE);         // PCR: TEB pointer
    thr[0x10C] = 0;                                                   // PCR: current CPU number
    put32be(thr + PCR_SIZE + 0x10, 0xFFFFFFFF);                      // TLS slot (quirk from Unleashed)
    put32be(thr + PCR_SIZE + TLS_SIZE + 0x14C, 1);                   // TEB: thread id

    PPCContext ctx{};
    ctx.r1.u64  = threadGuest + THREAD_TOTAL;   // stack pointer (grows down into STACK region)
    ctx.r13.u64 = threadGuest;                  // thread pointer -> PCR
    ctx.r3.u64  = 0;                            // entry argument (params.value)
    ctx.fpscr.loadFromHost();

    // The packet transport starts in the background now (Steam: a helper process, seconds to start):
    // a profile's sign-in asks for this PC's peer id (xlive.cpp LocalPeerId), which starts it if it
    // has not started yet, on the game's main thread. MEASURED: picking a profile froze the menus for
    // ~5 s (one 7.8 s stats window at 20 fps, no shader builds) while the helper started Steam.
    extern bool XSocketStartTransport();
    std::thread([] { XSocketStartTransport(); }).detach();

    std::fprintf(stderr, "[runtime] entering _xstart (base=%p, r13=0x%X, r1=0x%X)\n",
                 static_cast<void*>(base), (unsigned)ctx.r13.u32, (unsigned)ctx.r1.u32);
    g_base = base;
    extern void KernelRegisterGuestThread(uint32_t, uint32_t);
    extern void KernelPublishMainGuestContext(PPCContext*);
    KernelRegisterGuestThread(0xFFFFFFFF, 0);   // entry FFFFFFFF = the main (_xstart) thread

    AddVectoredExceptionHandler(1, NullPageVeh);
    NullPageProtect(true);

    if (const char* sd = std::getenv("TTT2_STACKDUMP"))
    {
        int secs = std::atoi(sd);
        if (secs > 0) std::thread(StackDumpAfter, secs).detach();
    }

    if (const char* w = std::getenv("TTT2_WATCH"))
    {
        g_watchAddr = static_cast<uint32_t>(std::strtoul(w, nullptr, 16));
        AddVectoredExceptionHandler(1, WatchVeh);
        WatchProtect(true);
        std::fprintf(stderr, "[watch] watching guest 0x%08X\n", g_watchAddr);
    }
    std::thread([] {
        bool vehInstalled = g_watchAddr != 0;
        // TTT2_INSTANCE=<tag>: watch<tag>.txt, so two instances can be armed separately.
        const char* tag = std::getenv("TTT2_INSTANCE");
        const std::string watchFile = std::string("C:/ttt2/port/watch") + (tag ? tag : "") + ".txt";
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            FILE* f = std::fopen(watchFile.c_str(), "rb");
            if (!f) continue;
            char text[32] = {};
            std::fread(text, 1, sizeof(text) - 1, f);
            std::fclose(f);
            DeleteFileA(watchFile.c_str());
            const uint32_t addr = static_cast<uint32_t>(std::strtoul(text, nullptr, 16));
            if (!addr) continue;
            if (g_watchAddr) WatchProtect(false);
            g_watchAddr = addr;
            g_watchLogged = 0;
            if (!vehInstalled) { AddVectoredExceptionHandler(1, WatchVeh); vehInstalled = true; }
            WatchProtect(true);
            std::fprintf(stderr, "[watch] watching guest 0x%08X (armed at run time)\n", addr);
        }
    }).detach();
    // Diagnostic gate: stop a corrupting write at its source, before it destroys
    // code or constants used by other guest threads. The first .rdata page holds
    // loader-patched imports, so leave that page writable. These ranges come from
    // the TU3 image's PE section table; the writable .data section starts at
    // 0x82DC0000. The normal fault logger identifies the writing host/guest code.
    if (std::getenv("TTT2_GUARD_IMAGE"))
    {
        DWORD oldProtect;
        if (!VirtualProtect(base + 0x82001000, 0x202000, PAGE_READONLY, &oldProtect) ||
            !VirtualProtect(base + PPC_CODE_BASE, PPC_CODE_SIZE, PAGE_READONLY, &oldProtect))
        {
            std::fprintf(stderr, "[guard] image protection failed (%lu)\n", GetLastError());
            return 1;
        }
        std::fprintf(stderr, "[guard] TU3 read-only data and code protected\n");
    }
    extern void KernelInstallTimeStampBundle(uint8_t*);
    KernelInstallTimeStampBundle(base);     // the KeTimeStampBundle data import (kernel_sys.cpp)
    KernelPublishMainGuestContext(&ctx);   // lets profile_threads.py walk the guest stack
    RunGuest(ctx, base);
    return 0;
}
