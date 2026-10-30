"""Exercise production dispatcher waits with synthetic guest events (no game data).

Compiles kernel_objects.cpp directly. Tests WaitAny indices, atomic WaitAll,
event reset modes, semaphore/mutant/thread readiness, blocking/wake races,
finite deadlines, rejected arguments, and PPC timeout argument registers.
"""

from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one


ROOT = Path(__file__).resolve().parents[2]
RECOMP = ROOT / "tools/XenonRecomp"

SHARED = r'''
#pragma once
#define PPC_CONFIG_H_INCLUDED
#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x11C0000ull
#define PPC_CODE_BASE 0x82250000ull
#define PPC_CODE_SIZE 0xB6848Cull
#include <ppc_context.h>
'''

CHECKER = r'''
#include "@OBJECTS@"
#include <cassert>
#include <future>
#include <vector>
bool WTrace() { return false; }
bool TryCloseGuestFile(uint32_t) { return false; }
bool TryCloseContentEnumerator(uint32_t, uint32_t&) { return false; }
static void Reporter(PPCContext&, uint8_t*) { std::abort(); }
PPCFunc* const g_unmappedReporter = Reporter;
using namespace std::chrono_literals;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)

struct ObservedEvent : KEvent {
    mutable std::atomic<unsigned> scans{0};
    ObservedEvent(bool manual) : KEvent(manual,false) {}
    bool IsSignaled(uint64_t pulse) const override {
        ++scans;
        return KEvent::IsSignaled(pulse);
    }
};
static void AwaitScans(ObservedEvent& event, unsigned count) {
    const auto limit=std::chrono::steady_clock::now()+1s;
    while(event.scans<count && std::chrono::steady_clock::now()<limit) std::this_thread::yield();
    CHECK(event.scans>=count);
}

static void WriteTimeout(uint8_t* base, uint32_t ptr, int64_t ticks) {
    GWrite32(base, ptr, uint32_t(uint64_t(ticks) >> 32));
    GWrite32(base, ptr + 4, uint32_t(ticks));
}

int main() {
    KEvent stop(true,false), ready(false,true);
    KObject* pair[]={&stop,&ready};
    CHECK(WaitMultiple(pair,2,true,0)==1); // Audio: [shutdown, mix-ready].
    CHECK(WaitMultiple(pair,2,true,0)==STATUS_TIMEOUT);
    stop.Set(); ready.Set();
    CHECK(WaitMultiple(pair,2,true,0)==0); // Lowest original index wins.
    CHECK(WaitMultiple(pair,2,true,0)==0); // Manual event stays set.
    stop.Reset();
    CHECK(WaitMultiple(pair,2,true,0)==1); // Other event wasn't consumed.

    KEvent first(false,true), second(false,false);
    KObject* all[]={&first,&second};
    CHECK(WaitMultiple(all,2,false,0)==STATUS_TIMEOUT);
    CHECK(first.state); // Failed WaitAll cannot consume the first event.
    second.Set();
    CHECK(WaitMultiple(all,2,false,0)==STATUS_SUCCESS);
    CHECK(!first.state && !second.state);

    KSemaphore semaphore(2,4);
    KMutant mutant;
    KObject* mixed[]={&semaphore,&mutant,&second};
    CHECK(WaitMultiple(mixed,3,false,0)==STATUS_TIMEOUT);
    CHECK(semaphore.count==2 && !mutant.held);
    second.Set();
    CHECK(WaitMultiple(mixed,3,false,0)==STATUS_SUCCESS);
    CHECK(semaphore.count==1 && mutant.held && mutant.recursion==1);
    KObject* owned[]={&mutant};
    CHECK(WaitMultiple(owned,1,false,0)==STATUS_SUCCESS);
    CHECK(mutant.recursion==2);
    auto otherOwner=std::async(std::launch::async,[&]{return WaitMultiple(owned,1,true,0);});
    CHECK(otherOwner.get()==STATUS_TIMEOUT);
    mutant.Release(); mutant.Release();
    CHECK(!mutant.held);

    // Blocking callback must sleep until mix-ready, then report slot 1.
    auto blocked=std::async(std::launch::async,[&]{return WaitMultiple(pair,2,true,1000);});
    CHECK(blocked.wait_for(10ms)==std::future_status::timeout);
    ready.Set();
    CHECK(blocked.get()==1);

    // A single signal can be consumed by only one waiter.
    auto a=std::async(std::launch::async,[&]{return WaitMultiple(pair,2,true,100);});
    auto b=std::async(std::launch::async,[&]{return WaitMultiple(pair,2,true,100);});
    ready.Set();
    const auto ar=a.get(),br=b.get();
    CHECK((ar==1 && br==STATUS_TIMEOUT) || (br==1 && ar==STATUS_TIMEOUT));

    // Exercise the signal-before-sleep race repeatedly, with a bounded failure.
    for(unsigned i=0;i<100;++i) {
        auto wake=std::async(std::launch::async,[&]{return WaitMultiple(pair,2,true,1000);});
        ready.Set();
        CHECK(wake.get()==1);
    }

    // Unrelated signals wake the global CV but must not restart this deadline.
    KEvent unrelated(false,false);
    std::atomic<bool> keepSignaling=true;
    std::thread noise([&]{while(keepSignaling){unrelated.Set();std::this_thread::sleep_for(1ms);}});
    const auto before=std::chrono::steady_clock::now();
    CHECK(WaitMultiple(pair,2,true,30)==STATUS_TIMEOUT);
    const auto elapsed=std::chrono::steady_clock::now()-before;
    keepSignaling=false;noise.join();
    CHECK(elapsed>=25ms && elapsed<1s);

    // Pulses release current waiters without allowing a later waiter through.
    ObservedEvent pulsed(false);
    KObject* pulsedObjects[]={&pulsed};
    auto pa=std::async(std::launch::async,[&]{return WaitMultiple(pulsedObjects,1,true,100);});
    auto pb=std::async(std::launch::async,[&]{return WaitMultiple(pulsedObjects,1,true,100);});
    AwaitScans(pulsed,2);pulsed.Pulse();
    const auto par=pa.get(),pbr=pb.get();
    CHECK((par==0 && pbr==STATUS_TIMEOUT) || (pbr==0 && par==STATUS_TIMEOUT));
    CHECK(WaitMultiple(pulsedObjects,1,true,0)==STATUS_TIMEOUT);
    ObservedEvent manualPulse(true);
    KObject* manualObjects[]={&manualPulse};
    auto ma=std::async(std::launch::async,[&]{return WaitMultiple(manualObjects,1,true,1000);});
    auto mb=std::async(std::launch::async,[&]{return manualPulse.Wait(1000);});
    AwaitScans(manualPulse,2);manualPulse.Pulse();
    CHECK(ma.get()==0 && mb.get()==0);
    CHECK(manualPulse.Wait(0)==STATUS_TIMEOUT);
    ObservedEvent allPulse(false);KEvent later(false,false);
    KObject* pulseAll[]={&allPulse,&later};
    auto pulsePending=std::async(std::launch::async,[&]{return WaitMultiple(pulseAll,2,false,100);});
    AwaitScans(allPulse,1);allPulse.Pulse();AwaitScans(allPulse,2);later.Set();
    CHECK(pulsePending.get()==STATUS_TIMEOUT);
    CHECK(later.state); // Neither failed WaitAll consumed this other signal.

    // NtSignalAndWaitForSingleObjectEx: a pulse landing between the signal and the start of
    // the wait (TTT2 job groups complete with NtPulseEvent) must still wake the waiter, which
    // counts pulses from the generation read before its signal. A later plain wait does not
    // see that pulse, and a wait that only starts counting after it (the old order) misses it.
    KEvent gap(false,false);
    uint64_t pulseBase; { std::lock_guard lk(g_dispatchMutex); pulseBase=gap.PulseGeneration(); }
    gap.Pulse();
    CHECK(gap.Wait(20)==STATUS_TIMEOUT);          // old order: the pulse is lost
    CHECK(gap.WaitFrom(20,pulseBase)==STATUS_SUCCESS); // atomic order: it is not
    CHECK(gap.Wait(20)==STATUS_TIMEOUT);          // and it wakes only that one waiter
    KEvent gapManual(true,false);
    { std::lock_guard lk(g_dispatchMutex); pulseBase=gapManual.PulseGeneration(); }
    gapManual.Pulse();
    CHECK(gapManual.WaitFrom(20,pulseBase)==STATUS_SUCCESS);
    CHECK(gapManual.Wait(20)==STATUS_TIMEOUT);

    // Thread completion participates in the same dispatcher notification.
    auto* completed=new KThread;
    completed->Start([]{std::this_thread::sleep_for(10ms);},false);
    KObject* threadObjects[]={completed};
    CHECK(WaitMultiple(threadObjects,1,true,1000)==STATUS_SUCCESS);
    CHECK(WaitMultiple(threadObjects,1,true,0)==STATUS_SUCCESS);

    KObject unsupported;
    KObject* invalid[]={&unsupported};
    KObject* duplicate[]={&first,&first};
    CHECK(WaitMultiple(pair,0,true,0)==STATUS_INVALID_PARAMETER);
    CHECK(WaitMultiple(pair,65,true,0)==STATUS_INVALID_PARAMETER);
    CHECK(WaitMultiple(duplicate,2,false,0)==STATUS_INVALID_PARAMETER);
    CHECK(WaitMultiple(invalid,1,true,0)==STATUS_OBJECT_TYPE_MISMATCH);

    // Real PPC import adapters, using synthetic big-endian dispatcher headers.
    std::vector<uint8_t> guest(4096);
    auto* base=guest.data();
    GWrite8(base,0x100,0);GWrite32(base,0x104,0); // shutdown manual event
    GWrite8(base,0x120,1);GWrite32(base,0x124,1); // ready auto event
    GWrite32(base,0x200,0x100);GWrite32(base,0x204,0x120);
    WriteTimeout(base,0x300,0);
    auto guestWait=[&](uint32_t type=1){
        PPCContext c{};c.r3.u32=2;c.r4.u32=0x200;c.r5.u32=type;
        c.r6.u32=3;c.r7.u32=1;c.r8.u32=0;c.r9.u32=0x300;c.r10.u32=0x340;
        __imp__KeWaitForMultipleObjects(c,base);return c.r3.u32;
    };
    CHECK(guestWait()==1);
    CHECK(guestWait()==STATUS_TIMEOUT);
    CHECK(guestWait(2)==STATUS_INVALID_PARAMETER);
    GWrite32(base,0x204,0x100);CHECK(guestWait()==STATUS_INVALID_PARAMETER);
    GWrite32(base,0x204,0x120);
    GWrite8(base,0x180,5);GWrite32(base,0x204,0x180);
    CHECK(guestWait()==STATUS_OBJECT_TYPE_MISMATCH);

    // ObReferenceObjectByHandle uses high handle values as object pointers.
    auto* referenced=new KSemaphore(1,1);
    GWrite32(base,0x204,KernelObjectHandle(referenced));
    CHECK(guestWait()==1 && referenced->count==0);
    auto* referencedEvent=new KEvent(false,false);
    const auto eventHandle=KernelObjectHandle(referencedEvent);
    GWrite32(base,0x204,eventHandle);
    PPCContext signal{};signal.r3.u32=eventHandle;__imp__KeSetEvent(signal,base);
    CHECK(signal.r3.u32==STATUS_SUCCESS && guestWait()==1);
    GWrite32(base,0x204,0xFFFFFFFE);CHECK(guestWait()==STATUS_INVALID_HANDLE);

    // r7 is KeSingle's timeout; r6 is only Alertable. With the old r6 bug this
    // would return success after the delayed signal instead of timing out now.
    auto signalLater=std::async(std::launch::async,[&]{std::this_thread::sleep_for(25ms);QueryGuestEvent(base,0x100)->Set();});
    PPCContext c{};c.r3.u32=0x100;c.r6.u32=0;c.r7.u32=0x300;
    __imp__KeWaitForSingleObject(c,base);
    CHECK(c.r3.u32==STATUS_TIMEOUT);
    signalLater.get();
    QueryGuestEvent(base,0x100)->Reset();
    CHECK(GuestTimeout(base,0)==-1);
    WriteTimeout(base,0x300,-1);CHECK(GuestTimeout(base,0x300)==1);
    WriteTimeout(base,0x300,-10001);CHECK(GuestTimeout(base,0x300)==2);
    WriteTimeout(base,0x300,INT64_MIN);CHECK(GuestTimeout(base,0x300)==-1);
    WriteTimeout(base,0x300,0);CHECK(GuestTimeout(base,0x300)==0);

    // Auto-reset hand-off: Set with a thread already waiting releases that thread, and the
    // setter waiting right after cannot take the signal back (console/Windows semantics; the
    // monitor loop sub_82AE4438 sets the event and waits on it again).
    {
        KEvent baton(false,false);
        auto waiter=std::async(std::launch::async,[&]{ return baton.Wait(2000); });
        const auto limit=std::chrono::steady_clock::now()+1s;
        for(;;) {
            {std::lock_guard lk(baton.m); if(!baton.queue.empty()) break;}
            CHECK(std::chrono::steady_clock::now()<limit);
            std::this_thread::yield();
        }
        baton.Set();
        CHECK(baton.Wait(50)==STATUS_TIMEOUT); // the setter must not re-take it
        CHECK(waiter.get()==STATUS_SUCCESS);
        CHECK(!baton.state && baton.queue.empty() && baton.granted.empty());
        // Nobody queued: the signal stays set for the next waiter and is consumed once.
        baton.Set(); CHECK(baton.state);
        CHECK(baton.Wait(0)==STATUS_SUCCESS); CHECK(baton.Wait(0)==STATUS_TIMEOUT);
        CHECK(baton.Wait(10)==STATUS_TIMEOUT && baton.queue.empty()); // timeouts leave the queue
    }
    std::printf("PASS %u dispatcher checks: indices, atomic consumption, reset modes, blocking, deadlines, PPC ABI, auto-reset hand-off\n",checks);
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="ttt2-waits-") as temp:
        folder = Path(temp)
        (folder / "ppc_recomp_shared.h").write_text(SHARED, encoding="utf-8")
        objects = ROOT / "port/src/runtime/kernel/kernel_objects.cpp"
        source = CHECKER.replace("@OBJECTS@", objects.as_posix())
        exe = compile_one(folder, "waits", source, [folder, RECOMP / "XenonUtils",
            RECOMP / "thirdparty/simde", ROOT / "port/src/runtime"])
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == "__main__":
    main()
