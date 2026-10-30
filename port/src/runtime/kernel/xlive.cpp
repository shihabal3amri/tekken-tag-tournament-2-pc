// The port's Xbox LIVE, phase 2 of port/ONLINE_PLAN.md: everything the game asks before and
// inside its online menus, answered locally. References: scratch ttt2_online_recon.md (what TTT2
// calls) and tools/ref_xenia_netplay (Xenia's netplay fork: message ids, struct layouts).
//  - XNet: started, link up (0x0B), our XNADDR (ETHERNET | STATIC | GATEWAY | DNS | ONLINE = 0xE6),
//    keys, virtual peer addresses (XNetXnAddrToInAddr), datacenter QoS (one complete entry).
//  - LIVE user state: every privilege, Gold, country from xconfig, no friends.
//  - XMsg: XGI (app 0xFB, xsession.cpp), XLiveBase (0xFC: logon status, NAT open, mute list,
//    friends / invites), XamApp (0xFE: deployment type 2 = no Online Pass needed).
//  - Enumerators (XamCreateEnumeratorHandle): title servers, friends, marketplace - all empty.
// TTT2_XLIVE_TRACE=1 logs every call (otherwise each kind once).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "kernel_content.h"
#include "kernel_objects.h"
#include "guest_heap.h"
#include "title_server.h"
#include "profiles.h"
#include "xlive.h"
#include "gin_service.h"
#include "live_stats.h"
#include "net_transport.h"
#include "community_net.h"
#include "matchmaking.h"
#include <algorithm>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

void XamNotifyEnqueueEvent(uint32_t id, uint32_t param);

namespace {

constexpr uint32_t S_OK_ = 0, E_FAIL_ = 0x80004005u, E_INVALIDARG_ = 0x80070057u;
constexpr uint32_t ERROR_FUNCTION_FAILED_ = 0x65B, ERROR_IO_PENDING_ = 997, ERROR_NO_MORE_FILES_ = 18;
constexpr uint32_t ERROR_INVALID_PARAMETER_ = 87, ERROR_NO_SUCH_USER_ = 0x525, ERROR_NOT_LOGGED_ON_ = 0x4DD;
constexpr uint32_t X_ONLINE_S_LOGON_CONNECTION_ESTABLISHED = 0x001510F0u;
constexpr uint32_t X_ONLINE_S_LOGON_DISCONNECTED = 0x001510F1u;
constexpr uint32_t X_ONLINE_E_SESSION_NOT_FOUND = 0x80155200u;

bool TraceAll() { static const bool on = std::getenv("TTT2_XLIVE_TRACE") != nullptr; return on; }

// Log a call: every time under TTT2_XLIVE_TRACE, otherwise the first time per key.
void Trace(const std::string& key, const char* fmt, ...)
{
    static std::mutex m;
    static std::set<std::string> seen;
    {
        std::lock_guard l(m);
        if (!TraceAll() && !seen.insert(key).second) return;
    }
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    std::fprintf(stderr, "[xlive] %s\n", line);
}

uint64_t Read64(uint8_t* base, uint32_t a) { return (uint64_t(GRead32(base, a)) << 32) | GRead32(base, a + 4); }
void Write64(uint8_t* base, uint32_t a, uint64_t v) { GWrite32(base, a, uint32_t(v >> 32)); GWrite32(base, a + 4, uint32_t(v)); }

// ---- peers: virtual IN_ADDRs for remote XNADDRs (identity = the peer id at XNADDR+0x18) ------
struct PeerTable
{
    std::mutex m;
    std::map<uint64_t, uint32_t> addrOf;      // peer id -> virtual address (host order)
    std::map<uint32_t, uint64_t> peerOf;
    uint32_t next = 1;
} g_peers;

constexpr uint32_t kVirtualNet = 0x0A4E0000u;  // 10.78.x.y: never routed, our sockets resolve it
uint32_t OwnAddr(uint64_t peer) { return 0x0A4D0000u | uint32_t(peer & 0xFFFF); }   // 10.77.x.y

// ---- invites: a Steam "Join Game" waiting for XInviteGetAcceptedInfo --------------------------
std::mutex g_inviteMutex;
bool g_invitePending = false;
mm::SessionRecord g_invite;

// ---- enumerators -------------------------------------------------------------------------------
struct Enumerator { uint32_t app = 0, open = 0, extra = 0, itemSize = 0, items = 0; bool done = false; };
std::mutex g_enumMutex;
std::unordered_map<uint32_t, Enumerator> g_enums;
uint32_t g_nextEnum = 0xD2000000u;

uint32_t NewEnumerator(uint32_t app, uint32_t open, uint32_t extraBytes, uint32_t itemCount)
{
    Enumerator e;
    e.app = app;
    e.open = open;
    e.items = itemCount;
    if (extraBytes) e.extra = xlive::GuestAlloc(extraBytes);
    std::lock_guard l(g_enumMutex);
    const uint32_t handle = g_nextEnum++;
    g_enums.emplace(handle, e);
    return handle;
}

} // namespace

// ---- identity ------------------------------------------------------------------------------------
namespace xlive {

bool Enabled() { return TitleServerEnabled(); }

void Random(void* out, size_t bytes)
{
    BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), ULONG(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

uint64_t LocalPeerId()
{
    // The transport's id for this instance (loopback: its UDP port; Steam: the SteamID).
    XSocketStartTransport();
    return net::LocalPeer();
}

uint32_t AddrOfPeer(uint64_t peer)
{
    if (!peer || peer == net::LocalPeer()) return OwnAddr(net::LocalPeer());
    std::lock_guard l(g_peers.m);
    auto it = g_peers.addrOf.find(peer);
    if (it == g_peers.addrOf.end())
    {
        const uint32_t addr = kVirtualNet | g_peers.next++;
        it = g_peers.addrOf.emplace(peer, addr).first;
        g_peers.peerOf.emplace(addr, peer);
    }
    return it->second;
}

uint64_t PeerOfAddr(uint32_t addr)
{
    const uint64_t self = LocalPeerId();
    if (addr == 0x7F000001u || addr == OwnAddr(self)) return self;
    std::lock_guard l(g_peers.m);
    const auto it = g_peers.peerOf.find(addr);
    return it == g_peers.peerOf.end() ? 0 : it->second;
}

uint64_t OnlineXuid(uint32_t user)
{
    // Online XUIDs are 0x0009xxxxxxxxxxxx: here unique per machine (the peer id), profile and local user
    // (users 1-3 are the profile's guests, kernel_xam.cpp).
    if (!profiles::SignedIn()) return 0;
    const uint64_t mix = LocalPeerId() * 0x9E3779B97F4A7C15ull ^ profiles::ActiveXuid() ^ uint64_t(user) * 0xD6E8FEB86659FD93ull;
    return 0x0009000000000000ull | (mix & 0x0000FFFFFFFFFFFFull);
}

void WriteOwnXnAddr(uint8_t* base, uint32_t a)
{
    std::memset(base + a, 0, 0x24);
    const uint64_t peer = LocalPeerId();
    const uint32_t ina = OwnAddr(peer);                                // our own address: 10.77.x.y
    GWrite32(base, a + 0x00, ina);                                     // ina (network order)
    GWrite32(base, a + 0x04, ina);                                     // inaOnline
    GWrite16(base, a + 0x08, 3074);                                    // wPortOnline
    for (int i = 0; i < 6; ++i) GWrite8(base, a + 0x0A + i, uint8_t(peer >> (8 * i)) | (i == 0 ? 0x02 : 0));
    Write64(base, a + 0x18, peer);                                     // abOnline: our peer id
    GWrite8(base, a + 0x20, 1);                                        // platform: Xbox 360
}

uint32_t GuestAlloc(uint32_t bytes)
{
    void* p = g_virtualHeap.Alloc(bytes ? bytes : 4, 16);
    if (!p) return 0;
    std::memset(p, 0, bytes);
    return uint32_t(static_cast<uint8_t*>(p) - g_base);
}

} // namespace xlive

// ---- XNet ------------------------------------------------------------------------------------------
// NetDll_* exports take the caller type in r3; the arguments follow from r4.
PPC_FUNC(__imp__NetDll_XNetStartup)
{
    KTrace("NetDll_XNetStartup");
    Trace("startup", "XNetStartup(params %08X)", ctx.r4.u32);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NetDll_XNetCleanup) { KTrace("NetDll_XNetCleanup"); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__NetDll_WSAStartup)
{
    KTrace("NetDll_WSAStartup");
    const uint32_t version = ctx.r4.u32 & 0xFFFF, data = ctx.r5.u32;
    if (data && XamGuestRange(data, 0x190, 1))
    {
        std::memset(base + data, 0, 0x190);
        GWrite16(base, data, uint16_t(version));
        GWrite16(base, data + 2, 0x0202);
    }
    Trace("wsastartup", "WSAStartup(%04X)", version);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NetDll_WSACleanup) { KTrace("NetDll_WSACleanup"); ctx.r3.u64 = 0; }

PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr)
{
    KTrace("NetDll_XNetGetTitleXnAddr");
    const uint32_t addr = ctx.r4.u32;
    if (!xlive::Enabled())
    {
        if (addr) std::memset(base + addr, 0, 0x24);
        ctx.r3.u64 = 1;                                    // XNET_GET_XNADDR_NONE
        return;
    }
    if (addr) xlive::WriteOwnXnAddr(base, addr);
    Trace("xnaddr", "XNetGetTitleXnAddr -> 0xE6 (peer %016llX)", (unsigned long long)xlive::LocalPeerId());
    ctx.r3.u64 = 0xE6;                                     // ETHERNET|STATIC|GATEWAY|DNS|ONLINE
}

PPC_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus)
{
    KTrace("NetDll_XNetGetEthernetLinkStatus");
    ctx.r3.u64 = xlive::Enabled() ? 0x0B : 0;              // ACTIVE | 100MBPS | FULL_DUPLEX
}

PPC_FUNC(__imp__NetDll_XNetRandom)
{
    KTrace("NetDll_XNetRandom");
    const uint32_t buffer = ctx.r4.u32, length = ctx.r5.u32;
    if (buffer && length && XamGuestRange(buffer, length, 1)) xlive::Random(base + buffer, length);
    ctx.r3.u64 = 0;
}

// XNKID (8) + XNKEY (16): a system-link id and the fixed key the reference uses (00..0F).
PPC_FUNC(__imp__NetDll_XNetCreateKey)
{
    KTrace("NetDll_XNetCreateKey");
    const uint32_t kid = ctx.r4.u32, key = ctx.r5.u32;
    uint64_t id = 0;
    xlive::Random(&id, sizeof(id));
    if (kid) Write64(base, kid, id & 0x00FFFFFFFFFFFFFFull);
    for (int i = 0; key && i < 16; ++i) GWrite8(base, key + i, uint8_t(i));
    Trace("createkey", "XNetCreateKey");
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NetDll_XNetRegisterKey)
{
    KTrace("NetDll_XNetRegisterKey");
    Trace("registerkey", "XNetRegisterKey(%016llX)", (unsigned long long)(ctx.r4.u32 ? Read64(base, ctx.r4.u32) : 0));
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NetDll_XNetUnregisterKey) { KTrace("NetDll_XNetUnregisterKey"); ctx.r3.u64 = 0; }

// The guest addresses peers by IN_ADDR; each remote peer id gets its own 10.78.x.y. Our own
// XNADDR maps to 127.0.0.1 (as Xenia does; some titles connect to themselves).
PPC_FUNC(__imp__NetDll_XNetXnAddrToInAddr)
{
    KTrace("NetDll_XNetXnAddrToInAddr");
    const uint32_t xna = ctx.r4.u32, out = ctx.r6.u32;
    if (!xna || !out) { ctx.r3.u64 = 10022; return; }      // WSAEINVAL
    const uint64_t peer = Read64(base, xna + 0x18);
    const uint32_t ina = (!peer || peer == xlive::LocalPeerId()) ? 0x7F000001u : xlive::AddrOfPeer(peer);
    GWrite32(base, out, ina);
    Trace("xnaddr2inaddr", "XNetXnAddrToInAddr(peer %016llX) -> %u.%u.%u.%u", (unsigned long long)peer,
          ina >> 24, (ina >> 16) & 0xFF, (ina >> 8) & 0xFF, ina & 0xFF);
    ctx.r3.u64 = 0;
}

// Title servers: Namco's GIN ("TTT2") is the port's GIN service peer (gin_service.cpp); the lobby servers (World
// Arena) do not exist.
PPC_FUNC(__imp__NetDll_XNetServerToInAddr)
{
    KTrace("NetDll_XNetServerToInAddr");
    if (gin_service::Enabled() && ctx.r5.u32 == gin_service::kGinService && ctx.r6.u32)
    {
        gin_service::Start();
        const uint32_t ina = xlive::AddrOfPeer(gin_service::kGinPeer);
        GWrite32(base, ctx.r6.u32, ina);
        std::fprintf(stderr, "[xlive] XNetServerToInAddr(server %08X, service %08X) -> GIN service %u.%u.%u.%u\n", ctx.r4.u32,
                     ctx.r5.u32, ina >> 24, (ina >> 16) & 0xFF, (ina >> 8) & 0xFF, ina & 0xFF);
        ctx.r3.u64 = 0;
        return;
    }
    Trace("server2inaddr", "XNetServerToInAddr(server %08X, service %08X) -> unavailable", ctx.r4.u32, ctx.r5.u32);
    if (ctx.r6.u32) GWrite32(base, ctx.r6.u32, 0);
    ctx.r3.u64 = 10065;                                     // WSAEHOSTUNREACH
}
PPC_FUNC(__imp__NetDll_XNetConnect) { KTrace("NetDll_XNetConnect"); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__NetDll_XNetUnregisterInAddr) { KTrace("NetDll_XNetUnregisterInAddr"); ctx.r3.u64 = 0; }

// Datacenter QoS: one complete entry with good numbers. XNQOS {cxnqos, cxnqosPending,
// XNQOSINFO[] at +8}; XNQOSINFO (0x18): flags, -, probes xmit/recv, cbData, pbData, rtt min/med,
// up / down bits per second.
PPC_FUNC(__imp__NetDll_XNetQosServiceLookup)
{
    KTrace("NetDll_XNetQosServiceLookup");
    const uint32_t event = ctx.r5.u32, out = ctx.r6.u32;
    if (!out) { ctx.r3.u64 = 10022; return; }
    const uint32_t q = xlive::GuestAlloc(8 + 0x18);
    if (!q) { ctx.r3.u64 = 10055; return; }                 // WSAENOBUFS
    GWrite32(base, q + 0, 1);
    GWrite32(base, q + 4, 0);
    const uint32_t info = q + 8;
    GWrite8(base, info + 0, 0x01 | 0x02 | 0x10);            // COMPLETE | TARGET_CONTACTED | PARTIAL
    GWrite16(base, info + 2, 8);
    GWrite16(base, info + 4, 8);
    GWrite16(base, info + 0x0C, 10);
    GWrite16(base, info + 0x0E, 10);
    GWrite32(base, info + 0x10, 40 * 1000 * 1000);
    GWrite32(base, info + 0x14, 40 * 1000 * 1000);
    GWrite32(base, out, q);
    if (event) KernelSignalEventHandle(event);
    Trace("qos", "XNetQosServiceLookup -> 1 complete entry");
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NetDll_XNetQosRelease)
{
    KTrace("NetDll_XNetQosRelease");
    if (const uint32_t q = ctx.r4.u32) g_virtualHeap.Free(base + q);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_XnpLogonGetStatus)
{
    KTrace("NetDll_XnpLogonGetStatus");
    if (ctx.r4.u32) std::memset(base + ctx.r4.u32, 0, 0x14);
    if (ctx.r5.u32) GWrite32(base, ctx.r5.u32, 0);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XNetLogonGetTitleID) { KTrace("XNetLogonGetTitleID"); ctx.r3.u64 = xlive::kTitleId; }

// ---- LIVE user state ------------------------------------------------------------------------------
PPC_FUNC(__imp__XamUserCheckPrivilege)
{
    KTrace("XamUserCheckPrivilege");
    const uint32_t user = ctx.r3.u32, privilege = ctx.r4.u32, out = ctx.r5.u32;
    if (out) GWrite32(base, out, 0);
    if (user >= 4 && user != 0xFF) { ctx.r3.u64 = ERROR_INVALID_PARAMETER_; return; }
    if (!profiles::SignedIn()) { ctx.r3.u64 = ERROR_NO_SUCH_USER_; return; }
    if (!xlive::Enabled()) { ctx.r3.u64 = ERROR_NOT_LOGGED_ON_; return; }
    if (out) GWrite32(base, out, 1);                         // every privilege granted
    Trace("priv" + std::to_string(privilege), "XamUserCheckPrivilege(%u, %u) -> granted", user, privilege);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XamUserGetMembershipTierFromXUID) { KTrace("XamUserGetMembershipTierFromXUID"); ctx.r3.u64 = 6; }   // Gold
PPC_FUNC(__imp__XamUserGetOnlineCountryFromXUID) { KTrace("XamUserGetOnlineCountryFromXUID"); ctx.r3.u64 = 103; } // US (xconfig)

// XamUserAreUsersFriends(user, xuids*, count, result*, overlapped*): nobody is a friend yet.
PPC_FUNC(__imp__XamUserAreUsersFriends)
{
    KTrace("XamUserAreUsersFriends");
    const uint32_t result = ctx.r6.u32, overlapped = ctx.r7.u32;
    if (result) GWrite32(base, result, 0);
    if (const uint32_t e = XamValidateOverlapped(base, overlapped)) { ctx.r3.u64 = e; return; }
    ctx.r3.u64 = XamCompleteOverlapped(base, overlapped, 0, 0, 0);
}

// ---- enumerators ----------------------------------------------------------------------------------
// XamCreateEnumeratorHandle(user, app, openMsg, closeMsg, extraSize, itemCount, flags, &handle)
PPC_FUNC(__imp__XamCreateEnumeratorHandle)
{
    KTrace("XamCreateEnumeratorHandle");
    const uint32_t app = ctx.r4.u32, open = ctx.r5.u32, extra = ctx.r7.u32, items = ctx.r8.u32, out = ctx.r10.u32;
    if (!out) { ctx.r3.u64 = ERROR_INVALID_PARAMETER_; return; }
    const uint32_t handle = NewEnumerator(app, open, extra, items);
    GWrite32(base, out, handle);
    Trace("enum" + std::to_string(open), "XamCreateEnumeratorHandle(app %02X, open %08X, extra %u, items %u) -> empty",
          app, open, extra, items);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XamGetPrivateEnumStructureFromHandle)
{
    KTrace("XamGetPrivateEnumStructureFromHandle");
    std::lock_guard l(g_enumMutex);
    const auto it = g_enums.find(ctx.r3.u32);
    if (it == g_enums.end()) { ctx.r3.u64 = 0xC0000008u; return; }   // STATUS_INVALID_HANDLE
    if (ctx.r4.u32) GWrite32(base, ctx.r4.u32, it->second.extra);
    ctx.r3.u64 = 0;
}

bool XLiveEnumerate(uint8_t* base, uint32_t handle, uint32_t buffer, uint32_t bytes, uint32_t& count, uint32_t& result)
{
    if (live_stats::Enumerate(base, handle, buffer, bytes, count, result)) return true;   // leaderboards
    std::lock_guard l(g_enumMutex);
    const auto it = g_enums.find(handle);
    if (it == g_enums.end()) return false;
    count = 0;
    result = ERROR_NO_MORE_FILES_;
    Enumerator& e = it->second;
    if (e.open == 0x00058039u)
    {
        // XTitleServerCreateEnumerator: its private block holds the title's query (logged); with GIN on one
        // XTITLE_SERVER_INFO {IN_ADDR inaServer, DWORD dwFlags, CHAR szServerInfo[200]} - the "TTT2" server.
        if (e.extra)
        {
            char hex[3 * 48 + 1] = {};
            for (int i = 0; i < 48; ++i) std::snprintf(hex + 3 * i, 4, "%02X ", GRead8(base, e.extra + i));
            std::fprintf(stderr, "[xlive] XTitleServerCreateEnumerator query block: %s\n", hex);
        }
        if (gin_service::Enabled() && !e.done && buffer && bytes >= 208 && XamGuestRange(buffer, 208, 1))
        {
            std::memset(base + buffer, 0, 208);
            GWrite32(base, buffer, 0x0A4FD307u);
            std::memcpy(base + buffer + 8, "TTT2", 4);
            e.done = true;
            count = 1;
            result = 0;
            std::fprintf(stderr, "[xlive] title servers: 1 (TTT2, GIN service)\n");
        }
    }
    return true;
}
bool XLiveCloseHandle(uint32_t handle, uint32_t& status)
{
    if (live_stats::Close(handle)) { status = 0; return true; }
    std::lock_guard l(g_enumMutex);
    const auto it = g_enums.find(handle);
    if (it == g_enums.end()) return false;
    if (it->second.extra) g_virtualHeap.Free(g_base + it->second.extra);
    g_enums.erase(it);
    status = 0;
    return true;
}

// ---- XMsg ------------------------------------------------------------------------------------------
namespace {

// X_ARGUMENT_LIST: 32 entries {u32 size, pad, u64 pointer to the value}; count at +0x200.
uint32_t Argument(uint8_t* base, uint32_t list, uint32_t i) { return GRead32(base, list + i * 0x10 + 0x0C); }

// XUserFindUsers: the gamertags behind online XUIDs (TEKKEN CHANNEL shows a replay's players by name), from the
// community server (the players it has seen). The request (MEASURED 2026-10-22, its task's +0x18 / +0x1C) is
// marshalled little-endian: issuer XUID at +0x20, a count at +0x28 (the game asks 100), then that many
// FIND_USER_INFO {u64 XUID, char name[16]} from +0x2C (unused ones 0). The results (task +0x2C / +0x30) are the
// game's FIND_USERS_RESPONSE, big-endian: {u32 count, u32 pointer to the entries} and the entries after it.
constexpr uint32_t kFindUserInfo = 24;

void FindUsers(uint8_t* base, uint32_t task, uint32_t results, uint32_t size)
{
    GWrite32(base, results + 4, results + 8);
    const uint32_t in = GRead32(base, task + 0x18), inSize = GRead32(base, task + 0x1C);
    if (!in || inSize < 0x2C || !XamGuestRange(in, inSize, 1) || size < 8) return;
    auto le32 = [&](uint32_t a) { uint32_t v; std::memcpy(&v, base + a, 4); return v; };
    auto le64 = [&](uint32_t a) { uint64_t v; std::memcpy(&v, base + a, 8); return v; };
    const uint32_t asked = (std::min)({ le32(in + 0x28), (inSize - 0x2C) / kFindUserInfo, (size - 8) / kFindUserInfo });
    std::vector<uint64_t> xuids;
    for (uint32_t i = 0; i < asked; ++i)
        if (const uint64_t x = le64(in + 0x2C + i * kFindUserInfo); x && std::find(xuids.begin(), xuids.end(), x) == xuids.end())
            xuids.push_back(x);

    // names stay known a while (a TEKKEN CHANNEL page asks for the same players again and again)
    static std::mutex m;
    static std::map<uint64_t, std::pair<std::string, ULONGLONG>> known;   // XUID -> name ("" = none), when asked
    constexpr ULONGLONG kKeepMs = 10 * 60 * 1000, kKeepNoneMs = 60 * 1000;
    const ULONGLONG now = GetTickCount64();
    std::vector<uint64_t> ask;
    {
        std::lock_guard l(m);
        for (const uint64_t x : xuids)
        {
            const auto it = known.find(x);
            if (it == known.end() || now - it->second.second > (it->second.first.empty() ? kKeepNoneMs : kKeepMs)) ask.push_back(x);
        }
    }
    const std::shared_ptr<community::Client> server = net::Community();
    for (size_t at = 0; server && at < ask.size(); at += 16)
    {
        const std::vector<uint64_t> part(ask.begin() + at, ask.begin() + (std::min)(ask.size(), at + 16));
        std::map<uint64_t, std::string> names;
        if (!server->NamesOf(part, names, 2000)) break;   // no answer: asked again next time
        std::lock_guard l(m);
        if (known.size() > 4096) known.clear();
        for (const uint64_t x : part) known[x] = { names.count(x) ? names[x] : std::string(), now };
    }
    uint32_t found = 0;
    {
        std::lock_guard l(m);
        for (const uint64_t x : xuids)
        {
            const auto it = known.find(x);
            if (it == known.end() || it->second.first.empty()) continue;
            const uint32_t e = results + 8 + found * kFindUserInfo;
            GWrite32(base, e, uint32_t(x >> 32));
            GWrite32(base, e + 4, uint32_t(x));
            const std::string& name = it->second.first;
            for (size_t c = 0; c < 16; ++c) base[e + 8 + c] = c < name.size() && c < 15 ? uint8_t(name[c]) : 0;
            ++found;
        }
    }
    GWrite32(base, results, found);
    Trace("findusers", "XUserFindUsers: %u of %zu players named by the server", found, xuids.size());
}

uint32_t XLiveBase(uint8_t* base, uint32_t msg, uint32_t p1, uint32_t p2, bool& handled)
{
    handled = true;
    switch (msg)
    {
    case 0x00058003:                                         // logon status (HRESULT)
        return xlive::Enabled() ? X_ONLINE_S_LOGON_CONNECTION_ESTABLISHED : X_ONLINE_S_LOGON_DISCONNECTED;
    case 0x00058004:                                         // XOnlineGetLogonID
        if (p1) GWrite32(base, p1, 1);
        return S_OK_;
    case 0x00058006:                                         // XOnlineGetNatType: open
        if (p1) GWrite32(base, p1, 1);
        Trace("nat", "XOnlineGetNatType -> open");
        return S_OK_;
    case 0x0005800C: case 0x0005800D:                        // mute list add / remove
        return S_OK_;
    case 0x0005800E:                                         // XUserMuteListQuery: not muted
        if (p2) GWrite32(base, p2, 0);
        return S_OK_;
    case 0x00058037: case 0x00058046: case 0x00058056:       // XPresenceInitialize
        return S_OK_;
    case 0x00058020:                                         // XFriendsCreateEnumerator: no friends
    {
        // args: user, start, count, &bufferSize, &handle (p2 = X_ARGUMENT_LIST)
        if (!p2) return E_INVALIDARG_;
        const uint32_t sizeOut = Argument(base, p2, 3), handleOut = Argument(base, p2, 4);
        const uint32_t count = Argument(base, p2, 2) ? GRead32(base, Argument(base, p2, 2)) : 0;
        if (handleOut) GWrite32(base, handleOut, NewEnumerator(0xFC, msg, 0, count));
        if (sizeOut) GWrite32(base, sizeOut, count * 0xC4);
        Trace("friends", "XFriendsCreateEnumerator -> empty");
        return S_OK_;
    }
    case 0x0005000C:                                         // XStringVerify (session / room names)
    case 0x0005000E:                                         // XUserFindUsers
    {
        // Marshalled requests: buffer = XLIVEBASE_ASYNC_MESSAGE {XLIVE_ASYNC_TASK*, progress};
        // the task's results buffer is at +0x2C (size +0x30).
        const uint32_t task = p1 ? GRead32(base, p1) : 0;
        const uint32_t results = task ? GRead32(base, task + 0x2C) : 0, size = task ? GRead32(base, task + 0x30) : 0;
        if (!results || !size || !XamGuestRange(results, size, 1)) return E_INVALIDARG_;
        std::memset(base + results, 0, size);
        if (msg == 0x0005000C)
        {
            // STRING_VERIFY_RESPONSE (packed): u16 count, u32 pointer at +2 (the game reads +2,
            // sub_8269D280), then one HRESULT per string - all S_OK (0x80157102 = profane).
            const uint32_t n = size >= 6 ? (size - 6) / 4 : 0;
            GWrite16(base, results, uint16_t(n));
            GWrite32(base, results + 2, results + 6);
            Trace("stringverify", "XStringVerify(%u strings) -> all accepted", n);
        }
        else FindUsers(base, task, results, size);
        return S_OK_;
    }
    case 0x00058023:                                         // XInviteGetAcceptedInfo
    {
        // args: user index, X_INVITE_INFO* (pack 4, 0x54): xuidInvitee, xuidInviter, title id,
        // XSESSION_INFO hostInfo (+0x14), fFromGameInvite (+0x50).
        const uint32_t info = p2 ? Argument(base, p2, 1) : 0;
        std::lock_guard l(g_inviteMutex);
        if (!g_invitePending || !info)
        {
            Trace("invite_none", "XInviteGetAcceptedInfo -> none");
            return X_ONLINE_E_SESSION_NOT_FOUND;
        }
        g_invitePending = false;
        std::memset(base + info, 0, 0x54);
        Write64(base, info + 0x00, xlive::OnlineXuid());
        Write64(base, info + 0x08, g_invite.hostXuid);
        GWrite32(base, info + 0x10, xlive::kTitleId);
        std::memcpy(base + info + 0x14, g_invite.info, 0x3C);
        GWrite32(base, info + 0x50, 1);
        Trace("invite_" + std::to_string(g_invite.id), "XInviteGetAcceptedInfo -> session %016llX", (unsigned long long)g_invite.id);
        return S_OK_;
    }
    case 0x00058032:                                         // XOnlineGetTaskProgress
        return S_OK_;
    default:
        handled = false;
        return E_FAIL_;
    }
}

uint32_t XamApp(uint8_t* base, uint32_t msg, uint32_t buffer, bool& handled)
{
    handled = true;
    switch (msg)
    {
    case 0x00022005:                                         // XTitleGetDeploymentType
    {
        // TTT2 needs 2 here, or it runs the Online Pass terms / code flow (sub_825FE840).
        const uint32_t out = buffer ? GRead32(base, buffer) : 0;
        if (out) GWrite32(base, out, 2);
        Trace("deploy", "XTitleGetDeploymentType -> 2");
        return S_OK_;
    }
    default:
        handled = false;
        return E_FAIL_;
    }
}

} // namespace

uint32_t XLiveDispatch(uint8_t* base, uint32_t app, uint32_t msg, uint32_t buffer, uint32_t length, bool& handled)
{
    handled = false;
    if (!xlive::Enabled()) return E_FAIL_;
    switch (app)
    {
    case 0xFB: return XgiDispatch(base, msg, buffer, length, handled);
    case 0xFC: return XLiveBase(base, msg, buffer, length, handled);
    case 0xFE: return XamApp(base, msg, buffer, handled);
    default: return E_FAIL_;
    }
}

void XLivePollInvites()
{
    mm::SessionRecord r;
    if (!xlive::Enabled() || !mm::TakeInvite(r)) return;
    {
        std::lock_guard l(g_inviteMutex);
        g_invite = r;
        g_invitePending = true;
    }
    std::fprintf(stderr, "[xlive] invite accepted: session %016llX -> XN_LIVE_INVITE_ACCEPTED\n", (unsigned long long)r.id);
    XamNotifyEnqueueEvent(0x02000002u, 0);                   // XN_LIVE_INVITE_ACCEPTED, user 0
}

// XMsgStartIORequest(app, msg, overlapped, buffer, length) / ...Ex(+ r8): handled synchronously;
// an overlapped request completes inline and reports pending, failures as ERROR_FUNCTION_FAILED
// with the HRESULT as extended error (real XAM; Xenia's fork loses it). Session searches can wait
// on Steam's lobby list for seconds: those complete from a worker thread instead.
// XMP (app 0xFA, the music player / custom soundtracks), offline: no media sources. TEKKEN TUNES
// asks XMPGetMediaSources (0x0007002B) on the way in; failing it left the screen black ~27 s
// (the user's run, 2026-10-15) before the page appeared. Arguments as Xenia's XmpApp:
// {client, connected-only, MSAL_MEDIASOURCEINFO* (0xB4 each), max, &returned}; a null array
// asks for the count (10 slots), the answer is 0 sources.
static uint32_t XmpDispatch(uint8_t* base, uint32_t msg, uint32_t buffer, bool& handled)
{
    handled = false;
    if (msg != 0x0007002B || !buffer) return E_FAIL_;
    handled = true;
    constexpr uint32_t kSlots = 10;
    const uint32_t sources = GRead32(base, buffer + 8), max = GRead32(base, buffer + 12), returned = GRead32(base, buffer + 16);
    if (!returned) return E_INVALIDARG_;
    if (!sources) { GWrite32(base, returned, kSlots); return 0; }
    if (max < kSlots) return 0x80070008u;                     // E_OUTOFMEMORY-style: buffer too small
    for (uint32_t i = 0; i < kSlots; ++i)
    {
        std::memset(base + sources + i * 0xB4, 0, 0x28);
        GWrite32(base, sources + i * 0xB4, i);
    }
    GWrite32(base, returned, 0);
    return 0;
}

static void StartIORequest(PPCContext& ctx, uint8_t* base, const char* name)
{
    const uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, overlapped = ctx.r5.u32, buffer = ctx.r6.u32, length = ctx.r7.u32;
    // Diagnostics: who takes a member out of a session (0xB0013 Leave) - the guest call chain.
    if (app == 0xFB && msg == 0x000B0013)
    {
        char line[256];
        int k = std::snprintf(line, sizeof(line), "[xlive] session leave requested, guest callers %08X", uint32_t(ctx.lr));
        uint32_t sp = ctx.r1.u32;
        for (int i = 0; i < 12 && sp && sp < 0xFFFFFFF0u && k < int(sizeof(line)) - 12; ++i)
        {
            const uint32_t prev = GRead32(base, sp);
            if (!prev || prev <= sp || prev - sp > 0x10000) break;
            k += std::snprintf(line + k, sizeof(line) - k, " %08X", GRead32(base, prev - 8));
            sp = prev;
        }
        std::fprintf(stderr, "%s\n", line);
    }
    if (app == 0xFA)
    {
        bool handled = false;
        const uint32_t hr = XmpDispatch(base, msg, buffer, handled);
        if (handled)
        {
            std::fprintf(stderr, "[xmp] %s(msg %08X) -> %08X (no media sources)\n", name, msg, hr);
            if (!overlapped) { ctx.r3.u64 = hr; return; }
            if (const uint32_t e = XamValidateOverlapped(base, overlapped)) { ctx.r3.u64 = e; return; }
            ctx.r3.u64 = XamCompleteOverlapped(base, overlapped, hr ? ERROR_FUNCTION_FAILED_ : 0, 0, hr);
            return;
        }
    }
    // ... and XUserReadStats (0xB0021), which asks the community server.
    const bool search = app == 0xFB && (msg == 0x000B0016 || msg == 0x000B001C || msg == 0x000B001B || msg == 0x000B0021);
    if (search && overlapped && xlive::Enabled() && !XamValidateOverlapped(base, overlapped) && buffer && length &&
        length <= 0x100 && XamGuestRange(buffer, length, 1))
    {
        // The XDK wrappers build the parameter block on their stack and return once the request is
        // pending: work on a copy (real XAM copies it too). The pointers inside stay valid.
        const uint32_t copy = xlive::GuestAlloc(length);
        if (!copy) { ctx.r3.u64 = 8; return; }               // ERROR_NOT_ENOUGH_MEMORY
        std::memcpy(base + copy, base + buffer, length);
        GWrite32(base, overlapped + 4, 0);
        GWrite32(base, overlapped + 24, 0);
        std::atomic_thread_fence(std::memory_order_release);
        GWrite32(base, overlapped, ERROR_IO_PENDING_);
        std::thread([base, app, msg, overlapped, copy, length] {
            bool handled = false;
            XSessionSearchIsAsync(true);                        // a search here may wait (xsession.cpp)
            const uint32_t hr = XLiveDispatch(base, app, msg, copy, length, handled);
            XamCompleteOverlapped(base, overlapped, hr ? ERROR_FUNCTION_FAILED_ : 0, 0, hr);
            g_virtualHeap.Free(base + copy);
        }).detach();
        ctx.r3.u64 = ERROR_IO_PENDING_;
        return;
    }
    bool handled = false;
    const uint32_t hr = XLiveDispatch(base, app, msg, buffer, length, handled);
    char key[48];
    std::snprintf(key, sizeof(key), "io%02X%08X_%08X", app, msg, hr);
    Trace(key, "%s(app %02X, msg %08X, len %u) -> %08X%s", name, app, msg, length, hr, handled ? "" : " (unhandled)");
    if (!overlapped) { ctx.r3.u64 = hr; return; }
    if (const uint32_t e = XamValidateOverlapped(base, overlapped)) { ctx.r3.u64 = e; return; }
    ctx.r3.u64 = XamCompleteOverlapped(base, overlapped, hr ? ERROR_FUNCTION_FAILED_ : 0, 0, hr);
}
PPC_FUNC(__imp__XMsgStartIORequest) { KTrace("XMsgStartIORequest"); StartIORequest(ctx, base, "XMsgStartIORequest"); }
PPC_FUNC(__imp__XMsgStartIORequestEx) { KTrace("XMsgStartIORequestEx"); StartIORequest(ctx, base, "XMsgStartIORequestEx"); }
PPC_FUNC(__imp__XMsgCancelIORequest) { KTrace("XMsgCancelIORequest"); ctx.r3.u64 = 0; }   // all complete inline
