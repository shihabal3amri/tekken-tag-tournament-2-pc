// Several consumers read the guest memory's write-watch bits (the GPU backend's texture tracker,
// rollback.cpp's state tracking). GetWriteWatch with WRITE_WATCH_FLAG_RESET hands a page's bit to
// whichever consumer asks first - rollback reading the whole guest heap every frame left the
// texture tracker blind (MEASURED 2026-10-18: fighters drawn with garbage textures). So every
// consumer publishes the pages it read, and the others drain those along with their own reads.
#include "kernel_shared.h"
#include <windows.h>
#include <atomic>
#include <vector>

namespace {
constexpr int kConsumers = 2;
constexpr uint32_t kWords = (1u << 20) / 64;      // one bit per 4 KB page of the 4 GB guest space
std::atomic<uint64_t> g_pending[kConsumers][kWords];
std::atomic<bool> g_any[kConsumers];
} // namespace

// Pages consumer `self` just read (and reset) with GetWriteWatch: flagged for the other consumers.
void WriteWatchPublish(int self, const PVOID* pages, size_t count)
{
    if (!count) return;
    for (int c = 0; c < kConsumers; ++c)
    {
        if (c == self) continue;
        for (size_t i = 0; i < count; ++i)
        {
            const uint64_t page = uint64_t(static_cast<const uint8_t*>(pages[i]) - g_base) >> 12;
            if (page < (1u << 20)) g_pending[c][page >> 6].fetch_or(1ull << (page & 63), std::memory_order_relaxed);
        }
        g_any[c].store(true, std::memory_order_release);
    }
}

// Pages in [firstPage, lastPage) another consumer read since consumer `self` last drained them.
void WriteWatchDrain(int self, uint32_t firstPage, uint32_t lastPage, std::vector<uint32_t>& out)
{
    if (!g_any[self].load(std::memory_order_acquire)) return;
    lastPage = lastPage < (1u << 20) ? lastPage : (1u << 20);
    for (uint32_t w = firstPage >> 6; w < (lastPage + 63) >> 6; ++w)
    {
        uint64_t bits = g_pending[self][w].load(std::memory_order_relaxed);
        if (!bits) continue;
        const uint32_t lo = w * 64;
        if (lo < firstPage) bits &= ~0ull << (firstPage - lo);
        if (lo + 64 > lastPage) bits &= ~0ull >> (lo + 64 - lastPage);
        if (!bits) continue;
        g_pending[self][w].fetch_and(~bits, std::memory_order_relaxed);
        for (; bits; bits &= bits - 1) out.push_back(lo + uint32_t(__builtin_ctzll(bits)));
    }
}
