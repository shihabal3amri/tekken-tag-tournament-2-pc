// ONLINE MODE -> SERVER (port/COMMUNITY_SERVER_PLAN.md phase 2): the network the online modes play on - Steam or
// a community server - chosen in the game's own online menus (server_ui.cpp).
#pragma once
#include "kernel_shared.h"
#include <string>

namespace server_ui {

// The SERVER row and the server list are in the game's online menus (TTT2_SRV_UI=0 leaves them out).
bool Enabled();
// Once per frame on the game's main thread (the UI hook, end of VdSwap).
void Frame(PPCContext& ctx, uint8_t* base);
// The server list's own key hints, added in front of its page's (its OnTick, lib::ServerListKeyInfo).
void KeyHints(PPCContext& ctx, uint8_t* base);
// The buttons the game may see (0xFFFF = all): at the ends of the list's 8 rows UP / DOWN scroll the list here
// (the game's cursor would wrap around).
uint16_t GameButtonMask();
// Main thread: the ONLINE MODE menu itself is up, settled, with nothing being connected or asked and no window of
// ours (rank_sync.cpp may send the game through its license reading then).
bool MenuSettled();

// Any thread, when ONLINE MODE opens (the game's first GIN request comes before its menu): the saved server is
// connected now - once per visit - and the call waits at most timeoutMs for the outcome. True when a community
// server is the network.
bool WaitForNetwork(int timeoutMs);
// Any thread: the community server's players right now for the information bar ("TTT2 Test Server: 12 players
// online, 4 playing."), "" on Steam or before the server answered.
std::string StatusLine();
// The Player Match room's Friend Invite (game's main thread): a picker of the player's online Steam friends in the
// game's dialog window; the chosen one is invited into the room's Steam lobby (on a community server: a note instead).
void FriendInvite();

} // namespace server_ui
