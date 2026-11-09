// UNLOCK ALL (unlock_all.cpp): a button code on the main menu unlocks what the game's own rewards unlock and fills the
// fight money, after a confirmation in the game's own window and a backup of the profile's save folder.
#pragma once
#include "kernel_shared.h"
#include <cstdint>
#include <string>

namespace unlock_all {

// Every state a game state machine enters, as it names it (the sub_825FE600 hook, online_2v2_team.cpp).
void NoteState(uint32_t machine, const std::string& name);
// Once per frame on the game's main thread (the UI hook): the button code and its confirmation.
void Frame(PPCContext& ctx, uint8_t* base);
// The start of each scene update (rollback.cpp): a confirmed unlock is applied and saved here.
void Update(PPCContext& ctx, uint8_t* base);
// Player 1's buttons the game gets (sys_ui.h's SysUiGameButtonMask): none from YES until the result box is answered.
uint16_t GameButtonMask();

} // namespace unlock_all
