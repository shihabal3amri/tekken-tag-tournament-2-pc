// PAIR PLAY: each player's save gets its own fight's results (online_2v2.h, port/ONLINE_2V2_PLAN.md).
//
// The game keeps one player record per side ([[0x82E6C5A8] + 256] + 60 + 4 * side) with two fighter slots (+640 / +4928,
// 4288 bytes each), filled from that record's own character table at character select (sub_8265A3B0) and written back
// into it at the match's end (the award sub_826510D0 ends with sub_82653A48 -> sub_82659F48 on both side records), then
// saved (sub_82654CE8 -> sub_8265F628). In a PAIR PLAY match each PC's own side record is its own player's:
//  - a partner's is lent after its picks (the battle setup's sub_82668D40(0, side), or online_straight.cpp's REMATCH lend)
//    as a clone of the save holder's object, so its slots held whatever that object last had - never filled from its own
//    table - and the write-back put those old blocks into the partner's table for both characters of its side (MEASURED
//    2026-11-09 t20a: C's 42 / 51 online rank 25 -> 0, D's 9 35 -> 0 and 13 35 -> 10, with the offline ranks and the
//    win / loss counters). Hook A refills them from its own table after the battle setup's apply (the game's own fill);
//  - each record credited both characters of its side to its one player (the offline PAIR model: one profile per side).
//    Online each character has its own player: hook B puts the teammate's character entry back after the award, so each
//    player's save gets its own character's results; the team's (the records' totals, fight money) stay for both.
// PAIR PLAY sides of two only: 1v1, a 2v1 room's solo side, watchers, Ranked and offline keep the game's own.
// Not synced state: the slots' blocks already differ between PCs (each holds its own player's record) and the fight's
// checks never read them (only displays do - the HUD's rank, the award's screens and unlock grants). Hook B keeps no state
// between calls and decides by the match's places (constant through a fight): a re-run award puts back the same entry.
#include "online_2v2.h"
#include "xlive.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

extern "C" PPC_FUNC(__imp__sub_826689C8);
extern "C" PPC_FUNC(__imp__sub_826510D0);
PPC_EXTERN_FUNC(sub_8265A3B0);   // fill: r3 a record, r4 / r5 the slots' characters - both slots from its own table
int32_t RollbackSessionFrame();   // rollback.cpp: the frame a session update simulates (-1: none)
bool RollbackResimulating();      // rollback.cpp: a session's re-simulation runs

namespace online2v2 {
namespace {
constexpr uint32_t kGame = 0x82E6C5A8u, kHolder = 0x82E70F04u;
constexpr uint32_t kSlot0 = 640, kSlotBytes = 4288, kTable = 9216, kEntryBytes = 76, kQuad = 14948, kLentSide = 16156;

uint32_t SlotOf(uint32_t rec, int i) { return rec + kSlot0 + kSlotBytes * uint32_t(i); }
uint32_t EntryOf(uint32_t rec, int c) { return rec + kTable + kEntryBytes * uint32_t(c); }

// The record this PC saves (sub_8265F628 while the holder is lent: [PM + 60 + 4 * holder+12]) - its own player's (type 1,
// this player's XUID), lent to that side (+16156); 0 otherwise.
uint32_t OwnSideRecord(uint8_t* base, int& side)
{
    side = -1;
    const uint32_t game = GRead32(base, kGame), pm = game ? GRead32(base, game + 256) : 0;
    if (!pm || GRead8(base, kHolder + 8)) return 0;
    const uint32_t s = GRead32(base, kHolder + 12);
    if (s > 1) return 0;
    const uint32_t rec = GRead32(base, pm + 60 + 4 * s);
    if (!rec || GRead8(base, rec) != 1 || GRead32(base, rec + kLentSide) != s) return 0;
    const uint64_t xuid = (uint64_t(GRead32(base, rec + 276)) << 32) | GRead32(base, rec + 280);
    if (!xuid || xuid != xlive::OnlineXuid(0)) return 0;
    side = int(s);
    return rec;
}

// What the write-back (sub_82659F48) copies from slot i into its character's entry, and the fill (sub_8265A3B0) the other
// way: {slot offset, entry offset, bytes}, plus the record's +14948 + 16 * i -> entry +60 (16 bytes). Exactly these: the
// award's display bytes between them (+4110..+4113) are not part of it.
struct Span { uint16_t slot, entry, size; };
constexpr Span kWriteBack[] = { { 4100, 0, 1 }, { 4102, 2, 2 }, { 4104, 4, 3 }, { 4116, 12, 8 },
                                { 4180, 20, 1 }, { 4184, 24, 2 }, { 4186, 26, 3 }, { 4192, 32, 28 } };

// Slot i of `rec` holds its character's entry as the table has it (the write-back would change nothing).
bool Fresh(uint8_t* base, uint32_t rec, int i)
{
    const int c = GRead8(base, SlotOf(rec, i));
    if (c >= 63) return true;                               // nothing the fill could do (it clamps to 62)
    for (const Span& s : kWriteBack)
        if (std::memcmp(base + SlotOf(rec, i) + s.slot, base + EntryOf(rec, c) + s.entry, s.size)) return false;
    return !std::memcmp(base + rec + kQuad + 16 * i, base + EntryOf(rec, c) + 60, 16);
}

// The game's own fill of both slots from the record's table (their characters kept) - a copy within the record: at worst
// a round trip of its own data (nothing writes these slot fields between the battle setup and the award).
void Refill(PPCContext& ctx, uint8_t* base, uint32_t rec)
{
    PPCContext c = ctx;
    c.r3.u64 = rec;
    c.r4.u64 = GRead8(base, rec + kSlot0);
    c.r5.u64 = GRead8(base, rec + kSlot0 + kSlotBytes);
    sub_8265A3B0(c, base);
}

int Rank(uint8_t* base, uint32_t at) { return GRead8(base, at); }
int Points(uint8_t* base, uint32_t at) { return int16_t(GRead16(base, at)); }

// A line once (a re-simulated frame or a select polling the exchange asks again with the same answer); `always` prints it
// even when it repeats (the award's own line: an award that ran again must show - award_journal.cpp).
void LogOnce(std::string& last, const std::string& line, bool always = false)
{
    static std::mutex lock;
    std::lock_guard l(lock);
    if (line == last && !always) return;
    last = line;
    std::fputs(line.c_str(), stderr);
}
} // namespace
} // namespace online2v2

// Hook A - the battle setup's apply (1: the exchange goes on; 0: done, or nothing to do - no session, no settings): the
// select controller's (room start, STAGE SELECT, CHARACTER SELECT, CHANGE TEAMS) and online_straight.cpp's REMATCH
// exchange. On a partner's PC its own lent record's two slots are filled from its own table, as the picks fill a leader's -
// before the VS screen and the fight read them. Only when they are not already (a repeated 0 changes nothing).
PPC_FUNC(sub_826689C8)
{
    __imp__sub_826689C8(ctx, base);
    using namespace online2v2;
    if ((ctx.r3.u32 & 0xFF) || CurrentRole() != Role::Partner || !SpectatorNode(base)) return;
    static std::string last;
    char line[360];
    int side = -1;
    const uint32_t rec = OwnSideRecord(base, side);
    if (!rec || side != PartnerSide() || PairOwnSlot(side) != 1)
    {
        const uint32_t game = GRead32(base, kGame), pm = game ? GRead32(base, game + 256) : 0;
        const uint32_t s = GRead32(base, kHolder + 12), r = pm && s < 2 ? GRead32(base, pm + 60 + 4 * s) : 0;
        std::snprintf(line, sizeof line, "[rewards] partner: no refill (side %d record %08X type %u +16156 %u, holder +8 %u +12 %u, PartnerSide %d, own slot %d)\n",
                      side, r, r ? unsigned(GRead8(base, r)) : 0u, r ? GRead32(base, r + kLentSide) : 0u, unsigned(GRead8(base, kHolder + 8)), s,
                      PartnerSide(), PairOwnSlot(side < 0 ? PartnerSide() : side));
        LogOnce(last, line);
        return;
    }
    const int c0 = GRead8(base, rec + kSlot0), c1 = GRead8(base, rec + kSlot0 + kSlotBytes);
    if (c0 >= 63 || c1 >= 63 || c0 == c1 || (Fresh(base, rec, 0) && Fresh(base, rec, 1))) return;
    const uint32_t s0 = SlotOf(rec, 0), s1 = SlotOf(rec, 1);
    const int r0 = Rank(base, s0 + 4180), p0 = Points(base, s0 + 4184), r1 = Rank(base, s1 + 4180), p1 = Points(base, s1 + 4184);
    const int o0 = Rank(base, s0 + 4100), o1 = Rank(base, s1 + 4100);
    Refill(ctx, base, rec);
    std::snprintf(line, sizeof line, "[rewards] partner's own slots refilled from its table: side %d record %08X - slot 0 (character %d, its leader's) "
                  "online rank %d -> %d pts %d -> %d offline %d -> %d; slot 1 (character %d, its own) online rank %d -> %d pts %d -> %d offline %d -> %d\n",
                  side, rec, c0, r0, Rank(base, s0 + 4180), p0, Points(base, s0 + 4184), o0, Rank(base, s0 + 4100),
                  c1, r1, Rank(base, s1 + 4180), p1, Points(base, s1 + 4184), o1, Rank(base, s1 + 4100));
    LogOnce(last, line);
}

// Hook B - the award (r3 PM + 76, r4 the winner's record, r5 the loser's, r6 a draw; its one caller sub_82654CE8 asks for
// the save after it): on a PC whose own record is on a PAIR PLAY side of two (by the match's places), the teammate's
// character's entry is put back as it was before the award - this player's own character keeps the game's result - and
// both slots are made the table's again (the teammate's slot would carry the award's change into the next write-back).
PPC_FUNC(sub_826510D0)
{
    using namespace online2v2;
    int side = -1, own = -1, cOwn = -1, cMate = -1;
    uint8_t mate[kEntryBytes], ownBefore[kEntryBytes];
    static std::string last;
    char line[480];
    const char* skip = nullptr;
    const uint32_t rec = OwnSideRecord(base, side);
    if (rec) own = PairPlacesSlot(side);
    if (own >= 0)
    {
        cOwn = GRead8(base, SlotOf(rec, own));
        cMate = GRead8(base, SlotOf(rec, own ^ 1));
        if (cOwn >= 63 || cMate >= 63 || cOwn == cMate) { skip = "the slots' characters"; own = -1; }
    }
    else skip = rec ? "no PAIR PLAY side of two with this player" : "not this PC's own record";
    if (own >= 0)
    {
        const bool fresh0 = Fresh(base, rec, 0), fresh1 = Fresh(base, rec, 1);
        if (!fresh0 || !fresh1)
        {
            // a path without hook A's refill: the award and the write-back then work from the record's own table
            Refill(ctx, base, rec);
            std::snprintf(line, sizeof line, "[rewards] award: side %d slot(s)%s%s did not match the record's table - refilled at the award (this PC the %s)\n",
                          side, fresh0 ? "" : " 0", fresh1 ? "" : " 1", own ? "partner" : "fighter");
            LogOnce(last, line);
        }
        std::memcpy(mate, base + EntryOf(rec, cMate), kEntryBytes);
        std::memcpy(ownBefore, base + EntryOf(rec, cOwn), kEntryBytes);
    }
    const uint32_t pm = ctx.r3.u32 - 76, winner = ctx.r4.u32, loser = ctx.r5.u32;
    const bool draw = (ctx.r6.u32 & 0xFF) != 0;
    __imp__sub_826510D0(ctx, base);
    if (own < 0)
    {
        if (skip && PairRoom())
        {
            std::snprintf(line, sizeof line, "[rewards] award: left to the game (%s)\n", skip);
            LogOnce(last, line);
        }
        return;
    }
    const uint32_t mateAt = EntryOf(rec, cMate), ownAt = EntryOf(rec, cOwn);
    int put = 0;
    for (uint32_t i = 0; i < kEntryBytes; ++i) put += base[mateAt + i] != mate[i];
    std::memcpy(base + mateAt, mate, kEntryBytes);
    Refill(ctx, base, rec);                                  // own slot: as just written back; the teammate's: the entry put back
    auto u32 = [](const uint8_t* e, uint32_t at) { return (uint32_t(e[at]) << 24) | (uint32_t(e[at + 1]) << 16) | (uint32_t(e[at + 2]) << 8) | e[at + 3]; };
    const uint8_t* after = base + ownAt;
    std::snprintf(line, sizeof line,
                  "[rewards] award (session frame %d, %s): side %d record %08X, this PC the %s (own slot %d), %s (PM+48 %08X) - own character %d: online rank %u pts %d "
                  "+32 %u +36 %u +40 %u +52 %u +56 %u -> rank %u pts %d +32 %u +36 %u +40 %u +52 %u +56 %u; the teammate's character %d kept (%d byte(s) put back)\n",
                  RollbackSessionFrame(), RollbackResimulating() ? "re-simulated" : "live",
                  side, rec, own ? "partner" : "fighter", own, draw ? "a draw" : rec == winner ? "won" : rec == loser ? "lost" : "result ?", GRead32(base, pm + 48),
                  cOwn, unsigned(ownBefore[20]), int(int16_t((ownBefore[24] << 8) | ownBefore[25])), u32(ownBefore, 32), u32(ownBefore, 36), u32(ownBefore, 40),
                  u32(ownBefore, 52), u32(ownBefore, 56), unsigned(after[20]), int(int16_t((after[24] << 8) | after[25])), u32(after, 32), u32(after, 36),
                  u32(after, 40), u32(after, 52), u32(after, 56), cMate, put);
    LogOnce(last, line, true);
}
