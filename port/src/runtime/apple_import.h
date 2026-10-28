// First-launch import of the player's disc image and Title Update 3 (apple_import.mm).
#pragma once

namespace ttt2_mac {
// Shows the import window on the main thread (after SDL_Init) and returns once the game data is in
// ~/Library/Application Support/TTT2Recomp/game; false when the player quit instead.
bool ImportGameData();
}
