// The end menu after a match in Player Match and PAIR PLAY rooms (online_2v2.h; port/ONLINE_2V2_PLAN.md "Start and rematch").
//
// After a match in a room every member of which played it (nobody waits for a place - otherwise the room's own rotation
// runs: back to the room as after any Player Match), the players get the ranked match's end menu with the room's choices
// (menu_doc.cpp AddRematchFrames): REMATCH, STAGE SELECT, CHARACTER SELECT (PAIR PLAY: CHANGE TEAMS), RETURN TO ROOM and
// LEAVE. Every choice goes to the whole room; the room host decides once every player has chosen (the larger change wins, no
// choice in 15 s - counted down in the help bar - counts as RETURN TO ROOM, a player without the menu makes it RETURN TO ROOM for all) and every player
// carries out the same decision: REMATCH, STAGE SELECT and CHARACTER SELECT start the next match straight from the end
// menu, as other Tekken games do (online_straight.cpp: the fight again in the same GameMode, or character select - for
// STAGE SELECT with the characters kept); RETURN TO ROOM goes back to the room. LEAVE (the user, 2026-11-09: "an option in
// the rematch menu for players to leave the session") is decided at once: RETURN TO ROOM for everyone, and the leaver's PC
// leaves the session from the room the way the room's own leave confirmation's YES does (RoomMain) - the room host's LEAVE
// closes the session (the game has no host migration): its guests leave first, then it.
// With the decision goes the next match's stage chooser (the sync engine's game host, room order position 0 / 1): the
// player who lost the last match chooses (the game's own rule - its room puts the loser second, the chooser; the straight
// paths keep the order, so the host passes it on: online_stage_chooser.cpp).
#include "online_2v2.h"
#undef PlaySound                               // windows.h (mmsystem): menu_host's is meant
#include "display_settings.h"
#include "menu_doc.h"
#include "menu_host.h"
#include "profiles.h"
#include "xlive.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

bool XSessionLocalHosts();                     // xsession.cpp: this game hosts the room
uint64_t XSessionCurrentId();                  // xsession.cpp: the room session (0: not in a room)

extern "C" PPC_FUNC(__imp__sub_8266C5E8);
extern "C" PPC_FUNC(__imp__sub_825D7C60);
extern "C" PPC_FUNC(__imp__sub_8256B638);
PPC_EXTERN_FUNC(sub_825B9188);   // the error slots
PPC_EXTERN_FUNC(sub_8266A3D0);   // ... cleared (r3 the slots)

namespace rematch {
namespace {

using online2v2::DiagMs;

constexpr uint32_t kOnline = 0x82E6B558u;      // the online manager (sub_82265948): +328 the room data, +344 the room's control
constexpr unsigned long long kChoiceGuardMs = 800;   // the menu's first moment: an A is the fight's, not a choice
// Every PC's own time to choose, counted down in the help bar ("(15)" as the PAIR PLAY screen counts): a player who has not
// chosen by then (away from the controller) chooses RETURN TO ROOM - the room's 16 s rule then gives a waiting player the
// place (the user, 2026-11-09: "having a counter in rematch menu will be good to have for afk players"; it was 20 s, unseen).
constexpr unsigned long long kChooseMs = 15000;
// The room host's own limit after the first ballot: every PC chooses for itself after kChooseMs, the menus open a few seconds
// apart (a partner's ~5 s before the fighters'), so this only decides for a player who is gone.
constexpr unsigned long long kHostDecideMs = 25000;
constexpr uint32_t kG = 0x82E6C5B8u + 7488u;    // the sync engine's net game state G (NG +7488): +4 the match's result

// Ms from `t` to `now`, 0 when `t` is later (a time another thread stamped after `now` was read, or one stamped since in
// this frame - an unsigned difference would wrap and pass every timeout at once).
unsigned long long Since(unsigned long long t, unsigned long long now) { return now > t ? now - t : 0; }

// The choices, by what they do (the menus' rows map to them: FrameRematch5 has no CHANGE TEAMS). The larger wins; LEAVE is
// decided at once (as RETURN TO ROOM for everyone, the leaver then leaving the session).
enum Choice : int { kRematch = 0, kStage = 1, kCharacters = 2, kTeams = 3, kRoom = 4, kLeave = 5 };
const char* const kChoiceName[6] = { "REMATCH", "STAGE SELECT", "CHARACTER SELECT", "CHANGE TEAMS", "RETURN TO ROOM", "LEAVE" };

// ---- the wire (2v2 packets, magic first; LEAVE and the decision's tail since protocol -13)
constexpr uint32_t kVoteMagic = 0x32765256u, kDecisionMagic = 0x32765244u, kInfoMagic = 0x32765249u;
constexpr uint8_t kLeaving = 1, kHostCloses = 2;   // Decision flags
#pragma pack(push, 1)
struct Vote { uint32_t magic; uint64_t session; uint32_t ballot; char name[24]; int8_t choice; int8_t result; };
struct Decision
{
    uint32_t magic;
    uint64_t session;
    uint32_t round;
    int8_t choice;
    uint8_t count;
    uint32_t ballots[6];
    int8_t chooser;                            // the next match's stage chooser: room order position 0 / 1 (-1: the game's)
    uint8_t flags;                             // kLeaving: a player leaves the session; kHostCloses: the room host does
    char leaver[24];
};
struct RoomInfo { uint32_t magic; uint64_t session; uint8_t rematchMenu; };
#pragma pack(pop)

struct Ballot { std::string name; int choice = -1; int result = -2; unsigned long long first = 0, seen = 0; };

std::mutex g_lock;                             // what the network thread and the game's main thread share
uint64_t g_session = 0;
bool g_hostMenuOn = false;                     // a guest: the room host's REMATCH MENU (its RoomInfo, every second)
std::map<uint32_t, Ballot> g_ballots;          // every ballot heard (by its number), this PC's too
int g_decision = -1;                           // the room host's decision for this PC's ballot (-1: none yet)
int g_decisionChooser = -1;                    // ... its stage chooser (room order position; -1 the game's)
uint8_t g_decisionFlags = 0;                   // ... kLeaving / kHostCloses
std::string g_decisionLeaver;                  // ... who leaves

// the game's main thread
bool g_played = false;                         // this PC plays the room's match (its battle start's character select, or
                                               // the match restarted from the end menu)
std::vector<std::string> g_matchPlayers;       // the match's players (their room names), from the room's battle start
// the room host
std::set<uint32_t> g_decided;                  // ballots decided
Decision g_decisionPacket{};
int g_decisionSends = 0;
unsigned long long g_decisionSentAt = 0;
unsigned long long g_leaveDecidedAt = 0;       // a LEAVE decided (its packet's flags / leaver stand): the menu round's later
                                               // ballots get the same at once (kLeaveStickyMs)
constexpr unsigned long long kLeaveStickyMs = 20000;
// a PC leaving the session once it is back in the room (RoomMain): the leaver, or every PC when the room host closes it
struct PendingLeave { uint64_t session = 0; bool host = false; unsigned long long at = 0; bool started = false; };
PendingLeave g_leave;
// the menu
std::atomic<bool> g_menuWanted{ false };       // the result's branch went the menu's way: its state runs here
uint32_t g_menuImpl = 0;                       // GameMode (its first frame here: 0)
int g_rows = 4;
uint32_t g_ballot = 0;
int g_myChoice = -1;
unsigned long long g_menuAt = 0, g_voteSentAt = 0, g_decidedAt = 0, g_leaveChosenAt = 0;
bool g_left = false;                           // the menu is done with (GameMode handed on, or the next match starting)
bool g_straight = false;                       // the next match starts from the end menu (online_straight.cpp)
std::string g_shownValue[6];
bool g_decisionShown = false;
std::string g_helpShown;                       // the help bar's last text before the decision (with its count)
int g_cursorShown = -1;                        // the row last logged as focused

bool InRoom(uint8_t* base) { return GRead32(base, kOnline + 344) != 0; }
uint32_t RoomData(uint8_t* base) { return InRoom(base) ? GRead32(base, kOnline + 328) : 0; }

std::string MemberName(uint8_t* base, uint32_t data, int i)
{
    std::string s;
    for (uint32_t k = 0; k < 16; ++k)
    {
        const char c = char(GRead8(base, data + 20 + 108 * uint32_t(i) + 30 + k));
        if (!c) break;
        s += c;
    }
    return s;
}

// Room names are gamertags: 15 characters (a profile's name may be longer - its first 15 are the room's).
bool SameName(const std::string& a, const std::string& b) { return !a.empty() && a.substr(0, 15) == b.substr(0, 15); }

// The room's members now (their names).
std::vector<std::string> RoomMembers(uint8_t* base)
{
    std::vector<std::string> out;
    const uint32_t data = RoomData(base);
    for (int i = 0; i < 6 && data; ++i)
        if (GRead8(base, data + 20 + 108 * uint32_t(i) + 4)) out.push_back(MemberName(base, data, i));
    return out;
}

std::string MyName() { return profiles::Active().name; }

bool MenuOn()
{
    if (!menu_doc::MenuRematchFrames()) return false;
    if (XSessionLocalHosts()) return display_settings::GetRematchMenu();
    std::lock_guard l(g_lock);
    return g_hostMenuOn;
}

void SendAll(const void* data, size_t size)
{
    uint64_t peers[8];
    const size_t n = XSocketRoomPeers(peers, 8, 0);
    for (size_t i = 0; i < n; ++i) XSocketSend2v2(peers[i], data, size);
}

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

// ---- the menu
// PAIR PLAY's menu has CHANGE TEAMS - a room that plays PAIR PLAY matches (three or more: a PAIR PLAY room of two plays
// the game's own 1 on 1, where CHANGE TEAMS has nothing to change)
int RowsHere() { return g_base && online2v2::PairMatchRoom(g_base) ? 6 : 5; }
// FrameRematch6: the choices in order; FrameRematch5: no CHANGE TEAMS (row 3 RETURN TO ROOM, row 4 LEAVE).
int ChoiceOfRowIn(int rows, int row) { return rows == 6 || row < 3 ? row : row + 1; }
int ChoiceOfRow(int row) { return ChoiceOfRowIn(g_rows, row); }
int RowOfChoice(int choice) { return g_rows == 6 || choice < 3 ? choice : choice == kTeams ? -1 : choice - 1; }
std::string MenuName() { return "MenuRematch" + std::to_string(g_rows); }
// A row's help: the room host's LEAVE closes the session for everyone.
std::string HelpOf(int choice) { return menu_doc::RematchHelp(choice, XSessionLocalHosts()); }

// The menu as a fresh one, before its frame is shown (the frame draws its cursor as it comes up): the cursor on REMATCH
// and the rows' own help and empty value texts - the frame keeps all three from the last time (2026-11-04: it opened on
// RETURN TO ROOM, "4 PLAYERS" and the last decision's words). The widgets are found in frames not shown too.
void ResetMenu(PPCContext& ctx, uint8_t* base, int rows)
{
    const std::string n = std::to_string(rows);
    menu_host::SetCursor(ctx, base, "MenuRematch" + n, 0);
    for (int row = 0; row < rows; ++row)
    {
        menu_host::SetText(ctx, base, "StaticRematch" + n + "_0" + std::to_string(row + 1), HelpOf(ChoiceOfRowIn(rows, row)));
        menu_host::SetText(ctx, base, "StaticRematch" + n + "Value" + std::to_string(row + 1), "");
    }
}

// This PC's result of the match just played, as a fighter: the sync engine's G+4 (0 / 1 the room order's first / second
// won, -1 a draw; sub_82654CE8 -> sub_82668178 at the match's decision). -2: none - a PAIR PLAY partner is the engine's
// spectator (its G+4 may still hold an older match's).
int8_t MyResult()
{
    if (!g_base) return -2;
    const int32_t r = int32_t(GRead32(g_base, kG + 4));
    if (r < -1 || r > 1) return -2;
    if (online2v2::PairMatchRoom(g_base))
    {
        online2v2::PairPlaces p;
        int mine = -1;
        if (!online2v2::PlacesLast(p, mine) || mine < 0 || (p.place[0] != mine && p.place[1] != mine)) return -2;
    }
    return int8_t(r);
}

void SendVote()
{
    Vote v{};
    v.magic = kVoteMagic;
    v.session = XSessionCurrentId();
    v.ballot = g_ballot;
    const std::string name = MyName();
    std::memcpy(v.name, name.data(), (std::min)(name.size(), sizeof(v.name)));
    v.choice = int8_t(g_myChoice);
    v.result = MyResult();
    SendAll(&v, sizeof(v));
    g_voteSentAt = DiagMs();
    std::lock_guard l(g_lock);
    Ballot& b = g_ballots[g_ballot];
    if (!b.first) b.first = g_voteSentAt;
    b.seen = g_voteSentAt;
    b.name = name;
    b.choice = g_myChoice;
    b.result = v.result;
}

void MenuOpen(PPCContext& ctx, uint8_t* base, uint32_t impl)
{
    g_menuImpl = impl;
    g_rows = RowsHere();
    g_cursorShown = -1;
    g_ballot = (uint32_t(DiagMs()) ^ uint32_t(xlive::OnlineXuid(0)) ^ uint32_t(xlive::OnlineXuid(0) >> 32)) | 1u;
    g_myChoice = -1;
    g_menuAt = DiagMs();
    g_decidedAt = 0;
    g_left = g_straight = false;
    g_decisionShown = false;
    g_helpShown.clear();
    for (std::string& s : g_shownValue) s.clear();
    // a leave armed by an earlier menu that ended in a straight start instead is not wanted any more
    if (g_leave.session && !g_leave.started)
        std::fprintf(stderr, "[rematch] an earlier menu's leave dropped (a match was played since)\n");
    if (!g_leave.started) g_leave = PendingLeave{};
    {
        std::lock_guard l(g_lock);
        g_decision = -1;
        g_decisionChooser = -1;
        g_decisionFlags = 0;
        g_decisionLeaver.clear();
    }
    if (menu_host::CurrentFrame(ctx, base) != "FrameRematch" + std::to_string(g_rows))   // not shown by the switch below
    {
        ResetMenu(ctx, base, g_rows);
        menu_host::ShowFrame(ctx, base, "FrameRematch" + std::to_string(g_rows));
    }
    SendVote();
    std::fprintf(stderr, "[rematch] the rematch menu (%d rows, ballot %08X)\n", g_rows, g_ballot);
}

void SetHelp(PPCContext& ctx, uint8_t* base, const std::string& text)
{
    for (int i = 1; i <= g_rows; ++i)
        menu_host::SetText(ctx, base, "StaticRematch" + std::to_string(g_rows) + "_0" + std::to_string(i), text);
}

// The help bar until the decision: each row's own help (once this PC chose: that it waits) and the seconds left to choose,
// as the PAIR PLAY screen counts. The count stops at 0 - this PC's choice is RETURN TO ROOM then (MenuFrame).
void ShowCount(PPCContext& ctx, uint8_t* base, unsigned long long now)
{
    const unsigned long long gone = Since(g_menuAt, now);
    const int left = gone >= kChooseMs ? 0 : int((kChooseMs - gone + 999) / 1000);
    const std::string count = left > 0 ? " (" + std::to_string(left) + ")" : "";
    const std::string key = std::to_string(g_myChoice) + count;
    if (key == g_helpShown) return;
    g_helpShown = key;
    const std::string n = std::to_string(g_rows);
    const std::string chosen = g_myChoice == kLeave ? "Leaving the session..." : "Waiting... the larger change wins.";
    for (int row = 0; row < g_rows; ++row)
        menu_host::SetText(ctx, base, "StaticRematch" + n + "_0" + std::to_string(row + 1),
                           (g_myChoice >= 0 ? chosen : HelpOf(ChoiceOfRow(row))) + (g_myChoice == kLeave ? std::string() : count));
}

// The decision's words in the help bar.
std::string DecisionText(int decision, uint8_t flags, const std::string& leaver)
{
    if (flags & kHostCloses) return XSessionLocalHosts() ? "Closing the session." : "The host closed the session.";
    if (flags & kLeaving)
        return g_myChoice == kLeave || SameName(leaver, MyName()) ? std::string("Leaving the session.")
                                                                : (leaver.empty() ? std::string("A player") : leaver) + " has left the session.";
    if (decision == kRoom && g_myChoice == kLeave && !XSessionLocalHosts()) return "Leaving the session.";   // MenuFrame's `mine`
    return std::string(kChoiceName[decision]) + (decision == kRoom ? "." : " - get ready.");
}

// Every frame of the menu: the room's choices in the value column, this PC's own vote, the decision and what it starts.
void MenuFrame(PPCContext& ctx, uint8_t* base, uint32_t impl)
{
    if (!g_menuImpl) MenuOpen(ctx, base, impl);
    if (g_straight)
    {
        if (!straight::Frame(ctx, base, impl))
        {
            g_straight = false;
            g_left = true;
        }
        return;
    }
    if (g_left) return;
    const unsigned long long now = DiagMs();
    GWrite8(base, impl + 157, 1);
    const std::string frame = "FrameRematch" + std::to_string(g_rows);
    if (menu_host::CurrentFrame(ctx, base) == "FrameRankMatchResult") menu_host::ShowFrame(ctx, base, frame);
    // the menu's first moment takes no input (Chosen): the fight's last presses leave the cursor on REMATCH
    if (Since(g_menuAt, now) < kChoiceGuardMs && menu_host::Cursor(ctx, base, MenuName()) > 0) menu_host::SetCursor(ctx, base, MenuName(), 0);
    if (const int cursor = menu_host::Cursor(ctx, base, MenuName()); cursor != g_cursorShown)
    {
        g_cursorShown = cursor;
        std::fprintf(stderr, "[rematch] cursor on row %d\n", cursor);
    }
    // no choice in time (the count ran out - away from the controller): RETURN TO ROOM
    if (g_myChoice < 0 && Since(g_menuAt, now) >= kChooseMs)
    {
        g_myChoice = kRoom;
        std::fprintf(stderr, "[rematch] no choice in %llu s: RETURN TO ROOM\n", kChooseMs / 1000);
        SendVote();
    }
    int decision, chooser;
    uint8_t flags;
    std::string leaver;
    std::vector<std::pair<int, std::string>> votes;
    {
        std::lock_guard l(g_lock);
        decision = g_decision;
        chooser = g_decisionChooser;
        flags = g_decisionFlags;
        leaver = g_decisionLeaver;
        for (const auto& [ballot, b] : g_ballots)
            if (b.seen + 5000 > g_menuAt && b.choice >= 0) votes.push_back({ b.choice, ballot == g_ballot ? MyName() : b.name });
    }
    if (decision < 0 && Since(g_voteSentAt, now) > 300) SendVote();
    // the value column: who chose each row
    for (int row = 0; row < g_rows; ++row)
    {
        std::string text;
        int n = 0;
        for (const auto& [choice, name] : votes)
            if (RowOfChoice(choice) == row) text += (n++ ? ", " : "") + name;
        if (n > 2 || text.size() > 16) text = std::to_string(n) + " PLAYERS";   // the help line shares the bar
        if (text == g_shownValue[row]) continue;
        g_shownValue[row] = text;
        menu_host::SetText(ctx, base, "StaticRematch" + std::to_string(g_rows) + "Value" + std::to_string(row + 1), text);
    }
    if (decision < 0)
    {
        ShowCount(ctx, base, now);
        // this PC decides for itself - final: a late packet from the room host is ignored then (Receive takes none once
        // g_decision is set), so what is carried out and what was shown can never disagree
        const bool hostGone = Since(g_menuAt, now) > 40000;
        const bool leaveAnyway = !hostGone && g_myChoice == kLeave && Since(g_leaveChosenAt, now) > 3000;
        if (!hostGone && !leaveAnyway) return;
        {
            std::lock_guard l(g_lock);
            if (g_decision < 0)
            {
                g_decision = kRoom;
                g_decisionChooser = -1;
                g_decisionFlags = leaveAnyway ? kLeaving : 0;
                g_decisionLeaver = leaveAnyway ? MyName() : std::string();
            }
            decision = g_decision;
            chooser = g_decisionChooser;
            flags = g_decisionFlags;
            leaver = g_decisionLeaver;
        }
        if (hostGone)
            std::fprintf(stderr, "[rematch] no decision from the room host in 40 s: back to the room\n");
        else
            // LEAVE is decided at once: no answer in 3 s - the room host is gone; this PC leaves anyway (from the room)
            std::fprintf(stderr, "[rematch] LEAVE: no decision from the room host in 3 s - back to the room and out\n");
    }
    if (!g_decisionShown)
    {
        g_decisionShown = true;
        g_decidedAt = now;
        SetHelp(ctx, base, DecisionText(decision, flags, leaver));
        std::fprintf(stderr, "[rematch] decided: %s%s%s%s (stage chooser: order position %d)\n", kChoiceName[decision],
                     flags & kHostCloses ? " - the room host closes the session" : "", flags & kLeaving ? " - leaving: " : "",
                     flags & kLeaving ? leaver.c_str() : "", chooser);
        // leaving the session: this PC's (it chose LEAVE), or every PC's when the room host closes it - once back in the
        // room (RoomMain). A straight decision that came first (the host decided before it heard this PC's LEAVE) is
        // carried out instead: the leave is dropped (the next end menu offers it again). A plain RETURN TO ROOM that came
        // first takes a guest to the room all the same: its LEAVE holds (a room host leaves only with kHostCloses - its
        // guests must hear the session close).
        const bool mine = g_myChoice == kLeave && decision == kRoom && ((flags & kLeaving) || !XSessionLocalHosts());
        if (decision == kRoom && (mine || (flags & kHostCloses)))
        {
            g_leave = PendingLeave{};
            g_leave.session = XSessionCurrentId();
            g_leave.host = XSessionLocalHosts();
            g_leave.at = now;
            std::fprintf(stderr, "[rematch] this PC leaves the session once it is back in the room%s\n",
                         g_leave.host ? " (the room host: after its guests)" : "");
        }
        else if (g_myChoice == kLeave)
            std::fprintf(stderr, "[rematch] WARNING: this PC chose LEAVE but %s was decided - carried out, the leave dropped\n", kChoiceName[decision]);
    }
    if (Since(g_decidedAt, now) <= 1200) return;
    if (decision == kRematch || decision == kStage || decision == kCharacters || decision == kTeams)
    {
        if (!g_leave.started) g_leave = PendingLeave{};   // never a leave past a match it did not stop
        g_straight = true;
        std::fprintf(stderr, "[rematch] %s: the next match starts from here\n", kChoiceName[decision]);
        straight::Begin(decision, impl, chooser);
        if (!straight::Frame(ctx, base, impl))
        {
            g_straight = false;
            g_left = true;
        }
        return;
    }
    // RETURN TO ROOM: the room's own way
    g_left = true;
    GWrite8(base, impl + 156, 1);
    AskMode(ctx, base, impl, 35, 1);
    std::fprintf(stderr, "[rematch] back to the room\n");
}

// The room host: decides once every player of the match (every member of the room: the menu shows only while nobody waits)
// has chosen, or kHostDecideMs after the first ballot - the larger change wins; a player without a choice (no menu, gone) makes it
// RETURN TO ROOM. One decision for all of them: every player carries out the same.
void HostDecide(uint8_t* base)
{
    const unsigned long long now = DiagMs();
    struct Open { uint32_t ballot; int choice; std::string name; int result; };
    std::vector<Open> open;
    unsigned long long first = ~0ull;
    bool resend = false;
    {
        std::lock_guard l(g_lock);
        for (auto it = g_ballots.begin(); it != g_ballots.end();)
        {
            if (Since(it->second.seen, now) > 60000) { it = g_ballots.erase(it); continue; }
            if (!g_decided.count(it->first) && Since(it->second.seen, now) < 5000)
            {
                open.push_back({ it->first, it->second.choice, it->second.name, it->second.result });
                first = (std::min)(first, it->second.first);
            }
            // a decided ballot still voting after the decision's last copy went out never heard it (every copy lost):
            // the decision once more (the PC stops voting once it has it; a second copy changes nothing there)
            else if (g_decided.count(it->first) && g_decisionSends == 0 && it->second.seen > g_decisionSentAt + 300 &&
                     Since(it->second.seen, now) < 1000 &&
                     std::find(g_decisionPacket.ballots, g_decisionPacket.ballots + (std::min)(int(g_decisionPacket.count), 6), it->first) !=
                         g_decisionPacket.ballots + (std::min)(int(g_decisionPacket.count), 6))
                resend = true;
            ++it;
        }
    }
    if (resend)
    {
        g_decisionSends = 1;
        std::fprintf(stderr, "[rematch] a player still votes after the decision - sent again\n");
    }
    if (open.empty()) return;
    // LEAVE: decided at once - RETURN TO ROOM for everyone (kRoom is the largest change: nothing straight can start without
    // the leaver), and the same for the menu round's ballots that come later (a partner's menu opens seconds before the
    // fighters')
    const auto leaving = std::find_if(open.begin(), open.end(), [](const Open& o) { return o.choice == kLeave; });
    const bool sticky = g_leaveDecidedAt && Since(g_leaveDecidedAt, now) < kLeaveStickyMs && g_decisionPacket.session == XSessionCurrentId();
    if (leaving != open.end() || sticky)
    {
        Decision d = sticky ? g_decisionPacket : Decision{};
        if (!sticky)
        {
            d.magic = kDecisionMagic;
            d.session = XSessionCurrentId();
            d.round = uint32_t(now) | 1u;
            d.choice = int8_t(kRoom);
            d.chooser = -1;
            d.flags = kLeaving;
            std::memcpy(d.leaver, leaving->name.data(), (std::min)(leaving->name.size(), sizeof(d.leaver) - 1));
        }
        // the room host's own LEAVE closes the session (the game has no host migration)
        if (std::any_of(open.begin(), open.end(), [](const Open& o) { return o.choice == kLeave && o.ballot == g_ballot; })) d.flags |= kHostCloses;
        {
            std::lock_guard l(g_lock);
            for (const Open& o : open)
            {
                g_decided.insert(o.ballot);
                if (d.count < 6 && std::find(d.ballots, d.ballots + d.count, o.ballot) == d.ballots + d.count) d.ballots[d.count++] = o.ballot;
            }
            for (uint8_t i = 0; i < d.count; ++i)
                if (d.ballots[i] == g_ballot && g_menuImpl && g_decision < 0)
                {
                    g_decision = d.choice;
                    g_decisionChooser = d.chooser;
                    g_decisionFlags = d.flags;
                    g_decisionLeaver.assign(d.leaver, strnlen(d.leaver, sizeof(d.leaver)));
                }
        }
        g_decisionPacket = d;
        g_decisionSends = 6;
        g_decisionSentAt = 0;
        if (!sticky) g_leaveDecidedAt = now;
        std::fprintf(stderr, "[rematch] the room host decides: RETURN TO ROOM - %s leaves%s (%zu ballots%s)\n",
                     std::string(d.leaver, strnlen(d.leaver, sizeof(d.leaver))).c_str(), d.flags & kHostCloses ? ", the room host closes the session" : "",
                     open.size(), sticky ? ", a later ballot of the round" : "");
        return;
    }
    const std::vector<std::string> members = RoomMembers(base);
    bool all = !members.empty();
    std::string missing;
    for (const std::string& m : members)
    {
        const bool chose = std::any_of(open.begin(), open.end(), [&](const Open& o) { return o.choice >= 0 && SameName(o.name, m); });
        if (!chose)
        {
            all = false;
            missing += (missing.empty() ? "" : ", ") + m;
        }
    }
    if (!(all && Since(first, now) >= 500) && Since(first, now) < kHostDecideMs) return;
    int choice = kRematch;
    for (const Open& o : open) choice = (std::max)(choice, o.choice < 0 ? int(kRoom) : o.choice);
    const char* why = "";
    if (!all) choice = kRoom, why = " - not every player chose";
    else if (choice == kTeams && !online2v2::PairMatchRoom(base)) choice = kRoom;
    else if (choice != kRoom && !NobodyWaits(base)) choice = kRoom, why = " - someone waits in the room";
    // the next match's stage chooser (room order position): REMATCH keeps the last match's (no stage is chosen), STAGE /
    // CHARACTER SELECT give it to the player (PAIR PLAY: the team) who lost - the room order's first / second won by the
    // fighters' results; a draw passes it on; unknown keeps it
    const int last = stagechooser::LastHostPos();
    int result = -2;
    bool differ = false;
    for (const Open& o : open)
        if (o.result >= -1 && o.result <= 1)
        {
            if (result != -2 && result != o.result) differ = true;
            result = o.result;
        }
    if (differ) result = -2;
    int chooser = -1;
    const char* rule = "";
    if (choice == kRematch) chooser = last, rule = "kept (REMATCH)";
    else if (choice == kStage || choice == kCharacters)
    {
        if (result == 0) chooser = 1, rule = "the loser chooses";
        else if (result == 1) chooser = 0, rule = "the loser chooses";
        else if (result == -1) chooser = 1 - last, rule = "a draw: passed on";
        else chooser = last, rule = differ ? "the results differ: kept" : "no result: kept";
    }
    Decision d{};
    d.magic = kDecisionMagic;
    d.session = XSessionCurrentId();
    d.round = uint32_t(now) | 1u;
    d.choice = int8_t(choice);
    d.chooser = int8_t(chooser);
    {
        std::lock_guard l(g_lock);
        for (const Open& o : open)
        {
            g_decided.insert(o.ballot);
            if (d.count < 6) d.ballots[d.count++] = o.ballot;
        }
    }
    g_decisionPacket = d;
    g_decisionSends = 6;
    g_decisionSentAt = 0;
    g_leaveDecidedAt = 0;
    {
        std::lock_guard l(g_lock);
        for (uint8_t i = 0; i < d.count; ++i)
            if (d.ballots[i] == g_ballot && g_menuImpl)
            {
                g_decision = choice;
                g_decisionChooser = chooser;
                g_decisionFlags = 0;
                g_decisionLeaver.clear();
            }
    }
    std::fprintf(stderr, "[rematch] the room host decides: %s (%zu ballots, %zu players%s%s)%s", kChoiceName[choice], open.size(),
                 members.size(), missing.empty() ? "" : "; no choice from ", missing.c_str(), why);
    if (chooser >= 0) std::fprintf(stderr, "; stage chooser: order position %d (%s; result %d, last %d)", chooser, rule, result, last);
    std::fprintf(stderr, "\n");
}

} // namespace

// Every member of the room played its last match (the room's battle start's players are all there, nobody else): the end
// menu's straight paths keep them together; anyone else waiting gets the room's own rotation.
bool NobodyWaits(uint8_t* base)
{
    const std::vector<std::string> members = RoomMembers(base);
    if (members.empty() || members.size() != g_matchPlayers.size()) return false;
    for (const std::string& m : members)
        if (std::none_of(g_matchPlayers.begin(), g_matchPlayers.end(), [&](const std::string& p) { return SameName(p, m); })) return false;
    return true;
}

int FightersResult()
{
    std::lock_guard l(g_lock);
    int result = -2;
    for (const auto& [ballot, b] : g_ballots)
    {
        if (b.seen + 5000 <= g_menuAt || b.result < -1 || b.result > 1) continue;   // not this menu's, or no fighter's
        if (result != -2 && result != b.result)
        {
            std::fprintf(stderr, "[rematch] WARNING: the fighters tell different results (%d, %d) - none taken\n", result, b.result);
            return -2;
        }
        result = b.result;
    }
    return result;
}

// ---- the network thread
bool Receive(uint64_t from, const uint8_t* data, size_t size)
{
    (void)from;
    if (size < 4) return false;
    uint32_t magic;
    std::memcpy(&magic, data, 4);
    const uint64_t session = XSessionCurrentId();
    if (magic == kVoteMagic && size >= sizeof(Vote))
    {
        Vote v;
        std::memcpy(&v, data, sizeof(v));
        if (v.session != session || v.choice > kLeave) return true;
        std::lock_guard l(g_lock);
        Ballot& b = g_ballots[v.ballot];
        const int was = b.choice;
        if (!b.first) b.first = DiagMs();
        b.seen = DiagMs();
        b.name.assign(v.name, strnlen(v.name, sizeof(v.name)));
        b.choice = v.choice;
        b.result = v.result;
        if (b.choice != was && b.choice >= 0) std::fprintf(stderr, "[rematch] %s chose %s\n", b.name.c_str(), kChoiceName[b.choice]);
        return true;
    }
    if (magic == kDecisionMagic && size >= sizeof(Decision))
    {
        Decision d;
        std::memcpy(&d, data, sizeof(d));
        if (d.session != session || d.choice < 0 || d.choice > kRoom || d.chooser < -1 || d.chooser > 1) return true;
        std::lock_guard l(g_lock);
        for (uint8_t i = 0; i < d.count && i < 6; ++i)
        {
            g_decided.insert(d.ballots[i]);
            if (d.ballots[i] == g_ballot && g_decision < 0)
            {
                g_decision = d.choice;
                g_decisionChooser = d.chooser;
                g_decisionFlags = d.flags;
                g_decisionLeaver.assign(d.leaver, strnlen(d.leaver, sizeof(d.leaver)));
            }
        }
        return true;
    }
    if (magic == kInfoMagic && size >= sizeof(RoomInfo))
    {
        RoomInfo r;
        std::memcpy(&r, data, sizeof(r));
        if (r.session != session) return true;
        std::lock_guard l(g_lock);
        if (g_hostMenuOn != (r.rematchMenu != 0))
            std::fprintf(stderr, "[rematch] the room host's rematch menu: %s\n", r.rematchMenu ? "on" : "off");
        g_hostMenuOn = r.rematchMenu != 0;
        return true;
    }
    return false;
}

// ---- the game's main thread
// The mode manager is asked for a mode (online_2v2_select.cpp's sub_825ED560 hook): the room's battle start - and the end
// menu's CHARACTER / STAGE SELECT (online_straight.cpp) - ask a fighter and a PAIR PLAY partner for CHARASELECT (8, flags
// 0x1000 | side): this PC plays the match.
uint32_t ModeAsked(uint8_t* base, uint32_t mode, uint32_t& flags)
{
    (void)base;
    if (mode == 8 && (flags & 0x1000u)) g_played = true;
    return mode;
}

// The match restarted from the end menu (online_straight.cpp REMATCH): this PC plays it, the menu is done with.
void Replaying()
{
    g_played = true;
    g_menuWanted = false;
    g_menuImpl = 0;
}

// A game state machine entered a state (online_2v2_team.cpp's sub_825FE600 hook).
void NoteState(const std::string& name)
{
    if (name.rfind("&GameMode::", 0) == 0 && name != "&GameMode::Impl::state_gamemode_online_result") g_menuWanted = false;
    // the next match's fight began: its result will be a new one (the room host's count at the end menu - online_pair_room.cpp
    // AfterMatchKeepOrder - stood until here, so a start that came to nothing never counted the last match twice)
    if (name == "&GameMode::Impl::state_game_battle") online2v2::AfterMatchGoesOn();
    else if (name.rfind("&OnlineInRoom::", 0) == 0) g_menuWanted = false;
    // the match's players: the room order's first two (a PAIR PLAY room's marked four)
    if (name == "&OnlineInRoom::Impl::state_battle_start" && g_base)
    {
        g_matchPlayers.clear();
        const uint32_t data = RoomData(g_base);
        if (!data) return;
        const bool pair = online2v2::PairMatchRoom(g_base);
        for (int k = 0; k < (pair ? 4 : 2); ++k)
        {
            const int i = pair ? int16_t(GRead16(g_base, data + 676 + 2 * uint32_t(k))) : int32_t(GRead32(g_base, data + 668 + 4 * uint32_t(k)));
            if (i >= 0 && i < 6 && GRead8(g_base, data + 20 + 108 * uint32_t(i) + 4)) g_matchPlayers.push_back(MemberName(g_base, data, i));
        }
        std::string list;
        for (const std::string& p : g_matchPlayers) list += (list.empty() ? "" : ", ") + p;
        std::fprintf(stderr, "[rematch] the match's players: %s\n", list.c_str());
    }
}

// The room's REMATCH MENU row (menu_ext.cpp): on / off.
void RowToggled(PPCContext& ctx, uint8_t* base)
{
    if (!XSessionLocalHosts()) return;
    display_settings::SetRematchMenu(!display_settings::GetRematchMenu());
    menu_host::PlaySound(ctx, base, menu_host::SoundDecide);
}

// The rematch menu's A (menu_ext.cpp, its OnEnter): the focused row is this PC's choice (it can change it until the
// decision).
void Chosen(PPCContext& ctx, uint8_t* base)
{
    if (!g_menuImpl || g_left || g_straight) return;
    {
        std::lock_guard l(g_lock);
        if (g_decision >= 0) return;
    }
    // the fight's last presses (attacks mashed through the KO and the win pose) never choose: the menu takes A once it
    // has been up a moment (2026-11-07 rm12a: a stray A the fight's input left chose STAGE SELECT as the menu opened)
    if (Since(g_menuAt, DiagMs()) < kChoiceGuardMs)
    {
        std::fprintf(stderr, "[rematch] an A %llu ms after the menu opened - not a choice yet\n", Since(g_menuAt, DiagMs()));
        return;
    }
    const int row = menu_host::Cursor(ctx, base, MenuName());
    if (row < 0 || row >= g_rows || g_myChoice == kLeave) return;   // LEAVE is final (decided at once)
    g_myChoice = ChoiceOfRow(row);
    if (g_myChoice == kLeave) g_leaveChosenAt = DiagMs();
    menu_host::PlaySound(ctx, base, menu_host::SoundDecide);
    std::fprintf(stderr, "[rematch] this PC chose %s\n", kChoiceName[g_myChoice]);
    SendVote();
}

// The room's main state (sub_826C03E0's hook, online_2v2_places.cpp; r3 its control C): a PC leaving the session (LEAVE) does
// what the room's own leave confirmation's YES does (sub_826BFB38 0x826BFB80..0x826BFBBC): the error slots cleared
// (sub_8266A3D0), the leave chain's first state (sub_826BE9F8, which sets C+66 and goes on to the session's leave and the
// way to the Online Mode menu - mode 29) with C+68 = 1 (a player's own leave), run once. The room host closing the session
// waits for its guests to leave first (the room shows only it) or 10 s. True: the main state is not to run this frame.
bool RoomMain(PPCContext& ctx, uint8_t* base, uint32_t control)
{
    if (!g_leave.session || g_leave.started) return false;
    if (g_leave.session != XSessionCurrentId() || !control)
    {
        std::fprintf(stderr, "[rematch] the leave dropped: %s\n", control ? "another session" : "no room control");
        g_leave = PendingLeave{};
        return false;
    }
    if (GRead8(base, control + 66)) return false;   // a leave of the game's own runs already
    const unsigned long long now = DiagMs();
    if (g_leave.host && RoomMembers(base).size() > 1 && Since(g_leave.at, now) < 10000) return false;
    if (control != GRead32(base, kOnline + 344))
        std::fprintf(stderr, "[rematch] WARNING: the room's main state's control %08X is not the online manager's %08X\n", control,
                     GRead32(base, kOnline + 344));
    g_leave.started = true;
    PPCContext c = ctx;
    sub_825B9188(c, base);
    sub_8266A3D0(c, base);
    constexpr uint32_t kLeaveState = 0x826BE9F8u;
    GWrite32(base, control + 40, kLeaveState);
    GWrite32(base, control + 44, 0);
    GWrite8(base, control + 68, 1);
    std::fprintf(stderr, "[rematch] leaving the session from the room (%s, %zu members, %llu ms after the decision)\n",
                 g_leave.host ? "the room host closes it" : "a guest", RoomMembers(base).size(), Since(g_leave.at, now));
    c.r3.u64 = control;
    PPC_LOOKUP_FUNC(base, kLeaveState)(c, base);
    return true;
}

void Frame(PPCContext& ctx, uint8_t* base, const std::string& frame)
{
    const unsigned long long now = DiagMs();
    const uint64_t session = XSessionCurrentId();
    if (session != g_session)
    {
        g_session = session;
        {
            std::lock_guard l(g_lock);
            g_hostMenuOn = false;
            g_ballots.clear();
            g_decided.clear();
        }
        g_played = false;
        g_matchPlayers.clear();
        g_decisionSends = 0;
        g_leaveDecidedAt = 0;
        if (g_leave.session && g_leave.session != session)
        {
            std::fprintf(stderr, "[rematch] the session changed%s\n", g_leave.started ? " - left" : ": the leave dropped");
            g_leave = PendingLeave{};
        }
    }
    if (!session) return;
    const bool host = XSessionLocalHosts();
    // the room host: its REMATCH MENU every second; the decision a few times
    if (host)
    {
        static unsigned long long infoAt = 0;
        if (Since(infoAt, now) >= 1000)
        {
            infoAt = now;
            const RoomInfo r{ kInfoMagic, session, uint8_t(MenuOn() ? 1 : 0) };
            SendAll(&r, sizeof(r));
        }
        if (g_decisionSends > 0 && Since(g_decisionSentAt, now) >= 150)
        {
            g_decisionSentAt = now;
            --g_decisionSends;
            SendAll(&g_decisionPacket, sizeof(g_decisionPacket));
        }
        HostDecide(base);
    }
    // the room's frame: the room host's REMATCH MENU row and its value
    if (frame != "FrameOnlinePlayerMatchRoom") return;
    static uint64_t rowRoom = 0;
    static int shownRow = -1, shownValue = -1;
    const bool row = host && menu_doc::MenuRematchFrames();
    const int value = display_settings::GetRematchMenu() ? 1 : 0;
    if (rowRoom != session || int(row) != shownRow)
    {
        menu_host::SetVisible(ctx, base, "MenuItemPlayerMatch06-Rematch", row);
        menu_host::SetFocusable(ctx, base, "MenuItemPlayerMatch06-Rematch", row);
        shownRow = int(row);
        shownValue = -1;
    }
    if (row && (rowRoom != session || value != shownValue))
    {
        menu_host::SetText(ctx, base, "StaticPlayerMatchRoomValue06", value ? "ON" : "OFF");
        shownValue = value;
    }
    rowRoom = session;
}

} // namespace rematch

// The match's kind (sub_8266C5E8: 0 / 1 ranked, 2 Player Match, 3 lobby - MEASURED: a Player Match is 2), asked by
// GameMode's online result at its branch (0x825D8E28): a match played in a room with the rematch menu on, every member of
// which played it, goes the ranked match's way - its end menu and state_gamemode_online_result, which the rematch code runs
// (below).
PPC_FUNC(sub_8266C5E8)
{
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_8266C5E8(ctx, base);
    if (lr != 0x825D8E28u || ctx.r3.u32 != 2 || !rematch::InRoom(base)) return;
    const bool played = std::exchange(rematch::g_played, false);
    const bool menuOn = played && rematch::MenuOn();
    const bool nobodyWaits = menuOn && rematch::NobodyWaits(base);
    if (!nobodyWaits)
    {
        std::fprintf(stderr, "[rematch] the match's end: %s\n", !played ? "this PC watched it" : !menuOn ? "the rematch menu is off"
                     : "the room has other members (or one of the match's left) - the room's own rotation");
        return;
    }
    ctx.r3.u64 = 0;
    rematch::g_menuImpl = 0;
    rematch::g_menuWanted = true;
}

// The menu manager's frame switch (sub_8256B638: r3 the manager, r4 the frame's name; looked up at once), from the ranked
// branch above (0x825D8E90, FrameRankMatchResult): the rematch menu's frame instead. The ranked menu was up for a frame
// before the rematch state's first frame replaced it and took presses - a DOWN there moved the cursor the rematch menu
// opened on (the room host voted REMATCH for RETURN TO ROOM, 2026-11-04), and its A would have been SEARCH AGAIN's.
PPC_FUNC(sub_8256B638)
{
    if (uint32_t(ctx.lr) == 0x825D8E90u && rematch::g_menuWanted.load())
    {
        const int rows = rematch::RowsHere();
        rematch::ResetMenu(ctx, base, rows);
        menu_host::ShowFrame(ctx, base, "FrameRematch" + std::to_string(rows));
        return;
    }
    __imp__sub_8256B638(ctx, base);
}

// GameMode::state_gamemode_online_result (the ranked match's end menu): the rematch menu's while it is up. Once the next
// match is starting from it (online_straight.cpp) that code handles a failed network itself (back into the room the game's
// way, the session it started closed first).
PPC_FUNC(sub_825D7C60)
{
    const uint32_t impl = ctx.r3.u32;
    if (!rematch::g_menuWanted.load())
    {
        __imp__sub_825D7C60(ctx, base);
        return;
    }
    if (!straight::Running())
    {
        PPCContext c = ctx;
        c.r3.u64 = impl;
        c.r4.u64 = 0;
        sub_825D5CC8(c, base);                  // the online game ended badly: the native state's own way out
        if (c.r3.u32 & 0xFF)
        {
            std::fprintf(stderr, "[rematch] the online game ended: the end menu's own way out\n");
            rematch::g_menuWanted = false;
            __imp__sub_825D7C60(ctx, base);
            return;
        }
    }
    rematch::MenuFrame(ctx, base, impl);
    ctx.r3.u64 = 0;
}
