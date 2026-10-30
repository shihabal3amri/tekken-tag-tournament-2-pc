// The game's sound across rollbacks. The game talks to its sound middleware through one API block
// (0x82B1B0B0-0x82B1CBD8, 52 entry points; MEASURED with TTT2_SND_CENSUS / TTT2_SND_TRACE: per frame
// a few updates and ~216 state queries; per sound event an object lookup (B8D0), a variable set
// (C0C8) and a start (B968) returning an instance in the sound arena; instances end by themselves).
// The sound engine runs on its own thread and is never rewound (rollback.cpp), so its calls must not
// be replayed twice: during live fight frames every call is recorded (arguments, result, the words
// its stack out-pointers r5 / r6 hold afterwards); a re-simulated frame's call that the original
// frame made too - same function, same arguments - is not made again, it returns what the original
// returned. Calls the original did not make are new in the corrected timeline and run (a sound then
// starts late by the rollback's depth instead of never). Main thread only.
#include "kernel_shared.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

bool RollbackIsRewound(uint32_t addr);   // rollback.cpp: a word a rewind restores

namespace {
constexpr int kCount = 52;
const uint32_t kAddr[kCount] = { 0x82B1B0B0, 0x82B1B128, 0x82B1B140, 0x82B1B158, 0x82B1B170, 0x82B1B1E8, 0x82B1B260, 0x82B1B2A0, 0x82B1B2F0, 0x82B1B388, 0x82B1B420, 0x82B1B4C0, 0x82B1B570, 0x82B1B5F8, 0x82B1B680, 0x82B1B710, 0x82B1B7A0, 0x82B1B830, 0x82B1B8C0, 0x82B1B8C8, 0x82B1B8D0, 0x82B1B900, 0x82B1B950, 0x82B1B968, 0x82B1BA68, 0x82B1BB50, 0x82B1BBB0, 0x82B1BBF0, 0x82B1BC78, 0x82B1BD00, 0x82B1BD88, 0x82B1BE08, 0x82B1BE70, 0x82B1BED8, 0x82B1BFB8, 0x82B1C050, 0x82B1C0C8, 0x82B1C140, 0x82B1C1B0, 0x82B1C208, 0x82B1C268, 0x82B1C2D0, 0x82B1C338, 0x82B1C390, 0x82B1C3E8, 0x82B1C440, 0x82B1C498, 0x82B1C500, 0x82B1CA40, 0x82B1CAC8, 0x82B1CB50, 0x82B1CBD8 };
// The argument registers each takes (bit k = r(3+k)): live at its entry - read before written on some
// path, by it or by a callee that takes them (liveness over the call tree; the engine's indirect calls
// take what their call sites set). Other registers hold the caller's leftovers and are not compared
// (MEASURED: comparing them made re-simulated handle releases (B968, r3 only) look new - released twice).
const uint8_t kArgs[kCount] = { 31, 3, 1, 1, 3, 0, 0, 1, 7, 7, 7, 7, 15, 15, 3, 3, 3, 1, 0, 0, 1, 1, 1, 1, 1, 3, 3, 1, 1, 1, 3, 3, 1, 7, 3, 7, 7, 3, 1, 15, 3, 3, 1, 1, 1, 1, 1, 3, 1, 15, 15, 3 };
constexpr int kRing = 32;

struct Call
{
    uint8_t fn;
    bool used;
    bool out5, out6;
    uint32_t r3, r4, r5, r6, r7, lr, result, v5, v6;
    std::vector<std::pair<uint32_t, uint8_t>> writes;     // the bytes it wrote into game objects
    uint32_t created = 0;                                  // the handle it stored in its out object
    uint32_t released = 0;                                 // B968: the handle whose reference it dropped
    // What it would have done to instances, held until the frame is final (SoundRollbackFinal):
    // reference drops (sub_82B1C6B0, false) and 'rele' posts (sub_82B1D318, true).
    std::vector<std::pair<uint32_t, bool>> deferred;
};
struct FrameCalls { uint64_t frame = ~0ull; std::vector<Call> calls; size_t cursor = 0; };
FrameCalls g_frames[kRing];
FrameCalls* g_rec = nullptr;          // live frame being recorded
FrameCalls* g_rep = nullptr;          // re-simulated frame being replayed
DWORD g_main = 0;
uint64_t g_replayed = 0, g_executed = 0;
// Calls the API makes into itself (sub_82B1BAC0 releasing an object's old handle through B968, ...)
// run as they are: only the game's own calls are recorded and answered (an outer call that runs for
// real must not have its inner calls answered from the record).
int g_depth = 0;
// Inside the sound library (any thread): its writes are the sound engine's - the pages they open stay
// the engine's, never rewound (rollback.cpp's page attribution). MEASURED: the engine's own allocator
// and objects (no game-heap calls) sat in pages the main thread's API calls had turned rewound; a
// restore set them back under the running engine - virtual calls through vtables that had become
// strings ("Syst", "undS"), lr 0x82B22440-0x82B32D10.
thread_local int t_inSound = 0;
// ... except into the game objects the call was handed (pointer arguments r3-r6: a sound object's
// handle / owner / flags at +4 / +8 / +16 live in game memory - MEASURED: BA68's flag write turned two
// pages of game objects "the engine's", never rewound, and the guest died in the next rewind).
thread_local uint32_t t_argPages[24];
thread_local int t_argCount = 0;
template <class F>
void SoundCall(F original, PPCContext& ctx, uint8_t* base)
{
    const int saved = t_argCount;
    for (uint32_t p : { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32 })
        if (p >= 0x00100000u && p < 0x40000000u && t_argCount < 22)
        {
            t_argPages[t_argCount++] = p >> 12;
            t_argPages[t_argCount++] = (p + 31) >> 12;
        }
    ++t_inSound;
    original(ctx, base);
    --t_inSound;
    t_argCount = saved;
}

// Deferred destruction. The engine destroys an instance once nothing holds it (reference count +28)
// or when it is posted a 'rele'; the game's handles to it live in rewound memory. A frame that can
// still be rolled back must not let the engine destroy anything: the timeline that replaces it may
// still use the instance (MEASURED: B9D0's virtual call through an instance whose vtable the engine had
// cleared - a release made by a discarded frame, the restored game still holding the handle). While a
// recorded or replayed call of a fight frame runs for real (main thread), the reference drops and
// 'rele' posts it makes - its own and those of the API's calls into itself - are held on its record;
// they happen when the frame is final, and are forgotten with the call if a rewind discards it.
bool g_collect = false;
std::vector<std::pair<uint32_t, bool>> g_collected;
uint64_t g_deferred = 0, g_deferApplied = 0, g_deferDropped = 0;
template <class F>
void CollectCall(F original, PPCContext& ctx, uint8_t* base)
{
    const bool outer = !g_collect;
    g_collect = true;
    SoundCall(original, ctx, base);
    if (outer) g_collect = false;
}
extern "C" PPC_FUNC(__imp__sub_82B1D318);
void ApplyDeferred(PPCContext& ctx, uint8_t* base, Call& c)
{
    for (const auto& [h, rele] : c.deferred)
    {
        if (rele)
        {
            PPCContext m = ctx;
            m.r3.u64 = h;
            m.r4.u64 = 0x72656C65;                       // 'rele'
            __imp__sub_82B1D318(m, base);
        }
        else
        {
            const uint32_t refs = GRead32(base, h + 28);
            if (refs) GWrite32(base, h + 28, refs - 1);
        }
        ++g_deferApplied;
    }
    c.deferred.clear();
}

// Handles (an instance in the sound arena, its reference count at +28): sub_82B1BAC0 stores a new one in
// a game object (+4) and takes a reference (sub_82B1C6A0); B968 drops it (sub_82B1C6B0) and clears +4;
// BA68 posts the instance a 'rele' (sub_82B1D318). Creating calls and the object they fill: B830 r3,
// C0C8 r5, CAC8 r6.
int IndexOf(uint32_t addr) { for (int i = 0; i < kCount; ++i) if (kAddr[i] == addr) return i; return -1; }
const int kB830 = IndexOf(0x82B1B830), kC0C8 = IndexOf(0x82B1C0C8), kCAC8 = IndexOf(0x82B1CAC8), kB968 = IndexOf(0x82B1B968);
uint32_t OutObject(int i, uint32_t r3, uint32_t r5, uint32_t r6) { return i == kB830 ? r3 : i == kC0C8 ? r5 : i == kCAC8 ? r6 : 0; }
bool Handle(uint32_t h) { return h >= 0x00100000u && h < 0x40000000u && (h & 3) == 0; }

// The main thread's guest stack (thread block at 0x40000000): out-pointers into it are the call's
// results; anything else may be live game state and is never written back.
bool OnStack(uint32_t a) { return a >= 0x40000E90u && a < 0x40040E90u; }

// The game-side objects a call writes - a sound object's handle / owner / flags (B8D0 constructs one,
// B830 / CAC8 store a new handle through sub_82B1BAC0, B968 / BA68 clear or flag it), an "out" object:
// pointer arguments into memory a rewind restores, 32 bytes each, compared before and after the call.
// A re-simulated call answered from the record writes the same bytes again, so the corrected
// timeline's objects agree with the engine (without them a re-simulated frame kept handles the
// original had released - released a second time later).
struct Before { uint32_t addr; uint8_t bytes[32]; };
int CaptureBefore(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, Before (&out)[4])
{
    int n = 0;
    for (uint32_t p : { r3, r4, r5, r6 })
    {
        if (p < 0x00100000u || p > 0xFFFFFF00u || OnStack(p) || !RollbackIsRewound(p) || !RollbackIsRewound(p + 31)) continue;
        bool seen = false;
        for (int k = 0; k < n; ++k) seen |= out[k].addr == p;
        if (seen) continue;
        out[n].addr = p;
        std::memcpy(out[n].bytes, g_base + p, 32);
        ++n;
    }
    return n;
}
void CaptureWrites(const Before (&before)[4], int n, std::vector<std::pair<uint32_t, uint8_t>>& writes)
{
    for (int k = 0; k < n; ++k)
        for (uint32_t j = 0; j < 32; ++j)
            if (g_base[before[k].addr + j] != before[k].bytes[j]) writes.push_back({ before[k].addr + j, g_base[before[k].addr + j] });
}

// Census (TTT2_SND_CENSUS=1): calls per function and frames with calls, every 300 frames.
uint32_t g_calls[kCount], g_framesWith[kCount];
bool g_thisFrame[kCount];
bool CensusOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_SND_CENSUS"); return e && *e == '1'; }();
    return on;
}

template <class F>
void Hook(int i, PPCContext& ctx, uint8_t* base, F original)
{
    if (GetCurrentThreadId() != g_main || !(g_rec || g_rep) || g_depth)
    {
        SoundCall(original, ctx, base);
        return;
    }
    if (CensusOn()) { ++g_calls[i]; g_thisFrame[i] = true; }
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32;
    if (g_rep)
    {
        const uint8_t m = kArgs[i];
        const uint32_t lr = uint32_t(ctx.lr);
        auto same = [&](const Call& c) {
            return !c.used && c.fn == i && c.lr == lr && (!(m & 1) || c.r3 == r3) && (!(m & 2) || c.r4 == r4) &&
                   (!(m & 4) || c.r5 == r5) && (!(m & 8) || c.r6 == r6) && (!(m & 16) || c.r7 == r7);
        };
        std::vector<Call>& calls = g_rep->calls;
        Call* hit = nullptr;
        if (g_rep->cursor < calls.size() && same(calls[g_rep->cursor])) hit = &calls[g_rep->cursor++];
        else
            for (Call& c : calls)
                if (same(c)) { hit = &c; break; }
        if (hit)
        {
            hit->used = true;
            if (hit->out5) GWrite32(base, r5, hit->v5);
            if (hit->out6) GWrite32(base, r6, hit->v6);
            for (const auto& [a, v] : hit->writes) base[a] = v;
            ctx.r3.u64 = hit->result;
            ++g_replayed;
            return;
        }
        ++g_executed;
        if (g_executed <= 30)                          // diagnostics: a re-simulated call without a match
        {
            const Call* site = nullptr;
            for (const Call& c : calls) if (c.fn == i && c.lr == lr) { site = &c; break; }
            std::fprintf(stderr, "[snd-rb] new %08X(%08X %08X %08X %08X) lr=%08X frame calls %zu%s", kAddr[i], r3, r4, r5, r6, lr,
                         calls.size(), site ? "" : ", none from this site");
            if (site) std::fprintf(stderr, "; first from this site (%08X %08X %08X %08X)%s", site->r3, site->r4, site->r5, site->r6, site->used ? " used" : "");
            std::fprintf(stderr, "\n");
        }
        Before before[4];
        const int captured = CaptureBefore(r3, r4, r5, r6, before);
        const uint32_t out = OutObject(i, r3, r5, r6);
        const uint32_t outBefore = out && RollbackIsRewound(out + 4) ? GRead32(base, out + 4) : 0;
        const uint32_t releasing = i == kB968 && RollbackIsRewound(r3 + 4) ? GRead32(base, r3 + 4) : 0;
        ++g_depth;
        CollectCall(original, ctx, base);
        --g_depth;
        // It reached the engine: a later rollback into this frame must find it.
        Call c{ uint8_t(i), true, OnStack(r5), OnStack(r6), r3, r4, r5, r6, r7, lr, ctx.r3.u32, 0, 0 };
        if (c.out5) c.v5 = GRead32(base, r5);
        if (c.out6) c.v6 = GRead32(base, r6);
        CaptureWrites(before, captured, c.writes);
        if (out && RollbackIsRewound(out + 4) && GRead32(base, out + 4) != outBefore && Handle(GRead32(base, out + 4))) c.created = GRead32(base, out + 4);
        if (Handle(releasing)) c.released = releasing;
        c.deferred.swap(g_collected);
        g_collected.clear();
        g_rep->calls.push_back(std::move(c));
        return;
    }
    const uint32_t lr = uint32_t(ctx.lr);
    Before before[4];
    const int captured = CaptureBefore(r3, r4, r5, r6, before);
    const uint32_t out = OutObject(i, r3, r5, r6);
    const uint32_t outBefore = out && RollbackIsRewound(out + 4) ? GRead32(base, out + 4) : 0;
    const uint32_t releasing = i == kB968 && RollbackIsRewound(r3 + 4) ? GRead32(base, r3 + 4) : 0;
    ++g_depth;
    CollectCall(original, ctx, base);
    --g_depth;
    Call c{ uint8_t(i), false, OnStack(r5), OnStack(r6), r3, r4, r5, r6, r7, lr, ctx.r3.u32, 0, 0 };
    if (c.out5) c.v5 = GRead32(base, r5);
    if (c.out6) c.v6 = GRead32(base, r6);
    CaptureWrites(before, captured, c.writes);
    if (out && RollbackIsRewound(out + 4) && GRead32(base, out + 4) != outBefore && Handle(GRead32(base, out + 4))) c.created = GRead32(base, out + 4);
    if (Handle(releasing)) c.released = releasing;
    c.deferred.swap(g_collected);
    g_collected.clear();
    g_rec->calls.push_back(std::move(c));
}

extern "C" PPC_FUNC(__imp__sub_82B1C6B0);
PPC_FUNC(sub_82B1C6B0)
{
    if (g_collect && GetCurrentThreadId() == g_main && Handle(ctx.r3.u32))
    {
        g_collected.push_back({ ctx.r3.u32, false });
        ++g_deferred;
        return;
    }
    __imp__sub_82B1C6B0(ctx, base);
}
PPC_FUNC(sub_82B1D318)
{
    if (g_collect && GetCurrentThreadId() == g_main && ctx.r4.u32 == 0x72656C65 && Handle(ctx.r3.u32))
    {
        g_collected.push_back({ ctx.r3.u32, true });
        ++g_deferred;
        ctx.r3.u64 = 0;                                   // queued (sub_82B1D228's success)
        return;
    }
    __imp__sub_82B1D318(ctx, base);
}
} // namespace

bool SoundLibraryWriting(uint32_t page)                // rollback.cpp's page attribution
{
    if (t_inSound <= 0) return false;
    for (int i = 0; i < t_argCount; ++i) if (t_argPages[i] == page >> 12) return false;
    return true;
}

// rollback.cpp, after re-simulating frames [from, to]: the calls the discarded timeline made and the
// corrected one did not are settled and forgotten (a later rewind must never answer from a timeline
// that is gone). A sound it started is told to release ('rele', as BA68 does - a loop would never stop
// otherwise) and its reference dropped (as B968 does); a reference it dropped is taken again (the
// corrected timeline still holds the handle and drops it itself later).
extern "C" PPC_FUNC(__imp__sub_82B1D318);
uint64_t g_settledStops = 0, g_settledRefs = 0;
void SoundRollbackSettle(PPCContext& ctx, uint8_t* base, uint64_t from, uint64_t to)
{
    g_rec = g_rep = nullptr;
    for (uint64_t frame = from; frame <= to; ++frame)
    {
        FrameCalls& f = g_frames[frame % kRing];
        if (f.frame != frame) continue;
        std::vector<Call> kept;
        kept.reserve(f.calls.size());
        for (Call& c : f.calls)
        {
            if (c.used) { kept.push_back(std::move(c)); continue; }
            bool heldDrop = false;
            for (const auto& d : c.deferred) heldDrop |= !d.second;
            g_deferDropped += c.deferred.size();
            c.deferred.clear();
            if (c.created)
            {
                PPCContext m = ctx;
                m.r3.u64 = c.created;
                m.r4.u64 = 0x72656C65;                       // 'rele'
                __imp__sub_82B1D318(m, base);
                const uint32_t refs = GRead32(base, c.created + 28);
                if (refs) GWrite32(base, c.created + 28, refs - 1);
                ++g_settledStops;
            }
            if (c.released && !heldDrop)                     // a drop that happened (not held): given back
            {
                GWrite32(base, c.released + 28, GRead32(base, c.released + 28) + 1);
                ++g_settledRefs;
            }
        }
        f.calls.swap(kept);
        for (Call& c : f.calls) c.used = false;
        f.cursor = 0;
    }
}

// rollback.cpp: frames up to `upTo` can no longer be rolled back - their held reference drops and
// 'rele' posts happen now (a session: before NACE's confirmed frame; the sync test: older than its
// depth; the end of a fight: all).
void SoundRollbackFinal(PPCContext& ctx, uint8_t* base, uint64_t upTo)
{
    for (FrameCalls& f : g_frames)
    {
        if (f.frame == ~0ull || f.frame > upTo) continue;
        for (Call& c : f.calls) if (!c.deferred.empty()) ApplyDeferred(ctx, base, c);
    }
}

// rollback.cpp, at the start of each fight frame's update (the frame's calls run until the next one):
// live (record) or re-simulated (replay); frame = ~0 stops both.
void SoundRollbackFrame(uint64_t frame, bool resim)
{
    g_main = GetCurrentThreadId();
    if (CensusOn())
    {
        static uint32_t frames = 0;
        for (int i = 0; i < kCount; ++i) { g_framesWith[i] += g_thisFrame[i]; g_thisFrame[i] = false; }
        if (++frames % 300 == 0)
        {
            char line[2048];
            int n = std::snprintf(line, sizeof(line), "[snd-census] 300 frames (calls/frames):");
            for (int i = 0; i < kCount; ++i)
                if (g_calls[i]) n += std::snprintf(line + n, sizeof(line) - n, " %08X=%u/%u", kAddr[i], g_calls[i], g_framesWith[i]);
            std::fprintf(stderr, "%s; re-simulated calls: %llu answered from the record, %llu new; discarded timelines: %llu sounds stopped, %llu references given back\n", line,
                         (unsigned long long)g_replayed, (unsigned long long)g_executed, (unsigned long long)g_settledStops, (unsigned long long)g_settledRefs);
            for (int i = 0; i < kCount; ++i) g_calls[i] = g_framesWith[i] = 0;
        }
    }
    g_rec = g_rep = nullptr;
    if (frame == ~0ull) return;
    FrameCalls& f = g_frames[frame % kRing];
    if (resim)
    {
        if (f.frame != frame) return;            // nothing recorded: the calls just run
        for (Call& c : f.calls) c.used = false;
        f.cursor = 0;
        g_rep = &f;
        return;
    }
    for (Call& c : f.calls)
        for (const auto& [h, rele] : c.deferred)
        {
            if (rele) { ++g_deferDropped; continue; }
            const uint32_t refs = GRead32(g_base, h + 28);
            if (refs) GWrite32(g_base, h + 28, refs - 1);
            ++g_deferApplied;
        }
    f.frame = frame;
    f.calls.clear();
    f.cursor = 0;
    g_rec = &f;
}

extern "C" PPC_FUNC(__imp__sub_82B1B0B0);
PPC_FUNC(sub_82B1B0B0) { Hook(0, ctx, base, __imp__sub_82B1B0B0); }
extern "C" PPC_FUNC(__imp__sub_82B1B128);
PPC_FUNC(sub_82B1B128) { Hook(1, ctx, base, __imp__sub_82B1B128); }
extern "C" PPC_FUNC(__imp__sub_82B1B140);
PPC_FUNC(sub_82B1B140) { Hook(2, ctx, base, __imp__sub_82B1B140); }
extern "C" PPC_FUNC(__imp__sub_82B1B158);
PPC_FUNC(sub_82B1B158) { Hook(3, ctx, base, __imp__sub_82B1B158); }
extern "C" PPC_FUNC(__imp__sub_82B1B170);
PPC_FUNC(sub_82B1B170) { Hook(4, ctx, base, __imp__sub_82B1B170); }
extern "C" PPC_FUNC(__imp__sub_82B1B1E8);
PPC_FUNC(sub_82B1B1E8) { Hook(5, ctx, base, __imp__sub_82B1B1E8); }
extern "C" PPC_FUNC(__imp__sub_82B1B260);
PPC_FUNC(sub_82B1B260) { Hook(6, ctx, base, __imp__sub_82B1B260); }
extern "C" PPC_FUNC(__imp__sub_82B1B2A0);
PPC_FUNC(sub_82B1B2A0) { Hook(7, ctx, base, __imp__sub_82B1B2A0); }
extern "C" PPC_FUNC(__imp__sub_82B1B2F0);
PPC_FUNC(sub_82B1B2F0) { Hook(8, ctx, base, __imp__sub_82B1B2F0); }
extern "C" PPC_FUNC(__imp__sub_82B1B388);
PPC_FUNC(sub_82B1B388) { Hook(9, ctx, base, __imp__sub_82B1B388); }
extern "C" PPC_FUNC(__imp__sub_82B1B420);
PPC_FUNC(sub_82B1B420) { Hook(10, ctx, base, __imp__sub_82B1B420); }
extern "C" PPC_FUNC(__imp__sub_82B1B4C0);
PPC_FUNC(sub_82B1B4C0) { Hook(11, ctx, base, __imp__sub_82B1B4C0); }
extern "C" PPC_FUNC(__imp__sub_82B1B570);
PPC_FUNC(sub_82B1B570) { Hook(12, ctx, base, __imp__sub_82B1B570); }
extern "C" PPC_FUNC(__imp__sub_82B1B5F8);
PPC_FUNC(sub_82B1B5F8) { Hook(13, ctx, base, __imp__sub_82B1B5F8); }
extern "C" PPC_FUNC(__imp__sub_82B1B680);
PPC_FUNC(sub_82B1B680) { Hook(14, ctx, base, __imp__sub_82B1B680); }
extern "C" PPC_FUNC(__imp__sub_82B1B710);
PPC_FUNC(sub_82B1B710) { Hook(15, ctx, base, __imp__sub_82B1B710); }
extern "C" PPC_FUNC(__imp__sub_82B1B7A0);
PPC_FUNC(sub_82B1B7A0) { Hook(16, ctx, base, __imp__sub_82B1B7A0); }
extern "C" PPC_FUNC(__imp__sub_82B1B830);
PPC_FUNC(sub_82B1B830) { Hook(17, ctx, base, __imp__sub_82B1B830); }
extern "C" PPC_FUNC(__imp__sub_82B1B8C0);
PPC_FUNC(sub_82B1B8C0) { Hook(18, ctx, base, __imp__sub_82B1B8C0); }
extern "C" PPC_FUNC(__imp__sub_82B1B8C8);
PPC_FUNC(sub_82B1B8C8) { Hook(19, ctx, base, __imp__sub_82B1B8C8); }
extern "C" PPC_FUNC(__imp__sub_82B1B8D0);
PPC_FUNC(sub_82B1B8D0) { Hook(20, ctx, base, __imp__sub_82B1B8D0); }
extern "C" PPC_FUNC(__imp__sub_82B1B900);
PPC_FUNC(sub_82B1B900) { Hook(21, ctx, base, __imp__sub_82B1B900); }
extern "C" PPC_FUNC(__imp__sub_82B1B950);
PPC_FUNC(sub_82B1B950) { Hook(22, ctx, base, __imp__sub_82B1B950); }
extern "C" PPC_FUNC(__imp__sub_82B1B968);
PPC_FUNC(sub_82B1B968) { Hook(23, ctx, base, __imp__sub_82B1B968); }
extern "C" PPC_FUNC(__imp__sub_82B1BA68);
PPC_FUNC(sub_82B1BA68) { Hook(24, ctx, base, __imp__sub_82B1BA68); }
extern "C" PPC_FUNC(__imp__sub_82B1BB50);
PPC_FUNC(sub_82B1BB50) { Hook(25, ctx, base, __imp__sub_82B1BB50); }
extern "C" PPC_FUNC(__imp__sub_82B1BBB0);
PPC_FUNC(sub_82B1BBB0) { Hook(26, ctx, base, __imp__sub_82B1BBB0); }
extern "C" PPC_FUNC(__imp__sub_82B1BBF0);
PPC_FUNC(sub_82B1BBF0) { Hook(27, ctx, base, __imp__sub_82B1BBF0); }
extern "C" PPC_FUNC(__imp__sub_82B1BC78);
PPC_FUNC(sub_82B1BC78) { Hook(28, ctx, base, __imp__sub_82B1BC78); }
extern "C" PPC_FUNC(__imp__sub_82B1BD00);
PPC_FUNC(sub_82B1BD00) { Hook(29, ctx, base, __imp__sub_82B1BD00); }
extern "C" PPC_FUNC(__imp__sub_82B1BD88);
PPC_FUNC(sub_82B1BD88) { Hook(30, ctx, base, __imp__sub_82B1BD88); }
extern "C" PPC_FUNC(__imp__sub_82B1BE08);
PPC_FUNC(sub_82B1BE08) { Hook(31, ctx, base, __imp__sub_82B1BE08); }
extern "C" PPC_FUNC(__imp__sub_82B1BE70);
PPC_FUNC(sub_82B1BE70) { Hook(32, ctx, base, __imp__sub_82B1BE70); }
extern "C" PPC_FUNC(__imp__sub_82B1BED8);
PPC_FUNC(sub_82B1BED8) { Hook(33, ctx, base, __imp__sub_82B1BED8); }
extern "C" PPC_FUNC(__imp__sub_82B1BFB8);
PPC_FUNC(sub_82B1BFB8) { Hook(34, ctx, base, __imp__sub_82B1BFB8); }
extern "C" PPC_FUNC(__imp__sub_82B1C050);
PPC_FUNC(sub_82B1C050) { Hook(35, ctx, base, __imp__sub_82B1C050); }
extern "C" PPC_FUNC(__imp__sub_82B1C0C8);
PPC_FUNC(sub_82B1C0C8) { Hook(36, ctx, base, __imp__sub_82B1C0C8); }
extern "C" PPC_FUNC(__imp__sub_82B1C140);
PPC_FUNC(sub_82B1C140) { Hook(37, ctx, base, __imp__sub_82B1C140); }
extern "C" PPC_FUNC(__imp__sub_82B1C1B0);
PPC_FUNC(sub_82B1C1B0) { Hook(38, ctx, base, __imp__sub_82B1C1B0); }
extern "C" PPC_FUNC(__imp__sub_82B1C208);
PPC_FUNC(sub_82B1C208) { Hook(39, ctx, base, __imp__sub_82B1C208); }
extern "C" PPC_FUNC(__imp__sub_82B1C268);
PPC_FUNC(sub_82B1C268) { Hook(40, ctx, base, __imp__sub_82B1C268); }
extern "C" PPC_FUNC(__imp__sub_82B1C2D0);
PPC_FUNC(sub_82B1C2D0) { Hook(41, ctx, base, __imp__sub_82B1C2D0); }
extern "C" PPC_FUNC(__imp__sub_82B1C338);
PPC_FUNC(sub_82B1C338) { Hook(42, ctx, base, __imp__sub_82B1C338); }
extern "C" PPC_FUNC(__imp__sub_82B1C390);
PPC_FUNC(sub_82B1C390) { Hook(43, ctx, base, __imp__sub_82B1C390); }
extern "C" PPC_FUNC(__imp__sub_82B1C3E8);
PPC_FUNC(sub_82B1C3E8) { Hook(44, ctx, base, __imp__sub_82B1C3E8); }
extern "C" PPC_FUNC(__imp__sub_82B1C440);
PPC_FUNC(sub_82B1C440) { Hook(45, ctx, base, __imp__sub_82B1C440); }
extern "C" PPC_FUNC(__imp__sub_82B1C498);
PPC_FUNC(sub_82B1C498) { Hook(46, ctx, base, __imp__sub_82B1C498); }
extern "C" PPC_FUNC(__imp__sub_82B1C500);
PPC_FUNC(sub_82B1C500) { Hook(47, ctx, base, __imp__sub_82B1C500); }
extern "C" PPC_FUNC(__imp__sub_82B1CA40);
PPC_FUNC(sub_82B1CA40) { Hook(48, ctx, base, __imp__sub_82B1CA40); }
extern "C" PPC_FUNC(__imp__sub_82B1CAC8);
PPC_FUNC(sub_82B1CAC8) { Hook(49, ctx, base, __imp__sub_82B1CAC8); }
extern "C" PPC_FUNC(__imp__sub_82B1CB50);
PPC_FUNC(sub_82B1CB50) { Hook(50, ctx, base, __imp__sub_82B1CB50); }
extern "C" PPC_FUNC(__imp__sub_82B1CBD8);
PPC_FUNC(sub_82B1CBD8) { Hook(51, ctx, base, __imp__sub_82B1CBD8); }
