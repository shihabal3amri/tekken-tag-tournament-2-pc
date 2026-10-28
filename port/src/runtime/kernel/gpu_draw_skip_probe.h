#pragma once

// Opt-in diagnostic, called only by the command-processor thread. Deliberately
// changes rendered results; never use this as a correctness-preserving setting.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if !defined(_WIN32)
#include <time.h>
#endif

namespace ttt2_hw::draw_skip_probe {
enum class Mode { Off, Early, Late };
struct Config { Mode mode = Mode::Off; unsigned percent = 0; };

inline bool Parse(const char* text, Config& result)
{
    char name[16]{}, extra{};
    unsigned percent = 0;
    if (std::sscanf(text, " %15s %c", name, &extra) == 1 &&
        std::strcmp(name, "off") == 0) { result = {}; return true; }
    if (std::sscanf(text, " %15s %u %c", name, &percent, &extra) != 2 ||
        (percent != 25 && percent != 50 && percent != 75)) return false;
    if (std::strcmp(name, "early") == 0) result = {Mode::Early, percent};
    else if (std::strcmp(name, "late") == 0) result = {Mode::Late, percent};
    else return false;
    return true;
}

inline bool Selected(uint64_t ordinal, unsigned percent)
{
    return ordinal % 4 < percent / 25;
}

using Clock = std::chrono::steady_clock;
inline const char* ControlPath()
{
    static const char* path = std::getenv("TTT2_DRAW_SKIP_FILE");
    return path && *path ? path : nullptr;
}
inline double ThreadCpuMs()
{
#if !defined(_WIN32)
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0)
        return double(time.tv_sec) * 1000 + double(time.tv_nsec) / 1e6;
#endif
    return 0;
}
inline const char* Name(Mode mode)
{
    return mode == Mode::Early ? "early" : mode == Mode::Late ? "late" : "off";
}
struct Counters {
    uint64_t frames = 0, eligible = 0, selected = 0;
    uint64_t early = 0, late = 0, submitted = 0;
    double drawWallMs = 0;
};
inline Config config;
inline Counters counters;
inline uint64_t ordinal = 0;
inline bool suppressLate = false;
inline Clock::time_point lastPoll{}, since{};
inline double cpuStartMs = 0;

inline void Report(Clock::time_point now)
{
    if (since != Clock::time_point{} && counters.frames)
    {
        const double seconds = std::chrono::duration<double>(now - since).count();
        std::fprintf(stderr,
            "[draw-skip] mode=%s percent=%u seconds=%.2f frames=%llu fps=%.2f "
            "eligible=%llu selected=%llu early_skipped=%llu late_skipped=%llu "
            "indexed_submitted=%llu draw_wall_ms/frame=%.3f cp_cpu_ms/frame=%.3f\n",
            Name(config.mode), config.percent, seconds,
            static_cast<unsigned long long>(counters.frames), counters.frames / seconds,
            static_cast<unsigned long long>(counters.eligible),
            static_cast<unsigned long long>(counters.selected),
            static_cast<unsigned long long>(counters.early),
            static_cast<unsigned long long>(counters.late),
            static_cast<unsigned long long>(counters.submitted),
            counters.drawWallMs / counters.frames,
            (ThreadCpuMs() - cpuStartMs) / counters.frames);
    }
    counters = {};
    since = now;
    cpuStartMs = ThreadCpuMs();
}

// Called at a completed present, so a mode change cannot split a guest draw.
inline void Present()
{
    const char* path = ControlPath();
    if (!path) return;
    const auto now = Clock::now();
    if (since == Clock::time_point{}) Report(now);
    else ++counters.frames;
    ordinal = 0; // The same ordinal mask is used by both early and late tests.
    if (now - lastPoll >= std::chrono::seconds(1))
    {
        lastPoll = now;
        if (FILE* input = std::fopen(path, "r"))
        {
            char text[64]{};
            const size_t bytes = std::fread(text, 1, sizeof(text) - 1, input);
            const bool complete = bytes < sizeof(text) - 1 && !std::ferror(input);
            std::fclose(input);
            Config next;
            if (complete && Parse(text, next) &&
                (next.mode != config.mode || next.percent != config.percent))
            {
                Report(now);
                config = next;
                std::fprintf(stderr, "[draw-skip] switched mode=%s percent=%u\n",
                             Name(config.mode), config.percent);
            }
        }
    }
    if (now - since >= std::chrono::seconds(5)) Report(now);
}

struct DrawScope {
    bool enabled = false, previous = false, early = false;
    Clock::time_point start{};
    explicit DrawScope(bool triangleGeometry)
    {
        if (!ControlPath()) return;
        enabled = true;
        previous = suppressLate;
        suppressLate = false;
        if (triangleGeometry)
        {
            start = Clock::now();
            ++counters.eligible;
            const bool chosen = config.mode != Mode::Off && Selected(ordinal++, config.percent);
            if (chosen) ++counters.selected;
            early = chosen && config.mode == Mode::Early;
            if (early) ++counters.early;
            suppressLate = chosen && config.mode == Mode::Late;
        }
    }
    ~DrawScope()
    {
        if (!enabled) return;
        if (start != Clock::time_point{})
            counters.drawWallMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        suppressLate = previous;
    }
    DrawScope(const DrawScope&) = delete;
    DrawScope& operator=(const DrawScope&) = delete;
};

inline bool SuppressIndexedDraw()
{
    if (!ControlPath()) return false;
    if (suppressLate) { ++counters.late; return true; }
    ++counters.submitted;
    return false;
}
} // namespace ttt2_hw::draw_skip_probe
