// Frame overlap: in a round, the game's next frame runs while the command processor still draws the last one.
//
// TTT2's frame kick (sub_82AD1B88) calls sub_82AD1DE8(jobs, 1): it waits for the job workers, then inserts a GPU
// fence and blocks on it (sub_8273A288, sub_82738E70): every frame the game thread waits until the GPU has run all it
// was given. On the Xbox 360 the GPU keeps up as the commands arrive; here the GPU's front end is the command
// processor thread, which turns the frame into D3D11 work only then - so a frame costs the game thread's work PLUS the
// command processor's, never the larger of the two. MEASURED 2026-10-28 (diag build, player log 23, i3-3240 +
// GT 730, Modern Oasis): game thread 7.5 + command processor 15 ms a frame -> 37-44 fps, while each alone fits 16.7.
// Reproduced here with the command processor slowed to his speed (TTT2_DIAG_CP_SPIN_NS): 30-46 fps.
//
// In a round (battle state 0x82EAC03C == 4) the kick's wait takes the previous kick's fence instead. D3D's swap
// still waits for the previous swap (sub_827413B0 -> 82738DA0 on [device+15344]), so the command processor stays at
// most one frame behind and the frame pace is unchanged on a PC that keeps up.
//
// What the frame in flight reads that the next frame's work changes - MEASURED (TTT2_RB_GPUMEM kick check, all 27
// stages): command buffers (D3D's ring: its own waits guard it); the skinned vertices - the skinning ring at 82E40580
// (+4 region size, +8.. three region bases, +20 index, +24 cursor, +68 region count; sub_82AD5BA0 advances it once a
// frame): a frame's draws read the two regions before the current one, and the next frame's advance hands out the
// older of them again; and on some stages vertex / index data the game rewrites every frame in place (stage
// particles and the like: B4A00000 on Festive Parade, B4B00000 index data on Arctic Dream, ...).
//
// So, two guards:
// - the skinning ring gets a fourth region: at each advance the new slot gets whichever region no slot holds, and a
//   region is written again only four frames later;
// - the vertex, index and constant pages the command processor reads in a round are write-protected while a frame
//   is in flight: a thread that writes one waits (here, in the exception handler) until the command processor has
//   finished that frame, then the run is opened and protected again at the next kick. The pages are learnt from the
//   command processor's reads: the first frames of each round run in turn while it reads, and a page first read
//   later is protected as it is read. Pages that are never written cost one protection a round.
// Not in online sessions (rollback rewinds the ring, has its own scratch and write protection) and not outside rounds
// (character select's costume bakes are read back by the CPU). TTT2_FRAME_OVERLAP=0 turns it all off, =ring keeps
// only the fourth region, =check runs frames in turn but learns the pages (the TTT2_RB_GPUMEM kick check then counts
// changes the guard would have missed).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "kernel_shared.h"
#include "guest_heap.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

bool RollbackSessionOn();       // rollback.cpp
bool RollbackFightOnline();     // rollback.cpp: this fight's NACE input sync ran
void GpuPrescanKicked(bool scan, void (*note)(int kind, uint32_t addr, uint32_t bytes));   // kernel_video.cpp
extern std::atomic<uint32_t> g_gpuProgress;                 // kernel_video.cpp: bumped by every GPU-side write
bool WaitGpuProgress(uint32_t seen, unsigned long ms);      // kernel_video.cpp

namespace frame_overlap {
namespace {
constexpr uint32_t kRing = 0x82E40580u, kBattleState = 0x82EAC03Cu;
constexpr uint32_t kViewFirst = 0xA0000u, kViewEnd = 0xC0000u;   // the physical A view's pages (the GPU's memory)
constexpr int kLearnKicks = 8;                                   // frames in turn at the start of each round
uint32_t g_extra = 0;                   // the fourth region (0: not allocated)
bool g_extraFailed = false;
bool g_ringBroken = false;              // the slot objects did not hold the regions: no fourth region
bool g_ringRotates = false;             // the fourth region is in use (offline)
std::atomic<uint32_t> g_regions[4];     // the ring's four regions (guest), and their size
std::atomic<uint32_t> g_regionSize{ 0 };
uint32_t g_previousFence = 0;
bool g_active = false;                  // a round: pages are learnt (and frames overlap after kLearnKicks)
int g_learnt = 0;
std::atomic<uint32_t> g_roundKick{ 0 };  // p_diag62 check mode: kicks since the round began
thread_local bool t_viaBuffer = false;   // p_diag62: NoteBuffer's call
// p_diag66 TEST ONLY (TTT2_OVERLAP_WAITLOG=1): how each page was first noted (kind | 0x10 pre-scan | 0x20 whole buffer
// | 0x40 the page past a vertex read), and the waits' pages.
uint8_t g_noteHow[1u << 20];
bool WaitLogOn() { static const bool on = [] { const char* e = std::getenv("TTT2_OVERLAP_WAITLOG"); return e && *e == '1'; }(); return on; }
bool g_overlapNow = false;              // this kick waits for the previous one only
std::atomic<uint64_t> g_kicks{ 0 }, g_overlapped{ 0 };   // the report's (reset every report)
std::atomic<uint32_t> g_kickSerial{ 0 };                  // every kick, never reset (check mode's first-read stamps)
std::atomic<bool> g_abTurn{ false };                      // DIAGNOSTIC BUILD A/B: this report window runs frames in turn

// The write guard: the pages the frame in flight reads are protected while it is in flight. Each kick's target: the
// pre-scan of the frame just kicked (g_predicted) and the command processor's reads of the last two frames (g_readNow,
// g_readOld: buffers drawn past their fetch constant's size, a growing buffer's next page). Only that frame's pages:
// MEASURED 2026-10-28, a guard holding every page read in the round made the game thread wait ~13 ms a frame (Modern
// Oasis, the player's speed emulated) - a 2 MB ring of small dynamic vertex buffers (B3CC0000-B3EC0000) wraps every
// few seconds onto pages read long before. g_guarded: pages protected now (changed under g_mutex, read anywhere);
// g_runs: the protected runs (first page -> end page); g_inFlight: the fence of the frame the game no longer waited
// for (0: none).
constexpr uint32_t kWords = (1u << 20) / 64;
std::atomic<uint64_t> g_guarded[kWords];
std::mutex g_mutex;                     // g_runs, the page sets, g_guarded's changes
std::map<uint32_t, uint32_t> g_runs;
// A set of pages: a bitmap and its pages in the order added. Caller holds g_mutex.
struct PageSet
{
    std::vector<uint64_t> bits = std::vector<uint64_t>(kWords);
    std::vector<uint32_t> pages;
    bool Has(uint32_t p) const { return (bits[p >> 6] >> (p & 63)) & 1; }
    bool Add(uint32_t p)
    {
        uint64_t& w = bits[p >> 6];
        const uint64_t m = 1ull << (p & 63);
        if (w & m) return false;
        w |= m;
        pages.push_back(p);
        return true;
    }
    void Clear() { for (uint32_t p : pages) bits[p >> 6] = 0; pages.clear(); }
};
PageSet g_predicted, g_readNow, g_readOld, g_target;
// Copy-on-write (TTT2_OVERLAP_COW=0: wait instead): a thread writing a page the frame in flight reads copies the pages
// of its window first - as they were at the kick - and writes at once; the command processor reads the copies for
// that frame (CpReadBegin). MEASURED 2026-10-28 (Modern Oasis, the player's speed emulated): waiting instead cost the
// game thread 10-18 ms a frame, on the page a ring of small dynamic vertex buffers shares between the frame in flight's
// last buffers and the next frame's first. g_copied: pages with a copy (set under g_mutex, read anywhere); g_copies:
// page -> copy; g_copyFence: the frame they belong to; g_cpReading: command processor reads in progress (a window is
// opened only after the reads that may have begun on its live pages end).
std::atomic<uint64_t> g_copied[kWords];
std::unordered_map<uint32_t, uint8_t*> g_copies;
std::vector<uint8_t*> g_freeCopies;
std::atomic<uint32_t> g_copyCount{ 0 };
uint32_t g_copyFence = 0;
std::atomic<int> g_cpReading{ 0 };
std::atomic<bool> g_recording{ false };
std::atomic<uint32_t> g_round{ 1 };     // bumped with each target (the notes' cache)
// p75: buffers drawn past their fetch constant's size this round (first page -> end page of the furthest read); in
// every target for the rest of the round, kGrowMargin pages further (NoteGrowing).
std::map<uint32_t, uint32_t> g_growing;
constexpr uint32_t kGrowMargin = 8;
std::atomic<bool> g_anyGuarded{ false };
std::atomic<uint32_t> g_inFlight{ 0 };
std::atomic<uint32_t> g_device{ 0 };
std::vector<uint32_t> g_stamp;          // check mode: the kick whose target held each page
bool g_kickWouldOverlap = false;        // check mode: this kick would have overlapped
thread_local bool t_cpThread = false;
thread_local int t_retries = 0;
// The report's counters.
std::atomic<uint64_t> g_waits{ 0 }, g_waitNs{ 0 }, g_opened{ 0 }, g_cpOpened{ 0 }, g_lateRuns{ 0 }, g_protectCalls{ 0 },
    g_gaveUp{ 0 }, g_ioWaits{ 0 }, g_readWaits{ 0 }, g_copiedPages{ 0 }, g_copyOpens{ 0 }, g_copyReads{ 0 }, g_spinNs{ 0 },
    g_lateBits{ 0 };    // p83: write faults that found the bit set only under g_mutex (a Protect in progress)

// TTT2_FRAME_OVERLAP: 0 off, "ring" the fourth region only, "check" frames in turn with the pages learnt (tests),
// unset or anything else overlapped frames. Player log 24 (diag10, late frames flipped at once, Tulip Festival): the
// game thread worked 8.2 ms a frame and then waited 6.9 ms for the command processor, in 19 ms frames - the wait
// this removes (here, at the player's balance, frames in turn already fit 16.7 ms: no difference measured).
// p85 (diag12): off unless TTT2_FRAME_OVERLAP is set (1 = on). MEASURED 2026-10-29 (player logs 26/27, diag11 A/B): the
// same ~60 fps overlapped or in turn on Tulip Festival / Modern Oasis / Wayang Kulit (on 2 cores the command processor
// slows by what the game thread gains, and writes waiting for the frame in flight cost 2-10 ms a frame), and its write
// guard's race (p83) froze / crashed the game there.
int Mode()
{
    static const int mode = [] {
        const char* e = std::getenv("TTT2_FRAME_OVERLAP");
        return !e ? 0 : *e == '0' ? 0 : std::strcmp(e, "ring") == 0 ? 1 : std::strcmp(e, "check") == 0 ? 3 : 2;
    }();
    return mode;
}
bool Enabled() { return Mode() != 0; }
bool CowOn() { static const bool on = [] { const char* e = std::getenv("TTT2_OVERLAP_COW"); return !(e && *e == '0'); }(); return on; }
// The battle states where frames overlap: 4 a round of a match, 21 Practice (MEASURED 2026-10-28: 18 menus, 16 character
// select, then 21 from the stage load to the exit).
bool RoundState(uint32_t s) { return s == 4 || s == 21; }
bool Online() { return RollbackSessionOn() || RollbackFightOnline(); }
bool Bit(const std::atomic<uint64_t>* bits, uint32_t page) { return (bits[page >> 6].load(std::memory_order_acquire) >> (page & 63)) & 1; }
int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// D3D's fence test (sub_82738DA0): reached once the GPU-written value [[device+11024]] is at or past it, counted back
// from the newest fence [device+11036].
bool FenceReached(uint32_t fence)
{
    const uint32_t dev = g_device.load(std::memory_order_acquire);
    if (!dev) return true;
    const uint32_t newest = GRead32(g_base, dev + 11036), done = GRead32(g_base, GRead32(g_base, dev + 11024));
    return newest - fence >= newest - done;
}
void WaitFence(uint32_t fence)
{
    if (FenceReached(fence)) return;
    const int64_t t0 = NowNs();
    while (!FenceReached(fence))
    {
        const uint32_t seen = g_gpuProgress.load(std::memory_order_acquire);
        if (FenceReached(fence)) break;
        WaitGpuProgress(seen, 1);
        if (NowNs() - t0 > 200'000'000)
        {
            // The command processor did not finish the frame in 200 ms: never block the game on it for longer.
            if (g_gaveUp.fetch_add(1, std::memory_order_relaxed) < 8)
                std::fprintf(stderr, "[overlap] a write waited 200 ms for frame fence %08X: opened without it\n", fence);
            break;
        }
    }
    g_waits.fetch_add(1, std::memory_order_relaxed);
    g_waitNs.fetch_add(uint64_t(NowNs() - t0), std::memory_order_relaxed);
}

// Caller holds g_mutex.
void SetBits(std::atomic<uint64_t>* bits, uint32_t first, uint32_t end, bool on)
{
    for (uint32_t p = first; p < end; ++p)
    {
        const uint64_t m = 1ull << (p & 63);
        if (on) bits[p >> 6].fetch_or(m, std::memory_order_release);
        else bits[p >> 6].fetch_and(~m, std::memory_order_release);
    }
}
// p83: the bits first, then the protection. MEASURED 2026-10-29 (player logs 26/27, diag11, i3-3240 2 cores): with the
// protection first, a write landing between VirtualProtect and SetBits found a read-only page whose bit was not set yet,
// the handler passed it on, and the runtime dropped the writer's callback mid-way - the XMA decoder (log 27, an audio
// context page next to the skinning ring) and two job workers (log 26, right after "pages learnt"): freezes / crashes on
// Tulip Festival, Modern Oasis, Wayang Kulit. On a 2-core PC the protecting thread is preempted inside that window; never
// seen here. TEST ONLY: TTT2_DIAG_GUARD_OLD=1 = diag11's order and handler; TTT2_DIAG_PROTECT_DELAY_US=<us> waits between
// the two steps (a preempted thread).
bool GuardOld()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_GUARD_OLD"); return e && *e == '1'; }();
    return on;
}
void ProtectDelay()
{
    static const int64_t ns = [] { const char* e = std::getenv("TTT2_DIAG_PROTECT_DELAY_US"); return e ? std::atoll(e) * 1000 : 0LL; }();
    if (ns <= 0) return;
    const int64_t until = NowNs() + ns;
    while (NowNs() < until) YieldProcessor();
}
void Protect(uint32_t first, uint32_t end)
{
    DWORD old;
    if (GuardOld())
    {
        if (!VirtualProtect(g_base + (uint64_t(first) << 12), size_t(end - first) << 12, PAGE_READONLY, &old)) return;
        ProtectDelay();
        SetBits(g_guarded, first, end, true);
    }
    else
    {
        SetBits(g_guarded, first, end, true);
        ProtectDelay();
        if (!VirtualProtect(g_base + (uint64_t(first) << 12), size_t(end - first) << 12, PAGE_READONLY, &old))
        {
            SetBits(g_guarded, first, end, false);
            return;
        }
    }
    g_protectCalls.fetch_add(1, std::memory_order_relaxed);
    // Join neighbouring runs: a buffer the game rewrites is opened by one fault.
    auto next = g_runs.find(end);
    if (next != g_runs.end()) { end = next->second; g_runs.erase(next); }
    auto it = g_runs.lower_bound(first);
    if (it != g_runs.begin() && std::prev(it)->second == first) { --it; it->second = end; }
    else g_runs[first] = end;
    g_anyGuarded.store(true, std::memory_order_release);
}
// The pages as runs, each protected. Caller holds g_mutex.
void ProtectPages(std::vector<uint32_t>& pages)
{
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    for (size_t i = 0; i < pages.size();)
    {
        size_t j = i + 1;
        while (j < pages.size() && pages[j] == pages[j - 1] + 1) ++j;
        // Split around pages protected already (a run may hold some).
        for (uint32_t p = pages[i], end = pages[j - 1] + 1; p < end;)
        {
            if (Bit(g_guarded, p)) { ++p; continue; }
            uint32_t q = p + 1;
            while (q < end && !Bit(g_guarded, q)) ++q;
            Protect(p, q);
            p = q;
        }
        i = j;
    }
}
// Open the run holding the page; the next kick's target protects it again. Caller holds g_mutex.
void OpenRunOf(uint32_t page)
{
    auto it = g_runs.upper_bound(page);
    if (it == g_runs.begin()) return;
    --it;
    if (page >= it->second) return;
    const uint32_t first = it->first, end = it->second;
    g_runs.erase(it);
    DWORD old;
    VirtualProtect(g_base + (uint64_t(first) << 12), size_t(end - first) << 12, PAGE_READWRITE, &old);
    SetBits(g_guarded, first, end, false);
    g_opened.fetch_add(1, std::memory_order_relaxed);
}
// Open [first, end)'s protected pages, splitting the runs around them. Caller holds g_mutex.
void OpenPages(uint32_t first, uint32_t end)
{
    std::vector<std::pair<uint32_t, uint32_t>> hit;
    auto it = g_runs.upper_bound(first);
    if (it != g_runs.begin()) --it;
    for (; it != g_runs.end() && it->first < end; ++it)
        if (it->second > first) hit.emplace_back(it->first, it->second);
    DWORD old;
    for (const auto& [rf, re] : hit)
    {
        g_runs.erase(rf);
        const uint32_t of = std::max(rf, first), oe = std::min(re, end);
        VirtualProtect(g_base + (uint64_t(of) << 12), size_t(oe - of) << 12, PAGE_READWRITE, &old);
        SetBits(g_guarded, of, oe, false);
        if (rf < of) g_runs[rf] = of;
        if (oe < re) g_runs[oe] = re;
    }
}
// The page as it is now (protected: as it was at the kick). Caller holds g_mutex.
void CopyPage(uint32_t p)
{
    if (Bit(g_copied, p)) return;
    uint8_t* c;
    if (!g_freeCopies.empty()) { c = g_freeCopies.back(); g_freeCopies.pop_back(); }
    else c = new uint8_t[4096];
    std::memcpy(c, g_base + (uint64_t(p) << 12), 4096);
    g_copies[p] = c;
    g_copied[p >> 6].fetch_or(1ull << (p & 63), std::memory_order_seq_cst);
    g_copyCount.fetch_add(1, std::memory_order_seq_cst);
    g_copiedPages.fetch_add(1, std::memory_order_relaxed);
}
// The copies' frame is done. Caller holds g_mutex.
void DropCopies()
{
    for (const auto& [p, c] : g_copies)
    {
        g_copied[p >> 6].fetch_and(~(1ull << (p & 63)), std::memory_order_release);
        g_freeCopies.push_back(c);
    }
    g_copies.clear();
    g_copyCount.store(0, std::memory_order_release);
}
// A write to a page the frame in flight reads: copy the protected pages of its 64 KB window (in its run), wait for the
// command processor's reads in progress (they may be reading those pages live), open them. Returns false if the page
// is not protected (opened meanwhile).
bool CopyAndOpen(uint32_t page, uint32_t fence)
{
    uint32_t wFirst, wEnd;
    {
        std::lock_guard l(g_mutex);
        if (!Bit(g_guarded, page)) return false;
        auto it = g_runs.upper_bound(page);
        if (it == g_runs.begin()) return false;
        --it;
        if (page >= it->second) return false;
        wFirst = std::max(it->first, page & ~15u);
        wEnd = std::min(it->second, (page & ~15u) + 16);
        if (g_copyFence != fence) { DropCopies(); g_copyFence = fence; }
        for (uint32_t p = wFirst; p < wEnd; ++p) CopyPage(p);
    }
    if (g_cpReading.load(std::memory_order_seq_cst))
    {
        const int64_t t0 = NowNs();
        for (int spins = 0; g_cpReading.load(std::memory_order_seq_cst); ++spins)
        {
            if (spins > 64) SwitchToThread(); else YieldProcessor();
            if (NowNs() - t0 > 50'000'000)
            {
                static std::atomic<int> logged{ 0 };
                if (logged.fetch_add(1) < 4) std::fprintf(stderr, "[overlap] a copy-on-write waited 50 ms for the command processor's reads: opened\n");
                break;
            }
        }
        g_spinNs.fetch_add(uint64_t(NowNs() - t0), std::memory_order_relaxed);
    }
    std::lock_guard l(g_mutex);
    OpenPages(wFirst, wEnd);
    g_copyOpens.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// End of a round's guard: everything open, nothing learnt. Kick thread, nothing in flight.
void OpenAll()
{
    std::lock_guard l(g_mutex);
    DWORD old;
    for (const auto& [first, end] : g_runs)
    {
        VirtualProtect(g_base + (uint64_t(first) << 12), size_t(end - first) << 12, PAGE_READWRITE, &old);
        SetBits(g_guarded, first, end, false);
    }
    g_runs.clear();
    DropCopies();
    g_predicted.Clear();
    g_readNow.Clear();
    g_readOld.Clear();
    g_target.Clear();
    g_growing.clear();
    g_round.fetch_add(1, std::memory_order_release);
    g_anyGuarded.store(false, std::memory_order_release);
}

// The frame just kicked's target (above); then the reads move on a frame. Caller holds g_mutex.
void BuildTarget()
{
    g_target.Clear();
    for (const PageSet* s : { &g_predicted, &g_readNow, &g_readOld })
        for (uint32_t p : s->pages) g_target.Add(p);
    for (const auto& [first, end] : g_growing)
        for (uint32_t p = first, e = std::min(end + kGrowMargin, kViewEnd); p < e; ++p) g_target.Add(p);
    std::sort(g_target.pages.begin(), g_target.pages.end());
    g_predicted.Clear();
    g_readOld.Clear();
    std::swap(g_readOld, g_readNow);
    g_round.fetch_add(1, std::memory_order_release);
}
// Protect the target; open what it no longer holds. The previous frame is done: its copies go. Caller holds g_mutex.
void ApplyTarget()
{
    DropCopies();
    DWORD old;
    for (auto it = g_runs.begin(); it != g_runs.end();)
    {
        const uint32_t first = it->first, end = it->second;
        uint32_t p = first;
        while (p < end && g_target.Has(p)) ++p;
        if (p == end) { ++it; continue; }
        // A run with pages the frame in flight does not read: opened (its target pages are protected again below).
        it = g_runs.erase(it);
        VirtualProtect(g_base + (uint64_t(first) << 12), size_t(end - first) << 12, PAGE_READWRITE, &old);
        SetBits(g_guarded, first, end, false);
    }
    std::vector<uint32_t> todo;
    for (uint32_t p : g_target.pages) if (!Bit(g_guarded, p)) todo.push_back(p);
    ProtectPages(todo);
}

// A write to a page the frame in flight may read: wait for that frame, open the run, write again.
LONG CALLBACK GuardVeh(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t offset = uint64_t(r->ExceptionInformation[1]) - reinterpret_cast<uint64_t>(g_base);
    if (offset >= 0x100000000ull) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t page = uint32_t(offset >> 12);
    if (page < kViewFirst || page >= kViewEnd) return EXCEPTION_CONTINUE_SEARCH;
    if (!Bit(g_guarded, page))
    {
        // Opened by another thread after this write faulted (or at a round's end): write again if it is writable now.
        MEMORY_BASIC_INFORMATION mbi{};
        if (t_retries < 4 && VirtualQuery(g_base + (uint64_t(page) << 12), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT
            && mbi.Protect == PAGE_READWRITE)
        {
            ++t_retries;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        // p83: or being protected right now - Protect runs under g_mutex, so once it can be taken the bit is final.
        bool guarded = false;
        if (!GuardOld())
        {
            std::lock_guard l(g_mutex);
            guarded = Bit(g_guarded, page);
        }
        if (!guarded)
        {
            t_retries = 0;
            static std::atomic<int> logged{ 0 };
            if (logged.fetch_add(1) < 8)
                std::fprintf(stderr, "[overlap] a write fault on page %08X the guard does not hold - passed on\n", page << 12);
            return EXCEPTION_CONTINUE_SEARCH;
        }
        g_lateBits.fetch_add(1, std::memory_order_relaxed);
    }
    t_retries = 0;
    // The command processor's own threads never wait for themselves.
    const uint32_t fence = g_inFlight.load(std::memory_order_acquire);
    if (WaitLogOn() && !CowOn() && fence && !t_cpThread && !FenceReached(fence))
    {
        // p_diag66 TEST ONLY: what the waits are for.
        static std::atomic<int> logs{ 0 };
        const int64_t t0 = NowNs();
        WaitFence(fence);
        if (logs.fetch_add(1) < 400)
        {
            uint32_t rf = 0, re = 0;
            {
                std::lock_guard l(g_mutex);
                auto it = g_runs.upper_bound(page);
                if (it != g_runs.begin()) { --it; if (page < it->second) { rf = it->first; re = it->second; } }
            }
            std::fprintf(stderr, "[overlap] wait %.2f ms: page %08X (noted %02X), run %08X-%08X (%u pages), thread %lu, rip +%llX\n",
                         double(NowNs() - t0) / 1e6, page << 12, g_noteHow[page], rf << 12, re << 12, re - rf, GetCurrentThreadId(),
                         (unsigned long long)(ep->ContextRecord->Rip - reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr))));
        }
    }
    else if (fence && !t_cpThread && CowOn() && !FenceReached(fence))
    {
        if (!CopyAndOpen(page, fence) && t_retries < 4) ++t_retries;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    else if (fence && !t_cpThread) WaitFence(fence);
    else if (t_cpThread) g_cpOpened.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard l(g_mutex);
    OpenRunOf(page);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Memory noted since the last target adds nothing: a cache per noting thread (the pre-scan's kick thread, the command
// processor's), emptied with each target. A frame's draws name the same buffers over and over.
bool NotedBefore(uint32_t addr, uint32_t bytes)
{
    struct Entry { uint32_t addr, bytes, round; };
    thread_local std::unique_ptr<Entry[]> cache;
    if (!cache) cache.reset(new Entry[4096]());
    Entry& e = cache[((addr >> 4) * 2654435761u) >> 20];
    const uint32_t round = g_round.load(std::memory_order_acquire);
    if (e.addr == addr && e.bytes == bytes && e.round == round) return true;
    e = { addr, bytes, round };
    return false;
}
constexpr uint32_t kBufferCap = 4u << 20;   // vertex buffers noted whole up to this size
} // namespace
void NoteReadImpl(int kind, uint32_t addr, uint32_t bytes, bool kicked);

// After the game's advance (diag_hooks.cpp sub_82AD5BA0): the new slot gets the region no slot holds. Online, the
// slots go back to the game's own three regions.
//
// The draws do not read a slot's region through the base at +8: the bump's callers (sub_82AD46F0, sub_82AD4780) hand
// out the offset in the region and a 32-byte D3D buffer object of the slot (ring + 76 + 32 * index, ring + 172 +
// 32 * index), whose GPU address is the region's. So the slot's two objects get the new region's address too (the
// field holding it is learnt from the game's three; MEASURED 2026-10-28: without that, menus and fights drew stale
// vertices - garbled UI, streaks).
void RingAdvanced(uint8_t* base, uint32_t ring)
{
    if (ring != kRing || !Enabled() || g_ringBroken) return;
    const uint32_t n = GRead32(base, kRing + 68), size = GRead32(base, kRing + 4), index = GRead32(base, kRing + 20);
    if (n != 3 || index >= 3 || size < 0x10000 || size > (64u << 20)) return;
    const bool online = Online();
    if (online && !g_extra) return;
    if (!g_extra)
    {
        if (g_extraFailed) return;
        if (void* p = g_physicalHeap.Alloc(size, 0x1000)) g_extra = uint32_t(static_cast<uint8_t*>(p) - base);
        else
        {
            g_extraFailed = true;
            std::fprintf(stderr, "[overlap] no %u MB of physical memory for a fourth skinning region: frames do not overlap\n", size >> 20);
            return;
        }
        std::fprintf(stderr, "[overlap] skinning ring %08X: a fourth region at %08X (%u MB) beside %08X %08X %08X\n", kRing, g_extra, size >> 20,
                     GRead32(base, kRing + 8), GRead32(base, kRing + 12), GRead32(base, kRing + 16));
    }
    // The four regions: the game's three (its first ones, before any rotation) and ours. A slot holding anything else
    // means the game set the ring up again: its three are learnt anew.
    static uint32_t regions[4] = {};
    const uint32_t a = GRead32(base, kRing + 8 + 4 * ((index + 1) % 3)), b = GRead32(base, kRing + 8 + 4 * ((index + 2) % 3));
    const uint32_t cur = GRead32(base, kRing + 8 + 4 * index);
    auto known = [&](uint32_t v) { return v == regions[0] || v == regions[1] || v == regions[2] || v == regions[3]; };
    static int field[2] = { -1, -1 };                       // the address field in the slot objects at +76 / +172
    if (!regions[0] || !known(a) || !known(b) || !known(cur))
    {
        if (regions[0]) std::fprintf(stderr, "[overlap] the skinning ring was set up again (%08X %08X %08X)\n", a, b, cur);
        for (int i = 0; i < 3; ++i) regions[i] = GRead32(base, kRing + 8 + 4 * i);
        regions[3] = g_extra;
        for (int s = 0; s < 2; ++s)
        {
            field[s] = -1;
            for (int f = 0; f < 32 && field[s] < 0; f += 4)
            {
                bool all = true;
                for (uint32_t i = 0; i < 3; ++i)
                    all = all && (GRead32(base, kRing + (s ? 172 : 76) + 32 * i + f) & 0x1FFFFFFCu) == (regions[i] & 0x1FFFFFFCu);
                if (all) field[s] = f;
            }
        }
        if (field[0] < 0 || field[1] < 0)
        {
            g_ringBroken = true;
            std::fprintf(stderr, "[overlap] the skinning ring's buffer objects do not hold its regions (fields %d %d): no fourth region, frames do not overlap\n",
                         field[0], field[1]);
            return;
        }
        std::fprintf(stderr, "[overlap] skinning ring regions %08X %08X %08X + %08X; the slots' buffer objects hold them at +%d / +%d\n",
                     regions[0], regions[1], regions[2], regions[3], field[0], field[1]);
        for (int i = 0; i < 4; ++i) g_regions[i].store(regions[i], std::memory_order_relaxed);
        g_regionSize.store(size, std::memory_order_release);
    }
    // Offline the new slot gets the region none of the three slots held (the frame in flight reads two of them, the
    // next frame's draws another); online, a slot holding the fourth region gets back the game's own free one.
    uint32_t pick = 0;
    if (online)
    {
        if (cur != regions[3]) return;
        for (int i = 0; i < 3 && !pick; ++i) if (regions[i] != a && regions[i] != b) pick = regions[i];
    }
    else
        for (uint32_t r : regions) if (!pick && r != a && r != b && r != cur) pick = r;
    if (!pick) return;
    if (!online) g_ringRotates = true;
    GWrite32(base, kRing + 8 + 4 * index, pick);
    GWrite32(base, kRing + 24, pick);
    for (int s = 0; s < 2; ++s)
    {
        const uint32_t at = kRing + (s ? 172 : 76) + 32 * index + uint32_t(field[s]), v = GRead32(base, at);
        GWrite32(base, at, (v & ~0x1FFFFFFCu) | (pick & 0x1FFFFFFCu));
    }
}

// The pre-scan's memory (kernel_video.cpp GpuPrescanKicked): what the frame just kicked reads, known before it runs.
//
// p75: a small vertex buffer with one page more - its draws may fetch past its fetch constant's size (NoteGrowing);
// MEASURED 2026-10-29 (check mode, Festive Parade's first round): stage particle buffers first drawn mid-round read up
// to a page past it.
void NoteKicked(int kind, uint32_t addr, uint32_t bytes)
{
    if (!g_recording.load(std::memory_order_acquire) || (kind == 1 && bytes > kBufferCap) || NotedBefore(addr, bytes)) return;
    if (kind == 1 && bytes <= (256u << 10)) bytes += 4096;
    NoteReadImpl(kind, addr, bytes, true);
}

// The frame kick's fence (diag_hooks.cpp, before its sub_82738E70 wait): in a round, after the first frames, the
// previous kick's.
uint32_t KickFence(uint8_t* base, uint32_t fence)
{
    g_kicks.fetch_add(1, std::memory_order_relaxed);
    g_kickSerial.fetch_add(1, std::memory_order_relaxed);
    if (WaitLogOn()) { static const bool once = [] { std::fprintf(stderr, "[overlap] kick thread %lu\n", GetCurrentThreadId()); return true; }(); (void)once; }
    {
        // p_diag56: the battle state's changes (which values the modes use).
        static uint32_t lastState = ~0u;
        static int stateLogs = 0;
        const uint32_t s = GRead32(base, kBattleState);
        if (s != lastState && stateLogs < 200)
        {
            ++stateLogs;
            std::fprintf(stderr, "[overlap] battle state %u -> %u (next %u)\n", lastState, s, GRead32(base, kBattleState + 4));
        }
        lastState = s;
    }
    const bool active = Mode() >= 2 && g_extra && g_ringRotates && !g_ringBroken && !Online() && RoundState(GRead32(base, kBattleState))
                        && !g_abTurn.load(std::memory_order_relaxed);
    if (active != g_active)
    {
        static int logged = 0;
        if (logged++ < 40) std::fprintf(stderr, "[overlap] %s (battle state %u)\n", active ? "a round: pages learnt" : "frames in turn", GRead32(base, kBattleState));
        g_active = active;
        g_learnt = 0;
        g_roundKick.store(0, std::memory_order_relaxed);
        if (active) g_recording.store(true, std::memory_order_release);   // this frame's reads (during the wait) count
    }
    const uint32_t previous = g_previousFence;
    g_previousFence = fence;
    g_overlapNow = active && previous && g_learnt >= kLearnKicks;
    g_kickWouldOverlap = g_overlapNow;
    if (active && g_learnt < kLearnKicks) ++g_learnt;
    g_roundKick.fetch_add(1, std::memory_order_relaxed);
    if (Mode() == 3) g_overlapNow = false;                   // check: learn, but in turn
    // The frame just kicked: what it reads, from its packets (in rounds; elsewhere the scan position moves on).
    if (Mode() >= 2) GpuPrescanKicked(active, NoteKicked);
    if (Mode() == 3 && active)
    {
        // Check mode: the target this kick would protect, stamped; what the command processor reads after it is late.
        std::lock_guard l(g_mutex);
        BuildTarget();
        if (g_stamp.empty()) g_stamp.assign(1u << 20, 0);
        const uint32_t kick = g_kickSerial.load(std::memory_order_relaxed);
        for (uint32_t p : g_target.pages) g_stamp[p] = kick;
    }
    if (!g_overlapNow) return fence;
    g_overlapped.fetch_add(1, std::memory_order_relaxed);
    return previous;
}

// After the frame kick's wait (diag_hooks.cpp): the frame just kicked is in flight - protect what it may read.
void AfterKickWait(uint32_t fence)
{
    if (!g_active)
    {
        g_inFlight.store(0, std::memory_order_release);
        if (g_recording.exchange(false) || g_anyGuarded.load(std::memory_order_acquire)) OpenAll();
        return;
    }
    if (Mode() == 3)
    {
        g_inFlight.store(0, std::memory_order_release);     // check: the target was built at the kick
        return;
    }
    static const bool veh = [] { AddVectoredExceptionHandler(1, GuardVeh); return true; }();
    (void)veh;
    std::lock_guard l(g_mutex);
    BuildTarget();
    if (!g_overlapNow)
    {
        g_inFlight.store(0, std::memory_order_release);     // a round's first frames: in turn, the reads learnt
        return;
    }
    g_inFlight.store(fence, std::memory_order_release);
    ApplyTarget();
}

// The kick's device (diag_hooks.cpp sub_82738DA0 from the kick): for the fence test.
void NoteDevice(uint32_t device) { g_device.store(device, std::memory_order_release); }
// Command processor threads (kernel_video.cpp): never wait in the guard.
void MarkCpThread() { t_cpThread = true; }

// The command processor read guest memory (kernel_video.cpp GpuNoteMemory): kind 1 vertex, 2 index, 3 constants;
// or (kicked) the pre-scan found memory the frame just kicked reads.
//
// Vertex reads: one page past the end too, into the next targets. A buffer that grows (stage particles: MEASURED
// 2026-10-28, Festive Parade, 0x240 bytes a frame, the draws fetching past the fetch constant's size - their range
// comes from the indices) then has its next page protected before a frame in flight first reads it; check mode found
// such pages early in rounds on Festive Parade, Arctic Dream and Tempest.
void NoteReadImpl(int kind, uint32_t addr, uint32_t bytes, bool kicked)
{
    if (!g_recording.load(std::memory_order_acquire) || kind < 1 || kind > 3 || !bytes) return;
    const uint32_t first = std::max(addr >> 12, kViewFirst);
    const uint32_t readEnd = std::min(uint32_t((uint64_t(addr) + bytes + 4095) >> 12), kViewEnd);
    const uint32_t end = std::min(readEnd + (kind == 1 && !kicked ? 1u : 0u), kViewEnd);
    if (first >= end) return;
    const uint32_t regionPages = (g_regionSize.load(std::memory_order_acquire) + 4095) >> 12;
    uint32_t ring[4];
    for (int i = 0; i < 4; ++i) ring[i] = g_regions[i].load(std::memory_order_relaxed) >> 12;
    // A frame in flight reading a page outside its target: protected now (the game may have written it already; the
    // target's sources keep that rare).
    const uint32_t fence = kicked ? 0 : g_inFlight.load(std::memory_order_acquire);
    const bool inFlight = fence && !FenceReached(fence);
    thread_local std::vector<uint32_t> late;
    late.clear();
    uint32_t outside = 0, outFirst = 0, outEnd = 0;
    std::lock_guard l(g_mutex);
    PageSet& set = kicked ? g_predicted : g_readNow;
    for (uint32_t p = first; p < end; ++p)
    {
        bool inRing = false;
        for (uint32_t r : ring) if (r && p >= r && p < r + regionPages) inRing = true;
        if (inRing) continue;                                 // the fourth region guards the skinning ring
        if (!set.Add(p)) continue;
        if (WaitLogOn())
            g_noteHow[p] = uint8_t(kind | (kicked ? 0x10 : 0) | (t_viaBuffer ? 0x20 : 0) | (p >= readEnd ? 0x40 : 0));
        if (kicked || p >= readEnd || g_target.Has(p)) continue;
        if (inFlight && !Bit(g_guarded, p)) late.push_back(p);
        if (!outside++) outFirst = p;
        outEnd = p + 1;
    }
    if (!late.empty())
    {
        g_lateRuns.fetch_add(1, std::memory_order_relaxed);
        ProtectPages(late);
    }
    if (Mode() == 3 && outside)
    {
        // p_diag67 TEST ONLY: reads outside the kick's target, after a round's first frames - what the check may call
        // unguarded.
        static int logs = 0;
        if (g_roundKick.load(std::memory_order_relaxed) > kLearnKicks && logs < 400)
        {
            ++logs;
            std::fprintf(stderr, "[overlap] check: kick %u (round kick %u): %u pages %08X-%08X outside the target, from %s %08X +%X\n",
                         g_kickSerial.load(std::memory_order_relaxed), g_roundKick.load(std::memory_order_relaxed), outside,
                         outFirst << 12, outEnd << 12,
                         t_viaBuffer ? "a whole buffer" : kind == 1 ? "a vertex read" : kind == 2 ? "an index read" : "a constant read",
                         addr, bytes);
        }
    }
}

void NoteRead(int kind, uint32_t addr, uint32_t bytes) { NoteReadImpl(kind, addr, bytes, false); }

// The command processor drew from a vertex buffer (gpu_hw_d3d11.h): the fetch constant's whole range, not only the part
// the draw fetched. A buffer drawn in parts (stage particles: more of it as an effect grows) otherwise has a part first
// read by a frame in flight while the next frame rewrites it, and the pre-scan sees only buffers bound anew. MEASURED
// 2026-10-28 (check mode, Festive Parade / Arctic Dream / Tempest): 13-57 such pages early in a round, with the
// pre-scan too. Buffers over 4 MB keep the fetched part only.
// p75: a draw fetched past its vertex buffer's fetch constant (to addr + bytes). MEASURED 2026-10-29 (check mode, all
// stages): Festive Parade's stage particle buffers, drawn every few frames, each time a page or two further on, had
// 27 pages read by a frame in flight outside its target (the reads of the last two frames and the pre-scan cover
// neither the parts first read nor the growth between draws). Kept in the target for the rest of the round.
void NoteGrowing(uint32_t addr, uint32_t bytes)
{
    if (!g_recording.load(std::memory_order_acquire) || !bytes || bytes > kBufferCap) return;
    const uint32_t first = addr >> 12, end = uint32_t((uint64_t(addr) + bytes + 4095) >> 12);
    if (first < kViewFirst || end > kViewEnd) return;
    std::lock_guard l(g_mutex);
    uint32_t& e = g_growing[first];
    e = std::max(e, end);
}
void NoteBuffer(uint32_t addr, uint32_t bytes)
{
    if (!g_recording.load(std::memory_order_acquire) || !bytes || bytes > kBufferCap || NotedBefore(addr, bytes)) return;
    t_viaBuffer = true;
    NoteReadImpl(1, addr, bytes, false);
    t_viaBuffer = false;
}

// Before the host writes guest memory outside the exception path (kernel_io.cpp ReadFile): open protected pages.
void BeforeHostWrite(uint32_t addr, uint32_t size)
{
    if (!g_anyGuarded.load(std::memory_order_acquire) || !size) return;
    const uint32_t first = std::max(addr >> 12, kViewFirst), end = std::min(uint32_t((uint64_t(addr) + size + 4095) >> 12), kViewEnd);
    bool any = false;
    for (uint32_t p = first; p < end && !any; ++p) any = Bit(g_guarded, p);
    if (!any) return;
    g_ioWaits.fetch_add(1, std::memory_order_relaxed);
    const uint32_t fence = g_inFlight.load(std::memory_order_acquire);
    if (fence && !t_cpThread) WaitFence(fence);
    std::lock_guard l(g_mutex);
    for (uint32_t p = first; p < end; ++p)
        if (Bit(g_guarded, p)) OpenRunOf(p);
}

// Before the CPU reads what the GPU wrote (phys_mirror.cpp, the cached view's first access to a chunk: the resolves
// covering it are written back): the frame in flight's resolves first.
void BeforeGpuDataRead()
{
    const uint32_t fence = g_inFlight.load(std::memory_order_acquire);
    if (!fence || t_cpThread || FenceReached(fence)) return;
    g_readWaits.fetch_add(1, std::memory_order_relaxed);
    WaitFence(fence);
}
bool InRound(uint32_t battleState) { return RoundState(battleState); }
bool On() { return Enabled(); }   // low_latency.cpp: TTT2_FRAME_OVERLAP set (any mode)

// The command processor reads vertex, index or constant data (gpu_hw_d3d11.h, gpu_render.cpp, kernel_video.cpp):
// [addr, addr + bytes) as the frame it draws sees it - guest memory, or, where the game wrote pages that frame reads
// after its kick, a copy in `scratch` with those pages as they were. Valid until CpReadEnd, which must follow.
const uint8_t* CpReadBegin(uint32_t addr, uint32_t bytes, std::vector<uint8_t>& scratch)
{
    g_cpReading.fetch_add(1, std::memory_order_seq_cst);
    if (!bytes || !g_copyCount.load(std::memory_order_seq_cst)) return g_base + addr;
    const uint32_t first = std::max(addr >> 12, kViewFirst), end = std::min(uint32_t((uint64_t(addr) + bytes + 4095) >> 12), kViewEnd);
    bool any = false;
    for (uint32_t p = first; p < end && !any; ++p) any = Bit(g_copied, p);
    if (!any) return g_base + addr;
    std::lock_guard l(g_mutex);
    if (!g_copyFence || FenceReached(g_copyFence)) return g_base + addr;   // past the copies' frame: memory is this one's
    scratch.resize(bytes);
    for (uint32_t done = 0; done < bytes;)
    {
        const uint32_t a = addr + done, n = std::min(bytes - done, 4096 - (a & 4095));
        const auto it = g_copies.find(a >> 12);
        std::memcpy(scratch.data() + done, it != g_copies.end() ? it->second + (a & 4095) : g_base + a, n);
        done += n;
    }
    g_copyReads.fetch_add(1, std::memory_order_relaxed);
    return scratch.data();
}
void CpReadEnd() { g_cpReading.fetch_sub(1, std::memory_order_seq_cst); }
// The same for a reader of guest addresses (gpu_render.cpp's CPU vertex fetch, up to 64 bytes): the address to read.
uint32_t CpReadGuestBegin(uint32_t addr, uint32_t bytes)
{
    thread_local std::vector<uint8_t> scratch;
    const uint8_t* p = CpReadBegin(addr, bytes, scratch);
    if (p == g_base + addr || bytes > 64) return addr;
    thread_local uint32_t guest = 0;
    if (!guest)
        if (void* a = g_virtualHeap.Alloc(64, 16)) guest = uint32_t(static_cast<uint8_t*>(a) - g_base);
    if (!guest) return addr;
    std::memcpy(g_base + guest, p, bytes);
    return guest;
}

// TTT2_RB_GPUMEM's kick check (kernel_video.cpp), check mode: the kick count, whether this kick would have
// overlapped, and whether a page would have been protected by then (first read before kick `kick`).
uint64_t KickCount() { return g_kickSerial.load(std::memory_order_relaxed); }
bool KickWouldOverlap() { return Mode() == 3 && g_kickWouldOverlap; }
bool WouldGuard(uint32_t page, uint64_t kick)
{
    if (page < kViewFirst || page >= kViewEnd) return false;
    std::lock_guard l(g_mutex);
    return !g_stamp.empty() && g_stamp[page] == uint32_t(kick);
}

// DIAGNOSTIC BUILD A/B (TTT2_DIAG_OVERLAP_AB=0 off): report windows alternate between overlapped frames and frames in
// turn, so a player's log compares both on his PC (gpu_hw_d3d11.h AbSwitch; the window's way is in the report).
bool AbOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_OVERLAP_AB"); return !(e && *e == '0') && Mode() == 2; }();
    return on;
}
void AbToggle() { if (AbOn()) g_abTurn.store(!g_abTurn.load(std::memory_order_relaxed), std::memory_order_relaxed); }
// p103: frames may be drawn from copies of guest memory (CpReadBegin) - the GPU backend's index cache stays off.
bool CopiesPossible() { return Mode() >= 2; }
bool AbTurn() { return g_abTurn.load(std::memory_order_relaxed); }

// The 5-s report: kicks, and those that waited for the previous frame only.
void TakeStats(uint64_t& kicks, uint64_t& overlapped)
{
    kicks = g_kicks.exchange(0, std::memory_order_relaxed);
    overlapped = g_overlapped.exchange(0, std::memory_order_relaxed);
}
// The guard's part of the report, per frame.
std::string TakeGuardStats(double presents)
{
    if (presents <= 0) presents = 1;
    size_t runs = 0, pages = 0;
    {
        std::lock_guard l(g_mutex);
        runs = g_runs.size();
        for (const auto& [first, end] : g_runs) pages += end - first;
    }
    char line[800];
    std::snprintf(line, sizeof(line),
                  "writes that waited for the frame in flight %.2f (%.2f ms), runs opened %.2f (by the command processor %llu), "
                  "protections %.2f (late reads %llu); protected now %zu pages in %zu runs; gave up %llu, file reads %llu, "
                  "reads of GPU data %llu; copy-on-write: %.2f pages (%.2f opens, %.3f ms waiting for reads), reads from copies %.2f; "
                  "writes during a protection %llu",
                  double(g_waits.exchange(0)) / presents, double(g_waitNs.exchange(0)) / 1e6 / presents,
                  double(g_opened.exchange(0)) / presents, (unsigned long long)g_cpOpened.exchange(0),
                  double(g_protectCalls.exchange(0)) / presents, (unsigned long long)g_lateRuns.exchange(0), pages, runs,
                  (unsigned long long)g_gaveUp.load(), (unsigned long long)g_ioWaits.exchange(0),
                  (unsigned long long)g_readWaits.exchange(0), double(g_copiedPages.exchange(0)) / presents,
                  double(g_copyOpens.exchange(0)) / presents, double(g_spinNs.exchange(0)) / 1e6 / presents,
                  double(g_copyReads.exchange(0)) / presents, (unsigned long long)g_lateBits.exchange(0));
    return line;
}
} // namespace frame_overlap
