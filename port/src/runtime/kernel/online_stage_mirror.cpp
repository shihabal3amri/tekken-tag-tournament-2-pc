// Online character select: the stage choice shown on the PC that does not choose it (2026-11-07).
//
// In an online select one PC chooses the stage (the room order's second fighter - the cursor step's state 22 -> 27, the
// select page's stage panel opens on that PC only). The other PC's cursor is done at once (22 -> 39): it sends its battle
// setup and shows the game's wait message 1330 ("Your opponent is selecting a character/stage. Please wait one
// moment.") over the character grid until the chooser's setup - with the stage - arrives. Players read it as "selecting a
// character" while the opponent picks the stage. Here, as offline Versus shows it:
//   - the chooser's PC tells the room its stage phase and its stage cursor (C+312 / C+300 / C+316, ten times a second and
//     on every change - an optional packet: an older build drops it and the other PC keeps the game's own screen);
//   - the other PC, its own picks sent and waiting (select phase 4, its cursor done), opens the select page's own stage
//     panel for the chooser's side (C+316, C+300, C+312 = 1 - the panel's cue; the panel plays face_all_out, brings the
//     stage grid in and opens its stage step gate C+320, which nothing runs here: the stage step is cursor state 27's)
//     and moves the cursor where the chooser's is; the panel's pad-slot test for the chooser's side (sub_8228DA18 at
//     0x826430C0: a remote side has no pad slot here) answers that side's slot, so the panel draws its red / blue tags as
//     for a local chooser;
//   - the random cells' preview cycles through the stages as the chooser's does (its roulette step, no random number);
//   - the wait text says what is happening: the game's own text 379 ("The opponent is selecting a stage.\nPlease wait.",
//     in all eight languages, unused by the game) in place of 1330 while the chooser is in its stage phase - in the
//     game's shown window, changed in place as the game changes a network message it shows;
//   - once the exchange is done (phase 4 -> 5) S+772 holds the chooser's stage (sub_826689C8): C+312 = 2 - the panel
//     shows the chosen stage, as the chooser's does, until the select leaves.
// The wait message on every PC that waits for the battle setup's exchange (select phase 4) says what it waits for, and
// only ever moves on: the game's 1330 while the other side may still pick characters; 379 while the other side's chooser
// is in its stage phase; the game's own 300 ("Waiting for response from opponent.\nPlease wait until the match
// begins.") once the other side is done too - only the exchange is left (about 4 s after a straight STAGE SELECT's
// choice, the end menu's REMATCH...). A side is done when its leader's select waits (phase 4): that PC tells the room
// ("GSd1", five times a second - a packet an older build drops).
// A PAIR PLAY partner's screen follows its leader's stage choice (online_2v2_pick.cpp: the cursor). The stage step reads
// only pad slot 9 + side - user 0 on a partner's PC, which plays only the port's presses (its own controller is user 1;
// online2v2::PartnerStageHold keeps it still there as well, a safeguard) - and when the leader chooses, the partner's
// panel takes the same cell and confirms it (PartnerStep), the leader's stage set in its pick's place (StageConfirmed),
// so both teammates see the chosen stage and the wait. The partner's entry is a spectator's: the match takes the
// fighters'.
// None of these words is part of the battle setup, the exchange or the rollback state: no fight state changes. The port
// never draws the fight RNG itself (sub_8227D2C0, the roulette's draw, is never called). A partner's confirm runs the
// game's own entry send (sub_82668F58), which steps the LCG at [0x82E6B05C]+16 as every online select's entry send does;
// that entry is a spectator's, and the fight follows the fighters' (ONLINE_2V2_PLAN.md measurements; 0 DESYNC m140).
// Research: C:\ttt2wt\re\stage2 (synth.txt, verify.txt; workflow wf_7d0ff325-6ac).
#include "online_2v2.h"
#include "xlive.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>

uint64_t XSessionCurrentId();                  // xsession.cpp: the room session (0: not in a room)

PPC_EXTERN_FUNC(sub_8266E2E8);   // the network message window: its text in place (r4 the text, r5 1 - W's slot 1)
PPC_EXTERN_FUNC(sub_82382A68);   // a window's size (r4 width, r5 height, r6 0 at once / 1 grown to it)
PPC_EXTERN_FUNC(sub_825B7370);   // the match settings S (+772 the stage)
extern "C" PPC_FUNC(__imp__sub_82259EC0);     // a text by id (r3 id | bank slot << 24)
extern "C" PPC_FUNC(__imp__sub_8228DA18);     // a side's pad slot (9..12; 0 none)
extern "C" PPC_FUNC(__imp__sub_8227B2F0);     // the stage step (1: a stage confirmed)
extern "C" PPC_FUNC(__imp__sub_8241EBC0);     // the dialog window W: show a text
extern "C" PPC_FUNC(__imp__sub_8241ED88);     // ... W's other openers
extern "C" PPC_FUNC(__imp__sub_8241E138);
extern "C" PPC_FUNC(__imp__sub_8241F1D8);
extern "C" PPC_FUNC(__imp__sub_8241E1F8);
extern "C" PPC_FUNC(__imp__sub_8241E328);
extern "C" PPC_FUNC(__imp__sub_8241EFB0);
extern "C" PPC_FUNC(__imp__sub_8241F6A0);

namespace stagemirror {
void TimerConfirmed(PPCContext& ctx, uint8_t* base, uint32_t C);   // below: the partner's select timer confirmed first
namespace {

using online2v2::DiagMs;

constexpr uint32_t kMagic = 0x32765347u;       // "GSv2"
constexpr uint32_t kDoneMagic = 0x31645347u;   // "GSd1": a side is done - its leader's select waits for the exchange
constexpr unsigned long long kFreshMs = 500;   // a packet older than this does not open the panel / change the text
constexpr unsigned long long kStaleMs = 1000;  // ... nor move the cursor
constexpr unsigned long long kDoneEveryMs = 200;
constexpr uint32_t kTextWait = 1330, kTextStage = 379, kTextDone = 300;
constexpr int kCells = 33;                     // the stage grid's cells (0x8200C8BC -> 0x82DC6760, 32-byte cells)

#pragma pack(push, 1)
struct Packet
{
    uint32_t magic;
    uint64_t session;                          // XSessionCurrentId()
    uint32_t nonce;                            // new for every select of the chooser
    uint16_t seq;
    int8_t side;                               // the chooser's side (C+316)
    int8_t phase;                              // C+312: 1 choosing, 2 chosen (a "GSd1" packet: 3)
    int8_t cell;                               // C+300 (-1 in a "GSd1" packet)
    int8_t stage;                              // S+772 once chosen, -1 before
};
#pragma pack(pop)

// ---- a waiting leader's PC: its side is done (main thread)
struct DoneSender
{
    uint32_t controller = 0, nonce = 0, packets = 0;
    uint16_t seq = 0;
    unsigned long long sentAt = 0;
};
DoneSender g_doneSend;

// ---- a PAIR PLAY partner's PC: its panel confirms its leader's choice (main thread)
struct PartnerConfirm
{
    uint32_t controller = 0, nonce = 0;
    int cell = -1, stage = -1;                 // the leader's choice, kept once heard (its packets stop with its select)
    bool have = false, gateLogged = false, capLogged = false, confirmed = false;
    bool stageSet = false;                     // S+772 holds the leader's stage (StageConfirmed or TimerConfirmed)
    bool lateLogged = false;                   // the exchange came first (TimerConfirmed's diagnostics)
    bool lockLogged = false;                   // the leader's cell is locked here (PressCell's diagnostics)
    const char* waitWhy = nullptr;             // why no choice is heard yet (diagnostics)
    int presses = 0;
    unsigned long long pressAt = 0;
};
constexpr int kPartnerPressCap = 25;           // about 10 s of presses, 400 ms apart, while the stage step's gate is open
constexpr unsigned long long kPartnerPressEveryMs = 400;
PartnerConfirm g_partner;

// ---- the chooser's PC (main thread)
struct Sender
{
    uint32_t controller = 0, nonce = 0, packets = 0;
    uint16_t seq = 0;
    int phase = 0, cell = -1;
    unsigned long long sentAt = 0;
};
Sender g_send;

// ---- what the chooser's PC said (the network thread writes, the main thread reads)
std::mutex g_lock;
struct Heard
{
    Packet p{};
    unsigned long long at = 0;
    bool any = false;
};
Heard g_heard;                                 // the chooser's stage packets ("GSv2")
Heard g_done[2];                               // each side's "done" ("GSd1")

// ---- the other PC (main thread)
struct Mirror
{
    bool on = false, chosen = false, gateLogged = false, staleLogged = false, picked = false;
    uint32_t controller = 0, page = 0, nonce = 0, cells = 0;
    int chooser = -1, cell = -1;
    uint32_t panel = ~0u;
    unsigned long long heardAt = 0;            // the last packet followed
};
Mirror g_m;
uint32_t g_page = 0, g_pageController = 0;     // the last select page built (sub_82643770) and its controller

// ---- the wait message (main thread): the game's show of 1330 and what the window holds since. The dialog object W
// ([0x82EDECE8]) holds two windows - slot 0 (W+180: the general dialogs, our own boxes) and slot 1 (W+184: the network
// messages - sub_8266E3C8 shows the wait message there); every W method takes the slot from W+204 (1: slot 1) and clears it.
constexpr uint32_t kW = 0x82EDECE8u;
constexpr uint32_t kWinOpen = 0x8B30u;         // a window: open (set by the open sub_82419BB0, cleared when it is gone)
constexpr uint32_t kWinMode = 0x8B4Cu;         // ... its kind (2: a message with the spinner, sub_8241EBC0's)
uint32_t g_winGen = 0;                         // every show on W's slot 1
struct Wait
{
    uint32_t controller = 0, slot = 0, gen = 0, text = 0;
    bool slotKnown = false, shown = false, over = false;   // over: no more changes (the window or the select moved on)
    const char* why = nullptr;                 // why the last change could not be made (logged on change)
};
Wait g_wait;
bool g_waitArmed = false;                      // the next show on W is the wait message's

uint32_t Call(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint64_t r3 = 0, uint64_t r4 = 0)
{
    PPCContext c = ctx;
    c.r3.u64 = r3;
    c.r4.u64 = r4;
    fn(c, base);
    return c.r3.u32;
}

uint32_t NewNonce()
{
    static std::mt19937 rng{ std::random_device{}() };
    uint32_t n = 0;
    while (!n) n = rng();
    return n;
}

void SendAll(const void* data, size_t size)
{
    uint64_t peers[8];
    const size_t n = XSocketRoomPeers(peers, 8, 0);
    for (size_t i = 0; i < n; ++i) XSocketSend2v2(peers[i], data, size);
}

// The latest packet from this room's session and its age; false: none.
bool Latest(Packet& p, unsigned long long& age)
{
    unsigned long long at = 0;
    {
        std::lock_guard l(g_lock);
        if (!g_heard.any) return false;
        p = g_heard.p;
        at = g_heard.at;
    }
    const unsigned long long now = DiagMs();
    age = now > at ? now - at : 0;
    return p.session == XSessionCurrentId();
}

unsigned long long HeardAt()
{
    std::lock_guard l(g_lock);
    return g_heard.at;
}

// The chooser in its stage phase on the other side of this PC (`mySide`): the wait text's test (phase 1 or 2) and the
// panel's (1 only - once chosen the exchange is about to end).
bool Choosing(const Packet& p, unsigned long long age, int mySide, bool openOnly)
{
    if (age >= kFreshMs || p.side < 0 || p.side > 1 || p.side == mySide) return false;
    return openOnly ? p.phase == 1 : (p.phase == 1 || p.phase == 2);
}

// The other side of this PC (`mySide`) is done: its leader's select waits for the exchange (a fresh "GSd1" of this
// room's session), or its chooser chose (a fresh phase-2 stage packet).
bool OtherDone(int mySide)
{
    if (mySide < 0 || mySide > 1) return false;
    Heard d;
    {
        std::lock_guard l(g_lock);
        d = g_done[1 - mySide];
    }
    const unsigned long long now = DiagMs();
    if (d.any && d.p.session == XSessionCurrentId() && now - (std::min)(now, d.at) < kFreshMs) return true;
    Packet p;
    unsigned long long age = 0;
    return Latest(p, age) && age < kFreshMs && p.side == 1 - mySide && p.phase == 2;
}

// The wait text that says what this PC waits for (`why`: for the log).
uint32_t WantText(int mySide, const char*& why)
{
    Packet p;
    unsigned long long age = 0;
    if (Latest(p, age) && Choosing(p, age, mySide, true))
    {
        why = "the other side's chooser is in its stage phase";
        return kTextStage;
    }
    if (OtherDone(mySide))
    {
        why = "the other side is done - only the exchange is left";
        return kTextDone;
    }
    why = "the other side may still pick";
    return kTextWait;
}

// The texts' order: the wait text only ever moves on.
int Rank(uint32_t text) { return text == kTextDone ? 2 : text == kTextStage ? 1 : 0; }

// An online select's controller (+85 online, +86 the search's select, +87 a random stage).
bool OnlineSelect(uint8_t* base, uint32_t C) { return GRead8(base, C + 85) && !GRead8(base, C + 86) && !GRead8(base, C + 87); }

// This PC's cursor is done (39) and its stage phase never started: the PC that does not choose.
bool Waiting(uint8_t* base, uint32_t C, int mySide)
{
    if (mySide < 0 || mySide > 1 || GRead32(base, C + 312) != 0) return false;
    const uint32_t K = GRead32(base, C + (mySide ? 56 : 48));
    return K && GRead32(base, K + 312) == 39;
}

// A leader's PC (or a 1v1 fighter's) whose select waits for the exchange (phase 4): its side is done - to the room,
// five times a second while it waits.
void DoneSend(uint8_t* base, uint32_t C)
{
    if (!OnlineSelect(base, C) || GRead32(base, C + 20) != 4) return;
    if (online2v2::CurrentRole() == online2v2::Role::Partner) return;   // the leader tells it for the team
    if (online2v2::SpectatorNode(base)) return;                         // any sync-engine spectator: never a side's word
    const int mySide = online2v2::ScreenSide(base);
    if (mySide < 0 || mySide > 1) return;
    const unsigned long long now = DiagMs();
    if (g_doneSend.controller != C)
    {
        g_doneSend = DoneSender{};
        g_doneSend.controller = C;
        g_doneSend.nonce = NewNonce();
        std::fprintf(stderr, "[stage-mirror] this side (%d) is done: telling the room until the exchange ends\n", mySide);
    }
    else if (now - g_doneSend.sentAt < kDoneEveryMs)
        return;
    g_doneSend.sentAt = now;
    const Packet p{ kDoneMagic, XSessionCurrentId(), g_doneSend.nonce, g_doneSend.seq++, int8_t(mySide), 3, -1, -1 };
    SendAll(&p, sizeof(p));
    ++g_doneSend.packets;
}

// The stage list's index of `stage` (C+92.., C+304 - 1 of them), -1 none.
int StageIndex(uint8_t* base, uint32_t C, int stage)
{
    const int n = int32_t(GRead32(base, C + 304)) - 1;
    for (int i = 0; i < n && i < 64; ++i)
        if (int32_t(GRead32(base, C + 92 + 4 * uint32_t(i))) == stage) return i;
    return -1;
}

// The stage grid (the select screens' fifth: {table, count} at 0x8200C8BC + 32 - online_2v2_pick.cpp's kGrids): 32-byte
// cells, +4 the cell's stage (51: a random cell). The stage step reads it as sub_8227B2F0 does.
constexpr uint32_t kStageGrid = 0x8200C8BCu + 32u;
int CellStage(uint8_t* base, int cell)
{
    const uint32_t table = GRead32(base, kStageGrid), count = GRead32(base, kStageGrid + 4);
    if (table < 0x1000 || cell < 0 || uint32_t(cell) >= count || cell >= kCells) return -1;
    return int32_t(GRead32(base, table + 32u * uint32_t(cell) + 4));
}

// The cell the partner's press confirms on: the leader's - unless its stage is locked on this PC (stage unlocks are each
// PC's own; the stage list holds only the unlocked ones, sub_8227D2C0, by the press's own test, sub_823577B0): the stage
// step refuses a press there (0x8227B41C -> 0x8227B464). A random cell takes any press (0x8227B494: the list's C+88) -
// the first one then; StageConfirmed puts the leader's stage and cell in after it.
int PressCell(uint8_t* base, uint32_t C, int cell)
{
    const int stage = CellStage(base, cell);
    if (stage < 0 || stage == 51 || StageIndex(base, C, stage) >= 0) return cell;
    for (int i = 0; i < kCells; ++i)
        if (CellStage(base, i) == 51)
        {
            if (!g_partner.lockLogged)
                std::fprintf(stderr, "[stage-mirror] the leader's cell %d holds stage %d, locked on this PC: this partner's press goes on the random cell %d\n",
                             cell, stage, i);
            g_partner.lockLogged = true;
            return i;
        }
    return cell;
}

// A PAIR PLAY partner's PC in its team's stage phase (C+312 1: its screen follows the leader's cursor,
// online_2v2_pick.cpp): once the leader chose, the partner's panel takes the leader's cell (and on a random cell the
// leader's stage as the roulette's preview, C+88) and confirms it - its user 0 (pad slot 9 + side, the only slot the
// stage step reads, sub_8227B2F0), which plays only the port's presses here - while the stage step's gate is open (C+320:
// a press before it is lost). The confirm's own pick (a random cell's roulette, an unlock filter) is set right after it
// to the leader's stage (StageConfirmed: the stage step's hook) - the panel shows the leader's choice. The partner's
// entry is a spectator's: the match takes the fighters'.
void PartnerStep(PPCContext& ctx, uint8_t* base, uint32_t C)
{
    if (online2v2::CurrentRole() != online2v2::Role::Partner || !OnlineSelect(base, C)) return;
    if (g_m.on && g_m.controller == C) return;
    // this team's side: in its stage phase the panel's own (C+316: the chooser's side - this PC's team, the other team's
    // panel being the mirror's), before it the pad slots' (ScreenSide)
    const uint32_t phaseNow = GRead32(base, C + 312);
    const int mySide = phaseNow == 1 || phaseNow == 2 ? int32_t(GRead32(base, C + 316)) : online2v2::ScreenSide(base);
    Packet p;
    unsigned long long age = 0;
    if (Latest(p, age) && age < kFreshMs && p.side == mySide && p.phase == 2 && p.stage >= 0 && p.cell >= 0 && p.cell < kCells)
    {
        if (g_partner.controller != C || g_partner.nonce != p.nonce)
        {
            g_partner = PartnerConfirm{};
            g_partner.controller = C;
            g_partner.nonce = p.nonce;
        }
        g_partner.cell = p.cell;
        g_partner.stage = p.stage;
        g_partner.have = true;
    }
    if (!g_partner.have || g_partner.controller != C)
    {
        // diagnostics: this partner's stage phase runs, no choice of its leader heard yet (logged on change)
        if (GRead32(base, C + 312) == 1)
        {
            const bool any = Latest(p, age);
            const char* why = !any                 ? "no stage packet of this session"
                              : age >= kFreshMs    ? "its packets are old"
                              : p.side != mySide   ? "the packets are the other side's"
                              : p.phase != 2       ? "the leader is choosing"
                                                   : "the packet has no stage or cell";
            if (why != g_partner.waitWhy)
                std::fprintf(stderr,
                             "[stage-mirror] this partner waits for its leader's choice: %s (this side %d, the pad slots' %d; packet side %d, phase %d, %llu ms old)\n",
                             why, mySide, online2v2::ScreenSide(base), any ? p.side : -1, any ? p.phase : -1, any ? age : 0ull);
            g_partner.waitWhy = why;
        }
        return;
    }
    const uint32_t stagePhase = GRead32(base, C + 312);
    if (stagePhase != 1)
    {
        if (stagePhase == 2 && g_partner.presses && !g_partner.confirmed)
        {
            g_partner.confirmed = true;
            std::fprintf(stderr, "[stage-mirror] this partner's panel confirmed the leader's stage %d after %d press(es)\n",
                         g_partner.stage, g_partner.presses);
        }
        if (stagePhase == 2 && !g_partner.stageSet) TimerConfirmed(ctx, base, C);
        return;
    }
    const int index = StageIndex(base, C, g_partner.stage);
    if (index >= 0) GWrite32(base, C + 88, uint32_t(index));
    const bool gate = GRead32(base, C + 320) != 0;
    const int cell = gate ? PressCell(base, C, g_partner.cell) : g_partner.cell;   // the press's cell once it can come
    if (int32_t(GRead32(base, C + 300)) != cell) GWrite32(base, C + 300, uint32_t(cell));
    if (!gate)
    {
        if (!g_partner.gateLogged)
            std::fprintf(stderr, "[stage-mirror] the leader chose stage %d (cell %d): this partner's panel confirms it once its stage step takes input\n",
                         g_partner.stage, g_partner.cell);
        g_partner.gateLogged = true;
        return;
    }
    const unsigned long long now = DiagMs();
    if (g_partner.presses >= kPartnerPressCap)
    {
        if (!g_partner.capLogged)
            std::fprintf(stderr, "[stage-mirror] this partner's panel did not take the confirm (%d presses) - the match goes on without it\n",
                         g_partner.presses);
        g_partner.capLogged = true;
        return;
    }
    if (g_partner.presses && now - g_partner.pressAt < kPartnerPressEveryMs) return;
    online2v2::SynthPress(0, 0x1000 /* A */, 80, 0);
    ++g_partner.presses;
    g_partner.pressAt = now;
    if (g_partner.presses == 1)
        std::fprintf(stderr, "[stage-mirror] the leader chose stage %d (cell %d, list index %d): this partner's panel confirms it\n",
                     g_partner.stage, g_partner.cell, index);
}

// The chooser's PC: its stage phase to the room.
void SendStep(PPCContext& ctx, uint8_t* base, uint32_t C)
{
    const int phase = int32_t(GRead32(base, C + 312));
    if (phase != 1 && phase != 2) return;
    if (g_m.on && g_m.controller == C) return;                         // the panel opened here: never told on
    if (!GRead8(base, C + 85) || GRead8(base, C + 86)) return;
    if (online2v2::CurrentRole() == online2v2::Role::Partner) return;  // PAIR PLAY: the chooser team's leader tells it
    if (g_send.controller != C)
    {
        if (g_send.packets)
            std::fprintf(stderr, "[stage-mirror] send stopped (the select changed, %u packets)\n", g_send.packets);
        g_send = Sender{};
        g_send.controller = C;
        g_send.nonce = NewNonce();
    }
    const int cell = int32_t(GRead32(base, C + 300));
    const int side = int32_t(GRead32(base, C + 316));
    const unsigned long long now = DiagMs();
    if (phase == g_send.phase && cell == g_send.cell && now - g_send.sentAt < 100) return;
    if (phase != g_send.phase)
        std::fprintf(stderr, "[stage-mirror] send: phase %d side %d cell %d (nonce %08X)\n", phase, side, cell, g_send.nonce);
    else if (cell != g_send.cell)
        std::fprintf(stderr, "[stage-mirror] send: cell %d\n", cell);
    g_send.phase = phase;
    g_send.cell = cell;
    g_send.sentAt = now;
    int stage = -1;
    if (phase == 2)
        if (const uint32_t S = Call(ctx, base, sub_825B7370)) stage = int32_t(GRead32(base, S + 772));
    const Packet p{ kMagic, XSessionCurrentId(), g_send.nonce, g_send.seq++, int8_t(side), int8_t(phase), int8_t(cell), int8_t(stage) };
    SendAll(&p, sizeof(p));
    ++g_send.packets;
}

// The wait message's window to the size its open gives it for the text now in it (sub_82419BB0, kind 2: width the
// text's + 50, at least 350; height the text's + the spinner's + 30), grown to it as the window grows.
void FitWait(PPCContext& ctx, uint8_t* base, uint32_t win)
{
    const int32_t tw = int16_t(GRead16(base, win + 2964 + 42)), th = int16_t(GRead16(base, win + 2964 + 44));
    const int32_t sh = int16_t(GRead16(base, win + 31728 + 44));
    const int32_t w = (std::max)(350, tw + 50), h = th + sh + 30;
    const int32_t ow = int16_t(GRead16(base, win + 38)), oh = int16_t(GRead16(base, win + 40));
    if (w == ow && h == oh) return;
    PPCContext c = ctx;
    c.r3.u64 = win;
    c.r4.u64 = uint64_t(w);
    c.r5.u64 = uint64_t(h);
    c.r6.u64 = 1;
    sub_82382A68(c, base);
    std::fprintf(stderr, "[stage-mirror] wait window %dx%d -> %dx%d\n", ow, oh, w, h);
}

// The wait text `id` in the window that shows the game's wait message for this select - in place, as the game changes a
// network message it shows (sub_82602DF8: sub_8266E2E8(-, text, 1)); only while that window still holds it (open, not
// closing, a message with the spinner, nothing shown on slot 1 since) and the select still waits (C+20 4: the game's own
// close at 0x82281104 comes later). Null: changed.
const char* ChangeWait(PPCContext& ctx, uint8_t* base, uint32_t C, uint32_t id)
{
    if (g_wait.controller != C || !g_wait.shown || !g_wait.slotKnown) return "no wait message of this select";
    if (g_winGen != g_wait.gen) return "the window shows something else since";
    if (GRead32(base, C + 20) != 4) return "the select stopped waiting";
    const uint32_t W = GRead32(base, kW);
    const uint32_t win = W ? GRead32(base, W + 184) : 0;
    if (!win || !GRead8(base, win + kWinOpen)) return "the window is closed";
    if (!GRead8(base, win + 50)) return "the window is closing";                   // its alpha's target 0: the close
    if (GRead32(base, win + kWinMode) != 2) return "the window shows another kind of message";
    const uint32_t text = Call(ctx, base, sub_82259EC0, g_wait.slot | id);
    if (!text) return "no such text";
    PPCContext c = ctx;
    c.r3.u64 = 0;
    c.r4.u64 = text;
    c.r5.u64 = 1;
    sub_8266E2E8(c, base);
    FitWait(ctx, base, win);
    return nullptr;
}

// The wait message of this select, every frame while it shows: on to the text that says what this PC waits for (the
// panel opening, the chooser's choice, the other side done) - until it changed to the last one or can never change
// (something else on the window since, the window closing, the select done waiting).
void WaitStep(PPCContext& ctx, uint8_t* base, uint32_t C)
{
    if (g_wait.controller != C || !g_wait.shown || g_wait.over) return;
    if (GRead32(base, C + 20) != 4)
    {
        g_wait.over = true;                                            // the exchange ended: the game closes it
        return;
    }
    const char* reason = nullptr;
    const uint32_t want = WantText(online2v2::ScreenSide(base), reason);
    if (Rank(want) <= Rank(g_wait.text)) return;
    const char* why = ChangeWait(ctx, base, C, want);
    if (!why)
    {
        std::fprintf(stderr, "[stage-mirror] wait text %u -> %u (%s)\n", g_wait.text, want, reason);
        g_wait.text = want;
        g_wait.why = nullptr;
        if (Rank(want) == 2) g_wait.over = true;
        return;
    }
    if (why != g_wait.why) std::fprintf(stderr, "[stage-mirror] wait text not changed to %u yet: %s\n", want, why);
    g_wait.why = why;
    if (std::strcmp(why, "the window is closed") != 0) g_wait.over = true;   // never: something else showed, the select moved on
}

// Why the panel does not open (diagnostics: logged on every change while this PC waits).
const char* g_notOpen = nullptr;
void NotOpen(const char* why, uint8_t* base, uint32_t C, int mySide)
{
    if (why == g_notOpen) return;
    g_notOpen = why;
    if (!why) return;
    const uint32_t K = mySide == 0 || mySide == 1 ? GRead32(base, C + (mySide ? 56 : 48)) : 0;
    std::fprintf(stderr,
                 "[stage-mirror] not open: %s (side %d, cursor %u, C+312 %u, page %08X for %08X (+148 %08X, +240 %u, +252 %u), controller %08X)\n",
                 why, mySide, K ? GRead32(base, K + 312) : 0xFFFFFFFFu, GRead32(base, C + 312), g_page, g_pageController,
                 g_page ? GRead32(base, g_page + 148) : 0, g_page ? GRead8(base, g_page + 240) : 0, g_page ? GRead32(base, g_page + 252) : 0,
                 C);
}

void TryOpen(PPCContext& ctx, uint8_t* base, uint32_t C)
{
    if (!OnlineSelect(base, C) || GRead32(base, C + 20) != 4) return;
    const int mySide = online2v2::ScreenSide(base);
    if (!Waiting(base, C, mySide)) return NotOpen("this PC chooses the stage or its cursor is not done", base, C, mySide);
    if (g_pageController != C || !g_page || GRead32(base, g_page + 148) != C)
        return NotOpen("no select page of this controller", base, C, mySide);
    if (!GRead8(base, g_page + 240) || GRead32(base, g_page + 252) != 1)    // the grid's intro done, the panel idle
        return NotOpen("the page's grid intro or its panel not ready", base, C, mySide);
    Packet p;
    unsigned long long age = 0;
    if (!Latest(p, age)) return NotOpen("no stage packet of this session", base, C, mySide);
    if (!Choosing(p, age, mySide, true)) return NotOpen(age >= kFreshMs ? "the packets are old" : "the chooser is not choosing", base, C, mySide);
    if (p.cell < 0 || p.cell >= kCells) return NotOpen("the chooser's cell is out of the grid", base, C, mySide);
    NotOpen(nullptr, base, C, mySide);
    GWrite32(base, C + 316, uint32_t(p.side));
    GWrite32(base, C + 300, uint32_t(p.cell));
    GWrite32(base, C + 312, 1);                                        // the panel's cue: last
    g_m = Mirror{};
    g_m.on = true;
    g_m.controller = C;
    g_m.page = g_page;
    g_m.nonce = p.nonce;
    g_m.chooser = p.side;
    g_m.cell = p.cell;
    g_m.heardAt = HeardAt();
    std::fprintf(stderr, "[stage-mirror] open: chooser side %d cell %d page %08X panel %u (packet %llu ms old); wait text %u%s\n",
                 p.side, p.cell, g_page, GRead32(base, g_page + 252), age, g_wait.text, g_wait.shown ? "" : " (not shown yet)");
}

// The random cells' roulette: the chooser's own step of it (sub_8227A800's stage part, 0x8227A944..0x8227A9AC - from its
// select phase 2, which this PC left when it sent its picks): the next stage of the list every third update (C+16 the
// update's count), the last back to the first. No random number: the game draws only the start (sub_8227D2C0, never
// called here). C+88 is read only by the panel here (the stage step's confirm, sub_8227B2F0, is the chooser's).
void Roulette(uint8_t* base, uint32_t C)
{
    const int32_t t = int32_t(GRead32(base, C + 16));
    if ((t + 2) % 3 != 0) return;
    const int32_t top = int32_t(GRead32(base, C + 304)) - 2;
    int32_t v = int32_t(GRead32(base, C + 88)) + 1;
    if (v < 0) v = top;
    else if (v > top) v = 0;
    GWrite32(base, C + 88, uint32_t(v));
}

void MirrorStep(PPCContext& ctx, uint8_t* base, uint32_t C, uint32_t phaseBefore)
{
    const uint32_t phase = GRead32(base, C + 20);
    const uint32_t panel = GRead32(base, g_m.page + 252);
    if (panel != g_m.panel)
    {
        std::fprintf(stderr, "[stage-mirror] panel %u\n", panel);
        g_m.panel = panel;
    }
    if (!g_m.chosen && phaseBefore == 4 && phase != 4)
    {
        g_m.chosen = true;
        if (GRead8(base, C + 84))
            std::fprintf(stderr, "[stage-mirror] closed (the session ended while waiting, %u cells mirrored)\n", g_m.cells);
        else
        {
            GWrite32(base, C + 312, 2);                                // the chosen stage (S+772: the exchange's)
            const uint32_t S = Call(ctx, base, sub_825B7370);
            std::fprintf(stderr, "[stage-mirror] chosen: phase 4 -> %u, S+772 %d, C+312 = 2 (cells mirrored %u, last cell %d)\n", phase,
                         S ? int32_t(GRead32(base, S + 772)) : -1, g_m.cells, g_m.cell);
        }
    }
    if (g_m.chosen) return;
    Packet p;
    unsigned long long age = 0;
    const bool fresh = Latest(p, age) && p.nonce == g_m.nonce;
    if (fresh && p.phase == 2 && !g_m.picked)
    {
        // the chooser chose: a random cell shows the chosen stage (the chooser's panel shows it from S+772), the roulette
        // stops
        g_m.picked = true;
        if (const int index = StageIndex(base, C, p.stage); index >= 0) GWrite32(base, C + 88, uint32_t(index));
        std::fprintf(stderr, "[stage-mirror] the chooser chose stage %d (cell %d)\n", p.stage, p.cell);
    }
    if (!g_m.picked) Roulette(base, C);
    if (!fresh) return;
    if (age >= kStaleMs)
    {
        if (!g_m.staleLogged) std::fprintf(stderr, "[stage-mirror] packets stale (%llu ms) - the cursor stays\n", age);
        g_m.staleLogged = true;
        return;
    }
    g_m.staleLogged = false;
    if (p.cell >= 0 && p.cell < kCells && p.cell != g_m.cell)
    {
        GWrite32(base, C + 300, uint32_t(p.cell));
        g_m.cell = p.cell;
        ++g_m.cells;
    }
}

}  // namespace

// online2v2::Receive (the network thread): a stage packet; false when it is not one.
bool Receive(uint64_t from, const uint8_t* data, size_t size)
{
    (void)from;
    if (size < sizeof(Packet)) return false;
    Packet p;
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kDoneMagic && p.magic != kMagic) return false;
    // another room's (a PC still heard from for 5 s after a room change, xsocket.cpp) or none: consumed, never stored -
    // the senders repeat every 100-200 ms
    if (p.session != XSessionCurrentId()) return true;
    if (p.magic == kDoneMagic)
    {
        if (p.side < 0 || p.side > 1) return true;
        std::lock_guard l(g_lock);
        Heard& d = g_done[p.side];
        d.p = p;
        d.at = DiagMs();
        d.any = true;
        return true;
    }
    std::lock_guard l(g_lock);
    // a late or repeated packet of the same select keeps the newer one (and its arrival time: old data never looks fresh)
    if (g_heard.any && p.session == g_heard.p.session && p.nonce == g_heard.p.nonce && int16_t(uint16_t(p.seq - g_heard.p.seq)) <= 0)
        return true;
    g_heard.p = p;
    g_heard.at = DiagMs();
    g_heard.any = true;
    return true;
}

// sub_82643770 (online_straight.cpp's hook): a select page is built - a new select.
void PageBuilt(uint32_t page, uint32_t controller)
{
    if (g_m.on) std::fprintf(stderr, "[stage-mirror] closed (a new select; %u cells mirrored)\n", g_m.cells);
    g_m = Mirror{};
    g_page = page;
    g_pageController = controller;
    g_wait = Wait{};
    g_waitArmed = false;
    g_notOpen = nullptr;
    g_partner = PartnerConfirm{};
    g_doneSend = DoneSender{};
    // the chooser's sender keeps its nonce (a page built again mid-select must not end the other PC's mirror - it follows
    // that nonce); only its log state starts over, so "send: phase 1" prints at every select
    g_send.phase = 0;
    g_send.cell = -1;
}

// The select controller's update (online_2v2_select.cpp's sub_82280888 hook), after the original (`phaseBefore`: its +20
// before it).
void Frame(PPCContext& ctx, uint8_t* base, uint32_t controller, uint32_t phaseBefore)
{
    if (!controller) return;
    if (g_m.on && g_m.controller != controller)
    {
        std::fprintf(stderr, "[stage-mirror] closed (another select controller; %u cells mirrored)\n", g_m.cells);
        g_m = Mirror{};
    }
    SendStep(ctx, base, controller);
    DoneSend(base, controller);
    PartnerStep(ctx, base, controller);
    if (g_m.on)
        MirrorStep(ctx, base, controller, phaseBefore);
    else
        TryOpen(ctx, base, controller);
    WaitStep(ctx, base, controller);
}

bool Mirrored(uint32_t controller) { return g_m.on && g_m.controller == controller; }

// The stage step confirmed (sub_8227B2F0 returned 1 - S+772 its pick; its caller sets C+312 = 2 next, in the same
// update): on a partner's PC whose confirm is PartnerStep's, the leader's stage in its place - a random cell's roulette,
// an unlock filter or a special stage would have picked another one, and the panel shows S+772 until the exchange
// writes the match's (sub_826689C8).
// The leader's choice is known here (g_partner.have) whatever confirmed the panel: PartnerStep's press, or the select's
// timer in the same update (the timer path picks the cell's stage, a random cell's the roulette's).
void StageConfirmed(PPCContext& ctx, uint8_t* base, uint32_t C)
{
    if (online2v2::CurrentRole() != online2v2::Role::Partner) return;
    if (!g_partner.have || g_partner.controller != C || g_partner.stage < 0) return;
    const uint32_t S = Call(ctx, base, sub_825B7370);
    if (!S) return;
    const int picked = int32_t(GRead32(base, S + 772));
    if (picked != g_partner.stage) GWrite32(base, S + 772, uint32_t(g_partner.stage));
    GWrite32(base, C + 300, uint32_t(g_partner.cell));
    g_partner.stageSet = true;
    std::fprintf(stderr, "[stage-mirror] this partner's confirm: stage %d%s (the leader's%s)\n", g_partner.stage,
                 picked != g_partner.stage ? " in place of its own pick" : "", g_partner.presses ? "" : ", its timer confirmed");
}

// The select's timer confirmed this partner's panel (C+312 2, no press of PartnerStep's) before the leader's choice
// came: both PCs' timers run out together and the leader's phase-2 packet follows its own confirm. Its pick (the cell's
// stage; a random cell's, the roulette's - not the leader's) is the leader's once that packet is here (PartnerStep,
// every update): the panel shows S+772. Only while the select waits for the exchange (C+20 4): the exchange writes the
// match's stage (sub_826689C8), which is never put back.
void TimerConfirmed(PPCContext& ctx, uint8_t* base, uint32_t C)
{
    if (!g_partner.have || g_partner.controller != C || g_partner.stage < 0 || g_partner.stageSet) return;
    if (GRead32(base, C + 20) > 4)
    {
        if (!g_partner.lateLogged)
            std::fprintf(stderr, "[stage-mirror] the leader's stage %d came after this partner's exchange - its panel shows the match's\n",
                         g_partner.stage);
        g_partner.lateLogged = true;
        return;
    }
    const uint32_t S = Call(ctx, base, sub_825B7370);
    if (!S) return;
    const int picked = int32_t(GRead32(base, S + 772));
    if (picked != g_partner.stage) GWrite32(base, S + 772, uint32_t(g_partner.stage));
    GWrite32(base, C + 300, uint32_t(g_partner.cell));
    if (const int index = StageIndex(base, C, g_partner.stage); index >= 0) GWrite32(base, C + 88, uint32_t(index));
    g_partner.stageSet = true;
    std::fprintf(stderr, "[stage-mirror] this partner's timer confirmed its panel first (its pick %d): the leader's stage %d (cell %d) in its place\n",
                 picked, g_partner.stage, g_partner.cell);
}

// The panel's pad-slot test (sub_8228DA18 from 0x826430C0 - r27 the controller, r31 the page): the chooser's side's slot
// while the panel is ours. False: the game's own answer.
bool PanelSlot(uint32_t controller, uint32_t page, uint32_t& slot)
{
    if (!g_m.on || controller != g_m.controller || page != g_m.page || g_m.chooser < 0) return false;
    slot = 9u + uint32_t(g_m.chooser);
    if (!g_m.gateLogged) std::fprintf(stderr, "[stage-mirror] gate: the panel's tags for side %d (slot %u)\n", g_m.chooser, slot);
    g_m.gateLogged = true;
    return true;
}

// The game's wait text (sub_82259EC0 from 0x82280F0C, r3 1330 | slot << 24, r27 the controller): what this PC waits for
// (WantText) - 379 while the other side's chooser is in its stage phase, 300 once the other side is done.
void WaitText(PPCContext& ctx, uint8_t* base)
{
    const uint32_t C = ctx.r27.u32;
    g_wait = Wait{};
    g_wait.controller = C;
    g_wait.slot = ctx.r3.u32 & 0xFF000000u;
    g_wait.slotKnown = true;
    g_wait.text = kTextWait;
    g_waitArmed = true;
    if (!OnlineSelect(base, C))
    {
        std::fprintf(stderr, "[stage-mirror] wait text 1330 kept (not an online select)\n");
        return;
    }
    const char* reason = nullptr;
    const uint32_t want = WantText(online2v2::ScreenSide(base), reason);
    g_wait.text = want;
    if (want != kTextWait) ctx.r3.u64 = g_wait.slot | want;
    std::fprintf(stderr, "[stage-mirror] wait text %u%s (%s)\n", want, want == kTextWait ? " kept" : " in place of 1330", reason);
}

uint32_t TextWaitId() { return kTextWait; }

// A show on W (its openers' hooks, after the original; `slot1`: W+204 as they began - only slot 1's count).
void WindowShown(bool slot1)
{
    if (!slot1) return;
    ++g_winGen;
    if (g_waitArmed)
    {
        g_wait.gen = g_winGen;
        g_wait.shown = true;
        g_waitArmed = false;
    }
}

}  // namespace stagemirror

// ---- hooks

// A text by id (r3 id | bank slot << 24): the select's wait text (0x82280F00 / bl at 0x82280F08).
PPC_FUNC(sub_82259EC0)
{
    if (uint32_t(ctx.lr) == 0x82280F0Cu && (ctx.r3.u32 & 0xFFFFu) == stagemirror::TextWaitId()) stagemirror::WaitText(ctx, base);
    __imp__sub_82259EC0(ctx, base);
}

// The stage step (r3 the select controller, r4 the cursor's side; 1: a stage confirmed, S+772 set). Its only caller is
// the select controller's update (0x82280698), which sets C+312 = 2 after it.
PPC_FUNC(sub_8227B2F0)
{
    const uint32_t controller = ctx.r3.u32;
    __imp__sub_8227B2F0(ctx, base);
    if (ctx.r3.u32 == 1) stagemirror::StageConfirmed(ctx, base, controller);
}

// A side's pad slot (r3 the side's pad-slot query; 9..12, 0 none): the stage panel's test for the chosen side
// (sub_82642AE8's state 4, the call at 0x826430C0 - r27 its controller, r31 its page: nonvolatile, as they were set at
// 0x82642AFC) while the panel shows the other PC's choice.
PPC_FUNC(sub_8228DA18)
{
    uint32_t slot = 0;
    if (uint32_t(ctx.lr) == 0x826430C4u && stagemirror::PanelSlot(ctx.r27.u32, ctx.r31.u32, slot))
    {
        ctx.r3.u64 = slot;
        return;
    }
    __imp__sub_8228DA18(ctx, base);
}

// The dialog window W's openers: every show on its slot 1 counts (stagemirror::ChangeWait changes only the game's own
// wait message). W+204 as they begin: the slot they open (they clear it).
static bool WaitSlot1(uint8_t* base)
{
    const uint32_t W = GRead32(base, 0x82EDECE8u);
    return W && GRead8(base, W + 204);
}
#define STAGEMIRROR_OPENER(fn)               \
    PPC_FUNC(fn)                             \
    {                                        \
        const bool slot1 = WaitSlot1(base);  \
        __imp__##fn(ctx, base);              \
        stagemirror::WindowShown(slot1);     \
    }
STAGEMIRROR_OPENER(sub_8241EBC0)
STAGEMIRROR_OPENER(sub_8241ED88)
STAGEMIRROR_OPENER(sub_8241E138)
STAGEMIRROR_OPENER(sub_8241F1D8)
STAGEMIRROR_OPENER(sub_8241E1F8)
STAGEMIRROR_OPENER(sub_8241E328)
STAGEMIRROR_OPENER(sub_8241EFB0)
STAGEMIRROR_OPENER(sub_8241F6A0)

// ---- who chooses the stage (online_2v2.h namespace stagechooser; the user, 2026-11-09: "if it's always the same player or
// the same team selecting the stage then I think it's not fair")
//
// The game: the room's match has one stage chooser, the sync engine's game host - room order position r7 of the engine's
// start (sub_8266CD08; the room's battle start passes 1, 0x825FB6A0) - and only the host's stage reaches the fight (its
// S+772 in the game env). In Player Match the select's stage test (sub_8227F108 state 22, 0x82280508..0x82280524) gives the
// stage phase to the cursor whose side is S+754 ^ 1 - the side of order[1] (S+754 = G+13, the engine's side swap). The room
// rotates its order after every match (the winner first, the loser second), so natively the loser chooses; the end menu's
// straight paths keep the order, so order[1] chose every time (MEASURED: the blue leader chose all 8 stages of the user's
// 2026-11-08 session, twice right after winning). Now the room host's decision carries the chooser's position
// (online_rematch.cpp: the loser chooses, a draw passes it on, REMATCH keeps it) and a PAIR PLAY screen's places a coin's
// (online_2v2_places.cpp); the port's engine starts pass it as r7, and while the host is order[0] the state-22 test reads
// the chooser's side (hostPos ^ G+13) - through a copy of S whose +754 says so (the game's S is never written). Order
// position 1 is byte for byte the game's own.
extern "C" PPC_FUNC(__imp__sub_8266CD08);
extern "C" PPC_FUNC(__imp__sub_825B7370);
namespace stagechooser {
namespace {
constexpr uint32_t kG = 0x82E6C5B8u + 7488u;    // the sync engine's net game state: +13 the side swap (S+754's source)
struct Record { uint64_t session = 0; int hostPos = 1; };
Record g_record;                               // the last engine start of this PC's room (main thread)
struct Pending { uint64_t session = 0; int hostPos = 1; bool set = false; };
Pending g_watcher;                             // a PAIR PLAY watcher's next room battle start (the places' coin)
uint32_t g_shadow = 0;                         // a guest block: the state-22 test's S (only +754 read)
uint32_t g_loggedSelect = 0;
} // namespace

void Started(int hostPos, const char* how)
{
    g_record.session = XSessionCurrentId();
    g_record.hostPos = hostPos == 0 ? 0 : 1;
    std::fprintf(stderr, "[stage-chooser] %s: the stage chooser (the game host) is room order position %d\n", how, g_record.hostPos);
}

int LastHostPos() { return g_record.session && g_record.session == XSessionCurrentId() ? g_record.hostPos : 1; }

void WatcherNext(int hostPos)
{
    g_watcher.session = XSessionCurrentId();
    g_watcher.hostPos = hostPos == 0 ? 0 : 1;
    g_watcher.set = true;
}

// The select's state-22 test (sub_825B7370 at 0x82280518 - the S it reads +754 of): a copy whose +754 makes the chooser's
// side the stage side, while this room's host is order[0]. 0: the game's own S.
uint32_t TestSettings(uint8_t* base, uint32_t S)
{
    if (!S || !g_record.session || g_record.session != XSessionCurrentId() || g_record.hostPos != 0) return 0;
    if (!g_shadow) g_shadow = xlive::GuestAlloc(1024);
    if (!g_shadow) return 0;
    const int swap = GRead8(base, kG + 13) & 1;
    const int side = g_record.hostPos ^ swap;
    GWrite8(base, g_shadow + 754, uint8_t(side ^ 1));
    if (g_loggedSelect != uint32_t(online2v2::DiagMs() / 10000))
    {
        g_loggedSelect = uint32_t(online2v2::DiagMs() / 10000);
        std::fprintf(stderr, "[stage-chooser] the select's stage test: side %d chooses (host order position 0, swap %d; the game's S+754 %u)\n",
                     side, swap, GRead8(base, S + 754));
    }
    return g_shadow;
}

} // namespace stagechooser

// NG: a session from the room order (r3 NG, r4 the session, r5 D, r6 2, r7 the game host's order position) - the room's
// battle start's call (0x825FB6A4): a PAIR PLAY watcher's host is the places' (stagechooser::WatcherNext); the start is
// recorded. Quick / ranked matches (0x82612F74) and the port's own starts pass by (those record themselves).
PPC_FUNC(sub_8266CD08)
{
    if (uint32_t(ctx.lr) != 0x825FB6A8u)
    {
        __imp__sub_8266CD08(ctx, base);
        return;
    }
    int pos = 1;
    if (stagechooser::g_watcher.set && stagechooser::g_watcher.session == XSessionCurrentId() && online2v2::PairMatchRoom(base))
        pos = stagechooser::g_watcher.hostPos;
    stagechooser::g_watcher.set = false;
    ctx.r7.u64 = uint64_t(pos);
    __imp__sub_8266CD08(ctx, base);
    if (ctx.r3.u32 & 0xFF) stagechooser::Started(pos, "the room's battle start");
}

// The match settings S - its state-22 read (0x82280518): stagechooser::TestSettings.
PPC_FUNC(sub_825B7370)
{
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_825B7370(ctx, base);
    if (lr == 0x82280518u)
        if (const uint32_t shadow = stagechooser::TestSettings(base, ctx.r3.u32)) ctx.r3.u64 = shadow;
}
