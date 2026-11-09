// Online PAIR PLAY: the game's own PAIR PLAY screen at a PAIR PLAY room's match start (online_2v2.h, port/ONLINE_2V2_PLAN.md
// "Native flow").
//
// The screen is the PADASSIGN mode (22) with its PAIR sequence (PadAssignSeqPair, mode flag 0x80), as offline PAIR PLAY's.
// When a PAIR PLAY room's match starts (online_pair_room.cpp: OnlineInRoom's battle start), its players - the room
// order's first members; seat k = the screen's controller k + 1, the same on every PC - open it, and the room's watchers
// wait at their battle start. Seat 0's PC runs the screen (the authority): every player's presses go to it, it numbers
// them and sends them back, and every player's PC plays them in that order on the same controllers (SynthPress) - each
// only once its screen has settled from the last (a move the game refuses while a stone is still moving on one PC and
// takes on another would part the screens). The game's rules decide what a press does; this file keeps the screen's
// ways out shut: OK (A: "Exiting player select. Continue anyway?") never reaches it, BACK (B) only steps off a stone -
// from the centre it would leave the screen. Everyone joins at once (START for each seat), and once every player stands
// on a stone and nothing has moved for a moment, the authority decides: the places go to the whole room (Done), the
// players' screens end the game's own way (the dialog's YES: sequence +12 = 2, +8 = 6 - the exit plays, CHARASELECT is
// asked) and the match goes straight on into character select (PlacesModeAsked -> online_pair_room.cpp StartPairMatch);
// the watchers go on from their battle start. Players without a stone after 30 s are placed (on the side with fewer).
//
// The screen's records (C:/ttt2wt/re/re_r1.md): the sequence [PadAssignMode::Impl +144] {+8 its state (2 input), +12
// the result, +16 the drawer (+4 its state: 3 interactive), +24 the model}; the model's maps (std::map nodes {+0 left, +4
// parent, +8 right, +12 key}): +8 the places (head [+12]; 1 red lower, 2 blue lower, 3 red upper, 4 blue upper -> {+16 a:
// 0 empty, 1 arriving, 3 leaving, 4 sliding down, 5 standing; +20 the controller}, nil flag +25) and +32 the controllers
// (head [+36]; 1-4 -> +16 the state: 0 none, 1 present, 2 joined (centre), 3 walking back, 4 / 5 walking to the red / blue
// side, 6 on a stone; nil flag +21).
#include "online_2v2.h"
#undef PlaySound                               // windows.h (mmsystem): menu_host's is meant
#include "menu_doc.h"
#include "menu_host.h"
#include "profiles.h"
#include "xlive.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <vector>

bool XSessionLocalHosts();                     // xsession.cpp: this game hosts the room
uint64_t XSessionCurrentId();                  // xsession.cpp: the room session (0: not in a room)

PPC_EXTERN_FUNC(sub_8228DFB8);   // the OK button's mask (A; B with the swapped button layout)
PPC_EXTERN_FUNC(sub_8228E048);   // the BACK button's mask
PPC_EXTERN_FUNC(sub_8238CDA0);   // the PAIR PLAY screen's help line (r3 the drawer, r4 the TEXT: a guest char*; 0 none)

namespace online2v2 {
uint32_t PadAssignImpl();                                            // online_2v2_select.cpp
bool InRoomMain();
std::vector<std::pair<uint64_t, std::string>> RoomPeersByName();   // online_2v2_team.cpp: members heard from (peer, name)
void ApplyPlacesNow();
namespace {

int64_t Ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool SameName(const std::string& a, const std::string& b) { return !a.empty() && a.substr(0, 15) == b.substr(0, 15); }

// ---- the wire
constexpr uint32_t kOpen = 0x32765153u, kPress = 0x32765150u, kEvents = 0x32765145u, kDone = 0x32765144u;   // "2vQS" Q P E D
constexpr int kSeats = 4, kLog = 32;
#pragma pack(push, 1)
struct WireSeat { uint64_t xuid; int8_t member; char name[24]; };
struct Open { uint32_t magic; uint64_t session; uint32_t round; uint8_t count; WireSeat seat[kSeats]; };
struct Press { uint32_t magic; uint64_t session; uint32_t round; uint16_t seq; uint16_t button; };
struct Event { uint16_t seq; uint8_t controller; uint8_t pad; uint16_t button; };
struct Events { uint32_t magic; uint64_t session; uint32_t round; uint8_t count; Event events[kLog]; uint16_t seen[kSeats]; };
struct Done
{
    uint32_t magic;
    uint64_t session;
    uint32_t round;
    int8_t place[4];                           // place 1..4 -> the seat there (-1 none)
    uint8_t cancelled;
    uint8_t count;
    WireSeat seat[kSeats];
    int8_t chooser;                            // the stage chooser: room order position 0 / 1 (a coin; protocol -13)
};
#pragma pack(pop)

constexpr uint16_t kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBackKey = 0x0020, kA = 0x1000, kB = 0x2000;
constexpr int kGapMs = 220, kHoldMs = 80, kSettleMs = 1500, kPlaceInMs = 30000, kAuthoritySilentMs = 12000;
constexpr size_t kHelpCapacity = 255;          // the help line's guest text buffer
constexpr int kGoneMs = 1500;                  // a seat's member gone from the room data this long: it left the room
// keepalive: every player tells the authority it is there (and the authority the room) every kBeatMs; a seat silent for
// kSeatSilentMs closed its game or lost its connection (the room itself takes about a minute to drop such a member)
constexpr int kBeatMs = 500, kSeatSilentMs = 10000;

enum Part : int { kNone = 0, kPlayer = 1, kWatcher = 2 };

std::mutex g_lock;
std::atomic<int> g_part{ kNone };
std::atomic<bool> g_active{ false };           // a player's screen: the game's controllers 1..count are the seats
std::atomic<int> g_activeCount{ 0 };
uint64_t g_session = 0;
uint32_t g_round = 0;                          // 0: a player that has not heard the authority yet
int g_count = 0;
WireSeat g_seat[kSeats];
uint64_t g_seatPeer[kSeats] = {};              // as this PC reaches them (0: this PC, or not heard from)
int g_mine = -1;
bool g_auth = false;
uint64_t g_authPeer = 0;
int64_t g_beganAt = 0, g_heardAt = 0, g_interactiveAt = 0;
// the presses played here
std::deque<Event> g_toPlay;
uint16_t g_played = 0;
int64_t g_nextPlayAt = 0;
// this PC's presses until the authority took them
std::deque<Press> g_myPresses;
uint16_t g_mySeq = 0;
int64_t g_pressSentAt = 0;
// the authority
uint16_t g_hostSeq = 0;
std::deque<Event> g_log;
uint16_t g_memberSeen[kSeats] = {};
int64_t g_openSentAt = 0, g_eventsSentAt = 0, g_allPlacedAt = 0, g_nextAutoAt = 0;
bool g_joinSent[kSeats] = {};
// the decision
Done g_done{};
bool g_haveDone = false;
int64_t g_doneAt = 0, g_doneSentAt = 0;
int g_doneSends = 0;
bool g_endAsked = false;
bool g_viewAllPlaced = false;                  // this PC's screen shows every player on a stone
uint32_t g_watchTaken = 0, g_teamsTaken = 0;   // the rounds a watcher's start / the teams took
int64_t g_goneSince[kSeats] = {};              // since when a seat's member is gone from the room data (0: there)
int64_t g_seatHeardAt[kSeats] = {};            // the authority: when each seat was last heard from
int64_t g_beatAt = 0;                          // this PC's last keepalive
// what the screen changes and character select must find as the room left it: the controllers' role requests (the screen
// gives each one on a stone role 4: sub_825811A8([0x82E6B054], 4, n)) and pad slots 9-12 (it writes its places there).
// The object (160 bytes, made once) has four role words at +120 - one-shot requests its step turns into an object per
// controller and sets back to 13 (sub_82581628; sub_825811E0 takes four too; the setter's bound lets n reach 4, which
// the game never passes) - and those four objects' pointers at +136, freed by the step when done (sub_82581588). Only
// the four role words go back: a pointer put back from before freed its object twice and gave the block to two owners
// (2026-11-08: the stack overflow after CHANGE TEAMS, a per-frame map's head written over).
constexpr uint32_t kRoles = 0x82E6B054u, kPadManager = 0x82E6ACA8u;
constexpr int kRoleWords = 4;
uint32_t g_savedRoles[kRoleWords] = {}, g_savedSlots[4] = {};
uint32_t g_savedRolesAt = 0;                   // the roles' object they were taken from
bool g_savedRolesOk = false, g_savedSlotsOk = false;
std::atomic<uint16_t> g_okMask{ kA }, g_backMask{ kB };

void Reset()
{
    g_round = 0;
    g_count = 0;
    std::memset(g_seat, 0, sizeof(g_seat));
    std::memset(g_seatPeer, 0, sizeof(g_seatPeer));
    g_mine = -1;
    g_auth = false;
    g_authPeer = 0;
    g_beganAt = g_heardAt = g_interactiveAt = 0;
    g_toPlay.clear();
    g_played = 0;
    g_nextPlayAt = 0;
    g_myPresses.clear();
    g_mySeq = 0;
    g_pressSentAt = 0;
    g_hostSeq = 0;
    g_log.clear();
    std::memset(g_memberSeen, 0, sizeof(g_memberSeen));
    g_openSentAt = g_eventsSentAt = g_allPlacedAt = g_nextAutoAt = 0;
    std::memset(g_joinSent, 0, sizeof(g_joinSent));
    g_haveDone = false;
    g_endAsked = false;
    g_viewAllPlaced = false;
    std::memset(g_goneSince, 0, sizeof(g_goneSince));
    std::memset(g_seatHeardAt, 0, sizeof(g_seatHeardAt));
    g_beatAt = 0;
    g_savedRolesOk = g_savedSlotsOk = false;   // a screen's start takes them again (PlacesBegin): never an earlier screen's
    g_savedRolesAt = 0;
}

std::string SeatName(int s)
{
    return s >= 0 && s < kSeats ? std::string(g_seat[s].name, strnlen(g_seat[s].name, sizeof(g_seat[s].name))) : std::string();
}

// A name as the screen's texts show it: without the game's text markup ('~' a colour, '%' a format) or control bytes, at
// most `maxChars` characters (UTF-8 kept whole).
std::string ShortName(const std::string& name, size_t maxChars)
{
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < name.size();)
    {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (c < 0x20 || c == 0x7F || c == '~' || c == '%' || (c >= 0x80 && len == 1) || i + len > name.size()) { ++i; continue; }
        if (chars++ == maxChars) break;
        out.append(name, i, len);
        i += len;
    }
    return out;
}

void SetSeats(const PairSeat* seats, int count)
{
    const auto peers = RoomPeersByName();
    g_count = (std::min)(count, kSeats);
    for (int s = 0; s < g_count; ++s)
    {
        g_seat[s].xuid = seats[s].xuid;
        g_seat[s].member = int8_t(seats[s].member);
        std::snprintf(g_seat[s].name, sizeof(g_seat[s].name), "%s", seats[s].name.c_str());
        g_seatPeer[s] = 0;
        for (const auto& [peer, name] : peers)
            if (SameName(name, seats[s].name)) g_seatPeer[s] = peer;
    }
}

void SendRoom(const void* data, size_t size)
{
    uint64_t peers[8];
    const size_t n = XSocketRoomPeers(peers, 8, 0);
    for (size_t i = 0; i < n; ++i) XSocketSend2v2(peers[i], data, size);
}

// ---- the screen as this PC's game holds it
struct View
{
    bool ok = false;
    uint32_t seq = 0, drawer = 0, model = 0;
    int seqState = -1, drawerState = -1;
    int state[5] = {};                         // controller 1..4
    int placeA[5] = {}, placeCtrl[5] = {};     // place 1..4
};

template <typename F> void Walk(uint8_t* base, uint32_t head, uint32_t nilAt, F&& visit)
{
    if (!head) return;
    uint32_t stack[16];
    int depth = 0, visits = 0;
    const uint32_t root = GRead32(base, head + 4);
    if (root && root != head) stack[depth++] = root;
    while (depth > 0 && visits++ < 16)
    {
        const uint32_t n = stack[--depth];
        if (!n || n == head || GRead8(base, n + nilAt)) continue;
        visit(n);
        const uint32_t left = GRead32(base, n), right = GRead32(base, n + 8);
        if (depth < 15 && left && left != head) stack[depth++] = left;
        if (depth < 15 && right && right != head) stack[depth++] = right;
    }
}

View ReadView(uint8_t* base)
{
    View v;
    const uint32_t impl = PadAssignImpl();
    if (!impl || !base) return v;
    v.seq = GRead32(base, impl + 144);
    if (!v.seq) return v;
    v.seqState = int(GRead32(base, v.seq + 8));
    v.drawer = GRead32(base, v.seq + 16);
    v.model = GRead32(base, v.seq + 24);
    if (!v.drawer || !v.model) return v;
    v.drawerState = int(GRead32(base, v.drawer + 4));
    Walk(base, GRead32(base, v.model + 36), 21, [&](uint32_t n) {
        const uint32_t k = GRead32(base, n + 12);
        if (k >= 1 && k <= 4) v.state[k] = int(GRead32(base, n + 16));
    });
    Walk(base, GRead32(base, v.model + 12), 25, [&](uint32_t n) {
        const uint32_t k = GRead32(base, n + 12);
        if (k >= 1 && k <= 4)
        {
            v.placeA[k] = int(GRead32(base, n + 16));
            v.placeCtrl[k] = int(GRead32(base, n + 20));
        }
    });
    v.ok = true;
    return v;
}
bool Interactive(const View& v) { return v.ok && v.seqState == 2 && v.drawerState == 3; }
// Nothing walks, slides or arrives: a press now does on every PC what it does here.
bool Settled(const View& v)
{
    if (!Interactive(v)) return false;
    for (int k = 1; k <= 4; ++k)
    {
        if (v.state[k] >= 3 && v.state[k] <= 5) return false;
        if (v.placeA[k] == 1 || v.placeA[k] == 3 || v.placeA[k] == 4) return false;
    }
    return true;
}
int Placed(const View& v, int count)
{
    int n = 0;
    for (int c = 1; c <= count; ++c) n += v.state[c] == 6;
    return n;
}
std::string Brief(const View& v)
{
    char s[160];
    std::snprintf(s, sizeof(s), "seq %d drawer %d, controllers %d %d %d %d, places %d/%d %d/%d %d/%d %d/%d", v.seqState, v.drawerState, v.state[1],
                  v.state[2], v.state[3], v.state[4], v.placeCtrl[1], v.placeA[1], v.placeCtrl[2], v.placeA[2], v.placeCtrl[3], v.placeA[3],
                  v.placeCtrl[4], v.placeA[4]);
    return s;
}

// The round is called off: no places - every PC goes back to the room (players: PlacesModeAsked; watchers: their battle
// start, online_pair_room.cpp). A player that calls it off tells the whole room (PlacesFrame), as the authority does.
void CancelRound(int64_t now, const std::string& why)
{
    g_done = Done{};
    g_done.magic = kDone;
    g_done.session = g_session;
    g_done.round = g_round;
    g_done.count = uint8_t(g_count);
    std::memcpy(g_done.seat, g_seat, sizeof(g_done.seat));
    std::memset(g_done.place, -1, sizeof(g_done.place));
    g_done.cancelled = 1;
    g_haveDone = true;
    g_doneAt = now;
    g_doneSends = 10;
    g_doneSentAt = 0;
    std::fprintf(stderr, "[places] called off (round %08X): %s\n", g_round, why.c_str());
}

// A seat's player left the room (its member gone from the room data for kGoneMs - the room control keeps the data up to
// date under the screen too): nobody can take that place on this screen, so the round is called off and the room's own
// order brings the next member in line. The authority calls it off for any seat; the other players and the watchers for
// the authority's (seat 1) - the authority calls the others' off itself, and its call reaches everyone.
void CheckSeatsGone(uint8_t* base, int64_t now)
{
    for (int s = 0; s < g_count && !g_haveDone; ++s)
    {
        const int present = PairMemberPresent(base, SeatName(s));
        if (present != 0)
        {
            g_goneSince[s] = 0;
            continue;
        }
        if (!g_goneSince[s]) g_goneSince[s] = now;
        else if (now - g_goneSince[s] >= kGoneMs && ((g_part.load() == kPlayer && g_auth) || s == 0))
            CancelRound(now, SeatName(s) + " left the room");
    }
    // the authority: a seat silent (no keepalive, no press) - its game closed or its connection is lost
    if (g_part.load() == kPlayer && g_auth)
        for (int s = 0; s < g_count && !g_haveDone; ++s)
            if (s != g_mine && now - (std::max)(g_seatHeardAt[s], g_beganAt) > kSeatSilentMs)
                CancelRound(now, SeatName(s) + " is silent (its game closed or lost the connection)");
}

// ---- the authority
// The round's seats to the whole room (the players learn the round from it; it is the authority's keepalive too).
void SendOpen(int64_t now)
{
    g_openSentAt = now;
    Open o{};
    o.magic = kOpen;
    o.session = g_session;
    o.round = g_round;
    o.count = uint8_t(g_count);
    std::memcpy(o.seat, g_seat, sizeof(o.seat));
    SendRoom(&o, sizeof(o));
}

void HostAppend(int seat, uint16_t button)
{
    Event e{};
    e.seq = ++g_hostSeq;
    e.controller = uint8_t(seat);
    e.button = button;
    g_log.push_back(e);
    while (g_log.size() > kLog) g_log.pop_front();
    g_toPlay.push_back(e);
    g_eventsSentAt = 0;                        // sent at once
}

// A player's press the authority takes: LEFT / RIGHT and BACK (stepping off; from the centre the game never sees it).
uint16_t Allowed(uint16_t button) { return button & (kLeft | kRight | g_backMask.load()); }

void TakeEvent(const Event& e)
{
    if (int16_t(e.seq - g_played) <= 0) return;
    for (const Event& q : g_toPlay)
        if (q.seq == e.seq) return;
    g_toPlay.push_back(e);
    std::sort(g_toPlay.begin(), g_toPlay.end(), [](const Event& a, const Event& b) { return int16_t(a.seq - b.seq) < 0; });
}

void Decide(const View& v, int64_t now)
{
    Done d{};
    d.magic = kDone;
    d.session = g_session;
    d.round = g_round;
    d.count = uint8_t(g_count);
    std::memcpy(d.seat, g_seat, sizeof(d.seat));
    int red = 0, blue = 0;
    for (int k = 1; k <= 4; ++k)
    {
        const int c = v.placeCtrl[k];
        d.place[k - 1] = int8_t(v.placeA[k] == 5 && c >= 1 && c <= g_count ? c - 1 : -1);
        if (d.place[k - 1] >= 0) (k & 1 ? red : blue)++;
    }
    if (!red || !blue || red + blue != g_count)
    {
        std::fprintf(stderr, "[places] not a match yet (%d red, %d blue of %d): %s\n", red, blue, g_count, Brief(v).c_str());
        g_allPlacedAt = 0;
        return;
    }
    // the new teams' first stage chooser: a coin (the red side's fighter 0, the blue side's 1) - after it, the loser chooses
    // (online_rematch.cpp)
    d.chooser = int8_t(std::random_device{}() & 1u);
    g_done = d;
    g_haveDone = true;
    g_doneAt = now;
    g_doneSends = 10;
    g_doneSentAt = 0;
    std::fprintf(stderr, "[places] decided (round %08X): red %s + %s, blue %s + %s; the %s side chooses the stage\n", g_round, SeatName(d.place[0]).c_str(),
                 d.place[2] >= 0 ? SeatName(d.place[2]).c_str() : "-", SeatName(d.place[1]).c_str(), d.place[3] >= 0 ? SeatName(d.place[3]).c_str() : "-",
                 d.chooser ? "blue" : "red");
}

void AuthorityFrame(const View& v, int64_t now)
{
    // seats not heard from at the start (no Hello yet): by name, once a second
    static int64_t peersAt = 0;
    if (now - peersAt >= 1000)
    {
        peersAt = now;
        for (int s = 0; s < g_count; ++s)
            if (s != g_mine && !g_seatPeer[s])
                for (const auto& [peer, name] : RoomPeersByName())
                    if (SameName(name, SeatName(s))) g_seatPeer[s] = peer;
    }
    if (!g_haveDone && now - g_openSentAt >= kBeatMs) SendOpen(now);
    if (now - g_eventsSentAt >= 100 && (!g_haveDone || now - g_doneAt < 3000))
    {
        g_eventsSentAt = now;
        Events ev{};
        ev.magic = kEvents;
        ev.session = g_session;
        ev.round = g_round;
        ev.count = uint8_t(g_log.size());
        for (size_t i = 0; i < g_log.size() && i < kLog; ++i) ev.events[i] = g_log[i];
        std::memcpy(ev.seen, g_memberSeen, sizeof(ev.seen));
        for (int s = 0; s < g_count; ++s)
            if (s != g_mine && g_seatPeer[s]) XSocketSend2v2(g_seatPeer[s], &ev, sizeof(ev));
    }
    if (!Interactive(v) || g_haveDone) return;
    // everyone joins at once (the game's START, each seat once its controller is there)
    for (int s = 0; s < g_count; ++s)
        if (!g_joinSent[s] && v.state[s + 1] == 1)
        {
            g_joinSent[s] = true;
            HostAppend(s, kStart);
        }
    const bool idle = Settled(v) && g_toPlay.empty() && now >= g_nextPlayAt;
    const bool all = Placed(v, g_count) == g_count;
    if (all && idle)
    {
        if (!g_allPlacedAt) g_allPlacedAt = now;
        else if (now - g_allPlacedAt >= kSettleMs) Decide(v, now);
    }
    else
        g_allPlacedAt = 0;
    // the time is up: the players without a stone are placed, one at a time - to the side with fewer (the red side on a tie)
    if (!all && idle && g_interactiveAt && now - g_interactiveAt >= kPlaceInMs && now >= g_nextAutoAt)
    {
        int red = 0, blue = 0;
        for (int k = 1; k <= 4; ++k)
            if (v.placeA[k] == 5) (k & 1 ? red : blue)++;
        for (int s = 0; s < g_count; ++s)
        {
            const int st = v.state[s + 1];
            if (st == 1) HostAppend(s, kStart);
            else if (st == 2) HostAppend(s, red <= blue ? kLeft : kRight);
            else continue;
            std::fprintf(stderr, "[places] time: %s placed (%s)\n", SeatName(s).c_str(), st == 1 ? "joins" : red <= blue ? "red side" : "blue side");
            break;
        }
        g_nextAutoAt = now + 400;
    }
}

// ---- every player
void Replay(const View& v, int64_t now)
{
    while (!g_toPlay.empty() && int16_t(g_toPlay.front().seq - g_played) <= 0) g_toPlay.pop_front();
    if (g_toPlay.empty() || uint16_t(g_toPlay.front().seq) != uint16_t(g_played + 1) || now < g_nextPlayAt || !Settled(v)) return;
    const Event e = g_toPlay.front();
    g_toPlay.pop_front();
    g_played = e.seq;
    g_nextPlayAt = now + kGapMs;
    SynthPress(e.controller, e.button, kHoldMs, 0);
    std::fprintf(stderr, "[places] play #%u: %s (controller %d) %04X - %s\n", e.seq, SeatName(e.controller).c_str(), e.controller + 1, e.button,
                 Brief(v).c_str());
}

bool Matches(const View& v)
{
    for (int k = 1; k <= 4; ++k)
    {
        const int want = g_done.place[k - 1];
        const int here = v.placeA[k] == 5 ? v.placeCtrl[k] - 1 : -1;
        if (want != here) return false;
    }
    return true;
}

// The places decided: the screen ends as the dialog's YES ends it, once this PC's screen shows them (or after 3 s).
void EndScreen(uint8_t* base, const View& v, int64_t now)
{
    if (!g_haveDone || g_endAsked || !Interactive(v)) return;
    const bool same = Settled(v) && g_toPlay.empty() && Matches(v);
    if (!same && now - g_doneAt < 3000) return;
    g_endAsked = true;
    GWrite32(base, v.seq + 12, 2);
    GWrite32(base, v.seq + 8, 6);
    std::fprintf(stderr, "[places] the screen ends (%s): %s\n", same ? "the places are the decided ones" : "WARNING: this screen differs - the decided places count",
                 Brief(v).c_str());
}

// The help line: what to do, the time left, this PC's controller (the badges on the controllers; the names are on the
// stones). sub_8238CDA0 takes the text itself (r4 a char*, copied into the line's text object by sub_823852B0 ->
// sub_82383540), not a text id - the screen's own calls pass the looked-up string.
std::string HelpText()
{
    if (g_haveDone && g_done.cancelled) return "A player left. Back to the room - please wait.";
    if (g_haveDone || g_viewAllPlaced) return "Everyone has a place. Get ready!";
    const int64_t left = g_interactiveAt ? kPlaceInMs - (Ms() - g_interactiveAt) : kPlaceInMs;
    // who is which controller (the badges' numbers), as TEST 18's line had it: the line holds about 105 characters
    std::string roster;
    for (int s = 0; s < g_count; ++s) roster += (roster.empty() ? "   " : "  ") + std::to_string(s + 1) + " " + ShortName(SeatName(s), 11);
    if (left <= 0) return "Placing the remaining players..." + roster;
    const std::string you = g_mine >= 0 ? "You are " + std::to_string(g_mine + 1) + ". " : std::string();
    return you + "LEFT / RIGHT: pick a side (" + std::to_string(int((left + 999) / 1000)) + ")" + roster;
}

} // namespace

bool PlacesActive() { return g_active.load(); }
int PlacesControllers() { return g_active.load() ? g_activeCount.load() : 0; }

bool PlacesBegin(const PairSeat* seats, int count, int mine)
{
    if (count < 2 || count > kSeats || mine < 0 || mine >= count) return false;
    std::lock_guard l(g_lock);
    Reset();
    g_session = XSessionCurrentId();
    SetSeats(seats, count);
    g_mine = mine;
    g_auth = mine == 0;
    g_authPeer = g_seatPeer[0];
    g_round = g_auth ? (uint32_t(Ms()) | 1u) : 0;
    g_beganAt = g_heardAt = Ms();
    for (int s = 0; s < kSeats; ++s) g_seatHeardAt[s] = g_beganAt;
    if (uint8_t* base = g_base)
    {
        const uint32_t roles = GRead32(base, kRoles);
        g_savedRolesOk = roles != 0;
        g_savedRolesAt = roles;
        for (int i = 0; i < kRoleWords && roles; ++i) g_savedRoles[i] = GRead32(base, roles + 120 + 4 * uint32_t(i));
        const uint32_t begin = GRead32(base, kPadManager + 84), end = GRead32(base, kPadManager + 88);
        g_savedSlotsOk = begin && end >= begin + 16;
        for (int i = 0; i < 4 && g_savedSlotsOk; ++i) g_savedSlots[i] = GRead32(base, begin + 4 * uint32_t(i));
    }
    g_activeCount = g_count;
    g_part = kPlayer;
    g_active = true;
    std::fprintf(stderr, "[places] the PAIR PLAY screen opens: %d players, this PC seat %d%s (round %08X)\n", g_count, mine + 1,
                 g_auth ? " - it runs the screen" : "", g_round);
    if (!g_auth && !g_authPeer) std::fprintf(stderr, "[places] WARNING: seat 1 (%s) is not heard from yet\n", SeatName(0).c_str());
    return true;
}

void PlacesWatch(const PairSeat* seats, int count)
{
    std::lock_guard l(g_lock);
    // a decision that came before this PC's battle start began (its own round, not yet taken) stands
    const bool keep = g_haveDone && g_done.session == XSessionCurrentId() && g_done.round != g_watchTaken && Ms() - g_doneAt < 60000;
    const Done kept = g_done;
    const int64_t keptAt = g_doneAt;
    Reset();
    g_session = XSessionCurrentId();
    SetSeats(seats, count);
    g_beganAt = Ms();
    if (keep)
    {
        g_done = kept;
        g_haveDone = true;
        g_doneAt = keptAt;
    }
    g_part = kWatcher;
    g_active = false;
}

bool PlacesDecided(PairPlaces& out)
{
    std::lock_guard l(g_lock);
    if (g_part.load() != kWatcher || !g_haveDone || (!g_done.cancelled && g_done.round == g_watchTaken)) return false;
    g_watchTaken = g_done.round;
    out.round = g_done.round;
    out.count = (std::min)(int(g_done.count), kSeats);
    out.cancelled = g_done.cancelled != 0;
    out.chooser = g_done.chooser == 0 ? 0 : 1;
    for (int k = 0; k < 4; ++k) out.place[k] = g_done.place[k];
    for (int s = 0; s < out.count; ++s)
    {
        out.seat[s].xuid = g_done.seat[s].xuid;
        out.seat[s].member = g_done.seat[s].member;
        out.seat[s].name.assign(g_done.seat[s].name, strnlen(g_done.seat[s].name, sizeof(g_done.seat[s].name)));
    }
    g_part = kNone;
    return true;
}

bool PlacesReceive(uint64_t from, const uint8_t* data, size_t size)
{
    if (size < 4) return false;
    uint32_t magic;
    std::memcpy(&magic, data, 4);
    if (magic != kOpen && magic != kPress && magic != kEvents && magic != kDone) return false;
    std::lock_guard l(g_lock);
    const int part = g_part.load();
    if (magic == kOpen && size >= sizeof(Open))
    {
        Open o;
        std::memcpy(&o, data, sizeof(o));
        if (o.session != g_session || part != kPlayer || g_auth || (g_round && o.round != g_round) || o.count < 2 || o.count > kSeats) return true;
        if (!g_round)
        {
            // the authority's seats count (every PC built its own from its room order - they should be the same)
            g_round = o.round;
            const uint64_t me = xlive::OnlineXuid(0);
            int mine = -1;
            for (int s = 0; s < o.count; ++s)
            {
                const std::string name(o.seat[s].name, strnlen(o.seat[s].name, sizeof(o.seat[s].name)));
                if ((me && o.seat[s].xuid == me) || SameName(name, profiles::Active().name)) mine = s;
                if (s >= g_count || !SameName(name, SeatName(s)))
                    std::fprintf(stderr, "[places] WARNING: the authority's seat %d is %s, this PC's %s\n", s + 1, name.c_str(), SeatName(s).c_str());
            }
            PairSeat seats[kSeats];
            for (int s = 0; s < o.count; ++s)
            {
                seats[s].xuid = o.seat[s].xuid;
                seats[s].member = o.seat[s].member;
                seats[s].name.assign(o.seat[s].name, strnlen(o.seat[s].name, sizeof(o.seat[s].name)));
            }
            SetSeats(seats, o.count);
            g_activeCount = g_count;
            if (mine >= 0) g_mine = mine;
            g_authPeer = from;
            std::fprintf(stderr, "[places] the authority (%s) runs round %08X - this PC seat %d\n", SeatName(0).c_str(), g_round, g_mine + 1);
        }
        g_heardAt = Ms();
        return true;
    }
    if (magic == kPress && size >= sizeof(Press))
    {
        Press p;
        std::memcpy(&p, data, sizeof(p));
        if (!g_auth || part != kPlayer || p.session != g_session || p.round != g_round || g_haveDone) return true;
        int seat = -1;
        for (int s = 0; s < g_count; ++s)
            if (s != g_mine && g_seatPeer[s] == from) seat = s;
        if (seat < 0)
        {
            // a seat this PC did not reach at the start (no Hello yet): by the sender's name now
            for (const auto& [peer, name] : RoomPeersByName())
                if (peer == from)
                    for (int s = 0; s < g_count; ++s)
                        if (s != g_mine && SameName(name, SeatName(s))) { g_seatPeer[s] = from; seat = s; }
        }
        if (seat < 0) return true;
        g_seatHeardAt[seat] = Ms();
        if (int16_t(p.seq - g_memberSeen[seat]) <= 0) return true;   // a keepalive (no new press) or one taken already
        g_memberSeen[seat] = p.seq;
        if (const uint16_t b = Allowed(p.button))
        {
            HostAppend(seat, b);
            std::fprintf(stderr, "[places] press %04X from %s (its #%u) -> event #%u\n", b, SeatName(seat).c_str(), p.seq, g_hostSeq);
        }
        return true;
    }
    if (magic == kEvents && size >= sizeof(Events))
    {
        Events ev;
        std::memcpy(&ev, data, sizeof(ev));
        if (part != kPlayer || g_auth || ev.session != g_session || ev.round != g_round || from != g_authPeer) return true;
        for (int i = 0; i < ev.count && i < kLog; ++i) TakeEvent(ev.events[i]);
        if (g_mine >= 0)
            while (!g_myPresses.empty() && int16_t(g_myPresses.front().seq - ev.seen[g_mine]) <= 0) g_myPresses.pop_front();
        g_heardAt = Ms();
        return true;
    }
    if (magic == kDone && size >= sizeof(Done))
    {
        Done d;
        std::memcpy(&d, data, sizeof(d));
        if (d.session != XSessionCurrentId() || g_haveDone) return true;
        // a player not told its round yet takes no round it already took (an older screen's authority repeats its Done for
        // 15 s - a call-off's cancel too - while the next start's screen may already be open here)
        if (part == kPlayer ? (g_round ? d.round != g_round : d.round == g_watchTaken) : d.round == g_watchTaken) return true;
        g_done = d;
        g_haveDone = true;
        g_doneAt = Ms();
        std::fprintf(stderr, "[places] %s (round %08X): places %d %d %d %d\n", d.cancelled ? "cancelled" : "decided", d.round, d.place[0], d.place[1],
                     d.place[2], d.place[3]);
        return true;
    }
    return true;
}

// The pad poll thread, while this PC's screen is open: its own controller (user 0 as the game would see it) - its presses
// go to the authority (the game sees only the presses everyone plays).
// Its keepalive goes from here too, every kBeatMs: this thread runs on while the main thread is busy (loading the screen
// takes seconds on a slow PC) - a player's press of no button to the authority, the authority's Open to the room.
void PlacesLocalPad(uint16_t buttons)
{
    static uint16_t last = 0;
    const uint16_t pressed = buttons & ~last;
    last = buttons;
    if (!g_active.load()) return;
    std::lock_guard l(g_lock);
    const int64_t now = Ms();
    if (g_part.load() == kPlayer && !g_haveDone && now - g_beatAt >= kBeatMs)
    {
        g_beatAt = now;
        if (g_auth) SendOpen(now);
        else if (g_round && g_authPeer)
        {
            const Press beat{ kPress, g_session, g_round, g_mySeq, 0 };
            XSocketSend2v2(g_authPeer, &beat, sizeof(beat));
        }
    }
    if (!pressed || g_mine < 0 || g_haveDone) return;
    for (uint16_t bit = 1; bit; bit <<= 1)
    {
        if (!(pressed & bit) || !Allowed(bit)) continue;
        if (g_auth) HostAppend(g_mine, bit);
        else
        {
            g_myPresses.push_back(Press{ kPress, g_session, g_round, ++g_mySeq, bit });
            g_pressSentAt = 0;
        }
    }
}

// The names frame (FrameOnlinePairNames, menu_doc.cpp AddPairNames) over the PAIR PLAY screen: shown once the screen takes
// presses - only when no frame is up (the room's battle start and CHANGE TEAMS leave none; never one this code does not
// own) - its texts kept to `names`, closed (empty first) as soon as the screen ends, before its exit asks for character
// select (which must start with no frame: 2026-11-07 m134). The game's own menu calls, on the main thread, unlocked.
// The room's chat log window is put away meanwhile too (it stayed over the red stones), back as it was in the room.
void NamesFrame(PPCContext& ctx, uint8_t* base, bool wanted, const std::string (&names)[4])
{
    static const std::string kFrame = "FrameOnlinePairNames";
    static bool up = false, logHidden = false, logWasShown = false;
    static std::string shown[4], warned;
    if (wanted || up)
    {
        const std::string cur = menu_host::CurrentFrame(ctx, base);
        const bool none = cur.empty() || cur == "(none)";
        if (up && cur != kFrame)
        {
            up = false;
            std::fprintf(stderr, "[places] the names frame was replaced by '%s'\n", cur.c_str());
        }
        else if (wanted && !up)
        {
            if (none)
            {
                for (std::string& t : shown) t = "\x01";   // never a name: the first pass sets every text
                menu_host::ShowFrame(ctx, base, kFrame);
                up = true;
                warned.clear();
                std::fprintf(stderr, "[places] the names frame shown\n");
            }
            else if (cur != warned)
            {
                warned = cur;
                std::fprintf(stderr, "[places] WARNING: the names frame not shown - the frame '%s' is up\n", cur.c_str());
            }
        }
        if (up)
        {
            bool changed = false;
            for (int i = 0; i < 4; ++i)
            {
                const std::string& t = wanted ? names[i] : std::string();
                if (t == shown[i]) continue;
                shown[i] = t;
                changed = true;
                menu_host::SetText(ctx, base, "TextPairPlace" + std::to_string(i + 1), t);
            }
            if (changed && wanted)
                std::fprintf(stderr, "[places] names: red %s / %s, blue %s / %s\n", shown[0].empty() ? "-" : shown[0].c_str(),
                             shown[1].empty() ? "-" : shown[1].c_str(), shown[2].empty() ? "-" : shown[2].c_str(), shown[3].empty() ? "-" : shown[3].c_str());
            if (!wanted)
            {
                menu_host::ShowFrame(ctx, base, "");
                up = false;
                std::fprintf(stderr, "[places] the names frame closed\n");
            }
        }
    }
    if (g_active.load() && !logHidden)
    {
        logHidden = true;
        logWasShown = menu_host::ChatLogShown(ctx, base);
        if (logWasShown) menu_host::ChatLogShow(ctx, base, false);
    }
    else if (!g_active.load() && logHidden && InRoomMain())
    {
        logHidden = false;
        if (logWasShown) menu_host::ChatLogShow(ctx, base, true);
    }
}

// The main thread, every frame (online2v2::Frame).
void PlacesFrame(PPCContext& ctx, uint8_t* base)
{
    const int64_t now = Ms();
    bool namesWanted = false;
    std::string names[4];
    std::unique_lock l(g_lock);
    const bool active = g_active.load();
    if (active)
    {
        PPCContext c = ctx;
        sub_8228DFB8(c, base);
        g_okMask = uint16_t(c.r3.u32);
        sub_8228E048(c, base);
        g_backMask = uint16_t(c.r3.u32);
    }
    // the decision to the whole room (the watchers wait for it at their battle start): ten times in the first 1.5 s,
    // then once a second for 15 s
    if ((g_auth || (g_done.cancelled && g_part.load() == kPlayer)) && g_haveDone && now - g_doneAt < 15000 &&
        now - g_doneSentAt >= (g_doneSends > 0 ? 150 : 1000))
    {
        g_doneSentAt = now;
        if (g_doneSends > 0) --g_doneSends;
        SendRoom(&g_done, sizeof(g_done));
    }
    if (!g_haveDone && (g_part.load() == kPlayer || g_part.load() == kWatcher)) CheckSeatsGone(base, now);
    if (active && g_part.load() == kPlayer)
    {
        const View v = ReadView(base);
        if (Interactive(v) && !g_interactiveAt) g_interactiveAt = now;
        g_viewAllPlaced = Interactive(v) && Placed(v, g_count) == g_count;
        if (g_auth) AuthorityFrame(v, now);
        else
        {
            if (g_round && !g_myPresses.empty() && now - g_pressSentAt >= 100 && g_authPeer)
            {
                g_pressSentAt = now;
                for (Press p : g_myPresses)
                {
                    p.round = g_round;
                    XSocketSend2v2(g_authPeer, &p, sizeof(p));
                }
            }
            // the authority silent (its game gone): the screen ends - the match starts without places, and character
            // select's own online end takes everyone back to the room
            if (!g_haveDone && now - g_heardAt > kAuthoritySilentMs) CancelRound(now, "the authority (" + SeatName(0) + ") is silent");
        }
        Replay(v, now);
        if (g_haveDone && g_done.cancelled && !g_endAsked && Interactive(v))
        {
            // no match: this PC's part of the game's close after a match first (the session's end, the room's entry -
            // PairCallOffClose; the game's own code, unlocked), then the screen ends and its mode ask goes back to the
            // room (PlacesModeAsked)
            l.unlock();
            const bool closed = PairCallOffClose(ctx, base);
            l.lock();
            if (closed)
            {
                g_endAsked = true;
                GWrite32(base, v.seq + 12, 2);
                GWrite32(base, v.seq + 8, 6);
            }
        }
        else
            EndScreen(base, v, now);
        // the names at the stones while the screen takes presses (gone once it ends or is called off): model place 1 red
        // lower, 2 blue lower, 3 red upper, 4 blue upper -> TextPairPlace 1 red lower, 2 red upper, 3 blue lower, 4 blue
        // upper; a name shows once its player stands there (a 5: arrived - the screen's own arrival callback)
        namesWanted = Interactive(v) && !g_endAsked && !(g_haveDone && g_done.cancelled) && menu_doc::MenuPairNamesFrame();
        static constexpr int kTextOfPlace[5] = { 0, 1, 3, 2, 4 };
        for (int k = 1; k <= 4 && namesWanted; ++k)
            if (v.placeA[k] == 5 && v.placeCtrl[k] >= 1 && v.placeCtrl[k] <= g_count) names[kTextOfPlace[k] - 1] = ShortName(SeatName(v.placeCtrl[k] - 1), 15);
        // the help line, every second while the screen takes presses (the text: PlacesText)
        static int64_t helpAt = 0;
        static uint32_t helpBuffer = 0;
        static std::string helpShown;
        const std::string help = HelpText();
        if (Interactive(v) && (help != helpShown || now - helpAt >= 1000))
        {
            if (!helpBuffer) helpBuffer = xlive::GuestAlloc(uint32_t(kHelpCapacity + 1));
            if (helpBuffer)
            {
                helpAt = now;
                helpShown = help;
                const size_t n = (std::min)(help.size(), kHelpCapacity);
                std::memcpy(base + helpBuffer, help.data(), n);
                base[helpBuffer + n] = 0;
                PPCContext c = ctx;
                c.r3.u64 = v.drawer;
                c.r4.u64 = helpBuffer;
                l.unlock();                    // the drawer's own code runs unlocked (it may ask for texts: PlacesText)
                sub_8238CDA0(c, base);
                l.lock();
            }
        }
    }
    l.unlock();
    NamesFrame(ctx, base, namesWanted, names);
}

// The mode manager is asked for a mode (sub_825ED560 hook): this PC's PAIR PLAY screen ended - CHARASELECT (8; 16 is its
// way back to the main menu, which the screen never takes here: the same). The controllers' roles and pad slots go back
// to the room's, the teams and the room's order follow the places, and the match starts as the room's battle start
// starts it (StartPairMatch: its flags). Returns the mode to ask for (unchanged when not ours).
uint32_t PlacesModeAsked(PPCContext& ctx, uint8_t* base, uint32_t mode, uint32_t& flags)
{
    if (g_part.load() != kPlayer || (mode != 8 && mode != 16)) return mode;
    PairPlaces p;
    int mine;
    {
        std::lock_guard l(g_lock);
        if (const uint32_t roles = GRead32(base, kRoles); roles && g_savedRolesOk && roles == g_savedRolesAt)
            for (int i = 0; i < kRoleWords; ++i) GWrite32(base, roles + 120 + 4 * uint32_t(i), g_savedRoles[i]);
        const uint32_t begin = GRead32(base, kPadManager + 84), end = GRead32(base, kPadManager + 88);
        if (g_savedSlotsOk && begin && end >= begin + 16)
            for (int i = 0; i < 4; ++i) GWrite32(base, begin + 4 * uint32_t(i), g_savedSlots[i]);
        g_savedRolesOk = g_savedSlotsOk = false;                         // put back once
        p.round = g_round;
        p.count = g_count;
        p.cancelled = !g_haveDone || g_done.cancelled;
        p.chooser = g_haveDone && g_done.chooser == 0 ? 0 : 1;
        for (int k = 0; k < 4; ++k) p.place[k] = g_haveDone ? g_done.place[k] : -1;
        for (int s = 0; s < g_count; ++s)
        {
            p.seat[s].xuid = g_seat[s].xuid;
            p.seat[s].member = g_seat[s].member;
            p.seat[s].name = SeatName(s);
        }
        mine = g_mine;
        g_part = kNone;
        g_active = false;
        g_toPlay.clear();
        if (!g_haveDone)                       // the screen ended without a decision (never: its ways out are shut)
        {
            g_done = Done{};
            g_done.magic = kDone;
            g_done.session = g_session;
            g_done.round = g_round;
            std::memset(g_done.place, -1, sizeof(g_done.place));
            g_done.cancelled = 1;
            g_haveDone = true;
        }
        g_watchTaken = g_done.round;           // this decision was this PC's: never a later start's
    }
    SynthClear();
    if (p.cancelled)
    {
        // no match: back to the room as after one (OnlineInRoom's flags 1 keep the room control; 0 makes a new one,
        // sub_825FBC30) - the room host's after-match step then finds no result (PairStartCancelled)
        PairStartCancelled();
        flags = 1;
        std::fprintf(stderr, "[places] the screen asked for mode %u: called off - back to the room\n", mode);
        return 35;
    }
    std::fprintf(stderr, "[places] the screen asked for mode %u: the match starts\n", mode);
    ApplyPlacesNow();
    WriteMatchOrder(ctx, base, p);
    flags = StartPairMatch(ctx, base, p, mine, p.chooser);
    return 8;
}

// The last match's places in this room, as the screen decided them (false: none - called off, another room), and this PC's
// seat in them (-1: it watched): the end menu's straight paths (online_straight.cpp) start the next match from them.
bool PlacesLast(PairPlaces& out, int& mine)
{
    std::lock_guard l(g_lock);
    if (!g_haveDone || g_done.cancelled || g_done.session != XSessionCurrentId()) return false;
    out = PairPlaces{};
    out.round = g_done.round;
    out.count = (std::min)(int(g_done.count), kSeats);
    out.chooser = g_done.chooser == 0 ? 0 : 1;
    for (int k = 0; k < 4; ++k) out.place[k] = g_done.place[k];
    for (int s = 0; s < out.count; ++s)
    {
        out.seat[s].xuid = g_done.seat[s].xuid;
        out.seat[s].member = g_done.seat[s].member;
        out.seat[s].name.assign(g_done.seat[s].name, strnlen(g_done.seat[s].name, sizeof(g_done.seat[s].name)));
    }
    mine = g_mine;
    return true;
}

// The screen's help line while it is open (menu_ext.cpp's text lookup; "" = the game's): 0x527 "Press START button of
// controller in use." / 0x628 "Please select a BATTLE POSITION." - with the time left and the players' numbers.
std::string PlacesText(uint8_t* base, uint32_t id)
{
    (void)base;
    if (!g_active.load() || (id != 0x527 && id != 0x628 && id != 0x624)) return {};
    std::lock_guard l(g_lock);
    return HelpText();
}

// The teams' code (online_2v2_team.cpp ApplyPlaces) takes each decision once.
bool PlacesTakeResult()
{
    std::lock_guard l(g_lock);
    if (!g_haveDone || g_done.round == g_teamsTaken) return false;
    g_teamsTaken = g_done.round;
    return true;
}

// The room's own control keeps running under the screen; it takes no input meanwhile (sub_826C03E0 r4: input allowed).
bool PlacesBlockRoomInput() { return g_active.load(); }

// The places decided: slot 9 10 11 12 (place 1..4) -> the seat there (-1: none), seat -> member; false: none (cancelled).
bool PlacesResult(int slots[4], uint64_t seatPeer[4], uint64_t seatXuid[4], int& mine)
{
    std::lock_guard l(g_lock);
    if (!g_haveDone || g_done.cancelled) return false;
    for (int s = 0; s < 4; ++s)
    {
        slots[s] = g_done.place[s];
        seatPeer[s] = s < g_done.count ? g_seatPeer[s] : 0;
        seatXuid[s] = s < g_done.count ? g_done.seat[s].xuid : 0;
    }
    mine = -1;
    const uint64_t me = xlive::OnlineXuid(0);
    for (int s = 0; s < g_done.count && s < kSeats; ++s)
        if ((me && g_done.seat[s].xuid == me) ||
            SameName(std::string(g_done.seat[s].name, strnlen(g_done.seat[s].name, sizeof(g_done.seat[s].name))), profiles::Active().name))
            mine = s;
    return true;
}

// The screen reads each controller's pressed buttons (sub_8238E8B0 -> sub_8228D578(pad manager, n = 1..4)): never its
// OK (the dialog that leaves it), BACK only from a stone (from the centre it leaves the joined players or the screen),
// START only to join.
uint32_t PlacesFilter(uint8_t* base, uint32_t n, uint32_t pressed)
{
    if (!pressed || n < 1 || n > 4) return pressed;
    if (int(n) > g_activeCount.load()) return 0;
    const View v = ReadView(base);
    const int state = v.state[n];
    pressed &= ~uint32_t(g_okMask.load());
    if (state <= 2) pressed &= ~uint32_t(g_backMask.load());
    if (state != 1) pressed &= ~uint32_t(kStart);
    pressed &= ~uint32_t(kBackKey);
    return pressed;
}

} // namespace online2v2

extern "C" PPC_FUNC(__imp__sub_826C03E0);
PPC_FUNC(sub_826C03E0)
{
    if (rematch::RoomMain(ctx, base, ctx.r3.u32))
    {
        ctx.r3.u64 = 0;
        return;
    }
    if (online2v2::PlacesBlockRoomInput()) ctx.r4.u64 = 0;
    __imp__sub_826C03E0(ctx, base);
}

// The room's update draws the session's name itself at its information plate (sub_826C0CE8 -> sub_826BB1B8(x, y, name,
// 1)): not while the screen is open.
extern "C" PPC_FUNC(__imp__sub_826BB1B8);
PPC_FUNC(sub_826BB1B8)
{
    if (online2v2::PlacesActive()) return;
    __imp__sub_826BB1B8(ctx, base);
}

// A controller's pressed buttons (r3 the pad manager, r4 n): the PAIR PLAY screen's input (sub_8238E8B0's four calls).
extern "C" PPC_FUNC(__imp__sub_8228D578);
PPC_FUNC(sub_8228D578)
{
    const uint32_t lr = uint32_t(ctx.lr), n = ctx.r4.u32;
    __imp__sub_8228D578(ctx, base);
    if (lr >= 0x8238E8B0u && lr < 0x8238F1F0u && online2v2::PlacesActive()) ctx.r3.u64 = online2v2::PlacesFilter(base, n, ctx.r3.u32);
}
