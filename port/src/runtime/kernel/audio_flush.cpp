// XAudio2's own filters, run with denormals flushed to zero. When a voice goes quiet, the state of its filters
// decays below the smallest normal float and stays there, and the recompiled code converts that state between
// float and double on every sample (cvtss2sd reads a denormal, cvtsd2ss makes one). Older Intel processors
// (Sandy/Ivy Bridge and others) take a ~150-cycle microcode assist for each; MEASURED 2026-10-30: ~29,000 per
// audio frame (5.4 million a second) on the sound thread, all in these functions. Flushed, the state is exactly
// zero instead of below 1.2e-38: no audible difference. The guest's own flush-mode switches compare against
// ctx.fpscr.csr, which keeps its flush bits clear, so they leave the host's mode alone.
// Off by default: the GT 730 tester's diag17 logs (34/35, on / off every report window) showed no difference in
// frame rate or sound. TTT2_AUDIO_FLUSH=1 always on, =2 on / off every report window (an A/B, tests only).
#include "ppc_recomp_shared.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <xmmintrin.h>

std::atomic<uint64_t> g_audioFlushCalls[5];   // diagnostics (data_watch.cpp prints them with [fptrap])

namespace {
int AudioFlushMode()
{
    static const int mode = [] {
        const char* e = std::getenv("TTT2_AUDIO_FLUSH");
        return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0;
    }();
    return mode;
}
std::atomic<bool> g_flushOn{ true };
bool AudioFlush() { return AudioFlushMode() == 1 || (AudioFlushMode() == 2 && g_flushOn.load(std::memory_order_relaxed)); }

template <void (*Fn)(PPCContext&, uint8_t*), int Slot>
void RunFlushed(PPCContext& ctx, uint8_t* base)
{
    g_audioFlushCalls[Slot].fetch_add(1, std::memory_order_relaxed);
    if (!AudioFlush()) { Fn(ctx, base); return; }
    // Called from another of these, the caller's flush mode stays on after this one returns.
    const unsigned outer = _mm_getcsr() & PPCFPSCRRegister::FlushMask;
    _mm_setcsr(ctx.fpscr.csr | PPCFPSCRRegister::FlushMask);
    Fn(ctx, base);
    _mm_setcsr(ctx.fpscr.csr | outer);
}
}

// gpu_hw_d3d11.h AbSwitch: the next report window's way.
void AudioFlushAbToggle()
{
    if (AudioFlushMode() == 2) g_flushOn.store(!g_flushOn.load());
}
const char* AudioFlushLabel() { return AudioFlushMode() == 0 ? " flush-off" : AudioFlush() ? " flush-on" : " flush-off"; }

extern "C" PPC_FUNC(__imp__sub_82D0D728);
extern "C" PPC_FUNC(__imp__sub_82D0C3C0);
extern "C" PPC_FUNC(__imp__sub_82D10520);
extern "C" PPC_FUNC(__imp__sub_82D0DED8);
extern "C" PPC_FUNC(__imp__sub_82D105C0);

PPC_FUNC(sub_82D0D728) { RunFlushed<__imp__sub_82D0D728, 0>(ctx, base); }   // voice filter
PPC_FUNC(sub_82D0C3C0) { RunFlushed<__imp__sub_82D0C3C0, 1>(ctx, base); }
PPC_FUNC(sub_82D10520) { RunFlushed<__imp__sub_82D10520, 2>(ctx, base); }
PPC_FUNC(sub_82D0DED8) { RunFlushed<__imp__sub_82D0DED8, 3>(ctx, base); }
PPC_FUNC(sub_82D105C0) { RunFlushed<__imp__sub_82D105C0, 4>(ctx, base); }

// TTT2_DIAG_SND_SPIN_US=<us> (TEST ONLY): XAudio2's processing pass on the sound thread takes that much longer
// (a slower PC's sound thread); calls counted in "[snd-spin]" every 2000.
extern "C" PPC_FUNC(__imp__sub_82D0CF38);
PPC_FUNC(sub_82D0CF38)
{
    __imp__sub_82D0CF38(ctx, base);
    static const int64_t us = [] { const char* e = std::getenv("TTT2_DIAG_SND_SPIN_US"); return e ? std::atoll(e) : 0ll; }();
    if (us <= 0) return;
    static std::atomic<uint64_t> calls{ 0 };
    if (++calls % 2000 == 0) std::fprintf(stderr, "[snd-spin] %llu passes\n", (unsigned long long)calls.load());
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < until) {}
}
