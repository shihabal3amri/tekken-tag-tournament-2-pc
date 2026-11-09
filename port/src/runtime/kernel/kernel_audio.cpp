// Audio HLE: each registered render client receives callbacks paced by completed
// 256-sample / 48 kHz buffers. TTT2 creates multiple engines; replacing a callback
// another stops the first engine's mixer and strands its queued Stop/Flush work.
// PCM samples go to a bounded Windows XAudio2 output queue. Each callback receives
// r3 = its own stable guest
// cell holding the registration context, as in Xenia's AudioSystem::RegisterClient.
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "guest_heap.h"
#include "audio_output.h"
void DiagNameThread(const wchar_t* name);   // kernel_thread.cpp (TEST ONLY)
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static constexpr uint32_t AUDIO_DRIVER_KEY = 0x41550000u;
static constexpr size_t AUDIO_MAX_CLIENTS = 8;
static constexpr int64_t FRAME_NS = 1000000000ll * 256 / 48000;
static constexpr uint32_t AUDIO_INVALID_HANDLE = 0xC0000008u;
static constexpr uint32_t AUDIO_INVALID_PARAMETER = 0xC000000Du;
static constexpr uint32_t AUDIO_NO_MEMORY = 0xC000009Au;

struct AudioClient
{
    // The registry mutex is never held while waiting for this mutex or calling
    // the guest. Recursive locking permits unregister from the callback itself.
    std::recursive_mutex execution;
    bool active = true;                       // protected by execution
    uint32_t callback = 0;
    uint32_t paramCell = 0;
    uint64_t pumped = 0;                       // protected by execution
    uint64_t maxCallbackUs = 0;
    std::atomic<uint64_t> submitted{0};
    std::unique_ptr<AudioOutputClient> output;
    ~AudioClient() { if (paramCell) g_virtualHeap.Free(g_base + paramCell); }
};

static std::mutex g_xaClientsMutex;
static std::array<std::shared_ptr<AudioClient>, AUDIO_MAX_CLIENTS> g_xaClients;
static std::atomic<uint64_t> g_xaPumped{0};
static std::atomic<uint64_t> g_xaSubmitted{0};
static std::atomic<bool> g_xaPumpStarted{false};

// NUSound, the game's sound engine (music, voices, effects), mixes on its own thread (sub_82D032D0) two
// 256-sample buffers ahead and hands one to its XAudio2 voice at the start of each mixing pass (sub_82D030A0).
// A pass that finds no buffer ready plays 5.3 ms of silence and everything NUSound plays falls 5.3 ms behind:
// the game counts these at state+200. On the console the passes came from the hardware every 5.3 ms and
// NUSound had a hardware thread of its own. Here the passes come in pairs (XAudio2 on Windows asks for 10 ms at
// a time) and NUSound shares the processor. 0.1.8 TEST 15: a pass is held until NUSound's buffer is ready, out
// of the sound already queued for the device (about 40 ms); only when that runs low does it go without it.
// MEASURED 2026-10-31: logs 44 and 45 (i3-3240, 2 cores / 4 threads, NUSound 2.85 ms a buffer) have 5-21
// all-silent output frames per 11 s inside the music of a Paris fight, where NUSound is the only thing playing:
// these misses, about 1-2 a second (the player: the music stutters, plays slower, out of sync with the picture).
// This PC at 4 threads, NUSound slowed to 2.85 ms a buffer (TTT2_DIAG_NUS_SPIN_US) and two busy loops beside
// it: 9 misses in a session; held: 0 (9 holds, 41 ms in all, the device queue never ran short).
// TTT2_NUS_WAIT=0: never hold a pass (the old behaviour).
namespace nus {
constexpr uint32_t kState = 0x830D4DE8;            // NUSound's output state (sub_82D03540 sets it up)
constexpr uint32_t kProduced = kState + 192;       // buffers mixed
constexpr uint32_t kConsumed = kState + 196;       // buffers handed to the voice
constexpr uint32_t kUnderruns = kState + 200;      // passes that found none
constexpr uint32_t kMinQueued = 2;                 // frames still queued for the device when a pass stops waiting
constexpr uint64_t kMaxWaitUs = 30000;
std::atomic<int> g_client{ -1 };                   // the render client whose passes run NUSound's voice
std::atomic<int> g_pumping{ -1 };                  // the client whose callback runs now (its pass runs on the
                                                   // title's XAudio2 thread while the callback waits for it)
bool WaitEnabled() { static const bool on = [] { const char* e = std::getenv("TTT2_NUS_WAIT"); return !e || *e != '0'; }(); return on; }
struct Counters { uint64_t passes = 0, lead[3] = {}, waits = 0, waitUs = 0, maxWaitUs = 0, gaveUp = 0, idle = 0; };
Counters g_counts;                                 // pump thread only
uint32_t g_idleAt = 0;                             // buffers mixed when NUSound last let a wait run out
bool g_idle = false;

int32_t Lead(uint8_t* base) { return int32_t(GRead32(base, kProduced) - GRead32(base, kConsumed)); }

// Before a pass of NUSound's client: hold it until NUSound's next buffer is mixed.
void HoldPass(uint8_t* base, const AudioOutputClient& output)
{
    Counters& c = g_counts;
    ++c.passes;
    int32_t lead = Lead(base);
    ++c.lead[std::clamp(lead, 0, 2)];
    if (lead > 0 || !WaitEnabled()) return;
    // A NUSound that stopped mixing (its thread stopped or blocked for long) is not waited for again until it
    // mixes another buffer.
    const uint32_t produced = GRead32(base, kProduced);
    if (g_idle && produced == g_idleAt) { ++c.idle; return; }
    g_idle = false;
    const auto began = std::chrono::steady_clock::now();
    uint64_t us = 0;
    for (;;)
    {
        if (output.Queued() <= kMinQueued || us >= kMaxWaitUs)
        {
            ++c.gaveUp;
            g_idle = true;
            g_idleAt = produced;
            break;
        }
        Sleep(1);
        us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count());
        if (Lead(base) > 0) break;
    }
    ++c.waits;
    c.waitUs += us;
    c.maxWaitUs = std::max(c.maxWaitUs, us);
}
}

// NUSound's voice callback at the start of each pass: names the client its passes belong to.
extern "C" PPC_FUNC(__imp__sub_82D030A0);
PPC_FUNC(sub_82D030A0)
{
    const int client = nus::g_pumping.load(std::memory_order_relaxed);
    if (client >= 0 && nus::g_client.exchange(client, std::memory_order_relaxed) != client)
        std::fprintf(stderr, "[snd] NUSound's voice is mixed by driver %08X; holding passes for its buffers: %s\n",
                     AUDIO_DRIVER_KEY | uint32_t(client), nus::WaitEnabled() ? "on" : "off (TTT2_NUS_WAIT=0)");
    __imp__sub_82D030A0(ctx, base);
}

// TEST ONLY: TTT2_DIAG_NUS_SPIN_US adds a busy wait to every buffer NUSound mixes (a slower PC's sound engine:
// log 45's i3-3240 took 2.85 ms a buffer in a Paris fight, this PC 0.64 ms). sub_82D00D78: once per buffer.
extern "C" PPC_FUNC(__imp__sub_82D00D78);
PPC_FUNC(sub_82D00D78)
{
    static const int64_t spinNs = [] { const char* e = std::getenv("TTT2_DIAG_NUS_SPIN_US"); return e ? std::atoll(e) * 1000 : 0LL; }();
    if (spinNs)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(spinNs);
        while (std::chrono::steady_clock::now() < until) {}
    }
    __imp__sub_82D00D78(ctx, base);
}

// ---- the pump's watchdog ----------------------------------------------------------------------------------------
// XAudio2's render client (the callback's context: sub_82C3E1E8 -> sub_82C3E008) pairs each callback with one engine
// pass: the callback sets the kick (client+40, auto-reset; not before the first frame: client+56 = 0) and waits for
// frame-ready (client+44) or quit (client+48); the engine (sub_82C3BAA0, its ctx+20 = that kick) runs one pass per
// kick (sub_82C5B218: 1 here), whose end (sub_82C3DFA8) sets frame-ready; the callback copies client+24 into
// 0x830BF9D0, submits it and counts it (client+56, 64-bit). MEASURED 2026-11-09 (TTT2_RB_FOREIGN=keep, Arcade, sync
// test 8 every 5 - the player's stall exactly): the pump stuck for good at sub_82C3E008+1A9 in that wait, both
// engines idle in sub_82C3A420 waiting for their kick, client 0014F920's count 143 behind the frames pumped - the
// pairing lost, and nothing in the log said so (no timeout on either wait). The watchdog: "[snd] pump stuck" at 1.5 s
// with the client's and its engine's words and the host events' states (every thread's stack once, the first three
// stalls), again at 15 s and every 60 s, and a line when the callback returns.
// TTT2_SND_RECOVER=0: no recovery (PumpRecover below), diagnostics only.
void DumpAllStacks(const char* why);                                          // main.cpp
uint32_t KernelGuestEventState(uint32_t addr);                                // kernel_objects.cpp
uint32_t KernelSetGuestEvent(uint8_t* base, uint32_t addr);
bool KernelThreadMultiWait(unsigned long tid, uint32_t* objs4, uint32_t& count, uint64_t& sinceMs);
namespace pumpwatch {
std::atomic<uint64_t> g_callSince{ 0 };            // GetTickCount64() when the running callback began; 0: none
std::atomic<uint64_t> g_callSeq{ 0 };              // callbacks begun (a stall's identity: two can begin in one tick)
std::atomic<unsigned long> g_pumpTid{ 0 };
// A start stamp (never 0: 0 is "none") and an age that cannot wrap: a stamp taken after the reader's "now" (the same
// 15.6 ms tick, or a later one) is age 0, never ~2^64 (review 2026-11-09: a GetTickCount64() | 1 stamp was now + 1 for a
// callback begun in the reader's tick - a healthy pump looked stuck every half minute).
uint64_t Stamp() { const uint64_t t = GetTickCount64(); return t ? t : 1; }
uint64_t Age(uint64_t since, uint64_t now) { return since && now > since ? now - since : 0; }
// XAudio2's engines parked in sub_82C3A420 (waiting for the kick, the timeout or quit): engine ctx -> since.
struct Park { std::atomic<uint32_t> ctx{ 0 }; std::atomic<uint64_t> since{ 0 }; std::atomic<unsigned long> tid{ 0 }; };
Park g_parks[8];
Park* ParkOf(uint32_t engine)
{
    for (Park& p : g_parks) if (p.ctx.load(std::memory_order_acquire) == engine) return &p;
    for (Park& p : g_parks)
    {
        uint32_t none = 0;
        if (p.ctx.compare_exchange_strong(none, engine)) return &p;
        if (none == engine) return &p;
    }
    return nullptr;
}
bool RecoverEnabled() { static const bool on = [] { const char* e = std::getenv("TTT2_SND_RECOVER"); return !e || *e != '0'; }(); return on; }
// "ADDR[type T state S host H]": the guest header's type byte and state word, the host event (unbound / set / clear).
int Event(char* out, size_t cap, uint32_t addr)
{
    if (!addr || addr >= 0xF0000000u || (addr & 3)) return std::snprintf(out, cap, "%08X[?]", addr);
    const uint32_t s = KernelGuestEventState(addr);
    return std::snprintf(out, cap, "%08X[type %u state %u host %s%s]", addr, unsigned(GRead8(g_base, addr)), GRead32(g_base, addr + 4),
                         !(s & 1) ? "unbound" : (s & 2) ? "set" : "clear", (s >> 8) ? " +waiters" : "");
}
bool Clear(uint32_t addr) { const uint32_t s = KernelGuestEventState(addr); return (s & 1) && !(s & 2); }
uint64_t Read64(uint32_t a) { return (uint64_t(GRead32(g_base, a)) << 32) | GRead32(g_base, a + 4); }
}

// XAudio2's engine waits for a kick (sub_82C3A420: r3 the engine, r4 the timeout in ms or -1; waits for any of
// {ctx+12 quit, ctx+16, ctx+20 kick}; 1 = run a pass, 0 = quit): the parked engines, for the watchdog.
extern "C" PPC_FUNC(__imp__sub_82C3A420);
PPC_FUNC(sub_82C3A420)
{
    pumpwatch::Park* p = pumpwatch::ParkOf(ctx.r3.u32);
    if (p)
    {
        p->tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        p->since.store(pumpwatch::Stamp(), std::memory_order_release);
    }
    __imp__sub_82C3A420(ctx, base);
    if (p) p->since.store(0, std::memory_order_release);
}

unsigned long AudioPumpThreadId() { return pumpwatch::g_pumpTid.load(std::memory_order_relaxed); }   // main.cpp's stack dump

static std::shared_ptr<AudioClient> FindAudioClient(uint32_t handle)
{
    const uint32_t index = handle & 0xFFFF;
    if ((handle & 0xFFFF0000u) != AUDIO_DRIVER_KEY || index >= AUDIO_MAX_CLIENTS)
        return {};
    std::lock_guard lock(g_xaClientsMutex);
    return g_xaClients[index];
}

static bool PumpAudioClients(uint64_t nowNs = 0)
{
    bool pumped = false;
    std::array<std::shared_ptr<AudioClient>, AUDIO_MAX_CLIENTS> clients;
    {
        std::lock_guard lock(g_xaClientsMutex);
        clients = g_xaClients;
    }
    for (size_t index = 0; index < clients.size(); ++index)
    {
        const auto& client = clients[index];
        if (!client) continue;
        std::lock_guard execution(client->execution);
        if (!client->active) continue;
        if (!client->output->TryAcquireRenderCredit(nowNs)) continue;
        if (int(index) == nus::g_client.load(std::memory_order_relaxed)) nus::HoldPass(g_base, *client->output);
        const auto began = std::chrono::steady_clock::now();
        nus::g_pumping.store(int(index), std::memory_order_relaxed);
        pumpwatch::g_callSeq.fetch_add(1, std::memory_order_relaxed);
        pumpwatch::g_callSince.store(pumpwatch::Stamp(), std::memory_order_release);
        GuestCall(client->callback, client->paramCell);
        pumpwatch::g_callSince.store(0, std::memory_order_release);
        nus::g_pumping.store(-1, std::memory_order_relaxed);
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - began).count();
        client->maxCallbackUs = std::max(client->maxCallbackUs, uint64_t(duration));
        pumped = true;
        ++g_xaPumped;
        if (++client->pumped % 2048 == 0)
        {
            std::fprintf(stderr, "[snd] driver=%08X pump frames=%llu submits=%llu context=%08X\n",
                         AUDIO_DRIVER_KEY | uint32_t(index),
                         (unsigned long long)client->pumped,
                         (unsigned long long)client->submitted.load(),
                         GRead32(g_base, client->paramCell));
            const auto output = client->output->Stats();
            // max_pass_gap_us / pass_gaps_over_20ms: XAudio2's pass starts on this voice since the line before.
            std::fprintf(stderr, "[snd-output] driver=%08X ready=%u failed=%u received=%llu submitted=%llu completed=%llu dropped=%llu queued=%u nonzero=%llu nonfinite=%llu deviceq=%u credits=%u starved=%llu starved_bytes=%llu max_callback_us=%llu engine_glitches=%u engine_latency=%u max_pass_gap_us=%llu pass_gaps_over_20ms=%u\n",
                         AUDIO_DRIVER_KEY | uint32_t(index), unsigned(output.ready), unsigned(output.failed),
                         (unsigned long long)output.received, (unsigned long long)output.submitted,
                         (unsigned long long)output.completed, (unsigned long long)output.dropped,
                         output.queued, (unsigned long long)output.nonzero,
                         (unsigned long long)output.nonfinite, output.deviceQueued, output.renderCredits,
                         (unsigned long long)output.starvationPasses, (unsigned long long)output.starvationBytes,
                         (unsigned long long)client->maxCallbackUs, output.engineGlitches, output.engineLatency,
                         (unsigned long long)output.maxPassGapUs, output.latePasses);
            client->maxCallbackUs = 0;
            if (int(index) == nus::g_client.load(std::memory_order_relaxed))
            {
                // lead = NUSound's buffers ready when a pass began; underruns = the game's own count of passes
                // that played silence instead (since it started).
                nus::Counters& c = nus::g_counts;
                std::fprintf(stderr, "[snd-nus] passes=%llu lead0/1/2=%llu/%llu/%llu underruns=%u held=%llu held_ms=%.1f max_held_us=%llu gave_up=%llu idle=%llu\n",
                             (unsigned long long)c.passes, (unsigned long long)c.lead[0], (unsigned long long)c.lead[1],
                             (unsigned long long)c.lead[2], GRead32(g_base, nus::kUnderruns), (unsigned long long)c.waits,
                             c.waitUs / 1000.0, (unsigned long long)c.maxWaitUs, (unsigned long long)c.gaveUp,
                             (unsigned long long)c.idle);
                c.maxWaitUs = 0;
            }
        }
    }
    return pumped;
}

// The watchdog's recovery. A re-kick alone is not enough: with the kick or frame-ready still set, or the engine busy
// elsewhere, one more kick runs one more pass than the callbacks ask for, and from then on every callback takes the
// PREVIOUS pass's frame-ready while the engine writes client+24 under its copy (a kick ahead for good). So the engine
// is kicked only when the pairing is provably lost and one kick restores it:
// - the pump has been in this client's callback for 1.5 s, waiting (KeWaitForMultipleObjects) on exactly the
//   client's {quit +48, frame-ready +44} - not on events a torn read of the client gave it;
// - frame-ready, quit and the kick (+40) are all clear on the host;
// - the engine whose kick that is (ctx+20 == client+40) has been parked in sub_82C3A420 for a second and its thread
//   is inside KeWaitForMultipleObjects on {ctx+12 quit, ctx+16, the kick}, its quit clear: it consumed every kick
//   it got, owes no pass, and the next kick wakes exactly it;
// - the client's own state agrees: its count (+56) above 0 so that later callbacks kick (else set to the frames
//   this client submitted - a count at 0 or below skips the kick for good).
// Then one kick = one pass = one frame-ready for the one callback waiting: 1:1 again. At most 3 tries a stall, 2 s
// apart; each is logged, and so is whether the callback came back.
static bool PumpRecover(int index, const AudioClient& client, uint32_t obj, uint64_t stuckMs, int attempt)
{
    using namespace pumpwatch;
    const uint32_t kick = GRead32(g_base, obj + 40), ready = GRead32(g_base, obj + 44), quit = GRead32(g_base, obj + 48);
    uint32_t objs[4] = {}, count = 0;
    uint64_t waitSince = 0;
    const char* no = nullptr;
    if (!KernelThreadMultiWait(g_pumpTid.load(), objs, count, waitSince)) no = "the pump is not in a KeWaitForMultipleObjects";
    else if (count != 2 || objs[0] != quit || objs[1] != ready) no = "the callback waits on events its client does not name (a torn read)";
    else if (!Clear(ready) || !Clear(quit) || !Clear(kick)) no = "frame-ready, quit or the kick is set or unbound";
    uint32_t engine = 0;
    uint64_t parked = 0;
    unsigned long engineTid = 0;
    if (!no)
    {
        for (const Park& p : g_parks)
        {
            const uint32_t e = p.ctx.load(std::memory_order_acquire);
            if (!e || GRead32(g_base, e + 20) != kick) continue;
            engine = e;
            engineTid = p.tid.load();
            const uint64_t since = p.since.load(std::memory_order_acquire);
            parked = Age(since, GetTickCount64());           // "now" after the stamp: an engine parked since is 0
        }
        // The engine thread inside KeWaitForMultipleObjects on {quit +12, +16, its kick +20} right now: it has not
        // taken a kick it was given (a thread past the wait but not yet out of sub_82C3A420 would be).
        uint32_t eobjs[4] = {}, ecount = 0;
        uint64_t esince = 0;
        if (!engine) no = "no engine kicked by this client was seen";
        else if (parked < 1000) no = "its engine is not parked waiting for a kick";
        else if (!KernelThreadMultiWait(engineTid, eobjs, ecount, esince) || ecount != 3 || eobjs[2] != kick)
            no = "its engine is not inside its wait for the kick";
        else if (!Clear(GRead32(g_base, engine + 12))) no = "its engine's quit is set or unbound";
    }
    if (no)
    {
        std::fprintf(stderr, "[snd] pump: driver %08X not re-kicked (try %d, %llu ms): %s\n", AUDIO_DRIVER_KEY | uint32_t(index),
                     attempt, (unsigned long long)stuckMs, no);
        return false;
    }
    const uint64_t count56 = Read64(obj + 56);
    const uint64_t submitted = client.submitted.load();
    if (int64_t(count56) <= 0)
    {
        GWrite32(g_base, obj + 56, uint32_t(submitted >> 32));
        GWrite32(g_base, obj + 60, uint32_t(submitted));
    }
    const uint32_t r = KernelSetGuestEvent(g_base, kick);
    std::fprintf(stderr, "[snd] pump: driver %08X re-kicked (try %d, stuck %llu ms; engine %08X parked %llu ms; client count %llu%s, "
                         "frames submitted %llu): status %08X\n",
                 AUDIO_DRIVER_KEY | uint32_t(index), attempt, (unsigned long long)stuckMs, engine, (unsigned long long)parked,
                 (unsigned long long)count56, int64_t(count56) <= 0 ? " - set to the frames submitted" : "",
                 (unsigned long long)submitted, r);
    return true;
}

static void PumpReport(int index, const AudioClient& client, uint32_t obj, uint64_t stuckMs)
{
    using namespace pumpwatch;
    char kick[96], ready[96], quit[96], line[1024];
    Event(kick, sizeof(kick), GRead32(g_base, obj + 40));
    Event(ready, sizeof(ready), GRead32(g_base, obj + 44));
    Event(quit, sizeof(quit), GRead32(g_base, obj + 48));
    uint32_t objs[4] = {}, count = 0;
    uint64_t waitSince = 0;
    const bool waiting = KernelThreadMultiWait(g_pumpTid.load(), objs, count, waitSince);
    std::fprintf(stderr, "[snd] pump stuck in driver %08X for %llu ms: client %08X vt %08X buf +24 %08X drv +32 %08X count +56 %llu "
                         "(frames submitted %llu) +64 %u; kick +40 %s ready +44 %s quit +48 %s; the callback %s %08X %08X (count %u)\n",
                 AUDIO_DRIVER_KEY | uint32_t(index), (unsigned long long)stuckMs, obj, GRead32(g_base, obj), GRead32(g_base, obj + 24),
                 GRead32(g_base, obj + 32), (unsigned long long)Read64(obj + 56), (unsigned long long)client.submitted.load(),
                 GRead32(g_base, obj + 64), kick, ready, quit, waiting ? "waits on" : "is not in a multi-object wait;", objs[0],
                 objs[1], count);
    for (const Park& p : g_parks)
    {
        const uint32_t e = p.ctx.load(std::memory_order_acquire);
        if (!e) continue;
        const uint64_t since = p.since.load(std::memory_order_acquire);
        const uint64_t now = GetTickCount64();
        char q[96], w[96], k[96];
        Event(q, sizeof(q), GRead32(g_base, e + 12));
        Event(w, sizeof(w), GRead32(g_base, e + 16));
        Event(k, sizeof(k), GRead32(g_base, e + 20));
        uint32_t x = GRead32(g_base, e + 156);
        if (x < VHEAP_BEGIN || x >= VHEAP_END || (x & 3)) x = 0;   // only a plausible heap object is read
        char state[48];
        if (since) std::snprintf(state, sizeof(state), "parked in sub_82C3A420 for %llu ms", (unsigned long long)Age(since, now));
        else std::snprintf(state, sizeof(state), "not parked (in a pass)");
        int n = std::snprintf(line, sizeof(line), "[snd]   engine %08X%s (tid %lu) %s: quit +12 %s +16 %s kick +20 %s; +60 %u "
                                                  "+136 %u +140 %u dev +144 %08X +152 %08X x +156 %08X (+124 %u +128 %08X +144 %u) +160 %u +164 %u; +592..+640:",
                              e, GRead32(g_base, e + 20) == GRead32(g_base, obj + 40) ? " (this client's)" : "", p.tid.load(), state, q, w, k,
                              GRead32(g_base, e + 60), GRead32(g_base, e + 136), GRead32(g_base, e + 140), GRead32(g_base, e + 144),
                              GRead32(g_base, e + 152), x, x ? GRead32(g_base, x + 124) : 0, x ? GRead32(g_base, x + 128) : 0,
                              x ? GRead32(g_base, x + 144) : 0, GRead32(g_base, e + 160), GRead32(g_base, e + 164));
        for (uint32_t off = 592; off <= 640 && n < int(sizeof(line)) - 12; off += 4)
            n += std::snprintf(line + n, sizeof(line) - n, " %08X", GRead32(g_base, e + off));
        std::fprintf(stderr, "%s\n", line);
    }
    std::fprintf(stderr, "[snd]   NUSound produced %u consumed %u underruns %u; frames pumped %llu submitted %llu (all drivers)\n",
                 GRead32(g_base, nus::kProduced), GRead32(g_base, nus::kConsumed), GRead32(g_base, nus::kUnderruns),
                 (unsigned long long)g_xaPumped.load(), (unsigned long long)g_xaSubmitted.load());
}

static void PumpWatchdog()
{
    using namespace pumpwatch;
    DiagNameThread(L"audio-watch");
    uint64_t stall = 0, stallSeq = 0, nextReport = 0, lastTry = 0;
    int reports = 0, tries = 0, kicks = 0, dumps = 0, stallIndex = -1;
    for (;;)
    {
        Sleep(250);
        const uint64_t seq = g_callSeq.load(std::memory_order_relaxed);
        const uint64_t since = g_callSince.load(std::memory_order_acquire);
        const int index = nus::g_pumping.load(std::memory_order_relaxed);
        const uint64_t now = GetTickCount64();
        const uint64_t age = Age(since, now);
        if (stall && (!since || seq != stallSeq))      // the stuck callback came back
        {
            std::fprintf(stderr, "[snd] pump: driver %08X's callback returned after about %llu ms%s\n", AUDIO_DRIVER_KEY | uint32_t(stallIndex),
                         (unsigned long long)Age(stall, now), kicks ? " (after a re-kick: recovered)" : "");
            stall = 0;
        }
        if (!since || index < 0 || age < 1500) continue;
        if (!stall || stallSeq != seq)
        {
            stall = since;
            stallSeq = seq;
            stallIndex = index;
            nextReport = 0;
            reports = tries = kicks = 0;
            lastTry = 0;
        }
        std::shared_ptr<AudioClient> client;
        {
            std::lock_guard lock(g_xaClientsMutex);
            client = g_xaClients[size_t(index) % AUDIO_MAX_CLIENTS];
        }
        if (!client) continue;
        const uint32_t obj = GRead32(g_base, client->paramCell);
        if (now >= nextReport)
        {
            PumpReport(index, *client, obj, age);
            nextReport = now + (reports++ == 0 ? 13500 : 60000);
            if (reports == 1 && dumps < 3)
            {
                ++dumps;
                DumpAllStacks("the audio pump stuck in a render callback");
            }
        }
        if (RecoverEnabled() && tries < 3 && now - lastTry >= 2000)
        {
            lastTry = now;
            ++tries;
            kicks += PumpRecover(index, *client, obj, age, tries) ? 1 : 0;
        }
    }
}

static void AudioPumpThread()
{
    DiagNameThread(L"audio-pump");
    pumpwatch::g_pumpTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    std::thread(PumpWatchdog).detach();
    for (;;)
    {
        const uint64_t version = AudioOutputWakeVersion();
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        if (!PumpAudioClients(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count())))
            WaitAudioOutputWake(version, FRAME_NS); // timeout only needed by silent fallback clients
    }
}

// XAudioRegisterRenderDriverClient(r3=&{callback, context}, r4=&driver OUT).
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    KTrace("XAudioRegisterRenderDriverClient");
    if (!ctx.r3.u32 || !ctx.r4.u32 || (ctx.r3.u32 & 3) || (ctx.r4.u32 & 3))
    {
        ctx.r3.u32 = AUDIO_INVALID_PARAMETER;
        return;
    }
    const uint32_t fn = GRead32(base, ctx.r3.u32);
    const uint32_t param = GRead32(base, ctx.r3.u32 + 4);
    if (!fn || (fn & 3)) { ctx.r3.u32 = AUDIO_INVALID_PARAMETER; return; }
    {
        std::lock_guard lock(g_xaClientsMutex);
        size_t index = 0;
        while (index < AUDIO_MAX_CLIENTS && g_xaClients[index]) ++index;
        if (index == AUDIO_MAX_CLIENTS) { ctx.r3.u32 = AUDIO_NO_MEMORY; return; }
        auto client = std::make_shared<AudioClient>();
        void* cell = g_virtualHeap.Alloc(8, 16);
        if (!cell) { ctx.r3.u32 = AUDIO_NO_MEMORY; return; }
        client->callback = fn;
        client->paramCell = GuestAddr(base, cell);
        GWrite32(base, client->paramCell, param);
        const uint32_t handle = AUDIO_DRIVER_KEY | uint32_t(index);
        client->output = std::make_unique<AudioOutputClient>(handle);
        // Publish the caller's handle before its first callback can submit.
        GWrite32(base, ctx.r4.u32, handle);
        g_xaClients[index] = client;
        std::fprintf(stderr, "[snd] render driver=%08X client fn=%08X param=%08X cell=%08X\n",
                     handle, fn, param, client->paramCell);
    }
    if (!g_xaPumpStarted.exchange(true))
        std::thread(AudioPumpThread).detach();
    ctx.r3.u32 = STATUS_SUCCESS;
}
PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient)
{
    KTrace("XAudioUnregisterRenderDriverClient");
    const uint32_t handle = ctx.r3.u32;
    auto client = FindAudioClient(handle);
    if (!client) { ctx.r3.u32 = AUDIO_INVALID_HANDLE; return; }
    // Wait for an existing callback before removing its submission target. A
    // snapshot taken earlier cannot start another callback after active=false.
    std::lock_guard execution(client->execution);
    {
        std::lock_guard lock(g_xaClientsMutex);
        auto& slot = g_xaClients[handle & 0xFFFF];
        if (slot != client) { ctx.r3.u32 = AUDIO_INVALID_HANDLE; return; }
        client->active = false;
        slot.reset();
    }
    client->output->Close();
    ctx.r3.u32 = STATUS_SUCCESS;
}
PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame)
{
    KTrace("XAudioSubmitRenderDriverFrame");
    auto client = FindAudioClient(ctx.r3.u32);
    if (!client) { ctx.r3.u32 = AUDIO_INVALID_HANDLE; return; }
    const uint32_t samples = ctx.r4.u32;
    if (!samples || uint64_t(samples) + AUDIO_PCM_BYTES > 0x100000000ull)
    { ctx.r3.u32 = AUDIO_INVALID_PARAMETER; return; }
    client->output->Submit(base + samples);
    ++client->submitted;
    ++g_xaSubmitted;
    ctx.r3.u32 = STATUS_SUCCESS;
}

// XAudioGetSpeakerConfig(r3=&config OUT) -> stereo.
PPC_FUNC(__imp__XAudioGetSpeakerConfig) { KTrace("XAudioGetSpeakerConfig"); if (ctx.r3.u32) GWrite32(base, ctx.r3.u32, 1); ctx.r3.u32 = STATUS_SUCCESS; }

// Volume/ducker queries -> unity / disabled.
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume)        { KTrace("XAudioGetVoiceCategoryVolume"); if (ctx.r4.u32) GWrite32(base, ctx.r4.u32, 0x3F800000 /*1.0f*/); ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__XAudioEnableDucker)                  { KTrace("XAudioEnableDucker"); ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__XAudioGetDuckerThreshold)            { KTrace("XAudioGetDuckerThreshold"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XAudioGetDuckerAttackTime)           { KTrace("XAudioGetDuckerAttackTime"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XAudioGetDuckerReleaseTime)          { KTrace("XAudioGetDuckerReleaseTime"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XAudioGetDuckerHoldTime)             { KTrace("XAudioGetDuckerHoldTime"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XAudioGetDuckerLevel)                { KTrace("XAudioGetDuckerLevel"); ctx.r3.u32 = 0; }

// XMACreateContext / XMAReleaseContext and the decoder itself: xma.cpp.
