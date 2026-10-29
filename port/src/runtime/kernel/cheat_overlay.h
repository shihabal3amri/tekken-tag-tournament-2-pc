#pragma once
#include <string>
namespace cheat_overlay {
// settings.ini [Mods] Overlay=1 enables the offline cheat panel; each cheat is opt-in.
bool Enabled();
bool ConsumeChord();
void Frame(const std::string& currentFrame);
}
bool HostCheatOverlayChord();
