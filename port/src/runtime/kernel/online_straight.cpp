// After a room's match: REMATCH, STAGE SELECT and CHARACTER SELECT straight from the end menu (online_2v2.h namespace
// straight; port/ONLINE_2V2_PLAN.md "Native flow", item 7). Research: C:/ttt2wt/re/re_m_plan.md, re_m_verified.md.
//
// The room is not visited between the matches. Every PC of the match runs, from GameMode's online result (the port's end
// menu, online_rematch.cpp), what the room does to start a match - the session's barrier and start (the room control's
// sub_826BEF60 .. sub_826BB8D0: XSessionStart) and the battle start's sync-engine start (OnlineInRoom::state_battle_start,
// sub_825FB650) - and then:
//   REMATCH: the battle setup is exchanged again from what the last match left (as OnlineQuickMatch's no-select start,
//     sub_82612D58 / sub_82612C40, does: the same characters, costumes and stage) and the same GameMode starts its next
//     fight the way the offline REMATCH does (the game's restart, sub_825E2760: back to state_makuai_init - the VS screen,
//     no mode change). GameMode's teardown never runs between the matches.
//   CHARACTER SELECT: character select is asked for (mode 8, the battle start's flags). GameMode's way out stops the sync
//     engine (state_end's sub_8266C608 at 0x825DB884): that one call is skipped for the engine just started.
//   STAGE SELECT: CHARACTER SELECT with this PC's characters kept - the game's own "keep this side's characters" select
//     (S+776+side 0, the arcade challenger's), sent on to the stage panel (cursor state 39 -> 20) instead of done.
//   CHANGE TEAMS (a PAIR PLAY room): the PAIR PLAY screen again for the same players (mode 22, its PAIR sequence), whose
//     places then start the match as the room's start does - no sync engine started here.
// Only when every member of the room played the match (the end menu shows only then: online_rematch.cpp). The room host
// first counts the last match as the room would (the win streaks) with the order kept. A step that fails takes the PC back
// into the room the game's way: nothing started - RETURN TO ROOM; the session started - the game's close after a match (the
// session's end, the room's entry: online2v2::MatchCloseStep) first, and when the room cannot be entered GameMode's own
// way out of a Player Match (state_gamemode_player_match_term, as its room entry does).
#include "online_2v2.h"
#undef PlaySound
#include "menu_host.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

PPC_EXTERN_FUNC(sub_82B35088);   // lib: a session handle's session object (+80 the room settings)
PPC_EXTERN_FUNC(sub_82B352B8);   // lib (r3 [R +4], 1, 0): the session's start barrier - 0: refused
PPC_EXTERN_FUNC(sub_82B352D0);   // ... its pump
PPC_EXTERN_FUNC(sub_82B352E8);   // ... its result: 0 going on, 1 every member there, 2 failed
PPC_EXTERN_FUNC(sub_82B35300);   // ... its end (after either answer, or to call it off)
PPC_EXTERN_FUNC(sub_826B8640);   // the online manager: every connected session member is in the room (+24 1)
PPC_EXTERN_FUNC(sub_826B28E0);   // the room net (R): this PC hosts the session
PPC_EXTERN_FUNC(sub_826C4938);   // the room data's match info (D +668)
PPC_EXTERN_FUNC(sub_826C4A38);   // the room data: send it (r4 1)
PPC_EXTERN_FUNC(sub_826C4A68);   // the room data's members present
PPC_EXTERN_FUNC(sub_826B2B00);   // the room net (R, members): the session's start (XSessionStart) - 0: refused
PPC_EXTERN_FUNC(sub_826B2B60);   // ... its answer: 0 going on, 1 started, 2 failed
PPC_EXTERN_FUNC(sub_82274750);   // the sync engine (NG)
PPC_EXTERN_FUNC(sub_826653B0);   // NG: a session runs (NG +40)
PPC_EXTERN_FUNC(sub_826655B8);   // NG: busy (NG +7200)
PPC_EXTERN_FUNC(sub_8266C5C8);   // NG: stop (r4 1, r5 0) - state_end's abort
PPC_EXTERN_FUNC(sub_82664F68);   // NG: after a stop (playresult_upload_wait's)
PPC_EXTERN_FUNC(sub_8266C5F0);   // NG: the match kind (2 a room's)
PPC_EXTERN_FUNC(sub_826677C0);
PPC_EXTERN_FUNC(sub_8266CD08);   // NG: a session from the room order (r4 session, r5 D, 2, 1) - 0: failed
PPC_EXTERN_FUNC(sub_82668F08);   // the battle records for the session (r3 1)
PPC_EXTERN_FUNC(sub_82356140);   // the match owner: a side entered here or not (r4 side, r5 1 / 0)
PPC_EXTERN_FUNC(sub_825B7370);   // the match settings S
PPC_EXTERN_FUNC(sub_825B6F60);   // S: a side's pair flag (r4 side, r5 flag)
PPC_EXTERN_FUNC(sub_8266C9D8);   // NG: this PC's pair flag of the last select (NG +8432)
PPC_EXTERN_FUNC(sub_826685A0);   // the pad slots 9 / 11 + side from the last select's controllers (r3 side)
PPC_EXTERN_FUNC(sub_82668F58);   // a fighter's battle setup sent (r3 side, r4 1) - 0: failed
PPC_EXTERN_FUNC(sub_826693A8);   // a spectator's (no setup of its own) - 0: failed
PPC_EXTERN_FUNC(sub_82654FD8);   // the own profile lent to a side (r3 PM = [[0x82E6C5A8]+256], r4 the side)
PPC_EXTERN_FUNC(sub_826689C8);   // the exchange: 1 going on, 0 done (or no session)
PPC_EXTERN_FUNC(sub_82667A70);   // NG: the session still runs
PPC_EXTERN_FUNC(sub_82667B00);   // the records' characters -> S's fighters and the model slots
PPC_EXTERN_FUNC(sub_825B9188);   // the error slots
PPC_EXTERN_FUNC(sub_826698B0);   // ... one set (r4 2: the network's)
PPC_EXTERN_FUNC(sub_8266A698);   // ... set one (r4 {code, 1})
PPC_EXTERN_FUNC(sub_8266A4B8);   // ... shown (r4 1)
PPC_EXTERN_FUNC(sub_825E2760);   // GameMode (impl): the restart - back to state_makuai_init
PPC_EXTERN_FUNC(sub_82649F28);   // the music: all of it stopped (the offline REMATCH's, MenuResultVS-01 at 0x825DBB4C)
PPC_EXTERN_FUNC(sub_825FE600);   // a state machine: the state's name (r3 impl, r4 the name)
PPC_EXTERN_FUNC(sub_826483C0);   // ... its function (r3 impl +8, r4 the function << 32, r5 0)
PPC_EXTERN_FUNC(sub_825D59E0);   // GameMode's "Please wait." (message 937; close_game_start's)
PPC_EXTERN_FUNC(sub_82281648);   // the network message window
PPC_EXTERN_FUNC(sub_8266E2D0);   // ... closed (r3 the window)
PPC_EXTERN_FUNC(sub_82281380);   // S: a fighter (r4 0..3: side, side + 2; r5 its model)
PPC_EXTERN_FUNC(sub_82276200);   // a character's model (r3 the character)
extern "C" PPC_FUNC(__imp__sub_8266C740);   // NG: this PC's side (the sync engine's own answer, not online_2v2_select.cpp's)
extern "C" PPC_FUNC(__imp__sub_8266C778);   // NG: this PC is a spectator node
extern "C" PPC_FUNC(__imp__sub_8266C608);   // NG: stop the session (r4 1 failed / 0)

namespace straight {
namespace {

using online2v2::DiagMs;

constexpr uint32_t kOnline = 0x82E6B558u;      // the online manager: +0 the room net R, +328 the room data D, +344 its control
constexpr uint32_t kGame = 0x82E6C5A8u;        // the game object: +240 the match owner
constexpr uint32_t kNG = 0x82E6C5B8u;          // the sync engine's manager: G = NG +7488 (+0 kind, +8 state, +96 the stage)
constexpr uint32_t kTermName = 0x820B9668u;    // "&GameMode::Impl::state_gamemode_player_match_term"
constexpr uint32_t kTermFn = 0x825D76B8u;

const char* const kPathName[4] = { "REMATCH", "STAGE SELECT", "CHARACTER SELECT", "CHANGE TEAMS" };

enum class Step { Idle, Begin, Members, Barrier, Starting, Engine, Exchange, Fail, Close, Done };

struct Run
{
    Step step = Step::Idle;
    int path = 0;
    unsigned long long began = 0, stepAt = 0;
    bool barrierOn = false, startAsked = false, engineStarted = false, spectator = false, stopAsked = false;
    int side = -1, stage = -1;
    int chars[2] = { -1, -1 };                 // this PC's last characters (NG +8444 / +8448)
    const char* why = "";
    online2v2::MatchClose close;
    bool pair = false;                         // a PAIR PLAY room: the last match's places start the next one
    online2v2::PairPlaces places;
    int mine = -1;                             // this PC's seat in them
    uint32_t pairFlags = 0;                    // StartPairMatch's character select flags (the team's side)
    int hostPos = 1;                           // the game host - the stage chooser: room order position (the decision's)
};
Run g_run;
// A PAIR PLAY partner's REMATCH: its own profile lent to its team's side (as its select path does), kept through the
// exchange's apply (online_2v2_select.cpp's sub_8266C740 hook answers its side there) - the partner's rewards are saved.
bool g_pairRematchApply = false;

// STAGE SELECT's character select: this PC's side keeps its characters (armed at the ask, used once by its select)
struct StagePreset
{
    bool armed = false, applied = false, pair = false;
    int side = -1;
    int chars[2] = { -1, -1 };
    uint32_t controller = 0;
    unsigned long long armedAt = 0;
    int held = 0, readyFor = 0;                // cursor steps held in state 0; steps since the select's grid was up
    uint32_t ui = 0, watchUntil = 0;           // the select's page (its diagnostics until the stage panel is live)
    uint32_t seen[4] = { ~0u, ~0u, ~0u, ~0u };
};
StagePreset g_stage;
// The select's page (sub_82643770, the face cards / face grid / timer / stage panel; +148 its controller), the last built
uint32_t g_ui = 0, g_uiController = 0;
bool g_skipStop = false;                       // CHARACTER / STAGE SELECT: GameMode's state_end leaves the new session alone
constexpr int kGridSettle = 6;                 // STAGE SELECT: cursor steps after the select's grid is up before the stage
constexpr int kHoldMax = 600;                  // ... and at most this many held (10 s): on regardless

unsigned long long Since(unsigned long long t) { const unsigned long long n = DiagMs(); return n > t ? n - t : 0; }

void Go(Step s) { g_run.step = s; g_run.stepAt = DiagMs(); }

uint32_t Call(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint64_t r3 = 0, uint64_t r4 = 0, uint64_t r5 = 0, uint64_t r6 = 0,
              uint64_t r7 = 0)
{
    PPCContext c = ctx;
    c.r3.u64 = r3;
    c.r4.u64 = r4;
    c.r5.u64 = r5;
    c.r6.u64 = r6;
    c.r7.u64 = r7;
    fn(c, base);
    return c.r3.u32;
}

uint32_t Room(uint8_t* base) { return GRead32(base, kOnline); }
uint32_t Lib(uint8_t* base) { const uint32_t r = Room(base); return r ? GRead32(base, r + 4) : 0; }
uint32_t Data(uint8_t* base) { return GRead32(base, kOnline + 344) ? GRead32(base, kOnline + 328) : 0; }
uint32_t Owner(uint8_t* base) { const uint32_t g = GRead32(base, kGame); return g ? GRead32(base, g + 240) : 0; }

// [impl +124]->vtable[1](manager, mode, flags, 0): what GameMode's own states do to change mode.
void AskMode(PPCContext& ctx, uint8_t* base, uint32_t impl, uint32_t mode, uint32_t flags)
{
    const uint32_t manager = GRead32(base, impl + 124);
    const uint32_t fn = manager ? GRead32(base, GRead32(base, manager) + 4) : 0;
    if (!fn) return;
    PPCContext c = ctx;
    c.r3.u64 = manager;
    c.r4.u64 = mode;
    c.r5.u64 = flags;
    c.r6.u64 = 0;
    PPC_LOOKUP_FUNC(base, fn)(c, base);
}

void CloseWait(PPCContext& ctx, uint8_t* base) { Call(ctx, base, sub_8266E2D0, Call(ctx, base, sub_82281648)); }

void Snapshot(uint8_t* base, uint32_t impl, const char* when)
{
    const uint32_t owner = Owner(base);
    const uint32_t S = owner ? GRead32(base, owner + 240) : 0;
    const uint32_t data = Data(base), control = GRead32(base, kOnline + 344), room = Room(base);
    std::fprintf(stderr, "[straight] %s: S %08X (+4 %u +472 %u +772 %d +776 %u %u) G kind %d state %d stage %d | engine %08X | owner +80 %u "
                 "+84 %u | impl +143 %u +156 %u +157 %u +216 %08X +356 %08X +364 %08X +372 %08X | order %d %d [%d %d %d %d] phase %u | "
                 "control %08X mode %u busy %u | R +48 %u +52 %u | NG +8432 %u characters %d %d\n", when, S, S ? GRead32(base, S + 4) : 0u,
                 S ? GRead32(base, S + 472) : 0u, S ? int32_t(GRead32(base, S + 772)) : -1, S ? GRead8(base, S + 776) : 0u,
                 S ? GRead8(base, S + 777) : 0u, int32_t(GRead32(base, kNG + 7488)), int32_t(GRead32(base, kNG + 7496)),
                 int16_t(GRead16(base, kNG + 7488 + 96)), GRead32(base, 0x82E6C5C8u + 24), owner ? GRead32(base, owner + 80) : 0u,
                 owner ? GRead32(base, owner + 84) : 0u, GRead8(base, impl + 143), GRead8(base, impl + 156), GRead8(base, impl + 157),
                 GRead32(base, impl + 216), GRead32(base, impl + 356), GRead32(base, impl + 364), GRead32(base, impl + 372),
                 data ? int32_t(GRead32(base, data + 668)) : -9, data ? int32_t(GRead32(base, data + 672)) : -9,
                 data ? int16_t(GRead16(base, data + 676)) : -9, data ? int16_t(GRead16(base, data + 678)) : -9,
                 data ? int16_t(GRead16(base, data + 680)) : -9, data ? int16_t(GRead16(base, data + 682)) : -9,
                 data ? GRead32(base, data + 700) : 0u, control ? GRead32(base, control + 40) : 0u, control ? GRead32(base, control + 92) : 0u,
                 control ? GRead8(base, control + 68) : 0u, room ? GRead32(base, room + 48) : 0u, room ? GRead32(base, room + 52) : 0u,
                 GRead8(base, kNG + 8432), int32_t(GRead32(base, kNG + 8444)), int32_t(GRead32(base, kNG + 8448)));
}

// A step failed: back into the room the game's way (Step::Fail, then Step::Close).
void Fail(const char* why)
{
    g_run.why = why;
    std::fprintf(stderr, "[straight] %s: %s - back to the room (after %llu ms)\n", kPathName[g_run.path], why, Since(g_run.began));
    Go(Step::Fail);
}

// The battle start's sync-engine start, from this PC's room order (OnlineInRoom::state_battle_start's calls).
bool StartEngine(PPCContext& ctx, uint8_t* base)
{
    const uint32_t lib = Lib(base);
    const uint32_t session = lib ? Call(ctx, base, sub_82B35088, lib) : 0;
    const uint32_t ng = Call(ctx, base, sub_82274750);
    Call(ctx, base, sub_8266C5F0, ng, 2);
    Call(ctx, base, sub_826677C0);
    // the game host (the stage chooser) at room order position hostPos; the sides stay the last match's: the side swap
    // (G+13) comes from the host's side preference (sub_8266C530 with r7's position: 0 swaps unless the position is 0, 1
    // swaps only there) - order[0]'s is held at the last swap for the start (order[1]'s, the game's own start, keeps it)
    const int hostPos = g_run.hostPos == 0 ? 0 : 1;
    const uint32_t data = GRead32(base, kOnline + 328);
    uint32_t prefAt = 0, prefWas = 0;
    if (hostPos == 0 && data)
    {
        const int m = int32_t(GRead32(base, data + 668));
        if (m >= 0 && m < 6)
        {
            prefAt = data + 20 + 108 * uint32_t(m) + 16;
            prefWas = GRead32(base, prefAt);
            GWrite32(base, prefAt, uint32_t(GRead8(base, kNG + 7488 + 13) & 1));
        }
    }
    const bool started = (Call(ctx, base, sub_8266CD08, ng, session, data, 2, uint64_t(hostPos)) & 0xFF) != 0;
    if (prefAt) GWrite32(base, prefAt, prefWas);
    if (!started)
    {
        Call(ctx, base, __imp__sub_8266C608, ng, 1);
        return false;
    }
    stagechooser::Started(hostPos, "a straight start");
    g_run.engineStarted = true;
    Call(ctx, base, sub_82668F08, 1);
    g_run.spectator = (Call(ctx, base, __imp__sub_8266C778, ng) & 0xFF) != 0;
    g_run.side = int32_t(Call(ctx, base, __imp__sub_8266C740, ng));
    return true;
}

// The battle start's side marks: this side entered here, the other not.
void MarkSides(PPCContext& ctx, uint8_t* base, int side)
{
    const uint32_t owner = Owner(base);
    for (uint32_t s = 0; s < 2 && owner; ++s) Call(ctx, base, sub_82356140, owner, s, int(s) == side ? 1 : 0);
}

// The room could not be entered again: what GameMode's room entry does then (sub_825D9E08 at 0x825D9EE4) - the sync
// engine stopped, the network error shown, state_gamemode_player_match_term (the way out of Player Match).
void PlayerMatchTerm(PPCContext& ctx, uint8_t* base, uint32_t impl)
{
    const uint32_t ng = Call(ctx, base, sub_82274750);
    Call(ctx, base, sub_8266C5C8, ng, 1, 0);
    const bool host = Room(base) && (Call(ctx, base, sub_826B28E0, Room(base)) & 0xFF);
    PPCContext c = ctx;
    const uint32_t saved = c.r1.u32;
    const uint32_t sp = (saved - 0x100) & ~0xFu;
    GWrite32(base, sp, saved);
    c.r1.u32 = sp;
    GWrite32(base, sp + 80, host ? 0x10000036u : 0x10000005u);
    GWrite32(base, sp + 84, 1);
    Call(c, base, sub_8266A698, Call(c, base, sub_825B9188), sp + 80);
    Call(c, base, sub_8266A4B8, Call(c, base, sub_825B9188), 1);
    Call(c, base, sub_825FE600, impl, kTermName);
    Call(c, base, sub_826483C0, impl + 8, uint64_t(kTermFn) << 32, 0);
}

} // namespace

bool Running() { return g_run.step != Step::Idle && g_run.step != Step::Done; }

void Begin(int path, uint32_t impl, int chooser)
{
    (void)impl;
    g_run = Run{};
    g_run.path = path >= 0 && path <= 3 ? path : 0;
    g_run.hostPos = chooser == 0 || chooser == 1 ? chooser : stagechooser::LastHostPos();
    g_run.began = DiagMs();
    g_pairRematchApply = false;
    Go(Step::Begin);
}

bool PairRematchApply() { return g_pairRematchApply; }

// Every frame of GameMode's online result once a straight path was decided (online_rematch.cpp MenuFrame; r3 the impl).
// False once GameMode has been handed on (the restart, a mode asked): the end menu's state no longer runs.
bool Frame(PPCContext& ctx, uint8_t* base, uint32_t impl)
{
    Run& r = g_run;
    const uint32_t room = Room(base), lib = Lib(base), data = Data(base);
    switch (r.step)
    {
    case Step::Idle:
    case Step::Done:
        return false;
    case Step::Begin:
    {
        Snapshot(base, impl, "the end menu");
        const int16_t fought = int16_t(GRead16(base, kNG + 7488 + 96));
        const uint32_t S = Call(ctx, base, sub_825B7370);
        r.stage = fought >= 0 && fought < 100 ? fought : S ? int32_t(GRead32(base, S + 772)) : -1;
        r.chars[0] = int32_t(GRead32(base, kNG + 8444));
        r.chars[1] = int32_t(GRead32(base, kNG + 8448));
        Call(ctx, base, sub_825D59E0, impl);
        if (!room || !lib || !data) { Fail("no room"); break; }
        if (online2v2::PairMatchRoom(base))
        {
            if (!online2v2::PlacesLast(r.places, r.mine) || r.mine < 0) { Fail("no places of the last match here"); break; }
            r.pair = true;
        }
        if (r.path == kTeams && !r.pair) { Fail("CHANGE TEAMS outside a PAIR PLAY room"); break; }
        if (!rematch::NobodyWaits(base)) { Fail("the room's members changed"); break; }
        std::fprintf(stderr, "[straight] %s: the session starts again (stage %d, this PC's characters %d / %d)\n", kPathName[r.path],
                     r.stage, r.chars[0], r.chars[1]);
        // the room host: the last match counted (the win streaks) and every member's record sent (D+913 - the order block
        // D+912 carries none) before anyone's battle setup is exchanged - every PC's VS screen and HUD show the streaks
        // from its room data (online_pair_room.cpp: the label's side, the host's count kept) (a start that comes to nothing does not count
        // it again: the count stands until the next match's fight begins - rematch::NoteState). A room host that partnered
        // in a PAIR PLAY match has no result of its own (the engine's spectator; the room would take the one its members
        // report when they come back, which they never do here): the fighters' votes carry theirs.
        if (Call(ctx, base, sub_826B28E0, room) & 0xFF)
        {
            const int32_t own = int32_t(GRead32(base, kNG + 7488 + 4));
            if (own < -1 || own > 1)
            {
                const int fought = rematch::FightersResult();
                if (fought >= -1 && fought <= 1)
                {
                    GWrite32(base, kNG + 7488 + 4, uint32_t(fought));
                    std::fprintf(stderr, "[straight] the room host had no result of its own: the fighters' (%d)\n", fought);
                }
            }
            online2v2::AfterMatchKeepOrder(ctx, base);
        }
        Go(Step::Members);
        break;
    }
    case Step::Members:
        // as the room's start does (sub_826BEF60): every connected session member in the room, or 10 s
        if (!(Call(ctx, base, sub_826B8640, kOnline) & 0xFF) && Since(r.stepAt) < 10000) break;
        if (!(Call(ctx, base, sub_82B352B8, lib, 1, 0) & 0xFF)) { Fail("the session's barrier refused"); break; }
        r.barrierOn = true;
        Go(Step::Barrier);
        break;
    case Step::Barrier:
    {
        Call(ctx, base, sub_82B352D0, lib);
        const uint32_t a = Call(ctx, base, sub_82B352E8, lib);
        if (a == 0 && Since(r.stepAt) < 20000) break;
        Call(ctx, base, sub_82B35300, lib);
        r.barrierOn = false;
        if (a != 1) { Fail(a == 2 ? "the session's barrier failed" : "the session's barrier: no answer in 20 s"); break; }
        // as sub_826BC838: the room host's phase 2 (a match), then every PC's start request
        if (Call(ctx, base, sub_826B28E0, room) & 0xFF)
        {
            GWrite32(base, Call(ctx, base, sub_826C4938, data) + 32, 2);
            Call(ctx, base, sub_826C4A38, data, 1);
        }
        r.startAsked = true;
        if (!(Call(ctx, base, sub_826B2B00, room, Call(ctx, base, sub_826C4A68, data)) & 0xFF)) { Fail("the session's start refused"); break; }
        std::fprintf(stderr, "[straight] the barrier passed after %llu ms - the session's start asked\n", Since(r.stepAt));
        Go(Step::Starting);
        break;
    }
    case Step::Starting:
    {
        const uint32_t session = lib ? Call(ctx, base, sub_82B35088, lib) : 0;
        const uint32_t a = session ? Call(ctx, base, sub_826B2B60, room) : 0;
        if (a == 0 && Since(r.stepAt) < 20000) break;
        if (a != 1) { Fail(a == 2 ? "the session's start failed" : "the session's start: no answer in 20 s"); break; }
        std::fprintf(stderr, "[straight] the session started after %llu ms\n", Since(r.stepAt));
        Go(Step::Engine);
        break;
    }
    case Step::Engine:
    {
        // the last session's end: a spectator node skips playresult_upload_wait - its engine may still be busy
        const uint32_t ng = Call(ctx, base, sub_82274750);
        const bool busy = (Call(ctx, base, sub_826653B0, ng) & 0xFF) || (Call(ctx, base, sub_826655B8, ng) & 0xFF);
        if (busy)
        {
            if (Since(r.stepAt) > 5000 && !r.stopAsked)
            {
                r.stopAsked = true;
                Call(ctx, base, sub_8266C5C8, ng, 1, 0);
                std::fprintf(stderr, "[straight] the last session still ran after 5 s - stopped\n");
            }
            if (Since(r.stepAt) > 10000) Fail("the last session did not end");
            break;
        }
        if (r.stopAsked) { Call(ctx, base, sub_82664F68, ng); r.stopAsked = false; }
        if (r.path == kTeams)
        {
            // CHANGE TEAMS: the PAIR PLAY screen again for the same players, as the room's battle start opens it
            // (online_pair_room.cpp HoldBattleStart: PlacesBegin, PADASSIGN - mode 22 - with its PAIR sequence 0x80). The
            // places it decides start the match the room's way (online_2v2_places.cpp PlacesModeAsked -> StartPairMatch:
            // the sync engine, then character select); the menu frame closed first, as the room's start leaves none
            online2v2::PairSeat seats[4];
            const int count = r.places.count < 4 ? r.places.count : 4;
            for (int s = 0; s < count; ++s) seats[s] = r.places.seat[s];
            if (!online2v2::PlacesBegin(seats, count, r.mine)) { Fail("the PAIR PLAY screen could not open"); break; }
            CloseWait(ctx, base);
            menu_host::ShowFrame(ctx, base, "");
            g_skipStop = true;
            GWrite8(base, impl + 156, 1);
            AskMode(ctx, base, impl, 22, 0x80);
            std::fprintf(stderr, "[straight] CHANGE TEAMS: the PAIR PLAY screen (%d players, this PC seat %d) after %llu ms\n", count,
                         r.mine + 1, Since(r.began));
            Go(Step::Done);
            return false;
        }
        if (r.pair)
        {
            // PAIR PLAY: the order the last places gave, the sync engine started from it as the places' start does
            // (StartPairMatch: the side preference held, the team's side marked - a partner's too)
            online2v2::WriteMatchOrder(ctx, base, r.places);
            int side = -1;
            r.pairFlags = online2v2::StartPairMatch(ctx, base, r.places, r.mine, r.hostPos, &side);
            if (side < 0) { Fail("the sync engine did not start"); break; }
            r.engineStarted = true;
            r.spectator = (Call(ctx, base, __imp__sub_8266C778, ng) & 0xFF) != 0;
            r.side = side;
        }
        else if (!StartEngine(ctx, base)) { Fail("the sync engine did not start"); break; }
        std::fprintf(stderr, "[straight] the sync engine started: this PC %s, side %d\n",
                     r.spectator ? (r.pair ? "a partner (a spectator node)" : "a spectator node") : "a fighter", r.side);
        if (r.path == kRematch)
        {
            const uint32_t S = Call(ctx, base, sub_825B7370);
            if (!S) { Fail("no match settings"); break; }
            GWrite32(base, S + 472, 0);
            bool sent;
            if (!r.spectator && (r.side == 0 || r.side == 1))
            {
                MarkSides(ctx, base, r.side);
                Call(ctx, base, sub_825B6F60, S, r.side, Call(ctx, base, sub_8266C9D8, ng) & 0xFF);
                if (r.stage >= 0) GWrite32(base, S + 772, uint32_t(r.stage));
                Call(ctx, base, sub_826685A0, r.side);
                sent = (Call(ctx, base, sub_82668F58, r.side, 1) & 0xFF) != 0;
            }
            else
            {
                sent = (Call(ctx, base, sub_826693A8) & 0xFF) != 0;
                // a PAIR PLAY partner (no setup of its own: the engine's spectator) - its own profile on its team's side, as
                // its select path lends it (sub_82668F58 -> sub_82668D40's sub_82654FD8(PM, side) at 0x82668D94): the match's
                // rewards are its own then, saved at the match's end (a received profile on both sides was never saved -
                // MEASURED 2026-11-08: partners missed every REMATCH's win / loss). Only when the select hook answers this
                // side during the apply (PartnerSide): else the side would keep a default record.
                if (sent && r.pair && r.spectator && (r.side == 0 || r.side == 1) && online2v2::PartnerSide() == r.side)
                {
                    const uint32_t holder = 0x82E70F04u;
                    const uint32_t pm = GRead32(base, GRead32(base, kGame) + 256);
                    const uint32_t before8 = GRead8(base, holder + 8);
                    if (pm) Call(ctx, base, sub_82654FD8, pm, uint32_t(r.side));
                    g_pairRematchApply = pm != 0;
                    std::fprintf(stderr, "[rewards] partner REMATCH: own profile lent to side %d (PM %08X, holder +8 %u -> %u, +12 %u)\n", r.side, pm,
                                 before8, GRead8(base, holder + 8), GRead32(base, holder + 12));
                }
            }
            if (!sent) { Fail("the battle setup was not sent"); break; }
            Go(Step::Exchange);
            break;
        }
        // CHARACTER SELECT / STAGE SELECT: the battle start's marks and character select (a PAIR PLAY partner: its
        // team's side, its select mirrored as at the room's start)
        const int side = r.side;
        if ((r.spectator && !r.pair) || (side != 0 && side != 1)) { Fail("no side for character select"); break; }
        MarkSides(ctx, base, side);
        const uint32_t session = Call(ctx, base, sub_82B35088, lib);
        const uint32_t settings = session ? GRead32(base, session + 80) : 0;
        const uint32_t flags = r.pair ? r.pairFlags
                                      : (side ? 2u : 1u) | 0x1000u | (settings && GRead8(base, settings + 161) == 1 ? 0x20u : 0u);
        // the end menu's frame closed: character select runs with none, as after the room's start (2026-11-07 rm12a: left
        // up, its rows stayed over the select and the stage chooser's stage panel never showed)
        CloseWait(ctx, base);
        menu_host::ShowFrame(ctx, base, "");
        g_skipStop = true;
        if (r.path == kStage)
        {
            g_stage = StagePreset{};
            g_stage.armed = true;
            g_stage.armedAt = DiagMs();
            // a PAIR PLAY side of two keeps its partner's join; a 2 v 1 room's solo side is a TAG / SOLO side (as its
            // select's own PAIR preset leaves it: online_2v2_pick.cpp PairPreset)
            g_stage.pair = r.pair && r.places.place[side] >= 0 && r.places.place[side + 2] >= 0;
            g_stage.side = side;
            g_stage.chars[0] = r.chars[0];
            g_stage.chars[1] = r.chars[1];
        }
        GWrite8(base, impl + 156, 1);
        AskMode(ctx, base, impl, 8, flags);
        std::fprintf(stderr, "[straight] %s: character select (flags %X) after %llu ms\n", kPathName[r.path], flags, Since(r.began));
        Go(Step::Done);
        return false;
    }
    case Step::Exchange:
    {
        if (Call(ctx, base, sub_826698B0, Call(ctx, base, sub_825B9188), 2) & 0xFF) { g_pairRematchApply = false; Fail("a network error"); break; }
        if (Call(ctx, base, sub_826689C8) & 0xFF)
        {
            if (Since(r.stepAt) > 90000) { g_pairRematchApply = false; Fail("the battle setup exchange took over 90 s"); }
            break;
        }
        if (std::exchange(g_pairRematchApply, false))
        {
            const uint32_t pm = GRead32(base, GRead32(base, kGame) + 256);
            const uint32_t rec = pm ? GRead32(base, pm + 60 + 4 * uint32_t(r.side & 1)) : 0;
            std::fprintf(stderr, "[rewards] partner REMATCH: the exchange applied - side %d's record %08X type %u\n", r.side, rec, rec ? GRead8(base, rec) : 0u);
        }
        if (!(Call(ctx, base, sub_82667A70) & 0xFF)) { Fail("the session ended during the exchange"); break; }
        if (!r.spectator) Call(ctx, base, sub_826685A0, r.side);
        Call(ctx, base, sub_82667B00);
        Snapshot(base, impl, "the exchange done");
        std::fprintf(stderr, "[straight] REMATCH: the battle setup exchanged after %llu ms - the fight again (%llu ms from the decision)\n",
                     Since(r.stepAt), Since(r.began));
        // the game's restart (offline REMATCH's): the end menu closed (no frame: as during a fight), the battle action
        // GameMode's teardown left (4: leave) cleared as its dispatcher does, the per-match flags as a new GameMode has them
        CloseWait(ctx, base);
        menu_host::ShowFrame(ctx, base, "");
        if (const uint32_t S = Call(ctx, base, sub_825B7370)) GWrite32(base, S + 4, 0);
        GWrite8(base, impl + 143, 0);
        GWrite8(base, impl + 156, 0);
        GWrite8(base, impl + 157, 0);
        rematch::Replaying();
        // the last match's stage music stopped as the offline REMATCH does in the same frame it restarts (0x825DBB4C, then
        // the restart): the online teardown left it playing (event 23, 0x825DAEB4) and the stage scene starts its track
        // only when it is not playing (0x8250CF58) - the fight's start plays it from the top again
        Call(ctx, base, sub_82649F28);
        std::fprintf(stderr, "[straight] REMATCH: the stage music stopped (the offline REMATCH's 0x825DBB4C)\n");
        Call(ctx, base, sub_825E2760, impl);
        Go(Step::Done);
        return false;
    }
    case Step::Fail:
    {
        g_pairRematchApply = false;
        if (r.barrierOn) { Call(ctx, base, sub_82B35300, lib); r.barrierOn = false; }
        const uint32_t ng = Call(ctx, base, sub_82274750);
        if (r.engineStarted && (Call(ctx, base, sub_826653B0, ng) & 0xFF))
        {
            if (!r.stopAsked) { r.stopAsked = true; Call(ctx, base, __imp__sub_8266C608, ng, 1); }
            if (Since(r.stepAt) < 5000) break;
        }
        r.close = online2v2::MatchClose{};
        Go(Step::Close);
        [[fallthrough]];
    }
    case Step::Close:
    {
        if (r.startAsked && !online2v2::MatchCloseStep(ctx, base, r.close, "straight start called off")) break;
        CloseWait(ctx, base);
        if (r.close.entry == 2)
        {
            std::fprintf(stderr, "[straight] %s called off (%s) and the room could not be entered again - GameMode's own way out\n",
                         kPathName[r.path], r.why);
            PlayerMatchTerm(ctx, base, impl);
            Go(Step::Done);
            return false;
        }
        Call(ctx, base, sub_8266C5F0, Call(ctx, base, sub_82274750), 2);
        GWrite8(base, impl + 156, 1);
        AskMode(ctx, base, impl, 35, 1);
        std::fprintf(stderr, "[straight] %s called off (%s) - into the room again\n", kPathName[r.path], r.why);
        Go(Step::Done);
        return false;
    }
    }
    return true;
}

// GameMode's state_end stops the sync engine on the way out (sub_8266C608(NG, 0) at 0x825DB884): not the session the
// straight CHARACTER / STAGE SELECT just started (once).
bool SkipEngineStop(uint32_t lr)
{
    if (lr != 0x825DB88Cu || !g_skipStop) return false;
    g_skipStop = false;
    std::fprintf(stderr, "[straight] GameMode's way out left the new session alone\n");
    return true;
}

// The select controller's update (r3 the controller), before the original: STAGE SELECT's preset before its first state 0
// (the cursors are built there and read S): this PC's side keeps its last characters (S's fighters, S+776+side 0 - the
// keep route) and its battle records are lent again.
void SelectUpdate(PPCContext& ctx, uint8_t* base, uint32_t controller)
{
    if (!g_stage.armed || g_stage.applied || !controller) return;
    if (Since(g_stage.armedAt) > 30000)
    {
        g_stage.armed = false;
        std::fprintf(stderr, "[straight] STAGE SELECT: no character select in 30 s - the kept characters dropped\n");
        return;
    }
    if (GRead32(base, controller + 20) != 0) return;
    g_stage.applied = true;
    g_stage.controller = controller;
    const uint32_t S = Call(ctx, base, sub_825B7370);
    const int s = g_stage.side, c1 = g_stage.chars[0], c2 = g_stage.chars[1];
    if (!S || s < 0 || s > 1 || c1 < 0 || c1 > 62 || c2 < 0 || c2 > 62)
    {
        g_stage.armed = false;
        std::fprintf(stderr, "[straight] STAGE SELECT: nothing to keep (S %08X, side %d, characters %d / %d) - a full character select\n", S,
                     s, c1, c2);
        return;
    }
    Call(ctx, base, sub_82281380, S, s, Call(ctx, base, sub_82276200, c1));
    Call(ctx, base, sub_82281380, S, s + 2, Call(ctx, base, sub_82276200, c2));
    GWrite8(base, S + 776 + uint32_t(s), 0);
    Call(ctx, base, sub_82668F08, 1);
    if (g_stage.pair)
    {
        Call(ctx, base, sub_826685A0, s);      // the pad slots 9 / 11 + side the last select's controllers held
        online2v2::PairJoinKept(ctx, base, s);
    }
    std::fprintf(stderr, "[straight] STAGE SELECT: side %d keeps characters %d / %d (models %d / %d)\n", s, c1, c2,
                 int32_t(GRead32(base, S + 432 + 4 * uint32_t(s))), int32_t(GRead32(base, S + 440 + 4 * uint32_t(s))));
}

// A side's cursor's step (the controller, the cursor; K+312 its state, K+316 the next), after the original: the kept
// side's state 0 goes on to state 20 (the picks finished - their characters written to the battle record - then the stage
// chooser's test, 22) instead of 39 (done).
void CursorStepped(uint8_t* base, uint32_t controller, uint32_t cursor, uint32_t stateBefore)
{
    if (!g_stage.armed || !g_stage.applied || controller != g_stage.controller || !cursor) return;
    if (int32_t(GRead32(base, cursor + 320)) != g_stage.side || stateBefore != 0) return;
    const uint32_t next = GRead32(base, cursor + 316);
    if (next == 0) return;
    if (next != 39)
    {
        g_stage.armed = false;
        std::fprintf(stderr, "[straight] STAGE SELECT: side %d's cursor goes on to state %u (not the kept route's 39) - left alone\n",
                     g_stage.side, next);
        return;
    }
    // the stage phase only once the select's page has its face grid up (page +240: the grid's intro past frame 60 -
    // sub_826417A0, 0x826421B0) and a few frames more: the page's stage panel (sub_82642AE8, its +252) takes the stage
    // phase's start (C+312) as its cue to send the faces out (face_all_out) and bring the stages in - a cue during the
    // intro is lost, the panel never opens and never lets the stage step run (C+320). A normal select's picks take that
    // long anyway; the cursor waits in state 0 (its kept route again: the picks as they are, the timer from the top).
    const uint32_t ui = g_uiController == controller ? g_ui : 0;
    // ... and its stage panel idle in its state 1 (waiting for C+312 - 0x82642B7C)
    const bool up = ui && GRead8(base, ui + 240) != 0 && GRead32(base, ui + 252) == 1;
    if (up) ++g_stage.readyFor;
    if ((!up || g_stage.readyFor < kGridSettle) && g_stage.held < kHoldMax)
    {
        if (g_stage.held++ == 0)
            std::fprintf(stderr, "[straight] STAGE SELECT: side %d's characters kept - the stage once the select's grid is up (page %08X)\n",
                         g_stage.side, ui);
        GWrite32(base, cursor + 316, 0);
        return;
    }
    g_stage.armed = false;
    g_stage.ui = ui;
    g_stage.watchUntil = menu_host::FrameCount() + 900;
    GWrite32(base, cursor + 316, 20);
    std::fprintf(stderr, "[straight] STAGE SELECT: side %d's characters kept - on to the stage after %d steps%s\n", g_stage.side,
                 g_stage.held, up ? "" : " (WARNING: the select's grid never came up)");
}

// The select's page is built (sub_82643770: r3 the page, r5 the controller).
void SelectPageBuilt(uint32_t page, uint32_t controller)
{
    g_ui = page;
    g_uiController = controller;
}

// The select controller's update, after the original: STAGE SELECT's stage panel on its way (diagnostics - the page's
// stage panel state +252, the stage phase C+312, its side C+316, the stage step's gate C+320) until it is live.
void SelectWatch(uint8_t* base, uint32_t controller)
{
    if (!g_stage.ui || controller != g_stage.controller) return;
    if (int32_t(menu_host::FrameCount() - g_stage.watchUntil) > 0)
    {
        std::fprintf(stderr, "[straight] STAGE SELECT: WARNING - the stage panel was not live in 15 s\n");
        g_stage.ui = 0;
        return;
    }
    const uint32_t now[4] = { GRead32(base, g_stage.ui + 252), GRead32(base, controller + 312), GRead32(base, controller + 316),
                              GRead32(base, controller + 320) };
    // the other PC chooses the stage: this side's cursor done (state 39, state 22's other branch) without a stage phase here
    const uint32_t cursor = GRead32(base, controller + (g_stage.side ? 56 : 48));
    if (!now[1] && cursor && GRead32(base, cursor + 312) == 39)
    {
        std::fprintf(stderr, "[straight] STAGE SELECT: the other PC chooses the stage - this side's picks are done\n");
        g_stage.ui = 0;
        return;
    }
    if (std::equal(now, now + 4, g_stage.seen)) return;
    std::copy(now, now + 4, g_stage.seen);
    std::fprintf(stderr, "[straight] STAGE SELECT: stage panel %u, stage phase %u (side %u), stage step %s\n", now[0], now[1], now[2],
                 now[3] ? "open" : "shut");
    if (now[0] == 4 && now[3]) g_stage.ui = 0;  // live: the panel's own from here
}

} // namespace straight

// Character select's page (r3 the page, r4 the page holder, r5 the select controller): built - straight::SelectPageBuilt.
extern "C" PPC_FUNC(__imp__sub_82643770);
PPC_FUNC(sub_82643770)
{
    const uint32_t page = ctx.r3.u32, controller = ctx.r5.u32;
    __imp__sub_82643770(ctx, base);
    straight::SelectPageBuilt(page, controller);
    stagemirror::PageBuilt(page, controller);
}

// NG: stop the session (r3 NG, r4 1 failed / 0) - straight::SkipEngineStop.
PPC_FUNC(sub_8266C608)
{
    if (straight::SkipEngineStop(uint32_t(ctx.lr))) return;
    __imp__sub_8266C608(ctx, base);
}

// ---- the music's starts and stops (diagnostics, rare calls): REMATCH's stage music stops, then the fight's start plays
// its track from the top (the stage scene's entry, sub_8250CCE8 -> sub_8264B048 at 0x8250D13C / sub_8264B518).
extern "C" PPC_FUNC(__imp__sub_82649F28);
PPC_FUNC(sub_82649F28)
{
    std::fprintf(stderr, "[bgm] stop all music (lr %08X)\n", uint32_t(ctx.lr));
    __imp__sub_82649F28(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_8264B048);
PPC_FUNC(sub_8264B048)
{
    std::fprintf(stderr, "[bgm] play track %d (r5 %d r6 %d, lr %08X)\n", int32_t(ctx.r4.u32), int32_t(ctx.r5.u32), int32_t(ctx.r6.u32),
                 uint32_t(ctx.lr));
    __imp__sub_8264B048(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_8264B518);
PPC_FUNC(sub_8264B518)
{
    std::fprintf(stderr, "[bgm] play track (Music Edit entry) r4 %d r5 %d (lr %08X)\n", int32_t(ctx.r4.u32), int32_t(ctx.r5.u32),
                 uint32_t(ctx.lr));
    __imp__sub_8264B518(ctx, base);
}
// the stage scene (r3) entered or left (r4 byte: nonzero leaving; r5 the other scene) - registers only (its leave branch
// can clear the music handle: nothing read through it here)
extern "C" PPC_FUNC(__imp__sub_8250CCE8);
PPC_FUNC(sub_8250CCE8)
{
    std::fprintf(stderr, "[bgm] stage scene %s (scene %08X, other %08X, lr %08X)\n", (ctx.r4.u32 & 0xFF) ? "leaving" : "entering",
                 ctx.r3.u32, ctx.r5.u32, uint32_t(ctx.lr));
    __imp__sub_8250CCE8(ctx, base);
}
