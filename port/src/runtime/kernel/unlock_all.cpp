// UNLOCK ALL (TEST 16, a tester's request): a button code on the main menu asks, in the game's own confirmation
// window, to unlock what the game's own rewards unlock and to fill the fight money. YES backs the profile's save folder
// up, then does it with the game's own grant and save calls.
//
// The code: LB RB LB RB LEFT RIGHT LEFT RIGHT X (keyboard by default Q W Q W, the arrow keys, Z), each press within
// 1.5 s of the one before, on the main menu (its state machine in state_mainmenu_main, menu frame FrameMainMenu) with a
// profile signed in and no box on screen.
//
// What it grants (MEASURED in the guest code and data 2026-11-01), each through sub_824117D0(the unlock manager,
// record), the call the game's main-menu entry makes for its bonus items:
//  - customization reward groups 0-162 but 74 (type 1, sub_827307D8: one item for each of the 63 characters). The
//    play-point tiers, character levels, Lucky Boxes and the mode clears grant these 162, group 37 included (Survival's
//    10 wins, sub_826510D0). No code grants group 74 (14 items, for 14 characters), so it stays out as paid or pre-order;
//  - the single reward items the game grants by number (type 0): 19076, 19080-19083, 2712, 2714, 2734, 2808, 2942-2944
//    (Fight Lab, sub_8240EDB8 / sub_8240ED10);
//  - the 90 "LED" entries the game grants by number (type 2, sub_824142C8 / sub_824146E8: 8-99 but 50 and 51);
//  - the character panels 66-125, 127-246 and 309-321 (type 3: the panel and its unlock-manager id, u16 at +8);
//  - the 13 print pattern categories (type 4, the play-point tiers' sub_82413F80), Customize's PRINT menu with them;
//  - unlock-manager ids 547 (every ending movie: the game's own reward at play-point tier 52), 329-331 (movies) and
//    322-325, the modes' first clears (type 5, u16 at +4).
// MEASURED 2026-11-04 by walking the game's own lists (sub_82414EE8 for the 52 tiers, sub_82415130 for every character
// and rank): of their 4,654 records nothing is left out but 56 empty slots (id -1) of the two fighters without items.
// Nothing DLC-bound: the ids the game sets from DLC (62-65, 332-546, 632, 639, 641) are put back at every main-menu
// entry anyway (sub_822AE060), and the title-storage switches 642/643 stay as the title server serves them.
// The fight money (the save object's +4) becomes 999,999,999 G, the game's own cap (addMoney 82709D60 caps there).
//
// Saved as the game saves after a match: sub_8265F628 (the save object; rank_sync's hook keeps a server's ranks out),
// then the unlock manager's dirty flag and sub_82411FF0. That runs at the start of the next scene update (rollback.cpp),
// on the game's main thread, as the game's own main-menu entry saves, once the gates hold again: the save manager idle,
// the customize manager's saves not pending, the unlock manager taking changes. Both calls only start the save: the
// save manager's thread writes it afterwards, in one container (MEASURED 2026-11-01: units 21, 14 and 01, ~10 ms after
// the calls returned). So the result waits for that container's close and the gates to hold again, and says so if
// the save failed: the save worker's "saving off" flag (its +1660, set by the save job sub_8258ADF8 when the player
// answers the game's Save Error box with CONTINUE WITHOUT SAVING, or when its checks fail; the job then stores the
// result 0, and every later save request is refused with 9 until the game restarts - MEASURED 2026-11-01 with
// read-only save files), its result (+3248: 0 saved, else the failure) or a failed create, flush or close of the
// container (kernel_content.cpp counts them). The unlocks then live only until the game quits. With saving already
// off, the code changes nothing and says why at once.
// From YES until the result box is on screen the game gets no buttons (at most 5 s, and none of it while a window of
// the game's own is open: its answer is the player's), or a press would go to the menu meanwhile (seen in the first
// test: A opened ONLINE MODE). The box itself opens only on the main menu, once it has settled with no window of the
// game's open (server_ui.cpp's rule: a box opened while one of the game's is up is replaced on screen by it, holding
// the controller - review 2026-11-01).
//
// The backup: <save root>\..\save_backups\<XUID>\<date and time>\, a copy of <save root>\<XUID> made just before
// (the last 5 kept), never inside the save folder (the save list would read it as a broken save). To undo: with the
// game closed, copy a backup's folders back over the profile's save folder.
#include "unlock_all.h"
#include "log_utf8.h"
#include "profiles.h"
#include "save_store.h"
#include "sys_ui.h"
#include "menu_host.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

PPC_EXTERN_FUNC(sub_824117D0);   // grant a reward record (the unlock manager, &record)
PPC_EXTERN_FUNC(sub_82276D38);   // the unlock manager (built on first use)
PPC_EXTERN_FUNC(sub_82411FF0);   // the unlock manager's save, when dirty
PPC_EXTERN_FUNC(sub_8265F628);   // the save object's save (rank_sync.cpp hooks it)
PPC_EXTERN_FUNC(sub_8265F5D8);   // the save object, from its holder
PPC_EXTERN_FUNC(sub_822547D0);   // the save manager
PPC_EXTERN_FUNC(sub_82588238);   // the save manager runs a save (its worker's +1656 == 2; 1 = a load, 0 = idle)

bool SaveInProgress();               // rollback.cpp: a save container is open
uint64_t SaveContainersClosed();     // rollback.cpp: save containers closed so far
uint64_t SaveFailures();             // kernel_content.cpp: save containers that failed to open, flush or close

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kSaveHolder = 0x82E70F04;     // the save object's holder (flag at +16)
constexpr uint32_t kCustomize = 0x82E6B6FC;      // the customize manager (+8 its unlock data, +20 / +28 saves pending)
constexpr uint32_t kMoneyCap = 999999999;
constexpr uint16_t kLB = 0x0100, kRB = 0x0200, kLeft = 0x0004, kRight = 0x0008, kX = 0x4000;
constexpr uint16_t kCode[] = { kLB, kRB, kLB, kRB, kLeft, kRight, kLeft, kRight, kX };
constexpr const char* kMainState = "&MainMenuMode::Impl::state_mainmenu_main";
constexpr int kSettleFrames = 20;                // the main menu calm this long before the result box (server_ui.cpp's)
constexpr int64_t kHoldMs = 5000;                // the game's buttons held back after YES for at most this long
constexpr const char* kSavingOffText = "UNLOCK ALL was not done: saving is off\n(an earlier save failed and the game went\n"
                                       "on without saving). Restart the game,\nthen try again.";

std::atomic<uint32_t> g_menuMachine{ 0 };
std::atomic<bool> g_menuMain{ false };           // the main menu's machine sits in its main state
std::atomic<bool> g_pending{ false };            // YES: apply at the next scene update
std::atomic<bool> g_busy{ false };               // from YES until the result is on screen: no code
std::atomic<uint64_t> g_pendingXuid{ 0 };
std::atomic<int64_t> g_holdSince{ -1 };          // YES (ms): the game gets no buttons until the result box is answered
std::atomic<bool> g_gameWindow{ false };         // a window of the game's own is open (Frame): the hold lets go

// After YES (Update, the game's main thread): the save, then the result for Frame to show.
enum class Stage { Idle, Saving };
Stage g_stage = Stage::Idle;
Clock::time_point g_stageSince;
Clock::time_point g_waitFrom;                    // the save's 20 s, counted while no box waits for the player
uint64_t g_closedBefore = 0;                     // save containers closed before the save started
uint64_t g_failuresBefore = 0;                   // and failed
int g_quietUpdates = 0;                          // scene updates in a row with the save done and nothing pending
std::string g_report, g_backupWhere;             // the result; the backups' folder for it ("TTT2Recomp\save_backups")
bool g_reportReady = false;                      // the result waits for the settled main menu (Frame)
int g_calmFrames = 0;

int64_t Ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

struct GuestStack
{
    PPCContext& ctx;
    uint32_t saved;
    GuestStack(PPCContext& c, uint8_t* base, uint32_t bytes) : ctx(c), saved(c.r1.u32)
    {
        const uint32_t sp = (saved - bytes) & ~0xFu;
        GWrite32(base, sp, saved);
        ctx.r1.u32 = sp;
    }
    ~GuestStack() { ctx.r1.u32 = saved; }
};

// Bits set in [from, from + bytes).
int Bits(uint8_t* base, uint32_t from, uint32_t bytes)
{
    int n = 0;
    for (uint32_t i = 0; i < bytes; ++i) n += std::popcount(unsigned(GRead8(base, from + i)));
    return n;
}

uint32_t LiveSave(PPCContext& ctx, uint8_t* base)
{
    if (!(GRead32(base, kSaveHolder + 16) & 1)) return 0;
    ctx.r3.u64 = kSaveHolder;
    sub_8265F5D8(ctx, base);
    return ctx.r3.u32;
}

bool SaveManagerIdle(PPCContext& ctx, uint8_t* base)
{
    sub_822547D0(ctx, base);
    if (!ctx.r3.u32) return false;
    sub_82588238(ctx, base);
    return ctx.r3.u32 == 0;
}

uint32_t SaveWorker(PPCContext& ctx, uint8_t* base)
{
    sub_822547D0(ctx, base);
    return ctx.r3.u32 ? GRead32(base, ctx.r3.u32 + 8) : 0;
}

// The save manager's last result: its worker's +3248 (sub_825882F8 / sub_82588CC0), set by the save job sub_8258ADF8:
// 0 when its save is written or given up (SavingOff), else the failure (the mount's error, 1, 8 or 9).
uint32_t SaveResult(PPCContext& ctx, uint8_t* base)
{
    const uint32_t worker = SaveWorker(ctx, base);
    return worker ? GRead32(base, worker + 3248) : 0;
}

// The game's saving is off (the worker's +1660, read by sub_82588628): a save failed and the player went on without
// saving, or the save job's checks failed. Save and load requests (sub_8258B598 / sub_8258B2F0) are refused (9) until
// the game restarts.
bool SavingOff(PPCContext& ctx, uint8_t* base)
{
    const uint32_t worker = SaveWorker(ctx, base);
    return worker && GRead8(base, worker + 1660);
}

// Nothing saving and changes taken: the customize manager's saves not pending, the unlock manager not refusing, the
// save manager idle.
bool Quiet(PPCContext& ctx, uint8_t* base, uint32_t um, uint32_t customize)
{
    return !GRead32(base, customize + 20) && !GRead32(base, customize + 28) && !(GRead8(base, um + 391) & 0x80) &&
           SaveManagerIdle(ctx, base);
}

// <save root>\<XUID> copied to <save root>\..\save_backups\<XUID>\<date and time>; it and the 4 newest others kept.
// Empty on failure, with nothing left behind.
std::filesystem::path Backup(uint64_t xuid, std::string& error)
{
    namespace fs = std::filesystem;
    fs::path root;
    if (const uint32_t e = SaveUserRoot(root)) { error = "no save folder (error " + std::to_string(e) + ")"; return {}; }
    char xuidText[17];
    std::snprintf(xuidText, sizeof(xuidText), "%016llX", static_cast<unsigned long long>(xuid));
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
    const fs::path backups = root.parent_path().parent_path() / "save_backups" / xuidText;
    const fs::path to = backups / stamp;
    std::error_code ec;
    // Not made: the folder is there already, an earlier backup from the same second, never to be copied into.
    const bool made = fs::create_directories(to, ec);
    if (!ec && !made) ec.assign(183, std::system_category());   // ERROR_ALREADY_EXISTS
    if (!ec && fs::exists(root, ec)) fs::copy(root, to, fs::copy_options::recursive, ec);
    // The Win32 code, as above: ec.message() is Windows' own text in the ANSI code page (not UTF-8) on a PC without
    // English messages.
    if (ec)
    {
        // A failed copy is no backup, and the pruning below would count it as one of the 5 kept (MEASURED 2026-11-02:
        // a save file held open by another program left part of a copy, and the next backup deleted a real one).
        std::error_code ignored;
        if (made) fs::remove_all(to, ignored);
        error = "error " + std::to_string(ec.value());
        return {};
    }
    // Only the folders named like `stamp` are pruned: a folder the player put there is never deleted. The names are
    // read wide (filename().string() throws at a name outside the ANSI code page, log_utf8.h).
    auto stamped = [](const std::wstring& n) {
        if (n.size() != 19) return false;
        for (size_t i = 0; i < n.size(); ++i)
        {
            const wchar_t sign = (i == 4 || i == 7 || i == 13 || i == 16) ? L'-' : i == 10 ? L'_' : 0;
            if (sign ? n[i] != sign : (n[i] < L'0' || n[i] > L'9')) return false;
        }
        return true;
    };
    std::vector<fs::path> all;
    for (auto it = fs::directory_iterator(backups, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->path() != to && stamped(it->path().filename().wstring()) && it->is_directory(ec)) all.push_back(it->path());
    // Oldest name first. The new backup is never pruned: with the clock set back it would sort first and go at once.
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i + 4 < all.size(); ++i) fs::remove_all(all[i], ec);
    return to;
}

// true: granted and the save started; `report` is the result box's text either way.
bool Apply(PPCContext& ctx, uint8_t* base, std::string& report)
{
    const uint64_t xuid = g_pendingXuid.load();
    std::string refusal;
    sub_82276D38(ctx, base);
    const uint32_t um = ctx.r3.u32, customize = GRead32(base, kCustomize);
    const uint32_t save = LiveSave(ctx, base);
    if (!g_menuMain.load() || !profiles::SignedIn() || profiles::ActiveXuid() != xuid) refusal = "the main menu was left";
    else if (!um || !customize || !save) refusal = "the save data is not loaded";
    else if (!Quiet(ctx, base, um, customize)) refusal = "the game is saving";
    else if (SavingOff(ctx, base))
    {
        std::fprintf(stderr, "[unlock-all] not done: the game's saving is off (an earlier save failed)\n");
        report = kSavingOffText;
        return false;
    }
    if (!refusal.empty())
    {
        std::fprintf(stderr, "[unlock-all] not now: %s\n", refusal.c_str());
        report = "UNLOCK ALL was not done:\n" + refusal + ".\nTry again on the main menu in a moment.";
        return false;
    }
    std::string error;
    const std::filesystem::path backup = Backup(xuid, error);
    if (backup.empty())
    {
        std::fprintf(stderr, "[unlock-all] no backup (%s): nothing changed\n", error.c_str());
        report = "UNLOCK ALL was not done:\nthe save could not be backed up.";
        return false;
    }
    const uint32_t unlocks = GRead32(base, customize + 8);
    const int itemsBefore = Bits(base, unlocks + 24, 2500), ledsBefore = Bits(base, unlocks + 5056, 13);
    const int entriesBefore = Bits(base, um + 4, 387);
    {
        GuestStack frame(ctx, base, 0x100);
        const uint32_t record = ctx.r1.u32 + 80;
        const auto grant = [&](uint32_t type, uint32_t id) {
            GWrite32(base, record + 0, type);
            GWrite32(base, record + 4, type == 5 ? id << 16 : id);   // type 5: a u16 at +4 (big-endian: the high half)
            GWrite32(base, record + 8, type == 3 ? id << 16 : 0);    // type 3: a u16 at +8
            ctx.r3.u64 = um;
            ctx.r4.u64 = record;
            sub_824117D0(ctx, base);
        };
        for (uint32_t group = 0; group <= 162; ++group)
            if (group != 74) grant(1, group);
        for (uint32_t item : { 19076u, 19080u, 19081u, 19082u, 19083u, 2712u, 2714u, 2734u, 2808u, 2942u, 2943u, 2944u }) grant(0, item);
        for (uint32_t led = 8; led <= 99; ++led)
            if (led != 50 && led != 51) grant(2, led);
        for (uint32_t panel = 66; panel <= 321; ++panel)
            if (panel != 126 && (panel <= 246 || panel >= 309)) grant(3, panel);
        for (uint32_t category = 0; category < 13; ++category) grant(4, category);
        for (uint32_t id : { 547u, 329u, 330u, 331u, 322u, 323u, 324u, 325u }) grant(5, id);
    }
    const uint32_t moneyBefore = GRead32(base, save + 4);
    GWrite32(base, save + 4, kMoneyCap);
    const int items = Bits(base, unlocks + 24, 2500) - itemsBefore, leds = Bits(base, unlocks + 5056, 13) - ledsBefore;
    const int entries = Bits(base, um + 4, 387) - entriesBefore;
    // Saved as after a match: the save object, then the unlock data. Both only start it (Update waits for the end).
    ctx.r3.u64 = kSaveHolder;
    sub_8265F628(ctx, base);
    GWrite32(base, um + 392, 1);
    ctx.r3.u64 = um;
    sub_82411FF0(ctx, base);
    std::fprintf(stderr, "[unlock-all] granted for %016llX: %d customization items, %d LED entries, unlock-manager bits %+d; "
                         "fight money %u -> %u G; backup %s; saving\n",
                 static_cast<unsigned long long>(xuid), items, leds, entries, moneyBefore, kMoneyCap, LogUtf8(backup).c_str());
    const std::filesystem::path backups = backup.parent_path().parent_path();   // <...>\TTT2Recomp\save_backups
    g_backupWhere = LogUtf8(backups.parent_path().filename()) + "\\" + LogUtf8(backups.filename());   // UTF-8, as the box's text
    report = "Everything is unlocked and saved:\n" + std::to_string(items) + " new customization items, LEDs,\n"
             "prints, panels, ending movies, 999,999,999 G.\nYour old save is backed up in\n" + g_backupWhere + ".";
    return true;
}

// The result is ready: Frame shows it once the main menu has settled.
void Report()
{
    g_stage = Stage::Idle;
    g_calmFrames = 0;
    g_reportReady = true;
}

void EndReport()
{
    g_reportReady = false;
    g_busy = false;
    g_holdSince = -1;
}

// Frame: the result box on the main menu once it has settled with no window of the game's open for kSettleFrames
// frames; dropped if another profile signed in.
void ShowReport(PPCContext& ctx, uint8_t* base)
{
    if (!profiles::SignedIn() || profiles::ActiveXuid() != g_pendingXuid.load())
    {
        std::fprintf(stderr, "[unlock-all] the profile changed: the result is not shown\n");
        EndReport();
        return;
    }
    const bool calm = g_menuMain.load() && !SysUiBoxBusy() && !menu_host::WindowShowing(ctx, base) &&
                      menu_host::CurrentFrame(ctx, base) == "FrameMainMenu";
    g_calmFrames = calm ? (std::min)(g_calmFrames + 1, kSettleFrames) : 0;
    if (g_calmFrames < kSettleFrames) return;
    SysUiMessageBox box;
    box.text = g_report;
    box.buttons = { "OK" };
    box.cancel = 0;
    box.done = [](int) { g_holdSince = -1; };   // the box held the controller until now
    if (!SysUiShowMessageBox(std::move(box))) return;
    std::fprintf(stderr, "[unlock-all] result shown\n");
    g_reportReady = false;
    g_busy = false;
}

} // namespace

namespace unlock_all {

// Every state a game state machine enters (the sub_825FE600 hook, online_2v2_team.cpp).
void NoteState(uint32_t machine, const std::string& name)
{
    if (name == kMainState)
    {
        g_menuMachine = machine;
        g_menuMain = true;
    }
    else if (machine == g_menuMachine.load())
        g_menuMain = false;
}

// Player 1's buttons the game gets (kernel_xam.cpp): none while the hold after YES lasts.
uint16_t GameButtonMask()
{
    const int64_t since = g_holdSince.load();
    return since >= 0 && Ms() - since < kHoldMs && !g_gameWindow.load() ? 0 : 0xFFFF;
}

// Once per frame on the game's main thread (menu_ext.cpp's UI hook): the button code, and the result box.
void Frame(PPCContext& ctx, uint8_t* base)
{
    static uint16_t last = 0;
    static size_t at = 0;
    static Clock::time_point lastPress;
    const uint16_t now = HostControllerButtons();
    const uint16_t pressed = uint16_t(now & ~last);
    last = now;
    if (const int64_t since = g_holdSince.load(); since >= 0)
    {
        if (Ms() - since < kHoldMs) g_gameWindow = menu_host::WindowShowing(ctx, base);
        else g_holdSince = -1;
    }
    if (g_reportReady) ShowReport(ctx, base);
    if (!g_menuMain.load() || g_busy.load() || SysUiOwnsInput() || SysUiBoxBusy() || !profiles::SignedIn())
    {
        at = 0;
        return;
    }
    if (at && Clock::now() - lastPress > std::chrono::milliseconds(1500)) at = 0;
    if (!pressed) return;
    for (uint16_t bit = 1; bit; bit = uint16_t(bit << 1))
    {
        if (!(pressed & bit)) continue;
        at = bit == kCode[at] ? at + 1 : bit == kCode[0] ? 1 : 0;
        lastPress = Clock::now();
        if (at < std::size(kCode)) continue;
        at = 0;
        if (menu_host::CurrentFrame(ctx, base) != "FrameMainMenu") return;
        const uint64_t xuid = profiles::ActiveXuid();
        std::fprintf(stderr, "[unlock-all] code entered on the main menu (profile %016llX)\n", static_cast<unsigned long long>(xuid));
        if (SavingOff(ctx, base))   // nothing could be kept: say so at once (Apply checks again)
        {
            std::fprintf(stderr, "[unlock-all] not done: the game's saving is off (an earlier save failed)\n");
            SysUiMessageBox off;
            off.text = kSavingOffText;
            off.buttons = { "OK" };
            off.cancel = 0;
            SysUiShowMessageBox(std::move(off));
            return;
        }
        SysUiMessageBox box;
        box.text = "UNLOCK ALL\nUnlock every customization item, LED, print,\ncharacter panel and ending movie, and fill\n"
                   "your fight money to 999,999,999 G?\nYour save data is backed up first.";
        box.buttons = { "YES", "NO" };
        box.active = 1;
        box.cancel = 1;
        box.done = [xuid](int choice) {
            std::fprintf(stderr, "[unlock-all] %s\n", choice == 0 ? "YES: at the next frame" : "NO: nothing changed");
            if (choice != 0) return;
            g_pendingXuid = xuid;
            g_busy = true;
            g_holdSince = Ms();
            g_pending = true;
        };
        SysUiShowMessageBox(std::move(box));
        return;
    }
}

// The start of each scene update (rollback.cpp), on the game's main thread: a YES is applied here, then its save is
// waited for and the result handed to Frame.
void Update(PPCContext& ctx, uint8_t* base)
{
    if (g_stage == Stage::Idle && !g_pending.load()) return;
    const PPCContext entry = ctx;
    const auto since = [] { return std::chrono::duration<double, std::milli>(Clock::now() - g_stageSince).count(); };
    const auto notSaved = [] {
        return "Everything is unlocked, but the game could\nnot save it: it may be lost when you quit.\n"
               "Your old save is backed up in\n" + g_backupWhere + ".";
    };
    if (g_pending.exchange(false))
    {
        g_closedBefore = SaveContainersClosed();
        g_failuresBefore = SaveFailures();
        if (Apply(ctx, base, g_report))
        {
            g_stage = Stage::Saving;
            g_stageSince = g_waitFrom = Clock::now();
            g_quietUpdates = 0;
        }
        else
            Report();
    }
    else if (g_stage == Stage::Saving)
    {
        // Done: the save's container closed, none open, and the gates hold again, for 5 scene updates in a row.
        sub_82276D38(ctx, base);
        const uint32_t um = ctx.r3.u32, customize = GRead32(base, kCustomize);
        const bool done = SaveContainersClosed() > g_closedBefore && !SaveInProgress() && um && customize &&
                          Quiet(ctx, base, um, customize);
        const uint64_t failed = SaveFailures() - g_failuresBefore;
        g_quietUpdates = done ? g_quietUpdates + 1 : 0;
        if (SysUiBoxBusy()) g_waitFrom = Clock::now();   // the game's Save Error box: the player's time
        if (g_quietUpdates >= 5)
        {
            const uint32_t result = SaveResult(ctx, base);
            if (const bool off = SavingOff(ctx, base); off || result || failed)
            {
                std::fprintf(stderr, "[unlock-all] the save FAILED (saving off %d, the save manager's result %u; %llu "
                                     "failed creates, flushes or closes; %.0f ms)\n",
                             int(off), result, static_cast<unsigned long long>(failed), since());
                g_report = notSaved();
            }
            else
                std::fprintf(stderr, "[unlock-all] saved (%.0f ms)\n", since());
            Report();
        }
        else if (std::chrono::duration<double, std::milli>(Clock::now() - g_waitFrom).count() > 20000)
        {
            const bool off = SavingOff(ctx, base);
            std::fprintf(stderr, "[unlock-all] the save has not ended after 20 s (saving off %d; %llu failed creates, "
                                 "flushes or closes)\n", int(off), static_cast<unsigned long long>(failed));
            g_report = off || failed ? notSaved()
                                     : "Everything is unlocked, but the game is\nstill saving it. Leave the game running\n"
                                       "a little longer before you quit.";
            Report();
        }
    }
    ctx = entry;
}

} // namespace unlock_all
