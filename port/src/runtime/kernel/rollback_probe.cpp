// Rollback feasibility probe (diagnostics): how much guest memory the game writes per frame.
// TTT2_DIRTY_MEASURE=1: at every game swap (VdSwap, the main thread's frame boundary) the guest
// address space's write-watch bits are read and reset; the pages written during that frame are
// counted per 256 MB region and per 1 MB block. Every 120 frames a "[dirty]" report: mean / max
// MB per frame by region, and the busiest blocks. While it runs, the GPU backend's texture tracker
// treats every range as written (it shares the write-watch bits), so textures re-upload on every
// use - slower, but the picture stays right.
#include "kernel_shared.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <vector>

bool DirtyMeasureOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_DIRTY_MEASURE"); return e && *e == '1'; }();
    return on;
}

// TTT2_INPUT_BT=1: every distinct guest call chain that reaches the gameplay input read
// (sub_8225BFC0, input_delay.cpp) is logged once, with the host thread and the calls per frame.
bool InputBacktraceOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_INPUT_BT"); return e && *e == '1'; }();
    return on;
}

namespace {
std::atomic<uint32_t> g_inputCalls{ 0 };
uint32_t g_swapCount = 0;
}

void InputBacktrace(PPCContext& ctx, uint32_t player)
{
    if (!InputBacktraceOn()) return;
    ++g_inputCalls;
    uint32_t chain[24];
    int depth = 0;
    chain[depth++] = uint32_t(ctx.lr);
    uint32_t frame = ctx.r1.u32;
    for (int i = 0; i < 23 && frame && frame < 0xFFFFFFF0u; ++i)
    {
        const uint32_t prev = GRead32(g_base, frame);
        if (!prev || prev <= frame || prev - frame > 0x10000) break;
        chain[depth++] = GRead32(g_base, prev - 8);
        frame = prev;
    }
    uint64_t key = 1469598103934665603ull ^ player;
    for (int i = 0; i < depth; ++i) key = (key ^ chain[i]) * 1099511628211ull;
    static std::mutex m;
    static std::set<uint64_t> seen;
    std::lock_guard l(m);
    if (!seen.insert(key).second || seen.size() > 2000) return;
    char line[600];
    int n = std::snprintf(line, sizeof(line), "[inputbt] tid %lu player %u swap %u obj %08X:", GetCurrentThreadId(), player, g_swapCount, ctx.r3.u32);
    for (int i = 0; i < depth; ++i) n += std::snprintf(line + n, sizeof(line) - n, " %08X", chain[i]);
    std::fprintf(stderr, "%s\n", line);
}

// TTT2_ALLOC_LOG=1: every guest memory allocation the kernel serves (kernel_memory.cpp), with the
// thread and the guest call chain - which subsystem owns which arena.
void LogGuestAlloc(const char* what, uint32_t addr, uint32_t size, PPCContext& ctx)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_ALLOC_LOG"); return e && *e == '1'; }();
    if (!on) return;
    char line[400];
    int n = std::snprintf(line, sizeof(line), "[alloc] %s %08X+%X tid %lu:", what, addr, size, GetCurrentThreadId());
    n += std::snprintf(line + n, sizeof(line) - n, " %08X", uint32_t(ctx.lr));
    uint32_t frame = ctx.r1.u32;
    for (int i = 0; i < 10 && frame && frame < 0xFFFFFFF0u; ++i)
    {
        const uint32_t prev = GRead32(g_base, frame);
        if (!prev || prev <= frame || prev - frame > 0x10000) break;
        n += std::snprintf(line + n, sizeof(line) - n, " %08X", GRead32(g_base, prev - 8));
        frame = prev;
    }
    std::fprintf(stderr, "%s\n", line);
}

void DirtyMeasureFrame()
{
    if (InputBacktraceOn())
    {
        ++g_swapCount;
        static uint32_t lastCalls = 0xFFFFFFFF;
        const uint32_t calls = g_inputCalls.exchange(0);
        if (calls != lastCalls) std::fprintf(stderr, "[inputbt] swap %u: %u input reads this frame\n", g_swapCount, calls);
        lastCalls = calls;
    }
    if (!DirtyMeasureOn() || !g_base) return;
    static std::vector<PVOID> pages(1u << 20);
    static uint64_t frames = 0;
    static double sum[16] = {}, peak[16] = {};
    static std::map<uint32_t, uint64_t> blocks;      // 1 MB block -> pages written (summed)
    ULONG_PTR count = pages.size();
    DWORD granularity = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_base, 0x100000000ull, pages.data(), &count, &granularity) != 0) return;
    if (frames++ == 0) return;                       // the first read covers everything since boot
    uint32_t perRegion[16] = {};
    for (ULONG_PTR i = 0; i < count; ++i)
    {
        const uint32_t guest = uint32_t(static_cast<uint8_t*>(pages[i]) - g_base);
        ++perRegion[guest >> 28];
        ++blocks[guest >> 20];
    }
    for (int r = 0; r < 16; ++r)
    {
        const double mb = perRegion[r] * 4096.0 / 1048576.0;
        sum[r] += mb;
        peak[r] = (std::max)(peak[r], mb);
    }
    if ((frames - 1) % 120 != 0) return;
    char line[512];
    int n = std::snprintf(line, sizeof(line), "[dirty] %llu frames, MB written per frame (mean/max) by region:", (unsigned long long)(frames - 1));
    double total = 0;
    for (int r = 0; r < 16; ++r)
    {
        total += sum[r] / double(frames - 1);
        if (peak[r] > 0) n += std::snprintf(line + n, sizeof(line) - n, " %X0000000 %.2f/%.2f", r, sum[r] / double(frames - 1), peak[r]);
    }
    std::fprintf(stderr, "%s; total %.2f MB/frame\n", line, total);
    std::vector<std::pair<uint64_t, uint32_t>> top;
    for (const auto& [b, c] : blocks) top.push_back({ c, b });
    std::sort(top.rbegin(), top.rend());
    n = std::snprintf(line, sizeof(line), "[dirty]   busiest 1 MB blocks (KB per frame):");
    for (size_t i = 0; i < top.size() && i < 16; ++i)
        n += std::snprintf(line + n, sizeof(line) - n, " %03X00000=%.0f", top[i].second, top[i].first * 4.0 / double(frames - 1));
    std::fprintf(stderr, "%s\n", line);
    std::fill(std::begin(sum), std::end(sum), 0.0);
    std::fill(std::begin(peak), std::end(peak), 0.0);
    blocks.clear();
    frames = 1;
}
