// The game's menu calls the port's own screens make (menu_ext.cpp implements them with the game's `lib::` natives).
// Call them on the game's main thread only - from the per-frame UI hook at the end of VdSwap. Two-argument menu
// natives take the value first and the widget name second; names are the documents' (port/MENU_PLAN.md).
#pragma once
#include "kernel_shared.h"
#include <cstdint>
#include <string>

namespace menu_host {

// lib::MenuGetCurrentFrameName
std::string CurrentFrame(PPCContext& ctx, uint8_t* base);
// lib::MenuEntitySetText(text, widget)
void SetText(PPCContext& ctx, uint8_t* base, const std::string& widget, const std::string& text);
// lib::MenuSetVisible / lib::MenuSetFocusable (bool, widget): a menu row hidden and skipped by the cursor
void SetVisible(PPCContext& ctx, uint8_t* base, const std::string& widget, bool visible);
void SetFocusable(PPCContext& ctx, uint8_t* base, const std::string& widget, bool focusable);
// lib::MenuEntitySetTextHide(bool, widget): a text widget's text hidden (the widget stays)
void TextHide(PPCContext& ctx, uint8_t* base, const std::string& widget, bool hide);
// lib::MenuIFLumenSetMovieVisible(movie, bool)
void MovieVisible(PPCContext& ctx, uint8_t* base, const std::string& movie, bool visible);
// lib::MenuContainerGetCursor(menu)
int Cursor(PPCContext& ctx, uint8_t* base, const std::string& menu);
// lib::MenuContainerSetCursor(index, menu)
void SetCursor(PPCContext& ctx, uint8_t* base, const std::string& menu, int index);
// lib::MenuSoundKeyOn with the menus' cursor / decide / cancel sounds
enum Sound : uint32_t { SoundCursor = 8426, SoundCancel = 8427, SoundDecide = 8428 };
void PlaySound(PPCContext& ctx, uint8_t* base, Sound sound);
// lib::WindowIsShowing: the game's dialog window is up (its own, or a system UI box)
bool WindowShowing(PPCContext& ctx, uint8_t* base);
// lib::WindowPrintKeyInfoText(key, text): one more hint in the bottom bar's key help, this frame
void KeyHint(PPCContext& ctx, uint8_t* base, uint32_t key, const std::string& text);
// The online rooms' chat log window (tkcsnet::lobby::CChatLogWindow, the lobby [0x82F3188C] + 52): shown or not.
bool ChatLogShown(PPCContext& ctx, uint8_t* base);
void ChatLogShow(PPCContext& ctx, uint8_t* base, bool shown);

// lib::MenuSwitchFrame(name): the menu shows that frame
void ShowFrame(PPCContext& ctx, uint8_t* base, const std::string& frame);

// Frames since the start (the UI hook's count; a native called from a page's OnTick tells its first call per
// frame by it).
uint32_t FrameCount();

} // namespace menu_host
