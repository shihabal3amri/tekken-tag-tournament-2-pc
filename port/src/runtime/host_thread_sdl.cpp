#include "host_thread.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace host_thread {
struct StartGate {
    std::mutex mutex;
    std::condition_variable ready;
    bool released = false;
    bool cancelled = false;
    Procedure procedure = nullptr;
    void* argument = nullptr;
};

struct Handle {
    SDL_Thread* thread = nullptr;
    SDL_ThreadID id = 0;
    std::shared_ptr<StartGate> gate;
};

namespace {
struct StartContext { Procedure procedure; void* argument; };

int DetachedEntry(void* raw) {
    std::unique_ptr<StartContext> context(static_cast<StartContext*>(raw));
    return static_cast<int>(context->procedure(context->argument));
}

int ManagedEntry(void* raw) {
    std::unique_ptr<std::shared_ptr<StartGate>> owner(
        static_cast<std::shared_ptr<StartGate>*>(raw));
    auto gate = std::move(*owner);
    owner.reset();
    Procedure procedure = nullptr;
    void* argument = nullptr;
    {
        std::unique_lock lock(gate->mutex);
        gate->ready.wait(lock, [&] { return gate->released; });
        if (gate->cancelled) return 0;
        procedure = gate->procedure;
        argument = gate->argument;
    }
    return static_cast<int>(procedure(argument));
}

SDL_Thread* Create(size_t stackBytes, SDL_ThreadFunction entry, void* argument) {
#if defined(__APPLE__)
    // Darwin's pthread_attr_setstacksize refuses a size that is not a multiple of the page size
    // (16 KiB on arm64) and SDL ignores the failure: the thread would get the 512 KiB default.
    stackBytes = (stackBytes + 0x3FFF) & ~size_t(0x3FFF);
#endif
    SDL_PropertiesID properties = SDL_CreateProperties();
    if (!properties) return nullptr;
    SDL_SetPointerProperty(properties, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER,
                           reinterpret_cast<void*>(entry));
    SDL_SetPointerProperty(properties, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, argument);
    SDL_SetNumberProperty(properties, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER,
                          static_cast<Sint64>(stackBytes));
    SDL_Thread* thread = SDL_CreateThreadWithProperties(properties);
    SDL_DestroyProperties(properties);
    return thread;
}
} // namespace

bool StartDetached(size_t stackBytes, Procedure procedure, void* argument) {
    if (!procedure) return false;
    auto context = std::unique_ptr<StartContext>(new (std::nothrow) StartContext{procedure, argument});
    if (!context) return false;
    SDL_Thread* thread = Create(stackBytes, DetachedEntry, context.get());
    if (!thread) return false;
    context.release();
    SDL_DetachThread(thread);
    return true;
}

uint32_t CurrentId() { return static_cast<uint32_t>(SDL_GetCurrentThreadID()); }

void Pause() {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

unsigned AvailableProcessors() {
    const int count = SDL_GetNumLogicalCPUCores();
    return count > 0 ? static_cast<unsigned>(count) : 1;
}

Handle* StartManaged(size_t stackBytes, Procedure procedure, void* argument) {
    if (!procedure) return nullptr;
    auto handle = std::unique_ptr<Handle>(new (std::nothrow) Handle);
    if (!handle) return nullptr;
    try { handle->gate = std::make_shared<StartGate>(); }
    catch (const std::bad_alloc&) { return nullptr; }
    handle->gate->procedure = procedure;
    handle->gate->argument = argument;
    auto owner = std::unique_ptr<std::shared_ptr<StartGate>>(
        new (std::nothrow) std::shared_ptr<StartGate>(handle->gate));
    if (!owner) return nullptr;
    handle->thread = Create(stackBytes, ManagedEntry, owner.get());
    if (!handle->thread) return nullptr;
    owner.release();
    handle->id = SDL_GetThreadID(handle->thread);
    return handle.release();
}

bool ReleaseStart(Handle* handle) {
    if (!handle || !handle->gate) return false;
    {
        std::lock_guard lock(handle->gate->mutex);
        if (handle->gate->cancelled) return false;
        handle->gate->released = true;
    }
    handle->gate->ready.notify_one();
    return true;
}

Handle* Current() {
    auto* handle = new (std::nothrow) Handle;
    if (handle) handle->id = SDL_GetCurrentThreadID();
    return handle;
}

void Close(Handle* handle) {
    if (!handle) return;
    if (auto gate = std::move(handle->gate)) {
        {
            std::lock_guard lock(gate->mutex);
            if (!gate->released) { gate->cancelled = true; gate->released = true; }
        }
        gate->ready.notify_one();
    }
    // Closing from the managed callback itself must not wait for that callback.
    if (handle->thread) SDL_DetachThread(handle->thread);
    delete handle;
}

bool PinToGuestCpu(Handle* handle, uint32_t guestMask) {
    if (!handle || !guestMask || (guestMask & (guestMask - 1)) || (guestMask & ~0x3Fu)) return false;
#if defined(_WIN32)
    DWORD_PTR allowed = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) return false;
    unsigned count = 0;
    for (DWORD_PTR bits = allowed; bits; bits &= bits - 1) ++count;
    if (!count) return false;
    unsigned guestCpu = 0;
    while ((guestMask >> guestCpu) != 1) ++guestCpu;
    unsigned index = guestCpu % count;
    DWORD_PTR selected = 0;
    for (unsigned bit = 0; bit < sizeof(DWORD_PTR) * 8; ++bit) {
        const DWORD_PTR one = DWORD_PTR(1) << bit;
        if ((allowed & one) && index-- == 0) { selected = one; break; }
    }
    HANDLE native = OpenThread(THREAD_SET_INFORMATION, FALSE, static_cast<DWORD>(handle->id));
    if (!native) return false;
    const bool success = selected && SetThreadAffinityMask(native, selected) != 0;
    CloseHandle(native);
    return success;
#else
    // SDL does not expose a portable thread-affinity operation.
    return false;
#endif
}

bool SetRelativePriority(Handle* handle, int32_t priority) {
    if (!handle) return false;
#if defined(_WIN32)
    HANDLE native = OpenThread(THREAD_SET_INFORMATION, FALSE, static_cast<DWORD>(handle->id));
    if (!native) return false;
    const int bounded = std::clamp(priority, int32_t(THREAD_PRIORITY_LOWEST),
                                  int32_t(THREAD_PRIORITY_HIGHEST));
    const bool success = SetThreadPriority(native, bounded) != 0;
    CloseHandle(native);
    return success;
#else
    // Guest priority is retained by KThread. Linux's default scheduler does
    // not offer equivalent unprivileged relative priorities.
    (void)priority;
    return true;
#endif
}
} // namespace host_thread
