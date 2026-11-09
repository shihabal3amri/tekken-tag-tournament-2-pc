#pragma once
// Hardware (Direct3D 11) backend, stage 1.
//
// The CPU keeps what is cheap and already validated: PM4 decoding, the vertex shader
// (interpreter), primitive assembly, rectangle synthesis and culling. The GPU does everything
// per pixel: rasterization, the translated pixel shader, alpha test, depth, blending, EDRAM
// surfaces, resolves and presentation. Nothing is read back in the steady state.
//
// EDRAM model: the same as the CPU renderer's "EDRAM-lite" (one store per colour/depth base,
// sample-space layout, pitch * samples-per-row), held in fixed-size GPU textures.
// Resolves copy into GPU textures registered by destination address; texture fetches of that
// address use them directly. Guest memory under a resolve is NOT written unless something
// needs the bytes (a mismatched fetch, TTT2_GPU_HW_WRITEBACK=1, or compare mode).
// CPU-written textures are decoded from guest memory and invalidated with Windows write
// watch (guest memory is allocated MEM_WRITE_WATCH in main.cpp).
//
// TTT2_GPU_HW=1 enables it; TTT2_GPU_HW=compare also runs the CPU renderer and compares every
// resolve. Include inside gpu_render.cpp after Vertex/Surface/Texture and the interpreter.
#include "gpu_hw_translate.h"
#include "gpu_adapter.h"
#include "display_settings.h"
#include "gpu_fps_overlay.h"
#include "log_utf8.h"
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <condition_variable>
#include <deque>
#include <cmath>
#include <limits>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <intrin.h>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <set>     // TEST ONLY (p_diag35b)

extern void (*g_gpuFrameNote)(const char*);   // gpu_render.cpp: the frame log (framelog.trigger)
extern HANDLE g_mainThreadHandle;               // main.cpp: the game's main thread (frame cost reports)
void DiagThreadsReport(double tscPerMs, uint64_t presents, double frameMs);   // kernel_thread.cpp (DIAGNOSTIC BUILD, p72)
void DiagThreadsReportAsync(double tscPerMs, uint64_t presents, double frameMs);   // diag16: on its own thread
void GpuNoteMemory(int kind, uint32_t addr, uint32_t bytes);   // kernel_video.cpp (TTT2_RB_GPUMEM diagnostics)
bool PoisonOn();                                                                              // TEST ONLY (p_diag35)
void PoisonScan(const char* kind, uint32_t addr, uint32_t bytes, uint64_t a, uint64_t b);     // kernel_memory.cpp
bool DirtyMeasureOn();                          // rollback_probe.cpp: the write-watch bits are its
void KernelFramePriorityAbToggle();             // kernel_objects.cpp (diag15): thread priorities' A/B
void AudioFlushAbToggle();                     // audio_flush.cpp (diag17): sound filters flushed / not
const char* AudioFlushLabel();
const char* KernelFramePriorityLabel();
uint64_t KernelTakePriorityRequests();
void KernelTakeFrameReads(uint64_t& reads, uint64_t& bytes, double& longestMs);   // kernel_io.cpp: since the last call
void DiagMainOffTotals(uint64_t out[4]);   // kernel_thread.cpp: the game main thread's waits, sleeps, critical sections, yields (TSC)
void DiagNameThread(const wchar_t* name);   // kernel_thread.cpp
uint64_t KernelTakePriorityHostCalls();   // kernel_objects.cpp: of those, the ones that reached SetThreadPriority
void WriteWatchPublish(int self, const PVOID* pages, size_t count);                          // write_watch.cpp
void WriteWatchDrain(int self, uint32_t firstPage, uint32_t lastPage, std::vector<uint32_t>& out);
extern std::atomic<int64_t> g_diagDrawPacketTicks;   // kernel_video.cpp (DIAGNOSTIC BUILD): GpuExecuteDraw's wall time
extern std::atomic<int64_t> g_diagKickNs, g_diagKickGpuNs, g_diagOtherGpuNs;   // diag_hooks.cpp (p_diag47): frame-kick waits
extern std::atomic<uint32_t> g_diagKicks;
std::string DiagTakeWaitSites(double presents);                                  // diag_hooks.cpp (p_diag47)
void GpuTakeFlipStats(uint64_t& entries, uint64_t& late, uint64_t& paced);        // kernel_video.cpp: late frames
bool GpuLateFlipAbOn();
void GpuLateFlipAbToggle();
bool LateFlipOn();
namespace frame_overlap {
void TakeStats(uint64_t& kicks, uint64_t& overlapped);
std::string TakeGuardStats(double presents);
bool AbOn();
void AbToggle();
bool AbTurn();
void NoteBuffer(uint32_t addr, uint32_t bytes);
void NoteGrowing(uint32_t addr, uint32_t bytes);
const uint8_t* CpReadBegin(uint32_t addr, uint32_t bytes, std::vector<uint8_t>& scratch);
void CpReadEnd();
bool CopiesPossible();
}   // frame_overlap.cpp
namespace post_dof {
bool AbOn();
bool AbOff();
void AbToggle();
void TakeStats(uint64_t& wanted, uint64_t& removed);
bool Off();
const char* Label();
void TakeDynStats(uint64_t& reduced, uint64_t& held);
void TakeFxStats(uint64_t* asked);
}   // post_dof.cpp (p86: the game's depth of field off / A/B; p87 the DEPTH OF FIELD menu row)
extern std::atomic<int64_t> g_diagPrepTicks[7];       // gpu_render.cpp (DIAGNOSTIC BUILD): parts of the draw preparation
extern std::atomic<uint32_t> g_stageLoads;             // kernel_video.cpp: stages loaded so far
extern std::atomic<uint32_t> g_cpSyncEpoch;            // kernel_video.cpp: command batches (DirtyTracker::SyncOnce)
extern uint32_t g_vsConstWrites, g_psConstWrites;
extern uint32_t g_vsConstLo, g_vsConstHi, g_psConstLo, g_psConstHi;   // p_diag46: registers written since the last compare       // kernel_video.cpp WriteReg: ALU constant register writes

namespace ttt2_hw {
using Microsoft::WRL::ComPtr;

enum class Mode { Off = 0, On = 1, Compare = 2 };

struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
// TTT2_D3D_CALLS=1 (diagnostics): every checked D3D call counted by its label, printed with the
// 5-second stats (which creations / maps a frame makes).
inline bool CallCountOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_D3D_CALLS"); return e && *e == '1'; }();
    return on;
}
inline std::mutex g_callCountMutex;
inline std::map<std::string, uint64_t> g_callCounts;
inline void Check(HRESULT hr, const char* what)
{
    if (CallCountOn())
    {
        std::lock_guard l(g_callCountMutex);
        ++g_callCounts[what];
    }
    if (FAILED(hr))
    {
        char text[160];
        std::snprintf(text, sizeof(text), "%s failed: HRESULT %08X", what, (unsigned)hr);
        throw Failure(text);
    }
}

inline uint64_t Fnv(const void* data, size_t size, uint64_t h = 1469598103934665603ull)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

inline Mode RequestedMode()
{
    static const Mode mode = [] {
        const char* e = std::getenv("TTT2_GPU_HW");
        if (!e) return Mode::Off;
        if (std::strcmp(e, "1") == 0) return Mode::On;
        if (std::strcmp(e, "compare") == 0) return Mode::Compare;
        return Mode::Off;
    }();
    return mode;
}
inline std::atomic<bool> g_failed{ false };
inline Mode ActiveMode() { return g_failed.load(std::memory_order_relaxed) ? Mode::Off : RequestedMode(); }

// ---------------------------------------------------------------------------------------
// Write-watch based dirty tracking. Every Sync consumes the OS write-watch bits of a range
// and stamps the written pages with a fresh sequence number, so several consumers can ask
// "written since sequence N?" about overlapping ranges without stealing each other's bits.
// rollback.cpp reads the bits too: write_watch.cpp passes each side the pages the other read.
// Measured cost (2026-10-10, this machine): first guest write to a page after a reset
// ~0.45 us, a 4 MB range query ~11 us. Only ranges actually used as textures are reset.
// ---------------------------------------------------------------------------------------
inline std::atomic<int64_t> g_diagSyncTicks{ 0 }, g_diagSyncCalls{ 0 };   // DIAGNOSTIC BUILD: GetWriteWatch calls
inline std::atomic<int64_t> g_diagSyncSkipped{ 0 }, g_diagSyncMissed{ 0 };
inline std::atomic<int64_t> g_diagSyncClustered{ 0 };     // p_diag45: ranges whose cluster was already synced in the batch
inline bool g_wwClustersOn = true;                         // p_diag45: TTT2_WW_CLUSTERS=0 off; the A/B switches it
// TEST ONLY (p102, TTT2_DIAG_GEODUMP=1): while C:/ttt2/port/fb/geodump.arm exists, every index / vertex range a draw reads
// and every write-watch range asked (kind I / V / S, command batch, present, address, bytes, content hash, key) goes to
// C:/ttt2/port/fb/geodumpT.txt - for measuring what a cache of unchanged geometry would keep (tools2/geoan.py).
inline std::atomic<uint32_t> g_geoPresent{ 0 };
inline FILE* GeoDumpFile()
{
    static FILE* f = [] { const char* e = std::getenv("TTT2_DIAG_GEODUMP"); return e && *e == '1' ? std::fopen("C:/ttt2/port/fb/geodumpT.txt", "w") : nullptr; }();
    return f;
}
inline void GeoDump(char kind, uint32_t addr, uint32_t bytes, const void* src, uint64_t key)
{
    FILE* f = GeoDumpFile();
    if (!f) return;
    static uint32_t calls = 0;
    static bool armed = false;
    if ((calls++ & 1023) == 0) armed = std::filesystem::exists("C:/ttt2/port/fb/geodump.arm");
    if (!armed) return;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ bytes;
    if (src)
    {
        const uint8_t* p = static_cast<const uint8_t*>(src);
        uint32_t i = 0;
        for (; i + 8 <= bytes; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
        for (; i < bytes; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    }
    std::fprintf(f, "%c %u %u %08X %X %016llX %016llX\n", kind, ::g_cpSyncEpoch.load(std::memory_order_relaxed),
                 g_geoPresent.load(std::memory_order_relaxed), addr, bytes, (unsigned long long)h, (unsigned long long)key);
}
// DIAGNOSTIC BUILD: when shaders were bound for the first time (ApplyPipe), to count those just before a freeze.
inline std::mutex g_diagFirstUseLock;
inline std::deque<std::pair<std::chrono::steady_clock::time_point, bool>> g_diagFirstUses;   // (when, pixel shader)
inline std::atomic<uint32_t> g_diagFirstUseCount[2] = {};   // TEST ONLY (p_diag27): vertex, pixel shaders first bound
inline std::atomic<uint32_t> g_diagStateCreates{ 0 };         // TEST ONLY (p_diag27): blend / depth / raster / sampler objects made
inline void DiagFirstUse(bool pixel)
{
    g_diagFirstUseCount[pixel ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    std::lock_guard l(g_diagFirstUseLock);
    g_diagFirstUses.emplace_back(std::chrono::steady_clock::now(), pixel);
    if (g_diagFirstUses.size() > 20000) g_diagFirstUses.pop_front();
}
inline void DiagFirstUsesWithin(double seconds, int& ps, int& vs)
{
    std::lock_guard l(g_diagFirstUseLock);
    const auto now = std::chrono::steady_clock::now();
    ps = vs = 0;
    for (auto it = g_diagFirstUses.rbegin(); it != g_diagFirstUses.rend(); ++it)
    {
        if (std::chrono::duration<double>(now - it->first).count() > seconds) break;
        (it->second ? ps : vs)++;
    }
}
// p_diag39: the diagnostic timers' clock. The timestamp counter (one instruction) scaled to QueryPerformanceCounter
// ticks, so the reports keep their units; QueryPerformanceCounter itself was ~12% of the command processor's busy
// time (a profile, 2026-10-28: 17 ns a call, ~40 calls a draw).
inline double DiagTscToQpc()
{
    static const double ratio = [] {
        LARGE_INTEGER f, q0, q1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q0);
        const uint64_t t0 = __rdtsc();
        do QueryPerformanceCounter(&q1); while (q1.QuadPart - q0.QuadPart < f.QuadPart / 50);
        const uint64_t t1 = __rdtsc();
        return t1 > t0 ? double(q1.QuadPart - q0.QuadPart) / double(t1 - t0) : 0.0;
    }();
    return ratio;
}
// Adds the ticks since tsc0 to a diagnostic counter (only the command-processor thread adds; a plain add).
inline void DiagAddSince(std::atomic<int64_t>& acc, uint64_t tsc0)
{
    const int64_t d = int64_t(double(__rdtsc() - tsc0) * DiagTscToQpc());
    acc.store(acc.load(std::memory_order_relaxed) + d, std::memory_order_relaxed);
}

// p109: DIAGNOSTIC BUILD A/B - Newest page by page (the d14-old windows of TTT2_DIAG_IX_AB=1).
inline bool g_newestBlocksOff = false;

class DirtyTracker
{
public:
    // Sync at most once per command batch (g_cpSyncEpoch): the game writes what a batch samples before it publishes
    // the batch, and a batch also ends wherever the CPU may act on the GPU's progress inside one (fence and memory
    // writes, waits, interrupts). Texture binds asked the write watch ~1450 times a frame, a system call each
    // (0.85 ms a frame on a fast PC, MEASURED 2026-10-25 - system calls cost several times more on older CPUs).
    uint32_t SyncOnce(uint32_t start, uint32_t size)
    {
        static const bool onceOn = [] { const char* e = std::getenv("TTT2_SYNC_ONCE"); return !(e && *e == '0'); }();   // p_diag31
        GeoDump('S', start, size, nullptr, 0);         // TEST ONLY (p102)
        if (!onceOn) return Sync(start, size);
        const uint32_t epoch = ::g_cpSyncEpoch.load(std::memory_order_relaxed);
        if (epoch != onceEpoch_) { onceEpoch_ = epoch; once_.clear(); }
        if (once_.insert(uint64_t(start) << 32 | size).second)
        {
            if (!ClustersOn() || !size) return Sync(start, size);
            // p_diag45: the run of adjoining texture ranges (used in the last seconds) this range is in - one call for
            // all of them, once a batch. A larger range synced is never wrong (each page keeps its own sequence).
            recent_[uint64_t(start) << 32 | size] = epoch;
            if (epoch - clustersBuilt_ > 2048) RebuildClusters(epoch);
            const uint32_t lo = start >> 12, hi = uint32_t((uint64_t(start) + size + 4095) >> 12);
            auto it = std::upper_bound(clusters_.begin(), clusters_.end(), lo, [](uint32_t v, const Cluster& c) { return v < c.lo; });
            if (it == clusters_.begin()) return Sync(start, size);
            Cluster& c = *(it - 1);
            if (c.lo > lo || hi > c.hi) return Sync(start, size);       // not in one cluster (yet): alone
            if (c.epoch != epoch) { c.epoch = epoch; return Sync(c.lo << 12, (c.hi - c.lo) << 12); }
            g_diagSyncClustered.fetch_add(1, std::memory_order_relaxed);
        }
        else g_diagSyncSkipped.fetch_add(1, std::memory_order_relaxed);
        static const bool verify = [] { const char* e = std::getenv("TTT2_DIAG_VERIFY"); return e && *e == '1'; }();
        if (verify && size && usable_)
        {
            // Written inside the batch, or (p_diag45) since its cluster was synced? (Read the bits without resetting them.)
            const uint64_t first = start >> 12, last = (uint64_t(start) + size + 4095) >> 12;
            std::vector<PVOID> a(size_t(last - first));
            ULONG_PTR count = a.size();
            DWORD granularity = 0;
            if (GetWriteWatch(0, g_base + (first << 12), size_t(last - first) << 12, a.data(), &count, &granularity) == 0 && count)
            {
                g_diagSyncMissed.fetch_add(1, std::memory_order_relaxed);
                static int logged = 0;      // p108 (TEST ONLY): which range, which pages
                if (logged++ < 40)
                    std::fprintf(stderr, "[verify] skipped range %08X+%X written inside its batch %u: %u page(s), first %08X\n", start, size,
                                 epoch, unsigned(count), uint32_t(static_cast<uint8_t*>(a[0]) - g_base));
            }
        }
        return seq_;
    }
    uint32_t Sync(uint32_t start, uint32_t size)
    {
        LARGE_INTEGER q0, q1;
        QueryPerformanceCounter(&q0);
        const uint32_t r = SyncImpl(start, size);
        QueryPerformanceCounter(&q1);
        g_diagSyncTicks.fetch_add(q1.QuadPart - q0.QuadPart, std::memory_order_relaxed);
        g_diagSyncCalls.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    uint32_t SyncImpl(uint32_t start, uint32_t size)
    {
        if (!size) return seq_;
        const uint64_t first = start >> 12, last = (uint64_t(start) + size + 4095) >> 12;
        if (pageSeq_.empty()) { pageSeq_.assign(1u << 20, 0); blockSeq_.assign(1u << 14, 0); }
        const size_t pages = size_t(last - first);
        if (addresses_.size() < pages) addresses_.resize(pages);
        ULONG_PTR count = pages;
        DWORD granularity = 0;
        if (!usable_ || DirtyMeasureOn()) { MarkAll(first, last); return seq_; }
        UINT r = GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_base + (first << 12), pages << 12,
                               addresses_.data(), &count, &granularity);
        if (r != 0)
        {
            usable_ = false;      // guest memory without MEM_WRITE_WATCH: treat as always dirty
            std::fprintf(stderr, "[gpu-hw] write watch unavailable (%lu); textures re-upload on every use\n",
                         GetLastError());
            MarkAll(first, last);
            return seq_;
        }
        {
            // TEST ONLY (p_diag44, TTT2_DIAG_WWDUMP=1): every sync while C:/ttt2/port/fb/wwdump.arm exists - batch, range,
            // pages written - for measuring how many calls merged ranges would save.
            static FILE* dump = [] { const char* e = std::getenv("TTT2_DIAG_WWDUMP"); return e && *e == '1' ? std::fopen("C:/ttt2/port/fb/wwdumpT.txt", "w") : nullptr; }();
            static bool armed = false;
            static uint32_t calls = 0;
            if (dump)
            {
                if ((calls++ & 255) == 0) armed = std::filesystem::exists("C:/ttt2/port/fb/wwdump.arm");
                if (armed) std::fprintf(dump, "%u %08X %X %u\n", ::g_cpSyncEpoch.load(std::memory_order_relaxed), start, size, unsigned(count));
            }
        }
        WriteWatchPublish(0, addresses_.data(), count);       // consumer 0: the GPU backend
        shared_.clear();
        WriteWatchDrain(0, uint32_t(first), uint32_t(last), shared_);   // pages rollback.cpp read
        if (count || !shared_.empty())
        {
            ++seq_;
            for (ULONG_PTR i = 0; i < count; ++i)
            {
                const uint64_t page = (static_cast<uint8_t*>(addresses_[i]) - g_base) >> 12;
                if (page < pageSeq_.size()) { pageSeq_[page] = seq_; blockSeq_[page >> 6] = seq_; }
            }
            for (uint32_t page : shared_)
                if (page < pageSeq_.size()) { pageSeq_[page] = seq_; blockSeq_[page >> 6] = seq_; }
        }
        return seq_;
    }
    // Highest write sequence on any page of the range (Sync it first). p109: whole 64-page blocks (256 KB) by their
    // own highest sequence - exact, as every page write sets its block's too (sequences only grow); a texture bind
    // looked at every page of the texture (a 2 MB one: 512).
    uint32_t Newest(uint32_t start, uint32_t size) const
    {
        if (pageSeq_.empty() || !size) return 0;
        uint64_t p = start >> 12;
        const uint64_t e = std::min<uint64_t>((uint64_t(start) + size + 4095) >> 12, pageSeq_.size());
        uint32_t newest = 0;
        if (g_newestBlocksOff || VerifyBlocks())
            for (uint64_t q = p; q < e; ++q) newest = std::max(newest, pageSeq_[q]);
        if (g_newestBlocksOff) return newest;
        uint32_t fast = 0;
        for (; p < e && (p & 63); ++p) fast = std::max(fast, pageSeq_[p]);
        for (; p + 64 <= e; p += 64) fast = std::max(fast, blockSeq_[p >> 6]);
        for (; p < e; ++p) fast = std::max(fast, pageSeq_[p]);
        if (VerifyBlocks())
        {
            static uint64_t checked = 0, bad = 0;   // TEST ONLY (TTT2_DIAG_VERIFY)
            if (fast != newest && bad++ < 20)
                std::fprintf(stderr, "[verify] newest write by blocks differs: %08X+%X %u, page by page %u\n", start, size, fast, newest);
            if (++checked % 5000000 == 0)
                std::fprintf(stderr, "[verify] %llu newest-write lookups by blocks checked, %llu differ\n",
                             (unsigned long long)checked, (unsigned long long)bad);
        }
        return fast;
    }
    static bool VerifyBlocks()
    {
        static const bool v = [] { const char* e = std::getenv("TTT2_DIAG_VERIFY"); return e && *e == '1'; }();
        return v;
    }
    uint32_t Current() const { return seq_; }
private:
    void MarkAll(uint64_t first, uint64_t last)
    {
        ++seq_;
        for (uint64_t p = first; p < last && p < pageSeq_.size(); ++p) { pageSeq_[p] = seq_; blockSeq_[p >> 6] = seq_; }
    }
    std::vector<uint32_t> pageSeq_;
    std::vector<uint32_t> blockSeq_;              // p109: the highest sequence of each 64 pages
    std::vector<PVOID> addresses_;
    std::vector<uint32_t> shared_;
    uint32_t seq_ = 1;
    bool usable_ = true;
    std::unordered_set<uint64_t> once_;           // ranges synced in this batch (SyncOnce)
    uint32_t onceEpoch_ = 0;
    // p_diag45: write-watch clusters. Measured (wwdump, Tulip + Extravagant fights): ~40 syncs a batch over only ~585
    // distinct ranges, 0.2% finding writes; a call costs ~0.26 us + ~2 ns a page here (tools2/wwbench), ~1.4 us a call
    // on the player's PC. Adjoining ranges (no gap: only pages that are texture data) as one call: -47% calls.
    struct Cluster { uint32_t lo, hi, epoch; };   // pages [lo, hi); epoch: the batch it was last synced in
    std::vector<Cluster> clusters_;
    std::unordered_map<uint64_t, uint32_t> recent_;   // range -> the batch it was last asked about
    uint32_t clustersBuilt_ = 0;
    static bool ClustersOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_WW_CLUSTERS"); return !(e && *e == '0'); }();
        return on && g_wwClustersOn;
    }
    // From the ranges asked about in the last ~4,000 batches (a few seconds), so memory that stopped holding
    // textures leaves the clusters: syncing resets pages, and a page the game writes often would fault again.
    void RebuildClusters(uint32_t epoch)
    {
        clustersBuilt_ = epoch;
        std::vector<std::pair<uint32_t, uint32_t>> spans;
        spans.reserve(recent_.size());
        for (auto it = recent_.begin(); it != recent_.end();)
        {
            if (epoch - it->second > 4096) { it = recent_.erase(it); continue; }
            const uint32_t st = uint32_t(it->first >> 32), n = uint32_t(it->first);
            spans.push_back({ st >> 12, uint32_t((uint64_t(st) + n + 4095) >> 12) });
            ++it;
        }
        std::sort(spans.begin(), spans.end());
        clusters_.clear();
        for (const auto& [lo, hi] : spans)
        {
            if (!clusters_.empty() && lo <= clusters_.back().hi && std::max(hi, clusters_.back().hi) - clusters_.back().lo <= 4096)
                clusters_.back().hi = std::max(clusters_.back().hi, hi);       // adjoining or overlapping, up to 16 MB
            else if (!clusters_.empty() && lo < clusters_.back().hi)
                continue;                                                       // overlaps a full cluster: synced alone
            else
                clusters_.push_back({ lo, hi, 0 });
        }
    }
};

// One CPU-assembled vertex in clip space plus the 16 interpolators (vertex-shader exports).
struct HwVertex { float pos[4]; float attr[16][4]; };

struct DrawState
{
    // colorBase: EDRAM base (RB_COLOR_INFO bits 0-11) | colour format (bits 16-19) << 12.
    uint32_t pitch = 0, sampX = 1, sampY = 1, colorBase = 0, depthBase = 0, writeMask = 0;
    bool zEnable = false, zWrite = false;
    uint32_t zFunc = 7;
    uint32_t blend = 0x00010001;
    float blendConst[4] = { 0, 0, 0, 0 };
    int scX0 = 0, scY0 = 0, scX1 = 0, scY1 = 0;
    bool alphaTest = false;
    uint32_t alphaFunc = 7;
    float alphaRef = 0;
    uint32_t cull = 0;            // PA_SU_SC_MODE_CNTL bits 0/1: cull front/back
    bool frontIsCW = false;       // bit 2: clockwise (on screen) is the front face
    bool depthClip = false;       // PA_CL_CLIP_CNTL clipping enabled: clip 0 <= z <= w like Xenos
    // Stencil: RB_DEPTHCONTROL (0x2200) bit 0 enable, bit 7 separate back-face state, funcs and
    // ops in bits 8-31; RB_STENCILREFMASK (0x210D) and _BF (0x210C): ref, read mask, write mask.
    bool stencil = false;
    uint32_t depthCtl = 0, stencilRefMask = 0, stencilRefMaskBf = 0;
};

// The logical EDRAM surface space in samples (the vertex transform's frame): wide enough for
// pitch 2048 at 2x horizontal samples, as tall as the CPU store's 2048 sample rows. Textures
// hold only the part in use, times the render scale (Backend::Grow).
constexpr UINT SurfaceW = 4096, SurfaceH = 2048;

struct PsStateCB
{
    uint32_t boolConsts[8];
    uint32_t loopConsts[32];
    uint32_t texInfo[32][4];
    float texScale[32][4];
    uint32_t texSampler[32][4];      // ClampMode u, v; bilinear; border colour (XAddr / TexAddress)
    float alphaRef; uint32_t alphaTest, alphaFunc, pad;
};

// Fetch-constant sampler state in the translator's texSampler layout.
inline void SamplerInfo(const Texture& t, uint32_t out[4])
{
    out[0] = t.valid ? t.clampX : 2u; out[1] = t.valid ? t.clampY : 2u;
    out[2] = t.valid && t.magFilter == 1 ? 1u : 0u; out[3] = t.valid ? t.border : 0u;
}

// The fetch constant's minification state for the D3D11 sampler (the tfetch instructions all defer to
// it): bit 0 min filter linear, 1-2 mip filter (0 point, 1 linear, 2 none), 3-5 anisotropic filter
// (0 off, 1..5 = 1:1..16:1), 6-9 / 10-13 first / last level (the last clamped to the levels the texture
// holds), 14-23 the LOD bias in 1/32 steps (signed; Xenia's sampler MipLODBias = lod_bias / 32).
inline uint32_t MipSampling(const Texture& t, uint32_t levels)
{
    const uint32_t maxLevel = levels > 1 && t.mipFilter != 2 ? std::min<uint32_t>(t.mipMax, levels - 1) : 0;
    return (t.minFilter == 0 ? 0u : 1u) | (std::min<uint32_t>(t.mipFilter, 2) << 1) | (std::min<uint32_t>(t.aniso, 5) << 3) |
           (std::min(t.mipMin, maxLevel) << 6) | (maxLevel << 10) | ((uint32_t(t.lodBias) & 0x3FF) << 14);
}

// The D3D11 sampler for that state (mirror-clamp-to-halfway/border as mirror once, as Xenia).
// userAniso: Options -> Display Settings' anisotropic filtering (0 off), for linear-minified mipped textures.
// userTrilinear: TEXTURE FILTERING's TRILINEAR and up: mipped linear textures blend two levels (many fetch constants pick one).
inline D3D11_SAMPLER_DESC SamplerDesc(const uint32_t info[4], uint32_t mip = 0, uint32_t userAniso = 0, bool userTrilinear = false)
{
    auto mode = [](uint32_t m) {
        switch (m)
        {
        case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
        case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case 3: case 5: case 7: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
        case 6: return D3D11_TEXTURE_ADDRESS_BORDER;
        default: return D3D11_TEXTURE_ADDRESS_CLAMP;
        }
    };
    D3D11_SAMPLER_DESC s{};
    const bool minLinear = mip & 1;
    const uint32_t mipMode = (mip >> 1) & 3, aniso = (mip >> 3) & 7, minLevel = (mip >> 6) & 0xF, maxLevel = (mip >> 10) & 0xF;
    const int32_t bias = int32_t(mip << 8) >> 22;
    s.Filter = D3D11_ENCODE_BASIC_FILTER(minLinear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                         info[2] ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                         mipMode == 1 || (userTrilinear && mipMode == 0 && maxLevel > 0 && minLinear)
                                             ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                         D3D11_FILTER_REDUCTION_TYPE_STANDARD);
    const uint32_t maxAniso = std::max<uint32_t>(aniso ? 1u << (aniso - 1) : 1u, userAniso);
    if (maxAniso > 1 && maxLevel > 0 && minLinear) { s.Filter = D3D11_FILTER_ANISOTROPIC; s.MaxAnisotropy = std::min<uint32_t>(maxAniso, 16); }
    s.AddressU = mode(info[0]); s.AddressV = mode(info[1]); s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MinLOD = float(std::min(minLevel, maxLevel)); s.MaxLOD = float(maxLevel);
    s.MipLODBias = std::clamp(float(bias) / 32.0f, -16.0f, 15.99f);
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    for (float& c : s.BorderColor) c = info[3] == 1 ? 1.0f : 0.0f;
    return s;
}

class Backend
{
    struct ColorSurface;
public:
    // ----------------------------------------------------------------- lifetime
    void Initialize()
    {
        if (device_) return;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (const char* d = std::getenv("TTT2_GPU_HW_DEBUG"); d && std::strcmp(d, "1") == 0)
            flags |= D3D11_CREATE_DEVICE_DEBUG;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL actual{};
        const ComPtr<IDXGIAdapter1> preferred = PreferredGpuAdapter();
        HRESULT created = D3D11CreateDevice(preferred.Get(), preferred ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                            levels, 2, D3D11_SDK_VERSION, &device_, &actual, &context_);
        if (FAILED(created) && preferred)               // that adapter refused: the default one, as before
            created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device_, &actual, &context_);
        Check(created, "D3D11CreateDevice");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adesc{};
        if (SUCCEEDED(device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
            adapter->GetDesc(&adesc);
        char name[3 * 128 + 1] = {};   // UTF-8: up to 3 bytes for each of the description's 128 units
        WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
        std::fprintf(stderr, "[gpu-hw] active on %s (feature level %X), mode=%s\n", name, (unsigned)actual,
                     RequestedMode() == Mode::Compare ? "compare" : "on");
        ComPtr<ID3D11Device5> device5;
        const char* fenceEnv = std::getenv("TTT2_READBACK_FENCE");
        if (!(fenceEnv && *fenceEnv == '0') && SUCCEEDED(device_.As(&device5)) && SUCCEEDED(context_.As(&context4_)) &&
            SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&readbackFence_))))
            std::fprintf(stderr, "[gpu-hw] CPU readbacks wait on a GPU fence\n");
        else { context4_.Reset(); readbackFence_.Reset(); }

        D3D11_BUFFER_DESC b{};
        b.ByteWidth = VertexRingBytes; b.Usage = D3D11_USAGE_DYNAMIC; b.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device_->CreateBuffer(&b, nullptr, &vertexRing_), "CreateVertexRing");
        b.ByteWidth = IndexRingBytes; b.BindFlags = D3D11_BIND_INDEX_BUFFER;
        Check(device_->CreateBuffer(&b, nullptr, &indexRing_), "CreateIndexRing");
        b.ByteWidth = 256 * 16; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device_->CreateBuffer(&b, nullptr, &psConstants_), "CreatePsConstants");
        b.ByteWidth = sizeof(PsStateCB);
        Check(device_->CreateBuffer(&b, nullptr, &psState_), "CreatePsState");
        // GPU vertex stage: 512 vertex ALU constants, per-draw state, and a raw ring holding
        // each draw's guest vertex data (host-order dwords).
        b.ByteWidth = 512 * 16;
        Check(device_->CreateBuffer(&b, nullptr, &vsConstants_), "CreateVsConstants");
        b.ByteWidth = sizeof(VsStateCB);
        Check(device_->CreateBuffer(&b, nullptr, &vsState_), "CreateVsState");
        b.ByteWidth = VdataRingBytes; b.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_INDEX_BUFFER;
        b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        // p_diag44: the ring is also the GPU-vertex draws' index buffer (one Map a draw for both); a driver refusing
        // the combination gets the old separate index ring.
        dataRingIbOk_ = SUCCEEDED(device_->CreateBuffer(&b, nullptr, &vdataRing_));
        if (!dataRingIbOk_) { b.BindFlags = D3D11_BIND_SHADER_RESOURCE; Check(device_->CreateBuffer(&b, nullptr, &vdataRing_), "CreateVertexDataRing"); }
        b.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC rv{};
        rv.Format = DXGI_FORMAT_R32_TYPELESS; rv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        rv.BufferEx.NumElements = VdataRingBytes / 4; rv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        Check(device_->CreateShaderResourceView(vdataRing_.Get(), &rv, &vdataSrv_), "CreateVertexDataSrv");
        {
            // NO_OVERWRITE on a dynamic buffer read through an SRV needs this D3D11.1 option;
            // without it every draw's vertex data is written with DISCARD at offset 0.
            D3D11_FEATURE_DATA_D3D11_OPTIONS o{};
            vdataNoOverwrite_ = SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o))) &&
                                o.MapNoOverwriteOnDynamicBufferSRV;
        }
        {
            // p103: the index cache's store, in the GPU's own memory (UpdateSubresource when a list is first kept).
            D3D11_BUFFER_DESC ix{};
            ix.ByteWidth = IxStoreBytes; ix.Usage = D3D11_USAGE_DEFAULT; ix.BindFlags = D3D11_BIND_INDEX_BUFFER;
            if (FAILED(device_->CreateBuffer(&ix, nullptr, &ixStore_))) ixStore_.Reset();
            std::fprintf(stderr, "[gpu-hw] index cache: %s\n", !ixStore_ ? "off (no store)" : IxCacheUsable() ? "on" : "off");
        }
        static const bool gpuVs = [] { const char* e = std::getenv("TTT2_GPU_VS"); return !e || std::strcmp(e, "0") != 0; }();
        gpuVsEnabled_ = gpuVs;
        std::fprintf(stderr, "[gpu-hw] vertex shaders on the %s (vertex-data ring %s)\n", gpuVs ? "GPU" : "CPU (TTT2_GPU_VS=0)",
                     vdataNoOverwrite_ ? "no-overwrite" : "discard per draw");
        {
            static const bool want = [] { const char* e = std::getenv("TTT2_DATA_RING"); return !(e && *e == '0'); }();
            dataRingOk_ = want && dataRingIbOk_ && vdataNoOverwrite_;
            dataRingIndices_ = dataRingOk_;
            std::fprintf(stderr, "[gpu-hw] GPU-vertex draws' indices: %s\n", dataRingOk_ ? "in the vertex-data ring (one map a draw)"
                         : !want ? "own ring (TTT2_DATA_RING=0)" : !dataRingIbOk_ ? "own ring (the driver refused a raw index buffer)"
                         : "own ring (no no-overwrite vertex data)");
        }

        D3D11_RASTERIZER_DESC r{};
        r.FillMode = D3D11_FILL_SOLID; r.CullMode = D3D11_CULL_NONE;   // culled on the CPU
        r.DepthClipEnable = FALSE;                                     // the CPU path never clips z
        r.ScissorEnable = TRUE;
        Check(device_->CreateRasterizerState(&r, &raster_), "CreateRasterizerState");

        D3D11_SAMPLER_DESC s{};
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX; s.ComparisonFunc = D3D11_COMPARISON_NEVER;
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        Check(device_->CreateSamplerState(&s, &pointClamp_), "CreatePointSampler");
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        Check(device_->CreateSamplerState(&s, &linearClamp_), "CreateLinearSampler");

        // The interpreter's answer for an invalid texture fetch constant: (0, 0, 0, 1).
        const float black[4] = { 0, 0, 0, 1 };
        D3D11_TEXTURE2D_DESC t{};
        t.Width = t.Height = 1; t.MipLevels = t.ArraySize = 1; t.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_IMMUTABLE; t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{ black, 16, 16 };
        ComPtr<ID3D11Texture2D> tex;
        Check(device_->CreateTexture2D(&t, &init, &tex), "CreateDefaultTexture");
        Check(device_->CreateShaderResourceView(tex.Get(), nullptr, &defaultSrv_), "CreateDefaultSrv");
        {
            const float white[4] = { 1, 1, 1, 1 };
            D3D11_SUBRESOURCE_DATA wi{ white, 16, 16 };
            ComPtr<ID3D11Texture2D> wt;
            Check(device_->CreateTexture2D(&t, &wi, &wt), "CreateWhiteTexture");
            Check(device_->CreateShaderResourceView(wt.Get(), nullptr, &whiteSrv_), "CreateWhiteSrv");
            D3D11_SHADER_RESOURCE_VIEW_DESC wv{};
            wv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; wv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            wv.Texture2DArray.MipLevels = 1; wv.Texture2DArray.ArraySize = 1;
            Check(device_->CreateShaderResourceView(wt.Get(), &wv, &whiteArraySrv_), "CreateWhiteArraySrv");
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC av{};
        av.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; av.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        av.Texture2DArray.MipLevels = 1; av.Texture2DArray.ArraySize = 1;
        Check(device_->CreateShaderResourceView(tex.Get(), &av, &defaultArraySrv_), "CreateDefaultArraySrv");

        SetUpConstantRing();
        shaderCacheDir_ = ShaderCacheDirectory();
        lastStats_ = std::chrono::steady_clock::now();
        StartShaderWarmup();
    }

    // ----------------------------------------------------------------- draw
    // verts[0..vertCount) are the draw's shaded vertices, extras follow them; slots index both
    // (three per triangle, already culled on the CPU). Only `interps` interpolators are sent.
    bool Draw(const uint32_t* regs, const std::vector<uint32_t>& psCode, const Vertex* verts, size_t vertCount,
              const std::vector<Vertex>& extras, const std::vector<uint32_t>& slots, uint32_t interps,
              const DrawState& st)
    {
        Initialize();
        UpdateScale();
        if (ShadowCaster(st)) { ++stats_.shadowSkips; return true; }
        ++stats_.draws;
        ColorSurface& color = Color(st.colorBase);
        CensusTarget(st.colorBase);
        color.touched = true;
        color.samplePitch = st.pitch * st.sampX;
        if (slots.empty()) { ++stats_.emptyDraws; return true; }

        ID3D11PixelShader* ps = PixelShader(psCode);
        if (!ps) { ++stats_.skippedDraws; return false; }
        const uint32_t textureSlots = lastSlots_;

        // ---- vertices: window space -> clip space for the sample-space viewport
        DiagScope dCpu(diagTicks_[DpCpuVertices]);
        interps = std::min<uint32_t>(interps, 16);
        const size_t stride = 16 + 16 * size_t(interps);
        const size_t total = vertCount + extras.size();
        const size_t vbBytes = total * stride, ibBytes = slots.size() * 4;
        if (vbBytes + stride > VertexRingBytes || ibBytes > IndexRingBytes) { ++stats_.skippedDraws; return false; }
        // Keep each draw's vertices stride-aligned so the base vertex is an integer.
        size_t vbOffset = (ringOffset_ + stride - 1) / stride * stride;
        D3D11_MAP vbMap = D3D11_MAP_WRITE_NO_OVERWRITE;
        const bool vbWrap = vbOffset + vbBytes > VertexRingBytes;
        if (vbWrap) { vbOffset = 0; if (RingDiscard()) vbMap = D3D11_MAP_WRITE_DISCARD; }
        if (!RingDiscard()) RingWrite(vertexFence_, VertexRingBytes, vbOffset, vbBytes, vbWrap);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context_->Map(vertexRing_.Get(), 0, vbMap, 0, &mapped), "MapVertexRing");
        uint8_t* out = static_cast<uint8_t*>(mapped.pData) + vbOffset;
        const float hpx = float(SurfaceH / st.sampY), pitch = float(st.pitch);
        const bool noClip = (regs[0x2204] & 0x10000u) != 0;
        auto convert = [&](size_t i) {
            const Vertex& v = i < vertCount ? verts[i] : extras[i - vertCount];
            float* h = reinterpret_cast<float*>(out + i * stride);
            const float w = v.invW != 0.0f ? 1.0f / v.invW : 1.0f;
            // PA_CL_CLIP_CNTL.clip_disable: Xenos rasterises negative-W vertices unclipped; D3D
            // would clip them all, so submit the same NDC with a positive W (perspective weights
            // use |W|; attributes keep their true value). With clipping enabled a negative W is
            // clipped on Xenos as well, so it is left for D3D to clip.
            const float wc = (w < 0.0f && noClip) ? -w : w;
            h[0] = (v.x / pitch * 2.0f - 1.0f) * wc;
            h[1] = (1.0f - v.y / hpx * 2.0f) * wc;
            h[2] = v.z * wc;
            h[3] = wc;
            for (uint32_t a = 0; a < interps; ++a)
                for (int c = 0; c < 4; ++c) h[4 + a * 4 + c] = v.attr[a][c] * w;
        };
        if (total >= 2048)
        {
            const std::function<void(int)> row = [&](int i) { convert(size_t(i)); };
            RasterPool::Get().ForRows(0, (int)total, row);
        }
        else for (size_t i = 0; i < total; ++i) convert(i);
        context_->Unmap(vertexRing_.Get(), 0);
        const UINT baseVertex = UINT(vbOffset / stride);
        ringOffset_ = vbOffset + vbBytes;
        // How far right / down the draw can reach (logical pixels), for surface sizing: the
        // vertices' window positions bound every triangle unless one is clipped at W = 0
        // (negative W with clipping on). TTT2's EDRAM clears are unscissored screen-space quads.
        float reachX = std::numeric_limits<float>::infinity(), reachY = reachX;
        {
            float mx = 0.0f, my = 0.0f;
            bool bounded = true;
            for (size_t i = 0; i < total && bounded; ++i)
            {
                const Vertex& v = i < vertCount ? verts[i] : extras[i - vertCount];
                const float w = v.invW != 0.0f ? 1.0f / v.invW : 1.0f;
                bounded = (w > 0.0f || noClip) && std::isfinite(v.x) && std::isfinite(v.y);
                mx = std::max(mx, v.x); my = std::max(my, v.y);
            }
            if (bounded) { reachX = std::ceil(mx); reachY = std::ceil(my); }
        }
        dCpu.Stop();

        DiagScope dIdx(diagTicks_[DpIndices]);
        D3D11_MAP ibMap = D3D11_MAP_WRITE_NO_OVERWRITE;
        const bool ibWrap = indexOffset_ + ibBytes > IndexRingBytes;
        if (ibWrap) { indexOffset_ = 0; if (RingDiscard()) ibMap = D3D11_MAP_WRITE_DISCARD; }
        if (!RingDiscard()) RingWrite(indexFence_, IndexRingBytes, indexOffset_, ibBytes, ibWrap);
        Check(context_->Map(indexRing_.Get(), 0, ibMap, 0, &mapped), "MapIndexRing");
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + indexOffset_, slots.data(), ibBytes);
        context_->Unmap(indexRing_.Get(), 0);
        const UINT firstIndex = UINT(indexOffset_ / 4);
        indexOffset_ += ibBytes;
        dIdx.Stop();

        DiagScope dSt(diagTicks_[DpState]);
        PassthroughVs& pvs = Passthrough(interps);
        want_ = WantVertex{ pvs.layout.Get(), vertexRing_.Get(), UINT(stride), pvs.vs.Get(), false, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                            indexRing_.Get() };
        dSt.Stop();
        PixelStageAndDraw(regs, psCode, ps, textureSlots, color, st, UINT(slots.size()), firstIndex, INT(baseVertex), reachX, reachY);
        return true;
    }

    // ----------------------------------------------------------------- GPU vertex stage
    struct VsEntry { ComPtr<ID3D11VertexShader> vs; VsTranslation t; bool failed = false; };

    // The translated vertex shader for this microcode, or null when it cannot run on the GPU
    // (the caller keeps the CPU vertex stage for it).
    VsEntry* VertexShader(const std::vector<uint32_t>& code)
    {
        Initialize();
        DiagScope dSh(diagTicks_[DpShaders]);
        if (!gpuVsEnabled_ || code.empty()) return nullptr;
        const uint64_t key = VsKey(code);
        if (auto it = vertexShaders_.find(key); it != vertexShaders_.end()) return it->second.failed ? nullptr : &it->second;
        VsEntry e;
        if (TakePrepared(preparedVs_, key, e) || SharedVertexShader(code, e)) ++stats_.warmHits;
        else
        {
            Built<VsEntry> b;
            if (!AwaitBuild(pendingVs_, key, b, [this, code] {
                    Built<VsEntry> r;
                    r.result = BuildVertexShader(code, r.entry, r.counts, r.why, true);
                    return r;
                }, false))
                return nullptr;                 // still compiling: the CPU vertex stage draws it meanwhile
            stats_.diskHits += b.counts.diskHits; stats_.compiles += b.counts.compiles;
            if (b.result != BuildResult::Ready)
            {
                std::fprintf(stderr, b.result == BuildResult::Untranslatable ? "[gpu-hw] vertex shader %016llX (%zu dwords) stays on the CPU: %s\n"
                                                                             : "[gpu-hw] vertex shader %016llX (%zu dwords) failed to compile - it stays on the CPU:\n%s\n",
                             (unsigned long long)key, code.size(), b.why.c_str());
                vertexShaders_[key].failed = true;
                return nullptr;
            }
            e = std::move(b.entry);
            RecordMicrocode("vs", key, code);
        }
        if (VerifyOn())     // p77 (TEST ONLY): the constant registers recorded (the block's size) against the shader's text
        {
            static uint64_t checked = 0;
            static int logged = 0;
            const uint32_t byText = VsConstRegsInText(e.t.function);
            if (byText != e.t.constRegs && logged++ < 30)
                std::fprintf(stderr, "[verify] vertex shader %016llX: its text reads %u constant registers, %u recorded\n",
                             (unsigned long long)key, byText, e.t.constRegs);
            if (++checked % 20 == 0) std::fprintf(stderr, "[verify] %llu vertex shaders' constant registers checked\n", (unsigned long long)checked);
        }
        VsEntry& slot = vertexShaders_[key];
        slot = std::move(e);
        return &slot;
    }
    // p77 (TEST ONLY): [0, n) = the constant registers a translated vertex shader's body reads, from its text: c[N], or
    // all 512 with a relative read (XC).
    static uint32_t VsConstRegsInText(const std::string& s)
    {
        if (s.find("XC(") != std::string::npos) return 512;
        uint32_t n = 0;
        for (size_t p = s.find("c["); p != std::string::npos; p = s.find("c[", p + 2))
        {
            if (p && (std::isalnum(uint8_t(s[p - 1])) || s[p - 1] == '_')) continue;
            size_t q = p + 2;
            uint32_t v = 0;
            bool digits = false;
            while (q < s.size() && std::isdigit(uint8_t(s[q]))) { v = v * 10 + uint32_t(s[q] - '0'); ++q; digits = true; }
            if (digits && q < s.size() && s[q] == ']') n = std::max(n, v + 1);
        }
        return n;
    }

    enum class GpuVsResult { Drawn, Skipped, Fallback };

    // p103: the index cache. A GPU-vertex draw's indices are read from guest memory, byte-swapped, offset, clamped and
    // assembled into a triangle list or strip, then copied into the vertex-data ring - every draw, every frame. MEASURED
    // 2026-10-29 (p102 dump, Riverside Promenade fight): 563 index ranges a frame (1.1 MB of guest indices, 2.2 MB as
    // 32-bit in the ring), none changed in the whole fight, two thirds of them drawn again in the same frame (the
    // crowd); the low-end player's log 30 there: index read 1.16 ms a frame, and every MB written to the rings costs
    // ~0.45 ms. Kept instead: the list as drawn (16-bit when it fits), once, in a store in the GPU's memory; a draw
    // with the same index registers (address, count, format, primitive, restart, offset, clamp) whose pages the write
    // watch saw no write to since draws from there and reads nothing. The write watch is asked once per command batch
    // (DirtyTracker::SyncOnce, the texture binds' rule); pages written in the last two frames are read every time, as
    // the vertex data cache does (a sync resets their write watch). Auto-index draws (no memory) are kept too.
    // TTT2_INDEX_CACHE=0 off; TTT2_DIAG_VERIFY=1: every hit read and assembled again and compared.
    struct IxKey
    {
        uint32_t addr = 0, count = 0, offset = 0, minIndex = 0, maxIndex = 0, restart = 0, flags = 0;
        bool operator==(const IxKey& o) const { return std::memcmp(this, &o, sizeof(IxKey)) == 0; }
    };
    struct IxEntry
    {
        IxKey key;
        uint32_t byteOffset = 0, count = 0, lowest = 0, highest = 0, maxIdx = 0;
        uint32_t seq = 0, epoch = 0, lap = 0, hash = 0;
        bool used = false, valid = false, strip = false, wide = false;
    };
    struct IxFind { const IxEntry* hit = nullptr; IxEntry* slot = nullptr; uint32_t seq = 0; };
    static constexpr uint32_t IxSlots = 1u << 14, IxStoreBytes = 32u << 20;
    bool IxCacheUsable() const
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_INDEX_CACHE"); return !(e && *e == '0'); }();
        return on && ixStore_ && !ixAbOff_ && !FlashMax() && !PoisonOn() && !VcacheStatsOn() && !frame_overlap::CopiesPossible();
    }
    static uint32_t IxHash(const IxKey& k)
    {
        uint32_t w[7];
        std::memcpy(w, &k, sizeof(w));
        uint64_t h = 0x9E3779B97F4A7C15ull;
        for (uint32_t v : w) { h = (h ^ v) * 0xFF51AFD7ED558CCDull; h ^= h >> 32; }
        return uint32_t(h);
    }
    static uint32_t IxListHash(const std::vector<uint32_t>& list, uint32_t lowest, uint32_t highest, bool strip)
    {
        uint32_t h = 2166136261u;
        for (uint32_t v : list) { h ^= v; h *= 16777619u; }
        return h ^ (lowest * 0x9E3779B1u) ^ (highest * 0x85EBCA77u) ^ (strip ? 0x27D4EB2Fu : 0u);
    }
    // This draw's indices: a hit (draw from the store), or a slot to keep them in after the read (none: not kept -
    // written lately, or the cache is off).
    IxFind IndexCacheFind(const IxKey& k, uint32_t bytes)
    {
        IxFind r;
        if (!IxCacheUsable()) return r;
        if (ixTable_.empty()) ixTable_.resize(IxSlots);
        if (vdHot_.empty()) { vdHot_.assign(1u << 20, 0); vdCache_.reserve(1u << 16); }
        const uint32_t epoch = ::g_cpSyncEpoch.load(std::memory_order_relaxed);
        const uint32_t p0 = k.addr >> 12, p1 = k.addr ? uint32_t((uint64_t(k.addr) + bytes + 4095) >> 12) : p0;
        for (uint32_t p = p0; p < p1; ++p)
            if (vdHot_[p] > vdPresent_) { ++ixHot_; return r; }
        IxEntry* slot = nullptr;
        uint32_t i = IxHash(k) & (IxSlots - 1);
        for (uint32_t n = 0; n < 16; ++n, i = (i + 1) & (IxSlots - 1))
        {
            IxEntry& e = ixTable_[i];
            if (!e.used) { slot = &e; break; }
            if (!(e.key == k)) continue;
            slot = &e;
            const bool live = e.lap == ixLap_ || (e.lap + 1 == ixLap_ && e.byteOffset >= ixCursor_);   // not written over
            if (e.valid && live)
            {
                if (!k.addr || e.epoch == epoch) { ++ixHits_; r.hit = &e; return r; }
                dirty_.SyncOnce(k.addr, bytes);
                if (dirty_.Newest(k.addr, bytes) <= e.seq) { e.epoch = epoch; ++ixHits_; r.hit = &e; return r; }
                ++ixWritten_;
            }
            e.valid = false;
            break;
        }
        if (!slot) { slot = &ixTable_[IxHash(k) & (IxSlots - 1)]; slot->valid = false; ++ixEvicted_; }   // a full run: the first
        ++ixMisses_;
        if (k.addr)
        {
            dirty_.SyncOnce(k.addr, bytes);
            if (dirty_.Newest(k.addr, bytes) > vdSeqAt_[(vdPresent_ - 2) & 3])
            {
                for (uint32_t p = p0; p < p1; ++p) vdHot_[p] = vdPresent_ + 120;
                ++ixHot_;
                return r;
            }
        }
        r.slot = slot;
        r.seq = dirty_.Current();
        return r;
    }
    // After a miss with a slot: the list as drawn into the store (nullptr: not kept - draw as before).
    const IxEntry* IndexCacheStore(const IxFind& f, const IxKey& k, const std::vector<uint32_t>& list, uint32_t lowest,
                                   uint32_t highest, bool strip, uint32_t maxIdx)
    {
        if (!f.slot || !ixStore_ || !IxCacheUsable()) return nullptr;
        const bool wide = highest >= 0xFFFFu;          // a strip's cut is 0xFFFF in 16-bit lists
        const uint32_t bytes = uint32_t(list.size()) * (wide ? 4u : 2u);
        if (bytes > IxStoreBytes / 16) return nullptr;
        uint32_t at = (ixCursor_ + 15) & ~15u;
        if (at + bytes > IxStoreBytes) { at = 0; ++ixLap_; }
        if (bytes)
        {
            const void* src = list.data();
            if (!wide)
            {
                thread_local std::vector<uint16_t> narrow;
                narrow.resize(list.size());
                for (size_t n = 0; n < list.size(); ++n) narrow[n] = uint16_t(list[n]);
                src = narrow.data();
            }
            const D3D11_BOX box{ at, 0, 0, at + bytes, 1, 1 };
            context_->UpdateSubresource(ixStore_.Get(), 0, &box, src, 0, 0);
        }
        ixCursor_ = at + bytes;
        if (!f.slot->used) ++ixFilled_;
        IxEntry& e = *f.slot;
        e.key = k; e.byteOffset = at; e.count = uint32_t(list.size());
        e.lowest = lowest; e.highest = highest; e.maxIdx = maxIdx;
        e.seq = f.seq; e.epoch = ::g_cpSyncEpoch.load(std::memory_order_relaxed); e.lap = ixLap_;
        e.hash = VdVerifyOn() ? IxListHash(list, lowest, highest, strip) : 0;
        e.used = e.valid = true; e.strip = strip; e.wide = wide;
        ++ixStored_;
        ixStoredBytes_ += bytes;
        if (ixFilled_ > IxSlots / 2)        // half full: start again (the store's lists stay, unreferenced)
        {
            for (IxEntry& x : ixTable_) x = IxEntry{};
            ixFilled_ = 0;
            ++ixCleared_;
            return nullptr;
        }
        return &e;
    }
    // TEST ONLY (TTT2_DIAG_VERIFY=1): a hit against the same draw's indices read and assembled again.
    void IndexCacheCheck(const IxEntry& e, const std::vector<uint32_t>& list, uint32_t lowest, uint32_t highest, bool strip, uint32_t maxIdx)
    {
        ++ixVerified_;
        const bool same = e.count == list.size() && e.lowest == lowest && e.highest == highest && e.strip == strip &&
                          e.maxIdx == maxIdx && e.hash == IxListHash(list, lowest, highest, strip);
        if (!same && ixDiffer_++ < 20)
            std::fprintf(stderr, "[verify] index cache: %08X (%u indices, flags %X) kept %u / %u..%u%s, read now %zu / %u..%u%s\n",
                         e.key.addr, e.key.count, e.key.flags, e.count, e.lowest, e.highest, e.strip ? " strip" : "",
                         list.size(), lowest, highest, strip ? " strip" : "");
    }
    std::vector<IxEntry> ixTable_;
    uint32_t ixFilled_ = 0, ixCursor_ = 0, ixLap_ = 1;
    ComPtr<ID3D11Buffer> ixStore_;
    uint64_t ixHits_ = 0, ixMisses_ = 0, ixStored_ = 0, ixStoredBytes_ = 0, ixHot_ = 0, ixWritten_ = 0, ixEvicted_ = 0, ixCleared_ = 0;
    uint64_t ixVerified_ = 0, ixDiffer_ = 0;
    bool ixAbOff_ = false;
    // p104: DIAGNOSTIC BUILD A/B (TTT2_DIAG_IX_AB=1): the index cache (p106: and the sparse constant blocks; p107: the
    // texture memo across batches and the drop shortcut; p109: Newest by blocks) off in every other report window.
    static bool IxAbOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_IX_AB"); return e && *e == '1'; }();   // diag15: off (diag14 measured it)
        return on;
    }

    // Draw with the vertex shader on the GPU. `indices` are vertex indices, three per triangle
    // in the CPU assembly order (restarts already applied); minIndex..maxIndex bound the guest
    // vertex data uploaded. Fallback: nothing was changed, run the CPU vertex stage instead.
    GpuVsResult DrawGpuVs(const uint32_t* regs, const std::vector<uint32_t>& psCode, VsEntry& vs,
                          const std::vector<uint32_t>& indices, uint32_t minIndex, uint32_t maxIndex, const DrawState& st,
                          D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, const IxEntry* ix = nullptr)
    {
        Initialize();
        DiagScope dSu(diagTicks_[DpVsSetup]);
        UpdateScale();
        int64_t lo[96], hi[96];
        bool used[96];
        VsFetchRanges(vs.t, minIndex, maxIndex, lo, hi, used);
        CensusVertex(vs.t.fetches);
        uint64_t totalDwords = 0;
        for (uint32_t s = 0; s < 96; ++s)
            if (used[s])
            {
                if (hi[s] - lo[s] > int64_t(16u << 20) / 4) return GpuVsResult::Fallback;
                totalDwords += uint64_t(hi[s] - lo[s]);
            }
        if (totalDwords * 4 + 64 > VdataRingBytes || indices.size() * 4 > IndexRingBytes) return GpuVsResult::Fallback;
        const bool ringIndices = !ix && dataRingIndices_ && totalDwords * 4 + indices.size() * 4 + 64 <= VdataRingBytes;   // p_diag44
        if (toggle_ & 2) return GpuVsResult::Drawn;
        if (ShadowCaster(st)) { ++stats_.shadowSkips; return GpuVsResult::Drawn; }

        ++stats_.draws;
        ++stats_.gpuVsDraws;
        ColorSurface& color = Color(st.colorBase);
        CensusTarget(st.colorBase);
        color.touched = true;
        color.samplePitch = st.pitch * st.sampX;
        if (ix ? !ix->count : indices.empty()) { ++stats_.emptyDraws; return GpuVsResult::Drawn; }
        ID3D11PixelShader* ps = PixelShader(psCode);
        if (!ps) { ++stats_.skippedDraws; return GpuVsResult::Skipped; }
        const uint32_t textureSlots = lastSlots_;
        dSu.Stop();

        // ---- vertex data: each fetched range of guest memory as host-order dwords
        VsStateCB& vst = pendingVsState_;      // p_diag44: written to the constant ring with the pixel stage's blocks
        vst = VsStateCB{};
        UINT ringFirstIndex = 0;
        if (totalDwords || ringIndices)
        {
            DiagScope dVd(diagTicks_[DpVertexData]);
            // p73: slots whose range is still in the ring, not written since it was copied (VdCached), draw from there.
            // p76: decided before the ring space is taken, which is now only what is copied (the cached slots' share
            // left holes, so the ring went round - dropping its copies - faster); a draw that may span chunks copies all.
            const bool cacheOn = VdCacheUsable() && size_t(totalDwords) * 4 + indices.size() * 4 + 64 <= vdataFence_.chunk;
            VdEntry* entries[96] = {};
            bool cached[96] = {};
            for (uint32_t s = 0; s < 96 && cacheOn; ++s)
            {
                if (!used[s]) continue;
                const uint32_t fc0 = regs[0x4800 + s * 2];
                if ((fc0 & 3) != 3) continue;
                const uint32_t phys = fc0 & ~3u;
                const uint64_t guest = (phys && phys < 0x20000000u) ? (phys | 0xA0000000u) : phys;
                const int64_t first = int64_t(guest) + lo[s] * 4, count = hi[s] - lo[s];
                if (!count || first < 0x10000 || uint64_t(first) + uint64_t(count) * 4 > 0x100000000ull) continue;
                if (!VdCached(uint32_t(first), uint32_t(count), entries[s])) continue;
                cached[s] = true;
                vst.vfRegion[s][0] = int32_t(int64_t(entries[s]->dword) - lo[s]);
                vst.vfRegion[s][1] = int32_t(lo[s]);
                vst.vfRegion[s][2] = int32_t(hi[s]);
                vst.vfRegion[s][3] = 1;
                GpuNoteMemory(1, uint32_t(first), uint32_t(count) * 4);
                const uint32_t fcBytes = ((regs[0x4800 + s * 2 + 1] >> 2) & 0xFFFFFFu) * 4;
                frame_overlap::NoteBuffer(uint32_t(guest), fcBytes);
                if (uint64_t(first) + uint64_t(count) * 4 > guest + fcBytes)        // p75: drawn past its fetch constant
                    frame_overlap::NoteGrowing(uint32_t(guest), uint32_t(uint64_t(first) + uint64_t(count) * 4 - guest));
            }
            uint64_t copyDwords = 0;
            for (uint32_t s = 0; s < 96; ++s)
                if (used[s] && !cached[s]) copyDwords += uint64_t(hi[s] - lo[s]);
            D3D11_MAP map = D3D11_MAP_WRITE_NO_OVERWRITE;
            size_t start = (vdataOffset_ + 15) & ~size_t(15);
            const size_t ringBytes = size_t(copyDwords) * 4 + (ringIndices ? indices.size() * 4 : 0);   // p_diag44: indices after the data
            if (!vdataNoOverwrite_) { start = 0; map = D3D11_MAP_WRITE_DISCARD; }
            else if (ringBytes)
            {
                const bool wrap = start + ringBytes > VdataRingBytes;
                if (wrap) { start = 0; if (RingDiscard()) map = D3D11_MAP_WRITE_DISCARD; }
                if (!RingDiscard()) RingWrite(vdataFence_, VdataRingBytes, start, ringBytes, wrap);
            }
            // p76: the chunks this draw reads copies in, marked again when the ring next enters a chunk (after this draw).
            for (uint32_t s = 0; s < 96; ++s)
                if (cached[s])
                    for (size_t c = entries[s]->chunk0; c <= entries[s]->chunk1; ++c)
                        if (c != vdataFence_.cur) vdataFence_.reread[c] = true;
            const bool mapped = copyDwords || ringIndices;     // p73: all of it cached - nothing to map
            D3D11_MAPPED_SUBRESOURCE m{};
            if (mapped) Check(context_->Map(vdataRing_.Get(), 0, map, 0, &m), "MapVertexData");
            uint32_t* out = mapped ? static_cast<uint32_t*>(m.pData) + start / 4 : nullptr;
            uint64_t cursor = start / 4;
            uint32_t flashVh = 2166136261u;     // TEST ONLY (p_diag33b): FNV-1a of the guest vertex data copied
            for (uint32_t s = 0; s < 96; ++s)
            {
                if (!used[s] || cached[s]) continue;
                const uint32_t fc0 = regs[0x4800 + s * 2];
                if ((fc0 & 3) != 3) continue;                    // not a vertex fetch constant: w = 0
                const uint32_t phys = fc0 & ~3u;
                const uint64_t guest = (phys && phys < 0x20000000u) ? (phys | 0xA0000000u) : phys;
                const int64_t first = int64_t(guest) + lo[s] * 4, count = hi[s] - lo[s];
                vst.vfRegion[s][3] = 1;
                if (first < 0x10000 || uint64_t(first) + uint64_t(count) * 4 > 0x100000000ull)
                {
                    vst.vfRegion[s][1] = vst.vfRegion[s][2] = 0;     // unreadable: fetches read 0
                    continue;
                }
                // As the frame drawn sees it (the frame overlap's copies of pages the game wrote since).
                thread_local std::vector<uint8_t> overlapScratch;
                const uint32_t* src = reinterpret_cast<const uint32_t*>(
                    frame_overlap::CpReadBegin(uint32_t(first), uint32_t(count) * 4, overlapScratch));
                for (int64_t i = 0; i < count; ++i) out[i] = __builtin_bswap32(src[i]);
                GeoDump('V', uint32_t(first), uint32_t(count) * 4, src, s);     // TEST ONLY (p102)
                if (entries[s]) VdStore(*entries[s], cursor, uint32_t(count), src, src == reinterpret_cast<const uint32_t*>(g_base + first));
                if (FlashMax()) for (int64_t i = 0; i < count; ++i) { flashVh ^= src[i]; flashVh *= 16777619u; }
                frame_overlap::CpReadEnd();
                GpuNoteMemory(1, uint32_t(first), uint32_t(count) * 4);
                const uint32_t fcBytes = ((regs[0x4800 + s * 2 + 1] >> 2) & 0xFFFFFFu) * 4;
                frame_overlap::NoteBuffer(uint32_t(guest), fcBytes);   // the whole buffer
                if (uint64_t(first) + uint64_t(count) * 4 > guest + fcBytes)        // p75: drawn past its fetch constant
                    frame_overlap::NoteGrowing(uint32_t(guest), uint32_t(uint64_t(first) + uint64_t(count) * 4 - guest));
                if (PoisonOn()) PoisonScan("vertex", uint32_t(first), uint32_t(count) * 4, (uint64_t(st.colorBase) << 32) | s,
                                           (uint64_t(psCode.size()) << 32) | uint32_t(indices.size()));   // TEST ONLY (p_diag35)
                if (PoisonOn())     // TEST ONLY (p_diag35b): marker dwords the vertex shader actually fetches
                {
                    static std::set<std::pair<const void*, uint32_t>> seenFetch;
                    static int loggedFetch = 0;
                    for (uint32_t fi = 0; fi < vs.t.fetches.size(); ++fi)
                    {
                        const detail::VsFetch& f = vs.t.fetches[fi];
                        if (f.slot != s || !f.dwords) continue;
                        uint32_t hits = 0, verts = 0, firstV = 0;
                        for (uint32_t v = minIndex; v <= maxIndex && v - minIndex < 65536; ++v)
                        {
                            const int64_t d0 = int64_t(v) * f.stride + f.offset;
                            bool hit = false;
                            for (uint32_t k = 0; k < f.dwords; ++k)
                            {
                                const int64_t rel = d0 + k - lo[s];
                                if (rel >= 0 && rel < count && src[rel] == 0xADDEC07Fu) hit = true;
                            }
                            ++verts;
                            if (hit && !hits++) firstV = v;
                        }
                        static int waited = 0;     // TEST ONLY (p_diag35c): written moments later, or never?
                        if (hits && fi == 0 && waited < 80)
                        {
                            ++waited;
                            const int64_t rel = int64_t(firstV) * f.stride + f.offset - lo[s];
                            const volatile uint32_t* hv = src + rel;
                            const auto w0 = std::chrono::steady_clock::now();
                            double goneMs = -1.0;
                            while (std::chrono::steady_clock::now() - w0 < std::chrono::milliseconds(20))
                            {
                                if (hv[0] != 0xADDEC07Fu || hv[1] != 0xADDEC07Fu || hv[2] != 0xADDEC07Fu)
                                {
                                    goneMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
                                    break;
                                }
                                _mm_pause();
                            }
                            float pos[3];
                            for (int k = 0; k < 3; ++k) { const uint32_t u = __builtin_bswap32(hv[k]); std::memcpy(&pos[k], &u, 4); }
                            if (goneMs >= 0)
                                std::fprintf(stderr, "[poison-wait] #%d present %u guest %08X vertex %u (%u of %u marker, %zu indices): written %.3f ms after the draw, pos %g %g %g\n",
                                             waited, flashPresent_, uint32_t(guest), firstV, hits, verts, indices.size(), goneMs, pos[0], pos[1], pos[2]);
                            else
                                std::fprintf(stderr, "[poison-wait] #%d present %u guest %08X vertex %u (%u of %u marker, %zu indices): still marker after 20 ms\n",
                                             waited, flashPresent_, uint32_t(guest), firstV, hits, verts, indices.size());
                        }
                        if (hits && loggedFetch < 300 && seenFetch.insert({ vs.vs.Get(), fi }).second)
                        {
                            ++loggedFetch;
                            std::fprintf(stderr, "[poison-fetch] vs %p ps %p color %03X slot %u fetch %u: offset %d dwords %u stride %u format %u - "
                                                 "%u of %u vertices fetch the marker (first %u); %zu indices, guest %08X\n",
                                         (const void*)vs.vs.Get(), (const void*)ps, st.colorBase, s, fi, f.offset, f.dwords, f.stride, f.format,
                                         hits, verts, firstV, indices.size(), uint32_t(guest));
                        }
                    }
                }
                if (VcacheStatsOn()) VcacheStat(uint32_t(first), uint32_t(count) * 4);
                vst.vfRegion[s][0] = int32_t(int64_t(cursor) - lo[s]);
                vst.vfRegion[s][1] = int32_t(lo[s]);
                vst.vfRegion[s][2] = int32_t(hi[s]);
                out += count;
                cursor += uint64_t(count);
            }
            if (FlashMax()) { flashVertexHash_ = flashVh; flashVertexDwords_ = uint32_t(cursor - start / 4); }   // TEST ONLY (p_diag33)
            if (ringIndices)
            {
                std::memcpy(static_cast<uint32_t*>(m.pData) + cursor, indices.data(), indices.size() * 4);
                ringFirstIndex = UINT(cursor);
                cursor += indices.size();
            }
            if (mapped)
            {
                context_->Unmap(vdataRing_.Get(), 0);
                vdataOffset_ = size_t(cursor) * 4;
            }
            stats_.vertexDataBytes += copyDwords * 4;
        }

        // ---- vertex constants and state
        DiagScope dVc(diagTicks_[DpConstants]);
        vsCbConsts_ = cbPartialOk_ ? std::clamp<uint32_t>((vs.t.constRegs + 15) & ~15u, 16, 512) : 512;    // p77
        // Compared only after a constant register write (~450 draws a frame, 8 + 4 KB each, MEASURED 2026-10-25).
        static const bool verifyConstants = [] { const char* e = std::getenv("TTT2_DIAG_VERIFY"); return e && *e == '1'; }();
        const bool vsWritten = vsConstWritesSeen_ != g_vsConstWrites;
        if (verifyConstants && !vsWritten && vsConstantsValid_ && std::memcmp(lastVsConstants_.data(), regs + 0x4000, 512 * 16) != 0)
        {
            static int logged = 0;
            if (logged++ < 20) std::fprintf(stderr, "[verify] vs constants changed without a register write\n");
        }
        vsConstWritesSeen_ = g_vsConstWrites;
        // p_diag46: only the written registers (dwords [vlo, vhi)); the rest still equals the last copy.
        const uint32_t vlo = vsConstantsValid_ ? (g_vsConstLo - 0x4000) & ~3u : 0, vhi = vsConstantsValid_ ? ((g_vsConstHi - 0x4000) | 3u) + 1 : 2048;
        g_vsConstLo = 0x4800; g_vsConstHi = 0x4000;
        if ((vsWritten || !vsConstantsValid_) && vlo < vhi &&
            (std::memcmp(lastVsConstants_.data() + vlo, regs + 0x4000 + vlo, size_t(vhi - vlo) * 4) != 0 || !vsConstantsValid_))
        {
            ++diagVsCbUploads_;
            if (verifyConstants)    // p_diag36: the "registers changed" count only in verify runs (512 compares an upload)
            {
                const uint32_t* a = reinterpret_cast<const uint32_t*>(lastVsConstants_.data());
                uint32_t lo = 512, hi = 0, n = 0;
                for (uint32_t r = 0; r < 512; ++r)
                    if (std::memcmp(a + r * 4, regs + 0x4000 + r * 4, 16) != 0) { ++n; lo = std::min(lo, r); hi = std::max(hi, r); }
                diagVsRegsChanged_ += n;
                if (n) diagVsSpan_ += hi - lo + 1;
            }
            std::memcpy(lastVsConstants_.data() + vlo, regs + 0x4000 + vlo, size_t(vhi - vlo) * 4);
            if (verifyConstants && std::memcmp(lastVsConstants_.data(), regs + 0x4000, 512 * 16) != 0)
            {
                static int logged = 0;
                if (logged++ < 20) std::fprintf(stderr, "[verify] vs constants differ outside the written registers\n");
            }
            if (cbRingOn_) pendingVsConst_ = true;           // p_diag44: into the ring with the other blocks
            else Upload(vsConstants_.Get(), lastVsConstants_.data(), 512 * 16);
            vsConstantsValid_ = true;
        }
        std::memcpy(vst.boolConsts, regs + 0x4900, sizeof(vst.boolConsts));
        std::memcpy(vst.loopConsts, regs + 0x4908, sizeof(vst.loopConsts));
        vst.vte = regs[0x2206];
        vst.xs = AsFloat(regs[0x210F]); vst.xo = AsFloat(regs[0x2110]);
        vst.ys = AsFloat(regs[0x2111]); vst.yo = AsFloat(regs[0x2112]);
        if (FramesOn()) FrNoteViewport(vst.xs, vst.ys);
        vst.zs = AsFloat(regs[0x2113]); vst.zo = AsFloat(regs[0x2114]);
        vst.pitch = float(st.pitch);
        vst.rows = float(SurfaceH / st.sampY);
        vst.noClip = (regs[0x2204] >> 16) & 1;
        VsFetchDescriptors(vs.t, vst.vfDesc);
        cbVsSlotMask_[0] = cbVsSlotMask_[1] = 0;            // p105: the parts of the block this shader reads
        for (const detail::VsFetch& f : vs.t.fetches)
            if (f.slot < 96) cbVsSlotMask_[f.slot >> 6] |= 1ull << (f.slot & 63);
        cbVsFetches_ = uint32_t(std::min<size_t>(vs.t.fetches.size(), detail::kMaxVsFetches));
        if (!cbRingOn_) Upload(vsState_.Get(), &vst, sizeof(vst));
        dVc.Stop();

        // ---- indices
        if (ix)                 // p103: the index cache's store
        {
            DiagScope dSt0(diagTicks_[DpState]);
            want_ = WantVertex{ nullptr, nullptr, 0, vs.vs.Get(), true, topology, ixStore_.Get(), ix->wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT };
            dSt0.Stop();
            PixelStageAndDraw(regs, psCode, ps, textureSlots, color, st, UINT(ix->count), ix->byteOffset / (ix->wide ? 4u : 2u), 0,
                              std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
            return GpuVsResult::Drawn;
        }
        if (ringIndices)        // p_diag44: already in the vertex-data ring
        {
            DiagScope dSt0(diagTicks_[DpState]);
            want_ = WantVertex{ nullptr, nullptr, 0, vs.vs.Get(), true, topology, vdataRing_.Get() };
            dSt0.Stop();
            PixelStageAndDraw(regs, psCode, ps, textureSlots, color, st, UINT(indices.size()), ringFirstIndex, 0,
                              std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
            return GpuVsResult::Drawn;
        }
        DiagScope dIdx(diagTicks_[DpIndices]);
        const size_t ibBytes = indices.size() * 4;
        D3D11_MAP ibMap = D3D11_MAP_WRITE_NO_OVERWRITE;
        const bool ibWrap = indexOffset_ + ibBytes > IndexRingBytes;
        if (ibWrap) { indexOffset_ = 0; if (RingDiscard()) ibMap = D3D11_MAP_WRITE_DISCARD; }
        if (!RingDiscard()) RingWrite(indexFence_, IndexRingBytes, indexOffset_, ibBytes, ibWrap);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context_->Map(indexRing_.Get(), 0, ibMap, 0, &mapped), "MapIndexRing");
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + indexOffset_, indices.data(), ibBytes);
        context_->Unmap(indexRing_.Get(), 0);
        const UINT firstIndex = UINT(indexOffset_ / 4);
        indexOffset_ += ibBytes;
        dIdx.Stop();

        DiagScope dSt(diagTicks_[DpState]);
        want_ = WantVertex{ nullptr, nullptr, 0, vs.vs.Get(), true, topology, indexRing_.Get() };
        dSt.Stop();
        PixelStageAndDraw(regs, psCode, ps, textureSlots, color, st, UINT(indices.size()), firstIndex, 0,
                          std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
        return GpuVsResult::Drawn;
    }

    // Shared tail of both vertex paths (vertex input already bound): pixel-shader constants,
    // textures and samplers, output merger, viewport, scissor, rasterizer state, the draw.
    // reachX / reachY: how far the geometry can reach in logical pixels (infinity: unknown).
    void PixelStageAndDraw(const uint32_t* regs, const std::vector<uint32_t>& psCode, ID3D11PixelShader* ps,
                           uint32_t textureSlots, ColorSurface& color, const DrawState& st,
                           UINT indexCount, UINT firstIndex, INT baseVertex, float reachX, float reachY)
    {
        // ---- constants and per-draw shader state
        DiagScope dPc(diagTicks_[DpConstants]);
        cbPsSlots_ = textureSlots;                          // p105
        psCbConsts_ = CbSparse() ? std::clamp<uint32_t>((lastPsConstRegs_ + 15) & ~15u, 16, 256) : 256;
        static const bool verifyConstants = [] { const char* e = std::getenv("TTT2_DIAG_VERIFY"); return e && *e == '1'; }();
        const bool psWritten = psConstWritesSeen_ != g_psConstWrites;
        if (verifyConstants && !psWritten && constantsValid_ && std::memcmp(lastPsConstants_.data(), regs + 0x4000 + 256 * 4, 4096) != 0)
        {
            static int logged = 0;
            if (logged++ < 20) std::fprintf(stderr, "[verify] ps constants changed without a register write\n");
        }
        psConstWritesSeen_ = g_psConstWrites;
        const uint32_t plo = constantsValid_ ? (g_psConstLo - 0x4400) & ~3u : 0, phi = constantsValid_ ? ((g_psConstHi - 0x4400) | 3u) + 1 : 1024;
        g_psConstLo = 0x4800; g_psConstHi = 0x4400;     // p_diag46
        if ((psWritten || !constantsValid_) && plo < phi &&
            (std::memcmp(lastPsConstants_.data() + plo, regs + 0x4400 + plo, size_t(phi - plo) * 4) != 0 || !constantsValid_))
        {
            ++diagPsCbUploads_;
            std::memcpy(lastPsConstants_.data() + plo, regs + 0x4400 + plo, size_t(phi - plo) * 4);
            if (verifyConstants && std::memcmp(lastPsConstants_.data(), regs + 0x4400, 4096) != 0)
            {
                static int logged = 0;
                if (logged++ < 20) std::fprintf(stderr, "[verify] ps constants differ outside the written registers\n");
            }
            if (cbRingOn_) pendingPsConst_ = true;           // p_diag44
            else Upload(psConstants_.Get(), lastPsConstants_.data(), 4096);
            constantsValid_ = true;
        }
        dPc.Stop();
        DiagScope dTx(diagTicks_[DpTextures]);
        PsStateCB cb{};
        std::memcpy(cb.boolConsts, regs + 0x4900, sizeof(cb.boolConsts));
        std::memcpy(cb.loopConsts, regs + 0x4908, sizeof(cb.loopConsts));
        cb.alphaRef = st.alphaRef; cb.alphaTest = st.alphaTest ? 1 : 0; cb.alphaFunc = st.alphaFunc;
        ID3D11ShaderResourceView* srvs[32] = {};
        ID3D11SamplerState* samplers[16] = {};
        static const bool memoVerify = [] { const char* e = std::getenv("TTT2_DIAG_VERIFY"); return e && *e == '1'; }();
        const bool memoOn = TexMemoOn() && !overrideMask_ && !g_gpuFrameNote && !CensusOn() && !MipCensusOn() && !MipVerifyOn();
        const uint32_t memoEpoch = ::g_cpSyncEpoch.load(std::memory_order_relaxed);
        // p107: the memo across batches (TexMemo2Find); the A/B's d14-old windows keep the one-batch memo of a slot.
        const bool memo2 = memoOn && TexMemo2On() && !ixAbOff_;
        const uint32_t memoFlags = uint32_t(std::clamp(display_settings::GetTextureFilter(), 0, display_settings::kMaxTextureFilter)) << 1;   // its sampler's
        for (uint32_t slot = 0; slot < 32; ++slot)
        {
            if (!(textureSlots & (1u << slot))) continue;
            ++diagTexBinds_;
            const bool memoCube = ((lastCubeSlots_ >> slot) & 1) != 0;
            TexMemo& memo = texMemo_[slot][memoCube ? 1 : 0];
            const uint32_t* memoFc = regs + 0x4800 + slot * 6;
            TexMemo2* m2 = nullptr;           // p107: the entry found, or the one a miss fills
            bool memoHit;
            if (memo2) m2 = TexMemo2Find(memoFc, memoFlags | (memoCube ? 1u : 0u), memoEpoch, memoHit);
            else memoHit = memoOn && memo.valid && memo.epoch == memoEpoch && memo.gen == texGen_ &&
                           memo.dirtySeq == dirty_.Current() && std::memcmp(memo.fc, memoFc, sizeof(memo.fc)) == 0;
            const TexMemo& hitMemo = m2 ? *m2 : memo;
            if (memoHit) ++diagTexMemoHits_;
            if (memoHit && !memoVerify)
            {
                srvs[slot] = hitMemo.srv;
                std::memcpy(cb.texInfo[slot], hitMemo.info, sizeof(hitMemo.info));
                std::memcpy(cb.texScale[slot], hitMemo.scale, sizeof(hitMemo.scale));
                std::memcpy(cb.texSampler[slot], hitMemo.sampInfo, sizeof(hitMemo.sampInfo));
                samplers[slot & 15] = hitMemo.sampler;
                continue;
            }
            Bound b = BindTexture(regs, slot, memoCube);
            srvs[slot] = b.srv;
            cb.texInfo[slot][0] = b.swizzle; cb.texInfo[slot][1] = b.mode;
            cb.texInfo[slot][2] = b.width; cb.texInfo[slot][3] = b.height;
            cb.texScale[slot][0] = b.scaleX; cb.texScale[slot][1] = b.scaleY; cb.texScale[slot][2] = float(b.slices);
            const Texture ft = DecodeTexture(regs, slot);
            SamplerInfo(ft, cb.texSampler[slot]);
            if (b.slices > 1) cb.texSampler[slot][0] = cb.texSampler[slot][1] = 2;   // cube faces clamp
            // Bit 30: every depth-tested (3D) draw; bit 31: every draw; second token: shader size.
            if (((overrideMask_ >> slot) & 1) &&
                (overridePsDwords_ ? psCode.size() == overridePsDwords_
                 : ((overrideMask_ >> 30) & 1) ? st.zEnable : (lastCubeSlots_ || (overrideMask_ >> 31))))
            {
                // Diagnostics (C:/ttt2/port/hw_override.txt): this slot samples opaque white in
                // shaders that fetch a cube (character/environment materials), or in every
                // shader when bit 31 is set.
                srvs[slot] = ((lastCubeSlots_ >> slot) & 1) ? whiteArraySrv_.Get() : whiteSrv_.Get();
                cb.texInfo[slot][0] = 0x688; cb.texInfo[slot][1] = 0; cb.texInfo[slot][2] = cb.texInfo[slot][3] = 1;
                cb.texScale[slot][0] = cb.texScale[slot][1] = cb.texScale[slot][2] = 1.0f;
            }
            samplers[slot & 15] = Sampler(cb.texSampler[slot], MipSampling(ft, b.levels));
            if (memoHit)
            {
                static uint64_t checked = 0, bad = 0;
                if ((hitMemo.srv != srvs[slot] || hitMemo.sampler != samplers[slot & 15] ||
                     std::memcmp(cb.texInfo[slot], hitMemo.info, sizeof(hitMemo.info)) != 0 ||
                     std::memcmp(cb.texScale[slot], hitMemo.scale, sizeof(hitMemo.scale)) != 0 ||
                     std::memcmp(cb.texSampler[slot], hitMemo.sampInfo, sizeof(hitMemo.sampInfo)) != 0) && bad++ < 20)
                    std::fprintf(stderr, "[verify] texture memo differs: slot %u base %08X%s\n", slot, (memoFc[1] >> 12) << 12,
                                 m2 ? (m2->kind == 1 ? " (across batches, guest bytes)" : m2->kind ? " (across batches, resolved)" : " (across batches, fallback)") : "");
                if (++checked % 200000 == 0)
                    std::fprintf(stderr, "[verify] %llu texture memo hits checked, %llu differ\n", (unsigned long long)checked,
                                 (unsigned long long)bad);
            }
            else if (m2)
            {
                m2->valid = true;
                std::memcpy(m2->fc, memoFc, sizeof(m2->fc));
                m2->flags = memoFlags | (memoCube ? 1u : 0u);
                m2->kind = b.memoKind;
                m2->base = b.rangeBase; m2->bytes = b.rangeBytes; m2->mipStart = b.mipStart; m2->mipSize = b.mipSize;
                m2->epoch = memoEpoch; m2->gen = texGen_; m2->fullGen = texFullGen_;
                m2->seq = m2->dirtySeq = dirty_.Current();
                m2->srv = srvs[slot]; m2->sampler = samplers[slot & 15];
                std::memcpy(m2->info, cb.texInfo[slot], sizeof(m2->info));
                std::memcpy(m2->scale, cb.texScale[slot], sizeof(m2->scale));
                std::memcpy(m2->sampInfo, cb.texSampler[slot], sizeof(m2->sampInfo));
            }
            else if (memoOn)
            {
                memo.valid = true;
                std::memcpy(memo.fc, memoFc, sizeof(memo.fc));
                memo.epoch = memoEpoch; memo.gen = texGen_; memo.dirtySeq = dirty_.Current();
                memo.srv = srvs[slot]; memo.sampler = samplers[slot & 15];
                std::memcpy(memo.info, cb.texInfo[slot], sizeof(memo.info));
                std::memcpy(memo.scale, cb.texScale[slot], sizeof(memo.scale));
                std::memcpy(memo.sampInfo, cb.texSampler[slot], sizeof(memo.sampInfo));
            }
        }
        dTx.Stop();
        DiagScope dPs(diagTicks_[DpConstants]);
        if (std::memcmp(&cb, &lastState_, sizeof(cb)) != 0 || !stateValid_)
        {
            ++diagPsStateUploads_;
            lastState_ = cb; stateValid_ = true;
            if (cbRingOn_) pendingPsState_ = true;           // p_diag44
            else Upload(psState_.Get(), &cb, sizeof(cb));
        }
        if (cbRingOn_) CbRingWrite();
        dPs.Stop();
        DiagScope dOm(diagTicks_[DpState]);

        // ---- output merger: the surfaces cover the scissored area (logical samples); a bound
        // colour / depth pair is grown to one size (D3D11 binds only equal-size views).
        const int64_t x1 = std::clamp<int64_t>(int64_t(st.scX1) * st.sampX, 0, SurfaceW);
        const int64_t y1 = std::clamp<int64_t>(int64_t(st.scY1) * st.sampY, 0, SurfaceH);
        DepthSurface* depth = nullptr;
        if (st.zEnable || st.stencil)
        {
            depth = &Depth(st.depthBase);
            depth->touched = true; depth->sampX = st.sampX; depth->sampY = st.sampY;
        }
        if (CensusOn()) CensusSurfaceUse(regs, st, reachY);
        auto reach = [](int64_t scissor, float geometry, uint32_t samples) {
            return UINT(std::isfinite(geometry) ? std::clamp<int64_t>(int64_t(std::min(geometry, 65536.0f)) * samples, 0, scissor)
                                                : scissor);
        };
        UINT needW = reach(x1, reachX, st.sampX), needH = reach(y1, reachY, st.sampY);
        if (st.writeMask && depth)
        {
            needW = std::max({ needW, color.extW, depth->extW });
            needH = std::max({ needH, color.extH, depth->extH });
        }
        // Native-size pair (a CPU-read bake surface and its depth) or the render scale. The game reuses
        // EDRAM ranges: a draw into a bake's colour or depth base at another sample pitch than the
        // bake's, or bigger than a bake (1024x1024), is something else drawn there (the win screen's
        // depth of field draws 640x180 into the fight intro bake's 4C0, MEASURED 2026-10-20) - back
        // to the render scale for good (UpdateNative flags it again at the next bake).
        bool native = (st.writeMask && nativeColors_.count(st.colorBase)) || (depth && nativeDepths_.count(st.depthBase));
        static const bool sceneFix = [] { const char* e = std::getenv("TTT2_NATIVE_SCENE_FIX"); return !(e && *e == '0'); }();
        const uint32_t samplePitch = st.pitch * st.sampX;
        const auto nc = st.writeMask ? nativeColors_.find(st.colorBase) : nativeColors_.end();
        const auto nd = depth ? nativeDepths_.find(st.depthBase) : nativeDepths_.end();
        const bool otherUse = (nc != nativeColors_.end() && nc->second != samplePitch) || (nd != nativeDepths_.end() && nd->second != samplePitch);
        if (native && (otherUse || needW > 1024 || needH > 1024))
        {
            static int logged = 0;
            if (logged++ < 32)
                std::fprintf(stderr, "[gpu-hw] native-size surface %05X / depth %03X drawn at sample pitch %u, %ux%u samples%s\n",
                             st.colorBase, depth ? st.depthBase : 0xFFFu, samplePitch, needW, needH, sceneFix ? ": back to the render scale" : "");
            if (sceneFix)
            {
                nativeColors_.erase(st.colorBase);
                if (depth) nativeDepths_.erase(st.depthBase);
                const auto d = depthWithColor_.find(st.colorBase);
                if (d != depthWithColor_.end()) nativeDepths_.erase(d->second);
                native = false;
            }
        }
        const float sc = native ? 1.0f : scale_;
        if (st.writeMask && depth) depthWithColor_[st.colorBase] = st.depthBase;
        if (st.writeMask) Grow(color, st.colorBase, needW, needH, sc);
        if (depth) Grow(*depth, st.depthBase, needW, needH, sc);
        ID3D11RenderTargetView* rtv = st.writeMask ? color.rtv.Get() : nullptr;
        ID3D11DepthStencilView* dsv = depth ? depth->dsv.Get() : nullptr;
        if ((st.cull & 3) == 3) { ++stats_.emptyDraws; return; }          // both faces culled
        float factor[4];
        ID3D11BlendState* blendState = BlendState(st, factor);
        ID3D11DepthStencilState* depthState = DepthState(st);
        const UINT stencilRef = StencilRef(st);
        // The sample-space viewport and scissor, times the render scale (vertices stay logical).
        D3D11_VIEWPORT vp{ 0, 0, float(st.pitch * st.sampX) * sc, float(SurfaceH) * sc, 0, 1 };
        D3D11_RECT scissor{ LONG(Px(std::max(st.scX0, 0) * int(st.sampX), sc)), LONG(Px(std::max(st.scY0, 0) * int(st.sampY), sc)),
                            LONG(Px(double(x1), sc)), LONG(Px(double(y1), sc)) };
        ApplyPipe(rtv, dsv, blendState, factor, depthState, stencilRef, vp, scissor, RasterState(st), ps, srvs, samplers, textureSlots);
        dOm.Stop();
        if (FlashMax())     // TEST ONLY (p_diag31): the draw and what each slot samples
        {
            if (flashCurConsts_.size() < 1024 * 1200)     // p_diag32/33: every draw's PS constant registers (a block per change)
            {
                const uint32_t* pc = regs + 0x4000 + 256 * 4;
                if (flashCurConsts_.empty() || std::memcmp(flashCurConsts_.data() + flashCurConsts_.size() - 1024, pc, 4096) != 0)
                    flashCurConsts_.insert(flashCurConsts_.end(), pc, pc + 1024);
                const uint32_t at = uint32_t(flashCurConsts_.size() / 1024) - 1;
                const bool stale = constantsValid_ && std::memcmp(lastPsConstants_.data(), pc, 4096) != 0;
                FlashNote({ 'C', at, stale ? 1u : 0u, 0, 0, 0, 0, nullptr, nullptr });
            }
            FlashNote({ 'V', flashVertexHash_, flashVertexDwords_, 0, 0, 0, 0, nullptr, nullptr });
            flashVertexHash_ = 0; flashVertexDwords_ = 0;
            FlashNote({ 'D', st.colorBase | ((st.writeMask & 0xFu) << 24), st.depthBase | (st.zEnable ? 0x10000u : 0u) | (st.zWrite ? 0x20000u : 0u),
                        st.blend, indexCount, uint32_t(psCode.size()), textureSlots, ps, want_.vs });
            for (uint32_t slot = 0; slot < 32; ++slot)
                if (textureSlots & (1u << slot))
                {
                    const uint32_t* fc = regs + 0x4800 + slot * 6;
                    FlashNote({ 'T', slot, (fc[1] >> 12) << 12, fc[1] & 0x3F, ((fc[2] & 0x1FFF) + 1) | ((((fc[2] >> 13) & 0x1FFF) + 1) << 16),
                                fc[0], fc[3], srvs[slot], samplers[slot & 15] });
                }
        }
        const uint32_t diagWho = st.colorBase | (native ? 0x80000000u : 0u), diagVp = (UINT(vp.Width) << 16) | UINT(vp.Height);
        DiagStamp('b', diagWho, diagVp, indexCount, uint32_t(psCode.size()));
        if (!(toggle_ & 1)) { DiagScope dDc(diagTicks_[DpDrawCall]); context_->DrawIndexed(indexCount, firstIndex, baseVertex); }
        DiagStamp('d', diagWho, diagVp, indexCount, uint32_t(psCode.size()));
        stats_.triangles += want_.topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ? (indexCount > 2 ? indexCount - 2 : 0) : indexCount / 3;
        MaybeReportStats();
    }

    // ----------------------------------------------------------------- resolve
    // srcSelect is RB_COPY_CONTROL.copy_src_select: 0-3 colour render targets, 4 depth/stencil.
    // A depth resolve copies the depth surface for depthBase into an R32F texture (the value the
    // guest's 24_8 / 24_8_FLOAT fetch returns); depthFloat is RB_DEPTH_INFO.depth_format (20e4).
    // destFormat is RB_COPY_DEST_INFO.copy_dest_format: a 16_16_FLOAT destination (31) keeps the
    // source's red and green as half floats (an R16G16_FLOAT texture the 16_16_FLOAT fetch
    // samples directly); every other colour destination is modelled as 8_8_8_8.
    void Resolve(uint32_t colorBase, uint32_t dest, uint32_t pitch, uint32_t height, bool hasLatch, uint32_t latch,
                 bool swapRB, uint32_t srcSelect, uint32_t depthBase, bool depthFloat, uint32_t destFormat = 6)
    {
        Initialize();
        DiagScope dRs(diagTicks_[DpResolve]);
        DiagStamp('R', dest, (pitch << 16) | height, colorBase, srcSelect);
        ++texGen_;   // p_diag25: texture lookups' inputs changed
        // TEST ONLY (p_diag20): where a slow resolve's time goes.
        LARGE_INTEGER slowT[8];
        int slowN = 0;
        bool slowMade = false, slowStaged = false;
        auto slowMark = [&] { if (slowN < 8) QueryPerformanceCounter(&slowT[slowN++]); };
        slowMark();
        UpdateScale();
        FinishSmallWritebacks();
        slowMark();
        ++stats_.resolves;
        const bool depth = srcSelect == 4;
        const bool rg16f = !depth && destFormat == 31;
        if (depth) NoteShadowResolve(depthBase, pitch, height);
        Resolved& r = resolved_[dest];
        float sourceScale = scale_;
        if (depth)
        {
            auto d = depths_.find(depthBase);
            if (d != depths_.end() && d->second.tex) sourceScale = d->second.scale;
        }
        else if (auto c = colors_.find(colorBase); c != colors_.end() && c->second.tex)
            sourceScale = c->second.scale;
        if (!r.tex || r.pitch != pitch || r.height != height || r.depth != depth || r.rg16f != rg16f || r.scale != sourceScale)
        {
            const bool hot = r.readbackHot;
            RetireResolvedTexture(r);
            r = Resolved{};
            r.pitch = pitch; r.height = height; r.depth = depth; r.rg16f = rg16f;
            r.readbackHot = hot;
            MakeResolvedTexture(r, sourceScale);
            slowMade = true;
        }
        FlashNote({ 'R', dest, colorBase, pitch | (height << 16), srcSelect | (destFormat << 8) | (hasLatch ? 0x10000u : 0u), latch,
                    slowMade ? 1u : 0u, r.srv.Get(), nullptr });   // TEST ONLY (p_diag31)
        slowMark();
        r.sourceKey = depth ? ~0u : colorBase;
        if (!depth) { const auto c = colors_.find(colorBase); r.sourcePitch = c != colors_.end() ? c->second.samplePitch : 0; }
        static const bool scaleLog = [] { const char* e = std::getenv("TTT2_RESOLVE_SCALE_LOG"); return e && *e == '1'; }();
        if (scaleLog && !censusSeen_[(5ull << 60) ^ (uint64_t(colorBase) << 32) ^ (uint64_t(dest) >> 4) ^ (uint64_t(pitch) << 44) ^ (uint64_t(height) << 20)]++)
        {
            const auto c = colors_.find(colorBase);
            std::fprintf(stderr, "[resolve-scale] %s %05X/%03X %ux%u -> %08X: source scale %.2f, texture scale %.2f, surface ext %ux%u native %d/%d\n",
                         depth ? "depth" : "colour", colorBase, depthBase, pitch, height, dest, sourceScale, r.scale,
                         c != colors_.end() ? c->second.extW : 0u, c != colors_.end() ? c->second.extH : 0u,
                         int(nativeColors_.count(colorBase)), int(nativeDepths_.count(depthBase)));
        }
        if (!depth)
        {
            auto& dests = resolveDests_[colorBase];
            auto [entry, added] = dests.try_emplace(dest, r.readbackHot);
            if (added || entry->second != r.readbackHot) { entry->second = r.readbackHot; UpdateNative(colorBase); }
        }
        slowMark();
        r.depthFloat = depthFloat;
        if (CensusOn() && !censusSeen_[(4ull << 60) ^ (uint64_t(colorBase) << 32) ^ (uint64_t(pitch) << 16) ^ height ^
                                       (uint64_t(srcSelect) << 28) ^ (uint64_t(depthBase) << 44)]++)
            std::fprintf(stderr, "[census] resolve from %s %05X/%03X %ux%u -> %08X\n", depth ? "depth" : "colour",
                         colorBase, depthBase, pitch, height, dest);
        auto it = colors_.find(colorBase);
        if (depth)
        {
            ++stats_.depthResolves;
            auto d = depths_.find(depthBase);
            if (toggle_ & 4) {}
            else if (ShadowMapOff(depthBase, pitch, height))
            {
                const float lit[4] = { 0, 0, 0, 0 };     // SHADOWS OFF: no occluder anywhere (see ShadowCaster)
                context_->ClearRenderTargetView(r.rtv.Get(), lit);
            }
            else if (d != depths_.end() && d->second.touched) BlitDepth(d->second, r);
            else
            {
                const float farDepth[4] = { 1, 1, 1, 1 };                 // depth surfaces start at 1.0
                context_->ClearRenderTargetView(r.rtv.Get(), farDepth);
            }
            r.copiedWidth = pitch;
        }
        else if (it == colors_.end() || !it->second.touched)
        {
            // Never rasterized: the CPU path fills the fast-clear latch (or zero). RB_COLOR_CLEAR
            // holds the 8_8_8_8 pixel itself, red in the low byte (Xenia's k_8_8_8_8 clear).
            const uint32_t px = hasLatch ? latch : 0;
            const float c[4] = { (px & 0xFF) / 255.0f, ((px >> 8) & 0xFF) / 255.0f,
                                 ((px >> 16) & 0xFF) / 255.0f, ((px >> 24) & 0xFF) / 255.0f };
            context_->ClearRenderTargetView(r.rtv.Get(), c);
            r.copiedWidth = pitch;
        }
        else
        {
            ColorSurface& s = it->second;
            const uint32_t width = std::min({ s.samplePitch ? s.samplePitch : pitch, pitch, SurfaceW });
            const uint32_t rows = std::min<UINT>(height, SurfaceH);
            // Nothing was ever drawn beyond the surface's extent (it covers every draw's reach):
            // that part reads as the cleared store, zero - the shader blits' out-of-range loads.
            const float k = r.scale;                // == s.scale: texel for texel
            const UINT copyW = std::min(Px(width, k), AllocPx(r.pitch, k)), copyH = std::min(Px(rows, k), AllocPx(r.height, k));
            const UINT haveW = s.tex ? std::min(copyW, AllocPx(s.extW, k)) : 0, haveH = s.tex ? std::min(copyH, AllocPx(s.extH, k)) : 0;
            if ((width != r.copiedWidth && width < pitch) || haveW < copyW || haveH < copyH)
            {
                const float zero[4] = { 0, 0, 0, 0 };
                context_->ClearRenderTargetView(r.rtv.Get(), zero);
            }
            if (!s.tex || (toggle_ & 4)) {}
            else if (r.rg16f) BlitColor(s, r, width, rows, true);
            else if (s.format != DXGI_FORMAT_B8G8R8A8_UNORM) BlitColor(s, r, width, rows, false);   // convert
            else
            {
                D3D11_BOX box{ 0, 0, 0, haveW, haveH, 1 };
                context_->CopySubresourceRegion(r.tex.Get(), 0, 0, 0, 0, s.tex.Get(), 0, &box);
            }
            r.copiedWidth = width;
        }
        slowMark();
        // The resolve owns its destination from now on; any older decode of these bytes is
        // stale. Consume earlier CPU writes so only LATER ones invalidate this entry.
        r.size = ResolveBytes(pitch, height);
        dirty_.Sync(dest, r.size);
        r.seq = dirty_.Current();
        r.valid = true;
        r.cpuCoherent = false;
        r.swapRB = swapRB;          // copy_dest_swap: the guest bytes hold red/blue exchanged
        DropTexturesOverlapping(dest, r.size);
        slowMark();
        static const bool writeback = [] { const char* e = std::getenv("TTT2_GPU_HW_WRITEBACK"); return e && std::strcmp(e, "1") == 0; }();
        // Tiny resolves are CPU readbacks (the game's auto exposure reads its 32x1 average-colour
        // resolve and turns it into the tone pass's exposure constant) - and the CPU reads them
        // without passing through the cached view, so nothing else makes them coherent. MEASURED
        // 2026-10-12 on Odeum of Illusions: without this, exposure 3.4 instead of ~1.5, luma 190 /
        // 25% white vs 119-143 / 1-3% on real-hardware footage; with it, luma 124-129, near-white
        // 2-3%, saturation 0.51-0.55 (reference 0.57), 60 fps. TTT2_SMALL_WRITEBACK=<pixels>
        // changes the limit (0 = off).
        static const uint32_t smallWriteback = [] {
            const char* e = std::getenv("TTT2_SMALL_WRITEBACK");
            return e ? uint32_t(std::strtoul(e, nullptr, 0)) : 32u;
        }();
        // Small ones are staged and written into guest memory once the GPU has done the copy
        // (FinishSmallWritebacks, at the next resolve or present) instead of waiting here: the
        // exposure readback came once per frame, and its blocking Map (with a new staging texture
        // each time) made the CP thread wait for the GPU every frame - MEASURED 2026-10-15 on the
        // TEKKEN TUNES page: 25-45 ms frames, present calls of 6-27 ms. TTT2_SMALL_WRITEBACK_SYNC=1
        // waits as before.
        static const bool smallSync = [] { const char* e = std::getenv("TTT2_SMALL_WRITEBACK_SYNC"); return e && *e == '1'; }();
        // Customize's thumbnail shot: the game resolves the 128x304 picture and its CPU reads it at once, also past the
        // cached view, for the JPEG of the save file and the preview (black without this, MEASURED 2026-11-04).
        const bool thumbnailShot = !depth && pitch == 128 && height == 304;
        if (writeback || (pitch * height <= smallWriteback && smallSync) || thumbnailShot) WriteBack(dest, r);
        else if (pitch * height <= smallWriteback) { if (!(toggle_ & 8)) { StageReadback(r); smallPending_.insert(dest); if (FlashMax()) flashSmallDests_.insert(dest); } }
        else if (r.readbackHot) { StageReadback(r); slowStaged = true; }
        slowMark();
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            const double perMs = double(f.QuadPart) / 1000.0;
            const double total = double(slowT[slowN - 1].QuadPart - slowT[0].QuadPart) / perMs;
            static int slowLogged = 0;
            if (total > 4.0 && slowLogged++ < 300)
            {
                FILETIME ft;
                GetSystemTimePreciseAsFileTime(&ft);
                const double unixMs = double(((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000ull) / 10000.0;
                double part[7] = {};
                for (int i = 1; i < slowN && i < 8; ++i) part[i - 1] = double(slowT[i].QuadPart - slowT[i - 1].QuadPart) / perMs;
                std::fprintf(stderr, "[slow-resolve] %.1f %s %05X -> %08X %ux%u: %.2f ms = small writebacks %.2f, texture %.2f%s, "
                             "native %.2f, copy %.2f, sync/drop %.2f, stage %.2f%s\n", unixMs, depth ? "depth" : "colour",
                             depth ? depthBase : colorBase, dest, pitch, height, total, part[0], part[1], slowMade ? " (new)" : "",
                             part[2], part[3], part[4], part[5], slowStaged ? " (for the cpu)" : "");
            }
        }
        MaybeReportStats();
    }

    // The CPU is about to read guest bytes in [start, start + size) - a first access through
    // the cached physical view (phys_mirror.cpp). Make every GPU-only resolve overlapping them
    // coherent first, as the console's shared memory would be. Returns true while a copy is still
    // on its way: the caller (GpuHwWriteBackRange) calls again WITHOUT the backend lock in between,
    // so the command processor keeps drawing and presenting while this thread waits for the GPU
    // (it used to hold the lock for the whole wait: the fight intro's reads stalled presents by
    // up to ~15 ms each, MEASURED 2026-10-14). `block` waits in Map instead (the caller's last resort).
    bool WriteBackRange(uint32_t start, uint32_t size, bool block = false)
    {
        ++texGen_;   // p_diag25: texture lookups' inputs changed (p110: resolves only - no cached texture dropped)
        bool pending = false;
        pendingFence_ = 0;
        for (auto& [rs, r] : resolved_)
        {
            if (!r.valid || r.cpuCoherent || !r.tex) continue;
            if (!(uint64_t(start) < uint64_t(rs) + r.size && uint64_t(rs) < uint64_t(start) + size)) continue;
            dirty_.Sync(rs, r.size);
            if (dirty_.Newest(rs, r.size) > r.seq) { r.valid = false; continue; }   // the CPU overwrote it
            if (!r.readbackHot && r.sourceKey != ~0u)
            {
                r.readbackHot = true;
                resolveDests_[r.sourceKey][rs] = true;
                UpdateNative(r.sourceKey);
            }
            else if (r.sourceKey != ~0u && !nativeColors_.count(r.sourceKey)) UpdateNative(r.sourceKey);   // a bake again
            r.readbackHot = true;                     // its next resolves are staged for the CPU
            if (!r.staged || r.stagedSeq != r.seq)
            {
                StageReadback(r);                     // not staged yet (a first read): copy now, map later
                if (!block) { pending = true; pendingFence_ = std::max(pendingFence_, r.stagedFence); continue; }
            }
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<uint32_t> px;
            if (!ReadStaging(r.staging.Get(), r.pitch, r.height, px, !block))
            {
                pending = true;
                pendingFence_ = std::max(pendingFence_, r.stagedFence);
                continue;
            }
            r.staged = false;
            lastStagedAgeMs_ = std::chrono::duration<double, std::milli>(t0 - r.stagedAt).count();
            if (r.pitch == 1024 && r.height == 1024) DiagGpuTimeline(rs, lastStagedAgeMs_);
            ++writebackCauses_["cpu read through the cached physical view"];
            static int logged = 0;
            if (logged < 40)
            {
                ++logged;
                std::fprintf(stderr, "[gpu-hw] cpu-read writeback dest=%08X %ux%u swapRB=%d rg16f=%d depth=%d\n",
                             rs, r.pitch, r.height, int(r.swapRB), int(r.rg16f), int(r.depth));
            }
            Scatter(rs, r, px, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        return pending;
    }

    // After a pending WriteBackRange: the fence value its copies complete at (0: poll instead).
    ComPtr<ID3D11Fence> PendingFence(uint64_t& value) const { value = pendingFence_; return value ? readbackFence_ : nullptr; }

    // Compare the GPU resolve with the CPU renderer's linear result (compare mode).
    void CompareResolve(uint32_t dest, const uint32_t* cpu, uint32_t pitch, uint32_t height)
    {
        auto it = resolved_.find(dest);
        if (it == resolved_.end() || !it->second.tex) return;
        std::vector<uint32_t> gpu;
        ReadLogical(it->second.tex.Get(), it->second.pitch, it->second.height, pitch, height, !it->second.depth, gpu);
        uint64_t differing = 0, sumDiff = 0;
        uint32_t maxDiff = 0;
        for (size_t i = 0; i < size_t(pitch) * height; ++i)
        {
            uint32_t a = cpu[i], b = gpu[i], worst = 0;
            for (int s = 0; s < 32; s += 8)
            {
                const uint32_t d = (uint32_t)std::abs(int((a >> s) & 0xFF) - int((b >> s) & 0xFF));
                worst = std::max(worst, d);
                sumDiff += d;
            }
            maxDiff = std::max(maxDiff, worst);
            differing += worst > 2;
        }
        const double pct = 100.0 * double(differing) / double(std::max<size_t>(size_t(pitch) * height, 1));
        ++compares_;
        if (compares_ <= 40 || compares_ % 120 == 0 || pct > 5.0)
            std::fprintf(stderr, "[gpu-hw-cmp] #%llu dest=%08X %ux%u differing(>2)=%llu (%.3f%%) max=%u mean=%.4f\n",
                         (unsigned long long)compares_, dest, pitch, height, (unsigned long long)differing, pct,
                         maxDiff, double(sumDiff) / double(std::max<size_t>(size_t(pitch) * height * 4, 1)));
        if (pct > 5.0 && dumps_ < 6)
        {
            ++dumps_;
            std::filesystem::create_directories("C:/ttt2/port/gpu_cmp");
            char path[160];
            std::snprintf(path, sizeof(path), "C:/ttt2/port/gpu_cmp/%03u_%08X_cpu.bmp", dumps_, dest);
            WriteBmp(path, cpu, pitch, height);
            std::snprintf(path, sizeof(path), "C:/ttt2/port/gpu_cmp/%03u_%08X_gpu.bmp", dumps_, dest);
            WriteBmp(path, gpu.data(), pitch, height);
        }
    }

    // Diagnostics: write the GPU texture registered for a resolve destination as a BMP.
    bool DumpResolved(uint32_t dest, const char* path)
    {
        auto it = resolved_.find(dest);
        if (it == resolved_.end() || !it->second.tex) return false;
        std::vector<uint32_t> px;
        ReadLogical(it->second.tex.Get(), it->second.pitch, it->second.height, it->second.pitch, it->second.height,
                    !it->second.depth, px);
        if (it->second.depth)
            for (uint32_t& v : px)
            {
                float d; std::memcpy(&d, &v, 4);
                const uint32_t g = uint32_t(std::clamp(d, 0.0f, 1.0f) * 255.0f + 0.5f);
                v = 0xFF000000u | (g << 16) | (g << 8) | g;
            }
        WriteBmp(path, px.data(), it->second.pitch, it->second.height);
        return true;
    }

    // ----------------------------------------------------------------- present
    void SetWindow(HWND w) { window_.store(w); }

    // displaySwapRB: the front buffer's fetch-constant swizzle exchanges red and blue (TTT2's
    // .zyx1). The image shown is swizzle(stored bytes); a GPU-resolved front buffer holds the
    // unswapped render, so swap only when exactly one of the two exchanges applies.
    bool Present(uint32_t frontBuffer, bool displaySwapRB, const uint32_t* gammaRamp, uint32_t gammaVersion)
    {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        if (FramesOn()) FrameRecord();                      // p81: the frame that ends here (before this present's own time)
        DiagScope dPr(diagTicks_[DpPresent]);
        Initialize();
        CbToggleCheck();
        VdPresent();
        UpdateScale();
        FollowMipToggle();
        FollowStageLoad();
        FinishSmallWritebacks();
        UpdateGamma(gammaRamp, gammaVersion);
        bool storedSwap = false;
        uint32_t logicalW = 1280, logicalH = 720;
        ID3D11Texture2D* source = FrontTexture(frontBuffer, storedSwap, logicalW, logicalH);
        if (!source) return false;
        HWND w = window_.load();
        if (!w) return false;
        if (!swapchain_)
        {
            ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; ComPtr<IDXGIFactory2> factory;
            Check(device_.As(&dxgi), "QueryDxgiDevice");
            Check(dxgi->GetAdapter(&adapter), "GetAdapter");
            Check(adapter->GetParent(IID_PPV_ARGS(&factory)), "GetFactory2");
            RECT rc{};
            GetClientRect(w, &rc);
            DXGI_SWAP_CHAIN_DESC1 d{};
            d.Width = std::max<LONG>(1, rc.right - rc.left); d.Height = std::max<LONG>(1, rc.bottom - rc.top);
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
            // TTT2_SWAP_BUFFERS=3/4 (experiment): MEASURED 2026-10-15 over 5 min of real play,
            // three buffers neither shortened the fight intro's readback waits (median 7.6 ms
            // either way) nor reduced dropped frames, so two stay the default.
            static const UINT buffers = [] { const char* e = std::getenv("TTT2_SWAP_BUFFERS"); const int n = e ? std::atoi(e) : 2; return UINT(std::clamp(n, 2, 4)); }();
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = buffers;
            d.Scaling = DXGI_SCALING_STRETCH; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            // VSYNC OFF presents with tearing allowed, which needs the swap chain flag: set whenever
            // the system supports it (ResizeBuffers keeps it, SwapChainFlags). TTT2_NO_TEARING=1
            // leaves it out (VSYNC OFF then only changes exclusive fullscreen).
            ComPtr<IDXGIFactory5> factory5;
            BOOL tearing = FALSE;
            static const bool noTearing = [] { const char* e = std::getenv("TTT2_NO_TEARING"); return e && *e == '1'; }();
            tearing_ = !noTearing && SUCCEEDED(factory.As(&factory5)) &&
                       SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) && tearing;
            d.Flags = SwapChainFlags();
            Check(factory->CreateSwapChainForHwnd(device_.Get(), w, &d, nullptr, nullptr, &swapchain_), "CreateSwapChainForHwnd");
            factory->MakeWindowAssociation(w, DXGI_MWA_NO_ALT_ENTER);       // the window thread owns Alt+Enter
            bufW_ = d.Width; bufH_ = d.Height;
            refreshHz_ = DisplayHz(w);
            std::fprintf(stderr, "[gpu-hw] presenting through a DXGI flip swap chain (%ux%u), tearing %s, display %u Hz, %s\n",
                         bufW_, bufH_, tearing_ ? "supported" : "not supported", refreshHz_, OutputInfo().c_str());
        }
        const auto t1 = clock::now();
        ApplyDisplay(w);
        ComPtr<ID3D11Texture2D> back;
        Check(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBackBuffer");
        const bool swap = storedSwap != displaySwapRB;
        const auto t2 = clock::now();
        PresentScaled(source, back.Get(), swap);
        DiagGpuLoad();
        lastSwap_ = swap;
        if (NativeDumpOn() && (stats_.presents + 1) % 60 == 0)      // the same present as fb_native.bmp
        {
            // Diagnostics (TTT2_FB_NATIVE=1): what the window shows, after the image options.
            std::vector<uint32_t> px;
            ReadTexture(back.Get(), bufW_, bufH_, px);
            for (uint32_t& v : px) v &= 0x00FFFFFFu;
            WriteBmp("C:/ttt2/port/fb_back.bmp", px.data(), bufW_, bufH_);
        }
        const auto t3 = clock::now();
        static const bool noWait = [] { const char* e = std::getenv("TTT2_PRESENT_NOWAIT"); return e && *e == '1'; }();
        static const bool diag = [] { const char* e = std::getenv("TTT2_PRESENT_DIAG"); return e && *e == '1'; }();
        if (diag)
        {
            // Diagnostics: the flush, then the GPU draining everything queued, timed apart from Present.
            using ms = std::chrono::duration<double, std::milli>;
            if (!diagQuery_)
            {
                D3D11_QUERY_DESC q{ D3D11_QUERY_EVENT, 0 };
                device_->CreateQuery(&q, &diagQuery_);
            }
            const auto a = clock::now();
            context_->Flush();
            const auto b = clock::now();
            context_->End(diagQuery_.Get());
            BOOL done = FALSE;
            while (context_->GetData(diagQuery_.Get(), &done, sizeof(done), 0) != S_OK || !done) YieldProcessor();
            const auto c = clock::now();
            diagFlushMs_ += ms(b - a).count();
            diagDrainMs_ += ms(c - b).count();
            diagDrainMax_ = (std::max)(diagDrainMax_, ms(c - b).count());
        }
        GpuTimerFrameEnd();
        ApplyFrameLatency();
        const auto [interval, presentFlags] = PresentMode(noWait);
        HRESULT hr = (toggle_ & 16) ? S_OK : swapchain_->Present(interval, presentFlags);
        GpuTimerFrameBegin();
        DiagStamp('P');
        const auto t4 = clock::now();
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { ++presentHist_[7]; hr = S_OK; }
        Probe("present");
        MarkPresentDone();
        {
            // TTT2_FRAMETIME=1: where a slow present call goes.
            static const bool timing = [] { const char* e = std::getenv("TTT2_FRAMETIME"); return e && *e == '1'; }();
            using ms = std::chrono::duration<double, std::milli>;
            auto printHist = [&] {
                const uint32_t n = (std::max)(presentHist_[6], 1u);
                std::fprintf(stderr, "[frame] DXGI Present over %u calls (toggle %X): <1 ms %u, 1-3 %u, 3-6 %u, 6-10 %u, 10-20 %u, >20 %u, mean %.2f ms, would block %u; flush mean %.2f ms, GPU drain mean %.2f max %.2f ms\n",
                             presentHist_[6], toggle_, presentHist_[0], presentHist_[1], presentHist_[2], presentHist_[3], presentHist_[4], presentHist_[5],
                             presentHistMs_ / n, presentHist_[7], diagFlushMs_ / n, diagDrainMs_ / n, diagDrainMax_);
                presentHist_ = {};
                presentHistMs_ = diagFlushMs_ = diagDrainMs_ = diagDrainMax_ = 0;
            };
            // TTT2_HW_TOGGLE=1 (diagnostics): C:/ttt2/port/hw_toggle.txt, a hex mask re-read every 30
            // presents, switches parts of each frame off live (the picture breaks): 1 draw calls,
            // 2 GPU-VS draws whole (vertex data too), 4 resolve copies, 8 staged readbacks,
            // 16 DXGI Present, 32 constant uploads.
            static const bool toggleFile = [] { const char* e = std::getenv("TTT2_HW_TOGGLE"); return e && *e == '1'; }();
            if (toggleFile && stats_.presents % 30 == 0)
            {
                uint32_t mask = 0;
                if (FILE* f = std::fopen("C:/ttt2/port/hw_toggle.txt", "r")) { if (std::fscanf(f, "%x", &mask) < 1) mask = 0; std::fclose(f); }
                if (mask != toggle_)
                {
                    if (timing) printHist();
                    toggle_ = mask;
                }
            }
            if (timing)
            {
                const double d = ms(t4 - t3).count();
                ++presentHist_[d < 1 ? 0 : d < 3 ? 1 : d < 6 ? 2 : d < 10 ? 3 : d < 20 ? 4 : 5];
                presentHistMs_ += d;
                if (++presentHist_[6] == 600) printHist();
            }
            static int logged = 0;
            if (timing && ms(t4 - t0).count() > 10.0 && logged++ < 300)
                std::fprintf(stderr, "[frame] present call %.1f ms: front texture %.1f, display %.1f, scaled copy %.1f, DXGI Present %.1f\n",
                             ms(t4 - t0).count(), ms(t1 - t0).count(), ms(t2 - t1).count(), ms(t3 - t2).count(), ms(t4 - t3).count());
        }
        buildBudgetMs_ = 24;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) Check(hr, "Present");
        if (hr != lastPresentHr_)
        {
            // A status (e.g. DXGI_STATUS_OCCLUDED 087A0001) means the frame did not reach the screen.
            static int logged = 0;
            if (logged++ < 64)
                std::fprintf(stderr, "[gpu-hw] present result %08X -> %08X (back buffer %ux%u, exclusive %d)\n",
                             static_cast<unsigned>(lastPresentHr_), static_cast<unsigned>(hr), bufW_, bufH_, int(exclusive_.load()));
            if (hr == DXGI_STATUS_OCCLUDED) LogWindowsAbove(w, "present occluded");
            lastPresentHr_ = hr;
        }
        lastFront_ = source; lastFrontW_ = logicalW; lastFrontH_ = logicalH;
        FlashPresent(source, logicalW, logicalH);   // TEST ONLY (p_diag31)
        ++stats_.presents;
        if (stats_.presents % 60 == 0 && NativeDumpOn())
        {
            // Diagnostics (TTT2_FB_NATIVE=1): the presented image at its rendered size, once a second.
            D3D11_TEXTURE2D_DESC sd{};
            source->GetDesc(&sd);
            std::vector<uint32_t> px;
            ReadTexture(source, sd.Width, sd.Height, px);
            for (uint32_t& v : px) v = DisplayPixel(v);
            WriteBmp("C:/ttt2/port/fb_native.bmp", px.data(), sd.Width, sd.Height);
        }
        static const bool overrideFile = [] { const char* e = std::getenv("TTT2_HW_OVERRIDE"); return e && *e == '1'; }();
        if (overrideFile && stats_.presents % 30 == 0)
        {
            // Diagnostics (TTT2_HW_OVERRIDE=1): C:/ttt2/port/hw_override.txt = hex mask of texture slots to force white.
            // Optional second token: only pixel shaders of that many dwords.
            uint32_t mask = 0, dwords = 0;
            if (FILE* f = std::fopen("C:/ttt2/port/hw_override.txt", "r"))
            { if (std::fscanf(f, "%x %u", &mask, &dwords) < 1) mask = 0; std::fclose(f); }
            if (mask != overrideMask_ || dwords != overridePsDwords_)
                std::fprintf(stderr, "[gpu-hw] texture override mask %08X ps=%u dwords\n", mask, dwords);
            overrideMask_ = mask; overridePsDwords_ = dwords;
        }
        return true;
    }

    // Copy of the last presented image as BGRX (alpha cleared), for BMP filmstrips.
    // TEST ONLY (gpu_render.cpp GpuHwDiagFrameTotals): the running per-part timers (ReportDiag zeroes them every 5 s)
    // and the resolve count.
    void DiagTotals(int64_t* ticks, int count, uint64_t* resolves) const
    {
        for (int i = 0; i < count && i < DpCount; ++i) ticks[i] = diagTicks_[i].load(std::memory_order_relaxed);
        *resolves = stats_.resolves;
    }
    bool ReadFront(uint32_t* out)
    {
        ++frShots_;                                         // p81: a screenshot read back in this frame
        if (!lastFront_) return false;
        std::vector<uint32_t> px;
        ReadLogical(lastFront_.Get(), lastFrontW_, lastFrontH_, 1280, 720, true, px);
        for (size_t i = 0; i < px.size(); ++i) out[i] = DisplayPixel(px[i]);
        return true;
    }
    // TEST ONLY (p_diag30): a GPU copy of the frame just shown (DiagKeepFront, every present), read back by ReadDiagPrev
    // when the next one turns out to be a one-draw frame.
    ComPtr<ID3D11Texture2D> diagPrevFront_;
    uint32_t diagPrevW_ = 0, diagPrevH_ = 0;
    void DiagKeepFront()
    {
        if (!lastFront_) return;
        D3D11_TEXTURE2D_DESC d{}, p{};
        lastFront_->GetDesc(&d);
        if (diagPrevFront_) diagPrevFront_->GetDesc(&p);
        if (!diagPrevFront_ || p.Width != d.Width || p.Height != d.Height || p.Format != d.Format || p.MipLevels != d.MipLevels)
        {
            diagPrevFront_.Reset();
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &diagPrevFront_))) return;
        }
        context_->CopyResource(diagPrevFront_.Get(), lastFront_.Get());
        diagPrevW_ = lastFrontW_; diagPrevH_ = lastFrontH_;
    }
    bool ReadDiagPrev(uint32_t* out)
    {
        if (!diagPrevFront_) return false;
        std::vector<uint32_t> px;
        ReadLogical(diagPrevFront_.Get(), diagPrevW_, diagPrevH_, 1280, 720, true, px);
        for (size_t i = 0; i < px.size(); ++i) out[i] = DisplayPixel(px[i]);
        return true;
    }

    // TEST ONLY (p_diag31, TTT2_DIAG_FLASH=n): one-frame colour flashes (Tempest after other stages, 2026-10-27).
    struct FlashEv { char kind; uint32_t a, b, c, d, e, f; const void* p; const void* q; };
    struct FlashFrame { uint32_t present = 0; double unixMs = 0; std::vector<FlashEv> ev; std::vector<uint32_t> consts; };
    std::vector<uint32_t> flashCurConsts_;                  // p_diag32: this frame's draws' PS constant registers
    uint32_t flashVertexHash_ = 0, flashVertexDwords_ = 0;  // p_diag33: the next draw's copied vertex data
    std::unordered_set<uint32_t> flashSmallDests_;                    // p_diag32: small CPU-readback destinations seen
    // p_diag32: channel means (x1000, bytes 2/1/0 of each texel = red/green/blue as the texture holds them) of a small
    // readback - 'S' as scattered, 'G' as guest memory holds it now (tiled, 32 texels a row).
    void FlashSmall(char kind, uint32_t dest, const uint32_t* px, uint32_t n)
    {
        if (!n) return;
        uint64_t sum[3] = {};
        uint32_t zeroCh = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t v = px[i];
            const uint32_t c[3] = { (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF };
            sum[0] += c[0]; sum[1] += c[1]; sum[2] += c[2];
            if (!c[0] || !c[1] || !c[2]) ++zeroCh;
        }
        const uint32_t m[3] = { uint32_t(sum[0] * 1000 / n), uint32_t(sum[1] * 1000 / n), uint32_t(sum[2] * 1000 / n) };
        FlashNote({ kind, dest, m[0], m[1], m[2], zeroCh, n, nullptr, nullptr });
        static int logged = 0;
        if (flashArmed_ && (m[0] == 0 || m[1] == 0 || m[2] == 0) && logged++ < 300)   // p_diag33: a channel 0 in every texel
            std::fprintf(stderr, "[flash-lum] %.1f %c %08X: means %.2f %.2f %.2f, %u of %u texels with a zero channel, first %08X\n", FlashUnixMs(),
                         kind, dest, m[0] / 1000.0, m[1] / 1000.0, m[2] / 1000.0, zeroCh, n, px[0]);
    }
    static constexpr uint32_t FlashKeep = 16, FlashStages = 6;
    FlashFrame flashFrames_[FlashKeep];
    std::vector<FlashEv> flashCur_;
    ComPtr<ID3D11Texture2D> flashMip_, flashStage_[FlashStages], flashCopy_[FlashKeep];
    ComPtr<ID3D11ShaderResourceView> flashMipSrv_;
    uint32_t flashStageAt_[FlashStages] = {}, flashCopyAt_[FlashKeep] = {}, flashCopyW_[FlashKeep] = {}, flashCopyH_[FlashKeep] = {};
    uint32_t flashPresent_ = 0, flashLevel_ = 0, flashGW_ = 0, flashGH_ = 0;
    DXGI_FORMAT flashFormat_ = DXGI_FORMAT_UNKNOWN;
    bool flashBroken_ = false, flashArmed_ = false;
    struct FlashGrid { uint32_t present; std::vector<uint8_t> rgb; };
    std::deque<FlashGrid> flashGrids_;
    unsigned flashDumps_ = 0, flashSeen_ = 0;
    static unsigned FlashMax()
    {
        static const unsigned n = [] { const char* e = std::getenv("TTT2_DIAG_FLASH"); return e ? unsigned(std::strtoul(e, nullptr, 0)) : 0u; }();
        return n;
    }
    void FlashNote(const FlashEv& e) { if (FlashMax() && flashCur_.size() < 30000) flashCur_.push_back(e); }
    static double FlashUnixMs()
    {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        return (double((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000.0) / 10000.0;
    }
    void FlashPresent(ID3D11Texture2D* source, uint32_t logicalW, uint32_t logicalH)
    {
        if (!FlashMax() || flashBroken_) return;
        const uint32_t P = ++flashPresent_;
        FlashFrame& fr = flashFrames_[P % FlashKeep];
        fr.present = P; fr.unixMs = FlashUnixMs();
        for (uint32_t dest : flashSmallDests_)        // p_diag32: what the game's CPU would read now
        {
            const auto it = resolved_.find(dest);
            if (it == resolved_.end() || it->second.pitch != 32 || it->second.height != 1) continue;
            uint32_t px[32];
            for (uint32_t x = 0; x < 32; ++x) px[x] = GRead32(g_base, dest + GpuTiledOffset2D(x, 0, 32, 2) * 4);
            FlashSmall('G', dest, px, 32);
        }
        fr.ev.swap(flashCur_);
        flashCur_.clear();
        fr.consts.swap(flashCurConsts_);
        flashCurConsts_.clear();
        D3D11_TEXTURE2D_DESC sd{}, md{};
        source->GetDesc(&sd);
        if (flashMip_) flashMip_->GetDesc(&md);
        if (!flashMip_ || md.Width != sd.Width || md.Height != sd.Height || md.Format != sd.Format)
        {
            flashMip_.Reset(); flashMipSrv_.Reset();
            for (auto& t : flashStage_) t.Reset();
            for (auto& t : flashCopy_) t.Reset();
            flashGrids_.clear();
            UINT support = 0;
            if (FAILED(device_->CheckFormatSupport(sd.Format, &support)) || !(support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN))
            {
                std::fprintf(stderr, "[flash] front format %u cannot make mips: catcher off\n", unsigned(sd.Format));
                flashBroken_ = true;
                return;
            }
            D3D11_TEXTURE2D_DESC t = sd;
            t.MipLevels = 0; t.ArraySize = 1; t.SampleDesc.Count = 1; t.SampleDesc.Quality = 0; t.Usage = D3D11_USAGE_DEFAULT;
            t.CPUAccessFlags = 0; t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; t.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(device_->CreateTexture2D(&t, nullptr, &flashMip_)) ||
                FAILED(device_->CreateShaderResourceView(flashMip_.Get(), nullptr, &flashMipSrv_)))
            { std::fprintf(stderr, "[flash] no mip texture: catcher off\n"); flashBroken_ = true; return; }
            flashLevel_ = 0;
            while ((sd.Width >> (flashLevel_ + 1)) >= 32 && (sd.Height >> (flashLevel_ + 1)) >= 16) ++flashLevel_;
            flashGW_ = std::max(1u, sd.Width >> flashLevel_); flashGH_ = std::max(1u, sd.Height >> flashLevel_);
            D3D11_TEXTURE2D_DESC g{};
            g.Width = flashGW_; g.Height = flashGH_; g.MipLevels = g.ArraySize = 1; g.Format = sd.Format; g.SampleDesc.Count = 1;
            g.Usage = D3D11_USAGE_STAGING; g.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            D3D11_TEXTURE2D_DESC c = sd;
            c.MipLevels = 1; c.ArraySize = 1; c.SampleDesc.Count = 1; c.SampleDesc.Quality = 0; c.Usage = D3D11_USAGE_DEFAULT;
            c.CPUAccessFlags = 0; c.BindFlags = D3D11_BIND_SHADER_RESOURCE; c.MiscFlags = 0;
            for (auto& x : flashStage_) if (FAILED(device_->CreateTexture2D(&g, nullptr, &x))) { flashBroken_ = true; return; }
            for (auto& x : flashCopy_) if (FAILED(device_->CreateTexture2D(&c, nullptr, &x))) { flashBroken_ = true; return; }
            std::memset(flashStageAt_, 0, sizeof(flashStageAt_));
            std::memset(flashCopyAt_, 0, sizeof(flashCopyAt_));
            flashFormat_ = sd.Format;
            std::fprintf(stderr, "[flash] watching %ux%u pictures (format %u) as %ux%u cells (mip %u)\n", sd.Width, sd.Height,
                         unsigned(sd.Format), flashGW_, flashGH_, flashLevel_);
        }
        context_->CopySubresourceRegion(flashMip_.Get(), 0, 0, 0, 0, source, 0, nullptr);
        context_->GenerateMips(flashMipSrv_.Get());
        const uint32_t si = P % FlashStages, ci = P % FlashKeep;
        context_->CopySubresourceRegion(flashStage_[si].Get(), 0, 0, 0, 0, flashMip_.Get(), flashLevel_, nullptr);
        flashStageAt_[si] = P;
        context_->CopySubresourceRegion(flashCopy_[ci].Get(), 0, 0, 0, 0, source, 0, nullptr);
        flashCopyAt_[ci] = P; flashCopyW_[ci] = logicalW; flashCopyH_[ci] = logicalH;
        if (P % 30 == 0)     // dumps only while C:/ttt2/port/flash<tag>.arm exists (the boot logos flash legitimately)
        {
            static const std::string arm = [] { const char* t = std::getenv("TTT2_INSTANCE"); return "C:/ttt2/port/flash" + std::string(t ? t : "") + ".arm"; }();
            const bool was = flashArmed_;
            flashArmed_ = GetFileAttributesA(arm.c_str()) != INVALID_FILE_ATTRIBUTES;
            if (was != flashArmed_) std::fprintf(stderr, "[flash] %.1f dumps %s\n", FlashUnixMs(), flashArmed_ ? "armed" : "disarmed");
        }
        if (P <= 3) return;
        const uint32_t ri = (P - 3) % FlashStages;
        if (flashStageAt_[ri] != P - 3) return;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(context_->Map(flashStage_[ri].Get(), 0, D3D11_MAP_READ, 0, &m))) return;
        FlashGrid grid{ P - 3, std::vector<uint8_t>(size_t(flashGW_) * flashGH_ * 3) };
        const bool rgba = flashFormat_ == DXGI_FORMAT_R8G8B8A8_UNORM || flashFormat_ == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        for (uint32_t y = 0; y < flashGH_; ++y)
            for (uint32_t x = 0; x < flashGW_; ++x)
            {
                const uint8_t* px = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch + size_t(x) * 4;
                uint8_t* o = &grid.rgb[(size_t(y) * flashGW_ + x) * 3];
                o[0] = rgba ? px[0] : px[2]; o[1] = px[1]; o[2] = rgba ? px[2] : px[0];
            }
        context_->Unmap(flashStage_[ri].Get(), 0);
        if (!flashGrids_.empty())
        {
            int most = 0;
            const auto& last = flashGrids_.back().rgb;
            for (size_t k = 0; k < last.size(); ++k) most = std::max(most, std::abs(int(last[k]) - int(grid.rgb[k])));
            if (most <= 1) return;                              // the same picture again (a held frame)
        }
        flashGrids_.push_back(std::move(grid));
        if (flashGrids_.size() > 3) flashGrids_.pop_front();
        if (flashGrids_.size() < 3) return;
        const auto& a = flashGrids_[0].rgb; const auto& b = flashGrids_[1].rgb; const auto& c = flashGrids_[2].rgb;
        auto dist = [](const uint8_t* p, const uint8_t* q) {
            const int r = int(p[0]) - q[0], g = int(p[1]) - q[1], bl = int(p[2]) - q[2];
            return std::sqrt(float(r * r + g * g + bl * bl)); };
        const uint32_t cells = flashGW_ * flashGH_;
        auto chroma = [](const uint8_t* p) { return int(std::max({ p[0], p[1], p[2] })) - int(std::min({ p[0], p[1], p[2] })); };
        uint32_t n = 0, sat = 0;     // sat: flashing cells that turn a pure colour (the bug: magenta, green, blue ...)
        for (uint32_t k = 0; k < cells; ++k)
            if (dist(&a[k * 3], &c[k * 3]) < 20.0f && dist(&a[k * 3], &b[k * 3]) > 45.0f && dist(&c[k * 3], &b[k * 3]) > 45.0f)
            {
                ++n;
                if (chroma(&b[k * 3]) >= 150 && chroma(&a[k * 3]) < 120) ++sat;   // p_diag33: 150 (a cyan 92,255,252 band)
            }
        if (n * 100 < cells * 3) return;
        const bool worth = n * 100 >= cells * 6 || sat >= 4;
        const uint32_t pa = flashGrids_[0].present, pb = flashGrids_[1].present, pc = flashGrids_[2].present;
        if (flashSeen_++ < 400)
            std::fprintf(stderr, "[flash] %.1f present %u: %u of %u cells flash, %u pure colour (before %u, after %u)%s\n",
                         flashFrames_[pb % FlashKeep].present == pb ? flashFrames_[pb % FlashKeep].unixMs : 0.0, pb, n, cells, sat, pa, pc,
                         worth && flashArmed_ && flashDumps_ < FlashMax() ? ", dumped" : "");
        if (worth && flashArmed_ && flashDumps_ < FlashMax()) FlashDump(pa, pb, pc, n, cells);
    }
    void FlashDump(uint32_t pa, uint32_t pb, uint32_t pc, uint32_t n, uint32_t cells)
    {
        static const std::string tag = [] { const char* t = std::getenv("TTT2_INSTANCE"); return std::string(t ? t : ""); }();
        const unsigned k = flashDumps_++;
        char path[160];
        std::snprintf(path, sizeof(path), "C:/ttt2/port/fb/flash%s_%03u.txt", tag.c_str(), k);
        FILE* f = std::fopen(path, "w");
        if (!f) return;
        std::unordered_map<const void*, std::string> who;
        char s[200];
        for (const auto& [dest, r] : resolved_)
            if (r.srv)
            {
                std::snprintf(s, sizeof(s), "%s %08X %ux%u%s%s seq %u", r.depth ? "depth-resolve" : "resolve", dest, r.pitch, r.height,
                              r.valid ? "" : " INVALID", r.rg16f ? " rg16f" : "", r.seq);
                std::string& w = who[r.srv.Get()];
                w += w.empty() ? s : std::string(" + ") + s;
            }
        for (const auto& [key, v] : resolvePool_)
            for (const auto& p : v)
                if (p.srv)
                {
                    std::snprintf(s, sizeof(s), "POOLED %ux%u fmt %u", unsigned(key >> 40), unsigned((key >> 16) & 0xFFFFFF), unsigned(key & 0xFFFF));
                    std::string& w = who[p.srv.Get()];
                    w += w.empty() ? s : std::string(" + ") + s;
                }
        for (const auto& [key, c] : textures_)
            if (c.srv)
            {
                std::snprintf(s, sizeof(s), "cached %08X fmt %u %ux%u seq %u", key.base, key.format, key.width, key.height, c.seq);
                std::string& w = who[c.srv.Get()];
                w += w.empty() ? s : std::string(" + ") + s;
            }
        who[defaultSrv_.Get()] = "fallback"; who[defaultArraySrv_.Get()] = "fallback-array";
        who[whiteSrv_.Get()] = "white"; who[whiteArraySrv_.Get()] = "white-array";
        auto name = [&](const void* p) -> std::string {
            if (!p) return "null";
            const auto it = who.find(p);
            return it == who.end() ? "unknown" : it->second;
        };
        std::fprintf(f, "flash %u: present %u, %u of %u cells (before %u, after %u); dirty seq %u, texGen %llu\n", k, pb, n, cells, pa, pc,
                     dirty_.Current(), (unsigned long long)texGen_);
        for (uint32_t p = pa; p <= pc; ++p)
        {
            if (pc - pa > FlashKeep && p != pa && p != pb && p != pc) continue;
            const FlashFrame& fr = flashFrames_[p % FlashKeep];
            if (fr.present != p) { std::fprintf(f, "== present %u: not kept\n", p); continue; }
            uint32_t draws = 0;
            for (const auto& e : fr.ev) draws += e.kind == 'D';
            std::fprintf(f, "== present %u%s at %.1f: %u draws, %zu records\n", p, p == pb ? " (FLASH)" : "", fr.unixMs, draws, fr.ev.size());
            uint32_t d = 0;
            for (const auto& e : fr.ev)
            {
                switch (e.kind)
                {
                case 'D':
                    std::fprintf(f, "D %u color %05X mask %X depth %03X%s%s blend %08X n %u ps %p/%u vs %p slots %08X\n", d++, e.a & 0xFFFFFF,
                                 e.a >> 24, e.b & 0xFFFF, (e.b & 0x10000) ? " z" : "", (e.b & 0x20000) ? "w" : "", e.c, e.d, e.p, e.e, e.q, e.f);
                    break;
                case 'T':
                    std::fprintf(f, "  t%u %08X fmt %u %ux%u fc0 %08X fc3 %08X srv %p smp %p = %s\n", e.a, e.b, e.c, e.d & 0xFFFF, e.d >> 16, e.e, e.f,
                                 e.p, e.q, name(e.p).c_str());
                    break;
                case 'R':
                    std::fprintf(f, "R dest %08X from %05X %ux%u src %u fmt %u latch %u/%08X %s srv %p = %s\n", e.a, e.b, e.c & 0xFFFF, e.c >> 16,
                                 e.d & 0xFF, (e.d >> 8) & 0xFF, (e.d >> 16) & 1, e.e, e.f ? "NEW TEXTURE" : "same texture", e.p, name(e.p).c_str());
                    break;
                case 'M': std::fprintf(f, "M %s %ux%u fmt %u srv %p\n", e.a ? "from pool" : "created", e.b, e.c, e.d, e.p); break;
                case 'X': std::fprintf(f, "X to pool %ux%u fmt %u (pool %u) srv %p\n", e.a, e.b, e.c, e.d, e.p); break;
                case 'U': std::fprintf(f, "U %08X fmt %u %ux%u levels %u slices %u tiled %u srv %p\n", e.a, e.b, e.c & 0xFFFF, e.c >> 16, e.d, e.e, e.f, e.p); break;
                case 'F': std::fprintf(f, "F texture cache flushed (%u)\n", e.a); break;
                case 'W': std::fprintf(f, "W write-back %08X %ux%u seq %u srv %p\n", e.a, e.b, e.c, e.d, e.p); break;
                case 'I': std::fprintf(f, "I resolve %08X (%u bytes, seq %u) overwritten by the CPU srv %p\n", e.a, e.b, e.c, e.p); break;
                case 'C': std::fprintf(f, "C constants block %u%s\n", e.a, e.b ? " (UPLOADED CONSTANTS DIFFER FROM THE REGISTERS)" : ""); break;
                case 'V': std::fprintf(f, "V vertex data %08X (%u dwords)\n", e.a, e.b); break;
                case 'S': case 'G':
                    std::fprintf(f, "%c small %08X %s: means %.3f %.3f %.3f, %u of %u texels with a zero channel\n", e.kind, e.a,
                                 e.kind == 'S' ? "scattered" : "guest memory", e.b / 1000.0, e.c / 1000.0, e.d / 1000.0, e.e, e.f);
                    break;
                default: break;
                }
            }
        }
        std::fclose(f);
        std::snprintf(path, sizeof(path), "C:/ttt2/port/fb/flash%s_%03u_consts.bin", tag.c_str(), k);   // p_diag32
        if (FILE* cf = std::fopen(path, "wb"))
        {
            for (uint32_t p = pa; p <= pc; ++p)
            {
                const FlashFrame& fr = flashFrames_[p % FlashKeep];
                if (fr.present != p) continue;
                const uint32_t hdr[2] = { p, uint32_t(fr.consts.size() / 1024) };
                std::fwrite(hdr, 4, 2, cf);
                if (!fr.consts.empty()) std::fwrite(fr.consts.data(), 4, fr.consts.size(), cf);
            }
            std::fclose(cf);
        }
        const uint32_t pics[3] = { pa, pb, pc };
        const char* sfx[3] = { "a", "b", "c" };
        std::vector<uint32_t> px;
        for (int i = 0; i < 3; ++i)
        {
            const uint32_t ci = pics[i] % FlashKeep;
            if (flashCopyAt_[ci] != pics[i]) continue;
            ReadLogical(flashCopy_[ci].Get(), flashCopyW_[ci], flashCopyH_[ci], 1280, 720, true, px);
            for (uint32_t& v : px) v = DisplayPixel(v);
            std::snprintf(path, sizeof(path), "C:/ttt2/port/fb/flash%s_%03u_%s.bmp", tag.c_str(), k, sfx[i]);
            WriteBmp(path, px.data(), 1280, 720);
        }
    }

    // A stored front-buffer texel as shown: red / blue as presented, the gamma ramp, alpha cleared.
    uint32_t DisplayPixel(uint32_t v) const
    {
        if (lastSwap_) v = (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
        if (gammaApplied_)
            v = (uint32_t(gamma8_[0][(v >> 16) & 0xFF]) << 16) | (uint32_t(gamma8_[1][(v >> 8) & 0xFF]) << 8) |
                gamma8_[2][v & 0xFF];
        return v & 0x00FFFFFFu;
    }
    static bool NativeDumpOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_FB_NATIVE"); return e && *e == '1'; }();
        return on;
    }

private:
    // ---- display gamma: the guest's 256-entry DC_LUT table (kernel_video.cpp), applied to the
    // presented image as the console's scan-out and Xenia's apply-gamma pass do (8 bpc: each
    // channel indexes the table), then Display Settings' BRIGHTNESS as a power curve on its
    // output (level L: value ^ 2^(-L/8), +5 lifts mid grey 0.50 to 0.64). Skipped while the
    // table is the identity and BRIGHTNESS is 0. TTT2_GAMMA_RAMP=0 presents the frame buffer
    // unramped (BRIGHTNESS still applies).
    static bool GammaRampEnabled()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_GAMMA_RAMP"); return !e || std::strcmp(e, "0") != 0; }();
        return on;
    }
    ComPtr<ID3D11Texture2D> gammaTex_;
    ComPtr<ID3D11ShaderResourceView> gammaSrv_;
    uint32_t gammaVersion_ = ~0u;
    int gammaBrightness_ = 0;
    bool gammaApplied_ = false;
    uint8_t gamma8_[3][256] = {};
    void UpdateGamma(const uint32_t* ramp, uint32_t version)
    {
        const int brightness = display_settings::GetBrightness();
        if (!ramp || (version == gammaVersion_ && brightness == gammaBrightness_)) return;
        const bool rampChanged = version != gammaVersion_;
        gammaVersion_ = version;
        gammaBrightness_ = brightness;
        const bool rampOn = GammaRampEnabled();
        const float exponent = std::exp2(-float(brightness) / 8.0f);
        float table[256][4];
        bool identity = true;
        for (uint32_t i = 0; i < 256; ++i)
        {
            const uint32_t e = ramp[i], identityValue = i * 0x3FF / 0xFF;
            const uint32_t c[3] = { (e >> 20) & 0x3FF, (e >> 10) & 0x3FF, e & 0x3FF };   // red, green, blue
            for (int k = 0; k < 3; ++k)
            {
                identity = identity && c[k] == identityValue;
                const float v = rampOn ? float(c[k]) / 1023.0f : float(i) / 255.0f;
                table[i][k] = brightness ? std::pow(v, exponent) : v;
                gamma8_[k][i] = brightness ? uint8_t(std::lround(table[i][k] * 255.0f))
                                           : uint8_t(((rampOn ? c[k] : identityValue) * 255 + 511) / 1023);
            }
            table[i][3] = 1.0f;
        }
        gammaApplied_ = (rampOn && !identity) || brightness != 0;
        static int logged = 0;
        if (rampChanged && logged++ < 4)
            std::fprintf(stderr, "[gpu-hw] display gamma ramp %s (entry 128 = %.4f, identity %.4f)%s\n",
                         identity ? "identity" : "loaded", float(ramp[128] >> 20 & 0x3FF) / 1023.0f, 128.0f / 255.0f,
                         !identity && !rampOn ? " - not applied (TTT2_GAMMA_RAMP=0)" : "");
        if (!gammaApplied_) return;
        if (!gammaTex_)
        {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = 256; d.Height = 1; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{ table, sizeof(table), 0 };
            Check(device_->CreateTexture2D(&d, &init, &gammaTex_), "CreateGammaTexture");
            Check(device_->CreateShaderResourceView(gammaTex_.Get(), nullptr, &gammaSrv_), "CreateGammaSrv");
        }
        else context_->UpdateSubresource(gammaTex_.Get(), 0, nullptr, table, sizeof(table), 0);
    }
    // ---- window modes (display_settings): the window thread (kernel_video.cpp) gives the window
    // its style and size; this thread follows its client size with the swap chain buffers and
    // owns exclusive fullscreen (entered at the chosen resolution, re-entered only while the
    // window is in the foreground so Alt+Tab is not fought).
    UINT bufW_ = 0, bufH_ = 0;
    bool tearing_ = false;                        // the system supports presents with tearing (VSYNC OFF)
    uint32_t refreshHz_ = 0;                      // the window's display, when the swap chain was made
    UINT SwapChainFlags() const
    {
        return DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    }
    static uint32_t DisplayHz(HWND w)
    {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi) ||
            !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
        return dm.dmDisplayFrequency;
    }
    // Diagnostics: the swap chain's output - name, colour space (HDR or not), bits per colour.
    std::string OutputInfo() const
    {
        ComPtr<IDXGIOutput> out;
        ComPtr<IDXGIOutput6> out6;
        DXGI_OUTPUT_DESC1 d{};
        if (!swapchain_ || FAILED(swapchain_->GetContainingOutput(&out)) || FAILED(out.As(&out6)) || FAILED(out6->GetDesc1(&d)))
            return "output unknown";
        char s[200];
        std::snprintf(s, sizeof(s), "output %s, colour space %d%s, %u bits per colour", LogUtf8(d.DeviceName).c_str(), int(d.ColorSpace),
                      d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? " (HDR on)" : "", d.BitsPerColor);
        return s;
    }
    // Diagnostics: the visible windows above the game's on its monitor (front first) - what covers an
    // exclusive fullscreen swap chain when DXGI reports it occluded or drops it. At most 12 reports.
    static void LogWindowsAbove(HWND w, const char* why)
    {
        static int reports = 0;
        if (reports++ >= 12) return;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
        std::fprintf(stderr, "[gpu-hw] %s: windows above the game (foreground %p, game %p):\n", why,
                     static_cast<void*>(GetForegroundWindow()), static_cast<void*>(w));
        int n = 0;
        for (HWND h = GetWindow(w, GW_HWNDPREV); h && n < 16; h = GetWindow(h, GW_HWNDPREV))
        {
            RECT r{}, x{};
            if (!IsWindowVisible(h) || !GetWindowRect(h, &r) || !IntersectRect(&x, &r, &mi.rcMonitor)) continue;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            // Wide, printed as UTF-8 (log_utf8.h): titles and program names come in the player's language, which -A
            // gave in the ANSI code page or as '?'. Sizes in characters.
            wchar_t exe[MAX_PATH] = L"?", cls[96] = L"?", title[96] = L"";
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
            {
                DWORD len = MAX_PATH;
                QueryFullProcessImageNameW(p, 0, exe, &len);
                CloseHandle(p);
            }
            GetClassNameW(h, cls, 96);
            // Its count and end are not trusted: for a window of this process they are the window's own WM_GETTEXT
            // answer (an ANSI window with a long title gave up to 191 and no terminator, MEASURED 2026-11-02).
            GetWindowTextW(h, title, 96);
            title[95] = 0;
            const size_t titleLength = std::wcslen(title);
            if (titleLength > 0 && IS_HIGH_SURROGATE(title[titleLength - 1])) title[titleLength - 1] = 0;   // a cut pair: LogUtf8 gives "?"
            const wchar_t* exeName = std::wcsrchr(exe, L'\\') ? std::wcsrchr(exe, L'\\') + 1 : exe;
            const LONG ex = GetWindowLongA(h, GWL_EXSTYLE);
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            std::fprintf(stderr, "[gpu-hw]   %s pid %lu class \"%s\" title \"%s\" %ld,%ld %ldx%ld%s%s%s%s\n",
                         LogUtf8(exeName).c_str(), pid, LogUtf8(cls).c_str(), LogUtf8(title).c_str(), r.left, r.top,
                         r.right - r.left, r.bottom - r.top, (ex & WS_EX_TOPMOST) ? " topmost" : "",
                         (ex & WS_EX_LAYERED) ? " layered" : "", (ex & WS_EX_TRANSPARENT) ? " click-through" : "",
                         cloaked ? " cloaked" : "");
            ++n;
        }
        if (!n) std::fprintf(stderr, "[gpu-hw]   (none)\n");
    }
    // VSYNC (Options -> Display Settings, display_settings.h). The game keeps its own 60 Hz pace either
    // way (the vblank ticker, kernel_video.cpp); this only decides when a finished frame is shown.
    //   Windowed / borderless: sync interval 0. ON without tearing - a newer frame replaces a queued
    //     one, so presents never wait for the display or queue up behind it; OFF with
    //     DXGI_PRESENT_ALLOW_TEARING, shown at once wherever Windows lets the swap chain flip by itself
    //     (independent flip). A window DWM composes waits for its next composition either way
    //     (MEASURED 2026-10-20: a borderless window in front was composed on the test PC).
    //   Exclusive fullscreen: sync interval 0 flips at once (tears) there, so ON takes interval 1, with
    //     DXGI_PRESENT_DO_NOT_WAIT: a frame that would wait for the display is dropped instead, so the
    //     display never holds the game under 60 frames a second (a 50 Hz mode shows 50 of them).
    // TTT2_PRESENT_INTERVAL=<n> (experiment) forces the sync interval.
    // p_diag68: how many frames DXGI lets this thread queue ahead of the GPU. Its default, 3, kept up to three finished
    // frames waiting behind a graphics card that could not keep up - each one a frame more from a press to the screen.
    // MEASURED 2026-11-01 (Practice; TTT2_DIAG_GPU_LOAD standing in for a slower card, the GPU done with a frame after its
    // present call): ~19 ms of GPU work a frame -> 52.6 fps, done 55 ms after the present (three frames queued); at most
    // one -> 51.7 fps, 19 ms. ~14 ms a frame: 60 fps either way, the heavier screens 21.6 -> 12.3 ms (medians). At the
    // tester's balance (TTT2_DIAG_CP_SPIN_NS=16000 + TTT2_DIAG_MAIN_SPIN_US=5800 + ~12 ms of GPU work): 60 fps either
    // way, p95 18.5 -> 15.0 ms. LOW LATENCY AUTO and ON keep one; OFF leaves DXGI's 3 (the old way). Set before every
    // present, so a change in the menu applies at once. TTT2_FRAME_LATENCY=<1-16> (tests) fixes it.
    UINT frameLatency_ = 0;
    void ApplyFrameLatency()
    {
        static const int forced = [] { const char* e = std::getenv("TTT2_FRAME_LATENCY"); const int n = e ? std::atoi(e) : 0; return n >= 1 && n <= 16 ? n : 0; }();
        const UINT want = forced ? UINT(forced) : display_settings::GetLowLatency() == display_settings::LowLatencyOff ? 3u : 1u;
        if (want == frameLatency_) return;
        if (!frameLatency_ && want == 3)                // TEST 18: OFF from the start leaves DXGI's own default alone, as TEST 15 did
        {
            frameLatency_ = 3;
            std::fprintf(stderr, "[gpu-hw] frames queued ahead of the GPU: DXGI's default (LOW LATENCY OFF)\n");
            return;
        }
        frameLatency_ = want;                           // tried once per change, failed or not
        ComPtr<IDXGIDevice1> dxgi1;
        const bool set = SUCCEEDED(device_.As(&dxgi1)) && SUCCEEDED(dxgi1->SetMaximumFrameLatency(want));
        std::fprintf(stderr, "[gpu-hw] frames queued ahead of the GPU: at most %u (%s)%s\n", want,
                     forced ? "TTT2_FRAME_LATENCY" : want == 1 ? "LOW LATENCY AUTO / ON" : "LOW LATENCY OFF", set ? "" : " - FAILED");
    }
    std::pair<UINT, UINT> PresentMode(bool noWait) const
    {
        static const int forced = [] { const char* e = std::getenv("TTT2_PRESENT_INTERVAL"); return e && *e ? std::clamp(std::atoi(e), 0, 4) : -1; }();
        const bool vsync = display_settings::GetVSync(), exclusive = exclusive_.load();
        UINT interval = exclusive && vsync ? 1u : 0u;
        if (forced >= 0) interval = UINT(forced);
        UINT flags = noWait || interval ? DXGI_PRESENT_DO_NOT_WAIT : 0u;
        if (!vsync && tearing_ && interval == 0 && !exclusive) flags |= DXGI_PRESENT_ALLOW_TEARING;
        return { interval, flags };
    }
    std::atomic<bool> exclusive_{ false };
    std::chrono::steady_clock::time_point lastExclusiveTry_{};
    display_settings::Resolution exclusiveRes_{ 0, 0 };     // display mode set for exclusive fullscreen
    HRESULT lastPresentHr_ = S_OK;
    uint32_t toggle_ = 0;                         // TTT2_HW_TOGGLE
    std::array<uint32_t, 8> presentHist_{};      // TTT2_FRAMETIME: DXGI Present durations, calls, would-block
    double presentHistMs_ = 0;
    ComPtr<ID3D11Query> diagQuery_;              // TTT2_PRESENT_DIAG
    double diagFlushMs_ = 0, diagDrainMs_ = 0, diagDrainMax_ = 0;
    bool resizeNeeded_ = false;                               // resize the buffers at the next present
    std::chrono::steady_clock::time_point lastInvalidRetry_{};
    // Exclusive fullscreen that Windows takes back at once while the game is still in front is retried a
    // few times, then left alone: the window stays a borderless popup over the monitor until the game is
    // switched back to or a display setting changes. MEASURED 2026-10-20 on a PC running Sunshine, Parsec,
    // Virtual Desktop and MuMuPlayer: DXGI kept the swap chain composed by DWM after SetFullscreenState,
    // the first present came back DXGI_STATUS_OCCLUDED and DXGI left fullscreen - and re-entering once a
    // second blinked the screen every second (v0.1.4 to v0.1.6 alike).
    int exclusiveFailures_ = 0;                               // taken back in front of the player, in a row
    bool exclusiveBlocked_ = false;
    bool wasForeground_ = false;
    uint32_t exclusiveGeneration_ = 0;                        // display_settings::Generation() last seen
    std::chrono::steady_clock::time_point exclusiveSince_{};
    std::chrono::steady_clock::time_point generationSince_{}; // the first present, then each change of exclusiveGeneration_
    uint32_t behindLoggedGeneration_ = ~0u;
    // Diagnostics: exclusive fullscreen chosen but the game is not the window in front (it waits for that), 2 s after a
    // settings change: which program's window is - its file name and window class, not its title. Once per change.
    void LogBehind(uint32_t generation)
    {
        static int reports = 0;
        if (behindLoggedGeneration_ == generation || std::chrono::steady_clock::now() - generationSince_ < std::chrono::seconds(2))
            return;
        behindLoggedGeneration_ = generation;
        if (reports++ >= 8) return;
        const HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        wchar_t exe[MAX_PATH] = L"?", cls[96] = L"?";   // printed as UTF-8 (log_utf8.h), as in LogWindowsAbove
        if (HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr)
        {
            DWORD len = MAX_PATH;
            QueryFullProcessImageNameW(p, 0, exe, &len);
            CloseHandle(p);
        }
        if (fg) GetClassNameW(fg, cls, 96);
        const std::string inFront = fg ? LogUtf8(std::wcsrchr(exe, L'\\') ? std::wcsrchr(exe, L'\\') + 1 : exe) : "no window";
        std::fprintf(stderr, "[gpu-hw] exclusive fullscreen waits: the game is not the window in front (in front: %s, pid %lu%s, "
                             "class \"%s\"); click the game or Alt+Tab to it\n",
                     inFront.c_str(), pid, pid == GetCurrentProcessId() ? " - this game" : "", LogUtf8(cls).c_str());
    }
    void ApplyDisplay(HWND w)
    {
        BOOL fs = FALSE;
        swapchain_->GetFullscreenState(&fs, nullptr);
        const bool want = display_settings::GetMode() == display_settings::Fullscreen;
        const display_settings::Resolution r = display_settings::GetResolution();
        DXGI_MODE_DESC m{};
        m.Width = r.width; m.Height = r.height; m.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        const bool foreground = GetForegroundWindow() == w;
        const uint32_t generation = display_settings::Generation();
        if ((foreground && !wasForeground_) || generation != exclusiveGeneration_)
        {
            const bool settingsChanged = generation != exclusiveGeneration_;
            if (exclusiveBlocked_)
                std::fprintf(stderr, "[gpu-hw] exclusive fullscreen: trying again (%s)\n",
                             settingsChanged ? "display settings changed" : "the game is in front again");
            // Back in front after a block: one try (a failure blocks again at once); new settings: three.
            exclusiveFailures_ = exclusiveBlocked_ && !settingsChanged ? 2 : 0;
            exclusiveBlocked_ = false;
        }
        wasForeground_ = foreground;
        if (generation != exclusiveGeneration_ || generationSince_ == std::chrono::steady_clock::time_point{})
            generationSince_ = std::chrono::steady_clock::now();
        exclusiveGeneration_ = generation;
        if (want && !fs && !foreground) LogBehind(generation);
        if (want && !fs && foreground && !exclusiveBlocked_ &&
            std::chrono::steady_clock::now() - lastExclusiveTry_ > std::chrono::seconds(1))
        {
            lastExclusiveTry_ = std::chrono::steady_clock::now();
            // DXGI picks the fullscreen mode from the buffers' size: a swap chain made while the window was
            // still 1280x720 switched the monitor to 1280x720 first, then to the chosen mode (MEASURED
            // 2026-10-20) - so the buffers take the chosen size before the switch.
            if (bufW_ != r.width || bufH_ != r.height) ResizeSwapBuffers(r.width, r.height);
            swapchain_->ResizeTarget(&m);
            const HRESULT hr = swapchain_->SetFullscreenState(TRUE, nullptr);
            std::fprintf(stderr, "[gpu-hw] exclusive fullscreen %ux%u: %s (%08X)\n", r.width, r.height,
                         SUCCEEDED(hr) ? "on" : "refused", static_cast<unsigned>(hr));
            if (SUCCEEDED(hr))
            {
                m.RefreshRate = {}; swapchain_->ResizeTarget(&m); fs = TRUE; exclusiveRes_ = r;
                exclusiveSince_ = std::chrono::steady_clock::now();
                refreshHz_ = DisplayHz(w);
                std::fprintf(stderr, "[gpu-hw] exclusive fullscreen refresh %u Hz\n", refreshHz_);
            }
        }
        else if (want && fs && (r.width != exclusiveRes_.width || r.height != exclusiveRes_.height))
        {
            // A new RESOLUTION while exclusive: switch the display mode in place (DXGI then resizes
            // the window; the buffers follow below).
            const HRESULT hr = swapchain_->ResizeTarget(&m);
            std::fprintf(stderr, "[gpu-hw] exclusive fullscreen mode -> %ux%u (%08X)\n", r.width, r.height, static_cast<unsigned>(hr));
            exclusiveRes_ = r;
            resizeNeeded_ = true;
        }
        else if (!want && fs)
        {
            swapchain_->SetFullscreenState(FALSE, nullptr);
            fs = FALSE;
            std::fprintf(stderr, "[gpu-hw] exclusive fullscreen off\n");
        }
        // A flip-model swap chain must resize its buffers after every fullscreen transition (ours,
        // or DXGI's own on Alt+Tab) even at an unchanged size: until then every Present fails
        // with DXGI_ERROR_INVALID_CALL and the screen keeps the last frame. MEASURED 2026-10-12:
        // borderless 2560x1440 -> fullscreen 2560x1440 froze the picture. A failing Present also
        // retries the resize (at most once a second).
        if ((fs != FALSE) != exclusive_.load() || resizeNeeded_)
        {
            resizeNeeded_ = true;
            refreshHz_ = DisplayHz(w);          // VSYNC in exclusive fullscreen follows the mode's refresh
        }
        if (lastPresentHr_ == DXGI_ERROR_INVALID_CALL &&
            std::chrono::steady_clock::now() - lastInvalidRetry_ > std::chrono::seconds(1))
        {
            lastInvalidRetry_ = std::chrono::steady_clock::now();
            resizeNeeded_ = true;
        }
        if ((fs != FALSE) != exclusive_.load())
        {
            static int logged = 0;
            if (logged++ < 40)
                std::fprintf(stderr, "[gpu-hw] %.1f ms: exclusive fullscreen state %d -> %d (wanted %d, foreground %d), %s\n",
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                             int(exclusive_.load()), int(fs != FALSE), int(want), int(GetForegroundWindow() == w), OutputInfo().c_str());
            if (!fs && want && foreground)
            {
                // Taken back while the game is in front: not an Alt+Tab. Three quick ones in a row and
                // the game stops asking (see exclusiveBlocked_).
                LogWindowsAbove(w, "exclusive fullscreen lost");
                exclusiveFailures_ = std::chrono::steady_clock::now() - exclusiveSince_ < std::chrono::seconds(10) ? exclusiveFailures_ + 1 : 1;
                if (exclusiveFailures_ >= 3 && !exclusiveBlocked_)
                {
                    exclusiveBlocked_ = true;
                    std::fprintf(stderr, "[gpu-hw] exclusive fullscreen: Windows keeps taking it back while the game is in "
                                         "front (another program capturing or covering the screen?) - staying a borderless "
                                         "window over the whole monitor until the game is switched back to\n");
                }
            }
        }
        exclusive_ = fs != FALSE;
        RECT rc{};
        GetClientRect(w, &rc);
        const UINT cw = std::max<LONG>(1, rc.right - rc.left), ch = std::max<LONG>(1, rc.bottom - rc.top);
        if (cw != bufW_ || ch != bufH_ || resizeNeeded_)
        {
            resizeNeeded_ = false;
            ResizeSwapBuffers(cw, ch);
        }
    }
    void ResizeSwapBuffers(UINT cw, UINT ch)
    {
        pipe_.valid = false;
        backRtv_.Reset();
        backRtvFor_ = nullptr;
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->Flush();
        const HRESULT hr = swapchain_->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, SwapChainFlags());
        if (SUCCEEDED(hr)) { bufW_ = cw; bufH_ = ch; }
        std::fprintf(stderr, "[gpu-hw] back buffer %ux%u%s\n", cw, ch, SUCCEEDED(hr) ? "" : " (resize failed)");
    }

    // The game image into the back buffer: letterboxed to its own aspect, bilinear, red/blue
    // exchanged when the stored order differs from the display swizzle, then the guest's gamma
    // ramp - one pass. Variants: [swap | gamma << 1].
    // ---- the presented image: the front buffer (render resolution) into the window's 16:9 area.
    // Passes (Options -> Display Settings, display_settings.h):
    //   1. display: the red / blue exchange and the game's gamma ramp, texel for texel
    //   2. ANTI-ALIASING SMAA 1x at render resolution (edges, blending weights, blending); SSAA
    //      raises the render scale instead (RequestedScale) and step 3 shrinks it
    //   3. scale to the window area: texel for texel when the sizes match; Catmull-Rom bicubic when
    //      enlarging; an area filter when shrinking (bilinear taps under a texel apart across the
    //      pixel's footprint - plain bilinear skipped texels: a 1080p frame in a window a little
    //      smaller rippled, and a resolution above the screen's aliased)
    //   4. SHARPNESS: contrast adaptive sharpening (the filter of AMD FidelityFX CAS, MIT) at the
    //      window's resolution.
    // Sizes equal and both options off: step 1 alone writes the back buffer.
    struct PresentCB { float srcW, srcH, dstW, dstH, originX, originY, sharp; uint32_t mode; };
    ComPtr<ID3D11VertexShader> presentVs_;
    ComPtr<ID3D11PixelShader> displayPs_[4], smaaEdgesPs_, smaaWeightsPs_, smaaBlendPs_, scalePs_, casPs_;
    ComPtr<ID3D11SamplerState> presentSampler_;
    ComPtr<ID3D11Buffer> presentCb_;
    struct PostTarget { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11RenderTargetView> rtv; ComPtr<ID3D11ShaderResourceView> srv; UINT w = 0, h = 0; };
    PostTarget post_[5];
    PostTarget& Post(int i, UINT w, UINT h)
    {
        PostTarget& p = post_[i];
        if (p.tex && p.w == w && p.h == h) return p;
        p = PostTarget{};
        D3D11_TEXTURE2D_DESC t{};
        t.Width = w; t.Height = h; t.MipLevels = t.ArraySize = 1; t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(device_->CreateTexture2D(&t, nullptr, &p.tex), "CreatePostTexture");
        Check(device_->CreateRenderTargetView(p.tex.Get(), nullptr, &p.rtv), "CreatePostRtv");
        Check(device_->CreateShaderResourceView(p.tex.Get(), nullptr, &p.srv), "CreatePostSrv");
        p.w = w; p.h = h;
        return p;
    }
    ComPtr<ID3D11PixelShader> PresentShader(const std::string& body, const char* name)
    {
        const std::string src =
            "cbuffer P : register(b0) { float2 srcSize; float2 dstSize; float2 origin; float sharp; uint mode; };\n"
            "Texture2D<float4> src : register(t0); Texture2D<float4> lut : register(t1); SamplerState lin : register(s0);\n" + body;
        auto code = Compile(src, "ps_5_0", name);
        ComPtr<ID3D11PixelShader> ps;
        Check(device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps), name);
        return ps;
    }
    void PresentShaders()
    {
        if (presentVs_) return;
        auto vs = Compile("struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };"
                          " O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);"
                          " o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = t; return o; }",
                          "vs_5_0", "ttt2 present vs");
        Check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &presentVs_), "CreatePresentVs");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device_->CreateSamplerState(&sd, &presentSampler_), "CreatePresentSampler");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(PresentCB); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device_->CreateBuffer(&bd, nullptr, &presentCb_), "CreatePresentCb");
        for (int v = 0; v < 4; ++v)
        {
            std::string body =
                "float4 main(float4 pos : SV_Position) : SV_Target {\n"
                "  float4 c = src.Load(int3(int2(pos.xy - origin), 0))";
            body += (v & 1) ? ".bgra;\n" : ";\n";
            if (v & 2)
                body += "  uint3 i = (uint3)round(saturate(c.rgb) * 255.0);\n"
                        "  c.rgb = float3(lut.Load(int3(i.r, 0, 0)).r, lut.Load(int3(i.g, 0, 0)).g, lut.Load(int3(i.b, 0, 0)).b);\n";
            body += "  return float4(c.rgb, 1); }\n";
            displayPs_[v] = PresentShader(body, "ttt2 present display ps");
        }
        // SMAA 1x (Jimenez et al., "SMAA: Enhanced Subpixel Morphological Antialiasing", MIT): luma edges; for each edge
        // the line's length both ways and the crossing edges at its two ends give its pattern, whose revectorized line
        // sets how much of the neighbour's colour a pixel takes. The pattern areas are computed here (SMAA's area texture
        // holds the same values); orthogonal patterns, no diagonal or corner passes; lines followed up to 24 pixels a way.
        smaaEdgesPs_ = PresentShader(
            "float Lu(int2 p) { return dot(src.Load(int3(clamp(p, int2(0, 0), int2(dstSize) - 1), 0)).rgb, float3(0.2126, 0.7152, 0.0722)); }\n"
            "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
            "  int2 p = int2(uv * dstSize);\n"
            "  float L = Lu(p), Ll = Lu(p + int2(-1, 0)), Lt = Lu(p + int2(0, -1));\n"
            "  float2 dl = abs(L - float2(Ll, Lt));\n"
            "  float2 e = step(0.1, dl);\n"
            "  if (e.x + e.y == 0.0) return float4(0, 0, 0, 1);\n"
            "  float2 mx = max(dl, abs(L - float2(Lu(p + int2(1, 0)), Lu(p + int2(0, 1)))));\n"
            "  mx = max(mx, abs(float2(Ll, Lt) - float2(Lu(p + int2(-2, 0)), Lu(p + int2(0, -2)))));\n"
            "  e *= step(max(mx.x, mx.y), 2.0 * dl);\n"
            "  return float4(e, 0, 1); }\n",
            "ttt2 present smaa edges ps");
        // Edges: r = an edge on the pixel's left side, g = on its top side. Weights: r / g = how much the pixel takes from
        // the one above / the one above takes from it, b / a = the same with the pixel on the left.
        smaaWeightsPs_ = PresentShader(
            "static const int MaxD = 24;\n"
            "float2 E(int2 p) { return src.Load(int3(p, 0)).rg; }\n"
            "float2 Area(float2 p1, float2 p2, float x) {\n"
            "  float2 d = p2 - p1;\n"
            "  float x1 = x, x2 = x + 1.0;\n"
            "  if (!((x1 >= p1.x && x1 < p2.x) || (x2 > p1.x && x2 <= p2.x))) return 0;\n"
            "  float y1 = p1.y + d.y * (x1 - p1.x) / d.x, y2 = p1.y + d.y * (x2 - p1.x) / d.x;\n"
            "  if ((y1 >= 0.0) == (y2 >= 0.0) || abs(y1) < 1e-4 || abs(y2) < 1e-4) {\n"
            "    float a = (y1 + y2) * 0.5; return a < 0.0 ? float2(-a, 0) : float2(0, a); }\n"
            "  float xz = p1.x - p1.y * d.x / d.y, f = frac(xz);\n"
            "  float a1 = xz > p1.x ? y1 * f * 0.5 : 0.0, a2 = xz < p2.x ? y2 * (1.0 - f) * 0.5 : 0.0;\n"
            "  return (abs(a1) > abs(a2) ? a1 : -a2) < 0.0 ? float2(abs(a1), abs(a2)) : float2(abs(a2), abs(a1)); }\n"
            "float2 Smooth(float d, float2 a1, float2 a2) {\n"
            "  float s = saturate(d / 32.0);\n"
            "  return lerp(sqrt(a1 * 2.0) * 0.5, a1, s) + lerp(sqrt(a2 * 2.0) * 0.5, a2, s); }\n"
            "float2 Ortho(int pt, float n, float f) {\n"
            "  float d = n + f + 1.0, h = d * 0.5;\n"
            "  if (pt == 1) return n <= f ? Area(float2(0, -0.5), float2(h, 0), n) : float2(0, 0);\n"
            "  if (pt == 2) return n >= f ? Area(float2(h, 0), float2(d, -0.5), n) : float2(0, 0);\n"
            "  if (pt == 3) return Smooth(d, Area(float2(0, -0.5), float2(h, 0), n), Area(float2(h, 0), float2(d, -0.5), n));\n"
            "  if (pt == 4) return n <= f ? Area(float2(0, 0.5), float2(h, 0), n) : float2(0, 0);\n"
            "  if (pt == 8) return n >= f ? Area(float2(h, 0), float2(d, 0.5), n) : float2(0, 0);\n"
            "  if (pt == 12) return Smooth(d, Area(float2(0, 0.5), float2(h, 0), n), Area(float2(h, 0), float2(d, 0.5), n));\n"
            "  if (pt == 6 || pt == 7 || pt == 14) return Area(float2(0, 0.5), float2(d, -0.5), n);\n"
            "  if (pt == 9 || pt == 11 || pt == 13) return Area(float2(0, -0.5), float2(d, 0.5), n);\n"
            "  return 0; }\n"
            "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
            "  int2 p = int2(uv * dstSize), size = int2(dstSize);\n"
            "  float2 e = E(p);\n"
            "  float4 w = 0;\n"
            "  if (e.g > 0.5) {\n"
            "    int l = 0, r = 0;\n"
            "    [loop] for (; l < MaxD; ++l) { int2 q = p - int2(l, 0);\n"
            "      if (q.x == 0 || E(q).r > 0.5 || E(q - int2(0, 1)).r > 0.5 || E(q - int2(1, 0)).g < 0.5) break; }\n"
            "    [loop] for (; r < MaxD; ++r) { int2 q = p + int2(r + 1, 0);\n"
            "      if (q.x >= size.x || E(q).r > 0.5 || E(q - int2(0, 1)).r > 0.5 || E(q).g < 0.5) break; }\n"
            "    int2 a = p - int2(l, 0), b = p + int2(r + 1, 0);\n"
            "    int pt = (E(a).r > 0.5 ? 1 : 0) | (E(a - int2(0, 1)).r > 0.5 ? 4 : 0);\n"
            "    if (b.x < size.x) pt |= (E(b).r > 0.5 ? 2 : 0) | (E(b - int2(0, 1)).r > 0.5 ? 8 : 0);\n"
            "    w.rg = Ortho(pt, l, r);\n"
            "  }\n"
            "  if (e.r > 0.5) {\n"
            "    int t = 0, u = 0;\n"
            "    [loop] for (; t < MaxD; ++t) { int2 q = p - int2(0, t);\n"
            "      if (q.y == 0 || E(q).g > 0.5 || E(q - int2(1, 0)).g > 0.5 || E(q - int2(0, 1)).r < 0.5) break; }\n"
            "    [loop] for (; u < MaxD; ++u) { int2 q = p + int2(0, u + 1);\n"
            "      if (q.y >= size.y || E(q).g > 0.5 || E(q - int2(1, 0)).g > 0.5 || E(q).r < 0.5) break; }\n"
            "    int2 a = p - int2(0, t), b = p + int2(0, u + 1);\n"
            "    int pt = (E(a).g > 0.5 ? 1 : 0) | (E(a - int2(1, 0)).g > 0.5 ? 4 : 0);\n"
            "    if (b.y < size.y) pt |= (E(b).g > 0.5 ? 2 : 0) | (E(b - int2(1, 0)).g > 0.5 ? 8 : 0);\n"
            "    w.ba = Ortho(pt, t, u);\n"
            "  }\n"
            "  return w; }\n",
            "ttt2 present smaa weights ps");
        // The weights (t1) applied: the stronger pair of sides, one bilinear tap each.
        smaaBlendPs_ = PresentShader(
            "float4 W(int2 p) { return lut.Load(int3(p, 0)); }\n"
            "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
            "  int2 p = int2(uv * dstSize); float2 rf = 1.0 / srcSize, tc = (float2(p) + 0.5) * rf;\n"
            "  float4 m = W(p), a = float4(W(p + int2(1, 0)).a, W(p + int2(0, 1)).g, m.b, m.r);\n"
            "  if (dot(a, 1.0) < 1e-5) return float4(src.Load(int3(p, 0)).rgb, 1);\n"
            "  bool h = max(a.x, a.z) > max(a.y, a.w);\n"
            "  float4 o = h ? float4(a.x, 0, a.z, 0) : float4(0, a.y, 0, a.w);\n"
            "  float2 wt = h ? a.xz : a.yw; wt /= dot(wt, 1.0);\n"
            "  float4 c = tc.xyxy + o * float4(rf, -rf);\n"
            "  return float4(wt.x * src.SampleLevel(lin, c.xy, 0).rgb + wt.y * src.SampleLevel(lin, c.zw, 0).rgb, 1); }\n",
            "ttt2 present smaa blend ps");
        // Scale: mode 0 texel for texel, 1 Catmull-Rom (9 bilinear taps), 2 area filter.
        scalePs_ = PresentShader(
            "float4 main(float4 pos : SV_Position) : SV_Target {\n"
            "  float2 p = pos.xy - origin;\n"
            "  if (mode == 0) return float4(src.Load(int3(int2(p), 0)).rgb, 1);\n"
            "  float2 s = p / dstSize * srcSize;\n"
            "  if (mode == 1) {\n"
            "    float2 t1 = floor(s - 0.5) + 0.5, f = s - t1;\n"
            "    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
            "    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
            "    float2 w12 = w1 + w2, t12 = (t1 + w2 / w12) / srcSize, t0 = (t1 - 1.0) / srcSize, t3 = (t1 + 2.0) / srcSize;\n"
            "    float3 r = src.SampleLevel(lin, float2(t0.x, t0.y), 0).rgb * w0.x * w0.y\n"
            "             + src.SampleLevel(lin, float2(t12.x, t0.y), 0).rgb * w12.x * w0.y\n"
            "             + src.SampleLevel(lin, float2(t3.x, t0.y), 0).rgb * w3.x * w0.y\n"
            "             + src.SampleLevel(lin, float2(t0.x, t12.y), 0).rgb * w0.x * w12.y\n"
            "             + src.SampleLevel(lin, float2(t12.x, t12.y), 0).rgb * w12.x * w12.y\n"
            "             + src.SampleLevel(lin, float2(t3.x, t12.y), 0).rgb * w3.x * w12.y\n"
            "             + src.SampleLevel(lin, float2(t0.x, t3.y), 0).rgb * w0.x * w3.y\n"
            "             + src.SampleLevel(lin, float2(t12.x, t3.y), 0).rgb * w12.x * w3.y\n"
            "             + src.SampleLevel(lin, float2(t3.x, t3.y), 0).rgb * w3.x * w3.y;\n"
            "    return float4(saturate(r), 1);\n"
            "  }\n"
            "  float2 ratio = srcSize / dstSize;\n"
            "  float2 a = (floor(p) + 0.0) * ratio, b = a + ratio;   // the pixel's footprint, source texels\n"
            "  int2 i0 = int2(floor(a)), i1 = min(int2(ceil(b)), int2(srcSize)) - 1;\n"
            "  float3 sum = 0; float wsum = 0;\n"
            "  [loop] for (int y = i0.y; y <= i1.y && y < i0.y + 12; ++y) {\n"
            "    float wy = min(b.y, y + 1.0) - max(a.y, (float)y);\n"
            "    [loop] for (int x = i0.x; x <= i1.x && x < i0.x + 12; ++x) {\n"
            "      float w = wy * (min(b.x, x + 1.0) - max(a.x, (float)x));\n"
            "      sum += src.Load(int3(x, y, 0)).rgb * w; wsum += w;\n"
            "    }\n"
            "  }\n"
            "  return float4(sum / max(wsum, 1e-6), 1); }\n",
            "ttt2 present scale ps");
        // Contrast adaptive sharpening (AMD FidelityFX CAS, no scaling): sharp 0..1.
        casPs_ = PresentShader(
            "float3 T(int2 p) { return src.Load(int3(clamp(p, int2(0, 0), int2(dstSize) - 1), 0)).rgb; }\n"
            "float4 main(float4 pos : SV_Position) : SV_Target {\n"
            "  int2 p = int2(pos.xy - origin);\n"
            "  float3 a = T(p + int2(-1, -1)), b = T(p + int2(0, -1)), c = T(p + int2(1, -1));\n"
            "  float3 d = T(p + int2(-1, 0)), e = T(p), f = T(p + int2(1, 0));\n"
            "  float3 g = T(p + int2(-1, 1)), h = T(p + int2(0, 1)), i = T(p + int2(1, 1));\n"
            "  float3 mn = min(min(min(d, e), min(f, b)), h);\n"
            "  float3 mn2 = min(mn, min(min(a, c), min(g, i)));\n"
            "  mn += mn2;\n"
            "  float3 mx = max(max(max(d, e), max(f, b)), h);\n"
            "  float3 mx2 = max(mx, max(max(a, c), max(g, i)));\n"
            "  mx += mx2;\n"
            "  float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));\n"
            "  float3 w = amp * (-1.0 / lerp(8.0, 5.0, sharp));\n"
            "  float3 o = saturate((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w));\n"
            "  return float4(o, 1); }\n",
            "ttt2 present cas ps");
    }
    void PostPass(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv,
                  const PresentCB& cb, ID3D11ShaderResourceView* second = nullptr)
    {
        context_->UpdateSubresource(presentCb_.Get(), 0, nullptr, &cb, 0, 0);
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->RSSetViewports(1, &vp);
        const D3D11_RECT sc{ LONG(vp.TopLeftX), LONG(vp.TopLeftY), LONG(vp.TopLeftX + vp.Width), LONG(vp.TopLeftY + vp.Height) };
        context_->RSSetScissorRects(1, &sc);
        context_->PSSetShader(ps, nullptr, 0);
        ID3D11ShaderResourceView* v[2] = { srv, second ? second : gammaSrv_.Get() };   // t1: the gamma ramp, or SMAA's weights
        context_->PSSetShaderResources(0, 2, v);
        ID3D11Buffer* b = presentCb_.Get();
        context_->PSSetConstantBuffers(0, 1, &b);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = { nullptr, nullptr };
        context_->PSSetShaderResources(0, 2, none);
    }
    // TEST ONLY (p_diag68): TTT2_DIAG_GPU_LOAD=<rounds> - a busy pixel shader before every present (<rounds> loop rounds a
    // pixel over a 256x256 target nothing reads), standing in for a slower graphics card (frame latency tests).
    ComPtr<ID3D11PixelShader> loadPs_;
    ComPtr<ID3D11RenderTargetView> loadRtv_;
    ComPtr<ID3D11Buffer> loadCb_;
    void DiagGpuLoad()
    {
        static const uint32_t rounds = [] { const char* e = std::getenv("TTT2_DIAG_GPU_LOAD"); return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 0u; }();
        if (!rounds) return;
        if (!loadPs_)
        {
            auto ps = Compile("cbuffer C : register(b0) { uint rounds; uint3 pad; };"
                              " float4 main(float4 pos : SV_Position) : SV_Target { float x = pos.x * 0.001 + pos.y * 0.0007;"
                              " [loop] for (uint i = 0; i < rounds; ++i) x = frac(x * 1.618034 + 0.31);"
                              " return float4(x, 0, 0, 1); }",
                              "ps_5_0", "ttt2 diag gpu load ps");
            Check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &loadPs_), "CreateLoadPs");
            D3D11_TEXTURE2D_DESC td{};
            td.Width = td.Height = 256; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> tex;
            Check(device_->CreateTexture2D(&td, nullptr, &tex), "CreateLoadTarget");
            Check(device_->CreateRenderTargetView(tex.Get(), nullptr, &loadRtv_), "CreateLoadRtv");
            const uint32_t cb[4] = { rounds, 0, 0, 0 };
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(cb); bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA init{ cb, 0, 0 };
            Check(device_->CreateBuffer(&bd, &init, &loadCb_), "CreateLoadCb");
            std::fprintf(stderr, "[gpu-hw] TEST ONLY: GPU load of %u rounds a pixel before every present\n", rounds);
        }
        pipe_.valid = false;
        PresentShaders();
        const D3D11_VIEWPORT vp{ 0, 0, 256, 256, 0, 1 };
        const D3D11_RECT all{ 0, 0, 256, 256 };            // raster_ scissors: PresentScaled left the window's 16:9 area set
        context_->OMSetRenderTargets(1, loadRtv_.GetAddressOf(), nullptr);
        context_->RSSetViewports(1, &vp);
        context_->RSSetScissorRects(1, &all);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(nullptr, 0);
        context_->RSSetState(raster_.Get());
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(presentVs_.Get(), nullptr, 0);
        context_->PSSetShader(loadPs_.Get(), nullptr, 0);
        ID3D11Buffer* b = loadCb_.Get();
        context_->PSSetConstantBuffers(0, 1, &b);
        context_->Draw(3, 0);
    }
    // ---- Options -> Display Settings -> FPS COUNTER: the frames shown a second, over half a second, in a panel of the
    // window's scale (gpu_fps_overlay.h) over the finished picture, so it counts what reaches the screen, in any menu or
    // fight, and never enters the game's own captures (pause, resolves).
    fps_overlay::Panel fpsPanel_;
    ComPtr<ID3D11Texture2D> fpsTex_;
    ComPtr<ID3D11ShaderResourceView> fpsSrv_;
    ComPtr<ID3D11PixelShader> fpsPs_;
    ComPtr<ID3D11BlendState> fpsBlend_;
    std::chrono::steady_clock::time_point fpsSince_{};
    uint32_t fpsFrames_ = 0, fpsTexW_ = 0, fpsTexH_ = 0;
    int fpsValue_ = 0;
    const void* fpsUploaded_ = nullptr;
    int fpsUploadedValue_ = -1;
    D3D11_VIEWPORT pictureArea_{};
    void DrawFpsCounter()
    {
        const auto now = std::chrono::steady_clock::now();
        ++fpsFrames_;
        if (fpsSince_ == std::chrono::steady_clock::time_point{}) fpsSince_ = now, fpsFrames_ = 0;
        const double held = std::chrono::duration<double>(now - fpsSince_).count();
        if (held >= 0.5)
        {
            fpsValue_ = int(std::lround(double(fpsFrames_) / held));
            fpsFrames_ = 0;
            fpsSince_ = now;
        }
        const int corner = display_settings::GetFpsCounter();
        if (corner == display_settings::FpsOff) return;
        if (!fpsPs_)
        {
            fpsPs_ = PresentShader("float4 main(float4 pos : SV_Position) : SV_Target { return src.Load(int3(int2(pos.xy - origin), 0)); }\n",
                                   "ttt2 present fps ps");
            D3D11_BLEND_DESC bd{};                         // premultiplied alpha over the picture
            bd.RenderTarget[0].BlendEnable = TRUE;
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            Check(device_->CreateBlendState(&bd, &fpsBlend_), "CreateFpsBlend");
        }
        const fps_overlay::Bitmap& image = fpsPanel_.Draw(fpsValue_, UINT(pictureArea_.Height));
        if (!fpsTex_ || fpsTexW_ != image.w || fpsTexH_ != image.h)
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = image.w;
            td.Height = image.h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            fpsTex_.Reset();
            fpsSrv_.Reset();
            Check(device_->CreateTexture2D(&td, nullptr, &fpsTex_), "CreateFpsTexture");
            Check(device_->CreateShaderResourceView(fpsTex_.Get(), nullptr, &fpsSrv_), "CreateFpsSrv");
            fpsTexW_ = image.w;
            fpsTexH_ = image.h;
            fpsUploadedValue_ = -1;
        }
        if (fpsUploadedValue_ != fpsValue_)
        {
            context_->UpdateSubresource(fpsTex_.Get(), 0, nullptr, image.px.data(), image.w * 4, 0);
            fpsUploadedValue_ = fpsValue_;
        }
        const float margin = std::round(12.0f * fps_overlay::Panel::Scale(pictureArea_.Height));
        const bool left = corner == display_settings::FpsTopLeft || corner == display_settings::FpsBottomLeft;
        const bool top = corner == display_settings::FpsTopLeft || corner == display_settings::FpsTopRight;
        const float x = left ? pictureArea_.TopLeftX + margin : pictureArea_.TopLeftX + pictureArea_.Width - margin - float(image.w);
        const float y = top ? pictureArea_.TopLeftY + margin : pictureArea_.TopLeftY + pictureArea_.Height - margin - float(image.h);
        const D3D11_VIEWPORT vp{ x, y, float(image.w), float(image.h), 0, 1 };
        PresentCB cb{};
        cb.srcW = cb.dstW = float(image.w);
        cb.srcH = cb.dstH = float(image.h);
        cb.originX = x;
        cb.originY = y;
        context_->OMSetBlendState(fpsBlend_.Get(), nullptr, 0xFFFFFFFF);
        PostPass(backRtv_.Get(), vp, fpsPs_.Get(), fpsSrv_.Get(), cb);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    }
    void PresentScaled(ID3D11Texture2D* source, ID3D11Texture2D* back, bool swap)
    {
        PresentChain(source, back, swap);
        DrawFpsCounter();
    }
    void PresentChain(ID3D11Texture2D* source, ID3D11Texture2D* back, bool swap)
    {
        pipe_.valid = false;
        PresentShaders();
        if (backRtvFor_ != back)
        {
            backRtv_.Reset();
            Check(device_->CreateRenderTargetView(back, nullptr, &backRtv_), "CreateBackRtv");
            backRtvFor_ = back;
        }
        D3D11_TEXTURE2D_DESC sd{};
        source->GetDesc(&sd);
        // The window's 16:9 area, whole pixels (texel for texel when it matches the render size).
        const float scale = std::min(float(bufW_) / 1280.0f, float(bufH_) / 720.0f);
        const UINT vw = std::max(1u, UINT(std::lround(1280.0f * scale))), vh = std::max(1u, UINT(std::lround(720.0f * scale)));
        const UINT vx = (bufW_ - std::min(vw, bufW_)) / 2, vy = (bufH_ - std::min(vh, bufH_)) / 2;
        const D3D11_VIEWPORT out{ float(vx), float(vy), float(vw), float(vh), 0, 1 };
        pictureArea_ = out;
        const float black[4] = { 0, 0, 0, 1 };
        ID3D11RenderTargetView* rtv = backRtv_.Get();
        context_->ClearRenderTargetView(rtv, black);
        ComPtr<ID3D11ShaderResourceView> srv;
        Check(device_->CreateShaderResourceView(source, nullptr, &srv), "CreatePresentSrv");
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(nullptr, 0);
        context_->RSSetState(raster_.Get());
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(presentVs_.Get(), nullptr, 0);
        ID3D11SamplerState* s = presentSampler_.Get();
        context_->PSSetSamplers(0, 1, &s);
        ID3D11PixelShader* display = displayPs_[(swap ? 1 : 0) | (gammaApplied_ ? 2 : 0)].Get();
        const UINT sw = sd.Width, sh = sd.Height;
        const int sharpness = display_settings::GetSharpness();
        const bool aa = display_settings::GetAntiAliasing() == 1;      // SMAA (SSAA is the render scale)
        const bool resample = sw != vw || sh != vh;
        PresentCB cb{ float(sw), float(sh), float(vw), float(vh), float(vx), float(vy), 0.0f, 0 };
        if (!resample && !aa && !sharpness) { PostPass(rtv, out, display, srv.Get(), cb); return; }
        const D3D11_VIEWPORT full{ 0, 0, float(sw), float(sh), 0, 1 };
        PresentCB at0{ float(sw), float(sh), float(sw), float(sh), 0, 0, 0.0f, 0 };
        PostTarget& p0 = Post(0, sw, sh);
        PostPass(p0.rtv.Get(), full, display, srv.Get(), at0);
        ID3D11ShaderResourceView* cur = p0.srv.Get();
        if (aa)
        {
            PostTarget& edges = Post(3, sw, sh);
            PostPass(edges.rtv.Get(), full, smaaEdgesPs_.Get(), cur, at0);
            PostTarget& weights = Post(4, sw, sh);
            PostPass(weights.rtv.Get(), full, smaaWeightsPs_.Get(), edges.srv.Get(), at0);
            PostTarget& p1 = Post(1, sw, sh);
            PostPass(p1.rtv.Get(), full, smaaBlendPs_.Get(), cur, at0, weights.srv.Get());
            cur = p1.srv.Get();
        }
        cb.mode = !resample ? 0u : vw > sw ? 1u : 2u;
        if (sharpness)
        {
            if (resample)
            {
                PostTarget& p2 = Post(2, vw, vh);
                const D3D11_VIEWPORT area{ 0, 0, float(vw), float(vh), 0, 1 };
                PresentCB toArea = cb; toArea.originX = toArea.originY = 0;
                PostPass(p2.rtv.Get(), area, scalePs_.Get(), cur, toArea);
                cur = p2.srv.Get();
            }
            PresentCB sharp{ float(vw), float(vh), float(vw), float(vh), float(vx), float(vy), float(sharpness - 1) / 9.0f, 0 };
            PostPass(rtv, out, casPs_.Get(), cur, sharp);
        }
        else PostPass(rtv, out, scalePs_.Get(), cur, cb);
    }
public:
    bool IsExclusiveFullscreen() const { return exclusive_.load(); }

private:
    static constexpr UINT VertexRingBytes = 64u << 20, IndexRingBytes = 16u << 20, VdataRingBytes = 64u << 20;
    static constexpr UINT CbRingBytes = 32u << 20;      // p_diag44: the constant ring (~5.5 MB a heavy frame)

    // Pass-through vertex shaders by interpolator count: the CPU already produced clip-space
    // vertices; the pixel shader always receives 16 interpolators (the unsent ones are zero,
    // exactly what the interpreter sees for interpolators a vertex shader never exported).
    struct PassthroughVs { ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11InputLayout> layout; };
    std::array<PassthroughVs, 17> passthrough_;
    PassthroughVs& Passthrough(uint32_t n)
    {
        PassthroughVs& p = passthrough_[n];
        if (p.vs) return p;
        std::string src = "struct V { float4 p : POSITION;";
        if (n) src += " float4 a[" + std::to_string(n) + "] : TEXCOORD0;";
        src += " };\nstruct O { float4 p : SV_Position; float4 a[16] : TEXCOORD0; };\n"
               "O main(V v) { O o; o.p = v.p; [unroll] for (int i = 0; i < 16; ++i) o.a[i] = float4(0, 0, 0, 0);\n";
        if (n) src += "  [unroll] for (int k = 0; k < " + std::to_string(n) + "; ++k) o.a[k] = v.a[k];\n";
        src += "  return o; }\n";
        auto code = Compile(src, "vs_5_0", "ttt2 passthrough vs");
        Check(device_->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &p.vs), "CreateVertexShader");
        D3D11_INPUT_ELEMENT_DESC elements[17]{};
        elements[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
        for (UINT i = 0; i < n; ++i)
            elements[i + 1] = { "TEXCOORD", i, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16 + i * 16, D3D11_INPUT_PER_VERTEX_DATA, 0 };
        Check(device_->CreateInputLayout(elements, n + 1, code->GetBufferPointer(), code->GetBufferSize(), &p.layout), "CreateInputLayout");
        return p;
    }

    // extW / extH: the logical area held, in samples (the texture is `scale` times that: the
    // render scale, or 1 for a surface rendered at native size - see NativeSize).
    struct ColorSurface { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11RenderTargetView> rtv; ComPtr<ID3D11ShaderResourceView> srv;
                          bool touched = false; uint32_t samplePitch = 0; DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
                          UINT extW = 0, extH = 0; float scale = 1.0f; };
    struct DepthSurface { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11DepthStencilView> dsv; ComPtr<ID3D11ShaderResourceView> srv;
                          bool touched = false; uint32_t sampX = 1, sampY = 1; UINT extW = 0, extH = 0; float scale = 1.0f; };
    struct Resolved
    {
        ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11RenderTargetView> rtv;
        uint32_t pitch = 0, height = 0, copiedWidth = 0, size = 0, seq = 0;   // logical (guest) pixels
        bool valid = false, cpuCoherent = false, swapRB = false, depth = false, depthFloat = false;
        bool rg16f = false;          // 16_16_FLOAT destination (R16G16_FLOAT texture)
        float scale = 1.0f;          // render scale the texture was made at (its source surface's)
        uint32_t sourceKey = ~0u, sourceDepth = ~0u;   // colour surface key / depth base it came from
        uint32_t sourcePitch = 0;                      // that surface's sample pitch at the resolve (UpdateNative)
        // CPU readback (WriteBack). Once the CPU has read this destination, each later resolve
        // starts its copy to `staging` at once (StageReadback), so the read finds it done instead
        // of waiting for the GPU: the fight intro reads its 1024x1024 bake ~100 times, 9.4 ms each
        // (MEASURED 2026-10-14, every ~4th frame late).
        bool readbackHot = false, staged = false;
        uint32_t stagedSeq = 0;
        std::chrono::steady_clock::time_point stagedAt{};   // when the staged copy was queued
        uint64_t stagedFence = 0;                            // readbackFence_ value after that copy
        ComPtr<ID3D11Texture2D> logical;          // scaled: the logical-size copy the CPU reads
        ComPtr<ID3D11RenderTargetView> logicalRtv;
        ComPtr<ID3D11Texture2D> staging;
    };
    struct TexKey
    {
        uint32_t base, format, width, height, pitch, tiled, endian, swizzle, slices;   // slices: count | 0x100 if array
        uint32_t mipBase, levels;                                                      // levels | 0x100 if packed
        bool operator==(const TexKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
    };
    struct TexKeyHash { size_t operator()(const TexKey& k) const { return (size_t)Fnv(&k, sizeof(k)); } };
    struct CachedTexture
    {
        ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv;
        uint32_t mode = 0, rangeStart = 0, rangeSize = 0, seq = 0, allocW = 1, allocH = 1, slices = 1;
        uint32_t levels = 1, mipStart = 0, mipSize = 0;     // mip levels held; their guest bytes (levels 1+)
        bool genericSwizzled = false;
    };
    // p107: memoKind - 0: from the fetch constant alone (fallback), 1: guest texture bytes (the ranges), 2: resolved data.
    struct Bound { ID3D11ShaderResourceView* srv; uint32_t swizzle, mode, width, height; float scaleX, scaleY; uint32_t slices = 1, levels = 1;
                   uint8_t memoKind = 2; uint32_t rangeBase = 0, rangeBytes = 0, mipStart = 0, mipSize = 0; };
    struct Stats { uint64_t draws = 0, triangles = 0, skippedDraws = 0, emptyDraws = 0, resolves = 0, presents = 0,
                   compiles = 0, diskHits = 0, uploads = 0, uploadBytes = 0, resolvedHits = 0, writebacks = 0,
                   depthResolves = 0, depthHits = 0, gpuVsDraws = 0, vertexDataBytes = 0, warmHits = 0, pendingSkips = 0, stagedReadbacks = 0, ringWaits = 0, resolveCreates = 0, resolvePoolHits = 0, shadowSkips = 0; double ringWaitMs = 0; };
    // ---- SHADOWS OFF (display_settings::GetShadows): the game's cast shadows come from square depth maps - about
    // 50 depth-only draws per frame (colour writes off, depth written, compare GEQUAL; cleared by draws with compare
    // ALWAYS) into an EDRAM depth surface, resolved as a 512 / 1024 / 2048 square depth texture, turned into an
    // 8_8_8_8 shadow texture the scene samples (MEASURED 2026-10-22 in frame traces; every other depth resolve -
    // depth of field, soft particles - is screen-shaped). The square resolve names the surface. With SHADOWS OFF
    // its caster draws are skipped (the GPU saves the pass) and its resolve writes 0.0 everywhere: no occluder
    // (MEASURED, same stage and pose: a map left with only its clear came out as shadow over most of the floor,
    // 1.0 shadowed all of it, 0.0 lit all of it - fighters unchanged). Drawing only - the game never reads it back.
    struct ShadowSurface { uint32_t base = 0, size = 0; uint64_t seen = 0; };
    ShadowSurface shadowSurfaces_[4];
    void NoteShadowResolve(uint32_t depthBase, uint32_t pitch, uint32_t height)
    {
        if (pitch != height || (pitch != 512 && pitch != 1024 && pitch != 2048)) return;
        ShadowSurface* slot = nullptr;
        for (ShadowSurface& e : shadowSurfaces_)
            if (e.size && e.base == depthBase && e.size == pitch) { slot = &e; break; }
        if (!slot)
        {
            slot = &shadowSurfaces_[0];
            for (ShadowSurface& e : shadowSurfaces_) if (e.seen < slot->seen) slot = &e;   // the oldest (or a free one)
            if (!slot->size || slot->base != depthBase)
                std::fprintf(stderr, "[gpu-hw] shadow map: depth surface %03X, %ux%u\n", depthBase, pitch, pitch);
            slot->base = depthBase;
            slot->size = pitch;
        }
        slot->seen = stats_.presents + 1;
    }
    // A caster: a depth-only draw that writes depth with a real compare into a shadow map's surface (its surface
    // pitch is the map's size or a little more), seen resolved in the last 10 s.
    bool ShadowMapOff(uint32_t depthBase, uint32_t pitch, uint32_t height) const
    {
        if (display_settings::GetShadows() || pitch != height) return false;
        for (const ShadowSurface& e : shadowSurfaces_)
            if (e.size == pitch && e.base == depthBase) return true;
        return false;
    }
    bool ShadowCaster(const DrawState& st) const
    {
        if (st.writeMask || !st.zEnable || !st.zWrite || st.zFunc == 7 || display_settings::GetShadows()) return false;
        for (const ShadowSurface& e : shadowSurfaces_)
            if (e.size && e.base == st.depthBase && st.pitch >= e.size && st.pitch <= e.size + 64 && stats_.presents < e.seen + 600)
                return true;
        return false;
    }
public:
    // Device-removed diagnostics for the failure log (DXGI reason code).
    HRESULT RemovedReason() const { return device_ ? device_->GetDeviceRemovedReason() : S_OK; }
private:
    // Per-format upload accounting (format -> {uploads, bytes}) and writeback causes, reported
    // with the periodic stats so the next fast path is chosen from measured data.
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> uploadByFormat_;
    std::map<std::string, uint64_t> writebackCauses_;
    std::map<uint32_t, uint64_t> undecodable_;         // texture format -> fetch binds

    // Format census (TTT2_FORMAT_CENSUS=1): the first use of every texture format / swizzle /
    // endian / decode path, colour-target format and vertex-fetch format is logged as
    // "[census] ...", so each one can be checked against a hardware-verified component order
    // (the 8_8_8_8 red/blue mix-up of 2026-10-12 hid in formats nobody had listed).
    static bool CensusOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_FORMAT_CENSUS"); return e && *e == '1'; }();
        return on;
    }
    enum CensusPath : uint32_t { CensusResolved, CensusDepth, CensusUndecodable, CensusUpload, CensusResolveBytes };
    std::map<uint64_t, uint64_t> censusSeen_;
    void CensusTexture(const Texture& t, uint32_t path)
    {
        static const char* names[] = { "gpu-resolve", "depth-resolve", "undecodable", "upload", "upload-of-resolve-bytes" };
        const uint64_t key = uint64_t(t.format) | uint64_t(t.swizzle) << 8 | uint64_t(t.endian) << 20 |
                             uint64_t(t.tiled ? 1 : 0) << 22 | uint64_t(t.dimension) << 23 | uint64_t(path) << 26 |
                             uint64_t(t.signs) << 32;
        if (censusSeen_[key]++) return;
        std::fprintf(stderr, "[census] texture fmt=%u swz=%03X endian=%u %s dim=%u signs=%02X%s path=%s (first: base=%08X %ux%u)\n",
                     t.format, t.swizzle, t.endian, t.tiled ? "tiled" : "linear", t.dimension, t.signs,
                     (t.signs & 0x3F) == 0x3F ? " GAMMA" : "", names[path], t.base, t.width, t.height);
    }
    // TTT2_MIP_CENSUS=1 (diagnostics): per distinct uploaded texture its mip / filter fields, a running
    // summary, and for the first textures with mips each level decoded where TextureMipLevel says
    // (variants 0, 1, 2, 4) against the 2x2 box of the level above (mean absolute difference, 0..1),
    // with the same against the box shifted half a level away (what unrelated texels give).
    static bool MipCensusOn() { static const bool on = [] { const char* e = std::getenv("TTT2_MIP_CENSUS"); return e && *e == '1'; }(); return on; }
    std::set<uint64_t> mipSeen_;
    uint32_t mipChecked_ = 0;
    uint64_t mipCount_[8] = {}, mipFilterHist_[4] = {}, minFilterHist_[4] = {}, magFilterHist_[4] = {}, anisoHist_[8] = {};
    static std::vector<V4> MipDecode(const Texture& t, const MipLevel& m)
    {
        Texture lt = t;
        lt.base = m.base; lt.pitch = m.pitch; lt.width = m.x + m.width; lt.height = m.y + m.height;
        lt.clampX = lt.clampY = 2;
        std::vector<V4> out(size_t(m.width) * m.height);
        for (uint32_t y = 0; y < m.height; ++y)
            for (uint32_t x = 0; x < m.width; ++x) out[size_t(y) * m.width + x] = SampleTexel(lt, int(m.x + x), int(m.y + y));
        return out;
    }
    static double MipError(const std::vector<V4>& up, uint32_t uw, uint32_t uh, const std::vector<V4>& lo, uint32_t lw, uint32_t lh, bool shifted)
    {
        double sum = 0;
        for (uint32_t y = 0; y < lh; ++y)
            for (uint32_t x = 0; x < lw; ++x)
            {
                const uint32_t sx = shifted ? (x + lw / 2) % lw : x, sy = shifted ? (y + lh / 2) % lh : y;
                for (int c = 0; c < 4; ++c)
                {
                    float b = 0;
                    for (uint32_t k = 0; k < 4; ++k)
                    {
                        const uint32_t ux = std::min(2 * sx + (k & 1), uw - 1), uy = std::min(2 * sy + (k >> 1), uh - 1);
                        b += up[size_t(uy) * uw + ux][c];
                    }
                    sum += std::fabs(lo[size_t(y) * lw + x][c] - b * 0.25f);
                }
            }
        return sum / (double(lw) * lh * 4);
    }
    void MipCensus(const Texture& t)
    {
        const uint64_t key = uint64_t(t.base) << 32 ^ uint64_t(t.width) << 18 ^ uint64_t(t.height) << 4 ^ t.format;
        if (!mipSeen_.insert(key).second) return;
        ++mipCount_[0];
        if (t.mipMax > 0) ++mipCount_[1];
        if (t.mipBase) ++mipCount_[2];
        if (t.packedMips) ++mipCount_[3];
        if (t.mipMin > 0) ++mipCount_[4];
        if (t.lodBias) ++mipCount_[5];
        if (t.tiled) ++mipCount_[6];
        if (IsBlockCompressed(t.format)) ++mipCount_[7];
        ++mipFilterHist_[t.mipFilter]; ++minFilterHist_[t.minFilter]; ++magFilterHist_[t.magFilter & 3]; ++anisoHist_[t.aniso];
        const uint64_t n = mipCount_[0];
        if (n == 25 || n == 50 || n == 100 || n == 200 || n == 400 || n == 800 || n == 1600)
            std::fprintf(stderr, "[mip] %llu textures: %llu with levels past 0 (mip max), %llu with a mip address, %llu packed, %llu first level > 0, %llu LOD bias, "
                         "%llu tiled, %llu block-compressed; mip filter point/linear/base/fc %llu/%llu/%llu/%llu, min %llu/%llu/%llu/%llu, mag %llu/%llu/%llu/%llu, "
                         "aniso 0..7 %llu %llu %llu %llu %llu %llu %llu %llu\n",
                         (unsigned long long)n, (unsigned long long)mipCount_[1], (unsigned long long)mipCount_[2], (unsigned long long)mipCount_[3],
                         (unsigned long long)mipCount_[4], (unsigned long long)mipCount_[5], (unsigned long long)mipCount_[6], (unsigned long long)mipCount_[7],
                         (unsigned long long)mipFilterHist_[0], (unsigned long long)mipFilterHist_[1], (unsigned long long)mipFilterHist_[2], (unsigned long long)mipFilterHist_[3],
                         (unsigned long long)minFilterHist_[0], (unsigned long long)minFilterHist_[1], (unsigned long long)minFilterHist_[2], (unsigned long long)minFilterHist_[3],
                         (unsigned long long)magFilterHist_[0], (unsigned long long)magFilterHist_[1], (unsigned long long)magFilterHist_[2], (unsigned long long)magFilterHist_[3],
                         (unsigned long long)anisoHist_[0], (unsigned long long)anisoHist_[1], (unsigned long long)anisoHist_[2], (unsigned long long)anisoHist_[3],
                         (unsigned long long)anisoHist_[4], (unsigned long long)anisoHist_[5], (unsigned long long)anisoHist_[6], (unsigned long long)anisoHist_[7]);
        if (t.mipMax == 0 || mipChecked_ >= 60 || t.width * t.height > 1024 * 1024 || (!FormatBpp(t.format) && !IsBlockCompressed(t.format))) return;
        ++mipChecked_;
        const uint32_t levels = std::min<uint32_t>(t.mipMax, Log2Ceil(std::max(t.width, t.height)));
        std::string line;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "[mip] check fmt=%u %ux%u p%u %s%s levels %u..%u base %08X mips %08X min/mip/aniso %u/%u/%u bias %d:",
                      t.format, t.width, t.height, t.pitch, t.tiled ? "tiled" : "linear", t.packedMips ? " packed" : "",
                      t.mipMin, t.mipMax, t.base, t.mipBase, t.minFilter, t.mipFilter, t.aniso, t.lodBias);
        line = buf;
        static const uint32_t variants[] = { 0, 1, 2, 4 };
        for (int vi = 0; vi < 5; ++vi)
        {
            const bool baseline = vi == 4;
            const uint32_t variant = baseline ? 0 : variants[vi];
            std::snprintf(buf, sizeof(buf), baseline ? " | shifted" : " | v%u", variant);
            line += buf;
            MipLevel m0;
            TextureMipLevel(t, 0, m0, variant);
            std::vector<V4> up = MipDecode(t, m0);
            uint32_t uw = m0.width, uh = m0.height;
            for (uint32_t l = 1; l <= levels; ++l)
            {
                MipLevel m;
                if (!TextureMipLevel(t, l, m, variant)) { line += " -"; break; }
                std::vector<V4> lo = MipDecode(t, m);
                std::snprintf(buf, sizeof(buf), " %.3f", MipError(up, uw, uh, lo, m.width, m.height, baseline));
                line += buf;
                up.swap(lo); uw = m.width; uh = m.height;
            }
        }
        std::fprintf(stderr, "%s\n", line.c_str());
        // Where are the levels past the 4x4 one really? Search the packed level's first tile.
        const uint32_t ls = Log2Ceil(std::min(t.width, t.height));
        const uint32_t packedLevel = ls > 4 ? ls - 4 : 0;
        if (!t.packedMips || !IsBlockCompressed(t.format) || levels < packedLevel + 3 || mipSearched_ >= 12) return;
        ++mipSearched_;
        MipLevel pm;
        TextureMipLevel(t, packedLevel, pm);
        if (dumpedTile_.insert(t.format).second)
        {
            // The packed tile: 32x32 blocks (128x128 texels) from the packed level's storage, 4x enlarged.
            Texture lt = t;
            lt.base = pm.base; lt.pitch = pm.pitch; lt.width = 128; lt.height = 128; lt.clampX = lt.clampY = 2; lt.swizzle = 0x688;
            std::vector<uint32_t> px(512 * 512);
            for (uint32_t y = 0; y < 512; ++y)
                for (uint32_t x = 0; x < 512; ++x)
                {
                    const V4 v = SampleTexel(lt, int(x / 4), int(y / 4));
                    auto b = [](float f) { return uint32_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
                    px[size_t(y) * 512 + x] = (b(v[0]) << 16) | (b(v[1]) << 8) | b(v[2]);
                }
            char path[96];
            std::snprintf(path, sizeof(path), "C:/ttt2/port/miptile_%u.bmp", t.format);
            WriteBmp(path, px.data(), 512, 512);
            std::fprintf(stderr, "[mip] packed tile of fmt=%u %ux%u (level %u at %08X pitch %u) -> %s\n", t.format, t.width, t.height, packedLevel, pm.base, pm.pitch, path);
        }
        MipLevel um;
        TextureMipLevel(t, packedLevel + 2, um);
        std::vector<V4> up = MipDecode(t, um);
        for (uint32_t l = packedLevel + 3; l <= levels; ++l)
        {
            MipLevel m;
            TextureMipLevel(t, l, m);
            struct Hit { double e; uint32_t bx, by; };
            std::vector<Hit> hits;
            for (uint32_t by = 0; by < 32; ++by)
                for (uint32_t bx = 0; bx < pm.pitch / 4; ++bx)
                {
                    MipLevel c = m;
                    c.base = pm.base; c.pitch = pm.pitch; c.x = bx * 4; c.y = by * 4;
                    hits.push_back({ MipError(up, um.width, um.height, MipDecode(t, c), m.width, m.height, false), bx, by });
                }
            std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.e < b.e; });
            std::string s;
            char b2[64];
            for (size_t i = 0; i < 5 && i < hits.size(); ++i) { std::snprintf(b2, sizeof(b2), " (%u,%u) %.3f", hits[i].bx, hits[i].by, hits[i].e); s += b2; }
            std::fprintf(stderr, "[mip] search fmt=%u %ux%u level %u (%ux%u): layout says block (%u,%u); best%s\n",
                         t.format, t.width, t.height, l, m.width, m.height, m.x / 4, m.y / 4, s.c_str());
            MipLevel best = m; best.base = pm.base; best.pitch = pm.pitch; best.x = hits[0].bx * 4; best.y = hits[0].by * 4;
            up = MipDecode(t, best); um = best;
        }
    }
    uint32_t mipSearched_ = 0;
    static bool MipVerifyOn() { static const bool on = [] { const char* e = std::getenv("TTT2_MIP_VERIFY"); return e && *e == '1'; }(); return on; }
    std::set<uint64_t> mipVerified_;
    uint32_t mipVerifyCount_ = 0, mipVerifyBad_ = 0;
    void MipVerify(const Texture& t, uint32_t levels)
    {
        const uint64_t key = uint64_t(t.base) << 32 ^ uint64_t(t.mipBase) ^ uint64_t(t.format) << 24 ^ uint64_t(t.width) << 12 ^ t.height;
        if (!mipVerified_.insert(key).second || t.width * t.height > 2048 * 2048) return;
        ++mipVerifyCount_;
        std::vector<std::vector<V4>> lv(levels);
        std::vector<MipLevel> ml(levels);
        for (uint32_t l = 0; l < levels; ++l) { TextureMipLevel(t, l, ml[l]); lv[l] = MipDecode(t, ml[l]); }
        bool bad = false;
        std::string errs;
        char b[48];
        for (uint32_t l = 1; l < levels; ++l)
        {
            const double e = MipError(lv[l - 1], ml[l - 1].width, ml[l - 1].height, lv[l], ml[l].width, ml[l].height, false);
            const double s = MipError(lv[l - 1], ml[l - 1].width, ml[l - 1].height, lv[l], ml[l].width, ml[l].height, true);
            std::snprintf(b, sizeof(b), " %.3f/%.3f", e, s);
            errs += b;
            if (e > 0.06 && e > 0.5 * s) bad = true;
        }
        if (bad) ++mipVerifyBad_;
        if (bad || mipVerifyCount_ % 50 == 0)
            std::fprintf(stderr, "[mip-verify] %s fmt=%u %ux%u p%u %s%s endian %u base %08X mips %08X levels %u (error/shifted per level:%s); %u checked, %u off\n",
                         bad ? "OFF" : "ok", t.format, t.width, t.height, t.pitch, t.tiled ? "tiled" : "linear", t.packedMips ? " packed" : "",
                         t.endian, t.base, t.mipBase, levels, errs.c_str(), mipVerifyCount_, mipVerifyBad_);
        if (!bad || mipVerifyBad_ > 40) return;
        // The chain side by side, every level enlarged (nearest) to the base size, alpha ignored.
        const uint32_t W = ml[0].width, H = ml[0].height;
        std::vector<uint32_t> px(size_t(W) * levels * H);
        for (uint32_t l = 0; l < levels; ++l)
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                {
                    const V4& v = lv[l][size_t(y * ml[l].height / H) * ml[l].width + x * ml[l].width / W];
                    auto q = [](float f) { return uint32_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
                    px[size_t(y) * W * levels + l * W + x] = (q(v[0]) << 16) | (q(v[1]) << 8) | q(v[2]);
                }
        CreateDirectoryA("C:/ttt2/port/mipv", nullptr);
        char path[96];
        std::snprintf(path, sizeof(path), "C:/ttt2/port/mipv/%08X_%u.bmp", t.base, t.format);
        WriteBmp(path, px.data(), W * levels, H);
    }
    std::set<uint32_t> dumpedTile_;
    void CensusTarget(uint32_t colorKey)
    {
        if (!CensusOn() || censusSeen_[(1ull << 40) | (colorKey >> 12)]++) return;
        std::fprintf(stderr, "[census] colour target fmt=%u (first: edram base %03X)\n", colorKey >> 12, colorKey & 0xFFF);
    }
    // Render-scale sizing data: the first draw of every target / depth / pitch / MSAA / scissor /
    // viewport combination (how far down each surface really is drawn).
    void CensusSurfaceUse(const uint32_t* regs, const DrawState& st, float reachY)
    {
        const float ys = AsFloat(regs[0x2111]), yo = AsFloat(regs[0x2112]);
        const uint32_t vte = regs[0x2206];
        const uint64_t key = (3ull << 60) ^ Fnv(&st.colorBase, 4) ^ (uint64_t(st.depthBase) << 20) ^ (uint64_t(st.pitch) << 32) ^
                             (uint64_t(st.sampX * 4 + st.sampY) << 46) ^ (uint64_t(st.scY1) << 48) ^ (uint64_t(st.zEnable) << 59) ^
                             (uint64_t(regs[0x2082]) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(int(yo + std::fabs(ys))) << 36);
        if (censusSeen_[key]++) return;
        std::fprintf(stderr, "[census] surface use colour=%05X%s depth=%03X%s pitch=%u samp=%ux%u scissor=(%d,%d)-(%d,%d) "
                     "win_tl=%08X win_br=%08X win_off=%08X screen_br=%08X vte=%03X vp_y=%.1f..%.1f clip_off=%u reach_y=%.0f\n",
                     st.colorBase, st.writeMask ? "" : "(masked)", st.depthBase, st.zEnable ? "" : "(off)", st.pitch, st.sampX, st.sampY,
                     st.scX0, st.scY0, st.scX1, st.scY1, regs[0x2081], regs[0x2082], regs[0x2080], regs[0x200F], vte,
                     yo - std::fabs(ys), yo + std::fabs(ys), (regs[0x2204] >> 16) & 1, std::isfinite(reachY) ? reachY : -1.0f);
    }
    // TTT2_VCACHE_STATS=1: per distinct uploaded range (address, bytes), whether it changed since its last
    // upload (write watch, synced once per frame per range); printed with the 5 s stats.
    static bool VcacheStatsOn() { static const bool on = [] { const char* e = std::getenv("TTT2_VCACHE_STATS"); return e && *e == '1'; }(); return on; }
    struct VcacheSeen { uint32_t seq = 0, frame = ~0u; };
    std::unordered_map<uint64_t, VcacheSeen> vcacheSeen_;
    uint64_t vcacheSame_ = 0, vcacheChanged_ = 0, vcacheNew_ = 0, vcacheSyncs_ = 0;
    void VcacheStat(uint32_t addr, uint32_t bytes)
    {
        const uint64_t key = uint64_t(addr) << 32 | bytes;
        VcacheSeen& s = vcacheSeen_[key];
        const uint32_t frame = uint32_t(stats_.presents);
        if (s.frame != frame) { dirty_.Sync(addr, bytes); ++vcacheSyncs_; }
        const uint32_t newest = dirty_.Newest(addr, bytes);
        if (s.frame == ~0u) vcacheNew_ += bytes;
        else if (newest <= s.seq) vcacheSame_ += bytes;
        else vcacheChanged_ += bytes;
        s.seq = dirty_.Current();
        s.frame = frame;
    }
    // p73: the vertex data cache. MEASURED (TTT2_VCACHE_STATS, fights): ~45% of the vertex bytes copied into the ring a
    // frame had not changed since their last copy - stage geometry, meshes drawn again in a frame; the low-end player's PC
    // spent 3.4 ms of a 13 ms command-processor frame copying on Tulip Festival (log 24). A range (address, dwords)
    // copied before is drawn from where it already is in the ring while (1) the write watch saw no write to its pages
    // since the copy, (2) the ring has not come round to it (RingFence::gen), and (3) it is not in the two chunks the
    // ring enters next: each chunk read again is marked again whenever the ring enters a chunk, so the ring reaches a
    // chunk only after the GPU has passed those reads. Pages written in the last two frames (skinned vertices,
    // particles) are not synced for it for two seconds - a sync resets their write watch, and each page's next write
    // then faults on the game's thread - and are copied as before. A copy made from the frame overlap's copies of pages
    // (not guest memory as it is now) is not kept. TTT2_VDATA_CACHE=0: off. TTT2_DIAG_VERIFY=1: hits checked against
    // guest memory.
    // p76: ranges under TTT2_VDATA_CACHE_MIN bytes (4096) are copied without asking: the lookup (write-watch sync, three
    // hash tables) cost about as much as copying ~4 KB on both PCs (MEASURED 2026-10-29: here copying costs 0.056 ms a
    // MB and the cache's lookups added 0.07-0.13 ms a frame; on the player's PC 0.45 ms a MB, log 24/25, all stages).
    struct VdEntry { uint32_t dword = 0, seq = 0, gen = 0, hash = 0; uint8_t chunk0 = 0, chunk1 = 0; bool valid = false; };
    std::unordered_map<uint64_t, VdEntry> vdCache_;
    std::vector<uint32_t> vdHot_;                 // page -> the present until which it counts as written every frame
    uint32_t vdPresent_ = 0, vdSeqAt_[4] = {};    // presents; the dirty tracker's sequence at the last four
    uint64_t vdHits_ = 0, vdHitBytes_ = 0, vdKept_ = 0, vdHotCopies_ = 0, vdVerified_ = 0, vdDiffer_ = 0;
    uint64_t vdSmall_ = 0, vdSmallBytes_ = 0;     // p76: under VdMinBytes, copied without a lookup
    static uint32_t VdMinBytes()
    {
        static const uint32_t n = [] { const char* e = std::getenv("TTT2_VDATA_CACHE_MIN"); return e && *e ? uint32_t(std::strtoul(e, nullptr, 0)) : 4096u; }();
        return n;
    }
    static bool VdVerifyOn() { static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_VERIFY"); return e && *e == '1'; }(); return on; }
    bool VdCacheAvailable() const
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_VDATA_CACHE"); return !(e && *e == '0'); }();
        return on && vdataNoOverwrite_ && !RingDiscard() && vdataFence_.chunk && !FlashMax() && !PoisonOn() && !VcacheStatsOn();
    }
    bool VdCacheUsable() const { return VdCacheAvailable() && !vdAbOff_; }
    // p78: DIAGNOSTIC BUILD A/B (TTT2_DIAG_VDATA_AB=0 off; with the frame overlap's A/B only): the cache is off in every
    // other pair of report windows, so the four combinations with overlapped / in-turn frames come round every 20 s.
    // Here copying is cheap and the cache's lookups cost about what they save (2026-10-29: vertex data 0.33 vs 0.31 ms on
    // Tulip Festival while 23% fewer bytes were copied); the player's PC copies ~8x slower but looks up 10-30x slower
    // (logs 24/25: "vs lookup call" 0.25-0.32 ms against 0.01 here) - only the player's log can tell.
    bool vdAbOff_ = false;
    uint32_t abWindows_ = 0;
    static bool VdAbOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_VDATA_AB"); return e && *e == '1'; }();   // p111: off (the cache on)
        return on;
    }
    bool VdNearWriter(size_t c) const
    {
        const size_t cur = vdataFence_.cur == SIZE_MAX ? RingFence::Chunks - 1 : vdataFence_.cur;
        const size_t d = (c + RingFence::Chunks - cur) % RingFence::Chunks;
        return d >= 1 && d <= 3;        // p76: asked before this draw's ring space is taken, which may enter the next chunk
    }
    static uint32_t VdHash(const uint32_t* p, uint32_t dwords)
    {
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < dwords; ++i) { h ^= p[i]; h *= 16777619u; }
        return h;
    }
    // True: drawn from the ring (entry). False: copy it - and keep the copy (VdStore) if `entry` is set.
    bool VdCached(uint32_t first, uint32_t count, VdEntry*& entry)
    {
        entry = nullptr;
        const uint32_t bytes = count * 4;
        if (bytes < VdMinBytes()) { ++vdSmall_; vdSmallBytes_ += bytes; return false; }
        const uint32_t p0 = first >> 12, p1 = uint32_t((uint64_t(first) + bytes + 4095) >> 12);
        if (vdHot_.empty()) { vdHot_.assign(1u << 20, 0); vdCache_.reserve(1u << 16); }
        for (uint32_t p = p0; p < p1; ++p)
            if (vdHot_[p] > vdPresent_) { ++vdHotCopies_; return false; }
        dirty_.SyncOnce(first, bytes);
        const uint32_t newest = dirty_.Newest(first, bytes);
        VdEntry& e = vdCache_[uint64_t(first) << 32 | count];
        if (e.valid && newest <= e.seq && vdataFence_.gen[e.chunk0] == e.gen && !VdNearWriter(e.chunk0) && !VdNearWriter(e.chunk1))
        {
            if (VdVerifyOn())
            {
                thread_local std::vector<uint8_t> scratch;
                const uint32_t h = VdHash(reinterpret_cast<const uint32_t*>(frame_overlap::CpReadBegin(first, bytes, scratch)), count);
                frame_overlap::CpReadEnd();
                ++vdVerified_;
                if (h != e.hash && vdDiffer_++ < 20)
                    std::fprintf(stderr, "[verify] vertex data cache: %08X (%u bytes) changed in guest memory, the write watch saw no write\n", first, bytes);
            }
            ++vdHits_;
            vdHitBytes_ += bytes;
            entry = &e;
            return true;
        }
        e.valid = false;
        if (newest > vdSeqAt_[(vdPresent_ - 2) & 3])
        {
            for (uint32_t p = p0; p < p1; ++p) vdHot_[p] = vdPresent_ + 120;
            ++vdHotCopies_;
            return false;
        }
        e.seq = dirty_.Current();
        entry = &e;
        return false;
    }
    void VdStore(VdEntry& e, uint64_t dword, uint32_t count, const uint32_t* src, bool fromGuest)
    {
        if (!fromGuest || !count) { e.valid = false; return; }      // the frame overlap's copies: not memory as it is now
        e.dword = uint32_t(dword);
        e.chunk0 = uint8_t(dword * 4 / vdataFence_.chunk);
        e.chunk1 = uint8_t((dword * 4 + uint64_t(count) * 4 - 1) / vdataFence_.chunk);
        e.gen = vdataFence_.gen[e.chunk0];
        e.hash = VdVerifyOn() ? VdHash(src, count) : 0;
        e.valid = true;
        ++vdKept_;
    }
    void VdPresent()
    {
        g_geoPresent.fetch_add(1, std::memory_order_relaxed);     // TEST ONLY (p102)
        ++vdPresent_;
        vdSeqAt_[vdPresent_ & 3] = dirty_.Current();
        if (vdCache_.size() > 200000) vdCache_.clear();
    }
    void CensusVertex(const std::vector<detail::VsFetch>& fetches)
    {
        if (!CensusOn()) return;
        for (const auto& f : fetches)
            if (!censusSeen_[(2ull << 40) | f.format]++)
                std::fprintf(stderr, "[census] vertex fetch fmt=%u (first: slot %u stride %u)\n", f.format, f.slot, f.stride);
    }
    uint64_t textureBytes_ = 0;
    uint32_t stageLoadsSeen_ = 0;                    // FollowStageLoad
    ComPtr<IDXGIAdapter3> memoryAdapter_;            // ReportVideoMemory
    bool memoryAdapterAsked_ = false;
    static constexpr uint64_t TextureBudgetBytes = 1536ull << 20;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    // CPU-read readbacks: a fence signalled after each staged copy, so the reading thread can wait
    // for exactly that copy without the context or the backend lock (ID3D11Fence is free-threaded).
    ComPtr<ID3D11DeviceContext4> context4_;
    ComPtr<ID3D11Fence> readbackFence_;
    uint64_t readbackFenceValue_ = 0, pendingFence_ = 0;
    // TTT2_GPU_PROBE=1 (diagnostics): the fence also marks every present and staged copy; a thread
    // logs each one the GPU passes more than 4 ms after it was queued (what holds the GPU queue?).
    struct ProbeMark { uint64_t value; const char* label; std::chrono::steady_clock::time_point queued; };
    std::mutex probeMutex_;
    std::condition_variable probeWake_;
    std::deque<ProbeMark> probeMarks_;
    void Probe(const char* label)
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_GPU_PROBE"); return e && *e == '1'; }();
        if (!on || !readbackFence_) return;
        static std::once_flag started;
        std::call_once(started, [this] {
            std::thread([this] {
                HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                int logged = 0;
                for (;;)
                {
                    ProbeMark m;
                    {
                        std::unique_lock l(probeMutex_);
                        probeWake_.wait(l, [this] { return !probeMarks_.empty(); });
                        m = probeMarks_.front();
                        probeMarks_.pop_front();
                    }
                    if (FAILED(readbackFence_->SetEventOnCompletion(m.value, done))) continue;
                    WaitForSingleObject(done, 2000);
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m.queued).count();
                    if (ms > 4.0 && logged++ < 2000) std::fprintf(stderr, "[gpu-probe] %s passed %.2f ms after it was queued\n", m.label, ms);
                }
            }).detach();
        });
        context4_->Signal(readbackFence_.Get(), ++readbackFenceValue_);
        {
            std::lock_guard l(probeMutex_);
            probeMarks_.push_back({ readbackFenceValue_, label, std::chrono::steady_clock::now() });
        }
        probeWake_.notify_one();
    }
    // p_diag68: how long after the present call the GPU is done with the frame - its queue (DXGI lets this thread run up
    // to 3 frames ahead) plus the frame's own unfinished work, input delay a slower graphics card adds. A fence value after
    // every present, a thread stamping each as it passes; the 5 s stats print the spread (ReportFrameCost).
    // DIAGNOSTIC since TEST 17: TTT2_DIAG_GPU_DONE=1. MEASURED (tester logs 46-51, TEST 15 vs 16): the extra fence signal
    // after every present took the command processor's present step from 0.39 to 0.77 ms (Paris) and flushed the next
    // frame's GPU-timer begin query at once, so "gpu ms between presents" read the frame interval instead of the GPU's
    // work; off, the D3D11 present is TEST 15's again.
    std::mutex doneMutex_;
    std::condition_variable doneWake_;
    std::deque<std::pair<uint64_t, std::chrono::steady_clock::time_point>> doneMarks_;
    std::vector<float> doneMs_;                          // under doneMutex_
    void MarkPresentDone()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_GPU_DONE"); return e && *e == '1'; }();
        if (!on || !readbackFence_) return;
        static std::once_flag started;
        std::call_once(started, [this] {
            std::thread([this] {
                DiagNameThread(L"gpu-done");
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);   // a stamp, not work: wakes 60 times a second
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                for (;;)
                {
                    std::pair<uint64_t, std::chrono::steady_clock::time_point> m;
                    {
                        std::unique_lock l(doneMutex_);
                        doneWake_.wait(l, [this] { return !doneMarks_.empty(); });
                        m = doneMarks_.front();
                        doneMarks_.pop_front();
                    }
                    if (!ev || FAILED(readbackFence_->SetEventOnCompletion(m.first, ev)) || WaitForSingleObject(ev, 2000) != WAIT_OBJECT_0)
                        continue;
                    const float ms = float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m.second).count());
                    std::lock_guard l(doneMutex_);
                    if (doneMs_.size() < 4096) doneMs_.push_back(ms);
                }
            }).detach();
        });
        context4_->Signal(readbackFence_.Get(), ++readbackFenceValue_);
        {
            std::lock_guard l(doneMutex_);
            if (doneMarks_.size() < 64) doneMarks_.emplace_back(readbackFenceValue_, std::chrono::steady_clock::now());
        }
        doneWake_.notify_one();
    }
    ComPtr<ID3D11Buffer> vertexRing_, indexRing_, psConstants_, psState_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11SamplerState> pointClamp_, linearClamp_;
    std::unordered_map<uint64_t, ComPtr<ID3D11SamplerState>> samplers_;
    ID3D11SamplerState* Sampler(const uint32_t info[4], uint32_t mip = 0)
    {
        static const uint32_t anisoFor[display_settings::kMaxTextureFilter + 1] = { 0, 0, 2, 4, 8, 16 };
        const int filter = std::clamp(display_settings::GetTextureFilter(), 0, display_settings::kMaxTextureFilter);
        const uint32_t userAniso = anisoFor[filter];
        const uint64_t key = info[0] | (info[1] << 3) | (info[2] << 6) | (info[3] << 7) | (uint64_t(mip) << 9) | (uint64_t(userAniso) << 33) | (uint64_t(filter != 0) << 38);
        ComPtr<ID3D11SamplerState>& s = samplers_[key];
        if (!s)
        {
            const D3D11_SAMPLER_DESC d = SamplerDesc(info, mip, userAniso, filter != 0);
            Check(device_->CreateSamplerState(&d, &s), "CreateSamplerState");
            g_diagStateCreates.fetch_add(1, std::memory_order_relaxed);
        }
        return s.Get();
    }
    ComPtr<ID3D11ShaderResourceView> defaultSrv_, defaultArraySrv_, whiteSrv_, whiteArraySrv_;
    uint32_t overrideMask_ = 0;       // texture slots forced to white (diagnostics)
    // p_diag25: a slot's last texture binding in this batch (PixelStageAndDraw). texGen_ counts every change to what
    // BindTextureFrom reads besides guest memory and the fetch constant: resolves, write-backs, uploads, cache drops.
    uint64_t texGen_ = 0;
    struct TexMemo
    {
        bool valid = false;
        uint32_t fc[6] = {}, epoch = 0, dirtySeq = 0;
        uint64_t gen = 0;
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11SamplerState* sampler = nullptr;
        uint32_t info[4] = {}, sampInfo[4] = {};
        float scale[4] = {};
    };
    TexMemo texMemo_[32][2];
    static bool TexMemoOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_TEX_MEMO"); return !(e && *e == '0'); }();
        return on;
    }
    // p107: the texture memo ACROSS batches - a binding (view, sampler, shader constants) by fetch constant. The memo
    // above lived one batch with nothing written anywhere (~60 batches a frame, vertex data written in most), so
    // nearly every bind ran BindTextureFrom: ~1.2 us a slot on the player's PC (2.4 ms a frame; MEASURED log 30). An
    // entry of guest texture bytes (kind 1) stays valid while: no cached texture was dropped, re-uploaded or flushed
    // (texFullGen_); no valid resolve overlaps its bytes (looked up again once after each resolve: texGen_); none of
    // its pages written since (synced once a batch exactly as BindTextureFrom does, then the pages' sequences).
    // Fallbacks (kind 0) need only the fetch constant and texGen_; resolved data (kind 2) keeps the one-batch rule.
    // TTT2_TEX_MEMO2=0: off.
    struct TexMemo2 : TexMemo
    {
        uint32_t flags = 0;                        // key besides the fetch constant: bit 0 cube, bits 1-3 the anisotropy row
        uint8_t kind = 0;
        uint32_t seq = 0;                          // the dirty sequence its pages were current at
        uint32_t base = 0, bytes = 0, mipStart = 0, mipSize = 0;
        uint64_t fullGen = 0;
    };
    static constexpr uint32_t kTexMemo2Entries = 2048;
    std::vector<TexMemo2> texMemo2_;
    uint64_t texFullGen_ = 0;                      // cached textures dropped, re-uploaded or flushed
    uint64_t diagLastFullGen_ = 0, diagLastTexGen_ = 0;
    uint64_t diagTexBinds_ = 0, diagTexMemoHits_ = 0, diagTexMemoStale_ = 0, diagTexMemoRechecks_ = 0;
    static bool TexMemo2On()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_TEX_MEMO2"); return !(e && *e == '0'); }();
        return on;
    }
    // The entry for this fetch constant: `hit` when it is still valid; otherwise the entry the bind refills.
    TexMemo2* TexMemo2Find(const uint32_t* fc, uint32_t flags, uint32_t epoch, bool& hit)
    {
        if (texMemo2_.empty()) texMemo2_.resize(kTexMemo2Entries);
        uint64_t h = flags;
        for (int i = 0; i < 6; ++i) h = (h ^ fc[i]) * 0x9E3779B97F4A7C15ull;
        const uint32_t home = uint32_t(h >> 40);
        TexMemo2* place = nullptr;
        hit = false;
        for (uint32_t i = 0; i < 4; ++i)
        {
            TexMemo2& e = texMemo2_[(home + i) & (kTexMemo2Entries - 1)];
            const bool live = e.valid && (e.kind == 1 ? e.fullGen == texFullGen_ : e.gen == texGen_);
            if (!live) { if (!place) place = &e; continue; }
            if (e.flags != flags || std::memcmp(e.fc, fc, sizeof(e.fc)) != 0) continue;
            hit = TexMemo2Valid(e, epoch);
            if (!hit) ++diagTexMemoStale_;
            return &e;
        }
        return place ? place : &texMemo2_[home & (kTexMemo2Entries - 1)];
    }
    bool TexMemo2Valid(TexMemo2& e, uint32_t epoch)
    {
        if (e.kind == 0) return true;
        if (e.kind != 1) return e.epoch == epoch && e.dirtySeq == dirty_.Current();
        if (e.gen != texGen_)
        {
            // Resolves since: BindTextureFrom reads a valid resolve over these bytes first.
            ++diagTexMemoRechecks_;
            auto over = [](uint32_t a, uint32_t an, uint32_t b, uint32_t bn) { return uint64_t(a) < uint64_t(b) + bn && uint64_t(b) < uint64_t(a) + an; };
            for (const auto& [rs, r] : resolved_)
                if (r.valid && (over(rs, r.size, e.base, e.bytes) || (e.mipSize && over(rs, r.size, e.mipStart, e.mipSize))))
                    return false;
            e.gen = texGen_;
        }
        if (e.epoch != epoch)
        {
            dirty_.SyncOnce(e.base, e.bytes);
            if (e.mipSize) dirty_.SyncOnce(e.mipStart, e.mipSize);
            GpuNoteMemory(5, e.base, e.bytes);
            if (e.mipSize) GpuNoteMemory(5, e.mipStart, e.mipSize);
            e.epoch = epoch;
        }
        else if (e.dirtySeq == dirty_.Current()) return true;
        if (dirty_.Newest(e.base, e.bytes) > e.seq || (e.mipSize && dirty_.Newest(e.mipStart, e.mipSize) > e.seq)) return false;
        e.dirtySeq = dirty_.Current();
        return true;
    }
    // p107: cached textures over each 64 KB of guest memory - DropTexturesOverlapping looks through the cache only when
    // a count over the range is not zero (every resolve dropped overlapping textures by visiting all ~1,750 cached
    // ones; a resolve's destination is almost never a cached texture's). TTT2_TEX_BLOCKS=0: always look.
    std::vector<uint16_t> texBlocks_;
    uint64_t diagDropCalls_ = 0, diagDropSkips_ = 0, diagDropTicks_ = 0;
    static bool TexBlocksOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_TEX_BLOCKS"); return !(e && *e == '0'); }();
        return on;
    }
    template <class Fn> static void TexBlockRange(uint32_t start, uint32_t size, Fn&& fn)
    {
        if (!size) return;
        for (uint64_t b = start >> 16, e = std::min<uint64_t>((uint64_t(start) + size + 0xFFFF) >> 16, 1u << 16); b < e; ++b) fn(size_t(b));
    }
    void TexBlocksCount(uint32_t rangeStart, uint32_t rangeSize, uint32_t mipStart, uint32_t mipSize, int delta)
    {
        if (texBlocks_.empty()) texBlocks_.assign(1u << 16, 0);
        auto add = [&](size_t b) { texBlocks_[b] = uint16_t(texBlocks_[b] + delta); };
        TexBlockRange(rangeStart, rangeSize, add);
        TexBlockRange(mipStart, mipSize, add);
    }
    bool TexBlocksAny(uint32_t start, uint32_t size) const
    {
        if (texBlocks_.empty()) return false;
        bool any = false;
        TexBlockRange(start, size, [&](size_t b) { any |= texBlocks_[b] != 0; });
        return any;
    }
    uint32_t overridePsDwords_ = 0;   // ...only in pixel shaders of this size (0 = cube shaders)
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11Texture2D> cpuFront_, lastFront_;
    uint32_t lastFrontW_ = 1280, lastFrontH_ = 720;     // guest size lastFront_ holds
    std::atomic<HWND> window_{ nullptr };
    bool lastSwap_ = false;
    ComPtr<ID3D11VertexShader> blitVs_;
    ComPtr<ID3D11RenderTargetView> backRtv_;
    ID3D11Texture2D* backRtvFor_ = nullptr;

    // Depth resolve: D3D11 cannot copy a sub-rectangle of a depth-stencil resource, so read the
    // typeless surface's R32F view in a full-screen pass (sample 0 of each pixel).
    // A resolve through a shader: redGreen keeps the colour surface's red and green (a
    // 16_16_FLOAT destination, R16G16_FLOAT texture); otherwise the texel converts as is (a
    // 16_16_FLOAT or other non-8888 surface resolved into an 8_8_8_8 destination).
    ComPtr<ID3D11PixelShader> redGreenBlitPs_, colorBlitPs_;
    void BlitColor(ColorSurface& s, Resolved& r, UINT w, UINT h, bool redGreen)
    {
        pipe_.valid = false;
        ComPtr<ID3D11PixelShader>& blitPs = redGreen ? redGreenBlitPs_ : colorBlitPs_;
        if (!blitPs)
        {
            BlitVs();
            auto code = Compile(redGreen
                                    ? "Texture2D<float4> src : register(t0);"
                                      " float4 main(float4 pos : SV_Position) : SV_Target { float4 c = src.Load(int3(int2(pos.xy), 0));"
                                      " return float4(c.r, c.g, 0, 0); }"
                                    : "Texture2D<float4> src : register(t0);"
                                      " float4 main(float4 pos : SV_Position) : SV_Target { return src.Load(int3(int2(pos.xy), 0)); }",
                                "ps_5_0", redGreen ? "ttt2 16_16_float resolve ps" : "ttt2 colour convert resolve ps");
            Check(device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &blitPs), "CreateColorBlitPs");
        }
        if (!s.srv) Check(device_->CreateShaderResourceView(s.tex.Get(), nullptr, &s.srv), "CreateColorSrv");
        ID3D11ShaderResourceView* nulls[32] = {};
        context_->PSSetShaderResources(0, 32, nulls);
        ID3D11RenderTargetView* rtv = r.rtv.Get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(nullptr, 0);
        w = Px(w, s.scale); h = Px(h, s.scale);     // texel for texel: both sides carry the scale
        D3D11_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(raster_.Get());
        D3D11_RECT sc{ 0, 0, LONG(w), LONG(h) };
        context_->RSSetScissorRects(1, &sc);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(blitVs_.Get(), nullptr, 0);
        context_->PSSetShader(blitPs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* v = s.srv.Get();
        context_->PSSetShaderResources(0, 1, &v);
        context_->Draw(3, 0);
        context_->PSSetShaderResources(0, 1, nulls);
    }
    std::map<uint32_t, ComPtr<ID3D11PixelShader>> depthBlitPs_;
    void BlitDepth(DepthSurface& s, Resolved& r)
    {
        pipe_.valid = false;
        const uint32_t sx = std::clamp<uint32_t>(s.sampX, 1, 4), sy = std::clamp<uint32_t>(s.sampY, 1, 4);
        ComPtr<ID3D11PixelShader>& ps = depthBlitPs_[(sx << 4) | sy];
        if (!ps)
        {
            BlitVs();
            // Beyond the surface's extent nothing was drawn: the cleared store's far depth.
            const std::string src = "Texture2D<float> src : register(t0);"
                " float main(float4 pos : SV_Position) : SV_Target { int2 p = int2(pos.xy) * int2(" +
                std::to_string(sx) + ", " + std::to_string(sy) + "); uint w, h; src.GetDimensions(w, h);"
                " return (uint(p.x) < w && uint(p.y) < h) ? src.Load(int3(p, 0)) : 1.0; }";
            auto code = Compile(src, "ps_5_0", "ttt2 depth resolve ps");
            Check(device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps), "CreateDepthBlitPs");
        }
        // The shader's integer sample step holds at any scale (pixel p of the scaled resolve =
        // sample p x samp of the scaled surface).
        const UINT lw = std::min<UINT>(r.pitch, SurfaceW / sx), lh = std::min<UINT>(r.height, SurfaceH / sy);
        ID3D11ShaderResourceView* nulls[32] = {};
        context_->PSSetShaderResources(0, 32, nulls);              // the target may still be bound as input
        ID3D11RenderTargetView* rtv = r.rtv.Get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(nullptr, 0);
        const UINT w = std::min(Px(lw, s.scale), AllocPx(r.pitch, r.scale)), h = std::min(Px(lh, s.scale), AllocPx(r.height, r.scale));
        D3D11_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(raster_.Get());
        D3D11_RECT sc{ 0, 0, LONG(w), LONG(h) };
        context_->RSSetScissorRects(1, &sc);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(blitVs_.Get(), nullptr, 0);
        context_->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* v = s.srv.Get();
        context_->PSSetShaderResources(0, 1, &v);
        context_->Draw(3, 0);
        context_->PSSetShaderResources(0, 1, nulls);
    }
    void BlitVs()
    {
        if (blitVs_) return;
        auto vs = Compile("float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2);"
                          " return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }", "vs_5_0", "ttt2 blit vs");
        Check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &blitVs_), "CreateBlitVs");
    }
    size_t ringOffset_ = 0, indexOffset_ = 0, vdataOffset_ = 0;
    // The dynamic rings wrap without DISCARD. MEASURED 2026-10-15 on Sakura Schoolyard (7 MB of
    // guest vertex data a frame): DISCARD at each wrap had the driver rename the 64 MB ring every
    // ~9 frames - a fresh allocation the kernel pages on the copy engine (the System process at
    // ~10% copy) - and presents across the whole desktop (other programs' too) blocked 10-20 ms on
    // a quarter of the frames. Instead each ring is eight chunks, an event query marking the last
    // command that read each one; writing into a chunk again first waits for its query - long
    // done in practice, the ring holding several frames. TTT2_RING_DISCARD=1: the old wraps.
    struct RingFence
    {
        static constexpr size_t Chunks = 8;
        size_t chunk = 0;                         // bytes per chunk
        ComPtr<ID3D11Query> done[Chunks];
        bool issued[Chunks] = {};                 // query ended for the chunk's previous cycle
        size_t cur = SIZE_MAX, ended = 0;         // furthest chunk written this cycle; chunks [0, ended) marked
        uint32_t gen[Chunks] = {};                // p73: bumped as the ring enters the chunk (the data in it goes)
        bool reread[Chunks] = {};                 // p73: a draw read the vertex data cache there since the chunk's mark
    };
    RingFence vertexFence_, indexFence_, vdataFence_;
    static bool RingDiscard()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_RING_DISCARD"); return e && *e == '1'; }();
        return on;
    }
    // Before writing [start, start + bytes) of a ring of `ringBytes`; `wrapped`: the write restarts it at 0.
    void RingWrite(RingFence& f, size_t ringBytes, size_t start, size_t bytes, bool wrapped)
    {
        if (!f.chunk)
        {
            f.chunk = ringBytes / RingFence::Chunks;
            for (auto& q : f.done)
            {
                D3D11_QUERY_DESC d{ D3D11_QUERY_EVENT, 0 };
                Check(device_->CreateQuery(&d, &q), "CreateRingQuery");
            }
        }
        auto mark = [&](size_t c) { context_->End(f.done[c].Get()); f.issued[c] = true; };
        if (wrapped)
        {
            if (f.cur != SIZE_MAX)
                for (size_t c = f.ended; c <= f.cur; ++c) mark(c);
            f.cur = SIZE_MAX;
            f.ended = 0;
        }
        // Chunks before this write's start: every command reading them is already queued.
        const size_t first = start / f.chunk, last = std::min(RingFence::Chunks - 1, (start + std::max<size_t>(bytes, 1) - 1) / f.chunk);
        for (; f.ended < first; ++f.ended) mark(f.ended);
        for (size_t c = f.cur == SIZE_MAX ? 0 : f.cur + 1; c <= last; ++c)
        {
            ++f.gen[c];
            // p73: chunks the vertex data cache read since their marks are marked again now - the ring is still at least
            // a chunk away from each (VdNearWriter), so their queries are long done when it gets there.
            for (size_t k = 0; k < RingFence::Chunks; ++k)
                if (f.reread[k]) { mark(k); f.reread[k] = false; }
            if (!f.issued[c]) continue;
            BOOL done = FALSE;
            if (context_->GetData(f.done[c].Get(), &done, sizeof(done), 0) != S_OK)
            {
                const auto t0 = std::chrono::steady_clock::now();
                while (context_->GetData(f.done[c].Get(), &done, sizeof(done), 0) != S_OK) YieldProcessor();
                ++stats_.ringWaits;
                stats_.ringWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
            f.issued[c] = false;
        }
        if (f.cur == SIZE_MAX || last > f.cur) f.cur = last;
    }
    struct PipeCache;
    // ---- p_diag44: the constant ring. Each draw wrote up to four constant buffers with a DISCARD map each (vertex
    // constants, vertex state, pixel constants, pixel state) plus the vertex data and the indices: ~2,700 maps a frame
    // on the player's PC (log 22: ~1.2 us each, 3.7 ms of a 14 ms frame). Now every block a draw changes goes into one
    // no-overwrite map of one ring, bound as windows (D3D11.1 constant-buffer offsets). Blocks never straddle the
    // ring's eight chunks: entering a chunk writes all four again, so no bound window refers to an earlier chunk and
    // RingWrite's chunk fences stay exact. Self-tested at start (some drivers report the feature and ignore offsets);
    // TTT2_CB_RING=0 or a failed test: the old buffers.
    ComPtr<ID3D11DeviceContext1> context1_;
    ComPtr<ID3D11Buffer> cbRing_;
    RingFence cbFence_;
    struct CbBlock { UINT first = 0, num = 0; uint32_t gen = 0; };   // window in constants; gen: cbChunkGen_ when written
    CbBlock cbBlock_[4];                                  // vertex constants, vertex state, pixel constants, pixel state
    size_t cbOffset_ = 0, cbChunk_ = 0;
    uint32_t cbChunkGen_ = 1;
    bool cbRingOk_ = false, cbRingOn_ = false, cbUnbindFirst_ = false;
    // p77: the vertex-constant block holds only the registers the draw's shader reads (VsTranslation::constRegs, in
    // 16s): ~320 draws a frame wrote all 512 (8 KB, 2.6 MB a frame in fights, MEASURED 2026-10-29) - copying cost the
    // low-end player's PC 1.4-1.9 ms a frame ("constants", logs 24/25). Bound windows smaller than the shader's
    // cbuffer: reads past them return 0 in D3D11, and the shader reads none; still self-tested (CbSelfTest partial).
    // TTT2_VS_CONST_TRIM=0: whole blocks.
    bool cbPartialOk_ = false;
    uint32_t vsCbConsts_ = 512;                           // the current draw's need, in constants (a multiple of 16)
    uint64_t diagVsBlockWrites_ = 0, diagVsBlockBytes_ = 0;
    bool pendingVsConst_ = false, pendingPsConst_ = false, pendingPsState_ = false;
    bool dataRingIbOk_ = false, dataRingOk_ = false, dataRingIndices_ = false;
    VsStateCB pendingVsState_{};
    size_t CbBlockBytes(int k) const
    {
        return k == 0 ? size_t(vsCbConsts_) * 16 : k == 1 ? (sizeof(VsStateCB) + 255) & ~size_t(255) : k == 2 ? size_t(psCbConsts_) * 16   // p105
                      : (sizeof(PsStateCB) + 255) & ~size_t(255);
    }
    void SetUpConstantRing()
    {
        static const bool want = [] { const char* e = std::getenv("TTT2_CB_RING"); return !(e && *e == '0'); }();
        D3D11_FEATURE_DATA_D3D11_OPTIONS o{};
        const bool opts = SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o)));
        const char* why = nullptr;
        if (!want) why = "TTT2_CB_RING=0";
        else if (!opts || !o.ConstantBufferOffsetting) why = "the driver has no constant-buffer offsets";
        else if (!o.MapNoOverwriteOnDynamicConstantBuffer) why = "the driver has no no-overwrite constant maps";
        else if (FAILED(context_.As(&context1_))) why = "no D3D11.1 context";
        else
        {
            D3D11_BUFFER_DESC b{};
            b.ByteWidth = CbRingBytes; b.Usage = D3D11_USAGE_DYNAMIC; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device_->CreateBuffer(&b, nullptr, &cbRing_))) why = "the ring could not be created";
            else
            {
                int test = -1;
                try { test = CbSelfTest(false) ? 0 : CbSelfTest(true) ? 1 : -1; }
                catch (const std::exception& e) { std::fprintf(stderr, "[gpu-hw] constant ring self-test: %s\n", e.what()); }
                if (test < 0) why = "its self-test failed";
                else { cbRingOk_ = true; cbUnbindFirst_ = test == 1; }
            }
        }
        cbRingOn_ = cbRingOk_;
        static const bool trim = [] { const char* e = std::getenv("TTT2_VS_CONST_TRIM"); return !(e && *e == '0'); }();
        if (cbRingOk_ && trim)
        {
            try { cbPartialOk_ = CbSelfTest(cbUnbindFirst_, true); }
            catch (const std::exception& e) { std::fprintf(stderr, "[gpu-hw] constant ring self-test (partial windows): %s\n", e.what()); }
        }
        pipe_.valid = false;
        if (cbRingOk_)
            std::fprintf(stderr, "[gpu-hw] constants: one ring, one map a draw (constant-buffer offsets, self-test passed%s); vertex constants: %s\n",
                         cbUnbindFirst_ ? " with unbinds" : "",
                         cbPartialOk_ ? "the registers the shader reads" : !trim ? "all 512 (TTT2_VS_CONST_TRIM=0)" : "all 512 (partial windows failed the self-test)");
        else
            std::fprintf(stderr, "[gpu-hw] constants: a buffer each (%s)\n", why);
    }
    // Draws with blocks bound at offsets of one buffer, rebinds that buffer at new offsets and writes into it with a
    // no-overwrite map while it is bound - what CbRingWrite / BindConstantRing do - and reads the answers back.
    // p77 `partial`: cbuffer A declared with 512 registers, bound as 16-register windows (the trimmed vertex constants).
    bool CbSelfTest(bool unbindFirst, bool partial = false)
    {
        const std::string common = std::string("cbuffer A : register(b0) { uint4 a[") + (partial ? "512" : "16") +
                                   "]; }; cbuffer B : register(b1) { uint4 b[16]; };"
                                   "struct V { float4 pos : SV_Position; nointerpolation uint4 v : TEXCOORD0; };";
        auto vsCode = Compile(std::string(common) +
            "V main(uint id : SV_VertexID) { V o; float2 p = float2((id << 1) & 2, id & 2);"
            " o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.v = uint4(a[0].x, b[0].x, a[15].w, b[15].w); return o; }",
            "vs_5_0", "cbtest_vs");
        auto psCode = Compile(std::string(common) +
            "uint4 main(V i) : SV_Target { return i.pos.x < 1.0 ? i.v : uint4(a[0].x, b[0].x, a[15].w, b[15].w); }",
            "ps_5_0", "cbtest_ps");
        ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
        Check(device_->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "CbTestVs");
        Check(device_->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "CbTestPs");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 65536 * 2; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> ring;
        Check(device_->CreateBuffer(&bd, nullptr, &ring), "CbTestRing");
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 2; td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target, staging[2];
        Check(device_->CreateTexture2D(&td, nullptr, &target), "CbTestTarget");
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& st : staging) Check(device_->CreateTexture2D(&td, nullptr, &st), "CbTestStaging");
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(device_->CreateRenderTargetView(target.Get(), nullptr, &rtv), "CbTestRtv");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
        ComPtr<ID3D11RasterizerState> rs;
        Check(device_->CreateRasterizerState(&rd, &rs), "CbTestRaster");
        // Block k (256 bytes at k * 4096): a[0].x = 10k + 1, a[15].w = 10k + 2. The second pass writes blocks 4 and 5
        // with a no-overwrite map while blocks 0-3 are still bound.
        auto fill = [&](D3D11_MAP map, int k0, int k1) {
            D3D11_MAPPED_SUBRESOURCE m{};
            Check(context_->Map(ring.Get(), 0, map, 0, &m), "CbTestMap");
            for (int k = k0; k < k1; ++k)
            {
                uint32_t* d = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(m.pData) + size_t(k) * 4096);
                std::memset(d, 0, 256);
                d[0] = 10u * k + 1; d[63] = 10u * k + 2;
            }
            context_->Unmap(ring.Get(), 0);
        };
        ID3D11Buffer* const two[2] = { ring.Get(), ring.Get() };
        ID3D11Buffer* const nulls[2] = { nullptr, nullptr };
        auto bind = [&](int vs0, int vs1, int ps0, int ps1) {
            const UINT vf[2] = { UINT(vs0) * 256, UINT(vs1) * 256 }, pf[2] = { UINT(ps0) * 256, UINT(ps1) * 256 }, n[2] = { 16, 16 };
            if (unbindFirst) { context_->VSSetConstantBuffers(0, 2, nulls); context_->PSSetConstantBuffers(0, 2, nulls); }
            context1_->VSSetConstantBuffers1(0, 2, two, vf, n);
            context1_->PSSetConstantBuffers1(0, 2, two, pf, n);
        };
        context_->ClearState();
        context_->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT vp{ 0, 0, 2, 1, 0, 1 };
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(rs.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs.Get(), nullptr, 0);
        context_->PSSetShader(ps.Get(), nullptr, 0);
        fill(D3D11_MAP_WRITE_DISCARD, 0, 4);
        bind(1, 2, 2, 3);
        context_->Draw(3, 0);
        context_->CopyResource(staging[0].Get(), target.Get());
        fill(D3D11_MAP_WRITE_NO_OVERWRITE, 4, 6);
        bind(5, 0, 4, 1);
        context_->Draw(3, 0);
        context_->CopyResource(staging[1].Get(), target.Get());
        context_->ClearState();
        pipe_.valid = false;
        const uint32_t want[2][8] = { { 11, 21, 12, 22, 21, 31, 22, 32 }, { 51, 1, 52, 2, 41, 11, 42, 12 } };
        bool ok = true;
        for (int pass = 0; pass < 2; ++pass)
        {
            D3D11_MAPPED_SUBRESOURCE m{};
            Check(context_->Map(staging[pass].Get(), 0, D3D11_MAP_READ, 0, &m), "CbTestRead");
            const uint32_t* got = static_cast<const uint32_t*>(m.pData);
            if (std::memcmp(got, want[pass], sizeof(want[pass])) != 0)
            {
                ok = false;
                std::fprintf(stderr, "[gpu-hw] constant ring self-test%s%s, pass %d: got %u %u %u %u / %u %u %u %u\n",
                             unbindFirst ? " (with unbinds)" : "", partial ? " (partial windows)" : "", pass + 1, got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7]);
            }
            context_->Unmap(staging[pass].Get(), 0);
        }
        return ok;
    }
    // The blocks this draw changed (and all four on entering a chunk) into the ring with one map.
    void CbRingWrite()
    {
        const void* src[4] = { lastVsConstants_.data(), &pendingVsState_, lastPsConstants_.data(), &lastState_ };
        const size_t srcBytes[4] = { 512 * 16, sizeof(VsStateCB), 4096, sizeof(PsStateCB) };
        // p77: also when the block holds fewer registers than this draw's shader reads.
        // p105: also when the pixel constant block holds fewer registers than this draw's shader reads, or the pixel state
        // block was written without one of the texture slots this draw's shader reads.
        bool w[4] = { pendingVsConst_ || (want_.gpuVs && cbBlock_[0].num < vsCbConsts_), want_.gpuVs,
                      pendingPsConst_ || cbBlock_[2].num < psCbConsts_, pendingPsState_ || (cbPsSlots_ & ~cbPsStateSlots_) != 0 };
        pendingVsConst_ = pendingPsConst_ = pendingPsState_ = false;
        for (int k = 0; k < 4; ++k) if (cbBlock_[k].gen != cbChunkGen_) w[k] = true;
        size_t need = 0;
        for (int k = 0; k < 4; ++k) if (w[k]) need += CbBlockBytes(k);
        if (!need) return;
        constexpr size_t chunk = CbRingBytes / RingFence::Chunks;
        size_t start = (cbOffset_ + 255) & ~size_t(255);
        bool wrapped = false;
        if (start + need > (cbChunk_ + 1) * chunk)          // the next chunk: every block again, there
        {
            cbChunk_ = cbChunk_ + 1 == RingFence::Chunks ? 0 : cbChunk_ + 1;
            wrapped = cbChunk_ == 0;
            start = cbChunk_ * chunk;
            ++cbChunkGen_;
            need = 0;
            for (int k = 0; k < 4; ++k) { w[k] = true; need += CbBlockBytes(k); }
        }
        RingWrite(cbFence_, CbRingBytes, start, need, wrapped);
        D3D11_MAPPED_SUBRESOURCE m{};
        Check(context_->Map(cbRing_.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m), "MapConstantRing");
        size_t at = start;
        for (int k = 0; k < 4; ++k)
        {
            if (!w[k]) continue;
            const size_t bytes = std::min(srcBytes[k], CbBlockBytes(k));      // p77: the vertex constants trimmed
            uint8_t* const dst = static_cast<uint8_t*>(m.pData) + at;
            const uint8_t* const from = static_cast<const uint8_t*>(src[k]);
            if (VerifyOn() && cbShadow_.empty()) cbShadow_.resize(CbRingBytes);
            CbPieces(k, bytes, [&](size_t o, size_t n) {
                std::memcpy(dst + o, from + o, n);
                if (VerifyOn()) std::memcpy(cbShadow_.data() + at + o, from + o, n);
                diagCbBytes_[k] += n;
            });
            if (k == 3) cbPsStateSlots_ = CbSparse() ? cbPsSlots_ : ~0u;
            if (k == 0) { ++diagVsBlockWrites_; diagVsBlockBytes_ += bytes; }
            cbBlock_[k] = CbBlock{ UINT(at / 16), UINT(CbBlockBytes(k) / 16), cbChunkGen_ };
            at += CbBlockBytes(k);
        }
        context_->Unmap(cbRing_.Get(), 0);
        cbOffset_ = at;
    }
    std::vector<uint8_t> cbShadow_;                      // TEST ONLY (p_diag45): what the ring holds, in verify runs
    // p105: the constant ring's blocks written only as far as the draw's shaders read them. Every draw wrote whole
    // blocks: the vertex state (2.3 KB: 96 fetch slots' regions + 32 fetch descriptors, of which a shader reads one to
    // a few), the pixel state (1.7 KB: 32 texture slots, a shader samples one to four) and 256 pixel constants (4 KB,
    // most shaders read a few dozen). MEASURED (diag build, player log 30, Riverside Promenade): ~3 MB a frame into the
    // constant ring, and each MB written into the GPU rings costs that PC ~0.45 ms. The rest of a block's window keeps
    // whatever the ring held there - no shader reads it. A pixel state block is written again when a draw's shader
    // samples a slot the block was written without (cbPsStateSlots_). TTT2_CB_SPARSE=0: whole blocks.
    bool cbSparseOff_ = false;                          // p106: the diag14 A/B's old way
    bool CbSparse() const
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_CB_SPARSE"); return !(e && *e == '0'); }();
        return on && cbPartialOk_ && !cbSparseOff_;
    }
    template <typename F>
    void CbPieces(int k, size_t bytes, F&& piece) const
    {
        if (k == 1 && CbSparse())
        {
            constexpr size_t head = offsetof(VsStateCB, vfRegion), fields = offsetof(VsStateCB, vte), desc = offsetof(VsStateCB, vfDesc);
            piece(0, head);
            for (uint32_t w = 0; w < 2; ++w)
                for (uint64_t m = cbVsSlotMask_[w]; m; m &= m - 1) piece(head + (w * 64 + uint32_t(__builtin_ctzll(m))) * 16, 16);
            piece(fields, desc - fields);
            if (cbVsFetches_) piece(desc, size_t(cbVsFetches_) * 16);
            return;
        }
        if (k == 3 && CbSparse())
        {
            constexpr size_t info = offsetof(PsStateCB, texInfo), scale = offsetof(PsStateCB, texScale),
                             samp = offsetof(PsStateCB, texSampler), tail = offsetof(PsStateCB, alphaRef);
            piece(0, info);
            for (uint32_t m = cbPsSlots_; m; m &= m - 1)
            {
                const size_t s = size_t(__builtin_ctz(m)) * 16;
                piece(info + s, 16); piece(scale + s, 16); piece(samp + s, 16);
            }
            piece(tail, sizeof(PsStateCB) - tail);
            return;
        }
        piece(0, bytes);
    }
    uint64_t cbVsSlotMask_[2] = {};                     // the vertex shader's fetch slots
    uint32_t cbVsFetches_ = 0, cbPsSlots_ = 0;          // its fetches; the pixel shader's texture slots
    uint32_t cbPsStateSlots_ = ~0u;                     // the slots the current pixel state block was written with
    uint32_t psCbConsts_ = 256;                         // the current draw's pixel constants, in registers (a multiple of 16)
    uint64_t diagCbBytes_[4] = {};
    static bool VerifyOn() { static const bool v = [] { const char* e = std::getenv("TTT2_DIAG_VERIFY"); return e && *e == '1'; }(); return v; }
    // TEST ONLY (p_diag45): the ring windows this draw reads hold exactly what the old buffers would.
    void VerifyRingData()
    {
        static uint64_t checked = 0, bad = 0;
        const void* src[4] = { lastVsConstants_.data(), &pendingVsState_, lastPsConstants_.data(), &lastState_ };
        const size_t srcBytes[4] = { 512 * 16, sizeof(VsStateCB), 4096, sizeof(PsStateCB) };
        if (want_.gpuVs && cbBlock_[0].num < vsCbConsts_ && bad++ < 30)      // p77
            std::fprintf(stderr, "[verify] vertex constant window %u registers, the shader reads %u\n", cbBlock_[0].num, vsCbConsts_);
        if (cbBlock_[2].num < psCbConsts_ && bad++ < 30)                       // p105
            std::fprintf(stderr, "[verify] pixel constant window %u registers, the shader reads %u\n", cbBlock_[2].num, psCbConsts_);
        if ((cbPsSlots_ & ~cbPsStateSlots_) && bad++ < 30)
            std::fprintf(stderr, "[verify] pixel state block written without texture slots %08X\n", cbPsSlots_ & ~cbPsStateSlots_);
        for (int k = want_.gpuVs ? 0 : 2; k < 4; ++k)
        {
            const size_t bytes = std::min(srcBytes[k], size_t(cbBlock_[k].num) * 16);
            const uint8_t* const from = static_cast<const uint8_t*>(src[k]);
            bool same = !cbShadow_.empty() && size_t(cbBlock_[k].first) * 16 + bytes <= cbShadow_.size();
            if (same)       // p105: the parts this draw's shaders read
                CbPieces(k, bytes, [&](size_t o, size_t n) {
                    if (std::memcmp(cbShadow_.data() + size_t(cbBlock_[k].first) * 16 + o, from + o, n) != 0) same = false;
                });
            if (!same && bad++ < 30) std::fprintf(stderr, "[verify] constant ring block %d differs from the draw's data\n", k);
        }
        if (++checked % 200000 == 0)
            std::fprintf(stderr, "[verify] %llu draws' constant ring blocks checked, %llu differ\n", (unsigned long long)checked, (unsigned long long)bad);
    }
    void BindConstantRing(PipeCache& p)
    {
        ID3D11Buffer* const two[2] = { cbRing_.Get(), cbRing_.Get() };
        ID3D11Buffer* const nulls[2] = { nullptr, nullptr };
        for (int stage = 0; stage < 2; ++stage)
        {
            const int k0 = stage * 2;
            const bool c0 = p.cbFirst[k0] != cbBlock_[k0].first, c1 = p.cbFirst[k0 + 1] != cbBlock_[k0 + 1].first;
            if (!c0 && !c1) continue;
            const UINT sl = c0 ? 0 : 1, n = c0 && c1 ? 2 : 1;
            const UINT first[2] = { cbBlock_[k0 + sl].first, cbBlock_[k0 + 1].first };
            const UINT num[2] = { cbBlock_[k0 + sl].num, cbBlock_[k0 + 1].num };
            if (stage == 0)
            {
                if (cbUnbindFirst_) context_->VSSetConstantBuffers(sl, n, nulls);
                context1_->VSSetConstantBuffers1(sl, n, two, first, num);
            }
            else
            {
                if (cbUnbindFirst_) context_->PSSetConstantBuffers(sl, n, nulls);
                context1_->PSSetConstantBuffers1(sl, n, two, first, num);
            }
            p.cbFirst[k0] = cbBlock_[k0].first; p.cbFirst[k0 + 1] = cbBlock_[k0 + 1].first;
        }
    }
    // TEST ONLY (TTT2_DIAG_CBTOGGLE=1): C:/ttt2/port/fb/cbring_off.arm switches both rings off live (the old buffers),
    // for a same-scene picture comparison of the two paths.
    void CbToggleCheck()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_CBTOGGLE"); return e && *e == '1'; }();
        if (!on) return;
        static uint32_t n = 0;
        if (++n % 20) return;
        const bool off = std::filesystem::exists("C:/ttt2/port/fb/cbring_off.arm");
        if (cbRingOn_ == (cbRingOk_ && !off) && dataRingIndices_ == (dataRingOk_ && !off)) return;
        cbRingOn_ = cbRingOk_ && !off;
        dataRingIndices_ = dataRingOk_ && !off;
        ++cbChunkGen_;
        vsConstantsValid_ = constantsValid_ = stateValid_ = false;
        pipe_.valid = false;
        std::fprintf(stderr, "[gpu-hw] TEST: constant ring %s, indices in the vertex-data ring %s\n", cbRingOn_ ? "on" : "off",
                     dataRingIndices_ ? "on" : "off");
    }
    std::array<uint32_t, 1024> lastPsConstants_{};
    std::array<uint32_t, 2048> lastVsConstants_{};
    bool vsConstantsValid_ = false, vdataNoOverwrite_ = false, gpuVsEnabled_ = true;
    uint32_t vsConstWritesSeen_ = 0, psConstWritesSeen_ = 0;    // g_vsConstWrites / g_psConstWrites at the last compare
    ComPtr<ID3D11Buffer> vsConstants_, vsState_, vdataRing_;
    ComPtr<ID3D11ShaderResourceView> vdataSrv_;
    std::unordered_map<uint64_t, VsEntry> vertexShaders_;
    bool constantsValid_ = false, stateValid_ = false;
    PsStateCB lastState_{};
    uint32_t lastSlots_ = 0, lastCubeSlots_ = 0;
    uint32_t lastPsConstRegs_ = 256;            // p105: the last pixel shader looked up reads [0, this) of its constants
    std::unordered_map<uint32_t, ColorSurface> colors_;
    std::unordered_map<uint32_t, DepthSurface> depths_;
    std::unordered_map<uint32_t, Resolved> resolved_;
    std::unordered_map<TexKey, CachedTexture, TexKeyHash> textures_;
    struct PsEntry { ComPtr<ID3D11PixelShader> ps; uint32_t slots = 0, cubeSlots = 0, constRegs = 256; bool failed = false; };   // p105
    std::unordered_map<uint64_t, PsEntry> pixelShaders_;
    std::mutex preparedMutex_;                          // guards prepared*_ (warm-up thread)
    std::unordered_map<uint64_t, VsEntry> preparedVs_;
    std::unordered_map<uint64_t, PsEntry> preparedPs_;
    mutable std::mutex vsTextMutex_;                    // guards vsByText_ (build threads, warm-up)
    mutable std::unordered_map<uint64_t, ComPtr<ID3D11VertexShader>> vsByText_;   // HLSL text key -> compiled
    double buildBudgetMs_ = 24;                         // per frame, for waiting on async builds
    std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> blendStates_;
    std::unordered_map<uint64_t, ComPtr<ID3D11DepthStencilState>> depthStates_;
    std::unordered_map<uint32_t, ComPtr<ID3D11RasterizerState>> rasterStates_;
    DirtyTracker dirty_;
    Stats stats_, reported_;
    std::chrono::steady_clock::time_point lastStats_;
    std::filesystem::path shaderCacheDir_;
    uint64_t compares_ = 0;
    uint32_t dumps_ = 0;
    double lastStagedAgeMs_ = 0;                        // the last staged readback's copy: queued this long before its read
    double lastMapWaitMs_ = 0;                          // the last ReadTexture's wait for the GPU (TTT2_FRAMETIME)

    // ----------------------------------------------------------------- helpers
    // D3DCompile behind a structured-exception guard: a crash inside the compiler (a compiler bug on
    // some input) fails that one shader - drawn by the CPU path or skipped - instead of the game.
    static HRESULT GuardedCompile(const char* text, size_t size, const char* target, const char* name, UINT flags,
                                  ID3DBlob** code, ID3DBlob** errors)
    {
        __try { return D3DCompile(text, size, name, nullptr, nullptr, "main", target, flags, 0, code, errors); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
    }
    static ComPtr<ID3DBlob> Compile(const std::string& text, const char* target, const char* name,
                                    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL1)
    {
        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = GuardedCompile(text.data(), text.size(), target, name, flags, &code, &errors);
        if (hr == E_UNEXPECTED && !code) throw std::runtime_error("the shader compiler crashed on this shader");
        if (FAILED(hr))
        {
            std::string why = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "D3DCompile failed";
            throw std::runtime_error(why);
        }
        return code;
    }

    // %LOCALAPPDATA%\TTT2Recomp\shadercache, or TTT2_SHADER_CACHE (tests start from an empty one).
    static std::filesystem::path ShaderCacheDirectory()
    {
        char buffer[MAX_PATH] = {};
        std::filesystem::path p;
        DWORD n = GetEnvironmentVariableA("TTT2_SHADER_CACHE", buffer, MAX_PATH);
        if (n && n < MAX_PATH) p = buffer;
        else
        {
            n = GetEnvironmentVariableA("LOCALAPPDATA", buffer, MAX_PATH);
            if (!n || n >= MAX_PATH) return {};
            p = std::filesystem::path(buffer) / "TTT2Recomp" / "shadercache";
        }
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return ec ? std::filesystem::path{} : p;
    }

    void Upload(ID3D11Buffer* buffer, const void* data, size_t size)
    {
        if (toggle_ & 32) return;
        D3D11_MAPPED_SUBRESOURCE m{};
        Check(context_->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "MapConstants");
        std::memcpy(m.pData, data, size);
        context_->Unmap(buffer, 0);
    }

    ID3D11PixelShader* PixelShader(const std::vector<uint32_t>& code)
    {
        DiagScope dSh(diagTicks_[DpShaders]);
        const uint64_t key = PsKey(code);
        auto it = pixelShaders_.find(key);
        if (it != pixelShaders_.end())
        {
            lastSlots_ = it->second.slots; lastCubeSlots_ = it->second.cubeSlots; lastPsConstRegs_ = it->second.constRegs;
            return it->second.failed ? nullptr : it->second.ps.Get();
        }
        PsEntry e;
        if (TakePrepared(preparedPs_, key, e)) ++stats_.warmHits;
        else
        {
            Built<PsEntry> b;
            if (!AwaitBuild(pendingPs_, key, b, [this, code] {
                    Built<PsEntry> r;
                    r.result = BuildPixelShader(code, r.entry, r.counts, r.why, true);
                    return r;
                }))
            {
                ++stats_.pendingSkips;          // still compiling: its draws wait a frame or two
                lastSlots_ = 0;
                return nullptr;
            }
            stats_.diskHits += b.counts.diskHits; stats_.compiles += b.counts.compiles;
            if (b.result != BuildResult::Ready)
            {
                std::fprintf(stderr, b.result == BuildResult::Untranslatable ? "[gpu-hw] pixel shader %016llX (%zu dwords) not translatable: %s - its draws are skipped\n"
                                                                             : "[gpu-hw] pixel shader %016llX (%zu dwords) failed to compile - its draws are skipped:\n%s\n",
                             (unsigned long long)key, code.size(), b.why.c_str());
                pixelShaders_[key].failed = true;
                lastSlots_ = 0;
                return nullptr;
            }
            e = std::move(b.entry);
            RecordMicrocode("ps", key, code);
        }
        PsEntry& slot = pixelShaders_[key];
        slot = std::move(e);
        lastSlots_ = slot.slots; lastCubeSlots_ = slot.cubeSlots; lastPsConstRegs_ = slot.constRegs;
        return slot.ps.Get();
    }

    // ----------------------------------------------------------------- shader builds
    // Building a shader (translate the microcode, take its bytecode from the disk cache or
    // compile it, create the driver object) costs milliseconds, D3DCompile far more, and a
    // scene's first frame needs dozens at once: the fight's opening "Ready" stalled ~460 ms on
    // 46 of them. So every shader built is also recorded as microcode
    // (shadercache/ucode/{vs,ps}_<key>.bin), and a thread started with the device builds all
    // recorded ones (prepared*_) while the game boots; the command processor adopts a
    // prepared entry on first use instead of building it. TTT2_SHADER_WARM=0 turns this off.
    enum class BuildResult { Ready, Untranslatable, CompileFailed };
    struct BuildCounts { uint64_t diskHits = 0, compiles = 0; };

    // ----------------------------------------------------------------- asynchronous builds
    // A shader that is neither cached nor prepared is built on its own thread. The frame that
    // needs it waits for it only within a per-frame budget (reset at every present); past that
    // the draw is skipped (pixel shader) or drawn by the CPU vertex stage (vertex shader) until
    // the build lands, instead of the whole game stalling on D3DCompile (one TTT2 pixel shader
    // takes 4.4 s even unoptimised; a cold cache stalled the fight start 3.5 s).
    // TTT2_SHADER_ASYNC=0: wait for every build.
    template <class Entry> struct Built { BuildResult result = BuildResult::CompileFailed; Entry entry; BuildCounts counts; std::string why; };
    std::unordered_map<uint64_t, std::future<Built<VsEntry>>> pendingVs_;
    std::unordered_map<uint64_t, std::future<Built<PsEntry>>> pendingPs_;
    // `wait` false: never wait (a vertex shader: the CPU vertex stage draws meanwhile - no frame
    // held for a build that is all but never needed now that the cache is built ahead of time).
    template <class Entry, class Build>
    bool AwaitBuild(std::unordered_map<uint64_t, std::future<Built<Entry>>>& pending, uint64_t key, Built<Entry>& out, Build build,
                    bool wait = true)
    {
        static const bool async = [] { const char* e = std::getenv("TTT2_SHADER_ASYNC"); return !e || std::strcmp(e, "0") != 0; }();
        auto it = pending.find(key);
        if (it == pending.end()) it = pending.emplace(key, std::async(std::launch::async, build)).first;
        const auto start = std::chrono::steady_clock::now();
        const bool ready = !async ? (it->second.wait(), true)
                                  : it->second.wait_for(std::chrono::duration<double, std::milli>(wait ? std::max(0.0, buildBudgetMs_) : 0.0)) ==
                                        std::future_status::ready;
        buildBudgetMs_ -= std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!ready) return false;
        out = it->second.get();
        pending.erase(it);
        return true;
    }


    // A microcode's lookup key, remembered with a copy of the code (p_diag36): FNV-1a takes a multiply per byte, ~2.5 us
    // a draw for its vertex and pixel shader on an i3-3240 (log 22: 0.9 ms a frame). Taken from the cache only when the
    // code is equal, so the same keys as before. Per thread; 4 ways x 256 sets, the set from the size and 8 sampled
    // dwords, the least recently used way replaced. TTT2_KEY_MEMO=0: always hashed.
    static uint64_t MemoKey(const std::vector<uint32_t>& code, uint64_t seed)
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_KEY_MEMO"); return !(e && *e == '0'); }();
        const size_t n = code.size();
        if (!on || !n) return Fnv(code.data(), n * 4, seed);
        struct Way { uint64_t seed = 0, key = 0; uint32_t used = 0; std::vector<uint32_t> code; };
        struct Set { Way way[4]; };
        thread_local std::vector<Set> sets(256);
        thread_local uint32_t clock = 0;
        uint32_t h = uint32_t(n) * 0x9E3779B1u ^ uint32_t(seed);
        for (size_t i = 0; i < 8; ++i) { h ^= code[(n - 1) * i / 7]; h *= 0x85EBCA77u; h ^= h >> 13; }
        Set& set = sets[(h ^ (h >> 16)) & 255];
        ++clock;
        Way* oldest = &set.way[0];
        for (Way& w : set.way)
        {
            if (w.seed == seed && w.code.size() == n && std::memcmp(w.code.data(), code.data(), n * 4) == 0)
            {
                w.used = clock;
                static const bool verify = [] { const char* e = std::getenv("TTT2_DIAG_VERIFY"); return e && *e == '1'; }();
                if (verify)     // p_diag40 (TEST ONLY)
                {
                    static std::atomic<uint64_t> checked{ 0 }, bad{ 0 };
                    if (Fnv(code.data(), n * 4, seed) != w.key) ++bad;
                    if (++checked % 500000 == 0)
                        std::fprintf(stderr, "[verify] %llu shader keys from the cache checked, %llu differ\n",
                                     (unsigned long long)checked.load(), (unsigned long long)bad.load());
                }
                return w.key;
            }
            if (w.used < oldest->used) oldest = &w;
        }
        oldest->seed = seed; oldest->used = clock;
        oldest->key = Fnv(code.data(), n * 4, seed);
        oldest->code.assign(code.begin(), code.end());
        return oldest->key;
    }
    static uint64_t VsKey(const std::vector<uint32_t>& code) { return MemoKey(code, 0x5EEDull + code.size()); }
    static uint64_t PsKey(const std::vector<uint32_t>& code) { return MemoKey(code, 0xC0FFEEull + code.size()); }

    // Safe off the command-processor thread: uses only the (free-threaded) device and the
    // cache directory, both fixed after Initialize.
    // Vertex shaders are compiled per HLSL text: the game's run-time copies of one shader differ only in
    // their vertex fetches' parameters, which reach the shader per draw (VsFetchDescriptors), so they
    // translate to the same text and share one compiled shader (vsByText_; the warm-up loads every
    // compiled one, vg_<text key>.dxbc, at boot).
    BuildResult BuildVertexShader(const std::vector<uint32_t>& code, VsEntry& e, BuildCounts& n, std::string& why,
                                  bool inGame = false) const
    {
        e.t = TranslateVertexShader(code);
        if (!e.t.unsupported.empty()) { why = e.t.unsupported; return BuildResult::Untranslatable; }
        const std::string hlsl = VertexShaderHlsl(e.t);
        const uint64_t textKey = Fnv(hlsl.data(), hlsl.size());
        {
            std::lock_guard lock(vsTextMutex_);
            if (auto it = vsByText_.find(textKey); it != vsByText_.end()) { e.vs = it->second; ++n.diskHits; return BuildResult::Ready; }
        }
        const BuildResult r = CreateFromHlsl(hlsl, true, inGame, n, why,
                                             [&](const void* p, size_t size) { return device_->CreateVertexShader(p, size, nullptr, &e.vs); });
        if (r == BuildResult::Ready)
        {
            std::lock_guard lock(vsTextMutex_);
            vsByText_.emplace(textKey, e.vs);
        }
        return r;
    }
    // A run-time copy of a shader whose compiled form is already loaded: ready at once (the translation
    // only, well under a millisecond), no build thread, no frame waiting.
    bool SharedVertexShader(const std::vector<uint32_t>& code, VsEntry& e)
    {
        VsTranslation t = TranslateVertexShader(code);
        if (!t.unsupported.empty()) return false;
        const std::string hlsl = VertexShaderHlsl(t);
        const uint64_t textKey = Fnv(hlsl.data(), hlsl.size());
        std::lock_guard lock(vsTextMutex_);
        auto it = vsByText_.find(textKey);
        if (it == vsByText_.end()) return false;
        e.vs = it->second;
        e.t = std::move(t);
        return true;
    }
    BuildResult BuildPixelShader(const std::vector<uint32_t>& code, PsEntry& e, BuildCounts& n, std::string& why,
                                 bool inGame = false) const
    {
        const PsTranslation t = TranslatePixelShader(code);
        if (!t.unsupported.empty()) { why = t.unsupported; return BuildResult::Untranslatable; }
        e.slots = t.textureSlots; e.cubeSlots = t.cubeSlots; e.constRegs = t.constRegs;
        if (VerifyOn())     // p105 (TEST ONLY): the constant registers recorded against the shader's text
        {
            static int logged = 0;
            const uint32_t byText = std::min<uint32_t>(VsConstRegsInText(t.function), 256);
            if (byText != t.constRegs && logged++ < 30)
                std::fprintf(stderr, "[verify] pixel shader: its text reads %u constant registers, %u recorded\n", byText, t.constRegs);
        }
        return CreateFromHlsl(PixelShaderHlsl(t), false, inGame, n, why,
                              [&](const void* p, size_t size) { return device_->CreatePixelShader(p, size, nullptr, &e.ps); });
    }

    // A shader first met in game (not in the corpus yet) stalls the frame that needs it, and an
    // optimised D3DCompile of a large TTT2 shader takes a couple of hundred ms (measured: a 426
    // ms fight-start frame on two of them). In game it is compiled without optimisation (a
    // fraction of the time) and the optimised bytecode is compiled for the cache on a background
    // thread, so the next launch's warm-up loads that. TTT2_SHADER_FAST=0: optimise in game too.
    template <class Create>
    BuildResult CreateFromHlsl(const std::string& hlsl, bool vertex, bool inGame, BuildCounts& n, std::string& why,
                               Create create) const
    {
        const uint64_t textKey = Fnv(hlsl.data(), hlsl.size());
        std::vector<char> blob;
        std::filesystem::path cached;
        if (!shaderCacheDir_.empty())
        {
            char name[64];
            std::snprintf(name, sizeof(name), "%s_%016llX.dxbc", vertex ? "vg" : "ps", (unsigned long long)textKey);
            cached = shaderCacheDir_ / name;
            std::ifstream f(cached, std::ios::binary);
            if (f) blob.assign(std::istreambuf_iterator<char>(f), {});
        }
        if (!blob.empty() && SUCCEEDED(create(blob.data(), blob.size()))) { ++n.diskHits; return BuildResult::Ready; }
        static const bool fastInGame = [] { const char* e = std::getenv("TTT2_SHADER_FAST"); return !e || std::strcmp(e, "0") != 0; }();
        // Vertex shaders always compile optimised: they are small (milliseconds either way), and an
        // unoptimised compile crashed D3DCompile itself (MEASURED: a vertex fetch into r0 inside the
        // program's do / while (false), d3dcompiler_47 +77EBC reading address -1; optimised: fine).
        const bool fast = inGame && fastInGame && !vertex;
        const char* target = vertex ? "vs_5_0" : "ps_5_0";
        const char* label = vertex ? "ttt2 translated vs" : "ttt2 translated ps";
        try
        {
            const auto start = std::chrono::steady_clock::now();
            if (const char* trace = std::getenv("TTT2_SHADER_TRACE"))
            {
                char name[64];
                std::snprintf(name, sizeof(name), "/%s_%016llX_%s.hlsl", vertex ? "vg" : "ps", (unsigned long long)textKey, fast ? "fast" : "opt");
                if (FILE* f = std::fopen((std::string(trace) + name).c_str(), "wb")) { std::fwrite(hlsl.data(), 1, hlsl.size(), f); std::fclose(f); }
                std::fprintf(stderr, "[gpu-hw] compiling %s\n", name + 1);
            }
            auto code5 = Compile(hlsl, target, label, fast ? D3DCOMPILE_SKIP_OPTIMIZATION : D3DCOMPILE_OPTIMIZATION_LEVEL1);
            Check(create(code5->GetBufferPointer(), code5->GetBufferSize()), vertex ? "CreateVertexShader" : "CreatePixelShader");
            ++n.compiles;
            if (inGame)
                std::fprintf(stderr, "[gpu-hw] %s shader compiled in game in %.0f ms (%s)\n", vertex ? "vertex" : "pixel",
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                             fast ? "unoptimised; optimised copy cached in the background" : "optimised");
            if (cached.empty()) return BuildResult::Ready;
            if (!fast) WriteCacheFile(cached, code5->GetBufferPointer(), code5->GetBufferSize());
            else
                std::thread([hlsl, cached, target, label] {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    try
                    {
                        auto optimised = Compile(hlsl, target, label);
                        WriteCacheFile(cached, optimised->GetBufferPointer(), optimised->GetBufferSize());
                    }
                    catch (...) {}
                }).detach();
            return BuildResult::Ready;
        }
        catch (const Failure&) { throw; }
        catch (const std::exception& ex) { why = ex.what(); return BuildResult::CompileFailed; }
    }

    // Write through a per-thread temporary and rename, so a reader never sees half a file.
    static void WriteCacheFile(const std::filesystem::path& path, const void* data, size_t size)
    {
        std::filesystem::path temp = path;
        temp += ".tmp" + std::to_string(GetCurrentThreadId());
        {
            std::ofstream f(temp, std::ios::binary);
            f.write(static_cast<const char*>(data), (std::streamsize)size);
            if (!f) return;
        }
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        if (ec) std::filesystem::remove(temp, ec);
    }

    void RecordMicrocode(const char* kind, uint64_t key, const std::vector<uint32_t>& code)
    {
        if (shaderCacheDir_.empty()) return;
        char name[64];
        std::snprintf(name, sizeof(name), "%s_%016llX.bin", kind, (unsigned long long)key);
        const std::filesystem::path dir = shaderCacheDir_ / "ucode", path = dir / name;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) return;
        std::filesystem::create_directories(dir, ec);
        WriteCacheFile(path, code.data(), code.size() * 4);
    }

    template <class Entry>
    bool TakePrepared(std::unordered_map<uint64_t, Entry>& prepared, uint64_t key, Entry& out)
    {
        std::lock_guard lock(preparedMutex_);
        auto it = prepared.find(key);
        if (it == prepared.end()) return false;
        out = std::move(it->second);
        prepared.erase(it);
        return true;
    }

public:
    // --build-shaders (main.cpp): every shader in the game's own files built into the disk cache
    // ahead of time, so play never waits for D3DCompile (MEASURED in a real match on a cold cache: 267
    // builds, ~2,400 skipped draws, 28-36 fps in the worst 5 s). The player's copy is the only source:
    // nothing of the game ships with the port. Xbox 360 compiled-shader containers (magic 102A11xx;
    // bit 0 set = vertex) hold the microcode at container + h[1] + w0, w1 bytes (w0 / w1 = the two words
    // at container + h[6]; big-endian). MEASURED: 5,722 distinct pixel shaders in data002.bin,
    // data003.bin and the game image - 1,100 of the 1,125 seen in play (the others are slices of a
    // container's microcode; the game builds and records those itself, as before). Vertex shaders
    // (557 distinct) build too: the game's D3D patches only their fetch parameters, which the
    // translation leaves to the draw (VsFetchDescriptors), so a file's shader serves every copy.
    // Writes the same files the game does (shadercache/ps_<HLSL key>.dxbc and vg_<HLSL key>.dxbc,
    // compiled optimised, and ucode/ps_<key>.bin, which the warm-up builds at every boot); work
    // already there is skipped.
    // Prints "[shaders] <done>/<total>" lines (the installer's progress). Returns 0 when done.
    // The prebuild's stamp: this exe's link time (every build's translator may differ). A completed
    // prebuild writes it to shadercache/prebuilt.txt; an installed game compares at boot (main.cpp).
    static std::string ShaderBuildStamp()
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
        char s[32];
        std::snprintf(s, sizeof(s), "ps+vs-%08lX", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
        return s;
    }
    static bool PrebuildCurrent()
    {
        const std::filesystem::path dir = ShaderCacheDirectory();
        if (dir.empty()) return true;                   // nowhere to build into: nothing to do
        std::ifstream f(dir / "prebuilt.txt");
        std::string stamp;
        return f && std::getline(f, stamp) && stamp == ShaderBuildStamp();
    }

    // TEST ONLY (p_diag28): --warm-driver [ps|vs|all] - every compiled shader of the disk cache drawn once (a 64x64
    // 8_8_8_8 target with the game's depth format; pixel shaders behind a fixed vertex shader, vertex shaders in front
    // of a fixed pixel shader, the translated shaders' own interface), flushed every 64 draws. Prints the time and the
    // slowest flushes: the driver's compile work shows there.
    static int WarmDriverShaders(const char* mode)
    {
        const std::filesystem::path dir = ShaderCacheDirectory();
        if (dir.empty()) { std::fprintf(stderr, "[warm] no cache directory\n"); return 2; }
        const bool doPs = std::strcmp(mode, "vs") != 0, doVs = std::strcmp(mode, "ps") != 0;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL actual{};
        const ComPtr<IDXGIAdapter1> preferred = PreferredGpuAdapter();
        if (FAILED(D3D11CreateDevice(preferred.Get(), preferred ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &device, &actual, &context)))
        {
            std::fprintf(stderr, "[warm] no D3D11 device\n");
            return 3;
        }
        D3D11_TEXTURE2D_DESC t{};
        t.Width = t.Height = 64; t.MipLevels = t.ArraySize = 1; t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
        t.Format = DXGI_FORMAT_R8G8B8A8_UNORM; t.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> color, depth;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11DepthStencilView> dsv;
        Check(device->CreateTexture2D(&t, nullptr, &color), "WarmColor");
        Check(device->CreateRenderTargetView(color.Get(), nullptr, &rtv), "WarmRtv");
        t.Format = DXGI_FORMAT_R32G8X24_TYPELESS; t.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        Check(device->CreateTexture2D(&t, nullptr, &depth), "WarmDepth");
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        Check(device->CreateDepthStencilView(depth.Get(), &dv, &dsv), "WarmDsv");
        const float clear[4] = { 0, 0, 0, 0 };
        context->ClearRenderTargetView(rtv.Get(), clear);
        context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
        const D3D11_VIEWPORT vp{ 0, 0, 64, 64, 0, 1 };
        context->RSSetViewports(1, &vp);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const std::string vsText = R"(struct VSOut { float4 pos : SV_Position; float4 a[16] : TEXCOORD0; };
VSOut main(uint id : SV_VertexID)
{
    VSOut o;
    o.pos = float4(id == 1 ? 0.5 : -0.5, id == 2 ? -0.5 : 0.5, 0.5, 1);
    [unroll] for (int i = 0; i < 16; ++i) o.a[i] = float4(0.25, 0.25, 0.25, 1);
    return o;
})";
        const std::string psText = R"(struct PSIn { float4 pos : SV_Position; float4 a[16] : TEXCOORD0; };
float4 main(PSIn input) : SV_Target
{
    float4 s = 0;
    [unroll] for (int i = 0; i < 16; ++i) s += input.a[i];
    return s;
})";
        ComPtr<ID3D11VertexShader> warmVs;
        ComPtr<ID3D11PixelShader> warmPs;
        {
            auto v = Compile(vsText, "vs_5_0", "ttt2 warm vs");
            auto p = Compile(psText, "ps_5_0", "ttt2 warm ps");
            Check(device->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &warmVs), "WarmVs");
            Check(device->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &warmPs), "WarmPs");
        }
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto& file : std::filesystem::directory_iterator(dir, ec))
        {
            // Wide: filename().string() throws at a name outside the ANSI code page (log_utf8.h).
            const std::wstring name = file.path().filename().wstring();
            if (file.path().extension() != ".dxbc") continue;
            if ((doPs && name.rfind(L"ps_", 0) == 0) || (doVs && name.rfind(L"vg_", 0) == 0)) files.push_back(file.path());
        }
        LARGE_INTEGER f, t0, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        const double perMs = double(f.QuadPart) / 1000.0;
        double flushMs = 0, slowest[5] = {};
        unsigned drawn = 0, failed = 0;
        auto flush = [&] {
            QueryPerformanceCounter(&a);
            context->Flush();
            QueryPerformanceCounter(&b);
            double ms = double(b.QuadPart - a.QuadPart) / perMs;
            flushMs += ms;
            for (double& s : slowest)          // kept in descending order
                if (ms > s) std::swap(s, ms);
        };
        for (size_t i = 0; i < files.size(); ++i)
        {
            std::ifstream in(files[i], std::ios::binary);
            const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
            const bool pixel = files[i].filename().wstring().rfind(L"ps_", 0) == 0;
            ComPtr<ID3D11PixelShader> ps;
            ComPtr<ID3D11VertexShader> vs;
            if (bytes.empty() ||
                FAILED(pixel ? device->CreatePixelShader(bytes.data(), bytes.size(), nullptr, &ps)
                             : device->CreateVertexShader(bytes.data(), bytes.size(), nullptr, &vs))) { ++failed; continue; }
            context->VSSetShader(pixel ? warmVs.Get() : vs.Get(), nullptr, 0);
            context->PSSetShader(pixel ? ps.Get() : warmPs.Get(), nullptr, 0);
            context->Draw(3, 0);
            ++drawn;
            if (drawn % 64 == 0) flush();
            if (drawn % 250 == 0)          // p_diag42: the installer's progress bar
            {
                if (device->GetDeviceRemovedReason() != S_OK)      // p_diag43: driver reset - stop, the game warms at first use
                {
                    std::fprintf(stderr, "[warm] the graphics device was lost after %u shaders (0x%08X)\n", drawn,
                                 unsigned(device->GetDeviceRemovedReason()));
                    return 4;
                }
                QueryPerformanceCounter(&b);
                std::fprintf(stderr, "[warm] %u / %zu drawn, %.0f ms\n", drawn, files.size(), double(b.QuadPart - t0.QuadPart) / perMs);
            }
        }
        flush();
        // Wait for the GPU: everything compiled and run.
        D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
        ComPtr<ID3D11Query> done;
        if (SUCCEEDED(device->CreateQuery(&qd, &done)))
        {
            context->End(done.Get());
            context->Flush();
            BOOL finished = FALSE;
            const ULONGLONG until = GetTickCount64() + 60000;      // p_diag43: bounded (a lost device never finishes)
            for (;;)
            {
                const HRESULT hr = context->GetData(done.Get(), &finished, sizeof(finished), 0);
                if (hr == S_OK && finished) break;
                if (FAILED(hr) || GetTickCount64() > until)
                {
                    std::fprintf(stderr, "[warm] the GPU did not finish (0x%08X)\n", unsigned(hr));
                    return 4;
                }
                Sleep(1);
            }
        }
        QueryPerformanceCounter(&b);
        std::fprintf(stderr, "[warm] %s: %u shaders drawn (%u failed) in %.0f ms; flushes %.0f ms, slowest %.1f %.1f %.1f %.1f %.1f ms\n",
                     mode, drawn, failed, double(b.QuadPart - t0.QuadPart) / perMs, flushMs, slowest[0], slowest[1], slowest[2],
                     slowest[3], slowest[4]);
        return 0;
    }

    static int PrebuildShaders(const std::vector<std::filesystem::path>& files, unsigned threads)
    {
        const std::filesystem::path dir = ShaderCacheDirectory();
        if (dir.empty()) { std::fprintf(stderr, "[shaders] no cache directory\n"); return 2; }
        std::error_code ec;
        std::filesystem::create_directories(dir / "ucode", ec);
        const auto start = std::chrono::steady_clock::now();
        auto be32 = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
        std::unordered_map<uint64_t, std::vector<uint32_t>> found, foundVs;
        size_t containers = 0;
        for (const auto& path : files)
        {
            const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (f == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "[shaders] cannot open %s\n", LogUtf8(path).c_str()); continue; }
            LARGE_INTEGER size{};
            GetFileSizeEx(f, &size);
            const HANDLE map = size.QuadPart > 64 ? CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
            const uint8_t* m = map ? static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0)) : nullptr;
            const size_t n = size_t(size.QuadPart);
            size_t here = 0;
            for (size_t s = 0; m && s + 32 <= n; s += 4)
            {
                if (m[s] != 0x10 || m[s + 1] != 0x2A || m[s + 2] != 0x11) continue;
                uint32_t h[8];
                for (int k = 0; k < 8; ++k) h[k] = be32(m + s + 4 * k);
                if (h[1] == 0 || h[1] > 0x100000 || h[6] == 0 || h[6] >= h[1] || s + h[6] + 8 > n) continue;
                const uint32_t w0 = be32(m + s + h[6]), w1 = be32(m + s + h[6] + 4);
                if (w1 == 0 || (w1 & 3) || (w0 & 3) || w0 > 0x10000 || w1 > 0x40000) continue;
                const size_t u0 = s + h[1] + w0;
                if (u0 + w1 > n || w0 + w1 > h[2]) continue;
                ++here;
                std::vector<uint32_t> code(w1 / 4);
                for (size_t k = 0; k < code.size(); ++k) code[k] = be32(m + u0 + 4 * k);
                // Bit 0 of the magic: a vertex shader (MEASURED: 102A1101 and 102A1121 hold vertex
                // shaders, 102A1100 pixel shaders).
                if (h[0] & 1) foundVs.emplace(VsKey(code), std::move(code));
                else found.emplace(PsKey(code), std::move(code));
            }
            if (m) UnmapViewOfFile(m);
            if (map) CloseHandle(map);
            CloseHandle(f);
            containers += here;
        }
        // Pixel shaders first (most of the work), then vertex shaders (key | 1 << 63 marks them here).
        std::vector<std::pair<uint64_t, std::vector<uint32_t>>> list(found.begin(), found.end());
        const size_t pixelCount = list.size();
        for (auto& [k, code] : foundVs) list.emplace_back(k, std::move(code));
        found.clear();
        foundVs.clear();
        std::fprintf(stdout, "[shaders] %zu containers, %zu distinct pixel and %zu vertex shaders\n", containers, pixelCount,
                     list.size() - pixelCount);
        std::fflush(stdout);
        std::atomic<size_t> next{ 0 }, done{ 0 }, compiled{ 0 }, untranslatable{ 0 }, failed{ 0 };
        auto worker = [&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;)
            {
                const size_t i = next++;
                if (i >= list.size()) return;
                const auto& [key, code] = list[i];
                try
                {
                    if (i >= pixelCount)
                    {
                        // Vertex: compiled per HLSL text (BuildVertexShader); the microcode is not recorded
                        // - the game's copies differ in their fetches and find the text's shader at boot.
                        const VsTranslation t = TranslateVertexShader(code);
                        if (!t.unsupported.empty()) { ++untranslatable; ++done; continue; }
                        const std::string hlsl = VertexShaderHlsl(t);
                        char name[64];
                        std::snprintf(name, sizeof(name), "vg_%016llX.dxbc", (unsigned long long)Fnv(hlsl.data(), hlsl.size()));
                        std::error_code e2;
                        if (!std::filesystem::exists(dir / name, e2))
                        {
                            auto blob = Compile(hlsl, "vs_5_0", "ttt2 translated vs");
                            WriteCacheFile(dir / name, blob->GetBufferPointer(), blob->GetBufferSize());
                            ++compiled;
                        }
                        ++done;
                        continue;
                    }
                    const PsTranslation t = TranslatePixelShader(code);
                    if (!t.unsupported.empty()) { ++untranslatable; ++done; continue; }
                    const std::string hlsl = PixelShaderHlsl(t);
                    char name[64];
                    std::snprintf(name, sizeof(name), "ps_%016llX.dxbc", (unsigned long long)Fnv(hlsl.data(), hlsl.size()));
                    const std::filesystem::path dxbc = dir / name;
                    std::error_code e2;
                    if (!std::filesystem::exists(dxbc, e2))
                    {
                        static const bool fastPs = [] { const char* e = std::getenv("TTT2_PREBUILD_FAST"); return e && *e == '1'; }();
                        auto blob = Compile(hlsl, "ps_5_0", "ttt2 translated ps", fastPs ? D3DCOMPILE_SKIP_OPTIMIZATION : D3DCOMPILE_OPTIMIZATION_LEVEL1);
                        WriteCacheFile(dxbc, blob->GetBufferPointer(), blob->GetBufferSize());
                        ++compiled;
                    }
                    std::snprintf(name, sizeof(name), "ps_%016llX.bin", (unsigned long long)key);
                    const std::filesystem::path ucode = dir / "ucode" / name;
                    if (!std::filesystem::exists(ucode, e2)) WriteCacheFile(ucode, code.data(), code.size() * 4);
                }
                catch (...) { ++failed; }
                ++done;
            }
        };
        if (!threads) threads = (std::max)(1u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u);
        std::vector<std::thread> pool;
        for (unsigned k = 0; k < threads; ++k) pool.emplace_back(worker);
        size_t shown = ~size_t(0);
        while (done.load() < list.size())
        {
            const size_t d = done.load();
            if (d != shown) { std::fprintf(stdout, "[shaders] %zu/%zu\n", d, list.size()); std::fflush(stdout); shown = d; }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        for (auto& t : pool) t.join();
        std::fprintf(stdout, "[shaders] %zu/%zu\n[shaders] done in %.1f s on %u threads: %zu compiled, %zu already cached, %zu not translatable, %zu failed\n",
                     list.size(), list.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), threads,
                     compiled.load(), list.size() - compiled.load() - untranslatable.load() - failed.load(), untranslatable.load(), failed.load());
        std::fflush(stdout);
        if (failed.load() || list.empty()) return 1;
        const std::string stamp = ShaderBuildStamp() + "\n" + std::to_string(pixelCount) + " pixel and " + std::to_string(list.size() - pixelCount) +
                                  " vertex shaders\n";
        WriteCacheFile(dir / "prebuilt.txt", stamp.data(), stamp.size());
        return 0;
    }
private:

    void StartShaderWarmup()
    {
        if (shaderCacheDir_.empty()) return;
        if (const char* e = std::getenv("TTT2_SHADER_WARM"); e && std::strcmp(e, "0") == 0) return;
        std::thread([this] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            const auto start = std::chrono::steady_clock::now();
            std::error_code ec;
            // Every compiled vertex shader (vg_<HLSL text key>.dxbc): run-time copies of one shader
            // share it (BuildVertexShader / SharedVertexShader).
            // The names are read wide: filename().string() throws at a name outside the ANSI code page (log_utf8.h),
            // which ended the game here (a detached thread) for a file the player put in the cache.
            unsigned shared = 0;
            if (gpuVsEnabled_)
                for (const auto& file : std::filesystem::directory_iterator(shaderCacheDir_, ec))
                {
                    const std::wstring name = file.path().filename().wstring();
                    if (name.size() != 24 || name.rfind(L"vg_", 0) != 0 || file.path().extension() != ".dxbc") continue;
                    const uint64_t textKey = std::wcstoull(name.substr(3, 16).c_str(), nullptr, 16);
                    std::ifstream f(file.path(), std::ios::binary);
                    const std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
                    ComPtr<ID3D11VertexShader> vs;
                    if (bytes.empty() || FAILED(device_->CreateVertexShader(bytes.data(), bytes.size(), nullptr, &vs))) continue;
                    std::lock_guard lock(vsTextMutex_);
                    vsByText_.emplace(textKey, vs);
                    ++shared;
                }
            // Every recorded microcode (ucode/{vs,ps}_<key>.bin, the prebuild's and the game's own),
            // built on a few threads (the bytecode comes from the disk cache).
            std::vector<std::filesystem::path> files;
            for (const auto& file : std::filesystem::directory_iterator(shaderCacheDir_ / "ucode", ec))
            {
                const std::wstring name = file.path().filename().wstring();
                const bool isVs = name.rfind(L"vs_", 0) == 0, isPs = name.rfind(L"ps_", 0) == 0;
                if ((!isVs && !isPs) || file.path().extension() != ".bin" || (isVs && !gpuVsEnabled_)) continue;
                files.push_back(file.path());
            }
            std::atomic<size_t> next{ 0 };
            std::atomic<unsigned> vs{ 0 }, ps{ 0 }, failed{ 0 };
            std::mutex countMutex;
            BuildCounts total;
            auto worker = [&] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                BuildCounts n;
                for (size_t i; (i = next++) < files.size();)
                {
                    const bool isVs = files[i].filename().wstring().rfind(L"vs_", 0) == 0;
                    std::ifstream f(files[i], std::ios::binary);
                    const std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
                    if (bytes.empty() || bytes.size() % 4) continue;
                    std::vector<uint32_t> code(bytes.size() / 4);
                    std::memcpy(code.data(), bytes.data(), bytes.size());
                    std::string why;
                    try
                    {
                        if (isVs)
                        {
                            VsEntry e;
                            if (BuildVertexShader(code, e, n, why) != BuildResult::Ready) { ++failed; continue; }
                            std::lock_guard lock(preparedMutex_);
                            preparedVs_.emplace(VsKey(code), std::move(e));
                            ++vs;
                        }
                        else
                        {
                            PsEntry e;
                            if (BuildPixelShader(code, e, n, why) != BuildResult::Ready) { ++failed; continue; }
                            std::lock_guard lock(preparedMutex_);
                            preparedPs_.emplace(PsKey(code), std::move(e));
                            ++ps;
                        }
                    }
                    catch (...) { ++failed; }
                }
                std::lock_guard l(countMutex);
                total.diskHits += n.diskHits;
                total.compiles += n.compiles;
            };
            const unsigned cores = std::thread::hardware_concurrency();
            const unsigned threads = cores >= 8 ? 4 : cores >= 4 ? 2 : 1;
            std::vector<std::thread> pool;
            for (unsigned k = 1; k < threads; ++k) pool.emplace_back(worker);
            worker();
            for (auto& t : pool) t.join();
            std::fprintf(stderr, "[gpu-hw] shader warm-up: %u compiled vertex shaders loaded, %u vertex + %u pixel shaders ready in %.0f ms on %u threads (%llu from the bytecode cache, %llu compiled, %u failed)\n",
                         shared, vs.load(), ps.load(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                         threads, (unsigned long long)total.diskHits, (unsigned long long)total.compiles, failed.load());
        }).detach();
    }

    // EDRAM colour formats (RB_COLOR_INFO.color_format) as D3D11 render targets. TTT2 renders
    // 8_8_8_8 (0) and, for its motion-blur velocity pass, 16_16_FLOAT (6): signed half floats
    // that an 8-bit unorm target clamped to [0, 1], which smeared every moving character.
    static DXGI_FORMAT ColorDxgiFormat(uint32_t format)
    {
        switch (format)
        {
        case 2: case 10:  return DXGI_FORMAT_R10G10B10A2_UNORM;      // 2_10_10_10
        case 4: case 6:   return DXGI_FORMAT_R16G16_FLOAT;           // 16_16 (+-32 fixed), 16_16_FLOAT
        case 3: case 5: case 7: case 12:
                          return DXGI_FORMAT_R16G16B16A16_FLOAT;     // 2_10_10_10_FLOAT, 16_16_16_16(_FLOAT)
        case 14:          return DXGI_FORMAT_R32_FLOAT;
        case 15:          return DXGI_FORMAT_R32G32_FLOAT;
        default:          return DXGI_FORMAT_B8G8R8A8_UNORM;         // 8_8_8_8, 8_8_8_8_GAMMA
        }
    }

    // `key` is the EDRAM base | colour format << 12: each format gets its own surface (a base
    // reused with another format is not reinterpreted bit for bit, as EDRAM would). The texture
    // is made by Grow, sized to the area actually used.
    ColorSurface& Color(uint32_t key)
    {
        auto [it, added] = colors_.try_emplace(key);
        if (added) it->second.format = ColorDxgiFormat((key >> 12) & 0xF);
        return it->second;
    }

    DepthSurface& Depth(uint32_t base) { return depths_[base]; }

    // ---- render scale. Surfaces and resolved textures hold scale_ x their logical (guest)
    // size; everything the guest sees stays logical. Px: one rounding for every size and edge.
    float scale_ = 1.0f;
    uint32_t scaleGeneration_ = ~0u;
    UINT Px(double logical) const { return Px(logical, scale_); }
    UINT AllocPx(double logical) const { return AllocPx(logical, scale_); }
    static UINT Px(double logical, float scale) { return UINT(std::floor(logical * scale + 0.5)); }
    static UINT AllocPx(double logical, float scale) { return std::clamp<UINT>(Px(logical, scale), 1, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION); }

    // ---- native-size surfaces. The fight intro bakes each fighter's textures: draws into a
    // 1024x1024 surface, a resolve, and the CPU reads it back and recompresses it. At render scale
    // 2 every bake drew 2048x2048 and the GPU took ~6 ms per bake (CPU waiting; MEASURED
    // 2026-10-15: copy latency median 5.6 ms at scale 2, 3.1 ms at scale 1, ~0.9 s per intro). Extra
    // resolution there buys nothing - the CPU reads the logical size - so such a bake surface
    // (UpdateNative) renders at scale 1, with its depth surface.
    // TTT2_NATIVE_READBACK=0 keeps them scaled.
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, bool>> resolveDests_;   // colour key -> dest -> CPU-read
    std::unordered_map<uint32_t, uint32_t> depthWithColor_;                          // colour key -> depth base last drawn with it
    std::unordered_map<uint32_t, uint32_t> nativeColors_, nativeDepths_;   // EDRAM base -> the bake's sample pitch
    static bool NativeReadbackOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_NATIVE_READBACK"); return !(e && *e == '0'); }();
        return on;
    }
    // Re-evaluate colour surface `key` after its resolves changed (a new destination, or one the
    // CPU read).
    void UpdateNative(uint32_t key)
    {
        if (!NativeReadbackOn() || key == ~0u) return;
        auto it = resolveDests_.find(key);
        // A texture bake: the CPU reads a square power-of-two resolve of it (1024x1024, 512x512), and
        // the surface is no bigger than that - never a scene-sized one (1280x720 and up), whose
        // resolves the GPU samples. Its other resolves are the bake's own intermediate layers
        // (MEASURED: 004C0 -> A751B000 read by the CPU, A7B6B000 sampled back into the bake).
        bool cpuOnly = false;
        uint32_t bakePitch = 0;
        const auto surface = colors_.find(key);
        if (it != resolveDests_.end() && surface != colors_.end() && surface->second.extW <= 1024 && surface->second.extH <= 1024)
            for (const auto& [dest, cpu] : it->second)
            {
                const auto r = resolved_.find(dest);
                if (cpu && r != resolved_.end() && r->second.pitch == r->second.height && r->second.pitch >= 256 &&
                    (r->second.pitch & (r->second.pitch - 1)) == 0)
                {
                    cpuOnly = true;
                    bakePitch = r->second.sourcePitch;
                }
            }
        const bool was = nativeColors_.count(key) != 0;
        if (cpuOnly == was) return;
        const auto d = depthWithColor_.find(key);
        if (cpuOnly)
        {
            const uint32_t pitch = bakePitch ? bakePitch : surface->second.samplePitch;
            nativeColors_[key] = pitch;
            if (d != depthWithColor_.end()) nativeDepths_[d->second] = pitch;
        }
        else
        {
            nativeColors_.erase(key);
            if (d != depthWithColor_.end()) nativeDepths_.erase(d->second);
        }
        static int logged = 0;
        if (logged++ < 32)
            std::fprintf(stderr, "[gpu-hw] colour surface %05X (depth %03X) now renders at %s (%s)\n", key,
                         d != depthWithColor_.end() ? d->second : 0xFFFu, cpuOnly ? "native size" : "the render scale",
                         cpuOnly ? "a texture bake the CPU reads back" : "no longer a texture bake");
    }
    static float& ScaleOverride() { static float s = 0; return s; }   // TTT2_IMAGE_TOGGLE tests
    static float RequestedScale()
    {
        if (ScaleOverride() > 0) return std::clamp(ScaleOverride(), 0.5f, 4.0f);
        if (const char* e = std::getenv("TTT2_RENDER_SCALE"); e && *e)
            return std::clamp(float(std::atof(e)), 0.5f, 4.0f);
        const display_settings::Resolution r = display_settings::GetResolution();
        // ANTI-ALIASING SSAA (diag15): above the RESOLUTION setting, scaled down by the presenter's area filter.
        return std::clamp(std::min(float(r.width) / 1280.0f, float(r.height) / 720.0f) * display_settings::SupersampleFactor(),
                          0.5f, 4.0f);
    }
    // Follow the RESOLUTION setting (checked once per change of the settings generation).
    void UpdateScale()
    {
        const uint32_t g = display_settings::Generation();
        if (g == scaleGeneration_) return;
        scaleGeneration_ = g;
        const float s = RequestedScale();
        if (s != scale_) Rescale(s);
    }

    // Surface extents grow in steps of 128 x 64 samples (fewer re-creations), never past the
    // EDRAM store's SurfaceW x SurfaceH.
    static UINT Extent(UINT need, UINT have, UINT step, UINT limit)
    {
        need = std::min(std::max(need, 1u), limit);
        return need <= have ? have : std::min((need + step - 1) / step * step, limit);
    }
    void MakeColorTexture(ColorSurface& s, float scale)
    {
        s.scale = scale;
        D3D11_TEXTURE2D_DESC t{};
        t.Width = AllocPx(s.extW, scale); t.Height = AllocPx(s.extH, scale); t.MipLevels = t.ArraySize = 1;
        t.Format = s.format; t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        s.tex.Reset(); s.rtv.Reset(); s.srv.Reset();
        Check(device_->CreateTexture2D(&t, nullptr, &s.tex), "CreateColorSurface");
        Check(device_->CreateRenderTargetView(s.tex.Get(), nullptr, &s.rtv), "CreateColorRtv");
        Check(device_->CreateShaderResourceView(s.tex.Get(), nullptr, &s.srv), "CreateColorSrv");
        const float zero[4] = { 0, 0, 0, 0 };          // the CPU store starts zeroed
        context_->ClearRenderTargetView(s.rtv.Get(), zero);
    }
    void MakeDepthTexture(DepthSurface& s, float scale)
    {
        s.scale = scale;
        D3D11_TEXTURE2D_DESC t{};
        t.Width = AllocPx(s.extW, scale); t.Height = AllocPx(s.extH, scale); t.MipLevels = t.ArraySize = 1;
        // Typeless so depth resolves can read it as R32_FLOAT (shader copy: D3D11 cannot
        // CopySubresourceRegion a sub-rectangle of a depth-stencil resource). 32-bit float depth
        // and the 8-bit stencil the console's D24FS8 / D24S8 has (TTT2's fight-end depth of field
        // marks the characters in stencil; without it the stage blurred into a flat colour).
        t.Format = DXGI_FORMAT_R32G8X24_TYPELESS; t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        s.tex.Reset(); s.dsv.Reset(); s.srv.Reset();
        Check(device_->CreateTexture2D(&t, nullptr, &s.tex), "CreateDepthSurface");
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        Check(device_->CreateDepthStencilView(s.tex.Get(), &dv, &s.dsv), "CreateDsv");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        Check(device_->CreateShaderResourceView(s.tex.Get(), &sv, &s.srv), "CreateDepthSrv");
        context_->ClearDepthStencilView(s.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);   // CPU store starts at 1.0
    }
    void LogGrowth(const char* kind, uint32_t key, UINT w, UINT h, float scale)
    {
        static int logged = 0;
        if (logged++ < 64)
            std::fprintf(stderr, "[gpu-hw] %s surface %05X covers %ux%u samples (%ux%u at scale %.2f)\n",
                         kind, key, w, h, AllocPx(w, scale), AllocPx(h, scale), scale);
    }
    // Make the surface cover logical samples [0, w) x [0, h): created, or re-created larger with
    // its contents copied (the new area starts cleared, as the CPU store does).
    // ... at `scale` (a change of scale resamples the contents).
    void Grow(ColorSurface& s, uint32_t key, UINT w, UINT h, float scale)
    {
        w = Extent(w, s.extW, 128, SurfaceW); h = Extent(h, s.extH, 64, SurfaceH);
        if (s.tex && w == s.extW && h == s.extH && s.scale == scale) return;
        ComPtr<ID3D11Texture2D> old = s.tex;
        ComPtr<ID3D11ShaderResourceView> oldSrv = s.srv;
        const float oldScale = s.scale;
        const UINT oldW = AllocPx(s.extW, oldScale), oldH = AllocPx(s.extH, oldScale);
        const UINT oldExtW = s.extW, oldExtH = s.extH;
        s.extW = w; s.extH = h;
        MakeColorTexture(s, scale);
        if (old && oldScale == scale)
        {
            D3D11_BOX box{ 0, 0, 0, oldW, oldH, 1 };
            context_->CopySubresourceRegion(s.tex.Get(), 0, 0, 0, 0, old.Get(), 0, &box);
        }
        else if (old)
            Resample(oldSrv.Get(), s.rtv.Get(), nullptr, AllocPx(oldExtW, scale), AllocPx(oldExtH, scale), 1.0f / float(AllocPx(oldExtW, scale)),
                     1.0f / float(AllocPx(oldExtH, scale)), Filterable(s.format));
        LogGrowth("colour", key, w, h, scale);
    }
    void Grow(DepthSurface& s, uint32_t key, UINT w, UINT h, float scale)
    {
        w = Extent(w, s.extW, 128, SurfaceW); h = Extent(h, s.extH, 64, SurfaceH);
        if (s.tex && w == s.extW && h == s.extH && s.scale == scale) return;
        ComPtr<ID3D11ShaderResourceView> old = s.srv;
        const UINT oldW = AllocPx(s.extW, s.scale), oldH = AllocPx(s.extH, s.scale);
        const UINT oldExtW = s.extW, oldExtH = s.extH;
        const float oldScale = s.scale;
        s.extW = w; s.extH = h;
        MakeDepthTexture(s, scale);
        if (old && oldScale == scale) Resample(old.Get(), nullptr, s.dsv.Get(), oldW, oldH, 1.0f / float(oldW), 1.0f / float(oldH), false);
        else if (old)
            Resample(old.Get(), nullptr, s.dsv.Get(), AllocPx(oldExtW, scale), AllocPx(oldExtH, scale), 1.0f / float(AllocPx(oldExtW, scale)),
                     1.0f / float(AllocPx(oldExtH, scale)), false);
        LogGrowth("depth", key, w, h, scale);
    }

    // Draw into `dst` (an RTV, or a DSV through SV_Depth) over [0, w) x [0, h), reading `src`
    // at uv = pixel centre * k: copies, rescales, and logical-size reads for the CPU.
    ComPtr<ID3D11PixelShader> resamplePs_, resampleDepthPs_;
    ComPtr<ID3D11Buffer> resampleCb_;
    ComPtr<ID3D11DepthStencilState> depthAlways_;
    void Resample(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv,
                  UINT w, UINT h, float kx, float ky, bool linear)
    {
        pipe_.valid = false;
        if (!resamplePs_)
        {
            BlitVs();
            const char* common = "cbuffer K : register(b0) { float4 k; }; SamplerState smp : register(s0);\n";
            auto ps = Compile(std::string(common) + "Texture2D<float4> src : register(t0);"
                              " float4 main(float4 pos : SV_Position) : SV_Target { return src.SampleLevel(smp, pos.xy * k.xy, 0); }",
                              "ps_5_0", "ttt2 resample ps");
            Check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &resamplePs_), "CreateResamplePs");
            auto dps = Compile(std::string(common) + "Texture2D<float> src : register(t0);"
                               " float main(float4 pos : SV_Position) : SV_Depth { return src.SampleLevel(smp, pos.xy * k.xy, 0); }",
                               "ps_5_0", "ttt2 resample depth ps");
            Check(device_->CreatePixelShader(dps->GetBufferPointer(), dps->GetBufferSize(), nullptr, &resampleDepthPs_), "CreateResampleDepthPs");
            D3D11_BUFFER_DESC b{};
            b.ByteWidth = 16; b.Usage = D3D11_USAGE_DYNAMIC; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            Check(device_->CreateBuffer(&b, nullptr, &resampleCb_), "CreateResampleCb");
            D3D11_DEPTH_STENCIL_DESC d{};
            d.DepthEnable = TRUE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; d.DepthFunc = D3D11_COMPARISON_ALWAYS;
            Check(device_->CreateDepthStencilState(&d, &depthAlways_), "CreateDepthAlways");
        }
        const float k[4] = { kx, ky, 0, 0 };
        Upload(resampleCb_.Get(), k, sizeof(k));
        ID3D11ShaderResourceView* nulls[32] = {};
        context_->PSSetShaderResources(0, 32, nulls);              // the target may still be bound as input
        context_->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, dsv);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(dsv ? depthAlways_.Get() : nullptr, 0);
        D3D11_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(raster_.Get());
        D3D11_RECT sc{ 0, 0, LONG(w), LONG(h) };
        context_->RSSetScissorRects(1, &sc);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(blitVs_.Get(), nullptr, 0);
        context_->PSSetShader(rtv ? resamplePs_.Get() : resampleDepthPs_.Get(), nullptr, 0);
        ID3D11Buffer* cb = resampleCb_.Get();
        context_->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* smp = linear ? linearClamp_.Get() : pointClamp_.Get();
        context_->PSSetSamplers(0, 1, &smp);
        context_->PSSetShaderResources(0, 1, &src);
        context_->Draw(3, 0);
        context_->PSSetShaderResources(0, 1, nulls);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    // Formats that may not filter on every feature-level-11 device: resample with point reads.
    static bool Filterable(DXGI_FORMAT f) { return f != DXGI_FORMAT_R32_FLOAT && f != DXGI_FORMAT_R32G32_FLOAT; }

    // A new RESOLUTION: every live surface and resolved texture is resampled to the new scale,
    // so what the game rendered once (and samples later) survives the change.
    void Rescale(float s)
    {
        const float old = scale_;
        scale_ = s;
        for (auto& [key, c] : colors_)
        {
            const float k = nativeColors_.count(key) ? 1.0f : s;
            if (!c.tex || c.scale == k) continue;
            ComPtr<ID3D11ShaderResourceView> src = c.srv;
            MakeColorTexture(c, k);
            Resample(src.Get(), c.rtv.Get(), nullptr, AllocPx(c.extW, k), AllocPx(c.extH, k),
                     1.0f / float(AllocPx(c.extW, k)), 1.0f / float(AllocPx(c.extH, k)), Filterable(c.format));
        }
        for (auto& [key, d] : depths_)
        {
            const float k = nativeDepths_.count(key) ? 1.0f : s;
            if (!d.tex || d.scale == k) continue;
            ComPtr<ID3D11ShaderResourceView> src = d.srv;
            MakeDepthTexture(d, k);
            Resample(src.Get(), nullptr, d.dsv.Get(), AllocPx(d.extW, k), AllocPx(d.extH, k),
                     1.0f / float(AllocPx(d.extW, k)), 1.0f / float(AllocPx(d.extH, k)), false);
        }
        for (auto& [dest, r] : resolved_)
        {
            if (!r.tex || r.scale != old) continue;         // native-size resolves stay at 1
            ComPtr<ID3D11ShaderResourceView> src = r.srv;
            MakeResolvedTexture(r, s);
            Resample(src.Get(), r.rtv.Get(), nullptr, AllocPx(r.pitch, s), AllocPx(r.height, s),
                     1.0f / float(AllocPx(r.pitch, s)), 1.0f / float(AllocPx(r.height, s)), !r.depth);
        }
        lastFront_.Reset();
        resolvePool_.clear();                               // sized for the old scale
        std::fprintf(stderr, "[gpu-hw] render scale %.2f -> %.2f (%zu colour, %zu depth surfaces, %zu resolved textures resampled; %.0f MB)\n",
                     old, s, colors_.size(), depths_.size(), resolved_.size(), SurfaceMegabytes());
    }
    // Resolve textures a destination gave up when it changed shape, by allocation size and format,
    // for the next destination of that shape. MEASURED 2026-10-15 on Sakura Schoolyard: ~13
    // destinations a frame alternated between shapes (one scratch address resolved at several
    // sizes) with a CreateTexture2D each time - 630 kernel allocations a second, each zero-filled
    // on the copy engine (the System process at ~10% copy) - and presents across the whole desktop,
    // other programs' too, blocked 10-20 ms on a quarter of the frames.
    struct PooledResolve { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11RenderTargetView> rtv; };
    std::unordered_map<uint64_t, std::vector<PooledResolve>> resolvePool_;
    static uint64_t ResolvePoolKey(UINT w, UINT h, DXGI_FORMAT f) { return (uint64_t(w) << 40) | (uint64_t(h) << 16) | uint64_t(f); }
    void RetireResolvedTexture(Resolved& r)
    {
        ++texGen_;   // p_diag25: texture lookups' inputs changed
        static const bool on = [] { const char* e = std::getenv("TTT2_RESOLVE_POOL"); return !(e && *e == '0'); }();
        if (!on || !r.tex || !r.srv || !r.rtv || r.tex == lastFront_) return;     // the screenshot source keeps its content
        D3D11_TEXTURE2D_DESC d{};
        r.tex->GetDesc(&d);
        auto& v = resolvePool_[ResolvePoolKey(d.Width, d.Height, d.Format)];
        if (v.size() < 4) v.push_back({ r.tex, r.srv, r.rtv });
        FlashNote({ 'X', d.Width, d.Height, uint32_t(d.Format), uint32_t(v.size()), 0, 0, r.srv.Get(), nullptr });   // TEST ONLY (p_diag31)
    }
    void MakeResolvedTexture(Resolved& r, float scale)
    {
        ++texGen_;   // p_diag25: texture lookups' inputs changed
        D3D11_TEXTURE2D_DESC t{};
        t.Width = AllocPx(r.pitch, scale); t.Height = AllocPx(r.height, scale); t.MipLevels = t.ArraySize = 1;
        t.Format = r.depth ? DXGI_FORMAT_R32_FLOAT : r.rg16f ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        r.tex.Reset(); r.srv.Reset(); r.rtv.Reset();
        auto pooled = resolvePool_.find(ResolvePoolKey(t.Width, t.Height, t.Format));
        if (pooled != resolvePool_.end() && !pooled->second.empty())
        {
            PooledResolve& p = pooled->second.back();
            r.tex = std::move(p.tex); r.srv = std::move(p.srv); r.rtv = std::move(p.rtv);
            pooled->second.pop_back();
            ++stats_.resolvePoolHits;
            FlashNote({ 'M', 1, t.Width, t.Height, uint32_t(t.Format), 0, 0, r.srv.Get(), nullptr });   // TEST ONLY (p_diag31)
        }
        else
        {
            Check(device_->CreateTexture2D(&t, nullptr, &r.tex), "CreateResolveTexture");
            Check(device_->CreateShaderResourceView(r.tex.Get(), nullptr, &r.srv), "CreateResolveSrv");
            Check(device_->CreateRenderTargetView(r.tex.Get(), nullptr, &r.rtv), "CreateResolveRtv");
            ++stats_.resolveCreates;
            FlashNote({ 'M', 0, t.Width, t.Height, uint32_t(t.Format), 0, 0, r.srv.Get(), nullptr });   // TEST ONLY (p_diag31)
        }
        const float zero[4] = { 0, 0, 0, 0 };
        context_->ClearRenderTargetView(r.rtv.Get(), zero);
        r.scale = scale;
    }
    double SurfaceMegabytes() const
    {
        auto bpp = [](DXGI_FORMAT f) {
            return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32_FLOAT ? 8.0 : 4.0;
        };
        double bytes = 0;
        for (const auto& [k, c] : colors_) if (c.tex) bytes += double(AllocPx(c.extW, c.scale)) * AllocPx(c.extH, c.scale) * bpp(c.format);
        for (const auto& [k, d] : depths_) if (d.tex) bytes += double(AllocPx(d.extW, d.scale)) * AllocPx(d.extH, d.scale) * 8.0;
        for (const auto& [k, r] : resolved_) if (r.tex) bytes += double(AllocPx(r.pitch, r.scale)) * AllocPx(r.height, r.scale) * 4.0;
        return bytes / 1048576.0;
    }

    ID3D11BlendState* BlendState(const DrawState& st, float factor[4])
    {
        const uint32_t b = st.blend;
        const uint32_t srcF = b & 0x1F, op = (b >> 5) & 7, dstF = (b >> 8) & 0x1F;
        const uint32_t srcFA = (b >> 16) & 0x1F, opA = (b >> 21) & 7, dstFA = (b >> 24) & 0x1F;
        const bool passthrough = srcF == 1 && dstF == 0 && srcFA == 1 && dstFA == 0 && op == 0 && opA == 0;
        // Constant ALPHA as a colour-channel factor has no D3D11 equivalent unless the colour
        // channels do not also use the constant colour: then feed (a, a, a, a) as the factor.
        auto usesConstColor = [](uint32_t f) { return f == 12 || f == 13; };
        auto usesConstAlpha = [](uint32_t f) { return f == 14 || f == 15; };
        const bool alphaAsFactor = (usesConstAlpha(srcF) || usesConstAlpha(dstF)) &&
                                   !usesConstColor(srcF) && !usesConstColor(dstF);
        for (int i = 0; i < 4; ++i) factor[i] = alphaAsFactor ? st.blendConst[3] : st.blendConst[i];
        const uint64_t key = (uint64_t(passthrough ? 0 : b) << 8) | (st.writeMask & 15) | (alphaAsFactor ? 16 : 0);
        auto it = blendStates_.find(key);
        if (it != blendStates_.end()) return it->second.Get();
        auto map = [](uint32_t f, bool alpha) -> D3D11_BLEND {
            switch (f)
            {
            case 0: return D3D11_BLEND_ZERO;
            case 1: return D3D11_BLEND_ONE;
            case 4: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
            case 5: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
            case 6: return D3D11_BLEND_SRC_ALPHA;
            case 7: return D3D11_BLEND_INV_SRC_ALPHA;
            case 8: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
            case 9: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
            case 10: return D3D11_BLEND_DEST_ALPHA;
            case 11: return D3D11_BLEND_INV_DEST_ALPHA;
            case 12: case 14: return D3D11_BLEND_BLEND_FACTOR;
            case 13: case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
            case 16: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
            default: return D3D11_BLEND_ONE;               // CPU BlendFactor default
            }
        };
        auto mapOp = [](uint32_t o) -> D3D11_BLEND_OP {
            switch (o)
            {
            case 1: return D3D11_BLEND_OP_SUBTRACT;
            case 2: return D3D11_BLEND_OP_MIN;
            case 3: return D3D11_BLEND_OP_MAX;
            case 4: return D3D11_BLEND_OP_REV_SUBTRACT;
            default: return D3D11_BLEND_OP_ADD;
            }
        };
        D3D11_BLEND_DESC d{};
        auto& rt = d.RenderTarget[0];
        rt.BlendEnable = passthrough ? FALSE : TRUE;
        rt.SrcBlend = map(srcF, false); rt.DestBlend = map(dstF, false); rt.BlendOp = mapOp(op);
        rt.SrcBlendAlpha = map(srcFA, true); rt.DestBlendAlpha = map(dstFA, true); rt.BlendOpAlpha = mapOp(opA);
        rt.RenderTargetWriteMask = UINT8(st.writeMask & 15);
        ComPtr<ID3D11BlendState> state;
        Check(device_->CreateBlendState(&d, &state), "CreateBlendState");
        g_diagStateCreates.fetch_add(1, std::memory_order_relaxed);
        return (blendStates_[key] = state).Get();
    }

    // Viewport y is flipped twice (window y-down -> NDC y-up -> render target y-down), so the
    // winding D3D sees on the render target is the game's on-screen winding.
    ID3D11RasterizerState* RasterState(const DrawState& st)
    {
        const uint32_t key = (st.cull & 3) | (st.frontIsCW ? 4u : 0u) | (st.depthClip ? 8u : 0u);
        ComPtr<ID3D11RasterizerState>& state = rasterStates_[key];
        if (!state)
        {
            D3D11_RASTERIZER_DESC r{};
            r.FillMode = D3D11_FILL_SOLID;
            r.CullMode = (st.cull & 1) ? D3D11_CULL_FRONT : (st.cull & 2) ? D3D11_CULL_BACK : D3D11_CULL_NONE;
            r.FrontCounterClockwise = st.frontIsCW ? FALSE : TRUE;
            // Xenos clips 0 <= z <= w unless PA_CL_CLIP_CNTL.clip_disable; without it, triangles
            // reaching behind the camera plane rasterised as long slivers across the battle floor.
            r.DepthClipEnable = st.depthClip ? TRUE : FALSE;
            r.ScissorEnable = TRUE;
            Check(device_->CreateRasterizerState(&r, &state), "CreateRasterizerState");
            g_diagStateCreates.fetch_add(1, std::memory_order_relaxed);
        }
        return state.Get();
    }

    // The stencil reference/mask register for a draw: the back-face one only when the draw
    // can show nothing but back faces (front faces culled), as Xenia's D3D12 path picks it -
    // D3D11, like D3D12, has one reference and one pair of masks for both faces.
    uint32_t StencilRefMask(const DrawState& st) const
    {
        const bool backOnly = ((st.depthCtl >> 7) & 1) && (st.cull & 3) == 1;
        return backOnly ? st.stencilRefMaskBf : st.stencilRefMask;
    }
    UINT StencilRef(const DrawState& st) const { return st.stencil ? (StencilRefMask(st) & 0xFF) : 0; }

    // Xenos compare functions (never, less, equal, ...) and stencil ops (keep, zero, replace,
    // incr-clamp, decr-clamp, invert, incr-wrap, decr-wrap) are in D3D11's order.
    ID3D11DepthStencilState* DepthState(const DrawState& st)
    {
        uint64_t key = (st.zEnable ? 1u : 0u) | (st.zWrite ? 2u : 0u) | (st.zFunc << 2);
        if (st.stencil)
        {
            const uint32_t masks = (StencilRefMask(st) >> 8) & 0xFFFF;          // read mask | write mask << 8
            key |= (1ull << 5) | (uint64_t(st.depthCtl >> 7) << 6) | (uint64_t(masks) << 31);
        }
        auto it = depthStates_.find(key);
        if (it != depthStates_.end()) return it->second.Get();
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = st.zEnable ? TRUE : FALSE;
        d.DepthWriteMask = st.zWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = D3D11_COMPARISON_FUNC(D3D11_COMPARISON_NEVER + (st.zFunc & 7));
        if (st.stencil)
        {
            const uint32_t c = st.depthCtl, rm = StencilRefMask(st);
            d.StencilEnable = TRUE;
            d.StencilReadMask = UINT8((rm >> 8) & 0xFF);
            d.StencilWriteMask = UINT8((rm >> 16) & 0xFF);
            auto face = [](uint32_t bits) {
                D3D11_DEPTH_STENCILOP_DESC f{};
                f.StencilFunc = D3D11_COMPARISON_FUNC(D3D11_COMPARISON_NEVER + (bits & 7));
                f.StencilFailOp = D3D11_STENCIL_OP(D3D11_STENCIL_OP_KEEP + ((bits >> 3) & 7));
                f.StencilPassOp = D3D11_STENCIL_OP(D3D11_STENCIL_OP_KEEP + ((bits >> 6) & 7));
                f.StencilDepthFailOp = D3D11_STENCIL_OP(D3D11_STENCIL_OP_KEEP + ((bits >> 9) & 7));
                return f;
            };
            d.FrontFace = face(c >> 8);
            d.BackFace = ((c >> 7) & 1) ? face(c >> 20) : d.FrontFace;
        }
        ComPtr<ID3D11DepthStencilState> state;
        Check(device_->CreateDepthStencilState(&d, &state), "CreateDepthStencilState");
        g_diagStateCreates.fetch_add(1, std::memory_order_relaxed);
        return (depthStates_[key] = state).Get();
    }

    static uint32_t ResolveBytes(uint32_t pitch, uint32_t height)
    {
        return ((pitch + 31) & ~31u) * ((height + 31) & ~31u) * 4;
    }

    // Guest bytes covered by a texture fetch constant (conservative for tiled layouts).
    static uint32_t TextureBytes(const Texture& t)
    {
        uint64_t elemBytes, cols, rows;
        if (IsBlockCompressed(t.format))
        {
            elemBytes = (t.format == FMT_DXT1 || t.format == FMT_DXT3A || t.format == FMT_DXT5A || t.format == FMT_CTX1) ? 8 : 16;
            cols = (t.pitch + 3) / 4; rows = (t.height + 3) / 4;
        }
        else
        {
            elemBytes = std::max<uint32_t>(FormatBpp(t.format) / 8, 1);
            cols = t.pitch; rows = t.height;
        }
        if (t.tiled) { cols = (cols + 31) & ~31ull; rows = (rows + 31) & ~31ull; }
        return (uint32_t)std::min<uint64_t>(cols * rows * elemBytes, 64ull << 20);
    }

    // Mip levels uploaded with a 2D texture: down to the last whose longer side is 4 texels or more.
    // TextureMipLevel's layout is MEASURED against the data that far (TTT2_MIP_CENSUS, fights: each
    // level matches the 2x2 box of the one above, mean difference 0.003-0.03 against 0.07-0.4 for
    // unrelated texels); the 2x2 / 1x1 levels of 8-byte-block formats sit elsewhere, and a whole
    // texture under 4 texels on screen needs none. 1 (the base alone): no mips, the mip filter "base
    // map", a cube / array binding, or sides not powers of two (none seen with mips).
    // A/B tests: TTT2_MIPS=0 uploads the base level only; TTT2_MIP_TOGGLE=1 follows the file
    // C:/ttt2/port/mips.off at run time (checked every 30 presents; the texture cache is flushed).
    static std::atomic<bool>& MipsOffFlag() { static std::atomic<bool> f{ [] { const char* e = std::getenv("TTT2_MIPS"); return e && *e == '0'; }() }; return f; }
    static bool MipsOff() { return MipsOffFlag().load(std::memory_order_relaxed); }
    // [gpu-mem], every 5-s report: what the texture cache holds against what Windows says this process
    // uses of the graphics card's own memory and of system memory lent to it. A budget under the use means
    // Windows is moving this game's memory in and out, which costs frames while nothing else looks busy.
    void ReportVideoMemory()
    {
        if (!memoryAdapterAsked_)
        {
            memoryAdapterAsked_ = true;
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;
            if (device_ && SUCCEEDED(device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
                adapter.As(&memoryAdapter_);
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
        const bool known = memoryAdapter_
            && SUCCEEDED(memoryAdapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))
            && SUCCEEDED(memoryAdapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared));
        const auto mb = [](uint64_t bytes) { return double(bytes) / 1048576.0; };
        if (known)
            std::fprintf(stderr, "[gpu-mem] textures %zu (%.0f MB); card memory used %.0f of budget %.0f MB; "
                                 "system memory used %.0f of budget %.0f MB\n",
                         textures_.size(), mb(textureBytes_), mb(local.CurrentUsage), mb(local.Budget),
                         mb(shared.CurrentUsage), mb(shared.Budget));
        else
            std::fprintf(stderr, "[gpu-mem] textures %zu (%.0f MB)\n", textures_.size(), mb(textureBytes_));
    }
    // Between stages, an emptied cache when it has grown large. Nothing drops a texture whose guest memory
    // merely stopped being used, so a long Customize session piles up every item looked at: logs 54 and
    // 55 carried 2,100-3,100 textures into fights (every other session: 1,355 at most, stage changes and all)
    // and those were the fights a GT 730 ran worst. What the next stage uses simply uploads again, which is
    // what the first fight of every session does anyway. TTT2_DIAG_TEX_FLUSH_AT: another size, for tests.
    void FollowStageLoad()
    {
        const uint32_t loads = g_stageLoads.load(std::memory_order_relaxed);
        if (loads == stageLoadsSeen_) return;
        stageLoadsSeen_ = loads;
        static const size_t limit = [] {
            const char* e = std::getenv("TTT2_DIAG_TEX_FLUSH_AT");
            return e && *e ? size_t(std::strtoul(e, nullptr, 0)) : size_t(2000);
        }();
        if (textures_.size() <= limit) return;
        std::fprintf(stderr, "[gpu-hw] stage load: texture cache emptied (%zu textures, %.1f MB)\n",
                     textures_.size(), double(textureBytes_) / 1048576.0);
        ++texGen_;
        ++texFullGen_;
        textures_.clear();
        texBlocks_.clear();
        textureBytes_ = 0;
    }
    void FollowMipToggle()
    {
        static const bool image = [] { const char* e = std::getenv("TTT2_IMAGE_TOGGLE"); return e && *e == '1'; }();
        if (image && stats_.presents % 30 == 0)
            if (FILE* f = std::fopen("C:/ttt2/port/image.txt", "r"))
            {
                int s = 0, a = 0, t = 0;
                float sc = 0;
                const int got = std::fscanf(f, "%d %d %d %f", &s, &a, &t, &sc);
                if (got >= 3)
                {
                    display_settings::SetSharpness(s);
                    display_settings::SetAntiAliasing(a);
                    display_settings::SetTextureFilter(t);
                }
                std::fclose(f);
                if (got == 4 && sc != ScaleOverride())
                {
                    ScaleOverride() = sc;
                    const float want = RequestedScale();
                    if (want != scale_) Rescale(want);
                }
            }
        static const bool on = [] { const char* e = std::getenv("TTT2_MIP_TOGGLE"); return e && *e == '1'; }();
        if (!on || stats_.presents % 30 != 0) return;
        const bool off = GetFileAttributesA("C:/ttt2/port/mips.off") != INVALID_FILE_ATTRIBUTES;
        if (off == MipsOff()) return;
        MipsOffFlag().store(off);
        ++texGen_;   // p_diag25: texture lookups' inputs changed
        ++texFullGen_;                                   // p107
        textures_.clear();
        texBlocks_.clear();
        textureBytes_ = 0;
        std::fprintf(stderr, "[mip] mip levels %s (texture cache flushed)\n", off ? "off" : "on");
    }
    static uint32_t MipLevelsFor(const Texture& t, bool array)
    {
        if (MipsOff() || array || t.mipMax == 0 || t.mipFilter == 2) return 1;
        if ((t.width & (t.width - 1)) || (t.height & (t.height - 1))) return 1;
        uint32_t levels = 1;
        for (uint32_t l = 1; l <= t.mipMax && l < 14; ++l)
        {
            MipLevel m;
            if (std::max(t.width >> l, t.height >> l) < 4 || !TextureMipLevel(t, l, m)) break;
            levels = l + 1;
        }
        return levels;
    }
    // The guest bytes of levels 1+ outside the base's storage: the mip address up to the end of the
    // last uploaded level's storage (none when the levels are packed into the base's tile).
    static void MipRange(const Texture& t, uint32_t levels, uint32_t& start, uint32_t& size)
    {
        start = size = 0;
        MipLevel m;
        if (levels <= 1 || !TextureMipLevel(t, levels - 1, m) || m.base == t.base || m.base < t.mipBase) return;
        const uint32_t bw = IsBlockCompressed(t.format) ? 4 : 1;
        start = t.mipBase;
        size = uint32_t(std::min<uint64_t>(uint64_t(m.base - t.mipBase) + uint64_t(m.pitch / bw) * ((m.rows + bw - 1) / bw) * TextureBlockBytes(t.format), 64ull << 20));
    }

    void DropTexturesOverlapping(uint32_t start, uint32_t size)
    {
        ++texGen_;   // p_diag25: texture lookups' inputs changed
        const uint64_t diagT0 = __rdtsc();
        ++diagDropCalls_;
        // p107: no cached texture over these bytes - nothing to visit (the A/B's d14-old windows always look).
        const bool skip = TexBlocksOn() && !ixAbOff_ && !TexBlocksAny(start, size);
        if (skip) ++diagDropSkips_;
        if (skip && !VerifyOn()) { diagDropTicks_ += __rdtsc() - diagT0; return; }
        bool erased = false;
        for (auto it = textures_.begin(); it != textures_.end();)
        {
            const CachedTexture& c = it->second;
            const bool overlap = (uint64_t(c.rangeStart) < uint64_t(start) + size && uint64_t(start) < uint64_t(c.rangeStart) + c.rangeSize) ||
                                 (c.mipSize && uint64_t(c.mipStart) < uint64_t(start) + size && uint64_t(start) < uint64_t(c.mipStart) + c.mipSize);
            if (overlap && skip)
            {
                static int logged = 0;      // TEST ONLY (TTT2_DIAG_VERIFY): the block counts said none
                if (logged++ < 20) std::fprintf(stderr, "[verify] texture drop skipped over %08X+%X but texture %08X+%X overlaps\n",
                                                start, size, c.rangeStart, c.rangeSize);
            }
            if (!overlap || skip) { ++it; continue; }
            TexBlocksCount(c.rangeStart, c.rangeSize, c.mipStart, c.mipSize, -1);   // p107
            it = textures_.erase(it);
            erased = true;
        }
        if (erased) ++texFullGen_;                       // p107: memo entries may hold the dropped views
        diagDropTicks_ += __rdtsc() - diagT0;
    }

    // The frame log (framelog.trigger) notes where each bound texture came from.
    Bound BindTexture(const uint32_t* regs, uint32_t slot, bool array = false)
    {
        const Bound b = BindTextureFrom(regs, slot, array);
        if (g_gpuFrameNote)
        {
            const char* kind = "guest bytes";
            if (b.srv == defaultSrv_.Get() || b.srv == defaultArraySrv_.Get()) kind = "fallback";
            else for (const auto& [dest, r] : resolved_) if (r.srv.Get() == b.srv) { kind = r.depth ? "depth resolve" : "resolve"; break; }
            const Texture t = DecodeTexture(regs, slot);
            char line[160];
            std::snprintf(line, sizeof(line), "    hw t%u: base=%08X fmt=%u %ux%u -> %s\n", slot, t.base, t.format, t.width, t.height, kind);
            g_gpuFrameNote(line);
        }
        return b;
    }
    Bound BindTextureFrom(const uint32_t* regs, uint32_t slot, bool array)
    {
        Texture t = DecodeTexture(regs, slot);
        ID3D11ShaderResourceView* fallback = array ? defaultArraySrv_.Get() : defaultSrv_.Get();
        if (!t.valid || t.width > 8192 || t.height > 8192)
            return { fallback, 0x688, 0, 1, 1, 1.0f, 1.0f, 1, 1, 0 };
        // Depth textures (24_8 / 24_8_FLOAT) over a GPU depth resolve: the R32F copy, depth
        // replicated (mode 3). TTT2's shadow map and scene-depth passes read these.
        if (!array && (t.format == FMT_24_8 || t.format == FMT_24_8_FLOAT))
        {
            auto it = resolved_.find(t.base);
            if (it != resolved_.end() && it->second.valid && it->second.depth && t.tiled &&
                ((t.pitch + 31) & ~31u) == ((it->second.pitch + 31) & ~31u) &&
                t.width <= it->second.pitch && t.height <= it->second.height)
            {
                Resolved& r = it->second;
                dirty_.SyncOnce(t.base, r.size);
                if (dirty_.Newest(t.base, r.size) <= r.seq)
                {
                    ++stats_.depthHits;
                    if (CensusOn()) CensusTexture(t, CensusDepth);
                    return { r.srv.Get(), t.swizzle, 3u, t.width, t.height,
                             float(t.width) / float(r.pitch), float(t.height) / float(r.height) };
                }
                r.valid = false;
                ++texGen_;   // p_diag25: texture lookups' inputs changed
            }
        }
        // Formats the interpreter cannot decode sample as unswizzled (0, 0, 0, 1) on the CPU
        // (SampleTexel's early return) - e.g. the 24_8 depth formats without a GPU depth resolve.
        // Same answer, no guest bytes needed: never write back or upload for them.
        if (!FormatBpp(t.format) && !IsBlockCompressed(t.format))
        {
            ++undecodable_[t.format];
            if (CensusOn()) CensusTexture(t, CensusUndecodable);
            return { fallback, 0x688, 0, t.width, t.height, 1.0f, 1.0f, 1, 1, 0 };
        }
        const uint32_t slices = array && t.dimension == 3 ? 6u : 1u;
        const uint32_t bytes = slices > 1 ? std::min<uint32_t>(slices * TextureFaceStride(t), 384u << 20) : TextureBytes(t);
        const uint32_t levels = MipLevelsFor(t, array);
        uint32_t mipStart = 0, mipSize = 0;
        MipRange(t, levels, mipStart, mipSize);

        // GPU-resolved data at this address?
        bool resolveBytes = false;
        for (auto it = resolved_.begin(); it != resolved_.end(); ++it)
        {
            Resolved& r = it->second;
            if (!r.valid) continue;
            const uint32_t rs = it->first;
            if (!(uint64_t(t.base) < uint64_t(rs) + r.size && uint64_t(rs) < uint64_t(t.base) + bytes) &&
                !(mipSize && uint64_t(mipStart) < uint64_t(rs) + r.size && uint64_t(rs) < uint64_t(mipStart) + mipSize)) continue;
            dirty_.SyncOnce(rs, r.size);
            if (dirty_.Newest(rs, r.size) > r.seq) { r.valid = false; ++texGen_; FlashNote({ 'I', rs, r.size, r.seq, 0, 0, 0, r.srv.Get(), nullptr });  // p_diag25: texture lookups' inputs changed
                                                     continue; }   // the CPU overwrote it
            if (!array && t.base == rs && (r.rg16f ? t.format == FMT_16_16_FLOAT : t.format == FMT_8_8_8_8) && t.tiled &&
                ((t.pitch + 31) & ~31u) == ((r.pitch + 31) & ~31u) && t.width <= r.pitch && t.height <= r.height)
            {
                ++stats_.resolvedHits;
                if (CensusOn()) CensusTexture(t, CensusResolved);
                return { r.srv.Get(), t.swizzle, (r.swapRB && !r.rg16f) ? 2u : 0u, t.width, t.height,
                         float(t.width) / float(r.pitch), float(t.height) / float(r.height) };
            }
            // Any other view of resolved bytes reads guest memory: make it coherent once.
            resolveBytes = true;
            if (!r.cpuCoherent)
            {
                char cause[96];
                std::snprintf(cause, sizeof(cause), "fmt%u %s%ux%u p%u vs resolve %ux%u%s", t.format,
                              t.tiled ? "tiled " : "", t.width, t.height, t.pitch, r.pitch, r.height,
                              t.base == rs ? "" : " offset");
                ++writebackCauses_[cause];
                WriteBack(rs, r);
            }
        }

        const bool fast = t.format == FMT_8_8_8_8 || t.format == FMT_8 ||
                          t.format == FMT_DXT1 || t.format == FMT_DXT2_3 || t.format == FMT_DXT4_5;
        TexKey key{ t.base, t.format, t.width, t.height, t.pitch, t.tiled ? 1u : 0u, t.endian, fast ? 0xFFFFFFFFu : t.swizzle,
                    slices | (array ? 0x100u : 0u), levels > 1 ? t.mipBase : 0u, levels | (levels > 1 && t.packedMips ? 0x100u : 0u) };
        dirty_.SyncOnce(t.base, bytes);
        if (mipSize) dirty_.SyncOnce(mipStart, mipSize);
        GpuNoteMemory(5, t.base, bytes);
        if (mipSize) GpuNoteMemory(5, mipStart, mipSize);
        if (CensusOn()) CensusTexture(t, resolveBytes ? CensusResolveBytes : CensusUpload);
        if (MipCensusOn() && !array) MipCensus(t);
        auto it = textures_.find(key);
        if (it != textures_.end() && dirty_.Newest(t.base, bytes) <= it->second.seq &&
            (!mipSize || dirty_.Newest(mipStart, mipSize) <= it->second.seq))
        {
            const CachedTexture& c = it->second;
            return { c.srv.Get(), c.genericSwizzled ? 0x688u : t.swizzle, c.mode, t.width, t.height,
                     float(t.width) / float(c.allocW), float(t.height) / float(c.allocH), slices, c.levels,
                     uint8_t(resolveBytes ? 2 : 1), t.base, bytes, mipStart, mipSize };
        }
        const bool diagAgain = it != textures_.end();     // TEST ONLY (DiagTexUpload): the game rewrote a cached texture
        LARGE_INTEGER diagT0;
        QueryPerformanceCounter(&diagT0);
        if (textures_.size() > 4096 || textureBytes_ > TextureBudgetBytes)
        {
            std::fprintf(stderr, "[gpu-hw] texture cache flushed: %zu entries, %.1f MB\n",
                         textures_.size(), double(textureBytes_) / 1048576.0);
            FlashNote({ 'F', uint32_t(textures_.size()), 0, 0, 0, 0, 0, nullptr, nullptr });   // TEST ONLY (p_diag31)
            textures_.clear();
            textureBytes_ = 0;
            texBlocks_.clear();                          // p107
        }
        if (levels > 1 && MipVerifyOn()) MipVerify(t, levels);
        ++texGen_;   // p_diag25: texture lookups' inputs changed
        ++texFullGen_;                                   // p107
        const size_t texturesBefore = textures_.size();
        CachedTexture& c = textures_[key];
        const bool texAdded = textures_.size() != texturesBefore;
        if (PoisonOn())     // TEST ONLY (p_diag35)
        {
            PoisonScan("texture", t.base, uint32_t(bytes), (uint64_t(t.format) << 32) | (t.width | (t.height << 16)), t.pitch);
            if (mipSize) PoisonScan("texture-mips", mipStart, uint32_t(mipSize), (uint64_t(t.format) << 32) | (t.width | (t.height << 16)), t.pitch);
        }
        UploadTexture(t, c, slices, array, levels);
        FlashNote({ 'U', t.base, t.format, t.width | (t.height << 16), levels, slices, t.tiled ? 1u : 0u, c.srv.Get(), nullptr });   // TEST ONLY (p_diag31)
        DiagTexUpload(t, slices, levels, uint64_t(bytes), diagAgain, diagT0);
        c.rangeStart = t.base; c.rangeSize = bytes; c.mipStart = mipStart; c.mipSize = mipSize; c.seq = dirty_.Current();
        if (texAdded) TexBlocksCount(c.rangeStart, c.rangeSize, c.mipStart, c.mipSize, +1);   // p107
        return { c.srv.Get(), c.genericSwizzled ? 0x688u : t.swizzle, c.mode, t.width, t.height,
                 float(t.width) / float(c.allocW), float(t.height) / float(c.allocH), slices, c.levels,
                 uint8_t(resolveBytes ? 2 : 1), t.base, bytes, mipStart, mipSize };
    }

    // TEST ONLY (TTT2_DIAG_TEXLOG=1): one line per texture upload - new or a rewritten cached one, format, size, cost.
    static void DiagTexUpload(const Texture& t, uint32_t slices, uint32_t levels, uint64_t bytes, bool again, LARGE_INTEGER t0)
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_TEXLOG"); return e && *e == '1'; }();
        if (!on) return;
        static const double qpcPerMs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(f.QuadPart) / 1000.0; }();
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        const double unixMs = double(((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000ull) / 10000.0;
        std::fprintf(stderr, "[texlog] %.1f %s base %08X fmt %u %ux%u%s pitch %u slices %u levels %u bytes %llu: %.3f ms\n", unixMs,
                     again ? "rewritten" : "new", t.base, t.format, t.width, t.height, t.tiled ? " tiled" : "", t.pitch, slices, levels,
                     (unsigned long long)bytes, double(t1.QuadPart - t0.QuadPart) / qpcPerMs);
    }

    // Decode one 2D surface (a texture, or one cube face) into host layout and DXGI format.
    // ---- Tiled surfaces of 4-, 8- and 16-byte elements (8_8_8_8 texels, DXT blocks) into rows: GpuTiledOffset2D is, for
    // these sizes, a 32x32-element tile's own pattern (the table) plus the tile's place ((x >> 5) + (y >> 5) * tiles per
    // row) << 10. TTT2_FAST_UNTILE=0: the per-element GpuTiledOffset2D loop.
    static bool FastUntileOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_FAST_UNTILE"); return !(e && *e == '0'); }();
        return on;
    }
    static const uint32_t* TileTable(uint32_t logB)
    {
        static const auto tables = [] {
            std::array<std::array<uint32_t, 1024>, 3> t{};
            for (uint32_t l = 2; l <= 4; ++l)
                for (uint32_t y = 0; y < 32; ++y)
                    for (uint32_t x = 0; x < 32; ++x) t[l - 2][y * 32 + x] = GpuTiledOffset2D(x, y, 32, l);
            return t;
        }();
        return tables[logB - 2].data();
    }
    // Words per element (1, 2, 4) and the word transform fixed at compile time; addresses wrap at 32 bits as before.
    template <uint32_t Words, class Word>
    static void UntileRows(uint32_t base, uint32_t pitchElems, uint32_t originX, uint32_t originY, uint32_t w, uint32_t h,
                           uint8_t* out, uint32_t rowPitch, Word word)
    {
        constexpr uint32_t logB = Words == 1 ? 2 : Words == 2 ? 3 : 4;
        const uint32_t* table = TileTable(logB);
        const uint32_t tilesPerRow = ((pitchElems + 31) & ~31u) >> 5;
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint32_t sy = y + originY;
            const uint32_t rowBase = ((sy >> 5) * tilesPerRow) << 10;
            const uint32_t* row = table + (sy & 31) * 32;
            uint32_t* dst = reinterpret_cast<uint32_t*>(out + size_t(y) * rowPitch);
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t sx = x + originX;
                const uint32_t addr = base + ((rowBase + ((sx >> 5) << 10) + row[sx & 31]) << logB);
                for (uint32_t k = 0; k < Words; ++k)
                {
                    uint32_t v;
                    std::memcpy(&v, g_base + uint32_t(addr + k * 4), 4);
                    dst[k] = word(v);
                }
                dst += Words;
            }
        }
    }
    template <uint32_t Words>
    static void UntileEndian(uint32_t endian, uint32_t base, uint32_t pitchElems, uint32_t originX, uint32_t originY, uint32_t w,
                             uint32_t h, uint8_t* out, uint32_t rowPitch)
    {
        // TextureWord's endian modes.
        switch (endian)
        {
        case 1: UntileRows<Words>(base, pitchElems, originX, originY, w, h, out, rowPitch,
                                  [](uint32_t v) { return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8); }); break;
        case 2: UntileRows<Words>(base, pitchElems, originX, originY, w, h, out, rowPitch, [](uint32_t v) { return __builtin_bswap32(v); }); break;
        case 3: UntileRows<Words>(base, pitchElems, originX, originY, w, h, out, rowPitch, [](uint32_t v) { return (v << 16) | (v >> 16); }); break;
        default: UntileRows<Words>(base, pitchElems, originX, originY, w, h, out, rowPitch, [](uint32_t v) { return v; }); break;
        }
    }
    static bool UntileVerifyOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_VERIFY"); return e && *e == '1'; }();
        return on;
    }
    static void UntileVerified(const Texture& t, const std::vector<uint8_t>& fast, const std::vector<uint8_t>& old)
    {
        static uint64_t checked = 0, bad = 0;
        if (fast != old && bad++ < 20)
            std::fprintf(stderr, "[verify] tiled upload differs: base %08X fmt %u %ux%u pitch %u origin %u,%u endian %u\n", t.base,
                         t.format, t.width, t.height, t.pitch, t.originX, t.originY, t.endian);
        if (++checked % 100 == 0)
            std::fprintf(stderr, "[verify] %llu tiled uploads checked, %llu differ\n", (unsigned long long)checked, (unsigned long long)bad);
    }

    void DecodeForUpload(const Texture& t, CachedTexture& c, DXGI_FORMAT& format, UINT& allocW, UINT& allocH,
                         UINT& rowPitch, std::vector<uint8_t>& data)
    {
        allocW = t.width; allocH = t.height;
        c.mode = 0; c.genericSwizzled = false;
        if (t.format == FMT_8_8_8_8)
        {
            // R8G8B8A8: the guest word's low byte is the fetched x (the interpreter's UnpackFormat).
            format = DXGI_FORMAT_R8G8B8A8_UNORM; rowPitch = allocW * 4;
            data.resize(size_t(rowPitch) * allocH);
            const bool fastTiled = t.tiled && FastUntileOn();
            if (fastTiled)
            {
                UntileRows<1>(t.base, t.pitch, t.originX, t.originY, t.width, t.height, data.data(), rowPitch,
                              [](uint32_t v) { return __builtin_bswap32(v); });
                if (!UntileVerifyOn()) return;
            }
            std::vector<uint8_t> fastData;
            if (fastTiled) { fastData.swap(data); data.assign(fastData.size(), 0); }
            auto* px = reinterpret_cast<uint32_t*>(data.data());
            for (uint32_t y = 0; y < t.height; ++y)
                for (uint32_t x = 0; x < t.width; ++x)
                {
                    const uint32_t sx = x + t.originX, sy = y + t.originY;
                    const uint32_t ti = t.tiled ? GpuTiledOffset2D(sx, sy, t.pitch, 2) : (sy * t.pitch + sx);
                    px[size_t(y) * allocW + x] = GRead32(g_base, t.base + ti * 4);
                }
            if (fastTiled) { UntileVerified(t, fastData, data); data.swap(fastData); }
        }
        else if (t.format == FMT_8)
        {
            format = DXGI_FORMAT_R8_UNORM; rowPitch = allocW;
            c.mode = 1;
            data.resize(size_t(rowPitch) * allocH);
            for (uint32_t y = 0; y < t.height; ++y)
            {
                if (!t.tiled)
                    std::memcpy(&data[size_t(y) * allocW], g_base + t.base + size_t(y + t.originY) * t.pitch + t.originX, t.width);
                else
                    for (uint32_t x = 0; x < t.width; ++x)
                        data[size_t(y) * allocW + x] = GRead8(g_base, t.base + GpuTiledOffset2D(x + t.originX, y + t.originY, t.pitch, 0));
            }
        }
        else if (t.format == FMT_DXT1 || t.format == FMT_DXT2_3 || t.format == FMT_DXT4_5)
        {
            const uint32_t blockBytes = t.format == FMT_DXT1 ? 8 : 16, logBpb = blockBytes == 8 ? 3 : 4;
            format = t.format == FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM : t.format == FMT_DXT2_3 ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
            allocW = (t.width + 3) & ~3u; allocH = (t.height + 3) & ~3u;
            const uint32_t blocksX = allocW / 4, blocksY = allocH / 4, bw = (t.pitch + 3) / 4;
            rowPitch = blocksX * blockBytes;
            data.resize(size_t(rowPitch) * blocksY);
            const bool fastTiled = t.tiled && FastUntileOn();
            if (fastTiled)
            {
                if (blockBytes == 8) UntileEndian<2>(t.endian, t.base, bw, t.originX / 4, t.originY / 4, blocksX, blocksY, data.data(), rowPitch);
                else UntileEndian<4>(t.endian, t.base, bw, t.originX / 4, t.originY / 4, blocksX, blocksY, data.data(), rowPitch);
                if (!UntileVerifyOn()) return;
            }
            std::vector<uint8_t> fastData;
            if (fastTiled) { fastData.swap(data); data.assign(fastData.size(), 0); }
            for (uint32_t by = 0; by < blocksY; ++by)
                for (uint32_t bx = 0; bx < blocksX; ++bx)
                {
                    const uint32_t sx = bx + t.originX / 4, sy = by + t.originY / 4;
                    const uint32_t bi = t.tiled ? GpuTiledOffset2D(sx, sy, bw, logBpb) : (sy * bw + sx);
                    const uint32_t addr = t.base + bi * blockBytes;
                    auto* dst = reinterpret_cast<uint32_t*>(&data[size_t(by) * rowPitch + size_t(bx) * blockBytes]);
                    for (uint32_t k = 0; k < blockBytes / 4; ++k) dst[k] = TextureWord(addr + k * 4, t.endian);
                }
            if (fastTiled) { UntileVerified(t, fastData, data); data.swap(fastData); }
        }
        else
        {
            // Every other format: the interpreter's own sampler, swizzle included (exact parity).
            format = DXGI_FORMAT_R32G32B32A32_FLOAT; rowPitch = allocW * 16;
            c.genericSwizzled = true;
            data.resize(size_t(rowPitch) * allocH);
            auto* px = reinterpret_cast<float*>(data.data());
            Texture st = t;
            st.width += t.originX; st.height += t.originY;
            for (uint32_t y = 0; y < t.height; ++y)
                for (uint32_t x = 0; x < t.width; ++x)
                {
                    V4 v = SampleTexel(st, int(x + t.originX), int(y + t.originY));
                    std::memcpy(&px[(size_t(y) * allocW + x) * 4], v.c, 16);
                }
        }
    }

    // Upload a texture. `array` binds it as Texture2DArray (cube-fetch slots); a cube texture
    // then has 6 slices, its faces in D3D order at TextureFaceStride intervals.
    void UploadTexture(const Texture& t, CachedTexture& c, uint32_t slices, bool array, uint32_t levels = 1)
    {
        DiagScope dTu(diagTicks_[DpTextureUploads]);
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT allocW = 1, allocH = 1, rowPitch = 0;
        std::vector<std::vector<uint8_t>> faces(slices);
        const uint32_t stride = slices > 1 ? TextureFaceStride(t) : 0;
        for (uint32_t s = 0; s < slices; ++s)
        {
            Texture face = t;
            face.base += s * stride;
            DecodeForUpload(face, c, format, allocW, allocH, rowPitch, faces[s]);
        }
        size_t total = 0;
        for (const auto& f : faces) total += f.size();
        // Levels 1+ (2D textures only, MipLevelsFor): each decoded like the base from where
        // TextureMipLevel puts it; D3D's level sizes (allocW >> l) are the guest's (powers of two).
        if (slices != 1) levels = 1;
        std::vector<std::vector<uint8_t>> mips(levels - 1);
        std::vector<UINT> mipPitch(levels - 1);
        for (uint32_t l = 1; l < levels; ++l)
        {
            MipLevel m;
            TextureMipLevel(t, l, m);
            Texture lt = t;
            lt.base = m.base; lt.pitch = m.pitch; lt.width = m.width; lt.height = m.height; lt.originX = m.x; lt.originY = m.y;
            CachedTexture scratch;
            DXGI_FORMAT lf = format;
            UINT lw = 1, lh = 1;
            DecodeForUpload(lt, scratch, lf, lw, lh, mipPitch[l - 1], mips[l - 1]);
            total += mips[l - 1].size();
        }
        D3D11_TEXTURE2D_DESC d{};
        d.Width = allocW; d.Height = allocH; d.MipLevels = levels; d.ArraySize = slices; d.Format = format;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (!c.tex || c.allocW != allocW || c.allocH != allocH || c.slices != slices || c.levels != levels)
        {
            c.tex.Reset(); c.srv.Reset();
            std::vector<D3D11_SUBRESOURCE_DATA> init(size_t(slices) * levels);
            for (uint32_t s = 0; s < slices; ++s) init[size_t(s) * levels] = { faces[s].data(), rowPitch, 0 };
            for (uint32_t l = 1; l < levels; ++l) init[l] = { mips[l - 1].data(), mipPitch[l - 1], 0 };
            Check(device_->CreateTexture2D(&d, init.data(), &c.tex), "CreateTexture");
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = format;
            if (array)
            {
                v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                v.Texture2DArray.MipLevels = levels; v.Texture2DArray.ArraySize = slices;
            }
            else { v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = levels; }
            Check(device_->CreateShaderResourceView(c.tex.Get(), &v, &c.srv), "CreateTextureSrv");
            textureBytes_ += total;
        }
        else
        {
            for (uint32_t s = 0; s < slices; ++s)
                context_->UpdateSubresource(c.tex.Get(), D3D11CalcSubresource(0, s, levels), nullptr, faces[s].data(), rowPitch, 0);
            for (uint32_t l = 1; l < levels; ++l)
                context_->UpdateSubresource(c.tex.Get(), D3D11CalcSubresource(l, 0, levels), nullptr, mips[l - 1].data(), mipPitch[l - 1], 0);
        }
        c.allocW = allocW; c.allocH = allocH; c.slices = slices; c.levels = levels;
        ++stats_.uploads;
        stats_.uploadBytes += total;
        auto& f = uploadByFormat_[t.format | (t.tiled ? 0x100u : 0u) | (slices > 1 ? 0x200u : 0u)];
        ++f.first; f.second += total;
    }

    // Guest word for a resolved depth texel: 24-bit depth in the high bits, stencil 0. The
    // float format is Xenos 20e4 (Xenia's Float32To20e4, round to nearest even).
    static uint32_t DepthWord(uint32_t f32, bool depthFloat)
    {
        float d; std::memcpy(&d, &f32, 4);
        if (!depthFloat)
            return uint32_t(std::clamp(d, 0.0f, 1.0f) * 16777215.0f + 0.5f) << 8;
        if (int32_t(f32) <= 0) return 0;
        if (f32 >= 0x3FFFFFF8u) return 0xFFFFFFu << 8;
        if (f32 < 0x38800000u)
        {
            const uint32_t shift = std::min<uint32_t>(113u - (f32 >> 23), 24u);
            f32 = (0x800000u | (f32 & 0x7FFFFFu)) >> shift;
        }
        else f32 += 0xC8000000u;
        f32 += 3u + ((f32 >> 3) & 1u);
        return ((f32 >> 3) & 0xFFFFFFu) << 8;
    }

    // Write a resolved texture into guest memory exactly as the CPU resolve does (tiled,
    // big-endian words in the hardware's order: red in the low byte, or - copy_dest_swap -
    // the A8R8G8B8 word 0xAARRGGBB), then accept those bytes as the entry's own. MEASURED
    // 2026-10-12: the character texture bake (CPU recompressors reading its 1024x1024 swapped
    // resolve) drew Devil Jin's red flames blue with the two orders exchanged.
    // Start a resolve's copy to its staging texture at the logical size WriteBack reads (scaled
    // surfaces are resampled first, as ReadLogical does). The GPU runs it in order with the rest of
    // the frame and the flush hands it over now, so a later Map waits for this copy at most.
    void StageReadback(Resolved& r)
    {
        DiagStamp('C', 0, (r.pitch << 16) | r.height);
        LARGE_INTEGER stT[6];                         // TEST ONLY (p_diag26): start, resample, staging, copy, signal, flush
        QueryPerformanceCounter(&stT[0]);
        stT[1] = stT[2] = stT[3] = stT[4] = stT[5] = stT[0];
        bool stResampled = false, stCreated = false;
        D3D11_TEXTURE2D_DESC d{};
        r.tex->GetDesc(&d);
        ID3D11Texture2D* source = r.tex.Get();
        if (d.Width != r.pitch || d.Height != r.height)
        {
            if (!r.logical)
            {
                D3D11_TEXTURE2D_DESC t{};
                t.Width = r.pitch; t.Height = r.height; t.MipLevels = t.ArraySize = 1; t.Format = d.Format;
                t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_RENDER_TARGET;
                Check(device_->CreateTexture2D(&t, nullptr, &r.logical), "CreateReadbackLogical");
                Check(device_->CreateRenderTargetView(r.logical.Get(), nullptr, &r.logicalRtv), "CreateReadbackLogicalRtv");
            }
            Resample(r.srv.Get(), r.logicalRtv.Get(), nullptr, r.pitch, r.height, 1.0f / float(r.pitch), 1.0f / float(r.height),
                     !r.depth && Filterable(d.Format));
            source = r.logical.Get();
            stResampled = true;
        }
        QueryPerformanceCounter(&stT[1]);
        if (!r.staging)
        {
            stCreated = true;
            D3D11_TEXTURE2D_DESC s{};
            s.Width = r.pitch; s.Height = r.height; s.MipLevels = s.ArraySize = 1; s.Format = d.Format;
            s.SampleDesc.Count = 1; s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Check(device_->CreateTexture2D(&s, nullptr, &r.staging), "CreateReadbackStaging");
        }
        QueryPerformanceCounter(&stT[2]);
        D3D11_BOX box{ 0, 0, 0, r.pitch, r.height, 1 };
        context_->CopySubresourceRegion(r.staging.Get(), 0, 0, 0, 0, source, 0, &box);
        DiagStamp('c', source != r.tex.Get(), (r.pitch << 16) | r.height);
        if (DiagDrawGpu() && r.pitch == 1024 && r.height == 1024) diagCopyTs_ = diagTsAt_;
        QueryPerformanceCounter(&stT[3]);
        if (readbackFence_) context4_->Signal(readbackFence_.Get(), r.stagedFence = ++readbackFenceValue_);
        Probe("staged copy");
        QueryPerformanceCounter(&stT[4]);
        static const bool noFlush = [] { const char* e = std::getenv("TTT2_STAGE_NOFLUSH"); return e && *e == '1'; }();
        if (!(noFlush && r.pitch * r.height <= 64)) context_->Flush();
        QueryPerformanceCounter(&stT[5]);
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            const double perMs = double(f.QuadPart) / 1000.0, total = double(stT[5].QuadPart - stT[0].QuadPart) / perMs;
            static int logged = 0;
            if (total > 4.0 && logged++ < 300)
            {
                FILETIME ft;
                GetSystemTimePreciseAsFileTime(&ft);
                const double unixMs = double(((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000ull) / 10000.0;
                auto part = [&](int i) { return double(stT[i].QuadPart - stT[i - 1].QuadPart) / perMs; };
                std::fprintf(stderr, "[slow-stage] %.1f %ux%u: %.2f ms = resample %.2f%s, staging %.2f%s, copy %.2f, signal %.2f, flush %.2f%s\n",
                             unixMs, r.pitch, r.height, total, part(1), stResampled ? " (done)" : "", part(2), stCreated ? " (new)" : "",
                             part(3), part(4), part(5), noFlush && r.pitch * r.height <= 64 ? " (skipped)" : "");
            }
        }
        r.staged = true;
        r.stagedSeq = r.seq;
        r.stagedAt = std::chrono::steady_clock::now();
        ++stats_.stagedReadbacks;
    }

    // Small resolves staged for the CPU (the exposure readback): into guest memory as soon as the
    // GPU has done their copy - never waiting for it.
    std::unordered_set<uint32_t> smallPending_;
    void FinishSmallWritebacks()
    {
        for (auto it = smallPending_.begin(); it != smallPending_.end();)
        {
            auto f = resolved_.find(*it);
            if (f == resolved_.end() || !f->second.valid || !f->second.staged || f->second.stagedSeq != f->second.seq)
            {
                it = smallPending_.erase(it);
                continue;
            }
            Resolved& r = f->second;
            if (readbackFence_ && readbackFence_->GetCompletedValue() < r.stagedFence) { ++it; continue; }
            std::vector<uint32_t> px;
            if (!ReadStaging(r.staging.Get(), r.pitch, r.height, px, true)) { ++it; continue; }
            r.staged = false;
            Scatter(*it, r, px, 0);
            if (FlashMax()) FlashSmall('S', *it, px.data(), uint32_t(px.size()));   // TEST ONLY (p_diag32)
            it = smallPending_.erase(it);
        }
    }

    // A resolve back into guest memory now, waiting for the GPU (TTT2_GPU_HW_WRITEBACK=1, and the
    // small ones with TTT2_SMALL_WRITEBACK_SYNC=1).
    void WriteBack(uint32_t dest, Resolved& r)
    {
        FlashNote({ 'W', dest, r.pitch, r.height, r.seq, 0, 0, r.srv.Get(), nullptr });   // TEST ONLY (p_diag31)
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<uint32_t> px;
        if (r.staged && r.stagedSeq == r.seq) ReadStaging(r.staging.Get(), r.pitch, r.height, px, false);   // copied at resolve time
        else ReadLogical(r.tex.Get(), r.pitch, r.height, r.pitch, r.height, !r.depth, px);
        r.staged = false;
        Scatter(dest, r, px, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }

    // The logical-size pixels `px` of resolve `r` into guest memory (tiled, in the guest's byte and
    // channel order); the resolve is CPU-coherent from now on.
    void Scatter(uint32_t dest, Resolved& r, const std::vector<uint32_t>& px, double readMs)
    {
        ++texGen_;   // p_diag25: texture lookups' inputs changed (p110: resolves and guest bytes - no cached texture dropped)
        using clock = std::chrono::steady_clock;
        const auto t1 = clock::now();
        const uint32_t aligned = (r.pitch + 31) & ~31u;
        auto rows = [&](uint32_t y0, uint32_t y1) {
            for (uint32_t y = y0; y < y1; ++y)
                for (uint32_t x = 0; x < r.pitch; ++x)
                {
                    uint32_t v = px[size_t(y) * r.pitch + x];
                    if (r.depth) v = DepthWord(v, r.depthFloat);
                    else if (r.rg16f) {}       // R16G16_FLOAT texel = red half | green half << 16, as fetched
                    else if (!r.swapRB) v = (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
                    GWrite32(g_base, dest + GpuTiledOffset2D(x, y, aligned, 2) * 4, v);
                }
        };
        // Large ones (the fight intro's 1024x1024 character texture bakes, ~2 ms each with a game
        // thread waiting) in four bands at once: every texel has its own guest address.
        if (size_t(r.pitch) * r.height >= 256 * 1024 && r.height >= 64)
        {
            const uint32_t band = ((r.height + 3) / 4 + 31) & ~31u;    // 4 bands cover every row
            std::thread helpers[3];
            for (uint32_t i = 0; i < 3; ++i)
                helpers[i] = std::thread(rows, std::min(r.height, band * (i + 1)), std::min(r.height, band * (i + 2)));
            rows(0, std::min(r.height, band));
            for (auto& t : helpers) t.join();
        }
        else rows(0, r.height);
        dirty_.Sync(dest, r.size);
        r.seq = dirty_.Current();
        r.cpuCoherent = true;
        ++stats_.writebacks;
        static const bool timing = [] { const char* e = std::getenv("TTT2_FRAMETIME"); return !(e && *e == '0'); }();
        if (timing && r.pitch * r.height > 64)
        {
            static int logged = 0;
            const auto t2 = clock::now();
            if (logged++ < 200)
                std::fprintf(stderr, "[frame] writeback %08X %ux%u: read %.2f ms (map wait %.2f ms), scatter %.2f ms, copy queued %.2f ms before\n",
                             dest, r.pitch, r.height, readMs, lastMapWaitMs_, std::chrono::duration<double, std::milli>(t2 - t1).count(),
                             lastStagedAgeMs_);
        }
        if (CensusOn() && r.pitch * r.height <= 64)
        {
            // Diagnostics: small resolves the CPU reads back (exposure / luminance readbacks):
            // destination and the first guest words as the CPU now sees them.
            static std::map<uint32_t, uint32_t> seen;
            if (seen[dest]++ % 120 == 0)
                std::fprintf(stderr, "[census] cpu reads resolve %08X %ux%u swap=%d: %08X %08X %08X %08X (read #%u)\n",
                             dest, r.pitch, r.height, int(r.swapRB), GRead32(g_base, dest), GRead32(g_base, dest + 4),
                             GRead32(g_base, dest + 8), GRead32(g_base, dest + 12), seen[dest]);
        }
    }

    // A staged readback (StageReadback): map the staging texture, its copy already queued. With
    // `noWait`, false (nothing read) while the GPU has not finished the copy yet.
    bool ReadStaging(ID3D11Texture2D* staging, uint32_t width, uint32_t height, std::vector<uint32_t>& out, bool noWait)
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        const auto mapStart = std::chrono::steady_clock::now();
        DiagScope dRb(diagTicks_[DpReadback]);
        const HRESULT hr = context_->Map(staging, 0, D3D11_MAP_READ, noWait ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0, &m);
        dRb.Stop();
        if (noWait && hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
        Check(hr, "MapReadbackStaging");
        lastMapWaitMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mapStart).count();
        out.resize(size_t(width) * height);
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(&out[size_t(y) * width], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, width * 4);
        context_->Unmap(staging, 0);
        return true;
    }

    void ReadTexture(ID3D11Texture2D* tex, uint32_t width, uint32_t height, std::vector<uint32_t>& out)
    {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        d.Width = width; d.Height = height; d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        Check(device_->CreateTexture2D(&d, nullptr, &staging), "CreateStaging");
        D3D11_BOX box{ 0, 0, 0, width, height, 1 };
        context_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        const auto mapStart = std::chrono::steady_clock::now();
        { DiagScope dRb(diagTicks_[DpReadback]); Check(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "MapStaging"); }
        lastMapWaitMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mapStart).count();
        out.resize(size_t(width) * height);
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(&out[size_t(y) * width], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, width * 4);
        context_->Unmap(staging.Get(), 0);
    }

    // The first w x h pixels of `tex` - whose whole extent is logicalW x logicalH guest pixels -
    // at logical size, for CPU readers: a plain copy at scale 1; otherwise read on the GPU at
    // logical pixel centres (bilinear: a 2x2 box at 2x, the middle texel at 3x; point reads for
    // depth and R32 formats - always a texel inside the logical pixel).
    void ReadLogical(ID3D11Texture2D* tex, uint32_t logicalW, uint32_t logicalH, uint32_t w, uint32_t h,
                     bool linear, std::vector<uint32_t>& out)
    {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        if (d.Width == logicalW && d.Height == logicalH) { ReadTexture(tex, w, h, out); return; }
        D3D11_TEXTURE2D_DESC t{};
        t.Width = w; t.Height = h; t.MipLevels = t.ArraySize = 1; t.Format = d.Format;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> logical;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
        Check(device_->CreateTexture2D(&t, nullptr, &logical), "CreateLogicalRead");
        Check(device_->CreateRenderTargetView(logical.Get(), nullptr, &rtv), "CreateLogicalReadRtv");
        Check(device_->CreateShaderResourceView(tex, nullptr, &srv), "CreateLogicalReadSrv");
        Resample(srv.Get(), rtv.Get(), nullptr, w, h, 1.0f / float(logicalW), 1.0f / float(logicalH), linear && Filterable(d.Format));
        ReadTexture(logical.Get(), w, h, out);
    }

    // logicalW / logicalH: the guest size the returned texture holds (scaled or not).
    ID3D11Texture2D* FrontTexture(uint32_t fb, bool& storedSwap, uint32_t& logicalW, uint32_t& logicalH)
    {
        storedSwap = false;
        if (!fb) return nullptr;
        auto it = resolved_.find(fb);
        if (it != resolved_.end() && it->second.valid && it->second.pitch >= 1280 && it->second.height >= 720)
        {
            dirty_.Sync(fb, it->second.size);
            if (dirty_.Newest(fb, it->second.size) <= it->second.seq)
            {
                storedSwap = it->second.swapRB;
                logicalW = it->second.pitch; logicalH = it->second.height;
                return it->second.tex.Get();
            }
            it->second.valid = false;
            ++texGen_;   // p_diag25: texture lookups' inputs changed
        }
        logicalW = 1280; logicalH = 720;
        // Not GPU-resolved (or overwritten by the CPU): decode guest memory like the GDI path.
        // The B8G8R8A8 view reads the word as ARGB, red and blue exchanged against the fetch
        // order (low byte = x), the same as a copy_dest_swap resolve.
        storedSwap = true;
        if (!cpuFront_)
        {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = 1280; d.Height = 720; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            Check(device_->CreateTexture2D(&d, nullptr, &cpuFront_), "CreateCpuFront");
        }
        static std::vector<uint32_t> px(1280 * 720);
        for (uint32_t y = 0; y < 720; ++y)
            for (uint32_t x = 0; x < 1280; ++x)
                px[size_t(y) * 1280 + x] = GRead32(g_base, fb + GpuTiledOffset2D(x, y, 1280, 2) * 4);
        context_->UpdateSubresource(cpuFront_.Get(), 0, nullptr, px.data(), 1280 * 4, 0);
        return cpuFront_.Get();
    }

    static void WriteBmp(const char* path, const uint32_t* px, uint32_t w, uint32_t h)
    {
        FILE* f = std::fopen(path, "wb");
        if (!f) return;
        uint8_t hdr[54] = {};
        const uint32_t size = w * h * 4;
        hdr[0] = 'B'; hdr[1] = 'M';
        *reinterpret_cast<uint32_t*>(hdr + 2) = 54 + size;
        *reinterpret_cast<uint32_t*>(hdr + 10) = 54;
        *reinterpret_cast<uint32_t*>(hdr + 14) = 40;
        *reinterpret_cast<int32_t*>(hdr + 18) = (int32_t)w;
        *reinterpret_cast<int32_t*>(hdr + 22) = -(int32_t)h;
        *reinterpret_cast<uint16_t*>(hdr + 26) = 1;
        *reinterpret_cast<uint16_t*>(hdr + 28) = 32;
        *reinterpret_cast<uint32_t*>(hdr + 34) = size;
        std::fwrite(hdr, 1, sizeof(hdr), f);
        std::fwrite(px, 4, size_t(w) * h, f);
        std::fclose(f);
    }

    // ---- frame cost (the 5 s stats): GPU time from one present to the next (timestamp queries; read
    // three frames later without waiting - GPU work plus any gaps the GPU waited for commands, so an
    // upper bound of its busy time), CPU time per frame of the game's main thread and of this command-
    // processor thread (thread cycle counters), and the whole process's CPU use.
    struct GpuTimer { ComPtr<ID3D11Query> disjoint, begin, end; bool open = false, pending = false;
                      ComPtr<ID3D11Query> stats; double unixMs = 0; };   // TEST ONLY (p_diag22): workload per frame
    static bool DiagGpuStatsOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_GPUSTATS"); return e && *e == '1'; }();
        return on;
    }
    static void DiagGpuStatsRow(double unixMs, double gpuMs, const D3D11_QUERY_DATA_PIPELINE_STATISTICS* s)
    {
        static FILE* f = [] {
            CreateDirectoryA("C:/ttt2/port/fb", nullptr);
            FILE* c = std::fopen("C:/ttt2/port/fb/gpustats.csv", "w");
            if (c) std::fputs("unix_ms,gpu_ms,ia_vertices,ia_prims,vs_inv,c_prims,ps_inv\n", c);
            return c;
        }();
        if (!f) return;
        if (s) std::fprintf(f, "%.1f,%.3f,%llu,%llu,%llu,%llu,%llu\n", unixMs, gpuMs, (unsigned long long)s->IAVertices,
                            (unsigned long long)s->IAPrimitives, (unsigned long long)s->VSInvocations,
                            (unsigned long long)s->CPrimitives, (unsigned long long)s->PSInvocations);
        else std::fprintf(f, "%.1f,%.3f,,,,,\n", unixMs, gpuMs);
        static int rows = 0;
        if (++rows % 60 == 0) std::fflush(f);
    }
    std::array<GpuTimer, 4> gpuTimers_{};
    uint32_t gpuTimerAt_ = 0;
    std::vector<float> gpuFrameMs_;
    void GpuTimerFrameEnd()
    {
        GpuTimer& t = gpuTimers_[gpuTimerAt_ & 3];
        if (!t.open) return;
        context_->End(t.end.Get());
        if (t.stats) context_->End(t.stats.Get());
        context_->End(t.disjoint.Get());
        t.open = false;
        t.pending = true;
    }
    void GpuTimerFrameBegin()
    {
        for (GpuTimer& t : gpuTimers_)
        {
            if (!t.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 a = 0, b = 0;
            if (context_->GetData(t.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            if (context_->GetData(t.begin.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                context_->GetData(t.end.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            D3D11_QUERY_DATA_PIPELINE_STATISTICS ps{};
            const bool haveStats = t.stats && context_->GetData(t.stats.Get(), &ps, sizeof(ps), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (t.stats && !haveStats) continue;
            t.pending = false;
            if (!dj.Disjoint && dj.Frequency && b > a && gpuFrameMs_.size() < 4096) gpuFrameMs_.push_back(float(double(b - a) * 1000.0 / double(dj.Frequency)));
            if (DiagGpuStatsOn()) DiagGpuStatsRow(t.unixMs, !dj.Disjoint && dj.Frequency && b > a ? double(b - a) * 1000.0 / double(dj.Frequency) : -1.0,
                                                  haveStats ? &ps : nullptr);
        }
        GpuTimer& t = gpuTimers_[++gpuTimerAt_ & 3];
        if (t.pending || t.open) return;                // still unread: this frame goes untimed
        if (!t.disjoint)
        {
            D3D11_QUERY_DESC q{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            D3D11_QUERY_DESC s{ D3D11_QUERY_TIMESTAMP, 0 };
            if (FAILED(device_->CreateQuery(&q, &t.disjoint)) || FAILED(device_->CreateQuery(&s, &t.begin)) || FAILED(device_->CreateQuery(&s, &t.end)))
            { t.disjoint.Reset(); return; }
        }
        if (DiagGpuStatsOn() && !t.stats)
        {
            D3D11_QUERY_DESC q{ D3D11_QUERY_PIPELINE_STATISTICS, 0 };
            device_->CreateQuery(&q, &t.stats);
        }
        context_->Begin(t.disjoint.Get());
        context_->End(t.begin.Get());
        if (t.stats) context_->Begin(t.stats.Get());
        {
            FILETIME ft;
            GetSystemTimePreciseAsFileTime(&ft);
            t.unixMs = double(((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000ull) / 10000.0;
        }
        t.open = true;
    }
    // ---- What is bound (ApplyPipe): each draw sets only what differs from the previous one. A draw used to set ~17
    // states, all 32 texture and 16 sampler slots included, whether they changed or not (MEASURED 2026-10-25).
    struct WantVertex { ID3D11InputLayout* layout; ID3D11Buffer* vb; UINT vbStride; ID3D11VertexShader* vs; bool gpuVs;
                        D3D11_PRIMITIVE_TOPOLOGY topology; ID3D11Buffer* ib; DXGI_FORMAT ibFormat = DXGI_FORMAT_R32_UINT; };   // p103
    WantVertex want_{};
    struct PipeCache
    {
        bool valid = false, vsBuffers = false;
        UINT cbFirst[4] = { ~0u, ~0u, ~0u, ~0u };            // p_diag44: the constant ring's windows bound (vs 0/1, ps 0/1)
        ID3D11InputLayout* layout = nullptr; ID3D11Buffer* vb = nullptr; UINT vbStride = 0; ID3D11Buffer* ib = nullptr;
        DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;           // p103
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ID3D11VertexShader* vs = nullptr; ID3D11ShaderResourceView* vsSrv = nullptr;
        ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
        ID3D11BlendState* blend = nullptr; float factor[4] = {}; ID3D11DepthStencilState* depth = nullptr; UINT stencilRef = 0;
        D3D11_VIEWPORT vp{}; D3D11_RECT scissor{}; ID3D11RasterizerState* raster = nullptr;
        ID3D11PixelShader* ps = nullptr; bool psBuffers = false;
        ID3D11ShaderResourceView* srvs[32] = {}; ID3D11SamplerState* samplers[16] = {};
    };
    PipeCache pipe_;
    std::unordered_set<void*> diagSeenPs_, diagSeenVs_;
    template <class T> static T* Unknown() { return reinterpret_cast<T*>(uintptr_t(1)); }
    void ApplyPipe(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, ID3D11BlendState* blend, const float factor[4],
                   ID3D11DepthStencilState* depth, UINT stencilRef, const D3D11_VIEWPORT& vp, const D3D11_RECT& scissor,
                   ID3D11RasterizerState* raster, ID3D11PixelShader* ps, ID3D11ShaderResourceView* const srvs[32],
                   ID3D11SamplerState* const samplers[16], uint32_t textureSlots)
    {
        PipeCache& p = pipe_;
        if (!p.valid)
        {
            // Unknown: everything is set once.
            p = PipeCache{};
            p.layout = Unknown<ID3D11InputLayout>(); p.vb = p.ib = Unknown<ID3D11Buffer>(); p.vbStride = ~0u;
            p.vs = Unknown<ID3D11VertexShader>(); p.vsSrv = Unknown<ID3D11ShaderResourceView>();
            p.rtv = Unknown<ID3D11RenderTargetView>(); p.dsv = Unknown<ID3D11DepthStencilView>();
            p.blend = Unknown<ID3D11BlendState>(); p.depth = Unknown<ID3D11DepthStencilState>(); p.stencilRef = ~0u;
            p.vp.Width = -1.0f; p.scissor.right = -1; p.raster = Unknown<ID3D11RasterizerState>(); p.ps = Unknown<ID3D11PixelShader>();
            for (auto& s : p.srvs) s = Unknown<ID3D11ShaderResourceView>();
            for (auto& s : p.samplers) s = Unknown<ID3D11SamplerState>();
            p.valid = true;
        }
        if (p.layout != want_.layout) { context_->IASetInputLayout(want_.layout); p.layout = want_.layout; }
        if (p.topology != want_.topology) { context_->IASetPrimitiveTopology(want_.topology); p.topology = want_.topology; }
        if (p.vb != want_.vb || p.vbStride != want_.vbStride)
        {
            ID3D11Buffer* vb = want_.vb; UINT stride = want_.vbStride, offset = 0;
            context_->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            p.vb = vb; p.vbStride = stride;
        }
        if (p.ib != want_.ib || p.ibFormat != want_.ibFormat)
        {
            context_->IASetIndexBuffer(want_.ib, want_.ibFormat, 0);
            p.ib = want_.ib; p.ibFormat = want_.ibFormat;
        }
        if (p.vs != want_.vs)
        {
            context_->VSSetShader(want_.vs, nullptr, 0); p.vs = want_.vs;
            if (diagSeenVs_.insert(want_.vs).second) DiagFirstUse(false);
        }
        if (cbRingOn_) BindConstantRing(p);
        if (want_.gpuVs)
        {
            if (!p.vsBuffers && !cbRingOn_)
            {
                ID3D11Buffer* vcbs[2] = { vsConstants_.Get(), vsState_.Get() };
                context_->VSSetConstantBuffers(0, 2, vcbs);
                p.vsBuffers = true;
            }
            if (p.vsSrv != vdataSrv_.Get()) { ID3D11ShaderResourceView* v = vdataSrv_.Get(); context_->VSSetShaderResources(0, 1, &v); p.vsSrv = v; }
        }
        if (p.rtv != rtv || p.dsv != dsv)
        {
            context_->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, dsv);
            p.rtv = rtv; p.dsv = dsv;
            // Binding a target unbinds its views as shader inputs: set every input again.
            for (auto& s : p.srvs) s = Unknown<ID3D11ShaderResourceView>();
        }
        if (p.blend != blend || std::memcmp(p.factor, factor, sizeof(p.factor)) != 0)
        {
            context_->OMSetBlendState(blend, factor, 0xFFFFFFFF);
            p.blend = blend; std::memcpy(p.factor, factor, sizeof(p.factor));
        }
        if (p.depth != depth || p.stencilRef != stencilRef) { context_->OMSetDepthStencilState(depth, stencilRef); p.depth = depth; p.stencilRef = stencilRef; }
        if (std::memcmp(&p.vp, &vp, sizeof(vp)) != 0) { context_->RSSetViewports(1, &vp); p.vp = vp; }
        if (std::memcmp(&p.scissor, &scissor, sizeof(scissor)) != 0) { context_->RSSetScissorRects(1, &scissor); p.scissor = scissor; }
        if (p.raster != raster) { context_->RSSetState(raster); p.raster = raster; }
        if (p.ps != ps)
        {
            context_->PSSetShader(ps, nullptr, 0); p.ps = ps;
            if (diagSeenPs_.insert(ps).second) DiagFirstUse(true);
        }
        if (!p.psBuffers && !cbRingOn_)
        {
            ID3D11Buffer* cbs[2] = { psConstants_.Get(), psState_.Get() };
            context_->PSSetConstantBuffers(0, 2, cbs);
            p.psBuffers = true;
        }
        int first = -1, last = -1;
        for (int i = 0; i < 32; ++i) if (p.srvs[i] != srvs[i]) { if (first < 0) first = i; last = i; }
        if (first >= 0)
        {
            context_->PSSetShaderResources(UINT(first), UINT(last - first + 1), srvs + first);
            for (int i = first; i <= last; ++i) p.srvs[i] = srvs[i];
        }
        first = last = -1;
        for (int i = 0; i < 16; ++i) if (p.samplers[i] != samplers[i]) { if (first < 0) first = i; last = i; }
        if (first >= 0)
        {
            context_->PSSetSamplers(UINT(first), UINT(last - first + 1), samplers + first);
            for (int i = first; i <= last; ++i) p.samplers[i] = samplers[i];
        }
        static const bool verify = [] { const char* e = std::getenv("TTT2_DIAG_VERIFY"); return e && *e == '1'; }();
        if (verify) VerifyPipe(rtv, dsv, blend, depth, stencilRef, vp, scissor, raster, ps, srvs, samplers, textureSlots);
    }
    // TTT2_DIAG_VERIFY=1: the bindings as the GPU will see them, against what this draw asked for. A texture input
    // read back empty while it is also this draw's target is the runtime's own hazard rule (as before), not counted.
    void VerifyPipe(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, ID3D11BlendState* blend, ID3D11DepthStencilState* depth,
                    UINT stencilRef, const D3D11_VIEWPORT& vp, const D3D11_RECT& scissor, ID3D11RasterizerState* raster,
                    ID3D11PixelShader* ps, ID3D11ShaderResourceView* const srvs[32], ID3D11SamplerState* const samplers[16], uint32_t textureSlots)
    {
        static uint64_t checked = 0, bad = 0;
        auto fail = [&](const char* what) { if (bad++ < 30) std::fprintf(stderr, "[verify] draw state differs: %s\n", what); };
        ComPtr<ID3D11InputLayout> l; context_->IAGetInputLayout(&l); if (l.Get() != want_.layout) fail("input layout");
        ComPtr<ID3D11Buffer> vb; UINT stride = 0, offset = 0; context_->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
        if (vb.Get() != want_.vb || (want_.vb && stride != want_.vbStride)) fail("vertex buffer");
        ComPtr<ID3D11Buffer> ib; DXGI_FORMAT f{}; UINT io = 0; context_->IAGetIndexBuffer(&ib, &f, &io); if (ib.Get() != want_.ib) fail("index buffer");
        if (ib && f != want_.ibFormat) fail("index format");      // p103
        D3D11_PRIMITIVE_TOPOLOGY t{}; context_->IAGetPrimitiveTopology(&t); if (t != want_.topology) fail("topology");
        ComPtr<ID3D11VertexShader> vs; context_->VSGetShader(&vs, nullptr, nullptr); if (vs.Get() != want_.vs) fail("vertex shader");
        if (want_.gpuVs)
        {
            ID3D11Buffer* raw[2] = {}; UINT first[2] = {}, num[2] = {};
            if (cbRingOn_)
            {
                context1_->VSGetConstantBuffers1(0, 2, raw, first, num);
                if (raw[0] != cbRing_.Get() || raw[1] != cbRing_.Get() || first[0] != cbBlock_[0].first || first[1] != cbBlock_[1].first ||
                    num[0] != cbBlock_[0].num || num[1] != cbBlock_[1].num || cbBlock_[0].gen != cbChunkGen_ || cbBlock_[1].gen != cbChunkGen_)
                    fail("vertex constants (ring)");
            }
            else
            {
                context_->VSGetConstantBuffers(0, 2, raw);
                if (raw[0] != vsConstants_.Get() || raw[1] != vsState_.Get()) fail("vertex constants");
            }
            for (auto* x : raw) if (x) x->Release();
            ComPtr<ID3D11ShaderResourceView> v; context_->VSGetShaderResources(0, 1, &v); if (v.Get() != vdataSrv_.Get()) fail("vertex data view");
        }
        ComPtr<ID3D11RenderTargetView> r; ComPtr<ID3D11DepthStencilView> d; context_->OMGetRenderTargets(1, &r, &d);
        if (r.Get() != rtv || d.Get() != dsv) fail("targets");
        ComPtr<ID3D11BlendState> b; float bf[4]; UINT m = 0; context_->OMGetBlendState(&b, bf, &m); if (b.Get() != blend) fail("blend");
        ComPtr<ID3D11DepthStencilState> ds; UINT ref = 0; context_->OMGetDepthStencilState(&ds, &ref); if (ds.Get() != depth || ref != stencilRef) fail("depth state");
        UINT n = 1; D3D11_VIEWPORT gv{}; context_->RSGetViewports(&n, &gv); if (std::memcmp(&gv, &vp, sizeof(vp)) != 0) fail("viewport");
        n = 1; D3D11_RECT gs{}; context_->RSGetScissorRects(&n, &gs); if (std::memcmp(&gs, &scissor, sizeof(gs)) != 0) fail("scissor");
        ComPtr<ID3D11RasterizerState> rs; context_->RSGetState(&rs); if (rs.Get() != raster) fail("raster");
        ComPtr<ID3D11PixelShader> p; context_->PSGetShader(&p, nullptr, nullptr); if (p.Get() != ps) fail("pixel shader");
        if (cbRingOn_)      // p_diag44
        {
            VerifyRingData();
            ID3D11Buffer* raw[2] = {}; UINT first[2] = {}, num[2] = {};
            context1_->PSGetConstantBuffers1(0, 2, raw, first, num);
            if (raw[0] != cbRing_.Get() || raw[1] != cbRing_.Get() || first[0] != cbBlock_[2].first || first[1] != cbBlock_[3].first ||
                num[0] != cbBlock_[2].num || num[1] != cbBlock_[3].num || cbBlock_[2].gen != cbChunkGen_ || cbBlock_[3].gen != cbChunkGen_)
                fail("pixel constants (ring)");
            for (auto* x : raw) if (x) x->Release();
        }
        ID3D11ShaderResourceView* got[32] = {}; context_->PSGetShaderResources(0, 32, got);
        ID3D11SamplerState* gotS[16] = {}; context_->PSGetSamplers(0, 16, gotS);
        for (int i = 0; i < 32; ++i)
        {
            if ((textureSlots >> i) & 1)
            {
                if (got[i] != srvs[i] && got[i]) fail("texture input");
                if (!got[i] && srvs[i]) ++hazards_;
                if (gotS[i & 15] != samplers[i & 15]) fail("sampler");
            }
            if (got[i]) got[i]->Release();
        }
        for (auto* s : gotS) if (s) s->Release();
        if (++checked % 50000 == 0)
            std::fprintf(stderr, "[verify] %llu draws' state checked (%llu differ, %llu inputs empty as their own target)\n",
                         (unsigned long long)checked, (unsigned long long)bad, (unsigned long long)hazards_);
    }
    uint64_t hazards_ = 0;

    // ---- DIAGNOSTIC BUILD (diag6, 2026-10-27): the GPU's timeline of a character-texture bake. GPU timestamps (bottom of
    // the pipe: a stamp passes when everything before it is done) before and after every draw ('b', 'd'), before every
    // resolve ('R'), around the staged copy ('C', 'c') and after every present ('P'), with the CPU time each was queued.
    // After a 1024x1024 bake read, the stretch from the present before its copy to the copy's end: d-b = a draw kept the
    // GPU busy; b-(previous) = what ran between draws (our uploads, state) or the GPU idle, waiting for work.
    struct DiagTs { ComPtr<ID3D11Query> q; char kind = 0; uint32_t a = 0, b = 0, c = 0, d = 0; int64_t cpu = 0; };
    std::vector<DiagTs> diagTs_;
    size_t diagTsAt_ = 0, diagCopyTs_ = 0;
    ComPtr<ID3D11Query> diagDisjoint_[4];
    int diagDisjointAt_ = 0;
    bool diagDisjointOpen_ = false;
    uint64_t diagTsFreq_ = 0;
    unsigned diagDisjointFrames_ = 0, diagDisjointSeen_ = 0;
    // diag7: TTT2_DIAG_DRAW_GPU=1 stamps every draw (the bake timeline; costs the command processor ~2 us a draw, MEASURED
    // 2026-10-27: 9.2 vs 7.2 us/draw in a Practice fight), =0 nothing, default: sampled frames (DiagFramePresent).
    static int DiagDrawGpuMode()
    {
        static const int mode = [] { const char* e = std::getenv("TTT2_DIAG_DRAW_GPU"); return e && *e == '1' ? 1 : e && *e == '0' ? 0 : 2; }();
        return mode;
    }
    static bool DiagDrawGpu() { return DiagDrawGpuMode() == 1; }
    void DiagStamp(char kind, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0)
    {
        const int mode = DiagDrawGpuMode();
        if (!mode || !device_ || (mode == 2 && kind != 'P' && !diagSampling_)) return;
        if (kind == 'P' || !diagDisjointOpen_)
        {
            // Timestamps count at the frequency a TIMESTAMP_DISJOINT query reports; one is open from present to present.
            if (diagDisjointOpen_) context_->End(diagDisjoint_[diagDisjointAt_ & 3].Get());
            for (auto& q : diagDisjoint_)
            {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
                if (q && context_->GetData(q.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && dj.Frequency)
                {
                    diagTsFreq_ = dj.Frequency;
                    ++diagDisjointFrames_;
                    diagDisjointSeen_ += dj.Disjoint ? 1 : 0;
                }
            }
            ComPtr<ID3D11Query>& next = diagDisjoint_[++diagDisjointAt_ & 3];
            next.Reset();
            D3D11_QUERY_DESC q{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            if (SUCCEEDED(device_->CreateQuery(&q, &next))) { context_->Begin(next.Get()); diagDisjointOpen_ = true; }
            else diagDisjointOpen_ = false;
        }
        if (diagTs_.empty()) diagTs_.resize(32768);
        DiagTs& t = diagTs_[diagTsAt_ % diagTs_.size()];
        if (!t.q)
        {
            D3D11_QUERY_DESC s{ D3D11_QUERY_TIMESTAMP, 0 };
            if (FAILED(device_->CreateQuery(&s, &t.q))) return;
        }
        context_->End(t.q.Get());
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        t.kind = kind; t.a = a; t.b = b; t.c = c; t.d = d; t.cpu = now.QuadPart;
        ++diagTsAt_;
        if (kind == 'P' && mode == 2) DiagFramePresent(diagTsAt_ - 1, now.QuadPart);
    }

    // ---- diag7: sampled frames. Present stamps always; between two of them, the draws of one frame in 30 and of the frame
    // after a 20-100 ms one (a drop, not a load). The sampled frame is read back at a later present (its stamps must all
    // be done: GetData without a flush, up to 8 tries).
    bool diagSampling_ = false, diagSampleSlow_ = false, diagPending_ = false, diagPendingSlow_ = false;
    size_t diagSampleBegin_ = 0, diagSampleEnd_ = 0;
    int64_t diagLastPresentCpu_ = 0;
    unsigned diagSampleCountdown_ = 30, diagPendingTries_ = 0, diagSampleUnread_ = 0;
    struct DiagFrameSum
    {
        unsigned frames = 0, draws = 0, worstDraws = 0;
        double cpu = 0, gpu = 0, inDraws = 0, longestOut = 0, worstGpu = 0, worstCpu = 0, worstIn = 0, worstLongest = 0;
    };
    DiagFrameSum diagFrames_[2];                        // [0] every 30th frame, [1] the frame after a slow one
    void DiagFramePresent(size_t at, int64_t cpu)
    {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        const double intervalMs = diagLastPresentCpu_ ? double(cpu - diagLastPresentCpu_) * 1000.0 / double(f.QuadPart) : 0.0;
        diagLastPresentCpu_ = cpu;
        if (diagSampling_)
        {
            diagSampling_ = false;
            diagSampleEnd_ = at;
            diagPending_ = true;
            diagPendingSlow_ = diagSampleSlow_;
            diagPendingTries_ = 0;
        }
        else if (diagPending_ && (DiagReadSample() || ++diagPendingTries_ > 8))
        {
            if (diagPendingTries_ > 8) ++diagSampleUnread_;
            diagPending_ = false;
        }
        if (diagPending_ || diagSampling_) return;
        const bool slow = intervalMs > 20.0 && intervalMs < 100.0;
        if (slow || --diagSampleCountdown_ == 0)
        {
            diagSampling_ = true;
            diagSampleSlow_ = slow;
            diagSampleBegin_ = at;
            diagSampleCountdown_ = 30;
        }
    }
    bool DiagReadSample()
    {
        const size_t n = diagTs_.size();
        if (!diagTsFreq_ || diagSampleEnd_ <= diagSampleBegin_ || diagTsAt_ - diagSampleBegin_ >= n) return true;
        std::vector<UINT64> v(diagSampleEnd_ - diagSampleBegin_ + 1);
        for (size_t i = diagSampleBegin_; i <= diagSampleEnd_; ++i)
        {
            const DiagTs& t = diagTs_[i % n];
            if (!t.q || context_->GetData(t.q.Get(), &v[i - diagSampleBegin_], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                return false;
        }
        double gpu = 0, inDraws = 0, longestOut = 0;
        unsigned draws = 0;
        for (size_t i = diagSampleBegin_ + 1; i <= diagSampleEnd_; ++i)
        {
            const UINT64 a = v[i - 1 - diagSampleBegin_], b = v[i - diagSampleBegin_];
            const double ms = b >= a ? double(b - a) * 1000.0 / double(diagTsFreq_) : 0.0;
            gpu += ms;
            if (diagTs_[i % n].kind == 'd') { inDraws += ms; ++draws; }
            else longestOut = (std::max)(longestOut, ms);
        }
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        const double cpu = double(diagTs_[diagSampleEnd_ % n].cpu - diagTs_[diagSampleBegin_ % n].cpu) * 1000.0 / double(f.QuadPart);
        DiagFrameSum& s = diagFrames_[diagPendingSlow_ ? 1 : 0];
        ++s.frames; s.draws += draws; s.cpu += cpu; s.gpu += gpu; s.inDraws += inDraws; s.longestOut += longestOut;
        if (gpu > s.worstGpu) { s.worstGpu = gpu; s.worstCpu = cpu; s.worstIn = inDraws; s.worstLongest = longestOut; s.worstDraws = draws; }
        return true;
    }
    void DiagFramesReport()
    {
        static const char* const names[2] = { "sampled frames", "frames after a slow one" };
        for (int k = 0; k < 2; ++k)
        {
            DiagFrameSum& s = diagFrames_[k];
            if (!s.frames) continue;
            const double m = 1.0 / double(s.frames);
            std::fprintf(stderr, "[diag-gpu] %s: %u, mean: GPU present to present %.2f ms (CPU %.2f ms), in draws %.2f ms (%u draws), "
                                 "longest stretch outside draws %.2f ms; worst: GPU %.2f ms (CPU %.2f ms), in draws %.2f ms (%u draws), "
                                 "longest outside %.2f ms%s\n",
                         names[k], s.frames, s.gpu * m, s.cpu * m, s.inDraws * m, unsigned(s.draws * m + 0.5), s.longestOut * m,
                         s.worstGpu, s.worstCpu, s.worstIn, s.worstDraws, s.worstLongest,
                         k == 0 && diagSampleUnread_ ? "; some samples never read back" : "");
            s = DiagFrameSum{};
        }
    }
    void DiagGpuTimeline(uint32_t dest, double waitedMs)
    {
        static int logged = 0;
        if (!DiagDrawGpu() || !diagCopyTs_ || logged >= 80) return;
        const size_t n = diagTs_.size(), end = diagCopyTs_;
        diagCopyTs_ = 0;
        if (!diagTsFreq_) { std::fprintf(stderr, "[diag-gpu] no timestamp frequency yet\n"); return; }
        const size_t oldest = diagTsAt_ > n ? diagTsAt_ - n + 1 : 0;
        size_t begin = end - 1;
        int presents = 0;
        while (begin > oldest && presents < 1) { --begin; presents += diagTs_[begin % n].kind == 'P'; }
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        struct Step { char kind; double gpu, cpu; const DiagTs* t; };
        std::vector<Step> steps;
        uint64_t prev = 0, first = 0;
        int64_t prevCpu = 0, firstCpu = 0;
        size_t unread = 0;
        for (size_t i = begin; i < end; ++i)
        {
            const DiagTs& t = diagTs_[i % n];
            UINT64 v = 0;
            if (!t.q || context_->GetData(t.q.Get(), &v, sizeof(v), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { ++unread; continue; }
            if (!first) { first = v; firstCpu = t.cpu; }
            else if (v >= prev)
                steps.push_back({ t.kind, double(v - prev) * 1000.0 / double(diagTsFreq_), double(t.cpu - prevCpu) * 1000.0 / double(f.QuadPart), &t });
            prev = v; prevCpu = t.cpu;
        }
        if (steps.empty()) return;
        ++logged;
        double draws = 0, gaps = 0, resolves = 0, copy = 0, other = 0;
        unsigned drawCount = 0;
        for (const Step& s : steps)
        {
            if (s.kind == 'd') { draws += s.gpu; ++drawCount; }
            else if (s.kind == 'b') gaps += s.gpu;
            else if (s.kind == 'R') resolves += s.gpu;
            else if (s.kind == 'c') copy += s.gpu;
            else other += s.gpu;
        }
        std::fprintf(stderr, "[diag-gpu] bake read %08X waited %.1f ms; GPU from the present before to the copy's end %.2f ms "
                             "(queued over %.2f ms): %u draws %.2f ms, between draws %.2f ms, resolves %.2f ms, copy %.2f ms, "
                             "other %.2f ms; %zu stamps unread; disjoint frames %u of %u\n",
                     dest, waitedMs, double(prev - first) * 1000.0 / double(diagTsFreq_),
                     double(prevCpu - firstCpu) * 1000.0 / double(f.QuadPart), drawCount, draws, gaps, resolves, copy, other,
                     unread, diagDisjointSeen_, diagDisjointFrames_);
        std::vector<const Step*> top;
        for (const Step& s : steps) top.push_back(&s);
        std::sort(top.begin(), top.end(), [](const Step* x, const Step* y) { return x->gpu > y->gpu; });
        for (size_t i = 0; i < top.size() && i < 8 && top[i]->gpu >= 0.05; ++i)
        {
            const Step& s = *top[i];
            const DiagTs& t = *s.t;
            if (s.kind == 'd' || s.kind == 'b')
                std::fprintf(stderr, "[diag-gpu]   %.2f ms %s (queued %.2f ms after the previous stamp): surface %05X%s viewport %ux%u, %u indices, ps %u dwords\n",
                             s.gpu, s.kind == 'd' ? "in a draw" : "before a draw", s.cpu, t.a & 0xFFFFF, (t.a >> 31) ? " native" : "",
                             t.b >> 16, t.b & 0xFFFF, t.c, t.d);
            else if (s.kind == 'R')
                std::fprintf(stderr, "[diag-gpu]   %.2f ms before a resolve (queued %.2f ms after): %05X -> %08X %ux%u select %u\n",
                             s.gpu, s.cpu, t.c, t.a, t.b >> 16, t.b & 0xFFFF, t.d);
            else
                std::fprintf(stderr, "[diag-gpu]   %.2f ms %s (queued %.2f ms after)\n", s.gpu,
                             s.kind == 'c' ? (t.a ? "in the copy (with a resample)" : "in the copy") : s.kind == 'C' ? "before the copy" :
                             s.kind == 'P' ? "at a present" : "?", s.cpu);
        }
    }

    // ---- DIAGNOSTIC BUILD (0.1.7-diag1, 2026-10-24): where the command processor's time goes, for a low-end PC
    // report (i3-3240 + GeForce GT 730: 11-16 ms of this thread per fight frame, fps drops when hits land). Wall
    // time of each part (QueryPerformanceCounter), per frame, with the frame cost every 5 s. Texture uploads are
    // inside textures; readback waits can be on other threads (a game thread reading a bake).
    enum DiagPart { DpCpuVertices, DpVertexData, DpIndices, DpConstants, DpTextures, DpTextureUploads, DpShaders, DpState,
                    DpDrawCall, DpResolve, DpPresent, DpReadback, DpVsSetup, DpCount };
    std::atomic<int64_t> diagTicks_[DpCount] = {};
    double lastCpMs_ = -1.0;

    // ---- p81 DIAGNOSTIC (TTT2_DIAG_FRAMES=<file>): one line per presented frame, so a drop of a second or two (a Tag
    // Assault, a wall break) is not averaged away by the 5-s report. The low-end player (i3-3240 + GT 730, 640x360)
    // reports drops in exactly such moments, and the game's own dynamic resolution (the console's: 1280 down to ~640
    // wide when the GPU is busy, MEASURED here as 320-640 wide half-size blur resolves when the command processor is slowed)
    // ran on their PC too (896-wide surfaces in logs 24/25; held at full width since, post_dof.cpp - it asks for narrow
    // widths on fast PCs as well). Columns: time, present interval, the
    // command processor's renderer parts (as the 5-s report adds them) and its draw packets, the game thread's frame kick and
    // its wait for the command processor, draws, gpu-vs draws, resolves, vertex data KB, the most common viewport width of
    // the frame's 720-tall draws (the game's render width) with its draw count, screenshots read back. Times in ms.
    int64_t frCum_[DpCount] = {};
    int64_t frPacketCum_ = 0, frKickCum_ = 0, frKickGpuCum_ = 0;
    struct FrameMark { int64_t qpc = 0, parts = 0, packets = 0, kick = 0, kickGpu = 0; uint64_t draws = 0, gpuVs = 0, resolves = 0, vbytes = 0;
                       uint64_t tsc = 0, mainCycles = 0, mainOff[4] = {}; int64_t part[DpCount] = {}; };
    FrameMark frLast_;
    std::FILE* frFile_ = nullptr;
    uint32_t frW_[8] = {}, frWn_[8] = {}, frShots_ = 0, frLines_ = 0;
    // p84 (diag12): always on - frames over 22 ms go to the log as "[slowframe]" (the player's drops: Electrics, hits,
    // bounds, Tag Assaults), every frame to TTT2_DIAG_FRAMES=<file> when set; TTT2_DIAG_FRAMES=0 off.
    // TEST 13 (asked by a developer reading log 41, whose 111-121 ms frames on stage 14 had ~6 ms of renderer and frame
    // kick): the clock time (to find the frame in a recording), the pixels the render width becomes at the render scale
    // (RESOLUTION 640x360 = 0.50: "render width 1280" is the game's own viewport, drawn 640 px wide), the game main
    // thread's frame by kind (on the processor, waiting for objects, sleeping, critical sections, yielding; the rest is
    // ready to run but not running) and the game's file reads in the frame.
    static bool FramesOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_FRAMES"); return !(e && *e == '0' && !e[1]); }();
        return on;
    }
    double frPrevDt_ = 0.0;
    uint64_t frPrevDraws_ = 0;
    uint32_t frSlowLogged_ = 0;
    void FrNoteViewport(float xs, float ys)
    {
        const float h = std::fabs(ys) * 2.0f;
        if (h < 719.0f || h > 721.0f) return;
        const uint32_t w = uint32_t(std::fabs(xs) * 2.0f + 0.5f);
        for (int i = 0; i < 8; ++i)
        {
            if (!frWn_[i]) { frW_[i] = w; frWn_[i] = 1; return; }
            if (frW_[i] == w) { ++frWn_[i]; return; }
        }
    }
    void FrameRecord()
    {
        static const char* const framesPath = [] { const char* e = std::getenv("TTT2_DIAG_FRAMES"); return e && *e ? e : nullptr; }();
        if (!frFile_ && framesPath && !frLines_)
        {
            frFile_ = std::fopen(framesPath, "w");
            ++frLines_;
        }
        if (frFile_ && frLines_ == 1)
        {
            std::setvbuf(frFile_, nullptr, _IOFBF, 1 << 16);
            LARGE_INTEGER q{}, f{};
            QueryPerformanceCounter(&q);
            QueryPerformanceFrequency(&f);
            FILETIME ft;
            GetSystemTimePreciseAsFileTime(&ft);
            const double unixMs = double(((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000ull) / 10000.0;
            std::fprintf(frFile_, "# clock %.1f = unix ms %.1f\n", double(q.QuadPart) * 1000.0 / double(f.QuadPart), unixMs);
            std::fprintf(frFile_, "t_ms dt_ms parts_ms packets_ms kick_ms kickcp_ms draws gvs res vdata_kb width width_draws shots\n");
            ++frLines_;
        }
        static const double msPerTick = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 / double(f.QuadPart); }();
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        FrameMark m;
        m.qpc = now.QuadPart;
        const int parts[] = { DpCpuVertices, DpVertexData, DpIndices, DpConstants, DpTextures, DpShaders, DpState, DpDrawCall, DpResolve, DpPresent };
        for (int i = 0; i < DpCount; ++i) m.part[i] = frCum_[i] + diagTicks_[i].load(std::memory_order_relaxed);
        for (int i : parts) m.parts += m.part[i];
        m.packets = frPacketCum_ + g_diagDrawPacketTicks.load(std::memory_order_relaxed);
        m.kick = frKickCum_ + g_diagKickNs.load(std::memory_order_relaxed);
        m.kickGpu = frKickGpuCum_ + g_diagKickGpuNs.load(std::memory_order_relaxed);
        m.draws = stats_.draws; m.gpuVs = stats_.gpuVsDraws; m.resolves = stats_.resolves; m.vbytes = stats_.vertexDataBytes;
        m.tsc = __rdtsc();
        if (g_mainThreadHandle) { ULONG64 c = 0; QueryThreadCycleTime(g_mainThreadHandle, &c); m.mainCycles = c; }
        DiagMainOffTotals(m.mainOff);
        uint64_t fileReads = 0, fileBytes = 0;
        double fileLongestMs = 0;
        KernelTakeFrameReads(fileReads, fileBytes, fileLongestMs);
        int best = -1;
        for (int i = 0; i < 8; ++i) if (frWn_[i] && (best < 0 || frWn_[i] > frWn_[best])) best = i;
        if (frLast_.qpc)
        {
            const double dt = double(m.qpc - frLast_.qpc) * msPerTick;
            const double partsMs = double(m.parts - frLast_.parts) * msPerTick, packetsMs = double(m.packets - frLast_.packets) * msPerTick;
            const double kickMs = double(m.kick - frLast_.kick) / 1e6, kickCpMs = double(m.kickGpu - frLast_.kickGpu) / 1e6;
            const unsigned long long draws = m.draws - frLast_.draws, gvs = m.gpuVs - frLast_.gpuVs, res = m.resolves - frLast_.resolves;
            const unsigned long long vkb = (m.vbytes - frLast_.vbytes) >> 10;
            const uint32_t width = best < 0 ? 0u : frW_[best], widthDraws = best < 0 ? 0u : frWn_[best];
            if (frFile_)
            {
                std::fprintf(frFile_, "%.1f %.2f %.2f %.2f %.2f %.2f %llu %llu %llu %llu %u %u %u\n", double(m.qpc) * msPerTick, dt,
                             partsMs, packetsMs, kickMs, kickCpMs, draws, gvs, res, vkb, width, widthDraws, frShots_);
                if (++frLines_ % 120 == 0) std::fflush(frFile_);
            }
            if (dt > 22.0 && dt < 1000.0 && frSlowLogged_ < 4000)
            {
                ++frSlowLogged_;
                const double tscPerMs = double(m.tsc - frLast_.tsc) / dt;
                double gameMain[5] = { double(m.mainCycles - frLast_.mainCycles) / tscPerMs };
                for (int k = 0; k < 4; ++k) gameMain[k + 1] = double(m.mainOff[k] - frLast_.mainOff[k]) / tscPerMs;
                // The frame kick's wait for the command processor is a host wait, not in the guest wait counters.
                const double mainRest = std::max(0.0, dt - gameMain[0] - gameMain[1] - gameMain[2] - gameMain[3] - gameMain[4] - kickCpMs);
                SYSTEMTIME clock{};
                GetLocalTime(&clock);
                // A renderer over 15 ms: its parts over 1 ms (the fight's first frame: 64 ms with 8 MB of vertex data in log 41).
                char split[256] = "";
                if (partsMs > 15.0)
                {
                    static const char* const names[DpCount] = { "cpu-path vertices", "vertex data", "indices", "constants", "textures",
                        "of which uploads", "shader lookups", "state", "draw calls", "resolves", "present", "readbacks", "gpu-vs setup" };
                    size_t at = 0;
                    for (int i : { int(DpCpuVertices), int(DpVertexData), int(DpIndices), int(DpConstants), int(DpTextures),
                                   int(DpTextureUploads), int(DpShaders), int(DpState), int(DpDrawCall), int(DpResolve), int(DpPresent) })
                    {
                        const double v = double(m.part[i] - frLast_.part[i]) * msPerTick;
                        if (v < 1.0 || at + 40 >= sizeof(split)) continue;
                        const int n = std::snprintf(split + at, sizeof(split) - at, "%s%s %.1f", at ? ", " : " (", names[i], v);
                        if (n > 0) at += size_t(n);
                    }
                    if (at) std::snprintf(split + at, sizeof(split) - at, ")");
                }
                std::fprintf(stderr, "[slowframe] %.1f ms at %02u:%02u:%02u.%03u: renderer %.1f%s, draw packets %.1f; game thread's frame kick %.1f "
                                     "(waiting for the command processor %.1f); draws %llu (gpu-vs %llu), resolves %llu, vertex data %llu KB, "
                                     "render width %u (%u draws; %u px at the render scale %.2f); game main thread: on the processor %.1f, "
                                     "waiting for objects %.1f, sleeping %.1f, critical sections %.1f, yielding %.1f, the rest (besides the frame kick's wait) %.1f; file reads %llu "
                                     "(%llu KB, the longest %.1f ms); the frame before %.1f ms, %llu draws; %s%s%s%s\n",
                             dt, unsigned(clock.wHour), unsigned(clock.wMinute), unsigned(clock.wSecond), unsigned(clock.wMilliseconds),
                             partsMs, split, packetsMs, kickMs, kickCpMs, draws, gvs, res, vkb, width, widthDraws,
                             unsigned(float(width) * scale_ + 0.5f), double(scale_), gameMain[0], gameMain[1], gameMain[2], gameMain[3], gameMain[4], mainRest,
                             (unsigned long long)fileReads, (unsigned long long)(fileBytes >> 10), fileLongestMs, frPrevDt_,
                             (unsigned long long)frPrevDraws_,
                             frame_overlap::AbOn() ? (frame_overlap::AbTurn() ? "turn" : "overlap") : "-",
                             !VdAbOn() ? "" : vdAbOff_ ? " vdcache-off" : " vdcache-on",
                             post_dof::Label(), frShots_ ? " screenshot" : "");
            }
            frPrevDt_ = dt;
            frPrevDraws_ = draws;
        }
        frLast_ = m;
        std::memset(frWn_, 0, sizeof(frWn_));
        frShots_ = 0;
    }
    uint64_t diagVsCbUploads_ = 0, diagPsCbUploads_ = 0, diagPsStateUploads_ = 0, diagVsRegsChanged_ = 0, diagVsSpan_ = 0;
    uint32_t diagLastEpoch_ = 0;
    struct DiagScope
    {
        std::atomic<int64_t>& acc;
        uint64_t t0 = 0;
        bool done = false;
        explicit DiagScope(std::atomic<int64_t>& a) : acc(a), t0(__rdtsc()) {}       // p_diag39: the TSC
        void Stop()
        {
            if (done) return;
            done = true;
            DiagAddSince(acc, t0);
        }
        ~DiagScope() { Stop(); }
    };
    void ReportDiag(uint64_t presents, uint64_t draws, uint64_t gpuVsDraws)
    {
        static const double msPerTick = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 / double(f.QuadPart); }();
        double ms[DpCount];
        for (int i = 0; i < DpCount; ++i)
        {
            const int64_t v = diagTicks_[i].exchange(0, std::memory_order_relaxed);
            frCum_[i] += v;                                 // p81: the per-frame record's running totals
            ms[i] = double(v) * msPerTick / double(presents ? presents : 1);
        }
        if (!presents || lastCpMs_ < 0) return;
        const double parts = ms[DpCpuVertices] + ms[DpVertexData] + ms[DpIndices] + ms[DpConstants] + ms[DpTextures] + ms[DpShaders] +
                             ms[DpState] + ms[DpDrawCall] + ms[DpResolve] + ms[DpPresent];
        const int64_t packetTicks = g_diagDrawPacketTicks.exchange(0, std::memory_order_relaxed);
        frPacketCum_ += packetTicks;
        const double packets = double(packetTicks) * msPerTick / double(presents);
        const double inRenderer = ms[DpCpuVertices] + ms[DpVertexData] + ms[DpIndices] + ms[DpConstants] + ms[DpTextures] + ms[DpShaders] +
                                  ms[DpState] + ms[DpDrawCall] + ms[DpVsSetup];
        double prep[7];
        for (int i = 0; i < 7; ++i) prep[i] = double(g_diagPrepTicks[i].exchange(0, std::memory_order_relaxed)) * msPerTick / double(presents);
        std::fprintf(stderr, "[diag]   constant uploads per frame: vs %.0f (%.1f registers changed, span %.1f), ps %.0f, ps state %.0f; "
                             "vertex-constant blocks written %.0f, %.2f KB each\n",
                     double(diagVsCbUploads_) / double(presents), diagVsCbUploads_ ? double(diagVsRegsChanged_) / double(diagVsCbUploads_) : 0.0,
                     diagVsCbUploads_ ? double(diagVsSpan_) / double(diagVsCbUploads_) : 0.0,
                     double(diagPsCbUploads_) / double(presents), double(diagPsStateUploads_) / double(presents),
                     double(diagVsBlockWrites_) / double(presents), diagVsBlockWrites_ ? double(diagVsBlockBytes_) / 1024.0 / double(diagVsBlockWrites_) : 0.0);
        diagVsCbUploads_ = diagPsCbUploads_ = diagPsStateUploads_ = diagVsRegsChanged_ = diagVsSpan_ = 0;
        diagVsBlockWrites_ = diagVsBlockBytes_ = 0;
        std::fprintf(stderr, "[diag]   constant ring KB written per frame: vertex constants %.0f, vertex state %.0f, pixel constants %.0f, "
                             "pixel state %.0f (%s)\n",
                     diagCbBytes_[0] / 1024.0 / double(presents), diagCbBytes_[1] / 1024.0 / double(presents),
                     diagCbBytes_[2] / 1024.0 / double(presents), diagCbBytes_[3] / 1024.0 / double(presents),
                     CbSparse() ? "the parts the shaders read" : "whole blocks");
        for (uint64_t& b : diagCbBytes_) b = 0;
        std::fprintf(stderr, "[diag]   write-watch syncs per frame: %.0f calls, %.2f ms; %.0f skipped (once per batch), %.0f in a synced cluster, %.0f batches; "
                             "skipped ranges written inside a batch: %lld\n",
                     double(g_diagSyncCalls.exchange(0)) / double(presents), double(g_diagSyncTicks.exchange(0)) * msPerTick / double(presents),
                     double(g_diagSyncSkipped.exchange(0)) / double(presents), double(g_diagSyncClustered.exchange(0)) / double(presents),
                     double(g_cpSyncEpoch.load() - diagLastEpoch_) / double(presents),
                     (long long)g_diagSyncMissed.exchange(0));
        diagLastEpoch_ = g_cpSyncEpoch.load();
        std::fprintf(stderr, "[diag]   texture binds per frame: %.0f, memo hits %.0f (%s; %.0f stale, %.0f looked up after resolves); "
                             "resolves' texture drops %.0f, %.0f skipped, %.3f ms\n",
                     double(diagTexBinds_) / double(presents), double(diagTexMemoHits_) / double(presents),
                     TexMemo2On() && !ixAbOff_ ? "across batches" : "one batch",
                     double(diagTexMemoStale_) / double(presents), double(diagTexMemoRechecks_) / double(presents),
                     double(diagDropCalls_) / double(presents), double(diagDropSkips_) / double(presents),
                     double(diagDropTicks_) * DiagTscToQpc() * msPerTick / double(presents));
        std::fprintf(stderr, "[diag]   texture memo: cached textures dropped / re-uploaded %.1f times a frame, resolves changed %.1f times\n",
                     double(texFullGen_ - diagLastFullGen_) / double(presents), double(texGen_ - diagLastTexGen_) / double(presents));
        diagLastFullGen_ = texFullGen_; diagLastTexGen_ = texGen_;
        diagTexBinds_ = diagTexMemoHits_ = diagTexMemoStale_ = diagTexMemoRechecks_ = 0;
        diagDropCalls_ = diagDropSkips_ = diagDropTicks_ = 0;
        std::fprintf(stderr, "[diag]   preparation per frame (ms): microcode loads %.2f, index read %.2f, triangle assembly %.2f, draw state %.2f, "
                             "gpu-vs setup %.2f, pixel-state decode %.2f, vs lookup call %.2f, gpu-vs draw call %.2f\n",
                     prep[0], prep[1], prep[2], prep[3], ms[DpVsSetup], prep[4], prep[5], prep[6]);
        {
            const double p = double(presents) * 1e6;
            uint64_t kicks = 0, overlapped = 0;
            frame_overlap::TakeStats(kicks, overlapped);
            const int64_t kickNs = g_diagKickNs.exchange(0), kickGpuNs = g_diagKickGpuNs.exchange(0);
            frKickCum_ += kickNs; frKickGpuCum_ += kickGpuNs;
            const double kick = double(kickNs) / p, kickGpu = double(kickGpuNs) / p;
            std::fprintf(stderr, "[diag]   game main thread per frame (ms): frame kick %.2f (%.2f kicks) = waiting for the job workers %.2f + "
                                 "waiting for the command processor %.2f; other GPU waits %.2f (%s); frames overlapped %llu of %llu\n",
                         kick, double(g_diagKicks.exchange(0)) / double(presents), kick - kickGpu, kickGpu,
                         double(g_diagOtherGpuNs.exchange(0)) / p, DiagTakeWaitSites(double(presents)).c_str(),
                         (unsigned long long)overlapped, (unsigned long long)kicks);
            std::fprintf(stderr, "[diag]   frame overlap write guard per frame: %s\n", frame_overlap::TakeGuardStats(double(presents)).c_str());
        }
        std::fprintf(stderr, "[diag] command processor per frame (ms): cpu-path vertices %.2f, vertex data %.2f, indices %.2f, constants %.2f, "
                             "textures %.2f (uploads %.2f), shader lookups %.2f, state %.2f, draw calls %.2f, resolves %.2f, present %.2f = %.2f; "
                             "thread cpu %.2f | draw packets %.2f, of which before the renderer %.2f | readback waits %.2f | "
                             "draws per frame %.0f (%.0f on the cpu vertex path)\n",
                     ms[DpCpuVertices], ms[DpVertexData], ms[DpIndices], ms[DpConstants], ms[DpTextures], ms[DpTextureUploads], ms[DpShaders],
                     ms[DpState], ms[DpDrawCall], ms[DpResolve], ms[DpPresent], parts, lastCpMs_, packets, packets - inRenderer, ms[DpReadback],
                     double(draws) / double(presents), double(draws - std::min(draws, gpuVsDraws)) / double(presents));
    }

    uint64_t costCpCycles_ = 0, costMainCycles_ = 0, costTsc_ = 0, costProc100ns_ = 0;
    void ReportFrameCost(double seconds, uint64_t presents)
    {
        ULONG64 cp = 0, mainCycles = 0;
        QueryThreadCycleTime(GetCurrentThread(), &cp);
        if (g_mainThreadHandle) QueryThreadCycleTime(g_mainThreadHandle, &mainCycles);
        const uint64_t tsc = __rdtsc();
        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        const uint64_t proc = (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
        if (costTsc_ && presents)
        {
            const double tscPerMs = double(tsc - costTsc_) / (seconds * 1000.0);
            const double cpMs = double(cp - costCpCycles_) / tscPerMs / double(presents);
            const double mainMs = double(mainCycles - costMainCycles_) / tscPerMs / double(presents);
            const double cores = double(proc - costProc100ns_) / (seconds * 1e7);
            std::sort(gpuFrameMs_.begin(), gpuFrameMs_.end());
            double sum = 0;
            for (float v : gpuFrameMs_) sum += v;
            const size_t n = gpuFrameMs_.size();
            std::fprintf(stderr, "[gpu-hw]   frame cost: gpu %.2f ms between presents (median %.2f, p95 %.2f, max %.2f; %zu frames), "
                         "cpu per frame: game main thread %.2f ms, command processor %.2f ms; process %.2f cores\n",
                         n ? sum / double(n) : 0.0, n ? gpuFrameMs_[n / 2] : 0.0f, n ? gpuFrameMs_[n * 95 / 100] : 0.0f, n ? gpuFrameMs_[n - 1] : 0.0f, n,
                         mainMs, cpMs, cores);
            {
                std::vector<float> done;
                {
                    std::lock_guard l(doneMutex_);
                    done.swap(doneMs_);
                }
                if (!done.empty())
                {
                    std::sort(done.begin(), done.end());
                    double total = 0;
                    for (float v : done) total += v;
                    const size_t k = done.size();
                    std::fprintf(stderr, "[gpu-hw]   GPU done with a frame after its present call: mean %.2f ms (median %.2f, p95 %.2f, max %.2f; %zu frames)\n",
                                 total / double(k), done[k / 2], done[k * 95 / 100], done[k - 1], k);
                }
            }
            DiagFramesReport();
            DiagThreadsReportAsync(tscPerMs, presents, seconds * 1000.0 / double(presents));
            lastCpMs_ = cpMs;
        }
        gpuFrameMs_.clear();
        costCpCycles_ = cp; costMainCycles_ = mainCycles; costTsc_ = tsc; costProc100ns_ = proc;
    }

    // p_diag45: DIAGNOSTIC BUILD A/B (TTT2_DIAG_AB=1 on; diag9's, off since diag10): the new ways (constant ring, indices
    // in the vertex-data ring, write-watch clusters) and the old alternate every report window, so a player's log
    // compares them on his PC. diag10's A/B is kernel_video.cpp's (late frames flipped at once / waiting for the next
    // vblank), or frame_overlap.cpp's (overlapped frames / frames in turn) with TTT2_FRAME_OVERLAP=2.
    bool abNew_ = true;
    void AbSwitch()
    {
        if (frame_overlap::AbOn()) frame_overlap::AbToggle();
        else GpuLateFlipAbToggle();
        KernelFramePriorityAbToggle();                      // diag15: thread priorities new, old, ... (diag16: TTT2_FRAME_PRIORITY=2)
        AudioFlushAbToggle();                               // diag17: sound filters flushed, not, ...
        ++abWindows_;
        if (VdAbOn()) vdAbOff_ = (abWindows_ >> 1) & 1;     // p78: on, on, off, off, ...
        if (post_dof::AbOn()) post_dof::AbToggle();         // p86: depth of field on, off, on, off, ...
        if (IxAbOn())                                       // p104: index cache on, off, on, off, ... (p106: and the
        {                                                   // constant blocks whole in the off windows)
            ixAbOff_ = cbSparseOff_ = (abWindows_ & 1) != 0;
            g_newestBlocksOff = ixAbOff_;                   // p109
            ++cbChunkGen_;
        }
        static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_AB"); return e && *e == '1'; }();
        if (!on) return;
        abNew_ = !abNew_;
        cbRingOn_ = cbRingOk_ && abNew_;
        dataRingIndices_ = dataRingOk_ && abNew_;
        g_wwClustersOn = abNew_;
        ++cbChunkGen_;
        vsConstantsValid_ = constantsValid_ = stateValid_ = false;
        pipe_.valid = false;
    }
    uint32_t statsClockSkip_ = 0;
    void MaybeReportStats()
    {
        if (++statsClockSkip_ & 63) return;     // p_diag41: the clock once in 64 calls (every draw calls)
        const auto now = std::chrono::steady_clock::now();
        if (now - lastStats_ < std::chrono::seconds(5)) return;
        const double s = std::chrono::duration<double>(now - lastStats_).count();
        lastStats_ = now;
        const Stats d{ stats_.draws - reported_.draws, stats_.triangles - reported_.triangles,
                       stats_.skippedDraws - reported_.skippedDraws, stats_.emptyDraws - reported_.emptyDraws,
                       stats_.resolves - reported_.resolves, stats_.presents - reported_.presents,
                       stats_.compiles - reported_.compiles, stats_.diskHits - reported_.diskHits,
                       stats_.uploads - reported_.uploads, stats_.uploadBytes - reported_.uploadBytes,
                       stats_.resolvedHits - reported_.resolvedHits, stats_.writebacks - reported_.writebacks,
                       stats_.depthResolves - reported_.depthResolves, stats_.depthHits - reported_.depthHits,
                       stats_.gpuVsDraws - reported_.gpuVsDraws, stats_.vertexDataBytes - reported_.vertexDataBytes };
        const uint64_t warmHits = stats_.warmHits - reported_.warmHits, pendingSkips = stats_.pendingSkips - reported_.pendingSkips;
        const uint64_t staged = stats_.stagedReadbacks - reported_.stagedReadbacks;
        const uint64_t ringWaits = stats_.ringWaits - reported_.ringWaits;
        const double ringWaitMs = stats_.ringWaitMs - reported_.ringWaitMs;
        const uint64_t resolveCreates = stats_.resolveCreates - reported_.resolveCreates;
        const uint64_t resolvePoolHits = stats_.resolvePoolHits - reported_.resolvePoolHits;
        const uint64_t shadowSkips = stats_.shadowSkips - reported_.shadowSkips;
        reported_ = stats_;
        if (shadowSkips) std::fprintf(stderr, "[gpu-hw]   shadows off: %llu shadow-map draws skipped\n", (unsigned long long)shadowSkips);
        std::fprintf(stderr,
            "[gpu-hw] %.1fs: draws=%llu tris=%llu skipped=%llu empty=%llu resolves=%llu presents=%llu (%.1f/s) "
            "ps_compiles=%llu ps_disk=%llu shader_warm=%llu shader_pending_skips=%llu tex_uploads=%llu (%.1f MB) resolved_hits=%llu writebacks=%llu (staged %llu) "
            "depth_resolves=%llu depth_hits=%llu shaders=%zu textures=%zu gpu_vs_draws=%llu (%.1f MB vertex data, %zu vs) ring_waits=%llu (%.1f ms) resolve_textures=%llu new %llu reused\n",
            s, (unsigned long long)d.draws, (unsigned long long)d.triangles, (unsigned long long)d.skippedDraws,
            (unsigned long long)d.emptyDraws, (unsigned long long)d.resolves, (unsigned long long)d.presents,
            double(d.presents) / s, (unsigned long long)d.compiles, (unsigned long long)d.diskHits, (unsigned long long)warmHits, (unsigned long long)pendingSkips,
            (unsigned long long)d.uploads, double(d.uploadBytes) / 1048576.0, (unsigned long long)d.resolvedHits,
            (unsigned long long)d.writebacks, (unsigned long long)staged, (unsigned long long)d.depthResolves, (unsigned long long)d.depthHits,
            pixelShaders_.size(), textures_.size(), (unsigned long long)d.gpuVsDraws,
            double(d.vertexDataBytes) / 1048576.0, vertexShaders_.size(), (unsigned long long)ringWaits, ringWaitMs,
            (unsigned long long)resolveCreates, (unsigned long long)resolvePoolHits);
        ReportVideoMemory();
        ReportFrameCost(s, d.presents);
        uint64_t flipEntries = 0, lateFlips = 0, pacedFlips = 0;
        GpuTakeFlipStats(flipEntries, lateFlips, pacedFlips);
        uint64_t dofWanted = 0, dofRemoved = 0;
        post_dof::TakeStats(dofWanted, dofRemoved);
        uint64_t dynReduced = 0, dynHeld = 0;
        post_dof::TakeDynStats(dynReduced, dynHeld);
        std::fprintf(stderr, "[diag]   a/b way of this window: %s%s%s; flip-queue entries %llu, late %llu, at once for the game's pace %llu; depth of field asked %llu, removed %llu; "
                             "narrow-width frames %llu, held full %llu\n",
                     frame_overlap::AbOn() ? (frame_overlap::AbTurn() ? "turn" : "overlap")
                     : GpuLateFlipAbOn()   ? (LateFlipOn() ? "late-flip" : "vblank-wait")
                     : abNew_              ? "new" : "old",
                     !VdAbOn() ? "" : vdAbOff_ ? " vdcache-off" : " vdcache-on",
                     (std::string(post_dof::Label()) + (!IxAbOn() ? "" : ixAbOff_ ? " d14-old" : " d14-new") + KernelFramePriorityLabel() + AudioFlushLabel() +
                      ", thread priority requests " + std::to_string(KernelTakePriorityRequests()) +
                      " (system calls " + std::to_string(KernelTakePriorityHostCalls()) + ")").c_str(),
                     (unsigned long long)flipEntries, (unsigned long long)lateFlips, (unsigned long long)pacedFlips,
                     (unsigned long long)dofWanted, (unsigned long long)dofRemoved,
                     (unsigned long long)dynReduced, (unsigned long long)dynHeld);
        {
            uint64_t fx[5] = {};
            post_dof::TakeFxStats(fx);
            std::fprintf(stderr, "[diag]   post filters asked (calls): depth of field %llu, heat haze %llu, glare %llu, SNN %llu, sun shafts %llu\n",
                         (unsigned long long)fx[0], (unsigned long long)fx[1], (unsigned long long)fx[2], (unsigned long long)fx[3],
                         (unsigned long long)fx[4]);
        }
        ReportDiag(d.presents, d.draws, d.gpuVsDraws);
        AbSwitch();
        if (VdCacheAvailable())
        {
            std::fprintf(stderr, "[gpu-hw]   vertex data cache: %llu hits (%.1f MB not copied), %llu copies kept, %llu copied as written lately, "
                                 "%llu small (%.1f MB) copied; %zu ranges known%s\n",
                         (unsigned long long)vdHits_, vdHitBytes_ / 1048576.0, (unsigned long long)vdKept_, (unsigned long long)vdHotCopies_,
                         (unsigned long long)vdSmall_, vdSmallBytes_ / 1048576.0, vdCache_.size(),
                         VdVerifyOn() ? ("; verify: " + std::to_string(vdVerified_) + " hits checked, " + std::to_string(vdDiffer_) + " differ").c_str() : "");
            vdHits_ = vdHitBytes_ = vdKept_ = vdHotCopies_ = vdSmall_ = vdSmallBytes_ = 0;
        }
        if (ixStore_)       // p103
        {
            std::fprintf(stderr, "[gpu-hw]   index cache: %llu hits, %llu read (%llu kept, %.2f MB; %llu written since kept, %llu as written lately), "
                                 "store %.1f of %u MB (round %u)%s%s\n",
                         (unsigned long long)ixHits_, (unsigned long long)ixMisses_, (unsigned long long)ixStored_, ixStoredBytes_ / 1048576.0,
                         (unsigned long long)ixWritten_, (unsigned long long)ixHot_, ixCursor_ / 1048576.0, IxStoreBytes >> 20, ixLap_,
                         ixCleared_ || ixEvicted_ ? (", " + std::to_string(ixCleared_) + " restarts, " + std::to_string(ixEvicted_) + " evictions").c_str() : "",
                         VdVerifyOn() ? ("; verify: " + std::to_string(ixVerified_) + " hits checked, " + std::to_string(ixDiffer_) + " differ").c_str() : "");
            ixHits_ = ixMisses_ = ixStored_ = ixStoredBytes_ = ixWritten_ = ixHot_ = 0;
        }
        if (VcacheStatsOn())
        {
            std::fprintf(stderr, "[gpu-hw]   vertex uploads: %.1f MB unchanged since their last upload, %.1f MB changed, %.1f MB first seen; %llu ranges synced (%zu known)\n",
                         vcacheSame_ / 1048576.0, vcacheChanged_ / 1048576.0, vcacheNew_ / 1048576.0, (unsigned long long)vcacheSyncs_, vcacheSeen_.size());
            vcacheSame_ = vcacheChanged_ = vcacheNew_ = vcacheSyncs_ = 0;
        }
        if (CallCountOn())
        {
            std::map<std::string, uint64_t> counts;
            {
                std::lock_guard l(g_callCountMutex);
                counts.swap(g_callCounts);
            }
            std::string line = "[gpu-hw]   d3d calls:";
            for (const auto& [label, n] : counts) line += " " + label + "=" + std::to_string(n);
            std::fprintf(stderr, "%s\n", line.c_str());
        }
        if (!uploadByFormat_.empty())
        {
            char line[512]; int n = std::snprintf(line, sizeof(line), "[gpu-hw]   uploads by format:");
            for (const auto& [format, v] : uploadByFormat_)
                if (n < 440)
                    n += std::snprintf(line + n, sizeof(line) - n, " %u%s:%llu/%.1fMB", format & 0xFF,
                                       (format & 0x100) ? "t" : "", (unsigned long long)v.first, double(v.second) / 1048576.0);
            std::fprintf(stderr, "%s cache=%.1fMB colour_surfaces=%zu depth_surfaces=%zu resolved=%zu scale=%.2f surfaces=%.0fMB\n", line,
                         double(textureBytes_) / 1048576.0, colors_.size(), depths_.size(), resolved_.size(), scale_,
                         SurfaceMegabytes());
            uploadByFormat_.clear();
        }
        for (const auto& [cause, count] : writebackCauses_)
            std::fprintf(stderr, "[gpu-hw]   writeback x%llu: %s\n", (unsigned long long)count, cause.c_str());
        writebackCauses_.clear();
        for (const auto& [format, count] : undecodable_)
            std::fprintf(stderr, "[gpu-hw]   undecodable texture format %u bound x%llu (constant 0,0,0,1 as on CPU)\n",
                         format, (unsigned long long)count);
        undecodable_.clear();
    }
};

inline Backend& Get()
{
    static Backend* b = new Backend;     // leaked on purpose: no device teardown at exit
    return *b;
}

// Serialises backend use: the command processor thread drives it, but a guest thread's first
// read of a cached-view page asks it for a writeback (WriteBackRange) from that thread.
inline std::recursive_mutex g_backendMutex;

// Run a backend operation; a device-level failure disables the backend for the rest of the
// run (the CPU renderer takes over from the next draw) instead of killing the process.
template <typename F>
inline bool Guarded(const char* what, F&& f)
{
    std::lock_guard<std::recursive_mutex> lock(g_backendMutex);
    try { f(); return true; }
    catch (const std::exception& e)
    {
        g_failed.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "[gpu-hw] DISABLED after failure in %s: %s (device removed reason %08X)\n",
                     what, e.what(), (unsigned)Get().RemovedReason());
        return false;
    }
}

} // namespace ttt2_hw
