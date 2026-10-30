// TTT2's monitor wait, sub_82AE4438(monitor, target): monitor = {+0 mutex handle, +4 auto-reset
// event handle, +16 u64 value}. The game's code:
//     lock(mutex)
//     while (value != target) {
//         SignalObjectAndWait(mutex, event, INFINITE)    // unlock, wait for a change
//         lock(mutex)
//         if (value != target) SetEvent(event)           // pass the wake on to other waiters
//     }
//     unlock(mutex)
// With one waiter the "pass it on" SetEvent finds nobody queued, stays set, and the same thread's
// next wait takes it straight back: the job thread (entry 82AE7090) spun a whole core in this loop
// in menus, pauses and fights alike (MEASURED 2026-10-15: 97% of a core, the user's PC felt slow).
// Same loop here, but the wake is passed on only when another thread is actually waiting on the
// event, and each wait times out after 2 ms - so a waiter that was just about to queue (and whose
// wake a skipped SetEvent would have been) re-checks the value within 2 ms. Hand-offs between
// queued waiters stay immediate (KEvent hand-off, kernel_objects.cpp). TTT2_MONITOR_WAIT=0 runs
// the game's loop.
#include "kernel_shared.h"
#include "timeline.h"
#include <atomic>
#include <cstdlib>
#include <intrin.h>

bool KernelEventHasWaiters(uint32_t handle);   // kernel_objects.cpp
void RollbackNoteJobObject(uint32_t addr, uint32_t size);   // rollback.cpp: never rewound
bool RollbackInResimDraw();                                  // rollback.cpp
void RollbackResimSkip();

extern "C" PPC_FUNC(__imp__sub_82AE4438);
PPC_EXTERN_FUNC(sub_82BEE890);   // WaitForSingleObject(handle, ms)
PPC_EXTERN_FUNC(sub_82BEE7D8);   // SignalObjectAndWait(signal, wait, ms, alertable)
PPC_EXTERN_FUNC(sub_82BEF380);   // SetEvent(handle)
PPC_EXTERN_FUNC(sub_82BEEF30);   // ReleaseMutex(handle)

PPC_FUNC(sub_82AE4438)
{
    const timeline::Scope mark{ timeline::WorkerCounterWait, ctx.r4.u32 };   // TTT2_TIMELINE=1
    RollbackNoteJobObject(ctx.r3.u32, 0x20);
    // A rollback's re-simulated frames: the frame kick (sub_82AD1DE8) waits on this monitor, which
    // moves with the vblank (sub_82AD17A8 ticks it) - MEASURED 15 ms per re-simulated frame. The
    // command processor discards those frames; nothing to pace.
    if (ctx.r3.u32 == 0x82F72178u && RollbackInResimDraw()) { RollbackResimSkip(); return; }
    static const bool on = [] { const char* e = std::getenv("TTT2_MONITOR_WAIT"); return !(e && *e == '0'); }();
    if (!on) { __imp__sub_82AE4438(ctx, base); return; }
    const uint32_t monitor = ctx.r3.u32;
    const uint64_t target = ctx.r4.u64;
    const uint32_t mutex = GRead32(base, monitor), event = GRead32(base, monitor + 4);
    auto value = [&] { return (uint64_t(GRead32(base, monitor + 16)) << 32) | GRead32(base, monitor + 20); };
    auto lock = [&] { ctx.r3.u64 = mutex; ctx.r4.s64 = -1; sub_82BEE890(ctx, base); };
    lock();
    while (value() != target)
    {
        ctx.r3.u64 = mutex; ctx.r4.u64 = event; ctx.r5.u64 = 2; ctx.r6.u64 = 0;
        sub_82BEE7D8(ctx, base);
        lock();
        if (value() != target && KernelEventHasWaiters(event))
        {
            ctx.r3.u64 = event;
            sub_82BEF380(ctx, base);
        }
    }
    ctx.r3.u64 = mutex;
    sub_82BEEF30(ctx, base);
}

// TTT2's D3D waits on the GPU - ring space (sub_82738DA0), fences, the swap counter: six loops
// that each call sub_8273D4A0 per step, 32 pause instructions and a hang check, then re-read a
// value the GPU side writes. MEASURED 2026-10-15 on Sakura Schoolyard: the main thread spent
// 89% of its time in that spin when the command processor fell behind - a whole core, 280 ms/s
// of it kernel time with the hypervisor (VBS) intercepting the PAUSE loop. Every one of those
// values is written by the command processor or a guest interrupt, which bump GpuProgress
// (kernel_video.cpp): each step first sleeps until the GPU side has written something since the
// caller last looked, at most 1 ms (the game's own hang check still runs every step).
// TTT2_GPU_WAIT=0 runs the game's spin.
extern std::atomic<uint32_t> g_gpuProgress;
bool WaitGpuProgress(uint32_t seen, unsigned long ms);   // kernel_video.cpp

extern "C" PPC_FUNC(__imp__sub_8273D4A0);
PPC_FUNC(sub_8273D4A0)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_GPU_WAIT"); return !(e && *e == '0'); }();
    // GpuProgress when this thread's previous step returned: the caller has checked its value
    // since, so an unchanged count means nothing it could be waiting for has been written.
    static thread_local uint32_t seen = ~0u;
    if (on && g_gpuProgress.load(std::memory_order_acquire) == seen)
    {
        for (int i = 0; i < 200 && g_gpuProgress.load(std::memory_order_acquire) == seen; ++i) _mm_pause();
        WaitGpuProgress(seen, 1);
    }
    __imp__sub_8273D4A0(ctx, base);
    seen = g_gpuProgress.load(std::memory_order_acquire);
}
