#include <SDL3/SDL.h>
// PC menu extensions (plan and reverse-engineering notes: port/MENU_PLAN.md).
//
// TTT2's menus are BX01 binary-XML documents (frames, menus, rows, value lists) whose event
// handlers are precompiled Xtal scripts calling `lib::` natives. This module
//  - edits the Options / main-menu document as the game loads it (menu_doc.cpp): DISPLAY MODE
//    and RESOLUTION rows in Display Settings, EXIT in the main menu with its confirmation;
//  - registers the natives those rows and the dialog call, next to the game's own options natives;
//  - initialises the new rows' value cursors whenever Display Settings initialises MOTION BLUR's;
//  - supplies the confirmation's text and YES / NO labels, and quits on YES;
//  - Controller Setup's KEYBOARD mode: player 1's keyboard keys shown and changed in the button
//    panel (keyui below, keyboard_keys.h).
// It also hosts the system UI (sys_ui.h: Select Profile and the new profile's name dialog).
// Diagnostics: TTT2_MENU_DUMP=1 writes every BX01 document (and the edited one) to
// C:/ttt2/port/menu_bin/; TTT2_TEXT_TRACE=1 logs each text id the game looks up (first time, with
// the caller). Development: TTT2_MENU_OVERRIDE=1 loads a document prepared with
// port/tools/menu_proto.py from C:/ttt2/port/menu_override/<fnv1a>.bin instead of editing.
#include "kernel_shared.h"
#include "guest_heap.h"
#include "display_settings.h"
#include "menu_doc.h"
#include "sys_ui.h"
#include "profiles.h"
#include "keyboard_keys.h"
#include "cheat_overlay.h"
#include "persistent_unlocks.h"
void DisplayChangeGuardFrame();
#include "host_keycodes.h"
#include "host_settings.h"
#include "host_input_sdl.h"
#include "../host_process.h"
#include "menu_host.h"
#include "server_ui.h"
#include "gin_service.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_823749C0);
extern "C" PPC_FUNC(__imp__sub_82346060);
extern "C" PPC_FUNC(__imp__sub_826002C8);
extern "C" PPC_FUNC(__imp__sub_82A8C170);
extern "C" PPC_FUNC(__imp__sub_823756A8);
extern "C" PPC_FUNC(__imp__sub_8256CD28);
extern "C" PPC_FUNC(__imp__sub_8237F5D0);
extern "C" PPC_FUNC(__imp__sub_8241E7A8);
extern "C" PPC_FUNC(__imp__sub_82623858);
extern "C" PPC_FUNC(__imp__sub_8241B9D0);
extern "C" PPC_FUNC(__imp__sub_8256C708);
extern "C" PPC_FUNC(__imp__sub_82374700);
extern "C" PPC_FUNC(__imp__sub_823747C0);
extern "C" PPC_FUNC(__imp__sub_8241F988);
extern "C" PPC_FUNC(__imp__sub_8241B588);
extern void (*g_uiFrameHook)(PPCContext& ctx, uint8_t* base);
void XamNotifyEnqueueEvent(uint32_t id, uint32_t param);

namespace {
bool EnvFlag(const char* name)
{
    const char* e = std::getenv(name);
    return e && *e == '1';
}
bool MenuDump() { static const bool on = EnvFlag("TTT2_MENU_DUMP"); return on; }
bool MenuOverride() { static const bool on = EnvFlag("TTT2_MENU_OVERRIDE"); return on; }

uint32_t Fnv1a(const uint8_t* p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

void SaveFile(const char* path, const void* data, size_t size)
{
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(data, 1, size, f); std::fclose(f); }
}

// A copy of `bytes` in never-freed guest memory (`prefix` zero bytes in front); guest address.
uint32_t GuestCopy(uint8_t* base, const void* bytes, size_t size, uint32_t prefix = 0)
{
    auto* host = static_cast<uint8_t*>(g_virtualHeap.Alloc(prefix + size + 16));
    std::memset(host, 0, prefix);
    std::memcpy(host + prefix, bytes, size);
    return static_cast<uint32_t>(host + prefix - base);
}
uint32_t GuestCString(uint8_t* base, const char* text, uint32_t prefix = 0)
{
    return GuestCopy(base, text, std::strlen(text) + 1, prefix);
}

// A string as the game's text lookup returns it: data 4 bytes into a table entry whose last
// header byte is the length.
uint32_t TableText(uint8_t* base, const char* text)
{
    const uint32_t addr = GuestCString(base, text, 4);
    GWrite8(base, addr - 1, static_cast<uint8_t>(std::strlen(text)));
    return addr;
}

std::string GuestString(uint8_t* base, uint32_t addr, size_t max = 260)
{
    std::string s;
    for (size_t i = 0; addr && i < max && base[addr + i]; ++i) s.push_back(static_cast<char>(base[addr + i]));
    return s;
}

// A scratch frame below the current guest stack pointer for calling guest functions from a
// hook; restores r1 on destruction. The ABI back chain is written.
struct GuestFrame
{
    PPCContext& ctx;
    uint32_t saved;
    GuestFrame(PPCContext& c, uint8_t* base, uint32_t bytes) : ctx(c), saved(c.r1.u32)
    {
        const uint32_t sp = (saved - bytes) & ~0xFu;
        GWrite32(base, sp, saved);
        ctx.r1.u32 = sp;
    }
    ~GuestFrame() { ctx.r1.u32 = saved; }
    uint32_t sp() const { return ctx.r1.u32; }
};

// Menu widget by name (sub_8256C160, as lib::MenuSelectValueSetCursor looks it up) or 0.
uint32_t FindWidget(PPCContext& ctx, uint8_t* base, uint32_t nameAddr)
{
    ctx.r3.u64 = nameAddr;
    sub_8256C160(ctx, base);
    return ctx.r3.u32;
}

// lib::MenuSwitchFrame(name) (impl sub_8256BF88: name is a C string).
void SwitchFrame(PPCContext& ctx, uint8_t* base, const char* name)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestCString(base, name);
    sub_8256BF88(ctx, base);
}

// ---- system UI (sys_ui.h, PROFILE_PLAN.md) ---------------------------------------------------
// The console-side screens. Select Profile opens when START / A is pressed on the title logo
// while nobody is signed in, drawn with FrameSysMenu<N> (menu_doc.cpp: N = 2..9 rows in Display
// Settings' style; the list uses the frame with exactly its row count); a new profile is named in
// the game's confirmation window. Everything here runs on the game's main thread at the end of
// VdSwap. Two-argument menu natives take the value first and the widget name second.

// Guest copies of strings, one per distinct text (texts change only on input; never freed).
uint32_t GuestStr(uint8_t* base, const std::string& s)
{
    static std::map<std::string, uint32_t> pool;
    const auto it = pool.find(s);
    if (it != pool.end()) return it->second;
    const uint32_t addr = GuestCString(base, s.c_str());
    pool.emplace(s, addr);
    return addr;
}
bool g_hostText = false;                              // MenuSetText is writing (the hook passes it on)
void MenuSetText(PPCContext& ctx, uint8_t* base, const std::string& widget, const std::string& text)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, text);
    ctx.r4.u64 = GuestStr(base, widget);
    g_hostText = true;
    sub_8256C708(ctx, base);                          // lib::MenuEntitySetText(text, widget)
    g_hostText = false;
}
void MenuMovieVisible(PPCContext& ctx, uint8_t* base, const std::string& movie, bool visible)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, movie);
    ctx.r4.u64 = visible ? 1 : 0;
    sub_8256D518(ctx, base);                          // lib::MenuIFLumenSetMovieVisible(movie, bool)
}
void MenuTelopTarget(PPCContext& ctx, uint8_t* base, const std::string& widget)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, widget);
    sub_825719A8(ctx, base);                          // lib::MenuTelopSetTargetEntity(widget)
}
void MenuSetCursor(PPCContext& ctx, uint8_t* base, const std::string& menu, int index)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = static_cast<uint32_t>(index);
    ctx.r4.u64 = GuestStr(base, menu);
    sub_8256C928(ctx, base);                          // lib::MenuContainerSetCursor(index, menu)
}

void MenuMoviePlay(PPCContext& ctx, uint8_t* base, const std::string& movie, const std::string& label, bool force)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, movie);
    ctx.r4.u64 = GuestStr(base, label);
    ctx.r5.u64 = force ? 1 : 0;
    sub_8256D5C8(ctx, base);                          // lib::MenuIFLumenMoviePlayFromLabelAndStop
}
void MenuTextHide(PPCContext& ctx, uint8_t* base, const std::string& widget, bool hide)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = hide ? 1 : 0;
    ctx.r4.u64 = GuestStr(base, widget);
    sub_8256C758(ctx, base);                          // lib::MenuEntitySetTextHide(bool, widget)
}
void MenuTickCount(PPCContext& ctx, uint8_t* base, const std::string& widget, uint32_t count)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = count;
    ctx.r4.u64 = GuestStr(base, widget);
    sub_8256C2B0(ctx, base);                          // lib::MenuSetTickCount(count, widget)
}
// lib::MenuSoundKeyOn(id) with the menus' SoundIdMenuCursor / Decide / Cancel (natives defined
// by sub_825A5098).
constexpr uint32_t kSoundCursor = 8426, kSoundCancel = 8427, kSoundDecide = 8428;
void MenuSound(PPCContext& ctx, uint8_t* base, uint32_t id)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = id;
    sub_8259E948(ctx, base);
}

namespace sysui {

enum class Screen { None, Profiles, Name };
constexpr int kRows = 9;
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kStart = 0x0010, kA = 0x1000, kB = 0x2000, kY = 0x8000;
constexpr uint32_t kNameTextId = 0x7F02;              // the name dialog's message (text lookup hook)

struct Row { std::string label, value, help; };
struct Ui
{
    Screen screen = Screen::None;
    int setupFrames = 0;            // frames left before the rows are written (after a switch)
    bool releasing = false;         // closed: keep the controller until every button is up
    bool continueAfter = false;     // then press START for the game (the title continues)
    int cursor = 0;
    std::vector<Row> rows, shown;   // wanted / last written
    int shownCursor = -1;
    int frameRows = 0;              // FrameSysMenu<N> on screen (0 = none yet)
    int settle = 0;                 // frames until the cursor is drawn again (after the menu's OnFocus)
    std::string name, note;         // the name dialog
    bool suggested = false;         // `name` is the suggestion: the first typed character replaces it
    uint16_t prev = 0, repeatKey = 0;
    int64_t repeatAt = 0;
};
Ui g_ui;
std::atomic<bool> g_owns{ false }, g_textEntry{ false }, g_signInRequest{ false };
std::atomic<bool> g_switchProfile{ false };   // Options -> SWITCH PROFILE confirmed: sign out on the logo
std::atomic<int64_t> g_titleLogoMs{ 0 };
std::mutex g_charMutex;
std::vector<uint32_t> g_chars;
// The name dialog's message: a guest buffer the text lookup returns for kNameTextId, and the
// window's text block it was laid out into (captured in sub_8241B9D0), laid out again as the name
// changes.
uint32_t g_nameText = 0;
std::atomic<uint32_t> g_nameBlock{ 0 }, g_nameFont{ 0 }, g_nameSize{ 0 };
// Message boxes (XamShowMessageBoxUI, kernel_ui.cpp): the queued one, the one on screen, its
// message (kBoxTextId, laid out again into g_boxBlock as the listed choice moves) and - while a
// window is being opened - its button labels for the lookups of the window's OK / CANCEL texts.
constexpr uint32_t kBoxTextId = 0x7F03;
enum class BoxStyle { Information, Confirmation, List };
std::mutex g_boxMutex;
std::vector<SysUiMessageBox> g_boxQueue;
std::vector<SysUiTextBox> g_textQueue;      // text boxes (SysUiShowTextBox) waiting
struct BoxState
{
    bool showing = false, releasing = false;
    SysUiMessageBox box;
    BoxStyle style = BoxStyle::Information;
    int cursor = 0;
    uint16_t prev = 0;
    bool typing = false;                    // the box is a text box: `edit` and its note
    SysUiTextBox edit;
    std::string note;
    int upFrames = 0, goneFrames = 0;       // frames on screen; frames in a row the game's window was closed
} g_box;
std::atomic<bool> g_boxOwns{ false }, g_boxDismiss{ false };
uint32_t g_boxText = 0;
std::atomic<uint32_t> g_boxBlock{ 0 }, g_boxFont{ 0 }, g_boxSize{ 0 };
std::mutex g_labelMutex;
std::vector<uint32_t> g_labels;             // guest texts, set only around the window's opening

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string Two(int i) { return std::string(i < 10 ? "0" : "") + std::to_string(i); }
bool TitleLogo() { return NowMs() - g_titleLogoMs.load() < 150; }

void Notify(uint32_t id, uint32_t param) { XamNotifyEnqueueEvent(id, param); }

void BuildProfiles(Ui& ui)
{
    ui.rows.clear();
    for (const profiles::Profile& p : profiles::List())
        ui.rows.push_back({ p.name, "", "Play as " + p.name + ". Y: rename." });
    if (ui.rows.size() < profiles::kMaxProfiles)
        ui.rows.push_back({ "CREATE NEW PROFILE", "", "Make a new profile with its own save data." });
}

// "PLAYER <n>": the first free number from the profile count up.
std::string SuggestedName()
{
    const std::vector<profiles::Profile> list = profiles::List();
    for (size_t n = list.size() + 1;; ++n)
    {
        const std::string name = "PLAYER " + std::to_string(n);
        bool used = false;
        for (const profiles::Profile& p : list) used |= p.name.size() == name.size() && std::equal(
            p.name.begin(), p.name.end(), name.begin(),
            [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
        if (!used) return name;
    }
}

// ---- the name dialog: the game's confirmation window (OK / CANCEL), answered from the host
std::string NameMessage(const Ui& ui)
{
    std::string hint = ui.note;
    if (hint.empty())
        hint = ui.suggested ? "Type a name, or confirm this one." : "ENTER: confirm   ESC: cancel";
    return "Name your new profile.\n\n" + ui.name + (ui.suggested ? "" : "_") + "\n\n" + hint;
}
// A text the lookup hook returns: a fixed guest buffer (allocated on first use) holding `text` as
// a text-table entry (length byte in front).
void WriteTableText(uint8_t* base, uint32_t& buffer, size_t capacity, const std::string& text)
{
    if (!buffer)
    {
        const std::vector<char> zeros(capacity + 1, 0);
        buffer = GuestCopy(base, zeros.data(), zeros.size(), 4);
    }
    const size_t n = (std::min)(text.size(), capacity);
    std::memcpy(base + buffer, text.data(), n);
    base[buffer + n] = 0;
    GWrite8(base, buffer - 1, static_cast<uint8_t>((std::min<size_t>)(n, 255)));
}
void WriteNameText(uint8_t* base, const std::string& text) { WriteTableText(base, g_nameText, 480, text); }
void ShowNameDialog(PPCContext& ctx, uint8_t* base, const Ui& ui)
{
    WriteNameText(base, NameMessage(ui));
    g_nameBlock = 0;
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = 0;
    ctx.r4.u64 = kNameTextId;
    ctx.r5.u64 = 0;
    sub_8241FAF0(ctx, base);                          // lib::WindowShowConfirmation(text id)
}
void UpdateNameDialog(PPCContext& ctx, uint8_t* base, const Ui& ui)
{
    WriteNameText(base, NameMessage(ui));
    const uint32_t block = g_nameBlock.load();
    if (!block) return;
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = block;
    ctx.r4.u64 = g_nameFont.load();
    ctx.r5.u64 = g_nameSize.load();
    ctx.r6.u64 = g_nameText;
    sub_8241B9D0(ctx, base);                          // the window lays its message out again
}
void HideDialog(PPCContext& ctx, uint8_t* base)
{
    GuestFrame frame(ctx, base, 0x100);
    sub_8241E7C8(ctx, base);                          // lib::WindowHideDialog()
}

void Open(Ui& ui, int cursor)
{
    ui.screen = Screen::Profiles;
    ui.cursor = cursor;
    ui.setupFrames = 1;
    ui.shown.clear();
    ui.shownCursor = -1;
    g_textEntry = false;
    g_owns = true;
}
void Close(PPCContext& ctx, uint8_t* base, Ui& ui, bool continueAfter)
{
    if (ui.screen == Screen::Name) HideDialog(ctx, base);
    SwitchFrame(ctx, base, "FrameSysEmpty");
    ui.screen = Screen::None;
    ui.frameRows = 0;
    ui.releasing = true;
    ui.continueAfter = continueAfter;
    g_textEntry = false;
    Notify(9, 0);                                        // XN_SYS_UI closed
}
void SignInAndClose(PPCContext& ctx, uint8_t* base, Ui& ui, int index)
{
    profiles::SignIn(index);
    Notify(0x0000000Au, 1);                              // XN_SYS_SIGNINCHANGED, user 0
    Close(ctx, base, ui, true);                          // and the title takes START
}

void DrawCursor(PPCContext& ctx, uint8_t* base, int count, int cursor, bool force);
void Draw(PPCContext& ctx, uint8_t* base, Ui& ui)
{
    BuildProfiles(ui);
    const int count = std::clamp(int(ui.rows.size()), 2, kRows);
    ui.rows.resize(size_t(count), Row{ "-", "", "-" });
    ui.cursor = std::clamp(ui.cursor, 0, count - 1);
    if (count != ui.frameRows)
    {
        // The frame with exactly this many rows (its bars follow its icon arrays); write the
        // texts once it is up.
        SwitchFrame(ctx, base, ("FrameSysMenu" + std::to_string(count)).c_str());
        ui.frameRows = count;
        ui.shown.clear();
        ui.shownCursor = -1;
        ui.setupFrames = 2;
        return;
    }
    if (ui.shown.empty())
    {
        // As Display Settings' OnFocus does: no large icon (IFMainMenuL would show its first,
        // the globe), and the bottom bar scrolls the game's tips.
        MenuMovieVisible(ctx, base, "rmc_icon_menu_L", false);
        MenuTelopTarget(ctx, base, "StaticTelopSysMenu" + std::to_string(count));
    }
    const std::string n = std::to_string(count) + "_";
    for (int i = 0; i < count; ++i)
    {
        const Row& row = ui.rows[size_t(i)];
        const bool fresh = i >= int(ui.shown.size());
        const Row old = fresh ? Row{} : ui.shown[size_t(i)];
        const std::string k = n + Two(i + 1);
        if (fresh || old.label != row.label) MenuSetText(ctx, base, "MenuItemSysMenu" + k, row.label);
        if (fresh || old.value != row.value) MenuSetText(ctx, base, "StaticValueSysMenu" + k, row.value);
        if (fresh || old.help != row.help) MenuSetText(ctx, base, "StaticSysMenu" + k, row.help);
    }
    if (ui.shown.empty()) ui.settle = 8;
    ui.shown = ui.rows;
    if (ui.cursor != ui.shownCursor)
    {
        DrawCursor(ctx, base, count, ui.cursor, ui.shownCursor < 0);
        ui.shownCursor = ui.cursor;
    }
}

// The cursor as the interface script global::IFMainMenuL_Cursor / _CursorSet draws it (menus get no
// input on the title, so their OnTrigger never runs): the selection bar, the row icons (the
// selected one lit), and only the selected row's help text.
void DrawCursor(PPCContext& ctx, uint8_t* base, int count, int cursor, bool force)
{
    const std::string n = std::to_string(count);
    MenuSetCursor(ctx, base, "MenuSysMenu" + n, cursor);
    MenuMoviePlay(ctx, base, "amc_menu_bar_sentaku_L", "menu_bar_sentaku_solo_" + Two(cursor + 1), force);
    for (int i = 0; i < count; ++i)
    {
        MenuMoviePlay(ctx, base, "mc_icon_menu_S_L_" + Two(i + 1), i == cursor ? "icon_s_006" : "icon_s_006_x", force);
        const std::string help = "StaticSysMenu" + n + "_0" + std::to_string(i + 1);
        MenuTextHide(ctx, base, help, i != cursor);
        MenuTickCount(ctx, base, help, 0);
    }
}

// Pressed-this-frame buttons, with auto-repeat for UP / DOWN (400 ms, then every 90 ms).
uint16_t Pressed(Ui& ui)
{
    const uint16_t now = HostControllerButtons();
    uint16_t pressed = now & ~ui.prev;
    const uint16_t dirs = now & (kUp | kDown);
    const int64_t t = NowMs();
    if (dirs && dirs == ui.repeatKey && t >= ui.repeatAt) { pressed |= dirs; ui.repeatAt = t + 90; }
    else if (dirs != ui.repeatKey) { ui.repeatKey = dirs; ui.repeatAt = t + 400; }
    ui.prev = now;
    return pressed;
}

void OpenNameDialog(PPCContext& ctx, uint8_t* base, Ui& ui)
{
    ui.screen = Screen::Name;
    ui.name = SuggestedName();
    ui.suggested = true;
    ui.note.clear();
    g_textEntry = true;
    ShowNameDialog(ctx, base, ui);
    std::fprintf(stderr, "[sys-ui] new profile: name dialog\n");
}
void CloseNameDialog(PPCContext& ctx, uint8_t* base, Ui& ui)
{
    HideDialog(ctx, base);
    g_textEntry = false;
    ui.screen = Screen::Profiles;
    ui.shownCursor = -1;
}

// ---- message boxes in the game's dialog windows (see SysUiMessageBox)
std::string Upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string BoxMessage(const BoxState& b)
{
    if (b.typing)
    {
        // the text with a cursor; a long one shows its end (the window is about 44 characters wide)
        std::string shown = b.edit.masked ? std::string(b.edit.text.size(), '*') : b.edit.text;
        if (shown.size() > 40) shown = "..." + shown.substr(shown.size() - 37);
        return b.edit.prompt + "\n\n" + shown + "_\n\n" + (b.note.empty() ? b.edit.hint : b.note);
    }
    if (b.style != BoxStyle::List) return b.box.text;
    std::string m = b.box.text + "\n";
    for (size_t i = 0; i < b.box.buttons.size(); ++i)
    {
        const std::string label = Upper(b.box.buttons[i]);
        m += "\n" + (int(i) == b.cursor ? "> " + label + " <" : label);
    }
    return m;
}
void ShowBox(PPCContext& ctx, uint8_t* base, const BoxState& b)
{
    WriteTableText(base, g_boxText, 2000, BoxMessage(b));
    g_boxBlock = 0;
    {
        static std::map<std::string, uint32_t> pool;
        std::lock_guard l(g_labelMutex);
        g_labels.clear();
        if (b.style != BoxStyle::List)
            for (const std::string& label : b.box.buttons)
            {
                auto it = pool.find(Upper(label));
                if (it == pool.end()) it = pool.emplace(Upper(label), TableText(base, Upper(label).c_str())).first;
                g_labels.push_back(it->second);
            }
    }
    {
        GuestFrame frame(ctx, base, 0x100);
        ctx.r3.u64 = 0;
        ctx.r4.u64 = kBoxTextId;
        // the window's label set: 1 = the A button's label (information); 0 = OK / CANCEL (texts 0x3D8 / 0x3DC,
        // replaced by the box's labels - with 1 a confirmation's second button reads BACK, MEASURED 2026-10-21)
        ctx.r5.u64 = b.style == BoxStyle::Confirmation ? 0 : 1;
        if (b.style == BoxStyle::Confirmation) sub_8241FAF0(ctx, base);   // lib::WindowShowConfirmation
        else sub_8241FB70(ctx, base);                                    // lib::WindowShowInformation
    }
    std::lock_guard l(g_labelMutex);
    g_labels.clear();
}
void UpdateBox(PPCContext& ctx, uint8_t* base, const BoxState& b)
{
    WriteTableText(base, g_boxText, 2000, BoxMessage(b));
    const uint32_t block = g_boxBlock.load();
    if (!block) return;
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = block;
    ctx.r4.u64 = g_boxFont.load();
    ctx.r5.u64 = g_boxSize.load();
    ctx.r6.u64 = g_boxText;
    sub_8241B9D0(ctx, base);                          // the window lays its message out again
}
// CTRL+V in a text box: the clipboard's first line (ASCII; the box keeps the characters it takes).
std::string ClipboardText()
{
    std::string out;
    char* text = SDL_GetClipboardText();
    if (text)
    {
        for (size_t i = 0; text[i] && text[i] != '\r' && text[i] != '\n' && out.size() < 512; ++i)
            if (text[i] >= 0x20 && text[i] < 0x7F) out.push_back(text[i]);
        SDL_free(text);
    }
    return out;
}

// A text box on screen: typed characters edit the text, ENTER / A / START confirm (after `check`), ESC / B cancel.
void TypeFrame(PPCContext& ctx, uint8_t* base, BoxState& b, uint16_t pressed)
{
    std::vector<uint32_t> chars;
    {
        std::lock_guard l(g_charMutex);
        chars.swap(g_chars);
    }
    bool changed = false;
    int answer = -1;                                     // 0 confirm, 1 cancel
    auto add = [&](uint32_t c) {
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && (c < 0x20 || c >= 0x7F || b.edit.allowed.find(char(c)) == std::string::npos)) return;
        if (b.edit.text.size() >= b.edit.maxLength) { b.note = "No more characters fit."; changed = true; return; }
        b.edit.text.push_back(char(c));
        b.note.clear();
        changed = true;
    };
    for (const uint32_t c : chars)
    {
        if (c == '\r') { answer = 0; break; }
        if (c == 0x1B) { answer = 1; break; }
        if (c == '\b')
        {
            if (!b.edit.text.empty()) b.edit.text.pop_back();
            b.note.clear();
            changed = true;
        }
        else if (c == 0x16)                              // CTRL+V
            for (const char p : ClipboardText()) add(uint8_t(p));
        else add(c);
    }
    if (answer < 0) answer = (pressed & (kA | kStart)) ? 0 : (pressed & kB) ? 1 : -1;
    if (answer == 0 && b.edit.check)
    {
        const std::string why = b.edit.check(b.edit.text);
        if (!why.empty())
        {
            b.note = why;
            MenuSound(ctx, base, kSoundCancel);
            UpdateBox(ctx, base, b);
            return;
        }
    }
    if (answer < 0)
    {
        if (changed) UpdateBox(ctx, base, b);
        return;
    }
    MenuSound(ctx, base, answer == 0 ? kSoundDecide : kSoundCancel);
    HideDialog(ctx, base);
    b.showing = false;
    b.releasing = true;
    b.typing = false;
    g_textEntry = false;
    std::fprintf(stderr, "[sys-ui] text box %s\n", answer == 0 ? "confirmed" : "cancelled");   // never the text
    const std::function<void(bool, const std::string&)> done = std::move(b.edit.done);
    const std::string text = std::move(b.edit.text);
    b.edit = SysUiTextBox{};
    if (done) done(answer == 0, text);
}

void BoxFrame(PPCContext& ctx, uint8_t* base)
{
    BoxState& b = g_box;
    const uint16_t now = HostControllerButtons();
    const uint16_t pressed = now & ~b.prev;
    b.prev = now;
    if (g_boxDismiss.exchange(false) && b.showing)
    {
        HideDialog(ctx, base);
        b.showing = false;
        b.releasing = true;
        if (b.typing) { b.typing = false; b.edit = SysUiTextBox{}; g_textEntry = false; }
        std::fprintf(stderr, "[sys-ui] message box closed\n");
        return;
    }
    if (b.releasing)
    {
        if (now == 0) { b.releasing = false; g_boxOwns = false; }   // the answer's press stays ours
        return;
    }
    if (!b.showing)
    {
        {
            std::lock_guard l(g_boxMutex);
            if (!g_textQueue.empty())
            {
                b.edit = std::move(g_textQueue.front());
                g_textQueue.clear();
                b.typing = true;
            }
            else if (!g_boxQueue.empty())
            {
                b.box = std::move(g_boxQueue.front());
                g_boxQueue.clear();
                b.typing = false;
            }
            else return;
        }
        if (b.typing)
        {
            b.box = SysUiMessageBox{};
            b.box.buttons = { "OK", "CANCEL" };
            b.box.cancel = 1;
            b.note.clear();
            {
                std::lock_guard l(g_charMutex);
                g_chars.clear();
            }
            g_textEntry = true;
        }
        const size_t n = b.box.buttons.size();
        const bool shortLabels = n == 2 && b.box.buttons[0].size() <= 8 && b.box.buttons[1].size() <= 8;
        b.style = n == 1 ? BoxStyle::Information : shortLabels ? BoxStyle::Confirmation : BoxStyle::List;
        b.cursor = std::clamp(b.box.active, 0, int(n) - 1);
        b.showing = true;
        b.upFrames = b.goneFrames = 0;
        g_boxOwns = true;
        ShowBox(ctx, base, b);
        return;
    }
    // The box holds the controller, so it must stay on screen: if the game closed its window (a window of the game's
    // own opened over it and went), it is shown again. Never an unseen box the next press answers (server_ui.cpp,
    // 2026-10-23).
    if (++b.upFrames > 30)
    {
        b.goneFrames = menu_host::WindowShowing(ctx, base) ? 0 : b.goneFrames + 1;
        if (b.goneFrames >= 10)
        {
            std::fprintf(stderr, "[sys-ui] the %s was off the screen - shown again\n", b.typing ? "text box" : "message box");
            b.upFrames = b.goneFrames = 0;
            ShowBox(ctx, base, b);
            return;
        }
    }
    if (b.typing)
    {
        TypeFrame(ctx, base, b, pressed);
        return;
    }
    int choice = -1;
    switch (b.style)
    {
    case BoxStyle::Information:
        if (pressed & (kA | kB | kStart)) choice = 0;
        break;
    case BoxStyle::Confirmation:
        if (pressed & (kA | kStart)) choice = 0;
        else if (pressed & kB) choice = 1;
        break;
    case BoxStyle::List:
    {
        const int n = int(b.box.buttons.size());
        if (pressed & (kUp | kDown))
        {
            b.cursor = (b.cursor + ((pressed & kUp) ? n - 1 : 1)) % n;
            MenuSound(ctx, base, kSoundCursor);
            UpdateBox(ctx, base, b);
        }
        if (pressed & (kA | kStart)) choice = b.cursor;
        else if ((pressed & kB) && b.box.cancel >= 0) choice = b.box.cancel;
        break;
    }
    }
    if (choice < 0) return;
    MenuSound(ctx, base, choice == b.box.cancel ? kSoundCancel : kSoundDecide);
    HideDialog(ctx, base);
    b.showing = false;
    b.releasing = true;
    std::fprintf(stderr, "[sys-ui] message box answered: %d '%s'\n", choice, b.box.buttons[size_t(choice)].c_str());
    const std::function<void(int)> done = std::move(b.box.done);
    if (done) done(choice);
}

// Confirm: an empty name takes the suggestion; the new profile signs in.
void Finish(PPCContext& ctx, uint8_t* base, Ui& ui)
{
    std::string name = profiles::Normalize(ui.name);
    if (name.empty()) name = SuggestedName();
    if (!profiles::ValidName(name)) { ui.note = "Use letters, digits and spaces."; return; }
    const int index = profiles::Create(name);
    if (index < 0) { ui.note = "That name is already used."; return; }
    MenuSound(ctx, base, kSoundDecide);
    SignInAndClose(ctx, base, ui, index);
}

void Frame(PPCContext& ctx, uint8_t* base)
{
    BoxFrame(ctx, base);
    Ui& ui = g_ui;
    // A text box over Select Profile (a rename): it takes the typed characters and the controller until it closes.
    if (ui.screen == Screen::Profiles && SysUiBoxBusy()) { Pressed(ui); return; }
    const uint16_t pressed = Pressed(ui);
    if (ui.releasing)
    {
        if (ui.prev == 0)
        {
            ui.releasing = false;
            g_owns = false;
            if (ui.continueAfter) HostPulseStart();
        }
        return;
    }
    if (g_signInRequest.exchange(false) && !profiles::SignedIn())
    {
        // The game asks itself (XamShowSigninUI): its sign-in states draw nothing, so the last
        // used profile signs in. (The title's START no longer gets there: see TitleLogo.)
        const int index = profiles::LastIndex();
        Notify(9, 1);
        profiles::SignIn(index);
        Notify(0x0000000Au, 1);
        Notify(9, 0);
        std::fprintf(stderr, "[sys-ui] the game asked for a sign-in: last profile\n");
        return;
    }
    if (ui.screen == Screen::None)
    {
        if (g_switchProfile.load() && TitleLogo())
        {
            // SWITCH PROFILE: the game is back on the title; the profile signs out here (not in
            // the middle of the mode change) and Select Profile opens by itself.
            g_switchProfile = false;
            profiles::SignOut();
            Notify(0x0000000Au, 1);                      // XN_SYS_SIGNINCHANGED, user 0
            Open(ui, profiles::LastIndex());
            Notify(9, 1);
            std::fprintf(stderr, "[sys-ui] switch profile: Select Profile\n");
            return;
        }
        if (!profiles::SignedIn() && TitleLogo() && (pressed & (kStart | kA)))
        {
            Open(ui, profiles::LastIndex());
            MenuSound(ctx, base, kSoundDecide);
            Notify(9, 1);                                // XN_SYS_UI opened
            std::fprintf(stderr, "[sys-ui] Select Profile\n");
        }
        return;
    }
    if (ui.setupFrames > 0)
    {
        if (--ui.setupFrames == 0) Draw(ctx, base, ui);
        return;
    }

    std::vector<uint32_t> chars;
    {
        std::lock_guard l(g_charMutex);
        chars.swap(g_chars);
    }
    if (ui.screen == Screen::Profiles)
    {
        if (ui.settle > 0 && --ui.settle == 0) ui.shownCursor = -1;   // draw the cursor once more
        const int rows = int(ui.rows.size());
        if (pressed & (kUp | kDown))
        {
            ui.cursor = (ui.cursor + ((pressed & kUp) ? rows - 1 : 1)) % rows;
            MenuSound(ctx, base, kSoundCursor);
        }
        if (pressed & (kA | kStart))
        {
            MenuSound(ctx, base, kSoundDecide);
            if (ui.cursor < int(profiles::List().size())) { SignInAndClose(ctx, base, ui, ui.cursor); return; }
            OpenNameDialog(ctx, base, ui);
            return;
        }
        if (pressed & kB)
        {
            MenuSound(ctx, base, kSoundCancel);
            Close(ctx, base, ui, false);                 // stays on the logo, signed out
            return;
        }
        if ((pressed & kY) && ui.cursor < int(profiles::List().size()))
        {
            // Rename: the game's confirmation window with the name to edit (a PC keyboard types it, as for a new
            // profile). Nobody is signed in on this screen, so no gamertag the game holds goes stale; the save
            // folder goes by the profile's XUID, which stays.
            const int index = ui.cursor;
            SysUiTextBox box;
            box.prompt = "New name for this profile.";
            box.text = profiles::List()[size_t(index)].name;
            box.hint = "Letters, digits and spaces - at most 15.";
            box.allowed = " ";
            box.maxLength = profiles::kMaxName;
            box.check = [index](const std::string& text) -> std::string {
                const std::string name = profiles::Normalize(text);
                if (!profiles::ValidName(name)) return "Use letters, digits and spaces.";
                const std::vector<profiles::Profile> list = profiles::List();
                for (size_t i = 0; i < list.size(); ++i)
                {
                    std::string a = list[i].name, b = name;
                    for (char& c : a) c = char(std::toupper(static_cast<unsigned char>(c)));
                    for (char& c : b) c = char(std::toupper(static_cast<unsigned char>(c)));
                    if (int(i) != index && a == b) return "That name is already used.";
                }
                return "";
            };
            box.done = [index](bool ok, const std::string& text) {
                if (!ok) return;
                if (profiles::Rename(index, text) == 0)
                {
                    BuildProfiles(g_ui);
                    g_ui.shown.clear();                  // the rows are written again
                }
            };
            if (SysUiShowTextBox(std::move(box))) MenuSound(ctx, base, kSoundDecide);
            return;
        }
        Draw(ctx, base, ui);
        return;
    }

    // The name dialog: typed text edits the name, ENTER / A / START confirm, ESC / B cancel.
    bool changed = false;
    for (uint32_t c : chars)
    {
        if (c == '\r') { Finish(ctx, base, ui); if (ui.screen != Screen::Name) return; changed = true; continue; }
        if (c == 0x1B) { MenuSound(ctx, base, kSoundCancel); CloseNameDialog(ctx, base, ui); return; }
        const bool printable = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ';
        if (c != '\b' && !printable) continue;
        if (ui.suggested) { ui.name.clear(); ui.suggested = false; }
        ui.note.clear();
        if (c == '\b') { if (!ui.name.empty()) ui.name.pop_back(); }
        else if (ui.name.size() >= profiles::kMaxName) ui.note = "A name has at most 15 characters.";
        else if (c != ' ' || (!ui.name.empty() && ui.name.back() != ' ')) ui.name.push_back(char(c));
        changed = true;
    }
    if (pressed & (kA | kStart)) { Finish(ctx, base, ui); if (ui.screen != Screen::Name) return; changed = true; }
    else if (pressed & kB) { MenuSound(ctx, base, kSoundCancel); CloseNameDialog(ctx, base, ui); return; }
    if (changed) UpdateNameDialog(ctx, base, ui);
}

} // namespace sysui

// ---- Options -> Controller Setup: KEYBOARD mode (KEYBOARD_PLAN.md) ------------------------------
// The screen's artwork has room for three rows, so player 1's BUTTON SETTINGS row carries a mode in
// its value text (menu_doc.cpp): PAD (the game's own button settings) or KEYBOARD, switched with
// LEFT / RIGHT while no other button is held (the game's own change is a held button + LEFT /
// RIGHT). In KEYBOARD mode the button panel's ten slots show each button's key instead of its
// action - the game writes the slots with lib::MenuEntitySetText, so its texts are recorded there
// (the hook below) and put back when the mode or the row is left - and the game sees only UP /
// DOWN on that row. A starts editing in the panel itself: a highlighted slot moves with the
// arrows, LB / RB turn to the second page (UP DOWN LEFT RIGHT START BACK and DEFAULTS), A asks
// for the slot's key (PRESS A KEY; keys then arrive from the window, SysUiOnKey, while the game
// gets neutral input), B or ESC ends editing. No windows open.
namespace keyui {

using namespace keyboard_keys;
constexpr int kButtonSettingsRow = 1;                    // BUTTON SETTINGS in player 1's menu
constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008, kPadStart = 0x0010,
                   kPadLB = 0x0100, kPadRB = 0x0200, kPadA = 0x1000, kPadB = 0x2000, kDpad = 0x000F;
constexpr int kDefaults = -2, kNone = -1;                 // page 2 cells without a control
const std::string kMenu = "MenuMode007-03-01-1", kValue = "StaticTextMenuItemMode007-03-02-1",
                  kHelp = "StaticTextMode007-03-02-1";
// Panel slot StaticTextMode007-03-SS-1-1 (SS = 01..10): the left column top down, then the right
// (MEASURED on screen 2026-10-14). Slot = column * 5 + row.
const int kPage[2][10] = {
    { LB, LT, X, A, LS, RB, RT, Y, B, RS },
    { Up, Down, Left, Right, kNone, Start, Back, kNone, kNone, kDefaults },
};

struct State
{
    bool here = false;                                   // Controller Setup was up last frame
    bool keyboard = false;                               // BUTTON SETTINGS shows KEYBOARD
    bool editing = false, capturing = false, releasing = false;
    int page = 0, slot = 0;
    std::string value;                                   // the value text as last written
    std::string gameText[10];                            // the game's slot texts (icon + action)
    std::string shown[10];                               // the slots while keys are shown
    bool showing = false;
    std::string help;                                    // the help line while keys are shown
    uint16_t prevButtons = 0;
    bool prevEsc = false;
} g_state;
std::atomic<bool> g_capture{ false };
std::atomic<uint16_t> g_gameMask{ 0xFFFF };              // the buttons the game may see (XamInputGetState)
std::mutex g_keyMutex;
std::vector<int> g_keys;                                 // keys pressed since the last frame

int SlotOf(const std::string& widget)
{
    // "StaticTextMode007-03-SS-1-1"
    static const std::string prefix = "StaticTextMode007-03-", suffix = "-1-1";
    if (widget.size() != prefix.size() + 2 + suffix.size() || widget.compare(0, prefix.size(), prefix) != 0 ||
        widget.compare(prefix.size() + 2, suffix.size(), suffix) != 0)
        return -1;
    const int s = (widget[prefix.size()] - '0') * 10 + (widget[prefix.size() + 1] - '0');
    return s >= 1 && s <= 10 ? s - 1 : -1;
}
std::string SlotWidget(int slot) { return "StaticTextMode007-03-" + sysui::Two(slot + 1) + "-1-1"; }

// A key's name as short as the panel's slots need.
std::string ShortKeyName(int vk)
{
    switch (vk)
    {
    case VK_BACK: return "BKSP";
    case VK_PRIOR: return "PG UP";
    case VK_NEXT: return "PG DN";
    default: return KeyName(vk);
    }
}

// What a slot shows: on page 1 the game's icon part ("~white~<icon> ") and the key, on page 2 the
// control's name and its key (just PRESS A KEY while it waits: the help line names the control).
// The key is in the panel's colour (~default~), the slot being edited in yellow - the colours the
// game's text system knows: red green yellow white gray blue lightgray default; a text has to
// start with one (~white~, as the game's own) for the others to be read, and two tags must not
// touch ("~~" breaks them: the whole text then shows literally).
std::string SlotText(const State& st, int slot)
{
    const int control = kPage[st.page][slot];
    if (control == kNone) return "";
    const bool selected = st.editing && slot == st.slot;
    const std::string colour = selected ? "~yellow~" : "~default~";
    if (control == kDefaults) return "~white~ " + colour + "DEFAULTS";
    const bool asking = st.capturing && selected;
    const std::string key = asking ? std::string("PRESS A KEY") : ShortKeyName(Key(control));
    if (st.page == 1)
        return asking ? "~white~ " + colour + key : std::string("~white~") + ControlName(control) + " " + colour + key;
    const std::string& game = st.gameText[slot];
    const size_t at = game.find("~default~");
    return (at == std::string::npos ? std::string("~white~") : game.substr(0, at)) + colour + key;
}

std::string HelpText(const State& st)
{
    const int control = kPage[st.page][st.slot];
    if (st.capturing) return std::string("Press a key for ") + ControlName(control) + ".   ESC: cancel";
    if (st.editing && control == kDefaults) return "A: restore the default keys   B: done";
    if (st.editing) return "A: change   LB / RB: page   B: done";
    return "A: edit keys";
}

std::string CurrentFrame(PPCContext& ctx, uint8_t* base)
{
    GuestFrame frame(ctx, base, 0x100);
    sub_8256C100(ctx, base);                              // lib::MenuGetCurrentFrameName
    return GuestString(base, ctx.r3.u32, 64);
}
int MenuCursor(PPCContext& ctx, uint8_t* base, const std::string& menu)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, menu);
    sub_8256C968(ctx, base);                              // lib::MenuContainerGetCursor(menu)
    return static_cast<int32_t>(ctx.r3.u32);
}
// The game's own help text of BUTTON SETTINGS (textid360 4,635), back after the keyboard hints.
void RestoreHelp(PPCContext& ctx, uint8_t* base)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = 4;
    ctx.r4.u64 = 635;
    ctx.r5.u64 = GuestStr(base, kHelp);
    sub_8256C7A8(ctx, base);                              // lib::MenuEntitySetTextID(table, id, widget)
}

bool Foreground()
{
    return host_input::Focused();
}
bool AnyKeyDown()
{
    return host_input::AnyKeyDown();
}

// The next slot from `slot` in a direction (page 2 skips its empty cells; rows and columns wrap).
int Move(int page, int slot, uint16_t dir)
{
    int col = slot / 5, row = slot % 5;
    for (int tries = 0; tries < 10; ++tries)
    {
        if (dir == kPadUp) row = (row + 4) % 5;
        else if (dir == kPadDown) row = (row + 1) % 5;
        else col ^= 1;
        if (kPage[page][col * 5 + row] != kNone) return col * 5 + row;
        if (dir & (kPadLeft | kPadRight)) dir = kPadUp;   // an empty cell across: go up to a used one
    }
    return slot;
}

void Edit(PPCContext& ctx, uint8_t* base, State& st, uint16_t pressed, bool esc)
{
    if (st.capturing)
    {
        std::vector<int> keys;
        {
            std::lock_guard l(g_keyMutex);
            keys.swap(g_keys);
        }
        bool done = (pressed & kPadB) != 0;               // a controller's B cancels too
        for (int vk : keys)
        {
            if (done) break;
            if (vk == VK_ESCAPE) { done = true; break; }
            if (!Assignable(vk)) continue;
            Assign(kPage[st.page][st.slot], vk);
            MenuSound(ctx, base, kSoundDecide);
            done = true;
        }
        if (!done) return;
        st.capturing = false;
        st.releasing = true;                              // g_capture stays until the keys are up
        return;
    }
    if (pressed & (kPadB) || esc)
    {
        st.editing = false;
        MenuSound(ctx, base, kSoundCancel);
        return;
    }
    if (pressed & (kPadLB | kPadRB))
    {
        st.page ^= 1;
        st.slot = 0;
        MenuSound(ctx, base, kSoundCursor);
        return;
    }
    for (const uint16_t dir : { kPadUp, kPadDown, kPadLeft, kPadRight })
        if (pressed & dir)
        {
            st.slot = Move(st.page, st.slot, dir);
            MenuSound(ctx, base, kSoundCursor);
            return;
        }
    if (pressed & (kPadA | kPadStart))
    {
        const int control = kPage[st.page][st.slot];
        MenuSound(ctx, base, kSoundDecide);
        if (control == kDefaults) { RestoreDefaults(); return; }
        st.capturing = true;
        {
            std::lock_guard l(g_keyMutex);
            g_keys.clear();
        }
        g_capture = true;
    }
}

void Draw(PPCContext& ctx, uint8_t* base, State& st, bool keys)
{
    if (keys)
    {
        for (int s = 0; s < 10; ++s)
        {
            const std::string text = SlotText(st, s);
            if (!st.showing || st.shown[s] != text) MenuSetText(ctx, base, SlotWidget(s), text);
            st.shown[s] = text;
        }
        const std::string help = HelpText(st);
        if (!st.showing || help != st.help) MenuSetText(ctx, base, kHelp, help);
        st.help = help;
        st.showing = true;
    }
    else if (st.showing)
    {
        for (int s = 0; s < 10; ++s)
            if (!st.gameText[s].empty()) MenuSetText(ctx, base, SlotWidget(s), st.gameText[s]);
        RestoreHelp(ctx, base);
        st.showing = false;
    }
}

void Frame(PPCContext& ctx, uint8_t* base)
{
    State& st = g_state;
    if (CurrentFrame(ctx, base) != "FrameControlerSetup")
    {
        if (st.here) std::fprintf(stderr, "[keyboard] controller setup closed\n");
        State fresh;                                      // the game's slot texts stay recorded
        std::copy(std::begin(st.gameText), std::end(st.gameText), std::begin(fresh.gameText));
        st = fresh;
        g_capture = false;
        g_gameMask = 0xFFFF;
        return;
    }
    const uint16_t buttons = HostControllerButtons();
    const bool esc = Foreground() && host_input::KeyDown(VK_ESCAPE);
    if (!st.here)
    {
        st.here = true;                                   // every visit starts on PAD
        st.prevButtons = buttons;                         // the A that opened the screen is not ours
        st.prevEsc = esc;
    }
    const uint16_t pressed = buttons & ~st.prevButtons;
    const bool escPressed = esc && !st.prevEsc;
    st.prevButtons = buttons;
    st.prevEsc = esc;

    if (st.releasing)
    {
        // The new key is still down: it acts as its button only once released.
        if (!AnyKeyDown() && !(buttons & 0xF3FF)) { st.releasing = false; g_capture = false; st.prevButtons = 0; }
    }
    else if (st.editing) Edit(ctx, base, st, pressed, escPressed);
    else if (MenuCursor(ctx, base, kMenu) == kButtonSettingsRow)
    {
        // Not while a button or trigger is held: that is the game's own change of its action.
        if ((pressed & (kPadLeft | kPadRight)) && !(buttons & ~kDpad) && !HostTriggerHeld())
        {
            st.keyboard = !st.keyboard;
            MenuSound(ctx, base, kSoundCursor);
        }
        else if (st.keyboard && (pressed & kPadA))
        {
            st.editing = true;
            st.page = 0;
            st.slot = 0;
            MenuSound(ctx, base, kSoundDecide);
        }
    }
    const bool onRow = st.editing || MenuCursor(ctx, base, kMenu) == kButtonSettingsRow;
    g_gameMask = st.editing ? uint16_t(0) : (onRow && st.keyboard) ? uint16_t(0x0003) : uint16_t(0xFFFF);

    const std::string value = st.keyboard ? "KEYBOARD" : "PAD";
    if (value != st.value)
    {
        MenuSetText(ctx, base, kValue, value);
        st.value = value;
    }
    Draw(ctx, base, st, onRow && st.keyboard);
}

} // namespace keyui

std::atomic<uint32_t> g_frameCount{ 0 };

} // namespace

// ---- menu_host.h: the calls the port's other screens (server_ui.cpp) make
namespace menu_host {
std::string CurrentFrame(PPCContext& ctx, uint8_t* base) { return keyui::CurrentFrame(ctx, base); }
void SetText(PPCContext& ctx, uint8_t* base, const std::string& widget, const std::string& text) { MenuSetText(ctx, base, widget, text); }
void SetVisible(PPCContext& ctx, uint8_t* base, const std::string& widget, bool visible)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = visible ? 1 : 0;
    ctx.r4.u64 = GuestStr(base, widget);
    sub_8256C5A0(ctx, base);                              // lib::MenuSetVisible(bool, widget)
}
void SetFocusable(PPCContext& ctx, uint8_t* base, const std::string& widget, bool focusable)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = focusable ? 1 : 0;
    ctx.r4.u64 = GuestStr(base, widget);
    sub_8256C638(ctx, base);                              // lib::MenuSetFocusable(bool, widget)
}
void MovieVisible(PPCContext& ctx, uint8_t* base, const std::string& movie, bool visible) { MenuMovieVisible(ctx, base, movie, visible); }
int Cursor(PPCContext& ctx, uint8_t* base, const std::string& menu) { return keyui::MenuCursor(ctx, base, menu); }
void PlaySound(PPCContext& ctx, uint8_t* base, Sound sound) { MenuSound(ctx, base, sound); }
bool WindowShowing(PPCContext& ctx, uint8_t* base)
{
    GuestFrame frame(ctx, base, 0x100);
    sub_8241E828(ctx, base);                              // lib::WindowIsShowing
    return (ctx.r3.u32 & 0xFF) != 0;
}
void KeyHint(PPCContext& ctx, uint8_t* base, uint32_t key, const std::string& text)
{
    // a text as the window's hints take them (the game passes its text table's entries): length byte in front
    static std::map<std::string, uint32_t> pool;
    auto it = pool.find(text);
    if (it == pool.end()) it = pool.emplace(text, TableText(base, text.c_str())).first;
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = key;
    ctx.r4.u64 = it->second;
    sub_8241FAB8(ctx, base);                              // lib::WindowPrintKeyInfoText(key, text)
}
uint32_t FrameCount() { return g_frameCount.load(); }
} // namespace menu_host

namespace {

// Runs on the game's main thread once per frame (end of VdSwap, registers restored after): the
// system UI, and a diagnostics trigger - C:/ttt2/port/ui.trigger, one command per line: a frame name
// switches the menu to that frame, "text <widget>=<text>" sets a widget's text ("\\n" = new line),
// "cursor <menu>=<index>" moves a menu's cursor, "movie <name>=0|1" hides / shows a menu movie,
// "telop <widget>" aims the bottom bar, "box <text>|<button>|<button>" shows a message box (UI mock-ups).
void UiFrame(PPCContext& ctx, uint8_t* base)
{
    g_frameCount.fetch_add(1);
    DisplayChangeGuardFrame();
    const std::string inputFrame = menu_host::CurrentFrame(ctx, base);
    if(EnvFlag("TTT2_MENU_FRAME_TRACE")){static std::string previousFrame;if(inputFrame!=previousFrame){std::fprintf(stderr,"[menu-frame] %s\n",inputFrame.c_str());previousFrame=inputFrame;}}
    // Apply the launcher preference through the game's own option setter only after
    // a signed-in profile reaches the main menu. Its normal save path owns serialization.
    static uint64_t motionBlurProfile=0;
    const uint64_t activeProfile=profiles::SignedIn()?profiles::ActiveXuid():0;
    if(inputFrame=="FrameMainMenu" && activeProfile && activeProfile!=motionBlurProfile && display_settings::GetMotionBlur()>=0){
        const PPCContext saved=ctx;
        { GuestFrame frame(ctx,base,0x100);ctx.r3.u64=display_settings::GetMotionBlur();__imp__sub_823749C0(ctx,base); }
        ctx=saved;motionBlurProfile=activeProfile;
        std::fprintf(stderr,"[display] applied motion blur %d through guest option setter\n",display_settings::GetMotionBlur());
    }
    cheat_overlay::Frame(inputFrame);
    persistent_unlocks::ProcessPending(ctx,base,inputFrame=="FrameMainMenu" || inputFrame.rfind("FrameMode007-",0)==0);
    HostSetSharedMenuInput(inputFrame == "FrameMainMenu" ||
        inputFrame.rfind("FrameMode007-", 0) == 0 ||
        inputFrame == "FrameOnlineMenu" || inputFrame == "FrameOnlineServerList");
    sysui::Frame(ctx, base);
    keyui::Frame(ctx, base);
    server_ui::Frame(ctx, base);
    static uint32_t tick = 0;
    if (++tick % 30) return;
    const std::string trigger = host_settings::DiagnosticPath("ui.trigger");
    FILE* f = std::fopen(trigger.c_str(), "rb");
    if (!f) return;
    std::string text;
    char chunk[4096];
    for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0 && text.size() < 65536;) text.append(chunk, n);
    std::fclose(f);
    std::error_code error;
    std::filesystem::remove(trigger, error);
    for (size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::fprintf(stderr, "[menu-ext] ui trigger: '%s'\n", line.c_str());
        const size_t eq = line.find('=');
        if (line.rfind("text ", 0) == 0 && eq != std::string::npos)
        {
            std::string value = line.substr(eq + 1);
            for (size_t p; (p = value.find("\\n")) != std::string::npos;) value.replace(p, 2, "\n");
            MenuSetText(ctx, base, line.substr(5, eq - 5), value);
        }
        else if (line.rfind("cursor ", 0) == 0 && eq != std::string::npos)
            MenuSetCursor(ctx, base, line.substr(7, eq - 7), std::atoi(line.c_str() + eq + 1));
        else if (line.rfind("movie ", 0) == 0 && eq != std::string::npos)
            MenuMovieVisible(ctx, base, line.substr(6, eq - 6), line[eq + 1] == '1');
        else if (line.rfind("telop ", 0) == 0)
            MenuTelopTarget(ctx, base, line.substr(6));
        else if (line.rfind("box ", 0) == 0)
        {
            SysUiMessageBox box;
            std::string rest = line.substr(4);
            for (size_t p; (p = rest.find("\\n")) != std::string::npos;) rest.replace(p, 2, "\n");
            size_t bar = rest.find('|');
            box.text = rest.substr(0, bar);
            while (bar != std::string::npos)
            {
                const size_t next = rest.find('|', bar + 1);
                box.buttons.push_back(rest.substr(bar + 1, next == std::string::npos ? std::string::npos : next - bar - 1));
                bar = next;
            }
            if (box.buttons.empty()) box.buttons.push_back("OK");
            box.cancel = int(box.buttons.size()) - 1;
            SysUiShowMessageBox(std::move(box));
        }
        else
            SwitchFrame(ctx, base, line.c_str());
    }
}
struct UiHookInstall { UiHookInstall() { g_uiFrameHook = UiFrame; } } g_uiHookInstall;
} // namespace

bool SysUiRequestSignIn() { sysui::g_signInRequest = true; return true; }
bool SysUiOwnsInput() { return sysui::g_owns.load() || sysui::g_boxOwns.load() || keyui::g_capture.load(); }
bool SysUiKeyCaptureActive() { return keyui::g_capture.load(); }
uint16_t SysUiGameButtonMask() { return keyui::g_gameMask.load() & server_ui::GameButtonMask(); }
void SysUiOnKey(int vk)
{
    if (!keyui::g_capture.load()) return;
    std::lock_guard l(keyui::g_keyMutex);
    if (keyui::g_keys.size() < 16) keyui::g_keys.push_back(vk);
}
bool SysUiTitleWantsProfile() { return !profiles::SignedIn() && sysui::TitleLogo(); }
bool SysUiTextEntryActive() { return sysui::g_textEntry.load(); }
bool SysUiShowMessageBox(SysUiMessageBox box)
{
    std::lock_guard l(sysui::g_boxMutex);
    if (sysui::g_boxOwns.load() || !sysui::g_boxQueue.empty() || !sysui::g_textQueue.empty() || box.buttons.empty()) return false;
    sysui::g_boxQueue.push_back(std::move(box));
    return true;
}
bool SysUiShowTextBox(SysUiTextBox box)
{
    std::lock_guard l(sysui::g_boxMutex);
    if (sysui::g_boxOwns.load() || !sysui::g_boxQueue.empty() || !sysui::g_textQueue.empty()) return false;
    sysui::g_textQueue.push_back(std::move(box));
    return true;
}
void SysUiDismissBox()
{
    std::lock_guard l(sysui::g_boxMutex);
    sysui::g_boxQueue.clear();
    sysui::g_textQueue.clear();
    sysui::g_boxDismiss = true;
}
bool SysUiBoxBusy()
{
    std::lock_guard l(sysui::g_boxMutex);
    return sysui::g_boxOwns.load() || !sysui::g_boxQueue.empty() || !sysui::g_textQueue.empty();
}
void SysUiOnChar(uint32_t ch)
{
    if (!sysui::g_textEntry.load()) return;
    std::lock_guard l(sysui::g_charMutex);
    if (sysui::g_chars.size() < 64) sysui::g_chars.push_back(ch);
}

namespace {

menu_doc::PcLists PcLists()
{
    menu_doc::PcLists lists;
    lists.modes = { "WINDOWED", "BORDERLESS", "FULLSCREEN" };
    for (const display_settings::Resolution& r : display_settings::Resolutions())
        lists.resolutions.push_back(std::to_string(r.width) + "x" + std::to_string(r.height));
    lists.serverRow = server_ui::Enabled();
    return lists;
}

// ---- natives -------------------------------------------------------------------------------
// Signature descriptors of the game's own natives (port/tools/xtal_natives.py). Implementations
// use the plain ABI (r3 in, r3 out).
constexpr uint32_t kSetterInt = 0x8203624C;   // void f(int)
constexpr uint32_t kGetterInt = 0x8205B9A4;   // int f()
constexpr uint32_t kGetterBool = 0x820025B8;  // bool f()

PPC_FUNC(NativeSetWindowMode) { (void)base; display_settings::SetMode(static_cast<int>(ctx.r3.u32)); }
PPC_FUNC(NativeGetWindowMode) { (void)base; ctx.r3.u64 = static_cast<uint32_t>(display_settings::GetMode()); }
PPC_FUNC(NativeSetResolution) { (void)base; display_settings::SetResolutionIndex(static_cast<int>(ctx.r3.u32)); }
PPC_FUNC(NativeGetResolution) { (void)base; ctx.r3.u64 = static_cast<uint32_t>(display_settings::GetResolutionIndex()); }
PPC_FUNC(NativeSetSharpness) { (void)base; display_settings::SetSharpness(static_cast<int>(ctx.r3.u32)); }
PPC_FUNC(NativeSetAntiAliasing) { (void)base; display_settings::SetAntiAliasing(static_cast<int>(ctx.r3.u32)); }
PPC_FUNC(NativeSetTextureFilter) { (void)base; display_settings::SetTextureFilter(static_cast<int>(ctx.r3.u32)); }
PPC_FUNC(NativeSetVSync) { (void)base; display_settings::SetVSync(ctx.r3.u32 == 0); }     // ON, OFF
PPC_FUNC(NativeSetShadows) { (void)base; display_settings::SetShadows(ctx.r3.u32 == 0); } // ON, OFF
// The room's Friend Invite: the native it replaces runs first (lib::OnlinePlayerMatchOperationMenuSetValue, sub_826002C8:
// the room closes its operation menu - MEASURED 2026-10-23: the friend picker opened while the menu stayed on screen,
// over the picker), then the Steam friend picker.
PPC_FUNC(NativeFriendInvite)
{
    __imp__sub_826002C8(ctx, base);
    server_ui::FriendInvite();
}
PPC_FUNC(NativeTrue) { (void)base; ctx.r3.u64 = 1; }

// lib::ExitGameWindow - the exit dialog's lib::WindowGetValue: -1 while it waits, 0 for the
// confirm button (A, YES), otherwise NO. Settings are saved as they change; the guest threads
// are not unwound (an immediate process exit, as for a closed window), so nothing can hang on the way out.
PPC_FUNC(NativeExitGameWindow)
{
    __imp__sub_8241E7A8(ctx, base);
    if (static_cast<int32_t>(ctx.r3.u32) == 0)
    {
        std::fprintf(stderr, "[menu] exit confirmed - quitting\n");
        std::fflush(stderr);
        std::fflush(stdout);
        LogDrain(); host_process::ExitNow(0);
    }
}

// lib::SwitchProfileWindow - the SWITCH PROFILE dialog's lib::WindowGetValue: 0 (YES) arms the
// sign-out for when the title is back (the script then requests the SIGNOUT mode).
PPC_FUNC(NativeSwitchProfileWindow)
{
    __imp__sub_8241E7A8(ctx, base);
    if (static_cast<int32_t>(ctx.r3.u32) == 0 && !sysui::g_switchProfile.exchange(true))
        std::fprintf(stderr, "[menu] switch profile confirmed\n");
}

// lib::ServerListKeyInfo - the server list page's lib::WindowPrintKeyInfo (menu_doc.cpp renames it in the page's
// OnTick): the page's own hints, with the server list's in front of the first of each frame.
PPC_FUNC(NativeServerListKeyInfo)
{
    static uint32_t lastFrame = 0;
    if (menu_host::FrameCount() != lastFrame)
    {
        lastFrame = menu_host::FrameCount();
        const PPCContext saved = ctx;
        server_ui::KeyHints(ctx, base);
        ctx = saved;
    }
    __imp__sub_8241F988(ctx, base);
}

struct HostNative { const char* name; uint32_t signature; PPCFunc* fn; };
const HostNative kNatives[] = {
    // main menu: the 9th item (STORE in the game, EXIT here) is shown when this is true
    { "IsExitMenuItemVisible", kGetterBool, NativeTrue },
    // the PC rows' OnTrigger is 3D DEPTH's with renamed natives: the capability gate, the
    // preview call and the store call (both apply live)
    { "DisplaySettingIsPCDisplayCapable", kGetterBool, NativeTrue },
    { "MenuOptionDisplayWindowMode", kSetterInt, NativeSetWindowMode },
    { "DisplaySettingSetWinMode", kSetterInt, NativeSetWindowMode },
    { "MenuOptionDisplayResolution", kSetterInt, NativeSetResolution },
    { "DisplaySettingSetDispRes", kSetterInt, NativeSetResolution },
    { "DisplaySettingGetWindowMode", kGetterInt, NativeGetWindowMode },
    { "DisplaySettingGetResolution", kGetterInt, NativeGetResolution },
    // VSYNC / SHARPNESS / ANTI-ALIASING / TEXTURE FILTERING (preview and store both apply live)
    { "MenuOptionDisplayVSyncOnOff", kSetterInt, NativeSetVSync },
    { "DisplaySettingSetVSyncOn", kSetterInt, NativeSetVSync },
    { "MenuOptionDisplaySharpening", kSetterInt, NativeSetSharpness },
    { "DisplaySettingSetSharpen", kSetterInt, NativeSetSharpness },
    { "MenuOptionDisplayEdgeSmooth", kSetterInt, NativeSetAntiAliasing },
    { "DisplaySettingSetEdgesAA", kSetterInt, NativeSetAntiAliasing },
    { "MenuOptionDisplayTexFilters", kSetterInt, NativeSetTextureFilter },
    { "DisplaySettingSetTexFilt", kSetterInt, NativeSetTextureFilter },
    { "MenuOptionDisplayShadowsOff", kSetterInt, NativeSetShadows },
    { "DisplaySettingSetShadows", kSetterInt, NativeSetShadows },
    // the Player Match room's Friend Invite (menu_doc.cpp AddFriendInvite): the signature of the native it replaces,
    // OnlinePlayerMatchOperationMenuSetValue (sub_826002C8, two arguments - MEASURED 2026-10-23: with a one-int
    // setter's the call failed and the item did nothing)
    { "OnlinePlayerMatchSteamFriendInviteOpen", 0x8209FB4C, NativeFriendInvite },
    // the exit confirmation's answer
    { "ExitGameWindow", kGetterInt, NativeExitGameWindow },
    // the switch-profile confirmation's answer
    { "SwitchProfileWindow", kGetterInt, NativeSwitchProfileWindow },
    // the server list's key hints (its OnTick's lib::WindowPrintKeyInfo, same signature)
    { "ServerListKeyInfo", 0x8209FB64, NativeServerListKeyInfo },
};
// Guest code addresses routed to the host natives: the interior of the development menu loader
// sub_823450B0 (never called in retail; nothing indirect-calls into a function's middle).
constexpr uint32_t kNativeSlotBase = 0x823450C0;

// Define the natives in `lib` exactly as sub_823756A8 defines the game's own:
// sub_82A802F0(&native, signature, &impl), sub_82A6F5E8(&id, name, length, 0),
// sub_82A69330(&copy, &native), sub_822501A0(lib, &id, &copy), sub_82A69360(&native).
void RegisterNatives(PPCContext& ctx, uint8_t* base, uint32_t lib)
{
    GuestFrame frame(ctx, base, 0x200);
    const uint32_t sp = frame.sp();
    const uint32_t implCell = sp + 0x80, native = sp + 0xA0, id = sp + 0xB0, copy = sp + 0xC0;
    for (size_t i = 0; i < sizeof(kNatives) / sizeof(kNatives[0]); ++i)
    {
        const HostNative& n = kNatives[i];
        const uint32_t slot = kNativeSlotBase + uint32_t(i) * 4;
        PPC_LOOKUP_FUNC(base, slot) = n.fn;
        GWrite32(base, implCell, slot);
        ctx.r3.u64 = native; ctx.r4.u64 = n.signature; ctx.r5.u64 = implCell;
        sub_82A802F0(ctx, base);
        ctx.r3.u64 = id; ctx.r4.u64 = GuestCString(base, n.name); ctx.r5.u64 = std::strlen(n.name); ctx.r6.u64 = 0;
        sub_82A6F5E8(ctx, base);
        ctx.r3.u64 = copy; ctx.r4.u64 = native;
        sub_82A69330(ctx, base);
        ctx.r5.u64 = ctx.r3.u32; ctx.r3.u64 = lib; ctx.r4.u64 = id;
        sub_822501A0(ctx, base);
        ctx.r3.u64 = native;
        sub_82A69360(ctx, base);
    }
    std::fprintf(stderr, "[menu] registered %zu PC natives\n", sizeof(kNatives) / sizeof(kNatives[0]));
}

// The exit and switch-profile confirmations (lib::WindowShowConfirmation(id) in the cloned
// dialogs). The window looks up its button labels right after its message: text 0x3D8 "OK" and
// 0x3DC "CANCEL" (MEASURED 2026-10-12); for these dialogs they read YES / NO.
constexpr uint32_t kExitTextId = 0x7F01, kSwitchTextId = 0x7F04, kOkTextId = 0x03D8, kCancelTextId = 0x03DC;
std::atomic<int> g_exitLabelsPending{ 0 };
} // namespace

// ---- hooks ---------------------------------------------------------------------------------

// Options natives registration; the PC natives are added right after, into the same `lib`:
// sub_823756A8 takes it as [sub_82582860(sub_82254450()) + 56] (script environment singleton).
PPC_FUNC(sub_823756A8)
{
    __imp__sub_823756A8(ctx, base);
    const PPCContext saved = ctx;
    {
        GuestFrame frame(ctx, base, 0x100);
        sub_82254450(ctx, base);
        sub_82582860(ctx, base);
    }
    const uint32_t lib = GRead32(base, ctx.r3.u32 + 56);
    RegisterNatives(ctx, base, lib);
    ctx = saved;
}

// lib::MenuSelectValueSetCursor(cursor, name): when Display Settings initialises MOTION BLUR's
// value (on open and on DEFAULT), the PC rows get their current values (cursor at widget+368); when
// Game Options initialises BATTLE POSITION's, INPUT DELAY gets its own (OFF = 0, ON = 1).
PPC_FUNC(sub_8256CD28)
{
    const uint32_t nameAddr = ctx.r4.u32;
    __imp__sub_8256CD28(ctx, base);
    const std::string name = GuestString(base, nameAddr, 64);
    const bool display = name == "SelectValueMenuItemMode007-04-02", game = name == "SelectValueMenuItemMode007-01-05";
    if (!display && !game) return;
    const PPCContext saved = ctx;
    {
        GuestFrame frame(ctx, base, 0x100);
        static const uint32_t modeName = GuestCString(base, "SelectValueMenuItemMode007-04-06");
        static const uint32_t resName = GuestCString(base, "SelectValueMenuItemMode007-04-07");
        static const uint32_t delayName = GuestCString(base, "SelectValueMenuItemMode007-01-07");
        if (display)
        {
            if (const uint32_t w = FindWidget(ctx, base, modeName)) GWrite32(base, w + 368, uint32_t(display_settings::GetMode()));
            if (const uint32_t w = FindWidget(ctx, base, resName))
                GWrite32(base, w + 368, uint32_t(display_settings::GetResolutionIndex()));
            static const uint32_t sharpName = GuestCString(base, "SelectValueMenuItemMode007-04-08");
            static const uint32_t aaName = GuestCString(base, "SelectValueMenuItemMode007-04-09");
            static const uint32_t filterName = GuestCString(base, "SelectValueMenuItemMode007-04-10");
            if (const uint32_t w = FindWidget(ctx, base, sharpName)) GWrite32(base, w + 368, uint32_t(display_settings::GetSharpness()));
            if (const uint32_t w = FindWidget(ctx, base, aaName)) GWrite32(base, w + 368, uint32_t(display_settings::GetAntiAliasing()));
            if (const uint32_t w = FindWidget(ctx, base, filterName)) GWrite32(base, w + 368, uint32_t(display_settings::GetTextureFilter()));
            static const uint32_t vsyncName = GuestCString(base, "SelectValueMenuItemMode007-04-11");
            if (const uint32_t w = FindWidget(ctx, base, vsyncName)) GWrite32(base, w + 368, display_settings::GetVSync() ? 0u : 1u);
            static const uint32_t shadowsName = GuestCString(base, "SelectValueMenuItemMode007-04-12");
            if (const uint32_t w = FindWidget(ctx, base, shadowsName)) GWrite32(base, w + 368, display_settings::GetShadows() ? 0u : 1u);
        }
        else
        {
            if (const uint32_t w = FindWidget(ctx, base, delayName))
                GWrite32(base, w + 368, display_settings::GetInputDelayOn() ? 1u : 0u);
            // ONLINE NETCODE (cursor = the setting), CUSTOM INPUT DELAY and CUSTOM ROLLBACK (= frames - 1)
            static const uint32_t netName = GuestCString(base, "SelectValueMenuItemMode007-01-08");
            static const uint32_t customDelayName = GuestCString(base, "SelectValueMenuItemMode007-01-09");
            static const uint32_t customRollbackName = GuestCString(base, "SelectValueMenuItemMode007-01-10");
            if (const uint32_t w = FindWidget(ctx, base, netName)) GWrite32(base, w + 368, uint32_t(display_settings::GetNetcode()));
            if (const uint32_t w = FindWidget(ctx, base, customDelayName))
                GWrite32(base, w + 368, uint32_t(display_settings::GetCustomDelay() - 1));
            if (const uint32_t w = FindWidget(ctx, base, customRollbackName))
                GWrite32(base, w + 368, uint32_t(display_settings::GetCustomRollback() - 1));
        }
    }
    ctx = saved;
}

// lib::GameOptionSetBattlePosition (impl sub_82374700): Game Options stores its rows as it closes,
// BATTLE POSITION last - INPUT DELAY's cursor is stored with it.
PPC_FUNC(sub_82374700)
{
    __imp__sub_82374700(ctx, base);
    const PPCContext saved = ctx;
    {
        GuestFrame frame(ctx, base, 0x100);
        static const uint32_t delayName = GuestCString(base, "SelectValueMenuItemMode007-01-07");
        if (const uint32_t w = FindWidget(ctx, base, delayName))
            display_settings::SetInputDelayOn(GRead32(base, w + 368) == 1);
        static const uint32_t netName = GuestCString(base, "SelectValueMenuItemMode007-01-08");
        static const uint32_t customDelayName = GuestCString(base, "SelectValueMenuItemMode007-01-09");
        static const uint32_t customRollbackName = GuestCString(base, "SelectValueMenuItemMode007-01-10");
        if (const uint32_t w = FindWidget(ctx, base, netName)) display_settings::SetNetcode(int(GRead32(base, w + 368)));
        if (const uint32_t w = FindWidget(ctx, base, customDelayName)) display_settings::SetCustomDelay(int(GRead32(base, w + 368)) + 1);
        if (const uint32_t w = FindWidget(ctx, base, customRollbackName))
            display_settings::SetCustomRollback(int(GRead32(base, w + 368)) + 1);
    }
    ctx = saved;
}

// lib::GameOptionResetToDefaults (impl sub_823747C0): Game Options' DEFAULT resets INPUT DELAY and
// the online netcode rows too (their cursors follow when the page re-initialises BATTLE POSITION's).
PPC_FUNC(sub_823747C0)
{
    __imp__sub_823747C0(ctx, base);
    display_settings::SetInputDelayOn(false);
    display_settings::SetNetcode(display_settings::NetStandard);
    display_settings::SetCustomDelay(1);
    display_settings::SetCustomRollback(display_settings::kMaxRollback);
}

// TitleMode::Impl::state_title_main: the logo waiting for START (sub_82622B70); impl+40 counts the
// frames in the state and the attract demo starts when it reaches impl+128 (MEASURED 2026-10-12).
// The logo stays up while the system UI is open.
PPC_FUNC(sub_82623858)
{
    sysui::g_titleLogoMs = sysui::NowMs();
    if (SysUiOwnsInput())
    {
        GWrite32(base, ctx.r3.u32 + 40, 0);
        GWrite32(base, ctx.r3.u32 + 44, 0);
    }
    __imp__sub_82623858(ctx, base);
}

// The bottom bar's key help gets one hint (sub_8241B588: window manager, key, text, -1, side - from
// lib::WindowPrintKeyInfo / _Text). TTT2_SRV_TRACE=1 logs each key / text pair once: which key number is
// which button.
PPC_FUNC(sub_8241B588)
{
    static const bool trace = EnvFlag("TTT2_SRV_TRACE");
    if (trace && ctx.r5.u32)
    {
        static std::mutex m;
        static std::map<std::string, int> seen;
        const std::string text = GuestString(base, ctx.r5.u32, 48);
        const std::string key = std::to_string(ctx.r4.u32) + " " + text;
        std::lock_guard l(m);
        if (seen[key]++ == 0)
            std::fprintf(stderr, "[keyhint] key %u side %u '%s' lr %08X\n", ctx.r4.u32, ctx.r7.u32, text.c_str(), uint32_t(ctx.lr));
    }
    __imp__sub_8241B588(ctx, base);
}

// A dialog window's text block takes a text (block, font, size, text: copied and laid out). The
// name dialog's and the message box's blocks are remembered to lay their texts out again.
PPC_FUNC(sub_8241B9D0)
{
    if (ctx.r6.u32 && ctx.r6.u32 == sysui::g_nameText)
    {
        sysui::g_nameBlock = ctx.r3.u32;
        sysui::g_nameFont = ctx.r4.u32;
        sysui::g_nameSize = ctx.r5.u32;
    }
    else if (ctx.r6.u32 && ctx.r6.u32 == sysui::g_boxText)
    {
        sysui::g_boxBlock = ctx.r3.u32;
        sysui::g_boxFont = ctx.r4.u32;
        sysui::g_boxSize = ctx.r5.u32;
    }
    __imp__sub_8241B9D0(ctx, base);
}

// lib::MenuEntitySetText(text, widget): Controller Setup's button panel (player 1) - the game's
// texts are recorded, and while the KEYBOARD mode shows keys the game's writes keep the key.
PPC_FUNC(sub_8256C708)
{
    if (!g_hostText && ctx.r4.u32 && ctx.r3.u32)
    {
        const int slot = keyui::SlotOf(GuestString(base, ctx.r4.u32, 40));
        if (slot >= 0)
        {
            keyui::g_state.gameText[slot] = GuestString(base, ctx.r3.u32, 96);
            if (keyui::g_state.showing) ctx.r3.u64 = GuestStr(base, keyui::SlotText(keyui::g_state, slot));
        }
    }
    __imp__sub_8256C708(ctx, base);
}

// Text lookup (text manager, id): the exit confirmation's message and button labels, the name
// dialog's message.
PPC_FUNC(sub_8237F5D0)
{
    const uint32_t id = ctx.r4.u32 & 0xFFFF;
    if (id == kExitTextId || id == kSwitchTextId)
    {
        static const uint32_t exitText = TableText(base, "Exit the game?");
        static const uint32_t switchText = TableText(base, "Switch to another profile?\nThe game returns to the title screen.");
        ctx.r3.u64 = id == kExitTextId ? exitText : switchText;
        g_exitLabelsPending = 2;
        return;
    }
    if ((id == kOkTextId || id == kCancelTextId) && g_exitLabelsPending.load() > 0)
    {
        static const uint32_t yes = TableText(base, "YES"), no = TableText(base, "NO");
        g_exitLabelsPending.fetch_sub(1);
        ctx.r3.u64 = id == kOkTextId ? yes : no;
        return;
    }
    if (id == sysui::kNameTextId && sysui::g_nameText)
    {
        ctx.r3.u64 = sysui::g_nameText;
        return;
    }
    if (id == sysui::kBoxTextId && sysui::g_boxText)
    {
        ctx.r3.u64 = sysui::g_boxText;
        return;
    }
    if (id == kOkTextId || id == kCancelTextId)
    {
        std::lock_guard l(sysui::g_labelMutex);           // a message box's button labels
        const size_t slot = id == kOkTextId ? 0 : 1;
        if (slot < sysui::g_labels.size())
        {
            ctx.r3.u64 = sysui::g_labels[slot];
            return;
        }
    }
    if ((ctx.r4.u32 >> 16) == 0x0300 && id >= 0x0660 && id <= 0x0698)
    {
        // The information bar's tips (the bottom bar of the menus and loading screens; text bank 0x0300, MEASURED
        // 2026-10-22): on a community server every other tip is the server's - its players right now ("TTT2 Test
        // Server: 12 players online, 4 playing.", server_ui.cpp) and its news lines, taking turns.
        const std::vector<std::string> news = gin_service::News();
        const std::string status = server_ui::StatusLine();
        static std::mutex m;
        static uint32_t lastId = 0, turn = 0, lastText = 0, statusText = 0;
        static std::map<std::string, uint32_t> texts;
        std::lock_guard l(m);
        if (!news.empty() || !status.empty())
        {
            if (id != lastId)                             // a new tip (the bar may ask again for the same one)
            {
                lastId = id;
                ++turn;
                lastText = 0;
                const uint32_t slot = turn / 2;
                if ((turn & 1) && !status.empty() && (news.empty() || (slot & 1) == 0))
                {
                    // the counts change: one guest text, written again each time
                    sysui::WriteTableText(base, statusText, 160, status);
                    lastText = statusText;
                }
                else if ((turn & 1) && !news.empty())
                {
                    // guest text for each line seen (at most 64: lines from a server that keeps changing them are not shown)
                    const std::string& line = news[(status.empty() ? slot : slot / 2) % news.size()];
                    const auto it = texts.find(line);
                    if (it != texts.end()) lastText = it->second;
                    else if (texts.size() < 64) lastText = texts[line] = TableText(base, line.c_str());
                }
            }
            if (lastText)
            {
                ctx.r3.u64 = lastText;
                return;
            }
        }
    }
    const uint32_t lr = uint32_t(ctx.lr);
    const bool corner = (id == 0x09 || id == 0x0A || id == 0x0B) && lr >= 0x82277000 && lr < 0x82278000;
    const bool logo = id == 0x05 && lr >= 0x82646000 && lr < 0x82647000;
    if ((corner || logo) && SysUiOwnsInput())
    {
        // The title's PRESS START / FREE PLAY texts (the arcade credit display in the corners,
        // looked up from 0x82277664 / 0x82277818, and the logo's from 0x8264656C - MEASURED
        // 2026-10-12) would sit on the system UI's key hints; they are blank while it is open.
        static const uint32_t empty = TableText(base, "");
        ctx.r3.u64 = empty;
        return;
    }
    // TTT2_TEXT_DUMP=<hex id>: when an id up to 0x100 above it is first looked up (its bank loaded by then)
    static const uint32_t dumpAt = std::getenv("TTT2_TEXT_DUMP") ? uint32_t(std::strtoul(std::getenv("TTT2_TEXT_DUMP"), nullptr, 16)) : 0xFFFFFFFFu;
    const bool dump = id >= dumpAt && id < dumpAt + 0x100;   // any id in that range
    if (dump)
    {
        // research: every text id 0..0xFFF of each text manager, once
        static std::mutex m;
        static std::map<uint32_t, bool> done;
        bool first;
        {
            std::lock_guard l(m);
            first = !done[ctx.r3.u32];
            done[ctx.r3.u32] = true;
        }
        if (first)
        {
            std::fprintf(stderr, "[text-dump] manager %08X (first asked for %08X by %08X)\n", ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
            PPCContext copy = ctx;
            for (uint32_t i = 0; i < 0x1000; ++i)
            {
                copy.r3 = ctx.r3;
                copy.r4.u64 = (ctx.r4.u32 & 0xFFFF0000u) | i;
                __imp__sub_8237F5D0(copy, base);
                if (copy.r3.u32 && base[copy.r3.u32]) std::fprintf(stderr, "[text-dump] %04X '%s'\n", i, GuestString(base, copy.r3.u32, 160).c_str());
            }
        }
    }
    __imp__sub_8237F5D0(ctx, base);
    static const bool trace = std::getenv("TTT2_TEXT_TRACE") != nullptr;
    if (trace)
    {
        static std::mutex m;
        static std::map<uint32_t, int> seen;
        std::lock_guard l(m);
        if (seen[id]++ == 0)
            std::fprintf(stderr, "[text] id %04X lr %08X '%s'\n", id, lr, GuestString(base, ctx.r3.u32, 48).c_str());
    }
}

// Retail menu document loader: r3 menu manager, r4 BX01 bytes, r5 size, r6 root widget. The
// Options / main-menu document gets the PC edits; the edited copy lives in a never-freed guest
// allocation, as the reader keeps pointing into its bytes.
PPC_FUNC(sub_82346060)
{
    const uint8_t* doc = base + ctx.r4.u32;
    const uint32_t size = ctx.r5.u32;
    if (MenuOverride())
    {
        char path[128];
        std::snprintf(path, sizeof(path), "menu_override/%08X.bin", Fnv1a(doc, size));
        const std::string overridePath = host_settings::DiagnosticPath(path);
        if (FILE* f = std::fopen(overridePath.c_str(), "rb"))
        {
            std::string bytes;
            char chunk[65536];
            for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) bytes.append(chunk, n);
            std::fclose(f);
            ctx.r4.u64 = GuestCopy(base, bytes.data(), bytes.size());
            ctx.r5.u64 = bytes.size();
            std::fprintf(stderr, "[menu] document replaced by %s (%zu bytes)\n", overridePath.c_str(), bytes.size());
        }
    }
    else
    {
        std::string why, note;
        const std::string edited = menu_doc::ApplyPcEdits(doc, size, PcLists(), &why, &note);
        if (!note.empty()) std::fprintf(stderr, "[menu] %s\n", note.c_str());
        if (!edited.empty())
        {
            if (MenuDump())
            {
                std::error_code error;
                std::filesystem::create_directories(host_settings::DiagnosticPath("menu_bin"), error);
                char path[128];
                std::snprintf(path, sizeof(path), "menu_bin/edited_%08X.bin", Fnv1a(doc, size));
                SaveFile(host_settings::DiagnosticPath(path).c_str(), edited.data(), edited.size());
            }
            ctx.r4.u64 = GuestCopy(base, edited.data(), edited.size());
            ctx.r5.u64 = edited.size();
            const bool online = std::string(reinterpret_cast<const char*>(doc), size).find("MenuItemOnline07-leaderboard") != std::string::npos;
            std::fprintf(stderr, "[menu] %s (%u -> %zu bytes)\n",
                         online ? "ONLINE MODE menu: SERVER row added" : "Options / main menu: PC rows and EXIT added", size, edited.size());
        }
        else if (!why.empty())
            std::fprintf(stderr, "[menu] menu document left unchanged: %s\n", why.c_str());
    }
    __imp__sub_82346060(ctx, base);
}

// BX01 document reader: r3 document, r4 bytes, r5 size. TTT2_MENU_DUMP=1 saves each document.
PPC_FUNC(sub_82A8C170)
{
    if (MenuDump() && ctx.r4.u32 && ctx.r5.u32 && ctx.r5.u32 < (64u << 20))
    {
        static std::atomic<uint32_t> index{ 0 };
        std::error_code error;
        std::filesystem::create_directories(host_settings::DiagnosticPath("menu_bin"), error);
        char path[128];
        std::snprintf(path, sizeof(path), "menu_bin/%03u_%08X.bin", index.fetch_add(1),
                      Fnv1a(base + ctx.r4.u32, ctx.r5.u32));
        SaveFile(host_settings::DiagnosticPath(path).c_str(), base + ctx.r4.u32, ctx.r5.u32);
    }
    __imp__sub_82A8C170(ctx, base);
}

PPC_FUNC(sub_823749C0) {
    const bool on=ctx.r3.u8!=0;
    __imp__sub_823749C0(ctx,base);
    display_settings::SetMotionBlur(on?1:0);
}
