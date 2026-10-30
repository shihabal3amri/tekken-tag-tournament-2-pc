// D3DXGetShaderConstantTable cache. TTT2's material binding (Namco NU: sub_82B0E598 / 6B0 / 820
// / 9B8) asks D3DX for a shader's constant table for every parameter it binds, looks the
// parameter up and releases the table again. Setting up a new fighter's effects therefore parsed
// the same few tables thousands of times in one frame - MEASURED 2026-10-15 in a fight: one
// 102 ms frame made 265,142 guest heap allocations and 482,196 frees, most of them in this parser
// (sub_82C12A48 under sub_82C0DF78).
//
// The table depends only on the shader's CTAB block (container shaders: offset at +16, block
// size at that offset, the data after it - the parser sub_82C12F38 reads nothing else with the
// flags 0 that sub_82C0DF78 passes) and the table copies it into its own buffers (its destructor
// sub_82C11BF8 frees +8, +20 [+24 entries], +28). So each thread keeps one table per distinct
// CTAB (an extra reference, never released) and hands it out again with the reference count
// raised, exactly what the guest's AddRef does. AddRef / Release become atomic because a shared
// table can now be referenced from two threads. TTT2_CTAB_CACHE=0 turns the cache off.
#include "kernel_shared.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <unordered_map>

extern "C" PPC_FUNC(__imp__sub_82C0DF78);   // D3DXGetShaderConstantTable(shader, &table) -> HRESULT
extern "C" PPC_FUNC(__imp__sub_82C0E8E8);   // constant table AddRef (count at +4)
extern "C" PPC_FUNC(__imp__sub_82C12C78);   // constant table Release
PPC_EXTERN_FUNC(sub_82C11BF8);              // constant table destructor
PPC_EXTERN_FUNC(sub_82BF8188);              // operator delete (r4 = allocation tag)

namespace ctab {
std::atomic<uint64_t> g_calls{ 0 }, g_hits{ 0 };

bool On()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_CTAB_CACHE"); return !(e && *e == '0'); }();
    return on;
}

// Atomic add on the big-endian guest reference count; returns the new count.
uint32_t AddCount(uint8_t* base, uint32_t object, int32_t delta)
{
    auto* word = reinterpret_cast<volatile long*>(base + object + 4);
    long seen = *word;
    for (;;)
    {
        const uint32_t next = _byteswap_ulong(_byteswap_ulong(uint32_t(seen)) + uint32_t(delta));
        const long previous = _InterlockedCompareExchange(word, long(next), seen);
        if (previous == seen) return _byteswap_ulong(next);
        seen = previous;
    }
}

uint64_t Hash(const uint8_t* p, uint32_t size)
{
    uint64_t h = 0xCBF29CE484222325ull ^ size;
    uint32_t i = 0;
    for (; i + 8 <= size; i += 8)
    {
        uint64_t v;
        std::memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
} // namespace ctab

PPC_FUNC(sub_82C0DF78)
{
    const uint32_t shader = ctx.r3.u32, out = ctx.r4.u32;
    if (!ctab::On() || !shader || !out || (GRead32(base, shader) & 0xFFFFFF00u) != 0x102A1100u)
    {
        __imp__sub_82C0DF78(ctx, base);
        return;
    }
    ++ctab::g_calls;
    const uint32_t offset = GRead32(base, shader + 16);
    const uint32_t size = offset ? GRead32(base, shader + offset) : 0;
    if (!offset || !size || size > (1u << 20))
    {
        __imp__sub_82C0DF78(ctx, base);
        return;
    }
    struct Key { uint64_t hash; uint32_t size; bool operator==(const Key&) const = default; };
    struct KeyHash { size_t operator()(const Key& k) const { return size_t(k.hash ^ k.size); } };
    thread_local std::unordered_map<Key, uint32_t, KeyHash> tables;
    const Key key{ ctab::Hash(base + shader + offset + 4, size), size };
    if (auto it = tables.find(key); it != tables.end())
    {
        ++ctab::g_hits;
        ctab::AddCount(base, it->second, 1);
        GWrite32(base, out, it->second);
        ctx.r3.u32 = 0;                                  // S_OK
        return;
    }
    __imp__sub_82C0DF78(ctx, base);
    const uint32_t table = GRead32(base, out);
    if (ctx.r3.s32 >= 0 && table)
    {
        ctab::AddCount(base, table, 1);                  // the cache's own reference
        tables.emplace(key, table);
    }
}

PPC_FUNC(sub_82C0E8E8)
{
    if (!ctab::On()) { __imp__sub_82C0E8E8(ctx, base); return; }
    ctx.r3.u32 = ctab::AddCount(base, ctx.r3.u32, 1);
}

PPC_FUNC(sub_82C12C78)
{
    if (!ctab::On()) { __imp__sub_82C12C78(ctx, base); return; }
    const uint32_t table = ctx.r3.u32;
    const uint32_t count = ctab::AddCount(base, table, -1);
    if (count == 0)
    {
        ctx.r3.u32 = table;
        sub_82C11BF8(ctx, base);
        ctx.r4.u32 = 0x24810000;                         // lis r4,9345: the parser's allocation tag
        ctx.r3.u32 = table;
        sub_82BF8188(ctx, base);
    }
    ctx.r3.u32 = count;
}

// Hitch reports (diag_hooks.cpp): calls and cache hits so far.
void CtabCacheCounts(uint64_t& calls, uint64_t& hits)
{
    calls = ctab::g_calls.load();
    hits = ctab::g_hits.load();
}
