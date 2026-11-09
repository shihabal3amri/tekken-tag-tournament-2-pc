// Online 2v2 teams in a PAIR PLAY room (online_2v2.h, port/ONLINE_2V2_PLAN.md "The new flow"): who plays with whom. Room
// members announce themselves to each other (Hello, once a second while in a room: name, role, side); the teams come from
// the PAIR PLAY screen's places (online_2v2_places.cpp, ApplyPlaces below). The room host's start opens that screen
// (online_rematch.cpp). The game's main thread runs the windows (TeamFrame).
#include "online_2v2.h"
#undef PlaySound                               // windows.h (mmsystem): menu_host's is meant
#include "menu_host.h"
#include "guest_heap.h"
#include "profiles.h"
#include "rank_sync.h"
#include "sys_ui.h"
#include "unlock_all.h"
#include "xlive.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

uint64_t XSessionCurrentId();                  // xsession.cpp: the room session (0: not in a room)
uint16_t HostControllerButtons();              // kernel_xam.cpp
bool XSessionLocalHosts();                     // xsession.cpp: this game hosts the room

namespace online2v2 {
namespace {

int64_t Ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- the game's character names (its own table: 0x82DD1DE0 = char* by character id; 0 Paul Phoenix .. 58 Tiger
// Jackson - the 59 fighters; 59 on are not playable)
constexpr uint32_t kNameTable = 0x82DD1DE0u;
constexpr int kFighters = 59;
std::string CharacterName(int id)
{
    if (id < 0 || id >= kFighters || !g_base) return "?";
    const uint32_t p = GRead32(g_base, kNameTable + 4 * uint32_t(id));
    std::string s;
    for (uint32_t i = 0; p && i < 40; ++i)
    {
        const char c = char(GRead8(g_base, p + i));
        if (!c) break;
        s += char(std::toupper(static_cast<unsigned char>(c)));
    }
    return s.empty() ? "?" : s;
}
// The fighters in name order (the row cycles through them; built once).
const std::vector<int>& ByName()
{
    static std::vector<int> order;
    if (order.empty() && g_base)
    {
        for (int i = 0; i < kFighters; ++i) order.push_back(i);
        std::sort(order.begin(), order.end(), [](int a, int b) { return CharacterName(a) < CharacterName(b); });
    }
    return order;
}

// ---- the wire (Team2v2 packets, magic first)
constexpr uint32_t kHello = 0x32763248u, kLeave = 0x3276324Cu;                                              // H L
constexpr uint32_t kPing = 0x32763270u, kPong = 0x32763271u;                                                // host <-> partner
#pragma pack(push, 1)
struct Hello
{
    uint32_t magic;
    uint64_t session;                           // the room (XSessionCurrentId): members of another room are not listed
    uint64_t xuid;
    char name[24];
    uint8_t role;                               // Role
    uint64_t teammate;                          // its teammate's XUID (0: none)
    int16_t character;                          // a partner's character (-1: none chosen)
    int8_t side;                                // the side the PAIR PLAY places put it on (0 red, 1 blue, -1 none)
    // protocol -13: the sender's own online character ranks (rank_sync::OwnOnlineRanks) - a PAIR PLAY fight shows each
    // character with the rank of the player who controls it (PairRank below)
    uint8_t ranksValid;                         // 1: `ranks` holds them
    uint8_t ranks[63];                          // by character id, 0..42
};
constexpr size_t kHelloNoRanks = offsetof(Hello, ranksValid);
struct Leave { uint32_t magic; };
struct Ping { uint32_t magic; int64_t sentMs; };
#pragma pack(pop)

struct Member
{
    uint64_t xuid = 0;
    std::string name;
    Role role = Role::None;
    uint64_t teammate = 0;
    int character = -1;
    int side = -1;
    int64_t seen = 0;
    bool haveRanks = false;                    // its own online character ranks (its Hello's)
    uint8_t ranks[63] = {};
};
std::mutex g_lock;
std::map<uint64_t, Member> g_members;          // room members heard from, by peer
uint64_t g_session = 0;                        // the room this game is in
std::string g_teammateName;
int64_t g_teamSince = 0;                       // when this team formed (a Hello sent before it is no reason to end it)
double g_rttMs = -1;                           // the host: round trip to its partner (smoothed; -1 unknown)
int g_mySide = -1;                             // the side the PAIR PLAY places put this PC's player on (-1: none)
bool g_haveOwnRanks = false;                   // this player's own online character ranks (TeamFrame, from the save)
uint8_t g_ownRanks[63] = {};
struct PairSide { uint64_t fighter = 0, partner = 0; std::string fighterName, partnerName; };
PairSide g_pairSide[2];                        // the places' two fighters (slot 9 / 10) and their partners (11 / 12)

// Windows wait here until the game's dialog window is free (the main thread shows them).
std::deque<SysUiMessageBox> g_boxes;
void Show(SysUiMessageBox b) { g_boxes.push_back(std::move(b)); }
void Info(const std::string& text)
{
    SysUiMessageBox b;
    b.text = text;
    b.buttons = { "OK" };
    b.cancel = 0;
    Show(std::move(b));
}

std::string MyName() { return profiles::Active().name; }

std::vector<std::pair<uint64_t, Member>> RoomMembers()
{
    std::vector<std::pair<uint64_t, Member>> out;
    const int64_t now = Ms();
    for (const auto& [peer, m] : g_members)
        if (now - m.seen < 4000) out.push_back({ peer, m });
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second.name < b.second.name; });
    return out;
}

void SendLeave()
{
    const uint64_t mate = Teammate();
    if (!mate) return;
    const Leave l{ kLeave };
    XSocketSend2v2(mate, &l, sizeof(l));
}

void EndTeam(const char* why)
{
    if (CurrentRole() == Role::None) return;
    std::fprintf(stderr, "[2v2] team ended: %s\n", why);
    SetTeam(Role::None, 0);
    g_teammateName.clear();
}

} // namespace

// ---- packets
bool TeamReceive(uint64_t from, const uint8_t* data, size_t size)
{
    uint32_t magic = 0;
    if (size < 4) return false;
    std::memcpy(&magic, data, 4);
    std::lock_guard l(g_lock);
    if (magic == kHello && size >= kHelloNoRanks)
    {
        Hello h{};
        std::memcpy(&h, data, (std::min)(size, sizeof(h)));
        if (!g_session || h.session != g_session) return true;   // another room
        Member& m = g_members[from];
        if (size >= sizeof(Hello) && h.ranksValid == 1)
        {
            uint8_t ranks[63];
            int ranked = 0, highest = 0;
            for (int c = 0; c < 63; ++c)
            {
                ranks[c] = h.ranks[c] <= 42 ? h.ranks[c] : 0;
                ranked += ranks[c] != 0;
                highest = (std::max)(highest, int(ranks[c]));
            }
            if (!m.haveRanks || std::memcmp(m.ranks, ranks, sizeof(ranks)) != 0)
            {
                std::string name(h.name, strnlen(h.name, sizeof(h.name)));
                std::fprintf(stderr, "[pair-ranks] %s's ranks: %d character(s) ranked, highest %d\n", name.c_str(), ranked, highest);
                std::memcpy(m.ranks, ranks, sizeof(ranks));
                m.haveRanks = true;
            }
        }
        m.xuid = h.xuid;
        m.name.assign(h.name, strnlen(h.name, sizeof(h.name)));
        m.role = Role(h.role);
        m.teammate = h.teammate;
        m.character = h.character;
        m.side = h.side;
        m.seen = Ms();
        // the teammate left the room or the team without a word
        if (from == Teammate() && Ms() - g_teamSince > 3000 && (m.teammate != xlive::OnlineXuid(0) || m.role == Role::None))
            EndTeam("the teammate is in no team with us");
        return true;
    }
    if (magic == kPing && size >= sizeof(Ping))
    {
        Ping p;
        std::memcpy(&p, data, sizeof(p));
        p.magic = kPong;
        if (from == Teammate()) XSocketSend2v2(from, &p, sizeof(p));
        return true;
    }
    if (magic == kPong && size >= sizeof(Ping))
    {
        Ping p;
        std::memcpy(&p, data, sizeof(p));
        const double rtt = double(Ms() - p.sentMs);
        if (from == Teammate() && rtt >= 0 && rtt < 2000) g_rttMs = g_rttMs < 0 ? rtt : g_rttMs * 0.8 + rtt * 0.2;
        return true;
    }
    if (magic == kLeave)
    {
        // no message box: the teams are the PAIR PLAY screen's, made again for every match, and the room's own log says
        // who left the session (a box there took the room's presses until it was answered - 2026-11-07)
        if (from == Teammate()) EndTeam("the teammate left");
        return true;
    }
    return false;
}

// Room members heard from in the last 4 s (peer, name), by name (the PAIR PLAY screen's controllers).
std::vector<std::pair<uint64_t, std::string>> RoomPeersByName()
{
    std::lock_guard l(g_lock);
    std::vector<std::pair<uint64_t, std::string>> out;
    for (const auto& [peer, m] : RoomMembers()) out.push_back({ peer, m.name });
    return out;
}
uint64_t PeerXuid(uint64_t peer)
{
    std::lock_guard l(g_lock);
    const auto it = g_members.find(peer);
    return it != g_members.end() ? it->second.xuid : 0;
}
uint64_t PeerOfXuid(uint64_t xuid)
{
    std::lock_guard l(g_lock);
    for (const auto& [peer, m] : g_members)
        if (m.xuid == xuid) return peer;
    return 0;
}
std::string NameOfXuid(uint64_t xuid)
{
    if (xuid == xlive::OnlineXuid(0)) return MyName();
    std::lock_guard l(g_lock);
    for (const auto& [peer, m] : g_members)
        if (m.xuid == xuid) return m.name;
    return std::string();
}

// The PAIR PLAY screen's places, once it is done (online_2v2_places.cpp): this PC's team follows them - slot 9 / 10 is a
// side's first player (the lower stone: it fights), 11 / 12 its second (the partner, who plays the second character
// with its own controller). A side with one player fights alone (2v1).
bool PlacesResult(int slots[4], uint64_t seatPeer[4], uint64_t seatXuid[4], int& mine);
bool PlacesTakeResult();
void ApplyPlaces()
{
    int slots[4], mine = -1;
    uint64_t seatPeer[4], seatXuid[4];
    if (!PlacesResult(slots, seatPeer, seatXuid, mine)) return;
    PairSide pair[2];                                              // every PC's, a watcher's too: the HUD's names
    for (int k = 0; k < 2; ++k)
    {
        const int fighter = slots[k], partner = slots[k + 2];
        pair[k].fighter = fighter >= 0 && fighter < 4 ? seatXuid[fighter] : 0;
        pair[k].partner = partner >= 0 && partner < 4 ? seatXuid[partner] : 0;
        if (pair[k].fighter) pair[k].fighterName = NameOfXuid(pair[k].fighter);
        if (pair[k].partner) pair[k].partnerName = NameOfXuid(pair[k].partner);
    }
    int mySlot = -1;
    for (int s = 0; s < 4; ++s)
        if (mine >= 0 && slots[s] == mine) mySlot = s;
    g_mySide = mySlot < 0 ? -1 : mySlot & 1;                       // slots 9 / 11 red (side 0), 10 / 12 blue (side 1)
    const int mateSlot = mySlot < 0 ? -1 : mySlot ^ 2;             // the same side's other place
    const int mateController = mateSlot >= 0 ? slots[mateSlot] : -1;
    const uint64_t mateXuid = mateController >= 0 && mateController < 4 ? seatXuid[mateController] : 0;
    const uint64_t matePeer = mateXuid ? PeerOfXuid(mateXuid) : 0;
    const std::string mateName = mateXuid ? NameOfXuid(mateXuid) : std::string();
    const bool teamed = mySlot >= 0 && matePeer;
    {
        std::lock_guard l(g_lock);
        g_pairSide[0] = pair[0];
        g_pairSide[1] = pair[1];
        if (!teamed)
        {
            SetTeam(Role::None, 0);
            g_teammateName.clear();
        }
        else
        {
            SetTeam(mySlot < 2 ? Role::Host : Role::Partner, matePeer);
            g_teammateName = mateName;
            g_teamSince = Ms();
        }
    }
    std::fprintf(stderr, "[places] this PC: %s%s%s\n", mySlot < 0 ? "watching" : mySlot < 2 ? "fights" : "partner",
                 mySlot < 0 ? "" : mySlot & 1 ? " (blue side)" : " (red side)", teamed && !mateName.empty() ? (" with " + mateName).c_str() : "");
}

// The places now (the PAIR PLAY screen ended on this PC: the match starts with them - online_2v2_places.cpp).
void ApplyPlacesNow()
{
    if (PlacesTakeResult()) ApplyPlaces();
}

std::string TeammateName() { std::lock_guard l(g_lock); return g_teammateName; }

// A message in the game's dialog window, once it is free (online_rematch.cpp's too).
void TeamInfo(const std::string& text) { Info(text); }

// The room's messages (text lookup, menu_ext.cpp) in a PAIR PLAY match room's words: text 0x171 "Waiting for a response
// from the opponent" is READY's while another player is not READY yet (sub_826C01F8) - here three may not be.
std::string RoomText(uint32_t id)
{
    if (id != 0x171 || !PairMatchRoom(g_base)) return {};
    return "Waiting for the other players to get READY.";
}

// The places' teams are past (a room battle start without the PAIR PLAY screen - a 1v1 of two members in a PAIR PLAY room -
// or another room): the fight's names and ranks are the game's own again (FightingName, PairRank).
void ClearPairSides(const char* why)
{
    std::lock_guard l(g_lock);
    if (!g_pairSide[0].fighter && !g_pairSide[1].fighter) return;
    g_pairSide[0] = PairSide{};
    g_pairSide[1] = PairSide{};
    std::fprintf(stderr, "[pair-names] the PAIR PLAY teams cleared: %s\n", why);
}

// ---- the game's main thread, every frame
namespace { bool wasRoomFrame = false; }        // the room's frame was shown last frame
void TeamFrame(PPCContext& ctx, uint8_t* base, const std::string& frame)
{
    const int64_t now = Ms();
    // the room: entered, left
    const uint64_t session = XSessionCurrentId();
    {
        std::lock_guard l(g_lock);
        if (session != g_session)
        {
            if (g_session && CurrentRole() != Role::None && !session) { SendLeave(); EndTeam("left the room"); }
            g_session = session;
            g_members.clear();
            if (g_pairSide[0].fighter || g_pairSide[1].fighter)
            {
                g_pairSide[0] = PairSide{};
                g_pairSide[1] = PairSide{};
                std::fprintf(stderr, "[pair-names] the PAIR PLAY teams cleared: another room\n");
            }
        }
    }
    // this player's own online character ranks: from the save object, outside matches (they do not change in one)
    {
        static int64_t ranksAt = 0;
        const uint32_t mode = CurrentMode();
        if (session && mode != 8 && mode != 9 && mode != 18 && now - ranksAt >= 2000)
        {
            ranksAt = now;
            uint8_t ranks[63];
            if (rank_sync::OwnOnlineRanks(ctx, base, ranks))
            {
                std::lock_guard l(g_lock);
                if (!g_haveOwnRanks || std::memcmp(g_ownRanks, ranks, sizeof(ranks)) != 0)
                {
                    int ranked = 0, highest = 0;
                    for (const uint8_t r : ranks) { ranked += r != 0; highest = (std::max)(highest, int(r)); }
                    std::fprintf(stderr, "[pair-ranks] own ranks: %d character(s) ranked, highest %d\n", ranked, highest);
                    std::memcpy(g_ownRanks, ranks, sizeof(ranks));
                    g_haveOwnRanks = true;
                }
            }
        }
        if (session) PairRankShadows();
    }
    // Hello once a second while in a room
    static int64_t helloAt = 0;
    if (session && now - helloAt >= 1000)
    {
        helloAt = now;
        Hello h{};
        h.magic = kHello;
        h.session = session;
        h.xuid = xlive::OnlineXuid(0);
        const std::string name = MyName();
        std::memcpy(h.name, name.data(), (std::min)(name.size(), sizeof(h.name)));
        h.role = uint8_t(CurrentRole());
        {
            std::lock_guard l(g_lock);
            const auto it = g_members.find(Teammate());
            h.teammate = it != g_members.end() ? it->second.xuid : 0;
            h.character = -1;
            h.side = int8_t(g_mySide);
            h.ranksValid = g_haveOwnRanks ? 1 : 0;
            std::memcpy(h.ranks, g_ownRanks, sizeof(h.ranks));
        }
        uint64_t peers[8];
        const size_t n = XSocketRoomPeers(peers, 8, 0);
        for (size_t i = 0; i < n; ++i) XSocketSend2v2(peers[i], &h, sizeof(h));
    }
    // waiting windows - not over a match: a box takes every press (one left from the room sat over the fight and its
    // players could not move, 2026-11-04), so as character select or the fight begins the one showing is closed and the
    // waiting ones are dropped (the room's hints are old news by then)
    {
        const uint32_t mode = CurrentMode();
        const bool match = mode == 8 || mode == 9 || mode == 18;
        static bool wasMatch = false, ours = false;
        if (ours && !SysUiBoxBusy()) ours = false;
        if (match && !std::exchange(wasMatch, true))
        {
            if (ours)
            {
                SysUiDismissBox();
                ours = false;
                std::fprintf(stderr, "[2v2] box closed: the match began\n");
            }
            if (!g_boxes.empty()) std::fprintf(stderr, "[2v2] %zu waiting box(es) dropped: the match began\n", g_boxes.size());
            g_boxes.clear();
        }
        else if (!match) wasMatch = false;
        if (!match && !g_boxes.empty() && !SysUiBoxBusy() && !menu_host::WindowShowing(ctx, base))
        {
            SysUiMessageBox b = std::move(g_boxes.front());
            g_boxes.pop_front();
            const std::string first = b.text.substr(0, b.text.find('\n'));
            if (!SysUiShowMessageBox(b)) g_boxes.push_front(std::move(b));
            else
            {
                ours = true;
                std::fprintf(stderr, "[2v2] box shown: %.60s\n", first.c_str());
            }
        }
    }
    if (PlacesTakeResult()) ApplyPlaces();
    // the host: the even delay of each match, once (the log)
    {
        static bool shown = false;
        const int even = EvenDelayMs();
        if (even > 0 && !shown) std::fprintf(stderr, "[2v2] even delay %d ms (round trip to the partner %.0f ms)\n", even, g_rttMs);
        shown = even > 0 ? true : (ForwardRemote() ? shown : false);
    }
    // the host: its partner's round trip, twice a second (the even delay below)
    static int64_t pingAt = 0;
    if (CurrentRole() == Role::Host && Teammate() && now - pingAt >= 500)
    {
        pingAt = now;
        const Ping p{ kPing, now };
        XSocketSend2v2(Teammate(), &p, sizeof(p));
    }
    // a teammate gone quiet (its game closed, its connection lost): the team ends - after the match when one is on (the
    // partner's character then stands still; the game's own rules end a match whose fighter left)
    if (const uint64_t mate = Teammate(); mate && CurrentRole() != Role::None)
    {
        int64_t seen = 0;
        {
            std::lock_guard l(g_lock);
            const auto it = g_members.find(mate);
            seen = it != g_members.end() ? it->second.seen : g_teamSince;
        }
        // no message box (the teams are made again for every match; the room's log says who left)
        if (now - (std::max)(seen, g_teamSince) > 8000 && !ForwardRemote())
        {
            std::lock_guard l(g_lock);
            EndTeam("the teammate is gone");
        }
    }
    // A PAIR PLAY room (online_2v2_pair.cpp): its session panel says so.
    if (frame != "FrameOnlinePlayerMatchRoom") { wasRoomFrame = false; return; }
    const bool fresh = std::exchange(wasRoomFrame, true) == false;
    const bool pair = PairRoom();
    static int shownPair = -1;
    if (fresh || int(pair) != shownPair)
    {
        if (pair) menu_host::SetText(ctx, base, "LabelRoomInfo", "PAIR PLAY Session");
        shownPair = int(pair);
    }
}

} // namespace online2v2

// ---- character select (online_2v2_pick.cpp keeps a team's two PCs in step)
namespace online2v2 {
namespace {
// A select cursor object (sub_8227EAA8(obj, side)): one per side; in pair play the side's two players take turns on it.
uint32_t g_cursor[2] = {};
bool g_selecting = false;                      // CharaSelectMode is in state_main (the cursors are live)
} // namespace

void NoteStateObject(uint32_t object, const std::string& name);   // online_2v2_select.cpp

// The game's state machines name each state they enter (sub_825FE600 hook below).
void NoteState(const std::string& name)
{
    const bool selecting = name == "&CharaSelectMode::Impl::state_main";
    if (name.rfind("&CharaSelectMode::", 0) == 0 || name.rfind("&OnlineInRoom::", 0) == 0) g_selecting = selecting;
}

uint32_t SelectCursor(int side) { return side == 0 || side == 1 ? g_cursor[side] : 0; }
int MySide() { return g_mySide; }
bool Selecting() { return g_selecting; }

// Remote controller input counts only in a fight (the room and character select are the host's: there the partner sees
// "Please wait." - its presses would move the host's cursors).
bool ForwardRemote()
{
    const uint32_t state = g_base ? GRead32(g_base, 0x82EAC03Cu) : 0;
    return state >= 1 && state <= 8;
}
bool MaskLocalPad() { return CurrentRole() == Role::Partner && ForwardRemote(); }

// The host, in a fight: how long its own controller is held back so both teammates play with the same delay - the
// partner's presses reach this game half a round trip late (port/ONLINE_2V2_PLAN.md "Fairness"). 0 otherwise.
// TTT2_2V2_EVEN=0 turns it off.
int EvenDelayMs()
{
    static const bool off = [] { const char* e = std::getenv("TTT2_2V2_EVEN"); return e && *e == '0'; }();
    if (off || CurrentRole() != Role::Host || !ForwardRemote() || g_rttMs < 0) return 0;
    return (std::min)(int(g_rttMs / 2 + 0.5), 150);
}
} // namespace online2v2

extern "C" PPC_FUNC(__imp__sub_825FE600);
PPC_FUNC(sub_825FE600)
{
    const uint32_t machine = ctx.r3.u32, name = ctx.r4.u32;
    std::string s;
    for (uint32_t i = 0; i < 96 && name; ++i)
    {
        const char c = char(GRead8(base, name + i));
        if (!c) break;
        s += c;
    }
    online2v2::NoteState(s);
    online2v2::NoteStateObject(machine, s);
    online2v2::PairRoomState(s);
    rank_sync::NoteStateName(machine, s);
    rank_sync::OnStateNamed(ctx, base, s);
    unlock_all::NoteState(machine, s);
    rematch::NoteState(s);
    static const bool trace = [] { const char* e = std::getenv("TTT2_2V2_DIAG"); return e && *e == '1'; }();
    if (trace)
    {
        static std::mutex lock;
        static std::map<uint32_t, std::string> last;
        std::lock_guard l(lock);
        std::string& was = last[machine];
        if (was != s)
        {
            std::fprintf(stderr, "[state] t=%llu %08X %s\n", online2v2::DiagMs(), machine, s.c_str());
            was = s;
        }
    }
    __imp__sub_825FE600(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_8227EAA8);
PPC_FUNC(sub_8227EAA8)
{
    const uint32_t obj = ctx.r3.u32, side = ctx.r4.u32;
    __imp__sub_8227EAA8(ctx, base);
    if (side < 2) online2v2::g_cursor[side] = obj;
    static const bool trace = [] { const char* e = std::getenv("TTT2_2V2_DIAG"); return e && *e == '1'; }();
    if (trace) std::fprintf(stderr, "[2v2] select cursor %08X made for side %u\n", obj, side);
}

// ---- the name under a side's health bar (user, 2026-11-04): in a PAIR PLAY fight it is the teammate who is fighting. The
// fight HUD's plates take each side's name every frame from the battle's player record of that side (sub_823D2080 ->
// sub_82657AD8(record): the record's identity at +276 by sub_824165C8; the records [[0x82E6C5A8] + 256] + 60 + 4 * side,
// +276 a player's XUID - the team host's, MEASURED 2026-11-04 in a fight; a partner's own PC showed the partner's on its side,
// ONLINE_2V2_PLAN.md). The side's fighter (0x82E88878 + 4 * side) +7792 is 0 while the partner's character is on point
// (ONLINE_2V2_PLAN.md 0.2: every tag flips it) - the partner's name then, the team host's else. The rank under it:
// HudRankSlot below.
namespace online2v2 {
namespace {
constexpr uint32_t kGame = 0x82E6C5A8u, kFighterTable = 0x82E88878u;

// A name as guest text (32 bytes, made once per name).
uint32_t GuestName(const std::string& name)
{
    static std::mutex lock;
    static std::map<std::string, uint32_t> texts;
    std::lock_guard l(lock);
    uint32_t& text = texts[name];
    if (!text)
    {
        uint8_t* p = static_cast<uint8_t*>(g_virtualHeap.Alloc(32));
        if (!p) return 0;
        std::memset(p, 0, 32);
        std::memcpy(p, name.data(), (std::min)(name.size(), size_t(19)));
        text = uint32_t(p - g_base);
    }
    return text;
}

// The name a fight's side record shows instead of its own (0: its own).
uint32_t FightingName(uint8_t* base, uint32_t record)
{
    if (!record || CurrentMode() != 9 || !PairRoom()) return 0;
    const uint32_t game = GRead32(base, kGame);
    const uint32_t players = game ? GRead32(base, game + 256) : 0;
    int side = -1;
    for (int s = 0; s < 2 && players; ++s)
        if (GRead32(base, players + 60 + 4 * s) == record) side = s;
    if (side < 0) return 0;
    const uint32_t fighter = GRead32(base, kFighterTable + 4 * side);
    const bool partnerOn = fighter && GRead32(base, fighter + 7792) == 0;
    const uint64_t xuid = (uint64_t(GRead32(base, record + 276)) << 32) | GRead32(base, record + 280);
    std::string name;
    {
        std::lock_guard l(g_lock);
        for (const PairSide& p : g_pairSide)
            if (xuid && p.partner && (p.fighter == xuid || p.partner == xuid)) name = partnerOn ? p.partnerName : p.fighterName;
    }
    static std::string shown[2] = { "-", "-" };
    if (shown[side] != name)
    {
        std::fprintf(stderr, "[pair-names] side %d under the health bar: %s (%s on point)\n", side,
                     name.empty() ? "the record's own name" : name.c_str(), partnerOn ? "the partner" : "the team host");
        shown[side] = name;
    }
    return name.empty() ? 0 : GuestName(name);
}
} // namespace

// ---- the character ranks a PAIR PLAY fight shows (user, 2026-11-09: "it shows from two players only"). The game keeps
// one player record per side ([[0x82E6C5A8] + 256] + 60 + 4 * side) with two fighter slots (+640, 4288 bytes each: +0
// the character, +4180 its online rank), both filled from that record's own character table (sub_8265A3B0) - the team
// host's on every PC but the partner's own, where its side holds the partner's. So each PC showed two players' ranks.
// Here each character's rank is its controller's own (its Hello's table, rank_sync::OwnOnlineRanks): only what the fight
// HUD and the VS screen read is changed, through port-owned copies - the records themselves (saved, uploaded, written
// back by sub_82659F48) are never touched. PAIR PLAY rooms only; a side of one player (2v1) keeps the game's own.
namespace {
constexpr uint32_t kSlotBytes = 4288, kRecordBytes = 16176;
uint32_t g_hudShadow[2][2] = {};               // a slot copy per side and slot (the HUD reads only its +4180)
uint32_t g_vsShadow[2] = {};                   // a record copy per side (the VS panel reads the record)

// The record's side (-1: not a side's).
int SideOfRecord(uint8_t* base, uint32_t record)
{
    const uint32_t game = GRead32(base, kGame);
    const uint32_t players = game ? GRead32(base, game + 256) : 0;
    for (int s = 0; s < 2 && players; ++s)
        if (GRead32(base, players + 60 + 4 * s) == record) return s;
    return -1;
}

// The rank of character `character` in a side record's slot `idx` (0 the team host's, 1 the partner's) as its
// controller has it; -1: the game's own (not PAIR PLAY, a side of one, no table from that player). `who`: their name.
int PairRank(uint8_t* base, uint32_t record, int idx, int character, std::string* who = nullptr)
{
    if (!record || idx < 0 || idx > 1 || character < 0 || character >= 63 || !PairRoom()) return -1;
    const uint64_t xuid = (uint64_t(GRead32(base, record + 276)) << 32) | GRead32(base, record + 280);
    const uint64_t me = xlive::OnlineXuid(0);
    if (!xuid) return -1;
    std::lock_guard l(g_lock);
    uint64_t player = 0;
    for (const PairSide& p : g_pairSide)
        if (p.partner && (p.fighter == xuid || p.partner == xuid))
        {
            player = idx ? p.partner : p.fighter;
            if (who) *who = idx ? p.partnerName : p.fighterName;
        }
    if (!player) return -1;
    if (player == me) return g_haveOwnRanks ? g_ownRanks[character] : -1;
    for (const auto& [peer, m] : g_members)
        if (m.xuid == player && m.haveRanks) return m.ranks[character];
    return -1;
}

void LogRank(const char* where, int side, int idx, int character, int rank, int native, const std::string& who)
{
    static std::mutex lock;
    static std::map<int, int> last;                 // (where, side, slot) -> character << 8 | rank
    std::lock_guard l(lock);
    const int key = (where[0] == 'H' ? 0 : 4) + side * 2 + idx, value = character << 8 | rank;
    const auto it = last.find(key);
    if (it != last.end() && it->second == value) return;
    last[key] = value;
    std::fprintf(stderr, "[pair-ranks] %s side %d: %s's %s (character %d) rank %d (the record's %d)\n", where, side,
                 who.empty() ? "?" : who.c_str(), CharacterName(character).c_str(), character, rank, native);
}
} // namespace

// The copies, made once (TeamFrame, in a room: never inside a match or a rewind).
void PairRankShadows()
{
    if (g_vsShadow[1]) return;
    for (auto& side : g_hudShadow)
        for (uint32_t& slot : side)
            if (uint8_t* p = static_cast<uint8_t*>(g_virtualHeap.Alloc(kSlotBytes))) { std::memset(p, 0, kSlotBytes); slot = uint32_t(p - g_base); }
    for (uint32_t& rec : g_vsShadow)
        if (uint8_t* p = static_cast<uint8_t*>(g_virtualHeap.Alloc(kRecordBytes))) { std::memset(p, 0, kRecordBytes); rec = uint32_t(p - g_base); }
}

// The fight HUD's rank under the name (sub_823D1C68: op[] sub_822C9BB8 on record + 640 at lr 0x823D1E00 / 0x823D1E24,
// then lbz +4180 of the slot it returns - and nothing else of it): the slot to read instead (0: the game's own).
uint32_t HudRankSlot(uint8_t* base, uint32_t record, uint32_t idx, uint32_t slot)
{
    if (!slot || idx > 1) return 0;
    const int character = GRead8(base, slot);
    std::string who;
    const int rank = PairRank(base, record, int(idx), character, &who);
    const int native = GRead8(base, slot + 4180);
    if (rank < 0 || rank == native) return 0;
    const int side = SideOfRecord(base, record);
    const uint32_t shadow = side >= 0 ? g_hudShadow[side][idx] : 0;
    if (!shadow) return 0;
    std::memcpy(base + shadow, base + slot, kSlotBytes);
    GWrite8(base, shadow + 4180, uint8_t(rank));
    LogRank("HUD", side, int(idx), character, rank, native, who);
    return shadow;
}

// The VS screen's character panel (sub_826357C8: the side's record from sub_82653478 at lr 0x82635854; it reads the
// slots' characters +640 / +4928 and online ranks +4820 / +9108, the type byte, +37): the record to read instead.
uint32_t VsRankRecord(uint8_t* base, uint32_t side, uint32_t record)
{
    if (!record || side > 1 || !PairRoom() || !g_vsShadow[side]) return 0;
    const int c0 = GRead8(base, record + 640), c1 = GRead8(base, record + 640 + kSlotBytes);
    std::string who0, who1;
    const int r0 = PairRank(base, record, 0, c0, &who0), r1 = PairRank(base, record, 1, c1, &who1);
    const int n0 = GRead8(base, record + 640 + 4180), n1 = GRead8(base, record + 640 + kSlotBytes + 4180);
    if ((r0 < 0 || r0 == n0) && (r1 < 0 || r1 == n1)) return 0;
    const uint32_t shadow = g_vsShadow[side];
    std::memcpy(base + shadow, base + record, kRecordBytes);
    if (r0 >= 0) { GWrite8(base, shadow + 640 + 4180, uint8_t(r0)); LogRank("VS", int(side), 0, c0, r0, n0, who0); }
    if (r1 >= 0) { GWrite8(base, shadow + 640 + kSlotBytes + 4180, uint8_t(r1)); LogRank("VS", int(side), 1, c1, r1, n1, who1); }
    return shadow;
}

// This PC's player's slot in side `side`'s record: 0 the side's fighter (pad slot 9 / 10, its first pick), 1 its partner
// (11 / 12, the second pick); -1 not a PAIR PLAY side of two with this player on it (1v1, a side of one, a watcher, a
// broken team, not a PAIR PLAY room).
int PairOwnSlot(int side)
{
    if ((side != 0 && side != 1) || !PairRoom() || !Teammate()) return -1;
    const uint64_t me = xlive::OnlineXuid(0);
    const Role role = CurrentRole();
    if (!me || role == Role::None) return -1;
    std::lock_guard l(g_lock);
    const PairSide& p = g_pairSide[side];
    if (!p.fighter || !p.partner) return -1;
    if (p.fighter == me && role == Role::Host) return 0;
    if (p.partner == me && role == Role::Partner) return 1;
    return -1;
}

// ... by the match's places alone (this player's XUID on side `side` of a side of two; no teammate or role asked): the
// same answer through a whole fight - a teammate leaving mid-match ends the team at once (EndTeam), not the match's places.
int PairPlacesSlot(int side)
{
    if (side != 0 && side != 1) return -1;
    const uint64_t me = xlive::OnlineXuid(0);
    if (!me) return -1;
    std::lock_guard l(g_lock);
    const PairSide& p = g_pairSide[side];
    if (!p.fighter || !p.partner) return -1;
    return p.fighter == me ? 0 : p.partner == me ? 1 : -1;
}
} // namespace online2v2

extern "C" PPC_FUNC(__imp__sub_822C9BB8);
PPC_FUNC(sub_822C9BB8)
{
    const uint32_t lr = uint32_t(ctx.lr), array = ctx.r3.u32, idx = ctx.r4.u32;
    __imp__sub_822C9BB8(ctx, base);
    if (lr != 0x823D1E00u && lr != 0x823D1E24u) return;
    if (const uint32_t slot = online2v2::HudRankSlot(base, array - 640, idx, ctx.r3.u32)) ctx.r3.u64 = slot;
}

extern "C" PPC_FUNC(__imp__sub_82657AD8);
PPC_FUNC(sub_82657AD8)
{
    const uint32_t record = ctx.r3.u32;
    __imp__sub_82657AD8(ctx, base);
    if (const uint32_t text = online2v2::FightingName(base, record)) ctx.r3.u64 = text;
}
