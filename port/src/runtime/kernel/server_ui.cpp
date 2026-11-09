// ONLINE MODE -> SERVER (server_ui.h; port/COMMUNITY_SERVER_PLAN.md phase 2, the user's pick D3 = B + C).
//  - The SERVER row (menu_doc.cpp AddServerRow, 7th in the ONLINE MODE menu) shows the network in its value column.
//  - FrameOnlineServerList (AddServerListFrame: the Search Session page's look) lists STEAM, the saved servers with
//    players and ping, servers found on the LAN, and ADD SERVER. The game moves the cursor (and B goes back); this
//    file writes the rows - 8 on screen, scrolled here past either end - and answers A (play here / add), Y
//    (SERVER INFO) and X (REMOVE).
//  - ADD SERVER, SERVER INFO, passwords, "Connecting..." and errors are the game's own confirmation / information
//    windows (sys_ui.h), shown only while the game's window is free.
//  - Entering ONLINE MODE connects to the server chosen last (servers.ini); the main menu disconnects (the choice
//    stays). A lost connection falls back to Steam with a note.
// Connecting and disconnecting block (TLS, Steam ticket, threads joined), so one worker thread runs them in order;
// everything else runs on the game's main thread.
//
// Tests: TTT2_SERVER=<address> (TTT2_SERVER_PASSWORD) starts with that server as the network, unsaved.
#include "server_ui.h"
#include "menu_doc.h"
#include "online_2v2.h"
#include "community_net.h"
#include "menu_host.h"
#include "net_transport.h"
#include "server_list.h"
#include "sys_ui.h"
#include "steam_net.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

uint64_t XSessionCurrentId();   // xsession.cpp: the room session this game is in (0: none)

namespace server_ui {
namespace {

constexpr int kRows = 8;                                  // rows on the list page (menu_doc.cpp kServerRows)
const char* const kListFrame = "FrameOnlineServerList";
const char* const kListMenu = "MenuOnlineServerList";
const char* const kOnlineMenu = "FrameOnlineMenu";
// The ONLINE MODE menu's rows a community server is needed for (the user: "Gray out ranked and other stuff that
// needs server if player selected playing on steam"): on Steam they are not selectable - the game draws a row it
// cannot open grayed and its cursor passes it, as VS BATTLE with one controller. WORLD ARENA needs Namco's lobby
// service, which no network offers: never selectable. Rows of "MenuOnlineMenu" by place: 0 RANKED MATCH, 1 PLAYER
// MATCH, 2 PAIR PLAY, 3 WORLD ARENA, 4 TEAM, 5 TEKKEN CHANNEL, 6 LEADERBOARDS, 7 SERVER - without the PAIR PLAY row
// (menu_doc.cpp MenuPairPlayRow) those after PLAYER MATCH one place up.
// RANKED MATCH is open on Steam too (the user, 2026-10-28): there each player plays with the rank on their PC and
// nothing checks it; a community server can keep its own (server/PROTOCOL.md "Ranks").
// TEKKEN CHANNEL stays open on Steam for its REPLAY THEATER (the replays saved on this PC - the user, 2026-10-23:
// watching them needs no server); its SEARCH REPLAYS needs a server (kReplay* below).
const char* const kOnlineMenuItems = "MenuOnlineMenu";
const char* const kServerOnlyRows[] = { "MenuItemOnline04-team", "MenuItemOnline07-leaderboard" };
const int kServerOnlyPlaces[] = { 4, 6 };
// TEKKEN CHANNEL's page: 0 SEARCH REPLAYS (the server's replays), 1 REPLAY THEATER.
const char* const kReplayFrame = "FrameOnline0501-arcade_replay";
const char* const kReplayMenu = "MenuOnline0501-arcade_replay";
const char* const kSearchReplaysRow = "MenuItemOnline0501-search_replay";
const char* const kWorldArenaRow = "MenuItemOnline03-lobby";
constexpr int kWorldArenaPlace = 3;
// Places as above, with or without the PAIR PLAY row.
int Place(int place) { return menu_doc::MenuPairPlayRow() || place < 2 ? place : place - 1; }
const char* RowValue() { return menu_doc::MenuPairPlayRow() ? "StaticOnlineModeValue008" : "StaticOnlineModeValue007"; }   // SERVER's value column
const char* RowHelp() { return menu_doc::MenuPairPlayRow() ? "StaticOnlineMode008" : "StaticOnlineMode007"; }              // and its help text
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kA = 0x1000, kX = 0x4000, kY = 0x8000;

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Key help numbers of the Y and X buttons (the online menu's own Y "Show Passcode" / X "SEARCH FOR Xbox LIVE
// PARTY" - MEASURED 2026-10-21 with TTT2_SRV_TRACE; the page's own: 9538 SELECT, 1 CONFIRM, 2 BACK).
constexpr uint32_t kHintY = 57382, kHintX = 57383;
// A menu frame counts once it stayed this many frames (documents loading at boot flash other frame names).
constexpr int kStableFrames = 8;
// A window of ours opens only after the screen has settled: a named menu frame shown and the game's own window
// closed for this many frames in a row (see Frame).
constexpr int kSettleFrames = 20;

// ---- text
// Text from a server, made safe for the game's text system: printable ASCII without its markup characters ('~'
// starts a colour, '%' a format), cut to `maxLen` with "..." when longer.
std::string GameText(const std::string& s, size_t maxLen)
{
    std::string out;
    for (unsigned char c : s)
        if (c >= 0x20 && c < 0x7F && c != '~' && c != '%') out.push_back(char(c));
        else if (c == '\n') out.push_back(' ');
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > maxLen) out = out.substr(0, maxLen > 3 ? maxLen - 3 : 0) + "...";
    return out;
}
std::string Upper(std::string s)
{
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
// Lines of at most `width` characters (words kept whole where they fit), at most `maxLines` of them.
std::string Wrap(const std::string& text, size_t width, size_t maxLines)
{
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < text.size() && lines.size() < maxLines)
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string para = text.substr(at, end - at);
        at = end + 1;
        while (lines.size() < maxLines)
        {
            if (para.size() <= width) { lines.push_back(para); break; }
            size_t cut = para.rfind(' ', width);
            if (cut == std::string::npos || cut == 0) cut = width;
            lines.push_back(para.substr(0, cut));
            para = para.substr(cut + (para[cut] == ' ' ? 1 : 0));
        }
    }
    std::string out;
    for (const std::string& l : lines) out += (out.empty() ? "" : "\n") + l;
    return out;
}
// The client's messages ("could not reach x") as a sentence ("Could not reach x.").
std::string Sentence(std::string s)
{
    if (s.empty()) return s;
    s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
    if (s.back() != '.' && s.back() != '!' && s.back() != '?' && s.back() != ')') s += '.';
    return s;
}
// A region id as the server reports it ("middle-east") as words ("Middle East").
std::string RegionName(const std::string& id)
{
    std::string out = GameText(id, 24);
    bool start = true;
    for (char& c : out)
    {
        if (c == '-' || c == '_') { c = ' '; start = true; continue; }
        c = char(start ? std::toupper(static_cast<unsigned char>(c)) : std::tolower(static_cast<unsigned char>(c)));
        start = false;
    }
    return out;
}
// The message of the day for a window: markup removed, line breaks kept.
std::string Motd(const std::string& motd)
{
    std::string out;
    for (unsigned char c : motd)
        if ((c >= 0x20 && c < 0x7F && c != '~' && c != '%') || c == '\n') out.push_back(char(c));
    return Wrap(out, 44, 7);
}

// ---- the network worker: connects and disconnects, in order
struct Job
{
    bool connect = true;
    std::string address, password;
    std::mutex m;
    bool done = false, cancelled = false, ok = false;
    community::Welcome welcome;
    std::string error;
    int refusal = 0;
    uint64_t banUntil = ~0ull;                 // RefusedBanned: until when (net::ConnectServer)
};
std::mutex g_jobMutex;
std::condition_variable g_jobCv;
std::deque<std::shared_ptr<Job>> g_jobs;

void Worker()
{
    for (;;)
    {
        std::shared_ptr<Job> job;
        {
            std::unique_lock l(g_jobMutex);
            g_jobCv.wait(l, [] { return !g_jobs.empty(); });
            job = g_jobs.front();
            g_jobs.pop_front();
        }
        if (!job->connect)
        {
            net::DisconnectServer();
            std::lock_guard l(job->m);
            job->done = true;
            continue;
        }
        community::Welcome welcome;
        std::string error;
        int refusal = 0;
        uint64_t banUntil = ~0ull;
        const bool ok = net::ConnectServer(job->address, job->password, welcome, error, refusal, &banUntil);
        bool undo = false;
        {
            std::lock_guard l(job->m);
            job->ok = ok;
            job->welcome = welcome;
            job->error = error;
            job->refusal = refusal;
            job->banUntil = banUntil;
            job->done = true;
            undo = ok && job->cancelled;                 // cancelled while it ran: not the network after all
        }
        if (undo) net::DisconnectServer();
    }
}
void Enqueue(const std::shared_ptr<Job>& job)
{
    static std::once_flag started;
    std::call_once(started, [] { std::thread(Worker).detach(); });
    {
        std::lock_guard l(g_jobMutex);
        g_jobs.push_back(job);
    }
    g_jobCv.notify_one();
}

// ---- state (main thread only)
struct Entry
{
    enum class Kind { Steam, Saved, Lan, Add } kind = Kind::Steam;
    std::string address, name, password;
    server_list::Live live;
};
struct RowText { std::string label, value, help; bool visible = false; };
// A window waiting for the game's window to be free; the "Connecting..." and "Asking..." ones are tagged (closed
// when what they wait for ends).
enum class Tag { None, Connecting, Asking };
struct Box
{
    std::variant<SysUiMessageBox, SysUiTextBox> window;
    Tag tag = Tag::None;
};

struct Ui
{
    std::string frame;                   // the menu frame (once it stayed kStableFrames)
    int calmFrames = 0;                  // frames in a row with a named frame settled and no window of the game's
    std::string candidate;               // the frame the menus report, and for how long
    int candidateFrames = 0;
    std::atomic<uint16_t> mask{ 0xFFFF };  // GameButtonMask
    bool online = false;                 // in ONLINE MODE (from its first frame to the main menu)
    // A GIN request outside ONLINE MODE (MEASURED: an offline record's upload, POST leaderboard/ after Survival)
    // connects the saved server too, but without windows: none of them belongs over an offline mode's screens.
    bool quiet = false;
    // the rows set for this visit of the ONLINE MODE menu and the network they were set for
    bool rowsSet = false;
    std::string rowsNetwork;
    int64_t nextStep = 0;   // when the cursor may be stepped off a row that needs a server again
    bool replayRowsSet = false;                 // TEKKEN CHANNEL's page: SEARCH REPLAYS set for the network
    std::string replayRowsNetwork;
    int64_t replayNextStep = 0;
    std::string lastNamed;               // the last menu frame (not "(none)"): FrameMainMenu = ONLINE MODE opening
    // the network
    std::string address, name;           // the server that is the network ("" = Steam), its name
    std::set<std::string> motdShown;     // servers whose message was shown in this run
    std::string lostNote;                // a lost connection, told once back in the menus
    // a connection being made
    std::shared_ptr<Job> job;
    std::string jobName;
    bool jobFromList = false;
    bool settleOnFinish = false;         // a WaitForNetwork caller waits for this connection
    // a question to a server being waited for (ADD SERVER, SERVER INFO)
    std::string askAddress;
    uint32_t askAnswers = 0;
    int64_t askUntil = 0;
    std::function<void(const server_list::Live&)> askDone;
    // windows waiting for the game's window to be free, and the tag of the one on screen
    std::deque<Box> boxes;
    Tag boxUp = Tag::None;
    // the list page
    bool listUp = false;
    int settle = 0;                      // frames before the rows are written (the page is loading)
    int top = 0;
    std::vector<Entry> entries;
    RowText shown[kRows];
    bool drawn = false;
    uint16_t prev = 0, repeatKey = 0;
    int64_t repeatAt = 0;
    // the SERVER row's value and help text
    std::string rowValue, rowHelp;
    int64_t nextStatusAsk = 0;           // the server played on is asked for its players every 10 s in ONLINE MODE
} g;

// Servers that banned this player (or keep them out for a while after a kick) are left out of the server list and
// not joined by themselves (2026-10-23). Only the player learns it, from the server at its authenticated login or
// when it is removed while playing (ERROR 6 with until when) - never from the anonymous INFO answers: nobody can ask a
// server about someone else, and those questions cost it nothing more. The ban is remembered with the player's servers
// (server_list::SetBanned) until it ends, and the server decides every connection: choosing it (by address) just
// connects - let in, the ban is forgotten; refused, the server says why. A ban lifted early (an unban, or a kick's
// block the owner ended): when the list opens, each server that banned this player is asked again, by a login that
// leaves at once (net::ProbeServer, every 2 minutes at most) - let in, it is back in the list (the user: once unbanned
// or the kick's block is over, "the player can connect without showing that message").
std::mutex g_statusMutex;
std::string g_status;                          // StatusLine(), made each frame in ONLINE MODE
bool BannedFrom(const std::string& address) { return server_list::Banned(address); }
// Remembers the ban a server told this player (until ~0: an older server did not say - an hour, then it is asked again).
void NoteBan(const std::string& address, uint64_t until)
{
    if (until == ~0ull) until = uint64_t(std::time(nullptr)) + 3600;
    server_list::SetBanned(address, until);
    if (server_list::SameAddress(server_list::Current(), address)) server_list::SetCurrent("");
}
std::string SavedPassword(const std::string& address);
// Asks the servers that banned this player whether they still do (the list page opened; main thread).
void RecheckBans()
{
    static std::map<std::string, std::chrono::steady_clock::time_point> asked;   // address -> when (main thread only)
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [address, until] : server_list::Bans())
    {
        const auto it = asked.find(address);
        if (it != asked.end() && now - it->second < std::chrono::minutes(2)) continue;
        asked[address] = now;
        std::thread([address, password = SavedPassword(address)] {
            uint64_t until = ~0ull;
            const int answer = net::ProbeServer(address, password, until);
            if (answer == 1)
            {
                server_list::SetBanned(address, 1);              // forgotten: back in the list
                std::fprintf(stderr, "[srv-ui] %s lets this player in again (the ban was lifted)\n", address.c_str());
            }
            else if (answer == 0 && until != ~0ull)
                server_list::SetBanned(address, until);          // still banned: until when the server says now
        }).detach();
    }
}
// "12 online, 4 playing" (playing: in rooms and their matches)
std::string Playing(const community::ServerInfo& i)
{
    return std::to_string(i.online) + " online, " + std::to_string(i.inMatch) + " playing";
}

void Show(SysUiMessageBox box, Tag tag = Tag::None) { g.boxes.push_back({ std::move(box), tag }); }

// ---- the room's Friend Invite: Steam's answers come on a worker (the Steam helper may be slow), the windows on the
// game's main thread (Pump)
struct FriendTask
{
    std::mutex m;
    bool done = false;
    std::vector<steamnet::FriendInfo> friends;   // a friends request
    bool sent = false;                           // an invite
    std::string name;                            // whom it invites
};
std::shared_ptr<FriendTask> g_friendList, g_invite;
// The dialog window shows about 9 lines clearly (MEASURED 2026-10-23: with 12 the first ones faded out): the question,
// a blank line, 5 friends, MORE FRIENDS and CANCEL.
constexpr size_t kFriendsPerPage = 5;
void Show(SysUiTextBox box) { g.boxes.push_back({ std::move(box), Tag::None }); }
// The tagged window ends: closed if on screen, dropped if still waiting.
void Close(Tag tag)
{
    if (g.boxUp == tag) { SysUiDismissBox(); g.boxUp = Tag::None; return; }
    for (auto it = g.boxes.begin(); it != g.boxes.end(); ++it)
        if (it->tag == tag) { g.boxes.erase(it); return; }
}
void Info(const std::string& text, std::function<void(int)> done = nullptr)
{
    SysUiMessageBox b;
    b.text = text;
    b.buttons = { "OK" };
    b.cancel = 0;
    b.done = std::move(done);
    Show(std::move(b));
}

std::string NameOf(const std::string& address, const std::string& name)
{
    const std::string n = GameText(name, 40);
    return n.empty() ? GameText(address, 40) : n;
}
std::string SavedPassword(const std::string& address)
{
    for (const server_list::Saved& s : server_list::List())
        if (server_list::SameAddress(s.address, address)) return s.password;
    return {};
}
bool Compatible(const community::ServerInfo& info)
{
    const std::string mine = net::GameProtocol();
    return info.gameProtocols.empty() || std::find(info.gameProtocols.begin(), info.gameProtocols.end(), mine) != info.gameProtocols.end();
}

// ---- the network for an ONLINE MODE visit
// WaitForNetwork's callers (the GIN thread) wait for g_settled to move; the main thread moves it when the visit's
// network is known (connected, failed, cancelled, or nothing to connect).
std::mutex g_waitMutex;
std::condition_variable g_waitCv;
uint64_t g_settled = 0;
std::atomic<bool> g_wanted{ false };

void Settle()
{
    {
        std::lock_guard l(g_waitMutex);
        ++g_settled;
    }
    g_waitCv.notify_all();
}

// ---- connecting
void Disconnect()
{
    if (g.address.empty()) return;
    std::fprintf(stderr, "[srv-ui] leaving %s\n", g.address.c_str());
    g.address.clear();
    g.name.clear();
    auto job = std::make_shared<Job>();
    job->connect = false;
    Enqueue(job);
}

void AskPassword(const std::string& address, const std::string& name, bool wrong, bool fromList);

void Connect(const std::string& address, const std::string& name, const std::string& password, bool fromList)
{
    if (g.job) return;
    g.job = std::make_shared<Job>();
    g.job->address = address;
    g.job->password = password;
    g.jobName = NameOf(address, name);
    g.jobFromList = fromList;
    Enqueue(g.job);
    std::fprintf(stderr, "[srv-ui] connecting to %s%s\n", address.c_str(), g.quiet ? " (no windows: outside ONLINE MODE)" : "");
    if (g.quiet) return;
    SysUiMessageBox b;
    b.text = "Connecting to " + g.jobName + "...";
    b.buttons = { "CANCEL" };
    b.cancel = 0;
    b.done = [](int) {
        // CANCEL: the connection is not wanted (if it is already made, it is closed)
        if (!g.job) return;
        bool undo = false;
        {
            std::lock_guard l(g.job->m);
            g.job->cancelled = true;
            undo = g.job->done && g.job->ok;
        }
        if (undo) { auto job = std::make_shared<Job>(); job->connect = false; Enqueue(job); }
        std::fprintf(stderr, "[srv-ui] connecting cancelled\n");
        g.job.reset();
        g.boxUp = Tag::None;
        if (g.settleOnFinish) { g.settleOnFinish = false; Settle(); }
    };
    Show(std::move(b), Tag::Connecting);
}

void ConnectChecked(const std::string& address, const std::string& name, const server_list::Live& live, bool fromList)
{
    // a remembered ban does not stop it: the server decides (it lets in a player it no longer bans, and tells one it
    // does why - Finished)
    if (live.state == server_list::State::Answered && !Compatible(live.info))
    {
        Info(NameOf(address, name) + " runs another version of the game.\nOnly players of the same version meet there.");
        return;
    }
    std::string password = SavedPassword(address);
    const char* test = std::getenv("TTT2_SERVER");
    if (password.empty() && test && server_list::SameAddress(test, address))
        if (const char* pw = std::getenv("TTT2_SERVER_PASSWORD")) password = pw;
    if (password.empty() && live.state == server_list::State::Answered && live.info.password)
    {
        AskPassword(address, name, false, fromList);
        return;
    }
    Connect(address, name, password, fromList);
}

void AskPassword(const std::string& address, const std::string& name, bool wrong, bool fromList)
{
    SysUiTextBox t;
    t.prompt = NameOf(address, name) + " asks for a password.";
    t.hint = wrong ? "That password was not accepted." : "ENTER: connect   ESC: cancel   CTRL+V: paste";
    t.allowed = " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    t.maxLength = 128;
    t.masked = true;
    t.check = [](const std::string& text) { return text.empty() ? std::string("Type the password.") : std::string(); };
    t.done = [address, name, fromList](bool ok, const std::string& text) {
        if (ok) Connect(address, name, text, fromList);
    };
    Show(std::move(t));
}

// A connection attempt ended (main thread).
void Finished(const std::shared_ptr<Job>& job)
{
    const std::string address = job->address;
    if (g.settleOnFinish)
    {
        g.settleOnFinish = false;
        Settle();                                     // the GIN request waiting for it goes on (the network switched)
    }
    if (!job->ok && g.quiet)
    {
        std::fprintf(stderr, "[srv-ui] could not connect to %s: %s\n", address.c_str(), job->error.c_str());
        return;
    }
    if (!job->ok)
    {
        if (job->refusal == community::RefusedPassword)
        {
            AskPassword(address, g.jobName, !job->password.empty(), g.jobFromList);
            return;
        }
        if (job->refusal == community::RefusedBanned)
        {
            // banned or kicked: ONLINE MODE plays on Steam from now on (it would be refused at every visit), and the
            // list leaves the server out until the ban ends
            NoteBan(address, job->banUntil);
            std::fprintf(stderr, "[srv-ui] %s refused this player (banned or kicked)\n", address.c_str());
        }
        std::string text = "Could not connect to " + g.jobName + ".\n\n" + Wrap(Sentence(GameText(job->error, 300)), 44, 5);
        if (!g.jobFromList) text += "\n\nYou are playing on Steam.";
        Info(text);
        return;
    }
    g.address = address;
    if (server_list::Banned(address)) server_list::SetBanned(address, 1);   // let in: no ban any more
    g.name = GameText(job->welcome.name, 40);
    if (g.name.empty()) g.name = GameText(address, 40);
    std::fprintf(stderr, "[srv-ui] playing on %s (%s)\n", address.c_str(), job->welcome.verified ? "Steam ID checked" : "Steam ID not checked");
    // the choice is remembered; a server played on is in the list (its password with it)
    const char* testServer = std::getenv("TTT2_SERVER");
    if (!(testServer && server_list::SameAddress(testServer, address)))
    {
        server_list::Save({ address, job->welcome.name, job->password });
        server_list::SetCurrent(address);
    }
    const std::string motd = Motd(job->welcome.motd);
    if (!motd.empty() && !g.quiet && (g.jobFromList || !g.motdShown.count(address)))
    {
        g.motdShown.insert(address);
        Info(g.name + "\n\n" + motd);
    }
}

// ONLINE MODE opens: the saved server (TTT2_SERVER for tests) becomes the network; WaitForNetwork's caller learns
// the outcome.
void EnterOnline()
{
    const char* test = std::getenv("TTT2_SERVER");
    const std::string target = test ? std::string(test) : server_list::Current();
    if (target.empty() || server_list::SameAddress(target, g.address))
    {
        Settle();
        return;
    }
    if (!g.job)
    {
        std::string name;
        for (const server_list::Saved& s : server_list::List())
            if (server_list::SameAddress(s.address, target)) name = s.name;
        const char* testPassword = test ? std::getenv("TTT2_SERVER_PASSWORD") : nullptr;
        Connect(target, name, testPassword ? testPassword : SavedPassword(target), false);
    }
    g.settleOnFinish = true;
}

// ---- asking a server (ADD SERVER, SERVER INFO): a window while the answer is awaited
void AskThen(const std::string& address, const std::string& name, std::function<void(const server_list::Live&)> done)
{
    g.askAddress = address;
    g.askAnswers = server_list::Get(address).answers;
    g.askUntil = NowMs() + 3000;
    g.askDone = std::move(done);
    server_list::Ask(address);
    SysUiMessageBox b;
    b.text = "Asking " + NameOf(address, name) + "...";
    b.buttons = { "CANCEL" };
    b.cancel = 0;
    b.done = [](int) { g.askAddress.clear(); g.askDone = nullptr; g.boxUp = Tag::None; };
    Show(std::move(b), Tag::Asking);
}

std::string InfoText(const std::string& address, const server_list::Live& live)
{
    const community::ServerInfo& i = live.info;
    std::string text = NameOf(address, i.name);
    if (!i.name.empty()) text += "  (" + GameText(address, 40) + ")";
    std::string line = RegionName(i.region);
    line += (line.empty() ? "" : "  -  ") + std::to_string(i.online) + " of " + std::to_string(i.maxPlayers) + " players online, " +
            std::to_string(i.inMatch) + " playing";
    if (live.pingMs >= 0) line += "  -  " + std::to_string(live.pingMs) + " ms";
    text += "\n" + line;
    text += i.ownRanks ? "\nRanks: kept by this server. Everyone starts here as a Beginner."
                       : "\nRanks: your own, as saved on your PC.";
    std::string notes;
    if (!Compatible(i)) notes += "Another version of the game. ";
    if (i.password) notes += "Asks for a password. ";
    if (!i.verified) notes += "Does not check Steam accounts. ";
    if (!notes.empty()) text += "\n" + GameText(notes, 120);
    const std::string motd = Motd(i.motd);
    if (!motd.empty()) text += "\n\n" + motd;
    return text;
}

void ServerInfo(const Entry& e)
{
    const std::string address = e.address, name = e.name;
    AskThen(address, name, [address, name](const server_list::Live& live) {
        if (live.state != server_list::State::Answered)
        {
            Info("No answer from " + NameOf(address, name) + "  (" + GameText(address, 40) + ").\nThe server may be down, or the address wrong.");
            return;
        }
        if (server_list::SameAddress(g.address, address)) { Info(InfoText(address, live) + "\n\nYou are playing here."); return; }
        SysUiMessageBox b;
        b.text = InfoText(address, live);
        b.buttons = { "CONNECT", "BACK" };
        b.cancel = 1;
        b.done = [address, name, live](int choice) { if (choice == 0) ConnectChecked(address, name, live, true); };
        Show(std::move(b));
    });
}

void AddServer()
{
    SysUiTextBox t;
    t.prompt = "Add a server by its address.";
    t.hint = "ENTER: add   ESC: cancel   CTRL+V: paste";
    t.allowed = ".:-[]";
    t.maxLength = 100;
    t.check = [](const std::string& text) {
        std::string host;
        uint16_t port;
        if (!community::ParseAddress(text, host, port)) return std::string("A name or IP address, :port if not 28200.");
        if (server_list::List().size() >= server_list::kMaxSaved) return std::string("Your list is full: remove a server first.");
        return std::string();
    };
    t.done = [](bool ok, const std::string& text) {
        if (!ok) return;
        const std::string address = text;
        bool known = false;
        for (const server_list::Saved& s : server_list::List()) known |= server_list::SameAddress(s.address, address);
        AskThen(address, "", [address, known](const server_list::Live& live) {
            if (live.state != server_list::State::Answered)
            {
                SysUiMessageBox b;
                b.text = "No server answered at " + GameText(address, 40) + ".\nAdd it to your list anyway?";
                b.buttons = { "YES", "NO" };
                b.cancel = 1;
                b.done = [address, known](int choice) { if (choice == 0 && !known) server_list::Save({ address, "", "" }); };
                Show(std::move(b));
                return;
            }
            if (server_list::Banned(address))
            {
                ConnectChecked(address, live.info.name, live, true);   // the server decides; let in, the server is saved
                return;
            }
            if (!known) server_list::Save({ address, live.info.name, "" });
            SysUiMessageBox b;
            b.text = InfoText(address, live) + (known ? "\n\nIt is already in your list." : "\n\nAdded to your list.");
            b.buttons = { "CONNECT", "BACK" };
            b.cancel = 1;
            b.done = [address, live](int choice) { if (choice == 0) ConnectChecked(address, live.info.name, live, true); };
            Show(std::move(b));
        });
    };
    Show(std::move(t));
}

void RemoveServer(const Entry& e)
{
    const std::string address = e.address, name = NameOf(e.address, e.name);
    SysUiMessageBox b;
    b.text = "Remove " + name + " from your list?\nIts saved password and server key are forgotten.";
    b.buttons = { "YES", "NO" };
    b.cancel = 1;
    b.done = [address](int choice) {
        if (choice != 0) return;
        if (server_list::SameAddress(g.address, address)) Disconnect();
        server_list::Remove(address);
        std::string host;
        uint16_t port;
        if (community::ParseAddress(address, host, port)) community::ForgetServerKey(net::KnownServersFile(), host, port);
    };
    Show(std::move(b));
}

// ---- the list page
void BuildEntries()
{
    std::vector<Entry> list;
    list.push_back({ Entry::Kind::Steam });
    for (const server_list::Saved& s : server_list::List())
        if (!BannedFrom(s.address)) list.push_back({ Entry::Kind::Saved, s.address, s.name, s.password, server_list::Get(s.address) });
    for (const server_list::Found& f : server_list::Lan())
        if (!BannedFrom(f.address)) list.push_back({ Entry::Kind::Lan, f.address, f.live.info.name, "", f.live });
    list.push_back({ Entry::Kind::Add });
    g.entries = std::move(list);
    g.top = std::clamp(g.top, 0, (std::max)(0, int(g.entries.size()) - kRows));
}

RowText TextOf(const Entry& e)
{
    RowText r;
    r.visible = true;
    const bool current = e.kind == Entry::Kind::Steam ? g.address.empty() : server_list::SameAddress(g.address, e.address);
    const bool connecting = g.job && e.kind != Entry::Kind::Steam && server_list::SameAddress(g.job->address, e.address);
    switch (e.kind)
    {
    case Entry::Kind::Steam:
        r.label = "STEAM";
        r.value = current ? "PLAYING HERE" : "FRIENDS & INVITES";
        r.help = "Play with your Steam friends. No server needed.";
        return r;
    case Entry::Kind::Add:
        r.label = "ADD SERVER";
        r.help = "Add a community server by its address.";
        return r;
    default:
        break;
    }
    const community::ServerInfo& i = e.live.info;
    const std::string name = Upper(GameText(e.name.empty() ? i.name : e.name, 24));
    r.label = e.kind == Entry::Kind::Lan ? "LAN: " + Upper(GameText(i.name, 19)) : name.empty() ? Upper(GameText(e.address, 24)) : name;
    switch (e.live.state)
    {
    case server_list::State::Answered:
        r.value = !Compatible(i) ? "OTHER VERSION"
                                 : std::to_string(i.online) + "/" + std::to_string(i.maxPlayers) + "   " + std::to_string(e.live.pingMs) + " ms";
        break;
    case server_list::State::Offline: r.value = "OFFLINE"; break;
    case server_list::State::BadAddress: r.value = "BAD ADDRESS"; break;
    default: r.value = "..."; break;
    }
    if (current) r.value = "PLAYING HERE";
    if (connecting) r.value = "CONNECTING";
    std::string help;
    if (e.kind == Entry::Kind::Lan) help = "On your local network. ";
    if (e.live.state == server_list::State::Answered)
    {
        if (Compatible(i)) help += Playing(i) + ". ";
        if (i.password) help += "Password. ";
        help += RegionName(i.region);
        const std::string motd = GameText(i.motd.substr(0, i.motd.find('\n')), 60);
        if (!motd.empty()) help += (help.empty() || help.back() == ' ' ? "" : ". ") + motd;
    }
    else if (e.live.state == server_list::State::Offline) help += "No answer from " + GameText(e.address, 40) + ".";
    r.help = GameText(help, 44);
    return r;
}

std::string Two(int i) { return std::string(i < 10 ? "0" : "") + std::to_string(i); }

void DrawList(PPCContext& ctx, uint8_t* base, bool force)
{
    const int n = int(g.entries.size());
    for (int i = 0; i < kRows; ++i)
    {
        const int at = g.top + i;
        const RowText want = at < n ? TextOf(g.entries[size_t(at)]) : RowText{};
        RowText& was = g.shown[i];
        const std::string row = "MenuItemOnlineSrv" + Two(i + 1);
        if (force || want.visible != was.visible)
        {
            // A row with no entry stays shown, blank, and the cursor skips it: a hidden row keeps its box, with
            // two cyan text marks in it (MEASURED 2026-10-24: a list of 5 entries showed 3 boxes with the marks).
            menu_host::SetVisible(ctx, base, row, true);
            menu_host::SetFocusable(ctx, base, row, want.visible);
            menu_host::SetVisible(ctx, base, "SelectValueOnlineSrv" + Two(i + 1), true);
        }
        if (force || want.label != was.label) menu_host::SetText(ctx, base, row, want.label.empty() ? " " : want.label);
        if (force || want.value != was.value) menu_host::SetText(ctx, base, "SelectValueOnlineSrv" + Two(i + 1), want.value);
        if (force || want.help != was.help) menu_host::SetText(ctx, base, "StaticOnlineSrv00" + std::to_string(i + 1), want.help);
        was = want;
    }
    // the page's scroll arrows: more above / below
    const bool up = g.top > 0, down = g.top + kRows < n;
    menu_host::MovieVisible(ctx, base, "yajirushi_up", up);
    menu_host::MovieVisible(ctx, base, "yajirushi_down", down);
    g.drawn = true;
}

// Pressed-this-frame buttons, with auto-repeat for UP / DOWN (400 ms, then every 90 ms).
uint16_t Pressed()
{
    const uint16_t now = HostControllerButtons();
    uint16_t pressed = now & ~g.prev;
    const uint16_t dirs = now & (kUp | kDown);
    const int64_t t = NowMs();
    if (dirs && dirs == g.repeatKey && t >= g.repeatAt) { pressed |= dirs; g.repeatAt = t + 90; }
    else if (dirs != g.repeatKey) { g.repeatKey = dirs; g.repeatAt = t + 400; }
    g.prev = now;
    return pressed;
}

const Entry* Focused(PPCContext& ctx, uint8_t* base)
{
    const int c = menu_host::Cursor(ctx, base, kListMenu);
    const int at = g.top + c;
    return c >= 0 && c < kRows && at < int(g.entries.size()) ? &g.entries[size_t(at)] : nullptr;
}

void ListFrame(PPCContext& ctx, uint8_t* base, bool windowFree)
{
    if (g.settle > 0) { --g.settle; Pressed(); return; }
    static uint32_t version = 0;
    const bool force = !g.drawn;
    if (force || server_list::Version() != version)
    {
        version = server_list::Version();
        BuildEntries();
    }
    const uint16_t pressed = Pressed();
    const bool idle = windowFree && !g.job && g.askAddress.empty() && g.boxes.empty();
    const int c = menu_host::Cursor(ctx, base, kListMenu);
    const int visible = (std::min)(kRows, int(g.entries.size()));
    if (idle)
    {
        // at the ends of the 8 rows on screen UP / DOWN scroll the list (GameButtonMask keeps them from the game,
        // whose cursor would wrap around - MEASURED 2026-10-21)
        if ((pressed & kDown) && c == visible - 1 && g.top + kRows < int(g.entries.size()))
        {
            ++g.top;
            menu_host::PlaySound(ctx, base, menu_host::SoundCursor);
        }
        else if ((pressed & kUp) && c == 0 && g.top > 0)
        {
            --g.top;
            menu_host::PlaySound(ctx, base, menu_host::SoundCursor);
        }
        if (const Entry* e = Focused(ctx, base))
        {
            const Entry entry = *e;
            if (pressed & kA)
            {
                menu_host::PlaySound(ctx, base, menu_host::SoundDecide);
                switch (entry.kind)
                {
                case Entry::Kind::Steam:
                    if (!g.address.empty()) { Disconnect(); server_list::SetCurrent(""); }
                    break;
                case Entry::Kind::Add:
                    AddServer();
                    break;
                default:
                    if (server_list::SameAddress(g.address, entry.address)) Info("You are playing on " + g.name + ".");
                    else ConnectChecked(entry.address, entry.name, entry.live, true);
                    break;
                }
            }
            else if ((pressed & kY) && entry.kind != Entry::Kind::Add)
            {
                menu_host::PlaySound(ctx, base, menu_host::SoundDecide);
                if (entry.kind == Entry::Kind::Steam)
                    Info("STEAM\n\nPlay with your Steam friends: invites, and anyone\nwho searches on Steam. No server needed.");
                else
                    ServerInfo(entry);
            }
            else if ((pressed & kX) && entry.kind == Entry::Kind::Saved)
            {
                menu_host::PlaySound(ctx, base, menu_host::SoundDecide);
                RemoveServer(entry);
            }
        }
    }
    DrawList(ctx, base, force);
    // A is answered here: the page's own menu would take on its pressed look (and keep it)
    g.mask = uint16_t(0xFFFF & ~kA & ~(c == visible - 1 ? kDown : 0) & ~(c == 0 ? kUp : 0));
}

// ---- every frame
void ShowFriends(std::vector<steamnet::FriendInfo> friends, size_t page);

void Pump(bool windowFree)
{
    // the room's Friend Invite: Steam answered
    for (std::shared_ptr<FriendTask>* task : { &g_friendList, &g_invite })
    {
        if (!*task) continue;
        std::unique_lock l((*task)->m);
        if (!(*task)->done) continue;
        const std::shared_ptr<FriendTask> t = std::move(*task);
        l.unlock();
        Close(Tag::Asking);
        if (task == &g_friendList)
        {
            if (t->friends.empty()) Info("None of your Steam friends is online right now.");
            else ShowFriends(std::move(t->friends), 0);
        }
        else if (t->sent)
            Info("Invited " + t->name + ".\nWhen they accept the invite in Steam, they join this room.");
        else
            Info("Steam could not send the invite.\nTry again in a moment.");
    }
    // a connection being made
    if (g.job)
    {
        bool done = false;
        {
            std::lock_guard l(g.job->m);
            done = g.job->done;
        }
        if (done)
        {
            const std::shared_ptr<Job> job = std::move(g.job);
            Close(Tag::Connecting);
            Finished(job);
        }
    }
    // a question being answered
    if (!g.askAddress.empty())
    {
        const server_list::Live live = server_list::Get(g.askAddress);
        const bool answered = live.answers > g.askAnswers && live.full;
        const bool failed = live.state == server_list::State::Offline || live.state == server_list::State::BadAddress;
        if (answered || failed || NowMs() > g.askUntil)
        {
            Close(Tag::Asking);
            auto done = std::move(g.askDone);
            g.askAddress.clear();
            if (done) done(live);
        }
    }
    // the next window, once the game's window is free (never over the game's own)
    if (!g.boxes.empty() && windowFree)
    {
        Box& next = g.boxes.front();
        const bool shown = std::holds_alternative<SysUiMessageBox>(next.window) ? SysUiShowMessageBox(std::get<SysUiMessageBox>(next.window))
                                                                              : SysUiShowTextBox(std::get<SysUiTextBox>(next.window));
        if (shown)
        {
            g.boxUp = next.tag;
            g.boxes.pop_front();
        }
    }
}

} // namespace

bool Enabled()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_SRV_UI"); return !(e && *e == '0'); }();
    return on;
}

void FriendInvite()
{
    std::fprintf(stderr, "[srv-ui] Friend Invite\n");
    if (g_friendList || g_invite) return;                       // one at a time
    if (!g.address.empty())
    {
        // a community server's rooms take only its players: a Steam invite would bring a friend who is not on it
        Info("Friend invites work on Steam (ONLINE MODE > SERVER > STEAM).\nOn " + g.name +
             ", ask your friend to join this server and search for your room.");
        return;
    }
    // TTT2_TEST_FRIENDS=<n> (tests, no Steam): n made-up friends - the picker's layout and paging
    static const int testFriends = [] { const char* e = std::getenv("TTT2_TEST_FRIENDS"); return e ? std::atoi(e) : 0; }();
    if (!steamnet::Running() && testFriends <= 0)
    {
        Info("Steam is not running. Start Steam, then the game, to invite your Steam friends.");
        return;
    }
    if (!XSessionCurrentId())
    {
        Info("There is no room to invite a friend to.");
        return;
    }
    auto t = std::make_shared<FriendTask>();
    g_friendList = t;
    std::thread([t] {
        std::vector<steamnet::FriendInfo> friends = steamnet::Friends(4000);
        for (int i = 0; i < testFriends && friends.size() < 200; ++i)
            friends.push_back({ uint64_t(i + 1), "Test Friend " + std::to_string(i + 1), i < 2 });
        std::lock_guard l(t->m);
        t->friends = std::move(friends);
        t->done = true;
    }).detach();
}

namespace {
void ShowFriends(std::vector<steamnet::FriendInfo> friends, size_t page)
{
    const size_t first = page * kFriendsPerPage, last = (std::min)(friends.size(), first + kFriendsPerPage);
    SysUiMessageBox b;
    b.text = "Invite a Steam friend:";
    if (friends.size() > kFriendsPerPage)
        b.text += "  page " + std::to_string(page + 1) + " of " + std::to_string((friends.size() + kFriendsPerPage - 1) / kFriendsPerPage);
    for (size_t i = first; i < last; ++i)
    {
        std::string name = GameText(friends[i].name, 24);
        if (name.empty()) name = "STEAM FRIEND";
        b.buttons.push_back(name + (friends[i].inGame ? "  - PLAYING" : ""));
    }
    const bool more = last < friends.size();
    if (more) b.buttons.push_back("MORE FRIENDS");
    b.buttons.push_back("CANCEL");
    b.cancel = int(b.buttons.size()) - 1;
    b.done = [friends, first, last, more, page](int choice) {
        const size_t pick = size_t(choice);
        if (more && pick == last - first) { ShowFriends(friends, page + 1); return; }
        if (pick >= last - first) return;                       // CANCEL
        const steamnet::FriendInfo f = friends[first + pick];
        const uint64_t session = XSessionCurrentId();
        auto t = std::make_shared<FriendTask>();
        t->name = GameText(f.name, 24);
        g_invite = t;
        std::thread([t, session, id = f.id] {
            const bool sent = steamnet::InviteToSession(session, id, 5000);
            std::lock_guard l(t->m);
            t->sent = sent;
            t->done = true;
        }).detach();
    };
    Show(std::move(b));
}
} // namespace

std::string StatusLine()
{
    std::lock_guard l(g_statusMutex);
    return g_status;
}

void Frame(PPCContext& ctx, uint8_t* base)
{
    if (!Enabled()) return;
    const std::string reported = menu_host::CurrentFrame(ctx, base);
    if (reported != g.candidate) { g.candidate = reported; g.candidateFrames = 0; }
    else if (g.candidateFrames < kStableFrames) ++g.candidateFrames;
    const bool changed = g.candidateFrames == kStableFrames && g.candidate != g.frame;
    if (changed)
    {
        static const bool trace = std::getenv("TTT2_SRV_TRACE") != nullptr;
        if (trace) std::fprintf(stderr, "[srv-ui] frame %s\n", g.candidate.c_str());
        g.frame = g.candidate;
    }
    const std::string frame = g.frame;
    // ONLINE MODE: from the game's first GIN request (WaitForNetwork) or its first frame, to the main menu
    if (frame.rfind("Frame", 0) == 0) g.lastNamed = frame;
    const bool ginAsks = g_wanted.exchange(false);
    if (!g.online && (ginAsks || frame.rfind("FrameOnline", 0) == 0))
    {
        g.online = true;
        g.quiet = ginAsks && frame.rfind("FrameOnline", 0) != 0 && g.lastNamed != "FrameMainMenu";
        EnterOnline();
    }
    else if (g.quiet && frame.rfind("FrameOnline", 0) == 0)
        g.quiet = false;                                  // ONLINE MODE after all: its windows again
    else if (ginAsks && !g.job) Settle();               // connected, or tried in this visit already
    else if (ginAsks) g.settleOnFinish = true;
    else if (g.online && frame == "FrameMainMenu")
    {
        // left ONLINE MODE: nothing of it stays on screen or running
        g.online = false;
        g.quiet = false;
        Settle();
        if (g.job)
        {
            bool undo = false;
            {
                std::lock_guard l(g.job->m);
                g.job->cancelled = true;
                undo = g.job->done && g.job->ok;
            }
            if (undo) { auto job = std::make_shared<Job>(); job->connect = false; Enqueue(job); }
            g.job.reset();
        }
        g.askAddress.clear();
        g.askDone = nullptr;
        g.boxes.clear();
        if (g.boxUp != Tag::None) { SysUiDismissBox(); g.boxUp = Tag::None; }
        Disconnect();
    }
    // a lost connection: back to Steam, and the player is told once in the menus
    if (!g.address.empty())
        if (const std::shared_ptr<community::Client> server = net::Community(); server && !server->Connected())
        {
            if (server->Refusal() == community::RefusedBanned)
            {
                // kicked or banned while playing: the server's own words; ONLINE MODE plays on Steam from now on and the
                // list leaves the server out until the ban (a kick's block) ends (2026-10-23)
                std::string why = server->CloseReason();
                if (const size_t at = why.find(": "); why.rfind("server error ", 0) == 0 && at != std::string::npos) why = why.substr(at + 2);
                uint64_t until = ~0ull;
                server->BanUntil(until);
                NoteBan(g.address, until);
                std::fprintf(stderr, "[srv-ui] %s removed this player (kicked or banned)\n", g.address.c_str());
                g.lostNote = g.name + "\n\n" + Wrap(Sentence(GameText(why, 200)), 44, 4) + "\n\nYou are playing on Steam.";
            }
            else
                g.lostNote = "The connection to " + g.name + " was lost.\n" + Wrap(Sentence(GameText(server->CloseReason(), 120)), 44, 3) +
                             "\n\nYou are playing on Steam. Choose SERVER to connect again.";
            Disconnect();
        }
    const bool menus = frame == kOnlineMenu || frame == kListFrame;
    if (!g.lostNote.empty() && menus) { Info(g.lostNote); g.lostNote.clear(); }

    // ONLINE MODE opens with the game's "Downloading data..." windows, in the same dialog window as ours: a window of
    // ours opened in a gap between two of them was replaced on screen by the next one while it still held the
    // controller - the menu then ignored every press until the first one answered the unseen window (MEASURED
    // 2026-10-23: the server's welcome text on the first visit; players learned to press START to "wake" the menu).
    // So ours wait until a named frame has settled and the game's window has been closed for kSettleFrames.
    const bool gameWindow = menu_host::WindowShowing(ctx, base);
    // ... and never over the PAIR PLAY screen (its names frame is a named frame too: a window there takes the controller
    // from the screen on one PC only)
    const bool calm = !gameWindow && g.candidate == g.frame && g.frame.rfind("Frame", 0) == 0 && !online2v2::PlacesActive();
    g.calmFrames = calm ? (std::min)(g.calmFrames + 1, kSettleFrames) : 0;
    const bool windowFree = !SysUiBoxBusy() && g.calmFrames >= kSettleFrames;
    Pump(windowFree);

    // the players of the server played on (the SERVER row, the information bar): asked every 10 s in ONLINE MODE
    if (g.online && NowMs() >= g.nextStatusAsk)
    {
        g.nextStatusAsk = NowMs() + 10000;
        if (!g.address.empty()) server_list::Ask(g.address);
    }
    {
        std::string status;
        if (g.online && !g.address.empty())
            if (const server_list::Live live = server_list::Get(g.address); live.state == server_list::State::Answered)
                status = GameText(g.name, 40) + ": " + std::to_string(live.info.online) +
                         (live.info.online == 1 ? " player online, " : " players online, ") +
                         std::to_string(live.info.inMatch) + " playing.";
        std::lock_guard l(g_statusMutex);
        g_status = std::move(status);
    }

    // the rows that need a server: selectable on a server only (again whenever the menu shows or the network changes)
    if (frame != kOnlineMenu) g.rowsSet = false;
    else if (!g.job && (!g.rowsSet || g.rowsNetwork != g.address))
    {
        const bool onServer = !g.address.empty();
        for (const char* row : kServerOnlyRows) menu_host::SetFocusable(ctx, base, row, onServer);
        menu_host::SetFocusable(ctx, base, kWorldArenaRow, false);
        g.rowsSet = true;
        g.rowsNetwork = g.address;
        g.nextStep = 0;
        std::fprintf(stderr, "[srv-ui] online modes for %s: team, leaderboards, tekken channel's search %s\n",
                     onServer ? g.address.c_str() : "Steam", onServer ? "on" : "grayed");
    }
    // TEKKEN CHANNEL's page: SEARCH REPLAYS on a server only; on Steam the cursor steps down to REPLAY THEATER as on
    // the ONLINE MODE menu
    if (frame != kReplayFrame) g.replayRowsSet = false;
    else if (!g.job && (!g.replayRowsSet || g.replayRowsNetwork != g.address))
    {
        menu_host::SetFocusable(ctx, base, kSearchReplaysRow, !g.address.empty());
        g.replayRowsSet = true;
        g.replayRowsNetwork = g.address;
        g.replayNextStep = 0;
    }
    if (frame == kReplayFrame && g.replayRowsSet && g.address.empty() && !g.job && NowMs() >= g.replayNextStep &&
        menu_host::Cursor(ctx, base, kReplayMenu) == 0)
    {
        HostPulseDown();
        g.replayNextStep = NowMs() + 400;
    }
    // The menu opens on RANKED MATCH: when that row needs a server the cursor steps down to the next selectable
    // row by a DOWN press - the menu draws the move itself (setting its cursor directly left the highlight bar and
    // the help text on RANKED MATCH). Again every 400 ms while the menu still sits there (its opening ignores
    // input for a moment).
    if (frame == kOnlineMenu && g.rowsSet && !g.job && NowMs() >= g.nextStep)
    {
        const int at = menu_host::Cursor(ctx, base, kOnlineMenuItems);
        bool off = at == Place(kWorldArenaPlace);
        for (int place : kServerOnlyPlaces) off = off || (g.address.empty() && at == Place(place));
        if (off)
        {
            HostPulseDown();
            g.nextStep = NowMs() + 400;
        }
    }

    // the SERVER row's value: the network
    if (frame == kOnlineMenu)
    {
        const std::string value = g.job ? std::string("CONNECTING") : g.address.empty() ? std::string("STEAM") : Upper(GameText(g.name, 18));
        // a server's name needs the room of the focused row's help text: a shorter one then, with its players
        const server_list::Live live = g.address.empty() ? server_list::Live{} : server_list::Get(g.address);
        const std::string help = g.address.empty() ? "Play on Steam or a community server."
                                 : live.state == server_list::State::Answered ? Playing(live.info) + " on this server."
                                                                              : "Steam or a community server.";
        if (changed || value != g.rowValue || help != g.rowHelp)
        {
            menu_host::SetText(ctx, base, RowValue(), value);
            menu_host::SetText(ctx, base, RowHelp(), help);
            g.rowValue = value;
            g.rowHelp = help;
        }
    }
    // the list page
    if (frame == kListFrame)
    {
        if (!g.listUp)
        {
            g.listUp = true;
            g.settle = 2;
            g.drawn = false;
            g.top = 0;
            g.prev = HostControllerButtons();            // the A that opened the page is not ours
            server_list::Watch(true);
            RecheckBans();
        }
        ListFrame(ctx, base, windowFree);
    }
    else if (g.listUp)
    {
        g.listUp = false;
        server_list::Watch(false);
    }
    if (!g.listUp) g.mask = 0xFFFF;
}

uint16_t GameButtonMask() { return g.mask.load(); }

bool MenuSettled()
{
    return g.online && g.frame == kOnlineMenu && !g.job && g.boxUp == Tag::None && g.boxes.empty() && g.askAddress.empty() &&
           g.calmFrames >= kSettleFrames;
}

bool WaitForNetwork(int timeoutMs)
{
    if (!Enabled()) return false;
    auto connected = [] {
        const std::shared_ptr<community::Client> server = net::Community();
        return server && server->Connected();
    };
    if (connected()) return true;
    std::unique_lock l(g_waitMutex);
    const uint64_t serial = g_settled;
    g_wanted = true;                                  // the main thread (Frame) takes it from here
    g_waitCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return g_settled != serial; });
    l.unlock();
    return connected();
}

void KeyHints(PPCContext& ctx, uint8_t* base)
{
    if (!g.listUp || !g.drawn) return;
    const Entry* e = Focused(ctx, base);
    if (!e) return;
    if (e->kind != Entry::Kind::Add) menu_host::KeyHint(ctx, base, kHintY, "SERVER INFO");
    if (e->kind == Entry::Kind::Saved) menu_host::KeyHint(ctx, base, kHintX, "REMOVE");
}

} // namespace server_ui
