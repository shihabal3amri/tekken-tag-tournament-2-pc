#pragma once

#include "gpu_hw_api.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>

namespace ttt2_hw {

// Runtime selection and failure isolation are shared by host GPU backends.
// A backend must provide ReportBackendFailure after its Get() function.
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

// The command processor and guest readback paths may enter the same backend.
inline std::recursive_mutex g_backendMutex;
void ReportBackendFailure(const char* what, const std::exception& error);

// Optional CPU-side timing around the shared D3D11/DXVK backend entry points.
// Separating lock wait from call time shows whether the game is waiting for
// another renderer thread or spending time inside a specific backend operation.
inline bool StageTimingOn()
{
    static const bool on = [] {
        const char* value = std::getenv("TTT2_GPU_STAGE_TIMING");
        return value && std::strcmp(value, "1") == 0;
    }();
    return on;
}

inline constexpr std::array<const char*, 8> kStageNames = {
    "vs", "draw", "resolve", "compare", "present", "readback", "dump", "writeback"
};
inline std::array<std::atomic<uint64_t>, kStageNames.size()> g_stageCalls{};
inline std::array<std::atomic<uint64_t>, kStageNames.size()> g_stageLockNs{};
inline std::array<std::atomic<uint64_t>, kStageNames.size()> g_stageCallNs{};

inline size_t StageIndex(const char* what)
{
    for (size_t i = 0; i < kStageNames.size(); ++i)
        if (std::strcmp(what, kStageNames[i]) == 0) return i;
    return kStageNames.size();
}

inline void RecordStage(size_t index, std::chrono::steady_clock::time_point start,
                        std::chrono::steady_clock::time_point locked)
{
    if (index == kStageNames.size()) return;
    const auto end = std::chrono::steady_clock::now();
    g_stageCalls[index].fetch_add(1, std::memory_order_relaxed);
    g_stageLockNs[index].fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(locked - start).count(),
        std::memory_order_relaxed);
    g_stageCallNs[index].fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - locked).count(),
        std::memory_order_relaxed);
}

inline void ReportStageTiming()
{
    if (!StageTimingOn()) return;
    for (size_t i = 0; i < kStageNames.size(); ++i)
    {
        const uint64_t calls = g_stageCalls[i].exchange(0, std::memory_order_relaxed);
        const uint64_t lockNs = g_stageLockNs[i].exchange(0, std::memory_order_relaxed);
        const uint64_t callNs = g_stageCallNs[i].exchange(0, std::memory_order_relaxed);
        if (calls)
            std::fprintf(stderr, "[gpu-stage] %s calls=%llu lock=%.2f ms body=%.2f ms\n",
                         kStageNames[i], (unsigned long long)calls,
                         double(lockNs) / 1000000.0, double(callNs) / 1000000.0);
    }
}

template <typename F>
inline bool Guarded(const char* what, F&& f)
{
    using clock = std::chrono::steady_clock;
    const bool timing = StageTimingOn();
    const size_t stage = timing ? StageIndex(what) : kStageNames.size();
    const auto start = timing ? clock::now() : clock::time_point{};
    std::lock_guard<std::recursive_mutex> lock(g_backendMutex);
    const auto locked = timing ? clock::now() : clock::time_point{};
    try
    {
        f();
        if (timing) RecordStage(stage, start, locked);
        return true;
    }
    catch (const std::exception& e)
    {
        if (timing) RecordStage(stage, start, locked);
        g_failed.store(true, std::memory_order_relaxed);
        ReportBackendFailure(what, e);
#if !defined(_WIN32)
        // A failed DXVK Native device makes the software renderer unusably slow.
        // Keep explicit TTT2_GPU_HW=0 software runs available for diagnostics.
        if (RequestedMode() != Mode::Off)
        {
            std::fprintf(stderr, "[gpu-hw] DXVK Native hardware renderer unavailable; exiting instead of falling back to CPU rendering.\n");
            std::exit(EXIT_FAILURE);
        }
#endif
        return false;
    }
}

} // namespace ttt2_hw
