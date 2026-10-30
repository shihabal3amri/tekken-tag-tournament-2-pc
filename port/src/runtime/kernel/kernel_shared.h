#pragma once
// Shared helpers for kernel import implementations. Each import is a PPC_FUNC(ctx, base):
// arguments arrive in ctx.r3..r10 (PPC ABI), the return value goes in ctx.r3, and `base`
// is the guest memory base (guest address G lives at base + G). Guest memory is big-endian.
#include "ppc_recomp_shared.h"
#include <cstdint>

inline uint32_t GRead32(uint8_t* base, uint32_t ga)  { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + ga)); }
inline uint16_t GRead16(uint8_t* base, uint32_t ga)  { return __builtin_bswap16(*reinterpret_cast<uint16_t*>(base + ga)); }
inline uint8_t  GRead8 (uint8_t* base, uint32_t ga)  { return *(base + ga); }
inline void GWrite32(uint8_t* base, uint32_t ga, uint32_t v) { if (ga) *reinterpret_cast<uint32_t*>(base + ga) = __builtin_bswap32(v); }
inline void GWrite16(uint8_t* base, uint32_t ga, uint16_t v) { if (ga) *reinterpret_cast<uint16_t*>(base + ga) = __builtin_bswap16(v); }
inline void GWrite8 (uint8_t* base, uint32_t ga, uint8_t  v) { if (ga) *(base + ga) = v; }

// Map a host pointer inside the guest block back to a guest address.
inline uint32_t GuestAddr(uint8_t* base, void* host) { return static_cast<uint32_t>(static_cast<uint8_t*>(host) - base); }

// Global guest memory base (set once in main). Lets kernel code reach guest memory without a ctx arg.
extern uint8_t* g_base;

// Filler for unmapped indirect-call slots (main.cpp). Slots holding this are still "free".
extern PPCFunc* const g_unmappedReporter;

// True when env TTT2_WTRACE is set: wait/sleep primitives log their guest caller (lr).
bool WTrace();

// The log (stderr) is written to its file by a background thread (main.cpp StartLogWriter).
// Before the process ends abruptly (TerminateProcess, crash report), wait for what is queued.
void LogDrain(unsigned timeoutMs = 300);

// Report a guest fault (main.cpp). Returns EXCEPTION_EXECUTE_HANDLER.
struct _EXCEPTION_POINTERS;
int GuestFaultReport(_EXCEPTION_POINTERS* ep);

// Call a guest function by address on the current thread's context (used to launch guest threads).
// The guest thread block is cached per host thread; GuestCall2 passes two arguments (r3, r4).
void GuestCall(uint32_t guestAddr, uint32_t arg);
void GuestCall2(uint32_t guestAddr, uint32_t arg0, uint32_t arg1);
void GuestCall4(uint32_t guestAddr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
void GuestSetCurrentCpu(uint8_t cpu);   // set this thread's KPCR current-processor byte
void KernelQueueGuestCall(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);

// File I/O: close a file-range handle (kernel_io.cpp); returns false if not a file handle.
bool TryCloseGuestFile(uint32_t handle);

static constexpr uint32_t STATUS_SUCCESS = 0x00000000u;
static constexpr uint32_t STATUS_UNSUCCESSFUL = 0xC0000001u;

// Guest heap regions (host base + these guest ranges). Kept clear of the thread block
// (0x40000000), the image (0x82000000+), and its function table.
static constexpr uint32_t VHEAP_BEGIN = 0x00100000, VHEAP_END = 0x3F000000;
static constexpr uint32_t PHEAP_BEGIN = 0xA0000000, PHEAP_END = 0xC0000000;
