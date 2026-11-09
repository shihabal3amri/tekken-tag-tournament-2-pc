// Online 2v2 on four PCs (port/ONLINE_2V2_PLAN.md). Each team has a host (one of the match's two fighting machines)
// and a partner (a room member who plays the host's second character). This file: the partner's controller as the
// host's controller 2.
#pragma once
#include "kernel_shared.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace online2v2 {

// A controller's state as the game reads it (the Xbox pad layout, kernel_xam.cpp's XboxPadData).
struct PadData
{
    uint16_t wButtons = 0;
    uint8_t bLeftTrigger = 0, bRightTrigger = 0;
    int16_t sThumbLX = 0, sThumbLY = 0, sThumbRX = 0, sThumbRY = 0;
};

// Diagnostics' clock (ms): one for every instance on this PC (the steady clock is the system's), so their lines line up.
inline unsigned long long DiagMs()
{
    return (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

enum class Role { None, Host, Partner };

// The team this machine is in: its role and the teammate's peer (net_transport.h ids). Tests set it with
// TTT2_2V2_ROLE=host|partner and TTT2_2V2_TEAMMATE=<peer id, hex>; the room sets it (Phase 3).
Role CurrentRole();
uint64_t Teammate();
void SetTeam(Role role, uint64_t teammate);

// Partner: this PC's controller (user 0's state as the game sees it, keyboard included) goes to the host - called
// by the pad poll thread after every poll; sends on change and at least every 16 ms.
void PartnerPadPolled(const PadData& pad);

// Host: the partner's controller, when one is heard from in the last second - the pad poll thread shows it as
// user 1 (the pair-play guest).
bool RemotePad(PadData& pad);

// The game's main thread, once per frame (menu_ext.cpp's UI hook): what the game is showing, for when the partner's
// controller counts on the host.
void Frame(PPCContext& ctx, uint8_t* base);

// xsocket.cpp: a 2v2 packet from `from`.
void Receive(uint64_t from, const uint8_t* data, size_t size);

// ---- teams in a PAIR PLAY room (online_2v2_team.cpp)
// A team packet (Hello / leave / ping); false when it is not one.
bool TeamReceive(uint64_t from, const uint8_t* data, size_t size);
// The game's main thread, every frame (`frame`: the menu frame shown): Hello, the windows, the room's TEAMS row.
void TeamFrame(PPCContext& ctx, uint8_t* base, const std::string& frame);
std::string TeammateName();

// ---- PAIR PLAY (online_2v2_pair.cpp): its own ONLINE MODE row and rooms
// Its pages are open (the menu's PAIR PLAY row opened QUICK / CUSTOM MATCH and CREATE SESSION): what this PC creates is
// a PAIR PLAY session, what it searches lists only those.
bool PairMode();
// The room this PC is in is a PAIR PLAY session.
bool PairRoom();
// The partner's controller counts on its host only in a fight (online_2v2_team.cpp).
bool ForwardRemote();
// A partner, while its match is on: this game does not take its controller (it plays on the host's).
bool MaskLocalPad();
// The host, in a fight: ms its own controller is held back (the partner's one-way delay; 0: none).
int EvenDelayMs();

// A fighting machine: NACE just stored its side's input up to record frame `frame` (rollback_session.cpp's
// sub_82664CE0 hook). The words node 0 holds are sent to the room's other members at once (TTT2_2V2_STREAM=0: off).
void FighterSent(uint8_t* base, int side, int32_t frame);
// The rollback session ended (a match is over): the stream forgets its frames (rollback_session.cpp).
void StreamReset();

// The online PAIR PLAY screen (online_2v2_places.cpp): open - the game's controllers 1-4 are the room's members, every
// PC playing the same presses (the pad poll thread: users 0..count-1 get only the port's presses, this PC's own
// controller goes to the room host through PlacesLocalPad).
bool PlacesActive();
// The PAIR PLAY screen's help line with the players' names while it is open ("": the game's text).
std::string PlacesText(uint8_t* base, uint32_t id);
// ---- a PAIR PLAY room's match start (online_pair_room.cpp: the room; online_2v2_places.cpp: the PAIR PLAY screen)
// A seat of the screen: one of the match's players (the room order's first members; seat k = the screen's controller k + 1).
struct PairSeat { uint64_t xuid = 0; int member = -1; std::string name; };
// The places decided: place 1..4 (red lower, blue lower, red upper, blue upper: the sides' fighters, then their partners)
// -> the seat standing there (-1 none).
// `chooser`: the match's stage chooser, room order position 0 (the red side's fighter) / 1 (the blue side's) - a coin the
// screen's authority drew (online_stage_mirror.cpp stagechooser).
struct PairPlaces { uint32_t round = 0; int place[4] = { -1, -1, -1, -1 }; PairSeat seat[4]; int count = 0; bool cancelled = false; int chooser = 1; };
// The room's match starts and this PC plays it (`mine`: its seat): the PAIR PLAY screen opens.
bool PlacesBegin(const PairSeat* seats, int count, int mine);
// ... and this PC watches it: it waits for the places.
void PlacesWatch(const PairSeat* seats, int count);
// A watcher: the places of the match starting, once decided (once).
bool PlacesDecided(PairPlaces& out);
// The last match's places in this room and this PC's seat in them (-1 a watcher); false: none.
bool PlacesLast(PairPlaces& out, int& mine);
// online_pair_room.cpp: every PC of the room writes the order the places give (the side preference held for the start);
// the match's sync engine started from it as the room's battle start does, this PC's side marked - the flags to ask
// character select with (`sideOut`: the side this PC's player is on, -1 the engine did not start).
void WriteMatchOrder(PPCContext& ctx, uint8_t* base, const PairPlaces& p);
// `hostPos`: the game host's (the stage chooser's) room order position - 0 the red side's fighter, 1 the blue side's.
uint32_t StartPairMatch(PPCContext& ctx, uint8_t* base, const PairPlaces& p, int mySeat, int hostPos, int* sideOut = nullptr);
// online_2v2_pick.cpp: a PAIR PLAY side whose characters character select keeps (STAGE SELECT from the end menu): its
// partner joined as the play-style choice joins it (the select's S, before its cursors exist).
void PairJoinKept(const PPCContext& ctx, uint8_t* base, int side);
// The room this PC is in plays PAIR PLAY matches (a PAIR PLAY room of three members or more).
bool PairMatchRoom(uint8_t* base);
// online_pair_room.cpp: the room's state machine entered a state; the room asks whether this PC watches the match
// starting (-1: not PAIR PLAY's question).
void PairRoomState(const std::string& name);
int PairWatcher(PPCContext& ctx, uint8_t* base);
// online_pair_room.cpp: a member of this PC's room by name (1 there, 0 gone, -1 no room data) - the room control keeps the
// room data up to date under the PAIR PLAY screen too.
int PairMemberPresent(uint8_t* base, const std::string& name);
// online_pair_room.cpp: the match that started was called off on its PAIR PLAY screen (a player left): no match, so the
// room host's after-match step finds no result (the last match's must not count twice).
void PairStartCancelled();
// online_pair_room.cpp: this PC's part of the game's close after a room's match (the session's end, the room's entry) for
// a start that came to nothing - the main thread, every frame until it answers true; then the PC goes back into the room.
// `entry`: the room's entry (0 not tried - the session never started, 1 in the room, 2 failed, 3 no answer).
struct MatchClose { int step = 0; long long at = 0; int entry = 0; };
bool MatchCloseStep(PPCContext& ctx, uint8_t* base, MatchClose& close, const char* what);
// ... for a start called off on its PAIR PLAY screen.
bool PairCallOffClose(PPCContext& ctx, uint8_t* base);
// online_pair_room.cpp, the room host before a match started straight from the end menu (online_straight.cpp): the last
// match's result counted as the room counts it (sub_826BD180: the win streaks) with the room's order kept (the same
// players, the same sides) - the room's own after-match step counts only the series' last match.
void AfterMatchKeepOrder(PPCContext& ctx, uint8_t* base);
// ... and the next match went on from the end menu (the room's step counts the next result again).
void AfterMatchGoesOn();
std::string RoomText(uint32_t id);            // online_2v2_team.cpp: a partner's own words for the room's messages ("" none)
std::string PairPagesText(uint32_t id);        // online_2v2_pair.cpp: PAIR PLAY's words on the Player Match pages ("" none)
// The side the PAIR PLAY places put this PC's player on (0 red / left, 1 blue / right, -1 none).
int MySide();
// The side this PC's player picks on at character select: the one whose first pad slot (9 / 10) holds a controller
// (character select's setup puts this PC's there: sub_825CB328, then sub_826684A8 swaps 9 / 10 and 11 / 12 when the side
// the match gives it is the other). -1: none yet.
int ScreenSide(uint8_t* base);
// A partner's PC during its character select (online_2v2_pick.cpp): its own controller is its game's user 1 (the second
// player's), user 0 gets only the port's presses.
bool PartnerSelectRemap();
// ... while its team's stage is chosen: its own controller holds still (the leader chooses; its screen follows and confirms
// the leader's choice - online_stage_mirror.cpp). A safeguard: the stage step reads only user 0's slot there.
bool PartnerStageHold();
// This PC's sync-engine node is a spectator's (online_2v2_select.cpp): partners, a broken team's partner.
bool SpectatorNode(uint8_t* base);
// The side a partner's character select is told it plays (its team's; -1 none) - online_2v2_select.cpp.
int PartnerSide();
// PAIR PLAY's character ranks, each its controller's (online_2v2_team.cpp): the copies made once in a room; the fight
// HUD's slot and the VS panel's record to read instead of the game's (0: its own).
void PairRankShadows();
// A room battle start without the PAIR PLAY screen: the last places' teams are past (online_2v2_team.cpp).
void ClearPairSides(const char* why);
uint32_t HudRankSlot(uint8_t* base, uint32_t record, uint32_t idx, uint32_t slot);
uint32_t VsRankRecord(uint8_t* base, uint32_t side, uint32_t record);
// This PC's player's slot in side `side`'s record (0 the fighter's, 1 the partner's; -1 not a PAIR PLAY side of two with
// this player on it) - online_2v2_rewards.cpp: each player's save gets its own character's results.
int PairOwnSlot(int side);
// ... by the match's places alone (no teammate or role asked: constant through a fight).
int PairPlacesSlot(int side);
// The select screens' grids (online_2v2_pick.cpp, sub_8227A0D8): headers {table, count} at 0x8200C8BC + 8 * the grid's
// number (the stage select's the fifth, 0x8200C8DC); 32-byte cells {character (63: random), stage, up, down, left, right,
// column, row}. Character select's grid (0: none - no select controller yet):
uint32_t CharacterGrid(uint8_t* base);
// The cell of `grid` whose word at `field` (0 the character, 4 the stage) is `value` (-1: none).
int GridCellOf(uint8_t* base, uint32_t grid, uint32_t field, int value);
// The first press of the shortest way between two cells, by the cells' own neighbours (0: no way).
uint16_t GridStep(uint8_t* base, uint32_t grid, int from, int to);
// Character select's next press from `cell` toward `target` (by its grid; without one, UP / DOWN between rows, LEFT /
// RIGHT along one).
uint16_t SelectStep(uint8_t* base, int cell, int target);
// A side's cursor's still free characters (+0x148 / +0x14C, 64 bits: a pick clears its character's - sub_8227CFC0), and
// the one character a pick took between two of them (-1: none or more than one).
uint64_t FreeCharacters(uint8_t* base, uint32_t cursor);
int TakenCharacter(uint64_t before, uint64_t after);
// The character the teammate's game took for its side's pick `index` in this or the last character select (-1: not
// known - two picks in one frame, or a build that sends none).
int TeammatePick(int index);
int PlacesControllers();
void PlacesLocalPad(uint16_t buttons);
// A spectator: NACE refused an entry for `frame` of `node` (it holds one already, maybe from the stream) - checks the two
// words are the same and logs it when not (rollback_session.cpp's sub_826F4698 hook).
void CheckRefused(uint8_t* base, uint32_t node, uint32_t entry);
// The newest frame of a side heard over that stream here (-1: none yet).
int32_t StreamNewest(int side);
// A spectator: the fighters' inputs heard over the stream go into its own sync engine as the room host's relay would
// put them, so its match runs with the fighters instead of up to two seconds behind. Called on the game's main thread
// before NACE's frame pump (rollback_session.cpp).
void InjectStream(PPCContext& ctx, uint8_t* base);
// A spectator: how many extra frames to simulate this update to catch up with the fighters (0-2) - when its sync engine
// holds at least 4 more of their frames than the next one (rollback.cpp SessionStep).
int CatchUpFrames(uint8_t* base, int32_t lastSimulated);
std::string NodesBrief(uint8_t* base);   // diagnostics: each node's newest/current, the loaded record
// The game's mode: the one the mode manager's factory made last (online_2v2_select.cpp; MEASURED: 5 title, 16 main menu,
// 18 character select, 9 fight, 21 Customize). ~0u before the first.
uint32_t CurrentMode();

} // namespace online2v2

// kernel_xam.cpp: `buttons` held on user `user`'s controller for `holdMs`, after `delayMs` (the game's own input path; a
// 2v2 host's second player joining the room, its character picked at character select).
namespace online2v2 { void SynthPress(uint32_t user, uint16_t buttons, int holdMs, int delayMs = 0); void SynthClear(); }
// kernel_xam.cpp: user `user`'s buttons as last polled (what the game reads; the port's presses included).
namespace online2v2 { uint16_t PadButtons(uint32_t user); }

// xsocket.cpp: a 2v2 packet to `peer` (unreliable channel); the room's peers heard from in 5 s but `except`.
bool XSocketSend2v2(uint64_t peer, const void* data, size_t size);
size_t XSocketRoomPeers(uint64_t* out, size_t max, uint64_t except);

// ---- the start of a room's match and the rematch menu after it, Player Match and PAIR PLAY rooms (online_rematch.cpp)
namespace rematch {
// The mode manager is asked for `mode` (online_2v2_select.cpp's sub_825ED560 hook): the mode to ask for instead, with
// `flags` changed to match.
uint32_t ModeAsked(uint8_t* base, uint32_t mode, uint32_t& flags);
// A game state machine entered the state `name` (online_2v2_team.cpp's sub_825FE600 hook).
void NoteState(const std::string& name);
// The game's main thread, every frame (online2v2::Frame; `frame`: the menu frame shown).
void Frame(PPCContext& ctx, uint8_t* base, const std::string& frame);
// A start / rematch packet (online2v2::Receive); false when it is not one.
bool Receive(uint64_t from, const uint8_t* data, size_t size);
// The room's main state (sub_826C03E0, r3 its control): a PC that chose LEAVE leaves the session from here, the room's own
// way. True: the main state is not to run this frame.
bool RoomMain(PPCContext& ctx, uint8_t* base, uint32_t control);
// menu_ext.cpp's natives: the room's REMATCH MENU row, the rematch menu's A.
void RowToggled(PPCContext& ctx, uint8_t* base);
void Chosen(PPCContext& ctx, uint8_t* base);
// The match restarted from the end menu (online_straight.cpp REMATCH): this PC plays the next one too (its end menu).
void Replaying();
// Every member of the room played its last match (nobody waits for a place).
bool NobodyWaits(uint8_t* base);
// The last match's result as its fighters' votes tell it (the sync engine's G+4: 0 / 1 the room order's first / second
// won, -1 a draw; -2 none told) - for a room host that partnered (no result of its own).
int FightersResult();
}

// ---- REMATCH, STAGE SELECT and CHARACTER SELECT straight from a room match's end menu (online_straight.cpp)
namespace straight {
enum Path : int { kRematch = 0, kStage = 1, kCharacters = 2, kTeams = 3 };
// The decision reached this PC on its end menu (GameMode impl `impl`): the next match starts from here, its stage chooser
// (the game host) room order position `chooser` (-1: the last match's).
void Begin(int path, uint32_t impl, int chooser);
// Every frame of the end menu's state from then (r3 the impl): false once GameMode is handed on.
bool Frame(PPCContext& ctx, uint8_t* base, uint32_t impl);
bool Running();
// online_2v2_select.cpp's sub_82280888 hook (r3 the select controller), before the original; online_2v2_pick.cpp's
// sub_8227F108 hook (the controller, a side's cursor and its state before the original), after it.
void SelectUpdate(PPCContext& ctx, uint8_t* base, uint32_t controller);
void CursorStepped(uint8_t* base, uint32_t controller, uint32_t cursor, uint32_t stateBefore);
void SelectWatch(uint8_t* base, uint32_t controller);
// A PAIR PLAY partner's REMATCH lent its own profile to its side: the exchange's apply keeps it (online_2v2_select.cpp).
bool PairRematchApply();
}

// ---- who chooses a room match's stage: the sync engine's game host, room order position 0 / 1 (online_stage_mirror.cpp)
namespace stagechooser {
// A port engine start (or the room's battle start) made room order position `hostPos` the game host.
void Started(int hostPos, const char* how);
// The last start's game host in this room's session (1: none - the game's own).
int LastHostPos();
// A PAIR PLAY watcher's next room battle start: the places' chooser (it starts the engine through the game's own call).
void WatcherNext(int hostPos);
}

// ---- online character select: the stage choice shown on the PC that does not choose it (online_stage_mirror.cpp)
namespace stagemirror {
// online2v2::Receive (the network thread): a stage packet; false when it is not one.
bool Receive(uint64_t from, const uint8_t* data, size_t size);
// sub_82643770 (the select page built: r3 the page, r5 its controller): a new select.
void PageBuilt(uint32_t page, uint32_t controller);
// The select controller's update (sub_82280888), after the original; `phaseBefore` its +20 before it.
void Frame(PPCContext& ctx, uint8_t* base, uint32_t controller, uint32_t phaseBefore);
// The stage panel on this PC shows the other PC's choice (online_2v2_pick.cpp's teammate stage follow stays out).
bool Mirrored(uint32_t controller);
}
