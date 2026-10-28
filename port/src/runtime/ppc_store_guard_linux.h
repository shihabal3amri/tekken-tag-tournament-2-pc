#pragma once

// Injected only into generated PPC translation units. Their scalar store macros
// are overridable, while vector stores, dcbz and stwcx write directly to the
// guest base. Handle all four forms before the process-wide null-page fallback.
#if !defined(__linux__) && !defined(__APPLE__)
#error "The generated PPC store guard requires Linux or macOS"
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include "ppc_recomp_shared.h"
#include "guest_memory.h"

namespace ttt2_native_ppc {
constexpr size_t NullGuardBytes = guest_memory::NullGuardBytes;

inline void StoreVector(uint8_t* base, simde__m128i* destination, simde__m128i value)
{
    const uintptr_t offset = reinterpret_cast<uintptr_t>(destination) -
                             reinterpret_cast<uintptr_t>(base);
    if (offset >= NullGuardBytes)
    {
        simde_mm_store_si128(destination, value);
        return;
    }
    if (offset + 16 <= NullGuardBytes) return;
    alignas(16) uint8_t bytes[16];
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(bytes), value);
    for (size_t i = NullGuardBytes - offset; i < 16; ++i)
        *(reinterpret_cast<uint8_t*>(destination) + i) = bytes[i];
}

inline void StoreVectorWithContext(PPCContext& ctx, uint8_t* base,
                                   simde__m128i* destination, simde__m128i value)
{
    // PPCContext is host-owned storage, not guest-addressed memory.
    // Register-member destinations fold to a constant offset at compile time.
    const uintptr_t registerOffset = reinterpret_cast<uintptr_t>(destination) -
                                     reinterpret_cast<uintptr_t>(&ctx);
    if (__builtin_constant_p(registerOffset) &&
        registerOffset <= sizeof(PPCContext) - sizeof(simde__m128i)) {
        simde_mm_store_si128(destination, value);
        return;
    }
    StoreVector(base, destination, value);
}

inline void* BlockSet(uint8_t* base, void* destination, int value, size_t bytes)
{
    guest_memory::FillGuest(base, destination, value, bytes);
    return destination;
}

template <typename T, typename Expected, typename Desired>
inline bool CompareAndSwap(uint8_t* base, T* destination,
                           Expected expected, Desired desired)
{
    static_assert(std::is_integral_v<T> && (sizeof(T) == 4 || sizeof(T) == 8));
    const T expectedWord = static_cast<T>(expected);
    const T desiredWord = static_cast<T>(desired);
    const uintptr_t offset = reinterpret_cast<uintptr_t>(destination) -
                             reinterpret_cast<uintptr_t>(base);
    if (offset <= NullGuardBytes - sizeof(T))
        return __atomic_load_n(destination, __ATOMIC_SEQ_CST) == expectedWord;
    return __sync_bool_compare_and_swap(destination, expectedWord, desiredWord);
}
} // namespace ttt2_native_ppc

#undef PPC_STORE_U8
#undef PPC_STORE_U16
#undef PPC_STORE_U32
#undef PPC_STORE_U64
#define PPC_STORE_U8(address, value)  guest_memory::StoreScalar(base, uint32_t(address), uint8_t(value))
#define PPC_STORE_U16(address, value) guest_memory::StoreScalar(base, uint32_t(address), __builtin_bswap16(uint16_t(value)))
#define PPC_STORE_U32(address, value) guest_memory::StoreScalar(base, uint32_t(address), __builtin_bswap32(uint32_t(value)))
#define PPC_STORE_U64(address, value) guest_memory::StoreScalar(base, uint32_t(address), __builtin_bswap64(uint64_t(value)))

#undef simde_mm_store_si128
#define simde_mm_store_si128(destination, value) ttt2_native_ppc::StoreVectorWithContext(ctx, base, destination, value)
#define memset(destination, value, bytes) ttt2_native_ppc::BlockSet(base, destination, value, bytes)
#define __sync_bool_compare_and_swap(destination, expected, desired) \
    ttt2_native_ppc::CompareAndSwap(base, destination, expected, desired)
