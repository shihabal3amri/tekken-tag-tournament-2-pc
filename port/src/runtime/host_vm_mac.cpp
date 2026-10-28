// Guest VM backend for macOS (Apple Silicon and Intel).
//
// The API in host_vm.h works in 4 KiB guest pages: write-watch results,
// permissions and the rollback observer are all per 4 KiB page. Apple Silicon
// maps memory in 16 KiB host pages, so mprotect cannot give each 4 KiB page
// its own protection. This backend keeps the 4 KiB bookkeeping of the Linux
// backend (permission, Armed/Dirty, rollback mask) and derives one native
// protection for each host page from the sub-pages it holds:
//
//  - The most permissive guest permission wins. A host page that mixes
//    NoAccess or ReadOnly sub-pages with writable ones is readable or writable
//    as a whole, so those sub-pages lose their guard (see "What 16 KiB costs").
//  - A host page with a writable sub-page stays read-only while any writable
//    sub-page is write-watch armed or rollback protected.
//  - A write fault opens the whole host page: every writable sub-page in it is
//    marked Dirty (conservative), and every rollback-protected sub-page in it is
//    handed to the rollback observer first, so the observer captures all of
//    them before any can change.
//
// Faults arrive as SIGBUS (protection failure on mapped memory) or SIGSEGV;
// both go through the same handler. With 4 KiB host pages (an x86-64 build
// under Rosetta, or Intel Macs) the derived protection is exact and this
// backend behaves like the Linux one.
//
// What 16 KiB costs:
//  - Write-watch is per host page: one guest store marks up to four 4 KiB
//    pages dirty, so the GPU tracker re-uploads more and phys_mirror writes
//    back up to three unchanged neighbours.
//  - A host page is opened by its first write; later writes to its other
//    sub-pages are not observed until it is armed again. The rollback observer
//    therefore sees every protected sub-page of the host page as written by the
//    first writer (frame/foreign attribution is per 16 KiB).
//  - Guards are per 16 KiB: Protect(None/ReadOnly) on a 4 KiB page next to a
//    readable/writable one is not enforced in hardware. CanWrite,
//    QueryReadRegion and ReadMemory still answer from the 4 KiB permissions.
//  - The 64 KiB null guard is four whole host pages, so it is exact. As on
//    Android ARM64, a direct (unguarded) store there is a fatal fault; the
//    generated code's stores go through ppc_store_guard_linux.h instead of the
//    x86-64 single-step path.
#include "host_vm.h"
#include "guest_memory.h"
#include "host_posix_compat.h"

#if !defined(__APPLE__)
#error "The macOS guest VM backend requires macOS"
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <new>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

namespace host_vm {
namespace {
constexpr size_t PageSize = 4096; // The guest GPU and write-watch call sites use 4 KiB pages.
constexpr uint8_t Armed = 1, Dirty = 2;
constexpr uint8_t NoAccess = 0, Read = 1, Write = 2;

uint8_t* g_base = nullptr;
size_t g_bytes = 0;
size_t g_hostPage = 0;     // native page size: 16 KiB on Apple Silicon
size_t g_sub = 1;          // 4 KiB pages per host page
std::unique_ptr<uint8_t[]> g_state;
std::unique_ptr<uint8_t[]> g_permission;
std::unique_ptr<std::atomic<uint8_t>[]> g_rollbackPages;
std::unique_ptr<int8_t[]> g_native; // per host page: the protection last applied
uint8_t* g_mirrorStage = nullptr;
uintptr_t g_mirrorStart = 0;
size_t g_mirrorBytes = 0;
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
struct sigaction g_previousSegv{};
struct sigaction g_previousBus{};
bool g_segvInstalled = false;
bool g_busInstalled = false;
uintptr_t g_deferredFirst = 0, g_deferredLast = 0;
DeferredFaultHandler g_deferredHandler = nullptr;
std::atomic<UnhandledFaultHandler> g_unhandledHandler{nullptr};
CaptureRollbackWriter g_captureRollback = nullptr;
RollbackWriteHandler g_rollbackWrite = nullptr;
std::atomic<bool> g_rollbackRegistered{false};
int g_faultPipe[2] = {-1, -1};
// A guest can return from main while this worker is still waiting for faults.
// pthread_t has no joinable-thread destructor; ReleaseGuest joins explicitly.
pthread_t g_faultWorker{};
bool g_faultWorkerStarted = false;
std::atomic<uint64_t> g_faultWorkerTid{0};

struct FaultRequest
{
    uintptr_t address;
    bool rollback = false;
    uint64_t writer = 0;
    alignas(4) int done = 0;
    int handled = 0;
};

struct Lock {
    Lock() { while (g_lock.test_and_set(std::memory_order_acquire)) {} }
    ~Lock() { g_lock.clear(std::memory_order_release); }
};

bool Range(void* address, size_t bytes, size_t& first, size_t& last)
{
    if (!g_base || !bytes) { errno = EINVAL; return false; }
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (start < base || start - base >= g_bytes || bytes > g_bytes - (start - base))
    {
        errno = EINVAL;
        return false;
    }
    first = (start - base) / PageSize;
    last = (start - base + bytes + PageSize - 1) / PageSize;
    return true;
}

size_t HostFirst(size_t first) { return first / g_sub; }
size_t HostLast(size_t last) { return (last + g_sub - 1) / g_sub; }

// The protection a host page needs for the current 4 KiB tables. Caller holds
// g_lock (the fault handler holds it too).
int NativeFor(size_t host)
{
    uint8_t widest = NoAccess;
    bool watched = false;
    for (size_t page = host * g_sub, end = page + g_sub; page < end; ++page)
    {
        const uint8_t permission = g_permission[page];
        if (permission > widest) widest = permission;
        if (permission == Write &&
            (g_state[page] != Dirty || g_rollbackPages[page].load(std::memory_order_relaxed)))
            watched = true;
    }
    if (widest == NoAccess) return PROT_NONE;
    if (widest == Read || watched) return PROT_READ;
    return PROT_READ | PROT_WRITE;
}

bool ProtectHost(size_t first, size_t last, int native)
{
    return mprotect(g_base + first * g_hostPage, (last - first) * g_hostPage, native) == 0;
}

// Apply NativeFor to host pages [first, last), one mprotect per run of equal,
// changed protection. Caller holds g_lock.
bool Commit(size_t first, size_t last)
{
    for (size_t host = first; host < last;)
    {
        const int native = NativeFor(host);
        if (native == g_native[host]) { ++host; continue; }
        const size_t begin = host++;
        while (host < last && g_native[host] != NativeFor(host) && NativeFor(host) == native) ++host;
        if (!ProtectHost(begin, host, native)) return false;
        for (size_t i = begin; i < host; ++i) g_native[i] = int8_t(native);
    }
    return true;
}

// Before tables change in a way that can remove write access, take write
// access away first. A store racing the update then faults and waits for
// g_lock instead of passing through a still-writable page unobserved; Commit
// reopens whatever the new tables allow. Caller holds g_lock.
bool RestrictWrites(size_t first, size_t last)
{
    for (size_t host = first; host < last;)
    {
        if (g_native[host] != (PROT_READ | PROT_WRITE)) { ++host; continue; }
        const size_t begin = host;
        while (host < last && g_native[host] == (PROT_READ | PROT_WRITE)) ++host;
        if (!ProtectHost(begin, host, PROT_READ)) return false;
        for (size_t i = begin; i < host; ++i) g_native[i] = PROT_READ;
    }
    return true;
}

// Re-arm [first, last). Caller holds g_lock.
bool ArmLocked(size_t first, size_t last)
{
    const size_t hostFirst = HostFirst(first), hostLast = HostLast(last);
    if (!RestrictWrites(hostFirst, hostLast)) return false;
    for (size_t page = first; page < last; ++page) g_state[page] = Armed;
    return Commit(hostFirst, hostLast);
}

void* FaultWorker(void*)
{
    g_faultWorkerTid.store(host_posix::ThreadId(), std::memory_order_release);
    for (;;)
    {
        FaultRequest* request = nullptr;
        ssize_t got;
        do { got = read(g_faultPipe[0], &request, sizeof(request)); }
        while (got < 0 && errno == EINTR);
        if (got != sizeof(request) || !request) break;
        request->handled = request->rollback
            ? g_rollbackRegistered.load(std::memory_order_acquire) && g_rollbackWrite(request->address, request->writer)
            : g_deferredHandler && g_deferredHandler(request->address);
        __atomic_store_n(&request->done, 1, __ATOMIC_RELEASE);
        host_posix::WakeOne(&request->done);
    }
    g_faultWorkerTid.store(0, std::memory_order_release);
    return nullptr;
}

bool DeferFault(uintptr_t address, bool rollback = false)
{
    // The worker must not wait for itself. A recursive fault means that the
    // callback touched the protected range and needs a different implementation.
    if (host_posix::ThreadId() == g_faultWorkerTid.load(std::memory_order_acquire))
        return false;
    FaultRequest request{address};
    request.rollback = rollback;
    if (rollback) request.writer = g_captureRollback(address);
    FaultRequest* pointer = &request;
    ssize_t sent;
    do { sent = write(g_faultPipe[1], &pointer, sizeof(pointer)); }
    while (sent < 0 && errno == EINTR);
    if (sent != sizeof(pointer)) return false;
    while (__atomic_load_n(&request.done, __ATOMIC_ACQUIRE) == 0)
        host_posix::WaitOnWord(&request.done, 0);
    return request.handled != 0;
}

void Chain(int number, siginfo_t* info, void* context)
{
    const auto& previous = number == SIGBUS ? g_previousBus : g_previousSegv;
    if (previous.sa_handler == SIG_IGN) return;
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN)
    {
        if (previous.sa_flags & SA_SIGINFO)
            previous.sa_sigaction(number, info, context);
        else
            previous.sa_handler(number);
        return;
    }
    struct sigaction defaultAction{};
    defaultAction.sa_handler = SIG_DFL;
    if (sigaction(number, &defaultAction, nullptr) != 0)
        _exit(128 + number);
    // Returning re-executes the faulting access under the default action.
    // Queue the signal as well, for a fault that was raised rather than taken.
    if (!host_posix::RaiseOnThisThread(number))
        _exit(128 + number);
}

// A data abort reports writes in ESR.WnR. Anything else (not expected for
// SIGBUS/SIGSEGV) is treated as a possible write; the tables decide.
bool MayBeWrite(void* context)
{
#if defined(__aarch64__)
    const auto* uc = static_cast<const ucontext_t*>(context);
    const uint64_t esr = uc->uc_mcontext->__es.__esr;
    const unsigned exceptionClass = unsigned(esr >> 26) & 0x3F;
    if (exceptionClass == 0x24 || exceptionClass == 0x25) return (esr & (1u << 6)) != 0;
    return true;
#else
    // x86-64 page-fault error code bit 1: the access was a write.
    const auto* uc = static_cast<const ucontext_t*>(context);
    return (uc->uc_mcontext->__es.__err & 2) != 0;
#endif
}

// Handle a write to the guest range. Returns true when the access may be retried.
bool OpenForWrite(uintptr_t address)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
    const size_t host = (address - base) / g_hostPage;
    const size_t first = host * g_sub, last = first + g_sub;
    // Every rollback-protected sub-page of this host page becomes writable
    // together, so the observer captures each one before the store lands.
    if (g_rollbackRegistered.load(std::memory_order_acquire))
        for (size_t page = first; page < last; ++page)
            if (g_rollbackPages[page].load(std::memory_order_acquire))
                DeferFault(base + page * PageSize, true);
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    bool writable = false;
    for (size_t page = first; page < last; ++page)
    {
        if (g_permission[page] != Write) continue;
        writable = true;
        // An observer that declined a page leaves it to write-watch, as the
        // Linux backend's fallthrough does.
        g_rollbackPages[page].store(0, std::memory_order_release);
        g_state[page] = Dirty;
    }
    bool retry = false;
    if (writable)
    {
        const int native = NativeFor(host);
        if (native == g_native[host]) retry = native & PROT_WRITE; // another thread opened it
        else if (mprotect(g_base + host * g_hostPage, g_hostPage, native) == 0)
        {
            g_native[host] = int8_t(native);
            retry = native & PROT_WRITE;
        }
    }
    g_lock.clear(std::memory_order_release);
    return retry;
}

void Fault(int number, siginfo_t* info, void* context)
{
    const int savedError = errno;
    if ((number == SIGSEGV || number == SIGBUS) && info && context && g_base)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
        const uintptr_t address = reinterpret_cast<uintptr_t>(info->si_addr);
        if (address >= base && address - base < g_bytes &&
            MayBeWrite(context) && OpenForWrite(address))
        {
            errno = savedError;
            return;
        }
        if (address >= g_deferredFirst && address < g_deferredLast &&
            g_deferredHandler && DeferFault(address))
        {
            errno = savedError;
            return;
        }
    }
    if (auto handler = g_unhandledHandler.load(std::memory_order_acquire))
        handler(number, info, context);
    Chain(number, info, context);
}
} // namespace

// Generated ARM64 stores and host writes use guest_memory's null guard. An
// unexpected direct null store is reported by host_fault instead of x86-style
// instruction single stepping (as on Android ARM64).
void CancelNullPageStep(void*) {}

uint8_t* ReserveGuest(size_t bytes)
{
    const long native = sysconf(_SC_PAGESIZE);
    if (g_base || bytes == 0 || native <= 0 || size_t(native) % PageSize ||
        bytes % size_t(native))
    {
        errno = EINVAL;
        return nullptr;
    }
    g_hostPage = size_t(native);
    g_sub = g_hostPage / PageSize;
    // Anonymous memory is committed on first touch on macOS (MAP_NORESERVE is
    // accepted and ignored).
    void* memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) return nullptr;
    const size_t pages = bytes / PageSize, hosts = bytes / g_hostPage;
    auto state = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[pages]);
    auto permission = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[pages]);
    auto rollbackPages = std::unique_ptr<std::atomic<uint8_t>[]>(new (std::nothrow) std::atomic<uint8_t>[pages]);
    auto nativeState = std::unique_ptr<int8_t[]>(new (std::nothrow) int8_t[hosts]);
    if (!state || !permission || !rollbackPages || !nativeState)
    {
        munmap(memory, bytes);
        errno = ENOMEM;
        return nullptr;
    }
    for (size_t page = 0; page < pages; ++page)
    {
        state[page] = Armed;
        permission[page] = Write;
        rollbackPages[page].store(0, std::memory_order_relaxed);
    }
    for (size_t host = 0; host < hosts; ++host) nativeState[host] = int8_t(PROT_READ);
    g_base = static_cast<uint8_t*>(memory);
    g_bytes = bytes;
    g_state = std::move(state);
    g_permission = std::move(permission);
    g_rollbackPages = std::move(rollbackPages);
    g_native = std::move(nativeState);
    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = Fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSEGV, &action, &g_previousSegv) != 0)
    {
        const int failure = errno;
        ReleaseGuest(g_base, bytes);
        errno = failure;
        return nullptr;
    }
    g_segvInstalled = true;
    if (sigaction(SIGBUS, &action, &g_previousBus) != 0)
    {
        const int failure = errno;
        ReleaseGuest(g_base, bytes);
        errno = failure;
        return nullptr;
    }
    g_busInstalled = true;
    if (mprotect(g_base, bytes, PROT_READ) != 0)
    {
        const int failure = errno;
        ReleaseGuest(g_base, bytes);
        errno = failure;
        return nullptr;
    }
    return g_base;
}

void ReleaseGuest(uint8_t* base, size_t bytes)
{
    if (base != g_base || bytes != g_bytes || !base) return;
    if (g_faultWorkerStarted)
    {
        close(g_faultPipe[1]);
        g_faultPipe[1] = -1;
        pthread_join(g_faultWorker, nullptr);
        g_faultWorkerStarted = false;
        close(g_faultPipe[0]);
        g_faultPipe[0] = -1;
    }
    g_deferredFirst = g_deferredLast = 0;
    g_deferredHandler = nullptr;
    g_rollbackRegistered.store(false, std::memory_order_release);
    g_captureRollback = nullptr;
    g_rollbackWrite = nullptr;
    if (g_segvInstalled) sigaction(SIGSEGV, &g_previousSegv, nullptr);
    if (g_busInstalled) sigaction(SIGBUS, &g_previousBus, nullptr);
    g_segvInstalled = g_busInstalled = false;
    if (g_mirrorStage) munmap(g_mirrorStage, g_mirrorBytes);
    g_mirrorStage = nullptr;
    g_mirrorStart = 0;
    g_mirrorBytes = 0;
    munmap(base, bytes);
    g_state.reset();
    g_permission.reset();
    g_rollbackPages.reset();
    g_native.reset();
    g_base = nullptr;
    g_bytes = 0;
}

bool Protect(void* address, size_t bytes, Protection protection)
{
    size_t first = 0, last = 0;
    if (!Range(address, bytes, first, last)) return false;
    const uint8_t permission = protection == Protection::None ? NoAccess :
        protection == Protection::ReadOnly ? Read : Write;
    const size_t hostFirst = HostFirst(first), hostLast = HostLast(last);
    Lock lock;
    if (permission != Write && !RestrictWrites(hostFirst, hostLast)) return false;
    for (size_t page = first; page < last; ++page)
    {
        g_permission[page] = permission;
        // Explicitly opening a page RW bypasses the watch fault. Mark it dirty
        // conservatively until the caller rearms that range with ResetWriteWatch.
        if (permission == Write) g_state[page] = Dirty;
    }
    return Commit(hostFirst, hostLast);
}

bool EnableNullPageGuard()
{
    // 64 KiB is a whole number of host pages, so this guard is exact.
    if (!g_base || g_bytes < guest_memory::NullGuardBytes ||
        guest_memory::NullGuardBytes % g_hostPage ||
        !Protect(g_base, guest_memory::NullGuardBytes, Protection::ReadOnly)) return false;
    return true;
}

bool QueryReadRegion(const void* address, size_t maxBytes,
                     size_t& regionBytes, bool& readable)
{
    if (!g_base || !maxBytes) { errno = EINVAL; return false; }
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (start < base || start - base >= g_bytes) { errno = EINVAL; return false; }
    const size_t offset = start - base;
    const size_t available = (std::min)(maxBytes, g_bytes - offset);
    const size_t first = offset / PageSize;
    const size_t last = (offset + available - 1) / PageSize + 1;
    Lock lock;
    readable = g_permission[first] != NoAccess;
    size_t end = first + 1;
    while (end < last && (g_permission[end] != NoAccess) == readable) ++end;
    const uintptr_t limit = base + end * PageSize;
    regionBytes = (std::min)(available, static_cast<size_t>(limit - start));
    return true;
}

bool ReadMemory(const void* address, void* output, size_t bytes)
{
    if (!bytes) return true;
    size_t first = 0, last = 0;
    if (!Range(const_cast<void*>(address), bytes, first, last)) return false;
    // A NoAccess 4 KiB page can share a readable host page; answer from the
    // guest permissions. No lock: the diagnostic sampler calls this from a
    // signal handler, and a racing Protect is benign here.
    for (size_t page = first; page < last; ++page)
        if (__atomic_load_n(&g_permission[page], __ATOMIC_RELAXED) == NoAccess)
        {
            errno = EFAULT;
            return false;
        }
    if (host_posix::ReadOwnMemory(address, output, bytes)) return true;
    errno = EFAULT;
    return false;
}

bool CanWrite(const void* address, size_t bytes)
{
    if (!bytes) return true;
    size_t first = 0, last = 0;
    if (!Range(const_cast<void*>(address), bytes, first, last)) return false;
    Lock lock;
    for (size_t page = first; page < last; ++page)
        if (g_permission[page] != Write) return false;
    return true;
}

bool PrepareHostWrite(void* address, size_t bytes)
{
    if (!bytes) return true;
    size_t first = 0, last = 0;
    if (!Range(address, bytes, first, last)) return false;
    const size_t hostFirst = HostFirst(first), hostLast = HostLast(last);
    // The whole host pages open, so the observer sees all of their protected
    // sub-pages, as on a write fault.
    const size_t openFirst = hostFirst * g_sub, openLast = hostLast * g_sub;
    if (g_rollbackRegistered.load(std::memory_order_acquire))
        for (size_t page = openFirst; page < openLast; ++page)
        {
            if (!g_rollbackPages[page].load(std::memory_order_acquire)) continue;
            const uintptr_t a = uintptr_t(g_base + page * PageSize);
            g_rollbackWrite(a, g_captureRollback(a));
        }
    Lock lock;
    for (size_t page = first; page < last; ++page)
        if (g_permission[page] != Write) { errno = EACCES; return false; }
    for (size_t page = openFirst; page < openLast; ++page)
    {
        if (g_permission[page] != Write) continue;
        g_rollbackPages[page].store(0, std::memory_order_release);
        g_state[page] = Dirty;
    }
    return Commit(hostFirst, hostLast);
}

bool ResetWriteWatch(void* address, size_t bytes)
{
    size_t first = 0, last = 0;
    if (!Range(address, bytes, first, last)) return false;
    Lock lock;
    return ArmLocked(first, last);
}

bool TakeWriteWatch(void* address, size_t bytes, void** pages,
                    size_t capacity, size_t& count, bool reset)
{
    size_t first = 0, last = 0;
    if (!Range(address, bytes, first, last) || (!pages && capacity)) return false;
    Lock lock;
    // Most texture queries are clean: let libc search the state bytes in bulk.
    if (!std::memchr(g_state.get() + first, Dirty, last - first))
    {
        count = 0;
        return true;
    }
    size_t written = 0;
    for (size_t page = first; page < last; ++page)
        if (g_state[page] == Dirty) ++written;
    if (written > capacity) { errno = ENOBUFS; return false; }
    count = written;
    for (size_t page = first, out = 0; page < last && out < written; ++page)
        if (g_state[page] == Dirty)
            pages[out++] = g_base + page * PageSize;
    // A clean range is already armed. The lock excludes the fault handler, so
    // no page can open between this decision and its release.
    if (reset && written && !ArmLocked(first, last)) return false;
    return true;
}

bool InstallMirrorBacking(void* address, size_t bytes)
{
    size_t first = 0, last = 0;
    if (g_mirrorStage || !Range(address, bytes, first, last) ||
        (reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(g_base)) % g_hostPage ||
        bytes % g_hostPage)
    {
        errno = EINVAL;
        return false;
    }
    // macOS has no memfd: an unlinked POSIX shared memory object gives the
    // same two-mapping arrangement (names are at most 31 bytes).
    char name[32];
    std::snprintf(name, sizeof(name), "/ttt2-mirror-%d", int(getpid()));
    shm_unlink(name);
    const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    shm_unlink(name);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0)
    {
        const int failure = errno;
        close(fd);
        errno = failure;
        return false;
    }
    void* stage = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (stage == MAP_FAILED)
    {
        const int failure = errno;
        close(fd);
        errno = failure;
        return false;
    }
    // This address belongs to ReserveGuest. Only this cached-view subrange is
    // replaced; the second mapping remains writable for the mirror worker.
    void* guest = mmap(address, bytes, PROT_NONE, MAP_SHARED | MAP_FIXED, fd, 0);
    const int failure = errno;
    close(fd);
    if (guest == MAP_FAILED)
    {
        munmap(stage, bytes);
        errno = failure;
        return false;
    }
    {
        Lock lock;
        for (size_t page = first; page < last; ++page)
        {
            g_permission[page] = NoAccess;
            g_state[page] = Armed;
        }
        for (size_t host = HostFirst(first); host < HostLast(last); ++host)
            g_native[host] = int8_t(PROT_NONE);
    }
    g_mirrorStage = static_cast<uint8_t*>(stage);
    g_mirrorStart = reinterpret_cast<uintptr_t>(address);
    g_mirrorBytes = bytes;
    return true;
}

bool FillMirrorView(void* address, const void* source, size_t bytes)
{
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (!g_mirrorStage || start < g_mirrorStart || bytes == 0 ||
        (start - g_mirrorStart) % g_hostPage || bytes % g_hostPage ||
        start - g_mirrorStart >= g_mirrorBytes ||
        bytes > g_mirrorBytes - (start - g_mirrorStart))
    {
        errno = EINVAL;
        return false;
    }
    const size_t offset = start - g_mirrorStart;
    const size_t first = (start - reinterpret_cast<uintptr_t>(g_base)) / PageSize;
    const size_t last = first + bytes / PageSize;
    {
        Lock lock;
        for (size_t page = first; page < last; ++page)
            if (g_permission[page] != NoAccess) { errno = EBUSY; return false; }
    }
    // The guest view remains PROT_NONE for the entire copy. Publish fully
    // populated pages together as readable and write-watch armed.
    std::memcpy(g_mirrorStage + offset, source, bytes);
    Lock lock;
    for (size_t page = first; page < last; ++page)
        if (g_permission[page] != NoAccess) { errno = EBUSY; return false; }
    for (size_t page = first; page < last; ++page)
    {
        g_permission[page] = Write;
        g_state[page] = Armed;
    }
    return Commit(HostFirst(first), HostLast(last));
}

unsigned long LastError() { return static_cast<unsigned long>(errno); }

bool RegisterUnhandledFaultHandler(UnhandledFaultHandler handler)
{
    UnhandledFaultHandler expected = nullptr;
    if (!handler || !g_unhandledHandler.compare_exchange_strong(
            expected, handler, std::memory_order_release,
            std::memory_order_relaxed))
    {
        errno = EINVAL;
        return false;
    }
    return true;
}

namespace {
bool EnsureFaultWorker()
{
    if (g_faultWorkerStarted) return true;
    if (host_posix::PipeCloexec(g_faultPipe) != 0) return false;
    const int startError = pthread_create(&g_faultWorker, nullptr, FaultWorker, nullptr);
    if (startError)
    {
        close(g_faultPipe[0]);
        close(g_faultPipe[1]);
        g_faultPipe[0] = g_faultPipe[1] = -1;
        errno = startError;
        return false;
    }
    g_faultWorkerStarted = true;
    return true;
}
}

bool ProtectForRollback(void* address, size_t bytes, bool readOnly)
{
    size_t first = 0, last = 0;
    if (!Range(address, bytes, first, last)) return false;
    const size_t hostFirst = HostFirst(first), hostLast = HostLast(last);
    Lock lock;
    // Tracked image ranges include immutable code, and heaps can contain
    // inaccessible pages. Rollback must preserve those logical permissions.
    // Write access goes first, so a simultaneous writer can never bypass the
    // observer through a still-writable host page.
    if (readOnly && !RestrictWrites(hostFirst, hostLast)) return false;
    for (size_t page = first; page < last; ++page)
    {
        if (g_permission[page] != Write) continue;
        g_rollbackPages[page].store(readOnly ? 1 : 0, std::memory_order_release);
        if (!readOnly) g_state[page] = Dirty;
    }
    return Commit(hostFirst, hostLast);
}

bool RegisterRollbackWriteHandler(CaptureRollbackWriter capture, RollbackWriteHandler handler)
{
    if (!capture || !handler || g_rollbackWrite) { errno = EINVAL; return false; }
    if (!EnsureFaultWorker()) return false;
    g_captureRollback = capture;
    g_rollbackWrite = handler;
    g_rollbackRegistered.store(true, std::memory_order_release);
    return true;
}

bool RegisterDeferredFaultHandler(void* address, size_t bytes,
                                  DeferredFaultHandler handler)
{
    size_t first = 0, last = 0;
    if (!handler || !Range(address, bytes, first, last) || g_deferredHandler)
    {
        errno = EINVAL;
        return false;
    }
    if (!EnsureFaultWorker()) return false;
    g_deferredFirst = reinterpret_cast<uintptr_t>(g_base) + first * PageSize;
    g_deferredLast = reinterpret_cast<uintptr_t>(g_base) + last * PageSize;
    g_deferredHandler = handler;
    return true;
}

} // namespace host_vm
