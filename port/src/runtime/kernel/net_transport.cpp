// Packet transport (net_transport.h). The loopback backend: one UDP socket per instance on
// 127.0.0.1:28100+n (the first free n of 64); peer id = 0x4C4F4F50'0000 | port. Localhost UDP does
// not reorder; a large receive buffer keeps it lossless in practice, which is enough for the
// two-instance test bed (Steam's reliable channel replaces it for real play). Tests can add delay,
// jitter and loss (TTT2_NET_DELAY / _JITTER / _LOSS, below).
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include "net_transport.h"
#include "community_net.h"
#include "profiles.h"
#include "steam_net.h"
#include "title_server.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <queue>
#include <thread>
#include <vector>

bool SessionEnabled();   // rollback_session.cpp: rollback sessions (TTT2_ROLLBACK)

#pragma comment(lib, "ws2_32.lib")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace net {
namespace {

constexpr uint64_t kLoopTag = 0x4C4F4F5000000000ull;      // "LOOP"
constexpr uint16_t kBasePort = 28100;

std::mutex g_mutex;
std::atomic<bool> g_started{ false };
std::atomic<uint64_t> g_local{ 0 };
SOCKET g_socket = INVALID_SOCKET;
Receiver g_receiver = nullptr;
bool g_steam = false;
// The community server players find each other on (sessions, invites, GIN); matches still run over this
// transport (Steam P2P). std::atomic_load / _store: matchmaking reads it from the game's threads.
std::shared_ptr<community::Client> g_server;
std::atomic<bool> g_serverActive{ false };
std::mutex g_serverMutex;                                   // one connect / disconnect at a time

// While a community server is the network, direct traffic is taken only from that server's players (the user: "when
// I join a server I play with the people on that server only"): a peer's first packet is held while the server is
// asked (PEER_CHECK), then its packets go through in order - or are dropped, and stay dropped (asked again after
// 10 s: the player may have joined the server since). A Steam player (no server) or another server's cannot reach a
// server player even knowing their Steam ID; the server's own lists never show them each other's rooms either.
// A peer let in is asked about again every 15 s while its packets come: one the server kicked or banned since (two
// "not a player" answers 5 s apart - a reconnecting player's short absence is no reason) is dropped like any other, so
// the room or match ends for it as for a disconnect (MEASURED 2026-10-23: a kicked player stayed in the room).
struct Held { Channel channel; std::vector<uint8_t> bytes; };
struct Gate
{
    enum State { Asking, Allowed, Denied } state = Asking;
    std::deque<Held> held;
    size_t heldBytes = 0;
    std::chrono::steady_clock::time_point decided;
    std::chrono::steady_clock::time_point recheck;           // Allowed: when the server is asked again
    bool rechecking = false;
    int misses = 0;                                          // "not a player" answers in a row
};
constexpr auto kRecheckEvery = std::chrono::seconds(15), kRecheckMiss = std::chrono::seconds(5);
std::mutex g_gateMutex;
std::map<uint64_t, Gate> g_gates;                           // under g_gateMutex
std::shared_ptr<community::Client> g_gateServer;            // whose players they are (a new network asks again)
constexpr size_t kMaxGates = 256, kMaxHeld = 256, kMaxHeldBytes = 256u << 10;

void Ask(std::shared_ptr<community::Client> server, uint64_t peer)
{
    std::thread([server, peer] {
        int answer = -1;
        for (int attempt = 0; attempt < 3 && answer < 0; ++attempt)
        {
            answer = server->PeerOnServer(peer, 4000);
            if (answer < 0 && server->Connected()) std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        std::lock_guard l(g_gateMutex);
        if (g_gateServer != server) return;                  // the network changed meanwhile
        Gate& g = g_gates[peer];
        g.decided = std::chrono::steady_clock::now();
        if (answer == 1)
        {
            g.state = Gate::Allowed;
            g.recheck = g.decided + kRecheckEvery;
            g.misses = 0;
            for (Held& h : g.held)
                if (g_receiver) g_receiver(peer, h.channel, h.bytes.data(), h.bytes.size());
        }
        else
        {
            g.state = Gate::Denied;
            std::fprintf(stderr, "[net] %016llX is not a player of this server: its packets are dropped\n", (unsigned long long)peer);
        }
        g.held.clear();
        g.heldBytes = 0;
    }).detach();
}

// An allowed peer asked about again (g_gateMutex not held).
void Recheck(std::shared_ptr<community::Client> server, uint64_t peer)
{
    std::thread([server, peer] {
        const int answer = server->PeerOnServer(peer, 4000);
        std::lock_guard l(g_gateMutex);
        if (g_gateServer != server) return;
        const auto it = g_gates.find(peer);
        if (it == g_gates.end() || it->second.state != Gate::Allowed) return;
        Gate& g = it->second;
        const auto now = std::chrono::steady_clock::now();
        g.rechecking = false;
        if (answer == 0 && ++g.misses >= 2)
        {
            g.state = Gate::Denied;
            g.decided = now;
            std::fprintf(stderr, "[net] %016llX is no longer a player of this server (kicked or banned): its packets are dropped\n",
                         (unsigned long long)peer);
            return;
        }
        if (answer == 1) g.misses = 0;
        g.recheck = now + (answer == 0 ? kRecheckMiss : kRecheckEvery);   // no answer (-1): as before, later
    }).detach();
}

void Deliver(uint64_t peer, Channel channel, const uint8_t* data, size_t size)
{
    if (!g_receiver) return;
    if (!g_serverActive.load())
    {
        g_receiver(peer, channel, data, size);
        return;
    }
    const std::shared_ptr<community::Client> server = std::atomic_load(&g_server);
    std::lock_guard l(g_gateMutex);                        // also keeps a peer's packets in order with a flush
    if (server != g_gateServer)
    {
        g_gates.clear();
        g_gateServer = server;
    }
    if (!server) return;
    auto it = g_gates.find(peer);
    if (it == g_gates.end())
    {
        if (g_gates.size() >= kMaxGates)
            for (auto d = g_gates.begin(); d != g_gates.end();)
                d = d->second.state == Gate::Denied ? g_gates.erase(d) : std::next(d);
        if (g_gates.size() >= kMaxGates) return;
        it = g_gates.emplace(peer, Gate{}).first;
        Ask(server, peer);
    }
    Gate& g = it->second;
    if (g.state == Gate::Denied && std::chrono::steady_clock::now() - g.decided > std::chrono::seconds(10))
    {
        g.state = Gate::Asking;                               // ask again: the player may be on the server now
        Ask(server, peer);
    }
    switch (g.state)
    {
    case Gate::Allowed:
        g_receiver(peer, channel, data, size);
        if (!g.rechecking && std::chrono::steady_clock::now() >= g.recheck)
        {
            g.rechecking = true;
            Recheck(server, peer);
        }
        break;
    case Gate::Asking:
        if (g.held.size() < kMaxHeld && g.heldBytes + size <= kMaxHeldBytes)
        {
            g.held.push_back({ channel, std::vector<uint8_t>(data, data + size) });
            g.heldBytes += size;
        }
        break;
    case Gate::Denied:
        break;
    }
}

void FromSteam(uint64_t peer, bool reliable, const uint8_t* data, size_t size)
{
    Deliver(peer, reliable ? Channel::Reliable : Channel::Unreliable, data, size);
}

// Network conditions for tests (loopback only): TTT2_NET_DELAY=<ms one way>, TTT2_NET_JITTER=<ms,
// 0..n more per packet>, TTT2_NET_LOSS=<percent of unreliable packets dropped>, TTT2_NET_SEED.
// Packets wait in a queue and leave from a sender thread at their time; the reliable channel is
// never dropped and keeps its order (it stands in for Steam's reliable messages).
struct Conditions { int delayMs = 0, jitterMs = 0, lossPct = 0; };
const Conditions& Cond()
{
    static const Conditions c = [] {
        auto get = [](const char* n, int hi) { const char* e = std::getenv(n); return e ? (std::max)(0, (std::min)(std::atoi(e), hi)) : 0; };
        Conditions r{ get("TTT2_NET_DELAY", 2000), get("TTT2_NET_JITTER", 2000), get("TTT2_NET_LOSS", 100) };
        if (r.delayMs || r.jitterMs || r.lossPct)
            std::fprintf(stderr, "[net] test conditions: %d ms one way + 0..%d ms jitter, %d%% unreliable loss\n", r.delayMs, r.jitterMs, r.lossPct);
        return r;
    }();
    return c;
}
struct Delayed
{
    std::chrono::steady_clock::time_point due;
    uint64_t order;
    sockaddr_in to;
    std::vector<uint8_t> bytes;
    bool operator>(const Delayed& o) const { return due != o.due ? due > o.due : order > o.order; }
};
std::mutex g_delayMutex;
std::condition_variable g_delayCv;
std::priority_queue<Delayed, std::vector<Delayed>, std::greater<Delayed>> g_delayed;
uint64_t g_delayOrder = 0;
std::chrono::steady_clock::time_point g_lastReliableDue{};
uint64_t g_rand = 0;
uint32_t NextRandom()
{
    if (!g_rand)
    {
        const char* e = std::getenv("TTT2_NET_SEED");
        g_rand = (e ? std::strtoull(e, nullptr, 10) : 0) * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
    }
    g_rand ^= g_rand << 13; g_rand ^= g_rand >> 7; g_rand ^= g_rand << 17;
    return uint32_t(g_rand >> 16);
}
void SendLoop()
{
    std::unique_lock l(g_delayMutex);
    for (;;)
    {
        if (g_delayed.empty()) { g_delayCv.wait(l); continue; }
        const auto due = g_delayed.top().due;
        if (std::chrono::steady_clock::now() < due) { g_delayCv.wait_until(l, due); continue; }
        Delayed d = g_delayed.top();
        g_delayed.pop();
        l.unlock();
        sendto(g_socket, reinterpret_cast<const char*>(d.bytes.data()), int(d.bytes.size()), 0,
               reinterpret_cast<const sockaddr*>(&d.to), sizeof(d.to));
        l.lock();
    }
}
// False: dropped (TTT2_NET_LOSS). The packet leaves later from SendLoop.
bool SendDelayed(const sockaddr_in& to, bool reliable, std::vector<uint8_t> bytes)
{
    const Conditions& c = Cond();
    std::lock_guard l(g_delayMutex);
    if (!reliable && c.lossPct && int(NextRandom() % 100) < c.lossPct) return true;   // lost on the way
    auto due = std::chrono::steady_clock::now() + std::chrono::milliseconds(c.delayMs + (c.jitterMs ? int(NextRandom() % uint32_t(c.jitterMs + 1)) : 0));
    if (reliable)
    {
        if (due < g_lastReliableDue) due = g_lastReliableDue;
        g_lastReliableDue = due;
    }
    static bool started = false;
    if (!started) { started = true; std::thread(SendLoop).detach(); }
    g_delayed.push({ due, g_delayOrder++, to, std::move(bytes) });
    g_delayCv.notify_one();
    return true;
}

void ReceiveLoop()
{
    std::vector<uint8_t> buffer(65536);
    for (;;)
    {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(g_socket, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n < 1) { if (n == SOCKET_ERROR && WSAGetLastError() == WSAENOTSOCK) return; continue; }
        const uint64_t peer = kLoopTag | ntohs(from.sin_port);
        // First byte: the channel; the rest is the packet.
        Deliver(peer, Channel(buffer[0] & 1), buffer.data() + 1, size_t(n - 1));
    }
}

} // namespace

std::string KnownServersFile()
{
    if (const char* e = std::getenv("TTT2_KNOWN_SERVERS")) return e;
    const char* local = std::getenv("LOCALAPPDATA");
    return std::string(local && *local ? local : ".") + "\\TTT2Recomp\\community\\known_servers.txt";
}

namespace {
// The login to a community server at `address` (its options; `ticket` is the Steam ticket made for it, cancelled by
// the caller once the server answered). False with `error` when there is none to make.
bool LoginOptions(const std::string& address, const std::string& password, community::Options& o, uint32_t& ticket,
                  std::string& error)
{
    if (!community::ParseAddress(address, o.host, o.port)) { error = "That is not a server address."; return false; }
    // The player is their Steam ID, proved with a Steam Web API ticket made for this server's identity
    // (COMMUNITY_SERVER_PLAN.md 7b S2): the sessions the server lists name their hosts by it, and the game reaches
    // them over Steam P2P as on Steam. TTT2_SERVER_TEST_ID (tests, on a server that trusts unverified IDs): the
    // loopback transport's peer id instead - two instances on one PC, their matches over 127.0.0.1.
    ticket = 0;
    if (!g_steam && std::getenv("TTT2_SERVER_TEST_ID")) o.steamId = g_local.load();
    else if (g_steam)
    {
        o.steamId = steamnet::LocalId();
        o.ticket = [&ticket](const std::string& identity) {
            // Test switches for the Steam checks (plan S7): another identity, the ticket written out.
            const char* other = std::getenv("TTT2_SERVER_TICKET_IDENTITY");
            std::string hex;
            if (!steamnet::WebApiTicket(other ? std::string(other) : identity, hex, ticket, 8000))
                std::fprintf(stderr, "[net] community server: Steam gave no ticket\n");
            if (const char* dump = std::getenv("TTT2_SERVER_TICKET_DUMP"))
                if (FILE* f = std::fopen(dump, "wb")) { std::fprintf(f, "%s\n%s\n", identity.c_str(), hex.c_str()); std::fclose(f); }
            return hex;
        };
    }
    else
    {
        error = "Steam is not running. Community servers know you by your Steam account: start Steam, then the game.";
        return false;
    }
    const profiles::Profile me = profiles::Active();
    o.profileName = me.name.empty() ? "PLAYER" : me.name;
    o.gameProtocol = GameProtocol();
    o.clientVersion = "tekken_recomp";
    o.knownServersFile = KnownServersFile();
    o.password = password;
    return true;
}
} // namespace

bool ConnectServer(const std::string& address, const std::string& password, community::Welcome& welcome,
                   std::string& error, int& refusal, uint64_t* banUntil)
{
    std::lock_guard l(g_serverMutex);
    refusal = community::RefusedNone;
    if (banUntil) *banUntil = ~0ull;
    community::Options o;
    uint32_t ticket = 0;
    if (!LoginOptions(address, password, o, ticket, error)) return false;
    auto client = std::make_shared<community::Client>();
    const bool connected = client->Connect(o, welcome, error);
    // The server checked the ticket (or refused): it is cancelled, so nobody can use it again.
    if (ticket && !std::getenv("TTT2_SERVER_TICKET_KEEP")) steamnet::CancelTicket(ticket);
    if (!connected)
    {
        refusal = client->Refusal();
        if (uint64_t until = 0; banUntil && client->BanUntil(until)) *banUntil = until;
        std::fprintf(stderr, "[net] community server %s: %s\n", address.c_str(), error.c_str());
        return false;
    }
    std::shared_ptr<community::Client> old = std::atomic_exchange(&g_server, client);
    g_serverActive = true;
    if (old) old->Disconnect();
    std::fprintf(stderr, "[net] network: community server %s (matches over %s)\n", address.c_str(), g_steam ? "Steam P2P" : "loopback");
    TitleServerShareOwnFiles(client);   // this player's license, for the others to view
    return true;
}

int ProbeServer(const std::string& address, const std::string& password, uint64_t& banUntil)
{
    std::lock_guard l(g_serverMutex);
    banUntil = ~0ull;
    community::Options o;
    uint32_t ticket = 0;
    std::string error;
    if (!LoginOptions(address, password, o, ticket, error)) return -1;
    o.timeoutMs = 6000;
    community::Client client;
    community::Welcome welcome;
    const bool in = client.Connect(o, welcome, error);
    if (ticket && !std::getenv("TTT2_SERVER_TICKET_KEEP")) steamnet::CancelTicket(ticket);
    if (in)
    {
        client.Disconnect();
        return 1;
    }
    if (client.Refusal() != community::RefusedBanned) return -1;
    client.BanUntil(banUntil);
    return 0;
}

void DisconnectServer()
{
    std::lock_guard l(g_serverMutex);
    std::shared_ptr<community::Client> old = std::atomic_exchange(&g_server, std::shared_ptr<community::Client>());
    g_serverActive = false;
    if (!old) return;
    old->Disconnect();
    std::fprintf(stderr, "[net] network: %s\n", g_steam ? "Steam" : "loopback");
}

const char* GameProtocol()
{
    // Rollback builds (TTT2_ROLLBACK, rollback_session.cpp) play a different netcode - a different
    // random-number split and the input delay picked by the host's ONLINE NETCODE - so they only meet
    // each other.
    // "-3": the time-sync reports carry an echo (rollback_session.cpp) - earlier rollback test builds differ.
    // "-4": the same wire format; test build 3 (2026-10-20) only meets itself, so a test is never a mix.
    // "-5": test build 4 - delay steering by the side the sync engine takes requests from, new presets.
    // "-6": test build 5 - the whole game image rewound (Havok's allocator objects).
    // "-7": test build 6 - Havok's workers rewound, the physics job finished before snapshots and restores.
    // "-8": test build 6c - an end-of-battle save a rollback replays is undone on disk first (save journal).
    // "-9": test build 6d - the file loaders at rest across snapshots and restores; the time sync slows the
    //       leader's vblank clock instead of yielding frames.
    // "-10": test build 7 - 6d plus the community servers (servers accept it: server/internal/config).
    return ::SessionEnabled() ? "ttt2-port-10-rollback" : "ttt2-port-1";
}

std::shared_ptr<community::Client> Community() { return std::atomic_load(&g_server); }

bool Start(Receiver receiver)
{
    std::lock_guard l(g_mutex);
    if (g_started) return true;
    const char* choice = std::getenv("TTT2_NET");
    const bool wantLoopback = choice && std::strcmp(choice, "loopback") == 0;
    const bool needSteam = choice && std::strcmp(choice, "steam") == 0;
    if (!wantLoopback)
    {
        g_receiver = receiver;
        if (steamnet::Start(FromSteam))
        {
            g_steam = true;
            g_local = steamnet::LocalId();
            g_started = true;
            return true;
        }
        if (needSteam) return false;
        std::fprintf(stderr, "[net] Steam unavailable - loopback transport (this PC only)\n");
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return false;
    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET) return false;
    int big = 8 << 20;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof(big));
    setsockopt(g_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&big), sizeof(big));
    BOOL exclusive = TRUE;
    setsockopt(g_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    // ICMP "port unreachable" must not turn into errors on the receive loop.
    BOOL off = FALSE;
    DWORD ignored = 0;
    WSAIoctl(g_socket, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ignored, nullptr, nullptr);
    uint16_t port = 0;
    for (uint16_t n = 0; n < 64 && !port; ++n)
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(kBasePort + n));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(g_socket, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) port = uint16_t(kBasePort + n);
    }
    if (!port) { closesocket(g_socket); g_socket = INVALID_SOCKET; return false; }
    g_receiver = receiver;
    g_local = kLoopTag | port;
    g_started = true;
    std::thread(ReceiveLoop).detach();
    std::fprintf(stderr, "[net] loopback transport on 127.0.0.1:%u (peer %016llX)\n", port,
                 (unsigned long long)g_local.load());
    return true;
}

uint64_t LocalPeer() { return g_local.load(); }
const char* BackendName() { return g_serverActive.load() ? "server" : g_steam ? "steam" : "loopback"; }

bool Send(uint64_t peer, Channel channel, const void* data, size_t size)
{
    if (g_steam) return steamnet::Send(peer, channel == Channel::Reliable, data, size);
    if (!g_started || (peer & 0xFFFFFFFF00000000ull) != kLoopTag || size + 1 > 65507) return false;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(peer & 0xFFFF));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    thread_local std::vector<uint8_t> packet;
    packet.resize(size + 1);
    packet[0] = uint8_t(channel);
    if (size) std::memcpy(packet.data() + 1, data, size);
    const Conditions& c = Cond();
    if (c.delayMs || c.jitterMs || c.lossPct) return SendDelayed(to, channel == Channel::Reliable, packet);
    return sendto(g_socket, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
                  reinterpret_cast<sockaddr*>(&to), sizeof(to)) == int(packet.size());
}

} // namespace net
