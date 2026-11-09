#pragma once
// The player's own downloadable content: Xbox LIVE Marketplace packages (content type 2 - TTT2's Bonus Tracks and
// Bonus Movies), which the installer (port/src/setup/dlc_install.cpp) extracts into <game>\dlc\: the folder
// <package file name>\ holds the package's files, <package file name>.ttt2-dlc its content record. Read-only.
#include "save_store.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dlc {
constexpr uint32_t kContentType = 2;   // XCONTENTTYPE_MARKETPLACE
// The installed packages' content records (XCONTENT_DATA), sorted by file name.
std::vector<SaveContentRecord> List();
// XamContentCreateEx of a type-2 record: mounts `root` on the package's folder. Opening only (the content is read-only).
// Win32 error; disposition 2 (opened) on success.
uint32_t Open(const std::string& root, const SaveContentRecord& record, uint32_t mode, uint32_t& disposition);
// XamContentClose (close) / XamContentFlush: true when `root` is a DLC mount.
bool Close(const std::string& root, bool close);
// A guest path on a mounted DLC root ("DLCDATTMP:\index.dat"): true, and the host path (which may not exist).
bool Resolve(const std::string& guest, std::string& host);
}
