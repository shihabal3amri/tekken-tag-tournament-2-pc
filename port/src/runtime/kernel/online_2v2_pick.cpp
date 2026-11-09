// Online 2v2: a team's character select kept in step on its two PCs (online_2v2.h, port/ONLINE_2V2_PLAN.md).
//
// Online, character select runs on each PC on its own. A team's leader and partner both see its side's PAIR select (the
// partner's game thanks to online_2v2_select.cpp). The side's cursor object (sub_8227EAA8: +0x38 its cell, +0x154 the picks
// made, +0x158 whose turn: 0 the first player, 1 the second) is shared by the side's two players, who pick in turn, as in
// offline PAIR PLAY. Each pick is made on one PC - the first on the leader's (its own controller), the second on the
// partner's (its own controller, which is its game's user 1 there) - and the other PC's cursor is driven to the same cell
// by the port's presses and confirms when the owner did. The leader's game is the one whose picks count (its side's
// inputs go to the match); the partner's only shows them.
#include "online_2v2.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace online2v2 {
uint32_t SelectCursor(int side);               // online_2v2_team.cpp
uint32_t SelectController();                   // online_2v2_select.cpp: the select screen's controller (0: none yet)
bool Selecting();
namespace {

int64_t Ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr uint32_t kPick = 0x3276504Bu;        // "2vPK"
#pragma pack(push, 1)
struct Pick
{
    uint32_t magic;
    uint32_t selectNo;                          // the sender's own count of character selects (the log)
    int8_t turn, picks, cell, pick0, pick1;     // its side's cursor; the cells the two picks were made on (-1: not yet)
    int8_t stage;                               // its stage cursor + 1 while it chooses the stage (0: not choosing - what
                                                // builds before it send in this byte)
    int8_t char0, char1;                        // the characters the two picks took + 1 (0: not known - two picks in one
                                                // frame, the select's timer; builds before 2026-11-04 send 0 here)
};
#pragma pack(pop)

constexpr uint32_t kSync = 0x82E6C5C8u + 64;
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kA = 0x1000;

// The grid: 62 cells in three rows (0-22, 23-45, 46-61); DOWN / UP move between rows, LEFT / RIGHT along one (wrapping).
int CellRow(int c) { return c < 23 ? 0 : c < 46 ? 1 : 2; }
int RowStart(int r) { return r == 0 ? 0 : r == 1 ? 23 : 46; }
int RowLength(int r) { return r == 2 ? 16 : 23; }
uint16_t StepToward(int cell, int target)
{
    const int r = CellRow(cell), tr = CellRow(target);
    if (r != tr) return (tr - r + 3) % 3 == 1 ? kDown : kUp;
    const int len = RowLength(r), d = ((target - RowStart(r)) - (cell - RowStart(r)) + len) % len;
    return d <= len / 2 ? kRight : kLeft;
}

// The select screens' grids (sub_8227A0D8): {table, count} at 0x8200C8BC + 8 * the grid's number - character select's
// is [select controller + 76] (62 cells), the stage select's the fifth. 32-byte cells {character (63: a random cell),
// stage, up, down, left, right, column, row}; the neighbours are where the cursor goes. The rows do not line up (the
// top row starts at the third column): from the first three columns and the last four, UP / DOWN never reach it, which
// StepToward's rows assumed (MEASURED 2026-11-04, the table read from a live copy).
constexpr uint32_t kGrids = 0x8200C8BCu;

std::mutex g_lock;
// The teammate's latest, of the select running now: a teammate sends only while its select runs (10 a second), so what
// arrived in the last 3 s is this select's - its own select may have started first (a numbering of selects threw the
// earlier one's packets away as old: MEASURED 2026-10-25, the red partner never saw its leader's pick).
Pick g_remote{};
int64_t g_remoteMs = 0;
uint32_t g_selectNo = 0;
bool g_inSelect = false;
int64_t g_selectAt = 0, g_pressAt = 0, g_sentAt = 0;
int g_lastPicks = 0, g_pick0 = -1, g_pick1 = -1, g_char0 = -1, g_char1 = -1;
uint64_t g_free = 0;                           // this PC's side's still free characters, last frame
bool g_confirmed = false;

} // namespace

uint32_t CharacterGrid(uint8_t* base)
{
    const uint32_t controller = SelectController();
    const uint32_t number = controller ? GRead32(base, controller + 76) : ~0u;
    return number < 4 ? kGrids + 8 * number : 0;
}

int GridCellOf(uint8_t* base, uint32_t grid, uint32_t field, int value)
{
    const uint32_t table = grid ? GRead32(base, grid) : 0, count = grid ? GRead32(base, grid + 4) : 0;
    for (uint32_t c = 0; table && c < count && c < 64; ++c)
        if (int32_t(GRead32(base, table + 32 * c + field)) == value) return int(c);
    return -1;
}

uint16_t GridStep(uint8_t* base, uint32_t grid, int from, int to)
{
    const uint32_t table = grid ? GRead32(base, grid) : 0;
    const int count = grid ? int((std::min)(GRead32(base, grid + 4), 64u)) : 0;
    if (!table || from < 0 || to < 0 || from >= count || to >= count) return 0;
    constexpr uint16_t kDirs[4] = { kUp, kDown, kLeft, kRight };
    uint16_t first[64] = {};
    int queue[64], head = 0, tail = 0;
    bool seen[64] = {};
    seen[from] = true;
    queue[tail++] = from;
    while (head < tail)
    {
        const int c = queue[head++];
        if (c == to) return first[c];
        for (int d = 0; d < 4; ++d)
        {
            const int n = int(GRead32(base, table + 32 * uint32_t(c) + 8 + 4 * uint32_t(d)));
            if (n < 0 || n >= count || seen[n]) continue;
            seen[n] = true;
            first[n] = c == from ? kDirs[d] : first[c];
            queue[tail++] = n;
        }
    }
    return 0;
}

uint16_t SelectStep(uint8_t* base, int cell, int target)
{
    if (const uint16_t step = GridStep(base, CharacterGrid(base), cell, target)) return step;
    return StepToward(cell, target);
}

int TeammatePick(int index)
{
    std::lock_guard l(g_lock);
    if (g_remote.magic != kPick || index < 0 || index > 1) return -1;
    return (index == 0 ? g_remote.char0 : g_remote.char1) - 1;
}

uint64_t FreeCharacters(uint8_t* base, uint32_t cursor)
{
    return uint64_t(GRead32(base, cursor + 0x148)) << 32 | GRead32(base, cursor + 0x14C);
}

int TakenCharacter(uint64_t before, uint64_t after)
{
    const uint64_t taken = before & ~after;
    if (!taken || (taken & (taken - 1))) return -1;
    int n = 0;
    while (!(taken >> n & 1)) ++n;
    return n;
}

int ScreenSide(uint8_t* base)
{
    constexpr uint32_t kPadManager = 0x82E6ACA8u;               // +84 / +88: the controller in pad slots 9, 10, ...
    const uint32_t b = GRead32(base, kPadManager + 84), e = GRead32(base, kPadManager + 88);
    for (int s = 0; s < 2; ++s)
        if (b && b + 4u * uint32_t(s) < e && int32_t(GRead32(base, b + 4u * uint32_t(s))) >= 0) return s;
    return -1;
}

bool PickReceive(uint64_t from, const uint8_t* data, size_t size)
{
    if (size < sizeof(Pick)) return false;
    Pick p;
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kPick) return false;
    if (from != Teammate()) return true;
    std::lock_guard l(g_lock);
    g_remote = p;
    g_remoteMs = Ms();
    return true;
}

// The partner's PC during its select: its own controller is its game's user 1 (the second player's), user 0 gets only the
// port's presses (the leader's picks mirrored) - kernel_xam.cpp.
bool PartnerSelectRemap() { return g_inSelect && CurrentRole() == Role::Partner && Teammate() != 0; }

// The partner's PC while its team's stage is chosen (the select controller's +0x138 1 or 2, not the panel showing the
// other team's choice): the leader chooses, this screen follows it (PickFrame) and confirms the leader's choice
// (online_stage_mirror.cpp) - the partner's own controller (user 1) holds still there (kernel_xam.cpp). A safeguard: the
// stage step (sub_8227B2F0) reads only pad slot 9 + side, user 0 here, which plays only the port's presses. Set by
// PickFrame (main thread).
std::atomic<bool> g_partnerStage{ false };
bool PartnerStageHold() { return g_partnerStage.load(std::memory_order_relaxed) && PartnerSelectRemap(); }

// Research (TTT2_2V2_SELDIAG=1): the pad manager's slot table (0x82E6ACA8 +84 / +88, the controller in slots 9..) on
// every change, with its buffer.
void SlotWatch(uint8_t* base)
{
    static const bool diag = [] { const char* e = std::getenv("TTT2_2V2_SELDIAG"); return e && *e == '1'; }();
    if (!diag) return;
    constexpr uint32_t kPadManager = 0x82E6ACA8u;
    const uint32_t b = GRead32(base, kPadManager + 84), e = GRead32(base, kPadManager + 88);
    int32_t v[4];
    for (int i = 0; i < 4; ++i) v[i] = b && b + 4u * uint32_t(i) < e ? int32_t(GRead32(base, b + 4u * uint32_t(i))) : -99;
    static uint32_t lb = 0, le = 0;
    static int32_t lv[4] = { -100, -100, -100, -100 };
    if (b == lb && e == le && std::memcmp(v, lv, sizeof(v)) == 0) return;
    std::fprintf(stderr, "[slots] t=%llu table %08X..%08X: %d %d %d %d (battle state %u)\n", online2v2::DiagMs(), b, e,
                 v[0], v[1], v[2], v[3], GRead32(base, 0x82EAC03Cu));
    lb = b;
    le = e;
    std::memcpy(lv, v, sizeof(v));
}

// The game's main thread, every frame (Frame).
void PickFrame(uint8_t* base)
{
    SlotWatch(base);
    {
        // research (TTT2_2V2_SELDIAG=1): C:/ttt2/port/synth<TTT2_INSTANCE>.txt "user hexbuttons" - one press on that user
        static int tick = 0;
        static const bool diag = [] { const char* e = std::getenv("TTT2_2V2_SELDIAG"); return e && *e == '1'; }();
        if (diag && ++tick % 15 == 0)
        {
            const char* tag = std::getenv("TTT2_INSTANCE");
            const std::string path = std::string("C:/ttt2/port/synth") + (tag ? tag : "") + ".txt";
            if (FILE* f = std::fopen(path.c_str(), "r"))
            {
                unsigned user = 0, buttons = 0;
                const int n = std::fscanf(f, "%u %x", &user, &buttons);
                std::fclose(f);
                std::remove(path.c_str());
                if (n == 2 && user < 4)
                {
                    SynthPress(user, uint16_t(buttons), 80, 0);
                    std::fprintf(stderr, "[pick] test: user %u presses %04X\n", user, buttons);
                }
            }
        }
    }
    const Role role = CurrentRole();
    const bool selecting = Selecting() && role != Role::None && Teammate() != 0;
    {
        const uint32_t c = selecting && role == Role::Partner ? SelectController() : 0;
        const uint32_t stagePhase = c ? GRead32(base, c + 0x138) : 0;
        const bool hold = c && (stagePhase == 1 || stagePhase == 2) && !stagemirror::Mirrored(c);
        if (hold != g_partnerStage.exchange(hold, std::memory_order_relaxed))
            std::fprintf(stderr, "[pick] the partner's own controller %s\n",
                         hold ? "holds still: the leader chooses the stage" : "plays again");
    }
    std::lock_guard l(g_lock);
    const int64_t now = Ms();
    if (selecting != g_inSelect)
    {
        g_inSelect = selecting;
        if (selecting)
        {
            ++g_selectNo;
            if (now - g_remoteMs > 3000) g_remote = Pick{};
            g_selectAt = now;
            g_lastPicks = 0;
            g_pick0 = g_pick1 = g_char0 = g_char1 = -1;
            g_free = 0;
            g_confirmed = false;
            constexpr uint32_t kPadManager = 0x82E6ACA8u;               // +84 / +88: the pad slots 9.. (the controller in each)
            const uint32_t b = GRead32(base, kPadManager + 84), e = GRead32(base, kPadManager + 88);
            int slots[4];
            for (int i = 0; i < 4; ++i) slots[i] = b && b + 4u * uint32_t(i) < e ? int32_t(GRead32(base, b + 4u * uint32_t(i))) : -9;
            const uint32_t impl = GRead32(base, 0x82E6B05Cu);
            std::fprintf(stderr, "[pick] character select %u starts (%s); pad slots 9-12: %d %d %d %d; battle position %08X (impl %08X)\n",
                         g_selectNo, role == Role::Host ? "leader" : "partner", slots[0], slots[1], slots[2], slots[3],
                         impl ? GRead32(base, impl + 760) : 0xFFFFFFFFu, impl);
        }
    }
    if (!selecting) return;
    const int side = ScreenSide(base);                 // this PC's own cursor (leader and partner alike)
    if (side < 0) return;
    {
        static int lastSide = -1;
        if (side != lastSide) std::fprintf(stderr, "[pick] this PC's screen side %d\n", side);
        lastSide = side;
    }
    const uint32_t cursor = SelectCursor(side);
    if (!cursor) return;
    const int cell = int32_t(GRead32(base, cursor + 0x38));
    const int picks = int32_t(GRead32(base, cursor + 0x154));
    const int turn = int32_t(GRead32(base, cursor + 0x158));
    const uint64_t free = FreeCharacters(base, cursor);
    if (picks > g_lastPicks)
    {
        // the character it took (a random cell's or the timer's draw is not the cell's)
        const int taken = picks == g_lastPicks + 1 && g_free ? TakenCharacter(g_free, free) : -1;
        if (g_lastPicks == 0)
        {
            g_pick0 = cell;
            g_char0 = picks == 1 ? taken : -1;
        }
        if (picks >= 2)
        {
            g_pick1 = cell;
            g_char1 = g_lastPicks == 1 ? taken : -1;
        }
        std::fprintf(stderr, "[pick] pick %d made on cell %d (character %d)\n", picks, cell, taken);
    }
    g_lastPicks = picks;
    g_free = free;
    // the stage select (the select controller's +0x138 1 while a stage is chosen, +0x12C its cursor, 11 stages to a row):
    // the side choosing it is one PC's; its teammate's screen follows the cursor, and when the leader chooses,
    // online_stage_mirror.cpp PartnerStep confirms the same cell / stage on the partner's panel (the partner's own pad
    // cannot: kernel_xam.cpp PartnerStageHold). The partner's entry is a spectator's - the match comes from the fighters'.
    const uint32_t controller = SelectController();
    // (not the stage panel showing the other team's choice - online_stage_mirror.cpp: nobody here chooses)
    const bool choosingStage = controller && GRead32(base, controller + 0x138) == 1 && !stagemirror::Mirrored(controller);
    const int stage = choosingStage ? int32_t(GRead32(base, controller + 0x12C)) : -1;
    // my side's cursor to the teammate, 10 times a second
    if (now - g_sentAt >= 100)
    {
        g_sentAt = now;
        const Pick p{ kPick, g_selectNo, int8_t(turn), int8_t(picks), int8_t(cell), int8_t(g_pick0), int8_t(g_pick1),
                      int8_t(stage >= 0 && stage < 100 && role == Role::Host ? stage + 1 : 0), int8_t(g_char0 + 1),
                      int8_t(g_char1 + 1) };
        XSocketSend2v2(Teammate(), &p, sizeof(p));
    }
    if (role == Role::Partner && choosingStage && stage >= 0 && g_remote.magic == kPick && g_remote.stage > 0)
    {
        const int target = g_remote.stage - 1;
        if (stage != target && now - g_pressAt >= 110)
        {
            g_pressAt = now;
            // by the stage grid's neighbours (a random cell's UP / DOWN is itself)
            const int row = stage / 11, targetRow = target / 11;
            uint16_t step = GridStep(base, kGrids + 8 * 4, stage, target);
            if (!step) step = row != targetRow ? (targetRow > row ? kDown : kUp) : (target > stage ? kRight : kLeft);
            SynthPress(0, step, 60, 0);
        }
        return;
    }
    // The side's play style is PAIR from the start (PairPreset below): no TAG / SOLO / PAIR window, no START prompt.
    if (now - g_pressAt < 110 || cell < 0 || cell >= 62) return;
    // whose pick this PC mirrors: the leader the second (user 1), the partner the first (user 0)
    const bool mirror = role == Role::Host ? (turn == 1 && picks == 1) : (turn == 0 && picks == 0);
    if (!mirror || g_remote.magic != kPick) return;
    const uint32_t user = role == Role::Host ? 1 : 0;
    const int remoteDone = role == Role::Host ? g_remote.pick1 : g_remote.pick0;
    const int remoteCharacter = (role == Role::Host ? g_remote.char1 : g_remote.char0) - 1;
    int target = remoteDone >= 0 ? remoteDone : g_remote.cell;
    // the character the teammate's pick took, on the cell that shows it: a random cell's draw is not the same on two
    // games, and the leader's pick is the one the match uses
    if (remoteDone >= 0 && remoteCharacter >= 0)
        if (const int shown = GridCellOf(base, CharacterGrid(base), 0, remoteCharacter); shown >= 0) target = shown;
    if (target < 0 || target >= 62) return;
    g_pressAt = now;
    if (cell != target)
    {
        SynthPress(user, SelectStep(base, cell, target), 60, 0);
        return;
    }
    if (remoteDone >= 0 && !g_confirmed)
    {
        g_confirmed = true;                          // once per pick (the turn moves on after it)
        SynthPress(user, kA, 80, 0);
        std::fprintf(stderr, "[pick] the %s's pick mirrored on cell %d (character %d)\n", role == Role::Host ? "partner" : "leader",
                     cell, remoteCharacter);
    }
}

} // namespace online2v2

// ---- a team's side picks like offline PAIR PLAY (user, 2026-11-07: no TAG / SOLO / PAIR window when two play a side)
// The cursor's play style is +0x8204 (1 TAG, 2 SOLO, 3 PAIR; the vtable at +48: vt[2] sub_82281DB8 reads it, vt[1]
// sub_8227B210 writes it). Its per-frame function sub_8227F108 (r3 the select controller, r4 the cursor) goes through its
// states at +312: state 0 clears the side's pair flag, 17 -> 18, and 18 skips the choice window (state 1 / 2) for state 19
// (picking) when the play style is already PAIR. Offline PAIR PLAY sets it there from the pad slots; online
// (controller +85) never. So the team's own cursor gets, at state 17 / 18, what choosing PAIR and the second controller's
// START would have done (sub_8227C1A0 at 0x8227C370, sub_8227E678 at 0x8227E7E8): PAIR, the settings' pair flags (+472,
// +528 + 4 * side), and the partner joined on pad slot 11 + side as user 1 - the partner's controller on both PCs (the
// leader's mirror of it, the partner's own: PartnerSelectRemap). Pick 1 then comes from slot 9 + side, pick 2 from slot 11
// + side, as offline.
PPC_EXTERN_FUNC(sub_8227B210);
PPC_EXTERN_FUNC(sub_825B7370);
PPC_EXTERN_FUNC(sub_825B6F60);
PPC_EXTERN_FUNC(sub_8266B900);
PPC_EXTERN_FUNC(sub_82254A08);
PPC_EXTERN_FUNC(sub_8228D9C8);
PPC_EXTERN_FUNC(sub_82265948);
PPC_EXTERN_FUNC(sub_826B8070);
PPC_EXTERN_FUNC(sub_82254C08);
PPC_EXTERN_FUNC(sub_825811A8);
PPC_EXTERN_FUNC(sub_82382400);
PPC_EXTERN_FUNC(sub_826B8078);
namespace online2v2 {
namespace {
void PairPreset(const PPCContext& ctx, uint8_t* base, uint32_t controller, uint32_t cursor)
{
    if (!controller || !cursor || !GRead8(base, controller + 85)) return;           // online selects only
    const uint32_t state = GRead32(base, cursor + 312);
    if (state != 17 && state != 18) return;
    if (!PairRoom() || CurrentRole() == Role::None || !Teammate()) return;
    const int side = int(GRead32(base, cursor + 320));
    if (side != ScreenSide(base) || GRead32(base, cursor + 0x8204) == 3 || GRead8(base, cursor + 0x1AD)) return;
    constexpr uint32_t kPartnerUser = 1;
    PPCContext c = ctx;
    c.r3.u64 = kPartnerUser;
    sub_8266B900(c, base);                                                          // the join's own check (0: may join)
    if (c.r3.u32 != 0)
    {
        std::fprintf(stderr, "[pick] PAIR preset: user %u may not join (%u) - the game's choice stays\n", kPartnerUser, c.r3.u32);
        return;
    }
    c = ctx;
    c.r3.u64 = cursor + 48;
    c.r4.u64 = 3;
    sub_8227B210(c, base);                                                          // play style PAIR
    c = ctx;
    sub_825B7370(c, base);
    if (const uint32_t settings = c.r3.u32)
    {
        GWrite32(base, settings + 472, 1);
        c = ctx;
        c.r3.u64 = settings;
        c.r4.u64 = side != 0;
        c.r5.u64 = 1;
        sub_825B6F60(c, base);                                                      // the side's pair flag
    }
    c = ctx;
    sub_82254A08(c, base);
    const uint32_t pads = c.r3.u32;
    c = ctx;
    c.r3.u64 = pads;
    c.r4.u64 = 11u + (side != 0);
    c.r5.u64 = 9u + kPartnerUser;
    sub_8228D9C8(c, base);                                                          // pad slot 11 + side: the partner
    c = ctx;
    sub_82265948(c, base);
    const uint32_t online = c.r3.u32;
    c = ctx;
    c.r3.u64 = online;
    c.r4.u64 = kPartnerUser;
    sub_826B8070(c, base);                                                          // the session's guest user
    c = ctx;
    sub_82254C08(c, base);
    if (const uint32_t roles = c.r3.u32 ? GRead32(base, c.r3.u32) : 0)
    {
        c = ctx;
        c.r3.u64 = roles;
        c.r4.u64 = 2;
        c.r5.u64 = kPartnerUser;
        sub_825811A8(c, base);
    }
    if (const uint32_t prompt = GRead32(base, cursor + 0x8214))
    {
        c = ctx;
        c.r3.u64 = prompt;
        c.r4.u64 = 1;
        sub_82382400(c, base);
    }
    c = ctx;
    c.r3.u64 = online;
    c.r4.u64 = 1;
    sub_826B8078(c, base);                                                          // a guest plays
    GWrite8(base, cursor + 0x1AD, 1);                                               // joined: state 19 asks for no START
    std::fprintf(stderr, "[pick] PAIR preset on side %d (state %u): picks from pad slots %d and %d\n", side, state, 9 + side, 11 + side);
}
} // namespace

// STAGE SELECT from the end menu (online_straight.cpp) keeps a PAIR PLAY side's characters: the select's cursors never
// reach the play-style choice (states 17 / 18) where PairPreset joins the partner, so the join is made before the cursors
// exist - PairPreset's own steps without the cursor's (its play style and +0x1AD: the keep route reads neither).
void PairJoinKept(const PPCContext& ctx, uint8_t* base, int side)
{
    constexpr uint32_t kPartnerUser = 1;
    PPCContext c = ctx;
    sub_825B7370(c, base);
    if (const uint32_t settings = c.r3.u32)
    {
        GWrite32(base, settings + 472, 1);
        c = ctx;
        c.r3.u64 = settings;
        c.r4.u64 = side != 0;
        c.r5.u64 = 1;
        sub_825B6F60(c, base);                                                      // the side's pair flag
    }
    c = ctx;
    sub_82254A08(c, base);
    const uint32_t pads = c.r3.u32;
    c = ctx;
    c.r3.u64 = pads;
    c.r4.u64 = 11u + (side != 0);
    c.r5.u64 = 9u + kPartnerUser;
    sub_8228D9C8(c, base);                                                          // pad slot 11 + side: the partner
    c = ctx;
    sub_82265948(c, base);
    const uint32_t online = c.r3.u32;
    c = ctx;
    c.r3.u64 = online;
    c.r4.u64 = kPartnerUser;
    sub_826B8070(c, base);                                                          // the session's guest user
    c = ctx;
    sub_82254C08(c, base);
    if (const uint32_t roles = c.r3.u32 ? GRead32(base, c.r3.u32) : 0)
    {
        c = ctx;
        c.r3.u64 = roles;
        c.r4.u64 = 2;
        c.r5.u64 = kPartnerUser;
        sub_825811A8(c, base);
    }
    c = ctx;
    c.r3.u64 = online;
    c.r4.u64 = 1;
    sub_826B8078(c, base);                                                          // a guest plays
    std::fprintf(stderr, "[pick] PAIR side %d kept: the partner joined before the select's cursors (pad slot %d)\n", side, 11 + side);
}
} // namespace online2v2

extern "C" PPC_FUNC(__imp__sub_8227F108);
PPC_FUNC(sub_8227F108)
{
    const uint32_t controller = ctx.r3.u32, cursor = ctx.r4.u32;
    const uint32_t before = cursor ? GRead32(base, cursor + 312) : ~0u;
    online2v2::PairPreset(ctx, base, controller, cursor);
    __imp__sub_8227F108(ctx, base);
    straight::CursorStepped(base, controller, cursor, before);   // STAGE SELECT: the kept side on to the stage
}
