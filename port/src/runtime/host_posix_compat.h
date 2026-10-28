#pragma once

// Small POSIX services whose spelling differs between Linux and macOS. Every
// function here is async-signal-safe on both: the VM fault handler and the
// diagnostic sampler call them from signal handlers.
#if !defined(__linux__) && !defined(__APPLE__)
#error "host_posix_compat.h is for Linux and macOS"
#endif

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#else
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <os/os_sync_wait_on_address.h>
#include <sys/ucontext.h> // <ucontext.h> needs _XOPEN_SOURCE on Darwin
#endif

// The x86-64 slice runs on macOS 13 (Intel Macs on Ventura), before the os_sync wrappers
// (14.4). There the private ulock calls they wrap are used directly, as libc++'s
// std::atomic::wait does on every macOS.
#if defined(__APPLE__) && __MAC_OS_X_VERSION_MIN_REQUIRED < 140400
#define TTT2_MAC_ULOCK 1
extern "C" int __ulock_wait(uint32_t operation, void* address, uint64_t value, uint32_t timeoutMicroseconds);
extern "C" int __ulock_wake(uint32_t operation, void* address, uint64_t wakeValue);
#endif

namespace host_posix {

// Block while *word still holds expected; wake-ups may be spurious. A negative
// timeout waits without limit. Process-private addresses only.
inline void WaitOnWord(int* word, int expected, int64_t timeoutNs = -1)
{
#if defined(__linux__)
    if (timeoutNs < 0)
    {
        syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
        return;
    }
    timespec timeout{time_t(timeoutNs / 1000000000), long(timeoutNs % 1000000000)};
    syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, expected, &timeout, nullptr, 0);
#elif defined(TTT2_MAC_ULOCK)
    constexpr uint32_t CompareAndWait = 1, NoErrno = 0x01000000; // UL_COMPARE_AND_WAIT, ULF_NO_ERRNO
    if (timeoutNs == 0) return;
    // A zero timeout means "no limit" to __ulock_wait; a timed wait lasts at least 1 us.
    const uint32_t timeoutUs = timeoutNs < 0 ? 0
        : uint32_t(std::min<int64_t>(std::max<int64_t>((timeoutNs + 999) / 1000, 1), UINT32_MAX));
    __ulock_wait(CompareAndWait | NoErrno, word, uint32_t(expected), timeoutUs);
#else
    // macOS 14.4+: the public wrappers over the kernel's ulock wait.
    if (timeoutNs < 0)
        os_sync_wait_on_address(word, uint64_t(uint32_t(expected)), sizeof(int),
                                OS_SYNC_WAIT_ON_ADDRESS_NONE);
    else if (timeoutNs > 0)
        os_sync_wait_on_address_with_timeout(word, uint64_t(uint32_t(expected)), sizeof(int),
                                             OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                             OS_CLOCK_MACH_ABSOLUTE_TIME, uint64_t(timeoutNs));
#endif
}

inline void WakeOne(int* word)
{
#if defined(__linux__)
    syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#elif defined(TTT2_MAC_ULOCK)
    __ulock_wake(1 | 0x01000000, word, 0); // UL_COMPARE_AND_WAIT | ULF_NO_ERRNO; no waiter is harmless
#else
    // ENOENT (no waiter) is the common, harmless result.
    os_sync_wake_by_address_any(word, sizeof(int), OS_SYNC_WAKE_BY_ADDRESS_NONE);
#endif
}

// A process-unique id for the calling thread (the kernel tid on Linux).
inline uint64_t ThreadId()
{
#if defined(__linux__)
    return uint64_t(syscall(SYS_gettid));
#else
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
#endif
}

// Copy from this process without faulting; false when any byte is unmapped
// or unreadable. Safe in a signal handler.
inline bool ReadOwnMemory(const void* address, void* output, size_t bytes)
{
    if (!bytes) return true;
#if defined(__linux__)
    size_t done = 0;
    while (done < bytes)
    {
        iovec local{static_cast<uint8_t*>(output) + done, bytes - done};
        iovec remote{static_cast<uint8_t*>(const_cast<void*>(address)) + done, bytes - done};
        const ssize_t got = syscall(SYS_process_vm_readv, getpid(), &local, 1ul, &remote, 1ul, 0ul);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        done += size_t(got);
    }
    return true;
#else
    mach_vm_size_t got = 0;
    const kern_return_t result = mach_vm_read_overwrite(
        mach_task_self(), mach_vm_address_t(uintptr_t(address)), mach_vm_size_t(bytes),
        mach_vm_address_t(uintptr_t(output)), &got);
    return result == KERN_SUCCESS && got == bytes;
#endif
}

// Raise signal number on thread (which must be a live thread of this process).
inline bool SignalThread(pthread_t thread, uint64_t nativeId, int number)
{
#if defined(__linux__)
    (void)thread;
    return syscall(SYS_tgkill, getpid(), pid_t(nativeId), number) == 0;
#else
    (void)nativeId;
    return pthread_kill(thread, number) == 0;
#endif
}

// Send the signal currently being handled back to this thread, after its
// disposition was reset, so the default action applies when the handler returns.
inline bool RaiseOnThisThread(int number)
{
#if defined(__linux__)
    return syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), number) == 0;
#else
    return pthread_kill(pthread_self(), number) == 0;
#endif
}

// The current thread's stack bounds [low, high).
inline bool CurrentStack(uintptr_t& low, uintptr_t& high)
{
#if defined(__linux__)
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0) return false;
    void* base = nullptr;
    size_t bytes = 0;
    const bool ok = pthread_attr_getstack(&attributes, &base, &bytes) == 0;
    pthread_attr_destroy(&attributes);
    if (!ok) return false;
    low = uintptr_t(base);
    high = uintptr_t(base) + bytes;
    return true;
#else
    // Darwin reports the stack's highest address and its size.
    high = uintptr_t(pthread_get_stackaddr_np(pthread_self()));
    low = high - pthread_get_stacksize_np(pthread_self());
    return true;
#endif
}

// pipe2(fds, O_CLOEXEC). macOS has no pipe2: a descriptor can leak into a
// child spawned by another thread between pipe() and fcntl().
inline int PipeCloexec(int fds[2])
{
#if defined(__linux__)
    return pipe2(fds, O_CLOEXEC);
#else
    if (pipe(fds) != 0) return -1;
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0)
    {
        const int failure = errno;
        close(fds[0]);
        close(fds[1]);
        errno = failure;
        return -1;
    }
    return 0;
#endif
}

// The running executable's path ("/proc/self/exe" on Linux). Not signal-safe.
// Returns false when the buffer is too small or the path is unknown.
inline bool ExecutablePath(char* buffer, size_t capacity)
{
#if defined(__linux__)
    const ssize_t got = readlink("/proc/self/exe", buffer, capacity - 1);
    if (got <= 0 || size_t(got) >= capacity - 1) return false;
    buffer[got] = 0;
    return true;
#else
    uint32_t size = uint32_t(capacity);
    if (_NSGetExecutablePath(buffer, &size) != 0) return false;
    // Resolve symlinks and ./.. components, as /proc/self/exe does.
    char resolved[PATH_MAX];
    if (!realpath(buffer, resolved)) return true;
    const size_t length = strlen(resolved);
    if (length >= capacity) return false;
    memcpy(buffer, resolved, length + 1);
    return true;
#endif
}

#if defined(__APPLE__) && defined(__aarch64__)
// Darwin arm64 signal context: uc_mcontext points at __ss (registers) and
// __es (exception state: fault address and syndrome).
inline uintptr_t ContextPc(const void* context)
{
    const auto* uc = static_cast<const ucontext_t*>(context);
    return uintptr_t(__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss));
}
inline uintptr_t ContextFp(const void* context)
{
    const auto* uc = static_cast<const ucontext_t*>(context);
    return uintptr_t(__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss));
}
// ESR_EL1 of a data abort: bit 6 (WnR) is set for a write. Cache maintenance
// (DC ZVA reports as a write) and atomics also report as writes.
inline bool ContextIsWrite(const void* context)
{
    const auto* uc = static_cast<const ucontext_t*>(context);
    const uint64_t esr = uc->uc_mcontext->__es.__esr;
    const unsigned exceptionClass = unsigned(esr >> 26) & 0x3F;
    const bool dataAbort = exceptionClass == 0x24 || exceptionClass == 0x25;
    return dataAbort && (esr & (1u << 6)) != 0;
}
#elif defined(__APPLE__) && defined(__x86_64__)
// Darwin x86-64 signal context (Intel Macs and Rosetta): rip/rbp in __ss, the page-fault
// error code in __es.__err.
inline uintptr_t ContextPc(const void* context)
{
    return uintptr_t(static_cast<const ucontext_t*>(context)->uc_mcontext->__ss.__rip);
}
inline uintptr_t ContextFp(const void* context)
{
    return uintptr_t(static_cast<const ucontext_t*>(context)->uc_mcontext->__ss.__rbp);
}
// Page-fault error code bit 1: the access was a write.
inline bool ContextIsWrite(const void* context)
{
    return (static_cast<const ucontext_t*>(context)->uc_mcontext->__es.__err & 2) != 0;
}
#endif

} // namespace host_posix
