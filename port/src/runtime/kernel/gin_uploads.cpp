// The game's GIN uploads after a ranked match (ghosts, replays): the order the game counts on.
//
// When a ranked match ends, the game starts its upload coordinator (sub_82664F50, from the match-end replay step
// sub_82654CE8), which runs the ghost upload task and then the replay upload task. The ghost task waits (its vtable
// [3], sub_826E54A0) only while the save worker writes (state 2) - on the Xbox 360 the post-match save takes long
// enough that the match-end ghost step (sub_82557F48: the winner's ghosts queued, at most 2) comes first. On PC the
// save is done at once: the task found an empty queue, its upload failed, and the close-game state
// (GameMode::state_gamemode_online_close_game_start) then cancelled every upload of the match - the replay, which
// waits for its data a few seconds longer, included (MEASURED 2026-10-22: no ghost or replay ever reached a server;
// the order and the cancel in re/srv_tools traces, TTT2_GIN_TRACE=1). So the ghost task also waits for this match's
// ghost step, at most kGhostStepWaitMs (the close-game state waits for the uploads, so never longer).
#include "kernel_shared.h"
#include <windows.h>
#include <atomic>
#include <cstdio>

extern "C" PPC_FUNC(__imp__sub_82664F50);
extern "C" PPC_FUNC(__imp__sub_82557F48);
extern "C" PPC_FUNC(__imp__sub_826E54A0);

namespace gin_trace { void GhostStep(bool begin, uint32_t lr); bool On(); }

namespace {
constexpr uint64_t kGhostStepWaitMs = 15000;
// a ghost step this close before the coordinator's start belongs to the same match
constexpr uint64_t kSameMatchMs = 10000;

std::atomic<uint64_t> g_waitUntil{ 0 };     // GetTickCount64 deadline of the wait; 0 = none
std::atomic<uint64_t> g_lastGhostStep{ 0 };  // when the last ghost step ran
} // namespace

// the upload coordinator's start (one per ranked match)
PPC_FUNC(sub_82664F50)
{
    const uint64_t now = GetTickCount64(), step = g_lastGhostStep.load();
    g_waitUntil = step && now - step < kSameMatchMs ? 0 : now + kGhostStepWaitMs;
    __imp__sub_82664F50(ctx, base);
}

// the match-end ghost step
PPC_FUNC(sub_82557F48)
{
    gin_trace::GhostStep(true, uint32_t(ctx.lr));
    __imp__sub_82557F48(ctx, base);
    gin_trace::GhostStep(false, 0);
    g_lastGhostStep = GetTickCount64();
    if (g_waitUntil.exchange(0) && gin_trace::On()) std::fprintf(stderr, "[gin-trace] ghost step done: the ghost upload goes on\n");
}

// the ghost upload task's "wait" check
PPC_FUNC(sub_826E54A0)
{
    __imp__sub_826E54A0(ctx, base);
    const uint64_t until = g_waitUntil.load();
    if (!(ctx.r3.u32 & 0xFF) && until)
    {
        if (GetTickCount64() < until) ctx.r3.u32 = 1;
        else if (g_waitUntil.exchange(0))
            std::fprintf(stderr, "[gin] no ghost step %llu s after the match: the ghost upload goes on without it\n",
                         (unsigned long long)(kGhostStepWaitMs / 1000));
    }
}
