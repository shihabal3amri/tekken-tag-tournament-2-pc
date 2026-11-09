// Online 2v2: a partner's character select (online_2v2.h, port/ONLINE_2V2_PLAN.md).
//
// Online, character select runs on each PC on its own (no sync-engine frames while picking; each fighter sees only its
// own side, the other side's picks are shown at the VS screen). OnlineInRoom::state_battle_start (sub_825FB650) sets the
// local side's "entered" flag (sub_82356140 on [0x82E6C5A8]+240) and opens CHARASELECT for a fighter; a spectator
// (sub_8266C778: node 0 +748) takes another branch ("Please wait."), and the select screen's controller (sub_82280888)
// sends it to its spectator state. The sync engine's own spectator/side answers (sub_826EF3D8 / sub_826EF378, under the
// wrappers sub_8266C778 / sub_8266C740) are left alone everywhere else.
#include "display_settings.h"
#include "online_2v2.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

extern "C" PPC_FUNC(__imp__sub_8266C778);
extern "C" PPC_FUNC(__imp__sub_8266C740);

namespace online2v2 {
namespace {

constexpr uint32_t kSyncNodes = 0x82E6C5C8u + 64 + 4828;

bool LocalSpectator(uint8_t* base)
{
    const uint32_t self = GRead32(base, kSyncNodes);
    return self && GRead8(base, self + 748);
}

// The side a spectator's character select is told it plays (-1: none, the game's own spectator screen): a 2v2 partner's
// team side (the PAIR PLAY places: red 0, blue 1, as its leader's); the research switch TTT2_2V2_SELTEST=<0|1> for any
// spectator. Every side question during its select gets it: one left out (sub_826684A8, which swaps the pad slots when
// the match's side differs from the one the setup assumed) left a red partner's cursor without a controller (MEASURED
// 2026-10-25).
int SelTestSide()
{
    static const int test = [] { const char* e = std::getenv("TTT2_2V2_SELTEST"); return e && (*e == '0' || *e == '1') ? *e - '0' : -1; }();
    if (CurrentRole() == Role::Partner && Teammate() && MySide() >= 0) return MySide();
    return test;
}

// The callers that decide what the select screen shows (return addresses).
const std::set<uint32_t> kSpectatorAsk = { 0x825FB728u /* battle start */, 0x822809D4u /* select controller, state 0 */ };
const std::set<uint32_t> kSideAsk = { 0x825FB784u /* battle start */, 0x825CB6B4u /* select setup */, 0x82280DF4u /* select controller */,
                                       0x826684CCu /* the setup's pad slot swap */ };
bool g_charaSelectAny = false;        // a CharaSelectMode state runs (every side question then gets the partner's side)

} // namespace

// The side a partner's select questions are told (-1: none).
int PartnerSide() { return SelTestSide(); }

// This PC's sync-engine node is a spectator's (partners, a broken team's partner): the room's nodes are set up (the node
// count NG+16+64+4856, as sub_826F5AD0 tests it) and this PC's own node (+4828) has its spectator flag (+748).
bool SpectatorNode(uint8_t* base)
{
    if (int32_t(GRead32(base, 0x82E6C5C8u + 64 + 4856)) <= 0) return false;
    return LocalSpectator(base);
}
} // namespace online2v2

namespace online2v2 {
uint32_t SelectCursor(int side);    // online_2v2_team.cpp
bool Selecting();

// Research (TTT2_2V2_SELDIAG=1), every frame while character select runs: the words of each side's cursor object that
// changed (the first 0x400 bytes), to learn which say the mode, the turn and the picks.
void SelectDiag(uint8_t* base)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_2V2_SELDIAG"); return e && *e == '1'; }();
    if (!on) return;
    static uint32_t last[2][256];
    static uint32_t lastObj[2] = {};
    static int lines = 0;
    if (!Selecting()) { lastObj[0] = lastObj[1] = 0; return; }
    for (int s = 0; s < 2; ++s)
    {
        const uint32_t obj = SelectCursor(s);
        if (!obj) continue;
        if (obj != lastObj[s])
        {
            for (int i = 0; i < 256; ++i) last[s][i] = GRead32(base, obj + 4 * i);
            lastObj[s] = obj;
            continue;
        }
        for (int i = 0; i < 256; ++i)
        {
            const uint32_t v = GRead32(base, obj + 4 * i);
            if (v == last[s][i]) continue;
            if (i == 0x10 / 4 || i == 0x3C / 4 || i == 0x198 / 4 || i == 0x19C / 4) { last[s][i] = v; continue; }   // timers
            if (++lines < 4000)
                std::fprintf(stderr, "[sel] t=%llu side %d +%03X: %08X -> %08X\n", online2v2::DiagMs(), s, 4 * i, last[s][i], v);
            last[s][i] = v;
        }
    }
}
} // namespace online2v2

namespace online2v2 {
namespace {
uint32_t g_roomImpl = 0;              // OnlineInRoom::Impl while its room_main runs (0: not in a room)
uint32_t g_modeManager = 0;           // the mode manager every mode asks (its Impl + 120 / + 124), once seen
}

// Every state a game state machine enters (sub_825FE600 hook, online_2v2_team.cpp): the room's object, for asking the
// mode manager for a mode as its own states do ([Impl + 120]->vtable[1](manager, mode, flags, 0)).
uint32_t g_padAssignImpl = 0;         // PadAssignMode::Impl while its main state runs (research)
bool PadAssignOpen() { return g_padAssignImpl != 0; }   // the PAIR PLAY screen takes presses (online_2v2_places.cpp)
uint32_t PadAssignImpl() { return g_padAssignImpl; }

void NoteStateObject(uint32_t object, const std::string& name)
{
    if (name.rfind("&CharaSelectMode::", 0) == 0) g_charaSelectAny = name != "&CharaSelectMode::Impl::state_end";
    else if (name.rfind("&GameMode::", 0) == 0 || name.rfind("&OnlineInRoom::", 0) == 0) g_charaSelectAny = false;
    if (name == "&PadAssignMode::Impl::state_main") g_padAssignImpl = object;
    else if (name.rfind("&PadAssignMode::", 0) == 0) g_padAssignImpl = 0;
    if (name == "&OnlineInRoom::Impl::state_room_main")
    {
        g_roomImpl = object;
        g_modeManager = GRead32(g_base, object + 120);
    }
    else if (name.rfind("&OnlineInRoom::", 0) == 0) g_roomImpl = 0;
}

// Research (TTT2_2V2_SELDIAG=1): the PadAssign sequence (PadAssignMode::Impl +144, a shared_ptr: +0 object, +4 count)
// while the stones screen runs - its vtable once, then every changed word of the object and of what its first words
// point at (the model / drawer), to learn where the four controllers' places are kept.
void PadAssignDiag(uint8_t* base)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_2V2_SELDIAG"); return e && *e == '1'; }();
    if (!on || !g_padAssignImpl) return;
    const uint32_t seq = GRead32(base, g_padAssignImpl + 144);
    if (!seq) return;
    constexpr int kObjs = 24, kWords = 96;
    static uint32_t lastSeq = 0, objs[kObjs], last[kObjs][kWords], parent[kObjs], at[kObjs];
    static int count = 0, lines = 0, settle = 0;
    auto isPtr = [](uint32_t v) { return v >= 0x00100000u && v < 0x20000000u && !(v & 3); };
    auto add = [&](uint32_t v, uint32_t from, uint32_t off) {
        if (!isPtr(v) || count >= kObjs) return;
        for (int k = 0; k < count; ++k)
            if (objs[k] == v) return;
        parent[count] = from;
        at[count] = off;
        objs[count++] = v;
    };
    if (seq != lastSeq || settle == 30)       // (re)collect: the sequence, what it points at, and one level below
    {
        if (seq != lastSeq) settle = 0;
        lastSeq = seq;
        count = 0;
        add(seq, 0, 0);
        for (int i = 1; i < 16; ++i) add(GRead32(base, seq + 4 * i), 0, 4 * i);
        const int firstLevel = count;
        for (int k = 1; k < firstLevel; ++k)
            for (int i = 1; i < 24; ++i) add(GRead32(base, objs[k] + 4 * i), k, 4 * i);
        for (int k = 0; k < count; ++k)
        {
            std::fprintf(stderr, "[pad] object %d %08X vtable %08X (object %u +%03X)\n", k, objs[k], GRead32(base, objs[k]), parent[k], at[k]);
            for (int i = 0; i < kWords; ++i) last[k][i] = GRead32(base, objs[k] + 4 * i);
        }
    }
    ++settle;
    for (int k = 0; k < count; ++k)
        for (int i = 0; i < kWords; ++i)
        {
            const uint32_t v = GRead32(base, objs[k] + 4 * i);
            if (v == last[k][i]) continue;
            if (++lines < 3000)
                std::fprintf(stderr, "[pad] t=%llu object %d +%03X: %08X -> %08X\n", online2v2::DiagMs(), k, 4 * i, last[k][i], v);
            last[k][i] = v;
        }
}

uint32_t ModeManager() { return g_modeManager; }
bool InRoomMain() { return g_roomImpl != 0; }

// Research (TTT2_2V2_DIAG=1), every frame while the room's main state runs: the words of the room's control (CRoomControl,
// [[room Impl +132] +344]) that changed (its first 0x400 bytes; +232 counts frames), to find its READY and countdown.
void RoomDiag(uint8_t* base)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_2V2_DIAG"); return e && *e == '1'; }();
    if (!on) return;
    static uint32_t last[256], lastObj = 0;
    static int lines = 0;
    static uint16_t perWord[256 + 240];          // lines per word: one that keeps changing is left out after 30
    const uint32_t manager = g_roomImpl ? GRead32(base, g_roomImpl + 132) : 0;
    const uint32_t obj = manager ? GRead32(base, manager + 344) : 0;
    if (!obj) { lastObj = 0; return; }
    if (obj != lastObj)
    {
        for (int i = 0; i < 256; ++i) last[i] = GRead32(base, obj + 4 * i);
        lastObj = obj;
        std::fprintf(stderr, "[room-diag] t=%llu control %08X (manager %08X)\n", online2v2::DiagMs(), obj, manager);
        return;
    }
    for (int i = 0; i < 256; ++i)
    {
        const uint32_t v = GRead32(base, obj + 4 * i);
        if (v == last[i]) continue;
        if (i != 232 / 4 && i != 0x10 / 4 && i != 0xF4 / 4 && ++perWord[i] <= 30 && ++lines < 6000)
            std::fprintf(stderr, "[room-diag] t=%llu +%03X: %08X -> %08X\n", online2v2::DiagMs(), 4 * i, last[i], v);
        last[i] = v;
    }
    // the room data ([control +52] +328: members at +20 + 108 * i, +668 the match info: +32 phase, +36 the host's
    // countdown frames), every word but the countdown
    static uint32_t lastData[240], lastDataObj = 0;
    const uint32_t data = GRead32(base, GRead32(base, obj + 52) + 328);
    if (!data) return;
    if (data != lastDataObj)
    {
        for (int i = 0; i < 240; ++i) lastData[i] = GRead32(base, data + 4 * i);
        lastDataObj = data;
        std::fprintf(stderr, "[room-diag] t=%llu data %08X\n", online2v2::DiagMs(), data);
        return;
    }
    for (int i = 0; i < 240; ++i)
    {
        const uint32_t v = GRead32(base, data + 4 * i);
        if (v == lastData[i]) continue;
        if (i != (668 + 36) / 4 && ++perWord[256 + i] <= 30 && ++lines < 6000)
            std::fprintf(stderr, "[room-diag] t=%llu data +%03X: %08X -> %08X\n", online2v2::DiagMs(), 4 * i, lastData[i], v);
        lastData[i] = v;
    }
    static uint32_t lastCount = 0;
    const uint32_t count = GRead32(base, data + 668 + 36);
    if (count / 60 != lastCount / 60)
        std::fprintf(stderr, "[room-diag] t=%llu countdown frames %u\n", online2v2::DiagMs(), count);
    lastCount = count;
}
uint32_t PlacesModeAsked(PPCContext& ctx, uint8_t* base, uint32_t mode, uint32_t& flags);   // online_2v2_places.cpp

// Research (TTT2_2V2_SELDIAG=1): C:/ttt2/port/padtest<TTT2_INSTANCE>.txt ("<mode> <flags hex>") present while the room's
// main state runs -> the room asks for that mode (default PADASSIGN, 22, with the PAIR sequence: 0x80).
void PlacesTestFrame(PPCContext& ctx, uint8_t* base)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_2V2_SELDIAG"); return e && *e == '1'; }();
    PadAssignDiag(base);
    if (!on || !g_modeManager) return;
    static int tick = 0;
    if (++tick % 30) return;
    const char* tag = std::getenv("TTT2_INSTANCE");
    const std::string path = std::string("C:/ttt2/port/padtest") + (tag ? tag : "") + ".txt";
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    unsigned mode = 22, flags = 0x80;
    if (std::fscanf(f, "%u %x", &mode, &flags) < 1) { mode = 22; flags = 0x80; }
    std::fclose(f);
    std::remove(path.c_str());
    const uint32_t manager = g_modeManager;
    const uint32_t fn = manager ? GRead32(base, GRead32(base, manager) + 4) : 0;
    std::fprintf(stderr, "[2v2-sel] the room asks for mode %u flags %08X (manager %08X, fn %08X)\n", mode, flags, manager, fn);
    if (!fn) return;
    PPCContext c = ctx;
    c.r3.u64 = manager;
    c.r4.u64 = mode;
    c.r5.u64 = flags;
    c.r6.u64 = 0;
    PPC_LOOKUP_FUNC(base, fn)(c, base);
}
} // namespace online2v2

// The room as a match starts (sub_825FB810): a member it takes for a watcher (sub_826BAED0 from 0x825FB85C) gets the
// room control's state 6 - its chat box stays up over the character select and the fight - and the notice "Please
// wait."; a fighter state 4 and neither. In a PAIR PLAY match room the match's four players are no watchers
// (online_pair_room.cpp PairWatcher; MEASURED 2026-10-25: a partner with state 6 kept the notice on its select).
extern "C" PPC_FUNC(__imp__sub_826BAED0);
PPC_FUNC(sub_826BAED0)
{
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_826BAED0(ctx, base);
    if (lr != 0x825FB860u) return;
    const int pair = online2v2::PairWatcher(ctx, base);
    if (pair >= 0 && uint32_t(pair) != (ctx.r3.u32 & 0xFF))
    {
        std::fprintf(stderr, "[2v2-sel] the match starts: this PC %s\n", pair ? "watches" : "plays (no watcher to the room)");
        ctx.r3.u64 = uint32_t(pair);
    }
}

// The mode manager asked for a mode (r3 manager, r4 mode, r5 flags, r6 0): the online PAIR PLAY screen's end starts the
// room's match (online2v2::PlacesModeAsked: CHARASELECT with the room's flags), then the room's start / rematch code.
extern "C" PPC_FUNC(__imp__sub_825ED560);
PPC_FUNC(sub_825ED560)
{
    const uint32_t mode = ctx.r4.u32;
    uint32_t flags = ctx.r5.u32;
    PPCContext c = ctx;
    uint32_t to = online2v2::PlacesModeAsked(c, base, mode, flags);
    to = rematch::ModeAsked(base, to, flags);
    if (to != mode || flags != ctx.r5.u32)
    {
        ctx.r4.u64 = to;
        ctx.r5.u64 = flags;
    }
    __imp__sub_825ED560(ctx, base);
}

// The mode manager's factory (r3: +8 the mode asked for, +12.. its parameters) - every mode change. The mode it makes is
// the game's from then on (CurrentMode, online_2v2.h: menu_ext.cpp's Customize lines); research: each new one is logged.
namespace online2v2 {
std::atomic<uint32_t> g_currentMode{ ~0u };
uint32_t CurrentMode() { return g_currentMode.load(); }
extern std::atomic<uint32_t> g_selectController;
std::atomic<bool> g_quickStartLoad{ false };   // QUICK START: the first title still owes the game's start-up loading
}
extern "C" PPC_FUNC(__imp__sub_825EEBD0);
extern "C" PPC_FUNC(__imp__sub_82622C98);
PPC_EXTERN_FUNC(sub_82625EC0);   // TitleMode: start its loading thread (state_bng_init's; impl+212 once it runs)
PPC_FUNC(sub_825EEBD0)
{
    const uint32_t mgr = ctx.r3.u32;
    static uint32_t lastMode = ~0u;
    const uint32_t mode = GRead32(base, mgr + 8);
    online2v2::g_currentMode = mode;
    // QUICK START: the first title (mode 5 from the boot mode, +12 0) takes the way back to the title, +20 = 1 (MEASURED
    // 2026-11-04: Return to Title's mode 5 has it): state_wait goes on to state_delayed_initwait1, past the logos and the
    // intro movie. That way never starts the loading thread that loads the menus: sub_82622C98's hook below does.
    if (mode == 5 && mode != lastMode && GRead32(base, mgr + 12) == 0 && GRead32(base, mgr + 20) == 0 && display_settings::GetQuickStart())
    {
        GWrite32(base, mgr + 20, 1);
        online2v2::g_quickStartLoad = true;
        std::fprintf(stderr, "[title] quick start: straight to the title\n");
    }
    if (mode != lastMode)
    {
        online2v2::g_selectController = 0;     // the last select's object is gone (a new select makes its own)
        std::fprintf(stderr, "[2v2-sel] mode factory: mode %u (+12 %08X +16 %08X +20 %08X +24 %08X)\n", mode, GRead32(base, mgr + 12),
                     GRead32(base, mgr + 16), GRead32(base, mgr + 20), GRead32(base, mgr + 24));
        lastMode = mode;
    }
    __imp__sub_825EEBD0(ctx, base);
}

// TitleMode's step that the logo and waiting states run each frame: impl+196 once sign-in has settled, which
// state_delayed_initwait1 waits for. A QUICK START boot gets there without state_bng_init, so its loading thread starts
// here (sub_826231E0: what the main menu needs, the menus' documents among it; MEASURED 2026-11-04: without it the main
// menu has no rows), and the title waits for the thread's impl+133, as the normal boot's state_havok_term does. Return
// to Title has all of that loaded and does not come here.
PPC_FUNC(sub_82622C98)
{
    if (online2v2::g_quickStartLoad.load(std::memory_order_relaxed))
    {
        const uint32_t impl = ctx.r3.u32;
        if (!GRead8(base, impl + 212))
        {
            sub_82625EC0(ctx, base);
            ctx.r3.u64 = impl;
        }
        if (!GRead8(base, impl + 133)) return;
        online2v2::g_quickStartLoad = false;
        std::fprintf(stderr, "[title] quick start: the game's loading is done\n");
    }
    __imp__sub_82622C98(ctx, base);
}

// The select screen's controller (r3, every frame of character select; +20 its state, +48 / +56 the two cursors, +84..86
// flags; the stage select: +0x138 1 while a stage is chosen (2 once it is), +0x12C the stage cursor - 11 to a row,
// MEASURED 2026-10-25). Research (TTT2_2V2_SELDIAG=1): its states and the words that change.
namespace online2v2 {
std::atomic<uint32_t> g_selectController{ 0 };
uint32_t SelectController() { return g_selectController.load(); }
}
extern "C" PPC_FUNC(__imp__sub_82280888);
PPC_FUNC(sub_82280888)
{
    const uint32_t obj = ctx.r3.u32;
    online2v2::g_selectController = obj;
    straight::SelectUpdate(ctx, base, obj);    // STAGE SELECT from a room match's end menu: the kept characters
    const uint32_t phaseBefore = obj ? GRead32(base, obj + 20) : 0;
    __imp__sub_82280888(ctx, base);
    const uint64_t ret = ctx.r3.u64;          // the select's own return value (the mirror calls guest code after it)
    straight::SelectWatch(base, obj);         // ... and its stage panel on the way (diagnostics)
    stagemirror::Frame(ctx, base, obj, phaseBefore);   // the stage choice on the PC that does not choose it
    ctx.r3.u64 = ret;
    static const bool on = [] { const char* e = std::getenv("TTT2_2V2_SELDIAG"); return e && *e == '1'; }();
    if (!on) return;
    static uint32_t lastObj = 0, lastState = ~0u, lastFlags = ~0u;
    const uint32_t state = GRead32(base, obj + 20), flags = GRead32(base, obj + 84);
    if (obj != lastObj || state != lastState || flags != lastFlags)
        std::fprintf(stderr, "[sel] t=%llu controller %08X state %u flags %08X cursors %08X %08X\n", online2v2::DiagMs(), obj,
                     state, flags, GRead32(base, obj + 48), GRead32(base, obj + 56));
    // the controller's own words that change (the first 0x300 bytes; the stage select's cursor is not in the side cursors)
    static uint32_t words[0xC0];
    static uint16_t changes[0xC0];             // a word that keeps changing (a timer) is left out after a dozen
    static int lines = 0;
    if (obj != lastObj)
        for (uint32_t i = 0; i < 0xC0; ++i) words[i] = GRead32(base, obj + 4 * i), changes[i] = 0;
    else
        for (uint32_t i = 0; i < 0xC0; ++i)
        {
            const uint32_t v = GRead32(base, obj + 4 * i);
            if (v == words[i]) continue;
            if (++changes[i] <= 12 && ++lines < 600)
                std::fprintf(stderr, "[sel] t=%llu controller +%03X: %08X -> %08X (state %u)\n", online2v2::DiagMs(), 4 * i,
                             words[i], v, state);
            words[i] = v;
        }
    lastObj = obj;
    lastState = state;
    lastFlags = flags;
}

PPC_FUNC(sub_8266C778)
{
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_8266C778(ctx, base);
    using namespace online2v2;
    if (SelTestSide() < 0 || !(ctx.r3.u32 & 0xFF)) return;
    static std::set<uint32_t> seen;                                    // research: every caller, once
    if (seen.insert(lr).second) std::fprintf(stderr, "[2v2-sel] spectator? first asked from %08X\n", lr);
    static const std::set<uint32_t> extra = [] {
        std::set<uint32_t> s;
        if (const char* e = std::getenv("TTT2_2V2_SELNO"))           // research: more callers told no (hex, comma-separated)
            for (const char* p = e; *p;)
            {
                char* end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 16);
                if (end == p) break;
                s.insert(uint32_t(v));
                p = *end == ',' ? end + 1 : end;
            }
        return s;
    }();
    if (!kSpectatorAsk.count(lr) && !extra.count(lr)) return;
    ctx.r3.u64 = 0;
    static std::set<uint32_t> told;
    if (told.insert(lr).second) std::fprintf(stderr, "[2v2-sel] spectator? asked from %08X: told no\n", lr);
}

PPC_FUNC(sub_8266C740)
{
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_8266C740(ctx, base);
    using namespace online2v2;
    // ... and a partner's REMATCH: the exchange's apply (sub_826689C8's two calls) keeps its own lent profile on its side
    // (online_straight.cpp, the partner's rewards)
    const bool rematchApply = straight::PairRematchApply() && (lr == 0x82668A70u || lr == 0x82668AD8u);
    if (SelTestSide() < 0 || !LocalSpectator(base) || !(kSideAsk.count(lr) || g_charaSelectAny || rematchApply)) return;
    static std::set<uint32_t> told;
    if (told.insert(lr).second) std::fprintf(stderr, "[2v2-sel] side? asked from %08X: %d, told %d\n", lr, int(ctx.r3.u32), SelTestSide());
    ctx.r3.u64 = uint32_t(SelTestSide());
}
