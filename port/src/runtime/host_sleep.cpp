#include "host_sleep.h"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <cerrno>
#include <time.h>
#endif

namespace host_sleep {
void For100ns(int64_t ticks, bool highResolution)
{
    if (ticks <= 0) return;
    if (!highResolution)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(ticks / 10 + (ticks % 10 != 0)));
        return;
    }

#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -ticks;
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
    {
        WaitForSingleObject(timer, INFINITE);
        return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(ticks / 10 + (ticks % 10 != 0)));
#elif defined(__linux__)
    timespec remaining{static_cast<time_t>(ticks / 10000000),
                       static_cast<long>(ticks % 10000000) * 100};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &remaining, &remaining) == EINTR) {}
#elif defined(__APPLE__)
    // macOS has no clock_nanosleep; nanosleep keeps nanosecond requests.
    timespec remaining{static_cast<time_t>(ticks / 10000000),
                       static_cast<long>(ticks % 10000000) * 100};
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {}
#else
    std::this_thread::sleep_for(std::chrono::microseconds(ticks / 10 + (ticks % 10 != 0)));
#endif
}
} // namespace host_sleep
