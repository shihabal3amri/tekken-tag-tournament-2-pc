#pragma once
// Cached physical view (guest 0xC0000000-0xDFFFFFFF) kept coherent with the 0xA0000000 view
// the port allocates physical memory in. See phys_mirror.cpp.
#include <cstdint>

// Close the whole C view (PAGE_NOACCESS) and install the fault handler that fills it.
void PhysMirrorInit(uint8_t* base);
// Bytes of the A view in [start, start + size) changed behind the CPU (a GPU resolve):
// close the C-view chunks that copied them, so the next read refills.
void PhysMirrorInvalidate(uint32_t start, uint32_t size);
// Close every open chunk (frame boundary): bounds staleness after CPU writes to the A view.
void PhysMirrorInvalidateAll();
