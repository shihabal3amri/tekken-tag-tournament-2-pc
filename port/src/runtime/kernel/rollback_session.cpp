// Online fights over the game's own netcode (NACE's input sync, "PlayCom"): how a fight frame gets its
// inputs, MEASURED before rollback sessions build on it. nace = 0x82E6C5C8 (online manager + 16),
// S = nace + 64 (the sync engine). Per update during a fight:
//   frame pump sub_826F13D0: receive, sync tick sub_826FB3B8 (advance, prune, pacer), record fetch
//     sub_826F0B30 -> nace+5976 {frame0, frame1, in0, in1}, ready +6020;
//   the battle scene: the game's own rollback sub_825D9178 (n = +5936 frames, its tick-only rewind
//     and re-ticks pumping sub_826F1390), the tick sub_822D36C8, the online policy sub_825D8328.
// The game predicts (+5932 on, ring +5940 head / +5944 count / +5948 capacity 3 / +5952 records)
// only in some states; S+5640 is the input delay (host-chosen by RTT, 3..9 frames).
//
// TTT2_RB_NACETRACE=1: one line per update during online fights (the first 400 of each fight, then
// every update with a rollback, a stall or a prediction change).
#include "kernel_shared.h"
#include "guest_heap.h"
#include "display_settings.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int RollbackFightRun();               // rollback.cpp: consecutive updates with a battle tick
int32_t RollbackSessionFrame();       // rollback.cpp: the frame a session update simulates (-1: none)
bool RollbackResimulating();          // rollback.cpp: a session's re-simulation runs
void RollbackNeverRewind(uint32_t addr, uint32_t size);   // rollback.cpp
bool RollbackIsRewound(uint32_t addr);                    // rollback.cpp

namespace {
constexpr uint32_t kNace = 0x82E6C5C8u;
constexpr uint32_t kSync = kNace + 64;

bool TraceOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_RB_NACETRACE"); return e && *e == '1'; }();
    return on;
}

std::string g_line;                    // this update's events
bool g_notable = false;                // rollback, stall or prediction change in this update
int g_lines = 0;
DWORD g_main = 0;
uint32_t g_ring = 0;                   // the session's larger prediction ring (a guest page of its own)

int32_t R32(uint32_t a) { return int32_t(GRead32(g_base, a)); }

// Sync engine node i: +720 newest frame stored, +724 current frame.
void NodeFrames(int i, int32_t& newest, int32_t& current)
{
    newest = current = -9;
    if (R32(kSync + 4856) <= i) return;
    const uint32_t node = GRead32(g_base, kSync + 4828 + 4 * i);
    if (!node) return;
    newest = R32(node + 720);
    current = R32(node + 724);
}

void Event(const char* what, bool notable = false)
{
    if (!TraceOn() || GetCurrentThreadId() != g_main) return;
    int32_t ln, lc, rn, rc;
    NodeFrames(0, ln, lc);
    NodeFrames(1, rn, rc);
    char buf[256];
    std::snprintf(buf, sizeof(buf), " %s[rdy %u rb %d pred %s %d/%d miss %d rec %d/%d L %d/%d R %d/%d dly %d]", what,
                  GRead8(g_base, kNace + 6020), R32(kNace + 5936), GRead8(g_base, kNace + 5932) ? "on" : "off",
                  R32(kNace + 5944), R32(kNace + 5948), R32(kNace + 5960), R32(kNace + 5976), R32(kNace + 5980),
                  ln, lc, rn, rc, R32(kSync + 5640));
    g_line += buf;
    g_notable |= notable;
}
} // namespace

bool SessionEnabled();
// rollback.cpp, at each update's start (main thread): the previous update's trace line; the larger
// prediction ring's page, allocated before any fight calibrates (a page of its own: never rewound).
void SessionTraceUpdate()
{
    if (SessionEnabled() && !g_ring)
    {
        void* p = g_virtualHeap.Alloc(4096, 4096);
        g_ring = p ? uint32_t(reinterpret_cast<uint8_t*>(p) - g_base) : 0;
        if (g_ring)
        {
            std::memset(p, 0, 4096);
            RollbackNeverRewind(g_ring, 4096);
        }
        // NACE's library globals (re/rollback_tools/nace_statics.py over 0x82B344A0-0x82B5CA78: its
        // managers, 64-bit times at 830B9428 / 830B94B8, a config block at 830C0000, three words at
        // 82E4CE20): the network's, never rewound (MEASURED: under 80 ms / 30 ms jitter / 3 % loss the
        // guest's NACE reported its peer lost - event 6, message 1000000C - in most matches with them
        // rewound).
        RollbackNeverRewind(0x830B9358u, 0x830B9548u - 0x830B9358u);
        RollbackNeverRewind(0x830C0000u, 0x70);
        RollbackNeverRewind(0x82E4CE20u, 12);
    }
    if (!TraceOn()) return;
    g_main = GetCurrentThreadId();
    const int run = RollbackFightRun();
    if (!g_line.empty() && (g_notable || (run > 0 && run < 400)) && g_lines < 20000)
    {
        ++g_lines;
        std::fprintf(stderr, "[nace] fight %d:%s\n", run, g_line.c_str());
    }
    g_line.clear();
    g_notable = false;
}
// The four fighters' state after a tick (FNV-1a over the objects' words): two instances' logs compare
// per record frame ("[nace-hash] <frame> <pending predictions> <ready> <hash> <rng>"; a re-ticked frame
// prints again). Pointer words are left out: each instance loads the fighters' data at its own
// addresses (MEASURED: lockstep fights differ only in words pointing into the heaps or the image).
uint32_t FighterHash()
{
    static const uint32_t kFighters[4] = { 0x82E88960u, 0x82E8B210u, 0x82E8DAC0u, 0x82E90370u };
    uint32_t h = 2166136261u;
    for (uint32_t f : kFighters)
        for (uint32_t i = 0; i < 0x28B0; i += 4)
        {
            const uint32_t w = GRead32(g_base, f + i);
            if ((w >= 0x00100000u && w < 0x30000000u) || (w >= 0x82000000u && w < 0x84000000u)) continue;
            h = (h ^ w) * 16777619u;
        }
    return h;
}
// The fight's RNG state (the word NACE's own desync check compares the low half of).
uint32_t FightRng() { const uint32_t holder = GRead32(g_base, 0x82E6B05Cu); return holder ? GRead32(g_base, holder + 16) : 0; }
void FlowTrace(const char* where);
void ArmPeerWatch();
// The fighters when a session snapshot is taken ("[nace-state] <frame> <hash> <rng>": the state at the
// start of that record frame; the last line per frame is the final timeline's). Taken with every
// frame thread idle - the tick-end hash ([nace-hash]) races the pool jobs the tick started
// (MEASURED: dumps of two instances equal while their tick-end hashes differed).
void SessionStateTrace(int32_t frame)
{
    if (!TraceOn()) return;
    std::fprintf(stderr, "[nace-state] %d %08X %08X\n", frame, FighterHash(), FightRng());
}
// A battle tick without a ready record (a stall) should leave the fighters alone: the words it changes
// (TTT2_RB_NACETRACE=1, the first 400) - a stall tick that moves the fight desyncs instances that
// stall at different times (MEASURED: the instance that stalled at a round start differed from the
// other from frame 60 on, the RNG still equal).
uint8_t g_stallBefore[4 * 0x28B0];
bool g_stallTick = false;
int g_stallWords = 0;
constexpr uint32_t kFighterObjects[4] = { 0x82E88960u, 0x82E8B210u, 0x82E8DAC0u, 0x82E90370u };
void SessionTraceTick(bool start)
{
    if (start)
    {
        Event("tick");
        g_stallTick = TraceOn() && GetCurrentThreadId() == g_main && R32(kSync + 4856) > 0 && !GRead8(g_base, kNace + 6020) && g_stallWords < 400;
        if (g_stallTick)
            for (int k = 0; k < 4; ++k) std::memcpy(g_stallBefore + k * 0x28B0, g_base + kFighterObjects[k], 0x28B0);
        return;
    }
    if (g_stallTick)
    {
        g_stallTick = false;
        for (int k = 0; k < 4 && g_stallWords < 400; ++k)
            for (uint32_t o = 0; o < 0x28B0 && g_stallWords < 400; o += 4)
            {
                const uint8_t* b = g_stallBefore + k * 0x28B0 + o;
                const uint32_t before = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
                const uint32_t after = GRead32(g_base, kFighterObjects[k] + o);
                if (before == after) continue;
                ++g_stallWords;
                std::fprintf(stderr, "[stall-tick] record %d/%d, fighter %d +%04X: %08X -> %08X\n", R32(kNace + 5976), R32(kNace + 5980), k, o, before, after);
            }
    }
    FlowTrace("after a tick");
    if (!TraceOn() || GetCurrentThreadId() != g_main || R32(kSync + 4856) <= 0) return;
    const int32_t a = R32(kNace + 5976), b = R32(kNace + 5980), own = RollbackSessionFrame();
    const int32_t frame = own >= 0 ? own : a > b ? a : b;   // sessions count frames; otherwise best effort
    const uint8_t ready = GRead8(g_base, kNace + 6020);
    const int32_t pending = R32(kNace + 5944);
    // ... plus the record the tick used: its two frames and two input words.
    std::fprintf(stderr, "[nace-hash] %d %d %u %08X %08X %d/%d %08X/%08X\n", frame, pending, ready, FighterHash(), FightRng(),
                 a, b, R32(kNace + 5984), R32(kNace + 5988));
    // TTT2_RB_FIGHTERDUMP=<dir>: the four fighter objects at the record frames in TTT2_RB_DUMPFRAMES
    // (default 300,600,900; the first tick of a real record for them), for word-by-word comparison
    // between instances.
    static const char* dir = std::getenv("TTT2_RB_FIGHTERDUMP");
    static int dumped = 0;
    static const std::string frames = [] { const char* e = std::getenv("TTT2_RB_DUMPFRAMES"); return std::string(",") + (e ? e : "300,600,900") + ","; }();
    // Every tick of a listed frame writes the file again: a re-simulated (corrected) frame replaces its
    // predicted run, so the file ends as the frame's final state.
    // TTT2_RB_DUMPFRAMES=all: every frame (a desync found afterwards can be diffed at its first frame).
    if (dir && ready && (frames == ",all," || frames.find("," + std::to_string(frame) + ",") != std::string::npos))
    {
        dumped = frame;
        const char* tag = std::getenv("TTT2_INSTANCE");
        char path[512];
        std::snprintf(path, sizeof(path), "%s\\fighters_%s_%d.bin", dir, tag && *tag ? tag : "A", frame);
        if (FILE* f = std::fopen(path, "wb"))
        {
            for (uint32_t a : { 0x82E88960u, 0x82E8B210u, 0x82E8DAC0u, 0x82E90370u }) std::fwrite(g_base + a, 1, 0x28B0, f);
            std::fclose(f);
        }
    }
}

// ---- rollback sessions -----------------------------------------------------------------------------
// TTT2_ROLLBACK=1 (in development): online fights predict the opponent with NACE's own prediction and
// rewind with rollback.cpp's full state instead of the game's tick-only rewind (MEASURED: forced on in
// normal fighting, the game's own rewind desynced two instances within ~80 frames). Per fight update:
//   - rollback.cpp calls SessionPrePump before the update: the frame pump (receive, sync tick, record
//     fetch); the game's own call inside the update then does nothing;
//   - a misprediction (+5936 = n): the record now in +5976 is the real one of the oldest predicted
//     frame; rollback.cpp restores that frame's snapshot and re-simulates the n frames, each after
//     SessionNext (one confirmed frame consumed + the next record fetched, as the game's re-ticks do);
//   - the game's own rewind sees no misprediction (+5936 = -1).
// The prediction ring's capacity (the game's is 3): how many frames a session predicts before it waits
// - Options' ONLINE NETCODE (8, CUSTOM: its CUSTOM ROLLBACK), read at each session's start;
// TTT2_RB_PREDICT=<frames> (tests) overrides it.
extern "C" PPC_FUNC(__imp__sub_826F13D0);
extern "C" PPC_FUNC(__imp__sub_826F1390);
extern "C" PPC_FUNC(__imp__sub_82667CD8);
int SessionInputDelay(double rtt);     // below
int GameLockstepDelay(double rtt);     // below
bool RollbackSessionOn();              // rollback.cpp: a session runs (its frames are counted)
int32_t RollbackSessionLast();         // rollback.cpp: the newest record frame the session consumed
bool XSessionLocalHosts();             // xsession.cpp
uint32_t XSessionHostNetcode();        // xsession.cpp: the joined session's host's ONLINE NETCODE (0: unknown)
namespace {
bool g_ownPump = false;                // the session runs the frame pump: the game's call does nothing
int g_steerWant = -1;                  // SteerDelay: the delay this session wants now (-1: not yet known)
bool g_steerRound = false;             // ... for a round (true) or lockstep
int g_steerCountdown = 0;              // updates until the ping is read again
int g_steerAsked = -1;                 // the delay last requested, and updates since
void TimeSyncReset();                  // below
void TimeSyncStop();
int g_steerSeen = -1;                  // the last reading's value (hysteresis)
int g_steerAskedAgo = 1000;

int PredictFrames()
{
    static const int forced = [] { const char* e = std::getenv("TTT2_RB_PREDICT"); return e && *e ? std::atoi(e) : 0; }();
    const int n = forced > 0 ? forced
                : display_settings::GetNetcode() == display_settings::NetCustom ? display_settings::GetCustomRollback()
                : display_settings::kMaxRollback;
    return n < 1 ? 1 : n > 15 ? 15 : n;
}
// The loaded record's frame when it is a real one (its two frames equal, no prediction outstanding):
// a predicted record carries the frames of the inputs it was made from, not the frame it stands for
// (MEASURED: one ahead at times), so sessions count frames themselves.
int32_t RealRecordFrame()
{
    const int32_t a = R32(kNace + 5976), b = R32(kNace + 5980);
    return a == b && R32(kNace + 5944) == 0 ? a : -1;
}
} // namespace

// Rollback sessions for online fights: on by default (rollback builds only meet each other - the
// lobby protocol, steam_net.cpp); TTT2_ROLLBACK=0 plays the game's own lockstep netcode (tests).
bool SessionEnabled()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_ROLLBACK"); return !(e && *e == '0'); }();
    return on;
}

// NACE runs the fight's input sync: the stateUpdate state with the battle's sync running (+6012 == 3)
// between at least two nodes.
bool SessionSyncRunning()
{
    return GRead32(g_base, kNace + 24) == 0x826F0488u && R32(kNace + 6012) == 3 && R32(kSync + 4856) >= 2;
}
// The end of the battle is announced (the host's "end at F" command: phase S+5040 >= 2): no new
// predictions; the session lasts until the outstanding ones are resolved.
bool SessionEnding() { return R32(kSync + 5040) >= 2; }
int SessionPending() { return R32(kNace + 5944); }

// rollback.cpp, at a session's start and end.
void SessionOwnPump(bool own)
{
    // The game's own ring (36 bytes from its allocator) goes back at the end - the next battle's sync
    // start frees what +5952 points to. rollback.cpp ends a session with no prediction outstanding
    // unless the sync itself stopped (then they no longer matter).
    static uint32_t gameRing = 0, gameCap = 0;
    g_ownPump = own;
    if (!own)
    {
        TimeSyncStop();                                    // the vblank schedule back to 60 Hz
        if (!gameRing) return;
        if (R32(kNace + 5944) != 0)
            std::fprintf(stderr, "[rb-session] ended with %d predictions outstanding\n", R32(kNace + 5944));
        for (uint32_t i = 0; i < gameCap; ++i)
        {
            GWrite32(g_base, gameRing + i * 12, uint32_t(-1));
            GWrite32(g_base, gameRing + i * 12 + 4, uint32_t(-1));
            GWrite32(g_base, gameRing + i * 12 + 8, 0);
        }
        GWrite32(g_base, kNace + 5952, gameRing);
        GWrite32(g_base, kNace + 5948, gameCap);
        GWrite32(g_base, kNace + 5940, 0);
        GWrite32(g_base, kNace + 5944, 0);
        gameRing = gameCap = 0;
        return;
    }
    GWrite8(g_base, kNace + 5932, 1);                  // predict from this update on
    g_steerWant = g_steerAsked = g_steerSeen = -1;
    TimeSyncReset();
    g_steerAskedAgo = 1000;
    // The larger prediction ring, installed while no prediction is outstanding.
    const int cap = PredictFrames();
    if (g_ring && R32(kNace + 5944) == 0 && R32(kNace + 5948) != cap)
    {
        gameRing = GRead32(g_base, kNace + 5952);
        gameCap = GRead32(g_base, kNace + 5948);
        for (int i = 0; i < cap; ++i)
        {
            GWrite32(g_base, g_ring + i * 12, uint32_t(-1));
            GWrite32(g_base, g_ring + i * 12 + 4, uint32_t(-1));
            GWrite32(g_base, g_ring + i * 12 + 8, 0);
        }
        GWrite32(g_base, kNace + 5952, g_ring);
        GWrite32(g_base, kNace + 5948, uint32_t(cap));
        GWrite32(g_base, kNace + 5940, 0);
        std::fprintf(stderr, "[rb-session] prediction ring: %d frames at %08X\n", cap, g_ring);
    }
}

// The session's input delay, steered every update. A battle's sync starts at 4 frames (sub_8266CD08 ->
// sub_826F65F8) and the game itself asks for its RTT-based delay only at battle counter 100 in states
// 3 / 21. While a round runs (state 4, prediction on) the session wants the ONLINE NETCODE delay;
// outside rounds (intros, KO, between rounds: lockstep, sessionPredicts()) the game's own lockstep
// delay - MEASURED with the low delay there: ~9 % of updates over 30 ms (every frame waited for the
// other side's input). Requests are in-band (sub_826F65A0): only the side whose sync node 0 is in state
// 2 may make one (sub_826F60A8; the other side's are dropped), one at a time (S+5068 pending, S+5128 a
// change in progress); NACE switches both at a frame they agree on (sub_826FA880). MEASURED: sessions
// stay in sync through changes (4 -> 1 at frame 819). Which side that is changes between battles of
// one lobby (MEASURED 2026-10-20, two PCs, five battles: the Live session's guest in three, its host in
// two) - so the side that may request steers, with the Live session host's ONLINE NETCODE either way
// (SessionInputDelay). Steering by the Live session's guest left two battles at the lockstep delay, its
// requests all dropped.
bool SyncMayRequest()
{
    if (R32(kSync + 4856) <= 0) return false;
    const uint32_t node = GRead32(g_base, kSync + 4828);
    const uint32_t peer = node ? GRead32(g_base, node + 736) : 0;
    return peer && R32(peer) == 2;
}
bool RoundPredicts();                  // below: prediction is on in this battle state
void SteerDelay(PPCContext& ctx, uint8_t* base)
{
    // The side the sync engine takes requests from, and only while the session runs: requests while it
    // waited for its first real record moved where the two sides started it (MEASURED: record frame 65 on
    // one, 137 on the other).
    if (!RollbackSessionOn() || !SyncMayRequest()) return;
    const bool round = RoundPredicts();
    if (round != g_steerRound || g_steerWant < 0 || --g_steerCountdown <= 0)
    {
        PPCContext c = ctx;
        __imp__sub_82667CD8(c, base);                  // the RTT (ms), read-only
        const double rtt = c.f1.f64;
        const int want = round ? SessionInputDelay(rtt) : GameLockstepDelay(rtt);
        // A new phase takes its delay at once; within one, a different value must come out of two
        // readings in a row (MEASURED: a ping around a table step flipped 4 / 5 every few seconds).
        if (round != g_steerRound || g_steerWant < 0 || want == g_steerSeen) g_steerWant = want;
        g_steerSeen = want;
        g_steerRound = round;
        g_steerCountdown = 120;                        // the ping again in 2 s
    }
    // One request per value: S+5640 changes only at the agreed frame, after S+5128 is back to 0
    // (MEASURED: asking again whenever S+5640 differed restarted the change every ~3 frames and the
    // instances diverged). The same value again only if it has not applied within 2 s.
    const bool busy = GRead8(g_base, kSync + 5068) != 0 || R32(kSync + 5128) != 0;
    if (g_steerAskedAgo < 1000) ++g_steerAskedAgo;
    if (busy || R32(kSync + 5640) == g_steerWant) return;
    if (g_steerAsked == g_steerWant && g_steerAskedAgo < 120) return;
    PPCContext d = ctx;
    d.r3.u64 = kSync;
    d.r4.u64 = uint32_t(g_steerWant);
    sub_826F65A0(d, base);
    g_steerAsked = g_steerWant;
    g_steerAskedAgo = 0;
}

// The frame pump for this update. Returns the misprediction depth (0: none); `ready`: a record is
// loaded (otherwise the battle stalls this update); `real`: its frame when it is a real one, else -1
// (after a misprediction it is: the oldest predicted frame's real record).
int SessionPrePump(PPCContext& ctx, uint8_t* base, int32_t& real, bool& ready)
{
    PPCContext c = ctx;
    c.r3.u64 = kNace;
    __imp__sub_826F13D0(c, base);
    const int32_t n = R32(kNace + 5936);
    GWrite32(base, kNace + 5936, uint32_t(-1));
    ready = GRead8(base, kNace + 6020) != 0;
    real = ready ? RealRecordFrame() : -1;
    if (TraceOn()) Event(n > 0 ? "prepump-miss" : "prepump", n > 0 || !ready);
    FlowTrace("pump");
    ArmPeerWatch();
    SteerDelay(ctx, base);
    return n > 0 ? n : 0;
}
// NACE node 0's confirmed frame (the newest record whose inputs are all real; -9 without a node).
int32_t SessionConfirmedFrame()
{
    int32_t newest, current;
    NodeFrames(0, newest, current);
    return current;
}
// The loaded record's frame if it is a real one (consistency checks), else -1.
int32_t SessionRealFrame() { return GRead8(g_base, kNace + 6020) ? RealRecordFrame() : -1; }

// Before re-simulating the next frame (and before the live one after a rewind): the frame just
// simulated is consumed, the next record fetched. `frame` as above.
bool SessionNext(PPCContext& ctx, uint8_t* base)
{
    PPCContext c = ctx;
    c.r3.u64 = kNace;
    __imp__sub_826F1390(c, base);
    const int32_t n = R32(kNace + 5936);
    if (n > 0) std::fprintf(stderr, "[rb-session] a misprediction while re-simulating (depth %d) - unexpected\n", n);
    GWrite32(base, kNace + 5936, uint32_t(-1));
    return GRead8(base, kNace + 6020) != 0;
}

extern "C" PPC_FUNC(__imp__sub_826F13D0);
PPC_FUNC(sub_826F13D0)
{
    if (g_ownPump)
    {
        if (TraceOn()) Event("pump-owned");
        return;
    }
    __imp__sub_826F13D0(ctx, base);
    if (TraceOn()) Event("pump", R32(kNace + 5936) >= 0 || !GRead8(g_base, kNace + 6020));
}

extern "C" PPC_FUNC(__imp__sub_826F1390);
PPC_FUNC(sub_826F1390)
{
    __imp__sub_826F1390(ctx, base);
    if (TraceOn()) Event("repump", true);
}

// The fight RNG's draws (sub_822572A0(state, range) on [0x82E6B05C] + 16), per record frame with the
// caller: two instances' logs show who draws where (TTT2_RB_NACETRACE=1).
extern "C" PPC_FUNC(__imp__sub_822572A0);
PPC_FUNC(sub_822572A0)
{
    const uint32_t state = ctx.r3.u32, range = ctx.r4.u32, lr = uint32_t(ctx.lr);
    __imp__sub_822572A0(ctx, base);
    if (!TraceOn() || GetCurrentThreadId() != g_main || R32(kSync + 4856) <= 0) return;
    const uint32_t holder = GRead32(g_base, 0x82E6B05Cu);
    if (!holder || state != holder + 16) return;
    const int32_t own = RollbackSessionFrame();
    std::fprintf(stderr, "[rng] %d lr %08X range %u -> %u\n", own, lr, range, ctx.r3.u32);
}

extern "C" PPC_FUNC(__imp__sub_825D9178);
PPC_FUNC(sub_825D9178)
{
    if (TraceOn()) Event("scene");
    __imp__sub_825D9178(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_825D8328);
PPC_FUNC(sub_825D8328)
{
    __imp__sub_825D8328(ctx, base);
    if (TraceOn()) Event("policy");
}

// NACE's desync check (TTT2_RB_NACETRACE=1): the policy publishes (record frame, low 16 bits of the
// fight RNG) at frame % 20 == 19 with no prediction outstanding (sub_8266C7A0 -> node +736: +24 frame,
// +20 value); sub_826F8230(S) compares every node's latest pair (equal frames, different values = a
// desync, the session ends). stateUpdate (sub_826F0488) leaving "running" (+6012 3 -> 4) logs why.
extern "C" PPC_FUNC(__imp__sub_8266C7A0);
PPC_FUNC(sub_8266C7A0)
{
    if (TraceOn())
        std::fprintf(stderr, "[nace-sync] publish frame %d value %04X (session frame %d%s)\n", ctx.r4.s32, ctx.r5.u32 & 0xFFFFu,
                     RollbackSessionFrame(), RollbackResimulating() ? ", re-simulating" : "");
    __imp__sub_8266C7A0(ctx, base);
}

void SyncPairs(char* out, size_t size)
{
    int k = 0;
    const int32_t nodes = R32(kSync + 4856);
    for (int i = 0; i < nodes && i < 4 && k < int(size) - 48; ++i)
    {
        const uint32_t node = GRead32(g_base, kSync + 4828 + 4 * i);
        if (!node) continue;
        const uint32_t pub = GRead32(g_base, node + 736);
        k += std::snprintf(out + k, size - k, " node%d[frame %d: published %d/%04X, gone %u]", i, R32(node + 724),
                           pub ? R32(pub + 24) : -99, pub ? GRead16(g_base, pub + 20) : 0u, GRead8(g_base, node + 748));
    }
}

// The round flow's "all players ready" (TTT2_RB_NACETRACE=1): the battle (sub_822DA2C0, phase counter
// 0x82E88860 held at 93) waits for its assets and sub_826F5B70 (S+5100 == S+5104); a request is
// sub_826F8850 (S+5104++, host flag S+5085); the host collects ready bits (S+5096) and sets the go
// frame (S+5088); sub_826F6288 applies it when node 0's frame (+724) equals it (S+5092 = go, S+5100 =
// S+5104, the wait counter S+5108 = 0). Printed on every change, with where it was seen.
void FlowTrace(const char* where)
{
    if (!TraceOn()) return;
    // Watched for changes (the battle counter and node 0's frame are printed, not watched): the ready
    // protocol, the end of battle (phase S+5040, end frame S+5044, frames to skip S+5060 / S+5064) and
    // the fighters' +0x70 words (set at a KO: MEASURED live 0x00B40000 / 0x00700000, 0 in a
    // re-simulation of the same frames).
    static uint32_t last[14] = {};
    int32_t ln, lc;
    NodeFrames(0, ln, lc);
    const uint32_t now[14] = { GRead32(g_base, kSync + 5084), GRead32(g_base, kSync + 5088), GRead32(g_base, kSync + 5092),
                               GRead32(g_base, kSync + 5096), GRead32(g_base, kSync + 5100), GRead32(g_base, kSync + 5104),
                               GRead32(g_base, kSync + 5040), GRead32(g_base, kSync + 5044), GRead32(g_base, kSync + 5060),
                               GRead32(g_base, kSync + 5064), GRead32(g_base, 0x82E88960u + 0x70), GRead32(g_base, 0x82E8DAC0u + 0x70),
                               GRead32(g_base, 0x82EAC03Cu), GRead32(g_base, 0x82EAC040u) };   // the battle's state / next state (sub_822DAB68)
    bool changed = false;
    for (int i = 0; i < 14; ++i) changed |= now[i] != last[i];
    if (!changed) return;
    std::memcpy(last, now, sizeof(last));
    std::fprintf(stderr, "[nace-flow] %s: session frame %d%s, node0 frame %d, battle counter %u; flags %08X go %d applied %d ready bits %X acked %u requested %u wait %d; "
                 "end phase %u at %d, skip %d/%d; fighters +70 %08X %08X; battle state %u next %u\n",
                 where, RollbackSessionFrame(), RollbackResimulating() ? " (re-simulating)" : "", lc, GRead32(g_base, 0x82E88860u), now[0],
                 int32_t(now[1]), int32_t(now[2]), now[3], now[4], now[5], R32(kSync + 5108), now[6], int32_t(now[7]), int32_t(now[8]),
                 int32_t(now[9]), now[10], now[11], now[12], now[13]);
}

extern "C" PPC_FUNC(__imp__sub_826F8850);
PPC_FUNC(sub_826F8850)
{
    // A session: a request while one is still pending (S+5100 != S+5104) is a re-simulated frame asking
    // again - it goes no further (MEASURED: the repeat reached the host after it had announced go 3590,
    // the host announced a second go 3593; the host went on at 3590, the guest only at 3593 - desync).
    if (g_ownPump && R32(kSync + 5100) != R32(kSync + 5104))
    {
        if (TraceOn()) FlowTrace("a repeated ready request, not sent");
        ctx.r3.u64 = 0;
        return;
    }
    FlowTrace("before a ready request");
    __imp__sub_826F8850(ctx, base);
    FlowTrace("ready request");
}

// Diagnostics: a node leaves NACE's sync (sub_826FAB78(S, peer) - sub_826FB100 removes every node whose
// peer object's state word (+0) is 0; with rollback under 80 ms / 30 ms jitter / 3 % loss this happened
// in most matches, never in lockstep) - who, when, and the peer's words.
extern "C" PPC_FUNC(__imp__sub_826FAB78);
PPC_FUNC(sub_826FAB78)
{
    const uint32_t peer = ctx.r4.u32;
    char line[400];
    int k = std::snprintf(line, sizeof(line), "[nace-flow] node leaves (session frame %d%s): peer %08X words %08X %08X %08X %08X, guest callers %08X",
                          RollbackSessionFrame(), RollbackResimulating() ? ", re-simulating" : "", peer,
                          peer ? GRead32(base, peer) : 0, peer ? GRead32(base, peer + 4) : 0, peer ? GRead32(base, peer + 8) : 0,
                          peer ? GRead32(base, peer + 12) : 0, uint32_t(ctx.lr));
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < 10 && sp && sp < 0xFFFFFFF0u && k < int(sizeof(line)) - 12; ++i)
    {
        const uint32_t prev = GRead32(base, sp);
        if (!prev || prev <= sp || prev - sp > 0x10000) break;
        k += std::snprintf(line + k, sizeof(line) - k, " %08X", GRead32(base, prev - 8));
        sp = prev;
    }
    std::fprintf(stderr, "%s\n", line);
    __imp__sub_826FAB78(ctx, base);
}

// Diagnostics: the online system's message lists (sub_8266A698(lists, msg): msg+4 = kind 0..2, a list
// of 20 bytes each at 0x82F30A70; the battle aborts when kind 2 - or, at its checks with the flag, kind
// 0 - holds a message: sub_825E0320 -> sub_825D5CC8 -> sub_826698B0). Who posts, in a session.
extern "C" PPC_FUNC(__imp__sub_8266A698);
PPC_FUNC(sub_8266A698)
{
    if (g_ownPump)
    {
        const uint32_t msg = ctx.r4.u32;
        char line[400];
        int k = std::snprintf(line, sizeof(line), "[nace-flow] online message kind %d words %08X %08X %08X (session frame %d%s), guest callers %08X",
                              msg ? R32(msg + 4) : -1, msg ? GRead32(base, msg) : 0, msg ? GRead32(base, msg + 8) : 0,
                              msg ? GRead32(base, msg + 12) : 0, RollbackSessionFrame(), RollbackResimulating() ? ", re-simulating" : "",
                              uint32_t(ctx.lr));
        uint32_t sp = ctx.r1.u32;
        for (int i = 0; i < 10 && sp && sp < 0xFFFFFFF0u && k < int(sizeof(line)) - 12; ++i)
        {
            const uint32_t prev = GRead32(base, sp);
            if (!prev || prev <= sp || prev - sp > 0x10000) break;
            k += std::snprintf(line + k, sizeof(line) - k, " %08X", GRead32(base, prev - 8));
            sp = prev;
        }
        std::fprintf(stderr, "%s\n", line);
    }
    __imp__sub_8266A698(ctx, base);
}

// At a session's start (host instance, TTT2_DATAWATCH=1): the remote node's peer state word armed for the
// data watch (C:/ttt2/port/datawatch.txt) - who sets it to 0.
void ArmPeerWatch()
{
    static bool armed = false;
    if (armed || R32(kSync + 4856) < 2) return;
    const char* tag = std::getenv("TTT2_INSTANCE");
    if (tag && *tag) return;
    const uint32_t node = GRead32(g_base, kSync + 4828 + 4);
    const uint32_t peer = node ? GRead32(g_base, node + 736) : 0;
    if (!peer) return;
    armed = true;
    // TTT2_RB_WATCHADDR=<hex>: that guest address instead (e.g. the online state word 82E6E2F8).
    const char* other = std::getenv("TTT2_RB_WATCHADDR");
    // A full line ("<hex> w <hex>": two addresses) is written as it is.
    const uint32_t watch = other && *other ? uint32_t(std::strtoul(other, nullptr, 16)) : peer;
    if (FILE* f = std::fopen("C:/ttt2/port/datawatch.txt", "w"))
    {
        if (other && std::strchr(other, ' ')) std::fprintf(f, "%s\n", other);
        else std::fprintf(f, "%08X w\n", watch);
        std::fclose(f);
    }
    std::fprintf(stderr, "[nace-flow] watching node 1's peer state word %08X (now %08X; %s)\n", peer, GRead32(g_base, peer),
                 RollbackIsRewound(peer) ? "REWOUND by restores" : "not rewound");
}

// The end-of-battle request (sub_826F87B8 sets S+5084; the host then sends "end at F") - who asks for
// it, in which frame, live or re-simulated. Its frame also names the end-of-battle save that follows
// (rollback.cpp RollbackSaveJournalFrame).
std::atomic<int32_t> g_endBattleFrame{ -1 };
int32_t SessionEndBattleFrame() { return g_endBattleFrame.load(); }
extern "C" PPC_FUNC(__imp__sub_826F87B8);
PPC_FUNC(sub_826F87B8)
{
    g_endBattleFrame = RollbackSessionFrame();
    char line[320];
    int k = std::snprintf(line, sizeof(line), "[nace-flow] end of battle requested (session frame %d%s), guest callers %08X",
                          RollbackSessionFrame(), RollbackResimulating() ? ", re-simulating" : "", uint32_t(ctx.lr));
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < 10 && sp && sp < 0xFFFFFFF0u && k < int(sizeof(line)) - 12; ++i)
    {
        const uint32_t prev = GRead32(base, sp);
        if (!prev || prev <= sp || prev - sp > 0x10000) break;
        k += std::snprintf(line + k, sizeof(line) - k, " %08X", GRead32(base, prev - 8));
        sp = prev;
    }
    std::fprintf(stderr, "%s\n", line);
    __imp__sub_826F87B8(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_826F6288);
PPC_FUNC(sub_826F6288)
{
    __imp__sub_826F6288(ctx, base);
    FlowTrace("ack check");
}

extern "C" PPC_FUNC(__imp__sub_826F8230);
PPC_FUNC(sub_826F8230)
{
    __imp__sub_826F8230(ctx, base);
    if ((ctx.r3.u32 & 0xFF) == 0) return;
    char pairs[512] = "";
    SyncPairs(pairs, sizeof(pairs));
    std::fprintf(stderr, "[nace-sync] DESYNC reported (session frame %d%s):%s\n", RollbackSessionFrame(),
                 RollbackResimulating() ? ", re-simulating" : "", pairs);
}

extern "C" PPC_FUNC(__imp__sub_826F0488);
PPC_FUNC(sub_826F0488)
{
    const uint32_t nace = ctx.r3.u32;
    const int32_t before = R32(nace + 6012);
    __imp__sub_826F0488(ctx, base);
    if (before != 3 || R32(nace + 6012) != 4) return;
    char pairs[512] = "";
    SyncPairs(pairs, sizeof(pairs));
    std::fprintf(stderr, "[nace-sync] the session stopped running (session frame %d): nodes %d (start %d), +4928 %d, S+5120 %d, +5172 %d, "
                 "+8 %u, flags 6032/6033/6034 %u/%u/%u, phase S+5040 %d;%s\n", RollbackSessionFrame(), R32(kSync + 4856), R32(nace + 4916),
                 R32(nace + 4928), R32(kSync + 5120), R32(nace + 5172), GRead8(g_base, nace + 8), GRead8(g_base, nace + 6032),
                 GRead8(g_base, nace + 6033), GRead8(g_base, nace + 6034), R32(kSync + 5040), pairs);
}

// The game's "predictions outstanding" query (online manager +5960 = nace+5944, sub_82664E18). The game
// reads it as game logic: at a KO the round waits until it is 0 plus 4 frames (sub_822D9A30, state 4 ->
// 5), a block of per-frame work is skipped while it is not 0 (sub_824CD100), sub_8257EC58 answers "is
// predicting". A session predicts on its own and rewinds every misprediction, so to the game every frame
// it simulates is a confirmed one: 0, as in lockstep (MEASURED: the live count made the round end on
// different frames live, re-simulated and on each instance). The policy (sub_825D8328, return 825D83D0)
// keeps the real count: it publishes the desync check's RNG only with nothing outstanding.
extern "C" PPC_FUNC(__imp__sub_82664E18);
PPC_FUNC(sub_82664E18)
{
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_82664E18(ctx, base);
    if (g_ownPump && lr != 0x825D83D0u) ctx.r3.u64 = 0;
}

// The online policy's lockstep switch (manager, lockstep): nace+5932 (prediction) = !lockstep. MEASURED:
// the policy passes 1 (prediction off) every frame of a normal fight. TTT2_RB_FORCEPREDICT=1
// (experiment): always prediction - the game's own 3-frame prediction and tick-only rollback.
// A session predicts only while a round runs (battle state 0x82EAC03C == 4; MEASURED per match:
// 0 / 1 / 3 intros for ~890 frames, 4 the round, 5 KO, 6 ... between rounds) and the battle's end is
// not announced: no rewind then re-simulates an intro, a KO sequence's end or a round reset - the
// loads and sound teardown they do (MEASURED: a sound object's null vtable, lr 82B1ED44, re-simulating
// across the 1 -> 3 change with data027.bin loads). The policy runs after the tick, so the frame after
// the state turns 4 is the first predicted one, and a KO's frame is the last. Elsewhere lockstep, with
// the game's own delay (SteerDelay). TTT2_RB_PREDICTALL=1 (tests): every battle state, as before.
bool RoundPredicts()
{
    static const bool all = [] { const char* e = std::getenv("TTT2_RB_PREDICTALL"); return e && *e == '1'; }();
    return !SessionEnding() && (all || GRead32(g_base, 0x82EAC03Cu) == 4);
}

extern "C" PPC_FUNC(__imp__sub_8266C418);
PPC_FUNC(sub_8266C418)
{
    static const bool force = [] { const char* e = std::getenv("TTT2_RB_FORCEPREDICT"); return e && *e == '1'; }();
    if (force || (g_ownPump && RoundPredicts())) ctx.r4.u64 = 0;
    else if (g_ownPump) ctx.r4.u64 = 1;
    const uint8_t before = GRead8(g_base, kNace + 5932);
    __imp__sub_8266C418(ctx, base);
    if (TraceOn())
        Event(GRead8(g_base, kNace + 5932) ? "predict-on" : "predict-off", before != GRead8(g_base, kNace + 5932));
}

// The battle's input delay. The host's online policy (sub_825D8328, at battle counter 100) asks
// sub_82667F88 for it: the ping (RTT in ms, sub_82667CD8's f1, which the comparisons leave in place)
// mapped to 3 frames below 99 ms, 4 below 133, ... 9 from 300 - enough for lockstep to wait on nothing.
// The sync engine sends it in-band (S+5072, applied at a frame both sides agree on), so the host's
// choice applies to both players. A rollback session predicts what the delay does not cover: Options'
// ONLINE NETCODE picks the delay relative to the game's own lockstep delay G for the ping (below):
//   PRIORITIZE RESPONSE G - 1, STANDARD G, PRIORITIZE GRAPHICS G + 1 (at least 1 frame),
//   CUSTOM its CUSTOM INPUT DELAY.
// MEASURED 2026-10-20 (two PCs, 16-33 ms ping, G = 3): delay 1 left both sides predicting 3.4 frames on
// average, up to 7 - the picture corrected several times a second; delay 3 predicted nothing.
// (Before: RESPONSE G - 2, STANDARD G - 1, GRAPHICS G.)
// TTT2_RB_DELAY=<frames> (tests) overrides it. Only with rollback sessions (TTT2_ROLLBACK=1).
// The game's own lockstep delay for a ping (sub_82667F88's table, 0x820DFB10..28 / 0x82006E8C):
// below 99 ms 3 frames, 133 -> 4, 166 -> 5, 183 -> 6, 240 -> 7, 300 -> 8, else 9.
int GameLockstepDelay(double rtt)
{
    return rtt < 99 ? 3 : rtt < 133 ? 4 : rtt < 166 ? 5 : rtt < 183 ? 6 : rtt < 240 ? 7 : rtt < 300 ? 8 : 9;
}

int SessionInputDelay(double rtt)
{
    static const int forced = [] { const char* e = std::getenv("TTT2_RB_DELAY"); return e && *e ? std::atoi(e) : -1; }();
    if (forced >= 0) return forced < 1 ? 1 : forced < 30 ? forced : 30;   // 0 is never applied (display_settings.h)
    // Relative to the game's lockstep delay G: NACE adds 2-3 frames of its own to the ping (MEASURED:
    // delay 1 at 130 ms -> 6.9 frames predicted on average, 8 the most, stalls at the ring's end).
    const int g = GameLockstepDelay(rtt);
    // The host's setting (its session record) when this instance joined; its own otherwise.
    int mode = display_settings::GetNetcode(), custom = display_settings::GetCustomDelay();
    if (const uint32_t host = XSessionLocalHosts() ? 0 : XSessionHostNetcode(); host & 0x80000000u)
    {
        mode = int(host & 0xFF);
        custom = std::clamp(int((host >> 8) & 0xFF), display_settings::kMinDelay, display_settings::kMaxCustomDelay);
    }
    switch (mode)
    {
    case display_settings::NetResponse: return (std::max)(g - 1, 1);
    case display_settings::NetGraphics: return g + 1;
    case display_settings::NetCustom: return custom;
    default: return g;
    }
}

extern "C" PPC_FUNC(__imp__sub_82667F88);
PPC_FUNC(sub_82667F88)
{
    __imp__sub_82667F88(ctx, base);
    if (!SessionEnabled()) return;
    const double rtt = ctx.f1.f64;
    const uint32_t game = ctx.r3.u32;
    ctx.r3.u64 = uint32_t(RoundPredicts() ? SessionInputDelay(rtt) : GameLockstepDelay(rtt));
    std::fprintf(stderr, "[rb-session] input delay %u frames (ping %.0f ms; lockstep would use %u; netcode setting %d)\n",
                 ctx.r3.u32, rtt, game, display_settings::GetNetcode());
}

// Diagnostics: NACE's connection timeouts. sub_82B3E840(conn) checks, every update, each connected
// peer's receive timer (entry +40: +4 start in seconds of the tick clock sub_82B37870, +8 running) and
// the host's; the host's timing out pushes event 6 (host lost) into the event queue (conn+392,
// sub_82B3C4E8), which the online code turns into message 0x1000000C and the battle into an abort.
// sub_82B47A80(conn, peer) pushes 6 (host) or 7 (another peer) when a peer is removed.
namespace {
thread_local int t_connCheck = 0;
thread_local uint32_t t_conn = 0;            // sub_82B3E840's connection object
double GuestDouble(uint32_t a)
{
    const uint64_t v = (uint64_t(GRead32(g_base, a)) << 32) | GRead32(g_base, a + 4);
    double d;
    std::memcpy(&d, &v, 8);
    return d;
}
float GuestFloat(uint32_t a)
{
    const uint32_t v = GRead32(g_base, a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
float NaceNow()   // sub_82B37870: the tick count times the constant at 0x8204D8A8
{
    const uint32_t bundle = GRead32(g_base, 0x82000720u);
    return bundle ? float(double(GRead32(g_base, bundle + 16)) * GuestDouble(0x8204D8A8u)) : 0.0f;
}
void Callers(char* line, size_t size, int& k, const PPCContext& ctx)
{
    k += std::snprintf(line + k, size - k, ", guest callers %08X", uint32_t(ctx.lr));
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < 10 && sp && sp < 0xFFFFFFF0u && k < int(size) - 12; ++i)
    {
        const uint32_t prev = GRead32(g_base, sp);
        if (!prev || prev <= sp || prev - sp > 0x10000) break;
        k += std::snprintf(line + k, size - k, " %08X", GRead32(g_base, prev - 8));
        sp = prev;
    }
}
}

extern "C" PPC_FUNC(__imp__sub_82B3E840);
PPC_FUNC(sub_82B3E840)
{
    ++t_connCheck;
    t_conn = ctx.r3.u32;
    __imp__sub_82B3E840(ctx, base);
    --t_connCheck;
}

extern "C" PPC_FUNC(__imp__sub_8267C590);
PPC_FUNC(sub_8267C590)
{
    // TTT2_RB_CONNTRACE=1: the timers' lines (the host's "times out" every 5 s by design - it is the
    // keepalive interval).
    static const bool on = [] { const char* e = std::getenv("TTT2_RB_CONNTRACE"); return e && *e == '1'; }();
    if (!on || !t_connCheck || !g_ownPump)
    {
        __imp__sub_8267C590(ctx, base);
        return;
    }
    const uint32_t timer = ctx.r3.u32;
    const bool host = (ctx.r5.u32 & 0xFF) != 0;
    const float timeout = float(ctx.f1.f64);
    const float start = GuestFloat(timer + 4);
    const uint8_t running = GRead8(base, timer + 8), flag9 = GRead8(base, timer + 9);
    __imp__sub_8267C590(ctx, base);
    const bool out = (ctx.r3.u32 & 0xFF) != 0;
    const float elapsed = NaceNow() - start;
    // the host's timer: every half second past 1 s (one line per timer and step), and every timeout
    static uint32_t lastTimer = 0;
    static int lastStep = 0;
    const int step = int(elapsed * 2.0f);
    const bool report = out || (host && elapsed > 1.0f && (timer != lastTimer || step != lastStep));
    if (!report) return;
    lastTimer = timer;
    lastStep = step;
    char line[512];
    int k = std::snprintf(line, sizeof(line), "[nace-conn] %s timer %08X (%s): elapsed %.3f s of %.3f (start %.3f, running %u/%u)%s, session frame %d%s",
                          host ? "host" : "peer", timer, RollbackIsRewound(timer + 4) ? "REWOUND" : "not rewound", elapsed, timeout, start,
                          running, flag9, out ? " TIMED OUT" : "", RollbackSessionFrame(), RollbackResimulating() ? ", re-simulating" : "");
    if (out && t_conn)
        k += std::snprintf(line + k, sizeof(line) - k, ", keepalives sent %d answered %d", R32(t_conn + 348), R32(t_conn + 352));
    if (out) Callers(line, sizeof(line), k, ctx);
    std::fprintf(stderr, "%s\n", line);
}

extern "C" PPC_FUNC(__imp__sub_82B3C4E8);
PPC_FUNC(sub_82B3C4E8)
{
    const uint32_t type = ctx.r4.u32 ? GRead32(base, ctx.r4.u32) : 0;
    if (g_ownPump && (type == 6 || type == 7))
    {
        char line[512];
        int k = std::snprintf(line, sizeof(line), "[nace-conn] event %u queued (%s) words %08X %08X %08X, session frame %d%s", type,
                              type == 6 ? "host lost" : "peer left", GRead32(base, ctx.r4.u32 + 4), GRead32(base, ctx.r4.u32 + 8),
                              GRead32(base, ctx.r4.u32 + 12), RollbackSessionFrame(), RollbackResimulating() ? ", re-simulating" : "");
        Callers(line, sizeof(line), k, ctx);
        std::fprintf(stderr, "%s\n", line);
    }
    __imp__sub_82B3C4E8(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82B47A80);
PPC_FUNC(sub_82B47A80)
{
    if (g_ownPump)
    {
        char line[512];
        int k = std::snprintf(line, sizeof(line), "[nace-conn] peer %u removed (host index %u), session frame %d%s", ctx.r4.u32,
                              GRead32(base, ctx.r3.u32 + 588), RollbackSessionFrame(), RollbackResimulating() ? ", re-simulating" : "");
        Callers(line, sizeof(line), k, ctx);
        std::fprintf(stderr, "%s\n", line);
    }
    __imp__sub_82B47A80(ctx, base);
}

// Diagnostics: the guest's keepalive to the host (conn vtable slot 0x82190B48, sub_82B493C0): +348
// counts the ones sent, a successful send (vfunc 68, type 20) restarts the host's receive timer.
extern "C" PPC_FUNC(__imp__sub_82B493C0);
PPC_FUNC(sub_82B493C0)
{
    static int calls = 0;
    const uint32_t conn = ctx.r3.u32;
    __imp__sub_82B493C0(ctx, base);
    if (!TraceOn() || (!g_ownPump && ++calls > 6)) return;
    char line[512];
    int k = std::snprintf(line, sizeof(line), "[nace-conn] keepalive %d (answered %d) %s, session frame %d%s", R32(conn + 348),
                          R32(conn + 352), (ctx.r3.u32 & 0xFF) ? "sent" : "NOT sent", RollbackSessionFrame(),
                          RollbackResimulating() ? ", re-simulating" : "");
    Callers(line, sizeof(line), k, ctx);
    std::fprintf(stderr, "%s\n", line);
}

// Diagnostics: an input delay request to the sync engine (sub_826F65A0(S, frames) -> S+5072).
extern "C" PPC_FUNC(__imp__sub_826F65A0);
PPC_FUNC(sub_826F65A0)
{
    const uint32_t s = ctx.r3.u32, want = ctx.r4.u32;
    const uint32_t node = R32(s + 4856) > 0 ? GRead32(base, s + 4828) : 0;
    const uint32_t peer = node ? GRead32(base, node + 736) : 0;
    const int32_t state = peer ? R32(peer) : -1;
    const uint8_t pending = GRead8(base, s + 5068);
    __imp__sub_826F65A0(ctx, base);
    char line[512];
    int k = std::snprintf(line, sizeof(line), "[nace-conn] input delay %u requested (session frame %d): node 0 state %d, already pending %u -> %s",
                          want, RollbackSessionFrame(), state, pending,
                          GRead8(base, s + 5068) && R32(s + 5072) == int32_t(want) && !pending ? "queued" : "dropped");
    Callers(line, sizeof(line), k, ctx);
    std::fprintf(stderr, "%s\n", line);
}

// Diagnostics: the sync engine starting a queued delay change (sub_826FA880: S+5128 -> 4, S+5132 the
// delay, S+5076 / 5080 its frame and value) - which side, when.
extern "C" PPC_FUNC(__imp__sub_826FA880);
PPC_FUNC(sub_826FA880)
{
    const uint32_t s = ctx.r3.u32;
    const int32_t before = R32(s + 5128);
    __imp__sub_826FA880(ctx, base);
    if (before != 4 && R32(s + 5128) == 4)
        std::fprintf(stderr, "[nace-conn] input delay change to %d started (from frame %d; in use %d), session frame %d\n", R32(s + 5132),
                     R32(s + 5076), R32(s + 5640), RollbackSessionFrame());
}

// Diagnostics: the host's answer to a keepalive (sub_82B49210(conn, packet): conn+352 = the echoed
// keepalive number, packet+7), on the guest.
extern "C" PPC_FUNC(__imp__sub_82B49210);
PPC_FUNC(sub_82B49210)
{
    const uint32_t conn = ctx.r3.u32, packet = ctx.r4.u32;
    const int32_t before = R32(conn + 352);
    __imp__sub_82B49210(ctx, base);
    static int calls = 0;
    if (!TraceOn() || (!g_ownPump && ++calls > 6)) return;
    std::fprintf(stderr, "[nace-conn] keepalive answer %d received (answered %d -> %d, conn %08X: %s), session frame %d%s\n",
                 packet ? R32(packet + 7) : -1, before, R32(conn + 352), conn, RollbackIsRewound(conn + 352) ? "REWOUND" : "not rewound",
                 RollbackSessionFrame(), RollbackResimulating() ? ", re-simulating" : "");
}

// Diagnostics: NACE's object deletes. sub_82B3A8E8(owner) deletes every element of the std::deque at
// owner+152 (+164 start, +168 size) through sub_82B54210(obj): obj->vfunc 0 (destructor), NACE's free.
// A null vtable there is an object deleted twice (MEASURED once on the host right after a rewind,
// with NACE's blocks in the arena): which owner, is its deque rewound, re-simulating?
uint32_t RollbackArenaSize(uint32_t a);    // rollback.cpp
extern "C" PPC_FUNC(__imp__sub_82B3A8E8);
PPC_FUNC(sub_82B3A8E8)
{
    const uint32_t owner = ctx.r3.u32;
    const int32_t size = R32(owner + 168);
    if (g_ownPump && size > 0)
    {
        static int lines = 0;
        if (lines++ < 40)
        {
            char line[512];
            int k = std::snprintf(line, sizeof(line), "[nace-conn] deque at %08X+152 cleared: %d elements from %d (owner %s, %s), session frame %d%s",
                                  owner, size, R32(owner + 164), RollbackArenaSize(owner) ? "in the arena" : "not in the arena",
                                  RollbackIsRewound(owner + 168) ? "REWOUND" : "not rewound", RollbackSessionFrame(),
                                  RollbackResimulating() ? ", re-simulating" : "");
            Callers(line, sizeof(line), k, ctx);
            std::fprintf(stderr, "%s\n", line);
        }
    }
    __imp__sub_82B3A8E8(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82B54210);
PPC_FUNC(sub_82B54210)
{
    const uint32_t obj = ctx.r3.u32;
    if (!obj || !GRead32(base, obj))
    {
        char line[512];
        int k = std::snprintf(line, sizeof(line), "[nace-conn] NACE object %08X deleted twice? (vtable 0; arena size %X, %s), session frame %d%s",
                              obj, obj ? RollbackArenaSize(obj) : 0, obj && RollbackIsRewound(obj) ? "REWOUND" : "not rewound", RollbackSessionFrame(),
                              RollbackResimulating() ? ", re-simulating" : "");
        Callers(line, sizeof(line), k, ctx);
        std::fprintf(stderr, "%s\n", line);
    }
    __imp__sub_82B54210(ctx, base);
}

// ---- the local input queue ---------------------------------------------------------------------
// NACE adds a node's next input frame through sub_826F40B8(node, limit, input) (refused when newest + 1
// >= limit); its clock-driven pacer (sub_826F9E10 -> sub_826F9108) keeps adding local frames while the
// battle stalls, so a stall lengthens the local input delay for good. MEASURED in a real two-PC match:
// while the other side froze 2.6 s the local queue ran 31 frames ahead of the simulation, and in the
// round it stayed at simulation + 6 with the delay at 2. In a session the local queue ends at
// simulation + delay + 1 - NACE's own steady state (newest = record frame + delay + 1, MEASURED in
// lockstep) - whatever the clock says.
extern "C" PPC_FUNC(__imp__sub_826F40B8);
PPC_FUNC(sub_826F40B8)
{
    static const bool capOn = [] { const char* e = std::getenv("TTT2_RB_INPUTCAP"); return !(e && *e == '0'); }();   // tests: 0 = off
    if (capOn && g_ownPump && RollbackSessionOn() && R32(kSync + 4856) > 0 && ctx.r3.u32 == GRead32(base, kSync + 4828))
    {
        const int32_t limit = RollbackSessionLast() + 1 + R32(kSync + 5640) + 2;
        if (int32_t(ctx.r4.u32) < 0 || int32_t(ctx.r4.u32) > limit) ctx.r4.u64 = uint32_t(limit);
    }
    __imp__sub_826F40B8(ctx, base);
}

// ---- time sync ------------------------------------------------------------------------------------
// Two instances drift apart in rounds whenever one of them loses frames (MEASURED in a real two-PC
// match: a cold shader cache cost one side 10-40 % of its frames in places; the other ran ~10 frames
// ahead, predicted at the ring's 8-frame end and stalled there - deep, costly rewinds). Lockstep made
// both wait for the slower one; a session has to do it itself (GGPO's time sync). Each instance
// measures how far it simulates past the other's newest input (depth = next frame - remote newest,
// averaged) and sends it every 2 updates (an xsocket TimeSync packet). With depth_A = offset + latency
// - delay and depth_B = -offset + latency - delay, (depth_A - depth_B) / 2 is A's lead without the
// ping in it. The other side's depth is from before it saw this side's latest yields: each yield it
// has not seen yet (the report echoes the newest of our reports it had) counts as seen.
// The leader yields frames (rollback.cpp SessionStep: the update starts a frame late) at a rate of
// 0.6 x (lead - 0.5) per update plus an integral term (a side that keeps losing frames needs a steady
// rate - without it the leader settled ~1 frame further ahead), at most one update in 4.
// MEASURED (tssim.py model, then the two-instance runs): the old fixed spacing (a yield, then 2-8
// updates without) left the leader 3 frames ahead of a side losing 23 % of its frames.
// Every yield shows a frame twice, so only a lead that costs something is worked off: this side
// predicts more than a frame (its rewinds) and leads by more than a frame. Yields that do not bring the
// lead down stop for 10 s (three times in a session: for the rest of it). MEASURED 2026-10-20 (two
// PCs, the delay covering the ping): one side read 0.9 frames ahead for a whole battle (depth 0.0 here,
// -1.8 there) and yielded every other update - 1707 yields, 1381 updates over 20 ms - with the lead
// unchanged; the other side never noticed.
// Only while rounds predict (lockstep keeps both in step by itself). TTT2_RB_TIMESYNC=0: off.
// How the leader gives frames up: a yield (the update starting 16 ms late) does not slow the game - its
// frame loop paces itself to the fixed 60 Hz vblank schedule (kernel_video.cpp's VideoTick) and runs the
// next frame at once to catch up, so a yield only showed a frame twice (MEASURED 2026-10-21, real two-PC
// LAN session: 13 yields in 120 updates while the other side advanced 116 frames and the lead stayed at
// 6.1 frames; the effect check then gave up for the rest of the match and the leader rewound 2492 times,
// up to 8 frames deep). The vblank schedule itself is stretched instead (VideoSetVblankStretch): the same
// rate, as a fraction of a frame per vblank, at most kTsMaxStretch (kTsMaxStretchFar far ahead) - every frame a little later, none
// shown twice, and nothing to catch up. TTT2_RB_TSMODE=yield: the old way (comparisons).
// Yields that do not bring the lead down still stop for 10 s - never for the whole session any more.
bool RollbackTimeSyncSend(const void* data, size_t size);   // xsocket.cpp
void VideoSetVblankStretch(int32_t us);                     // kernel_video.cpp
namespace {
constexpr uint32_t kTimeSyncMagic = 0x54535943u;           // "TSYC"
struct TimeSyncReport { uint32_t magic; int32_t depth256; int32_t frame; uint32_t seq; uint32_t echo; };
constexpr double kTsAlpha = 0.3, kTsGain = 0.6, kTsIntegral = 0.005, kTsMaxRate = 0.25;
constexpr double kTsMaxStretch = 0.06;                     // of a frame per vblank: at most 6 % slower,
constexpr double kTsMaxStretchFar = 0.15;                  // 15 % while more than kTsFarLead frames ahead (deep
constexpr double kTsFarLead = 3.0;                         // rewinds there cost more than a slower clock shows)
// What a correction is for (above): a yield shows a frame twice, so only a lead over a frame that costs
// rewinds (this side predicting over a frame) was worked off. A stretched clock shows nothing twice (a
// 60 Hz display with VSYNC on repeats a frame now and then while it runs), so the stretch works off a lead
// over half a frame: MEASURED 2026-10-22 (real two-PC session, community server, 31 ms ping, PRIORITIZE
// RESPONSE = delay 2): one side about a frame ahead for whole matches, the other side's inputs a frame late -
// 714-847 one-frame corrections a match on the leader (1-27 on the other side), the picture snapping ~4 times
// a second. TTT2_RB_TSLEAD=<min lead>:<min depth>:<target> (tests) overrides the stretch mode's values.
bool TsStretch();                                          // below
struct TsLimits { double minLead, minDepth, target; };
TsLimits TsLimit()
{
    static const TsLimits stretch = [] {
        TsLimits v{ 0.5, 0.5, 0.25 };
        if (const char* e = std::getenv("TTT2_RB_TSLEAD")) std::sscanf(e, "%lf:%lf:%lf", &v.minLead, &v.minDepth, &v.target);
        return v;
    }();
    static const TsLimits yield{ 1.0, 1.0, 0.5 };
    return TsStretch() ? stretch : yield;
}
constexpr int kTsWindow = 120, kTsBackoff = 600;
constexpr double kTsWindowYields = 8, kTsWindowStretched = 3;   // frames given up in a window before it is judged
constexpr int kTsSendEvery = 2;
bool TsStretch()
{
    static const bool v = [] { const char* e = std::getenv("TTT2_RB_TSMODE"); return !(e && std::strcmp(e, "yield") == 0); }();
    return v;
}
std::atomic<int32_t> g_tsRemote{ INT32_MIN };              // the other side's depth x 256
std::atomic<int64_t> g_tsRemoteMs{ 0 };
std::atomic<int32_t> g_tsRemoteFrame{ -1 };                // the other side's next frame, from its newest report
std::atomic<uint32_t> g_tsRemoteEcho{ 0 };                 // the newest of our reports it had then
std::atomic<uint32_t> g_tsRemoteSeq{ 0 };                  // its newest report (echoed in ours)
double g_tsDepth = 0, g_tsInteg = 0, g_tsBucket = 0;
bool g_tsHave = false;
int g_tsSendIn = 0;
uint32_t g_tsSeq = 0;
uint64_t g_tsYields = 0;                                   // yields (TTT2_RB_TSMODE=yield)
double g_tsGiven = 0;                                      // frames given up: yields, or stretched vblank time
double g_tsSentGiven[64] = {};                             // g_tsGiven when report seq (& 63) left
int g_tsWindowLeft = 0;                                    // the effect check: updates left in the window
double g_tsWindowGiven = 0;                                // frames given up in the window
double g_tsWindowLead = 0;                                 // the lead when the window started
int g_tsQuiet = 0, g_tsBackoffs = 0;                       // updates without yields left; back-offs this session
double g_tsStretch = 0;                                    // the vblank stretch now (fraction of a frame)
uint64_t g_tsStretchLogged = 0;
int32_t g_tsWindowRemote = -1;                             // the other side's frame when the window started
int64_t g_tsWindowMs = 0;                                  // and the time
int64_t TsNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool TimeSyncOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_RB_TIMESYNC"); return !(e && *e == '0'); }();
    return on;
}
} // namespace

namespace {
// The vblank stretch (stretch mode): set only when it changes.
void TsSetStretch(double s)
{
    if (s == g_tsStretch) return;
    g_tsStretch = s;
    VideoSetVblankStretch(int32_t(s * 16667.0));
}
void TimeSyncReset()
{
    g_tsHave = false;
    g_tsInteg = g_tsBucket = 0;
    g_tsSendIn = 0;
    g_tsRemote = INT32_MIN;
    g_tsRemoteEcho = 0;
    g_tsWindowLeft = 0;
    g_tsWindowGiven = 0;
    g_tsQuiet = g_tsBackoffs = 0;
    g_tsRemoteFrame = -1;
    g_tsWindowRemote = -1;
    TsSetStretch(0);
    g_tsGiven = 0;
    for (double& v : g_tsSentGiven) v = 0;
}
// A session's end: the clock back to 60 Hz (the frames given up stay for the session report).
void TimeSyncStop() { TsSetStretch(0); }
} // namespace
// rollback.cpp's session report: frames given up this session (yields, or stretched vblank time).
double SessionTimeSyncGiven() { return g_tsGiven; }
namespace {
} // namespace

// xsocket.cpp, the network thread: the other side's report.
void RollbackTimeSyncReceive(const uint8_t* data, size_t size)
{
    TimeSyncReport r;
    if (size < sizeof(r)) return;
    std::memcpy(&r, data, sizeof(r));
    if (r.magic != kTimeSyncMagic) return;
    g_tsRemote = r.depth256;
    g_tsRemoteEcho = r.echo;
    g_tsRemoteSeq = r.seq;
    g_tsRemoteMs = TsNowMs();
    g_tsRemoteFrame = r.frame;
}

// rollback.cpp's SessionStep, before the pump: true = this update yields a frame (yield mode; the stretch
// mode slows the vblank schedule instead and never yields).
bool SessionYield()
{
    if (!TimeSyncOn() || !RoundPredicts() || R32(kSync + 4856) < 2)
    {
        g_tsHave = false;
        g_tsInteg = g_tsBucket = 0;
        TsSetStretch(0);
        return false;
    }
    int32_t remoteNewest, remoteCurrent;
    NodeFrames(1, remoteNewest, remoteCurrent);
    const int32_t next = RollbackSessionLast() + 1;
    if (remoteNewest < 0 || next <= 0) { TsSetStretch(0); return false; }
    const double depth = double(next - remoteNewest);
    g_tsDepth = g_tsHave ? g_tsDepth * (1 - kTsAlpha) + depth * kTsAlpha : depth;
    g_tsHave = true;
    if (--g_tsSendIn <= 0)
    {
        ++g_tsSeq;
        g_tsSentGiven[g_tsSeq & 63] = g_tsGiven;
        const TimeSyncReport r{ kTimeSyncMagic, int32_t(g_tsDepth * 256.0), next, g_tsSeq, g_tsRemoteSeq.load() };
        RollbackTimeSyncSend(&r, sizeof(r));
        g_tsSendIn = kTsSendEvery;
    }
    const int32_t remote256 = g_tsRemote.load();
    if (remote256 == INT32_MIN || TsNowMs() - g_tsRemoteMs.load() > 1000)
    {
        g_tsBucket = 0;
        TsSetStretch(0);
        return false;
    }
    double remote = remote256 / 256.0;
    const uint32_t echo = g_tsRemoteEcho.load();
    if (echo && echo <= g_tsSeq && g_tsSeq - echo < 64) remote += g_tsGiven - g_tsSentGiven[echo & 63];
    const double lead = (g_tsDepth - remote) / 2.0;
    // The effect check: a window of updates with frames given up in it must end with the lead lower than at
    // the first - unless the other side lost frames meanwhile (it advanced less than 60 Hz would in that
    // time): then giving them up kept the two level, which is its job. The time is the clock's, not these
    // updates' (a stretched vblank makes them slower: counted against them, a side 3 % slow looked like one at
    // full speed - MEASURED, and the time sync stopped while it was working).
    const int32_t remoteFrame = g_tsRemoteFrame.load();
    const double windowNeed = TsStretch() ? kTsWindowStretched : kTsWindowYields;
    if (g_tsWindowLeft <= 0)
    {
        g_tsWindowLeft = kTsWindow;
        g_tsWindowGiven = 0;
        g_tsWindowLead = lead;
        g_tsWindowRemote = remoteFrame;
        g_tsWindowMs = TsNowMs();
    }
    else if (--g_tsWindowLeft == 0 && g_tsWindowGiven >= windowNeed && lead > g_tsWindowLead - 0.25 &&
             g_tsWindowRemote >= 0 && remoteFrame - g_tsWindowRemote >= int32_t(double(TsNowMs() - g_tsWindowMs) / 16.667 * 0.99))
    {
        ++g_tsBackoffs;
        g_tsQuiet = kTsBackoff;
        std::fprintf(stderr, "[rb-session] time sync: %.1f frames given up in %d updates left the lead at %.1f frames (%.1f at the first; "
                             "the other side advanced %d frames) - none given up for 10 s (%d times this session)\n",
                     g_tsWindowGiven, kTsWindow, lead, g_tsWindowLead, remoteFrame - g_tsWindowRemote, g_tsBackoffs);
    }
    if (g_tsQuiet > 0)
    {
        --g_tsQuiet;
        g_tsInteg = g_tsBucket = 0;
        TsSetStretch(0);
        return false;
    }
    const TsLimits lim = TsLimit();
    if (g_tsDepth < lim.minDepth || lead < lim.minLead)
    {
        g_tsInteg = (std::max)(0.0, g_tsInteg - kTsIntegral);
        g_tsBucket = 0;
        TsSetStretch(0);
        return false;
    }
    const double e = lead - lim.target;
    g_tsInteg = g_tsInteg + kTsIntegral * e;
    if (g_tsInteg < 0) g_tsInteg = 0;
    if (g_tsInteg > kTsMaxRate) g_tsInteg = kTsMaxRate;
    double rate = kTsGain * e + g_tsInteg;
    if (rate > kTsMaxRate) rate = kTsMaxRate;
    if (rate <= 0)
    {
        g_tsBucket = 0;
        TsSetStretch(0);
        return false;
    }
    if (TsStretch())
    {
        const double s = (std::min)(rate, lead > kTsFarLead ? kTsMaxStretchFar : kTsMaxStretch);
        const bool starting = g_tsStretch == 0;
        if (g_tsWindowGiven == 0) g_tsWindowLead = lead;     // the effect check's reference
        TsSetStretch(s);
        g_tsGiven += s;
        g_tsWindowGiven += s;
        if ((starting && g_tsStretchLogged < 30) || ++g_tsStretchLogged % 600 == 0)
        {
            if (starting) ++g_tsStretchLogged;
            std::fprintf(stderr, "[rb-session] time sync: %.1f frames ahead (depth %.1f here, %.1f there), rate %.2f (integral %.3f) - the clock %.1f %% slower at %d (%.1f frames given up so far)\n",
                         lead, g_tsDepth, remote, rate, g_tsInteg, s * 100.0, next, g_tsGiven);
        }
        return false;
    }
    g_tsBucket += rate;
    if (g_tsBucket < 1.0) return false;
    g_tsBucket -= 1.0;
    if (g_tsWindowGiven == 0) g_tsWindowLead = lead;         // the effect check's reference
    g_tsWindowGiven += 1;
    g_tsGiven += 1;
    if (g_tsYields++ < 30 || g_tsYields % 100 == 0)
        std::fprintf(stderr, "[rb-session] time sync: %.1f frames ahead (depth %.1f here, %.1f there), rate %.2f (integral %.3f) - a frame yielded at %d (%llu so far)\n",
                     lead, g_tsDepth, remote, rate, g_tsInteg, next, (unsigned long long)g_tsYields);
    return true;
}
