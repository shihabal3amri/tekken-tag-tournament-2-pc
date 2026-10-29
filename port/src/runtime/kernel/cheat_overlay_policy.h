#pragma once
#include <cstdint>
// Detect L3+R3 on one controller, with one opening per release/press cycle.
struct CheatOverlayChord {
    bool held = false;
    bool Update(bool down) { const bool edge = down && !held; held = down; return edge; }
};
inline bool CheatOverlayAllowed(bool onlineMenu, bool activeSession) {
    return !onlineMenu && !activeSession;
}
