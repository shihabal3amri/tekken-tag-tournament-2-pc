// Online PAIR PLAY rooms (online_2v2.h, port/ONLINE_2V2_PLAN.md "Native flow"): the room works as a six-player Player
// Match room does, for four players.
//
// The room's list shows its order (row k = the order's k-th member, CRoomControl's draw sub_826BDCE0) and marks the next
// match's two fighters; here its first four members are marked (their plates, sub_8256D5C8 below), all four press the
// game's own READY (sub_826BCD98 / sub_826BCCC8 / sub_826BBDC0 below: who may, how many are, when the match starts),
// and the room's 16-second rule swaps the ones who did not for waiting members (sub_826BD2D0). After a match the winning
// team stays - ready, as the game's champion does - and the losers go to the back (sub_826BCE58 from sub_826BD180).
// The match's start (OnlineInRoom::state_battle_start, sub_825FB650) opens the game's PAIR PLAY screen for the four
// players (online_2v2_places.cpp); its places make the teams and the sides, and the match goes on from it into
// character select (StartPairMatch: the sync engine started with the order the places give, as the battle start does).
// The room's watchers wait at their battle start for the places, then go on as the game's own spectators. A room of
// two members plays the game's own 1-on-1.
//
// The room data D = [online +328]: members at +20 + 108 * i (+4 used, +8 in a match, +12 win streak, +16 the preferred
// side, +20 READY, +30 name), the match info I = D + 668 (+0 / +4 the fighters, +8..18 the order, +20..30 its copy the
// rebuild starts from, +32 the phase: 0 after a match, 1 READY, 2 a match). The room host's copy is sent to the others
// (+912 the info block, +913 the member entries). Research: port/ONLINE_2V2_PLAN.md, C:/ttt2wt/re/re_r2.md.
#include "online_2v2.h"
#undef PlaySound                               // windows.h (mmsystem)
#include "profiles.h"
#include "xlive.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

bool XSessionLocalHosts();                     // xsession.cpp: this game hosts the room
uint64_t XSessionCurrentId();                  // xsession.cpp: the room session (0: not in a room)

PPC_EXTERN_FUNC(sub_826C4868);   // the room data's member entry of this PC (r3 D; 0: none)
PPC_EXTERN_FUNC(sub_82B35088);   // lib: a session handle's session object (+80 the room settings)
PPC_EXTERN_FUNC(sub_82274750);   // the sync engine (NG)
PPC_EXTERN_FUNC(sub_8266C5F0);   // NG: the match kind (2 a room's)
PPC_EXTERN_FUNC(sub_826677C0);
PPC_EXTERN_FUNC(sub_8266CD08);   // NG: start - a node per member of the room order (r4 session, r5 D, 2, 1); 0: failed
PPC_EXTERN_FUNC(sub_8266C608);   // NG: stop (1: failed)
PPC_EXTERN_FUNC(sub_82668F08);
PPC_EXTERN_FUNC(sub_8266C740);   // NG: this PC's side
PPC_EXTERN_FUNC(sub_82356140);   // the match owner ([[0x82E6C5A8] +240]): a side entered here or not (r4 side, r5 1/0)
PPC_EXTERN_FUNC(sub_8266C600);   // NG: the match result (r4; -2 none / taken)
PPC_EXTERN_FUNC(sub_82281648);   // the network message window ("Please wait." - state_room_main's for a watcher)
PPC_EXTERN_FUNC(sub_8266E2D0);   // ... closed (r3 the window; as state_battle_env_synchronize_start closes it)
PPC_EXTERN_FUNC(sub_826BBD08);   // the room control's win streaks after a match (r4: 0 / 1 a fighter's, 2 none)
PPC_EXTERN_FUNC(sub_826BCE58);   // the room control's order after a match (r4: 0 / 1 / 3 / -1 none)
PPC_EXTERN_FUNC(sub_826B81F8);   // the room data (r3 the room manager)
PPC_EXTERN_FUNC(sub_82265948);   // the room manager
PPC_EXTERN_FUNC(sub_826C4A38);   // the room data: send it (r4 1)
PPC_EXTERN_FUNC(sub_826C4A50);   // ... every member's record with it (r4 1; D+913)
PPC_EXTERN_FUNC(sub_825B7370);   // the match settings S (+754 the WIN label's side, +756 its count)
PPC_EXTERN_FUNC(sub_826B8B28);   // the online manager is busy (r3 the manager: its room control mid-change, control +68)
PPC_EXTERN_FUNC(sub_826B2BF0);   // the room net (r3 R = [online +0]): the session's close, from its start (R +52 = 0)
PPC_EXTERN_FUNC(sub_826B2C00);   // ... a step of it (r3 R): 0 going on, 1 closed, 2 failed
PPC_EXTERN_FUNC(sub_82B34778);   // lib (r3 [R +4]): into the room again after a match - its start
PPC_EXTERN_FUNC(sub_82B34790);   // ... its pump (r3 [R +4])
PPC_EXTERN_FUNC(sub_82B347A8);   // ... its result (r3 [R +4]): 0 going on, 1 in the room, 2 failed
PPC_EXTERN_FUNC(sub_826BD180);   // the room host's step after a match (r3 the control; hooked below)

namespace online2v2 {
std::vector<std::pair<uint64_t, std::string>> RoomPeersByName();   // online_2v2_team.cpp
uint64_t PeerXuid(uint64_t peer);
uint32_t PadAssignImpl();                                            // online_2v2_select.cpp

namespace {

constexpr uint32_t kOnline = 0x82E6B558u;      // the online manager: +0 the lib wrapper, +328 the room data, +344 the room control
constexpr uint32_t kGame = 0x82E6C5A8u;        // the game object: +240 the match owner
constexpr uint32_t kPlateLabels = 0x82E168F0u; // the plates' labels: {plate_p1_ready_ng, _ready_ok, _battle, plate_normal, p2 x4}

int64_t Ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string MyName() { return profiles::Active().name; }

// Room names are gamertags: 15 characters (a profile's name may be longer - its first 15 are the room's).
bool SameName(const std::string& a, const std::string& b) { return !a.empty() && a.substr(0, 15) == b.substr(0, 15); }

uint32_t Data(uint8_t* base) { return GRead32(base, kOnline + 344) ? GRead32(base, kOnline + 328) : 0; }
uint32_t MemberAt(uint32_t data, int i) { return data + 20 + 108 * uint32_t(i); }
bool Used(uint8_t* base, uint32_t data, int i) { return i >= 0 && i < 6 && GRead8(base, MemberAt(data, i) + 4) != 0; }
bool Ready(uint8_t* base, uint32_t data, int i) { return Used(base, data, i) && GRead8(base, MemberAt(data, i) + 20) != 0; }
std::string NameOf(uint8_t* base, uint32_t data, int i)
{
    std::string s;
    for (uint32_t k = 0; i >= 0 && i < 6 && k < 20; ++k)
    {
        const char c = char(GRead8(base, MemberAt(data, i) + 30 + k));
        if (!c) break;
        s += c;
    }
    return s;
}
int RawOrder(uint8_t* base, uint32_t data, int k) { return int16_t(GRead16(base, data + 668 + 8 + 2 * uint32_t(k))); }
// The order's members (I+8..18; unused entries and repeats skipped - the game compacts it, a gap repeats an entry).
std::vector<int> Order(uint8_t* base, uint32_t data)
{
    std::vector<int> out;
    for (int k = 0; k < 6; ++k)
    {
        const int m = RawOrder(base, data, k);
        if (Used(base, data, m) && std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
    }
    return out;
}
int Players(size_t members) { return int((std::min)(members, size_t(4))); }
std::string Names(uint8_t* base, uint32_t data, const std::vector<int>& order)
{
    std::string s;
    for (int m : order) s += (s.empty() ? "" : ", ") + NameOf(base, data, m);
    return s.empty() ? std::string("-") : s;
}
// A new order, as the game's sub_826BCE58 writes one: I+8..18 and the copy I+20..30 (-1 padded), the fighters I+0 / I+4
// its first two; the room host sends it (+912).
void WriteOrder(uint8_t* base, uint32_t data, const std::vector<int>& order)
{
    for (int k = 0; k < 6; ++k)
    {
        const uint16_t v = uint16_t(k < int(order.size()) ? order[k] : -1);
        GWrite16(base, data + 668 + 8 + 2 * uint32_t(k), v);
        GWrite16(base, data + 668 + 20 + 2 * uint32_t(k), v);
    }
    GWrite32(base, data + 668, uint32_t(order.size() > 0 ? order[0] : -1));
    GWrite32(base, data + 668 + 4, uint32_t(order.size() > 1 ? order[1] : -1));
    if (XSessionLocalHosts()) GWrite8(base, data + 912, 1);
}

// This PC's member (-1: none) - the game's own answer (sub_826C4868: the session's member index).
int MyMember(PPCContext& ctx, uint8_t* base, uint32_t data)
{
    PPCContext c = ctx;
    c.r3.u64 = data;
    sub_826C4868(c, base);
    const uint32_t entry = c.r3.u32;
    if (!entry || entry < data + 20) return -1;
    const uint32_t i = (entry - data - 20) / 108;
    return i < 6 ? int(i) : -1;
}

// The teams of the room's last PAIR PLAY match (by name: member numbers change as members come and go) - the room host's
// order after it (PairAfterMatch).
struct LastTeams { uint64_t session = 0; std::string leader[2], partner[2]; };
std::mutex g_lock;
LastTeams g_lastTeams;
// The preferred side of the order's second member, set for the sync engine's start (WriteMatchOrder) and put back after.
int g_prefMember = -1;
uint32_t g_prefValue = 0;
int g_prefMember2 = -1;                       // ... and the red side's fighter's (the host may be either)
uint32_t g_prefValue2 = 0;

std::atomic<uint32_t> g_battleStarts{ 0 };     // OnlineInRoom::state_battle_start entered (PairRoomState)
std::atomic<bool> g_resultCounted{ false };    // AfterMatchKeepOrder counted the last match: the room must not again
std::atomic<bool> g_startCancelled{ false };   // the last start's PAIR PLAY screen was called off (PairStartCancelled)

// A called-off start's close (PairCallOffClose; the main thread's).
MatchClose g_close;
enum CloseStep : int { kCloseStart = 0, kCloseClosing, kCloseEntering, kCloseDone };
constexpr int64_t kCloseStepMs = 15000;        // a step not answered in this long: the next one anyway
bool g_keepOrder = false;                      // AfterMatchKeepOrder: the room host's after-match step keeps the order

int MemberByName(uint8_t* base, uint32_t data, const std::string& name)
{
    for (int i = 0; i < 6 && !name.empty(); ++i)
        if (Used(base, data, i) && SameName(NameOf(base, data, i), name)) return i;
    return -1;
}

} // namespace

// ---- who plays
// The room this PC is in plays PAIR PLAY matches: a PAIR PLAY room of three members or more (two play the game's 1-on-1).
bool PairMatchRoom(uint8_t* base)
{
    if (!base || !PairRoom()) return false;
    const uint32_t data = Data(base);
    return data && Order(base, data).size() >= 3;
}

// A member is one of the next match's players: the order's first four.
static bool Marked(uint8_t* base, uint32_t data, int member)
{
    const std::vector<int> order = Order(base, data);
    const auto it = std::find(order.begin(), order.end(), member);
    return it != order.end() && int(it - order.begin()) < Players(order.size());
}

// The game's count of READY fighters (sub_826BCCC8: 2 both, 1 one): 2 once every player is READY, 1 when all but one are.
static uint32_t PairReadyCount(uint8_t* base)
{
    const uint32_t data = Data(base);
    const std::vector<int> order = Order(base, data);
    const int n = Players(order.size());
    int ready = 0;
    for (int k = 0; k < n; ++k) ready += Ready(base, data, order[k]);
    return ready >= n ? 2u : ready >= n - 1 ? 1u : 0u;
}

// The room as a match starts (sub_826BAED0 from state_room_main): is this PC a watcher? -1: not a PAIR PLAY match room's
// question (the game's answer stands).
int PairWatcher(PPCContext& ctx, uint8_t* base)
{
    if (!PairMatchRoom(base)) return -1;
    const uint32_t data = Data(base);
    const int me = MyMember(ctx, base, data);
    return me >= 0 && Marked(base, data, me) ? 0 : 1;
}

// A state machine entered a state (online_2v2_team.cpp's sub_825FE600 hook).
void PairRoomState(const std::string& name)
{
    if (name == "&OnlineInRoom::Impl::state_battle_start") ++g_battleStarts;
}

int PairMemberPresent(uint8_t* base, const std::string& name)
{
    const uint32_t data = base ? Data(base) : 0;
    if (!data) return -1;
    return MemberByName(base, data, name) >= 0 ? 1 : 0;
}

void PairStartCancelled() { g_startCancelled = true; }

// A called-off start (no match): this PC's part of what GameMode does after a room's match, before it goes back into the
// room - the session's close (state_gamemode_online_close_game_start / _wait: sub_826B2BF0, then sub_826B2C00 until it
// answers - the session's end, XSessionEnd) and the room's entry (state_gamemode_online_room_entry / _wait: sub_82B34778,
// then sub_82B34790 + sub_82B347A8). Without them each member stays "in a match" in the session (its member +28 3 since
// the battle start's XSessionStart) and the room host's member update clears every READY as it comes while all of them
// are (sub_826C21B8 at 0x826C2E58) - MEASURED 2026-11-07: after a called-off start the room took no READY. Only for a PC
// whose session started (the room net's +48 2, sub_826B2B60). Like GameMode's states it waits while the online manager
// is busy (sub_826B8B28), and like them it goes on to the room's entry whether the close worked or not.
bool MatchCloseStep(PPCContext& ctx, uint8_t* base, MatchClose& close, const char* what)
{
    const int64_t now = Ms();
    const uint32_t room = GRead32(base, kOnline);
    const uint32_t lib = room ? GRead32(base, room + 4) : 0;
    PPCContext c = ctx;
    const auto waiting = [&] {
        c.r3.u64 = kOnline;
        sub_826B8B28(c, base);
        return (c.r3.u32 & 0xFF) != 0 && now - close.at < kCloseStepMs;
    };
    switch (close.step)
    {
    case kCloseStart:
        if (!room || !lib || GRead32(base, room + 48) != 2)
        {
            std::fprintf(stderr, "[pair-room] %s: this PC's session never started - nothing to close\n", what);
            close.step = kCloseDone;
            return true;
        }
        c.r3.u64 = room;
        sub_826B2BF0(c, base);
        close.step = kCloseClosing;
        close.at = now;
        std::fprintf(stderr, "[pair-room] %s: the session closes (as after a match)\n", what);
        return false;
    case kCloseClosing:
    {
        if (waiting()) return false;
        c.r3.u64 = room;
        sub_826B2C00(c, base);
        const uint32_t r = c.r3.u32;
        if (r == 0 && now - close.at < kCloseStepMs) return false;
        std::fprintf(stderr, "[pair-room] %s: the session %s after %lld ms - into the room again\n", what,
                     r == 1 ? "closed" : r == 2 ? "WARNING: did not close (failed)" : "WARNING: did not close in time",
                     (long long)(now - close.at));
        c.r3.u64 = lib;
        sub_82B34778(c, base);
        close.step = kCloseEntering;
        close.at = now;
        return false;
    }
    case kCloseEntering:
    {
        if (waiting()) return false;
        c.r3.u64 = lib;
        sub_82B34790(c, base);
        c.r3.u64 = lib;
        sub_82B347A8(c, base);
        const uint32_t r = c.r3.u32;
        if (r == 0 && now - close.at < kCloseStepMs) return false;
        close.entry = r == 1 ? 1 : r == 2 ? 2 : 3;
        std::fprintf(stderr, "[pair-room] %s: %s after %lld ms\n", what,
                     r == 1 ? "in the room again" : r == 2 ? "WARNING: the room's entry failed" : "WARNING: the room's entry did not answer in time",
                     (long long)(now - close.at));
        close.step = kCloseDone;
        return true;
    }
    default:
        return true;
    }
}

bool PairCallOffClose(PPCContext& ctx, uint8_t* base) { return MatchCloseStep(ctx, base, g_close, "called-off start"); }

// The next match's fight began (rematch::NoteState: GameMode's state_game_battle): the result is that match's from now -
// AfterMatchKeepOrder's count of the last one stood until then (a straight start that came to nothing: the room's own
// after-match step finds it counted).
void AfterMatchGoesOn() { g_resultCounted = false; }

// The room host, before a match started straight from the end menu: sub_826BD180 as the room's after-match step runs it
// (the result taken, the win streaks - sub_826BBD08), its order step (sub_826BCE58, hooked below) told to keep the order -
// the win limit too: nobody waits for the place. Then the room data to every PC as the room's own after-match step sends
// it (sub_826BD630 at 0x826BD7A0): the order block (D+912) and every member's record (D+913) - the order block alone
// carries no streaks (MEASURED 2026-11-07 pr6c: no PC but the host had them, the HUD showed no WIN for a winner).
void AfterMatchKeepOrder(PPCContext& ctx, uint8_t* base)
{
    const uint32_t control = GRead32(base, kOnline + 344);
    const uint32_t data = Data(base);
    if (!control || !data) return;
    // no result here (the sync engine's G+4 - a partner's, and no fighter told it: rematch::FightersResult): nothing to
    // count - not the room's fallback either (the result its members report when they come back to the room, which they
    // never did since: an older match's), and never later (counted)
    if (const int32_t result = int32_t(GRead32(base, 0x82E6C5B8u + 7488u + 4u)); result < -1 || result > 1)
    {
        g_resultCounted = true;
        PPCContext c = ctx;
        c.r3.u64 = data;
        c.r4.u64 = 1;
        sub_826C4A50(c, base);                 // every PC holds the host's streaks all the same
        std::fprintf(stderr, "[pair-room] the last match: no result here (%d) - the win streaks stay, the order kept\n", result);
        return;
    }
    uint32_t before[6] = {};
    for (int i = 0; i < 6; ++i) before[i] = GRead32(base, MemberAt(data, i) + 12);
    g_keepOrder = true;
    PPCContext c = ctx;
    c.r3.u64 = control;
    sub_826BD180(c, base);
    g_keepOrder = false;
    g_resultCounted = true;
    std::string streaks;
    for (int i = 0; i < 6; ++i)
        if (Used(base, data, i))
            streaks += " " + NameOf(base, data, i) + " " + std::to_string(before[i]) + "->" + std::to_string(GRead32(base, MemberAt(data, i) + 12));
    c = ctx;
    c.r3.u64 = data;
    c.r4.u64 = 1;
    sub_826C4A38(c, base);
    c = ctx;
    c.r3.u64 = data;
    c.r4.u64 = 1;
    sub_826C4A50(c, base);
    std::fprintf(stderr, "[pair-room] the last match counted, the order kept, every member's record sent (win streaks:%s)\n",
                 streaks.c_str());
}

// ---- the room's plates: rows 2 / 3 as the game's own rows 0 / 1 are, while they hold a player
static void PairPlate(PPCContext& ctx, uint8_t* base)
{
    const int row = int32_t(ctx.r22.u32);
    if (row < 2 || row > 3 || !PairMatchRoom(base)) return;
    const uint32_t data = Data(base);
    const int member = RawOrder(base, data, row);
    if (!Used(base, data, member) || !Marked(base, data, member)) return;
    const uint32_t phase = GRead32(base, data + 668 + 32);
    const int n = phase == 0 ? 3 : (phase == 2 || Ready(base, data, member)) ? 2 : 0;
    if (const uint32_t label = GRead32(base, kPlateLabels + 4 * uint32_t(4 * (row - 2) + n))) ctx.r4.u64 = label;
}

// ---- the room host's 16-second rule (the countdown ran out in the READY phase)
// The game's: the first fighter READY and the second not - the second goes to the back; neither - both do (with someone
// waiting). Here: the players who are not READY give their places to the waiting members (the first in line first) and go
// to the back; nobody waiting - nothing changes and the countdown starts again, as the game's does with two members. A
// player who did not READY loses its win streak (the game resets a not-READY champion's).
static void PairTimer(uint8_t* base)
{
    const uint32_t data = Data(base);
    const std::vector<int> order = Order(base, data);
    const int n = Players(order.size());
    std::vector<int> keep, out, waiting(order.begin() + n, order.end());
    for (int k = 0; k < n; ++k) (Ready(base, data, order[k]) ? keep : out).push_back(order[k]);
    if (out.empty() || waiting.empty())
    {
        std::fprintf(stderr, "[pair-room] 16 s: %s - the order stays (%s)\n", out.empty() ? "every player is READY" : "nobody waiting",
                     Names(base, data, order).c_str());
        return;
    }
    const size_t take = (std::min)(out.size(), waiting.size());
    std::vector<int> next = keep;
    next.insert(next.end(), waiting.begin(), waiting.begin() + take);                  // in for the ones not READY
    next.insert(next.end(), out.begin(), out.end() - take);                            // not READY, nobody for them
    next.insert(next.end(), waiting.begin() + take, waiting.end());
    next.insert(next.end(), out.end() - take, out.end());                              // to the back
    for (int m : out) GWrite32(base, MemberAt(data, m) + 12, 0);
    WriteOrder(base, data, next);
    std::fprintf(stderr, "[pair-room] 16 s: not READY %s - the order now %s\n", Names(base, data, out).c_str(), Names(base, data, next).c_str());
}

// ---- the win streaks and the room host's order after a match
// The game's (sub_826BD180, the room host's step after a match): the result r = the sync engine's G+4 (0 / 1 the order's
// first / second fighter won, -1 a draw; a watcher with none takes the first fighter's reported one, member +50), then
// the streaks (sub_826BBD08, only while the room counts wins - session +28) and the order (sub_826BCE58, r4: 0 the first
// won - a draw counts as its win -, 1 the second, 3 the first with the room's WIN LIMIT reached - both to the back -, -1
// none). The game's rules hold only for its own order, where the first fighter is the champion and the second always a
// challenger with no streak: it counts the second's win as 1 and never tests it against the WIN LIMIT. Two paths break
// that: a match started straight from the end menu keeps the order (AfterMatchKeepOrder: a challenger that won stays
// second), and PAIR PLAY plays teams that change from match to match (the PAIR PLAY screen, CHANGE TEAMS). For those the
// streaks are written here from the step's own result: every player of the winning side one more win (its own streak),
// the losing side 0; the WIN LIMIT for whichever side won (one of its players reaching it: its streaks 0, both sides to
// the back); a draw as the game's - a win for the side that held the place (the higher streak; a tie: the first
// fighter's). A streak above 0 keeps a player READY for the next match (the game's own after-match step, sub_826BD630).
struct StreakBefore { bool valid = false; uint32_t data = 0; int32_t raw = -2; uint32_t streak[6] = {}; };
static StreakBefore g_streakBefore;               // the room host's after-match step: as it began (the main thread's)

struct MatchOutcome
{
    int win = -1;                          // the winning side: 0 the first fighter's, 1 the second's (-1 not counted here)
    bool teams = false;                    // the sides are the last PAIR PLAY match's teams
    bool limit = false;                    // the WIN LIMIT reached
    std::vector<int> side[2];              // the members of each side (the first / second fighter's)
};

// The last PAIR PLAY match's teams as members (-1: gone). False: not this session's.
static bool LastTeamMembers(uint8_t* base, uint32_t data, int leader[2], int partner[2])
{
    LastTeams t;
    {
        std::lock_guard l(g_lock);
        t = g_lastTeams;
    }
    if (t.session != XSessionCurrentId()) return false;
    for (int s = 0; s < 2; ++s)
    {
        leader[s] = MemberByName(base, data, t.leader[s]);
        partner[s] = MemberByName(base, data, t.partner[s]);
    }
    return true;
}

// The room host's after-match step begins (its hook, before the game's step): the result and the streaks as they are. A
// room host that partnered in the PAIR PLAY match has no result of its own (the sync engine's spectator: G+4 -2, and the
// game takes a reported one only for a watcher - a partner is none, the match's order holds it): the fighters' votes at
// the end menu carry theirs (rematch::FightersResult, as online_straight.cpp's straight start does).
static void AfterMatchBefore(PPCContext& ctx, uint8_t* base)
{
    g_streakBefore = {};
    const uint32_t data = Data(base);
    if (!data) return;
    constexpr uint32_t kResult = 0x82E6C5B8u + 7488u + 4u;
    int32_t raw = int32_t(GRead32(base, kResult));
    if ((raw < -1 || raw > 1) && !g_keepOrder && PairMatchRoom(base))
    {
        int leader[2], partner[2];
        const int me = MyMember(ctx, base, data);
        if (me >= 0 && LastTeamMembers(base, data, leader, partner) && (me == partner[0] || me == partner[1]))
        {
            const int fought = rematch::FightersResult();
            if (fought >= -1 && fought <= 1)
            {
                GWrite32(base, kResult, uint32_t(fought));
                raw = fought;
                std::fprintf(stderr, "[pair-room] the room host partnered in the match: the fighters' result (%d)\n", fought);
            }
            else
                std::fprintf(stderr, "[pair-room] WARNING: the room host partnered in the match and no fighter told the result\n");
        }
    }
    if (raw == -2)                         // the game's watcher: the first fighter's reported result, or the second's
    {
        const int i0 = int32_t(GRead32(base, data + 668)), i4 = int32_t(GRead32(base, data + 672));
        const int m = i0 >= 0 ? i0 : i4;
        if (m >= 0 && m < 6) raw = int8_t(GRead8(base, MemberAt(data, m) + 50));
    }
    g_streakBefore.valid = true;
    g_streakBefore.data = data;
    g_streakBefore.raw = raw;
    for (int i = 0; i < 6; ++i) g_streakBefore.streak[i] = GRead32(base, MemberAt(data, i) + 12);
}

// The game's count holds only for its own order, the second fighter a challenger with no streak. A match started
// straight from the end menu keeps the order (AfterMatchKeepOrder): a challenger that won stays second with its streak,
// and the match after it may end the room's way (RETURN TO ROOM: the room host's own after-match step) - the game would
// count that second fighter's win as 1 (a draw: 0) and never test it against the WIN LIMIT. True: the order step's
// streaks are this file's (MatchStreaks) for a 1v1 room too.
static bool SecondHeldStreak(uint8_t* base)
{
    const StreakBefore& b = g_streakBefore;
    const uint32_t data = Data(base);
    if (!b.valid || !data || b.data != data) return false;
    const int i4 = int32_t(GRead32(base, data + 672));
    return i4 >= 0 && i4 < 6 && b.streak[i4] > 0;
}

// The streaks after the match (sub_826BD180's order step, 0x826BD2AC: sub_826BBD08 counted the game's way just before).
// `control` the room control, `mode` the step's order mode. Returns the order mode to use.
static int MatchStreaks(PPCContext& ctx, uint8_t* base, uint32_t control, int mode, MatchOutcome& out)
{
    const StreakBefore b = g_streakBefore;
    g_streakBefore.valid = false;
    const uint32_t data = Data(base);
    if (!b.valid || !data || b.data != data || (mode != 0 && mode != 1 && mode != 3)) return mode;   // no result: as the game
    const int i0 = int32_t(GRead32(base, data + 668)), i4 = int32_t(GRead32(base, data + 672));
    if (i0 < 0 || i0 > 5 || i4 < 0 || i4 > 5 || i0 == i4) return mode;
    PPCContext c = ctx;
    const uint32_t net = GRead32(base, control + 56);
    c.r3.u64 = net ? GRead32(base, net + 4) : 0;
    sub_82B35088(c, base);
    const uint32_t S = c.r3.u32;
    if (!S || !GRead8(base, S + 28)) return mode;                      // the room counts no wins (sub_826BBD08's test)
    const uint32_t settings = GRead32(base, S + 80);
    const uint32_t limit = settings ? GRead8(base, settings + 165) : 0;
    out.side[0] = { i0 };
    out.side[1] = { i4 };
    int leader[2], partner[2];
    if (PairMatchRoom(base) && LastTeamMembers(base, data, leader, partner))
    {
        int found = 0;
        for (int k = 0; k < 2; ++k)
        {
            const int fighter = k == 0 ? i0 : i4;
            for (int s = 0; s < 2; ++s)
                if (leader[s] >= 0 && (fighter == leader[s] || fighter == partner[s]))
                {
                    out.side[k] = { leader[s] };
                    if (partner[s] >= 0) out.side[k].push_back(partner[s]);
                    ++found;
                    break;
                }
        }
        out.teams = found == 2 && out.side[0][0] != out.side[1][0];
        if (!out.teams)
        {
            out.side[0] = { i0 };
            out.side[1] = { i4 };
            std::fprintf(stderr, "[pair-room] the last match's fighters (%s, %s) are not its two teams - streaks by fighter\n",
                         NameOf(base, data, i0).c_str(), NameOf(base, data, i4).c_str());
        }
    }
    const auto best = [&](int s) {
        uint32_t v = 0;
        for (int m : out.side[s]) v = (std::max)(v, b.streak[m]);
        return v;
    };
    int win = mode == 1 ? 1 : 0;
    if (mode != 1 && b.raw == -1) win = best(1) > best(0) ? 1 : 0;   // a draw: the side that held the place
    out.win = win;
    out.limit = limit != 0 && best(win) + 1 >= limit;
    std::string log;
    for (int s : { win, 1 - win })
        for (int m : out.side[s])
        {
            const uint32_t v = s == win && !out.limit ? b.streak[m] + 1 : 0;
            GWrite32(base, MemberAt(data, m) + 12, v);
            log += (log.empty() ? "" : ", ") + NameOf(base, data, m) + " " + std::to_string(b.streak[m]) + "->" + std::to_string(v);
        }
    const int next = out.limit ? 3 : win;
    std::fprintf(stderr, "[pair-room] win streaks: %s (result %d, %s won%s; order mode %d -> %d)\n", log.c_str(), b.raw,
                 win == 0 ? "the first fighter's side" : "the second fighter's side",
                 out.limit ? (", the WIN LIMIT " + std::to_string(limit) + " reached").c_str() : "", mode, next);
    return next;
}

// The room host's order after a PAIR PLAY match (the order step, `mode` from MatchStreaks): the teams move as one - the
// game's own rule with the fighters as teams: the winning team stays first, the waiting members come next, the losing
// team goes to the back; at the WIN LIMIT both teams go to the back (the winners last). False: not known as two teams of
// this room (the game's order).
static bool PairAfterMatch(uint8_t* base, const MatchOutcome& o, int mode)
{
    if (!PairMatchRoom(base) || !o.teams || o.win < 0 || (mode != 0 && mode != 1 && mode != 3)) return false;
    const uint32_t data = Data(base);
    std::vector<int> winners, losers, waiting;
    for (int m : o.side[o.win])
        if (Used(base, data, m)) winners.push_back(m);
    for (int m : o.side[1 - o.win])
        if (Used(base, data, m)) losers.push_back(m);
    for (int m : Order(base, data))
        if (std::find(winners.begin(), winners.end(), m) == winners.end() && std::find(losers.begin(), losers.end(), m) == losers.end())
            waiting.push_back(m);
    std::vector<int> next;
    if (mode == 3)                                                     // the win limit: both teams to the back
    {
        next = waiting;
        next.insert(next.end(), losers.begin(), losers.end());
        next.insert(next.end(), winners.begin(), winners.end());
    }
    else
    {
        next = winners;
        next.insert(next.end(), waiting.begin(), waiting.end());
        next.insert(next.end(), losers.begin(), losers.end());
    }
    if (next.size() < 2) return false;
    WriteOrder(base, data, next);
    std::fprintf(stderr, "[pair-room] after the match: %s won%s - the order now %s\n", Names(base, data, winners).c_str(),
                 mode == 3 ? ", the win limit" : "", Names(base, data, next).c_str());
    return true;
}

// ---- the match's start
// The places decided (online_2v2_places.cpp): every PC of the room writes the order they give - the red side's lower
// stone (the side's first fighter), the blue side's, then the red side's upper stone (its partner), the blue side's, then
// the others as they were. The sync engine makes the order's first two the fighters and the rest spectators (partners
// play through their leaders' nodes); the first is its side 0 (left / red) unless the second prefers that side (M+16 0:
// sub_8266C530), so the second's preference is "none" (2) until the engine has started (RestoreSidePref). The room
// control's last fighters seen (C+236 / C+240) follow, or the room host would throw the match's result away (sub_826BD630).
void WriteMatchOrder(PPCContext& ctx, uint8_t* base, const PairPlaces& p)
{
    (void)ctx;
    const uint32_t data = Data(base);
    if (!data) return;
    int seatMember[4] = { -1, -1, -1, -1 };
    for (int s = 0; s < p.count && s < 4; ++s)
    {
        const int m = p.seat[s].member;
        seatMember[s] = Used(base, data, m) && SameName(NameOf(base, data, m), p.seat[s].name) ? m : MemberByName(base, data, p.seat[s].name);
    }
    auto at = [&](int place) { return p.place[place] >= 0 && p.place[place] < 4 ? seatMember[p.place[place]] : -1; };
    const int redLeader = at(0), blueLeader = at(1), redPartner = at(2), bluePartner = at(3);
    if (p.cancelled || redLeader < 0 || blueLeader < 0)
    {
        std::fprintf(stderr, "[pair-room] the match's order stays (%s)\n", p.cancelled ? "the PAIR PLAY screen was cancelled" : "no fighter for a side");
        return;
    }
    std::vector<int> next = { redLeader, blueLeader };
    for (int m : { redPartner, bluePartner })
        if (m >= 0) next.push_back(m);
    for (int m : Order(base, data))
        if (std::find(next.begin(), next.end(), m) == next.end()) next.push_back(m);
    WriteOrder(base, data, next);
    {
        std::lock_guard l(g_lock);
        g_prefMember = blueLeader;
        g_prefValue = GRead32(base, MemberAt(data, blueLeader) + 16);
        g_prefMember2 = redLeader;
        g_prefValue2 = GRead32(base, MemberAt(data, redLeader) + 16);
        g_lastTeams.session = XSessionCurrentId();
        g_lastTeams.leader[0] = NameOf(base, data, redLeader);
        g_lastTeams.leader[1] = NameOf(base, data, blueLeader);
        g_lastTeams.partner[0] = redPartner >= 0 ? NameOf(base, data, redPartner) : std::string();
        g_lastTeams.partner[1] = bluePartner >= 0 ? NameOf(base, data, bluePartner) : std::string();
    }
    // the side swap reads the game host's preference (sub_8266C530 with r7's position): both fighters' are held at "none" -
    // red stays left whichever of them is the host (the stage chooser: online_stage_mirror.cpp)
    GWrite32(base, MemberAt(data, blueLeader) + 16, 2);
    GWrite32(base, MemberAt(data, redLeader) + 16, 2);
    if (const uint32_t control = GRead32(base, kOnline + 344))
    {
        GWrite32(base, control + 236, uint32_t(redLeader));
        GWrite32(base, control + 240, uint32_t(blueLeader));
    }
    std::fprintf(stderr, "[pair-room] the match's order: %s (red %s%s%s, blue %s%s%s)\n", Names(base, data, next).c_str(),
                 NameOf(base, data, redLeader).c_str(), redPartner >= 0 ? " + " : "", redPartner >= 0 ? NameOf(base, data, redPartner).c_str() : "",
                 NameOf(base, data, blueLeader).c_str(), bluePartner >= 0 ? " + " : "", bluePartner >= 0 ? NameOf(base, data, bluePartner).c_str() : "");
}

static void RestoreSidePref(uint8_t* base)
{
    std::lock_guard l(g_lock);
    const uint32_t data = Data(base);
    if (g_prefMember >= 0 && data && Used(base, data, g_prefMember)) GWrite32(base, MemberAt(data, g_prefMember) + 16, g_prefValue);
    if (g_prefMember2 >= 0 && data && Used(base, data, g_prefMember2)) GWrite32(base, MemberAt(data, g_prefMember2) + 16, g_prefValue2);
    g_prefMember = g_prefMember2 = -1;
}

// A player's PAIR PLAY screen asked for CHARASELECT (online_2v2_places.cpp PlacesModeAsked): what OnlineInRoom's battle
// start does before it asks for it (sub_825FB650), with the order the places gave - the sync engine started (kind 2, a
// node per member of the order), this side entered, the other not. Returns the flags to ask with: the side (1 left, 2
// right) | 0x1000 (a room's match) | 0x20 (the room's VS MODE setting 1). A partner's side is its team's (its node is a
// spectator's: the select asks are answered for it, online_2v2_select.cpp). The engine failing to start is the game's
// battle start's failure (sub_8266C608(NG, 1)); character select's own online end takes the players back to the room.
uint32_t StartPairMatch(PPCContext& ctx, uint8_t* base, const PairPlaces& p, int mySeat, int hostPos, int* sideOut)
{
    hostPos = hostPos == 0 ? 0 : 1;
    int myPlace = -1;
    for (int k = 0; k < 4; ++k)
        if (mySeat >= 0 && p.place[k] == mySeat) myPlace = k;
    int side = myPlace < 0 ? 0 : (myPlace & 1);                        // places 1 / 3 red, 2 / 4 blue
    const bool leader = myPlace == 0 || myPlace == 1;
    PPCContext c = ctx;
    const uint32_t gs = GRead32(base, kOnline);
    c.r3.u64 = gs ? GRead32(base, gs + 4) : 0;
    sub_82B35088(c, base);
    const uint32_t session = c.r3.u32;
    const uint32_t settings = session ? GRead32(base, session + 80) : 0;
    sub_82274750(c, base);
    const uint32_t ng = c.r3.u32;
    c.r3.u64 = ng;
    c.r4.u64 = 2;
    sub_8266C5F0(c, base);
    sub_826677C0(c, base);
    c.r3.u64 = ng;
    c.r4.u64 = session;
    c.r5.u64 = GRead32(base, kOnline + 328);
    c.r6.u64 = 2;
    c.r7.u64 = uint64_t(hostPos);              // the game host - the stage chooser (the battle start's 1: the blue side's fighter)
    sub_8266CD08(c, base);
    const bool started = (c.r3.u32 & 0xFF) != 0;
    RestoreSidePref(base);
    if (started) stagechooser::Started(hostPos, "PAIR PLAY's start");
    if (!started)
    {
        c.r3.u64 = ng;
        c.r4.u64 = 1;
        sub_8266C608(c, base);
        std::fprintf(stderr, "[pair-room] the sync engine did not start - character select's own online end follows\n");
    }
    else
    {
        c.r3.u64 = 1;
        sub_82668F08(c, base);
        if (leader)
        {
            c.r3.u64 = ng;
            sub_8266C740(c, base);
            const int engineSide = int32_t(c.r3.u32);
            if (engineSide != side) std::fprintf(stderr, "[pair-room] WARNING: the engine puts this fighter on side %d, the places on %d\n", engineSide, side);
            if (engineSide == 0 || engineSide == 1) side = engineSide;
        }
        const uint32_t game = GRead32(base, kGame);
        const uint32_t owner = game ? GRead32(base, game + 240) : 0;
        for (uint32_t s = 0; s < 2 && owner; ++s)
        {
            c.r3.u64 = owner;
            c.r4.u64 = s;
            c.r5.u64 = int(s) == side ? 1 : 0;
            sub_82356140(c, base);
        }
    }
    const uint32_t flags = (side ? 2u : 1u) | 0x1000u | (settings && GRead8(base, settings + 161) == 1 ? 0x20u : 0u);
    if (sideOut) *sideOut = started ? side : -1;
    std::fprintf(stderr, "[pair-room] the match starts: this PC %s on the %s side - character select (flags %X)\n",
                 myPlace < 0 ? "(no place)" : leader ? "fights" : "partners", side ? "blue" : "red", flags);
    return flags;
}

// OnlineInRoom::state_battle_start of a PAIR PLAY match room (sub_825FB650 below): true - not yet. A player opens the
// PAIR PLAY screen (PADASSIGN, mode 22, its PAIR sequence: 0x80 - the room leaves for it as it does for character select),
// a watcher waits for the places and then goes on as the game's spectator (the battle start's own work, with the order
// they give).
static bool HoldBattleStart(PPCContext& ctx, uint8_t* base, uint32_t impl)
{
    enum class Kind { None, Player, Watcher, Closing, Leaving };
    static uint32_t seen = 0;
    static Kind kind = Kind::None;
    static bool asked = false;
    static int64_t since = 0;
    static const char* closingWhy = "";
    const int64_t now = Ms();
    if (g_battleStarts.load() != seen)
    {
        seen = g_battleStarts.load();
        g_startCancelled = false;              // a called-off start the host's after-match step never saw: past
        g_resultCounted = false;               // ... and a straight start's count: this match's result is new
        g_close = MatchClose{};                // a called-off start's close (PairCallOffClose) for this start
        kind = Kind::None;
        asked = false;
        since = now;
        if (!PairMatchRoom(base)) ClearPairSides("a room battle start of two players");
        if (PairMatchRoom(base))
        {
            const uint32_t data = Data(base);
            const std::vector<int> order = Order(base, data);
            const int n = Players(order.size());
            const int me = MyMember(ctx, base, data);
            const auto peers = RoomPeersByName();
            PairSeat seats[4];
            int mine = -1;
            for (int s = 0; s < n; ++s)
            {
                seats[s].member = order[s];
                seats[s].name = NameOf(base, data, order[s]);
                if (order[s] == me)
                {
                    mine = s;
                    seats[s].xuid = xlive::OnlineXuid(0);
                }
                else
                    for (const auto& [peer, name] : peers)
                        if (SameName(name, seats[s].name)) seats[s].xuid = PeerXuid(peer);
            }
            std::fprintf(stderr, "[pair-room] the match starts: players %s - this PC %s\n", Names(base, data, std::vector<int>(order.begin(), order.begin() + n)).c_str(),
                         mine >= 0 ? "plays (the PAIR PLAY screen)" : "watches (waits for the places)");
            if (mine >= 0)
            {
                if (PlacesBegin(seats, n, mine)) kind = Kind::Player;
            }
            else
            {
                PlacesWatch(seats, n);
                kind = Kind::Watcher;
            }
        }
    }
    if (kind == Kind::Player)
    {
        if (!asked)
        {
            asked = true;
            const uint32_t manager = GRead32(base, impl + 120);
            const uint32_t fn = manager ? GRead32(base, GRead32(base, manager) + 4) : 0;
            if (fn)
            {
                PPCContext c = ctx;
                c.r3.u64 = manager;
                c.r4.u64 = 22;
                c.r5.u64 = 0x80;
                c.r6.u64 = 0;
                PPC_LOOKUP_FUNC(base, fn)(c, base);
                std::fprintf(stderr, "[pair-room] the PAIR PLAY screen asked for (mode 22)\n");
            }
        }
        return true;
    }
    if (kind == Kind::Watcher)
    {
        // no match after all: the players' screen was called off (a player left), or no word of the places came - this PC
        // closes its part of the match (PairCallOffClose, the "Please wait." window still up) and then comes back into the
        // room as the players do (PlacesModeAsked)
        PairPlaces p;
        if (PlacesDecided(p))
        {
            kind = Kind::None;
            if (!p.cancelled)
            {
                WriteMatchOrder(ctx, base, p);
                stagechooser::WatcherNext(p.chooser);   // the battle start's own engine start (sub_8266CD08 at 0x825FB6A4)
                std::fprintf(stderr, "[pair-room] the places came after %lld ms - this PC watches the match\n", (long long)(now - since));
                return false;
            }
            closingWhy = "the PAIR PLAY screen was called off";
        }
        else if (now - since > 150000)
            closingWhy = "no places came";
        else
            return true;
        PairStartCancelled();
        kind = Kind::Closing;
        std::fprintf(stderr, "[pair-room] %s after %lld ms - no match\n", closingWhy, (long long)(now - since));
    }
    if (kind == Kind::Closing)
    {
        if (!PairCallOffClose(ctx, base)) return true;
        // back into the room as after a match: OnlineInRoom again with flags 1 (the room control kept). The battle start
        // already put the room's screen away (state_room_main's way here) and left its "Please wait." up.
        kind = Kind::Leaving;
        {
            PPCContext c = ctx;
            sub_82281648(c, base);
            sub_8266E2D0(c, base);
        }
        const uint32_t manager = GRead32(base, impl + 120);
        const uint32_t fn = manager ? GRead32(base, GRead32(base, manager) + 4) : 0;
        if (fn)
        {
            PPCContext c = ctx;
            c.r3.u64 = manager;
            c.r4.u64 = 35;
            c.r5.u64 = 1;
            c.r6.u64 = 0;
            PPC_LOOKUP_FUNC(base, fn)(c, base);
        }
        std::fprintf(stderr, "[pair-room] %s - this PC back into the room after %lld ms\n", closingWhy, (long long)(now - since));
        return true;
    }
    return kind == Kind::Leaving;              // the room is being left for itself: the game's battle start never runs
}

// ---- the win streaks: the room host's count is the room's
// The room host takes a member's entry in (message 513: sub_826C21B8 copies its 56 bytes over the member's record at
// 0x826C2A28, then sends that record to everyone). Every member builds its entries from its own copy of its record
// (sub_826C4868), win streak (+12) included - and only the room host ever counts one (sub_826BBD08, sub_826BD2D0,
// MatchStreaks, PairTimer, a member gone: sub_826C1E98): a fighter's entries every 3 s through a match (sub_826BB0D8) and
// a member's entry back in the room carried its stale copy over the host's count (MEASURED 2026-11-07 pr6c: B 1 -> 0
// within 1 s of a STAGE SELECT's count, D 2 -> 0 on RETURN TO ROOM). The host's count stays: put into the entry's copy
// before the copy - a late entry for a member gone keeps the host's 0 too. A joiner's record (514) is not an entry.
// `data` the room data, `member` the entry's member, `record` its record, `entry` the entry's copy.
void HostKeepsStreak(uint8_t* base, uint32_t data, uint32_t member, uint32_t record, uint32_t entry)
{
    if (member >= 6 || !record) return;
    const uint32_t mine = GRead32(base, record + 12), theirs = GRead32(base, entry + 12);
    static const bool diag = [] { const char* e = std::getenv("TTT2_2V2_DIAG"); return e && *e == '1'; }();
    if (diag)
        std::fprintf(stderr, "[room-msg] the host takes member %u's entry: its streak %u, the host's %u\n", member, theirs, mine);
    if (mine == theirs) return;
    GWrite32(base, entry + 12, mine);
    std::fprintf(stderr, "[pair-room] member %u (%s)'s entry carried win streak %u: the room host's %u kept\n", member,
                 NameOf(base, data, int(member)).c_str(), theirs, mine);
}

// The fight HUD's "WIN: n" in a room match (sprite::battle::GaugeTcb, sub_823D6980 -> sub_823D5CD0 for a match kind other
// than 0): one label, on side S+754 with count S+756 - both set on every PC by the battle setup's apply (sub_826689C8,
// from this PC's room data): the side the order's first fighter (I+0) plays and the higher of the two fighters' streaks.
// The game's room puts its champion first; a match started straight from the end menu keeps the order
// (AfterMatchKeepOrder), and a PAIR PLAY match puts the red leader first - the second fighter's streak was drawn under
// the first fighter. The label goes on the side that holds the streak, with that side's streak: the fighter's own, in a
// PAIR PLAY match its team's best (MatchStreaks' best: the host's count adds a win to every winner, so after CHANGE TEAMS a
// partner may hold more than its leader); a tie stays with the first fighter (the game's rule). Taken when the apply has
// set S+754 / S+756 (its call to sub_8266C5E0 right after, 0x82668C44), from the same room data (the host sends every
// record after its count - AfterMatchKeepOrder - so every PC holds its counts by then). S keeps the game's values (S+754
// has other readers - the sides' setup, the replay header): only the HUD call's arguments and the VS screen's copy
// (sub_82654AE0) get the label. Runtime state, never the rollback's.
struct StreakLabel
{
    bool on = false;
    uint64_t session = 0;                      // the room's (an offline match after it: none of ours)
    uint8_t gameSide = 0, side = 0;
    uint32_t gameN = 0, n = 0;
};
static std::mutex g_labelLock;                 // the HUD's draw is not proven to be the main thread's
static StreakLabel g_label;

static StreakLabel LabelNow()
{
    StreakLabel l;
    {
        std::lock_guard lk(g_labelLock);
        l = g_label;
    }
    if (!l.on || !l.session || l.session != XSessionCurrentId()) l.on = false;
    return l;
}

static uint32_t Streak(uint8_t* base, uint32_t data, int member) { return Used(base, data, member) ? GRead32(base, MemberAt(data, member) + 12) : 0; }

// The battle setup's apply set S+754 / S+756 (sub_8266C5E0 from 0x82668C44): the label from this PC's room data.
void LatchStreakLabel(PPCContext& ctx, uint8_t* base)
{
    {
        std::lock_guard lk(g_labelLock);
        g_label = StreakLabel{};
    }
    constexpr uint32_t kG = 0x82E6C5B8u + 7488u;
    if (GRead32(base, kG) != 2) return;        // a room's match kind (the HUD's one label)
    PPCContext c = ctx;
    sub_825B7370(c, base);
    const uint32_t S = c.r3.u32;
    const uint32_t data = Data(base);
    const uint64_t session = XSessionCurrentId();
    if (!S || !data || !session) return;
    const int fighter[2] = { int32_t(GRead32(base, data + 668)), int32_t(GRead32(base, data + 672)) };
    uint32_t best[2] = { Streak(base, data, fighter[0]), Streak(base, data, fighter[1]) };
    int leader[2], partner[2];
    bool teams = false;
    if (PairMatchRoom(base) && LastTeamMembers(base, data, leader, partner))
        for (int k = 0; k < 2; ++k)
            for (int t = 0; t < 2; ++t)
                if (fighter[k] >= 0 && (fighter[k] == leader[t] || fighter[k] == partner[t]))
                {
                    best[k] = (std::max)(Streak(base, data, leader[t]), Streak(base, data, partner[t]));
                    teams = true;
                    break;
                }
    StreakLabel l;
    l.on = true;
    l.session = session;
    l.gameSide = GRead8(base, S + 754);
    l.gameN = GRead32(base, S + 756);
    const int holder = best[1] > best[0] ? 1 : 0;
    l.side = holder == 0 ? l.gameSide : uint8_t(l.gameSide ? 0 : 1);
    l.n = best[holder];
    {
        std::lock_guard lk(g_labelLock);
        g_label = l;
    }
    std::fprintf(stderr, "[pair-room] the fight's WIN label: side %u, %u (%s: %s %u, %s %u%s; the game's: side %u, %u)\n", l.side, l.n,
                 teams ? "by team" : "by fighter", NameOf(base, data, fighter[0]).c_str(), best[0], NameOf(base, data, fighter[1]).c_str(),
                 best[1], holder ? " - the second fighter's side holds it" : "", l.gameSide, l.gameN);
}

// The HUD's label call (sub_823D5CD0 from 0x823D6A74: r4 the side, r5 the count, as the game read them from S): the
// label's, while S holds what the apply set.
void StreakLabelArgs(PPCContext& ctx)
{
    const StreakLabel l = LabelNow();
    if (!l.on || uint8_t(ctx.r4.u32) != l.gameSide || ctx.r5.u32 != l.gameN) return;
    ctx.r4.u64 = l.side;
    ctx.r5.u64 = l.n;
}

// The VS screen's copy of the label into the two sides' objects (sub_82654AE0: S+756 to side S+754's +16072, 0 to the
// other's; +15976 the holder's mark): S holds the label's side and count for the copy, then what it held. False: the
// game's copy as it is.
bool StreakLabelSwap(PPCContext& ctx, uint8_t* base, uint32_t& S, uint8_t& side, uint32_t& n)
{
    const StreakLabel l = LabelNow();
    if (!l.on || (l.side == l.gameSide && l.n == l.gameN)) return false;
    PPCContext c = ctx;
    sub_825B7370(c, base);
    S = c.r3.u32;
    if (!S || GRead8(base, S + 754) != l.gameSide || GRead32(base, S + 756) != l.gameN) return false;
    side = l.gameSide;
    n = l.gameN;
    GWrite8(base, S + 754, l.side);
    GWrite32(base, S + 756, l.n);
    return true;
}

} // namespace online2v2

// ---- hooks (CRoomControl's code, every PC's copy of the room; r3 the control)
// May a member press READY (sub_826C01F8, 0x826C02A8) / is it told it plays next (text 368, else 370 - sub_826BD418,
// 0x826BD464)? The game's: it is one of the order's first two.
extern "C" PPC_FUNC(__imp__sub_826BCD98);
PPC_FUNC(sub_826BCD98)
{
    const uint32_t lr = uint32_t(ctx.lr);
    if ((lr == 0x826BD464u || lr == 0x826C02A8u) && online2v2::PairMatchRoom(base))
    {
        ctx.r3.u64 = online2v2::Marked(base, online2v2::Data(base), int32_t(ctx.r4.u32)) ? 1 : 0;
        return;
    }
    __imp__sub_826BCD98(ctx, base);
}

// How many of the next match's fighters are READY (0 / 1 / 2): READY's "Waiting for a response" (0x826C02E0: under 1),
// the start's handshake (0x826BEF78, 0x826BDBB0: 2, else back to the room).
extern "C" PPC_FUNC(__imp__sub_826BCCC8);
PPC_FUNC(sub_826BCCC8)
{
    if (online2v2::PairMatchRoom(base))
    {
        ctx.r3.u64 = online2v2::PairReadyCount(base);
        return;
    }
    __imp__sub_826BCCC8(ctx, base);
}

// The order's member k (r4) - asked by the start check (sub_826BF870, 0x826BFA0C: the match starts when order[0] and
// order[1] are READY): "none" until every player is.
extern "C" PPC_FUNC(__imp__sub_826BBDC0);
PPC_FUNC(sub_826BBDC0)
{
    if (uint32_t(ctx.lr) == 0x826BFA0Cu && online2v2::PairMatchRoom(base) && online2v2::PairReadyCount(base) < 2)
    {
        ctx.r3.u64 = ~0ull;
        return;
    }
    __imp__sub_826BBDC0(ctx, base);
}

// A movie clip's label (r3 the clip, r4 the label, r5 0) - the room list's player plates (0x826BE5F8; r22 the row).
extern "C" PPC_FUNC(__imp__sub_8256D5C8);
PPC_FUNC(sub_8256D5C8)
{
    if (uint32_t(ctx.lr) == 0x826BE5F8u) online2v2::PairPlate(ctx, base);
    __imp__sub_8256D5C8(ctx, base);
}

// The room host's 16-second rule (sub_826BF870 at 960 frames of the READY phase).
extern "C" PPC_FUNC(__imp__sub_826BD2D0);
PPC_FUNC(sub_826BD2D0)
{
    if (online2v2::PairMatchRoom(base))
    {
        online2v2::PairTimer(base);
        return;
    }
    __imp__sub_826BD2D0(ctx, base);
}

// The room host's step after a match (r3 the control; sub_826BD630 once every member is back): the match's result -> the
// win streaks (sub_826BBD08) and the order (sub_826BCE58). After a start whose PAIR PLAY screen was called off there was no
// match: what the game does with no result (the result taken, streaks as they are, the order closed up) - the sync engine
// may still hold the last match's result, which must not count twice.
extern "C" PPC_FUNC(__imp__sub_826BD180);
PPC_FUNC(sub_826BD180)
{
    // a straight start that came to nothing after its count (AfterMatchKeepOrder): the result counted already
    const bool counted = !online2v2::g_keepOrder && online2v2::g_resultCounted.exchange(false);
    if (counted || (online2v2::g_startCancelled.exchange(false) && online2v2::PairMatchRoom(base)))
    {
        PPCContext c = ctx;
        sub_82274750(c, base);
        c.r4.u64 = uint64_t(int64_t(-2));
        sub_8266C600(c, base);
        c = ctx;
        c.r4.u64 = 2;
        sub_826BBD08(c, base);
        c = ctx;
        c.r4.u64 = uint64_t(int64_t(-1));
        sub_826BCE58(c, base);
        if (c.r3.u8)
        {
            sub_82265948(c, base);
            sub_826B81F8(c, base);
            c.r4.u64 = 1;
            sub_826C4A38(c, base);
        }
        std::fprintf(stderr, "[pair-room] after a called-off start: no result (%s) - the order stays\n",
                     counted ? "the last match counted before it" : "its PAIR PLAY screen was called off");
        return;
    }
    online2v2::AfterMatchBefore(ctx, base);    // the result and the streaks as the step finds them (MatchStreaks)
    __imp__sub_826BD180(ctx, base);
}

// The room host's new order (r4 the mode) - after a match (sub_826BD180, 0x826BD2AC; r3 the room control): the streaks
// (online2v2::MatchStreaks) for a match whose order was kept, a PAIR PLAY match, or one whose second fighter held a
// streak (an order a straight start kept), then the order - kept, the teams', or the game's with the mode the streaks
// gave.
extern "C" PPC_FUNC(__imp__sub_826BCE58);
PPC_FUNC(sub_826BCE58)
{
    if (uint32_t(ctx.lr) == 0x826BD2ACu &&
        (online2v2::g_keepOrder || online2v2::PairMatchRoom(base) || online2v2::SecondHeldStreak(base)))
    {
        online2v2::MatchOutcome o;
        const int mode = online2v2::MatchStreaks(ctx, base, ctx.r3.u32, int32_t(ctx.r4.u32), o);
        if (online2v2::g_keepOrder)
        {
            ctx.r3.u64 = 0;                     // the order as it is: nothing changed to send
            return;
        }
        if (online2v2::PairAfterMatch(base, o, mode))
        {
            ctx.r3.u64 = 1;
            return;
        }
        ctx.r4.u64 = uint64_t(int64_t(mode));
    }
    online2v2::g_streakBefore.valid = false;
    __imp__sub_826BCE58(ctx, base);
}

// OnlineInRoom::state_battle_start (r3 its Impl; every frame while it is the state - the game's own runs once and moves on).
extern "C" PPC_FUNC(__imp__sub_825FB650);
PPC_FUNC(sub_825FB650)
{
    if (online2v2::HoldBattleStart(ctx, base, ctx.r3.u32))
    {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_825FB650(ctx, base);
    online2v2::RestoreSidePref(base);
}

// The room data's member record (r3 D, r4 the member: D + 20 + 108 * member - a leaf). From 0x826C2A18: the room host's
// copy of a member's entry (message 513; the entry's copy at the caller's r1 + 432) over that record
// (online2v2::HostKeepsStreak).
extern "C" PPC_FUNC(__imp__sub_826C4858);
PPC_FUNC(sub_826C4858)
{
    const uint32_t data = ctx.r3.u32, member = ctx.r4.u32;
    __imp__sub_826C4858(ctx, base);
    if (uint32_t(ctx.lr) == 0x826C2A1Cu) online2v2::HostKeepsStreak(base, data, member, ctx.r3.u32, ctx.r1.u32 + 432);
}

// The fight HUD's streak label (r3 the gauge, r4 the side, r5 the count): from sub_823D6980's room-match call (0x823D6A74).
extern "C" PPC_FUNC(__imp__sub_823D5CD0);
PPC_FUNC(sub_823D5CD0)
{
    if (uint32_t(ctx.lr) == 0x823D6A78u) online2v2::StreakLabelArgs(ctx);
    __imp__sub_823D5CD0(ctx, base);
}

// The VS screen's per-side streak (r3 its object; from sub_825B8910).
extern "C" PPC_FUNC(__imp__sub_82654AE0);
PPC_FUNC(sub_82654AE0)
{
    uint32_t S = 0, n = 0;
    uint8_t side = 0;
    const bool swapped = online2v2::StreakLabelSwap(ctx, base, S, side, n);
    __imp__sub_82654AE0(ctx, base);
    if (!swapped) return;
    GWrite8(base, S + 754, side);
    GWrite32(base, S + 756, n);
}
