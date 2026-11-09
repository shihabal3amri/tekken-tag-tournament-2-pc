// Data watchpoints on guest memory (diagnostics): which recompiled function reads or writes a
// guest address. TTT2_DATAWATCH=1 starts a thread that polls C:/ttt2/port/datawatch.txt
// ("<guest address hex> [r|w] [<second address hex>]"; r = reads and writes, w = writes only); while
// it names an address, debug register DR0 (and DR1 for a second address) of every thread in the
// process watches those 4 bytes, and each
// hit's host instruction (an RVA inside a recompiled sub_XXXXXXXX: port/tools/rva2sym.py) and the
// guest LR of that thread are counted, printed once a second as "[datawatch]" lines.
#include <windows.h>
#include <tlhelp32.h>
#include "kernel_shared.h"
#include "log_utf8.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <xmmintrin.h>
#include <vector>
#include <functional>
#include <cstring>
#include <algorithm>

PPCContext* WatchGuestContextOf(DWORD tid);    // diag_hooks.cpp
extern std::atomic<uint64_t> g_audioFlushCalls[5];   // audio_flush.cpp

namespace {
std::atomic<uint64_t> g_watchHost{ 0 };
std::mutex g_hitsMutex;
std::map<std::pair<uint64_t, uint32_t>, uint32_t> g_hits;   // (host RVA | slot << 40, guest LR) -> count

LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    // A hit that came while Arm() set this thread's debug registers again (every 2 s) may carry no DR6 bit: still ours
    // while a watch is on (nothing else in the port single-steps) - passed on, it ended the game (2026-11-08).
    if (!(ep->ContextRecord->Dr6 & 3))
    {
        if (!g_watchHost.load()) return EXCEPTION_CONTINUE_SEARCH;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const uint64_t slot = (ep->ContextRecord->Dr6 & 2) ? 1 : 0;
    ep->ContextRecord->Dr6 = 0;
    const uint64_t rva = ep->ContextRecord->Rip - reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    PPCContext* c = WatchGuestContextOf(GetCurrentThreadId());
    const uint32_t lr = c ? uint32_t(c->lr) : 0;
    if (g_hitsMutex.try_lock())
    {
        ++g_hits[{ rva | (slot << 40), lr }];
        g_hitsMutex.unlock();
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void Arm(uint64_t host, bool writesOnly, uint64_t host2 = 0)
{
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != DWORD(-1))
        {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c))
            {
                c.Dr0 = host;
                c.Dr1 = host2;
                // L0 / L1 enable, RW = 01 write / 11 read-write, LEN = 11 (4 bytes)
                uint64_t d7 = c.Dr7 & ~0xFF000Full;
                const uint64_t rw = writesOnly ? 1ull : 3ull;
                if (host) d7 |= 1ull | (rw << 16) | (3ull << 18);
                if (host2) d7 |= 4ull | (rw << 20) | (3ull << 22);
                c.Dr7 = d7;
                SetThreadContext(t, &c);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
}

void Loop()
{
    AddVectoredExceptionHandler(1, WatchVeh);
    std::string last;
    bool writesOnly = false;
    auto lastArm = std::chrono::steady_clock::now();
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        char text[64] = {};
        if (FILE* f = std::fopen("C:/ttt2/port/datawatch.txt", "r")) { std::fgets(text, sizeof(text), f); std::fclose(f); }
        const std::string now = text;
        const auto t = std::chrono::steady_clock::now();
        if (now != last || (g_watchHost && t - lastArm > std::chrono::seconds(2)))
        {
            uint32_t guest = 0, guest2 = 0;
            char mode = 'r';
            const int n = std::sscanf(text, "%x %c %x", &guest, &mode, &guest2);
            writesOnly = n >= 2 && mode == 'w';
            const uint64_t host = (n >= 1 && guest) ? reinterpret_cast<uint64_t>(g_base) + guest : 0;
            const uint64_t host2 = (n >= 3 && guest2) ? reinterpret_cast<uint64_t>(g_base) + guest2 : 0;
            if (now != last)
                std::fprintf(stderr, "[datawatch] %s guest %08X (%s)\n", host ? "watching" : "stopped", guest, writesOnly ? "writes" : "reads+writes");
            // a stop clears the host only once every thread's debug registers are clear: a hit that lands while Arm
            // rewrites them (no DR6 B0/B1) is still this watch's (WatchVeh)
            if (host) g_watchHost = host;
            Arm(host, writesOnly, host2);           // again every 2 s: threads started since
            if (!host) g_watchHost = 0;
            last = now;
            lastArm = t;
        }
        std::map<std::pair<uint64_t, uint32_t>, uint32_t> hits;
        {
            std::lock_guard l(g_hitsMutex);
            hits.swap(g_hits);
        }
        for (const auto& [k, n] : hits)
            std::fprintf(stderr, "[datawatch] hit%s rva %llX guest lr %08X x%u\n", (k.first >> 40) ? " (2nd)" : "",
                         (unsigned long long)(k.first & 0xFFFFFFFFFFull), k.second, n);
    }
}

struct Start
{
    Start()
    {
        const char* e = std::getenv("TTT2_DATAWATCH");
        if (e && *e == '1') std::thread(Loop).detach();
    }
} g_start;
} // namespace

// TTT2_FP_TRAP=<guest thread entry hex> (diagnostics): count every SSE operation of that guest thread that
// reads a denormal or makes an underflowing result. The thread runs with those two exceptions unmasked; each
// trap is counted by host instruction, then the instruction is stepped once with them masked. Every 10 s:
// "[fptrap]" totals and the busiest instructions (RVAs: port/tools/rva2sym.py, or module+offset).
namespace {
constexpr unsigned kFpUnmask = 0x0100u | 0x0800u;   // DM (denormal operand) and UM (underflow) masks
thread_local bool t_fpStepping = false;
thread_local unsigned t_fpArmed = 0;
std::atomic<uint64_t> g_fpDe{ 0 }, g_fpUe{ 0 }, g_fpArms{ 0 };
struct FpSite { std::atomic<uint64_t> rip{ 0 }; std::atomic<uint64_t> n{ 0 }; };
FpSite g_fpSites[4096];

void FpCount(uint64_t rip)
{
    const size_t h = (rip * 0x9E3779B97F4A7C15ull) >> 52;
    for (size_t i = 0; i < 64; ++i)
    {
        FpSite& s = g_fpSites[(h + i) & 4095];
        uint64_t cur = s.rip.load(std::memory_order_relaxed);
        if (cur == 0 && s.rip.compare_exchange_strong(cur, rip)) cur = rip;
        if (cur == rip) { ++s.n; return; }
    }
}

LONG CALLBACK FpTrapVeh(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT* c = ep->ContextRecord;
    // Windows names an SSE trap after the first sticky flag it finds, stale ones included.
    if (code == STATUS_FLOAT_MULTIPLE_TRAPS || code == STATUS_FLOAT_MULTIPLE_FAULTS ||
        (code >= STATUS_FLOAT_DENORMAL_OPERAND && code <= STATUS_FLOAT_UNDERFLOW))
    {
        if (!t_fpArmed || t_fpStepping) return EXCEPTION_CONTINUE_SEARCH;
        if (c->MxCsr & 0x02) ++g_fpDe;
        if (c->MxCsr & 0x10) ++g_fpUe;
        FpCount(c->Rip);
        c->MxCsr = (c->MxCsr | kFpUnmask) & ~0x3Fu;
        c->FltSave.MxCsr = c->MxCsr;
        c->EFlags |= 0x100;
        t_fpStepping = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && t_fpStepping)
    {
        t_fpStepping = false;
        c->MxCsr = (c->MxCsr & ~kFpUnmask) & ~0x3Fu;
        c->FltSave.MxCsr = c->MxCsr;
        c->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void FpTrapReport()
{
    const uint64_t exe = uint64_t(GetModuleHandleW(nullptr));
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::vector<std::pair<uint64_t, uint64_t>> top;
        for (FpSite& s : g_fpSites)
            if (const uint64_t n = s.n.exchange(0)) top.emplace_back(n, s.rip.load());
        std::sort(top.begin(), top.end(), std::greater<>());
        std::fprintf(stderr, "[fptrap] 10 s: denormal reads %llu, underflows %llu, waits %llu; flushed filter calls %llu %llu %llu %llu; busiest:",
                     (unsigned long long)g_fpDe.exchange(0), (unsigned long long)g_fpUe.exchange(0),
                     (unsigned long long)g_fpArms.exchange(0),
                     (unsigned long long)g_audioFlushCalls[0].exchange(0), (unsigned long long)g_audioFlushCalls[1].exchange(0),
                     (unsigned long long)g_audioFlushCalls[2].exchange(0), (unsigned long long)g_audioFlushCalls[3].exchange(0));
        for (size_t i = 0; i < top.size() && i < 40; ++i)
        {
            HMODULE mod = nullptr;
            wchar_t name[MAX_PATH] = L"?";   // printed as UTF-8 (log_utf8.h)
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(top[i].second), &mod);
            if (uint64_t(mod) == exe) std::fprintf(stderr, " rva %llX x%llu;", (unsigned long long)(top[i].second - exe),
                                                   (unsigned long long)top[i].first);
            else
            {
                if (mod) GetModuleFileNameW(mod, name, MAX_PATH);
                const wchar_t* slash = std::wcsrchr(name, L'\\');
                std::fprintf(stderr, " %s+%llX x%llu;", LogUtf8(slash ? slash + 1 : name).c_str(),
                             (unsigned long long)(top[i].second - uint64_t(mod)), (unsigned long long)top[i].first);
            }
        }
        std::fprintf(stderr, "\n");
    }
}
}

// Called by a guest thread when it waits (kernel_objects.cpp, TTT2_SYNC_STATS=1): arms the thread named by
// TTT2_FP_TRAP, and keeps its context's FP control word unmasked so the guest's own MXCSR loads keep it.
void FpTrapArm(uint32_t entry, PPCContext* ctx)
{
    static const uint32_t target = [] {
        const char* e = std::getenv("TTT2_FP_TRAP");
        if (!e || !*e) return 0u;
        AddVectoredExceptionHandler(1, FpTrapVeh);
        std::thread(FpTrapReport).detach();
        return uint32_t(std::strtoul(e, nullptr, 16));
    }();
    if (!target || entry != target) return;
    ++g_fpArms;
    t_fpArmed = 1;
    if (ctx) ctx->fpscr.csr &= ~kFpUnmask;
    _mm_setcsr(_mm_getcsr() & ~kFpUnmask & ~0x3Fu);
}
