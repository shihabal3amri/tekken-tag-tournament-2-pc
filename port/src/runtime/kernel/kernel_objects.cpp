// Kernel dispatcher objects with REAL blocking waits (events, mutants, semaphores, threads).
// Nt* functions use handles; Ke* functions operate on guest dispatcher headers (bound lazily to
// a host object). Multi-threaded now that ExCreateThread spawns real host threads.
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "kernel_objects.h"
#include "xlive.h"
#include "kernel_content.h"
#include "timeline.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#undef STATUS_TIMEOUT   // winnt.h macro collides with our constant below
#undef STATUS_INVALID_PARAMETER
#undef STATUS_INVALID_HANDLE
#undef MAXIMUM_WAIT_OBJECTS
#include <chrono>
#include <array>

namespace {
// One dispatcher lock makes a WaitAll check and its state consumption atomic
// with single waits and signals. No guest code runs while this lock is held.
std::mutex g_dispatchMutex;
uint64_t g_dispatchGeneration = 0;
uint64_t g_dispatchNotifications = 0;

struct MultiWaitSignal
{
    std::condition_variable cv;
    uint64_t generation = 0;
};

// Intrusive links live in the waiting call's stack registration. Both the links
// and their notification target are accessed only under g_dispatchMutex.
struct MultiWaitLink
{
    MultiWaitSignal* signal = nullptr;
    MultiWaitLink* next = nullptr;
    MultiWaitLink** previous = nullptr;
};
}

static constexpr uint32_t STATUS_TIMEOUT = 0x00000102u;
static constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000Du;
static constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008u;
static constexpr uint32_t STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024u;
static constexpr uint32_t MAXIMUM_WAIT_OBJECTS = 64;

struct KObject
{
    std::mutex& m = g_dispatchMutex;
    std::condition_variable cv;
    std::atomic<int> refs{1};
    MultiWaitLink* multiWaiters = nullptr;
    virtual ~KObject() = default;
    virtual uint32_t Wait(int64_t timeoutMs) { return STATUS_SUCCESS; }
    // Wait as if it had started when the object's pulse generation was `pulseBase` (see
    // NtSignalAndWaitForSingleObjectEx). Only events have pulses.
    virtual uint32_t WaitFrom(int64_t timeoutMs, uint64_t pulseBase) { return Wait(timeoutMs); }
    // These operations require m. A multi-wait only consumes after the entire
    // selected condition succeeds, unlike a sequence of individual Wait calls.
    virtual bool CanWaitMultiple() const { return false; }
    virtual bool IsSignaled(uint64_t pulse) const { return false; }
    virtual void ConsumeSignal() {}
    virtual uint64_t PulseGeneration() const { return 0; }
    // Type-correct signal for NtSignalAndWaitForSingleObjectEx: event -> Set, mutant ->
    // Release, semaphore -> Release(1). A wrong-typed signal (e.g. treating a mutant as
    // an event) silently never releases it and deadlocks every later acquirer.
    virtual void Signal() {}
    // Caller holds m. An unrelated object's transition cannot satisfy this
    // object's waiters, so do not wake every multi-wait in the process.
    void DispatcherChanged()
    {
        ++g_dispatchGeneration;
        for (auto* link = multiWaiters; link; link = link->next)
        {
            ++link->signal->generation;
            ++g_dispatchNotifications;
            link->signal->cv.notify_one();
        }
    }
};

namespace {

struct MultiWaitRegistration : MultiWaitSignal
{
    std::array<MultiWaitLink, MAXIMUM_WAIT_OBJECTS> links{};
    const uint32_t count;
    // Construction and destruction both occur while the wait's dispatcher lock
    // is held, including all success/timeout returns. No notifier retains a link.
    MultiWaitRegistration(KObject* const* objects, uint32_t n) : count(n)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            auto& link = links[i];
            link.signal = this;
            link.next = objects[i]->multiWaiters;
            link.previous = &objects[i]->multiWaiters;
            if (link.next) link.next->previous = &link.next;
            objects[i]->multiWaiters = &link;
        }
    }
    ~MultiWaitRegistration()
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            auto& link = links[i];
            *link.previous = link.next;
            if (link.next) link.next->previous = link.previous;
        }
    }
    MultiWaitRegistration(const MultiWaitRegistration&) = delete;
    MultiWaitRegistration& operator=(const MultiWaitRegistration&) = delete;
};

int64_t GuestTimeout(uint8_t* base, uint32_t timeoutPtr)
{
    if (!timeoutPtr) return -1;                       // NULL = infinite
    int64_t t = (int64_t)(((uint64_t)GRead32(base, timeoutPtr) << 32) | GRead32(base, timeoutPtr + 4));
    if (t == (int64_t)0x8000000000000000ull) return -1;
    // Xbox timeouts are 100 ns units. Negative = relative delay; positive = absolute
    // deadline on the KeQuerySystemTime clock (FILETIME epoch) - compute the delta.
    if (t < 0) return (-t) / 10000 + ((-t) % 10000 != 0);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    int64_t now = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    return t > now ? (t - now) / 10000 + ((t - now) % 10000 != 0) : 0;
}

struct KEvent : KObject
{
    bool manualReset; bool state;
    uint64_t gen = 0;   // pulse generation: wakes CURRENT waiters without leaving state set
    bool pulseAvailable = false;
    // Auto-reset hand-off (console and Windows semantics): setting an auto-reset event that
    // threads are already waiting on releases the oldest of them and leaves the event
    // unsignaled - the setter cannot take the signal back by waiting right after. TTT2's
    // monitor waits (sub_82AE4438: while the count is not reached, SignalAndWait(mutex,
    // event), then SetEvent(event) to pass the wake on) rely on it: with a free-for-all
    // signal the job worker kept re-taking its own SetEvent (a core spent spinning) while
    // the thread already queued on the event was starved. Single-object waiters only;
    // multi-object waits keep the signaled-state path.
    std::deque<uint64_t> queue;     // waiting tickets, oldest first
    std::vector<uint64_t> granted;  // tickets released by Set and not yet returned
    uint64_t nextTicket = 0;
    KEvent(bool manual, bool initial) : manualReset(manual), state(initial) {}
    uint32_t Wait(int64_t t) override
    {
        std::unique_lock lk(m);
        return WaitLocked(lk, t, gen);
    }
    uint32_t WaitFrom(int64_t t, uint64_t g0) override
    {
        std::unique_lock lk(m);
        return WaitLocked(lk, t, g0);
    }
    uint32_t WaitLocked(std::unique_lock<std::mutex>& lk, int64_t t, uint64_t g0)
    {
        if (manualReset || IsSignaled(g0))
        {
            auto pred = [&] { return IsSignaled(g0); };
            if (t < 0) cv.wait(lk, pred);
            else if (!cv.wait_for(lk, std::chrono::milliseconds(t), pred)) return STATUS_TIMEOUT;
            ConsumeSignal();
            return STATUS_SUCCESS;
        }
        const uint64_t ticket = nextTicket++;
        queue.push_back(ticket);
        auto isGranted = [&] { return std::find(granted.begin(), granted.end(), ticket) != granted.end(); };
        auto pred = [&] { return isGranted() || IsSignaled(g0); };
        bool ok = true;
        if (t < 0) cv.wait(lk, pred);
        else ok = cv.wait_for(lk, std::chrono::milliseconds(t), pred);
        if (auto it = std::find(granted.begin(), granted.end(), ticket); it != granted.end())
        {
            granted.erase(it);              // released by a Set: the signal was never left set
            return STATUS_SUCCESS;
        }
        queue.erase(std::find(queue.begin(), queue.end(), ticket));
        if (!ok) return STATUS_TIMEOUT;
        ConsumeSignal();                    // a pulse (or a Set while no ticket was queued)
        return STATUS_SUCCESS;
    }
    bool CanWaitMultiple() const override { return true; }
    bool IsSignaled(uint64_t pulse) const override
    {
        return state || (gen != pulse && (manualReset || pulseAvailable));
    }
    void ConsumeSignal() override
    {
        if (!manualReset) { if (state) state = false; else pulseAvailable = false; }
    }
    uint64_t PulseGeneration() const override { return gen; }
    void Set()
    {
        std::lock_guard lk(m);
        // Setting an already-signaled event adds no signal or wake entitlement.
        // In particular, it must not broadcast a fictitious state transition to
        // every unrelated WaitMultiple caller. New waiters observe state while
        // holding this same lock; Reset/ConsumeSignal allow the next real Set.
        if (state) return;
        static const bool handoff = [] { const char* e = std::getenv("TTT2_EVENT_HANDOFF"); return !e || std::strcmp(e, "0") != 0; }();
        if (handoff && !manualReset && !queue.empty())
        {
            granted.push_back(queue.front());   // hand the signal to the oldest waiter
            queue.pop_front();
            cv.notify_all();
            return;
        }
        state = true;
        cv.notify_all();
        DispatcherChanged();
    }
    void Reset() { std::lock_guard lk(m); state = false; }
    // NtPulseEvent: release threads waiting NOW, leave the event unsignaled. The naive
    // "set, notify, clear" form is a lost-wake machine: waiters re-check after the clear.
    void Pulse() { std::lock_guard lk(m); state = false; ++gen; pulseAvailable = true; cv.notify_all(); DispatcherChanged(); }
    void Signal() override { Set(); }
};

struct KSemaphore : KObject
{
    int count, maxCount;
    KSemaphore(int c, int mx) : count(c), maxCount(mx) {}
    uint32_t Wait(int64_t t) override
    {
        std::unique_lock lk(m);
        auto pred = [&] { return count > 0; };
        if (t < 0) cv.wait(lk, pred);
        else if (!cv.wait_for(lk, std::chrono::milliseconds(t), pred)) return STATUS_TIMEOUT;
        --count; return STATUS_SUCCESS;
    }
    bool CanWaitMultiple() const override { return true; }
    bool IsSignaled(uint64_t) const override { return count > 0; }
    void ConsumeSignal() override { --count; }
    void Release(int n) { std::lock_guard lk(m); count += n; cv.notify_all(); DispatcherChanged(); }
    void Signal() override { Release(1); }
};

struct KMutant : KObject
{
    std::thread::id owner{}; int recursion = 0; bool held = false;
    uint32_t Wait(int64_t t) override
    {
        std::unique_lock lk(m);
        auto me = std::this_thread::get_id();
        if (held && owner == me) { ++recursion; return STATUS_SUCCESS; }
        auto pred = [&] { return !held; };
        if (t < 0) cv.wait(lk, pred);
        else if (!cv.wait_for(lk, std::chrono::milliseconds(t), pred)) return STATUS_TIMEOUT;
        held = true; owner = me; recursion = 1; return STATUS_SUCCESS;
    }
    bool CanWaitMultiple() const override { return true; }
    bool IsSignaled(uint64_t) const override { return !held || owner == std::this_thread::get_id(); }
    void ConsumeSignal() override
    {
        if (held) ++recursion;
        else { held = true; owner = std::this_thread::get_id(); recursion = 1; }
    }
    void Release() { std::lock_guard lk(m); if (--recursion <= 0) { held = false; cv.notify_one(); DispatcherChanged(); } }
    void Signal() override { Release(); }
};

struct KThread;
thread_local KThread* t_currentKernelThread = nullptr;

struct KThread : KObject
{
    bool done = false;
    uint32_t suspendCount = 0;
    bool waitingForResume = false;
    std::mutex sm; std::condition_variable scv;
    std::mutex schedulingMutex;
    HANDLE hostThread = nullptr;
    uint32_t guestAffinity = 1; // Initial KPCR CPU is zero, independently of host CPU numbering.
    int32_t guestPriority = 0;
    volatile uint8_t* guestCpuByte = nullptr;
    ~KThread() override { if (hostThread) CloseHandle(hostThread); }
    uint32_t Wait(int64_t t) override
    {
        std::unique_lock lk(m);
        auto pred = [&] { return done; };
        if (t < 0) cv.wait(lk, pred);
        else if (!cv.wait_for(lk, std::chrono::milliseconds(t), pred)) return STATUS_TIMEOUT;
        return STATUS_SUCCESS;
    }
    bool CanWaitMultiple() const override { return true; }
    bool IsSignaled(uint64_t) const override { return done; }
    void Start(std::function<void()> fn, bool sus)
    {
        suspendCount = sus ? 1 : 0;
        waitingForResume = sus;
        // 8 MB host stack: recompiled guest call chains run deep (default 1 MB overflows
        // silently and kills the process with no fault output).
        struct Ctx { KThread* self; std::function<void()> fn; };
        auto* c = new Ctx{ this, std::move(fn) };
        uintptr_t h = _beginthreadex(nullptr, 8 * 1024 * 1024,
            [](void* p) -> unsigned {
                std::unique_ptr<Ctx> c(static_cast<Ctx*>(p));
                KThread* self = c->self;
                t_currentKernelThread = self;
                {
                    std::unique_lock lk(self->sm);
                    self->scv.wait(lk, [&] { return self->suspendCount == 0; });
                    self->waitingForResume = false;
                }
                c->fn();
                {
                    std::lock_guard lk(self->schedulingMutex);
                    self->guestCpuByte = nullptr;
                    CloseHandle(self->hostThread);
                    self->hostThread = nullptr;
                }
                t_currentKernelThread = nullptr;
                { std::lock_guard lk(self->m); self->done = true; self->DispatcherChanged(); }
                self->cv.notify_all();
                return 0;
            }, c, CREATE_SUSPENDED, nullptr);
        if (h)
        {
            // Publish the real handle before any callback can use its current-thread alias.
            hostThread = reinterpret_cast<HANDLE>(h);
            ResumeThread(hostThread);
        }
        else
        {
            delete c;
            std::lock_guard lk(m); done = true; DispatcherChanged(); cv.notify_all();
        }
    }
    uint32_t Resume(uint32_t* previous)
    {
        std::lock_guard lk(sm);
        if (previous) *previous = suspendCount;
        if (suspendCount) --suspendCount;
        if (!suspendCount) scv.notify_all();
        return STATUS_SUCCESS;
    }
};

// Handle table (Nt*) ??? handles are high values so they never collide with guest pointers.
std::mutex g_htMutex;
std::unordered_map<uint32_t, KObject*> g_handles;
uint32_t g_nextHandle = 0xF0000000;

// Guest dispatcher-header binding (Ke*) ??? guest address -> host object.
std::mutex g_gdMutex;
std::unordered_map<uint32_t, KObject*> g_guestObjects;

KEvent* QueryGuestEvent(uint8_t* base, uint32_t addr)
{
    std::lock_guard lk(g_gdMutex);
    auto it = g_guestObjects.find(addr);
    if (it != g_guestObjects.end()) return static_cast<KEvent*>(it->second);
    uint8_t type = GRead8(base, addr);               // dispatcher Type: 0=notification(manual),1=sync(auto)
    auto* e = new KEvent(type == 0, GRead32(base, addr + 4) != 0);
    g_guestObjects[addr] = e;
    return e;
}

KObject* QueryGuestWaitable(uint8_t* base, uint32_t addr, uint32_t& error)
{
    // ObReferenceObjectByHandle currently returns the host-object handle itself.
    // Resolve that representation before trying to read a guest header.
    if (addr >= 0xF0000000u)
    {
        auto* object = KernelGetObject(addr);
        if (!object) error = STATUS_INVALID_HANDLE;
        return object;
    }
    if (!addr || (addr & 3)) { error = STATUS_INVALID_PARAMETER; return nullptr; }
    const uint8_t type = GRead8(base, addr);
    if (type != 0 && type != 1)
    {
        error = STATUS_OBJECT_TYPE_MISMATCH;
        return nullptr;
    }
    return QueryGuestEvent(base, addr);
}

uint32_t OperateGuestEvent(uint8_t* base, uint32_t addr, void (KEvent::*operation)())
{
    uint32_t error = STATUS_SUCCESS;
    auto* object = QueryGuestWaitable(base, addr, error);
    if (!object) return error;
    auto* event = dynamic_cast<KEvent*>(object);
    if (!event) return STATUS_OBJECT_TYPE_MISMATCH;
    (event->*operation)();
    return STATUS_SUCCESS;
}

uint32_t WaitMultiple(KObject* const* objects, uint32_t count, bool waitAny, int64_t timeoutMs)
{
    if (!count || count > MAXIMUM_WAIT_OBJECTS) return STATUS_INVALID_PARAMETER;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!objects[i] || !objects[i]->CanWaitMultiple()) return STATUS_OBJECT_TYPE_MISMATCH;
        for (uint32_t j = 0; j < i; ++j)
            if (objects[i] == objects[j]) return STATUS_INVALID_PARAMETER;
    }
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now).count();
    const auto deadline = timeoutMs < 0 || timeoutMs >= remaining ? Clock::time_point::max()
        : now + std::chrono::milliseconds(timeoutMs);
    std::unique_lock lk(g_dispatchMutex);
    MultiWaitRegistration registration(objects, count);
    std::array<uint64_t, MAXIMUM_WAIT_OBJECTS> pulses{};
    for (uint32_t i = 0; i < count; ++i) pulses[i] = objects[i]->PulseGeneration();
    for (;;)
    {
        bool all = true;
        for (uint32_t i = 0; i < count; ++i)
        {
            const bool ready = objects[i]->IsSignaled(pulses[i]);
            if (waitAny && ready)
            {
                objects[i]->ConsumeSignal();
                return i; // STATUS_WAIT_0 + the ORIGINAL array index
            }
            all &= ready;
        }
        if (!waitAny && all)
        {
            for (uint32_t i = 0; i < count; ++i) objects[i]->ConsumeSignal();
            return STATUS_SUCCESS;
        }
        // An unsuccessful WaitAll must not retain a pulse for a later signal.
        for (uint32_t i = 0; i < count; ++i) pulses[i] = objects[i]->PulseGeneration();
        if (timeoutMs >= 0 && Clock::now() >= deadline) return STATUS_TIMEOUT;
        const uint64_t generation = registration.generation;
        auto changed = [&] { return generation != registration.generation; };
        if (timeoutMs < 0) registration.cv.wait(lk, changed);
        else if (!registration.cv.wait_until(lk, deadline, changed)) return STATUS_TIMEOUT;
    }
}
} // namespace

uint32_t KernelObjectHandle(KObject* o) { std::lock_guard lk(g_htMutex); uint32_t h = g_nextHandle++; g_handles[h] = o; return h; }
KObject* KernelGetObject(uint32_t h)    { std::lock_guard lk(g_htMutex); auto it = g_handles.find(h); return it == g_handles.end() ? nullptr : it->second; }
KObject* KernelCreateThreadObject()     { return new KThread(); }
void     KernelStartHostThread(KObject* o, std::function<void()> fn, bool suspended) { static_cast<KThread*>(o)->Start(std::move(fn), suspended); }

namespace {
KThread* CurrentKernelThread()
{
    // The initial game entry and callback threads do not pass through ExCreateThread.
    // Duplicate the pseudo handle so their records can safely retain a real handle.
    static thread_local std::unique_ptr<KThread> attached;
    if (!t_currentKernelThread)
    {
        attached = std::make_unique<KThread>();
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                             &attached->hostThread, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return nullptr;
        t_currentKernelThread = attached.get();
    }
    return t_currentKernelThread;
}

KThread* SchedulingThread(uint32_t handle)
{
    if (handle == 0xFFFFFFFEu) return CurrentKernelThread();
    return dynamic_cast<KThread*>(KernelGetObject(handle));
}

// Xbox threads select one of six logical processors. Host numbering may be sparse
// or constrained by the process mask; use its nth available CPU, wrapping only on
// hosts with fewer than six available processors. Never widen process affinity.
DWORD_PTR MapGuestCpu(uint32_t mask, DWORD_PTR allowed)
{
    if (!mask || (mask & (mask - 1)) || (mask & ~0x3Fu) || !allowed) return 0;
    unsigned guestCpu = 0;
    while ((mask >> guestCpu) != 1) ++guestCpu;
    unsigned count = 0;
    for (DWORD_PTR bits = allowed; bits; bits &= bits - 1) ++count;
    unsigned index = guestCpu % count;
    for (unsigned bit = 0; bit < sizeof(DWORD_PTR) * 8; ++bit)
    {
        const DWORD_PTR one = DWORD_PTR(1) << bit;
        if ((allowed & one) && index-- == 0) return one;
    }
    return 0;
}
}

void KernelBindCurrentThreadCpu(volatile uint8_t* cpuByte)
{
    auto* thread = CurrentKernelThread();
    if (!thread || !cpuByte) return;
    std::lock_guard lk(thread->schedulingMutex);
    if (thread->guestCpuByte == cpuByte) return;
    thread->guestCpuByte = cpuByte;
    uint32_t cpu = 0, mask = thread->guestAffinity;
    while ((mask >> cpu) != 1) ++cpu;
    *cpuByte = static_cast<uint8_t>(cpu);
}

uint32_t KernelSetThreadAffinity(uint32_t handle, uint32_t mask, uint32_t& previous)
{
    // Xbox ABI and single-CPU contract: Xenia xboxkrnl_threading.cc / xthread.cc.
    if (!mask || (mask & (mask - 1)) || (mask & ~0x3Fu)) return STATUS_INVALID_PARAMETER;
    auto* thread = SchedulingThread(handle);
    if (!thread) return STATUS_INVALID_HANDLE;
    // The guest sees its CPU choice (previous mask, KPCR CPU byte), but host threads are not
    // pinned - Xenia's ignore_thread_affinities default. MEASURED 2026-10-12: TTT2 pins 32
    // threads onto host logical CPUs 1-5 (three physical cores on an 8-core SMT CPU); with the
    // window in the background (DWM composing it) the circus stage fell from 58 to 37-39 fps,
    // and unpinning them restored a flat 60. TTT2_THREAD_AFFINITY=1 pins as before (A/B).
    static const bool pinHost = [] { const char* e = std::getenv("TTT2_THREAD_AFFINITY"); return e && *e == '1'; }();
    DWORD_PTR hostMask = 0;
    if (pinHost)
    {
        DWORD_PTR allowed = 0, system = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) return STATUS_UNSUCCESSFUL;
        hostMask = MapGuestCpu(mask, allowed);
        if (!hostMask) return STATUS_UNSUCCESSFUL;
    }
    std::lock_guard lk(thread->schedulingMutex);
    if (!thread->hostThread || (pinHost && !SetThreadAffinityMask(thread->hostThread, hostMask)))
        return STATUS_UNSUCCESSFUL;
    previous = thread->guestAffinity;
    thread->guestAffinity = mask;
    if (thread->guestCpuByte)
    {
        unsigned cpu = 0;
        while ((mask >> cpu) != 1) ++cpu;
        *thread->guestCpuByte = static_cast<uint8_t>(cpu);
    }
    return STATUS_SUCCESS;
}

int32_t KernelSetThreadPriority(uint32_t handle, int32_t priority)
{
    auto* thread = SchedulingThread(handle);
    if (!thread) return 0;
    std::lock_guard lk(thread->schedulingMutex);
    const int32_t previous = thread->guestPriority;
    // Preserve the guest request for query/restore, but limit its host effect to
    // normal process-relative priorities. Never request time-critical/realtime.
    const int hostPriority = priority < THREAD_PRIORITY_LOWEST ? THREAD_PRIORITY_LOWEST :
        priority > THREAD_PRIORITY_HIGHEST ? THREAD_PRIORITY_HIGHEST : priority;
    if (thread->hostThread && SetThreadPriority(thread->hostThread, hostPriority))
        thread->guestPriority = priority;
    return previous;
}

int32_t KernelQueryThreadPriority(uint32_t handle)
{
    auto* thread = SchedulingThread(handle);
    if (!thread) return 0;
    std::lock_guard lk(thread->schedulingMutex);
    return thread->guestPriority;
}

uint32_t KernelResumeThread(uint32_t handle, uint32_t* previous)
{
    auto* thread = SchedulingThread(handle);
    if (!thread) return STATUS_INVALID_HANDLE;
    return thread->Resume(previous);
}

uint32_t KernelSuspendThread(uint32_t handle, uint32_t* previous)
{
    auto* thread = SchedulingThread(handle);
    if (!thread) return STATUS_INVALID_HANDLE;
    const bool self = thread == CurrentKernelThread();
    std::unique_lock lk(thread->sm);
    // Suspending a running foreign host thread could freeze it while it owns
    // dispatcher/CRT locks. Only self-suspension and an existing cooperative
    // startup/self wait are supported until a guest scheduling boundary exists.
    if (!self && !thread->waitingForResume) return 0xC0000002u; // STATUS_NOT_IMPLEMENTED
    if (thread->suspendCount == 127) return 0xC000004Au; // STATUS_SUSPEND_COUNT_EXCEEDED
    if (previous) *previous = thread->suspendCount;
    ++thread->suspendCount;
    if (self)
    {
        thread->waitingForResume = true;
        thread->scv.wait(lk, [&] { return thread->suspendCount == 0; });
        thread->waitingForResume = false;
    }
    return STATUS_SUCCESS;
}

// ================= Nt* (handle-based) =================
PPC_FUNC(__imp__NtCreateEvent)
{
    KTrace("NtCreateEvent");
    auto* e = new KEvent(ctx.r5.u32 == 0, ctx.r6.u32 != 0);
    GWrite32(base, ctx.r3.u32, KernelObjectHandle(e));
    ctx.r3.u32 = STATUS_SUCCESS;
}
PPC_FUNC(__imp__NtCreateMutant)
{
    KTrace("NtCreateMutant");
    GWrite32(base, ctx.r3.u32, KernelObjectHandle(new KMutant()));
    ctx.r3.u32 = STATUS_SUCCESS;
}
PPC_FUNC(__imp__NtCreateSemaphore)
{
    KTrace("NtCreateSemaphore");
    GWrite32(base, ctx.r3.u32, KernelObjectHandle(new KSemaphore((int)ctx.r5.u32, (int)ctx.r6.u32)));
    ctx.r3.u32 = STATUS_SUCCESS;
}
PPC_FUNC(__imp__NtClose)
{
    KTrace("NtClose");
    uint32_t contentStatus;
    if (TryCloseContentEnumerator(ctx.r3.u32, contentStatus)) { ctx.r3.u32 = contentStatus; return; }
    if (XLiveCloseHandle(ctx.r3.u32, contentStatus)) { ctx.r3.u32 = contentStatus; return; }   // Live enumerator
    if (TryCloseGuestFile(ctx.r3.u32)) { ctx.r3.u32 = STATUS_SUCCESS; return; }   // file handle
    if (ctx.r3.u32 >= 0xF0000000) { std::lock_guard lk(g_htMutex); g_handles.erase(ctx.r3.u32); }
    ctx.r3.u32 = STATUS_SUCCESS;
}
void KernelSignalEventHandle(uint32_t handle)
{
    if (auto* event = dynamic_cast<KEvent*>(KernelGetObject(handle))) event->Set();
}
bool KernelIsEventHandle(uint32_t handle) { return dynamic_cast<KEvent*>(KernelGetObject(handle)) != nullptr; }
// Is a thread waiting on event `handle` right now (queued single-object waiter, or a multi-wait
// that includes it)? Not an event: true (callers then keep the game's own behaviour).
bool KernelEventHasWaiters(uint32_t handle)
{
    auto* event = dynamic_cast<KEvent*>(KernelGetObject(handle));
    if (!event) return true;
    std::lock_guard lk(event->m);
    return !event->queue.empty() || event->multiWaiters != nullptr;
}

PPC_FUNC(__imp__NtSetEvent)
{
    KTrace("NtSetEvent");
    if (timeline::IsPoolEvent(base, ctx.r3.u32)) timeline::Mark(timeline::PoolSet, '.');
    if (auto* o = KernelGetObject(ctx.r3.u32)) static_cast<KEvent*>(o)->Set();
    ctx.r3.u32 = STATUS_SUCCESS;
}
PPC_FUNC(__imp__NtClearEvent)       { KTrace("NtClearEvent"); if (auto* o = KernelGetObject(ctx.r3.u32)) static_cast<KEvent*>(o)->Reset(); ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__NtPulseEvent)       { KTrace("NtPulseEvent"); if (auto* o = KernelGetObject(ctx.r3.u32)) static_cast<KEvent*>(o)->Pulse(); ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__NtReleaseMutant)    { KTrace("NtReleaseMutant");    if (auto* o = KernelGetObject(ctx.r3.u32)) static_cast<KMutant*>(o)->Release(); ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__NtReleaseSemaphore) { KTrace("NtReleaseSemaphore"); if (WTrace()) std::fprintf(stderr, "[w] NtReleaseSem h=%08X lr=%08X\n", ctx.r3.u32, (uint32_t)ctx.lr); if (auto* o = KernelGetObject(ctx.r3.u32)) static_cast<KSemaphore*>(o)->Release(ctx.r4.u32 ? ctx.r4.u32 : 1); ctx.r3.u32 = STATUS_SUCCESS; }

// Append the guest PPC backchain to `line` ([r1] = caller frame, saved LR at [frame-8]).
static int AppendGuestBacktrace(char* line, int n, size_t cap, PPCContext& ctx, uint8_t* base)
{
    uint32_t frame = ctx.r1.u32;
    for (int i = 0; i < 12 && frame && frame < 0xFFFFFFF0u; ++i)
    {
        uint32_t prev = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + frame));
        if (!prev || prev <= frame || prev - frame > 0x10000) break;
        uint32_t savedLr = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + prev - 8));
        n += std::snprintf(line + n, cap - n, " %08X", savedLr);
        frame = prev;
    }
    return n;
}

// Diagnose stuck boots: an infinite wait blocking >15s prints its guest backtrace once
// (walk the PPC backchain: [r1] = caller frame, saved LR at [callerFrame - 8]).
static uint32_t WaitDiag(KObject* o, int64_t ms, PPCContext& ctx, uint8_t* base,
                         const char* api, uint32_t which, const uint64_t* pulseBase = nullptr)
{
    auto wait = [&](int64_t t) { return pulseBase ? o->WaitFrom(t, *pulseBase) : o->Wait(t); };
    if (ms >= 0)
    {
        uint32_t r = wait(ms);
        // A timed wait that keeps timing out on the same object is a poll loop that
        // never gets satisfied - report it once per streak with a guest backtrace.
        if (r == STATUS_TIMEOUT)
        {
            static thread_local uint32_t pollObj = 0, pollCnt = 0;
            if (which == pollObj)
            {
                if (++pollCnt == 500)
                {
                    char line[512];
                    int n = std::snprintf(line, sizeof(line),
                        "[pollwait] %s obj=%08X lr=%08X x500 bt:", api, which, (uint32_t)ctx.lr);
                    n = AppendGuestBacktrace(line, n, sizeof(line), ctx, base);
                    std::snprintf(line + n, sizeof(line) - n, "\n");
                    std::fputs(line, stderr);
                    pollCnt = 0;
                }
            }
            else { pollObj = which; pollCnt = 1; }
        }
        return r;
    }
    uint32_t r = wait(15000);
    if (r != STATUS_TIMEOUT) return r;
    char line[512];
    int n = std::snprintf(line, sizeof(line), "[longwait] %s obj=%08X lr=%08X bt:",
                          api, which, (uint32_t)ctx.lr);
    n = AppendGuestBacktrace(line, n, sizeof(line), ctx, base);
    // For D3D fence waits (event at fence+60), dump the fence block and its counter chain.
    if (which < 0x40000000)
    {
        uint32_t fence = which - 60;
        uint32_t obj = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + fence));
        n += std::snprintf(line + n, sizeof(line) - n, " | fence@%08X:", fence);
        for (int i = 0; i < 8; ++i)
            n += std::snprintf(line + n, sizeof(line) - n, " %08X",
                __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + fence + i * 4)));
        if (obj && obj < 0xFFFFFE00u)
            n += std::snprintf(line + n, sizeof(line) - n, " ctr[obj+384]=%08X",
                __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + obj + 384)));
    }
    std::snprintf(line + n, sizeof(line) - n, "\n");
    std::fputs(line, stderr);
    return wait(-1);
}

PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    KTrace("NtWaitForSingleObjectEx");
    if (WTrace()) std::fprintf(stderr, "[w] NtWait h=%08X lr=%08X\n", ctx.r3.u32, (uint32_t)ctx.lr);
    auto* o = KernelGetObject(ctx.r3.u32);
    const uint32_t handle = ctx.r3.u32;
    ctx.r3.u32 = o ? WaitDiag(o, GuestTimeout(base, ctx.r6.u32), ctx, base, "NtWait", ctx.r3.u32) : STATUS_SUCCESS;
    if (timeline::IsPoolEvent(base, handle)) timeline::Mark(timeline::PoolWake, '.', ctx.r3.u32);
}
// (SignalHandle, WaitHandle, WaitMode, Alertable, Timeout*) - the engine's condvar wait:
// releases a MUTANT and waits an auto-reset event. Signal must be type-correct.
// The console performs the signal and the start of the wait atomically. The port releases
// the mutant first, so a waker that takes it in between and PULSES the event would find no
// waiter and the pulse would be lost: TTT2's job groups complete with NtPulseEvent
// (sub_823D8DB8) while their owner waits here (sub_823D8CD0), and that lost wake hung the
// boot at "Checking downloadable content..." about one run in eight. The wait therefore
// counts pulses from the generation read BEFORE the signal.
PPC_FUNC(__imp__NtSignalAndWaitForSingleObjectEx)
{
    KTrace("NtSignalAndWaitForSingleObjectEx");
    auto* w = KernelGetObject(ctx.r4.u32);
    uint64_t pulseBase = 0;
    if (w) { std::lock_guard lk(g_dispatchMutex); pulseBase = w->PulseGeneration(); }
    if (auto* o = KernelGetObject(ctx.r3.u32)) o->Signal();
    ctx.r3.u32 = w ? WaitDiag(w, GuestTimeout(base, ctx.r7.u32), ctx, base, "SigWait", ctx.r4.u32, &pulseBase)
                   : STATUS_SUCCESS;
}

// ================= Ke* (guest dispatcher header) =================
PPC_FUNC(__imp__KeSetEvent)   { KTrace("KeSetEvent");   ctx.r3.u32 = OperateGuestEvent(base, ctx.r3.u32, &KEvent::Set); }
PPC_FUNC(__imp__KeResetEvent) { KTrace("KeResetEvent"); ctx.r3.u32 = OperateGuestEvent(base, ctx.r3.u32, &KEvent::Reset); }
PPC_FUNC(__imp__KePulseEvent) { KTrace("KePulseEvent"); ctx.r3.u32 = OperateGuestEvent(base, ctx.r3.u32, &KEvent::Pulse); }
PPC_FUNC(__imp__KeWaitForSingleObject)
{
    KTrace("KeWaitForSingleObject");
    // Object, WaitReason, WaitMode, Alertable, Timeout*: r6 is NOT Timeout.
    int64_t t = GuestTimeout(base, ctx.r7.u32);
    uint32_t error = STATUS_SUCCESS;
    auto* object = QueryGuestWaitable(base, ctx.r3.u32, error);
    ctx.r3.u32 = object ? WaitDiag(object, t, ctx, base, "KeWait", ctx.r3.u32) : error;
}
PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    KTrace("KeWaitForMultipleObjects");
    // Count, Objects*, WaitType (0=All, 1=Any), Reason, Mode, Alertable,
    // Timeout*, WaitBlockArray*: the timeout is r9, and r10 is guest scratch.
    const uint32_t count = ctx.r3.u32;
    const uint32_t array = ctx.r4.u32;
    if (!count || count > MAXIMUM_WAIT_OBJECTS || ctx.r5.u32 > 1 || !array ||
        (array & 3) || uint64_t(array) + count * 4ull > 0x100000000ull)
    {
        ctx.r3.u32 = STATUS_INVALID_PARAMETER;
        return;
    }
    std::array<KObject*, MAXIMUM_WAIT_OBJECTS> objects{};
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t error = STATUS_SUCCESS;
        objects[i] = QueryGuestWaitable(base, GRead32(base, array + i * 4), error);
        if (!objects[i]) { ctx.r3.u32 = error; return; }
    }
    ctx.r3.u32 = WaitMultiple(objects.data(), count, ctx.r5.u32 == 1,
                             GuestTimeout(base, ctx.r9.u32));
}

// object references
PPC_FUNC(__imp__ObReferenceObjectByHandle) { KTrace("ObReferenceObjectByHandle"); if (ctx.r5.u32) GWrite32(base, ctx.r5.u32, ctx.r3.u32); ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__ObReferenceObject)         { KTrace("ObReferenceObject");   ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__ObDereferenceObject)       { KTrace("ObDereferenceObject"); ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__NtDuplicateObject)         { KTrace("NtDuplicateObject");   ctx.r3.u32 = STATUS_SUCCESS; }
