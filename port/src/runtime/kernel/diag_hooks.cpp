// Opt-in diagnostic wrappers around individual recompiled game functions. The generated
// code defines every sub_X as a weak alias of __imp__sub_X, so a strong definition here
// takes over all direct and table calls while the original stays reachable as __imp__.
// Each wrapper only observes; it never changes arguments or results.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
void DiagNameThread(const wchar_t* name);   // kernel_thread.cpp (TEST ONLY)
#include "kernel_shared.h"
#include "guest_heap.h"
#include "timeline.h"
#include "log_utf8.h"
#include <cmath>
#include <xmmintrin.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <string>
#include <chrono>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <array>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_82AD8A00);
extern "C" PPC_FUNC(__imp__sub_82BD3DF0);
extern "C" PPC_FUNC(__imp__sub_82BD43C0);
extern "C" PPC_FUNC(__imp__sub_82BD39B0);
extern "C" PPC_FUNC(__imp__sub_82BD4278);

// TTT2_TRACE_COPY=<text> (or hex:<bytes> for binary needles such as a XUID): log every call of
// the game's copy routines whose source starts with the needle - which routine, destination,
// size and the guest call chain - to find where a value (e.g. the default name "Player") is put
// into a structure. Routines: memcpy 82BD3DF0, strncpy 82BD43C0, memmove 82BD39B0, memcpy (2)
// 82BD4278; all take r3 dst, r4 src, r5 size.
static void TraceCopy(const char* routine, PPCContext& ctx, uint8_t* base)
{
    static std::string needleBytes = [] {
        const char* e = std::getenv("TTT2_TRACE_COPY");
        if (!e) return std::string();
        if (std::strncmp(e, "hex:", 4) != 0) return std::string(e);
        std::string b;
        for (const char* p = e + 4; p[0] && p[1]; p += 2) b.push_back(char(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
        return b;
    }();
    const size_t needleLen = needleBytes.size();
    if (!needleLen || ctx.r5.u32 < needleLen || ctx.r4.u32 < 0x1000 ||
        std::memcmp(base + ctx.r4.u32, needleBytes.data(), needleLen) != 0)
        return;
    static std::atomic<int> logged{ 0 };
    if (logged++ >= 20000) return;
    char line[400];
    int n = std::snprintf(line, sizeof(line), "[copy] %s '%s' %08X -> %08X (%u bytes) lr=%08X chain:", routine,
                          std::getenv("TTT2_TRACE_COPY"), ctx.r4.u32, ctx.r3.u32, ctx.r5.u32, uint32_t(ctx.lr));
    uint32_t frame = ctx.r1.u32;
    for (int i = 0; i < 10 && frame && frame < 0xFFFFFFF0u && n < 360; ++i)
    {
        const uint32_t prev = GRead32(base, frame);
        if (!prev || prev <= frame || prev - frame > 0x10000) break;
        n += std::snprintf(line + n, sizeof(line) - n, " %08X", GRead32(base, prev - 8));
        frame = prev;
    }
    std::fprintf(stderr, "%s\n", line);
}

PPC_FUNC(sub_82BD3DF0) { TraceCopy("memcpy", ctx, base); __imp__sub_82BD3DF0(ctx, base); }
PPC_FUNC(sub_82BD43C0) { TraceCopy("strncpy", ctx, base); __imp__sub_82BD43C0(ctx, base); }
PPC_FUNC(sub_82BD39B0) { TraceCopy("memmove", ctx, base); __imp__sub_82BD39B0(ctx, base); }
PPC_FUNC(sub_82BD4278) { TraceCopy("memcpy2", ctx, base); __imp__sub_82BD4278(ctx, base); }

namespace {
bool DiagBake()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_BAKE"); return e && *e == '1'; }();
    return on;
}

uint32_t NonZeroWords(uint8_t* base, uint32_t addr, uint32_t pitch, uint32_t rows, uint32_t rowBytes)
{
    uint32_t n = 0;
    for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t x = 0; x < rowBytes; x += 4)
            n += *reinterpret_cast<uint32_t*>(base + addr + y * pitch + x) != 0;
    return n;
}
}

// Texture recompression dispatcher (CharacterTextureBake output -> DXTn). On entry:
// r4 destination bits, r5 source bits, r6 width, r7 height, r8 source pitch, r9 format.
PPC_FUNC(sub_82AD8A00)
{
    static uint32_t calls = 0;                     // guest calls are serialised per caller
    const uint32_t n = calls++;
    if (DiagBake() && (n < 64 || (n & 63) == 0))    // all early calls, then a sample
    {
        const uint32_t src = ctx.r5.u32, w = ctx.r6.u32, h = ctx.r7.u32, pitch = ctx.r8.u32;
        const uint32_t rows = h < 4096 ? h : 0, rowBytes = pitch < 0x4000 ? (w * 4 < pitch ? w * 4 : pitch) : 0;
        std::fprintf(stderr, "[diag-bake] recompress #%u dst=%08X src=%08X %ux%u pitch=%u fmt=%u r10=%08X "
            "src_nonzero=%u/%u first=%08X %08X\n", n, ctx.r4.u32, src, w, h, pitch, ctx.r9.u32, ctx.r10.u32,
            NonZeroWords(base, src, pitch, rows, rowBytes), rows * (rowBytes / 4),
            GRead32(base, src), GRead32(base, src + 4));
    }
    __imp__sub_82AD8A00(ctx, base);
}

// CPU skinning kernels (vtable slot 3 of the skinning job classes, table at 0x82232EA0): job
// r3 = {+4 palette, +8 palette, +12 bone base, +16 output, +20 input, +24 vertex count}; output
// stride per kernel (16..64 bytes, from each kernel's output increment). TTT2_DIAG_SKIN=1 logs jobs whose output range overlaps another recent job
// on a different input: the thin streak lines are vertices one job writes into another's mesh.
namespace {
struct SkinJob { uint32_t job, out, end, in, lr, tid; uint64_t seq; uint32_t count, stride, palA, palB, boneBase, boneCount, kernel; };
std::mutex g_skinMutex;
SkinJob g_skinRecent[512];
uint64_t g_skinSeq = 0;
int g_skinLogged = 0;

bool DiagSkin()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_SKIN"); return e && *e == '1'; }();
    return on;
}

void NoteSkinJob(PPCContext& ctx, uint8_t* base, const char* kernel, uint32_t stride, uint32_t kernelAddr)
{
    const uint32_t job = ctx.r3.u32;
    const uint32_t count = GRead32(base, job + 24);
    const SkinJob cur{ job, GRead32(base, job + 16), GRead32(base, job + 16) + count * stride,
                       GRead32(base, job + 20), uint32_t(ctx.lr), GetCurrentThreadId(), 0,
                       count, stride, GRead32(base, job + 4), GRead32(base, job + 8),
                       GRead16(base, job + 12), GRead16(base, job + 14), kernelAddr };
    std::lock_guard lock(g_skinMutex);
    const uint64_t seq = ++g_skinSeq;
    for (const SkinJob& o : g_skinRecent)
    {
        if (!o.seq || seq - o.seq > 512 || g_skinLogged >= 80) continue;
        if (o.in != cur.in && o.out < cur.end && cur.out < o.end)
        {
            ++g_skinLogged;
            std::fprintf(stderr, "[diag-skin] OVERLAP %s job=%08X out=%08X..%08X in=%08X lr=%08X tid=%u"
                                 " | earlier(-%llu) job=%08X out=%08X..%08X in=%08X lr=%08X tid=%u\n",
                         kernel, cur.job, cur.out, cur.end, cur.in, cur.lr, cur.tid,
                         (unsigned long long)(seq - o.seq), o.job, o.out, o.end, o.in, o.lr, o.tid);
        }
    }
    SkinJob& slot = g_skinRecent[seq % 512];
    slot = cur;
    slot.seq = seq;
}
}

#define TTT2_SKIN_HOOK(addr, stride)                                                 \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                    \
    PPC_FUNC(sub_##addr)                                                       \
    {                                                                          \
        const timeline::Scope skinMark{ timeline::SkinJob, 0x##addr & 0xFFFFu };          \
        if (timeline::On()) timeline::Mark(timeline::SkinOut, '.', GRead32(base, ctx.r3.u32 + 16));   /* TEST ONLY */ \
        if (DiagSkin()) NoteSkinJob(ctx, base, #addr, stride, 0x##addr);                 \
        __imp__sub_##addr(ctx, base);                                          \
    }
TTT2_SKIN_HOOK(82B10800, 64)
TTT2_SKIN_HOOK(82B10908, 16)
TTT2_SKIN_HOOK(82B10A70, 32)
TTT2_SKIN_HOOK(82B10C88, 48)
TTT2_SKIN_HOOK(82B10F40, 64)
TTT2_SKIN_HOOK(82B11288, 20)
TTT2_SKIN_HOOK(82B11500, 36)
TTT2_SKIN_HOOK(82B118C0, 16)

// Dynamic vertex allocator (sub_82AD46F0 -> sub_82AD5B40: a plain cursor bump at this+24, no
// lock). TTT2_DIAG_SKIN=1 also reports two threads inside it at once (thread, guest CPU byte
// KPCR+0x10C, caller), which hands both callers the same output range.
extern "C" PPC_FUNC(__imp__sub_82AD46F0);
namespace {
std::atomic<int> g_allocInside{0};
std::atomic<uint32_t> g_allocOwner{0}, g_allocOwnerLr{0}, g_allocOwnerCpu{0};
std::atomic<int> g_allocLogged{0};
}
// TEST ONLY (timeline.h: input latency work): D3D's doorbell write (CP_RB_WPTR, r4 = the new write pointer) - which
// thread hands the command processor each command segment, and when.
extern "C" PPC_FUNC(__imp__sub_82739210);
static void MarkRingKick(PPCContext& ctx, uint8_t* base, uint32_t value)
{
    timeline::Mark(timeline::RingKick, '.', value);
    if (!timeline::On()) return;
    uint32_t frame = ctx.r1.u32;                          // the guest callers: saved LRs up the back chain
    timeline::Mark(timeline::RingKickLr, '.', uint32_t(ctx.lr));
    for (int i = 0; i < 10 && frame; ++i)
    {
        const uint32_t up = GRead32(base, frame);
        if (up <= frame || up - frame > 0x10000) break;
        timeline::Mark(timeline::RingKickLr, '.', GRead32(base, up - 8));
        frame = up;
    }
}
PPC_FUNC(sub_82739210)                                    // D3D's segment kick (the other doorbell writer)
{
    MarkRingKick(ctx, base, 0x10000000u | ctx.r4.u32);
    __imp__sub_82739210(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_827389F8);
PPC_FUNC(sub_827389F8)
{
    timeline::Mark(timeline::RingKick, '.', ctx.r4.u32);
    if (timeline::On())                                   // the guest callers: saved LRs up the back chain
    {
        uint32_t frame = ctx.r1.u32;
        timeline::Mark(timeline::RingKickLr, '.', uint32_t(ctx.lr));
        for (int i = 0; i < 10 && frame; ++i)
        {
            const uint32_t up = GRead32(base, frame);
            if (up <= frame || up - frame > 0x10000) break;
            timeline::Mark(timeline::RingKickLr, '.', GRead32(base, up - 8));
            frame = up;
        }
    }
    __imp__sub_827389F8(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82AD4780);
PPC_FUNC(sub_82AD4780)                                // TEST ONLY (timeline.h: input latency work)
{
    if (ctx.r3.u32 == 0x82E40580u) timeline::Mark(timeline::RingAlloc, '.', uint32_t(ctx.lr));
    __imp__sub_82AD4780(ctx, base);
}
PPC_FUNC(sub_82AD46F0)
{
    if (ctx.r3.u32 == 0x82E40580u) timeline::Mark(timeline::RingAlloc, '.', uint32_t(ctx.lr));   // TEST ONLY
    if (!DiagSkin()) { __imp__sub_82AD46F0(ctx, base); return; }
    const uint32_t tid = GetCurrentThreadId(), lr = uint32_t(ctx.lr);
    const uint32_t cpu = ctx.r13.u32 ? base[ctx.r13.u32 + 0x10C] : 0xFF;
    if (g_allocInside.fetch_add(1) > 0 && g_allocLogged.fetch_add(1) < 40)
        std::fprintf(stderr, "[diag-skin] CONCURRENT alloc this=%08X size=%u tid=%u cpu=%u lr=%08X while tid=%u cpu=%u lr=%08X\n",
                     ctx.r3.u32, ctx.r4.u32, tid, cpu, lr, g_allocOwner.load(), g_allocOwnerCpu.load(), g_allocOwnerLr.load());
    g_allocOwner = tid; g_allocOwnerLr = lr; g_allocOwnerCpu = cpu;
    __imp__sub_82AD46F0(ctx, base);
    g_allocInside.fetch_sub(1);
}

// Skinned-vertex ring advance (sub_82AD5BA0: this+20 index mod this+68 regions, cursor = base).
// A region written in frame n is reused in frame n + regions; the GPU must have drawn it by
// then. TTT2_DIAG_SKIN=1 compares, at each advance, the game's swaps issued two advances
// earlier with the frames the command processor has presented: a positive deficit means the
// CPU is about to overwrite vertices the GPU emulation has not drawn yet.
extern "C" PPC_FUNC(__imp__sub_82AD5BA0);
namespace frame_overlap { void RingAdvanced(uint8_t* base, uint32_t ring); }   // frame_overlap.cpp
extern std::atomic<uint64_t> g_cpPresents, g_gameSwaps;
namespace {
std::mutex g_ringMutex;
uint64_t g_ringSwaps[4] = {};
uint64_t g_ringAdvances = 0, g_ringDeficitHist[6] = {};
}
void ResimScratchAdvance();   // resim_scratch.cpp: a new scratch slot while re-simulating
PPC_FUNC(sub_82AD5BA0)
{
    ResimScratchAdvance();
    // TTT2_RB_GPUMEM=1: each ring object once (regions +68, size +4, bases +8...).
    static const bool gpuMem = [] { const char* e = std::getenv("TTT2_RB_GPUMEM"); return e && *e == '1'; }();
    if (gpuMem)
    {
        static std::mutex m;
        static std::set<uint32_t> seen;
        std::lock_guard l(m);
        if (seen.insert(ctx.r3.u32).second)
        {
            const uint32_t o = ctx.r3.u32, n = GRead32(base, o + 68);
            char line[512];
            int k = std::snprintf(line, sizeof(line), "[gpu-mem] vertex ring %08X: %u regions of %08X bytes, index %u, cursor %08X, bases",
                                  o, n, GRead32(base, o + 4), GRead32(base, o + 20), GRead32(base, o + 24));
            for (uint32_t i = 0; i < n && i < 12 && k < 450; ++i) k += std::snprintf(line + k, sizeof(line) - k, " %08X", GRead32(base, o + 8 + 4 * i));
            std::fprintf(stderr, "%s (caller %08X)\n", line, uint32_t(ctx.lr));
        }
    }
    if (DiagSkin())
    {
        std::lock_guard lock(g_ringMutex);
        const uint64_t n = g_ringAdvances++;
        const uint64_t swapsNow = g_gameSwaps.load(), presented = g_cpPresents.load();
        if (n >= 2)
        {
            const uint64_t needed = g_ringSwaps[(n - 2) & 3];
            const int64_t deficit = int64_t(needed) - int64_t(presented);
            ++g_ringDeficitHist[deficit <= 0 ? 0 : deficit >= 5 ? 5 : deficit];
        }
        g_ringSwaps[n & 3] = swapsNow;
        if (n % 600 == 599)
            std::fprintf(stderr, "[diag-skin] ring advances=%llu swaps=%llu presented=%llu deficit hist 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu\n",
                         (unsigned long long)(n + 1), (unsigned long long)swapsNow, (unsigned long long)presented,
                         (unsigned long long)g_ringDeficitHist[0], (unsigned long long)g_ringDeficitHist[1],
                         (unsigned long long)g_ringDeficitHist[2], (unsigned long long)g_ringDeficitHist[3],
                         (unsigned long long)g_ringDeficitHist[4], (unsigned long long)g_ringDeficitHist[5]);
    }
    const uint32_t ring = ctx.r3.u32;
    __imp__sub_82AD5BA0(ctx, base);
    if (ring == 0x82E40580u) timeline::Mark(timeline::RingAdvance, '.', GRead32(base, ring + 20));   // TEST ONLY
    frame_overlap::RingAdvanced(base, ring);      // frame_overlap.cpp: a fourth skinning region
}

// Frame-log helper: describe the skinning job (most recent covering `addr`) and, for the
// 4-bone kernel (96-byte input: +64 four bone indices, +80 four weights), the source record
// of the vertex written at `addr`. Returns false when no recorded job covers it.
bool DiagSkinDescribe(uint32_t addr, char* out, size_t size)
{
    std::lock_guard lock(g_skinMutex);
    const SkinJob* best = nullptr;
    for (const SkinJob& j : g_skinRecent)
        if (j.seq && j.out <= addr && addr < j.end && (!best || j.seq > best->seq)) best = &j;
    if (!best) return false;
    uint8_t* base = g_base;
    const uint32_t stride = best->stride;
    const uint32_t vertex = stride ? (addr - best->out) / stride : 0;
    int n = std::snprintf(out, size, "job=%08X stride=%u vertex %u/%u bones [%u,+%u) palA=%08X palB=%08X in=%08X tid=%u",
                          best->job, stride, vertex, best->count, best->boneBase, best->boneCount,
                          best->palA, best->palB, best->in, best->tid);
    if (stride == 64 && n > 0 && size_t(n) < size)
    {
        const uint32_t rec = best->in + vertex * 96;
        float w[4];
        for (int k = 0; k < 4; ++k) { const uint32_t raw = GRead32(base, rec + 80 + k * 4); std::memcpy(&w[k], &raw, 4); }
        n += std::snprintf(out + n, size - n, " idx=%u,%u,%u,%u w=%.3g,%.3g,%.3g,%.3g",
                           GRead32(base, rec + 64), GRead32(base, rec + 68), GRead32(base, rec + 72), GRead32(base, rec + 76),
                           w[0], w[1], w[2], w[3]);
        // Re-run the game's 4-bone kernel now on this one vertex (same input record and
        // palettes, scratch output): same result = the inputs give this position; different =
        // the palettes changed after the job ran (it read them mid-update).
        if (best->kernel == 0x82B10F40u && n > 0 && size_t(n) < size)
        {
            static uint32_t scratch = 0;
            if (!scratch)
                if (void* p = g_virtualHeap.Alloc(0x11000, 0x100)) scratch = GuestAddr(base, p);
            if (scratch)
            {
                GWrite32(base, scratch + 4, best->palA);
                GWrite32(base, scratch + 8, best->palB);
                GWrite16(base, scratch + 12, uint16_t(best->boneBase));
                GWrite16(base, scratch + 14, uint16_t(best->boneCount));
                GWrite32(base, scratch + 16, scratch + 0x40);
                GWrite32(base, scratch + 20, rec);
                GWrite32(base, scratch + 24, 1);
                // Guest FP/VMX code loads MXCSR from the context; keep the host thread's own
                // (a zeroed context unmasks every FP exception: C000008F on the next op).
                const unsigned int hostCsr = _mm_getcsr();
                PPCContext c{};
                c.fpscr.csr = hostCsr;
                c.r3.u32 = scratch;
                c.r1.u32 = scratch + 0x11000 - 0x100;
                __imp__sub_82B10F40(c, base);
                _mm_setcsr(hostCsr);
                float a[3], b[3];
                for (int k = 0; k < 3; ++k)
                {
                    const uint32_t ra = GRead32(base, scratch + 0x40 + k * 4), rb = GRead32(base, addr + k * 4);
                    std::memcpy(&a[k], &ra, 4); std::memcpy(&b[k], &rb, 4);
                }
                const float d = std::fabs(a[0] - b[0]) + std::fabs(a[1] - b[1]) + std::fabs(a[2] - b[2]);
                std::snprintf(out + n, size - n, " rerun=(%.0f,%.0f,%.0f) %s", a[0], a[1], a[2], d < 1.0f ? "same" : "DIFF");
            }
        }
    }
    return true;
}

// Frame pacing (TTT2_FRAMETIME=1): how long the main thread waits for TTT2's job pool.
// sub_824F58B8(job) kicks a job of the pool threads (sub_824F5458, which poll for work with
// Sleep(1) while idle) and waits for it by entering the job's lock (+24). Every 10 s: waits,
// total and longest wait (ms).
void RollbackNoteJobObject(uint32_t addr, uint32_t size);   // rollback.cpp
void RollbackJobTrace(const char* what, uint32_t obj, bool after);
bool RollbackInResimDraw();
void RollbackResimTime(int id, double ms);
struct ResimTimer
{
    int id;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~ResimTimer() { if (RollbackInResimDraw()) RollbackResimTime(id, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()); }
};
// The pool job threads' loop (sub_824F5458, r3 = the job): rollback's job wait learns which host thread
// runs which job (rollback.cpp RollbackNotePoolThread).
void RollbackNotePoolThread(uint32_t job);
void RollbackNotePoolThreadExit(uint32_t job);
extern "C" PPC_FUNC(__imp__sub_824F5458);
PPC_FUNC(sub_824F5458)
{
    const uint32_t job = ctx.r3.u32;
    RollbackNotePoolThread(job);
    __imp__sub_824F5458(ctx, base);
    RollbackNotePoolThreadExit(job);
}
extern "C" PPC_FUNC(__imp__sub_824F58B8);
PPC_FUNC(sub_824F58B8)
{
    RollbackNoteJobObject(ctx.r3.u32, 0xA8);
    RollbackJobTrace("kick", ctx.r3.u32, false);
    const ResimTimer resimTimer{ 1 };
    static const bool on = [] { const char* e = std::getenv("TTT2_FRAMETIME"); return e && *e == '1'; }();
    if (!on)
    {
        __imp__sub_824F58B8(ctx, base);
        return;
    }
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    __imp__sub_824F58B8(ctx, base);
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - start).count();
    static std::mutex m;
    static uint64_t waits = 0;
    static double total = 0, longest = 0;
    static clock::time_point report = clock::now();
    std::lock_guard lock(m);
    ++waits;
    total += ms;
    longest = (std::max)(longest, ms);
    if (clock::now() - report >= std::chrono::seconds(10))
    {
        std::fprintf(stderr, "[frame] job-pool waits 10s: %llu waits, %.0f ms total, mean %.3f ms, longest %.1f ms\n",
                     (unsigned long long)waits, total, waits ? total / double(waits) : 0.0, longest);
        waits = 0;
        total = longest = 0;
        report = clock::now();
    }
}

// Frame timeline (TTT2_TIMELINE=1, timeline.h): the game-side waits of a frame.
// sub_82ACFF90(obj): main thread, polls obj+8 (busy) with 1 ms waits on the event at obj+196.
// sub_82AE4438(monitor, target): job worker, waits until the monitor's 64-bit count = target
// (marked in its replacement, guest_monitor.cpp).
// sub_82738DA0: D3D's wait for the GPU (loops on sub_8273D4A0).
extern "C" PPC_FUNC(__imp__sub_82ACFF90);
PPC_FUNC(sub_82ACFF90)
{
    RollbackNoteJobObject(ctx.r3.u32, 0x100);
    const uint32_t obj = ctx.r3.u32;
    RollbackJobTrace("group-wait", obj, false);
    const ResimTimer resimTimer{ 0 };
    const timeline::Scope mark{ timeline::MainResourceWait, ctx.r3.u32 };
    __imp__sub_82ACFF90(ctx, base);
    RollbackJobTrace("group-wait", obj, true);
}
extern "C" PPC_FUNC(__imp__sub_82AD06F8);
PPC_FUNC(sub_82AD06F8)
{
    const uint32_t obj = ctx.r3.u32;
    RollbackJobTrace("dispatch", obj, false);
    const ResimTimer resimTimer{ 2 };
    __imp__sub_82AD06F8(ctx, base);
    RollbackJobTrace("dispatch", obj, true);
}
extern "C" PPC_FUNC(__imp__sub_82AD1B88);
PPC_FUNC(sub_82AD1B88)
{
    const uint32_t obj = ctx.r3.u32;
    const timeline::Scope tlKick{ timeline::FrameKick };   // TEST ONLY (timeline.h: input latency work)
    RollbackJobTrace("frame-kick", obj, false);
    const ResimTimer resimTimer{ 3 };
    __imp__sub_82AD1B88(ctx, base);
    RollbackJobTrace("frame-kick", obj, true);
}
#define RESIM_TIMED(addr, id) \
    extern "C" PPC_FUNC(__imp__sub_##addr); \
    PPC_FUNC(sub_##addr) { const ResimTimer resimTimer{ id }; __imp__sub_##addr(ctx, base); }
// DIAGNOSTIC BUILD (p_diag47): the frame kick sub_82AD1B88 calls sub_82AD1DE8(jobs, 1), which waits for the job
// workers and then inserts a GPU fence and blocks on it (sub_8273A288 + sub_82738E70): every frame the game thread
// waits until the command processor has run everything submitted so far. Wall time of the whole call, of its GPU
// part and of the kick thread's other fence waits, per frame in the 5-s report (gpu_hw_d3d11.h).
// TTT2_DIAG_NOIDLE=1 (EXPERIMENT ONLY): the GPU part waits for the previous kick's fence instead of this one.
std::atomic<int64_t> g_diagKickNs{0}, g_diagKickGpuNs{0}, g_diagOtherGpuNs{0};
std::atomic<uint32_t> g_diagKicks{0};
static std::atomic<uint32_t> g_diagKickThread{0};
static thread_local bool t_diagInKick = false;
struct DiagWaitSite { uint32_t lr = 0, lr2 = 0; int64_t ns = 0; uint32_t n = 0; };
static std::mutex g_diagSiteMutex;
static DiagWaitSite g_diagSites[16];
static int64_t DiagNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void GpuMemKickRelease();   // kernel_video.cpp (p_diag48, TTT2_RB_GPUMEM=1)
void GpuMemKickCheck();
bool LowLatencyEarlyWait();                 // low_latency.cpp: its wait before the swap, not the kick's own
void LowLatencyNoteKickWait(double ms);
extern "C" PPC_FUNC(__imp__sub_82AD1DE8);
PPC_FUNC(sub_82AD1DE8)
{
    const ResimTimer resimTimer{ 6 };
    const bool gpu = ctx.r4.u32 != 0, kick = gpu && !LowLatencyEarlyWait();
    if (kick) { g_diagKickThread.store(GetCurrentThreadId(), std::memory_order_relaxed); g_diagKicks.fetch_add(1, std::memory_order_relaxed); }
    if (kick) GpuMemKickCheck();
    // p_diag51 TEST ONLY: TTT2_DIAG_MAIN_SPIN_US adds a busy wait to the game thread's frame (a slower PC's game thread);
    // TTT2_DIAG_MAIN_SPIN_FILE=1 takes the microseconds from C:\ttt2\port\main_spin.txt instead (read every 60 kicks, none
    // without the file: a PC that gets slower mid-fight).
    static int64_t mainSpinNs = [] { const char* e = std::getenv("TTT2_DIAG_MAIN_SPIN_US"); return e ? std::atoll(e) * 1000 : 0LL; }();
    static const bool spinFile = [] { const char* e = std::getenv("TTT2_DIAG_MAIN_SPIN_FILE"); return e && *e == '1'; }();
    static uint32_t spinKicks = 0;
    if (kick && spinFile && spinKicks++ % 60 == 0)
    {
        long long us = 0;
        if (FILE* f = std::fopen("C:/ttt2/port/main_spin.txt", "r")) { if (std::fscanf(f, "%lld", &us) != 1) us = 0; std::fclose(f); }
        if (us * 1000 != mainSpinNs) std::fprintf(stderr, "[diag] TEST main thread spin -> %lld us a frame\n", us);
        mainSpinNs = us * 1000;
    }
    if (kick && mainSpinNs > 0)
    {
        const int64_t until = DiagNowNs() + mainSpinNs;
        while (DiagNowNs() < until) YieldProcessor();
    }
    const int64_t t0 = DiagNowNs();
    t_diagInKick = gpu;
    __imp__sub_82AD1DE8(ctx, base);
    t_diagInKick = false;
    const int64_t ns = DiagNowNs() - t0;
    if (kick) { g_diagKickNs.fetch_add(ns, std::memory_order_relaxed); LowLatencyNoteKickWait(double(ns) / 1e6); }
    if (kick) GpuMemKickRelease();
}
// rollback.cpp's sub_82738E70 hook calls this for the frame kick's fence wait.
extern "C" PPC_FUNC(__imp__sub_82738E70);
bool DiagInKick() { return t_diagInKick; }
namespace frame_overlap {
uint32_t KickFence(uint8_t* base, uint32_t fence);
void AfterKickWait(uint32_t fence);
void NoteDevice(uint32_t device);
}   // frame_overlap.cpp
void DiagKickFenceWait(PPCContext& ctx, uint8_t* base)
{
    if (LowLatencyEarlyWait()) { __imp__sub_82738E70(ctx, base); return; }   // frame overlap is off with low latency
    const uint32_t fence = ctx.r3.u32;
    ctx.r3.u64 = frame_overlap::KickFence(base, fence);         // frame_overlap.cpp: in a round, the previous kick's
    const int64_t t0 = DiagNowNs();
    __imp__sub_82738E70(ctx, base);
    g_diagKickGpuNs.fetch_add(DiagNowNs() - t0, std::memory_order_relaxed);
    frame_overlap::AfterKickWait(fence);                        // this kick's frame in flight: its write guard
}
// Options -> Display Settings -> LOW LATENCY (low_latency.cpp): the frame kick's hand-over of the render list
// (sub_82AD2CC0, lr 82AD1BD0), its release of the render thread (sub_82AD1DD8, lr 82AD1D3C) and the render thread's
// end of a list (sub_82AE43E0(+120, 0), lr 82AD2B88).
void LowLatencyHandOver(PPCContext& ctx, uint8_t* base, uint32_t renderContext);
bool LowLatencyKickRelease();
void LowLatencyListDrawn();
extern "C" PPC_FUNC(__imp__sub_82AD2CC0);
extern "C" PPC_FUNC(__imp__sub_82AD1DD8);
extern "C" PPC_FUNC(__imp__sub_82AE43E0);
PPC_FUNC(sub_82AD1DD8)
{
    if (uint32_t(ctx.lr) == 0x82AD1D3Cu && !LowLatencyKickRelease()) return;   // the list was drawn before the swap
    __imp__sub_82AD1DD8(ctx, base);
}
PPC_FUNC(sub_82AD2CC0)
{
    const ResimTimer resimTimer{ 7 };
    const uint32_t renderContext = ctx.r3.u32, lr = uint32_t(ctx.lr);
    __imp__sub_82AD2CC0(ctx, base);
    if (lr == 0x82AD1BD0u && int32_t(ctx.r3.u32) >= 0) LowLatencyHandOver(ctx, base, renderContext);
}
PPC_FUNC(sub_82AE43E0)
{
    const ResimTimer resimTimer{ 10 };
    if (uint32_t(ctx.lr) == 0x82AD2B88u) LowLatencyListDrawn();
    __imp__sub_82AE43E0(ctx, base);
}
// The frame kick's swap before its wait for the vblank after the swap (lr 82AD1C2C; the kick's r31 = its pacer). That
// wait (sub_82AD1AD0) passes once the pacer's monitor (+112, value at +128: vblanks since the last kick, counted by the
// vblank thread sub_82AD17A8 and reset by the kick) is above the pacer's +16, one less while 0x82E40BA0 is 0. When it
// is above already, the game moves on at once, and so does this frame's flip-queue entry (kernel_video.cpp).
void GpuKickSwapFlipAtOnce(bool atOnce);
extern "C" PPC_FUNC(__imp__sub_82AD4D98);
PPC_FUNC(sub_82AD4D98)
{
    const ResimTimer resimTimer{ 8 };
    if (uint32_t(ctx.lr) == 0x82AD1C2Cu)
    {
        const auto read64 = [&](uint32_t a) { return int64_t(uint64_t(GRead32(base, a)) << 32 | GRead32(base, a + 4)); };
        const uint32_t pacer = ctx.r31.u32;
        const int64_t target = read64(pacer + 16) - (GRead32(base, 0x82E40BA0) ? 0 : 1);
        GpuKickSwapFlipAtOnce(read64(pacer + 128) > target);
    }
    __imp__sub_82AD4D98(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82AD0AF0);
PPC_FUNC(sub_82AD0AF0)
{
    const uint32_t obj = ctx.r3.u32;
    RollbackJobTrace("submit", obj, false);
    const ResimTimer resimTimer{ 4 };
    __imp__sub_82AD0AF0(ctx, base);
    RollbackJobTrace("submit", obj, true);
}
extern "C" PPC_FUNC(__imp__sub_82738DA0);
PPC_FUNC(sub_82738DA0)
{
    const timeline::Scope mark{ timeline::MainGpuWait, ctx.r4.u32 };
    if (t_diagInKick) frame_overlap::NoteDevice(ctx.r3.u32);    // the frame overlap's fence test
    if (t_diagInKick || GetCurrentThreadId() != g_diagKickThread.load(std::memory_order_relaxed)) { __imp__sub_82738DA0(ctx, base); return; }
    const uint32_t lr = uint32_t(ctx.lr), frame = ctx.r1.u32;
    const int64_t t0 = DiagNowNs();
    __imp__sub_82738DA0(ctx, base);
    const int64_t ns = DiagNowNs() - t0;
    g_diagOtherGpuNs.fetch_add(ns, std::memory_order_relaxed);
    // The caller (lr) and its caller (the saved lr in the caller's frame, backchain), for the report's top sites.
    uint32_t lr2 = 0;
    const uint32_t prev = frame ? GRead32(base, frame) : 0;
    if (prev > frame && prev - frame < 0x10000) { const uint32_t up = GRead32(base, prev); if (up > prev && up - prev < 0x10000) lr2 = GRead32(base, up - 8); }
    std::lock_guard lock(g_diagSiteMutex);
    DiagWaitSite* slot = nullptr;
    for (DiagWaitSite& s : g_diagSites)
        if ((s.lr == lr && s.lr2 == lr2) || (!slot && !s.lr)) { slot = &s; if (s.lr == lr) break; }
    if (slot) { slot->lr = lr; slot->lr2 = lr2; slot->ns += ns; ++slot->n; }
}
// The report's "other GPU waits" sites, longest first; clears them.
std::string DiagTakeWaitSites(double presents)
{
    std::lock_guard lock(g_diagSiteMutex);
    std::vector<DiagWaitSite> v(std::begin(g_diagSites), std::end(g_diagSites));
    for (DiagWaitSite& s : g_diagSites) s = DiagWaitSite{};
    std::sort(v.begin(), v.end(), [](const DiagWaitSite& a, const DiagWaitSite& b) { return a.ns > b.ns; });
    std::string out;
    for (size_t i = 0; i < v.size() && i < 4 && v[i].lr; ++i)
    {
        char t[96];
        std::snprintf(t, sizeof(t), "%s%08X<%08X %.2f ms x%.2f", out.empty() ? "" : ", ", v[i].lr, v[i].lr2,
                      double(v[i].ns) / 1e6 / presents, double(v[i].n) / presents);
        out += t;
    }
    return out;
}

// TTT2_FRAMETIME=1: guest functions on the main thread's stack during the ~100 ms fight hitch
// (profile 2026-10-14) - each call over 8 ms is logged with its thread.
static void TimeCall(const char* name, PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*))
{
    static const bool on = [] { const char* e = std::getenv("TTT2_FRAMETIME"); return e && *e == '1'; }();
    if (!on) { fn(ctx, base); return; }
    const auto start = std::chrono::steady_clock::now();
    fn(ctx, base);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    static std::atomic<int> logged{ 0 };
    if (ms > 8.0 && logged++ < 400) std::fprintf(stderr, "[frame] slow call %s %.1f ms thread %lu\n", name, ms, GetCurrentThreadId());
}
#define TIMED_HOOK(addr) \
    extern "C" PPC_FUNC(__imp__sub_##addr); \
    PPC_FUNC(sub_##addr) { TimeCall("sub_" #addr, ctx, base, __imp__sub_##addr); }
TIMED_HOOK(82AD8F28)
TIMED_HOOK(82AFB798)
TIMED_HOOK(82B0BD68)
TIMED_HOOK(82C12250)
// TTT2_HITCH_PROFILE: an in-process sampler (profile_threads.py manages ~65 Hz from outside, too
// coarse for one 100 ms frame). A sampler thread suspends a thread, records its guest call chain
// (ctx.lr, then the r1 backchain: saved LR at [backchain - 8]) and its host stack (function starts
// via RtlVirtualUnwind; exe ones print as RVAs for port/tools/rva2sym.py, others as module+RVA).
//   1: the main thread every 0.5 ms while it runs a game task (sub_82A80AD8, hook below); a task
//      over 20 ms prints its profile.
//   2: also the main thread all the time (1 kHz), a profile per 10 s: where frame time goes.
//   3: flight recorder - the main and command-processor threads all the time (1 kHz), and for
//      every presented frame over 25 ms (HitchFrameGap) both threads' profiles over that gap.
//   4: the main and command-processor threads all the time (1 kHz), a profile of each per 10 s.
//   5: the main thread (1 kHz) only while a rollback re-simulation runs, its updates and its draws
//      apart, a profile of each per 10 s.
bool RollbackResimulating();   // rollback.cpp
bool RollbackResimDraw();
namespace hitch {
struct Sample { int64_t at; uint32_t chain[24]; uint32_t depth; uint64_t host[32]; uint32_t hostDepth; };
struct Target
{
    std::string name;
    DWORD tid = 0;
    HANDLE thread = nullptr;
    ULONG_PTR stackLow = 0, stackHigh = 0;
    PPCContext* ctx = nullptr;
    std::vector<Sample> ring;                     // flight recorder (mode 3), sampler thread only
    size_t next = 0;
    std::vector<Sample> window;                   // modes 2 and 4, sampler thread only
};
std::atomic<bool> g_active{ false }, g_busy{ false };
std::mutex g_targetsMutex, g_samplesMutex;
std::vector<Target*> g_targets;                   // [0] = main thread once known
std::atomic<Target*> g_main{ nullptr };
std::vector<Sample> g_samples;   // guest memory: the runtime's ::g_base (set before any thread)
struct Gap { int64_t end; double ms; };
std::mutex g_gapsMutex;
std::vector<Gap> g_gaps;
std::atomic<int> g_gapReports{ 0 };

int Mode()
{
    static const int mode = [] {
        const char* e = std::getenv("TTT2_HITCH_PROFILE");
        return e && *e >= '1' && *e <= '5' ? *e - '0' : 0;
    }();
    return mode;
}
bool On() { return Mode() != 0; }
void Report(const char* header, const std::vector<Sample>& samples, size_t guestTop = 40, size_t hostTop = 40, size_t leafTop = 20);
void Loop();

int64_t Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
int64_t Freq()
{
    static const int64_t f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return q.QuadPart; }();
    return f;
}

// Called on the thread to be sampled.
Target* Register(const char* name, PPCContext* ctx, bool main = false)
{
    auto* t = new Target;
    t->name = name;
    t->tid = GetCurrentThreadId();
    t->ctx = ctx;
    GetCurrentThreadStackLimits(&t->stackLow, &t->stackHigh);
    t->thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    if (Mode() == 3) t->ring.resize(1024);
    static std::once_flag started;
    std::call_once(started, [] { std::thread(Loop).detach(); });
    if (main) g_main = t;
    std::lock_guard l(g_targetsMutex);
    g_targets.push_back(t);
    return t;
}

uint32_t FunctionOf(uint32_t address)
{
    static const size_t count = [] { size_t n = 0; while (PPCFuncMappings[n].guest) ++n; return n; }();
    size_t lo = 0, hi = count;
    while (lo < hi)
    {
        const size_t mid = (lo + hi) / 2;
        if (PPCFuncMappings[mid].guest <= address) lo = mid + 1;
        else hi = mid;
    }
    return lo ? PPCFuncMappings[lo - 1].guest : 0;
}

// kernel_thread.cpp's registry: host thread -> the PPCContext of its current guest call.
struct GuestContextSlot { uint32_t tid; uint32_t entry; PPCContext* ctx; };
extern "C" struct GuestContextRegistry
{
    uint32_t magic, version, r1Offset, lrOffset, capacity;
    std::atomic<uint32_t> count;
    GuestContextSlot slots[512];
} g_guestContextRegistry;

uint32_t GuestEntryOfThread(DWORD tid)   // rollback.cpp diagnostics
{
    const uint32_t n = (std::min)(g_guestContextRegistry.count.load(), 512u);
    for (uint32_t i = 0; i < n; ++i)
        if (g_guestContextRegistry.slots[i].tid == tid) return g_guestContextRegistry.slots[i].entry;
    return 0;
}

// rollback.cpp's watchdog: every guest thread's current guest call chain (racy reads of live
// contexts - diagnostics only).
void DumpGuestThreads()
{
    const uint32_t n = (std::min)(g_guestContextRegistry.count.load(), 512u);
    for (uint32_t i = 0; i < n; ++i)
    {
        const GuestContextSlot& s = g_guestContextRegistry.slots[i];
        if (!s.ctx) continue;
        char line[512];
        int k = std::snprintf(line, sizeof(line), "[threads] %u entry %08X lr %08X:", s.tid, s.entry, uint32_t(s.ctx->lr));
        uint32_t frame = s.ctx->r1.u32;
        for (int d = 0; d < 12 && frame && frame < 0xFFFFFFF0u && k < int(sizeof(line)) - 12; ++d)
        {
            const uint32_t prev = GRead32(g_base, frame);
            if (!prev || prev <= frame || prev - frame > 0x10000) break;
            k += std::snprintf(line + k, sizeof(line) - k, " %08X", GRead32(g_base, prev - 8));
            frame = prev;
        }
        std::fprintf(stderr, "%s\n", line);
    }
}

// rollback.cpp's watchdog: the live contexts (racy reads - diagnostics only).
int GuestContexts(DWORD* tids, uint32_t* entries, PPCContext** ctxs, int max)
{
    const uint32_t n = (std::min)(g_guestContextRegistry.count.load(), 512u);
    int k = 0;
    for (uint32_t i = 0; i < n && k < max; ++i)
    {
        const GuestContextSlot& s = g_guestContextRegistry.slots[i];
        if (!s.ctx) continue;
        tids[k] = s.tid; entries[k] = s.entry; ctxs[k] = s.ctx; ++k;
    }
    return k;
}

PPCContext* GuestContextOf(DWORD tid)
{
    const uint32_t n = (std::min)(g_guestContextRegistry.count.load(), 512u);
    for (uint32_t i = 0; i < n; ++i)
        if (g_guestContextRegistry.slots[i].tid == tid) return g_guestContextRegistry.slots[i].ctx;
    return nullptr;
}

// The walks run while the thread is suspended; a fault (an odd host frame, a stale guest
// context) loses the sample instead of the process.
bool Walk(Target& t, Sample& s);
bool Take(Target& t, Sample& s)
{
    if (SuspendThread(t.thread) == DWORD(-1)) return false;
    s = {};
    s.at = Now();
    const bool ok = Walk(t, s);
    ResumeThread(t.thread);
    return ok;
}

bool Walk(Target& t, Sample& s)
{
    __try
    {
    CONTEXT hc{};
    hc.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(t.thread, &hc))
    {
        // Leaf functions without unwind data return through [rsp].
        for (; s.hostDepth < 32 && hc.Rip && hc.Rsp >= t.stackLow && hc.Rsp + 8 <= t.stackHigh; )
        {
            DWORD64 image = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(hc.Rip, &image, nullptr);
            s.host[s.hostDepth++] = fn && Mode() != 4 ? image + fn->BeginAddress : hc.Rip;   // p_diag38: mode 4 exact
            if (!fn)
            {
                hc.Rip = *reinterpret_cast<DWORD64*>(hc.Rsp);
                hc.Rsp += 8;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, hc.Rip, fn, &hc, &handlerData, &establisher, nullptr);
        }
    }
    if (PPCContext* c = t.ctx ? t.ctx : GuestContextOf(t.tid))
    {
        s.chain[s.depth++] = uint32_t(c->lr);
        uint32_t frame = c->r1.u32;
        for (int i = 0; i < 23 && frame && frame < 0xFFFFFFF0u; ++i)
        {
            const uint32_t prev = GRead32(g_base, frame);
            if (!prev || prev <= frame || prev - frame > 0x10000) break;
            s.chain[s.depth++] = GRead32(g_base, prev - 8);
            frame = prev;
        }
    }
    return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// A sample whose innermost host frame is a kernel wait / delay / yield stub (ntdll Nt* syscall
// stubs are short functions that end in syscall; NtWaitForSingleObject, NtDelayExecution,
// NtYieldExecution, NtSignalAndWaitForSingleObject, NtWaitForMultipleObjects, NtWaitForAlertByThreadId).
bool Waiting(uint64_t leaf)
{
    static const std::vector<uint64_t> stubs = [] {
        std::vector<uint64_t> v;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        for (const char* n : { "NtWaitForSingleObject", "NtDelayExecution", "NtYieldExecution", "NtSignalAndWaitForSingleObject",
                               "NtWaitForMultipleObjects", "NtWaitForAlertByThreadId", "NtRemoveIoCompletion", "ZwWaitForWorkViaWorkerFactory" })
            if (FARPROC f = GetProcAddress(ntdll, n)) v.push_back(reinterpret_cast<uint64_t>(f));
        return v;
    }();
    for (uint64_t a : stubs)
        if (leaf >= a && leaf < a + 0x20) return true;
    return false;
}

void ReportGaps()
{
    std::vector<Gap> gaps;
    {
        std::lock_guard l(g_gapsMutex);
        gaps.swap(g_gaps);
    }
    std::vector<Target*> targets;
    {
        std::lock_guard l(g_targetsMutex);
        targets = g_targets;
    }
    for (const Gap& gap : gaps)
    {
        if (g_gapReports++ >= 300) return;
        const int64_t start = gap.end - int64_t(gap.ms * Freq() / 1000.0);
        for (Target* t : targets)
        {
            std::vector<Sample> in;
            uint32_t busy = 0;
            for (const Sample& s : t->ring)
                if (s.at >= start && s.at <= gap.end)
                {
                    in.push_back(s);
                    busy += s.hostDepth && !Waiting(s.host[0]);
                }
            // Other threads only when they ran for a fifth of the gap (not parked in a wait).
            if (in.empty() || (t != g_main.load() && t->name != "cp" && busy * 5 < in.size())) continue;
            char header[96];
            std::snprintf(header, sizeof(header), "[gap] %.1f ms frame: %s thread (busy %u%%)", gap.ms, t->name.c_str(),
                          in.empty() ? 0u : unsigned(100.0 * busy / in.size() + 0.5));
            Report(header, in, 14, 16, 8);
        }
    }
}

// p_diag38 (TEST ONLY): the samples' host stacks as exe RVAs, counted, appended to C:/ttt2/port/fb/prof_<name>.txt.
void DumpStacks(const std::string& name, const std::vector<Sample>& samples)
{
    const uint64_t exe = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    std::map<std::string, uint32_t> stacks;
    uint32_t waiting = 0;
    for (const Sample& smp : samples)
    {
        if (!smp.hostDepth) continue;
        if (Waiting(smp.host[0])) { ++waiting; continue; }
        std::string key;
        for (uint32_t i = 0; i < smp.hostDepth && i < 16; ++i)
        {
            char b[24];
            const uint64_t a = smp.host[i];
            if (a >= exe && a - exe < 0x40000000ull) std::snprintf(b, sizeof(b), " %llX", (unsigned long long)(a - exe));
            else std::snprintf(b, sizeof(b), " x");
            key += b;
        }
        ++stacks[key];
    }
    const std::string path = "C:/ttt2/port/fb/prof_" + name + ".txt";
    if (FILE* f = std::fopen(path.c_str(), "a"))
    {
        std::fprintf(f, "# window %zu samples, %u waiting\n", samples.size(), waiting);
        for (const auto& [k, c] : stacks) std::fprintf(f, "%u%s\n", c, k.c_str());
        std::fclose(f);
    }
}

void Loop()
{
    DiagNameThread(L"hitch-sampler");
    const int mode = Mode();
    const bool always = mode >= 2;
    const int64_t step = Freq() / (always ? 1000 : 2000);   // 1 / 0.5 ms
    int64_t windowStart = Now();
    std::vector<Sample> resimUpdate, resimDraw;   // mode 5
    for (;;)
    {
        if (!always && !g_active.load(std::memory_order_acquire)) { Sleep(1); continue; }
        if (mode == 5)
        {
            Target* t = g_main.load();
            if (t && RollbackResimulating())
            {
                const bool draw = RollbackResimDraw();
                g_busy = true;
                Sample s;
                if (Take(*t, s)) (draw ? resimDraw : resimUpdate).push_back(s);
                g_busy = false;
            }
            const int64_t now = Now();
            if (now - windowStart >= Freq() * 10)
            {
                if (!resimUpdate.empty()) Report("[profile] re-simulated updates, last 10 s", resimUpdate, 40, 40, 30);
                if (!resimDraw.empty()) Report("[profile] re-simulated draws, last 10 s", resimDraw, 40, 40, 30);
                resimUpdate.clear(); resimDraw.clear();
                windowStart = now;
            }
            while (Now() < now + step) YieldProcessor();
            continue;
        }
        std::vector<Target*> targets;
        {
            std::lock_guard l(g_targetsMutex);
            targets = g_targets;
        }
        for (Target* t : targets)
        {
            const bool main = t == g_main;
            if (!main && mode != 3 && !(mode == 4 && (t->name == "cp" || t->name == "sound"))) continue;
            g_busy = main;
            const bool task = main && g_active.load(std::memory_order_acquire);
            Sample s;
            if ((task || always) && Take(*t, s))
            {
                if (mode == 3) { t->ring[t->next] = s; t->next = (t->next + 1) % t->ring.size(); }
                if ((mode == 2 && main) || mode == 4) t->window.push_back(s);
                std::lock_guard l(g_samplesMutex);
                if (task && g_samples.size() < 20000) g_samples.push_back(s);
            }
            g_busy = false;
        }
        const int64_t now = Now();
        if ((mode == 2 || mode == 4) && now - windowStart >= Freq() * 10)
        {
            for (Target* t : targets)
            {
                if (t->window.empty()) continue;
                const std::string header = "[profile] " + t->name + " thread, last 10 s";
                if (mode == 4) DumpStacks(t->name, t->window);   // p_diag38
                else Report(header.c_str(), t->window, 40, 60, 30);
                t->window.clear();
            }
            windowStart = now;
        }
        if (mode == 3) ReportGaps();
        while (Now() < now + step) YieldProcessor();
    }
}

void Begin(PPCContext& ctx, uint8_t* base)
{
    if (!On()) return;
    (void)base;
    if (!g_main) Register("main", &ctx, true);
    {
        std::lock_guard l(g_samplesMutex);
        g_samples.clear();
    }
    g_active.store(true, std::memory_order_release);
}

void End(double ms, uint32_t vtable)
{
    if (!On()) return;
    g_active.store(false, std::memory_order_release);
    while (g_busy.load()) YieldProcessor();
    if (ms < 20.0) return;
    std::vector<Sample> samples;
    {
        std::lock_guard l(g_samplesMutex);
        samples.swap(g_samples);
    }
    char header[96];
    std::snprintf(header, sizeof(header), "[hitch] task vtable %08X %.1f ms", vtable, ms);
    Report(header, samples);
}

void Report(const char* header, const std::vector<Sample>& samples, size_t guestTop, size_t hostTop, size_t leafTop)
{
    std::fprintf(stderr, "%s, %zu samples\n", header, samples.size());
    if (samples.empty()) return;
    std::map<uint32_t, uint32_t> inclusive, innermost;
    std::map<uint64_t, uint32_t> hostInclusive, hostLeaf;
    for (const Sample& s : samples)
    {
        std::set<uint32_t> seen;
        for (uint32_t i = 0; i < s.depth; ++i)
        {
            const uint32_t f = FunctionOf(s.chain[i]);
            if (f && seen.insert(f).second) ++inclusive[f];
        }
        if (s.depth) ++innermost[FunctionOf(s.chain[0])];
        std::set<uint64_t> seenHost;
        for (uint32_t i = 0; i < s.hostDepth; ++i)
            if (seenHost.insert(s.host[i]).second) ++hostInclusive[s.host[i]];
        if (s.hostDepth) ++hostLeaf[s.host[0]];
    }
    auto top = [](const auto& m, size_t n) {
        std::vector<std::pair<uint32_t, typename std::decay_t<decltype(m)>::key_type>> v;
        for (const auto& [k, c] : m) v.push_back({ c, k });
        std::sort(v.rbegin(), v.rend());
        if (v.size() > n) v.resize(n);
        return v;
    };
    auto pct = [&](uint32_t c) { return std::to_string(unsigned(100.0 * c / samples.size() + 0.5)) + "%"; };
    for (const auto& [label, m, n] : { std::tuple{ "inclusive", &inclusive, guestTop }, std::tuple{ "innermost", &innermost, guestTop / 2 } })
    {
        if (m->empty()) continue;
        std::string out = std::string("[hitch]   ") + label + ":";
        for (const auto& [c, f] : top(*m, n))
        {
            char b[16];
            std::snprintf(b, sizeof(b), " %08X=", f);
            out += b + pct(c);
        }
        std::fprintf(stderr, "%s\n", out.c_str());
    }
    // exe functions as RVAs (rva2sym.py), others as module+RVA.
    auto where = [](uint64_t address) {
        HMODULE m = nullptr;
        wchar_t path[MAX_PATH] = L"?";   // printed as UTF-8 (log_utf8.h)
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &m) && m)
            GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* name = std::wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        char b[96];
        if (m == GetModuleHandleW(nullptr)) std::snprintf(b, sizeof(b), "%llX", (unsigned long long)(address - uint64_t(m)));
        else std::snprintf(b, sizeof(b), "%s+%llX", LogUtf8(name).c_str(), (unsigned long long)(address - uint64_t(m)));
        return std::string(b);
    };
    for (const auto& [label, m, n] : { std::tuple{ "host inclusive", &hostInclusive, hostTop }, std::tuple{ "host leaf", &hostLeaf, leafTop } })
    {
        std::string out = std::string("[hitch]   ") + label + ":";
        for (const auto& [c, a] : top(*m, n)) out += " " + where(a) + "=" + pct(c);
        std::fprintf(stderr, "%s\n", out.c_str());
    }
}
} // namespace hitch

// Mode 3 hooks for kernel_video.cpp: the command-processor thread registers itself, and the
// present timer reports frames over 25 ms.
// data_watch.cpp: the guest context a host thread is running (its LR names the guest caller).
PPCContext* WatchGuestContextOf(DWORD tid) { return hitch::GuestContextOf(tid); }

void HitchRegisterThread(const char* name)
{
    if (hitch::Mode() == 3 || (hitch::Mode() == 4 && std::strcmp(name, "cp") == 0)) hitch::Register(name, nullptr);
}
// Mode 4 also samples the game's sound engine thread (NUSound, sub_82D032D0): its guest call chain.
void HitchRegisterGuestThread(const char* name, PPCContext* ctx)
{
    if (hitch::Mode() == 4) hitch::Register(name, ctx);
}
void HitchFrameGap(double ms)
{
    if (hitch::Mode() != 3) return;
    std::lock_guard l(hitch::g_gapsMutex);
    hitch::g_gaps.push_back({ hitch::Now(), ms });
}

// TTT2_HITCH_PROFILE: the guest heap's RtlAllocateHeap (82BFA928: r3 heap, r4 flags, r5 size)
// and RtlFreeHeap (82BFB220) counted and timed per thread, reported with each slow task: many
// cheap calls (allocation-heavy code) or a few slow ones (long free-list walks)?
namespace heapstat {
struct Counts { uint64_t allocs = 0, frees = 0; double allocMs = 0, freeMs = 0, slowest = 0; uint32_t slowestSize = 0; };
thread_local Counts t_counts;
thread_local uint64_t t_ops = 0;          // always counted (rollback.cpp: heap use per frame)
std::atomic<uint64_t> g_ops{ 0 };
inline double Ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
    static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return 1000.0 / double(q.QuadPart); }();
    return double(b.QuadPart - a.QuadPart) * f;
}
}
bool RollbackOwnedCaller(const PPCContext& ctx);          // rollback.cpp: network code allocating?
void RollbackNoteHeapBlock(uint32_t ptr, uint32_t size);
void RollbackNoteHeapFree(uint32_t ptr);
void RollbackHeapOp(bool alloc, uint32_t ptr, uint32_t size, const PPCContext& ctx);   // TTT2_RB_HEAPLOG
bool RollbackArenaOn();                                     // rollback.cpp: the network's blocks from an arena
uint32_t RollbackArenaAlloc(uint32_t size, bool zero, uint32_t align = 16);
uint32_t RollbackArenaSize(uint32_t a);
bool RollbackArenaFree(uint32_t a);
bool RollbackAudioArenaOn();                                // rollback.cpp: XAudio2's blocks from the arena too

// XMemAlloc (sub_82BF8050: r3 size, r4 the XALLOC attributes - bit 31 physical memory, bits 16-23 the allocator id)
// with allocator id 0x97 (eXALLOCAllocatorId_XAUDIO2) is XAudio2's: its engine contexts, render clients and their
// KEVENTs, voices and its pool (sub_82C1E9A8, manager 0x830BF408). Its heap path is sub_82C079A8 -> sub_82BFC0C8 ->
// RtlAllocateHeap on the process heap (no header of its own); XMemFree (sub_82BF8188) frees through RtlFreeHeap and
// XAudio2 never asks XMemSize (callers: allocators 0x8C / 0x8E only). Those blocks come from the never-rewound arena
// (as the network's do): the audio threads run on their own timeline, and on the process heap XAudio2's objects
// shared pages with the game's - a page a frame thread wrote went hot and every rewind put the render clients'
// and engines' words back under the running audio threads. MEASURED 2026-11-09 (TTT2_RB_FOREIGN=keep, Arcade, sync
// test 8 every 5): after 76 rewinds the pump hung for good in the render callback sub_82C3E008 waiting for frame-ready,
// both engines idle in sub_82C3A420 waiting for their kick, client 0014F920's submitted count (+56) 143 behind
// the frames pumped - the player's stall exactly. Physical XMemAlloc blocks (bit 31) are outside the tracked ranges.
static thread_local int t_xaudioAlloc = 0;
static std::atomic<uint64_t> g_xaudioArenaBlocks{ 0 };
extern "C" PPC_FUNC(__imp__sub_82BF8050);
PPC_FUNC(sub_82BF8050)
{
    const uint32_t attr = ctx.r4.u32;
    if ((attr & 0x80000000u) || ((attr >> 16) & 0xFF) != 0x97 || !RollbackAudioArenaOn())
    {
        __imp__sub_82BF8050(ctx, base);
        return;
    }
    ++t_xaudioAlloc;
    __imp__sub_82BF8050(ctx, base);
    --t_xaudioAlloc;
}

extern "C" PPC_FUNC(__imp__sub_82BFA928);
PPC_FUNC(sub_82BFA928)
{
    ++heapstat::t_ops;
    heapstat::g_ops.fetch_add(1, std::memory_order_relaxed);
    const uint32_t size = ctx.r5.u32;
    const bool audio = t_xaudioAlloc > 0;                // XAudio2's XMemAlloc (above)
    const bool owned = audio || RollbackOwnedCaller(ctx);
    if (owned && (audio || RollbackArenaOn()))
    {
        // r3 heap, r4 flags (HEAP_ZERO_MEMORY 8), r5 size.
        const PPCContext entry = ctx;
        if (const uint32_t p = RollbackArenaAlloc(size, (ctx.r4.u32 & 8) != 0))
        {
            ctx.r3.u64 = p;
            RollbackHeapOp(true, p, size, entry);
            if (audio && g_xaudioArenaBlocks.fetch_add(1, std::memory_order_relaxed) == 0)
                std::fprintf(stderr, "[rb] XAudio2's heap blocks come from the never-rewound arena (the first: %08X+%X, lr %08X)\n",
                             p, size, uint32_t(entry.lr));
            return;
        }
    }
    if (!hitch::On())
    {
        const PPCContext entry = ctx;
        __imp__sub_82BFA928(ctx, base);
        if (owned) RollbackNoteHeapBlock(ctx.r3.u32, size);
        RollbackHeapOp(true, ctx.r3.u32, size, entry);
        return;
    }
    LARGE_INTEGER a, b;
    const PPCContext entry = ctx;
    QueryPerformanceCounter(&a);
    __imp__sub_82BFA928(ctx, base);
    QueryPerformanceCounter(&b);
    if (owned) RollbackNoteHeapBlock(ctx.r3.u32, size);
    RollbackHeapOp(true, ctx.r3.u32, size, entry);
    auto& c = heapstat::t_counts;
    const double ms = heapstat::Ms(a, b);
    ++c.allocs;
    c.allocMs += ms;
    if (ms > c.slowest) { c.slowest = ms; c.slowestSize = size; }
}
extern "C" PPC_FUNC(__imp__sub_82BFB220);
PPC_FUNC(sub_82BFB220)
{
    ++heapstat::t_ops;
    heapstat::g_ops.fetch_add(1, std::memory_order_relaxed);
    if (RollbackArenaFree(ctx.r5.u32)) { ctx.r3.u64 = 1; return; }   // the network's arena block
    RollbackNoteHeapFree(ctx.r5.u32);
    RollbackHeapOp(false, ctx.r5.u32, 0, ctx);
    if (!hitch::On()) { __imp__sub_82BFB220(ctx, base); return; }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    __imp__sub_82BFB220(ctx, base);
    QueryPerformanceCounter(&b);
    auto& c = heapstat::t_counts;
    const double ms = heapstat::Ms(a, b);
    ++c.frees;
    c.freeMs += ms;
    if (ms > c.slowest) { c.slowest = ms; c.slowestSize = 0; }
}

// RtlReAllocateHeap (82BFB518: r3 heap, r4 flags, r5 block, r6 size) on an arena block: a new arena block,
// the content copied, the old one freed (the guest heap would read a header the arena does not have).
extern "C" PPC_FUNC(__imp__sub_82BFB518);
PPC_FUNC(sub_82BFB518)
{
    const uint32_t old = ctx.r5.u32;
    const uint32_t oldSize = old ? RollbackArenaSize(old) : 0;
    if (!oldSize) { __imp__sub_82BFB518(ctx, base); return; }
    const uint32_t size = ctx.r6.u32;
    const uint32_t p = RollbackArenaAlloc(size, (ctx.r4.u32 & 8) != 0);
    if (p) std::memcpy(base + p, base + old, oldSize < size ? oldSize : size);
    if (p) RollbackArenaFree(old);
    ctx.r3.u64 = p;
}
// RtlSizeHeap (82BF9BA0: r3 heap, r4 flags, r5 block; XMemSize's heap path sub_82C08B40, the CRT's _msize) on an arena
// block: its requested size (the guest heap would read a header the arena does not have). Nothing asks it about
// XAudio2's or the network's blocks today - this only keeps a future caller from reading garbage.
extern "C" PPC_FUNC(__imp__sub_82BF9BA0);
PPC_FUNC(sub_82BF9BA0)
{
    if (const uint32_t n = ctx.r5.u32 ? RollbackArenaSize(ctx.r5.u32) : 0) { ctx.r3.u64 = n; return; }
    __imp__sub_82BF9BA0(ctx, base);
}

// Havok's large-block allocator (sub_82C7F5A0 = its alloc, r3 = the allocator): rollback.cpp checks the
// free trees of every allocator seen after each rewind.
void RollbackNoteLargeBlockAllocator(uint32_t obj);
extern "C" PPC_FUNC(__imp__sub_82C7F5A0);
PPC_FUNC(sub_82C7F5A0)
{
    RollbackNoteLargeBlockAllocator(ctx.r3.u32);
    __imp__sub_82C7F5A0(ctx, base);
}

// The game's own heap: operator new sub_82A61BF8 -> sub_82A68428(heap handle, size, alignment, tag,
// ...) -> the heap object's allocate (sub_82A68368; four heap kinds, r5 the alignment: 0 / 1 none);
// operator delete sub_82A61C40 -> sub_82A67B48(handle, p). The online code calls these directly too
// (MEASURED, static calls: 130 allocations, 149 frees from 0x82660000-0x82700000 and NACE). Blocks
// the network owns come from its arena like the system heap's (MEASURED: the online event object
// 0x00341F80, 0x38 bytes, operator new from 826B3790 - its +4 points at NACE's pending event; left in
// the rewound heap while NACE's events were not, a rewind gave it back an event NACE had deleted, and
// the next clear deleted it again: a virtual call through a null vtable, lr 82B54238, then garbage
// frees). TTT2_RB_FINDBLOCK diagnostics see the game heap's blocks too.
void RollbackFindBlock(bool alloc, uint32_t ptr, uint32_t size, const PPCContext& ctx);   // rollback.cpp
extern "C" PPC_FUNC(__imp__sub_82A68428);
PPC_FUNC(sub_82A68428)
{
    const PPCContext entry = ctx;
    if (RollbackArenaOn() && RollbackOwnedCaller(ctx))
    {
        uint32_t align = 16;                     // a power of two at least the request (seen: 0, 4, 8, 12, 16)
        while (align < ctx.r5.u32 && align < 4096) align <<= 1;
        if (const uint32_t p = RollbackArenaAlloc(ctx.r4.u32, true, align))
        {
            ctx.r3.u64 = p;
            RollbackFindBlock(true, p, entry.r4.u32, entry);
            return;
        }
    }
    __imp__sub_82A68428(ctx, base);
    RollbackFindBlock(true, ctx.r3.u32, entry.r4.u32, entry);
}
// DIAGNOSTIC BUILD (diag3): the heap object's allocate, sub_82A68368 (r3 the heap object: +84 its type, +88 its kind;
// r4 size, r5 alignment, r6 tag), returns 0 when the heap cannot give the block - and then writes to address 0 on
// purpose, the game's own out-of-memory crash (the port ignores page-0 writes, so the game goes on with a null block:
// log 15, a hang after three such failures while a stage loaded on a GT 730). Each failure is logged with the heap
// object's first words and the guest backchain.
// Live bytes per heap object, from the allocate / free hooks (blocks given out before the first hook call - none: the
// hooks are linked in - and frees through other paths would show as growth).
static uint8_t* g_diagBase = nullptr;              // guest memory base, noted by the allocate hook
namespace diagheap {
struct Use { uint64_t live = 0, peak = 0, allocs = 0, frees = 0, failed = 0; };
std::mutex g_lock;
std::unordered_map<uint32_t, Use> g_heaps;                                  // heap object -> its use
std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> g_blocks;       // block -> (heap object, size)
}
// DIAGNOSTIC BUILD (diag4): the medium heap's blocks (sub_82A62B70), keyed by the first three return addresses
// outside the allocator (0x82A60000-0x82A69000), and a walk of the heap itself.
namespace diagmed {
struct Block { uint32_t size, key; };
std::unordered_map<uint32_t, Block> g_blocks;                    // block -> its size and caller key
std::map<std::array<uint32_t, 3>, uint32_t> g_keyIndex;
std::vector<std::array<uint32_t, 3>> g_keys;
struct Use { uint64_t live = 0, count = 0, allocs = 0; };
std::vector<Use> g_use;                                          // by caller key
uint64_t g_live = 0, g_peak = 0, g_failed = 0;
std::set<uint32_t> g_heaps;                                      // the medium heap objects seen
}
static void DiagMediumReportLocked(const char* why)
{
    using namespace diagmed;
    if (g_heaps.empty() || !g_diagBase) return;
    // Blocks freed by paths the hooks do not see: the block's header (64 bytes before it, +4 size | flags) no longer
    // says "in use" (flag 2) with room for it.
    for (auto it = g_blocks.begin(); it != g_blocks.end();)
    {
        const uint32_t w = GRead32(g_diagBase, it->first - 60);
        if ((w & 3u) == 2u && (w & ~3u) >= it->second.size) { ++it; continue; }
        g_live -= it->second.size;
        g_use[it->second.key].live -= it->second.size;
        --g_use[it->second.key].count;
        it = g_blocks.erase(it);
    }
    for (const uint32_t heap : g_heaps) {
    const uint32_t first = GRead32(g_diagBase, heap + 0), end = GRead32(g_diagBase, heap + 4), size = GRead32(g_diagBase, heap + 8);
    uint64_t bytes[4] = {}, blocks[4] = {}, largest[4] = {};
    uint32_t p = first, walked = 0;
    while (p >= first && p < first + size && walked < 4000000)
    {
        const uint32_t w = GRead32(g_diagBase, p + 4), s = w & ~3u, f = w & 3u;
        bytes[f] += s; ++blocks[f]; largest[f] = (std::max)(largest[f], uint64_t(s));
        ++walked;
        if (!s && !f) break;
        p += 64 + s;
    }
    std::fprintf(stderr, "[diag-med] %s: medium heap %08X region %08X..%08X (%.2f MB, end word %08X); walk ended at %08X after %u blocks; "
                         "by flags 0/1/2/3: %.2f/%.2f/%.2f/%.2f MB in %llu/%llu/%llu/%llu blocks, largest %u/%u/%u/%u KB; "
                         "tracked live %.2f MB (peak since the last report %.2f MB) in %zu blocks, %llu failed\n",
                 why, heap, first, first + size, size / 1048576.0, end, p, walked,
                 bytes[0] / 1048576.0, bytes[1] / 1048576.0, bytes[2] / 1048576.0, bytes[3] / 1048576.0,
                 (unsigned long long)blocks[0], (unsigned long long)blocks[1], (unsigned long long)blocks[2], (unsigned long long)blocks[3],
                 unsigned(largest[0] >> 10), unsigned(largest[1] >> 10), unsigned(largest[2] >> 10), unsigned(largest[3] >> 10),
                 g_live / 1048576.0, g_peak / 1048576.0, g_blocks.size(), (unsigned long long)g_failed);
    }
    g_peak = g_live;
    std::vector<uint32_t> order;
    for (uint32_t k = 0; k < g_use.size(); ++k) if (g_use[k].live) order.push_back(k);
    std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return g_use[a].live > g_use[b].live; });
    for (size_t i = 0; i < order.size() && i < 16; ++i)
    {
        const auto& k = g_keys[order[i]];
        const Use& u = g_use[order[i]];
        std::fprintf(stderr, "[diag-med]   %8.1f KB in %5llu blocks (%llu allocations) from %08X < %08X < %08X\n", u.live / 1024.0,
                     (unsigned long long)u.count, (unsigned long long)u.allocs, k[0], k[1], k[2]);
    }
}
extern "C" PPC_FUNC(__imp__sub_82A62B70);
PPC_FUNC(sub_82A62B70)
{
    g_diagBase = base;
    const PPCContext entry = ctx;
    __imp__sub_82A62B70(ctx, base);
    std::array<uint32_t, 3> key{};
    {
        uint32_t frame = entry.r1.u32, ret = uint32_t(entry.lr);
        // Skipped too (diag4b, local): the pool wrapper sub_82283B00 and the node-tree code 82A88000-82A8C000 /
        // 82AB4000-82AB5000, so tree pages are keyed by the code that builds the tree.
        auto skip = [](uint32_t a) {
            return (a >= 0x82A60000u && a < 0x82A69000u) || (a >= 0x82283B00u && a < 0x82283C00u) ||
                   (a >= 0x82A88000u && a < 0x82A8C000u) || (a >= 0x82AB4000u && a < 0x82AB5000u);
        };
        for (int i = 0, n = 0; i < 32 && n < 3; ++i)
        {
            if (ret && !skip(ret)) key[n++] = ret;
            const uint32_t prev = GRead32(base, frame);
            if (!prev || prev <= frame || prev - frame > 0x10000) break;
            ret = GRead32(base, prev - 8);
            frame = prev;
        }
    }
    std::lock_guard l(diagheap::g_lock);
    using namespace diagmed;
    g_heaps.insert(entry.r3.u32);
    if (!ctx.r3.u32)
    {
        if (g_failed++ == 0) DiagMediumReportLocked("out of memory");
        return;
    }
    auto [ki, fresh] = g_keyIndex.try_emplace(key, uint32_t(g_keys.size()));
    if (fresh) { g_keys.push_back(key); g_use.emplace_back(); }
    const auto [it, added] = g_blocks.try_emplace(ctx.r3.u32, Block{ entry.r4.u32, ki->second });
    if (!added)                                  // freed by a path the hooks do not see
    {
        g_live -= it->second.size;
        g_use[it->second.key].live -= it->second.size;
        --g_use[it->second.key].count;
        it->second = Block{ entry.r4.u32, ki->second };
    }
    g_live += entry.r4.u32;
    g_peak = (std::max)(g_peak, g_live);
    Use& u = g_use[ki->second];
    u.live += entry.r4.u32; ++u.count; ++u.allocs;
}
void DiagHeapReport(const char* why)
{
    std::lock_guard l(diagheap::g_lock);
    if (std::strcmp(why, "out of memory") != 0) DiagMediumReportLocked(why);   // a failure: the medium hook reports once
    for (auto& [heap, u] : diagheap::g_heaps)
    {
        if (u.peak < (1u << 20) && !u.failed) continue;
        std::fprintf(stderr, "[diag-heap] %s: heap %08X live %.2f MB, peak since the last report %.2f MB, %llu allocations, "
                             "%llu frees, %llu failed\n", why, heap, u.live / 1048576.0, u.peak / 1048576.0,
                     (unsigned long long)u.allocs, (unsigned long long)u.frees, (unsigned long long)u.failed);
        u.peak = u.live;
        static int dumped = 0;                    // the heap object's words, once (its size is among them)
        if (dumped++ < 2 && heap >= 0x10000)
        {
            std::fprintf(stderr, "[diag-heap] heap %08X words:", heap);
            for (uint32_t o = 0; o < 0xC0; o += 4) std::fprintf(stderr, " %08X", GRead32(g_diagBase, heap + o));
            std::fputc('\n', stderr);
        }
    }
}
// TTT2_DIAG_HEAPCHECK=1 (diagnostics; found the CHANGE TEAMS stack overflow of 2026-11-08 - online_2v2_places.cpp's
// roles): every block the game heap frees (sub_82A679F0, r3 the heap, r4 the block) is kept with the free's guest call
// chain until the heap gives it out again (sub_82A68368); a second free of it before that is logged with both chains and
// left out (the first free stands - a second one would hand the block to two owners). Also the per-frame maps' watch
// (TreeWatchCheck, below).
static bool HeapCheckOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_HEAPCHECK"); return e && *e == '1'; }();
    return on;
}
namespace diagfree {
struct Free { uint32_t heap; DWORD thread; uint32_t chain[8]; };
std::unordered_map<uint32_t, Free> g_freed;    // diagheap::g_lock
int g_logged = 0;
static void Chain(uint8_t* base, const PPCContext& ctx, uint32_t (&out)[8])
{
    std::memset(out, 0, sizeof(out));
    out[0] = uint32_t(ctx.lr);
    uint32_t frame = ctx.r1.u32;
    for (int i = 1; i < 8 && frame && frame < 0xFFFFFFF0u; ++i)
    {
        const uint32_t prev = GRead32(base, frame);
        if (!prev || prev <= frame || prev - frame > 0x10000) break;
        out[i] = GRead32(base, prev - 8);
        frame = prev;
    }
}
}

extern "C" PPC_FUNC(__imp__sub_82A679F0);
PPC_FUNC(sub_82A679F0)
{
    if (ctx.r4.u32)
    {
        std::lock_guard l(diagheap::g_lock);
        if (HeapCheckOn())
        {
            diagfree::Free f{ ctx.r3.u32, GetCurrentThreadId(), {} };
            diagfree::Chain(base, ctx, f.chain);
            const auto [it, fresh] = diagfree::g_freed.try_emplace(ctx.r4.u32, f);
            if (!fresh)
            {
                if (diagfree::g_logged++ < 40)
                {
                    const diagfree::Free& a = it->second;
                    std::fprintf(stderr,
                                 "[double-free] block %08X freed again (heap %08X, thread %lu): %08X %08X %08X %08X %08X %08X %08X %08X; "
                                 "its free before (heap %08X, thread %lu): %08X %08X %08X %08X %08X %08X %08X %08X - this one left out\n",
                                 ctx.r4.u32, f.heap, f.thread, f.chain[0], f.chain[1], f.chain[2], f.chain[3], f.chain[4], f.chain[5],
                                 f.chain[6], f.chain[7], a.heap, a.thread, a.chain[0], a.chain[1], a.chain[2], a.chain[3], a.chain[4],
                                 a.chain[5], a.chain[6], a.chain[7]);
                    std::fflush(stderr);
                }
                return;
            }
        }
        const auto it = diagheap::g_blocks.find(ctx.r4.u32);
        if (it != diagheap::g_blocks.end())
        {
            auto& u = diagheap::g_heaps[it->second.first];
            u.live -= it->second.second;
            ++u.frees;
            diagheap::g_blocks.erase(it);
        }
        const auto m = diagmed::g_blocks.find(ctx.r4.u32);
        if (m != diagmed::g_blocks.end())
        {
            diagmed::g_live -= m->second.size;
            diagmed::g_use[m->second.key].live -= m->second.size;
            --diagmed::g_use[m->second.key].count;
            diagmed::g_blocks.erase(m);
        }
    }
    __imp__sub_82A679F0(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82A68368);
PPC_FUNC(sub_82A68368)
{
    g_diagBase = base;
    const PPCContext entry = ctx;
    __imp__sub_82A68368(ctx, base);
    if (ctx.r3.u32)
    {
        std::lock_guard l(diagheap::g_lock);
        if (HeapCheckOn()) diagfree::g_freed.erase(ctx.r3.u32);
        auto& u = diagheap::g_heaps[entry.r3.u32];
        const auto [it, fresh] = diagheap::g_blocks.try_emplace(ctx.r3.u32, entry.r3.u32, entry.r4.u32);
        if (!fresh)                                  // freed by a path the hook does not see: count the new size only
        {
            auto& old = diagheap::g_heaps[it->second.first];
            old.live -= it->second.second;
            it->second = { entry.r3.u32, entry.r4.u32 };
        }
        u.live += entry.r4.u32;
        u.peak = (std::max)(u.peak, u.live);
        ++u.allocs;
        return;
    }
    {
        std::lock_guard l(diagheap::g_lock);
        ++diagheap::g_heaps[entry.r3.u32].failed;
    }
    static std::atomic<int> logged{ 0 };
    if (logged++ >= 40) return;
    DiagHeapReport("out of memory");
    const uint32_t heap = entry.r3.u32;
    const bool readable = heap >= 0x10000 && heap < 0xFFFF0000u;
    char line[768];
    int n = std::snprintf(line, sizeof(line), "[diag] OUT OF MEMORY: the game's heap %08X (kind %u, type %u) refused %u bytes "
                          "(alignment %u, tag %d), thread %lu, lr %08X, bt:", heap, readable ? GRead32(base, heap + 88) : 0,
                          readable ? GRead32(base, heap + 84) : 0, entry.r4.u32, entry.r5.u32, (int)entry.r6.s32,
                          GetCurrentThreadId(), (uint32_t)entry.lr);
    uint32_t frame = entry.r1.u32;
    for (int i = 0; i < 14 && frame && frame < 0xFFFFFFF0u && n < (int)sizeof(line) - 16; ++i)
    {
        const uint32_t prev = GRead32(base, frame);
        if (!prev || prev <= frame || prev - frame > 0x10000) break;
        n += std::snprintf(line + n, sizeof(line) - n, " %08X", GRead32(base, prev - 8));
        frame = prev;
    }
    if (readable && n < (int)sizeof(line) - 16)
    {
        n += std::snprintf(line + n, sizeof(line) - n, "; heap words:");
        for (uint32_t o = 0; o < 0x60 && n < (int)sizeof(line) - 12; o += 4)
            n += std::snprintf(line + n, sizeof(line) - n, " %08X", GRead32(base, heap + o));
    }
    std::fprintf(stderr, "%s\n", line);
}
extern "C" PPC_FUNC(__imp__sub_82A67B48);
PPC_FUNC(sub_82A67B48)
{
    if (ctx.r4.u32 && RollbackArenaFree(ctx.r4.u32)) return;   // the network's arena block
    __imp__sub_82A67B48(ctx, base);
}

// The online system's own allocator: sub_82661430(size) / sub_82661468(p) count the call (0x82F31434 /
// 0x82F31438) and call the game's memory manager through the callbacks at 0x82E11734 / 0x82E11738 -
// no RtlAllocateHeap, so the hooks above never see these blocks. NACE's library allocates through it
// (its allocator object 0x82E1682C: vfunc 4 sub_826B2268 -> here, vfunc 8 -> the free). MEASURED: the
// connection object (6568 bytes, keepalive counters conn+348 sent / +352 answered and the host's timer)
// came from here into the rewound heap; a rewind set "sent" back below an answer that then arrived,
// sent - answered wrapped (unsigned) past 4 and NACE declared the host lost - most matches under
// 80 ms / 30 ms jitter / 3 % loss, and at ~900 frames even under 50 ms / 10 ms. With rollback its
// blocks come from the network's arena (never rewound, host bookkeeping).
extern "C" PPC_FUNC(__imp__sub_82661430);
PPC_FUNC(sub_82661430)
{
    if (!RollbackArenaOn()) { __imp__sub_82661430(ctx, base); return; }
    GWrite32(base, 0x82F31434u, GRead32(base, 0x82F31434u) + 1);
    ctx.r3.u64 = RollbackArenaAlloc(ctx.r3.u32, true);
}
extern "C" PPC_FUNC(__imp__sub_82661468);
PPC_FUNC(sub_82661468)
{
    if (ctx.r3.u32 && RollbackArenaFree(ctx.r3.u32))
    {
        GWrite32(base, 0x82F31438u, GRead32(base, 0x82F31438u) + 1);
        return;
    }
    __imp__sub_82661468(ctx, base);
}

// The game's heap calls (RtlAllocateHeap / RtlFreeHeap, guest code) so far: this thread's, all threads'.
void GameHeapOps(uint64_t& thread, uint64_t& all)
{
    thread = heapstat::t_ops;
    all = heapstat::g_ops.load(std::memory_order_relaxed);
}

void ThreadIoTotals(uint64_t& calls, uint64_t& bytes, double& ms);   // kernel_io.cpp
void CtabCacheCounts(uint64_t& calls, uint64_t& hits);              // d3dx_ctab_cache.cpp
// The game's task runner (task->vtable[1] = step, called until it stops): name the task.
extern "C" PPC_FUNC(__imp__sub_82A80AD8);
void RollbackTaskStep(uint32_t task, uint32_t vtable, uint32_t step);   // rollback.cpp: the session's task census
bool RollbackSkipTask(uint32_t step);                                    // rollback.cpp: tasks a re-simulation leaves out
bool RollbackOnlineTask(uint32_t step);
void RollbackOnlineStep(bool begin, uint32_t step);                      // rollback.cpp: online steps' writes, learned
void RollbackOnlineTaskScope(bool in);
void TreeWatchCheck(uint8_t* base, const char* where, uint32_t task);   // below: the per-frame maps' heads (TTT2_DIAG_HEAPCHECK)
PPC_FUNC(sub_82A80AD8)
{
    struct AfterStep
    {
        uint8_t* base;
        uint32_t task;
        ~AfterStep() { TreeWatchCheck(base, "a task step", task); }
    } afterStep{ base, ctx.r3.u32 };
    if (ctx.r3.u32)
    {
        const uint32_t vt = GRead32(base, ctx.r3.u32);
        const uint32_t st = vt ? GRead32(base, vt + 4) : 0;
        RollbackTaskStep(ctx.r3.u32, vt, st);
        if (RollbackSkipTask(st)) { ctx.r3.u64 = 0; return; }   // a re-simulated update: not this task
        if (st >= 0x82690000u && st < 0x826D0000u && !RollbackOnlineTask(st))
        {
            // The online system's step in a live session update: its writes are learned (rollback.cpp).
            RollbackOnlineStep(true, st);
            __imp__sub_82A80AD8(ctx, base);
            RollbackOnlineStep(false, st);
            return;
        }
        if (RollbackOnlineTask(st))
        {
            // The online system's own task: what it allocates is never rewound (rollback.cpp).
            RollbackOnlineTaskScope(true);
            __imp__sub_82A80AD8(ctx, base);
            RollbackOnlineTaskScope(false);
            return;
        }
    }
    static const bool on = [] { const char* e = std::getenv("TTT2_FRAMETIME"); return e && *e == '1'; }();
    if (!on && !hitch::On()) { __imp__sub_82A80AD8(ctx, base); return; }       // no diagnostics: nothing added
    const uint32_t task = ctx.r3.u32, vtable = task ? GRead32(base, task) : 0, step = vtable ? GRead32(base, vtable + 4) : 0;
    const uint32_t lr = uint32_t(ctx.lr);
    uint64_t calls0 = 0, bytes0 = 0, calls1 = 0, bytes1 = 0;
    double io0 = 0, io1 = 0;
    ThreadIoTotals(calls0, bytes0, io0);
    static const DWORD mainThread = GetCurrentThreadId();
    thread_local int depth = 0;
    const bool outer = GetCurrentThreadId() == mainThread && depth++ == 0;
    if (outer) hitch::Begin(ctx, base);
    const heapstat::Counts heap0 = heapstat::t_counts;
    uint64_t ctabCalls0 = 0, ctabHits0 = 0;
    CtabCacheCounts(ctabCalls0, ctabHits0);
    const auto start = std::chrono::steady_clock::now();
    __imp__sub_82A80AD8(ctx, base);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (GetCurrentThreadId() == mainThread) --depth;
    if (outer) hitch::End(ms, vtable);
    if (outer && ms >= 20.0 && hitch::On())
    {
        const heapstat::Counts& h = heapstat::t_counts;
        std::fprintf(stderr, "[hitch]   heap: %llu allocs %.1f ms, %llu frees %.1f ms (thread's slowest call so far %.2f ms, size %u)\n",
                     (unsigned long long)(h.allocs - heap0.allocs), h.allocMs - heap0.allocMs,
                     (unsigned long long)(h.frees - heap0.frees), h.freeMs - heap0.freeMs, h.slowest, h.slowestSize);
        uint64_t ctabCalls = 0, ctabHits = 0;
        CtabCacheCounts(ctabCalls, ctabHits);
        std::fprintf(stderr, "[hitch]   constant tables: %llu requests, %llu from the cache\n",
                     (unsigned long long)(ctabCalls - ctabCalls0), (unsigned long long)(ctabHits - ctabHits0));
    }
    if (!on) return;
    ThreadIoTotals(calls1, bytes1, io1);
    static std::atomic<int> logged{ 0 };
    if (ms > 8.0 && logged++ < 400)
        std::fprintf(stderr, "[frame]   file calls during it: %llu, %llu KB, %.1f ms\n", (unsigned long long)(calls1 - calls0),
                     (unsigned long long)((bytes1 - bytes0) / 1024), io1 - io0);
    if (ms > 8.0 && logged < 400)
        std::fprintf(stderr, "[frame] slow task %08X vtable %08X step %08X %.1f ms (lr %08X) thread %lu\n", task, vtable, step, ms, lr,
                     GetCurrentThreadId());
}
TIMED_HOOK(82258D60)
TIMED_HOOK(825DEBD0)
// The battle tick: rollback.cpp needs to know which updates ran the fight.
void RollbackBattleTick(bool start);
extern "C" PPC_FUNC(__imp__sub_822D36C8);
PPC_FUNC(sub_822D36C8)
{
    RollbackBattleTick(true);
    TimeCall("sub_822D36C8", ctx, base, __imp__sub_822D36C8);
    RollbackBattleTick(false);
}

// The stage select's list of stages (sub_8227D2C0, r3 the select controller C: C+92.. the stage ids, C+304 the count with
// the random entry 51 last, C+88 the random cells' roulette start - a fight-RNG draw - C+300 the cursor's cell): logged
// on every build of it. TTT2_DIAG_ROULETTE=<index>|last (TEST ONLY) puts the roulette's start there afterwards.
extern "C" PPC_FUNC(__imp__sub_8227D2C0);
PPC_FUNC(sub_8227D2C0)
{
    const uint32_t C = ctx.r3.u32;
    __imp__sub_8227D2C0(ctx, base);
    const int32_t count = int32_t(GRead32(base, C + 304));
    static const char* force = std::getenv("TTT2_DIAG_ROULETTE");
    if (force && count > 1)
    {
        const int32_t v = std::strcmp(force, "last") == 0 ? count - 2 : (std::min)(int32_t(std::atoi(force)), count - 2);
        GWrite32(base, C + 88, uint32_t(v));
    }
    char line[400];
    int n = std::snprintf(line, sizeof(line), "[stage-list] controller %08X: %d entries, roulette at %d%s, cell %d, C+316 %d:", C,
                          count, int32_t(GRead32(base, C + 88)), force ? " (TTT2_DIAG_ROULETTE)" : "",
                          int32_t(GRead32(base, C + 300)), int32_t(GRead32(base, C + 316)));
    for (int32_t i = 0; i < count && i < 64 && n < 380; ++i)
        n += std::snprintf(line + n, sizeof(line) - n, " %d", int32_t(GRead32(base, C + 92 + 4 * i)));
    std::fprintf(stderr, "%s\n", line);
}

// A std::map / std::set clear (sub_824DA670, r3 the tree: +4 its head node; the head's +4 the root, +29 _Isnil): a head
// that is not the nil node (or none) would send the recursive erase (sub_824DA3D8) round node 0 until the stack ends -
// logged with the guest call chain before it runs.
static thread_local uint32_t t_clearTree = 0, t_clearR1 = 0, t_clearLr = 0;
static thread_local int t_eraseDepth = 0;
static thread_local uint32_t t_erasePath[64];
extern "C" PPC_FUNC(__imp__sub_824DA670);
PPC_FUNC(sub_824DA670)
{
    const uint32_t tree = ctx.r3.u32;
    const uint32_t head = tree >= 0x1000 ? GRead32(base, tree + 4) : 0;
    const bool bad = !head || head < 0x1000 || !GRead8(base, head + 29) || GRead32(base, head + 4) < 0x1000;
    static std::atomic<int> logged{ 0 };
    if (bad && logged++ < 50)
    {
        char line[400];
        int n = std::snprintf(line, sizeof(line), "[tree] clear of a broken tree %08X: head %08X", tree, head);
        if (head >= 0x1000)
            n += std::snprintf(line + n, sizeof(line) - n, " (+0 %08X +4 %08X +8 %08X _Isnil %u)", GRead32(base, head),
                               GRead32(base, head + 4), GRead32(base, head + 8), GRead8(base, head + 29));
        n += std::snprintf(line + n, sizeof(line) - n, " size %u thread %lu lr %08X chain:", GRead32(base, tree + 8),
                           GetCurrentThreadId(), uint32_t(ctx.lr));
        uint32_t frame = ctx.r1.u32;
        for (int i = 0; i < 12 && frame && frame < 0xFFFFFFF0u && n < 380; ++i)
        {
            const uint32_t prev = GRead32(base, frame);
            if (!prev || prev <= frame || prev - frame > 0x10000) break;
            n += std::snprintf(line + n, sizeof(line) - n, " %08X", GRead32(base, prev - 8));
            frame = prev;
        }
        std::fprintf(stderr, "%s\n", line);
        std::fflush(stderr);
    }
    t_clearTree = tree;
    t_clearR1 = ctx.r1.u32;
    t_clearLr = uint32_t(ctx.lr);
    __imp__sub_824DA670(ctx, base);
    t_clearTree = 0;
}

// The recursive erase itself (r3 the tree, r4 the subtree's root; it recurses on the right child +8, loops on the left
// +0, stops at _Isnil +29): deeper than any balanced tree (a loop in it, or a node that is not one) - logged once per
// clear with the path it took from the root and the clear's guest call chain (the frames above the clear stay as they
// were while it recurses). The report has its own frame: the hook's stays small (every level of the recursion has one).
static __declspec(noinline) void EraseTooDeep(uint8_t* base, uint32_t r3)
{
    static std::atomic<int> logged{ 0 };
    if (logged++ >= 20) return;
    const uint32_t tree = t_clearTree ? t_clearTree : r3;
    const uint32_t head = tree >= 0x1000 ? GRead32(base, tree + 4) : 0;
    std::fprintf(stderr, "[tree] erase %d deep: tree %08X (head %08X: +0 %08X +4 %08X +8 %08X _Isnil %u; size %u) thread %lu clear lr %08X\n",
                 t_eraseDepth, tree, head, head >= 0x1000 ? GRead32(base, head) : 0, head >= 0x1000 ? GRead32(base, head + 4) : 0,
                 head >= 0x1000 ? GRead32(base, head + 8) : 0, head >= 0x1000 ? GRead8(base, head + 29) : 0,
                 tree >= 0x1000 ? GRead32(base, tree + 8) : 0, GetCurrentThreadId(), t_clearLr);
    char line[512];
    int n = std::snprintf(line, sizeof(line), "[tree]   clear's chain:");
    uint32_t frame = t_clearTree ? t_clearR1 : 0;
    for (int i = 0; i < 14 && frame && frame < 0xFFFFFFF0u && n < 490; ++i)
    {
        const uint32_t prev = GRead32(base, frame);
        if (!prev || prev <= frame || prev - frame > 0x10000) break;
        n += std::snprintf(line + n, sizeof(line) - n, " %08X", GRead32(base, prev - 8));
        frame = prev;
    }
    std::fprintf(stderr, "%s\n", line);
    for (int d = 0; d < 64 && d < t_eraseDepth; d += 8)
    {
        n = std::snprintf(line, sizeof(line), "[tree]   path %2d:", d);
        for (int k = d; k < d + 8 && k < 64 && k < t_eraseDepth; ++k)
        {
            const uint32_t node = t_erasePath[k];
            n += std::snprintf(line + n, sizeof(line) - n, " %08X(%08X,%08X,%u)", node, node >= 0x1000 ? GRead32(base, node) : 0,
                               node >= 0x1000 ? GRead32(base, node + 8) : 0, node >= 0x1000 ? GRead8(base, node + 29) : 0);
        }
        std::fprintf(stderr, "%s\n", line);
    }
    std::fflush(stderr);
}

extern "C" PPC_FUNC(__imp__sub_824DA3D8);
PPC_FUNC(sub_824DA3D8)
{
    const int depth = t_eraseDepth++;
    if (depth < 64) t_erasePath[depth] = ctx.r4.u32;
    if (depth == 300) EraseTooDeep(base, ctx.r3.u32);
    __imp__sub_824DA3D8(ctx, base);
    --t_eraseDepth;
}

// TTT2_DIAG_HEAPCHECK=1 - the per-frame maps' heads (2026-11-08, the CHANGE TEAMS stack overflow): the object at [0x82EE0B58] holds
// five maps (24 bytes each: +4 the head node, +8 the size) that sub_824378B0 empties every frame (sub_82567DE8) by swapping
// each with a fresh one; one of those fresh heads was found written over before the next frame's swap (_Isnil 0, its
// left 1, its right 0). The heads are taken after every swap and checked after every task step (sub_82A80AD8): the first
// step after which one is broken - or its map holds another head - is logged with the task, once per swap.
namespace treewatch {
std::mutex g_lock;
uint32_t g_obj = 0, g_head[5] = {};
bool g_armed = false;
int g_logged = 0;                         // reports so far (40 at most)
}

static bool HeadOk(uint8_t* base, uint32_t head)
{
    return head >= 0x1000 && GRead8(base, head + 29) == 1 && GRead32(base, head) >= 0x1000 && GRead32(base, head + 8) >= 0x1000 &&
           GRead32(base, head + 4) >= 0x1000;
}

void TreeWatchCheck(uint8_t* base, const char* where, uint32_t task)
{
    if (!HeapCheckOn()) return;
    std::lock_guard lk(treewatch::g_lock);
    if (!treewatch::g_armed) return;
    for (int i = 0; i < 5; ++i)
    {
        const uint32_t E = treewatch::g_obj + 24u * uint32_t(i), head = treewatch::g_head[i];
        const uint32_t now = GRead32(base, E + 4);
        if (now == head && HeadOk(base, head)) continue;
        treewatch::g_armed = false;
        if (treewatch::g_logged++ >= 40) return;
        const uint32_t vt = task ? GRead32(base, task) : 0;
        std::fprintf(stderr,
                     "[tree-watch] map %d (%08X) broken after %s: task %08X vtable %08X step %08X thread %lu; its head %08X now %08X; "
                     "head %08X: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                     i, E, where, task, vt, vt ? GRead32(base, vt + 4) : 0, GetCurrentThreadId(), head, now, head,
                     GRead32(base, head), GRead32(base, head + 4), GRead32(base, head + 8), GRead32(base, head + 12),
                     GRead32(base, head + 16), GRead32(base, head + 20), GRead32(base, head + 24), GRead32(base, head + 28));
        std::fflush(stderr);
        return;
    }
}

extern "C" PPC_FUNC(__imp__sub_824378B0);
PPC_FUNC(sub_824378B0)
{
    const uint32_t obj = ctx.r3.u32;
    __imp__sub_824378B0(ctx, base);
    if (!HeapCheckOn() || obj != GRead32(base, 0x82EE0B58u)) return;
    std::lock_guard lk(treewatch::g_lock);
    treewatch::g_obj = obj;
    treewatch::g_armed = true;
    for (int i = 0; i < 5; ++i) treewatch::g_head[i] = GRead32(base, obj + 24u * uint32_t(i) + 4);
    for (int i = 0; i < 5; ++i)
        if (!HeadOk(base, treewatch::g_head[i]))
        {
            if (treewatch::g_logged++ < 40)
                std::fprintf(stderr, "[tree-watch] map %d's fresh head %08X is not a head\n", i, treewatch::g_head[i]);
            treewatch::g_armed = false;
        }
}

// A rollback rewind put the guest's memory back (rollback.cpp's Restore): the blocks freed since that snapshot are live
// again there and the maps hold that frame's heads - the double-free detector forgets its frees, the tree watch waits for
// the next swap (both TTT2_DIAG_HEAPCHECK only).
void DiagHeapRewound()
{
    if (!HeapCheckOn()) return;
    {
        std::lock_guard l(diagheap::g_lock);
        diagfree::g_freed.clear();
    }
    std::lock_guard lk(treewatch::g_lock);
    treewatch::g_armed = false;
}
