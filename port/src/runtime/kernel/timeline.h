// Frame timeline tracer (diagnostics, TTT2_TIMELINE=1): timestamped begin/end events from the
// threads that make up a frame (game main thread, job worker, command processor, vblank), kept
// in a lock-free ring. Creating C:/ttt2/port/timeline.trigger writes the last 3 seconds to
// C:/ttt2/port/timeline.txt ("<us> <thread> <event> <+|-|.> <value>"), to see which wait sits
// on the critical path of a late frame.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace timeline {

enum Event : uint8_t
{
    Swap, Vblank, CpVblankWait, CpRing, MainGpuWait, MainResourceWait, WorkerCounterWait, SkinJob, PoolSet,
    PoolWake, RbDrain, CpPresent, CpIb, InputRead,
    // TEST ONLY (input latency work, 2026-11-01): the game's update / draw / frame kick, the skinned-vertex ring's
    // advances and allocations (caller, region << 28 | offset, bytes), the skinning jobs' outputs and the command
    // processor's reads of the ring (region << 28 | offset; RingBump, RingBumpSize and CpRingRead came from a probe
    // removed after the measurement).
    GameUpdate, GameDraw, FrameKick, RingAdvance, RingAlloc, RingBump, RingBumpSize, SkinOut, CpRingRead, RingKick, RingKickLr,
    Count_
};
inline const char* Name(uint8_t e)
{
    static const char* names[] = { "swap", "vblank", "cp-vblank-wait", "cp-ring", "main-gpu-wait",
                                   "main-resource-wait", "worker-counter-wait", "skin-job", "pool-set",
                                   "pool-wake", "rb-drain", "cp-present", "cp-ib", "input-read",
                                   "game-update", "game-draw", "frame-kick", "ring-advance", "ring-alloc",
                                   "ring-bump", "ring-bump-size", "skin-out", "cp-ring-read", "ring-kick", "ring-kick-lr" };
    return e < Count_ ? names[e] : "?";
}

struct Entry { uint64_t us; uint32_t value; uint16_t thread; uint8_t event, phase; };
inline constexpr size_t kEntries = 1u << 20;

inline bool On()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_TIMELINE"); return e && *e == '1'; }();
    return on;
}
inline std::array<Entry, kEntries>& Ring() { static std::array<Entry, kEntries> ring{}; return ring; }
inline std::atomic<uint64_t>& Next() { static std::atomic<uint64_t> next{0}; return next; }
inline uint64_t NowUs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline uint16_t ThreadTag()
{
    static std::atomic<uint16_t> tags{0};
    thread_local const uint16_t tag = tags.fetch_add(1) + 1;
    return tag;
}
// phase: '+' begin, '-' end, '.' instant.
inline void Mark(Event e, char phase, uint32_t value = 0)
{
    if (!On()) return;
    Entry& slot = Ring()[Next().fetch_add(1) & (kEntries - 1)];
    slot = Entry{ NowUs(), value, ThreadTag(), uint8_t(e), uint8_t(phase) };
}
struct Scope
{
    Event e; uint32_t value;
    Scope(Event ev, uint32_t v = 0) : e(ev), value(v) { Mark(e, '+', value); }
    ~Scope() { Mark(e, '-', value); }
};

// The job pool's work event (TTT2: handle stored at 0x82F4BF00 + 188, which the sub_82AD00E8
// workers wait on): kernel set/wait calls on it are marked PoolSet / PoolWake.
inline bool IsPoolEvent(uint8_t* base, uint32_t handle)
{
    if (!On() || !handle) return false;
    uint32_t v; std::memcpy(&v, base + 0x82F4BFBCu, 4);
    return __builtin_bswap32(v) == handle;
}

// Called from the vblank thread: dump on the trigger file (checked every ~0.5 s).
void PollDumpTrigger();

} // namespace timeline
