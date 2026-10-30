// Rollback netcode, phase 2 (prototype): rewinding the fight and re-simulating frames, checked by a
// sync test (GGPO's idea: rewind, replay the same inputs, demand the same state).
//
// The frame (MEASURED 2026-10-18, profile of a Practice fight): the main loop sub_82583288 runs the
// scene UPDATE sub_825EF0B8 (scene manager -> battle state -> battle tick sub_822D36C8, ~0.7 ms),
// then the DRAW sub_825EBCC8 (scene draw, GPU wait, VdSwap). The fight's inputs reach the battle
// through sub_822745B0(pad slot): offline the local pad, online NACE's per-frame record.
//
// State = one consistent moment: at the start of every fight update, the pages written during the
// whole previous frame by anyone (update, draw, worker threads) are snapshotted - the "hot set",
// learned with the write watch over the guest heap and the image's data; thread blocks (stacks)
// and the D3D device (its ring / fence state belongs to the GPU side) stay out. A rewind puts a
// snapshot back whole. MEASURED dead ends (each hung the game): restoring only the pages the
// update writes (the draw's heap allocations / free lists share those pages - the allocator looped
// forever), undoing only the bytes the update changed (job counters the workers change too - the
// draw waited forever), the image's data only (linked lists half old, half new - a list walk
// looped forever). Partial rewinds mix two timelines.
//
// Random numbers: the game has one LCG (state word at [0x82E6B05C] + 16, advanced by sub_823183A0)
// shared by the fight and by cosmetic work in the draw. A re-simulation runs no draws, so the
// fight's sequence shifted (MEASURED: a hit's random roll differed). Rollback gives the updates
// their own state: swapped in for every update, out for everything else (both players do the same,
// so the fight's sequence no longer depends on how many draws ran).
//
// TTT2_RB_SYNCTEST=<n> (1..12): every TTT2_RB_EVERY (30) fight frames the fight is rewound n frames
// and the n updates run again with the recorded inputs; each must leave the state the original one
// left (fighter words and pages that differ are logged). The fight then goes on from the
// re-simulated state, as it will in a real rollback. TTT2_RB_FROM (600): fight updates before the
// hot set is learned (the round intro still streams assets). "[rb]" log lines.
#include "kernel_shared.h"
#include "guest_heap.h"
#include "save_store.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_825EF0B8);
extern "C" PPC_FUNC(__imp__sub_822745B0);
extern "C" PPC_FUNC(__imp__sub_82664CF8);
extern "C" PPC_FUNC(__imp__sub_82738E70);
extern "C" PPC_FUNC(__imp__sub_82AD1A48);
extern "C" PPC_FUNC(__imp__sub_82AD1AD0);
extern "C" PPC_FUNC(__imp__sub_82D032D0);
extern "C" PPC_FUNC(__imp__sub_82C3A4E0);
extern "C" PPC_FUNC(__imp__sub_82C3A540);
extern "C" PPC_FUNC(__imp__sub_82C7B7E8);
extern "C" PPC_FUNC(__imp__sub_823D94B8);
extern "C" PPC_FUNC(__imp__sub_823D8DB8);
extern "C" PPC_FUNC(__imp__sub_823D9340);
extern "C" PPC_FUNC(__imp__sub_823D95E8);
extern "C" PPC_FUNC(__imp__sub_823D9280);
extern "C" PPC_FUNC(__imp__sub_82CFF418);
extern "C" PPC_FUNC(__imp__sub_8250F070);
PPC_EXTERN_FUNC(sub_825EBCC8);   // the scene draw (the main loop's second call, same r3 as the update)
void GpuDiscardNextFrames(uint32_t n);   // kernel_video.cpp
void GpuDiscardEndNow();
void SoundRollbackFrame(uint64_t frame, bool resim);   // sound_rollback.cpp
bool SoundLibraryWriting(uint32_t page);               // sound_rollback.cpp: a sound API call writing the engine's memory
void XSessionReportMatch(bool started);                // xsession.cpp: a match starts / ends (the server's history)
void SoundRollbackFinal(PPCContext& ctx, uint8_t* base, uint64_t upTo);   // sound_rollback.cpp: held drops / 'rele's
bool RollbackNetWriting();                             // below: the online manager's per-frame update is running
void NetNoteFaultLocked(uint32_t page);                 // below: its faults while calibrating
void ApplyNetWords();                                   // below: its words, masked at calibration's end
void ClearNetWords();                                   // below: a new calibration
void SoundRollbackSettle(PPCContext& ctx, uint8_t* base, uint64_t from, uint64_t to);   // sound_rollback.cpp
extern std::atomic<uint32_t> g_markerSwaps;
uint32_t GpuCpMarkers();
bool GpuFinishInFlight(int maxMs, bool discardInFlight = false);
void GpuUnpaceInFlight();   // kernel_video.cpp: the frames in flight run without vblank waits
void GpuMemRewindBegin();   // kernel_video.cpp (TTT2_RB_GPUMEM diagnostics)
void GpuMemRewindEnd();
void GpuMemRewindCheck(int stage);
// resim_scratch.cpp: re-simulated frames' skinned vertices in scratch memory (no GPU wait before a rewind).
bool ResimScratchReady(uint8_t* base, std::vector<std::pair<uint32_t, uint32_t>>& neverRewind);
bool ResimScratchOn();
void ResimScratchBegin();
void ResimScratchEnd();
void ResimScratchStats(uint64_t& allocs, uint64_t& bytes, uint64_t& refused);
double GpuCpFrameMs();               // kernel_video.cpp: the command processor's work per frame (average)

void GuestThreadBlocks(std::vector<std::pair<uint32_t, uint32_t>>& out);   // kernel_thread.cpp
void GameHeapOps(uint64_t& thread, uint64_t& all);                          // diag_hooks.cpp
namespace hitch { uint32_t GuestEntryOfThread(DWORD tid); void DumpGuestThreads(); int GuestContexts(DWORD* tids, uint32_t* entries, PPCContext** ctxs, int max); }
// rollback_session.cpp: online rollback sessions over NACE (TTT2_ROLLBACK=1).
bool SessionEnabled();
void SessionStateTrace(int32_t frame);   // rollback_session.cpp (TTT2_RB_NACETRACE): the state a snapshot holds
bool SessionSyncRunning();
bool SessionEnding();
int SessionPending();
void SessionOwnPump(bool own);
int SessionPrePump(PPCContext& ctx, uint8_t* base, int32_t& real, bool& ready);
bool SessionNext(PPCContext& ctx, uint8_t* base);
int32_t SessionRealFrame();
int32_t SessionConfirmedFrame();   // rollback_session.cpp: NACE node 0's confirmed frame (-9 none)
bool SaveInProgress();             // below: a save container is open (RollbackSaveOpen)
bool SessionYield();               // rollback_session.cpp: time sync - this update yields a frame
double SessionTimeSyncGiven();     // rollback_session.cpp: frames the time sync gave up this session
bool CheckLargeBlockHeaps(const char* where);   // below: Havok's large-block free trees are sound
void PrintTaskCensus();   // below: the session's tasks (diagnostics)
void PrintImportCensus();   // below: the online tasks' imports (diagnostics)
uint32_t VdDeviceGuest();                                                   // kernel_video.cpp
void WriteWatchPublish(int self, const PVOID* pages, size_t count);         // write_watch.cpp
bool GpuRingDrained();                                                      // kernel_video.cpp
bool WaitGpuProgress(uint32_t seen, DWORD ms);
extern std::atomic<uint32_t> g_gpuProgress;
void WriteWatchDrain(int self, uint32_t firstPage, uint32_t lastPage, std::vector<uint32_t>& out);
bool CsThreadBlockedBy(unsigned long tid, unsigned long owner, uint32_t* section = nullptr, uint32_t* ownerLr = nullptr);   // kernel_misc.cpp
int CsLend(uint32_t ga, uint32_t& lr);                                      // kernel_misc.cpp: leave a section this thread holds
void CsReclaim(uint32_t ga, int depth, uint32_t lr);                        // and enter it again

namespace {
constexpr uint32_t kPage = 4096;
struct Range { uint32_t lo, hi; };
// The heap and the whole game image (code, data, bss: PPC_IMAGE_BASE + PPC_IMAGE_SIZE). MEASURED 2026-10-21: the
// image ends at 0x831C0000, not 0x83000000 - Havok's allocator objects live above 0x83000000 (830D1580, 830D1B6C); left
// out, their free lists' heads were never rewound while the free elements in the heap were, and a free list closed into
// a cycle (a job looped in Havok's free-list sort for good - the game froze; the 2026-10-19 host hang was the same).
constexpr uint32_t kImageEnd = 0x831C0000u;
const Range kRanges[] = { { VHEAP_BEGIN, VHEAP_END }, { 0x82000000u, kImageEnd } };
constexpr int kCalibrateFrames = 120, kRing = 16;

int Env(const char* name, int fallback, int lo, int hi)
{
    const char* e = std::getenv(name);
    return e ? std::clamp(std::atoi(e), lo, hi) : fallback;
}
int Depth() { static const int v = Env("TTT2_RB_SYNCTEST", 0, 0, 12); return v; }
int Every() { static const int v = Env("TTT2_RB_EVERY", 30, 1, 1 << 30); return v; }
// The fight's first updates run plain; calibration (the next kCalibrateFrames) learns the state.
// Sessions calibrate early, during the round's intro.
int CalibrateFrom() { static const int v = Env("TTT2_RB_FROM", SessionEnabled() ? 30 : 600, 1, 1 << 30); return v; }
// The machinery runs for the sync test or for online rollback sessions.
bool MachineryOn() { return Depth() > 0 || SessionEnabled(); }
// TTT2_RB_SET=update: the hot set is only the pages written while an update runs; pages only the
// draw writes (render lists, the D3D runtime's dynamic-buffer bookkeeping - rewinding that while
// frames were in flight drew the fighters with garbage vertices) are never rewound.
// TTT2_RB_SET=tick: only the pages written inside the battle tick (sub_822D36C8) - the fight's
// own state, without the scene's render preparation and job-system traffic.
bool UpdateSetOnly()
{
    static const bool v = [] { const char* e = std::getenv("TTT2_RB_SET"); return e && (std::strcmp(e, "update") == 0 || std::strcmp(e, "tick") == 0); }();
    return v;
}
bool TickSetOnly()
{
    static const bool v = [] { const char* e = std::getenv("TTT2_RB_SET"); return e && std::strcmp(e, "tick") == 0; }();
    return v;
}
double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// ---- pages ----------------------------------------------------------------------------------
std::vector<PVOID> g_ww(1u << 18);
std::vector<uint8_t> g_isHot(1u << 20), g_excluded(1u << 20);   // per guest page
std::vector<uint32_t> g_hot;                                    // append-only list of hot pages

// Pages of the tracked ranges written since the last call (the bits are reset).
void ReadWritten(std::vector<uint32_t>& out)
{
    for (const Range& r : kRanges)
    {
        ULONG_PTR count = g_ww.size();
        DWORD granularity = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_base + r.lo, r.hi - r.lo, g_ww.data(), &count, &granularity) != 0) continue;
        WriteWatchPublish(1, g_ww.data(), count);   // consumer 1; the GPU backend's texture tracker still sees them
        for (ULONG_PTR i = 0; i < count; ++i)
            out.push_back(uint32_t(static_cast<uint8_t*>(g_ww[i]) - g_base));
        const size_t own = out.size();
        WriteWatchDrain(1, r.lo >> 12, r.hi >> 12, out);   // pages the texture tracker read first
        for (size_t i = own; i < out.size(); ++i) out[i] <<= 12;
    }
}

// Words of hot pages that change BETWEEN updates (the draw, worker threads: job states, heap block
// headers, render lists) are not the fight's: a rewind leaves them as they are. MEASURED: rewinding
// a render job's state word (job+144 = pending) hung the draw in sub_824F58B8. Learned all the time:
// each update's result against the next update's start. One bit per 64-bit word, 8 x u64 per page.
std::vector<uint64_t> g_outside;
std::vector<uint8_t> g_lastPost;                  // the hot pages after the last update
size_t g_lastPostPages = 0;
uint32_t g_keepWord = 0;                           // a word that is the fight's even so (the RNG state)
bool Outside(size_t page, uint32_t word)
{
    return page * 8 + word / 64 < g_outside.size() && (g_outside[page * 8 + word / 64] >> (word % 64) & 1);
}
// The job system's objects are never the fight's either, and their counters often read the same
// at every frame boundary (the learning above misses them). MEASURED hangs: a job group's pending
// count (+8, waited on by sub_82ACFF90) and a render job's state (+144, sub_824F58B8) rewound. Every
// object those waits see is masked whole.
constexpr uint32_t kPoolJobSize = 0xA8;            // vtable, worker, self, event, ..., state +144
std::mutex g_jobMutex;
std::unordered_set<uint64_t> g_jobObjects;        // address << 16 | size
std::vector<std::pair<uint32_t, uint32_t>> g_newJobObjects;
std::vector<uint32_t> g_poolJobs;                  // pool jobs (sub_824F58B8), first-seen order
std::unordered_map<uint32_t, unsigned long> g_poolJobThreads;   // pool job -> host thread running its loop (0: loop ended)
std::vector<int32_t> g_hotIndex;                  // page -> index in g_hot, or -1
// Which host thread runs each pool job's loop (sub_824F5458, hooked in diag_hooks.cpp), for QuiesceJobs.
void NotePoolThread(uint32_t job)
{
    std::lock_guard l(g_jobMutex);
    g_poolJobThreads[job] = GetCurrentThreadId();
}
void NotePoolThreadExit(uint32_t job)
{
    std::lock_guard l(g_jobMutex);
    g_poolJobThreads[job] = 0;
}
void NoteJobObject(uint32_t addr, uint32_t size)
{
    std::lock_guard l(g_jobMutex);
    if (!g_jobObjects.insert(uint64_t(addr) << 16 | size).second) return;
    g_newJobObjects.push_back({ addr, size });
    if (size == kPoolJobSize) g_poolJobs.push_back(addr);
}
// Job objects are noted at their first use and the game frees some of them (a scene's own jobs), so
// a noted address may hold other data by now. A pool job is alive while its handshake pointer +128
// still points at its own +124. MEASURED: a freed loading job's memory reused - its "handshake" read
// 1 for good, every frame waited 50 ms for it (~10 fps, the other side stalled in lockstep; mod34,
// stut47off) - and every fight masked those 168 bytes of whatever lived there now: never rewound
// while the data around them was. Static objects (the job group, the render monitor) live for good.
bool JobObjectLive(uint32_t addr, uint32_t size)
{
    if (size != kPoolJobSize || addr >= 0x82000000u) return true;
    return GRead32(g_base, addr + 128) == addr + 124;
}
// Each fight's calibration: freed job objects are forgotten (noted again if the address becomes a job
// again).
void PruneJobObjects()
{
    std::lock_guard l(g_jobMutex);
    for (auto it = g_jobObjects.begin(); it != g_jobObjects.end();)
    {
        const uint32_t a = uint32_t(*it >> 16), s = uint32_t(*it & 0xFFFF);
        if (JobObjectLive(a, s)) { ++it; continue; }
        std::fprintf(stderr, "[rb] job object %08X/%X is gone (+128 %08X, not its +124): no longer masked or waited for\n", a, s,
                     GRead32(g_base, a + 128));
        g_poolJobs.erase(std::remove(g_poolJobs.begin(), g_poolJobs.end(), a), g_poolJobs.end());
        it = g_jobObjects.erase(it);
    }
    g_newJobObjects.erase(std::remove_if(g_newJobObjects.begin(), g_newJobObjects.end(),
                                         [](const std::pair<uint32_t, uint32_t>& o) { return !JobObjectLive(o.first, o.second); }),
                          g_newJobObjects.end());
}
void MaskRange(uint32_t addr, uint32_t size)
{
    if (g_outside.size() < g_hot.size() * 8) g_outside.resize(g_hot.size() * 8, 0);
    for (uint32_t a = addr & ~7u; a < addr + size; a += 8)
    {
        const int32_t i = (a >> 12) < g_hotIndex.size() ? g_hotIndex[a >> 12] : -1;
        if (i < 0) continue;
        const uint32_t k = (a & (kPage - 1)) / 8;
        g_outside[size_t(i) * 8 + k / 64] |= 1ull << (k % 64);
    }
}
// The job system's static objects: the job group 0x82F4BF00 (queues out to +0x249C4), the singletons
// at 0x82F72100 / 0x82F72C00. MEASURED: their lists half rewound hung list walks and waits.
constexpr uint32_t kJobGroup = 0x82F4BF00, kJobSystemEnd = 0x82F73000;

// The network's state is never rewound either: NACE (Namco's in-match netcode, code 0x82B344A0-
// 0x82B5CA78) keeps sequence numbers, queues and connections; the online manager's static object
// (0x82E6C5B8: the scene flow manager whose +16 is the online battle's input pipeline); and every heap
// block NACE's code allocates (diag_hooks.cpp's RtlAllocateHeap hook asks RollbackOwnedCaller).
struct CodeRange { uint32_t lo, hi; };
constexpr CodeRange kNeverCode[] = { { 0x82B344A0, 0x82B5CA78 } };
constexpr uint32_t kOnlineManager = 0x82E6C5B8, kOnlineManagerSize = 0x1800;
std::mutex g_blockMutex;
std::unordered_map<uint32_t, uint32_t> g_neverBlocks;            // heap block -> size
std::vector<std::pair<uint32_t, uint32_t>> g_newNeverBlocks;

// A pool job is masked whole. Its state +144 goes with the thread handshake (*[job+128] = job+124)
// and the gate lock +60 the main thread holds between runs (lock state kept host-side): the kick
// (sub_824F5798) waits for the thread to take work, then closes the gate again. MEASURED: rewinding the
// state word alone (TTT2_RB_JOBMASK=partial, masks only +16..+136) left a job "handed over" while
// the gate stayed closed - the next kick spun forever (Arcade, a job used after fights).
bool JobMaskFull()
{
    static const bool full = [] { const char* e = std::getenv("TTT2_RB_JOBMASK"); return !(e && std::strcmp(e, "partial") == 0); }();
    return full;
}
void MaskJobObject(uint32_t addr, uint32_t size)
{
    if (size == kPoolJobSize && !JobMaskFull()) MaskRange(addr + 16, 136 - 16);
    else MaskRange(addr, size);
}

// Diagnostics: a pool job's lock words (+16..+124) going all-zero is a broken job (its thread then
// never wakes). Checked at every update start and around restores; the first change is reported
// with what happened since the previous check.
const char* g_lastEvent = "start";
uint32_t g_lastEventFrame = 0;
uint32_t g_updates = 0;                   // updates since boot (diagnostics)
std::unordered_map<uint32_t, bool> g_jobLocksZero;
void CheckJobLocks(const char* where)
{
    std::vector<uint32_t> jobs;
    {
        std::lock_guard l(g_jobMutex);
        jobs = g_poolJobs;
    }
    for (uint32_t job : jobs)
    {
        if (!JobObjectLive(job, kPoolJobSize)) continue;
        bool zero = true;
        for (uint32_t k = 16; k < 124 && zero; k += 4) zero = GRead32(g_base, job + k) == 0;
        auto it = g_jobLocksZero.find(job);
        if (it == g_jobLocksZero.end()) { g_jobLocksZero[job] = zero; continue; }
        if (zero && !it->second)
            std::fprintf(stderr, "[rb] pool job %08X: its locks turned all-zero (%s, update %u; last event: %s at update %u)\n",
                         job, where, g_updates, g_lastEvent, g_lastEventFrame);
        it->second = zero;
    }
}
void NoteEvent(const char* what) { g_lastEvent = what; g_lastEventFrame = g_updates; }

// Masks job objects seen since the last call, and all of them again for pages that just turned hot.
void ApplyJobMasks(bool all)
{
    if (all) MaskRange(kJobGroup, kJobSystemEnd - kJobGroup);
    if (all) MaskRange(kOnlineManager, kOnlineManagerSize);
    {
        std::lock_guard b(g_blockMutex);
        if (all) for (auto [a, s] : g_neverBlocks) MaskRange(a, s);
        else for (auto [a, s] : g_newNeverBlocks) MaskRange(a, s);
        g_newNeverBlocks.clear();
    }
    std::lock_guard l(g_jobMutex);
    if (all)
    {
        for (uint64_t o : g_jobObjects)
            if (JobObjectLive(uint32_t(o >> 16), uint32_t(o & 0xFFFF))) MaskJobObject(uint32_t(o >> 16), uint32_t(o & 0xFFFF));
    }
    else
        for (auto [a, s] : g_newJobObjects)
            if (JobObjectLive(a, s)) MaskJobObject(a, s);
    g_newJobObjects.clear();
}

uint32_t LearnOutside()
{
    if (g_outside.size() < g_hot.size() * 8) g_outside.resize(g_hot.size() * 8, 0);
    uint32_t added = 0;
    for (size_t i = 0; i < g_lastPostPages; ++i)
    {
        const uint64_t* was = reinterpret_cast<const uint64_t*>(g_lastPost.data() + i * kPage);
        const uint64_t* now = reinterpret_cast<const uint64_t*>(g_base + g_hot[i]);
        if (std::memcmp(was, now, kPage) == 0) continue;
        for (uint32_t k = 0; k < kPage / 8; ++k)
            if (was[k] != now[k] && !Outside(i, k) && g_hot[i] + k * 8 != (g_keepWord & ~7u))
            {
                g_outside[i * 8 + k / 64] |= 1ull << (k % 64);
                ++added;
            }
    }
    return added;
}

// Islands: small pieces of excluded memory (thread blocks) that are the frame's state all the same -
// Havok's per-thread memory routers on its threads' stacks (below, "Havok"). Saved and restored with
// every snapshot; only the main thread changes the list.
struct Island { uint32_t addr, size; };
std::vector<Island> g_islands;
void AdoptPendingIslands();   // below: routers noted since (main thread)
void HavokCalibrationStart();                     // below ("Havok")
bool CheckHavokFreeLists(const char* where);
struct Image { uint64_t frame = ~0ull; size_t pages = 0; std::vector<uint8_t> data; size_t islands = 0; std::vector<uint8_t> islandData; };
void Save(Image& img, uint64_t frame)
{
    img.frame = frame;
    img.pages = g_hot.size();
    if (img.data.size() < img.pages * kPage) img.data.resize(img.pages * kPage + 512 * kPage);
    for (size_t i = 0; i < img.pages; ++i) std::memcpy(img.data.data() + i * kPage, g_base + g_hot[i], kPage);
    AdoptPendingIslands();
    size_t bytes = 0;
    for (const Island& s : g_islands) bytes += s.size;
    img.islandData.resize(bytes);
    img.islands = g_islands.size();
    bytes = 0;
    for (const Island& s : g_islands) { std::memcpy(img.islandData.data() + bytes, g_base + s.addr, s.size); bytes += s.size; }
}
void RestoreIslands(const Image& img)
{
    size_t bytes = 0;
    for (size_t i = 0; i < img.islands && i < g_islands.size(); ++i)
    {
        const Island& s = g_islands[i];
        std::memcpy(g_base + s.addr, img.islandData.data() + bytes, s.size);
        bytes += s.size;
    }
}
uint32_t Restore(const Image& img)
{
    // Blocks and job objects noted since the update started (e.g. NACE's pump allocating from the
    // network's arena right before a session's rewind) are masked before anything is copied back.
    ApplyJobMasks(false);
    RestoreIslands(img);
    uint32_t copied = 0;
    for (size_t i = 0; i < img.pages; ++i)
    {
        uint8_t* live = g_base + g_hot[i];
        const uint8_t* saved = img.data.data() + i * kPage;
        if (std::memcmp(live, saved, kPage) == 0) continue;
        ++copied;
        const bool masked = i * 8 + 7 < g_outside.size() &&
            (g_outside[i * 8] | g_outside[i * 8 + 1] | g_outside[i * 8 + 2] | g_outside[i * 8 + 3] |
             g_outside[i * 8 + 4] | g_outside[i * 8 + 5] | g_outside[i * 8 + 6] | g_outside[i * 8 + 7]);
        if (!masked) { std::memcpy(live, saved, kPage); continue; }
        for (uint32_t k = 0; k < kPage / 8; ++k)
            if (!Outside(i, k)) reinterpret_cast<uint64_t*>(live)[k] = reinterpret_cast<const uint64_t*>(saved)[k];
    }
    CheckJobLocks("right after a restore");
    NoteEvent("a restore");
    return copied;
}

void AddHotPage(uint32_t p);
int FighterOf(uint32_t addr, uint32_t& offset);

// ---- whose writes are the frame's ---------------------------------------------------------------
// Everything the frame's threads write is rewound: the main thread and every worker of its job and
// render pipeline (several kinds - the battle tick hands work to threads parked in sub_82258BD8;
// MEASURED: calling only a known few "the frame" kept their pages out and the fight diverged) -
// Havok's job-pool workers (sub_82C7B7E8) too (see "Havok" below). The exceptions run on their own
// timelines and their pages are never rewound: the sound engine (NUSound sub_82D032D0) and the audio
// library's threads (sub_82C3A4E0 / sub_82C3A540 - MEASURED: rewinding pages they share left the
// sound thread spinning in sub_82D3E2D8), the file loaders (sub_823D94B8), and the runtime's own
// host threads (DPC / vblank, command processor, audio output: no guest entry).
DWORD g_mainTid = 0;
std::atomic<DWORD> g_foreignThreads[64];
std::atomic<int> g_foreignThreadCount{ 0 };
void NoteForeignThread(const char* why = "its entry")
{
    const DWORD t = GetCurrentThreadId();
    const int n = (std::min)(g_foreignThreadCount.load(), 64);
    for (int i = 0; i < n; ++i) if (g_foreignThreads[i].load() == t) return;
    const int i = g_foreignThreadCount.fetch_add(1);
    if (i < 64) g_foreignThreads[i].store(t);
    std::fprintf(stderr, "[rb] thread %lu (entry %08X) runs on its own timeline (%s)\n", t, hitch::GuestEntryOfThread(t), why);
}
bool IsFrameThread(DWORD t)
{
    if (t == g_mainTid) return true;
    const int n = (std::min)(g_foreignThreadCount.load(), 64);
    for (int i = 0; i < n; ++i) if (g_foreignThreads[i].load() == t) return false;
    return hitch::GuestEntryOfThread(t) != 0;      // a host thread of the runtime: not the frame's
}

// ---- pages that turn hot after calibration ---------------------------------------------------
// Snapshots taken before a page joined the hot set lack it, and a rewind left it as it was: half
// the timeline rewound (MEASURED: a job's tree walk then looped forever). After calibration every
// other tracked page is write-protected; the first write (any thread, guest or host code) faults
// here, the page's content before it is kept, the page turns hot, and every older snapshot gets
// that content. Host file reads into protected pages would fail instead of faulting: kernel_io.cpp
// opens the range first (RollbackBeforeHostWrite).
SRWLOCK g_cowLock = SRWLOCK_INIT;
std::vector<uint8_t> g_protected(1u << 20);       // per page: write-protected by us
struct LatePage { uint32_t page; std::unique_ptr<uint8_t[]> before; };
std::vector<LatePage> g_pendingLate;               // under g_cowLock
std::atomic<bool> g_cowActive{ false };
// Calibration (full-frame state): every tracked page write-protected at each frame start; the first
// write of a frame says who writes it.
std::atomic<bool> g_attributing{ false };
std::vector<uint8_t> g_frameWritten(1u << 20), g_foreignWritten(1u << 20);
std::vector<DWORD> g_foreignTid(1u << 20);         // diagnostics: the first foreign writer
// TTT2_RB_FOREIGN=keep: rewind other timelines' pages too (diagnostics).
bool ExcludeForeign()
{
    static const bool v = [] { const char* e = std::getenv("TTT2_RB_FOREIGN"); return !(e && std::strcmp(e, "keep") == 0); }();
    return v;
}
std::vector<uint32_t> g_touched;                    // under g_cowLock: opened this frame
// Pages only foreign threads have written (since calibration): not rewound, but watched - protected
// again at every update start, and the first write by a frame thread turns the page into a rewound
// one (its content before that write goes into the older snapshots, as for any late page). Heap pages
// are shared: a page a foreign thread wrote first can hold the game's objects too (MEASURED: a UI
// node there was never rewound while the node linking to it was - a cyclic list, the update hung).
std::vector<uint8_t> g_foreign(1u << 20);          // pages only foreign threads wrote
std::vector<uint32_t> g_foreignOpened;             // under g_cowLock: opened by a foreign thread since the last update start
uint64_t g_foreignToHot = 0, g_foreignFaults = 0;
// Diagnostics: the first pages foreign threads opened after calibration (page, thread, entry, guest
// lr), noted under the lock in the fault handler and printed by the next update.
struct ForeignOpen { uint32_t page; DWORD tid; uint32_t entry, lr; };
std::vector<ForeignOpen> g_foreignOpens;
uint32_t g_foreignOpensTotal = 0;
}   // namespace
PPCContext* WatchGuestContextOf(DWORD tid);   // diag_hooks.cpp
namespace {

void OpenPageLocked(uint32_t page)
{
    if (!g_protected[page >> 12]) return;
    // The sound library's writes are the sound engine's, whichever thread makes them; the online
    // manager's per-frame update (RollbackNetWriting) is the network's.
    const bool frame = IsFrameThread(GetCurrentThreadId()) && !SoundLibraryWriting(page) && !RollbackNetWriting();
    if (g_attributing.load(std::memory_order_relaxed))
    {
        NetNoteFaultLocked(page);
        (frame ? g_frameWritten : g_foreignWritten)[page >> 12] = 1;
        if (!frame && !g_foreignTid[page >> 12]) g_foreignTid[page >> 12] = GetCurrentThreadId();
        DWORD old = 0;
        VirtualProtect(g_base + page, kPage, PAGE_READWRITE, &old);
        g_protected[page >> 12] = 0;
        g_touched.push_back(page);
        return;
    }
    if (!frame && ExcludeForeign() && !g_isHot[page >> 12])   // another timeline's page: not rewound, watched
    {
        DWORD old = 0;
        VirtualProtect(g_base + page, kPage, PAGE_READWRITE, &old);
        g_protected[page >> 12] = 0;
        g_foreignOpened.push_back(page);
        ++g_foreignFaults;
        if (g_foreign[page >> 12]) return;
        g_foreign[page >> 12] = 1;
        if (g_foreignOpensTotal++ < 200)
        {
            const DWORD tid = GetCurrentThreadId();
            const PPCContext* c = WatchGuestContextOf(tid);
            g_foreignOpens.push_back({ page, tid, hitch::GuestEntryOfThread(tid), c ? uint32_t(c->lr) : 0 });
        }
        return;
    }
    if (g_foreign[page >> 12]) { g_foreign[page >> 12] = 0; ++g_foreignToHot; }   // the frame writes it too: rewound from now on
    auto before = std::make_unique<uint8_t[]>(kPage);
    std::memcpy(before.get(), g_base + page, kPage);
    DWORD old = 0;
    VirtualProtect(g_base + page, kPage, PAGE_READWRITE, &old);
    g_protected[page >> 12] = 0;
    g_pendingLate.push_back({ page, std::move(before) });
}

bool InTracked(uint32_t guest)
{
    for (const Range& r : kRanges)
        if (guest >= r.lo && guest < r.hi) return true;
    return false;
}

LONG CALLBACK CowVeh(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1 || !g_base)
        return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t a = r->ExceptionInformation[1];
    if (a < uint64_t(g_base) || a >= uint64_t(g_base) + 0x100000000ull) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t guest = uint32_t(a - uint64_t(g_base));
    if (!InTracked(guest)) return EXCEPTION_CONTINUE_SEARCH;
    // Ours if we protected it - whatever the mode flags say a moment later (a thread can fault
    // between a mode ending and its pages opening) - or, while a mode runs, if another thread just
    // opened it. Anything else is a real fault.
    const bool active = g_cowActive.load(std::memory_order_acquire) || g_attributing.load(std::memory_order_acquire);
    AcquireSRWLockExclusive(&g_cowLock);
    const uint32_t page = guest & ~(kPage - 1);
    const bool ours = g_protected[page >> 12] != 0;
    if (ours && active) OpenPageLocked(page);
    else if (ours)
    {
        DWORD old = 0;
        VirtualProtect(g_base + page, kPage, PAGE_READWRITE, &old);
        g_protected[page >> 12] = 0;
    }
    ReleaseSRWLockExclusive(&g_cowLock);
    if (ours || active) return EXCEPTION_CONTINUE_EXECUTION;
    // Opened while this thread waited for the lock (a mode ending opens every page under it): the
    // write goes through now.
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(g_base + page, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

// Calibration start: every tracked page but the excluded ones, write-protected.
void StartAttribution()
{
    ClearNetWords();
    static std::once_flag veh;
    std::call_once(veh, [] { AddVectoredExceptionHandler(1, CowVeh); });
    AcquireSRWLockExclusive(&g_cowLock);
    std::fill(g_frameWritten.begin(), g_frameWritten.end(), 0);
    std::fill(g_foreignWritten.begin(), g_foreignWritten.end(), 0);
    std::fill(g_foreign.begin(), g_foreign.end(), 0);
    std::fill(g_foreignTid.begin(), g_foreignTid.end(), 0);
    g_touched.clear();
    for (const Range& r : kRanges)
        for (uint32_t p = r.lo; p < r.hi;)
        {
            if (g_excluded[p >> 12]) { p += kPage; continue; }
            const uint32_t start = p;
            while (p < r.hi && !g_excluded[p >> 12]) { g_protected[p >> 12] = 1; p += kPage; }
            DWORD old = 0;
            VirtualProtect(g_base + start, p - start, PAGE_READONLY, &old);
        }
    g_attributing.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_cowLock);
}
// Each calibration frame start: the pages written last frame, protected again.
void ReprotectTouched()
{
    AcquireSRWLockExclusive(&g_cowLock);
    std::sort(g_touched.begin(), g_touched.end());
    for (size_t i = 0; i < g_touched.size();)
    {
        size_t j = i + 1;
        while (j < g_touched.size() && g_touched[j] == g_touched[j - 1] + kPage) ++j;
        DWORD old = 0;
        VirtualProtect(g_base + g_touched[i], uint32_t(j - i) * kPage, PAGE_READONLY, &old);
        for (size_t k = i; k < j; ++k) g_protected[g_touched[k] >> 12] = 1;
        i = j;
    }
    g_touched.clear();
    ReleaseSRWLockExclusive(&g_cowLock);
}
void UnprotectAll();
// Calibration end: the hot set = pages only frame threads wrote; pages a foreign thread wrote are
// excluded for good.
void FinishAttribution()
{
    g_attributing.store(false, std::memory_order_release);
    UnprotectAll();
    uint32_t hot = 0, foreign = 0, shared = 0;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> byEntry;   // foreign thread entry -> pages, shared pages
    for (uint32_t p = 0; p < (1u << 20); ++p)
    {
        if (g_foreignWritten[p])
        {
            ++foreign;
            shared += g_frameWritten[p];
            auto& e = byEntry[hitch::GuestEntryOfThread(g_foreignTid[p])];
            ++e.first;
            e.second += g_frameWritten[p];
            if (g_frameWritten[p] && shared <= 60)
            {
                uint32_t off = 0;
                const int f = FighterOf(p << 12, off) >= 0 ? FighterOf(p << 12, off) : FighterOf((p << 12) + kPage - 4, off);
                std::fprintf(stderr, "[rb]   shared page %08X: frame + thread entry %08X%s\n", p << 12,
                             hitch::GuestEntryOfThread(g_foreignTid[p]), f >= 0 ? " (a fighter)" : "");
            }
            if (ExcludeForeign() && !g_frameWritten[p]) { g_foreign[p] = 1; continue; }   // watched (ProtectCold)
        }
        if ((g_frameWritten[p] || g_foreignWritten[p]) && !g_excluded[p] && !g_isHot[p]) { AddHotPage(p << 12); ++hot; }
    }
    std::fprintf(stderr, "[rb] attribution: %u hot pages; %u written by other threads (%u also by the frame)%s\n",
                 hot, foreign, shared, ExcludeForeign() ? ", those kept out" : "");
    for (auto& [entry, n] : byEntry)
        std::fprintf(stderr, "[rb]   thread entry %08X: %u pages (%u shared with the frame)\n", entry, n.first, n.second);
    // Diagnostics (TTT2_RB_PAGEMAP=1): every foreign page run with its writer's entry, and the hot runs.
    static const bool pagemap = [] { const char* e = std::getenv("TTT2_RB_PAGEMAP"); return e && *e == '1'; }();
    if (!pagemap) return;
    for (uint32_t p = 0; p < (1u << 20);)
    {
        if (!g_foreignWritten[p] && !g_isHot[p]) { ++p; continue; }
        const bool f = g_foreignWritten[p] != 0;
        const uint32_t entry = f ? hitch::GuestEntryOfThread(g_foreignTid[p]) : 0;
        uint32_t q = p;
        while (q < (1u << 20) && (g_foreignWritten[q] != 0) == f && (g_isHot[q] != 0 || g_foreignWritten[q]) &&
               (!f || hitch::GuestEntryOfThread(g_foreignTid[q]) == entry)) ++q;
        std::fprintf(stderr, "[rb]   pagemap %08X-%08X %s %08X\n", p << 12, q << 12, f ? "foreign" : "hot", entry);
        p = q;
    }
}

void ProtectCold()
{
    static std::once_flag veh;
    std::call_once(veh, [] { AddVectoredExceptionHandler(1, CowVeh); });
    AcquireSRWLockExclusive(&g_cowLock);
    uint32_t runs = 0, pages = 0;
    for (const Range& r : kRanges)
        for (uint32_t p = r.lo; p < r.hi;)
        {
            if (g_isHot[p >> 12] || g_excluded[p >> 12]) { p += kPage; continue; }
            const uint32_t start = p;
            while (p < r.hi && !g_isHot[p >> 12] && !g_excluded[p >> 12]) { g_protected[p >> 12] = 1; p += kPage; ++pages; }
            DWORD old = 0;
            VirtualProtect(g_base + start, p - start, PAGE_READONLY, &old);
            ++runs;
        }
    g_cowActive.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_cowLock);
    std::fprintf(stderr, "[rb] %u cold pages write-protected (%u runs)\n", pages, runs);
}

void UnprotectAll()
{
    AcquireSRWLockExclusive(&g_cowLock);
    g_cowActive.store(false, std::memory_order_release);
    g_attributing.store(false, std::memory_order_release);
    g_touched.clear();
    for (const Range& r : kRanges)
        for (uint32_t p = r.lo; p < r.hi;)
        {
            if (!g_protected[p >> 12]) { p += kPage; continue; }
            const uint32_t start = p;
            while (p < r.hi && g_protected[p >> 12]) { g_protected[p >> 12] = 0; p += kPage; }
            DWORD old = 0;
            VirtualProtect(g_base + start, p - start, PAGE_READWRITE, &old);
        }
    g_pendingLate.clear();
    g_foreignOpened.clear();
    ReleaseSRWLockExclusive(&g_cowLock);
}

// Every update start once calibrated: the foreign pages opened since, protected again (watched).
void ReprotectForeign()
{
    AcquireSRWLockExclusive(&g_cowLock);
    for (uint32_t page : g_foreignOpened)
    {
        if (!g_foreign[page >> 12] || g_protected[page >> 12] || g_isHot[page >> 12]) continue;
        DWORD old = 0;
        VirtualProtect(g_base + page, kPage, PAGE_READONLY, &old);
        g_protected[page >> 12] = 1;
    }
    g_foreignOpened.clear();
    ReleaseSRWLockExclusive(&g_cowLock);
}

// The main thread's stack below the update's frame: what the update finds there (leftovers of the
// previous draw) - TTT2_RB_STACK=1 puts each frame's back before re-simulating it (a test: does the
// update read uninitialised stack?).
constexpr uint32_t kStackBytes = 0x10000;
// On by default with online rollback (TTT2_RB_STACK=0 turns it off): MEASURED, without it the same frame
// re-simulated from the same snapshot with the same record gave different fighters (the fight code
// reads uninitialised locals - leftovers of whatever ran on the stack before).
const bool kStackRestore = [] {
    const char* e = std::getenv("TTT2_RB_STACK");
    if (e && *e) return *e == '1';
    const char* s = std::getenv("TTT2_ROLLBACK");
    return s && *s == '1';
}();
std::vector<uint8_t> g_stack[kRing];
void SaveStack(int slot, uint32_t r1)
{
    g_stack[slot].assign(g_base + r1 - kStackBytes, g_base + r1);
}

// ---- random numbers ---------------------------------------------------------------------------
constexpr uint32_t kRngHolder = 0x82E6B05C;      // static object; +0 -> the generator, state at +16
uint32_t g_rngAt = 0, g_simRng = 0, g_cosmeticRng = 0;
void RngIn()
{
    const uint32_t gen = GRead32(g_base, kRngHolder);
    if (!gen) return;
    if (gen + 16 != g_rngAt)
    {
        g_rngAt = gen + 16;
        g_keepWord = g_rngAt;
        g_simRng = GRead32(g_base, g_rngAt);
    }
    g_cosmeticRng = GRead32(g_base, g_rngAt);
    GWrite32(g_base, g_rngAt, g_simRng);
}
void RngOut()
{
    if (!g_rngAt) return;
    g_simRng = GRead32(g_base, g_rngAt);
    GWrite32(g_base, g_rngAt, g_cosmeticRng);
}

Image g_snap[kRing];      // the hot set at the start of each fight update
Image g_post[kRing];      // sync test: the hot set after each original update
uint64_t g_latePages = 0;

// Pages opened since the last call join the hot set; every image taken before keeps their earlier
// content (an image that lacks an earlier page is dropped).
void AddHotPage(uint32_t p);
void IncorporateLate()
{
    std::vector<LatePage> late;
    AcquireSRWLockExclusive(&g_cowLock);
    late.swap(g_pendingLate);
    ReleaseSRWLockExclusive(&g_cowLock);
    bool added = false;
    for (LatePage& lp : late)
    {
        if (g_isHot[lp.page >> 12]) continue;
        const size_t index = g_hot.size();
        AddHotPage(lp.page);
        ++g_latePages;
        added = true;
        for (Image* set : { g_snap, g_post })
            for (int s = 0; s < kRing; ++s)
            {
                Image& img = set[s];
                if (img.frame == ~0ull) continue;
                if (img.pages != index) { img.frame = ~0ull; continue; }
                if (img.data.size() < (index + 1) * kPage) img.data.resize((index + 1) * kPage + 256 * kPage);
                std::memcpy(img.data.data() + index * kPage, lp.before.get(), kPage);
                img.pages = index + 1;
            }
    }
    // The never-rewound words on the new pages (job threads' locks, NACE's blocks, ...) are masked
    // now, before any restore can write the page whole (MEASURED: masked only at the next update, a
    // restore in between zeroed a pool job's locks - its thread never woke, the next fight's kick of
    // that job spun forever).
    if (added) ApplyJobMasks(true);
}
uint64_t g_barrier = 0;   // no rewind to before this frame (a page joined the hot set later)

// ---- inputs: every sub_822745B0 call of an update, in order (pad slot, buttons) ---------------
struct Inputs { uint64_t frame = ~0ull; std::vector<std::pair<uint32_t, uint32_t>> calls; };
Inputs g_inputs[kRing];
Inputs* g_recording = nullptr;
const Inputs* g_replaying = nullptr;
size_t g_replayAt = 0;
uint32_t g_replayMismatch = 0;

// ---- fighters (diagnostics): the battle's per-side table (sub_824CB300 / sub_824CB340), +172 =
// the partner; objects of 0x28B0 bytes in .bss holding the fight's core state.
constexpr uint32_t kFighterTable = 0x82E88878, kFighterSize = 0x28B0;
uint32_t g_fighters[4] = {};
void FindFighters()
{
    for (int side = 0; side < 2; ++side)
    {
        const uint32_t p = GRead32(g_base, kFighterTable + side * 4);
        g_fighters[side * 2] = p;
        g_fighters[side * 2 + 1] = p ? GRead32(g_base, p + 172) : 0;
    }
}
int FighterOf(uint32_t addr, uint32_t& offset)
{
    for (int k = 0; k < 4; ++k)
        if (g_fighters[k] && addr >= g_fighters[k] && addr < g_fighters[k] + kFighterSize) { offset = addr - g_fighters[k]; return k; }
    return -1;
}

// ---- fight state ------------------------------------------------------------------------------
bool g_tick = false;               // the battle tick ran during the current update
bool g_resimDraw = false;          // a re-simulated frame's draw is running (main thread)
uint32_t g_throttleSkips = 0;
double g_resimTimes[16] = {};      // diagnostics: time in job-system calls during re-simulated draws
double g_fenceWaitMs = 0;          // diagnostics: the frame kick's GPU fence waits during one rewind
bool g_inUpdate = false;
std::vector<uint32_t> g_tickPages;  // TTT2_RB_SET=tick: the pages the last tick wrote
bool g_tickDone = false;
int g_fightRun = 0;                // consecutive updates with a battle tick
bool g_resimulating = false;       // a session's re-simulation is running (main thread)
bool g_fightOnline = false;         // this fight's NACE input sync ran (an online fight: sessions apply)
uint64_t g_frame = 0;              // fight frame counter (live updates)

struct Stats
{
    double save = 0, watch = 0;
    uint64_t heapMain = 0, heapOther = 0, heapMainOutside = 0, outside = 0;
    uint32_t frames = 0, tests = 0, failed = 0, late = 0, groupStuck = 0;
} g_st;

// Whole allocations that are another timeline's, recognised by who allocates them (the guest
// return address in the allocation's call chain): the sound system's arena (sub_8255DAB0, ~3.7 MB;
// MEASURED: the sound thread writes inside it, and so does the main thread - requests, voice
// objects - so rewinding the main thread's pages there hung the sound engine). Never rewound.
std::mutex g_neverMutex;
std::vector<std::pair<uint32_t, uint32_t>> g_neverRewind;
constexpr uint32_t kNeverOwners[] = { 0x8255DAB0 };

void StartCalibration()
{
    UnprotectAll();
    PruneJobObjects();
    std::fill(g_isHot.begin(), g_isHot.end(), 0);
    std::fill(g_excluded.begin(), g_excluded.end(), 0);
    g_hot.clear();
    g_hotIndex.assign(1u << 20, -1);
    g_outside.clear();
    g_lastPostPages = 0;
    for (auto& s : g_snap) s.frame = ~0ull;
    for (auto& s : g_post) s.frame = ~0ull;
    std::vector<std::pair<uint32_t, uint32_t>> blocks;
    GuestThreadBlocks(blocks);
    const uint32_t device = VdDeviceGuest();
    // The D3D device: its ring / fence / flip-queue state belongs to the GPU side and the vblank DPC
    // (fields out to +24460: the vblank event; MEASURED: rewinding its tail broke the swap token).
    if (device) blocks.push_back({ device, 0x6000 });
    {
        std::lock_guard l(g_neverMutex);
        ResimScratchReady(g_base, g_neverRewind);
        blocks.insert(blocks.end(), g_neverRewind.begin(), g_neverRewind.end());
    }
    for (auto [lo, size] : blocks)
        for (uint32_t p = lo & ~(kPage - 1); p < lo + size; p += kPage) g_excluded[p >> 12] = 1;
    std::fprintf(stderr, "[rb] fight: calibrating (%zu thread blocks and the D3D device %08X excluded)\n", blocks.size() - (device != 0), device);
    HavokCalibrationStart();
}

void AddHotPage(uint32_t p)
{
    g_isHot[p >> 12] = 1;
    if (g_hotIndex.empty()) g_hotIndex.assign(1u << 20, -1);
    g_hotIndex[p >> 12] = int32_t(g_hot.size());
    g_hot.push_back(p);
}

uint32_t AddHot(const std::vector<uint32_t>& pages, bool late)
{
    uint32_t added = 0;
    for (uint32_t p : pages)
    {
        if (g_excluded[p >> 12] || g_isHot[p >> 12]) continue;
        AddHotPage(p);
        ++added;
        if (late && g_st.late++ < 20) std::fprintf(stderr, "[rb]   late page %08X (frame %llu)\n", p, (unsigned long long)g_frame);
    }
    return added;
}

// A re-simulated update against the original one's result. Details for the first tests: every
// differing fighter word, a few other pages.
// Differences are logged for any test while a run-wide budget lasts (a failure late in a fight - a
// KO, a round end - shows its words too).
int g_fighterBudget = 600, g_pageBudget = 80;
uint32_t Compare(const Image& post, uint64_t frame, int& logged, bool detail, uint32_t& fighterWords)
{
    uint32_t diffs = 0;
    int fighterLogged = 0;
    detail = detail || g_fighterBudget > 0;
    for (size_t i = 0; i < post.pages; ++i)
    {
        const uint8_t* now = g_base + g_hot[i];
        const uint8_t* was = post.data.data() + i * kPage;
        if (std::memcmp(now, was, kPage) == 0) continue;
        ++diffs;
        uint32_t words = 0, first = kPage;
        for (uint32_t k = 0; k < kPage; k += 4)
        {
            if (std::memcmp(now + k, was + k, 4) == 0 || Outside(i, k / 8)) continue;
            ++words;
            if (first == kPage) first = k;
            uint32_t off = 0;
            const int f = FighterOf(g_hot[i] + k, off);
            if (f < 0) continue;
            ++fighterWords;
            if (detail && fighterLogged++ < 40 && g_fighterBudget-- > 0)
                std::fprintf(stderr, "[rb]   frame %llu fighter %d +%04X orig %08X resim %08X\n", (unsigned long long)frame, f, off,
                             _byteswap_ulong(*reinterpret_cast<const uint32_t*>(was + k)), _byteswap_ulong(*reinterpret_cast<const uint32_t*>(now + k)));
        }
        if (!words) { --diffs; continue; }
        if (!detail || logged >= 10 || (g_st.tests >= 3 && (fighterWords == 0 || g_pageBudget-- <= 0))) continue;
        std::fprintf(stderr, "[rb]   frame %llu page %08X: %u words differ, first +%03X orig %08X resim %08X\n",
                     (unsigned long long)frame, g_hot[i], words, first,
                     _byteswap_ulong(*reinterpret_cast<const uint32_t*>(was + first)),
                     _byteswap_ulong(*reinterpret_cast<const uint32_t*>(now + first)));
        ++logged;
    }
    return diffs;
}

void Update(PPCContext& ctx, uint8_t* base, const PPCContext& entry, uint64_t frame, bool resim, bool inputs = true)
{
    ctx = entry;
    Inputs& in = g_inputs[frame % kRing];
    if (!inputs) {}                                 // a session: the inputs are NACE's records
    else if (resim)
    {
        g_replaying = &in;
        g_replayAt = 0;
    }
    else
    {
        in.frame = frame;
        in.calls.clear();
        g_recording = &in;
    }
    g_tick = false;
    g_inUpdate = true;
    __imp__sub_825EF0B8(ctx, base);
    g_inUpdate = false;
    g_recording = nullptr;
    g_replaying = nullptr;
}

// Diagnostics: the job objects' key words (group +8 pending, job +144 state, monitor +16/+20 count;
// the render singletons 0x82F4BF00 +212, 0x82F72100 +4, 0x82F72C98 +8).
void DumpJobWords(const char* when)
{
    std::vector<uint64_t> objs;
    {
        std::lock_guard l(g_jobMutex);
        objs.assign(g_jobObjects.begin(), g_jobObjects.end());
    }
    std::sort(objs.begin(), objs.end());
    char line[2048];
    int n = std::snprintf(line, sizeof(line), "[rb] %s:", when);
    for (uint64_t o : objs)
    {
        const uint32_t a = uint32_t(o >> 16), size = uint32_t(o & 0xFFFF);
        const uint32_t word = size == 0x100 ? GRead32(g_base, a + 8) : size == kPoolJobSize ? GRead32(g_base, a + 144) : GRead32(g_base, a + 20);
        if (n < int(sizeof(line)) - 40) n += std::snprintf(line + n, sizeof(line) - n, " %08X/%X=%X", a, size, word);
        // A pool job's thread handshake word (*[+128]: 4 idle, 1 work kicked, 2 taken, 3 running, 5 exit).
        const uint32_t sync = size == kPoolJobSize ? GRead32(g_base, a + 128) : 0;
        if (sync >= 0x10000u && sync < 0xFFFF0000u && n < int(sizeof(line)) - 40)
            n += std::snprintf(line + n, sizeof(line) - n, "(sync@%08X=%X)", sync, GRead32(g_base, sync));
    }
    if (n < int(sizeof(line)) - 80)
        std::snprintf(line + n, sizeof(line) - n, " | 82F4BF00+212=%X 82F72100+4=%X 82F72C98+8=%X",
                      GRead32(g_base, 0x82F4BF00 + 212), GRead32(g_base, 0x82F72100 + 4), GRead32(g_base, 0x82F72C98 + 8));
    std::fprintf(stderr, "%s\n", line);
}

// The update starts pool jobs (animation and the like, state +144 = 2) that the draw finishes first
// thing (MEASURED with TTT2_RB_JOBTRACE). A re-simulated update has no draw after it: starting a job
// again while it is still pending broke the job system (the draw then waited forever). Finish them
// here, the draw's way.
// Pool jobs have a thread each (sub_824F5458): it polls its handshake word (*[job+128]: 4 idle, 1 work
// kicked, 2 taken, 3 running) about every millisecond and runs the job's callback while holding the
// job's lock (+24), so a frame's start is not quiet. Every snapshot and every restore waits until no
// job thread has work: nothing then runs across a restore (MEASURED: re-simulating other inputs, a
// job thread working across a restore left its lock broken and the draw's finish waited forever,
// ~2900 frames into a TTT2_RB_CHAOS run). Only waits - the jobs run where the game runs them.
// The render thread (sub_82AD2AF8) is the other one: the draw's frame kick sets its monitor
// 0x82F72178 to 1, the thread walks the frame's render nodes - the HUD / UI library's tables among
// them - and sets it back to 0, while the next update already runs. A snapshot taken meanwhile is
// torn and a restore pulls those tables out from under it (MEASURED: name lookups in the UI library
// at 0x82B6-0x82B7 looped forever in cyclic hash chains, in sync tests too). Every snapshot and
// restore waits for 0, as the next frame kick would.
constexpr uint32_t kRenderMonitor = 0x82F72178;
double g_quiesceMs = 0, g_renderWaitMs = 0;
uint64_t g_quiesceWaits = 0, g_quiesceTimeouts = 0;
uint64_t g_quiesceSelfBlocked = 0, g_quiesceLent = 0;
bool LendOn()
{
    static const bool v = [] { const char* e = std::getenv("TTT2_RB_LEND"); return !(e && *e == '0'); }();
    return v;
}
// Havok's job-pool workers inside a job run (sub_82C78918 on a sub_82C7B7E8 thread; "Havok" below).
std::atomic<int> g_hkBusy{ 0 };
uint64_t g_hkQuiesceWaits = 0, g_hkQuiesceTimeouts = 0;

// The file loaders' jobs. Four loader threads (sub_823D94B8) take jobs from queues (GetNext sub_823D9340 /
// sub_823D95E8: under the queue's mutex, list +8 head / +16 count, popped by sub_823D9280), run them and
// finish each through its group (sub_823D8DB8: finished +24 against submitted +20, the job back on the
// group's free list, the group's event set). The threads run on their own timeline (never rewound), but the
// queues (static, 0x82EDB8D4), the groups and the jobs live in memory the frame writes - the main thread
// submits - so a snapshot or a restore with a job queued or running tore the two apart. MEASURED 2026-10-21
// (two-PC LAN session, the guest's log): 15 s into a round the main thread waited for a group to finish
// (sub_823D8CD0 from sub_8251FBE8) with every loader thread idle - a finish a restore had undone, so the
// group stayed one job short for good: the freeze. The UNMAPPED lr 823D8E6C seen before was the same
// thing the other way round: a job finished whose object a restore had put back to before it existed.
// Every snapshot and restore waits until no queue holds a job and no loader thread runs one (QuiesceJobs),
// as for the pool jobs: what is rewound is then a loader at rest. A loader thread is busy from the pop
// (marked before the count drops, under the queue's mutex) to its next GetNext.
// TTT2_RB_LOADER=0: not waited for (diagnostics, the old behaviour).
bool LoaderQuiesceOn()
{
    static const bool v = [] { const char* e = std::getenv("TTT2_RB_LOADER"); return !(e && *e == '0'); }();
    return v;
}
std::mutex g_loaderMutex;
std::vector<uint32_t> g_loaderQueues;              // the queues loader threads take jobs from (under g_loaderMutex)
std::atomic<int> g_loaderBusy{ 0 };                // loader threads running a job they took
std::atomic<unsigned long> g_loaderBusyTid[8];     // which ones (lending, as for the pool jobs)
std::atomic<uint64_t> g_loaderTaken{ 0 };          // jobs taken (diagnostics)
thread_local bool t_loaderInGetNext = false;
thread_local int t_loaderSlot = -1;                // this loader thread's g_loaderBusyTid slot while busy
uint64_t g_loaderWaits = 0, g_loaderTimeouts = 0, g_loaderLent = 0;
double g_loaderWaitMs = 0, g_loaderWaitMax = 0;

void LoaderNoteQueue(uint32_t q)
{
    std::lock_guard l(g_loaderMutex);
    if (std::find(g_loaderQueues.begin(), g_loaderQueues.end(), q) == g_loaderQueues.end())
    {
        g_loaderQueues.push_back(q);
        std::fprintf(stderr, "[rb-ld] loader queue %08X (thread %lu)\n", q, GetCurrentThreadId());
    }
}
void LoaderDone()
{
    if (t_loaderSlot < 0) return;
    if (t_loaderSlot < 8) g_loaderBusyTid[t_loaderSlot].store(0);
    t_loaderSlot = -1;
    g_loaderBusy.fetch_sub(1);
}
// The groups jobs were finished through (sub_823D8DB8's r3; diagnostics: the watchdog's view).
std::atomic<uint32_t> g_loaderGroups[32];
void LoaderNoteGroup(uint32_t g)
{
    for (auto& slot : g_loaderGroups)
    {
        const uint32_t v = slot.load(std::memory_order_relaxed);
        if (v == g) return;
        uint32_t expected = 0;
        if (!v && slot.compare_exchange_strong(expected, g)) return;
    }
}
// Queue counts, loader threads busy, and the groups with jobs not finished (submitted +20, finished +24).
std::string LoaderStateText()
{
    char line[512];
    int n = std::snprintf(line, sizeof(line), "file loaders: %d running, queues", g_loaderBusy.load());
    {
        std::lock_guard l(g_loaderMutex);
        for (uint32_t q : g_loaderQueues)
            if (n < int(sizeof(line)) - 24) n += std::snprintf(line + n, sizeof(line) - n, " %08X=%u", q, GRead32(g_base, q + 16));
    }
    n += std::snprintf(line + n, sizeof(line) - n, "; groups not done:");
    int shown = 0;
    for (auto& slot : g_loaderGroups)
    {
        const uint32_t g = slot.load();
        if (!g || n >= int(sizeof(line)) - 40) continue;
        const uint32_t submitted = GRead32(g_base, g + 20), finished = GRead32(g_base, g + 24);
        if (submitted == finished) continue;
        n += std::snprintf(line + n, sizeof(line) - n, " %08X %u/%u", g, finished, submitted);
        ++shown;
    }
    if (!shown) std::snprintf(line + n, sizeof(line) - n, " none");
    return line;
}
// A queue still holding a job, or a loader thread still running one. The counts are read before the busy
// count: a thread marks itself busy before its pop lowers a count (program order, x86 stores in order).
bool LoaderBusy()
{
    {
        std::lock_guard l(g_loaderMutex);
        for (uint32_t q : g_loaderQueues)
            if (GRead32(g_base, q + 16) != 0) return true;
    }
    return g_loaderBusy.load() > 0;
}
void QuiesceJobs(uint8_t*, const PPCContext&)
{
    std::vector<uint32_t> jobs;
    {
        std::lock_guard l(g_jobMutex);
        jobs = g_poolJobs;
    }
    const auto r0 = std::chrono::steady_clock::now();
    while (GRead32(g_base, kRenderMonitor + 16) != 0 || GRead32(g_base, kRenderMonitor + 20) != 0)
    {
        if (Ms(r0, std::chrono::steady_clock::now()) > 100)
        {
            if (g_quiesceTimeouts++ < 10)
                std::fprintf(stderr, "[rb] the render thread still busy after 100 ms (monitor %08X%08X)\n",
                             GRead32(g_base, kRenderMonitor + 16), GRead32(g_base, kRenderMonitor + 20));
            break;
        }
        SwitchToThread();
    }
    // The job group's workers (sub_82AD00E8; pending count +8): idle too.
    while (GRead32(g_base, kJobGroup + 8) != 0)
    {
        if (Ms(r0, std::chrono::steady_clock::now()) > 100)
        {
            if (g_quiesceTimeouts++ < 10)
                std::fprintf(stderr, "[rb] the job group still has %u pending after 100 ms\n", GRead32(g_base, kJobGroup + 8));
            break;
        }
        SwitchToThread();
    }
    g_renderWaitMs += Ms(r0, std::chrono::steady_clock::now());
    const auto t0 = std::chrono::steady_clock::now();
    bool waited = false;
    const unsigned long self = GetCurrentThreadId();
    struct Lent { uint32_t section; int depth; uint32_t lr; };
    std::vector<Lent> lent;
    for (uint32_t job : jobs)
    {
        const uint32_t sync = GRead32(g_base, job + 128);
        if (sync != job + 124) continue;   // gone (JobObjectLive)
        unsigned long jobThread = 0;
        bool loopEnded = false;
        {
            std::lock_guard l(g_jobMutex);
            const auto it = g_poolJobThreads.find(job);
            if (it != g_poolJobThreads.end()) { jobThread = it->second; loopEnded = it->second == 0; }
        }
        for (;;)
        {
            const uint32_t s = GRead32(g_base, sync);
            if (s != 1 && s != 2 && s != 3) break;
            if (loopEnded) break;              // no thread left to take or finish it
            // Its thread waits for a section this thread holds: waiting alone only ran into the timeout,
            // frame after frame (the stutter between rounds, 2026-10-20), and going on without it left the
            // job half done across the snapshot or the restore. MEASURED 2026-10-21: the physics job
            // (vtable 8208322C) allocates under its group's section [job+148]+8 (lr 824F76A4), which the
            // main thread holds all frame and leaves only inside its wait for the job (sub_824F6C28) - a
            // step kicked in a draw that needs memory finishes in the next frame's draw, so a snapshot or
            // a restore came in the middle of it (a round running, battle state 4), and the step went on
            // with a freed shape's vtable (lr 82CF5B5C) - the online freeze. The section is lent, as the
            // game's own wait does: left here, the job finishes, entered again below. Same order in every
            // run of a frame (live, re-simulated, both players). TTT2_RB_LEND=0: not lent (diagnostics).
            uint32_t section = 0, ownerLr = 0;
            if (jobThread && CsThreadBlockedBy(jobThread, self, &section, &ownerLr))
            {
                uint32_t lentLr = 0;
                // Only a job inside its callback (3): one kicked but not taken yet (1 / 2) waits for its
                // gate, which the main thread leaves when it means the job to start.
                const int depth = LendOn() && s == 3 ? CsLend(section, lentLr) : 0;
                if (depth > 0)
                {
                    lent.push_back({ section, depth, lentLr });
                    if (g_quiesceLent++ < 5)
                        std::fprintf(stderr, "[rb] pool job %08X (handshake %u) waits for section %08X, which this thread holds (taken at lr %08X): lent until the job is done (fight update %d, battle state %u)\n",
                                     job, s, section, lentLr, g_fightRun, GRead32(g_base, 0x82EAC03Cu));
                    waited = true;
                    continue;
                }
                if (g_quiesceSelfBlocked++ < 20)
                {
                    const PPCContext* jc = WatchGuestContextOf(jobThread);
                    if (jc)
                    {
                        char chain[160];
                        int k = 0;
                        uint32_t sp = jc->r1.u32;
                        for (int i = 0; i < 10 && sp && sp < 0xFFFFFFF0u && k < int(sizeof(chain)) - 10; ++i)
                        {
                            const uint32_t prev = GRead32(g_base, sp);
                            if (!prev || prev <= sp || prev - sp > 0x10000) break;
                            k += std::snprintf(chain + k, sizeof(chain) - k, " %08X", GRead32(g_base, prev - 8));
                            sp = prev;
                        }
                        chain[k] = 0;
                        std::fprintf(stderr, "[rb]   its guest callers:%s\n", chain);
                    }
                    std::fprintf(stderr, "[rb] pool job %08X (handshake %u): its thread (guest lr %08X) waits for section %08X, which this thread holds (taken at lr %08X) - not waited for (fight update %d, frame %llu, battle state %u%s)\n",
                                 job, s, jc ? uint32_t(jc->lr) : 0, section, ownerLr, g_fightRun, (unsigned long long)g_frame,
                                 GRead32(g_base, 0x82EAC03Cu), g_resimulating ? ", re-simulating" : "");
                }
                break;
            }
            waited = true;
            if (Ms(t0, std::chrono::steady_clock::now()) > 50)
            {
                if (g_quiesceTimeouts++ < 10)
                    std::fprintf(stderr, "[rb] pool job %08X still busy (handshake %u) after 50 ms\n", job, s);
                break;
            }
            SwitchToThread();
        }
    }
    // Havok's workers: the pool job that steps the physics waits for them (their finish semaphore), so
    // they are idle whenever it is - still running a job here would be a step across the frame start.
    if (g_hkBusy.load() > 0)
    {
        ++g_hkQuiesceWaits;
        while (g_hkBusy.load() > 0)
        {
            if (Ms(t0, std::chrono::steady_clock::now()) > 50)
            {
                if (g_hkQuiesceTimeouts++ < 10)
                    std::fprintf(stderr, "[rb-hk] %d Havok workers still run a job after 50 ms\n", g_hkBusy.load());
                break;
            }
            SwitchToThread();
        }
    }
    // The file loaders: nothing queued, nothing running ("the file loaders' jobs" above). A loader thread
    // waiting for a section this thread holds gets it lent, as a pool job does.
    if (LoaderQuiesceOn() && g_base && LoaderBusy())
    {
        const auto l0 = std::chrono::steady_clock::now();
        ++g_loaderWaits;
        while (LoaderBusy())
        {
            bool lentOne = false;
            for (auto& slot : g_loaderBusyTid)
            {
                const unsigned long tid = slot.load();
                uint32_t section = 0, ownerLr = 0;
                if (!tid || !CsThreadBlockedBy(tid, self, &section, &ownerLr)) continue;
                uint32_t lentLr = 0;
                const int depth = LendOn() ? CsLend(section, lentLr) : 0;
                if (depth <= 0) continue;
                lent.push_back({ section, depth, lentLr });
                lentOne = true;
                if (g_loaderLent++ < 5)
                    std::fprintf(stderr, "[rb-ld] loader thread %lu waits for section %08X, which this thread holds (taken at lr %08X): lent until its job is done (fight update %d)\n",
                                 tid, section, lentLr, g_fightRun);
            }
            if (Ms(l0, std::chrono::steady_clock::now()) > 100)
            {
                if (g_loaderTimeouts++ < 10)
                {
                    char line[256];
                    int n = std::snprintf(line, sizeof(line), "[rb-ld] the file loaders still busy after 100 ms (%d running, queues:", g_loaderBusy.load());
                    {
                        std::lock_guard l(g_loaderMutex);
                        for (uint32_t q : g_loaderQueues)
                            if (n < int(sizeof(line)) - 24) n += std::snprintf(line + n, sizeof(line) - n, " %08X=%u", q, GRead32(g_base, q + 16));
                    }
                    std::fprintf(stderr, "%s) - going on (fight update %d%s)\n", line, g_fightRun, g_resimulating ? ", re-simulating" : "");
                }
                break;
            }
            if (!lentOne) SwitchToThread();
        }
        const double ms = Ms(l0, std::chrono::steady_clock::now());
        g_loaderWaitMs += ms;
        if (ms > g_loaderWaitMax) g_loaderWaitMax = ms;
        waited = true;
    }
    for (auto it = lent.rbegin(); it != lent.rend(); ++it) CsReclaim(it->section, it->depth, it->lr);
    if (waited) ++g_quiesceWaits;
    g_quiesceMs += Ms(t0, std::chrono::steady_clock::now());
}

void FinishPoolJobs(PPCContext& ctx, uint8_t* base, const PPCContext& entry)
{
    std::vector<uint32_t> jobs;
    {
        std::lock_guard l(g_jobMutex);
        jobs = g_poolJobs;
    }
    for (uint32_t job : jobs)
    {
        if (!JobObjectLive(job, kPoolJobSize) || GRead32(g_base, job + 144) != 2) continue;
        ctx = entry;
        ctx.r3.u64 = job;
        sub_824F58B8(ctx, base);
    }
    // Each update also queues one item on the job group the draw then dispatches and waits for
    // (MEASURED: pending +8 grew by 1 per re-simulated frame). Same here, with a bounded wait.
    ctx = entry;
    ctx.r3.u64 = kJobGroup;
    sub_82AD06F8(ctx, base);
    const auto t0 = std::chrono::steady_clock::now();
    while (GRead32(g_base, kJobGroup + 8) != 0 && Ms(t0, std::chrono::steady_clock::now()) < 50) SwitchToThread();
    if (GRead32(g_base, kJobGroup + 8) != 0 && g_st.groupStuck++ < 10)
        std::fprintf(stderr, "[rb]   job group still has %u pending after a re-simulated update\n", GRead32(g_base, kJobGroup + 8));
}

// TTT2_RB_CHAOS=1: the sync test as a misprediction. The re-simulated frames get other pad inputs
// (bits flipped), twice from the same snapshot: both runs must end in the same state - nothing left
// outside the rewound state may steer the simulation (sessions re-simulate other inputs than the
// first run had) - and the fight goes on in that timeline.
bool ChaosOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_RB_CHAOS"); return e && *e == '1'; }();
    return on;
}
uint32_t g_perturb = 0;                 // XORed into the replayed pad inputs
Image g_chaos[kRing];                   // the first chaotic run's images after each update
struct ChaosStats { uint64_t tests = 0, failed = 0; } g_chaosSt;

void RunChaos(PPCContext& ctx, uint8_t* base, const PPCContext& entry, uint64_t from)
{
    // Directions only (0x0001 up, 0x0002 down, 0x0004 left, 0x0008 right): 0x0010 / 0x0020 opened
    // Practice's pause menu, which saved its settings mid-rewind (an online match has no pause).
    static const uint32_t kFlips[] = { 0x0003, 0x000C, 0x0005, 0x000A, 0x0009, 0x0006 };
    g_perturb = kFlips[g_chaosSt.tests % 6];
    const int n = int(g_frame - from);
    uint32_t fighterWords = 0, pages = 0;
    int logged = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int pass = 0; pass < 2; ++pass)
    {
        if (!GpuFinishInFlight(50)) std::fprintf(stderr, "[rb-chaos] the GPU did not finish its frames in 50 ms\n");
        QuiesceJobs(base, entry);
        Restore(g_snap[from % kRing]);
        CheckLargeBlockHeaps("after a chaos test's restore");
        CheckHavokFreeLists("after a chaos test's restore");
        FindFighters();
        GpuDiscardNextFrames(uint32_t(n));
        for (uint64_t f = from; f < g_frame; ++f)
        {
            if (f != from) { QuiesceJobs(base, entry); IncorporateLate(); Save(g_snap[f % kRing], f); }
            if (kStackRestore && g_stack[f % kRing].size() == kStackBytes)
                std::memcpy(g_base + entry.r1.u32 - kStackBytes, g_stack[f % kRing].data(), kStackBytes);
            SoundRollbackFrame(f, true);
            Update(ctx, base, entry, f, true);
            IncorporateLate();
            if (pass == 0) Save(g_chaos[f % kRing], f);
            else pages += Compare(g_chaos[f % kRing], f, logged, g_chaosSt.tests < 20, fighterWords);
            RngOut();
            ctx = entry;
            g_resimDraw = true;
            sub_825EBCC8(ctx, base);
            g_resimDraw = false;
            RngIn();
        }
        GpuDiscardEndNow();
    }
    g_perturb = 0;
    ctx = entry;
    SoundRollbackSettle(ctx, base, from, g_frame - 1);
    QuiesceJobs(base, entry);
    IncorporateLate();
    Save(g_snap[g_frame % kRing], g_frame);         // this frame's start, now in the new timeline
    ++g_chaosSt.tests;
    g_chaosSt.failed += fighterWords != 0;
    std::fprintf(stderr, "[rb-chaos] test at frame %llu, %d frames with inputs ^%04X twice: %u fighter words and %u pages differ between the runs, %u input mismatches, %.2f ms; %llu tests, %llu failed\n",
                 (unsigned long long)g_frame, n, kFlips[(g_chaosSt.tests - 1) % 6], fighterWords, pages, g_replayMismatch,
                 Ms(t0, std::chrono::steady_clock::now()), (unsigned long long)g_chaosSt.tests, (unsigned long long)g_chaosSt.failed);
    g_replayMismatch = 0;
}

// The battle state (0x82EAC03C: 4 = a round runs) at each fight frame's start. Sessions rewind only inside
// a round (rollback_session.cpp's RoundPredicts: re-simulating an intro, a KO's end or a round reset broke
// the sound engine and the task lists); the sync test does the same, so it tests what a session does.
// TTT2_RB_TESTSTATES=all: every battle state, as before 2026-10-21.
uint32_t g_battleStateAt[kRing];
bool TestAllStates()
{
    static const bool v = [] { const char* e = std::getenv("TTT2_RB_TESTSTATES"); return e && std::strcmp(e, "all") == 0; }();
    return v;
}
void RunTest(PPCContext& ctx, uint8_t* base, const PPCContext& entry)
{
    const int n = Depth();
    const uint64_t from = g_frame - n;
    if (from < g_barrier) return;
    for (uint64_t f = from; f < g_frame; ++f)
    {
        const int s = int(f % kRing);
        if (g_snap[s].frame != f || g_inputs[s].frame != f || g_post[s].frame != f) return;
        if (!TestAllStates() && g_battleStateAt[s] != 4) return;
    }
    if (!TestAllStates() && GRead32(g_base, 0x82EAC03Cu) != 4) return;
    if (ChaosOn())
    {
        RunChaos(ctx, base, entry, from);
        return;
    }
    const auto tw = std::chrono::steady_clock::now();
    // Frames still in flight read guest memory the restore is about to rewind (the D3D runtime's
    // dynamic-buffer bookkeeping): let the command processor finish them first.
    if (!GpuFinishInFlight(50)) std::fprintf(stderr, "[rb]   the GPU did not finish its frames in 50 ms\n");
    if (g_st.tests < 3) DumpJobWords("job words before the rewind");
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t copied = Restore(g_snap[from % kRing]);
    CheckLargeBlockHeaps("after a sync test's restore");
    CheckHavokFreeLists("after a sync test's restore");
    if (g_st.tests < 3) DumpJobWords("job words after the restore");
    const auto t1 = std::chrono::steady_clock::now();
    uint32_t diffs = 0, fighterWords = 0;
    int logged = 0;
    FindFighters();
    if (g_st.tests < 3)
        std::fprintf(stderr, "[rb]   discard window from marker %u (written %u, CP done %u)\n", g_markerSwaps.load() + 1, g_markerSwaps.load(), GpuCpMarkers());
    GpuDiscardNextFrames(uint32_t(n));
    for (uint64_t f = from; f < g_frame; ++f)
    {
        if (f != from) { QuiesceJobs(base, entry); IncorporateLate(); Save(g_snap[f % kRing], f); }   // the re-simulated timeline's snapshots
        if (kStackRestore && g_stack[f % kRing].size() == kStackBytes)
            std::memcpy(g_base + entry.r1.u32 - kStackBytes, g_stack[f % kRing].data(), kStackBytes);
        const uint32_t swaps0 = g_markerSwaps.load(), cp0 = GpuCpMarkers();
        SoundRollbackFrame(f, true);
        Update(ctx, base, entry, f, true);
        const uint32_t swaps1 = g_markerSwaps.load(), cp1 = GpuCpMarkers();
        diffs += Compare(g_post[f % kRing], f, logged, g_st.tests < 40, fighterWords);
        // The draw too: the update and the draw are one pipeline (the draw finishes the pool jobs the
        // update started, its frame kick completes the job group's work; re-simulating updates alone
        // hung it). The command processor discards these frames' GPU work.
        RngOut();
        ctx = entry;
        g_resimDraw = true;
        const auto d0 = std::chrono::steady_clock::now();
        sub_825EBCC8(ctx, base);
        const auto d1 = std::chrono::steady_clock::now();
        g_resimDraw = false;
        RngIn();
        if (g_st.tests < 3)
            std::fprintf(stderr, "[rb]   frame %llu markers: written %u -> %u (update) -> %u (draw); CP done %u -> %u -> %u\n",
                         (unsigned long long)f, swaps0, swaps1, g_markerSwaps.load(), cp0, cp1, GpuCpMarkers());
        if (g_st.tests < 6)
            std::fprintf(stderr, "[rb]   re-simulated frame %llu: draw %.2f ms (%u throttle waits skipped); group wait %.2f, job finish %.2f, dispatch %.2f, frame kick %.2f, submit %.2f, VdSwap %.2f ms; in the kick: 1DE8 %.2f 2CC0 %.2f 4D98 %.2f 1AD0 %.2f 43E0 %.2f lock %.2f, GPU fence %.2f ms\n",
                         (unsigned long long)f, Ms(d0, d1), g_throttleSkips, g_resimTimes[0], g_resimTimes[1], g_resimTimes[2],
                         g_resimTimes[3], g_resimTimes[4], g_resimTimes[5], g_resimTimes[6], g_resimTimes[7], g_resimTimes[8],
                         g_resimTimes[9], g_resimTimes[10], g_resimTimes[11], g_resimTimes[12]);
        g_throttleSkips = 0;
        std::fill(std::begin(g_resimTimes), std::end(g_resimTimes), 0.0);

    }
    const auto t2 = std::chrono::steady_clock::now();
    if (g_st.tests < 3) DumpJobWords("job words after the re-simulation");
    ctx = entry;
    SoundRollbackSettle(ctx, base, from, g_frame - 1);
    QuiesceJobs(base, entry);
    IncorporateLate();
    Save(g_snap[g_frame % kRing], g_frame);         // this frame's start, now in the re-simulated timeline
    ++g_st.tests;
    g_st.failed += fighterWords != 0;
    // D3D's flip queue (tail - head) and its last entry's vblank vs. now: re-simulated frames must
    // leave neither (kernel_video.cpp, sub_82740610).
    const uint32_t dev = VdDeviceGuest();
    const uint32_t flips = dev ? GRead32(base, dev + 17112) - GRead32(base, dev + 17108) : 0;
    const int32_t ahead = dev ? int32_t(GRead32(base, dev + 16960) - GRead32(base, dev + 16952)) : 0;
    std::fprintf(stderr, "[rb] test at frame %llu, %d frames: GPU drain %.2f ms, restore %.2f ms (%u pages), re-sim %.2f ms (%.2f/frame), %u fighter words and %u pages differ, %u input mismatches, flip queue %u (%+d vblanks), GPU fence waits %.2f ms\n",
                 (unsigned long long)g_frame, n, Ms(tw, t0), Ms(t0, t1), copied, Ms(t1, t2), Ms(t1, t2) / n, fighterWords, diffs, g_replayMismatch,
                 flips, ahead, g_fenceWaitMs);
    g_replayMismatch = 0;
    g_fenceWaitMs = 0;
}

// ---- online rollback sessions (rollback_session.cpp drives NACE) -----------------------------------
// Snapshots are keyed by record frame (the NACE record an update consumes); an update that gets no
// record (the battle stalls) takes none. A misprediction found by the frame pump means: the record
// now loaded is the real one of the oldest predicted frame P; the state goes back to P's snapshot and
// the frames P..last are simulated again with NACE's corrected records, then the live frame runs.
struct PumpPage { uint32_t page; std::vector<uint8_t> before; };
std::vector<PumpPage> g_pumpPages;     // rewound pages NACE's pump writes (this fight)
uint64_t g_pumpMasked = 0;             // words masked as the pump's
uint64_t g_pumpOpened = 0;             // cold pages the pump opened (joined the rewound set at once)
struct OnlinePage { uint32_t page; std::vector<uint8_t> before; bool primed; };
std::vector<OnlinePage> g_onlinePages;  // rewound pages the online tasks write (RollbackOnlineStep)
std::vector<uint32_t> g_onlineScratch;
uint64_t g_onlineMasked = 0;           // words masked as the online tasks'
// Diagnostics: the first words masked as the pump's (address, before -> after; big-endian words).
void NotePumpWord(uint32_t a, const uint8_t* before, const char* how)
{
    static int logged = 0;
    if (logged++ >= 48) return;
    auto be = [](const uint8_t* b) { return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; };
    std::fprintf(stderr, "[rb-session] pump word %08X (%s): %08X %08X -> %08X %08X\n", a, how,
                 be(before), be(before + 4), GRead32(g_base, a), GRead32(g_base, a + 4));
}
bool g_inPump = false;                 // the session runs NACE's pump / record fetch (heap diagnostics)
uint64_t g_pumpHeapOps = 0;            // game heap calls made meanwhile (NACE's, never rewound)
struct Session
{
    bool on = false;
    int32_t last = -1;                  // the newest record frame consumed
    uint64_t updates = 0, consumed = 0, stalls = 0, rewinds = 0, framesBack = 0, refused = 0, yields = 0, dropped = 0, undrained = 0;
    int maxDepth = 0;
    double rewindMs = 0, maxRewindMs = 0;
} g_sess;
int32_t g_sessFrame = -1;              // the record frame this update consumes (-1: the battle stalls)
double g_rewindPhase[9] = {};          // rewind time by phase over the session (SessionRewind), ms
uint64_t g_interval[4] = {};           // session updates; intervals over 20 / 30 / 50 ms (SessionStep)
double g_intervalMax = 0;
// A rewind waits for the frame the command processor has not finished (GpuFinishInFlight) - its draws,
// ~4.3 ms here (MEASURED, timeline: the CP starts a frame only after the previous flip's vblank, so
// a rewind at the next update finds the whole frame queued). Waiting then restoring, re-simulating and
// running the live frame must fit a refresh, or the corrected frame comes a refresh late. When the
// estimate does not fit (an older CPU, a deep rewind), the in-flight frame is dropped instead: its
// packets run without draws (fast) and the screen keeps the previous frame for one refresh - the same
// one-refresh cost, without the wait pushing later frames too. Estimate: the CP's work per frame +
// ~0.6 ms restore + depth x the re-simulated frame cost + the main thread's work per frame (averages
// measured on this PC) against 14.5 ms. MEASURED (forced on, drop65): in sync, but the wait only went
// from ~4.4 to ~3.4 ms - most of it is the game's render thread still building the frame's command
// buffers (it runs into the next update), which a drop does not shorten - for a frame not shown. So
// it is off: TTT2_RB_DROP=1 always (tests), auto = the estimate above, default never.
double g_resimFrameMs = 2.6;           // re-simulated frame (update + draw), average
double g_mainFrameMs = 3.0;            // main thread's CPU per live frame, average
bool RewindDropsInFlight(int depth)
{
    static const int mode = [] { const char* e = std::getenv("TTT2_RB_DROP"); return !e ? 0 : *e == '1' ? 1 : std::strcmp(e, "auto") == 0 ? 2 : 0; }();
    if (mode != 2) return mode == 1;
    return GpuCpFrameMs() + 0.6 + depth * g_resimFrameMs + g_mainFrameMs > 14.5;
}
// The main thread's CPU per live frame: its cycle counter between session updates without a rewind.
void NoteMainFrameWork(bool rewound)
{
    static uint64_t lastCycles = 0, lastTsc = 0;
    static double tscPerMs = 0;
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    const uint64_t tsc = __rdtsc();
    static const auto qpc0 = std::chrono::steady_clock::now();
    static const uint64_t tsc0 = tsc;
    const double since = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - qpc0).count();
    if (since > 200) tscPerMs = double(tsc - tsc0) / since;
    if (lastCycles && tscPerMs > 0 && !rewound)
    {
        const double ms = double(c - lastCycles) / tscPerMs;
        if (ms < 50) g_mainFrameMs = g_mainFrameMs * 0.95 + ms * 0.05;
    }
    lastCycles = c;
    lastTsc = tsc;
}

// What a session update did (1 yield, 2 rewind, 4 stall), and the intervals over 20 ms by what the
// update they timed did: a rewind, a yield, a stall, none of these.
uint32_t g_stepFlags = 0;
uint64_t g_longBy[4] = {};

// ---- slow frames (sessions): where a long frame's time went -----------------------------------------
// From one live update start to the next: the frame start (job waits, write watch), the session step
// (NACE's pump, a rewind, the snapshot), the game's update, the draw without its vblank waits, the frame
// kick's vblank waits, and the rest of the frame loop (the online manager's update and the like). A frame
// over 25 ms is logged with its parts (a session's first 40, then each 50th), and every 1200 frames the
// slow frames' average parts: the next real session shows where its stutter comes from. MEASURED
// 2026-10-21 (two-PC LAN session): ~700 frames over 20 ms in one side's match with no rewind, yield or
// stall to blame, and the existing counters could not say why.
using Clock = std::chrono::steady_clock;
struct FrameProf
{
    Clock::time_point upd0, step0, step1, upd1, draw0, draw1;
    double vblankMs = 0;
    int depth = 0;                         // the rewind's frames, if the step rewound
    bool on = false;                       // a session update with all marks set
};
FrameProf g_fp;
struct SlowFrames { uint64_t frames = 0, slow = 0, logged = 0; double parts[7] = {}; } g_slowFrames;
const char* const kSlowPart[7] = { "start", "step", "update", "between", "draw", "vblank waits", "rest" };
// The update hook's entry: the frame before it ends here.
void FrameProfNext(Clock::time_point now)
{
    const FrameProf f = g_fp;
    g_fp = FrameProf{};
    g_fp.upd0 = now;
    if (!f.on || f.draw1 < f.draw0 || f.draw0 < f.upd1) return;
    const double total = Ms(f.upd0, now);
    const double parts[7] = { Ms(f.upd0, f.step0), Ms(f.step0, f.step1), Ms(f.step1, f.upd1), Ms(f.upd1, f.draw0),
                              (std::max)(0.0, Ms(f.draw0, f.draw1) - f.vblankMs), f.vblankMs, Ms(f.draw1, now) };
    ++g_slowFrames.frames;
    if (total <= 25.0) return;
    ++g_slowFrames.slow;
    for (int i = 0; i < 7; ++i) g_slowFrames.parts[i] += parts[i];
    if (g_slowFrames.logged++ < 40 || g_slowFrames.slow % 50 == 0)
    {
        char line[400];
        int n = std::snprintf(line, sizeof(line), "[rb-slow] frame %d: %.1f ms =", g_sess.last, total);
        for (int i = 0; i < 7; ++i) n += std::snprintf(line + n, sizeof(line) - n, "%s %s %.1f", i ? "," : "", kSlowPart[i], parts[i]);
        if (f.depth) n += std::snprintf(line + n, sizeof(line) - n, " (the step rewound %d frames)", f.depth);
        if (g_stepFlags & 1) n += std::snprintf(line + n, sizeof(line) - n, " (a time-sync yield)");
        if (g_stepFlags & 4) std::snprintf(line + n, sizeof(line) - n, " (a stall)");
        std::fprintf(stderr, "%s\n", line);
    }
}
void SlowFramesReport()
{
    if (!g_slowFrames.frames) return;
    char line[400];
    int n = std::snprintf(line, sizeof(line), "[rb-slow] %llu frames, %llu over 25 ms", (unsigned long long)g_slowFrames.frames,
                          (unsigned long long)g_slowFrames.slow);
    if (g_slowFrames.slow)
    {
        n += std::snprintf(line + n, sizeof(line) - n, "; their average parts:");
        for (int i = 0; i < 7; ++i)
            n += std::snprintf(line + n, sizeof(line) - n, "%s %s %.1f", i ? "," : "", kSlowPart[i], g_slowFrames.parts[i] / double(g_slowFrames.slow));
    }
    std::fprintf(stderr, "%s\n", line);
    const uint64_t logged = g_slowFrames.logged;
    g_slowFrames = SlowFrames{};
    g_slowFrames.logged = logged;                  // the per-frame lines stay rationed for the session
}
int32_t g_sessBarrier = 0;             // no rewind to before this record frame (I/O happened later)

void SessionReport(const char* why)
{
    std::fprintf(stderr, "[rb-session] %s: %llu updates, %llu frames, %llu stalls; %llu rewinds (%.2f frames on average, at most %d), %.2f ms on average, at most %.2f ms; %llu refused; %llu heap calls in NACE's pump; %llu words masked as the pump's, %llu pages it opened; %llu words masked as the online tasks'\n",
                 why, (unsigned long long)g_sess.updates, (unsigned long long)g_sess.consumed, (unsigned long long)g_sess.stalls,
                 (unsigned long long)g_sess.rewinds, g_sess.rewinds ? double(g_sess.framesBack) / double(g_sess.rewinds) : 0.0,
                 g_sess.maxDepth, g_sess.rewinds ? g_sess.rewindMs / double(g_sess.rewinds) : 0.0, g_sess.maxRewindMs,
                 (unsigned long long)g_sess.refused, (unsigned long long)g_pumpHeapOps, (unsigned long long)g_pumpMasked, (unsigned long long)g_pumpOpened,
                 (unsigned long long)g_onlineMasked);
    std::fprintf(stderr, "[rb-session] time sync: %llu frames yielded, %.1f frames given up in all\n", (unsigned long long)g_sess.yields,
                 SessionTimeSyncGiven());
    std::fprintf(stderr, "[rb-session] rewinds that dropped the frame in flight: %llu (renderer %.2f ms per frame, re-simulation %.2f ms per frame, "
                 "main thread %.2f ms per frame)\n", (unsigned long long)g_sess.dropped, GpuCpFrameMs(), g_resimFrameMs, g_mainFrameMs);
    {
        uint64_t allocs = 0, bytes = 0, refusedAllocs = 0;
        ResimScratchStats(allocs, bytes, refusedAllocs);
        std::fprintf(stderr, "[rb-session] rewinds without waiting for the GPU: %llu of %llu (scratch: %llu skinned-vertex allocations, %.1f MB, %llu refused)\n",
                     (unsigned long long)g_sess.undrained, (unsigned long long)g_sess.rewinds, (unsigned long long)allocs,
                     double(bytes) / 1048576.0, (unsigned long long)refusedAllocs);
    }
    std::fprintf(stderr, "[rb-session] update intervals: %llu, over 20 ms %llu, over 30 ms %llu, over 50 ms %llu, longest %.1f ms\n",
                 (unsigned long long)g_interval[0], (unsigned long long)g_interval[1], (unsigned long long)g_interval[2],
                 (unsigned long long)g_interval[3], g_intervalMax);
    std::fprintf(stderr, "[rb-session] intervals over 20 ms after: a rewind %llu, a yield %llu, a stall %llu, none of these %llu\n",
                 (unsigned long long)g_longBy[0], (unsigned long long)g_longBy[1], (unsigned long long)g_longBy[2], (unsigned long long)g_longBy[3]);
    if (g_sess.rewinds)
        std::fprintf(stderr, "[rb-session] rewind time by phase (ms per rewind): gpu %.2f, restore %.2f, pump %.2f, quiesce %.2f, late %.2f, "
                     "snapshot %.2f, update %.2f, draw %.2f, sound %.2f\n", g_rewindPhase[0] / g_sess.rewinds, g_rewindPhase[1] / g_sess.rewinds,
                     g_rewindPhase[2] / g_sess.rewinds, g_rewindPhase[3] / g_sess.rewinds, g_rewindPhase[4] / g_sess.rewinds,
                     g_rewindPhase[5] / g_sess.rewinds, g_rewindPhase[6] / g_sess.rewinds, g_rewindPhase[7] / g_sess.rewinds,
                     g_rewindPhase[8] / g_sess.rewinds);
}

// A real record must be for the frame the session counted to.
void CheckRealFrame(int32_t frame)
{
    const int32_t real = SessionRealFrame();
    static int logged = 0;
    if (real >= 0 && real != frame && logged++ < 20)
        std::fprintf(stderr, "[rb-session] counted frame %d but the real record is for %d\n", frame, real);
}

// The NACE record each consumed frame ran with (nace+5976: frame0, frame1, in0:in1, 16 bytes; ready
// +6020): the session self-test re-simulates with exactly these.
constexpr uint32_t kNaceRecord = 0x82E6C5C8u + 5976, kNaceReady = 0x82E6C5C8u + 6020;
struct SessionRecord { int32_t frame = -1; uint8_t rec[16] = {}; uint8_t ready = 0; };
SessionRecord g_rec[kRing];
void KeepRecord(int32_t frame)
{
    SessionRecord& r = g_rec[frame % kRing];
    r.frame = frame;
    std::memcpy(r.rec, g_base + kNaceRecord, 16);
    r.ready = GRead8(g_base, kNaceReady);
}
// NACE keeps the previous record right after the current one (nace+5988..5999, copied by the record
// fetch sub_826F0B30); the game's online input reads take "newly pressed" as current & ~previous
// (sub_82274688 -> sub_82664CD8 / sub_82664CE8: e.g. the win-pose skip, sub_822D7198). The previous
// record must be the one frame k-1 ran with in this timeline - after a rewind or with a record written
// by the session NACE's copy is another timeline's (MEASURED: the next round started on different
// frames in a re-simulation and live, the health refill sub_822FB920 one to three frames apart).
constexpr uint32_t kNacePrevious = 0x82E6C5C8u + 5988;
void SetPrevious(int32_t frame)
{
    const SessionRecord& r = g_rec[(frame - 1 + kRing) % kRing];
    if (frame < 1 || r.frame != frame - 1) return;
    std::memcpy(g_base + kNacePrevious, r.rec, 12);
}

// Back to P's snapshot, P..last simulated again; `frame` comes back as the live update's record frame.
// A save in progress (RollbackNoteIo) is finished before any session restore - a rewind in the middle of
// one broke it - and saves of frames from p on are undone on disk (save_store.cpp's journal), so the
// re-simulated frames save into exactly the files the restored memory expects. MEASURED 2026-10-21: the
// save stayed open behind the game's "Save Error" box, the 1 s wait ran out, the restore went ahead in the
// middle of it and the heap's free list closed into a cycle (the next match froze). The wait is 3 s now.
void SessionSaveBeforeRestore(int32_t p, const char* what)
{
    if (SaveInProgress())
    {
        const auto w0 = std::chrono::steady_clock::now();
        while (SaveInProgress() && std::chrono::steady_clock::now() - w0 < std::chrono::milliseconds(3000)) Sleep(1);
        std::fprintf(stderr, "[rb-session] %s to frame %d waited %.1f ms for a save to finish%s\n", what, p,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count(),
                     SaveInProgress() ? " - still open, going on anyway" : "");
    }
    SaveJournalUndo(p);
}

void SessionRewind(PPCContext& ctx, uint8_t* base, const PPCContext& entry, int32_t p, int32_t& frame)
{
    const int32_t last = g_sess.last;
    const int depth = last - p + 1;
    if (depth < 1 || depth >= kRing || p < g_sessBarrier || g_snap[p % kRing].frame != uint64_t(p))
    {
        // Nothing to rewind to: the fight goes on from the wrong timeline (NACE's desync check will
        // tell). Never expected; counted and logged.
        ++g_sess.refused;
        std::fprintf(stderr, "[rb-session] cannot rewind to frame %d (last %d, barrier %d, snapshot %lld)\n", p, last, g_sessBarrier,
                     (long long)g_snap[p % kRing].frame);
        return;
    }
    SessionSaveBeforeRestore(p, "rewind");
    const auto t0 = std::chrono::steady_clock::now();
    // Phase times (ms): GPU finish, restore, pump, quiesce, late pages, snapshot, update, draw, sound.
    double ph[9] = {};
    auto lap = [](std::chrono::steady_clock::time_point& t) {
        const auto n = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(n - t).count();
        t = n;
        return ms;
    };
    auto tp = t0;
    // With the re-simulation's skinned vertices in scratch memory (resim_scratch.cpp) nothing the frame
    // still being drawn reads changes under it: no wait for the GPU. TTT2_RB_DRAIN=1 waits anyway (A/B).
    static const bool forceDrain = [] { const char* e = std::getenv("TTT2_RB_DRAIN"); return e && *e == '1'; }();
    const bool drop = RewindDropsInFlight(depth);
    const bool drain = forceDrain || drop || !ResimScratchOn();
    if (drain && !GpuFinishInFlight(50, drop)) std::fprintf(stderr, "[rb-session] the GPU did not finish its frames in 50 ms\n");
    static const bool unpace = [] { const char* e = std::getenv("TTT2_RB_UNPACE"); return !(e && *e == '0'); }();
    if (!drain && unpace) GpuUnpaceInFlight();
    if (!drain) ++g_sess.undrained;
    ph[0] += lap(tp);
    GpuMemRewindBegin();
    if (drop) ++g_sess.dropped;
    QuiesceJobs(base, entry);
    CheckLargeBlockHeaps("before a session rewind's restore");
    CheckHavokFreeLists("before a session rewind's restore");
    // Pages opened since the last snapshot join first (their earlier content into the snapshot):
    // a pending page is not restored - it would stay as the newest frame left it while the objects
    // around it go back (MEASURED: list nodes there, cyclic lists in the sound manager and an
    // effect library after rewinds).
    IncorporateLate();
    Restore(g_snap[p % kRing]);
    CheckLargeBlockHeaps("after a session rewind's restore");
    CheckHavokFreeLists("after a session rewind's restore");
    GpuDiscardNextFrames(uint32_t(depth));
    ph[1] += lap(tp);
    GpuMemRewindCheck(0);
    int run = 0;
    bool more = true;
    g_resimulating = true;
    ResimScratchBegin();
    for (int32_t k = p; k <= last; ++k)
    {
        if (k != p)
        {
            g_inPump = true;
            const bool got = SessionNext(ctx, base);
            g_inPump = false;
            ph[2] += lap(tp);
            if (!got)
            {
                std::fprintf(stderr, "[rb-session] no record for frame %d while re-simulating - stopped\n", k);
                more = false;
                break;
            }
            CheckRealFrame(k);
            QuiesceJobs(base, entry);
            ph[3] += lap(tp);
            IncorporateLate();
            ph[4] += lap(tp);
            Save(g_snap[k % kRing], uint64_t(k));
            ph[5] += lap(tp);
            SessionStateTrace(k);
        }
        KeepRecord(k);
        SetPrevious(k);
        g_sessFrame = k;
        if (kStackRestore && g_stack[k % kRing].size() == kStackBytes)
            std::memcpy(g_base + entry.r1.u32 - kStackBytes, g_stack[k % kRing].data(), kStackBytes);
        SoundRollbackFrame(uint64_t(k), true);
        lap(tp);
        Update(ctx, base, entry, uint64_t(k), true, false);
        ph[6] += lap(tp);
        if (k == p) GpuMemRewindCheck(1);
        RngOut();
        ctx = entry;
        g_resimDraw = true;
        sub_825EBCC8(ctx, base);
        g_resimDraw = false;
        RngIn();
        ph[7] += lap(tp);
        ++run;
    }
    g_resimulating = false;
    ResimScratchEnd();
    GpuMemRewindEnd();
    GpuDiscardEndNow();
    SoundRollbackFrame(~0ull, false);
    ctx = entry;
    if (run > 0) SoundRollbackSettle(ctx, base, uint64_t(p), uint64_t(p + run - 1));
    ph[8] += lap(tp);
    // The live update's record (none when the re-simulation stopped early: this update stalls).
    g_inPump = true;
    frame = more && SessionNext(ctx, base) ? last + 1 : -1;
    g_inPump = false;
    if (!more) g_sess.last = p + run - 1;
    const double ms = Ms(t0, std::chrono::steady_clock::now());
    ++g_sess.rewinds;
    g_sess.framesBack += uint64_t(run);
    g_sess.maxDepth = (std::max)(g_sess.maxDepth, depth);
    g_sess.rewindMs += ms;
    g_sess.maxRewindMs = (std::max)(g_sess.maxRewindMs, ms);
    for (int i = 0; i < 9; ++i) g_rewindPhase[i] += ph[i];
    if (run > 0) g_resimFrameMs = g_resimFrameMs * 0.9 + (ph[6] + ph[7]) / run * 0.1;
    static int logged = 0;
    if (logged++ < 30 || ms > 12.0)
        std::fprintf(stderr, "[rb-session] rewind to frame %d: %d frames again in %.2f ms (gpu %.2f, restore %.2f, pump %.2f, quiesce %.2f, "
                     "late %.2f, snapshot %.2f, update %.2f, draw %.2f, sound %.2f); live frame %d\n", p, run, ms,
                     ph[0], ph[1], ph[2], ph[3], ph[4], ph[5], ph[6], ph[7], ph[8], frame);
}

// The round flow's "all players ready" (rollback_session.cpp's FlowTrace has the protocol): the
// battle waits until NACE reports every player ready, and NACE reports it once the *confirmed* frame
// reaches the host's go frame F (S+5092). In lockstep that is the frame being simulated; a session
// simulates ahead, so its frames F..last ran "not ready" - each instance would go on at F + its own
// prediction depth (MEASURED: self-test failures at the go frames of both round transitions, the
// instances 1-2 frames apart from there). When NACE applies a go frame the session has passed, the
// frames from F are simulated again with the records they ran with (predicted ones are checked by NACE
// as usual later), now seeing "ready" from F exactly as lockstep would.
constexpr uint32_t kSyncGoApplied = 0x82E6C5C8u + 64 + 5092;
int32_t g_goSeen = -1;            // the newest go frame NACE applied, as seen by the session
uint64_t g_goReplays = 0;
void SessionReplay(PPCContext& ctx, uint8_t* base, const PPCContext& entry, int32_t p, const char* why)
{
    const int32_t last = g_sess.last;
    const int depth = last - p + 1;
    bool ok = depth >= 1 && depth < kRing && p >= g_sessBarrier;
    for (int32_t k = p; ok && k <= last; ++k)
        ok = g_rec[k % kRing].frame == k && (k != p || g_snap[k % kRing].frame == uint64_t(k));
    if (!ok)
    {
        ++g_sess.refused;
        std::fprintf(stderr, "[rb-session] cannot simulate frames %d..%d again (%s): no snapshot / records\n", p, last, why);
        return;
    }
    SessionSaveBeforeRestore(p, "replay");
    const auto t0 = std::chrono::steady_clock::now();
    if (!GpuFinishInFlight(50)) std::fprintf(stderr, "[rb-session] the GPU did not finish its frames in 50 ms\n");
    QuiesceJobs(base, entry);
    IncorporateLate();
    uint8_t liveRec[24];                                // current + previous record
    std::memcpy(liveRec, g_base + kNaceRecord, 24);
    const uint8_t liveReady = GRead8(g_base, kNaceReady);
    Restore(g_snap[p % kRing]);
    GpuDiscardNextFrames(uint32_t(depth));
    g_resimulating = true;
    for (int32_t k = p; k <= last; ++k)
    {
        if (k != p)
        {
            QuiesceJobs(base, entry);
            IncorporateLate();
            Save(g_snap[k % kRing], uint64_t(k));
            SessionStateTrace(k);
        }
        std::memcpy(g_base + kNaceRecord, g_rec[k % kRing].rec, 12);
        SetPrevious(k);
        GWrite8(g_base, kNaceReady, g_rec[k % kRing].ready);
        g_sessFrame = k;
        if (kStackRestore && g_stack[k % kRing].size() == kStackBytes)
            std::memcpy(g_base + entry.r1.u32 - kStackBytes, g_stack[k % kRing].data(), kStackBytes);
        SoundRollbackFrame(uint64_t(k), true);
        Update(ctx, base, entry, uint64_t(k), true, false);
        RngOut();
        ctx = entry;
        g_resimDraw = true;
        sub_825EBCC8(ctx, base);
        g_resimDraw = false;
        RngIn();
    }
    g_resimulating = false;
    GpuDiscardEndNow();
    SoundRollbackFrame(~0ull, false);
    ctx = entry;
    SoundRollbackSettle(ctx, base, uint64_t(p), uint64_t(last));
    std::memcpy(g_base + kNaceRecord, liveRec, 24);
    GWrite8(g_base, kNaceReady, liveReady);
    ++g_goReplays;
    std::fprintf(stderr, "[rb-session] frames %d..%d again (%s) in %.2f ms\n", p, last, why, Ms(t0, std::chrono::steady_clock::now()));
}

// TTT2_RB_SESSTEST=<every> (diagnostics): the session's own sync test. Every <every> consumed frames
// (with no misprediction this update), the last kSelfTestDepth frames are re-simulated from their
// snapshot with the records they ran with, the result compared word by word with the live state they
// produced (the masked words aside), then the live state is put back exactly (every hot page, the
// pages the test opened, NACE's record). Any difference is re-simulation that depends on something a
// rewind does not restore - in the online setting (NACE's live state, the online tasks, ...).
constexpr int kSelfTestDepth = 3;
Image g_testNow;
struct SelfTestStats { uint64_t tests = 0, failed = 0; } g_selfTest;
int SelfTestEvery()
{
    static const int every = [] { const char* e = std::getenv("TTT2_RB_SESSTEST"); return e ? std::atoi(e) : 0; }();
    return every;
}
void SessionSelfTest(PPCContext& ctx, uint8_t* base, const PPCContext& entry)
{
    const int32_t last = g_sess.last, p = last - kSelfTestDepth + 1;
    if (p < g_sessBarrier) return;
    for (int32_t k = p; k <= last; ++k)
        if (g_snap[k % kRing].frame != uint64_t(k) || g_rec[k % kRing].frame != k) return;
    const auto t0 = std::chrono::steady_clock::now();
    if (!GpuFinishInFlight(50)) std::fprintf(stderr, "[rb-selftest] the GPU did not finish its frames in 50 ms\n");
    QuiesceJobs(base, entry);
    IncorporateLate();
    // The live state (put back like a rewind would: masked words stay as they are - the pool jobs'
    // words go with their threads' host-side locks; MEASURED: putting them back raw left a job
    // "running" with its thread idle and the next kick spun).
    Save(g_testNow, ~0ull);
    uint8_t liveRec[24];                                // current + previous record
    std::memcpy(liveRec, g_base + kNaceRecord, 24);
    const uint8_t liveReady = GRead8(g_base, kNaceReady);
    const int32_t liveFrame = g_sessFrame;
    Restore(g_snap[p % kRing]);
    GpuDiscardNextFrames(uint32_t(kSelfTestDepth));
    g_resimulating = true;
    for (int32_t k = p; k <= last; ++k)
    {
        if (k != p) QuiesceJobs(base, entry);
        std::memcpy(g_base + kNaceRecord, g_rec[k % kRing].rec, 12);
        SetPrevious(k);
        GWrite8(g_base, kNaceReady, g_rec[k % kRing].ready);
        g_sessFrame = k;
        if (kStackRestore && g_stack[k % kRing].size() == kStackBytes)
            std::memcpy(g_base + entry.r1.u32 - kStackBytes, g_stack[k % kRing].data(), kStackBytes);
        SoundRollbackFrame(uint64_t(k), true);
        Update(ctx, base, entry, uint64_t(k), true, false);
        RngOut();
        ctx = entry;
        g_resimDraw = true;
        sub_825EBCC8(ctx, base);
        g_resimDraw = false;
        RngIn();
    }
    g_resimulating = false;
    GpuDiscardEndNow();
    SoundRollbackFrame(~0ull, false);
    ctx = entry;
    SoundRollbackSettle(ctx, base, uint64_t(p), uint64_t(last));
    QuiesceJobs(base, entry);
    // Compare (rewound words only). The draws are discarded in the re-simulation, so render-side
    // memory differs every time; what counts is the fight: the fighter objects. A test with fighter
    // differences prints them, then the first word of each other differing page.
    uint32_t pages = 0, words = 0, fighterWords = 0;
    struct Diff { uint32_t addr, live, resim; int fighter; uint32_t off; };
    std::vector<Diff> fighterDiffs, pageFirsts;
    for (size_t i = 0; i < g_testNow.pages && i < g_hot.size(); ++i)
    {
        const uint8_t* now = g_base + g_hot[i];
        const uint8_t* was = g_testNow.data.data() + i * kPage;
        if (std::memcmp(now, was, kPage) == 0) continue;
        uint32_t pageWords = 0;
        for (uint32_t k = 0; k < kPage; k += 4)
        {
            if (std::memcmp(now + k, was + k, 4) == 0 || Outside(i, k / 8)) continue;
            const Diff d{ g_hot[i] + k, _byteswap_ulong(*reinterpret_cast<const uint32_t*>(was + k)),
                          _byteswap_ulong(*reinterpret_cast<const uint32_t*>(now + k)), -1, 0 };
            uint32_t off = 0;
            const int f = FighterOf(d.addr, off);
            if (f >= 0) { ++fighterWords; if (fighterDiffs.size() < 64) fighterDiffs.push_back({ d.addr, d.live, d.resim, f, off }); }
            else if (pageWords == 0 && pageFirsts.size() < 400) pageFirsts.push_back(d);
            ++pageWords;
        }
        if (pageWords) { ++pages; words += pageWords; }
    }
    static int detailed = 0;
    if (fighterWords && detailed++ < 6)
    {
        for (const Diff& d : fighterDiffs)
            std::fprintf(stderr, "[rb-selftest]   frame %d fighter %d +%04X: live %08X resim %08X\n", last, d.fighter, d.off, d.live, d.resim);
        for (const Diff& d : pageFirsts)
            std::fprintf(stderr, "[rb-selftest]   frame %d page %08X first word %08X: live %08X resim %08X\n", last, d.addr & ~(kPage - 1), d.addr, d.live, d.resim);
    }
    // Put the live state back: pages the test opened first (their content from before), then the
    // hot pages, then NACE's record.
    {
        std::vector<LatePage> opened;
        AcquireSRWLockExclusive(&g_cowLock);
        opened.swap(g_pendingLate);
        ReleaseSRWLockExclusive(&g_cowLock);
        for (LatePage& lp : opened) std::memcpy(g_base + lp.page, lp.before.get(), kPage);
        AcquireSRWLockExclusive(&g_cowLock);
        for (LatePage& lp : opened) g_pendingLate.push_back(std::move(lp));
        ReleaseSRWLockExclusive(&g_cowLock);
        IncorporateLate();
    }
    Restore(g_testNow);
    std::memcpy(g_base + kNaceRecord, liveRec, 24);
    GWrite8(g_base, kNaceReady, liveReady);
    g_sessFrame = liveFrame;
    ++g_selfTest.tests;
    g_selfTest.failed += fighterWords != 0;
    if (fighterWords || g_selfTest.tests % 100 == 1)
        std::fprintf(stderr, "[rb-selftest] frames %d..%d again: %u words on %u pages differ (%u fighter words), %.2f ms; %llu tests, %llu failed\n",
                     p, last, words, pages, fighterWords, Ms(t0, std::chrono::steady_clock::now()),
                     (unsigned long long)g_selfTest.tests, (unsigned long long)g_selfTest.failed);
}

// rollback.cpp's update hook, once calibrated: true when an online session runs this update (its
// frame pump done, a rewind done if NACE found a misprediction, the snapshot of this update's record
// frame taken).
bool SessionStep(PPCContext& ctx, uint8_t* base, const PPCContext& entry)
{
    g_sessFrame = -1;
    // Smoothness: the time between update starts while a session runs (16.7 ms at 60 fps; a frame
    // the rewind made late shows as ~33 ms), counted for the session report.
    {
        static auto lastStart = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - lastStart).count();
        lastStart = now;
        const uint32_t before = g_stepFlags;
        g_stepFlags = 0;
        if (g_sess.on) NoteMainFrameWork((before & 2) != 0);
        if (g_sess.on)
        {
            ++g_interval[0];
            if (ms > 20.0) ++g_longBy[(before & 2) ? 0 : (before & 1) ? 1 : (before & 4) ? 2 : 3];
            if (ms > 20.0) ++g_interval[1];
            if (ms > 30.0) ++g_interval[2];
            if (ms > 50.0) ++g_interval[3];
            g_intervalMax = (std::max)(g_intervalMax, ms);
        }
    }
    const bool want = SessionEnabled() && SessionSyncRunning() && !(SessionEnding() && SessionPending() == 0);
    if (!want)
    {
        if (g_sess.on)
        {
            SessionOwnPump(false);
            g_sess.on = false;
            SaveJournalClear();                       // no rewind any more: every save stands
            SessionReport("session over");
            XSessionReportMatch(false);
            SlowFramesReport();
            g_slowFrames = SlowFrames{};
        }
        return false;
    }
    if (g_sess.on) SaveJournalPrune(SessionConfirmedFrame());   // confirmed frames are never re-simulated
    if (!g_sess.on)
    {
        // A session starts after a real record (the one the game's own pump fetched for the last
        // update): frames are counted on from it. A predicted record carries the frames of the inputs
        // it was made from, so a start there mislabels every frame (MEASURED: NACE at frame 66 with a
        // prediction outstanding when the session began - its first frame counted as 0, the first
        // misprediction could not be rewound, the instances drifted apart). Until then the game runs
        // its own way.
        const int32_t startReal = SessionRealFrame();
        if (startReal < 0)
        {
            static int waits = 0;
            if (waits++ < 20) std::fprintf(stderr, "[rb-session] waiting for a real record to start on (fight update %d)\n", g_fightRun);
            return false;
        }
        g_sess = Session{};
        g_sess.on = true;
        // The session report's other counters are this session's too.
        for (double& v : g_rewindPhase) v = 0;
        for (uint64_t& v : g_interval) v = 0;
        for (uint64_t& v : g_longBy) v = 0;
        g_intervalMax = 0;
        g_sess.last = startReal;                     // the next frame is startReal + 1
        SaveJournalClear();
        g_goSeen = int32_t(GRead32(g_base, kSyncGoApplied));
        g_sessBarrier = 0;
        g_pumpPages.clear();
        g_onlinePages.clear();
        for (auto& s : g_snap) s.frame = ~0ull;      // the sync test's keys are not record frames
        SessionOwnPump(true);
        std::fprintf(stderr, "[rb-session] session on (fight update %d, after record frame %d)\n", g_fightRun, startReal);
        XSessionReportMatch(true);                   // the community server's match history (xsession.cpp)
    }
    ++g_sess.updates;
    // Time sync (rollback_session.cpp): this instance runs ahead of the other - this update starts a
    // frame late (the game shows its last frame once more) and the other side catches up a frame.
    // Everything else runs as usual. MEASURED: skipping this update's pump instead (the battle stalled
    // as for a late record) desynced - frames NACE was verifying lost their corrections (a misprediction
    // at 917 rewound only from 919); a side that is merely slow (TTT2_RB_SLOW) stays in sync.
    if (SessionYield())
    {
        Sleep(16);
        ++g_sess.yields;
        g_stepFlags |= 1;
    }
    int32_t real = -1, frame = -1;
    bool ready = false;
    // NACE's pump writes network state into the game's memory too - the online manager's words
    // around the masked object, a heap page of its own every other frame (MEASURED: 0 of 512 words
    // masked there). Rewinding those under the network corrupts it. The pages the pump writes are
    // learned (write watch around it) and copied before each pump; every word it changes is masked
    // (never rewound) for the rest of the fight.
    std::vector<uint32_t> pumpPages;
    ReadWritten(pumpPages);                              // drain: from here on, the pump's
    // The pages the previous frame opened join now: a page that opens from here on is the pump's
    // (only frame threads' writes land in the late list; the jobs and the render thread are quiet).
    IncorporateLate();
    for (PumpPage& pp : g_pumpPages) std::memcpy(pp.before.data(), g_base + pp.page, kPage);
    g_inPump = true;
    const int n = SessionPrePump(ctx, base, real, ready);
    g_inPump = false;
    pumpPages.clear();
    ReadWritten(pumpPages);
    for (uint32_t p : pumpPages)
    {
        if (!g_isHot[p >> 12] || g_excluded[p >> 12]) continue;
        PumpPage* known = nullptr;
        for (PumpPage& pp : g_pumpPages) if (pp.page == p) known = &pp;
        if (!known)
        {
            g_pumpPages.push_back({ p, std::vector<uint8_t>(kPage) });   // diffed from the next pump on
            std::fprintf(stderr, "[rb-session] NACE's pump writes page %08X: its changes are masked from now on\n", p);
            continue;
        }
        uint32_t added = 0;
        for (uint32_t w = 0; w < kPage; w += 8)
        {
            if (std::memcmp(g_base + p + w, known->before.data() + w, 8) == 0) continue;
            const int32_t hi = g_hotIndex[p >> 12];
            if (hi >= 0 && Outside(size_t(hi), w / 8)) continue;
            NotePumpWord(p + w, known->before.data() + w, "rewound page");
            MaskRange(p + w, 8);
            ++added;
        }
        g_pumpMasked += added;
    }
    // Cold pages the pump opened: they join the rewound set now (a rewind below must not find them
    // pending - it would leave them as the pump left them while everything around goes back), and the
    // words the pump changed there are masked (their earlier content goes into the older snapshots).
    {
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> opened;
        AcquireSRWLockExclusive(&g_cowLock);
        for (const LatePage& lp : g_pendingLate)
            if (!g_isHot[lp.page >> 12]) opened.push_back({ lp.page, std::vector<uint8_t>(lp.before.get(), lp.before.get() + kPage) });
        ReleaseSRWLockExclusive(&g_cowLock);
        IncorporateLate();
        for (auto& [p, before] : opened)
        {
            uint32_t added = 0;
            for (uint32_t w = 0; w < kPage; w += 8)
            {
                if (std::memcmp(g_base + p + w, before.data() + w, 8) == 0) continue;
                const int32_t hi = g_hotIndex[p >> 12];
                if (hi < 0 || Outside(size_t(hi), w / 8)) continue;
                NotePumpWord(p + w, before.data() + w, "new page");
                MaskRange(p + w, 8);
                ++added;
            }
            g_pumpMasked += added;
            ++g_pumpOpened;
            bool known = false;
            for (PumpPage& pp : g_pumpPages) known |= pp.page == p;
            if (!known) g_pumpPages.push_back({ p, std::vector<uint8_t>(kPage) });
        }
    }
    // Frames before NACE's confirmed one can no longer be rolled back: the sound operations they held
    // happen now (a go-frame replay starts at the confirmed frame itself).
    if (const int32_t confirmed = SessionConfirmedFrame(); confirmed > 0)
    {
        PPCContext c = entry;
        SoundRollbackFinal(c, base, uint64_t(confirmed - 1));
    }
    if (const int32_t go = int32_t(GRead32(g_base, kSyncGoApplied)); go != g_goSeen)
    {
        g_goSeen = go;
        if (go >= 0 && go <= g_sess.last) SessionReplay(ctx, base, entry, go, "the go frame of the players' ready");
    }
    if (n > 0 && real >= 0)
    {
        if (n != g_sess.last - real + 1)
            std::fprintf(stderr, "[rb-session] misprediction of %d frames at frame %d, last %d - inconsistent\n", n, real, g_sess.last);
        const uint64_t refused = g_sess.refused;
        g_stepFlags |= 2;
        g_fp.depth = n;
        SessionRewind(ctx, base, entry, real, frame);
        if (g_sess.refused != refused) frame = real;   // NACE went back to P all the same
    }
    else if (n > 0)
        std::fprintf(stderr, "[rb-session] misprediction of %d frames without a real record - unexpected\n", n);
    else if (ready)
    {
        frame = g_sess.last >= 0 ? g_sess.last + 1 : real;
        if (frame < 0) frame = 0;
        CheckRealFrame(frame);
    }
    g_sessFrame = frame;
    if (frame >= 0)
    {
        if (SelfTestEvery() > 0 && n == 0 && g_sess.consumed % uint64_t(SelfTestEvery()) == 0) SessionSelfTest(ctx, base, entry);
        QuiesceJobs(base, entry);
        IncorporateLate();
        KeepRecord(frame);
        SetPrevious(frame);
        Save(g_snap[frame % kRing], uint64_t(frame));
        SessionStateTrace(frame);
        SaveStack(frame % kRing, entry.r1.u32);
        g_sess.last = frame;
        ++g_sess.consumed;
        if (g_sess.consumed % 1200 == 0) { SessionReport("1200 frames"); SlowFramesReport(); }
        if (g_sess.consumed == 120) PrintTaskCensus();
        if (g_sess.consumed == 1200) PrintImportCensus();
    }
    else
    {
        ++g_sess.stalls;
        g_stepFlags |= 4;
    }
    return true;
}
} // namespace

// kernel_io.cpp: the host is about to write guest memory directly (a file read) - open protected
// pages the way a fault would.
void RollbackBeforeHostWrite(uint32_t addr, uint32_t size)
{
    if (!(g_cowActive.load(std::memory_order_acquire) || g_attributing.load(std::memory_order_acquire)) || !size) return;
    AcquireSRWLockExclusive(&g_cowLock);
    for (uint64_t p = addr & ~(kPage - 1); p < uint64_t(addr) + size; p += kPage)
        if (p < 0x100000000ull && InTracked(uint32_t(p))) OpenPageLocked(uint32_t(p));
    ReleaseSRWLockExclusive(&g_cowLock);
}

// Diagnostics hooks (diag_hooks.cpp, kernel_video.cpp): time spent in a call during a re-simulated draw.
bool RollbackInResimDraw() { return g_resimDraw && GetCurrentThreadId() == g_mainTid; }
void RollbackResimSkip() { ++g_throttleSkips; }
void RollbackResimTime(int id, double ms)
{
    if (g_resimDraw && id >= 0 && id < 16) g_resimTimes[id] += ms;
}

// kernel_memory.cpp: an allocation the kernel just served (NtAllocateVirtualMemory).
void RollbackNoteAllocation(uint32_t addr, uint32_t size, PPCContext& ctx)
{
    uint32_t chain[14];
    int depth = 0;
    chain[depth++] = uint32_t(ctx.lr);
    uint32_t frame = ctx.r1.u32;
    for (int i = 0; i < 13 && frame && frame < 0xFFFFFFF0u; ++i)
    {
        const uint32_t prev = GRead32(g_base, frame);
        if (!prev || prev <= frame || prev - frame > 0x10000) break;
        chain[depth++] = GRead32(g_base, prev - 8);
        frame = prev;
    }
    for (int i = 0; i < depth; ++i)
        for (uint32_t owner : kNeverOwners)
            if (chain[i] == owner)
            {
                std::lock_guard l(g_neverMutex);
                g_neverRewind.push_back({ addr, size });
                std::fprintf(stderr, "[rb] %08X+%X (allocated by %08X) is never rewound\n", addr, size, owner);
                return;
            }
}

// diag_hooks.cpp's RtlAllocateHeap / RtlFreeHeap hooks: blocks the network code allocates.
// The network's heap blocks: the innermost caller in a known code range decides - NACE's library
// (0x82B344A0-0x82B5CA78; MEASURED: 0x82B344A0 on is called from the online code, 0x82B343A8 and below from the sound core) and the game's online code with NACE's sync engine "PlayCom" (0x82660000-
// 0x82700000) own the block; the UI library (0x82B60000-0x82B80000: HUD objects online code creates
// are linked into its rewound tables) and the sound library (0x82B1B000-0x82B344A0) do not. MEASURED:
// the sync engine's peer objects were rewound (only NACE's library counted) - a restore set a peer's
// state back, NACE took the node out of the session (most matches under 80 ms / 30 ms jitter / 3 %
// loss, never in lockstep).
bool RollbackOwnedCaller(const PPCContext& ctx)
{
    extern thread_local int t_onlineTaskDepth;
    if (t_onlineTaskDepth > 0) return true;           // inside one of the online system's tasks (experiment)
    auto classify = [](uint32_t a) -> int {
        if (a >= 0x82B344A0u && a < 0x82B5CA78u) return 1;
        if (a >= 0x82660000u && a < 0x82700000u) return 1;
        if (a >= 0x82B60000u && a < 0x82B80000u) return -1;
        if (a >= 0x82B1B000u && a < 0x82B344A0u) return -1;
        return 0;
    };
    if (!g_base) return false;
    if (const int c = classify(uint32_t(ctx.lr))) return c > 0;
    uint32_t frame = ctx.r1.u32;
    for (int i = 0; i < 16 && frame && frame < 0xFFFFFFF0u; ++i)
    {
        const uint32_t prev = GRead32(g_base, frame);
        if (!prev || prev <= frame || prev - frame > 0x10000) break;
        if (const int c = classify(GRead32(g_base, prev - 8))) return c > 0;
        frame = prev;
    }
    return false;
}

// The network's blocks come from an arena of their own (TTT2_ROLLBACK=1), with host bookkeeping: no
// rewind can hand a block out again. (In the game's heap a rewind set the free lists back: a block the
// network allocated after the snapshot was free again for the next allocation.) The arena's memory is
// chunks of g_virtualHeap that are never given back: a freed block is reused by the arena only, so a
// page that held the network's blocks never holds the game's - the chunks are registered whole as
// never rewound (RollbackNoteHeapBlock) and those masks are permanent. MEASURED before chunks: blocks
// went back to g_virtualHeap and its other users got the addresses while the masks stayed - game
// objects never rewound (the task runner called through a cleared vtable, lr 82A80A94, ser26).
void RollbackNoteHeapBlock(uint32_t ptr, uint32_t size);   // below
void RollbackNoteHeapFree(uint32_t ptr);
std::mutex g_arenaMutex;
std::unordered_map<uint32_t, uint32_t> g_arena;     // live block -> requested size
std::unordered_map<uint32_t, uint32_t> g_arenaSlot; // block -> its slot size (live and free)
std::map<uint32_t, std::vector<uint32_t>> g_arenaFree;   // slot size -> free blocks
struct ArenaChunk { uint32_t base, size, top; };
std::vector<ArenaChunk> g_arenaChunks;
uint64_t g_arenaAllocs = 0, g_arenaBytes = 0;
constexpr uint32_t kArenaChunk = 4u << 20;
bool RollbackArenaOn() { return SessionEnabled(); }
// Slot sizes: powers of two from 16 up to 64 KB, then whole 4 KB pages.
uint32_t ArenaSlot(uint32_t size)
{
    if (size <= 65536)
    {
        uint32_t s = 16;
        while (s < size) s <<= 1;
        return s;
    }
    return (size + 4095) & ~4095u;
}
uint32_t RollbackArenaAlloc(uint32_t size, bool zero, uint32_t align)
{
    if (align < 16) align = 16;
    const uint32_t slot = ArenaSlot(size ? size : 1);
    uint32_t a = 0;
    bool newChunk = false;
    uint32_t chunkBase = 0, chunkSize = 0;
    {
        std::lock_guard l(g_arenaMutex);
        // A free slot of this size with the alignment.
        auto& list = g_arenaFree[slot];
        for (size_t i = list.size(); i-- > 0;)
            if (list[i] % align == 0)
            {
                a = list[i];
                list[i] = list.back();
                list.pop_back();
                break;
            }
        // Else carved from the last chunk, or a new one.
        if (!a)
        {
            auto carve = [&](ArenaChunk& c) -> uint32_t {
                const uint32_t at = (c.top + align - 1) & ~(align - 1);
                if (at + slot > c.base + c.size) return 0;
                c.top = at + slot;
                return at;
            };
            if (!g_arenaChunks.empty()) a = carve(g_arenaChunks.back());
            if (!a)
            {
                chunkSize = slot + align > kArenaChunk ? ((slot + align + 65535) & ~65535u) : kArenaChunk;
                void* p = g_virtualHeap.Alloc(chunkSize, 65536);
                if (!p) return 0;
                chunkBase = uint32_t(static_cast<uint8_t*>(p) - g_base);
                g_arenaChunks.push_back({ chunkBase, chunkSize, chunkBase });
                newChunk = true;
                a = carve(g_arenaChunks.back());
                if (!a) return 0;
            }
            g_arenaSlot[a] = slot;
        }
        g_arena[a] = size;
        ++g_arenaAllocs;
        g_arenaBytes += slot;
    }
    if (newChunk)
    {
        RollbackNoteHeapBlock(chunkBase, chunkSize);
        std::fprintf(stderr, "[rb] the network's arena: chunk %08X+%X (%zu chunks)\n", chunkBase, chunkSize, g_arenaChunks.size());
    }
    if (zero) std::memset(g_base + a, 0, size);
    return a;
}
uint32_t RollbackArenaSize(uint32_t a)               // 0: not the arena's
{
    std::lock_guard l(g_arenaMutex);
    const auto it = g_arena.find(a);
    return it == g_arena.end() ? 0 : (it->second ? it->second : 1);
}
bool RollbackArenaFree(uint32_t a)
{
    std::lock_guard l(g_arenaMutex);
    const auto it = g_arena.find(a);
    if (it == g_arena.end()) return false;
    g_arena.erase(it);
    const uint32_t slot = g_arenaSlot[a];
    g_arenaFree[slot].push_back(a);
    g_arenaBytes -= slot;
    return true;
}
void RollbackNoteHeapBlock(uint32_t ptr, uint32_t size)
{
    if (!ptr || !size) return;
    std::lock_guard l(g_blockMutex);
    g_neverBlocks[ptr] = size;
    g_newNeverBlocks.push_back({ ptr, size });
}
void RollbackNoteHeapFree(uint32_t ptr)
{
    if (!ptr) return;
    std::lock_guard l(g_blockMutex);
    if (!g_neverBlocks.empty()) g_neverBlocks.erase(ptr);
}

// rollback_session.cpp: a range NACE's state lives in (the session's prediction ring), registered
// before any fight calibrates.
void RollbackNeverRewind(uint32_t addr, uint32_t size)
{
    std::lock_guard l(g_neverMutex);
    g_neverRewind.push_back({ addr, size });
}
// sound_rollback.cpp: is this word one a rewind restores (a hot page, not excluded, not masked)?
bool RollbackIsRewound(uint32_t addr)
{
    const uint32_t p = addr >> 12;
    if (p >= g_isHot.size() || !g_isHot[p] || g_excluded[p]) return false;
    const int32_t i = p < g_hotIndex.size() ? g_hotIndex[p] : -1;
    return i >= 0 && !Outside(size_t(i), (addr & (kPage - 1)) / 8);
}

// diag_hooks.cpp's task runner hook (sub_82A80AD8): the tasks a session's live updates run (main
// thread), printed once at the session's 1200-frame report - which are the fight, which are not.
std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_taskCensus;
void RollbackTaskStep(uint32_t, uint32_t vtable, uint32_t step)
{
    if (!g_sess.on || GetCurrentThreadId() != g_mainTid || g_resimDraw || g_sessFrame < 0) return;
    ++g_taskCensus[{ step, vtable }];
}
// TTT2_RB_SKIPONLINE=1 (experiment): re-simulated updates leave out the online system's tasks (step
// functions in the online code, 0x82690000-0x826D0000; 827122B8 / 82713838 above are the game's own) - they talk to NACE and read the wall clock.
// The online system's tasks (step functions in the game's online code): what they allocate - at any
// time, menus included - is the network's, never rewound (MEASURED: a timeout cache's map of the online
// task 826C5FE0 looped forever after rewinds; its nodes were rewound while the network moved on).
// TTT2_RB_MASKONLINE=1 (experiment, off: it also masked HUD objects those tasks create, which the UI
// library links into its rewound tables).
bool RollbackOnlineTask(uint32_t step)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_RB_MASKONLINE"); return e && *e == '1'; }();
    return on && step >= 0x82690000u && step < 0x826D0000u;
}
thread_local int t_onlineTaskDepth = 0;
void RollbackOnlineTaskScope(bool in) { t_onlineTaskDepth += in ? 1 : -1; }
// TTT2_RB_SKIPONLINE=1 (experiment, off: with the online tasks' memory rewound, leaving them out of
// re-simulations set their state machines back on every rewind - MEASURED: a loader callback through a
// garbage pointer, the guest crashed).
bool RollbackSkipTask(uint32_t step)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_RB_SKIPONLINE"); return e && *e == '1'; }();
    return on && g_resimulating && GetCurrentThreadId() == g_mainTid && step >= 0x82690000u && step < 0x826D0000u;
}

// The online system's tasks keep the network's state (acknowledgements, timeouts, messages received)
// in the game heap: rewound, a rewind took back what they had received meanwhile (MEASURED: the host's
// online layer took the guest out of the session at ~1163 and ~1482 in two runs, both instances in
// sync). The words an online task step changes in rewound pages are learned around each live step
// (write watch, then a word diff against a copy taken before the step) and masked - never rewound -
// for the rest of the fight. Learned for the first TTT2_RB_ONLINELEARN session frames (experiment,
// default 0 = off: the write watch also sees other threads' writes during a step - MEASURED: pages
// 00524000.. "written" by whichever task ran, masked, the fight stalled after 23 frames).
int OnlineLearnFrames()
{
    static const int frames = [] { const char* e = std::getenv("TTT2_RB_ONLINELEARN"); return e ? std::atoi(e) : 0; }();
    return frames;
}
// Diagnostics (TTT2_RB_NACETRACE=1): the system imports the online tasks call in live session updates
// (KTrace names), printed with the session's 1200-frame report - the boundary a record / replay of the
// online system would have to cover.
thread_local int t_inOnlineStep = 0;
std::map<std::string, uint64_t> g_importCensus;
void RollbackImportNote(const char* name)
{
    if (!t_inOnlineStep || !g_sess.on || g_resimulating) return;
    ++g_importCensus[name];
}
// xsocket.cpp's sendto: an online task's send while a session re-simulates frames (they ran live
// already - their messages went out then; MEASURED: the online tasks call sendto ~40 times per 1200
// frames and never receive themselves - NACE's pump does). NACE's own sends (its pump runs between
// re-simulated frames) go out as usual.
uint64_t g_sendsSuppressed = 0;
bool RollbackSuppressSend()
{
    if (!t_inOnlineStep || !g_resimulating || GetCurrentThreadId() != g_mainTid) return false;
    if (g_sendsSuppressed++ < 10) std::fprintf(stderr, "[rb-session] an online task's send in a re-simulated frame: not sent again\n");
    return true;
}
void PrintImportCensus()
{
    for (const auto& [name, n] : g_importCensus)
        std::fprintf(stderr, "[rb-session] online tasks called %s %llu times\n", name.c_str(), (unsigned long long)n);
    g_importCensus.clear();
}
void RollbackOnlineStep(bool begin, uint32_t step)
{
    if (GetCurrentThreadId() == g_mainTid) t_inOnlineStep += begin ? 1 : -1;
    if (!g_sess.on || g_resimulating || g_resimDraw || GetCurrentThreadId() != g_mainTid || g_sessFrame < 0) return;
    if (step < 0x82690000u || step >= 0x826D0000u || g_sess.consumed >= uint64_t(OnlineLearnFrames())) return;
    if (begin)
    {
        g_onlineScratch.clear();
        ReadWritten(g_onlineScratch);                   // drain: from here on, the task's
        for (OnlinePage& op : g_onlinePages) { std::memcpy(op.before.data(), g_base + op.page, kPage); op.primed = true; }
        return;
    }
    g_onlineScratch.clear();
    ReadWritten(g_onlineScratch);
    for (uint32_t p : g_onlineScratch)
    {
        if (!g_isHot[p >> 12] || g_excluded[p >> 12]) continue;
        OnlinePage* known = nullptr;
        for (OnlinePage& op : g_onlinePages) if (op.page == p) known = &op;
        if (!known)
        {
            g_onlinePages.push_back({ p, std::vector<uint8_t>(kPage), false });   // diffed from its next step on
            std::fprintf(stderr, "[rb-session] online task %08X writes page %08X: its changes are masked from now on\n", step, p);
            continue;
        }
        if (!known->primed) continue;
        const int32_t hi = g_hotIndex[p >> 12];
        if (hi < 0) continue;
        for (uint32_t w = 0; w < kPage; w += 8)
        {
            if (std::memcmp(g_base + p + w, known->before.data() + w, 8) == 0 || Outside(size_t(hi), w / 8)) continue;
            MaskRange(p + w, 8);
            if (g_onlineMasked++ < 40)
                std::fprintf(stderr, "[rb-session] online word %08X (task %08X): %08X %08X -> %08X %08X\n", p + w, step,
                             _byteswap_ulong(*reinterpret_cast<const uint32_t*>(known->before.data() + w)),
                             _byteswap_ulong(*reinterpret_cast<const uint32_t*>(known->before.data() + w + 4)),
                             GRead32(g_base, p + w), GRead32(g_base, p + w + 4));
        }
    }
}
void PrintTaskCensus()
{
    for (const auto& [k, n] : g_taskCensus)
        std::fprintf(stderr, "[rb-session] task step %08X vtable %08X: %llu calls\n", k.first, k.second, (unsigned long long)n);
    g_taskCensus.clear();
}

// ---- Havok's large-block heap (diagnostics) --------------------------------------------------------
// MEASURED in a real two-PC match: after an 8-frame rewind a pool worker (a render job, vtable
// 8200EB6C, growing an array) looped forever in sub_82C7F088 - the large-block allocator's search of
// its free trees (dlmalloc's tmalloc_large: bins at +28 + 4 x index, chunk +4 size, +8/+12 fd/bk,
// +16/+20 children) - and the draw waited on that job for good. A free tree with a cycle: the heap's
// state and the free chunks it points into went back to different moments. Every allocator object
// sub_82C7F5A0 sees is checked after each rewind's restore (jobs quiet): each tree is walked (cycles,
// pointers out of guest memory, same-size rings) and a broken one is reported with how each piece of
// memory is kept. TTT2_RB_LBACHECK=0: off.
std::atomic<uint32_t> g_lbaObjects[4];
const char* MemKind(uint32_t a)
{
    const uint32_t p = a >> 12;
    if (p >= g_isHot.size()) return "?";
    if (g_excluded[p]) return "excluded";
    if (g_foreign[p]) return "foreign";
    if (!g_isHot[p]) return "untracked";
    const int32_t i = p < g_hotIndex.size() ? g_hotIndex[p] : -1;
    return i >= 0 && Outside(size_t(i), (a & (kPage - 1)) / 8) ? "masked" : "rewound";
}
bool LbaCheckOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_RB_LBACHECK"); return !(e && *e == '0'); }();
    return on;
}
// diag_hooks.cpp's sub_82C7F5A0 hook: an allocator object in use.
void RollbackNoteLargeBlockAllocator(uint32_t obj)
{
    for (auto& slot : g_lbaObjects)
    {
        uint32_t cur = slot.load();
        if (cur == obj) return;
        if (cur == 0 && slot.compare_exchange_strong(cur, obj))
        {
            std::fprintf(stderr, "[rb-lba] large-block allocator %08X (%s): treemap %08X top %08X (%s) topsize %08X\n", obj, MemKind(obj),
                         GRead32(g_base, obj + 12), GRead32(g_base, obj + 20), MemKind(GRead32(g_base, obj + 20)), GRead32(g_base, obj + 24));
            return;
        }
        if (cur == obj) return;
    }
}
namespace {
bool LbaSane(uint32_t a) { return (a & 7) == 0 && a >= 0x00100000u && a < 0xC0000000u; }
int g_lbaReports = 0;
void LbaReport(uint32_t obj, int bin, const char* what, uint32_t node, uint32_t from, const char* where)
{
    if (g_lbaReports++ >= 12) return;
    std::fprintf(stderr, "[rb-lba] BROKEN free tree (%s): allocator %08X (%s) bin %d root %08X (word %s): %s at %08X (%s), reached from %08X (%s)\n",
                 where, obj, MemKind(obj), bin, GRead32(g_base, obj + 28 + 4 * bin), MemKind(obj + 28 + 4 * bin), what, node,
                 LbaSane(node) ? MemKind(node) : "-", from, LbaSane(from) ? MemKind(from) : "-");
    if (LbaSane(from))
    {
        char w[512];
        int k = std::snprintf(w, sizeof(w), "[rb-lba]   %08X words:", from);
        for (uint32_t o = 0; o < 32 && k < int(sizeof(w)) - 24; o += 4)
            k += std::snprintf(w + k, sizeof(w) - k, " %08X(%s)", GRead32(g_base, from + o), MemKind(from + o));
        std::fprintf(stderr, "%s\n", w);
    }
}
} // namespace
// True: every tree of every known allocator is sound.
bool CheckLargeBlockHeaps(const char* where)
{
    if (!LbaCheckOn()) return true;
    bool ok = true;
    std::unordered_set<uint32_t> seen;
    std::vector<std::pair<uint32_t, uint32_t>> stack;   // node, reached from
    for (auto& slot : g_lbaObjects)
    {
        const uint32_t obj = slot.load();
        if (!obj) continue;
        for (int bin = 0; bin < 32 && ok; ++bin)
        {
            const uint32_t root = GRead32(g_base, obj + 28 + 4 * bin);
            if (!root) continue;
            seen.clear();
            stack.clear();
            stack.push_back({ root, obj });
            while (!stack.empty() && ok)
            {
                const auto [t, from] = stack.back();
                stack.pop_back();
                if (!LbaSane(t)) { LbaReport(obj, bin, "a pointer out of guest memory", t, from, where); ok = false; break; }
                if (!seen.insert(t).second) { LbaReport(obj, bin, "a cycle", t, from, where); ok = false; break; }
                if (seen.size() > 65536) { LbaReport(obj, bin, "over 65536 nodes", t, from, where); ok = false; break; }
                uint32_t f = GRead32(g_base, t + 8);
                for (int k = 0; f != t; ++k)
                {
                    if (!LbaSane(f) || k > 65536) { LbaReport(obj, bin, "a broken same-size ring", f, t, where); ok = false; break; }
                    f = GRead32(g_base, f + 8);
                }
                if (!ok) break;
                for (uint32_t c : { GRead32(g_base, t + 16), GRead32(g_base, t + 20) })
                    if (c) stack.push_back({ c, t });
            }
        }
    }
    return ok;
}

// kernel_video.cpp diagnostics (TTT2_RB_GPUMEM): would a restore write this page (hot, not excluded,
// not masked whole)? Racy reads - diagnostics only.
bool RollbackPageRewound(uint32_t page)
{
    if (page >= g_isHot.size() || !g_isHot[page] || g_excluded[page]) return false;
    const int32_t i = page < g_hotIndex.size() ? g_hotIndex[page] : -1;
    if (i < 0) return false;
    for (uint32_t k = 0; k < kPage / 8; ++k)
        if (!Outside(size_t(i), k)) return true;
    return false;
}

// rollback_session.cpp's trace: the record frame the current session update simulates (-1: none).
int32_t RollbackSessionFrame() { return g_sess.on ? g_sessFrame : -1; }
bool RollbackSessionOn() { return g_sess.on; }
int32_t RollbackSessionLast() { return g_sess.on ? g_sess.last : -1; }   // the newest record frame consumed
bool RollbackResimulating() { return g_resimulating; }
bool RollbackResimDraw() { return g_resimDraw; }   // diag_hooks.cpp (TTT2_HITCH_PROFILE=5)

// kernel_content.cpp: save containers opened and closed. A save in progress: one open, opened in the
// last 3 s (a container left open does not hold rewinds back for good).
std::atomic<int> g_savesOpen{ 0 };
std::atomic<int64_t> g_saveOpenedMs{ 0 };
int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void RollbackSaveOpen(bool open)
{
    if (open)
    {
        g_savesOpen.fetch_add(1);
        g_saveOpenedMs.store(NowMs());
    }
    else if (g_savesOpen.fetch_sub(1) <= 0)
        g_savesOpen.store(0);
}
bool SaveInProgress() { return g_savesOpen.load() > 0 && NowMs() - g_saveOpenedMs.load() < 3000; }

// kernel_content.cpp (XamContentCreateEx, on the saving thread): the session frame a save belongs to if a
// rewind may still re-simulate it, else -1 (save_store.cpp's journal then copies the container first).
// The end-of-battle save starts on a worker right after the battle asked NACE for its end (sub_826F87B8,
// in the KO's frame - MEASURED in every log so far: the save's I/O carries that frame), so a request
// that recent names the frame; otherwise the frame being simulated, or the newest one.
int32_t SessionEndBattleFrame();   // rollback_session.cpp
int32_t RollbackSaveJournalFrame()
{
    if (!g_sess.on) return -1;
    int32_t frame = g_sessFrame >= 0 ? g_sessFrame : g_sess.last;
    const int32_t endAt = SessionEndBattleFrame();
    if (endAt >= 0 && endAt <= frame && frame - endAt <= 3) frame = endAt;
    return frame > SessionConfirmedFrame() ? frame : -1;
}

// kernel_io.cpp / kernel_content.cpp: the game reached the outside world - a file written, a save
// container created / flushed / closed / deleted (any thread), a file opened, read, sought or deleted
// (`frameOnly`: the frame's own threads; a streaming thread's are its own business). Re-simulating that frame would do
// it again (MEASURED: an Arcade save inside a sync test's window ran a second time, truncated the
// file and never wrote it), so no rewind may cross this frame: the barrier moves past it.
void RollbackNoteIo(bool frameOnly, const char* op, const PPCContext* ctx)
{
    if (!MachineryOn() || !g_base || g_fightRun == 0) return;
    if (frameOnly && !IsFrameThread(GetCurrentThreadId())) return;
    // A save in progress (a save container opened, kernel_content.cpp): its I/O is the save's, which is
    // a whole: a rewind waits until the container is closed (SessionRewind) and the timeline that stays
    // saves again if it gets there too - a complete save done twice is an overwrite. What broke a save
    // was a rewind in the middle of one (MEASURED: an Arcade save re-run from its middle truncated
    // 21_000_0.BIN). MEASURED why no barrier: the final KO in a predicted frame saved the profile (on a
    // pool worker, Tag25SaveData 21/19/14_000_0.BIN) and the barrier refused the rewind a misprediction
    // needed - the instances ended 2-5 frames apart (mod33 run 1, mod35 run 2).
    if (g_sess.on && SaveInProgress())
    {
        static int logged = 0;
        if (logged++ < 20)
            std::fprintf(stderr, "[rb-session] save I/O at frame %d (%s, thread %lu): part of the save, no barrier\n", g_sess.last, op,
                         GetCurrentThreadId());
        return;
    }
    // A thread outside the frame (the game's save thread) reaching the outside world while a session's
    // frames are still predictions: it waits until they are confirmed, so what it does belongs to the
    // timeline that stays, and a rewind may still correct those frames (no barrier). MEASURED: the
    // final KO in a predicted frame started the profile save (XamContentCreateEx + Tag25SaveData files
    // on worker 16584); the barrier then refused the rewind a misprediction needed and the instances
    // ended 5 frames apart. The main thread never waits here (it confirms frames).
    if (g_sess.on && !IsFrameThread(GetCurrentThreadId()))
    {
        const int32_t upTo = g_sess.last;
        const auto start = std::chrono::steady_clock::now();
        int32_t confirmed = SessionConfirmedFrame();
        while (g_sess.on && confirmed < upTo && std::chrono::steady_clock::now() - start < std::chrono::seconds(3))
        {
            Sleep(1);
            confirmed = SessionConfirmedFrame();
        }
        static int logged = 0;
        if (logged++ < 40)
            std::fprintf(stderr, "[rb-session] I/O by thread %lu outside the frame (%s) at frame %d: waited %.1f ms for confirmation (confirmed %d)\n",
                         GetCurrentThreadId(), op, upTo, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                         confirmed);
        return;
    }
    if (g_sess.on && g_sessBarrier < g_sess.last + 1)
    {
        g_sessBarrier = g_sess.last + 1;
        // Diagnostics: who reaches the outside world mid-fight (the guest call chain).
        uint32_t chain[6] = {};
        if (ctx)
        {
            chain[0] = uint32_t(ctx->lr);
            uint32_t sp = ctx->r1.u32;
            for (int i = 1; i < 6 && sp && sp < 0xFFFFFFF0u; ++i)
            {
                const uint32_t prev = GRead32(g_base, sp);
                if (!prev || prev <= sp || prev - sp > 0x10000) break;
                chain[i] = GRead32(g_base, prev - 8);
                sp = prev;
            }
        }
        std::fprintf(stderr, "[rb-session] I/O at frame %d (%s, %s thread %lu, %s update): no rewind before frame %d; callers %08X %08X %08X %08X %08X %08X\n",
                     g_sess.last, op, GetCurrentThreadId() == g_mainTid ? "main" : "worker", GetCurrentThreadId(), g_inUpdate ? "in the" : "outside the",
                     g_sessBarrier, chain[0], chain[1], chain[2], chain[3], chain[4], chain[5]);
    }
    if (g_barrier < g_frame + 1)
    {
        g_barrier = g_frame + 1;
        static int logged = 0;
        if (logged++ < 20) std::fprintf(stderr, "[rb] I/O at frame %llu (thread %lu): no rewind before frame %llu\n",
                                        (unsigned long long)g_frame, GetCurrentThreadId(), (unsigned long long)g_barrier);
    }
}

// TTT2_RB_HEAPLOG=1 (diagnostics): heap calls during a fight from outside the rewound frame work -
// another thread's, or the main thread's between frames (system pumps, NACE). A rewind puts the
// heap's free lists back, so a block allocated there would be handed out a second time.
bool g_inFrameWork = false;        // main thread: from the update's start to the draw's end

// Watchdog (machinery on): the main thread spending over 3 s in one frame's work (update + draw) is
// stuck - its guest call stack (the live context's stack back-chain) goes to the log, once per stall.
static std::atomic<const PPCContext*> g_frameCtx{ nullptr };
static std::atomic<int64_t> g_frameSince{ 0 };      // steady clock ms at the frame work's start, 0 outside
static int64_t WatchdogMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void FrameWatchdog()
{
    int64_t reported = 0;
    for (;;)
    {
        Sleep(500);
        const int64_t since = g_frameSince.load();
        const PPCContext* c = g_frameCtx.load();
        if (!since || !c || since == reported || WatchdogMs() - since < 3000) continue;
        reported = since;
        char line[512];
        int n = std::snprintf(line, sizeof(line), "[rb-watchdog] the main thread is stuck in a frame for %lld ms (session %d, frame %d, fight update %d); guest lr %08X stack:",
                              (long long)(WatchdogMs() - since), g_sess.on ? 1 : 0, g_sessFrame, g_fightRun, uint32_t(c->lr));
        uint32_t sp = c->r1.u32;
        for (int i = 0; i < 16 && sp && sp < 0xFFFFFFF0u && n < int(sizeof(line)) - 12; ++i)
        {
            const uint32_t prev = GRead32(g_base, sp);
            if (!prev || prev <= sp || prev - sp > 0x10000) break;
            n += std::snprintf(line + n, sizeof(line) - n, " %08X", GRead32(g_base, prev - 8));
            sp = prev;
        }
        std::fprintf(stderr, "%s\n", line);
        std::fprintf(stderr, "[rb-watchdog]   this session: %llu rewinds so far; %s\n", (unsigned long long)g_sess.rewinds, LoaderStateText().c_str());
        // A job wait (sub_824F58B8: r31 = the pool job, r30 = its lock at +24): the job's state word
        // and the lock's words (an Xbox critical section).
        const uint32_t r30 = c->r30.u32, r31 = c->r31.u32;
        auto word = [](uint32_t a) { return a >= 0x10000u && a < 0xFFFF0000u ? GRead32(g_base, a) : 0xFFFFFFFFu; };
        std::fprintf(stderr, "[rb-watchdog]   r30 %08X r31 %08X; [r31+144] %08X; [r30..+28]: %08X %08X %08X %08X %08X %08X %08X %08X\n", r30, r31,
                     word(r31 + 144), word(r30), word(r30 + 4), word(r30 + 8), word(r30 + 12), word(r30 + 16), word(r30 + 20),
                     word(r30 + 24), word(r30 + 28));
        DumpJobWords("job words while stuck");
        hitch::DumpGuestThreads();
        // A pool job (r31): its vtable's first methods and its words (+0..+0xA8).
        if (r31 >= 0x10000u && r31 < 0xFFFF0000u)
        {
            const uint32_t vt = GRead32(g_base, r31);
            char words[1024];
            int w = std::snprintf(words, sizeof(words), "[rb-watchdog]   r31 vtable %08X:", vt);
            for (int k = 0; k < 8 && vt >= 0x82000000u && vt < kImageEnd; ++k) w += std::snprintf(words + w, sizeof(words) - w, " %08X", GRead32(g_base, vt + 4 * k));
            w += std::snprintf(words + w, sizeof(words) - w, "; words:");
            for (uint32_t k = 0; k < 0xA8 && w < int(sizeof(words)) - 12; k += 4) w += std::snprintf(words + w, sizeof(words) - w, " %08X", GRead32(g_base, r31 + k));
            std::fprintf(stderr, "%s\n", words);
        }
        // A list walk (sub_82B7BC30: node +40 = next): the chain from r31, and how each node's memory is
        // kept (rewound / excluded / masked word / untracked).
        auto kind = [](uint32_t a) -> const char* {
            const uint32_t p = a >> 12;
            if (p >= g_isHot.size()) return "?";
            if (g_excluded[p]) return "excluded";
            if (g_foreign[p]) return "foreign";
            if (!g_isHot[p]) return "untracked";
            const int32_t i = p < g_hotIndex.size() ? g_hotIndex[p] : -1;
            return i >= 0 && Outside(size_t(i), (a & (kPage - 1)) / 8) ? "masked" : "rewound";
        };
        // Every register that looks like an address, and how its memory is kept.
        {
            char regs[1024];
            int q = std::snprintf(regs, sizeof(regs), "[rb-watchdog]   registers:");
            const std::pair<int, uint32_t> named[] = {
                { 3, c->r3.u32 }, { 4, c->r4.u32 }, { 5, c->r5.u32 }, { 6, c->r6.u32 }, { 7, c->r7.u32 }, { 8, c->r8.u32 },
                { 9, c->r9.u32 }, { 10, c->r10.u32 }, { 24, c->r24.u32 }, { 25, c->r25.u32 }, { 26, c->r26.u32 },
                { 27, c->r27.u32 }, { 28, c->r28.u32 }, { 29, c->r29.u32 }, { 30, c->r30.u32 }, { 31, c->r31.u32 } };
            for (auto [k, v] : named)
            {
                if (q >= int(sizeof(regs)) - 40) break;
                if (v < 0x00100000u || v >= kImageEnd || (v >= VHEAP_END && v < 0x82000000u)) continue;
                q += std::snprintf(regs + q, sizeof(regs) - q, " r%d=%08X(%s)", k, v, kind(v));
            }
            std::fprintf(stderr, "%s\n", regs);
        }
        char chain[1024];
        int m = std::snprintf(chain, sizeof(chain), "[rb-watchdog]   chain from r31:");
        uint32_t node = r31;
        for (int i = 0; i < 12 && node >= 0x10000u && node < 0xFFFF0000u && m < int(sizeof(chain)) - 48; ++i)
        {
            m += std::snprintf(chain + m, sizeof(chain) - m, " %08X(vt %08X, %s)", node, GRead32(g_base, node), kind(node));
            node = GRead32(g_base, node + 40);
        }
        std::fprintf(stderr, "%s\n", chain);
        // The other threads that are not in a kernel wait (lr outside the import / CRT range
        // 0x82BE0000-0x82C10000), e.g. a pool job thread spinning in a list walk the main thread waits
        // for (sub_82BB2940: node +4 = next): their registers and the +4 chain from r31 (a node seen
        // twice = a cycle).
        {
            DWORD tids[64]; uint32_t entries[64]; PPCContext* ctxs[64];
            const int count = hitch::GuestContexts(tids, entries, ctxs, 64);
            for (int t = 0; t < count; ++t)
            {
                const PPCContext* o = ctxs[t];
                const uint32_t lr = uint32_t(o->lr);
                if (o == c || (lr >= 0x82BE0000u && lr < 0x82C10000u) || lr == 0x824F54F0u) continue;
                char regs[1400];
                int q = std::snprintf(regs, sizeof(regs), "[rb-watchdog]   thread %lu (entry %08X) lr %08X registers:", tids[t], entries[t], lr);
                const PPCRegister* gpr[] = { &o->r3, &o->r4, &o->r5, &o->r6, &o->r7, &o->r8, &o->r9, &o->r10, &o->r11,
                    &o->r14, &o->r15, &o->r16, &o->r17, &o->r18, &o->r19, &o->r20, &o->r21, &o->r22, &o->r23, &o->r24,
                    &o->r25, &o->r26, &o->r27, &o->r28, &o->r29, &o->r30, &o->r31 };
                const int num[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
                for (int k = 0; k < int(std::size(gpr)) && q < int(sizeof(regs)) - 40; ++k)
                {
                    const uint32_t v = gpr[k]->u32;
                    if (v < 0x00100000u || v >= kImageEnd || (v >= VHEAP_END && v < 0x82000000u)) continue;
                    q += std::snprintf(regs + q, sizeof(regs) - q, " r%d=%08X(%s)", num[k], v, kind(v));
                }
                std::fprintf(stderr, "%s\n", regs);
                char links[1400];
                int z = std::snprintf(links, sizeof(links), "[rb-watchdog]   thread %lu chain +4 from r31:", tids[t]);
                uint32_t seen[24]; int seenCount = 0;
                uint32_t at = o->r31.u32;
                for (int i = 0; i < 24 && at >= 0x00100000u && at < kImageEnd && !(at >= VHEAP_END && at < 0x82000000u) && z < int(sizeof(links)) - 48; ++i)
                {
                    bool again = false;
                    for (int j = 0; j < seenCount; ++j) again |= seen[j] == at;
                    z += std::snprintf(links + z, sizeof(links) - z, " %08X(%s)%s", at, kind(at), again ? " AGAIN" : "");
                    if (again) break;
                    seen[seenCount++] = at;
                    at = GRead32(g_base, at + 4);
                }
                std::fprintf(stderr, "%s\n", links);
                // The effect-like library's object list (sub_82BB2940, called from sub_82BAED70 at 82BAEDA8):
                // its frame saved the caller's r31 - the object - at back-chain - 16 (std r31,-16(r1)). The
                // object's list: head +1128, tail +1132, count +1136; node +0 prev, +4 next, +960 payload.
                uint32_t sp = o->r1.u32;
                for (int d = 0; d < 12 && sp && sp < 0xFFFFFFF0u; ++d)
                {
                    const uint32_t prev = GRead32(g_base, sp);
                    if (!prev || prev <= sp || prev - sp > 0x10000) break;
                    if (GRead32(g_base, prev - 8) == 0x82BAEDACu)
                    {
                        const uint32_t obj = GRead32(g_base, prev - 12);
                        if (obj < 0x00100000u || obj >= kImageEnd) break;
                        char list[2400];
                        int y = std::snprintf(list, sizeof(list), "[rb-watchdog]   thread %lu effect object %08X(%s): head %08X tail %08X count %u; nodes:",
                                              tids[t], obj, kind(obj), GRead32(g_base, obj + 1128), GRead32(g_base, obj + 1132), GRead32(g_base, obj + 1136));
                        uint32_t nodes[64]; int nodeCount = 0;
                        uint32_t at2 = GRead32(g_base, obj + 1128), back = 0;
                        while (nodeCount < 64 && at2 >= 0x00100000u && at2 < kImageEnd && y < int(sizeof(list)) - 64)
                        {
                            bool again = false;
                            for (int j = 0; j < nodeCount; ++j) again |= nodes[j] == at2;
                            const uint32_t p0 = GRead32(g_base, at2);
                            y += std::snprintf(list + y, sizeof(list) - y, " %08X(%s%s, +960 %08X)%s", at2, kind(at2),
                                               p0 == back ? "" : ", prev wrong", GRead32(g_base, at2 + 960), again ? " AGAIN" : "");
                            if (again) break;
                            nodes[nodeCount++] = at2;
                            back = at2;
                            at2 = GRead32(g_base, at2 + 4);
                        }
                        std::fprintf(stderr, "%s\n", list);
                        const uint32_t pool = GRead32(g_base, 0x830BCE90u);
                        if (pool >= 0x00100000u && pool < kImageEnd)
                            std::fprintf(stderr, "[rb-watchdog]   node pool %08X(%s): +136 %08X +144 %08X +148 %08X +156 %08X\n", pool, kind(pool),
                                         GRead32(g_base, pool + 136), GRead32(g_base, pool + 144), GRead32(g_base, pool + 148), GRead32(g_base, pool + 156));
                        break;
                    }
                    sp = prev;
                }
            }
        }
        std::fflush(stderr);
    }
}
// Diagnostics: TTT2_RB_FINDBLOCK=<hex guest address>[,<hex>...] - the allocation that comes to hold one
// of them (the system heap's hook and the game heap's, diag_hooks.cpp), its call chain and whether the
// network owns it.
void RollbackFindBlock(bool alloc, uint32_t ptr, uint32_t size, const PPCContext& ctx)
{
    // A comma-separated list: the first address the block holds is reported.
    static const std::vector<uint32_t> findBlocks = [] {
        std::vector<uint32_t> v;
        for (const char* e = std::getenv("TTT2_RB_FINDBLOCK"); e && *e;)
        {
            char* end = nullptr;
            const uint32_t a = uint32_t(std::strtoul(e, &end, 16));
            if (end == e) break;
            v.push_back(a);
            e = *end == ',' ? end + 1 : end;
        }
        return v;
    }();
    uint32_t findBlock = 0;
    if (alloc && g_base)
        for (const uint32_t a : findBlocks)
            if (ptr <= a && a - ptr < (size ? size : 1)) { findBlock = a; break; }
    if (findBlock)
    {
        char line[400];
        int k = std::snprintf(line, sizeof(line), "[rb] block %08X+%X holds %08X (owned by the network: %s), callers %08X", ptr, size,
                              findBlock, RollbackOwnedCaller(ctx) ? "yes" : "no", uint32_t(ctx.lr));
        uint32_t sp = ctx.r1.u32;
        for (int i = 0; i < 14 && sp && sp < 0xFFFFFFF0u && k < int(sizeof(line)) - 12; ++i)
        {
            const uint32_t prev = GRead32(g_base, sp);
            if (!prev || prev <= sp || prev - sp > 0x10000) break;
            k += std::snprintf(line + k, sizeof(line) - k, " %08X", GRead32(g_base, prev - 8));
            sp = prev;
        }
        std::fprintf(stderr, "%s\n", line);
    }
}

void RollbackHeapOp(bool alloc, uint32_t ptr, uint32_t size, const PPCContext& ctx)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_RB_HEAPLOG"); return e && *e == '1'; }();
    RollbackFindBlock(alloc, ptr, size, ctx);
    const DWORD t = GetCurrentThreadId();
    // A session's NACE pump allocating from the game heap: counted (the heap's free lists are
    // rewound, NACE's blocks are not).
    if (g_inPump && t == g_mainTid && g_base)
    {
        if (g_pumpHeapOps++ < 40)
            std::fprintf(stderr, "[rb-session] heap %s %08X size %X during NACE's pump, callers %08X\n", alloc ? "alloc" : "free", ptr, size, uint32_t(ctx.lr));
        return;
    }
    // Sessions: heap calls whose call chain runs through the network code (NACE 0x82B344A0-0x82B5CA78)
    // or the game's online code (0x82660000-0x82700000) - their blocks live in the rewound heap.
    if (g_sess.on && g_base)
    {
        uint32_t chain[8] = { uint32_t(ctx.lr) };
        uint32_t sp = ctx.r1.u32;
        for (int i = 1; i < 8 && sp && sp < 0xFFFFFFF0u; ++i)
        {
            const uint32_t prev = GRead32(g_base, sp);
            if (!prev || prev <= sp || prev - sp > 0x10000) break;
            chain[i] = GRead32(g_base, prev - 8);
            sp = prev;
        }
        // The sound library (its API 0x82B1B0B0.. and core, up to NACE at 0x82B344A0): its objects live on
        // in the sound engine whatever the frame's timeline does - a block of the rewound heap freed by a
        // rewind and handed out again is a use-after-free (MEASURED: virtual calls through vtables that
        // had become strings, "Syst" / "undS", lr 0x82B2xxxx-0x82B32xxx).
        int snd = -1;
        for (int i = 0; i < 8 && snd < 0; ++i)
            if (chain[i] >= 0x82B1B000u && chain[i] < 0x82B344A0u) snd = i;
        if (snd >= 0)
        {
            static std::atomic<uint64_t> sndCalls{ 0 };
            if (sndCalls.fetch_add(1) < 60)
                std::fprintf(stderr, "[rb-session] sound heap %s %08X size %X (frame %d%s), callers %08X %08X %08X %08X %08X %08X\n",
                             alloc ? "alloc" : "free", ptr, size, g_sessFrame, g_resimulating ? ", re-simulating" : "",
                             chain[0], chain[1], chain[2], chain[3], chain[4], chain[5]);
        }
        int hit = -1;
        for (int i = 0; i < 8 && hit < 0; ++i)
            if ((chain[i] >= 0x82660000u && chain[i] < 0x82700000u) || (chain[i] >= 0x82B344A0u && chain[i] < 0x82B5CA78u)) hit = i;
        if (hit >= 0)
        {
            static std::atomic<uint64_t> calls{ 0 };
            if (calls.fetch_add(1) < 40)
                std::fprintf(stderr, "[rb-session] online heap %s %08X size %X (thread %s, frame %d), callers %08X %08X %08X %08X %08X %08X\n",
                             alloc ? "alloc" : "free", ptr, size, t == g_mainTid ? "main" : "other", g_sessFrame,
                             chain[0], chain[1], chain[2], chain[3], chain[4], chain[5]);
        }
    }
    if (!on || !g_base || g_fightRun < 2) return;
    const char* who = t == g_mainTid ? (g_inFrameWork ? nullptr : "main, between frames") : IsFrameThread(t) ? nullptr : "foreign";
    if (!who) return;
    static std::atomic<int> left{ 600 };
    if (left.fetch_sub(1) <= 0) return;
    uint32_t chain[4] = { uint32_t(ctx.lr), 0, 0, 0 };
    uint32_t frame = ctx.r1.u32;
    for (int i = 1; i < 4 && frame && frame < 0xFFFFFFF0u; ++i)
    {
        const uint32_t prev = GRead32(g_base, frame);
        if (!prev || prev <= frame || prev - frame > 0x10000) break;
        chain[i] = GRead32(g_base, prev - 8);
        frame = prev;
    }
    std::fprintf(stderr, "[rb-heap] fight frame %d, %s thread %lu: %s %08X size %X, callers %08X %08X %08X %08X\n", g_fightRun, who, t,
                 alloc ? "alloc" : "free", ptr, size, chain[0], chain[1], chain[2], chain[3]);
}

// kernel_thread.cpp: a runtime host thread runs a guest callback (DPC, interrupt, I/O APC, audio):
// not the frame's.
// Only the runtime's own host threads (no guest entry): a guest thread that runs a callback (an APC in
// an alertable wait) stays what its entry says (MEASURED: pool-job threads, entry 82A74760, ended up
// "foreign" in one fight and their 89 pages were never rewound).
void RollbackNoteForeignThread()
{
    if (hitch::GuestEntryOfThread(GetCurrentThreadId()) == 0) NoteForeignThread("a host thread running a guest callback");
}

void SessionTraceTick(bool start);   // rollback_session.cpp (TTT2_RB_NACETRACE)
void SessionTraceUpdate();
int RollbackFightRun() { return g_fightRun; }
void RollbackBattleTick(bool start)   // diag_hooks.cpp's sub_822D36C8 hook, around the tick
{
    g_tick = true;
    SessionTraceTick(start);
    if (!Depth() || !TickSetOnly() || g_fightRun < CalibrateFrom()) return;
    g_tickPages.clear();
    ReadWritten(g_tickPages);                       // start: the update so far; end: the tick
    if (start) g_tickPages.clear();
    else g_tickDone = true;
}

// Job-system objects (diag_hooks.cpp / guest_monitor.cpp hooks): a job group (sub_82ACFF90: pending
// count +8, lock +24, event +196), a pool job (sub_824F58B8: state +144, lock +24), a worker monitor
// (sub_82AE4438: u64 count +16).
void RollbackNoteJobObject(uint32_t addr, uint32_t size)
{
    if (MachineryOn()) NoteJobObject(addr, size);
}
// The pool job threads' loop (sub_824F5458, diag_hooks.cpp): which host thread runs which job.
void RollbackNotePoolThread(uint32_t job) { NotePoolThread(job); }
void RollbackNotePoolThreadExit(uint32_t job) { NotePoolThreadExit(job); }

// TTT2_RB_JOBTRACE=1: for 6 fight frames after calibration, every job-system call of the main thread
// in order (kick / group wait / dispatch / frame kick) with the job group's pending count.
bool JobTraceOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_RB_JOBTRACE"); return e && *e == '1'; }();
    return on;
}
void RollbackJobTrace(const char* what, uint32_t obj, bool after)
{
    if (!JobTraceOn() || g_fightRun < CalibrateFrom() + kCalibrateFrames || g_fightRun > CalibrateFrom() + kCalibrateFrames + 6) return;
    std::fprintf(stderr, "[jobtrace] frame %llu %s %s obj %08X group+8=%u%s thread %lu\n", (unsigned long long)g_frame,
                 g_inUpdate ? "update" : "draw", what, obj, GRead32(g_base, kJobGroup + 8), after ? " (after)" : "", GetCurrentThreadId());
}

// D3D's swap throttle: the frame kick waits until the GPU fence of the previous frame passed. It also
// keeps the CPU from rewriting command buffers the command processor still reads: skipped during
// re-simulation, a re-simulated frame overwrote one mid-parse (MEASURED: "[pm4-fault] packet exceeds
// stream" after ~400 rewinds of 7 frames). It once cost 15.4 ms per re-simulated frame because the
// previous frame's fence waited for its vblank; discarded frames no longer wait for one
// (kernel_video.cpp), so the wait is the command processor's few packets. Timed for the report.
PPC_FUNC(sub_82738E70)
{
    if (!RollbackInResimDraw())
    {
        __imp__sub_82738E70(ctx, base);
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    __imp__sub_82738E70(ctx, base);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    g_resimTimes[12] += ms;
    g_fenceWaitMs += ms;
}

// Threads on their own timelines (entry routines and loops that never return): registered as they start.
PPC_FUNC(sub_82D032D0) { NoteForeignThread(); __imp__sub_82D032D0(ctx, base); }
PPC_FUNC(sub_82C3A4E0) { NoteForeignThread(); __imp__sub_82C3A4E0(ctx, base); }
PPC_FUNC(sub_82C3A540) { NoteForeignThread(); __imp__sub_82C3A540(ctx, base); }

// ---- Havok -------------------------------------------------------------------------------------
// The physics (Havok 2010-era) steps inside the frame: a pool job (vtable 8208322C, sub_82A54E30 ->
// sub_82A57428) hands the step's jobs to Havok's job-thread pool - three workers (sub_82C7B7E8: wait
// on the pool's semaphore, sub_82C78918 = processAllJobs, release the finish semaphore) - and waits
// for them. MEASURED 2026-10-21 (two real online matches, both froze ~3200 frames in): stages where
// the physics runs mid-fight put the workers to work after calibration; they were "foreign" (never
// rewound), so the objects they built and the free-list elements they took stayed in the newest
// timeline while Havok's allocator (page 830D1000, rewound) went back - a freed element's vtable
// was called (lr 82CF5B5C) and the step looped. The bot fights never ran the physics (0 worker
// writes in ~30 test fights; 33 and 51 pages in the two real ones). The workers are frame threads.
// TTT2_RB_HAVOK=foreign: the old way (diagnostics).
// Each Havok thread has a memory router (sub_828AFFB8; hkMemoryRouter: +0 its LIFO stack allocator
// - +4 implementation, +8 slab size, +12 current, +16 end, +20 first non-LIFO end, +24 cached empty
// slab, +28/+32/+36 slab / large / internal allocators - then +40.. the heap allocators), set up by
// sub_828C1600 (initThread: the router into TLS +14140). The workers' routers live on their stacks
// (excluded): a slab the LIFO took from the rewound heap in a re-simulated frame stayed its current
// or cached slab after the rewind while the heap had it free again. The LIFO part of every router
// in a thread block is an island (rewound with each snapshot). TTT2_RB_HAVOK=noislands: without.
namespace {
int HavokMode()   // 0 frame threads + islands, 1 foreign (old), 2 frame threads without islands
{
    static const int v = [] {
        const char* e = std::getenv("TTT2_RB_HAVOK");
        return !e ? 0 : std::strcmp(e, "foreign") == 0 ? 1 : std::strcmp(e, "noislands") == 0 ? 2 : 0;
    }();
    return v;
}
std::mutex g_hkMutex;
struct HkRouter { uint32_t addr; DWORD tid; uint32_t entry; bool adopted; };
std::vector<HkRouter> g_hkRouters;              // under g_hkMutex
std::atomic<bool> g_hkPending{ false };
std::atomic<DWORD> g_hkWorkers[8];
std::atomic<int> g_hkWorkerCount{ 0 };
std::atomic<uint64_t> g_hkRuns{ 0 }, g_hkRunsResim{ 0 };
std::atomic<int> g_hkFirstRun{ -1 };            // fight update of the workers' first job run after calibration
bool IsHavokWorker(DWORD t)
{
    const int n = (std::min)(g_hkWorkerCount.load(), 8);
    for (int i = 0; i < n; ++i) if (g_hkWorkers[i].load() == t) return true;
    return false;
}
constexpr uint32_t kHkLifoBytes = 40;

// Routers in thread blocks become islands; the main thread adopts them (calibration start, snapshots).
void AdoptPendingIslands()
{
    if (HavokMode() != 0 || !g_hkPending.load() || g_excluded.empty()) return;
    std::lock_guard l(g_hkMutex);
    g_hkPending = false;
    for (HkRouter& r : g_hkRouters)
    {
        if (r.adopted || !g_excluded[r.addr >> 12]) continue;
        r.adopted = true;
        g_islands.push_back({ r.addr, kHkLifoBytes });
        std::fprintf(stderr, "[rb-hk] Havok memory router %08X of thread %lu (entry %08X) is on its stack: rewound with each snapshot\n",
                     r.addr, r.tid, r.entry);
    }
}
// Havok's free lists (hkFreeListAllocator: one per element size, in the static page 830D1000 - +0 the
// first free element, +4 the element size, +20 the block size, +40 the large-block allocator
// 830D1580): walked before and after every rewind's restore (jobs quiet) - a cycle or a pointer out of
// guest memory is reported, and so is a free element in memory a rewind leaves alone (another
// timeline's page, a thread block, masked words): the list and the element would go back to different
// moments. TTT2_RB_LBACHECK=0: off (with the large-block trees' check).
std::vector<uint32_t> g_hkLists;
int g_hkReports = 0, g_hkOddReports = 0;
uint64_t g_hkChecks = 0, g_hkElements = 0, g_hkOdd = 0;
double g_hkCheckMs = 0;
void FindHavokFreeLists()
{
    g_hkLists.clear();
    for (uint32_t a = 0x830D1000u; a + 48 <= 0x830D2000u; a += 4)
    {
        const uint32_t es = GRead32(g_base, a + 4), bs = GRead32(g_base, a + 20), la = GRead32(g_base, a + 40);
        if (es && es <= 512 && (es & (es - 1)) == 0 && (bs == 0x1000 || bs == 0x2000 || bs == 0x4000 || bs == 0x8000) && la == 0x830D1580u)
            g_hkLists.push_back(a);
    }
}
bool HkSane(uint32_t a) { return (a & 3) == 0 && ((a >= VHEAP_BEGIN && a < VHEAP_END) || (a >= 0x82000000u && a < kImageEnd)); }
bool CheckHavokFreeLists(const char* where)
{
    if (!LbaCheckOn() || g_hkLists.empty()) return true;
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = true;
    ++g_hkChecks;
    for (uint32_t list : g_hkLists)
    {
        uint32_t from = list, e = GRead32(g_base, list), tortoise = 0, power = 1, lam = 0;   // Brent's cycle check
        uint64_t n = 0;
        const char* bad = nullptr;
        while (e)
        {
            if (!HkSane(e)) { bad = "a pointer out of guest memory"; break; }
            if (e == tortoise) { bad = "a cycle"; break; }
            const char* kind = MemKind(e);
            if (kind[0] == 'f' || kind[0] == 'e' || kind[0] == 'm')   // foreign / excluded / masked
            {
                ++g_hkOdd;
                if (g_hkOddReports++ < 12)
                    std::fprintf(stderr, "[rb-hk] free list %08X (%u-byte elements, %s): element %08X is in %s memory (element %llu, after %08X)\n",
                                 list, GRead32(g_base, list + 4), where, e, kind, (unsigned long long)n, from);
            }
            if (++lam == power) { tortoise = e; power <<= 1; lam = 0; }
            if (++n > (1u << 22)) { bad = "over 4M elements"; break; }
            from = e;
            e = GRead32(g_base, e);
        }
        g_hkElements += n;
        if (!bad) continue;
        ok = false;
        if (g_hkReports++ < 12)
            std::fprintf(stderr, "[rb-hk] BROKEN free list %08X (%u-byte elements, %s): %s at %08X (%s) after %llu elements, reached from %08X (%s)\n",
                         list, GRead32(g_base, list + 4), where, bad, e, HkSane(e) ? MemKind(e) : "-", (unsigned long long)n, from,
                         HkSane(from) ? MemKind(from) : "-");
    }
    g_hkCheckMs += Ms(t0, std::chrono::steady_clock::now());
    return ok;
}
void HavokCalibrationStart()
{
    g_islands.clear();
    FindHavokFreeLists();
    size_t routers = 0;
    {
        std::lock_guard l(g_hkMutex);
        for (HkRouter& r : g_hkRouters) r.adopted = false;
        routers = g_hkRouters.size();
        g_hkPending = true;
    }
    g_hkFirstRun = -1;
    std::fprintf(stderr, "[rb-hk] %zu Havok free lists in 830D1000, %zu memory routers; workers %s\n", g_hkLists.size(), routers,
                 HavokMode() == 1 ? "on their own timeline (TTT2_RB_HAVOK=foreign)" : HavokMode() == 2 ? "the frame's, routers not rewound (TTT2_RB_HAVOK=noislands)" : "the frame's");
}
}   // namespace

PPC_FUNC(sub_82C7B7E8)
{
    if (HavokMode() == 1) NoteForeignThread();
    const int i = g_hkWorkerCount.fetch_add(1);
    if (i < 8) g_hkWorkers[i].store(GetCurrentThreadId());
    std::fprintf(stderr, "[rb-hk] thread %lu is one of Havok's job-pool workers: %s\n", GetCurrentThreadId(),
                 HavokMode() == 1 ? "on its own timeline" : "the frame's");
    __imp__sub_82C7B7E8(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82C78918);
PPC_FUNC(sub_82C78918)   // processAllJobs (the workers' and the stepping thread's)
{
    const bool worker = IsHavokWorker(GetCurrentThreadId());
    if (worker)
    {
        if (HavokMode() != 1) g_hkBusy.fetch_add(1);
        g_hkRuns.fetch_add(1);
        if (g_resimulating) g_hkRunsResim.fetch_add(1);
        int none = -1;
        if (g_fightRun >= CalibrateFrom() + kCalibrateFrames && g_hkFirstRun.load() < 0 && g_hkFirstRun.compare_exchange_strong(none, g_fightRun))
            std::fprintf(stderr, "[rb-hk] Havok's workers run jobs in this fight (first at fight update %d)\n", g_fightRun);
    }
    __imp__sub_82C78918(ctx, base);
    if (worker && HavokMode() != 1) g_hkBusy.fetch_sub(1);
}
extern "C" PPC_FUNC(__imp__sub_828C1600);
PPC_FUNC(sub_828C1600)   // hkBaseSystem::initThread(router)
{
    const DWORD tid = GetCurrentThreadId();
    {
        std::lock_guard l(g_hkMutex);
        g_hkRouters.push_back({ ctx.r3.u32, tid, hitch::GuestEntryOfThread(tid), false });
        g_hkPending = true;
    }
    std::fprintf(stderr, "[rb-hk] thread %lu (entry %08X) sets up Havok memory router %08X\n", tid, hitch::GuestEntryOfThread(tid), ctx.r3.u32);
    __imp__sub_828C1600(ctx, base);
}
PPC_FUNC(sub_823D94B8)
{
    // Diagnostics: who runs it (the call chain, once per thread).
    static thread_local bool logged = false;
    if (!logged)
    {
        logged = true;
        uint32_t chain[6] = { uint32_t(ctx.lr) };
        uint32_t sp = ctx.r1.u32;
        for (int i = 1; i < 6 && sp && sp < 0xFFFFFFF0u; ++i)
        {
            const uint32_t prev = GRead32(g_base, sp);
            if (!prev || prev <= sp || prev - sp > 0x10000) break;
            chain[i] = GRead32(g_base, prev - 8);
            sp = prev;
        }
        std::fprintf(stderr, "[rb] sub_823D94B8 on thread %lu (entry %08X), callers %08X %08X %08X %08X %08X %08X\n", GetCurrentThreadId(),
                     hitch::GuestEntryOfThread(GetCurrentThreadId()), chain[0], chain[1], chain[2], chain[3], chain[4], chain[5]);
    }
    NoteForeignThread();
    __imp__sub_823D94B8(ctx, base);
}
// The loader threads' GetNext (two queue kinds): the job they ran before is done (run and finished);
// the pop inside marks the thread busy ("the file loaders' jobs").
void LoaderGetNext(PPCContext& ctx, uint8_t* base, void (*imp)(PPCContext&, uint8_t*))
{
    LoaderDone();
    LoaderNoteQueue(ctx.r3.u32);
    t_loaderInGetNext = true;
    imp(ctx, base);
    t_loaderInGetNext = false;
}
// A group's finish (sub_823D8DB8): the group noted for the watchdog.
PPC_FUNC(sub_823D8DB8) { LoaderNoteGroup(ctx.r3.u32); __imp__sub_823D8DB8(ctx, base); }
PPC_FUNC(sub_823D9340) { LoaderGetNext(ctx, base, __imp__sub_823D9340); }
PPC_FUNC(sub_823D95E8) { LoaderGetNext(ctx, base, __imp__sub_823D95E8); }
// The intrusive list's pop (r3 = list: head +0, tail +4, count +8). In a loader's GetNext, under the queue's
// mutex: busy before the count drops.
PPC_FUNC(sub_823D9280)
{
    if (t_loaderInGetNext && t_loaderSlot < 0 && GRead32(base, ctx.r3.u32) != 0)
    {
        const unsigned long tid = GetCurrentThreadId();
        g_loaderBusy.fetch_add(1);
        for (int i = 0; i < 8; ++i)
        {
            unsigned long expected = 0;
            if (g_loaderBusyTid[i].compare_exchange_strong(expected, tid)) { t_loaderSlot = i; break; }
        }
        if (t_loaderSlot < 0) t_loaderSlot = 8;    // no slot left: busy all the same (LoaderDone)
        g_loaderTaken.fetch_add(1);
    }
    __imp__sub_823D9280(ctx, base);
}
// The audio library's streaming thread (started by sub_82CFF6C0): it opens, seeks and reads the data
// archives mid-fight (MEASURED: online sessions, rounds' first seconds) on its own timeline.
PPC_FUNC(sub_82CFF418) { NoteForeignThread(); __imp__sub_82CFF418(ctx, base); }
// Diagnostics: the stage a fight loads (sub_8250F070 builds "stg%03d/..." paths from [r4]; stage 46 loads 49's
// effects). Some stages stream resources through the file loaders all fight long (the loader waits above).
PPC_FUNC(sub_8250F070)
{
    if (ctx.r4.u32) std::fprintf(stderr, "[stage] loading stage %u's resources\n", GRead32(base, ctx.r4.u32));
    __imp__sub_8250F070(ctx, base);
}

// The frame kick's vblank wait: until the vblank monitor (ticked by sub_82AD17A8) reaches a target.
PPC_FUNC(sub_82AD1A48)
{
    if (RollbackInResimDraw()) { ++g_throttleSkips; return; }
    const auto t = std::chrono::steady_clock::now();
    __imp__sub_82AD1A48(ctx, base);
    if (GetCurrentThreadId() == g_mainTid) g_fp.vblankMs += Ms(t, std::chrono::steady_clock::now());
}
// The same wait for "past the target" (the frame kick's other vblank wait).
PPC_FUNC(sub_82AD1AD0)
{
    if (RollbackInResimDraw()) { ++g_throttleSkips; return; }
    const auto t = std::chrono::steady_clock::now();
    __imp__sub_82AD1AD0(ctx, base);
    if (GetCurrentThreadId() == g_mainTid) g_fp.vblankMs += Ms(t, std::chrono::steady_clock::now());
}

// The fight's inputs from outside the state: a pad slot's buttons (sub_822745B0: the local pad, or
// online NACE's current record) and, online, whether the next frame's record is ready
// (sub_82664CF8 - the battle skips its tick when not). Recorded per update; a re-simulated update
// gets the same answers in the same order. Calls are keyed by function | argument.
constexpr uint32_t kReadyKey = 0x80000000u;
bool Replay(PPCContext& ctx, uint32_t key)
{
    if (!g_replaying) return false;
    if (g_replayAt < g_replaying->calls.size() && g_replaying->calls[g_replayAt].first == key)
    {
        const uint32_t v = g_replaying->calls[g_replayAt++].second;
        ctx.r3.u64 = key == kReadyKey ? v : v ^ g_perturb;   // TTT2_RB_CHAOS: other inputs
        return true;
    }
    ++g_replayMismatch;
    return false;
}

PPC_FUNC(sub_822745B0)
{
    const uint32_t slot = ctx.r4.u32;
    if (Replay(ctx, slot)) return;
    __imp__sub_822745B0(ctx, base);
    if (g_recording) g_recording->calls.push_back({ slot, ctx.r3.u32 });
}

PPC_FUNC(sub_82664CF8)
{
    if (Replay(ctx, kReadyKey)) return;
    __imp__sub_82664CF8(ctx, base);
    if (g_recording) g_recording->calls.push_back({ kReadyKey, ctx.r3.u32 });
}

void SoundRollbackFrame(uint64_t frame, bool resim);   // sound_rollback.cpp

// The scene draw: the end of the frame's own work (the heap diagnostics' "between frames" starts).
extern "C" PPC_FUNC(__imp__sub_825EBCC8);
PPC_FUNC(sub_825EBCC8)
{
    if (!g_resimDraw) g_fp.draw0 = Clock::now();
    __imp__sub_825EBCC8(ctx, base);
    if (!g_resimDraw) { g_fp.draw1 = Clock::now(); g_inFrameWork = false; g_frameSince = 0; }
    if (MachineryOn()) CheckJobLocks(g_resimDraw ? "after a re-simulated draw" : "after a draw");
}

// The online manager's per-frame update (sub_8266D8C0: the online system's state machines - sessions,
// timeouts, the teardown sub_826BA2C8), called by the frame loop outside the fight update: it runs once
// per live frame and is never re-simulated, but its state sat in rewound pages, so every rewind set it
// back (MEASURED: in sync, the host's online layer took the guest out of the session mid-match in
// several runs, XSessionLeaveRemote from 826BA528). Its writes are the network's: the pages it opens
// first are not rewound (page attribution above), and the words it changes in pages the fight writes
// too are learned during calibration - every page is write-protected then and a fault names its
// thread: the main thread's faults inside this call keep the page's content from before, compared
// after the call - and masked (never rewound) when calibration ends. (A write watch around the call
// also saw the render thread's pages; making the frame threads idle first stalled the frame loop.)
// TTT2_RB_NETWORDS=1 (experiment, off: MEASURED in session fights 133-270 words on 39-59 pages, and
// with them masked a rewind at the session's start hung one instance and the other exited - some are
// read by the fight).
bool NetWordsOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_RB_NETWORDS"); return e && *e == '1'; }();
    return on;
}
thread_local int t_netWriting = 0;
bool RollbackNetWriting() { return t_netWriting > 0 && NetWordsOn(); }
std::map<uint32_t, std::vector<uint8_t>> g_netBefore;          // under g_cowLock: pages it opened this call
std::map<uint32_t, std::array<uint64_t, 8>> g_netWords;        // words it changed (one bit per 8 bytes)
void NetNoteFaultLocked(uint32_t page)                         // OpenPageLocked, attributing
{
    if (!t_netWriting || GetCurrentThreadId() != g_mainTid || g_netBefore.count(page)) return;
    g_netBefore[page].assign(g_base + page, g_base + page + kPage);
}
void ClearNetWords() { g_netWords.clear(); }
// Diagnostics (session on): the online manager's teardown branch (sub_826BA2C8 at 826BA348) runs when
// [0x82E6C5A8], its +224 or that one's +92 is 0. The chain is printed when it changes, and the last
// link armed for the data watch (TTT2_DATAWATCH=1 polls C:/ttt2/port/datawatch.txt) - who zeroes it.
void NetTeardownWatch()
{
    if (!g_sess.on || !g_base) return;
    // The teardown (sub_826BA2C8) acts only when the online event object's pending word is set:
    // [0x82E6B558] (sub_82265948's singleton) -> +4. That word is armed for the data watch.
    static uint32_t last[5] = {};
    const uint32_t a = GRead32(g_base, 0x82E6C5A8u);
    const uint32_t b = a ? GRead32(g_base, a + 224) : 0;
    const uint32_t c = b ? GRead32(g_base, b + 92) : 0;
    const uint32_t e = GRead32(g_base, 0x82E6B558u);
    const uint32_t ev = e ? GRead32(g_base, e + 4) : 0;
    if (a == last[0] && b == last[1] && c == last[2] && e == last[3] && ev == last[4]) return;
    std::fprintf(stderr, "[rb-session] online teardown chain: [82E6C5A8] %08X, +224 %08X, +92 %08X; event object %08X pending %08X (frame %d)\n",
                 a, b, c, e, ev, g_sessFrame);
    const char* tag = std::getenv("TTT2_INSTANCE");             // two-instance tests: the untagged one (the host) arms it
    (void)tag;   // the event word was not the trigger (MEASURED: never written); the data watch now goes to the
                 // remote node's peer state (rollback_session.cpp's ArmPeerWatch)
    last[0] = a; last[1] = b; last[2] = c; last[3] = e; last[4] = ev;
}
void ApplyNetWords()                                           // calibration's end
{
    if (!NetWordsOn()) return;
    uint32_t words = 0, pages = 0, notRewound = 0;
    for (const auto& [p, bits] : g_netWords)
    {
        if (!g_isHot[p >> 12] || g_excluded[p >> 12]) { ++notRewound; continue; }
        ++pages;
        for (uint32_t k = 0; k < kPage / 8; ++k)
            if (bits[k / 64] >> (k % 64) & 1) { MaskRange(p + k * 8, 8); ++words; }
    }
    std::fprintf(stderr, "[rb] the online manager's update: %u words on %u rewound pages masked, %u pages it wrote are not rewound\n",
                 words, pages, notRewound);
}
extern "C" PPC_FUNC(__imp__sub_8266D8C0);
PPC_FUNC(sub_8266D8C0)
{
    const bool learn = NetWordsOn() && g_base && g_attributing.load(std::memory_order_acquire) && GetCurrentThreadId() == g_mainTid;
    NetTeardownWatch();
    if (learn) ReprotectTouched();                 // pages the update / draw opened this frame fault again
    ++t_netWriting;
    __imp__sub_8266D8C0(ctx, base);
    --t_netWriting;
    if (!learn) return;
    std::map<uint32_t, std::vector<uint8_t>> before;
    AcquireSRWLockExclusive(&g_cowLock);
    before.swap(g_netBefore);
    ReleaseSRWLockExclusive(&g_cowLock);
    for (const auto& [p, b] : before)
    {
        std::array<uint64_t, 8>& bits = g_netWords[p];
        for (uint32_t w = 0; w < kPage; w += 8)
            if (std::memcmp(g_base + p + w, b.data() + w, 8) != 0) bits[w / 512] |= 1ull << ((w / 8) % 64);
    }
}

PPC_FUNC(sub_825EF0B8)
{
    SoundRollbackFrame(~0ull, false);                 // the previous frame's sound calls end here
    FrameProfNext(Clock::now());                      // the previous frame's parts (slow frames)
    g_inFrameWork = true;
    SessionTraceUpdate();
    if (!g_foreignOpens.empty())
    {
        std::vector<ForeignOpen> opens;
        AcquireSRWLockExclusive(&g_cowLock);
        opens.swap(g_foreignOpens);
        ReleaseSRWLockExclusive(&g_cowLock);
        for (const ForeignOpen& o : opens)
            std::fprintf(stderr, "[rb] page %08X first written after calibration by a foreign thread %lu (entry %08X, lr %08X): never rewound\n",
                         o.page, o.tid, o.entry, o.lr);
    }
    if (MachineryOn())
    {
        static bool watching = false;
        if (!watching) { watching = true; std::thread(FrameWatchdog).detach(); }
        g_frameCtx = &ctx;
        g_frameSince = WatchdogMs();
    }
    if (!MachineryOn() || !g_base)
    {
        __imp__sub_825EF0B8(ctx, base);
        return;
    }
    // Offline fights pay nothing: with sessions on (and no sync test), a fight whose NACE input sync
    // never ran in its first CalibrateFrom() updates (Arcade, Practice, Versus...) gets the game's own
    // update from there on - no calibration, snapshots, sound recording or random-number split. Only
    // the fight's update count goes on (its end resets it).
    if (g_fightRun < CalibrateFrom() && SessionEnabled() && SessionSyncRunning()) g_fightOnline = true;
    if (Depth() == 0 && !g_fightOnline && g_fightRun >= CalibrateFrom())
    {
        g_tick = false;
        __imp__sub_825EF0B8(ctx, base);
        if (g_tick) ++g_fightRun;
        else g_fightRun = 0;
        return;
    }
    // TTT2_RB_HITCH=<fight update>:<ms> (tests): the main thread sleeps once, at that update of each
    // fight - one instance freezing for a moment (MEASURED in a real two-PC match: a 2.6 s hitch on one
    // side during the intro left the other ~10 frames ahead for good).
    {
        static const std::pair<int, int> hitch = [] {
            const char* e = std::getenv("TTT2_RB_HITCH");
            int at = 0, ms = 0;
            if (e) std::sscanf(e, "%d:%d", &at, &ms);
            return std::pair<int, int>{ at, ms };
        }();
        // TTT2_RB_HITCH_ON=<instance> (A = the untagged one): only that instance
        static const bool here = [] {
            const char* on = std::getenv("TTT2_RB_HITCH_ON");
            const char* tag = std::getenv("TTT2_INSTANCE");
            if (!on || !*on) return true;
            return std::strcmp(on, tag && *tag ? tag : "A") == 0;
        }();
        // TTT2_RB_SLOW=<ms>:<every n updates> (tests): while a round runs (battle state 4) the main
        // thread sleeps that long every n updates - a side that keeps losing frames (MEASURED in a real
        // two-PC match: a cold shader cache, 28-50 fps in places).
        static const std::pair<int, int> slow = [] {
            const char* e = std::getenv("TTT2_RB_SLOW");
            int ms = 0, every = 0;
            if (e) std::sscanf(e, "%d:%d", &ms, &every);
            return std::pair<int, int>{ ms, every };
        }();
        if (here && slow.first > 0 && slow.second > 0 && g_fightRun % slow.second == 0 && GRead32(g_base, 0x82EAC03Cu) == 4)
            Sleep(DWORD(slow.first));
        if (here && hitch.second > 0 && g_fightRun == hitch.first)
        {
            std::fprintf(stderr, "[rb] test hitch: the main thread sleeps %d ms at fight update %d\n", hitch.second, g_fightRun);
            Sleep(DWORD(hitch.second));
        }
    }
    const PPCContext entry = ctx;
    g_mainTid = GetCurrentThreadId();
    ++g_updates;
    CheckJobLocks("at an update start");
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint32_t> written;
    const bool calibrating = g_fightRun >= CalibrateFrom() && g_fightRun < CalibrateFrom() + kCalibrateFrames;
    const bool calibrated = g_fightRun >= CalibrateFrom() + kCalibrateFrames;
    if (g_fightRun >= CalibrateFrom()) QuiesceJobs(base, entry);   // a quiet frame start (QuiesceJobs)
    if (calibrated) ReprotectForeign();                             // foreign pages watched again
    // The whole previous frame, every thread - until calibration ends; then (full-frame state) new
    // pages announce themselves by faulting.
    if (UpdateSetOnly()) ReadWritten(written);
    if (g_fightRun == CalibrateFrom())
    {
        StartCalibration();
        NoteEvent("calibration start");
        if (!UpdateSetOnly()) StartAttribution();
    }
    else if (calibrating && !UpdateSetOnly()) ReprotectTouched();
    // Full-frame state (the default): the draw is re-simulated too, so its words are restored like
    // the update's; only the update-only sets learn words to leave alone.
    const uint32_t outsideAdded = g_lastPostPages && UpdateSetOnly() ? LearnOutside() : 0;
    if (g_fightRun >= CalibrateFrom()) ApplyJobMasks(g_hot.size() != g_lastPostPages);
    RngIn();                                        // before the snapshot: it holds the fight's state
    // A page first written after calibration: older snapshots lack it, so a rewind leaves it as it
    // is (MEASURED: the fight's replay recording grows into a new page every ~7 frames; re-simulated
    // frames rewrite what they append).
    if (calibrated && !UpdateSetOnly()) AddHot(written, true);
    const auto t1 = std::chrono::steady_clock::now();
    // An online rollback session: NACE's frame pump now, a rewind on a misprediction, this update's
    // record frame's snapshot (SessionStep). Otherwise the sync test's per-update snapshots.
    const bool session = calibrated && SessionStep(ctx, base, entry);
    if (calibrated && !session) IncorporateLate();
    if (calibrated && !session) Save(g_snap[g_frame % kRing], g_frame);
    if (calibrated && !session) g_battleStateAt[g_frame % kRing] = GRead32(g_base, 0x82EAC03Cu);
    if (calibrated && !session) SaveStack(int(g_frame % kRing), entry.r1.u32);
    const auto t2 = std::chrono::steady_clock::now();
    g_fp.step0 = t1;
    g_fp.step1 = t2;
    if (calibrated && !session && Depth() && g_frame % Every() == 0 && g_frame >= g_barrier + kRing) RunTest(ctx, base, entry);

    uint64_t mainHeap0, allHeap0, mainHeap1, allHeap1;
    GameHeapOps(mainHeap0, allHeap0);
    static uint64_t mainHeapAtEnd = 0;
    const uint64_t mainHeapOutside = mainHeap0 - mainHeapAtEnd;
    if (session) SoundRollbackFrame(g_sessFrame >= 0 ? uint64_t(g_sessFrame) : ~0ull, false);   // a stall: not recorded
    else if (calibrated) SoundRollbackFrame(g_frame, false);
    Update(ctx, base, entry, g_frame, false, !session);
    g_fp.upd1 = Clock::now();
    g_fp.on = session;
    GameHeapOps(mainHeap1, allHeap1);
    mainHeapAtEnd = mainHeap1;
    if (UpdateSetOnly())
    {
        std::vector<uint32_t> updated;
        ReadWritten(updated);                       // the update's pages (and other threads' meanwhile)
        if (TickSetOnly()) updated.swap(g_tickPages);
        g_tickDone = false;
        if (calibrating) AddHot(updated, false);
        if (calibrated) AddHot(updated, true);
    }
    if (calibrated && !session) { IncorporateLate(); Save(g_post[g_frame % kRing], g_frame); }
    // The sync test rewinds at most kRing / 2 frames: older frames' held sound operations happen now.
    if (calibrated && !session && g_frame > uint64_t(kRing / 2))
    {
        PPCContext c = entry;
        SoundRollbackFinal(c, base, g_frame - kRing / 2 - 1);
    }
    if (g_fightRun >= CalibrateFrom())
    {
        g_lastPostPages = g_hot.size();
        g_st.outside += outsideAdded;
        if (g_lastPost.size() < g_lastPostPages * kPage) g_lastPost.resize(g_lastPostPages * kPage + 512 * kPage);
        for (size_t i = 0; i < g_lastPostPages; ++i) std::memcpy(g_lastPost.data() + i * kPage, g_base + g_hot[i], kPage);
    }
    RngOut();

    if (!g_tick)
    {
        {
            PPCContext c = entry;
            SoundRollbackFinal(c, base, ~0ull);       // the fight is over: every held sound operation happens
        }
        if (g_fightRun) std::fprintf(stderr, "[rb] fight over after %d updates; %u tests, %u with fighter differences\n",
                                     g_fightRun, g_st.tests, g_st.failed);
        if (g_cowActive.load()) UnprotectAll();
        if (g_fightRun) NoteEvent("fight over");
        g_fightRun = 0;
        g_fightOnline = false;
        return;
    }
    if (g_fightRun == CalibrateFrom() + kCalibrateFrames - 1)
    {
        FindFighters();
        g_barrier = g_frame + 1;
        if (!UpdateSetOnly())
        {
            FinishAttribution();
            ApplyNetWords();
            ProtectCold();
        }
        DumpJobWords("job words at calibration");
        std::fprintf(stderr, "[rb] calibrated: %zu hot pages (%.1f MB); fighters %08X %08X %08X %08X\n", g_hot.size(),
                     g_hot.size() * 4096.0 / 1048576.0, g_fighters[0], g_fighters[1], g_fighters[2], g_fighters[3]);
    }
    ++g_fightRun;
    if (calibrated)
    {
        g_st.watch += Ms(t0, t1);
        g_st.save += Ms(t1, t2);
        g_st.heapMain += mainHeap1 - mainHeap0;
        g_st.heapMainOutside += mainHeapOutside;
        g_st.heapOther += (allHeap1 - allHeap0) - (mainHeap1 - mainHeap0);
        if (++g_st.frames % 600 == 0)
        {
            std::fprintf(stderr, "[rb] 600 frames: hot %zu pages (%llu joined after calibration), %llu words learned as not the fight's; per frame: write watch %.3f ms, snapshot %.3f ms, waiting for the render thread %.3f ms, pool jobs %.3f ms (%llu waits, %llu timeouts, %llu sections lent in all, %llu jobs not waited for); foreign-page faults %llu, foreign pages turned the frame's %llu; game heap calls: main thread %llu in updates, %llu outside; other threads %llu during updates\n",
                         g_hot.size(), (unsigned long long)g_latePages, (unsigned long long)g_st.outside, g_st.watch / 600, g_st.save / 600,
                         g_renderWaitMs / 600, g_quiesceMs / 600, (unsigned long long)g_quiesceWaits, (unsigned long long)g_quiesceTimeouts,
                         (unsigned long long)g_quiesceLent, (unsigned long long)g_quiesceSelfBlocked,
                         (unsigned long long)g_foreignFaults, (unsigned long long)g_foreignToHot, (unsigned long long)g_st.heapMain,
                         (unsigned long long)g_st.heapMainOutside, (unsigned long long)g_st.heapOther);
            // The image's static data above 0x83000000 (tracked since 2026-10-21): Havok's allocator objects.
            size_t tail = 0;
            for (uint32_t p : g_hot) tail += p >= 0x83000000u && p < kImageEnd;
            std::fprintf(stderr, "[rb] image static data above 0x83000000: %zu hot pages; Havok allocators' page 830D1000 %s\n",
                         tail, g_isHot[0x830D1000u >> 12] ? "hot (rewound)" : g_excluded[0x830D1000u >> 12] ? "excluded" : "not written yet");
            std::fprintf(stderr, "[rb-hk] 600 frames: Havok's workers ran %llu job batches (%llu re-simulating), waited for %llu times (%llu timeouts); free lists checked %llu times (%.3f ms, %llu elements each), %llu elements in memory a rewind leaves alone; %zu islands\n",
                         (unsigned long long)g_hkRuns.load(), (unsigned long long)g_hkRunsResim.load(), (unsigned long long)g_hkQuiesceWaits,
                         (unsigned long long)g_hkQuiesceTimeouts, (unsigned long long)g_hkChecks, g_hkChecks ? g_hkCheckMs / g_hkChecks : 0.0,
                         (unsigned long long)(g_hkChecks ? g_hkElements / g_hkChecks : 0), (unsigned long long)g_hkOdd, g_islands.size());
            std::fprintf(stderr, "[rb-ld] 600 frames: the file loaders took %llu jobs; %llu snapshots / restores waited for them (%.3f ms on average, at most %.2f ms; %llu timeouts, %llu sections lent in all)\n",
                         (unsigned long long)g_loaderTaken.exchange(0), (unsigned long long)g_loaderWaits,
                         g_loaderWaits ? g_loaderWaitMs / g_loaderWaits : 0.0, g_loaderWaitMax, (unsigned long long)g_loaderTimeouts,
                         (unsigned long long)g_loaderLent);
            g_loaderWaits = 0;
            g_loaderWaitMs = g_loaderWaitMax = 0;
            g_hkRuns = 0;
            g_hkRunsResim = 0;
            g_hkQuiesceWaits = 0;
            g_hkChecks = g_hkElements = g_hkOdd = 0;
            g_hkCheckMs = 0;
            g_quiesceMs = 0;
            g_renderWaitMs = 0;
            g_foreignFaults = 0;
            g_quiesceWaits = 0;
            g_st.save = g_st.watch = 0;
            g_st.heapMain = g_st.heapOther = g_st.heapMainOutside = 0;
        }
    }
    ++g_frame;
}
