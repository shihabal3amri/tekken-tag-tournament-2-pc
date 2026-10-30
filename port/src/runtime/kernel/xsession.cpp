// XGI (XMsg app 0xFB): user contexts / properties, XSession objects and stats
// (port/ONLINE_PLAN.md). Host sessions that use matchmaking or the peer network are published
// through matchmaking.h; searches and searches by id read them back. Stats stay in memory.
// Layouts: scratch xenia_netplay_reference.md section 2 / 4 (all guest structs big-endian).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "profiles.h"
#include "xlive.h"
#include "matchmaking.h"
#include "display_settings.h"
#include "live_stats.h"
#include "community_net.h"
#include "net_transport.h"
#include <chrono>
#include <memory>
#include <random>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

bool SessionEnabled();   // rollback_session.cpp: rollback sessions (TTT2_ROLLBACK)

namespace {

constexpr uint32_t S_OK_ = 0, E_FAIL_ = 0x80004005u, E_INVALIDARG_ = 0x80070057u;
constexpr uint32_t ERROR_INSUFFICIENT_BUFFER_ = 122, ERROR_FUNCTION_FAILED_ = 0x65B;
constexpr uint32_t STATUS_INVALID_HANDLE_ = 0xC0000008u;
constexpr uint32_t X_ONLINE_E_SESSION_NOT_FOUND = 0x80155200u;
// The XDK wrappers size the result buffers themselves and pass the size VALUE in the block
// (sub_82853320 / sub_82853408 / sub_828530F0 / sub_82BEEC18, MEASURED 2026-10-12); a too small
// buffer is answered with this HRESULT, which they turn into ERROR_INSUFFICIENT_BUFFER.
constexpr uint32_t X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER = 0x80155207u;

// Session flags (XSessionCreate)
constexpr uint32_t kHost = 0x1, kStats = 0x4, kMatchmaking = 0x8, kArbitration = 0x10, kPeerNetwork = 0x20;

uint64_t Read64(uint8_t* base, uint32_t a) { return (uint64_t(GRead32(base, a)) << 32) | GRead32(base, a + 4); }
void Write64(uint8_t* base, uint32_t a, uint64_t v) { GWrite32(base, a, uint32_t(v >> 32)); GWrite32(base, a + 4, uint32_t(v)); }

void Log(const std::string& key, const char* fmt, ...)
{
    static std::mutex m;
    static std::set<std::string> seen;
    static const bool all = std::getenv("TTT2_XLIVE_TRACE") != nullptr;
    {
        std::lock_guard l(m);
        if (!all && !seen.insert(key).second) return;
    }
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    std::fprintf(stderr, "[xsession] %s\n", line);
}

enum class State : uint32_t { Lobby = 0, Registration = 1, InGame = 2, Reporting = 3, Deleted = 4 };
struct Member { uint64_t xuid = 0; uint32_t userIndex = 0xFE; bool isPrivate = false; };
struct Session
{
    uint32_t guestObject = 0;                  // X_KSESSION {be u32 handle}
    bool created = false;
    uint32_t flags = 0, maxPublic = 0, maxPrivate = 0;
    uint64_t nonce = 0, id = 0;
    std::vector<uint8_t> info;                 // XSESSION_INFO (0x3C)
    std::vector<Member> members;
    State state = State::Lobby;
};

std::mutex g_mutex;
std::map<uint32_t, Session> g_sessions;
uint32_t g_nextSession = 0xD3000000u;
std::map<uint32_t, uint32_t> g_contexts;                       // user 0 context id -> value
std::map<uint32_t, std::vector<uint8_t>> g_properties;         // user 0 property id -> bytes

Session* Lookup(uint8_t* base, uint32_t objectPtr)
{
    if (!objectPtr) return nullptr;
    const uint32_t handle = GRead32(base, objectPtr);
    const auto it = g_sessions.find(handle);
    return it == g_sessions.end() ? nullptr : &it->second;
}

// A host's matchmaking / peer-network session, as searchers see it (under g_mutex).
void Publish(const Session& s)
{
    if (!(s.flags & kHost) || !(s.flags & (kMatchmaking | kPeerNetwork)) || s.state == State::Deleted || s.info.size() != 0x3C)
        return;
    mm::SessionRecord r;
    r.id = s.id;
    r.hostPeer = xlive::LocalPeerId();
    r.hostXuid = xlive::OnlineXuid();
    r.flags = s.flags;
    r.maxPublic = s.maxPublic;
    r.maxPrivate = s.maxPrivate;
    for (const Member& m : s.members) (m.isPrivate ? r.filledPrivate : r.filledPublic)++;
    std::memcpy(r.info, s.info.data(), sizeof(r.info));
    for (const auto& [id, value] : g_contexts) r.contexts.push_back({ id, value });
    for (const auto& [id, value] : g_properties) r.properties.push_back({ id, value });
    if (SessionEnabled())
        r.netcode = mm::kNetcodeKnown | uint32_t(display_settings::GetNetcode()) | uint32_t(display_settings::GetCustomDelay()) << 8 |
                    uint32_t(display_settings::GetCustomRollback()) << 16;
    mm::Publish(r);
}

// 0xB0010 XSessionCreate {obj, flags, maxPublic, maxPrivate, user, pSessionInfo, pNonce}
uint32_t Create(uint8_t* base, uint32_t b)
{
    std::lock_guard l(g_mutex);
    Session* s = Lookup(base, GRead32(base, b));
    if (!s || s->created) return ERROR_FUNCTION_FAILED_;
    const uint32_t flags = GRead32(base, b + 4), info = GRead32(base, b + 0x14), nonceOut = GRead32(base, b + 0x18);
    if (!profiles::SignedIn() || !info) return ERROR_FUNCTION_FAILED_;
    s->created = true;
    s->flags = flags;
    s->maxPublic = GRead32(base, b + 8);
    s->maxPrivate = GRead32(base, b + 0xC);
    if ((flags & kHost) || flags == kStats || flags == 0)
    {
        xlive::Random(&s->nonce, sizeof(s->nonce));
        if (nonceOut) Write64(base, nonceOut, s->nonce);
        uint64_t id = 0;
        xlive::Random(&id, sizeof(id));
        s->id = (flags & ~kHost) ? (0x8000000000000000ull | (id & 0x00FFFFFFFFFFFFFFull)) : (id & 0x00FFFFFFFFFFFFFFull);
        Write64(base, info, s->id);                                // XNKID sessionID
        xlive::WriteOwnXnAddr(base, info + 0x08);                  // XNADDR hostAddress
        for (int i = 0; i < 16; ++i) GWrite8(base, info + 0x2C + i, uint8_t(i));   // XNKEY
    }
    else
    {
        s->id = Read64(base, info);                                // joining: the search result's id
        if (nonceOut) s->nonce = Read64(base, nonceOut);
    }
    s->info.assign(base + info, base + info + 0x3C);
    Log("create" + std::to_string(flags), "XSessionCreate(flags %03X, public %u, private %u) -> %016llX", flags,
        s->maxPublic, s->maxPrivate, (unsigned long long)s->id);
    Publish(*s);
    return S_OK_;
}

// 0xB0012 / 0xB0013 Join / Leave {obj, count, xuids (0 = local), userIndices, private}
uint32_t JoinLeave(uint8_t* base, uint32_t b, bool join)
{
    std::lock_guard l(g_mutex);
    Session* s = Lookup(base, GRead32(base, b));
    if (!s) return STATUS_INVALID_HANDLE_;
    const uint32_t count = GRead32(base, b + 4), xuids = GRead32(base, b + 8), users = GRead32(base, b + 0xC);
    const uint32_t privates = GRead32(base, b + 0x10);
    for (uint32_t i = 0; i < count && i < 16; ++i)
    {
        Member m;
        if (xuids) m.xuid = Read64(base, xuids + i * 8);
        else
        {
            m.userIndex = users ? GRead32(base, users + i * 4) : 0;
            m.xuid = xlive::OnlineXuid(m.userIndex < 4 ? m.userIndex : 0);   // a pair's second player: a guest
        }
        m.isPrivate = privates && GRead32(base, privates + i * 4);
        auto it = std::find_if(s->members.begin(), s->members.end(), [&](const Member& x) { return x.xuid == m.xuid; });
        if (join && it == s->members.end()) s->members.push_back(m);
        if (!join && it != s->members.end()) s->members.erase(it);
    }
    const uint32_t firstUser = !xuids && users && count ? GRead32(base, users) : 0;
    Log(std::string(join ? (xuids ? "joinremote" : "joinlocal") : (xuids ? "leaveremote" : "leavelocal")) + std::to_string(firstUser),
        "XSession%s%s(%u, first %016llX%s)", join ? "Join" : "Leave", xuids ? "Remote" : "Local", count,
        (unsigned long long)(xuids && count ? Read64(base, xuids) : xlive::OnlineXuid(firstUser < 4 ? firstUser : 0)),
        xuids ? "" : firstUser ? ", a guest" : "");
    Publish(*s);
    return S_OK_;
}

uint32_t SetState(uint8_t* base, uint32_t b, State state, const char* name)
{
    std::lock_guard l(g_mutex);
    Session* s = Lookup(base, GRead32(base, b));
    if (!s) return STATUS_INVALID_HANDLE_;
    s->state = state;
    Log(name, "XSession%s(%016llX)", name, (unsigned long long)s->id);
    return S_OK_;
}

// Search results: XSESSION_SEARCHRESULT_HEADER {count, pResults}, the results (0x5C each:
// XSESSION_INFO, open public / private, filled public / private, cProperties, cContexts,
// pProperties, pContexts), then their XUSER_PROPERTY (0x18) / XUSER_CONTEXT (8) arrays and string /
// binary payloads, all inside the caller's buffer. Every property and context the host set is
// returned (the title's XLAST query would pick a subset; the game reads by id).
uint32_t WriteResults(uint8_t* base, const std::vector<mm::SessionRecord>& found, uint32_t results, uint32_t size)
{
    const uint32_t count = uint32_t(found.size());
    uint32_t tail = results + 8 + count * 0x5C;
    const uint32_t end = results + size;
    GWrite32(base, results, count);
    GWrite32(base, results + 4, results + 8);
    for (uint32_t i = 0; i < count; ++i)
    {
        const mm::SessionRecord& r = found[i];
        const uint32_t e = results + 8 + i * 0x5C;
        std::memcpy(base + e, r.info, 0x3C);
        GWrite32(base, e + 0x3C, r.maxPublic > r.filledPublic ? r.maxPublic - r.filledPublic : 0);
        GWrite32(base, e + 0x40, r.maxPrivate > r.filledPrivate ? r.maxPrivate - r.filledPrivate : 0);
        GWrite32(base, e + 0x44, r.filledPublic);
        GWrite32(base, e + 0x48, r.filledPrivate);
        // properties
        const uint32_t props = tail;
        uint32_t nProps = 0;
        if (props + uint32_t(r.properties.size()) * 0x18 <= end)
        {
            tail += uint32_t(r.properties.size()) * 0x18;
            for (const mm::Property& p : r.properties)
            {
                const uint32_t at = props + nProps * 0x18;
                std::memset(base + at, 0, 0x18);
                GWrite32(base, at, p.id);
                const uint8_t type = uint8_t(p.id >> 28);
                GWrite8(base, at + 8, type);
                if (type == 4 || type == 6)                       // UNICODE / BINARY: payload in the tail
                {
                    const uint32_t n = uint32_t(p.value.size());
                    if (tail + n > end) continue;
                    if (n) std::memcpy(base + tail, p.value.data(), n);
                    GWrite32(base, at + 0x10, n);
                    GWrite32(base, at + 0x14, tail);
                    tail += (n + 3) & ~3u;
                }
                else
                    std::memcpy(base + at + 0x10, p.value.data(), (std::min<size_t>)(p.value.size(), 8));
                ++nProps;
            }
        }
        // contexts
        const uint32_t ctxs = tail;
        uint32_t nCtx = 0;
        if (ctxs + uint32_t(r.contexts.size()) * 8 <= end)
        {
            for (const mm::Context& c : r.contexts)
            {
                GWrite32(base, ctxs + nCtx * 8, c.id);
                GWrite32(base, ctxs + nCtx * 8 + 4, c.value);
                ++nCtx;
            }
            tail += nCtx * 8;
        }
        GWrite32(base, e + 0x4C, nProps);
        GWrite32(base, e + 0x50, nCtx);
        GWrite32(base, e + 0x54, nProps ? props : 0);
        GWrite32(base, e + 0x58, nCtx ? ctxs : 0);
    }
    return S_OK_;
}

// A search running on xlive.cpp's worker thread (the game asked asynchronously): it may wait.
thread_local bool t_asyncSearch = false;
} // namespace
void XSessionSearchIsAsync(bool on) { t_asyncSearch = on; }
namespace {

// 0xB0016 / 0xB001C XSessionSearch(Ex) {proc, user, numResults, u16 numProps, u16 numContexts,
// props, contexts, cbResults, pResults[, numUsers]}: published sessions of other
// hosts with open public slots whose GAME_TYPE / GAME_MODE match the searcher's contexts (the
// title's XLAST rules are not evaluated). Buffer sizing as the game computes it: 8 + 1326 each.
// A ranked search that finds nothing asks once more after a random 1.5-4.5 s (MEASURED 2026-10-23, the user and a
// friend: after a match both games searched in the same second, found nothing - each one's room was not up yet, or
// was being removed - and hosted; their rooms then timed out in the same second, and so on, minute after minute).
// The two searches now return at different moments: the first hosts, the second finds its room.
uint32_t Search(uint8_t* base, uint32_t b)
{
    const uint32_t proc = GRead32(base, b), numResults = GRead32(base, b + 8);
    const uint32_t numContexts = GRead16(base, b + 0xE), contexts = GRead32(base, b + 0x14);
    const uint32_t size = GRead32(base, b + 0x18), results = GRead32(base, b + 0x1C);
    if (!results) return E_INVALIDARG_;
    if (size < 8 + 0x5C * numResults) return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
    // the query's parameters (XUSER_PROPERTY: id, XUSER_DATA type at +8, a 4-byte value at +0x10), in the log once per
    // different set: the ranked search's rank range is among them (VS_MATCHING, MATCHING_THIS_RANK min / max)
    {
        const uint32_t numProps = GRead16(base, b + 0xC), props = GRead32(base, b + 0x10);
        std::string text;
        for (uint32_t i = 0; props && i < numProps && i < 32; ++i)
        {
            char one[32];
            std::snprintf(one, sizeof(one), "%s%08X=%d", i ? " " : "", GRead32(base, props + i * 0x18), int32_t(GRead32(base, props + i * 0x18 + 0x10)));
            text += one;
        }
        Log("searchprops" + text, "XSessionSearch(proc %u) parameters: %s", proc, text.c_str());
    }
    std::map<uint32_t, uint32_t> wanted;
    for (uint32_t i = 0; contexts && i < numContexts && i < 32; ++i)
    {
        const uint32_t id = GRead32(base, contexts + i * 8), value = GRead32(base, contexts + i * 8 + 4);
        if (id == 0x800A || id == 0x800B) wanted[id] = value;
    }
    std::vector<mm::SessionRecord> found;
    const std::vector<std::pair<uint32_t, uint32_t>> filters(wanted.begin(), wanted.end());
    auto list = [&] {
        found.clear();
        for (mm::SessionRecord& r : mm::List(64, filters, true))
        {
            if (found.size() >= numResults) break;
            if (r.maxPublic <= r.filledPublic) continue;
            bool match = true;
            for (const auto& [id, value] : wanted)
                for (const mm::Context& c : r.contexts)
                    if (c.id == id && c.value != value) match = false;
            if (match) found.push_back(std::move(r));
        }
    };
    list();
    const auto gameType = wanted.find(0x800A);              // X_CONTEXT_GAME_TYPE: 0 ranked
    if (found.empty() && t_asyncSearch && gameType != wanted.end() && gameType->second == 0)
    {
        thread_local std::mt19937 rng{ std::random_device{}() };
        const int waitMs = std::uniform_int_distribution<int>(1500, 4500)(rng);
        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
        list();
        Log("searchagain", "XSessionSearch: a ranked search that found nothing asked again after %d ms -> %zu sessions", waitMs,
            found.size());
    }
    Log("search" + std::to_string(proc) + "_" + std::to_string(found.size()), "XSessionSearch(proc %u, %u results, %u contexts) -> %zu sessions",
        proc, numResults, numContexts, found.size());
    // the hosts found, with the attributes the title's query compares (once per host and attribute set)
    for (const mm::SessionRecord& r : found)
    {
        std::string text;
        for (const mm::Property& p : r.properties)
            if (p.value.size() == 4)
            {
                char one[32];
                std::snprintf(one, sizeof(one), " %08X=%d", p.id,
                              int32_t(uint32_t(p.value[0]) << 24 | uint32_t(p.value[1]) << 16 | uint32_t(p.value[2]) << 8 | p.value[3]));
                text += one;
            }
        char head[64];
        std::snprintf(head, sizeof(head), "found %016llX of %016llX:", (unsigned long long)r.id, (unsigned long long)r.hostPeer);
        Log(std::string("searchhost") + head + text, "XSessionSearch %s%s", head, text.c_str());
    }
    return WriteResults(base, found, results, size);
}

// 0xB001B XSessionSearchByID {user, XNKID (8, at +4), cbResults, pResults}
uint32_t SearchById(uint8_t* base, uint32_t b)
{
    const uint64_t id = Read64(base, b + 4);
    const uint32_t size = GRead32(base, b + 0xC), results = GRead32(base, b + 0x10);
    if (!results) return E_INVALIDARG_;
    if (size < 8 + 0x5C) return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
    mm::SessionRecord r;
    if (!mm::Find(id, r))
    {
        Log("searchbyid_miss", "XSessionSearchByID(%016llX) -> not found", (unsigned long long)id);
        return X_ONLINE_E_SESSION_NOT_FOUND;
    }
    Log("searchbyid", "XSessionSearchByID(%016llX) -> found", (unsigned long long)id);
    return WriteResults(base, { r }, results, size);
}

// 0xB001A XSessionArbitrationRegister {obj, flags, nonce, duration, cbResults, pResults}: the
// local machine only (phase 3 adds the session's peers). XSESSION_REGISTRATION_RESULTS {n, p},
// XSESSION_REGISTRANT (0x18) {u64 machine, trust, nUsers, pUsers}.
uint32_t ArbitrationRegister(uint8_t* base, uint32_t b)
{
    std::lock_guard l(g_mutex);
    Session* s = Lookup(base, GRead32(base, b));
    if (!s) return STATUS_INVALID_HANDLE_;
    const uint32_t size = GRead32(base, b + 0x14), results = GRead32(base, b + 0x18);
    if (!results) return E_INVALIDARG_;
    if (size < 8 + 0x18 + 8) return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
    const uint32_t reg = results + 8, users = reg + 0x18;
    GWrite32(base, results, 1);
    GWrite32(base, results + 4, reg);
    Write64(base, reg, 0xFA00000000000000ull | (xlive::LocalPeerId() & 0xFFFFFFFFFFFFull));
    GWrite32(base, reg + 8, 1);
    GWrite32(base, reg + 0xC, 1);
    GWrite32(base, reg + 0x10, users);
    Write64(base, users, xlive::OnlineXuid());
    s->state = State::Registration;
    // On a community server: register for the ranked match (its results count only for registered players).
    const std::shared_ptr<community::Client> server = net::Community();
    const uint64_t opponent = XSocketMatchPeer();
    if (server && server->Connected()) server->RegisterRanked(s->id, xlive::OnlineXuid(), opponent);
    Log("arbitration", "XSessionArbitrationRegister -> 1 registrant%s (opponent %016llX)", server ? " (registered with the server)" : "",
        (unsigned long long)opponent);
    return S_OK_;
}

} // namespace

// True while the game has an online session (created, not deleted): Player / Ranked Match
// lobbies and their matches. input_delay.cpp keeps the game's own input delay then - both
// sides of an online match must use the same one.
bool XSessionAnyActive()
{
    std::lock_guard l(g_mutex);
    for (const auto& [handle, s] : g_sessions)
        if (s.created && s.state != State::Deleted) return true;
    return false;
}

// The room session this game is in (hosted or joined), 0 when none: the Friend Invite's lobby (server_ui.cpp).
uint64_t XSessionCurrentId()
{
    std::lock_guard l(g_mutex);
    uint64_t id = 0;
    for (const auto& [handle, s] : g_sessions)
        if (s.created && s.state != State::Deleted && (s.flags & (kMatchmaking | kPeerNetwork)) && s.id) id = s.id;
    return id;
}

// A match of the room session starts / ends (rollback.cpp: an online fight's rollback session): the community server
// keeps the matches played on it (its match history; only a server that asks for them, community_net.h).
void XSessionReportMatch(bool started)
{
    const std::shared_ptr<community::Client> server = net::Community();
    if (!server || !server->Connected()) return;
    uint64_t id = 0;
    uint8_t local = 0;
    {
        std::lock_guard l(g_mutex);
        for (const auto& [handle, s] : g_sessions)
            if (s.created && s.state != State::Deleted && (s.flags & (kMatchmaking | kPeerNetwork)) && s.id)
            {
                id = s.id;
                local = 0;
                for (const Member& m : s.members) local += m.userIndex < 4 ? 1 : 0;
            }
    }
    const uint64_t opponent = XSocketMatchPeer();
    std::vector<uint64_t> peers;
    if (opponent) peers.push_back(opponent);
    server->ReportMatch(id, started, (std::max)(local, uint8_t(1)), peers);
    Log(started ? "matchstart" : "matchend", "match %s in session %016llX (opponent %016llX) reported to the server",
        started ? "start" : "end", (unsigned long long)id, (unsigned long long)opponent);
}

// This instance hosts an active session (created with XSESSION_CREATE_HOST) - rollback_session.cpp:
// the host's ONLINE NETCODE setting decides the shared input delay.
bool XSessionLocalHosts()
{
    std::lock_guard l(g_mutex);
    for (const auto& [handle, s] : g_sessions)
        if (s.created && s.state != State::Deleted && (s.flags & kHost)) return true;
    return false;
}

// The host's ONLINE NETCODE as its session record carries it (mm::SessionRecord::netcode), for a
// session this instance joined; 0 when unknown.
uint32_t XSessionHostNetcode()
{
    uint64_t joined = 0;
    {
        std::lock_guard l(g_mutex);
        for (const auto& [handle, s] : g_sessions)
            if (s.created && s.state != State::Deleted && !(s.flags & kHost) && s.id) joined = s.id;
    }
    mm::SessionRecord r;
    return joined && mm::Find(joined, r) ? r.netcode : 0;
}

// ---- session handles ------------------------------------------------------------------------------
PPC_FUNC(__imp__XamSessionCreateHandle)
{
    KTrace("XamSessionCreateHandle");
    const uint32_t out = ctx.r3.u32;
    if (!out) { ctx.r3.u64 = E_INVALIDARG_; return; }
    const uint32_t object = xlive::GuestAlloc(4);
    std::lock_guard l(g_mutex);
    const uint32_t handle = g_nextSession++;
    Session s;
    s.guestObject = object;
    g_sessions.emplace(handle, std::move(s));
    GWrite32(base, object, handle);
    GWrite32(base, out, handle);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XamSessionRefObjByHandle)
{
    KTrace("XamSessionRefObjByHandle");
    std::lock_guard l(g_mutex);
    const auto it = g_sessions.find(ctx.r3.u32);
    if (it == g_sessions.end()) { ctx.r3.u64 = STATUS_INVALID_HANDLE_; return; }
    if (ctx.r4.u32) GWrite32(base, ctx.r4.u32, it->second.guestObject);
    ctx.r3.u64 = 0;
}

// ---- XGI dispatch ---------------------------------------------------------------------------------
uint32_t XgiDispatch(uint8_t* base, uint32_t msg, uint32_t b, uint32_t length, bool& handled)
{
    handled = true;
    switch (msg)
    {
    case 0x000B0006:                                         // XUserSetContext {user, -, xuid, id, value}
    {
        const uint32_t id = GRead32(base, b + 0x10), value = GRead32(base, b + 0x14);
        { std::lock_guard l(g_mutex); g_contexts[id] = value; }
        Log("ctx" + std::to_string(id), "XUserSetContext(%08X = %u)", id, value);
        return S_OK_;
    }
    case 0x000B0007:                                         // XUserSetProperty {user, -, xuid, id, size, data*}
    {
        const uint32_t id = GRead32(base, b + 0x10), size = GRead32(base, b + 0x14), data = GRead32(base, b + 0x18);
        if (data && size <= 0x1000)
        {
            std::lock_guard l(g_mutex);
            g_properties[id].assign(base + data, base + data + size);
        }
        Log("prop" + std::to_string(id), "XUserSetProperty(%08X, %u bytes)", id, size);
        return S_OK_;
    }
    case 0x000B0008: return S_OK_;                           // XUserWriteAchievements
    case 0x000B0010: return Create(base, b);
    case 0x000B0011:                                         // XSessionDelete
    {
        std::lock_guard l(g_mutex);
        Session* s = Lookup(base, GRead32(base, b));
        if (!s) return STATUS_INVALID_HANDLE_;
        s->state = State::Deleted;
        mm::Remove(s->id);
        XSocketForgetMatchPeer();                            // the next match names its own opponent
        Log("delete", "XSessionDelete(%016llX)", (unsigned long long)s->id);
        return S_OK_;
    }
    case 0x000B0012: return JoinLeave(base, b, true);
    case 0x000B0013: return JoinLeave(base, b, false);
    case 0x000B0014: return SetState(base, b, State::InGame, "Start");
    case 0x000B0015: return SetState(base, b, State::Reporting, "End");
    case 0x000B0016: case 0x000B001C: return Search(base, b);
    case 0x000B0018:                                         // XSessionModify {obj, flags, maxPub, maxPriv}
    {
        std::lock_guard l(g_mutex);
        Session* s = Lookup(base, GRead32(base, b));
        if (!s) return STATUS_INVALID_HANDLE_;
        s->flags = (s->flags & ~0xF80u) | (GRead32(base, b + 4) & 0xF80u);
        s->maxPublic = GRead32(base, b + 8);
        s->maxPrivate = GRead32(base, b + 0xC);
        Log("modify", "XSessionModify(flags %03X, %u / %u)", s->flags, s->maxPublic, s->maxPrivate);
        Publish(*s);
        return S_OK_;
    }
    case 0x000B001A: return ArbitrationRegister(base, b);
    case 0x000B001B: return SearchById(base, b);
    case 0x000B0020: return S_OK_;                           // XUserResetStatsView
    case 0x000B0021: return live_stats::ReadPlayers(base, b);  // XUserReadStats (the leaderboards, live_stats.h)
    case 0x000B0025:                                         // XSessionWriteStats: to the server (live_stats.h)
    {
        uint64_t id = 0;
        {
            std::lock_guard l(g_mutex);
            if (const Session* s = Lookup(base, GRead32(base, b))) id = s->id;
        }
        live_stats::Write(base, b, length, id);
        return S_OK_;
    }
    case 0x000B0026:                                         // XSessionFlushStats
        Log("flushstats", "XSessionFlushStats");
        return S_OK_;
    default:
        handled = false;
        (void)length;
        return E_FAIL_;
    }
}
