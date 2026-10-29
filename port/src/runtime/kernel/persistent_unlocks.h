#pragma once
#include <cstdint>
struct PPCContext;

namespace persistent_unlocks {
// A distinct one-shot action. The existing temporary UnlockContent toggle is
// unchanged. Caller should explain the mapped scope before queueing.
bool QueuePermanentUnlocks();
// Main game thread only, after the ordinary UI has released its controller.
// Only explicitly known main/options menus may pass allowMenu=true.
void ProcessPending(PPCContext& ctx, uint8_t* base, bool allowMenu);
}
