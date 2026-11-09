#pragma once
// Emulation of TTT2's Xbox Live title-storage file (default on, TTT2_TITLE_SERVER=0 disables); see
// title_server.cpp. While enabled, user 0 reports a Live sign-in.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace community { class Client; }

bool TitleServerEnabled();
// The title_storage.bin image served for `level` (0..3), zero-padded to `bufferBytes`.
std::vector<uint8_t> TitleStorageImage(uint32_t level, uint32_t bufferBytes);
// One of this PC's players' shareable items (licence.dat) for another player who asked by XUID; false: not ours.
bool TitleServerOwnItem(uint64_t owner, const std::string& item, std::vector<uint8_t>& data);
// After a login to a community server that keeps player files: this player's license, so others can view it.
void TitleServerShareOwnFiles(const std::shared_ptr<community::Client>& server);
// Ranks (server/PROTOCOL.md): false while the license the game read last came from another home than the network's
// now (a server whose ranks are its own, or this PC) - the game must read it again (rank_sync.cpp); true before the
// first read.
bool TitleServerLicenceAtHome();
// The license the game read last came from a server whose ranks are its own.
bool TitleServerLicenceFromServer();
