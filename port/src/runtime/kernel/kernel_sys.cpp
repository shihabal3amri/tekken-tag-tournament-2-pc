// System-info, module (Xex), and time kernel imports.
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "guest_time.h"
#include "guest_heap.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// --- Xex module queries. The recompiler already resolved the game's own calls, so these serve
//     optional runtime probing; a fake module handle + "not found" for arbitrary procs is safe. ---
PPC_FUNC(__imp__XexCheckExecutablePrivilege)
{
    KTrace("XexCheckExecutablePrivilege"); ctx.r3.u32 = 0; }              // privilege not held
PPC_FUNC(__imp__XexGetModuleHandle)
{
    KTrace("XexGetModuleHandle"); if (ctx.r4.u32) GWrite32(base, ctx.r4.u32, 0x80000000); ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__XexGetModuleSection)
{
    KTrace("XexGetModuleSection"); ctx.r3.u32 = STATUS_UNSUCCESSFUL; }
// The game resolves system procs (e.g. xam.xex ordinal 36 = XNetLogonTaskContinue, a Live logon fn)
// and calls the result unconditionally. Offline we don't need those, but we must return a *callable*
// address. Hand out one registered no-op thunk placed in a free slot of the indirect-call table
// (that table only covers the code range, so the thunk address must live there too).
static void GuestProcNoop(PPCContext& ctx, uint8_t* /*base*/) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XexGetProcedureAddress)
{
    KTrace("XexGetProcedureAddress");
    static uint32_t s_thunk = 0;
    if (!s_thunk)
    {
        for (uint32_t a = PPC_CODE_BASE + (uint32_t)PPC_CODE_SIZE - 4; a > PPC_CODE_BASE; a -= 4)
        {
            PPCFunc*& slot = PPC_LOOKUP_FUNC(base, a);
            if (slot == nullptr || slot == g_unmappedReporter) { slot = GuestProcNoop; s_thunk = a; break; }
        }
    }
    if (ctx.r5.u32) GWrite32(base, ctx.r5.u32, s_thunk);
    ctx.r3.u32 = STATUS_SUCCESS;
}
PPC_FUNC(__imp__XexLoadImage)
{
    KTrace("XexLoadImage"); ctx.r3.u32 = STATUS_UNSUCCESSFUL; }
PPC_FUNC(__imp__XexLoadImageHeaders)
{
    KTrace("XexLoadImageHeaders"); ctx.r3.u32 = STATUS_UNSUCCESSFUL; }
PPC_FUNC(__imp__XexUnloadImage)
{
    KTrace("XexUnloadImage"); ctx.r3.u32 = STATUS_SUCCESS; }

// --- status / error mapping ---
PPC_FUNC(__imp__RtlNtStatusToDosError)
{
    KTrace("RtlNtStatusToDosError");
    // The file source distinguishes ERROR_HANDLE_EOF (38) from other failures.
    // NTSTATUS low bits are not Win32 error numbers: STATUS_END_OF_FILE's are 17.
    // Use Windows' documented mapping, including ERROR_MR_MID_NOT_FOUND for an
    // unknown status, rather than inventing a successful or unrelated error code.
    using StatusToDosError = ULONG (NTAPI*)(LONG);
    static const StatusToDosError convert = reinterpret_cast<StatusToDosError>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    ctx.r3.u32 = convert ? convert(static_cast<LONG>(ctx.r3.u32)) : ERROR_MR_MID_NOT_FOUND;
}

// --- memory statistics: fill the guest MM_STATISTICS struct with plausible values ---
PPC_FUNC(__imp__MmQueryStatistics)
{
    KTrace("MmQueryStatistics");
    uint32_t p = ctx.r3.u32;
    if (p)
    {
        // Zero the struct, then set a couple of size fields (total/available physical pages).
        for (uint32_t i = 0; i < 0x40; i += 4) GWrite32(base, p + i, 0);
        GWrite32(base, p + 0x00, 0x40);          // Length
        GWrite32(base, p + 0x04, 0x00020000);    // TotalPhysicalPages (~512 MiB / 4 KiB)
        GWrite32(base, p + 0x08, 0x00010000);    // AvailablePages
    }
    ctx.r3.u32 = STATUS_SUCCESS;
}

// --- time ---
// KeTimeStampBundle (xboxkrnl data export 0xAD): {u64 InterruptTime (100 ns since boot), u64
// SystemTime (FILETIME), u32 TickCount (ms since boot)}, kept current by the kernel. Game code
// reads it straight from memory through the import slot 0x82000720 (e.g. sub_82BEF480 = NACE's
// clock, which drives the online session search and its timeouts). The loader never patched
// the slot (it still held the ordinal 0xAD), so those clocks read 0 and never advanced
// (MEASURED 2026-10-12: the Player Match search waited forever on a 1 s timer). A host thread
// refreshes one guest cell every millisecond.
void KernelInstallTimeStampBundle(uint8_t* base)
{
    static std::atomic<bool> started{ false };
    if (started.exchange(true)) return;
    void* cell = g_virtualHeap.Alloc(0x18, 16);
    if (!cell) return;
    const uint32_t guest = uint32_t(static_cast<uint8_t*>(cell) - base);
    std::memset(cell, 0, 0x18);
    GWrite32(base, 0x82000720u, guest);
    std::thread([base, guest] {
        const auto boot = std::chrono::steady_clock::now();
        for (;;)
        {
            const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - boot).count());
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            const uint64_t system = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            const uint64_t interrupt = ns / 100;
            GWrite32(base, guest + 0, uint32_t(interrupt >> 32));
            GWrite32(base, guest + 4, uint32_t(interrupt));
            GWrite32(base, guest + 8, uint32_t(system >> 32));
            GWrite32(base, guest + 12, uint32_t(system));
            GWrite32(base, guest + 16, uint32_t(ns / 1000000));
            Sleep(1);
        }
    }).detach();
    std::fprintf(stderr, "[kernel] KeTimeStampBundle -> %08X (import slot 0x82000720)\n", guest);
}

PPC_FUNC(__imp__KeQuerySystemTime)
{
    KTrace("KeQuerySystemTime");
    // Xbox system time = 100 ns intervals since 1601 (same epoch as FILETIME).
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    uint32_t p = ctx.r3.u32;
    if (p) { GWrite32(base, p + 0, uint32_t(t >> 32)); GWrite32(base, p + 4, uint32_t(t)); }  // BE 64-bit
}
PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    KTrace("KeQueryPerformanceFrequency");
    // Return in r3; the guest's QueryPerformanceFrequency wrapper stores r3
    // directly as a 64-bit integer. r4 is not the low half of this result.
    ctx.r3.u64 = PPC_TIME_BASE_FREQUENCY;
}
