// Guest threads: ExCreateThread spawns a real host thread that sets up its own PCR/TLS/TEB/stack
// and runs the guest start routine. Layout ported from Unleashed's GuestThreadContext.
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "guest_heap.h"
#include "kernel_objects.h"
#include "log_utf8.h"
#include <thread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
#include <process.h>
#include <cstddef>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <cwchar>
#include <map>
#include <string>
#include <unordered_map>

uint8_t* g_base = nullptr;

// Registry of guest threads (host tid -> guest entry fn) so thread dumps can name them.
void DiagNameThread(const wchar_t* name);   // TEST ONLY (below, outside the anonymous namespace)
uint64_t DiagMainOffBegin();                // DIAGNOSTIC BUILD (p72, below): the game main thread's time off the processor
void DiagMainOffEnd(int kind, uint64_t t0);
void RollbackNoteHeapBlock(uint32_t ptr, uint32_t size);          // rollback.cpp: word masks - never rewound
void RollbackNoteHeapFree(uint32_t ptr);
uint32_t RollbackArenaCarveNoGrow(uint32_t size, uint32_t align);  // rollback.cpp: the never-rewound arena (0: no room)
bool RollbackThreadBlockReleased(uint32_t lo, uint32_t size);      // rollback.cpp: Havok routers on a given-back stack
void RollbackGuestThreadStarted();                                 // rollback.cpp: a dead foreign thread's id, reused
uint64_t RollbackRestoreCount();                                   // rollback.cpp: snapshot restores so far
void RollbackSaveJobMark(const char* what, bool request, double ms);   // rollback.cpp: the save job's timeline (log only)
void XamSaveJobBoxClear();                // kernel_ui.cpp: the job's system box is gone with it
void XamSaveJobAwake();                   // kernel_ui.cpp: the job's sleep returned (an answered box is seen now)
// This host thread runs the save manager's save / load job (set for each guest thread this file starts): kernel_ui.cpp's
// "the job asks the player" flag. Not by the thread records (with TTT2_THREAD_RECYCLE=0 a reused id finds a dead
// thread's entry - review wf_fd6326af-ace).
thread_local bool t_saveJobThread = false;
bool KernelOnSaveJobThread() { return t_saveJobThread; }
namespace {
std::mutex g_gtMutex;
struct GuestThreadRec { unsigned long tid; uint32_t entry, ctxArg; };
std::vector<GuestThreadRec> g_guestThreads;
uint64_t g_guestThreadsEnded = 0;                 // under g_gtMutex
// Guest threads that end give back their thread block (PCR / TLS / TEB / stack), context-registry slot and record when
// their start routine returns (2026-11-09: every save starts a new thread - 4-5 an online match - and each one kept its
// 260 KB block and registry slot for good: the 512 slots ran out after ~100 matches). TTT2_THREAD_RECYCLE=0
// (diagnostics): kept for good, no spare blocks at a fight's calibration, blocks 0x40E90 bytes - as before.
bool ThreadRecycleOn()
{
    static const bool on = [] {
        const char* e = std::getenv("TTT2_THREAD_RECYCLE");
        const bool v = !(e && *e == '0');
        if (!v) std::fprintf(stderr, "[thr] TTT2_THREAD_RECYCLE=0: thread blocks, context slots and thread records are never given back (as before)\n");
        return v;
    }();
    return on;
}
void DropThreadRecordLocked(unsigned long tid)
{
    g_guestThreads.erase(std::remove_if(g_guestThreads.begin(), g_guestThreads.end(),
                                        [tid](const GuestThreadRec& t) { return t.tid == tid; }), g_guestThreads.end());
}
}
void HitchRegisterThread(const char* name);   // diag_hooks.cpp (TTT2_HITCH_PROFILE=3)
void KernelRegisterGuestThread(uint32_t entry, uint32_t ctxArg)
{
    {
        std::lock_guard lk(g_gtMutex);
        // A record still under this id is a dead thread's (Windows reuses thread ids; lookups take the first match).
        if (ThreadRecycleOn()) DropThreadRecordLocked(GetCurrentThreadId());
        g_guestThreads.push_back({ GetCurrentThreadId(), entry, ctxArg });
    }
    // The game's two thread wrappers name the real routine in their start block: 82A74760 calls {fn, arg} (its
    // ThreadClass threads: stage particles, snow field, loaders...), 82AE7090 sets affinity / priority and calls
    // fn(arg) from +20 / +24 (the job workers). "guest <wrapper>><routine>" tells them apart in the report.
    uint32_t routine = 0;
    if (ctxArg && ctxArg < 0xF0000000u && (entry == 0x82A74760u || entry == 0x82AE7090u))
        routine = GRead32(g_base, ctxArg + (entry == 0x82AE7090u ? 20 : 0));
    char name[32];
    if (routine) std::snprintf(name, sizeof(name), "guest %08X>%08X", entry, routine);
    else std::snprintf(name, sizeof(name), "guest %08X", entry);
    HitchRegisterThread(name);
    wchar_t wide[32];                           // DIAGNOSTIC BUILD (p72): the per-thread report's name
    if (routine) std::swprintf(wide, 32, L"guest %08X>%08X", entry, routine);
    else std::swprintf(wide, 32, L"guest %08X", entry);
    DiagNameThread(wide);
}
uint32_t KernelGuestThreadEntry(unsigned long tid)
{
    std::lock_guard lk(g_gtMutex);
    for (const auto& t : g_guestThreads)
        if (t.tid == tid) return t.entry;
    return 0;
}
// rollback.cpp (SessionSaveBeforeRestore): live guest threads started at `entry` - a record is dropped when its thread
// ends (thread recycling; with TTT2_THREAD_RECYCLE=0 dead threads keep theirs, so this says 0).
int KernelGuestThreadsWithEntry(uint32_t entry)
{
    if (!ThreadRecycleOn()) return 0;
    std::lock_guard lk(g_gtMutex);
    int n = 0;
    for (const auto& t : g_guestThreads) n += t.entry == entry;
    return n;
}
// rollback.cpp (SessionWaitSaveJob): the host thread id of a live guest thread started at `entry`, 0: none.
unsigned long KernelGuestThreadTidWithEntry(uint32_t entry)
{
    if (!ThreadRecycleOn()) return 0;
    std::lock_guard lk(g_gtMutex);
    for (const auto& t : g_guestThreads)
        if (t.entry == entry) return t.tid;
    return 0;
}
void KernelDumpGuestThreads()
{
    std::lock_guard lk(g_gtMutex);
    for (auto& t : g_guestThreads)
        std::fprintf(stderr, "[threads] tid=%lu entry=%08X ctx=%08X\n", t.tid, t.entry, t.ctxArg);
    if (g_guestThreadsEnded)
        std::fprintf(stderr, "[threads] %llu guest threads ended so far (their records dropped)\n", (unsigned long long)g_guestThreadsEnded);
}

// Read-only diagnostic registry for out-of-process samplers (port/tools/profile_threads.py):
// host thread id -> the PPCContext of the guest call currently running on that thread, so a
// sampler can read guest r1/lr while the thread is suspended and walk the real PPC backchain
// (host stack scans only find stale return addresses). Writers are the owning threads; a slot
// is claimed once per host thread. The header records the PPCContext
// field offsets so the reader does not depend on this build's register-config macros. A guest thread that ends gives
// its slot back (count stays the high-water mark readers scan to): a reader must check a slot's tid is still the one
// it sampled.
struct GuestContextSlot { uint32_t tid; uint32_t entry; PPCContext* ctx; };
static constexpr uint32_t GuestContextCapacity = 512;
extern "C" struct GuestContextRegistry
{
    uint32_t magic, version, r1Offset, lrOffset, capacity;
    std::atomic<uint32_t> count;
    GuestContextSlot slots[GuestContextCapacity];
} g_guestContextRegistry = { 0x47435458u /* 'XTCG' */, 1, offsetof(PPCContext, r1),
                             offsetof(PPCContext, lr), GuestContextCapacity, {0}, {} };

namespace {
thread_local GuestContextSlot* t_contextSlot = nullptr;
std::mutex g_slotMutex;
std::vector<uint32_t> g_freeSlots;               // under g_slotMutex: slots given back by guest threads that ended
uint64_t g_slotsReused = 0, g_slotsStale = 0;    // under g_slotMutex
// A slot for this host thread: one given back, else a new one. Readers take the FIRST slot with a tid, so a slot still
// under this thread's id - a dead host thread's (Windows reuses ids) - is retired first: it would hide this thread
// (MEASURED t4d: tid 53792, a save worker counted as foreign for a whole match).
GuestContextSlot* ClaimContextSlot()
{
    const uint32_t me = GetCurrentThreadId();
    auto& r = g_guestContextRegistry;
    if (!ThreadRecycleOn())
    {
        const uint32_t index = r.count.fetch_add(1);
        if (index >= GuestContextCapacity) return nullptr;
        r.slots[index].tid = me;
        return &r.slots[index];
    }
    std::lock_guard l(g_slotMutex);
    const uint32_t n = (std::min)(r.count.load(), GuestContextCapacity);
    for (uint32_t i = 0; i < n; ++i)
        if (r.slots[i].tid == me)
        {
            r.slots[i].tid = 0;
            std::atomic_thread_fence(std::memory_order_release);
            r.slots[i].entry = 0;
            r.slots[i].ctx = nullptr;
            g_freeSlots.push_back(i);
            ++g_slotsStale;
        }
    uint32_t index = 0;
    if (!g_freeSlots.empty()) { index = g_freeSlots.back(); g_freeSlots.pop_back(); ++g_slotsReused; }
    else
    {
        if (r.count.load() >= GuestContextCapacity)
        {
            static bool told = false;
            if (!told) { told = true; std::fprintf(stderr, "[thr] the guest context registry is full (%u slots): new threads run without one\n", GuestContextCapacity); }
            return nullptr;
        }
        index = r.count.fetch_add(1);
    }
    r.slots[index].entry = 0;
    r.slots[index].ctx = nullptr;
    std::atomic_thread_fence(std::memory_order_release);
    r.slots[index].tid = me;
    return &r.slots[index];
}
// GuestThreadEnded: the slot goes back. tid first: from then on no reader matches it (tid 0 is never a host thread's).
uint32_t ReleaseContextSlot()
{
    GuestContextSlot* s = t_contextSlot;
    if (!s || !ThreadRecycleOn()) return ~0u;
    t_contextSlot = nullptr;
    s->tid = 0;
    std::atomic_thread_fence(std::memory_order_release);
    s->entry = 0;
    s->ctx = nullptr;
    const uint32_t index = uint32_t(s - g_guestContextRegistry.slots);
    std::lock_guard l(g_slotMutex);
    g_freeSlots.push_back(index);
    return index;
}
// Publish ctx as this thread's active guest context; returns the previous one for nesting.
PPCContext* PublishGuestContext(PPCContext* ctx, uint32_t entry)
{
    if (!t_contextSlot && !(t_contextSlot = ClaimContextSlot())) return nullptr;
    PPCContext* previous = t_contextSlot->ctx;
    t_contextSlot->entry = entry;
    t_contextSlot->ctx = ctx;
    return previous;
}
struct ScopedGuestContext
{
    PPCContext* previous;
    uint32_t previousEntry;
    ScopedGuestContext(PPCContext* ctx, uint32_t entry)
        : previousEntry(t_contextSlot ? t_contextSlot->entry : 0)
    { previous = PublishGuestContext(ctx, entry); }
    ~ScopedGuestContext()
    {
        if (t_contextSlot) { t_contextSlot->ctx = previous; t_contextSlot->entry = previousEntry; }
    }
};
}
void KernelPublishMainGuestContext(PPCContext* ctx) { PublishGuestContext(ctx, 0xFFFFFFFFu); }
uint32_t CurrentGuestEntry() { return t_contextSlot ? t_contextSlot->entry : 0; }
PPCContext* CurrentGuestContext() { return t_contextSlot ? t_contextSlot->ctx : nullptr; }

// TTT2_WTRACE=1: log the guest caller (lr) of wait/sleep primitives to find spin loops.
bool WTrace() { static const bool on = std::getenv("TTT2_WTRACE") != nullptr; return on; }

namespace {
constexpr uint32_t PCR_SIZE = 0xAB0, TLS_SIZE = 0x100, TEB_SIZE = 0x2E0, STACK_SIZE = 0x40000;
constexpr uint32_t TOTAL = PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE;
// A pooled block spans whole pages: rollback.cpp excludes blocks page by page, so nothing else may share the last one
// (a TOTAL-sized block shared it with the heap's next block). The layout inside is unchanged: the stack top stays at +TOTAL.
constexpr uint32_t SPAN = (TOTAL + 0xFFFu) & ~0xFFFu;          // 0x41000
uint32_t BlockBytes() { return ThreadRecycleOn() ? SPAN : TOTAL; }
std::atomic<uint32_t> g_threadIdCounter{2};   // main thread is 1

void put32be(uint8_t* p, uint32_t v) { *reinterpret_cast<uint32_t*>(p) = __builtin_bswap32(v); }

// Rollback (rollback.cpp) keeps every guest thread block out of its state: each fight's calibration excludes the blocks
// in g_blocks page by page - never protected, never hot, never written by a restore. A thread created after that (the
// save worker 8258B2C0 starts at each KO) got a fresh block from g_virtualHeap that was NOT excluded: its stack and PCR
// pages turned hot and every restore wrote that fight's old contents back into a live stack (MEASURED t4a: the stack-top
// and PCR pages of every save worker; no crash seen - the saves ran after the rewinds - but nothing prevented it).
// So blocks are pooled: a block never goes back to g_virtualHeap, it stays in g_blocks (excluded again at every
// calibration, so no other allocation can land in an excluded page), and while a fight is tracked a thread only gets a
// block made before that fight's calibration. Spares are made at the calibration start (nothing protected, no image yet:
// the heap writes its block headers where no rewind can reach - ArenaReserve's rule).
std::mutex g_blocksMutex;
std::vector<std::pair<uint32_t, uint32_t>> g_blocks;   // guest base, size of every thread block - never removed, never freed
// Blocks given back by guest threads that ended, and spares, oldest first (all under g_blocksMutex). epoch = g_blockEpoch
// when the block was made (or given back during a tracked fight). While a fight is tracked a block is handed out only
// when it is older than that fight's calibration: it was in the list StartCalibration excluded.
struct FreeBlock { uint32_t g, epoch; };
std::deque<FreeBlock> g_freeBlocks;
uint32_t g_blockEpoch = 0;                 // fight calibrations so far (GuestThreadBlocksForFight)
bool g_blockTracking = false;              // a fight is tracked: from its calibration to its end (rollback.cpp)
bool g_fightOn = false;                    // the same, also with TTT2_THREAD_RECYCLE=0 (the fight-over line's count)
std::vector<uint32_t> g_maskedBlocks;      // made from the heap while a fight was tracked: masked until the next calibration
uint32_t g_madeTracked = 0, g_madeThisFight = 0, g_spareExtra = 0;
uint64_t g_blocksMade = 0, g_blocksTaken = 0, g_blocksGivenBack = 0, g_blocksMadeTracked = 0, g_blocksKeptOut = 0;
thread_local uint32_t t_blockEpoch = 0;    // this thread's block's epoch
thread_local bool t_blockHeapTracked = false;   // made from the heap during a tracked fight: never pooled
thread_local uint64_t t_restores0 = 0;     // RollbackRestoreCount() when the block was set up
// TTT2_THREAD_SPARE=<0..16>: free blocks ready at each fight's calibration (default 4; MEASURED: at most 2 guest threads -
// the save workers 8258B2C0 - start between a calibration and an online fight's end). A fight that still had to make blocks
// adds that many for the following ones (up to 16).
int SpareBlocks()
{
    static const int v = [] { const char* e = std::getenv("TTT2_THREAD_SPARE"); return e ? std::clamp(std::atoi(e), 0, 16) : 4; }();
    return v;
}
bool LogBlockEvent(uint64_t n) { return n <= 40 || n % 100 == 0; }

// Set up a guest thread block (a free one, else a new one), returning its guest base and stack-top.
uint32_t SetupThreadBlock(uint32_t threadId, uint32_t& stackTopOut)
{
    const uint32_t bytes = BlockBytes();
    uint32_t g = 0;
    bool tracking = false, tracked = false, fromArena = false;
    size_t freeLeft = 0;
    uint64_t taken = 0, made = 0;
    t_blockHeapTracked = false;
    if (ThreadRecycleOn())
    {
        std::lock_guard l(g_blocksMutex);
        for (auto it = g_freeBlocks.begin(); it != g_freeBlocks.end(); ++it)
            if (!g_blockTracking || it->epoch < g_blockEpoch)
            {
                g = it->g;
                t_blockEpoch = it->epoch;
                g_freeBlocks.erase(it);
                taken = ++g_blocksTaken;
                break;
            }
        freeLeft = g_freeBlocks.size();
        tracking = g_blockTracking;
    }
    if (!g)
    {
        // During a tracked fight: from the never-rewound arena if it has room (no guest heap header written mid-fight).
        if (tracking) { g = RollbackArenaCarveNoGrow(bytes, 0x1000); fromArena = g != 0; }
        if (!g)
        {
            void* host = g_virtualHeap.Alloc(bytes, 0x1000);
            if (!host)
            {
                std::fprintf(stderr, "[runtime] FATAL: guest memory exhausted - no thread block (%u bytes)\n", bytes);
                std::fflush(stderr);
                std::abort();
            }
            g = GuestAddr(g_base, host);
        }
        std::lock_guard l(g_blocksMutex);
        g_blocks.push_back({ g, bytes });
        t_blockEpoch = g_blockEpoch;
        // Either read: a heap block allocated while the fight was still tracked had its heap headers written where a
        // rewind could reach them, even if the fight ended before this lock (review 2026-11-09) - never pooled.
        tracked = ThreadRecycleOn() && (tracking || g_blockTracking);
        if (g_fightOn) ++g_madeThisFight;
        if (tracked)
        {
            ++g_madeTracked;
            ++g_blocksMadeTracked;
            // Not excluded in this fight. Every word masked - never rewound, whichever of its pages turns hot - from the
            // next restore on (an arena block: its chunk is masked whole already); dropped at the next calibration, which
            // excludes the block. Residual, as every thread had before the pool: g_virtualHeap's headers around the block
            // are on pages a rewind can reach, and a restore already running may still copy over hot pages - reached
            // only when the pool's spares and the arena's 2 MB spare are both used up in one fight.
            if (!fromArena)
            {
                g_maskedBlocks.push_back(g);
                RollbackNoteHeapBlock(g, bytes);
                t_blockHeapTracked = true;
            }
        }
        made = ++g_blocksMade;
    }
    // A pooled block: host events still bound to guest headers in it (bound after it was given back) are forgotten before
    // its new owner builds its own objects there.
    if (taken) KernelForgetGuestObjects(g, bytes);
    t_restores0 = RollbackRestoreCount();
    uint8_t* thr = g_base + g;
    std::memset(thr, 0, bytes);
    put32be(thr + 0x00,  g + PCR_SIZE);
    put32be(thr + 0x100, g + PCR_SIZE + TLS_SIZE);
    put32be(thr + PCR_SIZE + 0x10, 0xFFFFFFFF);
    put32be(thr + PCR_SIZE + TLS_SIZE + 0x14C, threadId);
    stackTopOut = g + TOTAL;
    const DWORD tid = GetCurrentThreadId();
    const uint32_t entry = KernelGuestThreadEntry(tid);
    if (tracked)
    {
        static std::atomic<int> trackedLines{ 0 };
        if (trackedLines.fetch_add(1) < 50)
            std::fprintf(stderr, "[thr] no free thread block older than this fight's calibration: %08X+%X made from %s for thread %lu (entry %08X) - %s\n",
                         g, bytes, fromArena ? "the never-rewound arena" : "the heap", tid, entry,
                         fromArena ? "never rewound (the arena's masks)" : "every word masked (never rewound), never pooled");
    }
    else if (made)
        std::fprintf(stderr, "[thr] thread block %08X+%X made for thread %lu (entry %08X; %llu made in all)\n", g, bytes, tid, entry, (unsigned long long)made);
    else if (LogBlockEvent(taken))
        std::fprintf(stderr, "[thr] thread %lu (entry %08X) takes free thread block %08X (epoch %u; %zu free left; %llu taken so far)\n",
                     tid, entry, g, t_blockEpoch, freeLeft, (unsigned long long)taken);
    return g;
}
} // namespace

// The guest thread blocks (PCR / TLS / TEB / stack) of every host thread that ran guest code, in use or free; the
// main thread's block at 0x40000000 is not in the list. rollback.cpp keeps them out of its state.
void GuestThreadBlocks(std::vector<std::pair<uint32_t, uint32_t>>& out)
{
    std::lock_guard l(g_blocksMutex);
    out = g_blocks;
}
// rollback.cpp's StartCalibration (main thread; nothing write-protected, every snapshot just dropped): spare blocks made
// now - g_virtualHeap writes its headers before any image exists (ArenaReserve's rule) - then a new epoch: a block made
// from here to the fight's end is not excluded in it. The masks of blocks the last fight had to make are dropped: they
// are excluded from now on.
void GuestThreadBlocksForFight(std::vector<std::pair<uint32_t, uint32_t>>& out)
{
    std::vector<uint32_t> unmask;
    uint32_t spares = 0;
    if (ThreadRecycleOn())
    {
        size_t have = 0, want = 0;
        {
            std::lock_guard l(g_blocksMutex);
            g_spareExtra = (std::min)(16u, g_spareExtra + g_madeTracked);   // the last fight ran short by that many
            g_madeTracked = 0;
            have = g_freeBlocks.size();
            want = (std::min)(size_t(16), size_t(SpareBlocks()) + g_spareExtra);
        }
        for (; have < want; ++have)
        {
            void* host = g_virtualHeap.Alloc(SPAN, 0x1000);
            if (!host) break;
            const uint32_t g = GuestAddr(g_base, host);
            {
                std::lock_guard l(g_blocksMutex);
                g_blocks.push_back({ g, SPAN });
                g_freeBlocks.push_back({ g, g_blockEpoch });
                ++g_blocksMade;
            }
            ++spares;
            std::fprintf(stderr, "[thr] spare thread block %08X+%X made at a fight's calibration start\n", g, SPAN);
        }
    }
    size_t all = 0, freeBlocks = 0;
    uint32_t epoch = 0;
    uint64_t taken = 0, given = 0, madeTracked = 0, keptOut = 0;
    {
        std::lock_guard l(g_blocksMutex);
        epoch = ++g_blockEpoch;
        g_blockTracking = ThreadRecycleOn();
        g_fightOn = true;
        g_madeThisFight = 0;
        unmask.swap(g_maskedBlocks);
        out = g_blocks;
        all = g_blocks.size();
        freeBlocks = g_freeBlocks.size();
        taken = g_blocksTaken; given = g_blocksGivenBack; madeTracked = g_blocksMadeTracked; keptOut = g_blocksKeptOut;
    }
    for (uint32_t g : unmask) RollbackNoteHeapFree(g);
    if (!ThreadRecycleOn()) return;
    uint32_t used = 0;
    size_t freeSlots = 0;
    uint64_t reused = 0, stale = 0;
    {
        std::lock_guard l(g_slotMutex);
        freeSlots = g_freeSlots.size();
        used = (std::min)(g_guestContextRegistry.count.load(), GuestContextCapacity) - uint32_t(freeSlots);
        reused = g_slotsReused; stale = g_slotsStale;
    }
    std::fprintf(stderr, "[thr] fight calibration %u: %zu thread blocks (%zu in use or kept out, %zu free; %u spares made now, %zu masks dropped); so far "
                 "%llu taken from the pool, %llu given back, %llu made during tracked fights, %llu kept out; context slots %u used of %u, "
                 "%zu free (%llu reused, %llu stale retired)\n",
                 epoch, all, all - freeBlocks, freeBlocks, spares, unmask.size(), (unsigned long long)taken, (unsigned long long)given,
                 (unsigned long long)madeTracked, (unsigned long long)keptOut, used, GuestContextCapacity, freeSlots,
                 (unsigned long long)reused, (unsigned long long)stale);
}
// rollback.cpp: the tracked fight ended - no restore before the next calibration, so any free block may be handed out.
void GuestThreadBlocksFightOver(uint32_t& madeThisFight, std::vector<std::pair<uint32_t, uint32_t>>* blocks)
{
    std::lock_guard l(g_blocksMutex);
    if (blocks) *blocks = g_blocks;            // the same moment as madeThisFight (the fight-over line)
    g_blockTracking = false;
    g_fightOn = false;
    madeThisFight = g_madeThisFight;
}

// Launch a guest routine on the current host thread. The guest thread block (PCR/TLS/stack)
// is created once per host thread and reused, so periodic callbacks (vblank) don't leak.
namespace {
thread_local uint32_t t_pcr = 0, t_stackTop = 0;
thread_local bool t_guestFaulted = false;    // GuestInvoke caught a fault on this thread
void EnsureThreadBlock()
{
    if (!t_pcr)
    {
        t_pcr = SetupThreadBlock(g_threadIdCounter++, t_stackTop);
        KernelBindCurrentThreadCpu(g_base + t_pcr + 0x10C);
    }
}
// The ExCreateThread lambda, after the guest start routine returned (XAPI's ExitThread sub_82BF76C0 returns into it -
// ExTerminateThread is a no-op): no guest code runs on this host thread again. Its block, context slot and record go
// back before the thread object signals (kernel_objects.cpp's trampoline), so a thread its waiter starts next can have
// them. A block given back while a fight is tracked waits for the next calibration: a restore can bring back state from
// when its old owner was alive (pointers into that stack), and the block must not be another thread's live stack then.
void GuestThreadEnded()
{
    if (!ThreadRecycleOn()) return;
    const DWORD tid = GetCurrentThreadId();
    const uint32_t entry = KernelGuestThreadEntry(tid);
    KernelUnbindCurrentThreadCpu();          // KernelSetThreadAffinity writes PCR+0x10C through the thread object
    const uint32_t slot = ReleaseContextSlot();
    {
        std::lock_guard lk(g_gtMutex);
        DropThreadRecordLocked(tid);
        ++g_guestThreadsEnded;
    }
    const uint32_t g = t_pcr;
    if (!g) return;
    t_pcr = t_stackTop = 0;
    const uint64_t restores = RollbackRestoreCount() - t_restores0;
    // Havok memory routers on that stack are forgotten; true: one is an island in this fight (its 40 bytes put back with
    // every restore until the next calibration).
    const bool island = RollbackThreadBlockReleased(g, SPAN);
    const char* keptOut = t_guestFaulted ? "its guest code faulted (guest objects may still point into that stack)"
                        : t_blockHeapTracked ? "it was made from the heap during a tracked fight (its heap headers were not safe from rewinds)"
                        : nullptr;
    if (keptOut)
    {
        uint64_t n = 0;
        {
            std::lock_guard l(g_blocksMutex);
            n = ++g_blocksKeptOut;
        }
        std::fprintf(stderr, "[thr] thread %lu (entry %08X) ended: thread block %08X kept out of the pool - %s (%llu restores while it ran; %llu kept out so far)\n",
                     tid, entry, g, keptOut, (unsigned long long)restores, (unsigned long long)n);
        return;
    }
    const size_t events = KernelForgetGuestObjects(g, SPAN);   // again when it is handed out (SetupThreadBlock)
    uint64_t n = 0;
    size_t freeNow = 0;
    bool quarantined = false;
    {
        std::lock_guard l(g_blocksMutex);
        quarantined = island || g_blockTracking;
        g_freeBlocks.push_back({ g, quarantined ? g_blockEpoch : t_blockEpoch });
        n = ++g_blocksGivenBack;
        freeNow = g_freeBlocks.size();
    }
    static std::atomic<int> restoreLines{ 0 };
    if (LogBlockEvent(n) || island || (restores && restoreLines.fetch_add(1) < 200))
        std::fprintf(stderr, "[thr] thread %lu (entry %08X) ended: thread block %08X and context slot %d given back%s (%llu restores while it ran; "
                             "%zu host events forgotten; %zu free; %llu given back so far)\n",
                     tid, entry, g, slot == ~0u ? -1 : int(slot),
                     island ? " - a Havok router island in this fight: not handed out before the next calibration"
                     : quarantined ? " - during a tracked fight: not handed out before the next calibration" : "",
                     (unsigned long long)restores, events, freeNow, (unsigned long long)n);
}
} // namespace

// Set the current host thread's guest PCR "current processor" byte (KPCR+0x10C). The D3D
// interrupt DPC services only the CURRENT cpu's fence wait block, so the vblank dispatcher
// rotates this across all 6 cores per tick.
void GuestSetCurrentCpu(uint8_t cpu)
{
    EnsureThreadBlock();
    g_base[t_pcr + 0x10C] = cpu;
}

static void GuestInvoke(PPCFunc* fn, PPCContext& ctx);
void RollbackNoteForeignThread();   // rollback.cpp: callbacks run on their own timeline

// Run a guest routine on the current host thread (a guest thread's start routine, or a callback).
static void GuestRun2(uint32_t guestAddr, uint32_t arg0, uint32_t arg1)
{
    EnsureThreadBlock();
    PPCContext ctx{};
    ctx.r1.u64 = t_stackTop;
    ctx.r13.u64 = t_pcr;
    ctx.r3.u64 = arg0;
    ctx.r4.u64 = arg1;
    ctx.ctr.u64 = guestAddr; // Match an indirect entry and identify missing callbacks in reports.
    ctx.fpscr.loadFromHost();
    PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, guestAddr);
    ScopedGuestContext published(&ctx, guestAddr);
    if (fn) GuestInvoke(fn, ctx);
}

// A callback from one of the runtime's own threads (DPC, interrupt, audio): not the frame's timeline.
void GuestCall2(uint32_t guestAddr, uint32_t arg0, uint32_t arg1)
{
    RollbackNoteForeignThread();
    GuestRun2(guestAddr, arg0, arg1);
}

// SEH-guarded invoke: a fault in a guest callback must be REPORTED, not silently kill
// the process (unhandled exceptions in secondary threads terminate with no output).
static void GuestInvoke(PPCFunc* fn, PPCContext& ctx)
{
    __try { fn(ctx, g_base); }
    __except (GuestFaultReport(GetExceptionInformation()))
    {
        t_guestFaulted = true;                     // GuestThreadEnded keeps this thread's block out of the pool
        std::fprintf(stderr, "[runtime] guest callback faulted (thread continues)\n");
    }
}

void GuestCall4(uint32_t guestAddr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    RollbackNoteForeignThread();
    EnsureThreadBlock();
    PPCContext ctx{};
    ctx.r1.u64 = t_stackTop;
    ctx.r13.u64 = t_pcr;
    ctx.r3.u64 = a0;
    ctx.r4.u64 = a1;
    ctx.r5.u64 = a2;
    ctx.r6.u64 = a3;
    ctx.ctr.u64 = guestAddr;
    ctx.fpscr.loadFromHost();
    PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, guestAddr);
    ScopedGuestContext published(&ctx, guestAddr);
    if (fn) GuestInvoke(fn, ctx);
}

void GpuProgress();     // kernel_video.cpp: wakes waiters on GPU-side writes

// --- DPC dispatch: KeInsertQueueDpc queues {routine, dpc, context, args} to a dedicated
// dispatcher thread, mimicking deferred execution at "interrupt time". ---
namespace {
struct DpcItem { uint32_t routine, dpc, context, arg1, arg2; };
std::mutex g_dpcMutex;
std::condition_variable g_dpcCv;
std::deque<DpcItem> g_dpcQueue;
std::atomic<bool> g_dpcThreadStarted{false};

void DpcThread()
{
    DiagNameThread(L"dpc");
    for (;;)
    {
        DpcItem item;
        {
            std::unique_lock lk(g_dpcMutex);
            g_dpcCv.wait(lk, [] { return !g_dpcQueue.empty(); });
            item = g_dpcQueue.front();
            g_dpcQueue.pop_front();
        }
        GuestCall4(item.routine, item.dpc, item.context, item.arg1, item.arg2);
        GpuProgress();          // the swap DPC writes the scratch tokens the GPU waits on
    }
}
} // namespace

// Queue an arbitrary guest call to the DPC dispatcher thread (used for I/O APCs: they
// must not run inline on the issuing thread, whose guest stack is in use mid-syscall).
void KernelQueueGuestCall(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    if (!fn) return;
    if (!g_dpcThreadStarted.exchange(true))
    {
        uintptr_t h = _beginthreadex(nullptr, 8 * 1024 * 1024,
            [](void*) -> unsigned { DpcThread(); return 0; }, nullptr, 0, nullptr);
        if (h) CloseHandle(reinterpret_cast<HANDLE>(h));
    }
    {
        std::lock_guard lk(g_dpcMutex);
        g_dpcQueue.push_back({fn, a0, a1, a2, a3});
    }
    g_dpcCv.notify_one();
}

// KeInitializeDpc(r3=KDPC, r4=DeferredRoutine, r5=DeferredContext)
PPC_FUNC(__imp__KeInitializeDpc)
{
    KTrace("KeInitializeDpc");
    GWrite32(base, ctx.r3.u32 + 16, ctx.r4.u32);   // routine
    GWrite32(base, ctx.r3.u32 + 20, ctx.r5.u32);   // context
}

// KeInsertQueueDpc(r3=KDPC, r4=SystemArgument1, r5=SystemArgument2)
PPC_FUNC(__imp__KeInsertQueueDpc)
{
    KTrace("KeInsertQueueDpc");
    uint32_t routine = GRead32(base, ctx.r3.u32 + 16);
    uint32_t context = GRead32(base, ctx.r3.u32 + 20);
    if (routine)
    {
        if (!g_dpcThreadStarted.exchange(true))
        {
            // 8 MB stack: DPC routines run deep guest chains.
            uintptr_t h = _beginthreadex(nullptr, 8 * 1024 * 1024,
                [](void*) -> unsigned { DpcThread(); return 0; }, nullptr, 0, nullptr);
            if (h) CloseHandle(reinterpret_cast<HANDLE>(h));
        }
        {
            std::lock_guard lk(g_dpcMutex);
            g_dpcQueue.push_back({routine, ctx.r3.u32, context, ctx.r4.u32, ctx.r5.u32});
        }
        g_dpcCv.notify_one();
    }
    ctx.r3.u32 = 1;                                // was not already queued
}

PPC_FUNC(__imp__KeRemoveQueueDpc)
{
    KTrace("KeRemoveQueueDpc"); ctx.r3.u32 = 0; }

void GuestCall(uint32_t guestAddr, uint32_t arg)
{
    GuestCall2(guestAddr, arg, 0);
}

// ExCreateThread(r3=&handle, r4=stackSize, r5=&threadId, r6=xApiStartup, r7=startAddress,
//                r8=startContext, r9=creationFlags)
PPC_FUNC(__imp__ExCreateThread)
{
    KTrace("ExCreateThread");
    uint32_t handlePtr = ctx.r3.u32;
    uint32_t threadIdPtr = ctx.r5.u32;
    uint32_t startAddress = ctx.r7.u32;
    uint32_t startContext = ctx.r8.u32;
    uint32_t creationFlags = ctx.r9.u32;

    uint32_t threadId = g_threadIdCounter.load();
    auto* obj = KernelCreateThreadObject();          // waitable thread object (handle table)

    // creationFlags bit0 = created suspended. For bring-up we start immediately if not suspended.
    bool suspended = (creationFlags & 1) != 0;
    // The save manager's save / load job (rollback.cpp's SaveJobRunning): its timeline while a session runs (log only).
    const bool saveJob = startAddress == 0x8258B2C0u || startAddress == 0x8258B2D8u;
    if (saveJob) RollbackSaveJobMark(startAddress == 0x8258B2C0u ? "save job requested" : "load job requested", true, -1);
    KernelStartHostThread(obj, [startAddress, startContext, saveJob]() {
        if (ThreadRecycleOn()) RollbackGuestThreadStarted();   // a dead foreign thread's id is not this thread
        KernelRegisterGuestThread(startAddress, startContext);
        t_saveJobThread = saveJob;
        if (saveJob) RollbackSaveJobMark("job thread running", false, -1);
        GuestRun2(startAddress, startContext, 0);
        GuestThreadEnded();                                    // block, context slot and record given back
        t_saveJobThread = false;
        if (saveJob) { XamSaveJobBoxClear(); RollbackSaveJobMark("job thread ended", false, -1); }
    }, suspended);

    GWrite32(base, handlePtr, KernelObjectHandle(obj));
    if (threadIdPtr) GWrite32(base, threadIdPtr, threadId);
    ctx.r3.u32 = STATUS_SUCCESS;
}

PPC_FUNC(__imp__ExTerminateThread)        { KTrace("ExTerminateThread"); ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__NtResumeThread)
{
    KTrace("NtResumeThread");
    uint32_t previous = 0;
    const auto status = KernelResumeThread(ctx.r3.u32, &previous);
    if (status == STATUS_SUCCESS && ctx.r4.u32) GWrite32(base, ctx.r4.u32, previous);
    ctx.r3.u32 = status;
}
PPC_FUNC(__imp__NtSuspendThread)
{
    KTrace("NtSuspendThread");
    if (WTrace()) std::fprintf(stderr, "[w] NtSuspendThread h=%08X lr=%08X\n", ctx.r3.u32, (uint32_t)ctx.lr);
    uint32_t previous = 0;
    const auto status = KernelSuspendThread(ctx.r3.u32, &previous);
    if (status == STATUS_SUCCESS && ctx.r4.u32) GWrite32(base, ctx.r4.u32, previous);
    ctx.r3.u32 = status;
}
PPC_FUNC(__imp__KeSetBasePriorityThread)
{
    KTrace("KeSetBasePriorityThread");
    if (ctx.r13.u32) KernelBindCurrentThreadCpu(base + ctx.r13.u32 + 0x10C);
    ctx.r3.u32 = static_cast<uint32_t>(KernelSetThreadPriority(ctx.r3.u32, ctx.r4.s32));
}
PPC_FUNC(__imp__KeSetAffinityThread)
{
    KTrace("KeSetAffinityThread");
    if (ctx.r13.u32) KernelBindCurrentThreadCpu(base + ctx.r13.u32 + 0x10C);
    uint32_t previous = 0;
    const uint32_t status = KernelSetThreadAffinity(ctx.r3.u32, ctx.r4.u32, previous);
    // Unlike Windows KeSetAffinityThread, Xbox returns status in r3 and the old
    // mask through r5. The guest XSetThreadProcessor wrapper reads this output.
    if (status == STATUS_SUCCESS && ctx.r5.u32) GWrite32(base, ctx.r5.u32, previous);
    ctx.r3.u32 = status;
}
PPC_FUNC(__imp__KeQueryBasePriorityThread)
{
    KTrace("KeQueryBasePriorityThread");
    ctx.r3.u32 = static_cast<uint32_t>(KernelQueryThreadPriority(ctx.r3.u32));
}
PPC_FUNC(__imp__NtYieldExecution)
{
    const uint64_t t0 = DiagMainOffBegin();
    std::this_thread::yield();
    DiagMainOffEnd(3, t0);
}
// A relative guest delay of `ticks` 100 ns units. TTT2's job threads poll with 1 ms sleeps
// (sub_824F5458: ten polls, then Sleep(1) per poll while idle) and the main thread waits on
// those jobs several times a frame (sub_824F58B8), so the wake-up latency lands in the frame
// time. std::this_thread::sleep_for(1 ms) lasts 1-2 ms on Windows even at timeBeginPeriod(1);
// a high-resolution waitable timer (Windows 10 1803+) keeps the requested length, including
// sub-millisecond ones. TTT2_SLEEP_HIRES=0 restores sleep_for.
static void GuestDelay(int64_t ticks)
{
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    static const bool hires = [] { const char* e = std::getenv("TTT2_SLEEP_HIRES"); return !e || std::strcmp(e, "0") != 0; }();
    // TTT2_SLEEP_SCALE=<percent> shortens delays of 2 ms or less (poll-loop sleeps; experiment).
    static const int64_t scale = [] { const char* e = std::getenv("TTT2_SLEEP_SCALE"); return e ? (std::max<int64_t>)(1, std::atoll(e)) : 100; }();
    if (scale != 100 && ticks <= 20000) ticks = (std::max<int64_t>)(1, ticks * scale / 100);
    if (hires)
    {
        static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                                   TIMER_ALL_ACCESS);
        LARGE_INTEGER due;
        due.QuadPart = -ticks;
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    std::this_thread::sleep_for(std::chrono::microseconds((ticks + 9) / 10));
}

// (WaitMode, Alertable, Interval*) - Interval is 100ns units, negative = relative delay.
PPC_FUNC(__imp__KeDelayExecutionThread)
{
    KTrace("KeDelayExecutionThread");
    if (WTrace()) std::fprintf(stderr, "[w] KeDelay lr=%08X\n", (uint32_t)ctx.lr);
    int64_t ticks = 10000;                        // 1 ms
    if (uint32_t p = ctx.r5.u32)
    {
        int64_t t = (int64_t)(((uint64_t)GRead32(g_base, p) << 32) | GRead32(g_base, p + 4));
        ticks = t < 0 ? -t : 0;
        if (ticks > 500000) ticks = 500000;       // 50 ms: keep watchdog-style long sleeps responsive
    }
    const uint64_t off0 = DiagMainOffBegin();
    if (ticks <= 0) std::this_thread::yield();
    else GuestDelay(ticks);
    DiagMainOffEnd(ticks <= 0 ? 3 : 1, off0);
    if (t_saveJobThread) XamSaveJobAwake();       // a box answered while it slept: the job works again from here
    {
        // TTT2_SYNC_STATS=1 (diagnostics): guest sleeps by thread entry, every 10 s.
        static const bool stats = [] { const char* e = std::getenv("TTT2_SYNC_STATS"); return e && *e == '1'; }();
        struct SleepEntry { std::atomic<uint32_t> entry{0}, lr{0}; std::atomic<uint64_t> n{0}, ticks{0}; };
        static SleepEntry table[64];
        static std::atomic<bool> reporter{false};
        if (stats)
        {
            const uint32_t entry = CurrentGuestEntry(), lr = uint32_t(ctx.lr);
            for (SleepEntry& e : table)
            {
                uint32_t cur = e.entry.load(std::memory_order_relaxed);
                if (cur == 0 && e.entry.compare_exchange_strong(cur, entry)) { e.lr = lr; cur = entry; }
                if (cur != entry || e.lr.load(std::memory_order_relaxed) != lr) continue;
                ++e.n; e.ticks += uint64_t(ticks);
                break;
            }
            if (!reporter.exchange(true))
                std::thread([] {
                    for (;;)
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(10));
                        char line[1500]; int n = std::snprintf(line, sizeof(line), "[sync] guest sleeps in 10 s by entry/lr:");
                        for (SleepEntry& e : table)
                            if (const uint64_t c = e.n.exchange(0); c && n < int(sizeof(line)) - 60)
                                n += std::snprintf(line + n, sizeof(line) - n, " %08X/%08X %llu (%.0f us avg);", e.entry.load(), e.lr.load(),
                                                   (unsigned long long)c, double(e.ticks.exchange(0)) / 10.0 / double(c));
                        std::snprintf(line + n, sizeof(line) - n, "\n");
                        std::fputs(line, stderr);
                    }
                }).detach();
        }
    }
    // A thread sleeping over and over from the same call site is a poll loop -
    // print it with the guest backchain to identify the poller, at 2000 sleeps in a streak, then 20000, 200000...
    // (it printed every 2000: 25,881 identical lines in one evening's log, 2026-10-24).
    {
        static thread_local uint32_t sleepLr = 0, sleepCnt = 0, sleepNext = 2000;
        uint32_t lr = (uint32_t)ctx.lr;
        if (lr == sleepLr)
        {
            if (++sleepCnt == sleepNext)
            {
                char line[512];
                int n = std::snprintf(line, sizeof(line), "[pollsleep] lr=%08X x%u bt:", lr, sleepCnt);
                uint32_t frame = ctx.r1.u32;
                for (int i = 0; i < 12 && frame && frame < 0xFFFFFFF0u; ++i)
                {
                    uint32_t prev = __builtin_bswap32(*reinterpret_cast<uint32_t*>(g_base + frame));
                    if (!prev || prev <= frame || prev - frame > 0x10000) break;
                    uint32_t savedLr = __builtin_bswap32(*reinterpret_cast<uint32_t*>(g_base + prev - 8));
                    n += std::snprintf(line + n, sizeof(line) - n, " %08X", savedLr);
                    frame = prev;
                }
                std::snprintf(line + n, sizeof(line) - n, "\n");
                std::fputs(line, stderr);
                sleepNext = sleepNext < 200000000u ? sleepNext * 10 : 0;
            }
        }
        else { sleepLr = lr; sleepCnt = 1; sleepNext = 2000; }
    }
    ctx.r3.u32 = 0;
}

// TEST ONLY: a name for this host thread in per-thread CPU samples.
void DiagNameThread(const wchar_t* name) { SetThreadDescription(GetCurrentThread(), name); }

// DIAGNOSTIC BUILD (p72): where the low-end player's processor time goes. Player logs 24/25 (i3-3240, 2 cores / 4
// threads; Tulip Festival): the process used ~3.0 cores at 53 fps, the game main thread and the command processor only
// ~1.1 of them; and the main thread's frame (19 ms) was ~4 ms longer than its processor time plus its wait for the
// command processor. So: every thread's processor time a frame, grouped by name - the names threads give themselves
// (ours; "guest <entry>" for the game's own), else the module their start address is in (the graphics driver's, the
// audio stack's; "exe+<offset>" for our unnamed ones) - and the main thread's time off the processor by kind (object
// waits, sleeps, critical sections, yields; timestamp-counter ticks). Printed with the frame cost every report.
extern HANDLE g_mainThreadHandle;                   // main.cpp
static std::atomic<uint64_t> g_diagMainOff[4];      // objects, sleeps, critical sections, yields
static bool DiagIsMainThread()
{
    thread_local int is = -1;
    if (is < 0)
    {
        if (!g_mainThreadHandle) return false;
        is = GetThreadId(g_mainThreadHandle) == GetCurrentThreadId() ? 1 : 0;
    }
    return is == 1;
}
uint64_t DiagMainOffBegin() { return DiagIsMainThread() ? __rdtsc() : 0; }
void DiagMainOffEnd(int kind, uint64_t t0)
{
    if (t0) g_diagMainOff[kind].fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
}
// TEST 13 ("[mainwait]", kernel_objects.cpp / kernel_misc.cpp): how long a wait of the game main thread lasted, in ms,
// from its DiagMainOffBegin tick (0 on other threads, and in the first half second, while the tick rate is measured).
double DiagMainWaitMs(uint64_t t0)
{
    if (!t0) return 0;
    const uint64_t t1 = __rdtsc();
    static uint64_t calTsc = 0;                       // the main thread only: no race
    static int64_t calQpc = 0;
    static double tscPerMs = 0;
    if (tscPerMs <= 0)
    {
        LARGE_INTEGER q{}, f{};
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&f);
        if (!calTsc) { calTsc = t1; calQpc = q.QuadPart; }
        else if (q.QuadPart - calQpc > f.QuadPart / 2)
            tscPerMs = double(t1 - calTsc) * double(f.QuadPart) / (double(q.QuadPart - calQpc) * 1000.0);
        return 0;
    }
    return double(t1 - t0) / tscPerMs;
}
// [slowframe] (gpu_hw_d3d11.h): the running totals, in timestamp-counter ticks.
void DiagMainOffTotals(uint64_t out[4])
{
    for (int k = 0; k < 4; ++k) out[k] = g_diagMainOff[k].load(std::memory_order_relaxed);
}
static std::string DiagThreadLabel(HANDLE h, DWORD id)
{
    if (g_mainThreadHandle && GetThreadId(g_mainThreadHandle) == id) return "game main";
    std::string s;
    PWSTR d = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &d)) && d)
    {
        for (const wchar_t* p = d; *p; ++p) s += char(*p < 128 ? *p : '?');
        LocalFree(d);
        if (!s.empty()) return s;
    }
    using QueryFn = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const QueryFn query = reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    PVOID start = nullptr;
    if (!query || query(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr) != 0 || !start) return "unknown";
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(start), &mod) || !GetModuleFileNameW(mod, path, MAX_PATH))
        return "unknown";
    if (mod == GetModuleHandleW(nullptr))
    {
        char rva[32];
        std::snprintf(rva, sizeof(rva), "exe+%llX", (unsigned long long)(static_cast<uint8_t*>(start) - reinterpret_cast<uint8_t*>(mod)));
        return rva;
    }
    const wchar_t* name = std::wcsrchr(path, L'\\');
    return LogUtf8(name ? name + 1 : path);   // UTF-8 (log_utf8.h): -A gave '?' for a name outside the ANSI code page
}
// diag16: the report on a thread of its own - its thread snapshot held the command processor for a few ms every 5 s
// (log 32's periodic spike). A report still running when the next is due skips that one.
// TEST 18: what else the PC did in the same 5 s, on the report's own thread. Logs 54 / 55 (TEST 15 / 17, same PC, same
// evening, same stages): the game main thread's "the rest" 2.4 -> 6.3 ms a frame with its own work unchanged, and the log
// could not say whether other programs, interrupts or page faults had that time. x64 layouts of SYSTEM_PROCESS_INFORMATION
// (0x100 bytes, then 0x50 per thread) and SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION; only program file names are logged.
static void DiagSystemReport()
{
    using QueryFn = LONG(WINAPI*)(int, PVOID, ULONG, PULONG);
    static const QueryFn query = reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query) return;
    struct CpuTimes { LARGE_INTEGER idle, kernel, user, dpc, interrupt; ULONG count; };
    static CpuTimes cpus[256];
    ULONG got = 0;
    if (query(8 /* SystemProcessorPerformanceInformation */, cpus, sizeof(cpus), &got) != 0) return;
    uint64_t idle = 0, total = 0, dpc = 0, irq = 0;
    for (ULONG i = 0; i < got / sizeof(CpuTimes); ++i)
    {
        idle += uint64_t(cpus[i].idle.QuadPart);
        total += uint64_t(cpus[i].kernel.QuadPart) + uint64_t(cpus[i].user.QuadPart);     // kernel includes idle
        dpc += uint64_t(cpus[i].dpc.QuadPart);
        irq += uint64_t(cpus[i].interrupt.QuadPart);
    }
    static std::vector<uint8_t> buffer(1u << 20);
    for (;;)
    {
        ULONG need = 0;
        const LONG st = query(5 /* SystemProcessInformation */, buffer.data(), ULONG(buffer.size()), &need);
        if (st == 0) break;
        if (st != LONG(0xC0000004) /* length mismatch */ || buffer.size() >= (16u << 20)) return;
        buffer.resize((std::min<size_t>)(16u << 20, buffer.size() * 2));
    }
    const auto u32At = [](const uint8_t* p, size_t at) { uint32_t v; std::memcpy(&v, p + at, 4); return v; };
    const auto u64At = [](const uint8_t* p, size_t at) { uint64_t v; std::memcpy(&v, p + at, 8); return v; };
    static std::unordered_map<uint64_t, uint64_t> lastCpu;
    static uint64_t lastIdle = 0, lastTotal = 0, lastDpc = 0, lastIrq = 0, lastFaults = 0, lastHard = 0, lastSwitches = 0, lastMs = 0;
    std::unordered_map<uint64_t, uint64_t> nowCpu;
    const uint64_t self = GetCurrentProcessId(), mainTid = g_mainThreadHandle ? GetThreadId(g_mainThreadHandle) : 0;
    uint64_t own = 0, others = 0, faults = 0, hard = 0, switches = 0;
    struct Top { uint64_t cpu; std::string name; } top[2] = {};
    for (const uint8_t* p = buffer.data();;)
    {
        const uint64_t pid = u64At(p, 0x50), cpu = u64At(p, 0x28) + u64At(p, 0x30);
        nowCpu[pid] = cpu;
        const auto old = lastCpu.find(pid);
        const uint64_t delta = old == lastCpu.end() ? 0 : cpu - old->second;
        if (pid == self)
        {
            own = delta;
            hard = u32At(p, 0x10);
            faults = u32At(p, 0x80);
            const uint32_t threads = u32At(p, 4);
            for (uint32_t i = 0; i < threads; ++i)
                if (u64At(p, 0x100 + size_t(i) * 0x50 + 0x30) == mainTid) switches = u32At(p, 0x100 + size_t(i) * 0x50 + 0x40);
        }
        else if (pid)
        {
            others += delta;
            const int slot = delta > top[0].cpu ? 0 : delta > top[1].cpu ? 1 : -1;
            if (slot >= 0)
            {
                if (slot == 0) top[1] = top[0];
                uint16_t bytes;
                std::memcpy(&bytes, p + 0x38, 2);
                const wchar_t* name = reinterpret_cast<const wchar_t*>(u64At(p, 0x40));
                top[slot] = { delta, name && bytes ? LogUtf8(std::wstring(name, bytes / 2).c_str()) : std::string("system") };
            }
        }
        const uint32_t next = u32At(p, 0);
        if (!next) break;
        p += next;
    }
    const uint64_t ms = GetTickCount64();
    if (lastMs && total > lastTotal)
    {
        const double all = double(total - lastTotal), seconds = double(ms - lastMs) / 1000.0;
        const auto pct = [&](uint64_t v) { return 100.0 * double(v) / all; };
        char named[200] = "";
        if (top[0].cpu && pct(top[0].cpu) >= 0.5)
        {
            int k = std::snprintf(named, sizeof(named), ": %s %.1f%%", top[0].name.c_str(), pct(top[0].cpu));
            if (top[1].cpu && pct(top[1].cpu) >= 0.5 && k > 0 && k < int(sizeof(named)))
                std::snprintf(named + k, sizeof(named) - k, ", %s %.1f%%", top[1].name.c_str(), pct(top[1].cpu));
        }
        std::fprintf(stderr, "[system] %.1f s: processors %.1f%% busy (this game %.1f%%, other programs %.1f%%%s), interrupts %.1f%%, "
                             "DPCs %.1f%%; this game's page faults %.0f/s, from disk %.0f/s; game main thread started running %.0f times/s\n",
                     seconds, 100.0 - pct(idle - lastIdle), pct(own), pct(others), named, pct(irq - lastIrq), pct(dpc - lastDpc),
                     double(faults - lastFaults) / seconds, double(hard - lastHard) / seconds, double(switches - lastSwitches) / seconds);
    }
    lastCpu.swap(nowCpu);
    lastIdle = idle; lastTotal = total; lastDpc = dpc; lastIrq = irq; lastFaults = faults; lastHard = hard; lastSwitches = switches; lastMs = ms;
}
void DiagThreadsReport(double tscPerMs, uint64_t presents, double frameMs);
void DiagThreadsReportAsync(double tscPerMs, uint64_t presents, double frameMs)
{
    static std::atomic<bool> busy{ false };
    if (busy.exchange(true)) return;
    std::thread([=] { DiagThreadsReport(tscPerMs, presents, frameMs); DiagSystemReport(); busy.store(false); }).detach();
}
void DiagThreadsReport(double tscPerMs, uint64_t presents, double frameMs)
{
    struct Rec { HANDLE h; uint64_t cycles; std::string label; bool seen; };
    static std::unordered_map<DWORD, Rec> threads;
    static bool primed = false;
    static uint64_t offLast[4] = {};
    if (!presents || tscPerMs <= 0) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    for (auto& [id, r] : threads) r.seen = false;
    std::map<std::string, std::pair<double, int>> groups;
    double all = 0, mainMs = 0;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid) continue;
        auto it = threads.find(te.th32ThreadID);
        bool fresh = false;
        if (it == threads.end())
        {
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            it = threads.emplace(te.th32ThreadID, Rec{ h, 0, std::string(), false }).first;
            fresh = true;
        }
        Rec& r = it->second;
        r.seen = true;
        ULONG64 c = 0;
        if (!QueryThreadCycleTime(r.h, &c)) continue;
        r.label = DiagThreadLabel(r.h, te.th32ThreadID);   // each time: threads name themselves once they run
        const uint64_t delta = fresh && !primed ? 0 : c - r.cycles;   // started since the last report: all of it
        r.cycles = c;
        const double ms = double(delta) / tscPerMs / double(presents);
        auto& g = groups[r.label];
        g.first += ms;
        ++g.second;
        all += ms;
        if (r.label == "game main") mainMs = ms;
    }
    CloseHandle(snap);
    for (auto it = threads.begin(); it != threads.end();)
        if (!it->second.seen) { CloseHandle(it->second.h); it = threads.erase(it); }
        else ++it;
    double off[4];
    for (int k = 0; k < 4; ++k)
    {
        const uint64_t v = g_diagMainOff[k].load(std::memory_order_relaxed);
        off[k] = double(v - offLast[k]) / tscPerMs / double(presents);
        offLast[k] = v;
    }
    if (!primed) { primed = true; return; }
    std::vector<std::pair<std::string, std::pair<double, int>>> sorted(groups.begin(), groups.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    std::string line;
    double rest = 0;
    int restThreads = 0;
    for (size_t i = 0; i < sorted.size(); ++i)
    {
        const auto& [label, v] = sorted[i];
        if (i >= 20 || v.first < 0.05) { rest += v.first; restThreads += v.second; continue; }
        char item[160];
        if (v.second > 1) std::snprintf(item, sizeof(item), "%s%s %.2f (x%d)", line.empty() ? "" : ", ", label.c_str(), v.first, v.second);
        else std::snprintf(item, sizeof(item), "%s%s %.2f", line.empty() ? "" : ", ", label.c_str(), v.first);
        line += item;
    }
    std::fprintf(stderr, "[diag]   threads, processor ms a frame: %s; %d more threads %.2f; all %.2f (%.2f cores)\n",
                 line.c_str(), restThreads, rest, all, frameMs > 0 ? all / frameMs : 0.0);
    std::fprintf(stderr, "[diag]   game main thread's frame (ms): %.2f = on the processor %.2f + waiting for objects %.2f + sleeping %.2f + "
                         "critical sections %.2f + yielding %.2f + the rest %.2f (ready to run but not running, or untimed)\n",
                 frameMs, mainMs, off[0], off[1], off[2], off[3], frameMs - mainMs - off[0] - off[1] - off[2] - off[3]);
}
