// Xam user/profile, XConfig, XMsg, and misc system imports. These return real
// "retail US console, user 0 signed in locally" data (values modeled on UnleashedRecomp)
// so engine init that branches on region/language/profile gets sane values, not stub zeros.
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "guest_heap.h"
#include "kernel_content.h"
#include "title_server.h"
#include "profiles.h"
#include "sys_ui.h"
#include "keyboard_keys.h"
#include "direction_filter.h"
#include "cheat_overlay.h"
#include "host_keycodes.h"
#include "host_input_sdl.h"
#include "xlive.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

// SDL snapshots use the Xbox pad wire layout; these local host structures only
// hold the cached state before the guest fields are written in big-endian order.
struct XboxPadData { uint16_t wButtons = 0; uint8_t bLeftTrigger = 0, bRightTrigger = 0;
    int16_t sThumbLX = 0, sThumbLY = 0, sThumbRX = 0, sThumbRY = 0; };
struct XboxPadState { uint32_t dwPacketNumber = 0; XboxPadData Gamepad{}; };
struct XboxPadCapabilities { uint8_t Type = 0, SubType = 0; uint16_t Flags = 0;
    XboxPadData Gamepad{}; struct { uint16_t wLeftMotorSpeed = 0, wRightMotorSpeed = 0; } Vibration; };
constexpr uint8_t XboxPadDeviceType = 1, XboxPadDeviceSubtype = 1;
constexpr uint8_t XboxPadTriggerThreshold = 30;

// --- ExGetXConfigSetting(category, setting, buffer, bufferSize, requiredSize) ---
PPC_FUNC(__imp__ExGetXConfigSetting)
{
    KTrace("ExGetXConfigSetting");
    uint32_t category = ctx.r3.u32 & 0xFFFF, setting = ctx.r4.u32 & 0xFFFF;
    uint32_t buffer = ctx.r5.u32, size = ctx.r6.u32 & 0xFFFF, required = ctx.r7.u32;
    uint32_t value = 0;
    bool known = false;
    if (category == 0x0002)                                          // XCONFIG_SECURED_CATEGORY
    {
        if (setting == 0x0002) { value = 0x00001000; known = true; } // AV region: USA/Canada
    }
    else if (category == 0x0003)                                     // XCONFIG_USER_CATEGORY
    {
        switch (setting)
        {
        case 0x0001: case 0x0002: case 0x0003: case 0x0004:          // time-zone fields: zero
        case 0x0005: case 0x0006: case 0x0007:
            value = 0; known = true; break;
        case 0x0009: value = 1;          known = true; break;        // language: English
        case 0x000A: value = 0x00040000; known = true; break;        // video flags: widescreen
        case 0x000C: value = 1;          known = true; break;        // retail flags
        case 0x000E: value = 103;        known = true; break;        // country: United States
        }
    }
    if (!known)
    {
        std::fprintf(stderr, "[xconfig] unknown query category=%04X setting=%04X size=%u\n",
                     category, setting, size);
        ctx.r3.u32 = 1;
        return;
    }
    for (uint32_t i = 0; i < size && i < 16; ++i) GWrite8(base, buffer + i, 0);
    if (size >= 4) GWrite32(base, buffer, value);
    GWrite32(base, required, 4);
    ctx.r3.u32 = 0;
}

// --- console identity ---
PPC_FUNC(__imp__XGetGameRegion)
{
    KTrace("XGetGameRegion"); ctx.r3.u32 = 0x03FF; }                 // region-free
PPC_FUNC(__imp__XGetLanguage)
{
    KTrace("XGetLanguage"); ctx.r3.u32 = 1; }                        // English
PPC_FUNC(__imp__XGetAVPack)
{
    KTrace("XGetAVPack"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamGetSystemVersion)
{
    KTrace("XamGetSystemVersion"); ctx.r3.u32 = 0; }

// --- user 0: the signed-in local profile (profiles.cpp: name = gamertag, offline XUID; nobody
// at boot until the Select Profile screen signs one in). With TTT2_TITLE_SERVER=1 the profile
// reports a Live sign-in (state 2), which TTT2's title-storage download requires
// (title_server.cpp); its online XUID is stable and distinct from the offline one, because
// the profile monitor sub_82B37080 compares both every poll. ---
static bool User0SignedIn() { return profiles::SignedIn(); }

// --- users 1-3: guests of user 0's profile. On the console a second player signs in (a guest of the
// first player's profile, or a profile of their own) before playing online, and TTT2's online PAIR PLAY
// takes only a signed-in controller (MEASURED 2026-10-22: at "Press START button of controller in use."
// player 2's START did nothing - user report: "the second controller can't join at all"; offline pair
// play takes any controller). A guest signs in when its controller is first used (PadPollState::guestOf
// claims it) and stays while user 0 is signed in; it is named after the profile ("NAME(1)"), has XUIDs
// of its own derived from the profile's, and cannot save (the content calls take user 0 only, as on the
// console, where guests have no storage).
// Shared front-end navigation is enabled only by an explicit menu-frame allowlist.
std::atomic<bool> g_sharedMenuInput{false};
void HostSetSharedMenuInput(bool enabled) { g_sharedMenuInput.store(enabled); }

std::atomic<uint32_t> g_usedUsers{ 0 };       // bit u: user u's controller has been used
static bool SignedIn(uint32_t user)
{
    if (user == 0) return User0SignedIn();
    return user < 4 && (g_usedUsers.load() >> user & 1) && User0SignedIn();
}
static uint64_t OfflineXuid(uint32_t user)
{
    const uint64_t xuid = profiles::ActiveXuid();
    if (user == 0) return xuid;
    return 0xE000000000000000ull | ((xuid ^ uint64_t(user) * 0xD6E8FEB86659FD93ull) & 0x0000FFFFFFFFFFFFull);
}
// One of this PC's users' XUIDs (offline or online, signed in or not): title storage serves those from here.
bool XamLocalUserXuid(uint64_t xuid)
{
    for (uint32_t user = 0; xuid && user < 4; ++user)
        if (xuid == OfflineXuid(user) || xuid == xlive::OnlineXuid(user)) return true;
    return false;
}
static std::string UserName(uint32_t user)
{
    std::string name = profiles::Active().name;
    if (user == 0) return name;
    const std::string tag = "(" + std::to_string(user) + ")";
    return name.substr(0, 15 - tag.size()) + tag;
}

// TTT2_XAM_TRACE=1: each new (function, caller, user, result) of the user queries, timestamped;
// TTT2_XAM_TRACE=2: users 1-3's again every second, with the caller's caller.
static void UserTrace(const char* fn, uint32_t lr, uint32_t user, uint32_t result, uint32_t lr2 = 0)
{
    static const char* env = std::getenv("TTT2_XAM_TRACE");
    if (!env) return;
    static std::mutex m;
    static std::unordered_map<uint64_t, int> seen;
    uint64_t key = (uint64_t(lr) << 32) ^ (uint64_t(user) << 24) ^ result ^ (uint64_t(uintptr_t(fn)) << 8);
    if (*env == '2' && user) key ^= (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) * 0x9E3779B97F4A7C15ull ^ uint64_t(lr2) << 16;
    std::lock_guard l(m);
    if (seen[key]++) return;
    std::fprintf(stderr, "[xam-user] %llu ms %s user %u lr %08X (%08X) -> %08X\n",
                 (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), fn, user, lr, lr2, result);
}
// The caller's caller: the saved LR in the caller's frame (back chain at r1, LR at -8 of the previous frame).
static uint32_t Caller2(uint8_t* base, const PPCContext& ctx)
{
    const uint32_t back = GRead32(base, ctx.r1.u32);
    return back > 8 ? GRead32(base, back - 8) : 0;
}

PPC_FUNC(__imp__XamUserGetSigninState)
{
    KTrace("XamUserGetSigninState");
    const uint32_t user = ctx.r3.u32;
    ctx.r3.u32 = SignedIn(user) ? (TitleServerEnabled() ? 2 : 1) : 0;
    UserTrace("GetSigninState", uint32_t(ctx.lr), user, ctx.r3.u32, Caller2(base, ctx));
}

// Xenia Canary's GetXUID uses a profile-type mask (type 1|2), independently
// from GetSigninInfo's online/offline XUID flags. This does not sign in online.
// https://github.com/xenia-canary/xenia-canary/blob/canary_experimental/src/xenia/kernel/xam/xam_user.cc
// A profile signed in to LIVE answers the online bit (2; XUserGetXUID asks 7) with its online
// XUID, as on the console: TTT2's match code (NACE) identifies players by it, and every
// player's first profile shares the offline XUID B13EBABEBABEBABE (MEASURED 2026-10-12: two
// such players replaced each other in the session room).
PPC_FUNC(__imp__XamUserGetXUID)
{
    KTrace("XamUserGetXUID");
    const uint32_t user=ctx.r3.u32, mask=ctx.r4.u32, out=ctx.r5.u32;
    if(!XamGuestRange(out,8)){ctx.r3.u32=0x80070057u;return;}
    GWrite32(base,out,0);GWrite32(base,out+4,0);
    if(user>=4 || (mask!=1&&mask!=2&&mask!=3&&mask!=4&&mask!=7))
    {ctx.r3.u32=0x80070057u;return;}
    if(!(mask&3) || !SignedIn(user)){ctx.r3.u32=0x80070525u;UserTrace("GetXUID",uint32_t(ctx.lr),user,ctx.r3.u32,Caller2(base,ctx));return;}
    const uint64_t xuid=(mask&2) && TitleServerEnabled() ? xlive::OnlineXuid(user) : OfflineXuid(user);
    GWrite32(base,out,uint32_t(xuid>>32));GWrite32(base,out+4,uint32_t(xuid));
    ctx.r3.u32=0;
    if(user)UserTrace("GetXUID",uint32_t(ctx.lr),user,mask,Caller2(base,ctx));
}

PPC_FUNC(__imp__XamUserGetSigninInfo)
{
    KTrace("XamUserGetSigninInfo");
    uint32_t user = ctx.r3.u32, flags = ctx.r4.u32, info = ctx.r5.u32;
    if (!info) { ctx.r3.u32 = 0x80070057u; return; }               // E_INVALIDARG
    std::memset(base + info, 0, 0x28);
    if (!SignedIn(user))                                               // HRESULT: no such user
    {
        ctx.r3.u32 = 0x80070525u;
        UserTrace("GetSigninInfo", uint32_t(ctx.lr), user, ctx.r3.u32, Caller2(base, ctx));
        return;
    }
    // Bit 1 requests the offline identity; bit 0 requests an online identity.
    // There is no online profile. Returning the local XUID for both makes the
    // game's profile monitor (82B37080) detect a sign-in transition every frame.
    // Matches Xenia Canary XamUserGetSigninInfo's offline-only behavior.
    const bool live = TitleServerEnabled();
    if (!flags || (flags & 2))
    {
        const uint64_t offline = OfflineXuid(user);
        GWrite32(base, info + 0, uint32_t(offline >> 32));         // XUID (hi)
        GWrite32(base, info + 4, uint32_t(offline));               // XUID (lo)
    }
    else if (live)
    {
        const uint64_t online = xlive::OnlineXuid(user);           // unique per machine, profile and user
        GWrite32(base, info + 0, uint32_t(online >> 32));
        GWrite32(base, info + 4, uint32_t(online));
    }
    // XUSER_INFO_FLAG_LIVE_ENABLED (1) | XUSER_INFO_FLAG_GUEST (2); a guest's number and sponsor (user 0).
    GWrite32(base, info + 8, (live ? 1u : 0u) | (user ? 2u : 0u));
    GWrite32(base, info + 12, live ? 2 : 1);                         // SigninState: Live / local
    GWrite32(base, info + 16, user);
    const std::string name = UserName(user);
    const size_t n = (std::min)(name.size(), size_t(15));            // gamertag (ASCII, 16 bytes)
    std::memcpy(base + info + 24, name.data(), n);
    GWrite8(base, info + 24 + uint32_t(n), 0);
    ctx.r3.u32 = 0;
    if (user) UserTrace("GetSigninInfo", uint32_t(ctx.lr), user, flags, Caller2(base, ctx));
}

PPC_FUNC(__imp__XamUserGetName)
{
    KTrace("XamUserGetName");
    uint32_t user = ctx.r3.u32, buf = ctx.r4.u32, size = ctx.r5.u32;
    if (!buf || !size || !SignedIn(user)) { ctx.r3.u32 = 0x525; return; }
    const std::string name = UserName(user);
    const uint32_t n = (std::min)(uint32_t(name.size()), size - 1);
    std::memcpy(base + buf, name.data(), n);
    GWrite8(base, buf + n, 0);
    ctx.r3.u32 = 0;
    if (user) UserTrace("GetName", uint32_t(ctx.lr), user, 0, Caller2(base, ctx));
}

// (titleId, userIndex, xuidCount, xuids, settingCount, settingIds, bufferSize, buffer, overlapped)
// Query pass (buffer null): report 4 bytes needed. Read pass: zero the buffer; the game
// tolerates all-zero profile settings (Unleashed behavior).
// Profile settings (XUserReadProfileSettings, ...ByXuid with xuids), as XAM answers them: the first call (no buffer,
// or one too small) gets ERROR_INSUFFICIENT_BUFFER and the size; the second an XUSER_READ_PROFILE_SETTING_RESULT
// {u32 count, u32 settings} with one XUSER_PROFILE_SETTING (40 bytes: +0 source, +8 user index / xuid, +0x10 setting
// id, +0x18 XUSER_DATA {u8 type, +8 value / {u32 size, u32 pointer}}) per player and setting, each "no value"
// (source 0: the game's defaults; no profile settings are kept here), then the strings' and blobs' room.
// MEASURED 2026-10-23: the license screen (sub_826D5CB0) reads a player's gamer picture key (0x4064000F) and requires
// 122 from the first call, else "Failed to obtain license."; the second call is overlapped. (The old answer - success,
// 4 bytes, a zeroed buffer - failed every read: the menus asked again and again, ~1000 times a session.)
void ReadProfileSettings(PPCContext& ctx, uint8_t* base)
{
    const uint32_t xuidCount = ctx.r5.u32, xuids = ctx.r6.u32, settingCount = ctx.r7.u32, ids = ctx.r8.u32;
    const uint32_t players = (std::max)(xuidCount, 1u);   // none: the requesting user (r4)
    const uint32_t sizePtr = ctx.r9.u32, buffer = ctx.r10.u32;
    const uint32_t overlapped = XamGuestRange(ctx.r1.u32 + 0x54, 4) ? GRead32(base, ctx.r1.u32 + 0x54) : 0;
    if (xuidCount > 16 || settingCount > 64 || (xuidCount && !XamGuestRange(xuids, uint64_t(xuidCount) * 8)) ||
        !XamGuestRange(ids, uint64_t(settingCount) * 4) || !XamGuestRange(sizePtr, 4))
    {
        ctx.r3.u32 = 0x57;                                   // ERROR_INVALID_PARAMETER
        return;
    }
    if (const uint32_t error = XamValidateOverlapped(base, overlapped)) { ctx.r3.u32 = error; return; }
    // The dashboard's settings (title FFFE07D1): only the voice library (XHV, sub_82C6CE5C) reads them - voice muted,
    // through speakers, volume (1004000C..E) - and it gets "nothing to read" (success, 4 bytes, the buffer zeroed), as
    // before 2026-10-23. MEASURED 2026-10-23: given the settings, its worker held its lock 50-130 ms at a time while
    // the main thread waited for it ([cs] slow acquire at 82C692B4) - 70-105 frames over 50 ms in every online fight
    // (the user: "the match weren't smooth"); answered this way, 0 frames over 25 ms, the longest 28 ms. No voice here.
    if (ctx.r3.u32 == 0xFFFE07D1u)
    {
        if (buffer) { if (const uint32_t n = GRead32(base, sizePtr); n && XamGuestRange(buffer, n)) std::memset(base + buffer, 0, n); }
        else GWrite32(base, sizePtr, 4);
        ctx.r3.u32 = 0;
        return;
    }
    const uint32_t entries = players * settingCount;
    uint32_t needed = 8 + entries * 40;
    for (uint32_t i = 0; i < settingCount; ++i)
    {
        const uint32_t id = GRead32(base, ids + i * 4), type = id >> 28;
        if (type == 4 || type == 6) needed += players * ((((id >> 16) & 0xFFF) + 3) & ~3u);   // unicode / binary
    }
    if (!buffer || GRead32(base, sizePtr) < needed || !XamGuestRange(buffer, needed))
    {
        GWrite32(base, sizePtr, needed);
        ctx.r3.u32 = 122;                                    // ERROR_INSUFFICIENT_BUFFER
        return;
    }
    std::memset(base + buffer, 0, needed);
    GWrite32(base, buffer, entries);
    GWrite32(base, buffer + 4, buffer + 8);
    uint32_t extra = buffer + 8 + entries * 40;
    for (uint32_t x = 0; x < players; ++x)
        for (uint32_t i = 0; i < settingCount; ++i)
        {
            const uint32_t e = buffer + 8 + (x * settingCount + i) * 40, id = GRead32(base, ids + i * 4), type = id >> 28;
            if (xuidCount)
            {
                GWrite32(base, e + 0x08, GRead32(base, xuids + x * 8));
                GWrite32(base, e + 0x0C, GRead32(base, xuids + x * 8 + 4));
            }
            else
                GWrite32(base, e + 0x08, ctx.r4.u32);
            GWrite32(base, e + 0x10, id);
            GWrite8(base, e + 0x18, uint8_t(type));
            if (type == 4 || type == 6)
            {
                GWrite32(base, e + 0x24, extra);             // size 0: empty, the room zeroed
                extra += (((id >> 16) & 0xFFF) + 3) & ~3u;
            }
        }
    ctx.r3.u32 = XamCompleteOverlapped(base, overlapped, 0, 0, 0);
}

PPC_FUNC(__imp__XamUserReadProfileSettings)
{
    KTrace("XamUserReadProfileSettings");
    uint32_t bufferSize = ctx.r9.u32, buffer = ctx.r10.u32;
    if (std::getenv("TTT2_XLIVE_TRACE"))
    {
        const uint32_t xuidCount = ctx.r5.u32, xuids = ctx.r6.u32, settingCount = ctx.r7.u32, ids = ctx.r8.u32;
        std::fprintf(stderr, "[xam] XamUserReadProfileSettings(user %u, %u xuids first %08X%08X, %u settings first %08X, buffer %08X) lr=%08X\n",
                     ctx.r4.u32, xuidCount, xuids && xuidCount ? GRead32(base, xuids) : 0u, xuids && xuidCount ? GRead32(base, xuids + 4) : 0u,
                     settingCount, ids && settingCount ? GRead32(base, ids) : 0u, buffer, uint32_t(ctx.lr));
    }
    (void)bufferSize;
    (void)buffer;
    ReadProfileSettings(ctx, base);
}

// --- Xam notification listeners. A listener has an area mask and a FIFO of (id, param)
// pairs; XNotifyGetNext is a non-blocking poll (r4 = exact-id filter, 0 = any). Nothing is
// queued at boot (reference-runtime behavior); UI shims post XN_SYS_UI (9) when invoked. ---
namespace {
struct XamListener { uint64_t areas; std::deque<std::pair<uint32_t, uint32_t>> queue; };
std::mutex g_lnMutex;
std::unordered_map<uint32_t, XamListener> g_listeners;
uint32_t g_nextListener = 0xD0000001;
} // namespace

void XamNotifyEnqueueEvent(uint32_t id, uint32_t param)
{
    std::lock_guard lk(g_lnMutex);
    // Area formula matches the game's own dispatcher: (id >> 25) & 0x3F.
    for (auto& [h, l] : g_listeners)
        if ((1ull << ((id >> 25) & 0x3F)) & l.areas)
            l.queue.emplace_back(id, param);
}

PPC_FUNC(__imp__XamNotifyCreateListener)
{
    KTrace("XamNotifyCreateListener");
    std::lock_guard lk(g_lnMutex);
    uint32_t h = g_nextListener++;
    auto& l = g_listeners[h];
    l.areas = ctx.r3.u64 ? ctx.r3.u64 : ~0ull;
    // Real XAM broadcasts the current sign-in state to every new listener that watches
    // the system area: XN_SYS_SIGNINCHANGED (0x0000000A) with param = mask of changed
    // user slots. Bit 0 set = user 0 - the handler then queries XamUserGetSigninState(0)
    // and records the locally signed-in profile (the boot save-check requires it).
    if (l.areas & 1ull)
        l.queue.emplace_back(0x0000000Au, 1u);
    // A LIVE listener learns the connection state at once (Xenia's netplay fork: some titles
    // wait for it in their online menus): XN_LIVE_CONNECTIONCHANGED, logon established.
    if ((l.areas & 2ull) && xlive::Enabled())
        l.queue.emplace_back(0x02000001u, 0x001510F0u);
    ctx.r3.u32 = h;
}

// XamAlloc(flags, size, pOut): allocate XAM-owned memory from the guest heap. The save
// system calls this and DEREFERENCES the out pointer - a stub that returns success
// without writing it hands the game a garbage pointer (boot save-check wedge).
PPC_FUNC(__imp__XamAlloc)
{
    KTrace("XamAlloc");
    uint32_t size = ctx.r4.u32 ? ctx.r4.u32 : 16, out = ctx.r5.u32;
    void* p = g_virtualHeap.Alloc(size, 16);
    if (!p || !out) { ctx.r3.u32 = 0x8007000Eu; return; }   // E_OUTOFMEMORY
    std::memset(p, 0, size);
    GWrite32(base, out, static_cast<uint32_t>(static_cast<uint8_t*>(p) - base));
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamFree)
{
    KTrace("XamFree");
    if (ctx.r3.u32) g_virtualHeap.Free(base + ctx.r3.u32);
    ctx.r3.u32 = 0;
}

// XamShowDeviceSelectorUI(user, contentType, contentFlags, cbData, pDeviceId, pOverlapped):
// Select the virtual HDD (device 1). An overlapped call returns ERROR_IO_PENDING,
// even when completion is immediate: the game's save worker explicitly requires
// 997 before polling the completion record. Xenia xam_ui.cc has the same contract.
PPC_FUNC(__imp__XamShowDeviceSelectorUI)
{
    KTrace("XamShowDeviceSelectorUI");
    uint32_t pDeviceId = ctx.r7.u32, ovl = ctx.r8.u32;
    if (!XamGuestRange(pDeviceId, 4)) { ctx.r3.u32 = 87; return; }
    if (ctx.r3.u32 != 0) { ctx.r3.u32 = 1317; return; }
    if (const uint32_t error = XamValidateOverlapped(base, ovl))
    { ctx.r3.u32 = error; return; }
    GWrite32(base, pDeviceId, 1);
    XamNotifyEnqueueEvent(9, 1);           // XN_SYS_UI open
    ctx.r3.u32 = XamCompleteOverlapped(base, ovl, 0, 0, 0);
    XamNotifyEnqueueEvent(9, 0);           // XN_SYS_UI closed
}

// XamContentGetDeviceState(deviceId, pOverlapped): device 1 (virtual HDD) is always ready.
PPC_FUNC(__imp__XamContentGetDeviceState)
{
    KTrace("XamContentGetDeviceState");
    uint32_t ovl = ctx.r4.u32;
    if (const uint32_t error = XamValidateOverlapped(base, ovl))
    { ctx.r3.u32 = error; return; }
    if (ctx.r3.u32 != 1)
    {
        // Xenia xam_content_device.cc: disconnected devices report the specific
        // error synchronously, or FUNCTION_FAILED plus the extended error async.
        ctx.r3.u32 = ovl ? XamCompleteOverlapped(base, ovl, 1627, 0, 1167) : 1167;
        return;
    }
    ctx.r3.u32 = XamCompleteOverlapped(base, ovl, 0, 0, 0);
}

// XamShowSigninUI: with a profile signed in there is nothing to choose - report the same
// notifications the console produces (UI opened, closed, sign-in changed). With nobody signed in
// the system UI (menu_ext.cpp) signs the last used profile in and posts them: the game asks from
// MainMenuMode's sign-in states, which draw nothing. (Players choose on the title logo instead,
// where START / A open Select Profile before the game sees them.)
PPC_FUNC(__imp__XamShowSigninUI)
{
    KTrace("XamShowSigninUI");
    std::fprintf(stderr, "[xam] XamShowSigninUI(panes=%u, flags=%08X) lr=%08X signed-in=%d\n", ctx.r3.u32, ctx.r4.u32,
                 (uint32_t)ctx.lr, int(User0SignedIn()));
    if (!User0SignedIn() && SysUiRequestSignIn())
    {
        ctx.r3.u32 = 0;
        return;
    }
    XamNotifyEnqueueEvent(9, 1);            // XN_SYS_UI open
    XamNotifyEnqueueEvent(9, 0);            // XN_SYS_UI closed
    XamNotifyEnqueueEvent(0x0000000Au, 1);  // XN_SYS_SIGNINCHANGED, user 0 changed
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XNotifyGetNext)
{
    KTrace("XNotifyGetNext");
    uint32_t handle = ctx.r3.u32, filter = ctx.r4.u32;
    uint32_t idOut = ctx.r5.u32, paramOut = ctx.r6.u32;
    XLivePollInvites();                                     // a Steam "Join Game" -> invite accepted
    std::lock_guard lk(g_lnMutex);
    auto it = g_listeners.find(handle);
    ctx.r3.u32 = 0;
    if (it == g_listeners.end()) return;
    auto& q = it->second.queue;
    for (size_t i = 0; i < q.size(); ++i)
    {
        if (filter == 0 || q[i].first == filter)
        {
            GWrite32(base, idOut, q[i].first);
            GWrite32(base, paramOut, q[i].second);
            q.erase(q.begin() + i);
            ctx.r3.u32 = 1;
            return;
        }
    }
}

// --- XMsg in-process calls (XAM app messages). 0x7001B = XGI/Live query whose param
// block's second dword points at an 8-byte output the game expects zeroed. ---
PPC_FUNC(__imp__XMsgInProcessCall)
{
    KTrace("XMsgInProcessCall");
    uint32_t app = ctx.r3.u32, message = ctx.r4.u32, param1 = ctx.r5.u32;
    static const bool trace = std::getenv("TTT2_XAM_TRACE") != nullptr;
    static int logged = 0;
    if (trace && (logged < 40 || message != 0x7001B))
    {
        ++logged;
        std::fprintf(stderr, "[xam] XMsgInProcessCall app=%08X msg=%08X lr=%08X\n", app, message, (uint32_t)ctx.lr);
    }
    // One-shot guest backtrace of the polling thread: walk the PPC backchain
    // ([r1] = caller frame; saved LR at [callerFrame - 8]) to find the boot loop.
    static std::atomic<int> calls{0};
    if (trace && ++calls == 500)
    {
        char line[512];
        int n = std::snprintf(line, sizeof(line), "[xam] poll backtrace: lr=%08X", (uint32_t)ctx.lr);
        uint32_t frame = ctx.r1.u32;
        for (int i = 0; i < 12 && frame && frame < 0xFFFFFFF0u; ++i)
        {
            uint32_t prev = GRead32(g_base, frame);
            if (!prev || prev <= frame || prev - frame > 0x10000) break;
            uint32_t savedLr = GRead32(g_base, prev - 8);
            n += std::snprintf(line + n, sizeof(line) - n, " %08X", savedLr);
            frame = prev;
        }
        std::snprintf(line + n, sizeof(line) - n, "\n");
        std::fputs(line, stderr);
    }
    if (app == 0xFC && message == 0x00058035)
    {
        // XStorageBuildServerPath(ByXuid), marshalled by the XDK library (sub_828546C0): param
        // block {+0 user, +8 xuid, +0x10 facility, +0x14 save-game ptr, +0x18 size, +0x1C item
        // name (wide), +0x20 out path (wide), +0x24 out length}. The path only has to round-trip
        // to the XStorage calls (title_server.cpp), which match on the item name and, for a
        // per-user item (facility 3: licence.dat), on its owner: the XUID goes in the path, as
        // the game views other players' licenses by their XUID.
        const uint32_t facility = GRead32(base, param1 + 0x10), item = GRead32(base, param1 + 0x1C);
        const uint32_t out = GRead32(base, param1 + 0x20), outLength = GRead32(base, param1 + 0x24);
        if (!item || !out) { ctx.r3.u32 = 0x80070057u; return; }
        const uint64_t owner = uint64_t(GRead32(base, param1 + 8)) << 32 | GRead32(base, param1 + 12);
        char prefix[64];
        if (facility == 3 && owner)
            std::snprintf(prefix, sizeof(prefix), "/storage/%08X/%u/%016llX/", 0x4E4D0859u, facility, (unsigned long long)owner);
        else
            std::snprintf(prefix, sizeof(prefix), "/storage/%08X/%u/", 0x4E4D0859u, facility);
        std::u16string path;
        for (const char* c = prefix; *c; ++c) path += char16_t(*c);
        for (uint32_t i = 0; i < 96; ++i)
        {
            const uint16_t c = GRead16(base, item + i * 2);
            if (!c) break;
            path += char16_t(c);
        }
        constexpr size_t CAPACITY = 128;                            // the game clears 256 bytes
        if (path.size() + 1 > CAPACITY) { ctx.r3.u32 = 0x8007007Au; return; }
        for (size_t i = 0; i < path.size(); ++i) GWrite16(base, out + uint32_t(i * 2), uint16_t(path[i]));
        GWrite16(base, out + uint32_t(path.size() * 2), 0);
        if (outLength) GWrite32(base, outLength, uint32_t(path.size() + 1));
        ctx.r3.u32 = 0;
        return;
    }
    if (app == 0xFB || app == 0xFC || app == 0xFE)
    {
        // The port's Xbox LIVE (xlive.cpp): logon status, NAT type, mute list, deployment type...
        bool handled = false;
        const uint32_t hr = XLiveDispatch(base, app, message, param1, ctx.r6.u32, handled);
        if (handled) { ctx.r3.u32 = hr; return; }
        std::fprintf(stderr, "[xam] XMsgInProcessCall app=%02X msg=%08X not handled\n", app, message);
    }
    if (message == 0x7001B)
    {
        // XMP (app 0xFA, the music player) "get playback controller" - not Live (the online recon,
        // 2026-10-12, corrected the old reading). Answer with a hard
        // error: Live is absent, take the offline path (we target LAN, not Live).
        ctx.r3.u32 = 0xFFFFFFFF;
        return;
    }
    ctx.r3.u32 = 0;
}

// --- RtlTimeToTimeFields(time, fields): 100ns-since-1601 -> {Year,Month,Day,Hour,Min,Sec,Ms,Weekday} ---
// RtlFillMemoryUlong(r3=dest, r4=length bytes, r5=32-bit pattern)
PPC_FUNC(__imp__RtlFillMemoryUlong)
{
    KTrace("RtlFillMemoryUlong");
    uint32_t dst = ctx.r3.u32, len = ctx.r4.u32 & ~3u, pat = ctx.r5.u32;
    for (uint32_t i = 0; i < len; i += 4) GWrite32(base, dst + i, pat);
}

PPC_FUNC(__imp__RtlTimeToTimeFields)
{
    KTrace("RtlTimeToTimeFields");
    uint32_t tp = ctx.r3.u32, fp = ctx.r4.u32;
    uint64_t t = (static_cast<uint64_t>(GRead32(base, tp)) << 32) | GRead32(base, tp + 4);
    uint32_t ms  = static_cast<uint32_t>((t / 10000ull) % 1000ull);
    uint64_t secs = t / 10000000ull;
    uint32_t sec = static_cast<uint32_t>(secs % 60), min = static_cast<uint32_t>((secs / 60) % 60);
    uint32_t hour = static_cast<uint32_t>((secs / 3600) % 24);
    int64_t days1601 = static_cast<int64_t>(secs / 86400);
    int32_t weekday = static_cast<int32_t>((days1601 + 1) % 7);      // 1601-01-01 was a Monday
    // Civil-from-days (Hinnant), shifted from the 1601 epoch to 1970 then to era days.
    int64_t z = days1601 - 134774 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    GWrite16(base, fp + 0,  static_cast<uint16_t>(y));
    GWrite16(base, fp + 2,  static_cast<uint16_t>(m));
    GWrite16(base, fp + 4,  static_cast<uint16_t>(d));
    GWrite16(base, fp + 6,  static_cast<uint16_t>(hour));
    GWrite16(base, fp + 8,  static_cast<uint16_t>(min));
    GWrite16(base, fp + 10, static_cast<uint16_t>(sec));
    GWrite16(base, fp + 12, static_cast<uint16_t>(ms));
    GWrite16(base, fp + 14, static_cast<uint16_t>(weekday));
}

// --- RtlRaiseException: log and continue. 0x406D1388 is the MS thread-naming exception,
// raised routinely at thread start; a debugger would swallow it and resume. ---
PPC_FUNC(__imp__RtlRaiseException)
{
    KTrace("RtlRaiseException");
    uint32_t rec = ctx.r3.u32;
    uint32_t code = rec ? GRead32(base, rec) : 0;
    if (code != 0x406D1388)
        std::fprintf(stderr, "[kernel] RtlRaiseException code=%08X (continuing)\n", code);
}

PPC_FUNC(__imp__ExRegisterTitleTerminateNotification)
{
    KTrace("ExRegisterTitleTerminateNotification"); ctx.r3.u32 = 0; }

// --- input: SDL gamepads plus focused keyboard controls for player 0 ---
// Guest users 0-3 receive Xbox-style pad values from SDL, assigned by use rather than
// by SDL host slot: the first pad to press a button or trigger becomes user 0, the only
// signed-in profile, the next one user 1, and so on (see PadPollState::guestOf).
//
// The guest polls inside a critical section shared with its main loop. The host
// poll thread reads SDL's event snapshot rather than querying any device driver.

namespace {
struct PadSnapshot
{
    std::atomic<bool> connected{false};
    std::mutex m;
    XboxPadState state{};
    XboxPadCapabilities caps{};
    bool physical = false; // protected by m; separate from logical connected state
};
PadSnapshot g_pads[4];
std::atomic<bool> g_padThreadStarted{false};

// Keyboard controls augment player 0 even when a controller is connected. Physical
// analog controls remain intact. Only accept keys while our game is foreground,
// so typing in another application cannot operate the game.
// Every control has the key chosen in Controller Setup -> KEYBOARD (keyboard_keys.h; by default
// arrows = D-pad, A/S/Z/X = A/B/X/Y, Q/W = shoulders, E/R = triggers, D/F = stick clicks,
// Enter = START, Backspace = BACK); Space is an extra START while no control has it.
struct KeyboardPad { uint16_t buttons = 0; bool leftTrigger = false, rightTrigger = false; };
static KeyboardPad ReadKeyboardPad(const host_input::Snapshot& host)
{
    KeyboardPad pad;
    if (!host.focused) return pad;
    auto down = [&host](int vk) { return vk > 0 && vk < 256 && host.keys[vk]; };
    if (SysUiKeyCaptureActive()) return pad;              // the keys are being chosen
    if (SysUiTextEntryActive())
    {
        // Name entry takes typed characters (WM_CHAR); only the arrows still steer.
        if (down(VK_UP))    pad.buttons |= 0x0001;
        if (down(VK_DOWN))  pad.buttons |= 0x0002;
        if (down(VK_LEFT))  pad.buttons |= 0x0004;
        if (down(VK_RIGHT)) pad.buttons |= 0x0008;
        return pad;
    }
    for (int c = 0; c < keyboard_keys::Count; ++c)
    {
        if (!down(keyboard_keys::Key(c))) continue;
        pad.buttons |= keyboard_keys::Mask(c);
        pad.leftTrigger |= c == keyboard_keys::LT;
        pad.rightTrigger |= c == keyboard_keys::RT;
    }
    if (down(VK_SPACE) && !keyboard_keys::Bound(VK_SPACE)) pad.buttons |= 0x0010;
    return pad;
}

struct PadPollState
{
    input_direction::Filter keyboardDirections;
    bool physicalConnected[4]{};
    bool keyboardEverPressed = false;
    int keyboardGuest = -1;
    uint16_t previousKeyboard = 0;
    uint16_t previousButtons[4]{};
    // Host pad -> guest user, -1 until the pad is first used. A pad claims the lowest user no
    // other claimed pad holds on its first button or trigger press (sticks drift, so they do
    // not count), and keeps it across disconnects. Whoever picks up a controller is player 1
    // whatever its XInput slot: with an idle device on slot 0 the real pad used to be user 1,
    // which TTT2 treats as signed out - no profile, no save, no title-storage unlock. Pads not
    // claimed yet fill the free users in host order, so a lone pad is user 0 before any press.
    int guestOf[4]{ -1, -1, -1, -1 };
};

// Scripted input for unattended runs (skipping movies, driving menus, two-player fights):
//   TTT2_INPUT_SCRIPT="8000:START:150,12000:A:150,..."  ms since start : buttons : hold ms
//   C:/ttt2/port/pad_inject.txt  lines "BUTTONS hold_ms [gap_ms]", consumed then deleted
//                                (pad_inject<tag>.txt with TTT2_INSTANCE=<tag>)
// BUTTONS is one or more names joined by '+': START BACK UP DOWN LEFT RIGHT A B X Y LB RB, and LSL LSR LSU LSD
// (the left stick pushed fully left / right / up / down).
// Presses go to player 1 (user 0); a P2 name sends them to player 2 (user 1), e.g.
// "60000:P2+START:150" or "P2+DOWN+A 150" - user 1 then stays connected as a virtual pad.
struct ScriptedPress { uint64_t at = 0, until = 0; uint16_t buttons = 0; int player = 0; uint8_t stick = 0; };
struct PadScript
{
    std::vector<ScriptedPress> presses;
    bool active = false, player2 = false;
    uint64_t injectBusyUntil = 0, nextInjectPoll = 0;
};

static uint16_t ParseButtons(const std::string& names, int* player = nullptr, uint8_t* stick = nullptr)
{
    static const std::pair<const char*, uint8_t> sticks[] = { { "LSL", 1 }, { "LSR", 2 }, { "LSU", 4 }, { "LSD", 8 } };
    static const std::pair<const char*, uint16_t> map[] = {
        { "START", 0x0010 }, { "BACK", 0x0020 }, { "UP", 0x0001 }, { "DOWN", 0x0002 },
        { "LEFT", 0x0004 }, { "RIGHT", 0x0008 }, { "A", 0x1000 }, { "B", 0x2000 },
        { "X", 0x4000 }, { "Y", 0x8000 }, { "LB", 0x0100 }, { "RB", 0x0200 },
        { "LS", 0x0040 }, { "RS", 0x0080 } };
    uint16_t b = 0;
    size_t pos = 0;
    while (pos <= names.size())
    {
        const size_t end = (std::min)(names.find('+', pos), names.size());
        const std::string name = names.substr(pos, end - pos);
        for (const auto& [n, v] : map)
            if (name == n) b |= v;
        if (name == "P2" && player) *player = 1;
        for (const auto& [n, v] : sticks)
            if (name == n && stick) *stick |= v;
        pos = end + 1;
    }
    return b;
}

static void LoadScript(PadScript& s, const char* text)
{
    if (!text || !*text) return;
    std::string all(text);
    size_t pos = 0;
    while (pos < all.size())
    {
        const size_t end = (std::min)(all.find(',', pos), all.size());
        const std::string item = all.substr(pos, end - pos);
        const size_t c1 = item.find(':'), c2 = item.find(':', c1 == std::string::npos ? 0 : c1 + 1);
        if (c1 != std::string::npos && c2 != std::string::npos)
        {
            ScriptedPress p;
            p.at = std::strtoull(item.c_str(), nullptr, 10);
            p.buttons = ParseButtons(item.substr(c1 + 1, c2 - c1 - 1), &p.player, &p.stick);
            p.until = p.at + std::strtoull(item.c_str() + c2 + 1, nullptr, 10);
            if (p.buttons || p.stick) s.presses.push_back(p);
            s.player2 |= (p.buttons || p.stick) && p.player == 1;
        }
        pos = end + 1;
    }
    s.active = !s.presses.empty();
    std::fprintf(stderr, "[input] script: %zu timed presses\n", s.presses.size());
}

// Queue the injection file's presses back to back from `now`, then delete the file.
static void PollInjectFile(PadScript& s, uint64_t now)
{
    if (now < s.nextInjectPoll) return;
    s.nextInjectPoll = now + 100;
    // TTT2_INSTANCE=<tag>: pad_inject<tag>.txt, so two instances can be driven separately.
    static const std::string injectPath = [] {
        const char* tag = std::getenv("TTT2_INSTANCE");
#if defined(__ANDROID__)
        // ADB can place a command file here while the SDL activity is running.
        if (const char* root = std::getenv("TTT2_DEBUG_ROOT"); root && *root)
            return (std::filesystem::path(root).parent_path() / "config" /
                    (std::string("pad_inject") + (tag ? tag : "") + ".txt")).string();
#endif
        return std::string("C:/ttt2/port/pad_inject") + (tag ? tag : "") + ".txt";
    }();
    const char* path = injectPath.c_str();
    FILE* f = std::fopen(path, "r");
    if (!f) return;
    std::erase_if(s.presses, [now](const ScriptedPress& p) { return p.until < now; });
    char line[128];
    uint64_t at = (std::max)(now, s.injectBusyUntil), previous = at;
    static int logged = 0;
    while (std::fgets(line, sizeof(line), f))
    {
        char names[96] = {};
        unsigned hold = 150, gap = 250;
        if (std::sscanf(line, "%95s %u %u", names, &hold, &gap) < 1) continue;
        // "&BUTTONS ..." starts together with the line before (both players at once).
        const bool together = names[0] == '&';
        int player = 0;
        uint8_t stick = 0;
        const uint16_t b = ParseButtons(names + together, &player, &stick);
        if (!b && !stick) continue;
        const uint64_t start = together ? previous : at;
        s.presses.push_back({ start, start + hold, b, player, stick });
        s.player2 |= player == 1;
        if (logged < 2000 && ++logged)
            std::fprintf(stderr, "[input] inject %s at +%llums hold %u\n", names,
                         (unsigned long long)(start - now), hold);
        previous = start;
        at = (std::max)(at, start + hold + gap);
    }
    std::fclose(f);
    std::remove(path);
    s.injectBusyUntil = at;
    s.active = true;
}

static uint16_t ScriptButtons(PadScript& s, uint64_t now, int player = 0, uint8_t* stick = nullptr)
{
    if (player == 0) PollInjectFile(s, now);
    uint16_t b = 0;
    for (const ScriptedPress& p : s.presses)
        if (p.player == player && now >= p.at && now < p.until)
        {
            b |= p.buttons;
            if (stick) *stick |= p.stick;
        }
    return b;
}
// A scripted left stick (LSL / LSR / LSU / LSD bits) on a player's pad state.
static void ApplyScriptStick(XboxPadState& state, uint8_t stick)
{
    if (stick & 1) state.Gamepad.sThumbLX = -32768;
    if (stick & 2) state.Gamepad.sThumbLX = 32767;
    if (stick & 4) state.Gamepad.sThumbLY = 32767;
    if (stick & 8) state.Gamepad.sThumbLY = -32768;
}

void PublishPad(uint32_t index, bool connected, bool physical,
                const XboxPadState& state, const XboxPadCapabilities& caps)
{
    auto& pad = g_pads[index];
    std::lock_guard lk(pad.m);
    // One guest packet sequence spans keyboard and physical input. Raw host and
    // keyboard packet counters may coincide at a source change, hiding a button
    // release or press from consumers that skip unchanged packet numbers.
    uint32_t packet = pad.state.dwPacketNumber;
    if (connected != pad.connected.load() || physical != pad.physical ||
        std::memcmp(&state.Gamepad, &pad.state.Gamepad, sizeof(state.Gamepad)) != 0)
    {
        ++packet;
        static const bool timing = [] { const char* e = std::getenv("TTT2_INPUT_TIMING"); return e && *e == '1'; }();
        if (timing && (state.Gamepad.wButtons || pad.state.Gamepad.wButtons))
            std::fprintf(stderr, "[input] snapshot user=%u connected=%u physical=%u buttons=%04X\n",
                         unsigned(index), unsigned(connected), unsigned(physical), state.Gamepad.wButtons);
    }
    pad.state = state;
    pad.state.dwPacketNumber = packet;
    pad.caps = caps;
    pad.physical = physical;
    pad.connected.store(connected);
}

void PollPadsOnce(PadPollState& polling, bool autoStart, uint64_t elapsedMs, PadScript* script = nullptr)
{
    // AUTOSTART is an explicit unattended test mode. Apply its START pulse to
    // controller 0 even when a physical pad is present or the window is inactive.
    const bool startPulse = autoStart && elapsedMs > 45000 && (elapsedMs % 9000) < 300;
    // Scripted presses join the keyboard's (controller 0, foreground not required).
    const host_input::Snapshot host = host_input::Read();
    KeyboardPad keyboard = ReadKeyboardPad(host);
    keyboard.buttons = polling.keyboardDirections.Apply(keyboard.buttons, keyboard_keys::SOCD());
    uint8_t stick1 = 0, stick2 = 0;
    const uint16_t keyboardButtons = keyboard.buttons | (script ? ScriptButtons(*script, elapsedMs, 0, &stick1) : 0);
    {
        // Input latency tests: when player 1's buttons change, on the [burst] lines' clock.
        static uint16_t shown = 0;
        if (keyboardButtons != shown && script && script->active)
            std::fprintf(stderr, "[input] buttons %04X visible at %.1f ms\n", keyboardButtons,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count());
        shown = keyboardButtons;
    }
    const uint16_t player2Buttons = script ? ScriptButtons(*script, elapsedMs, 1, &stick2) : 0;
    if (script && script->active) autoStart = true;        // keep pad 0 connected for the script
    polling.keyboardEverPressed |= keyboardButtons != 0 || keyboard.leftTrigger || keyboard.rightTrigger;
    XboxPadState sampled[4]{};
    XboxPadCapabilities hostCaps[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        const auto& p = host.pads[i];
        polling.physicalConnected[i] = p.connected;
        if (!p.connected) continue;
        auto& g = sampled[i].Gamepad;
        g.wButtons = p.buttons;
        g.bLeftTrigger = p.leftTrigger;
        g.bRightTrigger = p.rightTrigger;
        g.sThumbLX = p.leftX;
        g.sThumbLY = p.leftY;
        g.sThumbRX = p.rightX;
        g.sThumbRY = p.rightY;
        g.sThumbLX=input_direction::Stick(g.sThumbLX,keyboard_keys::DPadOnly(),keyboard_keys::StickDeadzone());
        g.sThumbLY=input_direction::Stick(g.sThumbLY,keyboard_keys::DPadOnly(),keyboard_keys::StickDeadzone());
        input_direction::TuneDiagonal(g.sThumbLX,g.sThumbLY,keyboard_keys::StickDiagonal());
        auto& caps = hostCaps[i];
        caps.Type = XboxPadDeviceType;
        caps.SubType = XboxPadDeviceSubtype;
        caps.Gamepad.wButtons = 0xF3FF;
        caps.Gamepad.bLeftTrigger = caps.Gamepad.bRightTrigger = 255;
        caps.Gamepad.sThumbLX = caps.Gamepad.sThumbLY = 32767;
        caps.Gamepad.sThumbRX = caps.Gamepad.sThumbRY = 32767;
    }

    const bool dynamic = keyboard_keys::SeparatePlayer();
    auto claimed = [&](int user) {
        if (dynamic && polling.keyboardGuest == user) return true;
        for (uint32_t j = 0; j < 4; ++j)
            if (polling.guestOf[j] == user) return true;
        return false;
    };
    // Keyboard wins a same-poll tie; otherwise ownership follows the first sampled edge.
    // An idle device never reserves a slot. After P1, only START joins another player.
    const uint16_t keyboardEdge = keyboard.buttons & ~polling.previousKeyboard;
    polling.previousKeyboard = keyboard.buttons;
    if (dynamic && polling.keyboardGuest < 0 && keyboardEdge &&
        (claimed(0) || keyboard_keys::PlayerOnePreference() != 2) &&
        (keyboardEdge & 0x0010))
        for (int user = 0; user < 4; ++user)
            if (!claimed(user)) { polling.keyboardGuest = user; break; }
    for (uint32_t i = 0; i < 4; ++i)
    {
        const XboxPadData& g = sampled[i].Gamepad;
        const uint16_t edge = g.wButtons & ~polling.previousButtons[i];
        polling.previousButtons[i] = g.wButtons;
        const bool used = g.wButtons || g.bLeftTrigger > XboxPadTriggerThreshold ||
                          g.bRightTrigger > XboxPadTriggerThreshold;
        if (!polling.physicalConnected[i] || polling.guestOf[i] >= 0 || !used) continue;
        if (dynamic && (!(edge & 0x0010) || (!claimed(0) && keyboard_keys::PlayerOnePreference() == 1))) continue;
        for (int user = 0; user < 4; ++user)
            if (!claimed(user)) { polling.guestOf[i] = user; break; }
    }
    // Users 1-3 sign in as guests on their controller's first use (player 2's scripted presses count).
    uint32_t used = (script && script->player2 && player2Buttons) ? 2u : 0u;
    if (dynamic && polling.keyboardGuest > 0) used |= 1u << polling.keyboardGuest;
    for (uint32_t i = 0; i < 4; ++i)
        if (polling.guestOf[i] > 0) used |= 1u << polling.guestOf[i];
    if (const uint32_t added = used & ~g_usedUsers.fetch_or(used))
        for (uint32_t user = 1; user < 4; ++user)
            if (added >> user & 1 && User0SignedIn())
            {
                std::fprintf(stderr, "[xam] user %u signed in: '%s', a guest of the profile\n", user, UserName(user).c_str());
                XamNotifyEnqueueEvent(0x0000000Au, 1u << user);   // XN_SYS_SIGNINCHANGED
            }
    int source[4]{ -1, -1, -1, -1 };   // guest user -> host pad
    for (uint32_t i = 0; i < 4; ++i)
        if (polling.guestOf[i] >= 0 && polling.physicalConnected[i]) source[polling.guestOf[i]] = int(i);
    for (uint32_t i = 0; i < 4; ++i)
    {
        if (dynamic || polling.guestOf[i] >= 0 || !polling.physicalConnected[i]) continue;
        for (int user = 0; user < 4; ++user)
            if (source[user] < 0 && !claimed(user)) { source[user] = int(i); break; }
    }

    for (uint32_t user = 0; user < 4; ++user)
    {
        XboxPadState state{};
        XboxPadCapabilities caps{};
        const bool physical = source[user] >= 0;
        bool connected = physical;
        if (physical)
        {
            state = sampled[source[user]];
            caps = hostCaps[source[user]];
        }
        if (user == 1 && script && script->player2)
        {
            state.Gamepad.wButtons |= player2Buttons;
            ApplyScriptStick(state, stick2);
            caps.Gamepad.wButtons |= 0xF33F;
            if (!physical)
            {
                connected = true;
                caps.Type = XboxPadDeviceType;
                caps.SubType = XboxPadDeviceSubtype;
            }
        }
        const int keyboardUser = dynamic ? polling.keyboardGuest : 0;
        if (int(user) == keyboardUser || (user == 0 && (autoStart || script)))
        {
            const bool ownsKeyboard = int(user) == keyboardUser;
            uint16_t buttons = ownsKeyboard ? keyboard.buttons : 0;
            if (user == 0) buttons |= keyboardButtons & ~keyboard.buttons;
            // Preserve unattended START pulses: after 45 s, 300 ms down every 9 s.
            if (startPulse)
                buttons |= 0x0010;
            state.Gamepad.wButtons |= buttons;
            ApplyScriptStick(state, stick1);
            if (ownsKeyboard && keyboard.leftTrigger) state.Gamepad.bLeftTrigger = 255;
            if (ownsKeyboard && keyboard.rightTrigger) state.Gamepad.bRightTrigger = 255;
            caps.Gamepad.wButtons |= 0xF3FF; // supported mapped controls, independent of current presses
            caps.Gamepad.bLeftTrigger = caps.Gamepad.bRightTrigger = 255;
            if (!physical)
            {
                polling.keyboardEverPressed |= buttons != 0 || keyboard.leftTrigger || keyboard.rightTrigger;
                connected = dynamic ? ownsKeyboard || autoStart : polling.keyboardEverPressed || autoStart;
                caps.Type = XboxPadDeviceType;
                caps.SubType = XboxPadDeviceSubtype;
            }
        }
        // Availability is not ownership: VS/PAIR checks connected-device count
        // before showing the join screen. Advertise idle slots with neutral input,
        // but only START assigns a device and signs in a guest.
        if (dynamic && user > 0 && !claimed(int(user)) && claimed(0)) {
            unsigned available = 1; // keyboard is always an available local device
            for (bool present : polling.physicalConnected) available += present;
            if (user < available) {
                connected = true;
                caps.Type = XboxPadDeviceType;
                caps.SubType = XboxPadDeviceSubtype;
                caps.Gamepad.wButtons = 0xF3FF;
            }
        }
        PublishPad(user, connected, physical, state, caps);
    }
}

static bool AutoStartEnabled(const char* value)
{
    // Require the documented opt-in value. An empty inherited environment entry
    // (including one left by PowerShell) must not skip movies or title screens.
    return value && std::strcmp(value, "1") == 0;
}

void PadPollThread()
{
    PadPollState polling;
    const bool autoStart = AutoStartEnabled(std::getenv("TTT2_AUTOSTART"));
    std::fprintf(stderr, "[input] automatic Start %s\n", autoStart ? "enabled" : "disabled");
    PadScript script;
    LoadScript(script, std::getenv("TTT2_INPUT_SCRIPT"));
    const auto started = std::chrono::steady_clock::now();
    for (;;)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        PollPadsOnce(polling, autoStart, uint64_t(elapsed), &script);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void EnsurePadThread()
{
    if (!g_padThreadStarted.exchange(true))
        std::thread(PadPollThread).detach();
}

std::atomic<int64_t> g_startPulseUntilMs{ 0 };
std::atomic<int64_t> g_downPulseUntilMs{ 0 };
int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

bool HostCheatOverlayChord()
{
    EnsurePadThread();
    for (auto& pad : g_pads) {
        std::lock_guard lock(pad.m);
        if (pad.connected.load() && (pad.state.Gamepad.wButtons & 0x00C0) == 0x00C0) return true;
    }
    return false;
}

// The system UI's view of the controllers (sys_ui.h): every connected pad's buttons (keyboard
// included) with the left stick as a D-pad - read even while the game is given neutral input.
uint16_t HostControllerButtons()
{
    EnsurePadThread();
    uint16_t b = 0;
    for (auto& pad : g_pads)
    {
        if (!pad.connected.load()) continue;
        std::lock_guard lk(pad.m);
        const XboxPadData& g = pad.state.Gamepad;
        b |= g.wButtons;
        if (g.sThumbLY > 16000) b |= 0x0001;
        if (g.sThumbLY < -16000) b |= 0x0002;
        if (g.sThumbLX < -16000) b |= 0x0004;
        if (g.sThumbLX > 16000) b |= 0x0008;
    }
    static const bool timing = [] { const char* e = std::getenv("TTT2_INPUT_TIMING"); return e && *e == '1'; }();
    static uint16_t last = 0;
    if (timing && b != last)
        std::fprintf(stderr, "[input] system UI buttons %04X\n", b);
    last = b;
    return b;
}

// True while a connected pad (keyboard included) holds a trigger past the XInput threshold.
bool HostTriggerHeld()
{
    EnsurePadThread();
    for (auto& pad : g_pads)
    {
        if (!pad.connected.load()) continue;
        std::lock_guard lk(pad.m);
        const XboxPadData& g = pad.state.Gamepad;
        if (g.bLeftTrigger > XboxPadTriggerThreshold || g.bRightTrigger > XboxPadTriggerThreshold) return true;
    }
    return false;
}

// Player 1 presses START for 150 ms (the title continues after a profile signs in).
void HostPulseStart() { g_startPulseUntilMs = NowMs() + 150; }

// Player 1 presses DOWN for 80 ms: one step of a menu's cursor, drawn by the menu itself (the server UI moves the
// ONLINE MODE cursor off a row that needs a server).
void HostPulseDown() { g_downPulseUntilMs = NowMs() + 80; }

PPC_FUNC(__imp__XamInputGetCapabilities)
{
    KTrace("XamInputGetCapabilities");
    // r3=userIndex, r4=flags, r5=XboxPadCapabilities* out
    EnsurePadThread();
    uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
    XboxPadCapabilities caps;
    if (user > 3 || !out || !g_pads[user].connected.load())
    {
        ctx.r3.u32 = 0x48F;   // ERROR_DEVICE_NOT_CONNECTED
        return;
    }
    {
        std::lock_guard lk(g_pads[user].m);
        caps = g_pads[user].caps;
    }
    GWrite8(base, out + 0, caps.Type);
    GWrite8(base, out + 1, caps.SubType);
    GWrite16(base, out + 2, caps.Flags);
    GWrite16(base, out + 4, caps.Gamepad.wButtons);
    GWrite8(base, out + 6, caps.Gamepad.bLeftTrigger);
    GWrite8(base, out + 7, caps.Gamepad.bRightTrigger);
    GWrite16(base, out + 8, (uint16_t)caps.Gamepad.sThumbLX);
    GWrite16(base, out + 10, (uint16_t)caps.Gamepad.sThumbLY);
    GWrite16(base, out + 12, (uint16_t)caps.Gamepad.sThumbRX);
    GWrite16(base, out + 14, (uint16_t)caps.Gamepad.sThumbRY);
    GWrite16(base, out + 16, caps.Vibration.wLeftMotorSpeed);
    GWrite16(base, out + 18, caps.Vibration.wRightMotorSpeed);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamInputGetState)
{
    KTrace("XamInputGetState");
    // r3=userIndex, r4=flags, r5=XboxPadState* out
    EnsurePadThread();
    uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
    XboxPadState st{};
    const bool sharedMenu = user == 0 && keyboard_keys::SeparatePlayer() && g_sharedMenuInput.load();
    if (user > 3 || !out || (!g_pads[user].connected.load() && !sharedMenu))
    {
        ctx.r3.u32 = 0x48F;
        return;
    }
    {
        std::lock_guard lk(g_pads[user].m);
        if (g_pads[user].connected.load()) st = g_pads[user].state;
    }
    // Main/options navigation accepts either joined player. Combat and character
    // selection retain their own snapshots; never merge pads in an unknown frame.
    if (user == 0 && keyboard_keys::SeparatePlayer() && g_sharedMenuInput.load())
        for (unsigned other = 1; other < 4; ++other) {
            std::lock_guard lk(g_pads[other].m);
            if (!g_pads[other].connected.load()) continue;
            const auto& peer = g_pads[other].state;
            st.dwPacketNumber += peer.dwPacketNumber;
            st.Gamepad.wButtons |= peer.Gamepad.wButtons;
            st.Gamepad.bLeftTrigger = (std::max)(st.Gamepad.bLeftTrigger, peer.Gamepad.bLeftTrigger);
            st.Gamepad.bRightTrigger = (std::max)(st.Gamepad.bRightTrigger, peer.Gamepad.bRightTrigger);
            if (std::abs(int(peer.Gamepad.sThumbLX)) > std::abs(int(st.Gamepad.sThumbLX))) st.Gamepad.sThumbLX = peer.Gamepad.sThumbLX;
            if (std::abs(int(peer.Gamepad.sThumbLY)) > std::abs(int(st.Gamepad.sThumbLY))) st.Gamepad.sThumbLY = peer.Gamepad.sThumbLY;
        }
    if (cheat_overlay::ConsumeChord() && (st.Gamepad.wButtons & 0x00C0) == 0x00C0)
        st.Gamepad.wButtons &= ~uint16_t(0x00C0);
    if (SysUiOwnsInput())
        st.Gamepad = XboxPadData{};          // a system screen has the controller
    else if (SysUiTitleWantsProfile())
        st.Gamepad.wButtons &= ~uint16_t(0x0010 | 0x1000);   // START / A open Select Profile instead
    else if (const uint16_t mask = SysUiGameButtonMask(); user == 0 && mask != 0xFFFF)
    {
        // Controller Setup's KEYBOARD mode takes the other buttons (and the stick's LEFT / RIGHT); the server list
        // takes UP / DOWN at its ends (and the stick's UP / DOWN with them).
        st.Gamepad.wButtons &= mask;
        st.Gamepad.bLeftTrigger = st.Gamepad.bRightTrigger = 0;
        st.Gamepad.sThumbLX = 0;
        if ((mask & 0x0003) != 0x0003) st.Gamepad.sThumbLY = 0;
    }
    else if (user == 0 && NowMs() < g_startPulseUntilMs.load())
    {
        st.Gamepad.wButtons |= 0x0010;
        st.dwPacketNumber += 0x10000;           // a new packet while the pulse lasts
    }
    else if (user == 0 && NowMs() < g_downPulseUntilMs.load())
    {
        st.Gamepad.wButtons |= 0x0002;          // DPAD_DOWN
        st.dwPacketNumber += 0x20000;
    }
    if (user == 0)
    {
        // Input latency tests: the first read that returns new buttons (same clock as [burst]).
        static uint16_t lastRead = 0;
        static const bool timing = [] { const char* e = std::getenv("TTT2_INPUT_TIMING"); return e && *e == '1'; }();
        if (timing && st.Gamepad.wButtons != lastRead)
            std::fprintf(stderr, "[input] game read %04X at %.1f ms\n", st.Gamepad.wButtons,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count());
        lastRead = st.Gamepad.wButtons;
    }
    GWrite32(base, out + 0, st.dwPacketNumber);
    GWrite16(base, out + 4, st.Gamepad.wButtons);
    GWrite8(base, out + 6, st.Gamepad.bLeftTrigger);
    GWrite8(base, out + 7, st.Gamepad.bRightTrigger);
    GWrite16(base, out + 8, (uint16_t)st.Gamepad.sThumbLX);
    GWrite16(base, out + 10, (uint16_t)st.Gamepad.sThumbLY);
    GWrite16(base, out + 12, (uint16_t)st.Gamepad.sThumbRX);
    GWrite16(base, out + 14, (uint16_t)st.Gamepad.sThumbRY);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XamInputGetKeystrokeEx)
{
    KTrace("XamInputGetKeystrokeEx");
    // r3=userIndex* in/out, r4=device flags, r5=XINPUT_KEYSTROKE* out.
    // This runtime publishes controller state, but has no queued text/key events.
    // Success would falsely claim an event and make the guest consume stale bytes.
    // XAM's non-gamepad filter matches Xenia's xam_input.cc contract.
    uint32_t userPtr = ctx.r3.u32, flags = ctx.r4.u32, out = ctx.r5.u32;
    if (!userPtr || !out)
    {
        ctx.r3.u32 = 160; // ERROR_BAD_ARGUMENTS
        return;
    }
    if ((flags & 0xFF) && !(flags & 1))
    {
        ctx.r3.u32 = 1167; // ERROR_DEVICE_NOT_CONNECTED: no raw keyboard device
        return;
    }
    EnsurePadThread();
    uint32_t user = GRead32(base, userPtr);
    bool connected = false;
    if ((user & 0xFF) == 0xFF || (flags & 0x40000000))
    {
        for (const auto& pad : g_pads) connected |= pad.connected.load();
    }
    else if (user < 4) connected = g_pads[user].connected.load();
    // Preserve both output buffers on failure, including the in/out user index.
    ctx.r3.u32 = connected ? 4306 : 1167; // ERROR_EMPTY / ERROR_DEVICE_NOT_CONNECTED
}

// --- RtlInitAnsiString(out {len16, maxlen16, buf32}, cstr) ---
PPC_FUNC(__imp__RtlInitAnsiString)
{
    KTrace("RtlInitAnsiString");
    uint32_t s = ctx.r3.u32, cstr = ctx.r4.u32;
    uint16_t len = 0;
    if (cstr) { while (g_base[cstr + len] && len < 0xFFFE) ++len; }
    GWrite16(base, s + 0, len);
    GWrite16(base, s + 2, cstr ? len + 1 : 0);
    GWrite32(base, s + 4, cstr);
}

// RtlMultiByteToUnicodeN(dest, destBytes, pWrittenBytes, src, srcBytes) and the reverse.
// Same semantics as Xenia xboxkrnl_rtl.cc: each byte widens to one big-endian UCS-2 unit;
// narrowing maps units above 0xFF to '?'. The game's MultiByteToWideChar wrapper
// (sub_82BEF1C0) uses it for e.g. the title-storage item name; as a stub it left every
// converted name empty.
PPC_FUNC(__imp__RtlMultiByteToUnicodeN)
{
    KTrace("RtlMultiByteToUnicodeN");
    const uint32_t dst = ctx.r3.u32, dstBytes = ctx.r4.u32, written = ctx.r5.u32;
    const uint32_t src = ctx.r6.u32, srcBytes = ctx.r7.u32;
    const uint32_t n = (std::min)(dstBytes >> 1, srcBytes);
    for (uint32_t i = 0; i < n; ++i) GWrite16(base, dst + i * 2, GRead8(base, src + i));
    if (written) GWrite32(base, written, n * 2);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__RtlUnicodeToMultiByteN)
{
    KTrace("RtlUnicodeToMultiByteN");
    const uint32_t dst = ctx.r3.u32, dstBytes = ctx.r4.u32, written = ctx.r5.u32;
    const uint32_t src = ctx.r6.u32, srcBytes = ctx.r7.u32;
    const uint32_t n = (std::min)(srcBytes >> 1, dstBytes);
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint16_t c = GRead16(base, src + i * 2);
        GWrite8(base, dst + i, c < 0x100 ? uint8_t(c) : uint8_t('?'));
    }
    if (written) GWrite32(base, written, n);
    ctx.r3.u32 = 0;
}
