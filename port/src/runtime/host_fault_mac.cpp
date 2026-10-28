#include "host_fault.h"
#include "host_vm.h"

// Guest fault boundary for macOS. host_vm_mac.cpp owns SIGSEGV and SIGBUS
// (protection faults arrive as SIGBUS on macOS) and calls Unhandled for the
// faults it declines; this file adds SIGILL and SIGFPE.
#if !defined(__APPLE__)
#error "The macOS guest fault backend requires macOS"
#endif
#include "host_posix_compat.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <csetjmp>
#include <iterator>
#include <memory>
#include <new>
#include <sys/ucontext.h>
#include <unistd.h>

namespace host_fault {
namespace {
struct Frame
{
    sigjmp_buf jump;
    Frame* previous;
};

thread_local Frame* t_frame = nullptr;
struct FaultDetails
{
    volatile sig_atomic_t signal;
    volatile uintptr_t address;
    volatile uintptr_t pc;
    volatile sig_atomic_t write;
};
thread_local FaultDetails t_fault{};
// SIGBUS stays with host_vm: replacing its handler would hide write-watch faults.
constexpr int OtherFaults[] = {SIGILL, SIGFPE};
struct sigaction g_previous[2]{};
constexpr size_t AlternateStackBytes = 64 * 1024;
// Darwin has no SS_AUTODISARM (sigaltstack rejects unknown flags). None is
// needed: Darwin's siglongjmp clears the "on signal stack" state itself
// (checked on macOS 26 by mac/tests/host_vm_test.cpp).
constexpr int AutoDisarm = 0;

struct AlternateStack
{
    stack_t previous{};
    bool installed = false;
    int error = 0;

    AlternateStack()
    {
        static thread_local std::unique_ptr<char[]> storage;
        if (!storage) storage.reset(new (std::nothrow) char[AlternateStackBytes]);
        if (!storage) { error = ENOMEM; return; }
        stack_t current{};
        current.ss_sp = storage.get();
        current.ss_size = AlternateStackBytes;
        current.ss_flags = AutoDisarm;
        installed = sigaltstack(&current, &previous) == 0;
        if (!installed) error = errno;
    }

    ~AlternateStack()
    {
        // The buffer is thread-local and may be freed at thread exit. Never
        // leave the kernel pointing at it after this guarded call ends.
        // Darwin checks ss_size before SS_DISABLE: the "no stack" it reported
        // (size 0) is refused with ENOMEM unless given a valid size.
        if (installed && (previous.ss_flags & SS_DISABLE) && previous.ss_size < MINSIGSTKSZ)
            previous.ss_size = MINSIGSTKSZ;
        if (installed && sigaltstack(&previous, nullptr) != 0) std::abort();
    }
};

void Unhandled(int number, void* rawInfo, void* rawContext)
{
    Frame* frame = t_frame;
    if (!frame) return;
    auto* info = static_cast<siginfo_t*>(rawInfo);
    auto* context = static_cast<ucontext_t*>(rawContext);
    t_fault.signal = number;
    t_fault.address = info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;
    t_fault.pc = context ? host_posix::ContextPc(context) : 0;
    t_fault.write = context && (number == SIGSEGV || number == SIGBUS) &&
                    host_posix::ContextIsWrite(context);
    siglongjmp(frame->jump, 1);
}

void OtherFault(int number, siginfo_t* info, void* context)
{
    host_vm::CancelNullPageStep(context);
    if (t_frame) Unhandled(number, info, context);
    for (size_t i = 0; i < std::size(OtherFaults); ++i)
    {
        if (OtherFaults[i] != number) continue;
        const auto& previous = g_previous[i];
        if (previous.sa_handler == SIG_IGN) return;
        if (previous.sa_handler != SIG_DFL)
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
        if (!host_posix::RaiseOnThisThread(number))
            _exit(128 + number);
        return; // The default disposition runs when this handler returns.
    }
}

bool RegisterOtherFaults()
{
    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = OtherFault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (size_t i = 0; i < std::size(OtherFaults); ++i)
        if (sigaction(OtherFaults[i], &action, &g_previous[i]) != 0)
        {
            while (i) { --i; sigaction(OtherFaults[i], &g_previous[i], nullptr); }
            return false;
        }
    return true;
}
}

bool Run(Procedure procedure, void* argument)
{
    // The guest VM installs the SIGSEGV handler before the first guest call.
    // Function-local initialization serializes the first calls from guest threads.
    static const int registrationError = [] {
        if (!host_vm::RegisterUnhandledFaultHandler(Unhandled) ||
            !RegisterOtherFaults()) return errno ? errno : EINVAL;
        return 0;
    }();
    if (registrationError)
    {
        static const bool reported = [] {
            std::fprintf(stderr, "[fault] cannot install guest fault handlers (%d)\n",
                         registrationError);
            return true;
        }();
        (void)reported;
        return false;
    }
    AlternateStack alternateStack;
    if (!alternateStack.installed)
    {
        static thread_local bool warned = false;
        if (!warned)
        {
            std::fprintf(stderr, "[fault] alternate signal stack unavailable (%d)\n",
                         alternateStack.error);
            warned = true;
        }
        return false;
    }
    Frame frame{};
    frame.previous = t_frame;
    if (sigsetjmp(frame.jump, 1) == 0)
    {
        t_frame = &frame;
        try { procedure(argument); }
        catch (...)
        {
            t_frame = frame.previous;
            throw;
        }
        t_frame = frame.previous;
        return true;
    }
    t_frame = frame.previous;
    const int signal = t_fault.signal;
    if (signal == SIGSEGV || signal == SIGBUS)
        std::fprintf(stderr, "[fault] signal %d host=0x%llX pc=0x%llX %s\n",
                     signal, (unsigned long long)t_fault.address,
                     (unsigned long long)t_fault.pc, t_fault.write ? "write" : "read");
    else
        std::fprintf(stderr, "[fault] signal %d pc=0x%llX\n",
                     signal, (unsigned long long)t_fault.pc);
    return false;
}
}
