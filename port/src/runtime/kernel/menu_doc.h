// Run-time edits of TTT2's Options / main-menu BX01 document (the PC rows and the EXIT item;
// see port/MENU_PLAN.md). Pure byte-level work: no guest access, testable on a dumped document.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace menu_doc {

struct PcLists
{
    std::vector<std::string> modes;          // DISPLAY MODE values, index = display_settings::Mode
    std::vector<std::string> resolutions;    // RESOLUTION values, index = resolution index
    bool serverRow = false;                  // ONLINE MODE menu: the SERVER row and the server list (server_ui.cpp)
};

// The edited document, or an empty string when `doc` is not the Options / main-menu document (or,
// with `serverRow`, the ONLINE MODE document) or an edit's anchor is missing (`why` says which); the
// game then keeps its own document.
// `note` reports an optional edit that was left out (the rest still applies).
std::string ApplyPcEdits(const uint8_t* doc, size_t size, const PcLists& lists, std::string* why,
                         std::string* note = nullptr);

// The ONLINE MODE menu has the PAIR PLAY row (after PLAYER MATCH: the rows after it one place down).
bool MenuPairPlayRow();
// The online document has the rematch menu (FrameRematch5 / FrameRematch6, online_rematch.cpp) and the room's REMATCH
// MENU row.
bool MenuRematchFrames();
// A rematch menu row's help text, by choice (0 REMATCH .. 4 RETURN TO ROOM, 5 LEAVE - the room host's closes the session).
const char* RematchHelp(int choice, bool host = false);
// The online document has the PAIR PLAY screen's names frame (FrameOnlinePairNames: TextPairPlace1-4 at the stones).
bool MenuPairNamesFrame();

} // namespace menu_doc
