#pragma once

#include <cstddef>
#include <cstdint>

// Host virtual memory for the flat 32-bit guest address space. Write-watch results
// contain host page addresses. All ranges passed to Reset/TakeWriteWatch must be
// inside the single ReserveGuest allocation; the caller owns the output storage.
// Explicitly opening a range ReadWrite may count those pages as dirty on Linux;
// callers that initialize a range should ResetWriteWatch after initialization.
namespace host_vm {

enum class Protection { None, ReadOnly, ReadWrite };

uint8_t* ReserveGuest(size_t bytes);
// Call only after guest threads are quiescent; Linux joins the mirror fault worker.
void ReleaseGuest(uint8_t* base, size_t bytes);
bool Protect(void* address, size_t bytes, Protection protection);
// Report the contiguous region starting at address, capped by maxBytes and
// the end of the reserved guest allocation.
// A readable region may still be modified by another thread after this query.
bool QueryReadRegion(const void* address, size_t maxBytes,
                     size_t& regionBytes, bool& readable);
// Check guest buffers before asynchronous UI requests. ReadMemory returns false
// for a protected, unmapped, or out-of-guest range; CanWrite checks logical guest permissions
// even when write-watch has temporarily armed a writable page read-only.
bool ReadMemory(const void* address, void* output, size_t bytes);
bool CanWrite(const void* address, size_t bytes);
// Kernel or device writes to a user buffer may not deliver a user-mode fault.
// Open the range for such a write and conservatively count its pages as dirty.
bool PrepareHostWrite(void* address, size_t bytes);
bool ResetWriteWatch(void* address, size_t bytes);
bool TakeWriteWatch(void* address, size_t bytes, void** pages,
                    size_t capacity, size_t& count, bool reset);
// The physical mirror populates a cached view and arms write tracking.
// On Linux, InstallMirrorBacking creates a separate writable staging mapping
// so FillMirrorView can publish the complete chunk after copying. Windows
// retains its page-wise fill. InstallMirrorBacking runs before guest threads.
bool InstallMirrorBacking(void* address, size_t bytes);
bool FillMirrorView(void* address, const void* source, size_t bytes);
// Rollback first-write observer. Capture runs on the writer (also in a POSIX
// signal handler): it must not allocate, lock, or call non-signal-safe functions.
// Handle runs in ordinary context, with the writer parked, before write-watch
// opens the page. Host writes invoke both callbacks on their calling thread.
using CaptureRollbackWriter = uint64_t (*)(uintptr_t address);
using RollbackWriteHandler = bool (*)(uintptr_t address, uint64_t writer);
bool RegisterRollbackWriteHandler(CaptureRollbackWriter capture, RollbackWriteHandler handler);
// Change native write protection without changing logical guest permissions.
bool ProtectForRollback(void* address, size_t bytes, bool readOnly);
unsigned long LastError();

#if defined(__linux__) || defined(__APPLE__)
// Keep the first 64 KiB zero-backed and read-only. On x86-64 Linux a guest write
// executes once under single-step, then the whole guarded range is cleared before
// protection returns; ARM64 (Android, macOS) relies on the generated-store guard.
// macOS: the API stays in 4 KiB pages over 16 KiB host pages (host_vm_mac.cpp).
bool EnableNullPageGuard();
// Signal-handler cleanup if another fault interrupts a single-stepped null
// write. Restores zero contents and read-only protection before recovery.
void CancelNullPageStep(void* context);
// Called only from the SIGSEGV handler after write-watch and deferred faults
// declined the fault. A handler must use only async-signal-safe operations;
// returning leaves normal signal chaining in place.
using UnhandledFaultHandler = void (*)(int signal, void* info, void* context);
bool RegisterUnhandledFaultHandler(UnhandledFaultHandler handler);
// Used by Linux fault adapters whose callbacks need ordinary C++ locks and
// device calls. The signal handler parks the faulting thread while a service
// thread invokes the callback, then retries the faulting instruction.
using DeferredFaultHandler = bool (*)(uintptr_t fault);
bool RegisterDeferredFaultHandler(void* address, size_t bytes,
                                  DeferredFaultHandler handler);
#endif

} // namespace host_vm
