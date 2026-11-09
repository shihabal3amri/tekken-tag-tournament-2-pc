// Ranks (server/PROTOCOL.md "Ranks", title_server.cpp): ONLINE MODE reads the license again when the network changed
// to one whose license lives elsewhere (rank_sync.cpp).
#pragma once
#include "kernel_shared.h"
#include <cstdint>
#include <string>
#include <vector>

namespace rank_sync {

// Every state a game state machine enters, as it names it (the sub_825FE600 hook, online_2v2_team.cpp).
void NoteStateName(uint32_t machine, const std::string& name);
// The same, with the game's context: the main menu ends a server's license in the save object (rank_sync.cpp).
void OnStateNamed(PPCContext& ctx, uint8_t* base, const std::string& name);
// Once per frame on the game's main thread (the UI hook).
void Frame(PPCContext& ctx, uint8_t* base);
// The buttons the game may see (0xFFFF = all): none while ONLINE MODE reads the license again.
uint16_t GameButtonMask();

// A license file (licence.dat) for a 3400-byte license body, stored as the game stores one.
std::vector<uint8_t> PackLicence(const uint8_t* body);
// While a server's license is in use: the player's own, as the game built it from their save when it was taken aside.
bool OwnLicence(std::vector<uint8_t>& file);
// The hooks (rank_sync.cpp): a license copied into the save object; a save asked for (the player's own fields in for
// its copy, the server's back after).
void BeforeLicenceToSave(PPCContext& ctx, uint8_t* base);
uint32_t OwnFieldsIn(PPCContext& ctx, uint8_t* base, std::vector<uint8_t>& server);
void ServerFieldsBack(uint8_t* base, uint32_t live, const std::vector<uint8_t>& server);
// The player's own online character ranks (0..42, by character id), as the game shows them here - the save object's
// character table (+9236 + 76 * character: a server's when its ranks are its own). False: no save object of this
// player's now (none, or a match's side object of someone else). The game's main thread (it calls guest code).
bool OwnOnlineRanks(PPCContext& ctx, uint8_t* base, uint8_t out[63]);

} // namespace rank_sync
