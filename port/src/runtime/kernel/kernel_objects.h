#pragma once
// Kernel object model with real blocking waits, shared between kernel_objects.cpp (Nt/Ke imports)
// and kernel_thread.cpp (ExCreateThread).
#include <cstdint>
#include <functional>

struct KObject;

uint32_t  KernelObjectHandle(KObject* o);   // register in handle table, return guest handle
KObject*  KernelGetObject(uint32_t handle); // lookup (nullptr if not a handle)

KObject*  KernelCreateThreadObject();
void      KernelStartHostThread(KObject* threadObj, std::function<void()> fn, bool suspended);
uint32_t  KernelResumeThread(uint32_t handle, uint32_t* previous = nullptr);
uint32_t  KernelSuspendThread(uint32_t handle, uint32_t* previous = nullptr);
// ObReferenceObjectByHandle currently exposes these handles as guest thread objects.
// 0xFFFFFFFE identifies the caller, including the bootstrap thread.
void      KernelBindCurrentThreadCpu(volatile uint8_t* cpuByte);
void      KernelUnbindCurrentThreadCpu();                      // kernel_thread.cpp's GuestThreadEnded: its block goes back
size_t    KernelForgetGuestObjects(uint32_t lo, uint32_t size); // the same: host events bound to headers in that block
uint32_t  KernelSetThreadAffinity(uint32_t handle, uint32_t mask, uint32_t& previous);
int32_t   KernelSetThreadPriority(uint32_t handle, int32_t priority);
uint32_t  KernelGuestThreadEntry(unsigned long tid);   // kernel_thread.cpp: a guest thread's entry, 0 unknown
int32_t   KernelQueryThreadPriority(uint32_t handle);

// Signal an event by guest handle (used by file I/O to complete async reads: the NT kernel
// always signals the caller-provided event when an I/O finishes). No-op for non-events.
void      KernelSignalEventHandle(uint32_t handle);
bool      KernelIsEventHandle(uint32_t handle);
