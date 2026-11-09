// Online rollback: the end-of-battle award's journal (fix A', 2026-11-09).
//
// The end-of-battle step sub_82654CE8(PM) (PM = [[0x82E6C5A8] + 256]) runs on the main thread in the KO frame F: guard
// PM+60 and PM+64 (the two side records, 16176 bytes each) set and PM+152 == 0, then the award sub_826510D0 (counters +=,
// rank / points, Battle Points, money, the write-back into the character entries), the save (sub_8265F628, save_store.cpp's
// journal) and PM+152 = 1. On a fighter PC the records are blocks of the never-rewound arena (rollback.cpp) while PM is
// rewound: a restore to a frame p <= F ran the award again on records that already held it - doubled wins / losses /
// matches, Battle Points, money, and in Ranked a second rank update (MEASURED t4b / t20u / t20w). A partner's REMATCH-lent
// record comes from the rewound game heap instead.
//
// The journal: while a session runs, each award's changes to both records are kept host-side as aligned 8-byte units
// {before, after}. A restore to p (SessionSaveBeforeRestore, right after the save journal's undo) takes back every award of
// a frame >= p, newest first: a unit a restore puts back anyway (RollbackIsRewound) is left to it; one still holding the
// award's value gets its value from before; in one something else wrote since, only the award's own bytes go back (when none
// of them was written again; else the unit is left as it is, counted). A record that is no longer PM+60 /
// PM+64, or whose arena block was freed or handed out again (RollbackArenaSerial), is left alone. Nothing is written to
// guest memory unless a restore crosses an award; the capture is host-side copies only.
//
// The detector (log only): an award that runs while its record still holds the units the previous award of the same match
// changed is a double. "The same match": the same session, or within 10 s of that session's end with no session started
// since (the live KO after a re-simulation that stopped early, t20u) - the next match always starts a session of its own.
#include "kernel_shared.h"
#include "award_journal.h"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace gin_trace { void ReplayStep(PPCContext& ctx, uint8_t* base); }   // gin_trace.cpp: its trace around the call
bool RollbackSessionOn();                          // rollback.cpp
int32_t RollbackSessionFrame();                    // rollback.cpp: the frame a session update simulates (-1: none)
int32_t RollbackSessionLast();                     // rollback.cpp: the newest record frame consumed
bool RollbackResimulating();                       // rollback.cpp
bool RollbackIsRewound(uint32_t addr);             // rollback.cpp: a word a restore puts back
void RollbackBeforeHostWrite(uint32_t addr, uint32_t size);   // rollback.cpp: open protected pages before a host write
uint64_t RollbackArenaSerial(uint32_t addr);       // rollback.cpp: the arena block's allocation serial (0: not the arena's)

namespace {
constexpr uint32_t kGame = 0x82E6C5A8u;            // [kGame] + 256: PM
constexpr uint32_t kRecordBytes = 16176;           // a side record (online_2v2_team.cpp)
constexpr auto kAfterSession = std::chrono::seconds(10);

struct Unit { uint32_t addr; uint64_t before, after; };   // raw guest bytes (big-endian as they are)
struct Record { uint32_t ptr = 0; uint64_t serial = 0; std::vector<Unit> units; };
struct Entry { int32_t frame = -1; bool resim = false; uint32_t pm = 0; DWORD tid = 0; Record rec[2]; };

std::mutex g_lock;
std::vector<Entry> g_journal;                      // this session's awards not undone, oldest first
Entry g_last;                                      // the detector: the session's newest award left standing
bool g_lastValid = false;
std::chrono::steady_clock::time_point g_overAt{};

uint32_t CurrentPm(uint8_t* base)
{
    const uint32_t game = GRead32(base, kGame);
    return game ? GRead32(base, game + 256) : 0;
}
uint32_t SpanLo(uint32_t rec) { return rec & ~7u; }
uint32_t SpanHi(uint32_t rec) { return (rec + kRecordBytes + 7) & ~7u; }
uint64_t Load8(const uint8_t* base, uint32_t a)
{
    uint64_t v;
    std::memcpy(&v, base + a, 8);
    return v;
}
const char* How(bool resim) { return resim ? "re-simulated" : "live"; }

// One award in progress (the override's locals): the records as they were before it.
struct Pending
{
    int32_t frame = -1;
    bool resim = false;
    uint32_t pm = 0, ptr[2] = {};
    uint64_t serial[2] = {};
    std::vector<uint8_t> before[2];
};

// The detector, before an award runs on records a / b.
void Detect(uint8_t* base, uint32_t a, uint32_t b)
{
    std::lock_guard l(g_lock);
    const Entry* prev = nullptr;
    bool oneShot = false;
    if (RollbackSessionOn()) prev = g_journal.empty() ? nullptr : &g_journal.back();
    else if (g_lastValid)
    {
        oneShot = true;                            // the first award after the session's end decides
        if (std::chrono::steady_clock::now() - g_overAt < kAfterSession) prev = &g_last;
    }
    if (prev)
        for (const Record& r : prev->rec)
        {
            if (!r.ptr || r.units.empty() || (r.ptr != a && r.ptr != b) || RollbackArenaSerial(r.ptr) != r.serial) continue;
            uint32_t held = 0;
            for (const Unit& u : r.units) held += Load8(base, u.addr) == u.after;
            if (held)
                std::fprintf(stderr, "[award-journal] WARNING: award ran again on a record holding an earlier award (%d, %d): record %08X, "
                             "%u of %zu units still the earlier award's\n", prev->frame, RollbackSessionFrame(), r.ptr, held, r.units.size());
        }
    if (oneShot) g_lastValid = false;
}

// Before the call: the guard, the detector, the copies (a session only).
bool Begin(uint8_t* base, uint32_t pm, Pending& p)
{
    if (!pm) return false;
    const uint32_t a = GRead32(base, pm + 60), b = GRead32(base, pm + 64);
    if (!a || !b || GRead32(base, pm + 152) != 0) return false;   // the step does nothing (the guest's own guard)
    Detect(base, a, b);
    if (!RollbackSessionOn()) return false;
    const int32_t f = RollbackSessionFrame();
    p.frame = f >= 0 ? f : RollbackSessionLast();
    p.resim = RollbackResimulating();
    p.pm = pm;
    p.ptr[0] = a;
    p.ptr[1] = b != a ? b : 0;
    for (int r = 0; r < 2; ++r)
    {
        if (!p.ptr[r]) continue;
        p.serial[r] = RollbackArenaSerial(p.ptr[r]);
        const uint32_t lo = SpanLo(p.ptr[r]), hi = SpanHi(p.ptr[r]);
        p.before[r].assign(base + lo, base + hi);
    }
    return true;
}

// After the call: the units the award changed, journaled.
void End(uint8_t* base, Pending& p)
{
    Entry e;
    e.frame = p.frame;
    e.resim = p.resim;
    e.pm = p.pm;
    e.tid = GetCurrentThreadId();
    for (int r = 0; r < 2; ++r)
    {
        if (!p.ptr[r]) continue;
        Record& rec = e.rec[r];
        rec.ptr = p.ptr[r];
        rec.serial = p.serial[r];
        const uint32_t lo = SpanLo(rec.ptr), hi = SpanHi(rec.ptr);
        const uint8_t* was = p.before[r].data();
        for (uint32_t a = lo; a < hi; a += 8)
        {
            uint64_t before, after;
            std::memcpy(&before, was + (a - lo), 8);
            std::memcpy(&after, base + a, 8);
            if (before != after) rec.units.push_back({ a, before, after });
        }
    }
    std::fprintf(stderr, "[award-journal] award in frame %d (%s): record %08X %zu units, record %08X %zu units\n", e.frame, How(e.resim),
                 e.rec[0].ptr, e.rec[0].units.size(), e.rec[1].ptr, e.rec[1].units.size());
    std::lock_guard l(g_lock);
    g_journal.push_back(std::move(e));
}
} // namespace

void AwardJournalUndo(int32_t p)
{
    std::lock_guard l(g_lock);
    while (!g_journal.empty() && g_journal.back().frame >= p)
    {
        Entry e = std::move(g_journal.back());
        g_journal.pop_back();
        static bool threadNoted = false;
        if (e.tid != GetCurrentThreadId() && !threadNoted)
        {
            threadNoted = true;
            std::fprintf(stderr, "[award-journal] note: undo on thread %lu, the award ran on %lu\n", GetCurrentThreadId(), e.tid);
        }
        const uint32_t pm = CurrentPm(g_base);
        uint32_t put = 0, merged = 0, total = 0, changed = 0, left = 0;
        for (const Record& rec : e.rec)
        {
            if (!rec.ptr) continue;
            total += uint32_t(rec.units.size());
            const bool linked = pm && (GRead32(g_base, pm + 60) == rec.ptr || GRead32(g_base, pm + 64) == rec.ptr);
            const uint64_t serial = RollbackArenaSerial(rec.ptr);
            if (!linked || serial != rec.serial)
            {
                std::fprintf(stderr, "[award-journal] rewind to frame %d: record %08X of the award of frame %d left alone (%s; %zu units)\n", p,
                             rec.ptr, e.frame, !linked ? "no longer PM+60 / PM+64" : "its arena block was freed or handed out again",
                             rec.units.size());
                continue;
            }
            for (const Unit& u : rec.units)
            {
                if (RollbackIsRewound(u.addr)) { ++left; continue; }
                const uint64_t live = Load8(g_base, u.addr);
                if (live == u.after)
                {
                    RollbackBeforeHostWrite(u.addr, 8);
                    std::memcpy(g_base + u.addr, &u.before, 8);
                    ++put;
                    continue;
                }
                // Something else wrote this unit after the award. A unit holds several fields: record +16072 = u16 +16072,
                // u16 +16074 (the loser's += 1 in the award) and u32 +16076, which ticks on its own after the KO (t22b) -
                // leaving the whole unit kept the += 1 and the re-simulated award added it again. Per byte: a byte the
                // award changed that still holds the award's value gets its value from before; a byte the award did not
                // change keeps what was written since. If a byte the award changed was written again since, that field
                // has two writers: the whole unit is left as it is (putting part of it back could tear a field).
                uint8_t b[8], a[8], l[8];
                std::memcpy(b, &u.before, 8);
                std::memcpy(a, &u.after, 8);
                std::memcpy(l, &live, 8);
                bool conflict = false;
                for (int i = 0; i < 8; ++i) conflict |= b[i] != a[i] && l[i] != a[i];
                uint64_t result = live;
                if (!conflict)
                {
                    RollbackBeforeHostWrite(u.addr, 8);
                    for (int i = 0; i < 8; ++i)
                        if (b[i] != a[i]) g_base[u.addr + i] = b[i];   // only the award's own bytes; the others untouched
                    result = Load8(g_base, u.addr);
                    ++merged;
                }
                else
                    ++changed;
                // The record offset and the values as guest bytes (big-endian, as the record's fields read).
                if (merged + changed <= 6)
                    std::fprintf(stderr, "[award-journal]   record %08X +%u: before %016llX award %016llX now %016llX -> %016llX (%s)\n", rec.ptr,
                                 u.addr - rec.ptr, (unsigned long long)_byteswap_uint64(u.before), (unsigned long long)_byteswap_uint64(u.after),
                                 (unsigned long long)_byteswap_uint64(live), (unsigned long long)_byteswap_uint64(result),
                                 conflict ? "a byte of the award's written again since - left as it is" : "the award's bytes put back, the rest kept");
            }
        }
        std::fprintf(stderr, "[award-journal] rewind to frame %d: the award of frame %d undone (%u / %u units, %u merged with later writes, "
                     "%u changed since, %u left to restore)\n", p, e.frame, put + merged, total, merged, changed, left);
    }
}

void AwardJournalSessionOn()
{
    std::lock_guard l(g_lock);
    g_journal.clear();
    g_last = Entry{};
    g_lastValid = false;
}

void AwardJournalSessionOver()
{
    std::lock_guard l(g_lock);
    g_lastValid = !g_journal.empty();
    g_last = g_lastValid ? std::move(g_journal.back()) : Entry{};
    g_overAt = std::chrono::steady_clock::now();
    g_journal.clear();
}

int32_t AwardJournalNewestFrame()
{
    std::lock_guard l(g_lock);
    return g_journal.empty() ? -1 : g_journal.back().frame;
}

// The program's one override of the end-of-battle step: the journal around gin_trace.cpp's trace (TTT2_GIN_TRACE) and the
// game's own step.
PPC_FUNC(sub_82654CE8)
{
    Pending p;
    const bool journal = Begin(base, ctx.r3.u32, p);
    gin_trace::ReplayStep(ctx, base);
    if (journal) End(base, p);
}
