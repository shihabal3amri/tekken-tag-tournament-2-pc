// GIN upload research (TTT2_GIN_TRACE=1; nothing changes without it): how the game decides to upload a ghost or a
// replay, traced step by step (2026-10-22; the findings in gin_uploads.cpp, server/PROTOCOL.md "GIN"). The game's GIN
// client keeps one object per upload kind (manager 0x82E6C5B8: ghost +7712, replay +7984, match report +8392, record
// +8296), run by tasks (+6872 ghost, +6992 replay, both under the coordinator +7176 that a ranked match's end starts).
//   - the match-end ghost step (sub_825590F8): the winner's (+666) ghosts when the match did not end in a tie-break
//     (+688 != 3), in ranked mode (the object at +7488: [+0] 0), queued by sub_826E5560 if the player's best rank is
//     >= 10 (sub_826E6960) and the ghost belongs to one of their fighters (sub_826E54D0)
//   - the ghost upload's begin (sub_826E5258): both sides' players there, ranked mode, rank >= 10, no replay uploaded
//     this match (replay object +48), the last ghost upload a day ago (+40)
//   - the replay: armed at match start (sub_826EDD70: match type 1 or 8), its data handed over a few seconds after the
//     match (sub_826ED7B8, <= 64 KB), uploaded by the winner when both sides qualify, per rank a number per session
//   - an upload that failed (sub_8266C978) makes the close-game state cancel the rest (sub_826655C8)
// Research switches: TTT2_GIN_FORCE_WINNER=<side> (the ghost step's winner), TTT2_GIN_FORCE_DAN=1 (the two rank checks
// see 1st dan).
#include "kernel_shared.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>

extern "C" PPC_FUNC(__imp__sub_826E56B8);
extern "C" PPC_FUNC(__imp__sub_826EDCF8);
extern "C" PPC_FUNC(__imp__sub_826EDE30);
extern "C" PPC_FUNC(__imp__sub_826EAE68);
extern "C" PPC_FUNC(__imp__sub_826E93C0);
extern "C" PPC_FUNC(__imp__sub_825590F8);
extern "C" PPC_FUNC(__imp__sub_8266C808);
extern "C" PPC_FUNC(__imp__sub_823069E0);
extern "C" PPC_FUNC(__imp__sub_82291F60);
extern "C" PPC_FUNC(__imp__sub_82653250);
extern "C" PPC_FUNC(__imp__sub_825B7370);
extern "C" PPC_FUNC(__imp__sub_82654CE8);
extern "C" PPC_FUNC(__imp__sub_82653478);
extern "C" PPC_FUNC(__imp__sub_825548F0);
extern "C" PPC_FUNC(__imp__sub_8266C038);
extern "C" PPC_FUNC(__imp__sub_8266BFF8);
extern "C" PPC_FUNC(__imp__sub_8266B3B0);
extern "C" PPC_FUNC(__imp__sub_8266C5F0);
extern "C" PPC_FUNC(__imp__sub_8266C5E0);
extern "C" PPC_FUNC(__imp__sub_826E5258);
extern "C" PPC_FUNC(__imp__sub_826EDC60);
extern "C" PPC_FUNC(__imp__sub_826EF378);
extern "C" PPC_FUNC(__imp__sub_826E6960);
extern "C" PPC_FUNC(__imp__sub_826E54D0);
extern "C" PPC_FUNC(__imp__sub_826E7F50);
extern "C" PPC_FUNC(__imp__sub_8265A1A8);
extern "C" PPC_FUNC(__imp__sub_826ED7B8);
extern "C" PPC_FUNC(__imp__sub_826B0C18);
extern "C" PPC_FUNC(__imp__sub_826EDD70);
extern "C" PPC_FUNC(__imp__sub_826ED878);
extern "C" PPC_FUNC(__imp__sub_826656F8);
extern "C" PPC_FUNC(__imp__sub_82657B30);
extern "C" PPC_FUNC(__imp__sub_826B0FC0);
extern "C" PPC_FUNC(__imp__sub_826EA6B8);
extern "C" PPC_FUNC(__imp__sub_826E91E8);
extern "C" PPC_FUNC(__imp__sub_826655C8);
extern "C" PPC_FUNC(__imp__sub_826E5128);
extern "C" PPC_FUNC(__imp__sub_8266C748);
extern "C" PPC_FUNC(__imp__sub_8266C830);
extern "C" PPC_FUNC(__imp__sub_826EF3D8);
extern "C" PPC_FUNC(__imp__sub_826ED9C0);

namespace gin_trace {
bool On()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_GIN_TRACE"); return e && *e == '1'; }();
    return on;
}
void GhostStep(bool begin, uint32_t lr);
} // namespace gin_trace
using gin_trace::On;

namespace {
void Gate(const char* what, uint32_t result, uint32_t lr, uint32_t& last)
{
    if (On() && result != last)
    {
        last = result;
        std::fprintf(stderr, "[gin-trace] %s upload retry check -> %u (lr %08X)\n", what, result, lr);
    }
}

thread_local int g_inGhostStep = 0;

unsigned long long Ms() { return GetTickCount64() % 1000000; }
} // namespace

PPC_FUNC(sub_826EDCF8)
{
    static uint32_t last = 0xFFFFFFFFu;
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_826EDCF8(ctx, base);
    Gate("replay", ctx.r3.u32, lr, last);
}

PPC_FUNC(sub_826E56B8)
{
    const uint32_t obj = ctx.r3.u32, lr = uint32_t(ctx.lr);
    __imp__sub_826E56B8(ctx, base);
    if (On()) std::fprintf(stderr, "[gin-trace] ghost upload body -> %u (object %08X, queued %d, lr %08X)\n", ctx.r3.u32 & 0xFF, obj,
                            int32_t(GRead32(base, obj + 12)), lr);
}

PPC_FUNC(sub_826EDE30)
{
    const uint32_t obj = ctx.r3.u32, lr = uint32_t(ctx.lr);
    __imp__sub_826EDE30(ctx, base);
    if (On()) std::fprintf(stderr, "[gin-trace] replay upload body -> %u (object %08X, lr %08X)\n", ctx.r3.u32 & 0xFF, obj, lr);
}

// The replay object's second interface (at +4) [2]: the recorded replay handed over (kept only while armed, at most
// 64 KB) - the upload waits for it.
PPC_FUNC(sub_826ED7B8)
{
    const uint32_t self = ctx.r3.u32, size = ctx.r5.u32, lr = uint32_t(ctx.lr);
    const uint8_t armed = base[self + 12];
    __imp__sub_826ED7B8(ctx, base);
    if (On())
        std::fprintf(stderr, "[gin-trace] %llu ms: replay data handed over: %u bytes, armed %u -> kept %u (lr %08X)\n", Ms(), size,
                     armed,
                     base[self + 12] && GRead32(base, self + 8) ? 1u : 0u, lr);
}

// The replay upload's arm (interface [1], at match start: armed if both players are valid and the match type is 1
// or 8) and reset ([6])
PPC_FUNC(sub_826EDD70)
{
    const uint32_t self = ctx.r3.u32, lr = uint32_t(ctx.lr);
    __imp__sub_826EDD70(ctx, base);
    if (On()) std::fprintf(stderr, "[gin-trace] replay upload armed -> %u (lr %08X)\n", base[self + 12], lr);
}

PPC_FUNC(sub_826ED878)
{
    if (On()) std::fprintf(stderr, "[gin-trace] %llu ms: replay upload reset (lr %08X)\n", Ms(), uint32_t(ctx.lr));
    __imp__sub_826ED878(ctx, base);
}

// The online close-game state (GameMode::state_gamemode_online_close_game_start) cancels the ghost + replay upload
// coordinator (sub_826655C8, every frame) once an upload failed (sub_8266C978: ghost +52, replay +49, match report
// +36, 8040's +32); logged once per change of those flags.
PPC_FUNC(sub_826655C8)
{
    static uint32_t last = 0xFFFFFFFFu;
    const uint32_t m = ctx.r3.u32;
    const uint32_t flags = base[m + 7764] | base[m + 8033] << 8 | base[m + 8428] << 16 | uint32_t(base[m + 8072]) << 24;
    if (On() && flags != last)
    {
        last = flags;
        std::fprintf(stderr, "[gin-trace] %llu ms: uploads cancelled: failed ghost %u replay %u match %u 8040 %u (lr %08X)\n", Ms(),
                     base[m + 7764], base[m + 8033], base[m + 8428], base[m + 8072], uint32_t(ctx.lr));
    }
    __imp__sub_826655C8(ctx, base);
}

// The ghost upload's wait (vtable [3]: while the save worker writes) and finish ([5]: false = failed, +52)
PPC_FUNC(sub_826E5128)
{
    if (On()) std::fprintf(stderr, "[gin-trace] %llu ms: ghost upload finished, ok %u (lr %08X)\n", Ms(), ctx.r4.u32 & 0xFF, uint32_t(ctx.lr));
    __imp__sub_826E5128(ctx, base);
}

// The GIN HTTP object's completion (sub_826B0FC0): the request's status (+228; negative = failed) and the result
// (+201) the upload's finish gets
PPC_FUNC(sub_826B0FC0)
{
    const uint32_t http = ctx.r3.u32;
    const uint8_t before = base[http + 201];
    __imp__sub_826B0FC0(ctx, base);
    if (On())
    {
        static uint32_t lastStatus = 0x12345678u;
        const uint32_t status = GRead32(base, http + 228);
        if (status != lastStatus || base[http + 201] != before)
        {
            lastStatus = status;
            std::fprintf(stderr, "[gin-trace] %llu ms: GIN request done: status %08X result %u (type %u)\n", Ms(), status,
                         base[http + 201], GRead32(base, http + 96));
        }
    }
}

// the match report's and the offline record's finish (vtable [5]): false = failed
PPC_FUNC(sub_826EA6B8)
{
    if (On()) std::fprintf(stderr, "[gin-trace] %llu ms: match report finished, ok %u\n", Ms(), ctx.r4.u32 & 0xFF);
    __imp__sub_826EA6B8(ctx, base);
}

PPC_FUNC(sub_826E91E8)
{
    if (On()) std::fprintf(stderr, "[gin-trace] %llu ms: offline record finished, ok %u\n", Ms(), ctx.r4.u32 & 0xFF);
    __imp__sub_826E91E8(ctx, base);
}

// The GIN client's "cancel everything" (sub_826656F8): EndMode's network release and OnlineTerm's cancel wait
PPC_FUNC(sub_826656F8)
{
    if (On()) std::fprintf(stderr, "[gin-trace] %llu ms: GIN work cancelled (lr %08X)\n", Ms(), uint32_t(ctx.lr));
    __imp__sub_826656F8(ctx, base);
}

// The GIN client's HTTP request (sub_826B0C18(http, endpoint id, body, ...)): false = not sent
PPC_FUNC(sub_826B0C18)
{
    const uint32_t id = ctx.r4.u32, lr = uint32_t(ctx.lr);
    __imp__sub_826B0C18(ctx, base);
    if (On()) std::fprintf(stderr, "[gin-trace] GIN request %u -> %u (lr %08X)\n", id, ctx.r3.u32 & 0xFF, lr);
}

PPC_FUNC(sub_826EAE68)
{
    if (On()) std::fprintf(stderr, "[gin-trace] match report body (object %08X, lr %08X)\n", ctx.r3.u32, uint32_t(ctx.lr));
    __imp__sub_826EAE68(ctx, base);
}

PPC_FUNC(sub_826E93C0)
{
    if (On()) std::fprintf(stderr, "[gin-trace] offline record body (object %08X, lr %08X)\n", ctx.r3.u32, uint32_t(ctx.lr));
    __imp__sub_826E93C0(ctx, base);
}

// the match-end ghost step (hooked in gin_uploads.cpp): the checks it makes are logged while it runs
void gin_trace::GhostStep(bool begin, uint32_t lr)
{
    if (begin)
    {
        if (On()) std::fprintf(stderr, "[gin-trace] %llu ms: match end: ghost step (lr %08X)\n", Ms(), lr);
        ++g_inGhostStep;
    }
    else --g_inGhostStep;
}

PPC_FUNC(sub_823069E0)
{
    __imp__sub_823069E0(ctx, base);
    if (On() && g_inGhostStep) std::fprintf(stderr, "[gin-trace]   sub_823069E0 -> %u (0 = go on)\n", ctx.r3.u32);
}

PPC_FUNC(sub_825590F8)
{
    if (On())
    {
        // the match-info object the step reads (sub_825B7370): per side +528/+532 (non-zero: skip that side, if
        // +472), +666 local side, +684 (must be 0), +688 (3: no upload)
        PPCContext c = ctx;
        __imp__sub_825B7370(c, base);
        const uint32_t info = c.r3.u32;
        if (info)
            std::fprintf(stderr, "[gin-trace]   ghost step for %08X: info %08X +472 %08X +528 %08X +532 %08X +666 %u +684 %08X +688 %u\n",
                         ctx.r3.u32, info, GRead32(base, info + 472), GRead32(base, info + 528), GRead32(base, info + 532),
                         base[info + 666], GRead32(base, info + 684), base[info + 688]);
        else
            std::fprintf(stderr, "[gin-trace]   ghost step for %08X: no match info\n", ctx.r3.u32);
        // +666 is the side that won (MEASURED: 1 after the bot lost a Ghost Battle, 0 in a ranked match side 0
        // won): only a winner's ghost is uploaded. TTT2_GIN_FORCE_WINNER=<side> names the winner for this step only
        // (research: the upload's format without having to win), then the real one is put back.
        static const char* force = std::getenv("TTT2_GIN_FORCE_WINNER");
        if (info && force && (*force == '0' || *force == '1'))
        {
            const uint8_t real = base[info + 666];
            base[info + 666] = uint8_t(*force - '0');
            std::fprintf(stderr, "[gin-trace]   winner %u taken as %u for this step\n", real, base[info + 666]);
            __imp__sub_825590F8(ctx, base);
            base[info + 666] = real;
            return;
        }
    }
    __imp__sub_825590F8(ctx, base);
}

PPC_FUNC(sub_82291F60)
{
    __imp__sub_82291F60(ctx, base);
    if (On() && g_inGhostStep) std::fprintf(stderr, "[gin-trace]   sub_82291F60 -> %u (0 = go on)\n", ctx.r3.u32 & 0xFF);
}

PPC_FUNC(sub_82653250)
{
    __imp__sub_82653250(ctx, base);
    if (On() && g_inGhostStep) std::fprintf(stderr, "[gin-trace]   sub_82653250 (mode) -> %u (1 = go on)\n", ctx.r3.u32);
}

// The GIN status reads the requests check: sub_8266C038 (manager +7488: [+8], negative = down), sub_8266BFF8 (a
// request's session [+0], 0 = up) and sub_8266B3B0 (the network check); logged when the answer changes.
PPC_FUNC(sub_8266C038)
{
    static uint32_t last = 0xFFFFFFFEu;
    const uint32_t lr = uint32_t(ctx.lr), obj = ctx.r3.u32;
    __imp__sub_8266C038(ctx, base);
    if (On() && ctx.r3.u32 != last)
    {
        last = ctx.r3.u32;
        std::fprintf(stderr, "[gin-trace] GIN state (%08X +8) -> %d (lr %08X)\n", obj, int32_t(ctx.r3.u32), lr);
    }
}

PPC_FUNC(sub_8266BFF8)
{
    static uint32_t last = 0xFFFFFFFEu;
    const uint32_t lr = uint32_t(ctx.lr), obj = ctx.r3.u32;
    __imp__sub_8266BFF8(ctx, base);
    if (On() && ctx.r3.u32 != last)
    {
        last = ctx.r3.u32;
        std::fprintf(stderr, "[gin-trace] GIN session (%08X) -> %d (lr %08X)\n", obj, int32_t(ctx.r3.u32), lr);
    }
}

PPC_FUNC(sub_8266B3B0)
{
    static uint32_t last = 0xFFFFFFFEu;
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_8266B3B0(ctx, base);
    if (On() && (ctx.r3.u32 & 0xFF) != last)
    {
        last = ctx.r3.u32 & 0xFF;
        std::fprintf(stderr, "[gin-trace] GIN network check -> %u (lr %08X)\n", last, lr);
    }
}

// The manager's setters of that object: [+0] (sub_8266C5F0; the ghost request needs 0) and [+8] (sub_8266C5E0: 10
// when an online match is set up, 11 after it, -1 otherwise)
PPC_FUNC(sub_8266C5F0)
{
    if (On()) std::fprintf(stderr, "[gin-trace] GIN mode [+0] = %d (lr %08X)\n", int32_t(ctx.r4.u32), uint32_t(ctx.lr));
    __imp__sub_8266C5F0(ctx, base);
}

PPC_FUNC(sub_8266C5E0)
{
    if (On()) std::fprintf(stderr, "[gin-trace] GIN state [+8] = %d (lr %08X)\n", int32_t(ctx.r4.u32), uint32_t(ctx.lr));
    __imp__sub_8266C5E0(ctx, base);
}

// per side in the ghost step: the user slot of the side (sub_82653478: byte 0 must be 1) and the ghost data check
// (sub_825548F0: non-zero = go on)
PPC_FUNC(sub_82653478)
{
    const uint32_t side = ctx.r4.u32;
    __imp__sub_82653478(ctx, base);
    if (On() && g_inGhostStep)
        std::fprintf(stderr, "[gin-trace]   user slot of side %u: %08X byte0 %u\n", side, ctx.r3.u32, ctx.r3.u32 ? base[ctx.r3.u32] : 0);
}

PPC_FUNC(sub_825548F0)
{
    __imp__sub_825548F0(ctx, base);
    if (On() && g_inGhostStep) std::fprintf(stderr, "[gin-trace]   ghost data check -> %u (non-zero = go on)\n", ctx.r3.u32);
}

PPC_FUNC(sub_8266C808)
{
    if (On()) std::fprintf(stderr, "[gin-trace] ghost upload REQUESTED (lr %08X)\n", uint32_t(ctx.lr));
    __imp__sub_8266C808(ctx, base);
    // the ghost object (manager +7712) queues requests at +16 (8 bytes each), +12 = how many
    if (On()) std::fprintf(stderr, "[gin-trace]   ghost requests queued: %d\n", int32_t(GRead32(base, 0x82E6C5B8 + 7712 + 12)));
}

// The ghost request's checks (sub_826E5560): the GIN user (sub_826EF378) must be the side's player, then
// sub_826E6960 (the user slot) and sub_826E54D0 (the ghost) must say yes, and sub_826E7F50 stores it in the queue
PPC_FUNC(sub_826EF378)
{
    __imp__sub_826EF378(ctx, base);
    if (On() && g_inGhostStep) std::fprintf(stderr, "[gin-trace]     GIN user -> %d\n", int32_t(ctx.r3.u32));
}

PPC_FUNC(sub_826E6960)
{
    __imp__sub_826E6960(ctx, base);
    if (On() && g_inGhostStep) std::fprintf(stderr, "[gin-trace]     slot check -> %u\n", ctx.r3.u32 & 0xFF);
}

PPC_FUNC(sub_826E54D0)
{
    __imp__sub_826E54D0(ctx, base);
    if (On() && g_inGhostStep) std::fprintf(stderr, "[gin-trace]     ghost check -> %u\n", ctx.r3.u32 & 0xFF);
}

PPC_FUNC(sub_826E7F50)
{
    __imp__sub_826E7F50(ctx, base);
    if (On() && g_inGhostStep) std::fprintf(stderr, "[gin-trace]     queue store -> %u\n", ctx.r3.u32 & 0xFF);
}

// The player's rank as the uploads see it (sub_8265A1A8: the higher of the two fighters' ranks, at most 39). Ghosts
// (sub_826E6960) and replays (sub_826EDAF8) are uploaded only from 10 (1st dan) up; replays per rank: 10-16 one,
// 17-24 two, 25-32 ten, above that any number. TTT2_GIN_FORCE_DAN=1 (research: the uploads without playing a test
// player up to 1st dan) makes those two checks see 1st dan; every other use of the rank is left alone.
PPC_FUNC(sub_8265A1A8)
{
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_8265A1A8(ctx, base);
    static const bool force = [] { const char* e = std::getenv("TTT2_GIN_FORCE_DAN"); return e && *e == '1'; }();
    if (force && (lr == 0x826E6988u || lr == 0x826EDC00u) && int32_t(ctx.r3.u32) < 10)
    {
        if (On()) std::fprintf(stderr, "[gin-trace]     rank %d taken as 1st dan (lr %08X)\n", int32_t(ctx.r3.u32), lr);
        ctx.r3.u32 = 10;
    }
}

// The upload classes' begin methods (vtable [1]): the GIN client calls them when it starts an upload.
PPC_FUNC(sub_826E5258)
{
    const uint32_t obj = ctx.r3.u32, lr = uint32_t(ctx.lr);
    if (On())
    {
        // its checks: the network, both sides' user slots valid (sub_82657B30), the session's mode 0
        PPCContext c = ctx;
        __imp__sub_8266B3B0(c, base);
        const uint32_t net = c.r3.u32 & 0xFF, game = GRead32(base, 0x82E6C5A8);
        uint32_t valid[2] = { 9, 9 };
        for (uint32_t side = 0; game && side < 2; ++side)
        {
            c = ctx;
            c.r3.u32 = GRead32(base, game + 256);
            c.r4.u32 = side;
            __imp__sub_82653478(c, base);
            const uint32_t slot = c.r3.u32;
            if (!slot) { valid[side] = 8; continue; }
            c = ctx;
            c.r3.u32 = slot;
            __imp__sub_82657B30(c, base);
            valid[side] = c.r3.u32 & 0xFF;
        }
        const uint32_t session = GRead32(base, obj + 4);
        std::fprintf(stderr, "[gin-trace] %llu ms: ghost upload begin checks: network %u, slots valid %u %u (8 = none), mode %d\n",
                     Ms(), net, valid[0], valid[1], session ? int32_t(GRead32(base, session)) : -99);
    }
    __imp__sub_826E5258(ctx, base);
    if (On())
        std::fprintf(stderr, "[gin-trace] ghost upload begin -> %u (queued %d, mode %d, lr %08X)\n", ctx.r3.u32 & 0xFF,
                     int32_t(GRead32(base, obj + 12)), int32_t(GRead32(base, 0x82E6C5B8 + 7488)), lr);
}

PPC_FUNC(sub_826EDC60)
{
    const uint32_t obj = ctx.r3.u32, lr = uint32_t(ctx.lr);
    __imp__sub_826EDC60(ctx, base);
    if (On())
        std::fprintf(stderr, "[gin-trace] replay upload begin: enabled %u pending %u (mask %08X, lr %08X)\n", base[obj + 16],
                     base[obj + 24], GRead32(base, obj + 20), lr);
}

// The replay upload: sub_82654CE8 (needs its +60/+64 set and +152 clear) asks the manager whether GIN is up
// (sub_8266C748) and sub_823069E0, then sub_8266C830 requests it (replay object sub_826ED9C0) unless the
// manager's +16 user check sub_826EF3D8 says no.
PPC_FUNC(sub_82654CE8)
{
    if (On())
        std::fprintf(stderr, "[gin-trace] replay step %08X: +60 %08X +64 %08X +152 %08X (lr %08X)\n", ctx.r3.u32,
                     GRead32(base, ctx.r3.u32 + 60), GRead32(base, ctx.r3.u32 + 64), GRead32(base, ctx.r3.u32 + 152),
                     uint32_t(ctx.lr));
    ++g_inGhostStep;
    __imp__sub_82654CE8(ctx, base);
    --g_inGhostStep;
}

PPC_FUNC(sub_8266C748)
{
    __imp__sub_8266C748(ctx, base);
    if (On() && g_inGhostStep) std::fprintf(stderr, "[gin-trace]   sub_8266C748 (GIN up) -> %u (1 = go on)\n", ctx.r3.u32 & 0xFF);
}

PPC_FUNC(sub_8266C830)
{
    if (On()) std::fprintf(stderr, "[gin-trace]   replay upload asked (lr %08X)\n", uint32_t(ctx.lr));
    __imp__sub_8266C830(ctx, base);
}

PPC_FUNC(sub_826EF3D8)
{
    __imp__sub_826EF3D8(ctx, base);
    if (On() && g_inGhostStep) std::fprintf(stderr, "[gin-trace]   sub_826EF3D8 (user check) -> %u (0 = go on)\n", ctx.r3.u32 & 0xFF);
}

PPC_FUNC(sub_826ED9C0)
{
    const uint32_t obj = ctx.r3.u32;
    if (On()) std::fprintf(stderr, "[gin-trace] replay upload REQUESTED (object %08X, lr %08X)\n", obj, uint32_t(ctx.lr));
    __imp__sub_826ED9C0(ctx, base);
    if (On())
    {
        // [+20]: a bit per party member whose byte at user slot +4750 (+4288 per member) is 1..3 (none: no upload)
        std::fprintf(stderr, "[gin-trace]   replay members mask %08X; bytes:", GRead32(base, obj + 20));
        const uint32_t game = GRead32(base, 0x82E6C5A8);
        for (uint32_t side = 0; game && side < 2; ++side)
        {
            PPCContext c = ctx;
            c.r3.u32 = GRead32(base, game + 256);
            c.r4.u32 = side;
            __imp__sub_82653478(c, base);
            const uint32_t slot = c.r3.u32;
            if (slot) std::fprintf(stderr, " side %u: %u %u", side, base[slot + 4750], base[slot + 4750 + 4288]);
        }
        std::fprintf(stderr, "\n");
    }
}
