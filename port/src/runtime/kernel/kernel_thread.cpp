// Guest threads: ExCreateThread spawns a real host thread that sets up its own PCR/TLS/TEB/stack
// and runs the guest start routine. Layout ported from Unleashed's GuestThreadContext.
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "guest_heap.h"
#include "kernel_objects.h"
#include <thread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
#include <process.h>
#include <cstddef>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint8_t* g_base = nullptr;

// Registry of guest threads (host tid -> guest entry fn) so thread dumps can name them.
namespace {
std::mutex g_gtMutex;
struct GuestThreadRec { unsigned long tid; uint32_t entry, ctxArg; };
std::vector<GuestThreadRec> g_guestThreads;
}
void HitchRegisterThread(const char* name);   // diag_hooks.cpp (TTT2_HITCH_PROFILE=3)
void KernelRegisterGuestThread(uint32_t entry, uint32_t ctxArg)
{
    {
        std::lock_guard lk(g_gtMutex);
        g_guestThreads.push_back({ GetCurrentThreadId(), entry, ctxArg });
    }
    char name[32];
    std::snprintf(name, sizeof(name), "guest %08X", entry);
    HitchRegisterThread(name);
}
void KernelDumpGuestThreads()
{
    std::lock_guard lk(g_gtMutex);
    for (auto& t : g_guestThreads)
        std::fprintf(stderr, "[threads] tid=%lu entry=%08X ctx=%08X\n", t.tid, t.entry, t.ctxArg);
}

// Read-only diagnostic registry for out-of-process samplers (port/tools/profile_threads.py):
// host thread id -> the PPCContext of the guest call currently running on that thread, so a
// sampler can read guest r1/lr while the thread is suspended and walk the real PPC backchain
// (host stack scans only find stale return addresses). Writers are the owning threads; slots
// are claimed once per host thread and never released. The header records the PPCContext
// field offsets so the reader does not depend on this build's register-config macros.
struct GuestContextSlot { uint32_t tid; uint32_t entry; PPCContext* ctx; };
static constexpr uint32_t GuestContextCapacity = 512;
extern "C" struct GuestContextRegistry
{
    uint32_t magic, version, r1Offset, lrOffset, capacity;
    std::atomic<uint32_t> count;
    GuestContextSlot slots[GuestContextCapacity];
} g_guestContextRegistry = { 0x47435458u /* 'XTCG' */, 1, offsetof(PPCContext, r1),
                             offsetof(PPCContext, lr), GuestContextCapacity, {0}, {} };

namespace {
thread_local GuestContextSlot* t_contextSlot = nullptr;
// Publish ctx as this thread's active guest context; returns the previous one for nesting.
PPCContext* PublishGuestContext(PPCContext* ctx, uint32_t entry)
{
    if (!t_contextSlot)
    {
        const uint32_t index = g_guestContextRegistry.count.fetch_add(1);
        if (index >= GuestContextCapacity) return nullptr;
        t_contextSlot = &g_guestContextRegistry.slots[index];
        t_contextSlot->tid = GetCurrentThreadId();
    }
    PPCContext* previous = t_contextSlot->ctx;
    t_contextSlot->entry = entry;
    t_contextSlot->ctx = ctx;
    return previous;
}
struct ScopedGuestContext
{
    PPCContext* previous;
    uint32_t previousEntry;
    ScopedGuestContext(PPCContext* ctx, uint32_t entry)
        : previousEntry(t_contextSlot ? t_contextSlot->entry : 0)
    { previous = PublishGuestContext(ctx, entry); }
    ~ScopedGuestContext()
    {
        if (t_contextSlot) { t_contextSlot->ctx = previous; t_contextSlot->entry = previousEntry; }
    }
};
}
void KernelPublishMainGuestContext(PPCContext* ctx) { PublishGuestContext(ctx, 0xFFFFFFFFu); }

// TTT2_WTRACE=1: log the guest caller (lr) of wait/sleep primitives to find spin loops.
bool WTrace() { static const bool on = std::getenv("TTT2_WTRACE") != nullptr; return on; }

namespace {
constexpr uint32_t PCR_SIZE = 0xAB0, TLS_SIZE = 0x100, TEB_SIZE = 0x2E0, STACK_SIZE = 0x40000;
constexpr uint32_t TOTAL = PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE;
std::atomic<uint32_t> g_threadIdCounter{2};   // main thread is 1

void put32be(uint8_t* p, uint32_t v) { *reinterpret_cast<uint32_t*>(p) = __builtin_bswap32(v); }

std::mutex g_blocksMutex;
std::vector<std::pair<uint32_t, uint32_t>> g_blocks;   // guest base, size of every thread block

// Set up a fresh guest thread block, returning its guest base and stack-top.
uint32_t SetupThreadBlock(uint32_t threadId, uint32_t& stackTopOut)
{
    void* host = g_virtualHeap.Alloc(TOTAL, 0x1000);
    uint8_t* thr = static_cast<uint8_t*>(host);
    std::memset(thr, 0, TOTAL);
    uint32_t g = GuestAddr(g_base, host);
    {
        std::lock_guard l(g_blocksMutex);
        g_blocks.push_back({ g, TOTAL });
    }
    put32be(thr + 0x00,  g + PCR_SIZE);
    put32be(thr + 0x100, g + PCR_SIZE + TLS_SIZE);
    put32be(thr + PCR_SIZE + 0x10, 0xFFFFFFFF);
    put32be(thr + PCR_SIZE + TLS_SIZE + 0x14C, threadId);
    stackTopOut = g + TOTAL;
    return g;
}
} // namespace

// The guest thread blocks (PCR / TLS / TEB / stack) of every host thread that ran guest code; the
// main thread's block at 0x40000000 is not in the list. rollback.cpp keeps them out of its state.
void GuestThreadBlocks(std::vector<std::pair<uint32_t, uint32_t>>& out)
{
    std::lock_guard l(g_blocksMutex);
    out = g_blocks;
}

// Launch a guest routine on the current host thread. The guest thread block (PCR/TLS/stack)
// is created once per host thread and reused, so periodic callbacks (vblank) don't leak.
namespace {
thread_local uint32_t t_pcr = 0, t_stackTop = 0;
void EnsureThreadBlock()
{
    if (!t_pcr)
    {
        t_pcr = SetupThreadBlock(g_threadIdCounter++, t_stackTop);
        KernelBindCurrentThreadCpu(g_base + t_pcr + 0x10C);
    }
}
} // namespace

// Set the current host thread's guest PCR "current processor" byte (KPCR+0x10C). The D3D
// interrupt DPC services only the CURRENT cpu's fence wait block, so the vblank dispatcher
// rotates this across all 6 cores per tick.
void GuestSetCurrentCpu(uint8_t cpu)
{
    EnsureThreadBlock();
    g_base[t_pcr + 0x10C] = cpu;
}

static void GuestInvoke(PPCFunc* fn, PPCContext& ctx);
void RollbackNoteForeignThread();   // rollback.cpp: callbacks run on their own timeline

// Run a guest routine on the current host thread (a guest thread's start routine, or a callback).
static void GuestRun2(uint32_t guestAddr, uint32_t arg0, uint32_t arg1)
{
    EnsureThreadBlock();
    PPCContext ctx{};
    ctx.r1.u64 = t_stackTop;
    ctx.r13.u64 = t_pcr;
    ctx.r3.u64 = arg0;
    ctx.r4.u64 = arg1;
    ctx.ctr.u64 = guestAddr; // Match an indirect entry and identify missing callbacks in reports.
    ctx.fpscr.loadFromHost();
    PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, guestAddr);
    ScopedGuestContext published(&ctx, guestAddr);
    if (fn) GuestInvoke(fn, ctx);
}

// A callback from one of the runtime's own threads (DPC, interrupt, audio): not the frame's timeline.
void GuestCall2(uint32_t guestAddr, uint32_t arg0, uint32_t arg1)
{
    RollbackNoteForeignThread();
    GuestRun2(guestAddr, arg0, arg1);
}

// SEH-guarded invoke: a fault in a guest callback must be REPORTED, not silently kill
// the process (unhandled exceptions in secondary threads terminate with no output).
static void GuestInvoke(PPCFunc* fn, PPCContext& ctx)
{
    __try { fn(ctx, g_base); }
    __except (GuestFaultReport(GetExceptionInformation()))
    {
        std::fprintf(stderr, "[runtime] guest callback faulted (thread continues)\n");
    }
}

void GuestCall4(uint32_t guestAddr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    RollbackNoteForeignThread();
    EnsureThreadBlock();
    PPCContext ctx{};
    ctx.r1.u64 = t_stackTop;
    ctx.r13.u64 = t_pcr;
    ctx.r3.u64 = a0;
    ctx.r4.u64 = a1;
    ctx.r5.u64 = a2;
    ctx.r6.u64 = a3;
    ctx.ctr.u64 = guestAddr;
    ctx.fpscr.loadFromHost();
    PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, guestAddr);
    ScopedGuestContext published(&ctx, guestAddr);
    if (fn) GuestInvoke(fn, ctx);
}

void GpuProgress();     // kernel_video.cpp: wakes waiters on GPU-side writes

// --- DPC dispatch: KeInsertQueueDpc queues {routine, dpc, context, args} to a dedicated
// dispatcher thread, mimicking deferred execution at "interrupt time". ---
namespace {
struct DpcItem { uint32_t routine, dpc, context, arg1, arg2; };
std::mutex g_dpcMutex;
std::condition_variable g_dpcCv;
std::deque<DpcItem> g_dpcQueue;
std::atomic<bool> g_dpcThreadStarted{false};

void DpcThread()
{
    for (;;)
    {
        DpcItem item;
        {
            std::unique_lock lk(g_dpcMutex);
            g_dpcCv.wait(lk, [] { return !g_dpcQueue.empty(); });
            item = g_dpcQueue.front();
            g_dpcQueue.pop_front();
        }
        GuestCall4(item.routine, item.dpc, item.context, item.arg1, item.arg2);
        GpuProgress();          // the swap DPC writes the scratch tokens the GPU waits on
    }
}
} // namespace

// Queue an arbitrary guest call to the DPC dispatcher thread (used for I/O APCs: they
// must not run inline on the issuing thread, whose guest stack is in use mid-syscall).
void KernelQueueGuestCall(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    if (!fn) return;
    if (!g_dpcThreadStarted.exchange(true))
    {
        uintptr_t h = _beginthreadex(nullptr, 8 * 1024 * 1024,
            [](void*) -> unsigned { DpcThread(); return 0; }, nullptr, 0, nullptr);
        if (h) CloseHandle(reinterpret_cast<HANDLE>(h));
    }
    {
        std::lock_guard lk(g_dpcMutex);
        g_dpcQueue.push_back({fn, a0, a1, a2, a3});
    }
    g_dpcCv.notify_one();
}

// KeInitializeDpc(r3=KDPC, r4=DeferredRoutine, r5=DeferredContext)
PPC_FUNC(__imp__KeInitializeDpc)
{
    KTrace("KeInitializeDpc");
    GWrite32(base, ctx.r3.u32 + 16, ctx.r4.u32);   // routine
    GWrite32(base, ctx.r3.u32 + 20, ctx.r5.u32);   // context
}

// KeInsertQueueDpc(r3=KDPC, r4=SystemArgument1, r5=SystemArgument2)
PPC_FUNC(__imp__KeInsertQueueDpc)
{
    KTrace("KeInsertQueueDpc");
    uint32_t routine = GRead32(base, ctx.r3.u32 + 16);
    uint32_t context = GRead32(base, ctx.r3.u32 + 20);
    if (routine)
    {
        if (!g_dpcThreadStarted.exchange(true))
        {
            // 8 MB stack: DPC routines run deep guest chains.
            uintptr_t h = _beginthreadex(nullptr, 8 * 1024 * 1024,
                [](void*) -> unsigned { DpcThread(); return 0; }, nullptr, 0, nullptr);
            if (h) CloseHandle(reinterpret_cast<HANDLE>(h));
        }
        {
            std::lock_guard lk(g_dpcMutex);
            g_dpcQueue.push_back({routine, ctx.r3.u32, context, ctx.r4.u32, ctx.r5.u32});
        }
        g_dpcCv.notify_one();
    }
    ctx.r3.u32 = 1;                                // was not already queued
}

PPC_FUNC(__imp__KeRemoveQueueDpc)
{
    KTrace("KeRemoveQueueDpc"); ctx.r3.u32 = 0; }

void GuestCall(uint32_t guestAddr, uint32_t arg)
{
    GuestCall2(guestAddr, arg, 0);
}

// ExCreateThread(r3=&handle, r4=stackSize, r5=&threadId, r6=xApiStartup, r7=startAddress,
//                r8=startContext, r9=creationFlags)
PPC_FUNC(__imp__ExCreateThread)
{
    KTrace("ExCreateThread");
    uint32_t handlePtr = ctx.r3.u32;
    uint32_t threadIdPtr = ctx.r5.u32;
    uint32_t startAddress = ctx.r7.u32;
    uint32_t startContext = ctx.r8.u32;
    uint32_t creationFlags = ctx.r9.u32;

    uint32_t threadId = g_threadIdCounter.load();
    auto* obj = KernelCreateThreadObject();          // waitable thread object (handle table)

    // creationFlags bit0 = created suspended. For bring-up we start immediately if not suspended.
    bool suspended = (creationFlags & 1) != 0;
    KernelStartHostThread(obj, [startAddress, startContext]() {
        KernelRegisterGuestThread(startAddress, startContext);
        GuestRun2(startAddress, startContext, 0);
    }, suspended);

    GWrite32(base, handlePtr, KernelObjectHandle(obj));
    if (threadIdPtr) GWrite32(base, threadIdPtr, threadId);
    ctx.r3.u32 = STATUS_SUCCESS;
}

PPC_FUNC(__imp__ExTerminateThread)        { KTrace("ExTerminateThread"); ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__NtResumeThread)
{
    KTrace("NtResumeThread");
    uint32_t previous = 0;
    const auto status = KernelResumeThread(ctx.r3.u32, &previous);
    if (status == STATUS_SUCCESS && ctx.r4.u32) GWrite32(base, ctx.r4.u32, previous);
    ctx.r3.u32 = status;
}
PPC_FUNC(__imp__NtSuspendThread)
{
    KTrace("NtSuspendThread");
    if (WTrace()) std::fprintf(stderr, "[w] NtSuspendThread h=%08X lr=%08X\n", ctx.r3.u32, (uint32_t)ctx.lr);
    uint32_t previous = 0;
    const auto status = KernelSuspendThread(ctx.r3.u32, &previous);
    if (status == STATUS_SUCCESS && ctx.r4.u32) GWrite32(base, ctx.r4.u32, previous);
    ctx.r3.u32 = status;
}
PPC_FUNC(__imp__KeSetBasePriorityThread)
{
    KTrace("KeSetBasePriorityThread");
    if (ctx.r13.u32) KernelBindCurrentThreadCpu(base + ctx.r13.u32 + 0x10C);
    ctx.r3.u32 = static_cast<uint32_t>(KernelSetThreadPriority(ctx.r3.u32, ctx.r4.s32));
}
PPC_FUNC(__imp__KeSetAffinityThread)
{
    KTrace("KeSetAffinityThread");
    if (ctx.r13.u32) KernelBindCurrentThreadCpu(base + ctx.r13.u32 + 0x10C);
    uint32_t previous = 0;
    const uint32_t status = KernelSetThreadAffinity(ctx.r3.u32, ctx.r4.u32, previous);
    // Unlike Windows KeSetAffinityThread, Xbox returns status in r3 and the old
    // mask through r5. The guest XSetThreadProcessor wrapper reads this output.
    if (status == STATUS_SUCCESS && ctx.r5.u32) GWrite32(base, ctx.r5.u32, previous);
    ctx.r3.u32 = status;
}
PPC_FUNC(__imp__KeQueryBasePriorityThread)
{
    KTrace("KeQueryBasePriorityThread");
    ctx.r3.u32 = static_cast<uint32_t>(KernelQueryThreadPriority(ctx.r3.u32));
}
PPC_FUNC(__imp__NtYieldExecution)         { std::this_thread::yield(); }
// A relative guest delay of `ticks` 100 ns units. TTT2's job threads poll with 1 ms sleeps
// (sub_824F5458: ten polls, then Sleep(1) per poll while idle) and the main thread waits on
// those jobs several times a frame (sub_824F58B8), so the wake-up latency lands in the frame
// time. std::this_thread::sleep_for(1 ms) lasts 1-2 ms on Windows even at timeBeginPeriod(1);
// a high-resolution waitable timer (Windows 10 1803+) keeps the requested length, including
// sub-millisecond ones. TTT2_SLEEP_HIRES=0 restores sleep_for.
static void GuestDelay(int64_t ticks)
{
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    static const bool hires = [] { const char* e = std::getenv("TTT2_SLEEP_HIRES"); return !e || std::strcmp(e, "0") != 0; }();
    // TTT2_SLEEP_SCALE=<percent> shortens delays of 2 ms or less (poll-loop sleeps; experiment).
    static const int64_t scale = [] { const char* e = std::getenv("TTT2_SLEEP_SCALE"); return e ? (std::max<int64_t>)(1, std::atoll(e)) : 100; }();
    if (scale != 100 && ticks <= 20000) ticks = (std::max<int64_t>)(1, ticks * scale / 100);
    if (hires)
    {
        static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                                   TIMER_ALL_ACCESS);
        LARGE_INTEGER due;
        due.QuadPart = -ticks;
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    std::this_thread::sleep_for(std::chrono::microseconds((ticks + 9) / 10));
}

// (WaitMode, Alertable, Interval*) - Interval is 100ns units, negative = relative delay.
PPC_FUNC(__imp__KeDelayExecutionThread)
{
    KTrace("KeDelayExecutionThread");
    if (WTrace()) std::fprintf(stderr, "[w] KeDelay lr=%08X\n", (uint32_t)ctx.lr);
    int64_t ticks = 10000;                        // 1 ms
    if (uint32_t p = ctx.r5.u32)
    {
        int64_t t = (int64_t)(((uint64_t)GRead32(g_base, p) << 32) | GRead32(g_base, p + 4));
        ticks = t < 0 ? -t : 0;
        if (ticks > 500000) ticks = 500000;       // 50 ms: keep watchdog-style long sleeps responsive
    }
    if (ticks <= 0) std::this_thread::yield();
    else GuestDelay(ticks);
    // A thread sleeping over and over from the same call site is a poll loop -
    // print it once per streak with the guest backchain to identify the poller.
    {
        static thread_local uint32_t sleepLr = 0, sleepCnt = 0;
        uint32_t lr = (uint32_t)ctx.lr;
        if (lr == sleepLr)
        {
            if (++sleepCnt == 2000)
            {
                char line[512];
                int n = std::snprintf(line, sizeof(line), "[pollsleep] lr=%08X x2000 bt:", lr);
                uint32_t frame = ctx.r1.u32;
                for (int i = 0; i < 12 && frame && frame < 0xFFFFFFF0u; ++i)
                {
                    uint32_t prev = __builtin_bswap32(*reinterpret_cast<uint32_t*>(g_base + frame));
                    if (!prev || prev <= frame || prev - frame > 0x10000) break;
                    uint32_t savedLr = __builtin_bswap32(*reinterpret_cast<uint32_t*>(g_base + prev - 8));
                    n += std::snprintf(line + n, sizeof(line) - n, " %08X", savedLr);
                    frame = prev;
                }
                std::snprintf(line + n, sizeof(line) - n, "\n");
                std::fputs(line, stderr);
                sleepCnt = 0;
            }
        }
        else { sleepLr = lr; sleepCnt = 1; }
    }
    ctx.r3.u32 = 0;
}
