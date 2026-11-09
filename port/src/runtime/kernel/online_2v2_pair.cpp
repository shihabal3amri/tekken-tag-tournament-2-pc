// Online 2v2: PAIR PLAY, its own row of the ONLINE MODE menu (menu_doc.cpp AddPairPlayRow; online_2v2.h). It opens the same
// pages as PLAYER MATCH (QUICK MATCH, CUSTOM MATCH, CREATE SESSION) in pair mode: the sessions it creates are marked
// PAIR PLAY in their record (xsession.cpp, mm::kPairRoom) and its searches list only those - Player Match's never do.
#include "online_2v2.h"
#undef PlaySound                               // windows.h (mmsystem): menu_host's is meant
#include "matchmaking.h"
#include "menu_host.h"
#include <atomic>
#include <cstdio>
#include <string>

uint64_t XSessionCurrentId();                  // xsession.cpp: the room session (0: not in a room)
bool XSessionLocalHosts();                     // xsession.cpp: this game hosts the room
uint32_t XSessionHostNetcode();                // xsession.cpp: the joined session's record word (netcode, kPairRoom)

namespace online2v2 {
namespace {
std::atomic<bool> g_pairMode{ false };
std::atomic<bool> g_pairArmed{ false };        // the ONLINE MODE menu's cursor is on PAIR PLAY (its pages may open next)
// MenuOnlineMenu's rows by place: 0 RANKED MATCH, 1 PLAYER MATCH, 2 PAIR PLAY, 3 WORLD ARENA, ... (menu_doc.cpp)
constexpr int kPairPlayPlace = 2;
}

bool PairMode() { return g_pairMode.load(); }

// The Player Match pages' help lines that say 1-on-1 / Player Match (text lookup, menu_ext.cpp), in PAIR PLAY's words while
// it has them open - and while the ONLINE MODE menu's cursor is on it, as the next page may take its texts before PairFrame
// sees it (the ONLINE MODE menu shows neither text).
std::string PairPagesText(uint32_t id)
{
    if (!g_pairMode.load() && !g_pairArmed.load()) return {};
    if (id == 0xB1) return "Quickly join an open PAIR PLAY session.";      // "Quickly enter a 1-on-1 battle."
    if (id == 0xB3) return "Create a PAIR PLAY session.";                   // "Create a Player Match session."
    return {};
}

bool PairRoom()
{
    if (!XSessionCurrentId()) return false;
    if (XSessionLocalHosts()) return g_pairMode.load();
    return (XSessionHostNetcode() & mm::kPairRoom) != 0;
}

// The game's main thread, every frame (Frame): which ONLINE MODE row opened the Player Match pages.
void PairFrame(PPCContext& ctx, uint8_t* base, const std::string& frame)
{
    static std::string lastFrame;
    static int lastCursor = -1;
    if (frame == "FrameOnlineMenu")
    {
        lastCursor = menu_host::Cursor(ctx, base, "MenuOnlineMenu");
        g_pairArmed = lastCursor == kPairPlayPlace;
        if (g_pairMode.exchange(false)) std::fprintf(stderr, "[pair] back on the ONLINE MODE menu: pair mode off\n");
    }
    else if (frame == "FrameOnline0201-quick_match" && lastFrame == "FrameOnlineMenu")
    {
        const bool pair = lastCursor == kPairPlayPlace;
        g_pairMode = pair;
        std::fprintf(stderr, "[pair] %s opened its pages\n", pair ? "PAIR PLAY" : "PLAYER MATCH");
    }
    if (frame != "FrameOnlineMenu") g_pairArmed = false;
    lastFrame = frame;
}

} // namespace online2v2
