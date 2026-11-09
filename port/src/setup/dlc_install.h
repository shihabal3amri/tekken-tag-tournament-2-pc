// The player's downloadable content (optional): the installer finds the game's packages (content type 2: the bonus
// tracks and bonus movies) in the folder the player chooses and extracts each one to game\dlc\<name>\, with
// <name>.ttt2-dlc beside it ("TTT2DLC1" + the package's XCONTENT_DATA record: device 1, type 2, display name, file
// name). The game lists those as its downloadable content (port/src/runtime/kernel/dlc_content.cpp). Avatar items
// (type 0x9000) are the dashboard's and are skipped. Like the disc, the packages are the player's own files.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ttt2::setup {

struct DlcPackage {
    std::wstring path;      // the package file
    std::string name;       // its folder in game\dlc: the package's file name, or its content id
    std::wstring display;   // "TEKKEN TAG 2 Bonus Tracks Pack B", ...
    uint64_t bytes = 0;     // its files' total size once extracted
};

// The game's packages in `root` (a package file, or a folder searched up to 6 levels down), one per name, sorted by
// display name. Other games' packages and other content types are left out; `damaged` (optional) counts this game's
// packages that could not be read.
std::vector<DlcPackage> FindDlc(const std::wstring& root, int* damaged = nullptr);

// Extracts `p` into <gameDir>\dlc, every 4 KB block checked against the package's own hashes. It goes to a temporary
// folder first: on failure that is removed and a copy of the package installed before stays as it was. The .ttt2-dlc
// file is written last, so the game never lists a half-extracted package.
// `progress(bytes of this package so far)` returns false to cancel (err = "Installation cancelled."). The paths
// created are appended to `written` (folders after their files' folder, for the install's undo).
bool InstallDlc(const DlcPackage& p, const std::wstring& gameDir, const std::function<bool(uint64_t)>& progress,
                std::vector<std::wstring>& written, std::string& err);

} // namespace ttt2::setup
