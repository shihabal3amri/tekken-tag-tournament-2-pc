// Steam backend, in-process part (steam_net.h `direct`): runs inside the --steam-bridge helper
// process, never in the game process (Steam Input would hide the controllers; steam_bridge.cpp).
// One service thread owns Steam's manual callback dispatch: it runs
// the frame, routes call results to waiters (by SteamAPICall_t), accepts peer sessions, turns
// "Join Game" requests into pending invites, and drains SteamNetworkingMessages to the receiver.
// Other threads only issue requests and wait on the results it stores.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "steam_net.h"
#include "net_transport.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#if TTT2_HAVE_STEAM
#include "steam/steam_api.h"
#include "steam/isteamnetworkingmessages.h"
#include "steam/isteamnetworkingutils.h"

bool SessionEnabled();   // rollback_session.cpp: rollback sessions (TTT2_ROLLBACK)
bool RollbackSessionOn(); // rollback.cpp: a rollback session runs (a match)

namespace steamnet::direct {
namespace {

// Lobbies of other builds / games are ignored: the protocol string (and its history) lives in
// net_transport.cpp, net::GameProtocol - community servers use the same one.
const char* Protocol() { return net::GameProtocol(); }
constexpr int kChannel = 0;

std::atomic<bool> g_running{ false };
std::atomic<uint64_t> g_local{ 0 };
Receiver g_receiver = nullptr;
HSteamPipe g_pipe = 0;

std::mutex g_mutex;
std::condition_variable g_cv;
std::map<SteamAPICall_t, std::vector<uint8_t>> g_results;      // finished call results
std::map<uint64_t, bool> g_lobbyData;                            // lobby -> data update arrived
struct Hosted { uint64_t lobby = 0; SteamAPICall_t pending = 0; std::vector<uint8_t> record; bool removed = false; };
std::map<uint64_t, Hosted> g_hosted;                             // session id -> its lobby
uint64_t g_joinLobby = 0;                                        // a "Join Game" request in progress
std::map<uint64_t, uint64_t> g_lobbyOf;                          // session id -> lobby, of sessions seen (searches, Join Game)
std::map<uint32_t, std::pair<int, std::vector<uint8_t>>> g_tickets;   // Web API tickets: handle -> result, bytes
std::vector<uint8_t> g_joinRecord;
bool g_joinReady = false;

std::string Hex(const std::vector<uint8_t>& b)
{
    static const char* d = "0123456789ABCDEF";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t x : b) { s.push_back(d[x >> 4]); s.push_back(d[x & 15]); }
    return s;
}
std::vector<uint8_t> Unhex(const char* s)
{
    std::vector<uint8_t> b;
    auto v = [](char c) { return c >= 'A' ? (c & 7) + 9 : c - '0'; };
    for (size_t i = 0; s && s[i] && s[i + 1]; i += 2) b.push_back(uint8_t(v(s[i]) << 4 | v(s[i + 1])));
    return b;
}
// A session record's id (matchmaking.cpp: u32 magic "SES2", u64 id little-endian); 0 when it is not one.
uint64_t SessionOf(const std::vector<uint8_t>& rec)
{
    if (rec.size() < 12 || rec[0] != 'S' || rec[1] != 'E' || rec[2] != 'S' || rec[3] != '2') return 0;
    uint64_t id = 0;
    for (int i = 0; i < 8; ++i) id |= uint64_t(rec[4 + i]) << (8 * i);
    return id;
}

// ---- diagnostics (GitHub issue #30: the helper ended after repeated SteamNetworkingSockets ICE
// assertions, and lobbies were gone until both games restarted)
// Steam's networking messages, warning level and up (bugs - assertions -, errors, important,
// warnings), into the log: each distinct message 3 times, then every 100th repeat, 300 lines at
// most. TTT2_STEAM_NETLOG=<1..8> picks another level (5 = Steam's recommended "msg").
std::mutex g_netLogMutex;
std::unordered_map<std::string, uint32_t> g_netLogSeen;
uint32_t g_netLogLines = 0;
void NetDebugOutput(ESteamNetworkingSocketsDebugOutputType type, const char* msg)
{
    static const char* kNames[] = { "none", "bug", "error", "important", "warning", "msg", "verbose", "debug", "everything" };
    std::string text(msg ? msg : "");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    std::string key;                                  // repeats differ in ids and times: digits ignored
    for (char c : text.substr(0, 160)) key.push_back(c >= '0' && c <= '9' ? '#' : c);
    std::lock_guard l(g_netLogMutex);
    uint32_t n = 1;
    if (g_netLogSeen.size() < 2000 || g_netLogSeen.count(key)) n = ++g_netLogSeen[key];
    if (g_netLogLines >= 300 || (n > 3 && n % 100 != 0)) return;
    ++g_netLogLines;
    const char* name = type >= 0 && type <= 8 ? kNames[type] : "?";
    if (n > 3) std::fprintf(stderr, "[steam-net] %s (%u times so far): %s\n", name, n, text.c_str());
    else std::fprintf(stderr, "[steam-net] %s: %s\n", name, text.c_str());
    if (g_netLogLines == 300) std::fprintf(stderr, "[steam-net] (no more Steam networking messages in this log)\n");
}

// Each peer's session: state, route (direct or relayed, and through which relay), ping - logged when
// the state or route changes and every 30 s while connected; during a rollback match every 5 s, with the
// link's real-time status (rates, bytes waiting, queue time): late inputs from the network show there,
// a frame offset between the two games does not.
struct PeerInfo { int state = -1, flags = -1; uint32_t relay = 0; std::chrono::steady_clock::time_point logged{}; };
std::mutex g_peerMutex;
std::map<uint64_t, PeerInfo> g_peers;
void NotePeer(uint64_t peer)
{
    std::lock_guard l(g_peerMutex);
    if (g_peers.size() < 64) g_peers.try_emplace(peer);
}
const char* StateName(int s)
{
    switch (s)
    {
    case k_ESteamNetworkingConnectionState_None: return "no session";
    case k_ESteamNetworkingConnectionState_Connecting: return "connecting";
    case k_ESteamNetworkingConnectionState_FindingRoute: return "finding a route";
    case k_ESteamNetworkingConnectionState_Connected: return "connected";
    case k_ESteamNetworkingConnectionState_ClosedByPeer: return "closed by the peer";
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: return "problem detected locally";
    default: return "?";
    }
}
void ReportPeers()
{
    std::lock_guard l(g_peerMutex);
    const auto now = std::chrono::steady_clock::now();
    for (auto& [peer, last] : g_peers)
    {
        SteamNetworkingIdentity id;
        id.SetSteamID64(peer);
        SteamNetConnectionInfo_t info = {};
        SteamNetConnectionRealTimeStatus_t status = {};
        const int state = int(SteamNetworkingMessages()->GetSessionConnectionInfo(id, &info, &status));
        const bool changed = state != last.state || info.m_nFlags != last.flags || info.m_idPOPRelay != last.relay;
        const bool inMatch = ::RollbackSessionOn();
        const bool periodic = state == k_ESteamNetworkingConnectionState_Connected &&
                              now - last.logged > std::chrono::seconds(inMatch ? 5 : 30);
        if (!changed && !periodic) continue;
        char route[96];
        if (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed)
        {
            if (info.m_idPOPRelay) std::snprintf(route, sizeof(route), "relayed via %s", SteamNetworkingPOPIDRender(info.m_idPOPRelay).c_str());
            else std::snprintf(route, sizeof(route), "relayed (TURN)");
        }
        else
        {
            char addr[SteamNetworkingIPAddr::k_cchMaxString] = "";
            info.m_addrRemote.ToString(addr, sizeof(addr), false);
            std::snprintf(route, sizeof(route), "direct%s%s", addr[0] ? " to " : "", addr);
        }
        if (state == k_ESteamNetworkingConnectionState_Connected || state == k_ESteamNetworkingConnectionState_FindingRoute ||
            state == k_ESteamNetworkingConnectionState_Connecting)
            std::fprintf(stderr, "[steam] connection to %016llX: %s, %s, ping %d ms, quality %.0f%% here / %.0f%% there%s\n",
                         (unsigned long long)peer, StateName(state), route, status.m_nPing,
                         status.m_flConnectionQualityLocal * 100.0f, status.m_flConnectionQualityRemote * 100.0f,
                         (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Fast) ? ", LAN" : "");
        else
            std::fprintf(stderr, "[steam] connection to %016llX: %s (end reason %d: %s)\n", (unsigned long long)peer,
                         StateName(state), info.m_eEndReason, info.m_szEndDebug);
        if (inMatch && state == k_ESteamNetworkingConnectionState_Connected)
            std::fprintf(stderr, "[steam] link to %016llX: out %.1f KB/s %.0f packets/s, in %.1f KB/s %.0f packets/s, send rate %d KB/s; waiting %d unreliable / %d reliable bytes, %d sent not acked, queue %lld us\n",
                         (unsigned long long)peer, status.m_flOutBytesPerSec / 1024.0f, status.m_flOutPacketsPerSec,
                         status.m_flInBytesPerSec / 1024.0f, status.m_flInPacketsPerSec, status.m_nSendRateBytesPerSecond / 1024,
                         status.m_cbPendingUnreliable, status.m_cbPendingReliable, status.m_cbSentUnackedReliable,
                         (long long)status.m_usecQueueTime);
        last.state = state;
        last.flags = info.m_nFlags;
        last.relay = info.m_idPOPRelay;
        last.logged = now;
    }
}

void WriteLobbyData(uint64_t lobby, const std::vector<uint8_t>& record)
{
    CSteamID id(lobby);
    SteamMatchmaking()->SetLobbyData(id, "proto", Protocol());
    SteamMatchmaking()->SetLobbyData(id, "rec", Hex(record).c_str());
}

// Service thread: callbacks, call results, packets.
void Service()
{
    std::vector<SteamNetworkingMessage_t*> messages(64);
    auto lastReport = std::chrono::steady_clock::now();
    for (;;)
    {
        SteamAPI_ManualDispatch_RunFrame(g_pipe);
        CallbackMsg_t cb;
        while (SteamAPI_ManualDispatch_GetNextCallback(g_pipe, &cb))
        {
            if (cb.m_iCallback == SteamAPICallCompleted_t::k_iCallback)
            {
                const auto* done = reinterpret_cast<const SteamAPICallCompleted_t*>(cb.m_pubParam);
                std::vector<uint8_t> result(done->m_cubParam);
                bool failed = false;
                if (SteamAPI_ManualDispatch_GetAPICallResult(g_pipe, done->m_hAsyncCall, result.data(), int(result.size()),
                                                             done->m_iCallback, &failed) && !failed)
                {
                    std::lock_guard l(g_mutex);
                    // A hosted lobby being created: store its id and write the data.
                    bool hostedCall = false;
                    for (auto& [sid, h] : g_hosted)
                        if (h.pending == done->m_hAsyncCall)
                        {
                            hostedCall = true;
                            h.pending = 0;
                            const auto* created = reinterpret_cast<const LobbyCreated_t*>(result.data());
                            if (created->m_eResult == k_EResultOK)
                            {
                                h.lobby = created->m_ulSteamIDLobby;
                                if (h.removed) SteamMatchmaking()->LeaveLobby(CSteamID(h.lobby));
                                else WriteLobbyData(h.lobby, h.record);
                                std::fprintf(stderr, "[steam] lobby %016llX hosts session %016llX\n",
                                             (unsigned long long)h.lobby, (unsigned long long)sid);
                            }
                            else
                                std::fprintf(stderr, "[steam] CreateLobby failed (%d)\n", int(created->m_eResult));
                        }
                    if (!hostedCall) g_results[done->m_hAsyncCall] = std::move(result);
                    g_cv.notify_all();
                }
            }
            else if (cb.m_iCallback == SteamNetworkingMessagesSessionRequest_t::k_iCallback)
            {
                const auto* req = reinterpret_cast<const SteamNetworkingMessagesSessionRequest_t*>(cb.m_pubParam);
                SteamNetworkingMessages()->AcceptSessionWithUser(req->m_identityRemote);
                NotePeer(req->m_identityRemote.GetSteamID64());
                std::fprintf(stderr, "[steam] session request from %016llX: accepted\n",
                             (unsigned long long)req->m_identityRemote.GetSteamID64());
            }
            else if (cb.m_iCallback == SteamNetworkingMessagesSessionFailed_t::k_iCallback)
            {
                const auto* f = reinterpret_cast<const SteamNetworkingMessagesSessionFailed_t*>(cb.m_pubParam);
                std::fprintf(stderr, "[steam] session with %016llX failed (end reason %d): %s\n",
                             (unsigned long long)f->m_info.m_identityRemote.GetSteamID64(), f->m_info.m_eEndReason, f->m_info.m_szEndDebug);
            }
            else if (cb.m_iCallback == SteamShutdown_t::k_iCallback)
                std::fprintf(stderr, "[steam] Steam is shutting down (SteamShutdown_t)\n");
            else if (cb.m_iCallback == SteamServersDisconnected_t::k_iCallback)
                std::fprintf(stderr, "[steam] lost the connection to the Steam servers (result %d)\n",
                             int(reinterpret_cast<const SteamServersDisconnected_t*>(cb.m_pubParam)->m_eResult));
            else if (cb.m_iCallback == SteamServerConnectFailure_t::k_iCallback)
            {
                const auto* f = reinterpret_cast<const SteamServerConnectFailure_t*>(cb.m_pubParam);
                std::fprintf(stderr, "[steam] could not connect to the Steam servers (result %d%s)\n", int(f->m_eResult),
                             f->m_bStillRetrying ? ", still retrying" : "");
            }
            else if (cb.m_iCallback == SteamServersConnected_t::k_iCallback)
                std::fprintf(stderr, "[steam] connected to the Steam servers\n");
            else if (cb.m_iCallback == GetTicketForWebApiResponse_t::k_iCallback)
            {
                const auto* t = reinterpret_cast<const GetTicketForWebApiResponse_t*>(cb.m_pubParam);
                const int n = t->m_cubTicket < 0 ? 0 : (std::min)(t->m_cubTicket, int(GetTicketForWebApiResponse_t::k_nCubTicketMaxLength));
                std::lock_guard l(g_mutex);
                if (g_tickets.size() < 16)
                    g_tickets[t->m_hAuthTicket] = { int(t->m_eResult), std::vector<uint8_t>(t->m_rgubTicket, t->m_rgubTicket + n) };
                g_cv.notify_all();
            }
            else if (cb.m_iCallback == GameLobbyJoinRequested_t::k_iCallback)
            {
                const auto* j = reinterpret_cast<const GameLobbyJoinRequested_t*>(cb.m_pubParam);
                std::lock_guard l(g_mutex);
                g_joinLobby = j->m_steamIDLobby.ConvertToUint64();
                g_joinReady = false;
                SteamMatchmaking()->RequestLobbyData(j->m_steamIDLobby);
                std::fprintf(stderr, "[steam] Join Game request: lobby %016llX\n", (unsigned long long)g_joinLobby);
            }
            else if (cb.m_iCallback == LobbyDataUpdate_t::k_iCallback)
            {
                const auto* u = reinterpret_cast<const LobbyDataUpdate_t*>(cb.m_pubParam);
                std::lock_guard l(g_mutex);
                g_lobbyData[u->m_ulSteamIDLobby] = u->m_bSuccess != 0;
                if (u->m_ulSteamIDLobby == g_joinLobby && u->m_bSuccess)
                {
                    const CSteamID lobby(g_joinLobby);
                    const char* proto = SteamMatchmaking()->GetLobbyData(lobby, "proto");
                    if (proto && std::strcmp(proto, Protocol()) == 0)
                    {
                        g_joinRecord = Unhex(SteamMatchmaking()->GetLobbyData(lobby, "rec"));
                        g_joinReady = !g_joinRecord.empty();
                        if (const uint64_t sid = SessionOf(g_joinRecord)) g_lobbyOf[sid] = g_joinLobby;
                    }
                    else
                        std::fprintf(stderr, "[steam] Join Game: not a session of this port version\n");
                    g_joinLobby = 0;
                }
                g_cv.notify_all();
            }
            SteamAPI_ManualDispatch_FreeLastCallback(g_pipe);
        }
        // Packets: first byte = reliable flag (as the loopback backend), then the packet.
        for (;;)
        {
            const int n = SteamNetworkingMessages()->ReceiveMessagesOnChannel(kChannel, messages.data(), int(messages.size()));
            if (n <= 0) break;
            for (int i = 0; i < n; ++i)
            {
                SteamNetworkingMessage_t* m = messages[size_t(i)];
                const auto* data = static_cast<const uint8_t*>(m->GetData());
                if (g_receiver && m->GetSize() >= 1)
                    g_receiver(m->m_identityPeer.GetSteamID64(), data[0] != 0, data + 1, size_t(m->GetSize() - 1));
                m->Release();
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::seconds(2))
        {
            lastReport = now;
            ReportPeers();
        }
        Sleep(1);
    }
}

// Wait for a call result of type T (on any thread); false on timeout.
template <typename T> bool Await(SteamAPICall_t call, T& out, int timeoutMs)
{
    std::unique_lock l(g_mutex);
    if (!g_cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return g_results.count(call) != 0; })) return false;
    const std::vector<uint8_t> r = std::move(g_results[call]);
    g_results.erase(call);
    if (r.size() < sizeof(T)) return false;
    std::memcpy(&out, r.data(), sizeof(T));
    return true;
}

} // namespace

bool Start(Receiver receiver)
{
    static std::mutex m;
    std::lock_guard l(m);
    if (g_running) return true;
    static bool tried = false;
    if (tried) return false;
    tried = true;
    if (!LoadLibraryW(L"steam_api64.dll"))
    {
        std::fprintf(stderr, "[steam] steam_api64.dll not found - Steam online unavailable\n");
        return false;
    }
    SetEnvironmentVariableW(L"SteamAppId", L"480");
    SetEnvironmentVariableW(L"SteamGameId", L"480");
    SteamErrMsg error = {};
    if (SteamAPI_InitEx(&error) != k_ESteamAPIInitResult_OK)
    {
        std::fprintf(stderr, "[steam] not available: %s\n", error);
        return false;
    }
    SteamAPI_ManualDispatch_Init();
    g_pipe = SteamAPI_GetHSteamPipe();
    {
        const char* e = std::getenv("TTT2_STEAM_NETLOG");
        const int level = e && *e ? std::atoi(e) : int(k_ESteamNetworkingSocketsDebugOutputType_Warning);
        SteamNetworkingUtils()->SetDebugOutputFunction(ESteamNetworkingSocketsDebugOutputType(level < 1 ? 1 : level > 8 ? 8 : level),
                                                       NetDebugOutput);
    }
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    g_receiver = receiver;
    g_local = SteamUser()->GetSteamID().ConvertToUint64();
    g_running = true;
    std::thread(Service).detach();
    std::fprintf(stderr, "[steam] running as %s (%016llX), app 480\n", SteamFriends()->GetPersonaName(),
                 (unsigned long long)g_local.load());
    return true;
}

bool Running() { return g_running.load(); }
uint64_t LocalId() { return g_local.load(); }

bool Send(uint64_t peer, bool reliable, const void* data, size_t size)
{
    if (!g_running || size + 1 > 512 * 1024) return false;
    thread_local uint64_t noted = 0;                     // the peer's route gets reported (ReportPeers)
    if (peer != noted) { noted = peer; NotePeer(peer); }
    thread_local std::vector<uint8_t> packet;
    packet.resize(size + 1);
    packet[0] = reliable ? 1 : 0;
    if (size) std::memcpy(packet.data() + 1, data, size);
    SteamNetworkingIdentity id;
    id.SetSteamID64(peer);
    const int flags = (reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoNagle) |
                      k_nSteamNetworkingSend_AutoRestartBrokenSession;
    return SteamNetworkingMessages()->SendMessageToUser(id, packet.data(), uint32_t(packet.size()), flags, kChannel) == k_EResultOK;
}

void PublishLobby(uint64_t sessionId, const std::vector<uint8_t>& record)
{
    if (!g_running) return;
    std::lock_guard l(g_mutex);
    Hosted& h = g_hosted[sessionId];
    h.record = record;
    h.removed = false;
    if (h.lobby) WriteLobbyData(h.lobby, record);
    else if (!h.pending) h.pending = SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, 8);
}

void RemoveLobby(uint64_t sessionId)
{
    if (!g_running) return;
    std::lock_guard l(g_mutex);
    const auto it = g_hosted.find(sessionId);
    if (it == g_hosted.end()) return;
    if (it->second.lobby) { SteamMatchmaking()->LeaveLobby(CSteamID(it->second.lobby)); g_hosted.erase(it); }
    else it->second.removed = true;                       // leave once the creation completes
}

std::vector<std::vector<uint8_t>> ListLobbies(int max, int timeoutMs)
{
    std::vector<std::vector<uint8_t>> out;
    if (!g_running) return out;
    SteamMatchmaking()->AddRequestLobbyListStringFilter("proto", Protocol(), k_ELobbyComparisonEqual);
    SteamMatchmaking()->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
    SteamMatchmaking()->AddRequestLobbyListResultCountFilter(max);
    const SteamAPICall_t call = SteamMatchmaking()->RequestLobbyList();
    LobbyMatchList_t list = {};
    if (!Await(call, list, timeoutMs)) { std::fprintf(stderr, "[steam] lobby list timed out\n"); return out; }
    for (uint32_t i = 0; i < list.m_nLobbiesMatching; ++i)
    {
        const CSteamID lobby = SteamMatchmaking()->GetLobbyByIndex(int(i));
        std::vector<uint8_t> rec = Unhex(SteamMatchmaking()->GetLobbyData(lobby, "rec"));
        if (const uint64_t sid = SessionOf(rec))
        {
            std::lock_guard l(g_mutex);
            if (g_lobbyOf.size() > 4096) g_lobbyOf.clear();
            g_lobbyOf[sid] = lobby.ConvertToUint64();
        }
        if (!rec.empty()) out.push_back(std::move(rec));
    }
    std::fprintf(stderr, "[steam] lobby list: %u lobbies, %zu sessions\n", list.m_nLobbiesMatching, out.size());
    return out;
}

bool TakeJoinRequest(std::vector<uint8_t>& record)
{
    std::lock_guard l(g_mutex);
    if (!g_joinReady) return false;
    g_joinReady = false;
    record = std::move(g_joinRecord);
    return true;
}

bool WebApiTicket(const std::string& identity, std::string& hex, uint32_t& handle, int timeoutMs)
{
    if (!g_running) return false;
    const HAuthTicket h = SteamUser()->GetAuthTicketForWebApi(identity.c_str());
    if (h == k_HAuthTicketInvalid) return false;
    std::unique_lock l(g_mutex);
    g_cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return g_tickets.count(h) != 0; });
    const auto it = g_tickets.find(h);
    if (it == g_tickets.end()) { l.unlock(); SteamUser()->CancelAuthTicket(h); return false; }
    const int result = it->second.first;
    hex = Hex(it->second.second);
    g_tickets.erase(it);
    l.unlock();
    if (result != k_EResultOK || hex.empty())
    {
        std::fprintf(stderr, "[steam] Web API ticket refused by Steam (result %d)\n", result);
        SteamUser()->CancelAuthTicket(h);
        hex.clear();
        return false;
    }
    handle = h;
    return true;
}

void CancelTicket(uint32_t handle)
{
    if (g_running && handle) SteamUser()->CancelAuthTicket(handle);
}

std::vector<FriendInfo> Friends()
{
    std::vector<FriendInfo> out;
    if (!g_running) return out;
    std::lock_guard l(g_mutex);
    const AppId_t app = SteamUtils()->GetAppID();
    const int n = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
    for (int i = 0; i < n && out.size() < 200; ++i)
    {
        const CSteamID id = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
        if (SteamFriends()->GetFriendPersonaState(id) == k_EPersonaStateOffline) continue;
        FriendInfo f;
        f.id = id.ConvertToUint64();
        const char* name = SteamFriends()->GetFriendPersonaName(id);
        f.name = name ? name : "";
        FriendGameInfo_t game{};
        f.inGame = SteamFriends()->GetFriendGamePlayed(id, &game) && game.m_gameID.AppID() == app;
        out.push_back(std::move(f));
    }
    auto lower = [](std::string s) { for (char& c : s) c = char(std::tolower(uint8_t(c))); return s; };
    std::stable_sort(out.begin(), out.end(), [&](const FriendInfo& a, const FriendInfo& b) {
        return a.inGame != b.inGame ? a.inGame : lower(a.name) < lower(b.name);
    });
    return out;
}

bool InviteToSession(uint64_t sessionId, uint64_t friendId)
{
    if (!g_running || !sessionId || !friendId) return false;
    std::lock_guard l(g_mutex);
    uint64_t lobby = 0;
    if (const auto h = g_hosted.find(sessionId); h != g_hosted.end()) lobby = h->second.lobby;
    if (!lobby)
        if (const auto s = g_lobbyOf.find(sessionId); s != g_lobbyOf.end()) lobby = s->second;
    if (!lobby)
    {
        std::fprintf(stderr, "[steam] invite: no Steam lobby known for session %016llX\n", (unsigned long long)sessionId);
        return false;
    }
    const bool ok = SteamMatchmaking()->InviteUserToLobby(CSteamID(lobby), CSteamID(friendId));
    std::fprintf(stderr, "[steam] invite of %016llX to lobby %016llX (session %016llX): %s\n", (unsigned long long)friendId,
                 (unsigned long long)lobby, (unsigned long long)sessionId, ok ? "sent" : "refused by Steam");
    return ok;
}

} // namespace steamnet::direct

#else  // no Steamworks SDK in this build

namespace steamnet::direct {
bool Start(Receiver) { return false; }
bool Running() { return false; }
uint64_t LocalId() { return 0; }
bool Send(uint64_t, bool, const void*, size_t) { return false; }
void PublishLobby(uint64_t, const std::vector<uint8_t>&) {}
void RemoveLobby(uint64_t) {}
std::vector<std::vector<uint8_t>> ListLobbies(int, int) { return {}; }
bool TakeJoinRequest(std::vector<uint8_t>&) { return false; }
bool WebApiTicket(const std::string&, std::string&, uint32_t&, int) { return false; }
void CancelTicket(uint32_t) {}
std::vector<FriendInfo> Friends() { return {}; }
bool InviteToSession(uint64_t, uint64_t) { return false; }
} // namespace steamnet::direct

#endif
