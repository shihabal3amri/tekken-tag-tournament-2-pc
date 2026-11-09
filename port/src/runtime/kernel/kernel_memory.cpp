// Memory-management kernel imports (real implementations).
#include "kernel_shared.h"
#include <windows.h>
#include "guest_heap.h"
#include "kernel_trace.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

void LogGuestAlloc(const char* what, uint32_t addr, uint32_t size, PPCContext& ctx);   // rollback_probe.cpp
void RollbackNoteAllocation(uint32_t addr, uint32_t size, PPCContext& ctx);            // rollback.cpp

// Fresh kernel allocations: small physical blocks come zeroed. Tempest's particles (a physical allocation of 0xBB500
// bytes from the game's physical allocator, made at the stage load) draw trailing quads the game never writes, and the
// guest heaps hand out blocks earlier stages used - one- or two-frame sheets of pure colour in the first Tempest load
// after other stages (0.1.7). MEASURED 2026-10-30 (stages 0-7 then Tempest, screen captured, 150 s): zeroing physical
// blocks of 2 MB or less 0 such frames, zeroing nothing 7; ~20 MB of memset a menu-to-fight run where zeroing every
// allocation (diag17, 0.1.8 TEST 9) was ~900 MB. TTT2_ALLOC_ZERO=0: as they are; 1: every allocation zeroed; TEST
// ONLY (p_diag34/35): 2 also logs the big ones, 3 = the marker fill.
static int AllocZeroMode()
{
    static const int m = [] { const char* e = std::getenv("TTT2_ALLOC_ZERO"); return e ? std::atoi(e) : 4; }();
    return m;
}
// p_diag35: mode 3 = the marker fill + the allocations it went to (for PoisonScan's report).
struct PoisonAlloc { uint32_t addr, size, lr, seq; const char* what; };
static std::mutex g_poisonMutex;
static std::vector<PoisonAlloc> g_poisonAllocs;
static uint32_t g_poisonSeq = 0;
bool PoisonOn() { return AllocZeroMode() == 3; }
void PoisonScan(const char* kind, uint32_t addr, uint32_t bytes, uint64_t a, uint64_t b)
{
    if (!PoisonOn() || !bytes || uint64_t(addr) + bytes > 0x100000000ull) return;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(g_base + ((addr + 3) & ~3u));
    const uint32_t n = uint32_t(((uint64_t(addr) + bytes) & ~3ull) - ((addr + 3) & ~3u)) / 4;
    uint32_t hits = 0, first = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (w[i] == 0xADDEC07Fu) { if (!hits++) first = i; }
    if (!hits) return;
    const uint32_t at = ((addr + 3) & ~3u) + first * 4;
    std::lock_guard l(g_poisonMutex);
    const PoisonAlloc* pa = nullptr;
    for (auto it = g_poisonAllocs.rbegin(); it != g_poisonAllocs.rend(); ++it)
        if (at >= it->addr && at - it->addr < it->size) { pa = &*it; break; }
    static std::set<std::pair<std::string, uint32_t>> seen;
    static int logged = 0;
    if (!seen.insert({ kind, pa ? pa->addr : (at >> 16) }).second || logged >= 600) return;
    ++logged;
    std::fprintf(stderr, "[poison] %s read %08X+%X: %u marker dwords of %u, first at %08X | %llX %llX | alloc %s %08X+%X lr %08X #%u\n",
                 kind, addr, bytes, hits, n, at, (unsigned long long)a, (unsigned long long)b,
                 pa ? pa->what : "?", pa ? pa->addr : 0, pa ? pa->size : 0, pa ? pa->lr : 0, pa ? pa->seq : 0);
}
static void AllocZeroNote(const char* what, void* p, uint32_t addr, uint32_t size, PPCContext& ctx)
{
    if (!p || !AllocZeroMode()) return;
    if (AllocZeroMode() == 4)       // the default: physical blocks of 2 MB or less
    {
        if (std::strcmp(what, "physical") == 0 && size <= (2u << 20)) std::memset(p, 0, size);
        return;
    }
    if (AllocZeroMode() == 3)      // p_diag35: the marker, only while C:/ttt2/port/flashT.arm exists (boot needs its zeros)
    {
        if (GetFileAttributesA("C:/ttt2/port/flashT.arm") == INVALID_FILE_ATTRIBUTES) return;
        uint32_t* w = static_cast<uint32_t*>(p);
        for (uint32_t i = 0; i < size / 4; ++i) w[i] = 0xADDEC07Fu;
        std::lock_guard l(g_poisonMutex);
        g_poisonAllocs.push_back({ addr, size, uint32_t(ctx.lr), ++g_poisonSeq, what });
        return;
    }
    std::memset(p, 0, size);
    if (AllocZeroMode() >= 2 && size >= 0x10000)
        std::fprintf(stderr, "[alloc0] %s %08X+%X lr %08X\n", what, addr, size, uint32_t(ctx.lr));
}

// NtAllocateVirtualMemory(r3=PVOID* BaseAddress, r4=PSIZE_T RegionSize, r5=AllocType, r6=Protect)
// The whole guest space is committed, so honor a requested address as-is; otherwise carve from the heap.
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    KTrace("NtAllocateVirtualMemory");
    uint32_t basePtr = ctx.r3.u32;
    uint32_t sizePtr = ctx.r4.u32;
    uint32_t reqBase = basePtr ? GRead32(base, basePtr) : 0;
    uint32_t size    = sizePtr ? GRead32(base, sizePtr) : 0;
    uint32_t rounded = (size + 0xFFFF) & ~0xFFFFu;   // 64 KiB granularity
    if (rounded == 0) rounded = 0x10000;

    uint32_t addr = reqBase;
    if (addr == 0)
    {
        void* p = g_virtualHeap.Alloc(rounded, 0x10000);
        if (!p) { ctx.r3.u32 = STATUS_UNSUCCESSFUL; return; }
        addr = GuestAddr(base, p);
        AllocZeroNote("virtual", p, addr, rounded, ctx);
    }
    GWrite32(base, basePtr, addr);
    GWrite32(base, sizePtr, rounded);
    LogGuestAlloc("virtual", addr, rounded, ctx);
    if (!reqBase) RollbackNoteAllocation(addr, rounded, ctx);
    ctx.r3.u32 = STATUS_SUCCESS;
}

// The game's medium-block heap: a TLSF heap for blocks of 353 bytes to 128 KB, which the small-object pools also take
// their pages from. The game sizes it at 15.5 MB of a 64 MB budget (sub_8256A1E8 -> sub_82A68C60(heap, total
// 0x04000000, medium 0x00F80000, ...); the region comes from VirtualAlloc). MEASURED 2026-10-26 (player logs 17-19,
// i3-3240 + GT 730): it ran out while Tulip Festival loaded - small-pool pages held by a few live objects took ~3.5 MB
// more than on a fast PC (the order threads allocate in decides it) - and the game wrote to address 0 and then retried
// forever (a freeze) or went on with null blocks (broken graphics). Here a 12 MB medium heap fails the same way at the
// same load (640 bytes, tag 1, same backtrace); with 16 MB more the load leaves ~20 MB free. The heap's pages are in
// g_virtualHeap, which rollback tracks whole, and the desync check compares the fight's RNG, not memory, so peers with
// either size play together. The rest of the budget stays as it was. TTT2_MEDIUM_HEAP_KB=<size> sets it for tests
// (15872 = the console's).
extern "C" PPC_FUNC(__imp__sub_82A68C60);
PPC_FUNC(sub_82A68C60)
{
    if (ctx.r4.u32 == 0x04000000u && ctx.r5.u32 == 0x00F80000u)
    {
        static const uint32_t medium = [] {
            const char* e = std::getenv("TTT2_MEDIUM_HEAP_KB");
            const uint32_t kb = e ? uint32_t(std::strtoul(e, nullptr, 10)) : 0;
            return kb >= 4096 && kb <= 262144 ? (kb << 10) & ~0xFFFFu : 0x00F80000u + (16u << 20);
        }();
        ctx.r4.u32 += medium - ctx.r5.u32;
        ctx.r5.u32 = medium;
        std::fprintf(stderr, "[heap] the game's medium heap: %.2f MB (the console's 15.50 MB)\n", medium / 1048576.0);
    }
    __imp__sub_82A68C60(ctx, base);
}

// NtFreeVirtualMemory(r3=PVOID* BaseAddress, r4=PSIZE_T RegionSize, r5=FreeType) -> honor best-effort.
PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    KTrace("NtFreeVirtualMemory");
    uint32_t basePtr = ctx.r3.u32;
    uint32_t addr = basePtr ? GRead32(base, basePtr) : 0;
    if (addr) { void* h = base + addr; if (g_virtualHeap.Owns(h)) g_virtualHeap.Free(h); }
    ctx.r3.u32 = STATUS_SUCCESS;
}

// ExAllocatePool(r3=size) / ExAllocatePoolTypeWithTag(r3=size, r4=tag, r5=type) -> guest pool ptr in r3.
PPC_FUNC(__imp__ExAllocatePool)
{
    KTrace("ExAllocatePool");
    const uint32_t size = ctx.r3.u32 ? ctx.r3.u32 : 16;
    void* p = g_virtualHeap.Alloc(size, 16);
    ctx.r3.u32 = p ? GuestAddr(base, p) : 0;
    LogGuestAlloc("pool", ctx.r3.u32, size, ctx);
}
PPC_FUNC(__imp__ExAllocatePoolTypeWithTag)
{
    KTrace("ExAllocatePoolTypeWithTag");
    const uint32_t size = ctx.r3.u32 ? ctx.r3.u32 : 16;
    void* p = g_virtualHeap.Alloc(size, 16);
    if (p) std::memset(p, 0, ctx.r3.u32);
    ctx.r3.u32 = p ? GuestAddr(base, p) : 0;
    LogGuestAlloc("pool-tag", ctx.r3.u32, size, ctx);
}
PPC_FUNC(__imp__ExFreePool)
{
    KTrace("ExFreePool");
    if (ctx.r3.u32) { void* h = base + ctx.r3.u32; if (g_virtualHeap.Owns(h)) g_virtualHeap.Free(h); }
}

// MmAllocatePhysicalMemoryEx(r3=flags, r4=size, r5=protect, r6=min, r7=max, r8=alignment) -> phys guest addr.
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    KTrace("MmAllocatePhysicalMemoryEx");
    uint32_t size = ctx.r4.u32;
    uint32_t align = ctx.r8.u32 ? ctx.r8.u32 : 0x1000;
    const uint32_t flags = ctx.r3.u32;
    void* p = g_physicalHeap.Alloc(size ? size : 0x1000, align);
    ctx.r3.u32 = p ? GuestAddr(base, p) : 0;
    AllocZeroNote("physical", p, ctx.r3.u32, size ? size : 0x1000, ctx);
    LogGuestAlloc("physical", ctx.r3.u32, size, ctx);
    static const bool diag = [] { const char* e = std::getenv("TTT2_DIAG_BAKE"); return e && *e == '1'; }();
    if (diag && size >= 0x40000)
        std::fprintf(stderr, "[diag-bake] phys alloc flags=%08X size=%08X protect=%08X align=%08X -> %08X\n",
            flags, size, ctx.r5.u32, align, ctx.r3.u32);
}
PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    KTrace("MmFreePhysicalMemory");
    // r3 = type, r4 = address (Xbox signature); free the physical allocation.
    uint32_t addr = ctx.r4.u32;
    if (addr) { void* h = base + addr; if (g_physicalHeap.Owns(h)) g_physicalHeap.Free(h); }
}

// GPU sees the same flat memory: physical address == virtual offset.
PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    KTrace("MmGetPhysicalAddress"); /* r3 stays: identity mapping */ }
// MmMapIoSpace(r3=?, r4=physical address, r5=size, r6=protect) -> virtual address. TTT2's XAudio2
// maps XMA contexts with it; physical and virtual addresses are the same in the port.
PPC_FUNC(__imp__MmMapIoSpace)
{
    KTrace("MmMapIoSpace"); ctx.r3.u32 = ctx.r4.u32; }
PPC_FUNC(__imp__MmQueryAddressProtect)
{
    KTrace("MmQueryAddressProtect"); ctx.r3.u32 = 4; /* PAGE_READWRITE */ }
PPC_FUNC(__imp__MmQueryAllocationSize)
{
    KTrace("MmQueryAllocationSize");
    void* h = base + ctx.r3.u32;
    size_t sz = g_virtualHeap.Owns(h) ? g_virtualHeap.Size(h) : (g_physicalHeap.Owns(h) ? g_physicalHeap.Size(h) : 0);
    ctx.r3.u32 = static_cast<uint32_t>(sz);
}
