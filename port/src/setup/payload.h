// The files carried inside the installer - ours only: the port's executable, the Steamworks
// client library, the VC++ runtime and a readme. No game data.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ttt2::setup {

extern const wchar_t kGameExeName[];

uint64_t PayloadBytes();
// `written` (optional) collects the paths created, for clean-up after a failed install.
bool WritePayload(const wchar_t* destDir, std::string& err, std::vector<std::wstring>* written = nullptr);

} // namespace ttt2::setup
