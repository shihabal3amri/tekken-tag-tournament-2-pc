// The game's depth of field (post filter "PostFilter_Dof", per-stage FocusDistance / Near* / Far* parameters).
// sub_8239A968(postFilter, wanted) returns the post filters that run this frame as a mask: bit 0 depth of field
// (when sub_8239A0F0 gives a DOF mode: 1 the stage's parameters, 2 a camera animation's), 1 heat haze, 2 glare,
// 3 SNN, 4 sun shafts. Without bit 0 the frame update (sub_8239BC80) skips the DOF parameters and stores -1 as its
// two technique handles (+3712 near / +3716 far), and the filter chain (sub_8239E490) skips sub_82399588 (its
// resolves, half-size blurs and composite) - the game's own path for modes without depth of field; the next
// filter reads the scene as drawn.
// Options -> Graphics Settings -> DEPTH OF FIELD (display_settings.h, p87 / p95) turns it off; the picture only,
// the game runs the same (rollback, online).
// DIAGNOSTIC BUILD (p86): TTT2_DOF=0 / 1 forces off / on, =ab alternates with every report window (on, off, ...).
// p92: TTT2_POSTFX_OFF=<mask> removes those post filters always, TTT2_POSTFX_AB=<mask> every other report window
// (mask bits as above: 2 heat haze, 4 glare, 8 SNN, 16 sun shafts; hex with 0x).
// TEST ONLY: TTT2_DOF_FILE=<path> - a file holding 0 turns it off (read every 200 ms; same-moment screenshots).
// TEST ONLY (p97): TTT2_POSTFX_FILE=<path> - a file holding a mask (decimal) removes those post filters (read every
// 200 ms; same-moment screenshots of glare / sun shafts / heat haze / SNN).
// p93: the game's dynamic resolution (the console's: the scene drawn 1024 / 960 / 800 / 720 / 640 wide instead of 1280
// when its GPU clock says the frame is slow). sub_822645A0 (the frame setup, from sub_8226E768) picks the level and
// stores it at +4 of sub_82568590's object (0x82EEDB58; +12 a flag for the special levels 6 / 7); every render
// pass reads it from there (sub_82398E68 and the scene's surface ids, level << 16 / << 20). Levels 1-5 are the
// narrow widths of the table at 0x82DDC798, 0 the full one. Level 0 (full width) is always held: TEST 17 removed the
// DYNAMIC RESOLUTION row (Graphics Settings, default OFF since it came). MEASURED (tester logs 50 / 51, i3-3240 +
// GT 730, Paris): the game asks for a narrow width in ~300 of 300 fight frames on any PC (also the 9800X3D, its command
// processor nearly idle - its clock is not our GPU's load), ON cost the same CPU (2.82 vs 2.80 cores) and fps, the
// GPU 1.6 ms less a frame, half the picture's width, and a second present at each width change (early present guessed
// the old resolve). TTT2_DYNRES=0 / 1 forces full / the game's (developers), =ab alternates with the report windows.
#include "kernel_shared.h"
#include "display_settings.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_8239A968);
extern "C" PPC_FUNC(__imp__sub_822645A0);

namespace post_dof {
namespace {
int Mode()      // 0 off, 1 on, 2 A/B, 3 the menu's setting
{
    static const int mode = [] {
        const char* e = std::getenv("TTT2_DOF");
        return !e || !*e ? 3 : std::strcmp(e, "ab") == 0 ? 2 : *e == '0' ? 0 : 1;
    }();
    return mode;
}
uint32_t EnvMask(const char* name)
{
    const char* e = std::getenv(name);
    return e && *e ? uint32_t(std::strtoul(e, nullptr, 0)) : 0u;
}
uint32_t FxOff() { static const uint32_t m = EnvMask("TTT2_POSTFX_OFF"); return m; }
uint32_t FxAb() { static const uint32_t m = EnvMask("TTT2_POSTFX_AB"); return m; }
int DynMode()   // 0 full width, 1 the game's, 2 A/B, 3 not set (full width)
{
    static const int mode = [] {
        const char* e = std::getenv("TTT2_DYNRES");
        return !e || !*e ? 3 : std::strcmp(e, "ab") == 0 ? 2 : *e == '0' ? 0 : 1;
    }();
    return mode;
}
std::atomic<uint64_t> g_dynReduced{ 0 }, g_dynHeld{ 0 };
std::atomic<bool> g_abOff{ false };
std::atomic<uint64_t> g_frames{ 0 }, g_removed{ 0 };
std::atomic<uint64_t> g_fxAsked[5] = {};        // p96: calls whose mask had each post filter (before any removal)
bool FileOff()
{
    static const char* const path = std::getenv("TTT2_DOF_FILE");
    if (!path || !*path) return false;
    static std::atomic<int64_t> next{ 0 };
    static std::atomic<bool> off{ false };
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now >= next.load(std::memory_order_relaxed))
    {
        next.store(now + 200, std::memory_order_relaxed);
        bool value = false;
        if (FILE* f = std::fopen(path, "rb")) { value = std::fgetc(f) == '0'; std::fclose(f); }
        off.store(value, std::memory_order_relaxed);
    }
    return off.load(std::memory_order_relaxed);
}
uint32_t FileMask()
{
    static const char* const path = std::getenv("TTT2_POSTFX_FILE");
    if (!path || !*path) return 0;
    static std::atomic<int64_t> next{ 0 };
    static std::atomic<uint32_t> mask{ 0 };
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now >= next.load(std::memory_order_relaxed))
    {
        next.store(now + 200, std::memory_order_relaxed);
        char text[16] = {};
        if (FILE* f = std::fopen(path, "rb")) { std::fread(text, 1, sizeof(text) - 1, f); std::fclose(f); }
        mask.store(uint32_t(std::strtoul(text, nullptr, 10)) & 0x1Eu, std::memory_order_relaxed);
    }
    return mask.load(std::memory_order_relaxed);
}
} // namespace

bool AbOn() { return Mode() == 2 || FxAb() != 0 || DynMode() == 2; }
bool AbOff() { return g_abOff.load(std::memory_order_relaxed); }
void AbToggle() { g_abOff.store(!g_abOff.load(std::memory_order_relaxed), std::memory_order_relaxed); }
bool Off() { return Mode() == 0 || (Mode() == 2 && AbOff()) || (Mode() == 3 && !display_settings::GetDepthOfField()) || FileOff(); }
// The report windows' label: what this window removes.
const char* Label()
{
    if (DynMode() == 2) return AbOff() ? " dynres-full" : " dynres-game";
    if (FxAb()) return AbOff() ? " fx-off" : " fx-on";
    return Off() ? " dof-off" : Mode() == 2 ? " dof-on" : "";
}
// Calls that asked for depth of field (several a frame) and how many of them had it removed, since the last call.
void TakeStats(uint64_t& wanted, uint64_t& removed)
{
    wanted = g_frames.exchange(0, std::memory_order_relaxed);
    removed = g_removed.exchange(0, std::memory_order_relaxed);
}
// Frames the game chose a narrow width for and how many of them were held at the full width, since the last call.
void TakeDynStats(uint64_t& reduced, uint64_t& held)
{
    reduced = g_dynReduced.exchange(0, std::memory_order_relaxed);
    held = g_dynHeld.exchange(0, std::memory_order_relaxed);
}
// p96: the calls (several a frame) that asked for depth of field, heat haze, glare, SNN, sun shafts, since the last call.
void TakeFxStats(uint64_t* asked)
{
    for (int i = 0; i < 5; ++i) asked[i] = g_fxAsked[i].exchange(0, std::memory_order_relaxed);
}
bool DynHeld()
{
    return DynMode() == 0 || DynMode() == 3 || (DynMode() == 2 && AbOff());
}
} // namespace post_dof

PPC_FUNC(sub_8239A968)
{
    __imp__sub_8239A968(ctx, base);
    for (int i = 0; i < 5; ++i)
        if (ctx.r3.u32 & (1u << i)) post_dof::g_fxAsked[i].fetch_add(1, std::memory_order_relaxed);
    ctx.r3.u32 &= ~(post_dof::FxOff() | post_dof::FileMask() | (post_dof::AbOff() ? post_dof::FxAb() : 0u));
    if (!display_settings::GetBloom()) ctx.r3.u32 &= ~4u;           // p99: Graphics Settings -> BLOOM / LIGHT SHAFTS
    if (!display_settings::GetLightShafts()) ctx.r3.u32 &= ~16u;
    if (!(ctx.r3.u32 & 1)) return;
    post_dof::g_frames.fetch_add(1, std::memory_order_relaxed);
    if (post_dof::Off())
    {
        ctx.r3.u32 &= ~1u;
        post_dof::g_removed.fetch_add(1, std::memory_order_relaxed);
    }
}

PPC_FUNC(sub_822645A0)
{
    __imp__sub_822645A0(ctx, base);
    constexpr uint32_t kLevel = 0x82EEDB58 + 4, kSpecial = 0x82EEDB58 + 12;
    const uint32_t level = PPC_LOAD_U32(kLevel);
    if (level < 1 || level > 5 || PPC_LOAD_U8(kSpecial)) return;
    post_dof::g_dynReduced.fetch_add(1, std::memory_order_relaxed);
    if (!post_dof::DynHeld()) return;
    PPC_STORE_U32(kLevel, 0);
    post_dof::g_dynHeld.fetch_add(1, std::memory_order_relaxed);
}
