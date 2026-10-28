// Headless test of host_vm_mac.cpp + host_fault_mac.cpp: 4 KiB guest bookkeeping
// over 16 KiB host pages (arm64), or exact 4 KiB host pages (x86_64: Intel Macs and
// Rosetta; build with ARCH=x86_64 mac/build_game.sh --tests). No window, no game. Build: CMake target
// ttt2_host_vm_test (mac/build_game.sh --tests), or by hand:
//   clang++ -std=c++20 -O2 -I port/src/runtime mac/tests/host_vm_test.cpp \
//     port/src/runtime/host_vm_mac.cpp port/src/runtime/host_fault_mac.cpp -o host_vm_test
#include "host_vm.h"
#include "host_fault.h"
#include "guest_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
int g_failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++g_failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

constexpr size_t K4 = 4096;
constexpr size_t kReserve = 64u << 20;          // 64 MiB "guest"
uint8_t* g_base = nullptr;
size_t g_host = 0;

std::vector<uintptr_t> Take(size_t offset, size_t bytes, bool reset = true)
{
    std::vector<void*> pages(bytes / K4 + 1);
    size_t count = 0;
    CHECK(host_vm::TakeWriteWatch(g_base + offset, bytes, pages.data(), pages.size(), count, reset));
    std::vector<uintptr_t> out;
    for (size_t i = 0; i < count; ++i) out.push_back(uintptr_t(static_cast<uint8_t*>(pages[i]) - g_base));
    return out;
}

// ---- rollback observer
std::atomic<int> g_captures{0};
std::atomic<int> g_handled{0};
std::vector<uintptr_t> g_handledPages; // touched only by the VM fault worker / host writes
std::atomic<uint64_t> g_captureThread{0};
bool g_declineNext = false;

uint64_t Capture(uintptr_t address)
{
    g_captures.fetch_add(1);
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    g_captureThread.store(id);
    return address;
}

bool Handle(uintptr_t address, uint64_t writer)
{
    if (writer != address) return false;
    if (g_declineNext) { g_declineNext = false; return false; }
    g_handled.fetch_add(1);
    g_handledPages.push_back(address - uintptr_t(g_base));
    // rollback.cpp opens exactly the 4 KiB page it captured.
    return host_vm::ProtectForRollback(reinterpret_cast<void*>(address), K4, false);
}

// ---- mirror
std::atomic<int> g_fills{0};
uint8_t* g_mirror = nullptr;
constexpr size_t kMirrorBytes = 1u << 20, kChunk = 0x10000;
std::vector<uint8_t> g_source;

bool MirrorFault(uintptr_t fault)
{
    const size_t chunk = (fault - uintptr_t(g_mirror)) / kChunk;
    g_fills.fetch_add(1);
    return host_vm::FillMirrorView(g_mirror + chunk * kChunk, g_source.data() + chunk * kChunk, kChunk);
}

void WriteByte(void* p) { *static_cast<volatile uint8_t*>(p) = 0x5A; }
void ReadByte(void* p) { (void)*static_cast<volatile uint8_t*>(p); }
} // namespace

int main()
{
    g_host = size_t(sysconf(_SC_PAGESIZE));
    std::printf("host page %zu bytes, guest page %zu bytes\n", g_host, K4);
    const size_t sub = g_host / K4;
    // Another 4 KiB page inside the same host page; with 4 KiB host pages, the page itself.
    const size_t second = sub > 1 ? K4 : 0, third = sub > 1 ? 2 * K4 : 0;

    g_base = host_vm::ReserveGuest(kReserve);
    CHECK(g_base != nullptr);
    if (!g_base) return 1;
    CHECK(uintptr_t(g_base) % g_host == 0);

    // 1. Write-watch on 4 KiB sub-pages of one host page.
    const size_t page = 0x100000;                 // host-page aligned offset
    CHECK(host_vm::ResetWriteWatch(g_base + page, g_host));
    CHECK(Take(page, g_host).empty());
    g_base[page + second + 7] = 1;                // second 4 KiB page
    auto dirty = Take(page, g_host);
    CHECK(dirty.size() == sub);                   // conservative: the whole host page
    CHECK(!dirty.empty() && dirty[0] == page);
    CHECK(Take(page, g_host).empty());            // re-armed
    g_base[page + 3] = 2;                         // faults again after the reset
    CHECK(Take(page, g_host).size() == sub);
    // A neighbouring host page stays clean.
    CHECK(host_vm::ResetWriteWatch(g_base + page, 2 * g_host));
    g_base[page + g_host + 5] = 3;
    dirty = Take(page, 2 * g_host);
    CHECK(dirty.size() == sub && !dirty.empty() && dirty[0] == page + g_host);

    // Partial reset: arm only the first 4 KiB page. The others stay dirty and
    // the host page is read-only again until the next store.
    if (sub > 1)
    {
        g_base[page + 1] = 4;
        CHECK(Take(page, g_host, false).size() == sub);
        CHECK(host_vm::ResetWriteWatch(g_base + page, K4));
        dirty = Take(page, g_host, false);
        CHECK(dirty.size() == sub - 1);
        g_base[page + 2 * K4] = 5;                // store to an already-dirty page: must not hang
        CHECK(Take(page, g_host, false).size() == sub);
        CHECK(g_base[page + 2 * K4] == 5);
    }

    // 2. Mixed permissions inside one host page.
    const size_t mixed = 0x200000;
    CHECK(host_vm::Protect(g_base + mixed, K4, host_vm::Protection::None));
    CHECK(host_vm::Protect(g_base + mixed + K4, K4, host_vm::Protection::ReadOnly));
    uint8_t byte = 0;
    CHECK(!host_vm::ReadMemory(g_base + mixed, &byte, 1));        // logical NoAccess
    CHECK(host_vm::ReadMemory(g_base + mixed + K4, &byte, 1));
    CHECK(!host_vm::CanWrite(g_base + mixed + K4, 1));
    CHECK(host_vm::CanWrite(g_base + mixed + 2 * K4, 1));
    size_t region = 0; bool readable = true;
    CHECK(host_vm::QueryReadRegion(g_base + mixed, 3 * K4, region, readable));
    CHECK(!readable && region == K4);
    g_base[mixed + 2 * K4] = 6;                   // the writable neighbour still writes
    CHECK(g_base[mixed + 2 * K4] == 6);
    if (sub > 1)
    {
        // Guard lost by design: the NoAccess page is physically readable.
        CHECK(host_fault::Run([](void* p) { ReadByte(p); }, g_base + mixed));
    }
    // A whole NoAccess host page faults.
    const size_t closed = 0x300000;
    CHECK(host_vm::Protect(g_base + closed, g_host, host_vm::Protection::None));
    CHECK(!host_fault::Run([](void* p) { ReadByte(p); }, g_base + closed));
    // A whole read-only host page rejects stores.
    CHECK(host_vm::Protect(g_base + closed, g_host, host_vm::Protection::ReadOnly));
    CHECK(!host_fault::Run([](void* p) { WriteByte(p); }, g_base + closed));
    CHECK(host_fault::Run([](void* p) { ReadByte(p); }, g_base + closed));

    // 3. Rollback first-write capture covers every protected sub-page.
    CHECK(host_vm::RegisterRollbackWriteHandler(Capture, Handle));
    const size_t rb = 0x400000;
    CHECK(host_vm::ResetWriteWatch(g_base + rb, g_host));
    CHECK(host_vm::ProtectForRollback(g_base + rb, g_host, true));
    std::memset(g_base + rb, 0, 0);               // nothing yet
    g_handledPages.clear();
    g_captures = 0; g_handled = 0;
    std::thread writer([] { g_base[0x400000 + 3] = 0x77; });
    uint64_t writerId = 0;
    pthread_threadid_np(writer.native_handle(), &writerId);
    writer.join();
    CHECK(g_base[rb + 3] == 0x77);
    CHECK(g_captures.load() == int(sub));
    CHECK(g_handled.load() == int(sub));
    CHECK(g_captureThread.load() == writerId);    // captured on the writer
    CHECK(g_handledPages.size() == sub && g_handledPages[0] == rb);
    CHECK(Take(rb, g_host).size() == sub);
    // A declined observer call falls through to write-watch (no hang).
    CHECK(host_vm::ProtectForRollback(g_base + rb, g_host, true));
    g_captures = 0; g_handled = 0; g_declineNext = true;
    g_base[rb + second] = 0x78;
    CHECK(g_base[rb + second] == 0x78);
    CHECK(g_handled.load() == int(sub) - 1);
    // Host writes run the observer for the whole host page too.
    CHECK(host_vm::ProtectForRollback(g_base + rb, g_host, true));
    g_captures = 0; g_handled = 0;
    CHECK(host_vm::PrepareHostWrite(g_base + rb + third, 16));
    CHECK(g_handled.load() == int(sub));
    std::memset(g_base + rb + third, 0x11, 16);
    // Rollback protection on a read-only page leaves it alone.
    CHECK(host_vm::Protect(g_base + rb + g_host, g_host, host_vm::Protection::ReadOnly));
    CHECK(host_vm::ProtectForRollback(g_base + rb + g_host, g_host, true));
    CHECK(!host_vm::CanWrite(g_base + rb + g_host, 1));

    // 4. Physical-mirror style backing: a shared mapping filled on first access.
    g_mirror = g_base + 0x1000000;                // 16 MiB, host aligned
    g_source.resize(kMirrorBytes);
    for (size_t i = 0; i < kMirrorBytes; ++i) g_source[i] = uint8_t(i * 7 + 1);
    CHECK(host_vm::InstallMirrorBacking(g_mirror, kMirrorBytes));
    CHECK(host_vm::RegisterDeferredFaultHandler(g_mirror, kMirrorBytes, MirrorFault));
    CHECK(host_vm::Protect(g_mirror, kMirrorBytes, host_vm::Protection::None));
    CHECK(g_mirror[3 * kChunk + 5] == g_source[3 * kChunk + 5]);
    CHECK(g_fills.load() == 1);
    CHECK(Take(0x1000000 + 3 * kChunk, kChunk).empty());
    g_mirror[3 * kChunk + 2 * K4 + 1] = 0xEE;      // write-watch on the filled chunk
    dirty = Take(0x1000000 + 3 * kChunk, kChunk);
    CHECK(dirty.size() == sub && !dirty.empty() && dirty[0] == 0x1000000 + 3 * kChunk + (2 * K4 / g_host) * g_host);
    // Close it as phys_mirror does, and refill on the next access.
    CHECK(host_vm::Protect(g_mirror + 3 * kChunk, kChunk, host_vm::Protection::None));
    g_source[3 * kChunk + 5] = 0x42;
    CHECK(g_mirror[3 * kChunk + 5] == 0x42);
    CHECK(g_fills.load() == 2);
    // An unaligned mirror is refused.
    CHECK(!host_vm::FillMirrorView(g_mirror + K4, g_source.data(), K4) || g_host == K4);

    // 5. Null guard: 64 KiB read-only, stores fault, reads return zero.
    CHECK(host_vm::EnableNullPageGuard());
    CHECK(g_base[0] == 0 && g_base[guest_memory::NullGuardBytes - 1] == 0);
    for (int i = 0; i < 3; ++i)                    // longjmp out repeatedly (alternate stack)
        CHECK(!host_fault::Run([](void*) { WriteByte(g_base + 0x20); }, nullptr));
    CHECK(!host_fault::Run([](void*) { WriteByte(g_base + guest_memory::NullGuardBytes - 1); }, nullptr));
    CHECK(host_fault::Run([](void*) { WriteByte(g_base + guest_memory::NullGuardBytes); }, nullptr));
    guest_memory::StoreScalar<uint32_t>(g_base, 0x10, 0xFFFFFFFFu); // the guarded path drops it
    CHECK(g_base[0x10] == 0);
    guest_memory::StoreScalar<uint32_t>(g_base, guest_memory::NullGuardBytes - 2, 0xA1B2C3D4u);
    // Little-endian bytes D4 C3 | B2 A1: the two below 64 KiB are dropped.
    CHECK(g_base[guest_memory::NullGuardBytes] == 0xB2 && g_base[guest_memory::NullGuardBytes - 1] == 0);

    // 6. Concurrency: writers on separate sub-pages, a collector taking and re-arming.
    const size_t hot = 0x800000;
    const size_t watched = std::max(4 * g_host, 12 * K4); // the six writers' pages
    CHECK(host_vm::ResetWriteWatch(g_base + hot, watched));
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> stores{0};
    std::vector<std::thread> writers;
    for (int t = 0; t < 6; ++t)
        writers.emplace_back([&, t] {
            volatile uint32_t* word = reinterpret_cast<volatile uint32_t*>(g_base + hot + t * K4 * 2 + 64);
            uint32_t n = 0;
            while (!stop.load(std::memory_order_relaxed)) { *word = ++n; stores.fetch_add(1, std::memory_order_relaxed); }
        });
    size_t takes = 0, seen = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < until) { seen += Take(hot, watched).size(); ++takes; }
    stop = true;
    for (auto& t : writers) t.join();
    CHECK(seen > 0);
    // After the writers stop, one more store per page must still be observed.
    Take(hot, watched);
    for (int t = 0; t < 6; ++t) g_base[hot + t * K4 * 2 + 100] = 1;
    size_t hostPages = 0;
    for (int t = 0; t < 6; ++t)
        if (t == 0 || (t * K4 * 2) / g_host != ((t - 1) * K4 * 2) / g_host) ++hostPages;
    CHECK(Take(hot, watched).size() == hostPages * sub); // every sub-page of each written host page
    std::printf("concurrency: %zu takes, %zu dirty pages seen, %llu stores\n",
                takes, seen, (unsigned long long)stores.load());

    host_vm::ReleaseGuest(g_base, kReserve);
    if (g_failures) { std::printf("%d check(s) failed\n", g_failures); return 1; }
    std::printf("all host_vm checks passed\n");
    return 0;
}
