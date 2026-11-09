// Ranks (server/PROTOCOL.md "Ranks", rank_sync.h): the game reads its license once per ONLINE MODE visit
// (OnlineMenuMode::Impl::state_sync_storage, in the visit's opening) and keeps it in memory. Where it read it from -
// a server whose ranks are its own, or this PC - is its home (title_server.cpp). When the network changes inside
// ONLINE MODE (the SERVER row, a lost connection) to one whose home is another, the ONLINE MODE menu sends the game
// through sync_storage again: the menu's state (OnlineMenuMode::Impl::state_mainmenu_main) is switched to
// sync_storage as the game's own states switch (sub_826483C0: the state holder at Impl + 8). The penalties that follow
// it (state_penalize_upload: a ranked match left early, kept with the license) run too, and the state the game picks
// after them is the player panel's load (state_paneldata_load and its _wait: the ranks the menu's panel shows) instead
// of the rest of the opening (stats, ghosts, QoS ...), then the menu's (state_mainmenu_init, which puts the panel up
// with them) and the menu. The game takes no buttons meanwhile.
//
// sync_storage reads only when the storage was not read yet (the byte at [Impl + 136] + 25, which the game itself
// clears when its last read is an hour old): the byte is cleared first.
//
// The two states' functions are taken from the game's own switches to them (each switch names its state first:
// sub_825FE600, then sets it: sub_826483C0), so no address is assumed. TTT2_RANK_TRACE=1 logs OnlineMenuMode's states.
//
// The player's save keeps the license's fields too (MEASURED 2026-10-28): the game copies a license it reads into its
// save object (sub_826DA630 with 0: ranks, points, Battle Points, online counts), builds the license it stores from the
// save object (sub_826DA630 with 1), and builds one from the save when none is found. Each copy into the save object
// asks for the save to be written (sub_8265F628: the save object is copied for the save thread there, at once). So
// while a server's own ranks are in use, the save object holds that server's: the player's own fields are taken aside
// when the first server license is copied in (and the player's own license, as the game builds it from them), and
// every save written meanwhile gets them back for its copy - the save on this PC never holds a server's ranks. A
// license from this PC copied in ends it, and so does the main menu (MainMenuMode's state_mainmenu_main: ONLINE MODE
// left): the player's own fields go back into the save object, for the offline screens (PROFILE's RANK LIST), and
// the license storage is marked not read, so the next ONLINE MODE visit reads the server's license in again before
// anything is built from the save object.
#include "rank_sync.h"
#include "profiles.h"
#include "server_ui.h"
#include "sys_ui.h"
#include "title_server.h"
#include "xlive.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>

extern "C" PPC_FUNC(__imp__sub_826DA630);
extern "C" PPC_FUNC(__imp__sub_8265F5D8);

namespace rank_sync {
namespace {

constexpr const char* kPrefix = "&OnlineMenuMode::Impl::";
constexpr const char* kMain = "&OnlineMenuMode::Impl::state_mainmenu_main";
constexpr const char* kSync = "&OnlineMenuMode::Impl::state_sync_storage";       // and its _wait / _error
constexpr const char* kPenalize = "&OnlineMenuMode::Impl::state_penalize_upload";  // and its _wait
constexpr const char* kPanel = "&OnlineMenuMode::Impl::state_paneldata_load";      // and its _wait
constexpr const char* kMenuInit = "&OnlineMenuMode::Impl::state_mainmenu_init";

struct StateFn { uint64_t r4 = 0, r5 = 0; uint32_t Fn() const { return uint32_t(r4 >> 32); } };   // a state as sub_826483C0 takes it

enum class Phase { Idle, Reading, Panel };          // Panel: the panel's ranks are loaded again after the read

std::mutex g_mutex;                                 // the game's threads name states; the main thread reads
uint32_t g_impl = 0;                                // OnlineMenuMode::Impl (0: not seen)
std::map<uint32_t, std::string> g_lastName;         // per state machine: the state it named last
StateFn g_main, g_sync, g_panel;                    // the menu's state, sync_storage and the panel's load, as set
Phase g_phase = Phase::Idle;
bool g_buttons = true;                              // the game takes buttons (not while the license is read)
int64_t g_since = 0;
bool g_failed = false;                              // the last read failed (the game ended the visit): not again
bool g_checkHome = false;                           // a read just ended: its license must be the network's
uint32_t g_storage = 0;                             // [Impl + 136], the license storage (it outlives the visits)

bool Trace() { static const bool on = [] { const char* e = std::getenv("TTT2_RANK_TRACE"); return e && *e == '1'; }(); return on; }

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// What sub_826483C0 does: the current state becomes the previous (holder + 16), the frames in the state (holder + 32)
// start again, and the new state is current.
void SetState(uint8_t* base, uint32_t holder, const StateFn& s)
{
    uint32_t cur[4];
    for (int i = 0; i < 4; ++i) cur[i] = GRead32(base, holder + 4 * i);
    const uint32_t next[4] = { uint32_t(s.r4 >> 32), uint32_t(s.r4), uint32_t(s.r5 >> 32), uint32_t(s.r5) };
    if (std::equal(cur, cur + 4, next)) return;
    GWrite32(base, holder + 32, 0);
    GWrite32(base, holder + 36, 0);
    for (int i = 0; i < 4; ++i) GWrite32(base, holder + 16 + 4 * i, cur[i]);
    for (int i = 0; i < 4; ++i) GWrite32(base, holder + 4 * i, next[i]);
}

} // namespace

void NoteStateName(uint32_t machine, const std::string& name)
{
    std::lock_guard l(g_mutex);
    if (name.rfind(kPrefix, 0) == 0)
    {
        g_impl = machine;
        g_lastName[machine] = name;
    }
    else if (machine == g_impl)
        g_lastName[machine] = name;
}

// sub_826483C0(state holder, the new state in r4 / r5): OnlineMenuMode's switches are learned (the menu's state,
// sync_storage) and, while the license is read again, the state after sync_storage is the menu.
void OnSetState(PPCContext& ctx)
{
    std::lock_guard l(g_mutex);
    if (!g_impl || ctx.r3.u32 != g_impl + 8) return;
    const auto it = g_lastName.find(g_impl);
    const std::string name = it == g_lastName.end() ? std::string() : it->second;
    if (Trace()) std::fprintf(stderr, "[ranks] state %s (%08X)\n", name.c_str(), uint32_t(ctx.r4.u64 >> 32));
    if (name == kMain) g_main = { ctx.r4.u64, ctx.r5.u64 };
    else if (name == kSync) g_sync = { ctx.r4.u64, ctx.r5.u64 };
    else if (name == kPanel) g_panel = { ctx.r4.u64, ctx.r5.u64 };
    if (g_phase == Phase::Panel)
    {
        if (name.rfind(kPanel, 0) == 0 || name == kMenuInit) return;
        if (name != kMain)
        {
            std::fprintf(stderr, "[ranks] back to the ONLINE MODE menu instead of %s\n", name.c_str());
            ctx.r4.u64 = g_main.r4;
            ctx.r5.u64 = g_main.r5;
        }
        std::fprintf(stderr, "[ranks] the license was read again (%lld ms)\n", (long long)(NowMs() - g_since));
        g_phase = Phase::Idle;
        g_checkHome = true;
        return;
    }
    if (g_phase != Phase::Reading) return;
    const bool sync = name.rfind(kSync, 0) == 0, penalize = name.rfind(kPenalize, 0) == 0;
    if (sync || penalize)
    {
        if (name.find("_error") != std::string::npos)
        {
            // the server did not answer: the game tells the player and ends the visit, as at its opening
            g_phase = Phase::Idle;
            g_failed = true;
            std::fprintf(stderr, "[ranks] the license could not be read again (%s): the game ends the ONLINE MODE visit\n",
                         name.c_str());
        }
        else if (penalize)
            g_buttons = true;                           // a penalty's message wants a button
        return;
    }
    if (!g_main.Fn()) return;
    if (g_panel.Fn())
    {
        // the panel shows the ranks of the license just read
        ctx.r4.u64 = g_panel.r4;
        ctx.r5.u64 = g_panel.r5;
        g_phase = Phase::Panel;
        g_buttons = false;
        return;
    }
    std::fprintf(stderr, "[ranks] the license was read again (%lld ms); back to the ONLINE MODE menu instead of %s\n",
                 (long long)(NowMs() - g_since), name.c_str());
    ctx.r4.u64 = g_main.r4;
    ctx.r5.u64 = g_main.r5;
    g_phase = Phase::Idle;
    g_checkHome = true;
}

void Frame(PPCContext& ctx, uint8_t* base)
{
    (void)ctx;
    {
        std::lock_guard l(g_mutex);
        if (!g_impl || !g_main.Fn() || !g_sync.Fn()) return;
        if (g_phase != Phase::Idle)
        {
            if (NowMs() - g_since > 30000)
            {
                g_phase = Phase::Idle;
                std::fprintf(stderr, "[ranks] reading the license again took too long: given up\n");
            }
            return;
        }
        if (GRead32(base, g_impl + 8) != g_main.Fn())
        {
            g_failed = false;                           // another visit (or another part of ONLINE MODE)
            return;
        }
        if (const uint32_t storage = GRead32(base, g_impl + 136); storage != g_storage)
        {
            if (Trace()) std::fprintf(stderr, "[ranks] license storage %08X (was %08X)\n", storage, g_storage);
            g_storage = storage;
        }
        if (g_failed) return;
    }
    const bool atHome = TitleServerLicenceAtHome();
    {
        std::lock_guard l(g_mutex);
        if (std::exchange(g_checkHome, false) && !atHome)
        {
            // read, but not from the network's home (it changed meanwhile, or the game did not read): not again on
            // this visit to the menu, so the menu never loops; the license kept is not written anywhere else
            g_failed = true;
            std::fprintf(stderr, "[ranks] the license read is not the network's: not read again until the menu is left\n");
            return;
        }
    }
    // on the ONLINE MODE menu itself, nothing being connected or asked, no window up
    if (atHome || !server_ui::MenuSettled() || SysUiBoxBusy()) return;
    std::lock_guard l(g_mutex);
    if (g_phase != Phase::Idle || GRead32(base, g_impl + 8) != g_main.Fn()) return;
    const uint32_t storage = GRead32(base, g_impl + 136);
    if (!storage) return;
    std::fprintf(stderr, "[ranks] the network's license lives elsewhere: ONLINE MODE reads it again\n");
    GWrite8(base, storage + 25, 0);                     // not read yet: sync_storage reads
    SetState(base, g_impl + 8, g_sync);
    g_phase = Phase::Reading;
    g_buttons = false;
    g_since = NowMs();
}

uint16_t GameButtonMask()
{
    std::lock_guard l(g_mutex);
    return g_phase != Phase::Idle && !g_buttons ? 0 : 0xFFFF;
}

namespace {

constexpr uint32_t kSaveHolder = 0x82E70F04;        // the save object's holder (sub_822816C8: made once, flag at + 16)
constexpr uint32_t kBody = 3400;                    // a license without its 20-byte storage header (sub_826CF508)

struct Span { uint16_t at, size; };

// The save object's fields sub_826DA630 copies a license into (with 0): offset, bytes. The rest it writes there comes
// from the team data (license manager + 3408), not the license.
const std::vector<Span>& MirrorSpans()
{
    static const std::vector<Span> spans = [] {
        std::vector<Span> s = { { 12, 4 }, { 16, 1 }, { 88, 2 }, { 90, 2 }, { 95, 1 }, { 96, 1 }, { 97, 1 }, { 592, 4 },
                                { 600, 1 }, { 14012, 4 }, { 14016, 4 }, { 14020, 4 }, { 14040, 32 }, { 14937, 1 },
                                { 14940, 4 }, { 14944, 4 } };
        for (uint16_t c = 0; c < 63; ++c)               // per character: 76 bytes in the save, 48 in the license
        {
            const uint16_t at = uint16_t(9236 + 76 * c);
            for (const Span f : { Span{ 0, 1 }, Span{ 4, 2 }, Span{ 6, 3 }, Span{ 24, 8 } })
                s.push_back({ uint16_t(at + f.at), f.size });
        }
        return s;
    }();
    return spans;
}

struct Mirror
{
    bool server = false;                            // the save object holds a server's license fields
    uint32_t live = 0;                              // the save object last seen (logged when it changes)
    uint64_t xuid = 0;                              // the profile then
    std::vector<uint8_t> own;                       // the player's own fields (MirrorSpans order)
    std::vector<uint8_t> ownBody;                   // the player's own license, as the game built it from them
};
std::mutex g_mirrorMutex;
Mirror g_mirror;

// The save object, as the game finds it (sub_8265F5D8 on the holder: the one it keeps at + 4, else the signed-in
// profile's from its table): 0 when none.
uint32_t LiveSave(PPCContext& ctx, uint8_t* base)
{
    if (!(GRead32(base, kSaveHolder + 16) & 1)) return 0;
    const PPCContext entry = ctx;
    ctx.r3.u64 = kSaveHolder;
    __imp__sub_8265F5D8(ctx, base);
    const uint32_t live = ctx.r3.u32;
    ctx = entry;
    return live;
}

std::vector<uint8_t> ReadSpans(uint8_t* base, uint32_t live)
{
    std::vector<uint8_t> out;
    for (const Span& s : MirrorSpans())
        out.insert(out.end(), base + live + s.at, base + live + s.at + s.size);
    return out;
}

void WriteSpans(uint8_t* base, uint32_t live, const std::vector<uint8_t>& bytes)
{
    size_t i = 0;
    for (const Span& s : MirrorSpans())
    {
        std::copy_n(bytes.data() + i, s.size, base + live + s.at);
        i += s.size;
    }
}

// Mirror::server for the signed-in profile (another profile signed in: dropped). Not tied to one save object: a
// match builds one per side (MEASURED 2026-10-28: the holder's + 12 then picks the player's side in the table; a
// ranked match's 2P side is 1) and the game saves that one, with the server's fields copied in.
bool ServerHere(uint32_t live)
{
    if (g_mirror.server && g_mirror.xuid != profiles::ActiveXuid())
    {
        std::fprintf(stderr, "[ranks] another profile is signed in: the license fields taken aside are dropped\n");
        g_mirror = {};
    }
    if (g_mirror.server && live && live != g_mirror.live)
    {
        std::fprintf(stderr, "[ranks] the save object is now %08X (was %08X)\n", live, g_mirror.live);
        g_mirror.live = live;
    }
    return g_mirror.server && live;
}

} // namespace

std::vector<uint8_t> PackLicence(const uint8_t* body)
{
    constexpr size_t kHeader = 20, kCrcSpan = 8176;
    std::vector<uint8_t> file(kHeader + kBody, 0);
    file[5] = 1;                                    // 1, type 0 (a license), length, an 8-byte time (none)
    file[10] = uint8_t(kBody >> 8);
    file[11] = uint8_t(kBody);
    std::copy_n(body, kBody, file.data() + kHeader);
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0;
    for (size_t i = 4; i < 4 + kCrcSpan; ++i) crc = table[(crc ^ (i < file.size() ? file[i] : 0)) & 0xFF] ^ (crc >> 8);
    for (int i = 0; i < 4; ++i) file[i] = uint8_t(crc >> (24 - 8 * i));
    return file;
}

bool OwnLicence(std::vector<uint8_t>& file)
{
    std::lock_guard l(g_mirrorMutex);
    if (!g_mirror.server || g_mirror.ownBody.size() != kBody) return false;
    file = PackLicence(g_mirror.ownBody.data());
    return true;
}

// sub_826DA630(license manager, 0): a license copied into the save object (the manager's at + 8). The save it asks
// for in there is written with the player's own fields when the license is a server's.
void BeforeLicenceToSave(PPCContext& ctx, uint8_t* base)
{
    const uint32_t manager = ctx.r3.u32;
    const bool fromServer = TitleServerLicenceFromServer();
    const uint32_t live = LiveSave(ctx, base);
    std::lock_guard l(g_mirrorMutex);
    if (!fromServer)
    {
        if (ServerHere(live))
        {
            g_mirror = {};
            std::fprintf(stderr, "[ranks] this PC's license is used again\n");
        }
        return;
    }
    if (ServerHere(live) || !live || !manager) return;
    Mirror m;
    m.server = true;
    m.live = live;
    m.xuid = profiles::ActiveXuid();
    m.own = ReadSpans(base, live);
    // the player's own license: the game's own build from the save object (with 1), the server's put back after
    std::vector<uint8_t> server(base + manager + 8, base + manager + 8 + kBody);
    const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64;
    ctx.r4.u64 = 1;
    __imp__sub_826DA630(ctx, base);
    m.ownBody.assign(base + manager + 8, base + manager + 8 + kBody);
    std::copy(server.begin(), server.end(), base + manager + 8);
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    g_mirror = std::move(m);
    std::fprintf(stderr, "[ranks] a server's license is used: the player's own license fields are kept aside for the save\n");
}

uint32_t OwnFieldsIn(PPCContext& ctx, uint8_t* base, std::vector<uint8_t>& server)
{
    const uint32_t live = LiveSave(ctx, base);
    std::lock_guard l(g_mirrorMutex);
    if (!ServerHere(live)) return 0;
    server = ReadSpans(base, live);
    WriteSpans(base, live, g_mirror.own);
    return live;
}

void ServerFieldsBack(uint8_t* base, uint32_t live, const std::vector<uint8_t>& server)
{
    std::lock_guard l(g_mirrorMutex);
    WriteSpans(base, live, server);
}

bool OwnOnlineRanks(PPCContext& ctx, uint8_t* base, uint8_t out[63])
{
    // lr 0: sub_8265F5D8 tail-calls the hooked sub_82653478 with the caller's lr (the VS panel's rank hook keys on one)
    PPCContext c = ctx;
    c.lr = 0;
    const uint32_t live = LiveSave(c, base);
    if (!live || GRead8(base, live) != 1) return false;
    const uint64_t xuid = (uint64_t(GRead32(base, live + 276)) << 32) | GRead32(base, live + 280);
    if (!xuid || xuid != xlive::OnlineXuid(0)) return false;   // after a match: a side's save object, maybe another's
    for (uint32_t c63 = 0; c63 < 63; ++c63)
    {
        const uint8_t r = GRead8(base, live + 9236 + 76 * c63);
        out[c63] = r <= 42 ? r : 0;                            // the rank names: 0..42 (sub_823D0FA8's tables)
    }
    return true;
}

void OnStateNamed(PPCContext& ctx, uint8_t* base, const std::string& name)
{
    if (name != "&MainMenuMode::Impl::state_mainmenu_main") return;
    uint32_t storage;
    {
        std::lock_guard l(g_mutex);
        storage = g_storage;
    }
    const uint32_t live = LiveSave(ctx, base);
    std::lock_guard l(g_mirrorMutex);
    // without the storage, the next visit might not read: the server's fields stay in the save object until it does
    if (!g_mirror.server || !storage) return;
    if (live && g_mirror.xuid == profiles::ActiveXuid()) WriteSpans(base, live, g_mirror.own);
    GWrite8(base, storage + 25, 0);
    g_mirror = {};
    std::fprintf(stderr, "[ranks] main menu: the player's own license fields are back in the save; ONLINE MODE reads "
                         "the license again\n");
}

} // namespace rank_sync

extern "C" PPC_FUNC(__imp__sub_826483C0);
PPC_FUNC(sub_826483C0)
{
    rank_sync::OnSetState(ctx);
    __imp__sub_826483C0(ctx, base);
}

// sub_826DA630(license manager, 0: license -> save object / 1: save object -> license)
PPC_FUNC(sub_826DA630)
{
    if ((ctx.r4.u32 & 0xFF) == 0) rank_sync::BeforeLicenceToSave(ctx, base);
    __imp__sub_826DA630(ctx, base);
}

// sub_8265F628(save holder): the save object is copied for the save thread, which writes it.
extern "C" PPC_FUNC(__imp__sub_8265F628);
PPC_FUNC(sub_8265F628)
{
    std::vector<uint8_t> server;
    const uint32_t live = rank_sync::OwnFieldsIn(ctx, base, server);
    __imp__sub_8265F628(ctx, base);
    if (live) rank_sync::ServerFieldsBack(base, live, server);
}
