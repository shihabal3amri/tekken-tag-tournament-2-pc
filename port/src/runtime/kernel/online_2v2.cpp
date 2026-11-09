// Online 2v2 on four PCs (online_2v2.h, port/ONLINE_2V2_PLAN.md).
#include "online_2v2.h"
#include "menu_host.h"
#include "xlive.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace online2v2 {
namespace {

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<Role> g_role{ Role::None };
std::atomic<uint64_t> g_teammate{ 0 };

const bool g_fromEnv = [] {
    const char* role = std::getenv("TTT2_2V2_ROLE");
    const char* mate = std::getenv("TTT2_2V2_TEAMMATE");
    if (role && mate && *mate)
    {
        const Role r = std::strcmp(role, "host") == 0 ? Role::Host : std::strcmp(role, "partner") == 0 ? Role::Partner : Role::None;
        if (r != Role::None)
        {
            g_role = r;
            g_teammate = std::strtoull(mate, nullptr, 16);
            std::fprintf(stderr, "[2v2] team from the environment: %s, teammate %016llX\n", r == Role::Host ? "host" : "partner",
                         (unsigned long long)g_teammate.load());
        }
    }
    return true;
}();

// Tests (TTT2_2V2_TEAMFILE=1): C:/ttt2/port/team2v2<TTT2_INSTANCE>.txt "host|partner|none <teammate peer hex>", read
// once a second - roles set after the room has picked its fighters.
void TeamFileLoop()
{
    const char* tag = std::getenv("TTT2_INSTANCE");
    const std::string path = std::string("C:/ttt2/port/team2v2") + (tag ? tag : "") + ".txt";
    std::string last;
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) continue;
        char role[16] = {}, mate[32] = {};
        const int n = std::fscanf(f, "%15s %31s", role, mate);
        std::fclose(f);
        const std::string now = std::string(role) + " " + mate;
        if (n < 1 || now == last) continue;
        last = now;
        const Role r = std::strcmp(role, "host") == 0 ? Role::Host : std::strcmp(role, "partner") == 0 ? Role::Partner : Role::None;
        SetTeam(r, n > 1 ? std::strtoull(mate, nullptr, 16) : 0);
    }
}
const bool g_teamFile = [] {
    const char* e = std::getenv("TTT2_2V2_TEAMFILE");
    if (e && *e == '1') std::thread(TeamFileLoop).detach();
    return true;
}();

// ---- the partner's controller ----
// Wire: one packet per change (and a 16 ms heartbeat), each the whole state - a lost one is replaced by the next.
constexpr uint32_t kPadMagic = 0x32763250u;    // "P2v2"
#pragma pack(push, 1)
struct PadPacket
{
    uint32_t magic;
    uint32_t seq;
    uint16_t buttons;
    uint8_t leftTrigger, rightTrigger;
    int16_t lx, ly, rx, ry;
};
#pragma pack(pop)
static_assert(sizeof(PadPacket) == 20);

std::mutex g_padLock;
PadData g_remote{};                             // host: the partner's latest state
uint32_t g_remoteSeq = 0;
int64_t g_remoteMs = INT64_MIN / 2;
uint64_t g_remoteCount = 0;

// ---- a fighter's inputs, straight to the room (Phase 2 measurements) ----
// Each packet carries the newest 8 frames of the side's entries, so a lost one costs nothing. An entry is what NACE
// sends of a frame (StoredEntry): its first word and its last - a flag byte and the input. Only the input went once, the
// flag made 1 on the partner, which NACE gives the side's player (MEASURED 2026-10-25: 0 in the red side's entries, 1 in
// the blue side's): both partners' fights left the fighters' at the same frame and stayed apart (one leader's game
// over, the partners' waiting for a go that never came).
constexpr uint32_t kInputMagic = 0x3276324Au;  // "J2v2" (whole entries; "I2v2" packets carried only the input)
#pragma pack(push, 1)
struct InputPacket
{
    uint32_t magic;
    uint8_t side, count;
    uint16_t valid;                             // bit i: entry i is the sync engine's (not known yet otherwise)
    int32_t newest;                             // record frame of entry 0; entry i is for newest - i
    uint32_t head[8], tail[8];                  // the entries' first and last words
};
#pragma pack(pop)
std::mutex g_inLock;
uint32_t g_sentHead[64], g_sentTail[64];        // this fighter's entries by frame % 64 ...
int32_t g_sentFrame[64];                        // ... and the frame each one is for (-1: none)
int32_t g_sentNewest = -1;
// Arrival: the first time each frame of each side was heard of here, by frame % 4096 (diagnostics).
int64_t g_arrivedMs[2][4096];
int32_t g_arrivedFrame[2][4096];
uint32_t g_arrivedHead[2][4096], g_arrivedTail[2][4096];
int32_t g_newestHeard[2] = { -1, -1 };

void ClearSide(int side)
{
    for (int32_t& f : g_arrivedFrame[side]) f = -1;
    g_newestHeard[side] = -1;
}

// ---- the round flow's go, straight to the partners (see GoFrame) ----
constexpr uint32_t kGoMagic = 0x32763247u;     // "G2v2"
#pragma pack(push, 1)
struct GoPacket { uint32_t magic; uint32_t request; int32_t go; };
#pragma pack(pop)
std::mutex g_goLock;
uint32_t g_goRequest = 0;                      // the newest go a fighter told: its battle request (S+5104) ...
int32_t g_goFrame = -1;                        // ... and frame (-1: none)

const bool g_streamInit = [] {
    for (int32_t& f : g_sentFrame) f = -1;
    ClearSide(0);
    ClearSide(1);
    return true;
}();

} // namespace

// A match's session over: the next match's frames start low again, so nothing of this one may count - a second match
// took the first one's words for the frames both had (the receive table is by frame % 4096) or sent nothing until it
// passed the first one's last frame.
void StreamReset()
{
    {
        std::lock_guard g(g_goLock);
        g_goFrame = -1;                         // a go of this match is no go of the next (its frames start low again)
    }
    std::lock_guard l(g_inLock);
    g_sentNewest = -1;
    for (int32_t& f : g_sentFrame) f = -1;
    ClearSide(0);
    ClearSide(1);
}

// NACE's sync engine (S = nace + 64): the node table at S+4828 (count S+4856), node 0 this machine. A node keeps 36
// input slots of 20 bytes from node+0, in no frame order: {+12 frame (-1: free), +16 a flag byte (the side's player:
// 0 red, 1 blue), +18 the input}, the entry NACE copies being slot +8 (12 bytes: a word, the frame, the flag and the
// input); node +720 the newest frame stored, +724 the current one, +744 the player (side), +748 a spectator.
// sub_826F4698(node, entry, local current frame) stores a received entry (NACE's own receive path, sub_826F7B60; an
// entry for a frame it holds already is refused).
constexpr uint32_t kNace = 0x82E6C5C8u, kSync = kNace + 64;

// The entry a node holds for `frame` - its first word (slot +8) and last (slot +16: the flag and the input) - false: none.
bool StoredEntry(uint8_t* base, uint32_t node, int32_t frame, uint32_t& head, uint32_t& tail)
{
    for (int i = 0; i < 36; ++i)
        if (int32_t(GRead32(base, node + 20 * i + 12)) == frame)
        {
            head = GRead32(base, node + 20 * i + 8);
            tail = GRead32(base, node + 20 * i + 16);
            return true;
        }
    return false;
}

// Called after this fighter parked its side's input for NACE (`newest`: node 0's newest frame). The words sent are the
// ones node 0 holds - exactly what the other machines get - and only once final: NACE makes the entry of its newest
// frame with input 0 and writes the input a frame later (MEASURED 2026-10-25: "frame 189 changed from 0000 to 4000" at
// newest 190, on every frame). Sending the newest word (0000 whenever a button was held) made a partner's game take
// 0000 for good - NACE keeps the first word it gets for a frame - drift from the fight and drop out of the match
// (both partners in a real 4-PC session, 2026-10-24; reproduced on three copies). Frames up to newest - 2 are sent.
void FighterSent(uint8_t* base, int side, int32_t newest)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_2V2_STREAM"); return !(e && *e == '0'); }();   // TTT2_2V2_STREAM=0: off
    const int32_t frame = newest - 2;
    if (!on || frame < 0 || side < 0 || side > 1) return;
    const uint32_t self = GRead32(base, kSync + 4828);
    if (!self) return;
    InputPacket p{};
    {
        std::lock_guard l(g_inLock);
        if (frame + 600 < g_sentNewest)                                // a new match all the same (see StreamReset)
        {
            g_sentNewest = -1;
            for (int32_t& f : g_sentFrame) f = -1;
        }
        if (frame <= g_sentNewest) return;
        // The new frames, and any of the last 8 not held yet when last looked at.
        for (int32_t f = (std::max)(frame - 7, 0); f <= frame; ++f)
        {
            uint32_t head, tail;
            if (g_sentFrame[f & 63] == f)
            {
                // Diagnostics: an entry node 0 changed after it was sent (it must not).
                if (StoredEntry(base, self, f, head, tail) && (head != g_sentHead[f & 63] || tail != g_sentTail[f & 63]))
                {
                    static int changes = 0;
                    if (++changes <= 20)
                        std::fprintf(stderr, "[2v2] stream: node 0's entry for frame %d changed from %08X %08X to %08X %08X after it was sent "
                                             "(newest %d)\n", f, g_sentHead[f & 63], g_sentTail[f & 63], head, tail, newest);
                }
                continue;
            }
            if (StoredEntry(base, self, f, head, tail))
            {
                g_sentHead[f & 63] = head;
                g_sentTail[f & 63] = tail;
                g_sentFrame[f & 63] = f;
            }
        }
        g_sentNewest = frame;
        p.magic = kInputMagic;
        p.side = uint8_t(side);
        p.count = 8;
        p.newest = frame;
        for (int i = 0; i < 8; ++i)
            if (g_sentFrame[(frame - i) & 63] == frame - i)
            {
                p.head[i] = g_sentHead[(frame - i) & 63];
                p.tail[i] = g_sentTail[(frame - i) & 63];
                p.valid |= uint16_t(1u << i);
            }
    }
    uint64_t peers[8];
    const size_t n = XSocketRoomPeers(peers, 8, 0);
    for (size_t i = 0; i < n; ++i) XSocketSend2v2(peers[i], &p, sizeof(p));
}

// Diagnostics: when the record of `frame` was first heard of over the stream (ms, steady clock), -1 if not.
int64_t StreamArrivalMs(int side, int32_t frame)
{
    std::lock_guard l(g_inLock);
    return g_arrivedFrame[side & 1][frame & 4095] == frame ? g_arrivedMs[side & 1][frame & 4095] : -1;
}
int32_t StreamNewest(int side) { std::lock_guard l(g_inLock); return g_newestHeard[side & 1]; }

static uint64_t g_injected = 0, g_injectRuns = 0;
static int32_t g_injectedFrame[2][4096], g_injectedAt[2][4096];   // diagnostics: what the stream gave the sync engine, and when
} // namespace online2v2
extern "C" PPC_FUNC(__imp__sub_826F4698);
namespace online2v2 {

// The round flow's ready protocol: the battle asks (S+5104++) and waits until a go is applied (S+5100 == S+5104); the
// host announces the go frame (S+5088), and sub_826F6288 applies it (S+5092) only in the frame node 0 is AT that frame
// - one passed is never applied. Natively a spectator runs a second or more behind and always hears of the go first
// (it comes the slow way, with the room host's relay). Fed by the stream it runs close to the fighters and passed the
// go before it heard of it: it waited 900 frames and its session ended (MEASURED 2026-10-25, three copies, PRIORITIZE
// RESPONSE, 25 ms - the partners' drop in the real session too). So while it waits for a go it has not heard of, the
// stream gives it nothing; when it has, nothing past it; and no catch-up while it waits (catch-up frames skip the check).
bool WaitingForGo(uint8_t* base) { return GRead32(base, kSync + 5100) != GRead32(base, kSync + 5104); }
bool GoKnown(uint8_t* base, int32_t& go)
{
    go = int32_t(GRead32(base, kSync + 5088));
    return go != int32_t(GRead32(base, kSync + 5092));
}

// Diagnostics: the sync engine's view for a spectator's catch-up / stall lines.
std::string NodesBrief(uint8_t* base)
{
    char line[300];
    int n = 0;
    const int32_t count = int32_t(GRead32(base, kSync + 4856));
    for (int i = 0; i < count && i < 7 && n < 220; ++i)
    {
        const uint32_t node = GRead32(base, kSync + 4828 + 4 * i);
        if (!node) continue;
        n += std::snprintf(line + n, sizeof(line) - n, "%s%d:%d/%d%s", i ? " " : "", i, int32_t(GRead32(base, node + 720)),
                           int32_t(GRead32(base, node + 724)), GRead8(base, node + 748) ? "s" : "");
    }
    std::snprintf(line + n, sizeof(line) - n, "; record %d/%d loaded %u pending %d; delay %d", int32_t(GRead32(base, kNace + 5976)),
                  int32_t(GRead32(base, kNace + 5980)), GRead8(base, kNace + 6020), int32_t(GRead32(base, kNace + 5944)),
                  int32_t(GRead32(base, kSync + 5640)));
    return line;
}

int CatchUpFrames(uint8_t* base, int32_t lastSimulated)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_2V2_STREAM"); return !(e && *e == '0'); }();   // TTT2_2V2_STREAM=0: off
    if (!on || lastSimulated < 0 || WaitingForGo(base)) return 0;
    const int32_t count = int32_t(GRead32(base, kSync + 4856));
    if (count < 3) return 0;
    const uint32_t self = GRead32(base, kSync + 4828);
    if (!self || !GRead8(base, self + 748)) return 0;
    int32_t ready = INT32_MAX;                                       // the newest frame every fighter's inputs reach
    int fighters = 0;
    for (int i = 1; i < count && i < 7; ++i)
    {
        const uint32_t node = GRead32(base, kSync + 4828 + 4 * i);
        if (!node || GRead8(base, node + 748)) continue;
        ready = (std::min)(ready, int32_t(GRead32(base, node + 720)));
        ++fighters;
    }
    if (fighters < 2) return 0;
    const int32_t ahead = ready - (lastSimulated + 1);
    const int extra = ahead >= 4 ? (std::min)(2, ahead - 3) : 0;
    static uint64_t calls = 0;
    if (extra > 0 && (++calls <= 40 || calls % 120 == 0))
        std::fprintf(stderr, "[2v2] catch-up #%llu: ready %d, last %d -> %d extra (nodes %s)\n", (unsigned long long)calls, ready,
                     lastSimulated, extra, NodesBrief(base).c_str());
    return extra;
}

void InjectStream(PPCContext& ctx, uint8_t* base)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_2V2_STREAM"); return !(e && *e == '0'); }();   // TTT2_2V2_STREAM=0: off
    if (!on) return;
    const int32_t count = int32_t(GRead32(base, kSync + 4856));
    if (count < 3) return;                                           // a room of two has no spectator
    const uint32_t self = GRead32(base, kSync + 4828);
    if (!self || !GRead8(base, self + 748)) return;                  // this machine fights
    static uint32_t entry = 0;
    if (!entry) entry = xlive::GuestAlloc(16);
    if (!entry) return;
    const int32_t localCurrent = int32_t(GRead32(base, self + 724));
    // Only once its match runs on its own (frame 0 reached): inputs given while it still loaded (frame -1) kept it from
    // starting - it went back to the room (MEASURED 2026-10-24).
    if (localCurrent < 0) return;
    int32_t until = localCurrent + 18;                               // sub_826F4698 takes up to 17 frames ahead
    static int32_t heldAt = -1;
    if (WaitingForGo(base))                                          // see WaitingForGo
    {
        int32_t go;
        bool known = GoKnown(base, go);
        if (!known)                                                  // a fighter told it (GoFrame)?
        {
            uint32_t request;
            int32_t told;
            {
                std::lock_guard l(g_goLock);
                request = g_goRequest;
                told = g_goFrame;
            }
            if (told > localCurrent && told != int32_t(GRead32(base, kSync + 5092)))
            {
                GWrite32(base, kSync + 5088, uint32_t(told));
                known = GoKnown(base, go);
                std::fprintf(stderr, "[2v2-go] the go %d from a fighter (its request %u, here %u; this node at %d)\n", told, request,
                             GRead32(base, kSync + 5104), localCurrent);
            }
        }
        if (heldAt < 0)
        {
            heldAt = localCurrent;
            std::fprintf(stderr, "[2v2] stream: the round flow waits for a go at frame %d (%s)\n", localCurrent,
                         known ? ("go " + std::to_string(go) + " known").c_str() : "not heard of yet: the stream holds");
        }
        if (!known) return;
        until = (std::min)(until, go + 1);
    }
    else if (heldAt >= 0)
    {
        std::fprintf(stderr, "[2v2] stream: the go applied at frame %d (waited from %d)\n", localCurrent, heldAt);
        heldAt = -1;
    }
    for (int i = 1; i < count && i < 7; ++i)
    {
        const uint32_t node = GRead32(base, kSync + 4828 + 4 * i);
        if (!node || GRead8(base, node + 748)) continue;              // another spectator
        const int side = int(GRead32(base, node + 744));
        if (side < 0 || side > 1) continue;
        const int32_t newest = int32_t(GRead32(base, node + 720));
        const int32_t from = (std::max)(newest + 1, localCurrent + 1);
        for (int32_t f = from; f < until; ++f)
        {
            uint32_t head, tail;
            {
                std::lock_guard l(g_inLock);
                if (g_arrivedFrame[side][f & 4095] != f) break;           // in order: the first missing frame ends it
                head = g_arrivedHead[side][f & 4095];
                tail = g_arrivedTail[side][f & 4095];
            }
            GWrite32(base, entry + 0, head);                             // the entry as NACE sends it (StoredEntry)
            GWrite32(base, entry + 4, uint32_t(f));
            GWrite32(base, entry + 8, tail);
            PPCContext c = ctx;
            c.r3.u64 = node;
            c.r4.u64 = entry;
            c.r5.u64 = uint32_t(localCurrent);
            __imp__sub_826F4698(c, base);
            if (int32_t(c.r3.u32) == 0)
            {
                ++g_injected;
                g_injectedFrame[side][f & 4095] = f;                       // diagnostics (CheckRefused)
                g_injectedAt[side][f & 4095] = localCurrent;
            }
        }
    }
    if (++g_injectRuns % 600 == 0)
        std::fprintf(stderr, "[2v2] stream: %llu inputs given to the sync engine so far\n", (unsigned long long)g_injected);
}

void CheckRefused(uint8_t* base, uint32_t node, uint32_t entry)
{
    const uint32_t self = GRead32(base, kSync + 4828);
    if (!self || !GRead8(base, self + 748)) return;                  // a fighter: the stream is not its input
    const int32_t frame = int32_t(GRead32(base, entry + 4));
    uint32_t heldHead, held;
    if (frame < 0 || !StoredEntry(base, node, frame, heldHead, held)) return;
    const uint32_t sent = GRead32(base, entry + 8);
    static uint64_t same = 0, differ = 0;
    if (held == sent)
    {
        if (++same % 3600 == 0)
            std::fprintf(stderr, "[2v2] stream check: %llu words the same as the sync engine's, %llu different\n", (unsigned long long)same,
                         (unsigned long long)differ);
        return;
    }
    ++differ;
    const int side = int(GRead32(base, node + 744)) & 1;
    const bool fromStream = g_injectedFrame[side][frame & 4095] == frame;
    if (differ <= 20 || differ % 100 == 0)
        std::fprintf(stderr, "[2v2] stream check: side %d frame %d - held %08X (%s), the sync engine sent %08X; local current %d, node newest %d "
                             "(%llu different, %llu the same)\n", side, frame, held,
                     fromStream ? ("from the stream at " + std::to_string(g_injectedAt[side][frame & 4095])).c_str() : "not from the stream", sent,
                     int32_t(GRead32(base, self + 724)), int32_t(GRead32(base, node + 720)), (unsigned long long)differ, (unsigned long long)same);
}

Role CurrentRole() { return g_role.load(); }
uint64_t Teammate() { return g_teammate.load(); }
void SetTeam(Role role, uint64_t teammate)
{
    g_role = role;
    g_teammate = teammate;
    std::fprintf(stderr, "[2v2] team: %s, teammate %016llX\n", role == Role::Host ? "host" : role == Role::Partner ? "partner" : "none",
                 (unsigned long long)teammate);
}

void PartnerPadPolled(const PadData& g)
{
    if (g_role.load() != Role::Partner) return;
    const uint64_t mate = g_teammate.load();
    if (!mate) return;
    static uint32_t seq = 0;
    static PadData last{};
    static int64_t lastMs = 0;
    const int64_t now = NowMs();
    if (std::memcmp(&g, &last, sizeof(g)) == 0 && now - lastMs < 16) return;
    last = g;
    lastMs = now;
    const PadPacket p{ kPadMagic, ++seq, g.wButtons, g.bLeftTrigger, g.bRightTrigger, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY };
    XSocketSend2v2(mate, &p, sizeof(p));
}

bool RemotePad(PadData& pad)
{
    if (g_role.load() != Role::Host) return false;
    std::lock_guard l(g_padLock);
    if (NowMs() - g_remoteMs > 1000) return false;
    pad = g_remote;
    return true;
}

void SelectDiag(uint8_t* base);     // online_2v2_select.cpp (research)
void RoomDiag(uint8_t* base);       // online_2v2_select.cpp (research)
void PlacesTestFrame(PPCContext& ctx, uint8_t* base);
uint32_t ModeManager();
void PlacesFrame(PPCContext& ctx, uint8_t* base);                          // online_2v2_places.cpp
bool PlacesReceive(uint64_t from, const uint8_t* data, size_t size);
void PickFrame(uint8_t* base);                                            // online_2v2_pick.cpp
bool PickReceive(uint64_t from, const uint8_t* data, size_t size);

void PairFrame(PPCContext& ctx, uint8_t* base, const std::string& frame);   // online_2v2_pair.cpp

// The round flow's go (WaitingForGo) in a room with spectators: a fighter that knows it tells the room at once, every
// 50 ms until it applies - natively it reaches a spectator the slow way, with the room host's relay, and a partner fed by
// the stream was past it before it heard of it (MEASURED 2026-10-25, four copies: the third round's go never reached the
// partners in time, they waited 373 frames and their sessions ended mid-fight). The partner takes it in InjectStream.
// Every copy logs the flow's changes ([2v2-go]). TTT2_2V2_GORELAY=0: no telling.
void GoFrame(uint8_t* base)
{
    const int32_t count = int32_t(GRead32(base, kSync + 4856));
    if (count < 3) return;
    const uint32_t self = GRead32(base, kSync + 4828);
    if (!self) return;
    const int32_t go = int32_t(GRead32(base, kSync + 5088)), applied = int32_t(GRead32(base, kSync + 5092));
    const uint32_t request = GRead32(base, kSync + 5104), done = GRead32(base, kSync + 5100);
    const int32_t current = int32_t(GRead32(base, self + 724));
    const bool spectator = GRead8(base, self + 748) != 0;
    static int32_t lastGo = INT32_MIN, lastApplied = INT32_MIN;
    static uint32_t lastRequest = ~0u, lastDone = ~0u;
    if (go != lastGo || applied != lastApplied || request != lastRequest || done != lastDone)
    {
        std::fprintf(stderr, "[2v2-go] %s: go %d, applied %d, request %u, done %u; this node at %d, record %d\n",
                     spectator ? "partner" : "fighter", go, applied, request, done, current, int32_t(GRead32(base, kNace + 5976)));
        lastGo = go;
        lastApplied = applied;
        lastRequest = request;
        lastDone = done;
    }
    static const bool relay = [] { const char* e = std::getenv("TTT2_2V2_GORELAY"); return !(e && *e == '0'); }();
    if (!relay || spectator || go == applied || done == request) return;
    static int64_t sentAt = 0;
    const int64_t now = NowMs();
    if (now - sentAt < 50) return;
    sentAt = now;
    const GoPacket p{ kGoMagic, request, go };
    uint64_t peers[8];
    const size_t n = XSocketRoomPeers(peers, 8, 0);
    for (size_t i = 0; i < n; ++i) XSocketSend2v2(peers[i], &p, sizeof(p));
}

void Frame(PPCContext& ctx, uint8_t* base)
{
    const std::string frame = menu_host::CurrentFrame(ctx, base);
    PairFrame(ctx, base, frame);
    TeamFrame(ctx, base, frame);
    SelectDiag(base);
    RoomDiag(base);
    PlacesTestFrame(ctx, base);
    PlacesFrame(ctx, base);
    PickFrame(base);
    GoFrame(base);
    rematch::Frame(ctx, base, frame);
    static const bool diag = [] { const char* e = std::getenv("TTT2_2V2_DIAG"); return e && *e == '1'; }();
    if (!diag) return;
    // Diagnostics: the menu frame and the battle's state on every change (what character select looks like here).
    static std::string lastFrame;
    static uint32_t lastState = ~0u, lastPhase = ~0u;
    const uint32_t state = GRead32(base, 0x82EAC03Cu), phase = GRead32(base, 0x82E88860u);
    if (frame != lastFrame || state != lastState)
    {
        std::fprintf(stderr, "[2v2] screen: menu frame '%s', battle state %u, battle counter %u\n", frame.c_str(), state, phase);
        lastFrame = frame;
        lastState = state;
        lastPhase = phase;
    }
}

void Receive(uint64_t from, const uint8_t* data, size_t size)
{
    if (PlacesReceive(from, data, size)) return;
    if (PickReceive(from, data, size)) return;
    if (TeamReceive(from, data, size)) return;
    if (stagemirror::Receive(from, data, size)) return;
    if (rematch::Receive(from, data, size)) return;
    if (size >= sizeof(GoPacket))
    {
        GoPacket p;
        std::memcpy(&p, data, sizeof(p));
        if (p.magic == kGoMagic)
        {
            std::lock_guard l(g_goLock);
            if (p.go != g_goFrame) std::fprintf(stderr, "[2v2-go] a fighter tells the go %d (its request %u)\n", p.go, p.request);
            g_goRequest = p.request;
            g_goFrame = p.go;
            return;
        }
    }
    if (size >= sizeof(InputPacket))
    {
        InputPacket p;
        std::memcpy(&p, data, sizeof(p));
        if (p.magic == kInputMagic && p.side < 2 && p.count <= 8)
        {
            std::lock_guard l(g_inLock);
            const int64_t now = NowMs();
            if (p.newest + 600 < g_newestHeard[p.side]) ClearSide(p.side);   // a new match all the same (see StreamReset)
            for (int i = 0; i < p.count; ++i)
            {
                const int32_t f = p.newest - i;
                if (!(p.valid & (1u << i)) || f < 0 || g_arrivedFrame[p.side][f & 4095] == f) continue;
                g_arrivedFrame[p.side][f & 4095] = f;
                g_arrivedMs[p.side][f & 4095] = now;
                g_arrivedHead[p.side][f & 4095] = p.head[i];
                g_arrivedTail[p.side][f & 4095] = p.tail[i];
            }
            if (p.newest > g_newestHeard[p.side]) g_newestHeard[p.side] = p.newest;
            return;
        }
    }
    if (size >= sizeof(PadPacket))
    {
        PadPacket p;
        std::memcpy(&p, data, sizeof(p));
        if (p.magic == kPadMagic)
        {
            if (g_role.load() != Role::Host || from != g_teammate.load()) return;
            std::lock_guard l(g_padLock);
            const bool fresh = NowMs() - g_remoteMs > 1000;
            if (!fresh && int32_t(p.seq - g_remoteSeq) <= 0) return;     // late or repeated
            if (fresh) std::fprintf(stderr, "[2v2] the partner's controller is here (%016llX)\n", (unsigned long long)from);
            g_remoteSeq = p.seq;
            g_remoteMs = NowMs();
            g_remote = PadData{ p.buttons, p.leftTrigger, p.rightTrigger, p.lx, p.ly, p.rx, p.ry };
            ++g_remoteCount;
            return;
        }
    }
}

} // namespace online2v2

