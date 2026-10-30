// Virtual Xbox sockets (port/ONLINE_PLAN.md phase 3): the game's NetDll socket API on top of the
// packet transport (net_transport.h) - no host sockets for game traffic. Addresses are the
// virtual IN_ADDRs XNetXnAddrToInAddr hands out (xlive.cpp: one per remote peer, 10.78.x.y; our
// own 10.77.x.y, also reachable as 127.0.0.1).
//  - datagrams (UDP 17 / VDP 254, payload untouched): one packet each, unreliable channel;
//  - streams (TCP): SYN / SYN-ACK / DATA / FIN / RST on the reliable channel, a connection is
//    (peer, connection id, direction);
//  - select / FIONBIO / FIONREAD / options as the game's code uses them (NACE: TCP 21000,
//    VDP 20000+n, UDP 10000 / 1000; scratch ttt2_online_recon.md section 2);
//  - service peers (xsocket_service.h): streams to an in-process handler instead of the transport.
// Guest structs are big-endian: XSOCKADDR_IN {u16 family, u16 port, u32 addr, 8 zero},
// fd_set {u32 count, u32 handles[64]}, timeval {s32 sec, s32 usec}.
// TTT2_NET_TRACE=1 logs every socket call and packet.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "net_transport.h"
#include "xlive.h"
#include "xsocket_service.h"
#include "title_server.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

void RollbackTimeSyncReceive(const uint8_t* data, size_t size);   // rollback_session.cpp

namespace {
std::atomic<uint64_t> g_timeSyncPeer{ 0 };

constexpr uint32_t WSAEWOULDBLOCK_ = 10035, WSAENOTSOCK_ = 10038, WSAEINVAL_ = 10022, WSAEFAULT_ = 10014;
constexpr uint32_t WSAEADDRINUSE_ = 10048, WSAEISCONN_ = 10056, WSAENOTCONN_ = 10057, WSAECONNREFUSED_ = 10061;
constexpr uint32_t WSAECONNRESET_ = 10054, WSAETIMEDOUT_ = 10060, WSAEHOSTUNREACH_ = 10065, WSAEMSGSIZE_ = 10040;
constexpr uint32_t WSAEPROTONOSUPPORT_ = 10043, WSAEOPNOTSUPP_ = 10045;

enum class Kind : uint8_t { Stream, Datagram };
enum class TcpState : uint8_t { Idle, Listening, Connecting, Connected, PeerClosed, Refused };

struct Datagram { uint32_t fromAddr; uint16_t fromPort; std::vector<uint8_t> data; };
struct Socket
{
    Kind kind = Kind::Datagram;
    uint8_t proto = 17;
    bool nonBlocking = false, bound = false, reuse = false, connector = false, sendShut = false;
    uint16_t port = 0;
    TcpState state = TcpState::Idle;
    uint64_t peer = 0;
    uint16_t peerPort = 0;
    uint32_t connId = 0;
    uint32_t pendingError = 0;
    std::deque<uint8_t> stream;
    std::deque<uint32_t> accepted;
    std::deque<Datagram> datagrams;
    std::map<uint32_t, uint32_t> options;
};

std::mutex g_m;
std::condition_variable g_cv;
std::map<uint32_t, Socket> g_sockets;
uint32_t g_nextHandle = 0xD4000000u;
uint16_t g_nextEphemeral = 49152;
thread_local uint32_t t_error = 0;

bool Trace() { static const bool on = std::getenv("TTT2_NET_TRACE") != nullptr; return on; }

// ---- wire format ----
// TimeSync: rollback sessions' frame-advantage reports (rollback_session.cpp), never a socket's.
// FileAsk / FileData: a player's license asked of the games connected to this one (title_server.cpp), never a socket's.
enum class Op : uint8_t { Datagram = 1, Syn = 2, SynAck = 3, Reset = 4, Data = 5, Fin = 6, TimeSync = 7, FileAsk = 8, FileData = 9 };
#pragma pack(push, 1)
struct Header { Op op; uint8_t proto; uint8_t toConnector; uint8_t pad; uint16_t srcPort, dstPort; uint32_t connId; };
#pragma pack(pop)

bool SendPacket(uint64_t peer, net::Channel channel, Op op, uint8_t proto, bool toConnector, uint16_t src, uint16_t dst,
                uint32_t connId, const uint8_t* data, size_t size)
{
    thread_local std::vector<uint8_t> packet;
    packet.resize(sizeof(Header) + size);
    Header h{ op, proto, uint8_t(toConnector), 0, src, dst, connId };
    std::memcpy(packet.data(), &h, sizeof(h));
    if (size) std::memcpy(packet.data() + sizeof(h), data, size);
    return net::Send(peer, channel, packet.data(), packet.size());
}

Socket* Find(uint32_t handle)
{
    const auto it = g_sockets.find(handle);
    return it == g_sockets.end() ? nullptr : &it->second;
}

int Fail(uint32_t error) { t_error = error; return -1; }

// Trace helper: "<what> <handle> ... <n> bytes: first 24 bytes in hex".
void TraceBytes(const char* what, uint32_t handle, uint32_t addr, uint16_t port, const uint8_t* data, int n)
{
    if (!Trace()) return;
    // TTT2_NET_TRACE=2: whole payloads (up to 2 KB); otherwise the first 24 bytes.
    static const int limit = [] { const char* e = std::getenv("TTT2_NET_TRACE"); return e && *e == '2' ? 2048 : 24; }();
    char hex[4100] = {};
    int at = 0;
    for (int i = 0; i < n && i < limit; ++i) at += std::snprintf(hex + at, sizeof(hex) - at, "%02X", data[i]);
    std::fprintf(stderr, "[xsocket] %s %08X %u.%u.%u.%u:%u %d bytes %s\n", what, handle, addr >> 24, (addr >> 16) & 0xFF,
                 (addr >> 8) & 0xFF, addr & 0xFF, port, n, hex);
}

uint16_t Ephemeral()
{
    for (;;)
    {
        const uint16_t p = g_nextEphemeral++;
        if (g_nextEphemeral == 0) g_nextEphemeral = 49152;
        bool used = false;
        for (auto& [h, s] : g_sockets) used |= s.bound && s.port == p;
        if (!used) return p;
    }
}

// Guest sockaddr <-> (addr, port), both in host order.
void ReadAddr(uint8_t* base, uint32_t sa, uint32_t& addr, uint16_t& port)
{
    port = GRead16(base, sa + 2);
    addr = GRead32(base, sa + 4);
}
void WriteAddr(uint8_t* base, uint32_t sa, uint32_t lenPtr, uint32_t addr, uint16_t port)
{
    if (!sa) return;
    std::memset(base + sa, 0, 16);
    GWrite16(base, sa, 2);
    GWrite16(base, sa + 2, port);
    GWrite32(base, sa + 4, addr);
    if (lenPtr) GWrite32(base, lenPtr, 16);
}

// Readiness for select (under g_m).
bool Readable(const Socket& s)
{
    if (s.kind == Kind::Datagram) return !s.datagrams.empty();
    if (s.state == TcpState::Listening) return !s.accepted.empty();
    return !s.stream.empty() || s.state == TcpState::PeerClosed || s.state == TcpState::Refused;
}
bool IsServicePeer(uint64_t peer);
bool Writable(const Socket& s)
{
    if (s.kind == Kind::Datagram) return true;
    // A service's stream it closed stays writable, as TCP after the peer's FIN (a send then fails): the GIN
    // client polls for writable before reading a response that ends with the server's close.
    return s.state == TcpState::Connected || (s.state == TcpState::PeerClosed && IsServicePeer(s.peer));
}
bool Excepted(const Socket& s) { return s.kind == Kind::Stream && s.state == TcpState::Refused; }

// ---- players' files from their games (title_server.cpp: another player's license when no server keeps it) ----
// FileAsk {u64 owner XUID, u8 length, item}; FileData {u64 owner, u8 length, item, u8 found, the file}. Every game
// heard from in the last 5 minutes (a ranked match's opponent, still on the results screen) is asked; each answers (found or not), so the asker stops at the first copy or when
// all said no.
struct FileWait { uint64_t owner; std::string item; int unanswered; bool found = false; std::vector<uint8_t> data; };
std::mutex g_fileMutex;
std::condition_variable g_fileCv;
std::vector<FileWait*> g_fileWaits;                         // under g_fileMutex
std::map<uint64_t, std::chrono::steady_clock::time_point> g_heardFrom;   // peers by their last packet, under g_fileMutex

std::vector<uint8_t> FilePayload(uint64_t owner, const std::string& item)
{
    std::vector<uint8_t> p(8);
    std::memcpy(p.data(), &owner, 8);
    p.push_back(uint8_t(item.size()));
    p.insert(p.end(), item.begin(), item.end());
    return p;
}

void FileReceive(uint64_t from, Op op, const uint8_t* p, size_t n)
{
    if (n < 9 || n < 9 + size_t(p[8])) return;
    uint64_t owner;
    std::memcpy(&owner, p, 8);
    const std::string item(reinterpret_cast<const char*>(p + 9), p[8]);
    p += 9 + item.size();
    n -= 9 + item.size();
    if (op == Op::FileAsk)
    {
        std::thread([from, owner, item] {
            std::vector<uint8_t> data;
            const bool found = TitleServerOwnItem(owner, item, data);
            std::vector<uint8_t> reply = FilePayload(owner, item);
            reply.push_back(found ? 1 : 0);
            if (found) reply.insert(reply.end(), data.begin(), data.end());
            SendPacket(from, net::Channel::Reliable, Op::FileData, 0, false, 0, 0, 0, reply.data(), reply.size());
            if (found)
                std::fprintf(stderr, "[xsocket] %016llX asked for %s: given (%zu bytes)\n", (unsigned long long)from, item.c_str(),
                             data.size());
        }).detach();
        return;
    }
    if (n < 1) return;
    const bool found = p[0] == 1 && n > 1 && n - 1 <= (16u << 10);
    std::lock_guard l(g_fileMutex);
    for (FileWait* w : g_fileWaits)
        if (w->owner == owner && w->item == item && !w->found)
        {
            if (found) { w->found = true; w->data.assign(p + 1, p + n); }
            --w->unanswered;
        }
    g_fileCv.notify_all();
}

// ---- incoming packets (transport thread) ----
void Receive(uint64_t from, net::Channel, const uint8_t* data, size_t size)
{
    if (size < sizeof(Header)) return;
    {
        std::lock_guard l(g_fileMutex);
        if (g_heardFrom.size() > 64) g_heardFrom.clear();
        g_heardFrom[from] = std::chrono::steady_clock::now();
    }
    Header h;
    std::memcpy(&h, data, sizeof(h));
    const uint8_t* payload = data + sizeof(h);
    const size_t payloadSize = size - sizeof(h);
    if (h.op == Op::TimeSync)
    {
        g_timeSyncPeer.store(from);
        RollbackTimeSyncReceive(payload, payloadSize);
        return;
    }
    if (h.op == Op::FileAsk || h.op == Op::FileData)
    {
        FileReceive(from, h.op, payload, payloadSize);
        return;
    }
    if (h.op == Op::Datagram) g_timeSyncPeer.store(from);   // the match's peer (NACE's datagrams)
    std::lock_guard l(g_m);
    const uint32_t fromAddr = xlive::AddrOfPeer(from);
    if (Trace())
        std::fprintf(stderr, "[xsocket] rx op %u proto %u %u->%u conn %08X %zu bytes from %016llX\n", unsigned(h.op), h.proto,
                     h.srcPort, h.dstPort, h.connId, payloadSize, (unsigned long long)from);
    switch (h.op)
    {
    case Op::Datagram:
    {
        bool delivered = false;
        for (auto& [handle, s] : g_sockets)
            if (s.kind == Kind::Datagram && s.bound && s.port == h.dstPort && s.proto == h.proto)
            {
                if (s.datagrams.size() < 512)
                {
                    s.datagrams.push_back({ fromAddr, h.srcPort, std::vector<uint8_t>(payload, payload + payloadSize) });
                    delivered = true;
                }
                break;
            }
        if (!delivered && Trace())
            std::fprintf(stderr, "[xsocket] rx datagram DROPPED: no socket bound to proto %u port %u (or queue full)\n",
                         h.proto, h.dstPort);
        break;
    }
    case Op::Syn:
    {
        Socket* listener = nullptr;
        for (auto& [handle, s] : g_sockets)
            if (s.kind == Kind::Stream && s.state == TcpState::Listening && s.port == h.dstPort) { listener = &s; break; }
        if (!listener || listener->accepted.size() >= 16)
        {
            SendPacket(from, net::Channel::Reliable, Op::Reset, h.proto, true, h.dstPort, h.srcPort, h.connId, nullptr, 0);
            break;
        }
        Socket c;
        c.kind = Kind::Stream;
        c.proto = 6;
        c.bound = true;
        c.port = h.dstPort;
        c.state = TcpState::Connected;
        c.peer = from;
        c.peerPort = h.srcPort;
        c.connId = h.connId;
        c.connector = false;
        const uint32_t handle = g_nextHandle++;
        g_sockets.emplace(handle, std::move(c));          // map nodes are stable: `listener` stays valid
        listener->accepted.push_back(handle);
        SendPacket(from, net::Channel::Reliable, Op::SynAck, 6, true, h.dstPort, h.srcPort, h.connId, nullptr, 0);
        break;
    }
    default:
        for (auto& [handle, s] : g_sockets)
        {
            if (s.kind != Kind::Stream || s.peer != from || s.connId != h.connId || s.connector != bool(h.toConnector)) continue;
            if (h.op == Op::SynAck && s.state == TcpState::Connecting) s.state = TcpState::Connected;
            else if (h.op == Op::Reset) { s.state = s.state == TcpState::Connecting ? TcpState::Refused : TcpState::PeerClosed; s.pendingError = WSAECONNRESET_; }
            else if (h.op == Op::Data) s.stream.insert(s.stream.end(), payload, payload + payloadSize);
            else if (h.op == Op::Fin) s.state = TcpState::PeerClosed;
            break;
        }
        break;
    }
    g_cv.notify_all();
}

bool EnsureTransport() { return net::Start(Receive); }

std::mutex g_serviceMutex;
std::map<uint64_t, xsvc::Handler*> g_services;
bool IsServicePeer(uint64_t peer) { return xsvc::Find(peer) != nullptr; }
} // namespace

namespace xsvc {
void Register(uint64_t peer, Handler* handler)
{
    std::lock_guard l(g_serviceMutex);
    g_services[peer] = handler;
}
Handler* Find(uint64_t peer)
{
    if ((peer & 0xFFFFFF0000000000ull) != kServicePeer) return nullptr;
    std::lock_guard l(g_serviceMutex);
    const auto it = g_services.find(peer);
    return it == g_services.end() ? nullptr : it->second;
}
void Reply(uint64_t peer, uint32_t conn, const uint8_t* data, size_t size)
{
    std::lock_guard l(g_m);
    for (auto& [handle, s] : g_sockets)
        if (s.kind == Kind::Stream && s.peer == peer && s.connId == conn && s.connector && s.state == TcpState::Connected)
        {
            s.stream.insert(s.stream.end(), data, data + size);
            break;
        }
    g_cv.notify_all();
}
void Finish(uint64_t peer, uint32_t conn)
{
    std::lock_guard l(g_m);
    for (auto& [handle, s] : g_sockets)
        if (s.kind == Kind::Stream && s.peer == peer && s.connId == conn && s.connector && s.state == TcpState::Connected)
        {
            s.state = TcpState::PeerClosed;
            break;
        }
    g_cv.notify_all();
}
} // namespace xsvc

uint64_t XSocketMatchPeer() { return g_timeSyncPeer.load(); }
void XSocketForgetMatchPeer() { g_timeSyncPeer.store(0); }

// rollback_session.cpp: a frame-advantage report to the match's peer (the last one whose datagrams
// arrived - a 1v1 match has one).
int XSocketAskPeersForFile(uint64_t owner, const std::string& item, std::vector<uint8_t>& data, int timeoutMs)
{
    if (!owner || item.empty() || item.size() > 32) return 0;
    std::vector<uint64_t> peers;
    {
        std::lock_guard l(g_fileMutex);
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [peer, at] : g_heardFrom)
            if (now - at < std::chrono::minutes(5) && peer != net::LocalPeer() && !IsServicePeer(peer)) peers.push_back(peer);
    }
    if (peers.empty()) return 0;
    FileWait wait{ owner, item, int(peers.size()) };
    {
        std::lock_guard l(g_fileMutex);
        g_fileWaits.push_back(&wait);
    }
    const std::vector<uint8_t> ask = FilePayload(owner, item);
    for (const uint64_t peer : peers)
        if (!SendPacket(peer, net::Channel::Reliable, Op::FileAsk, 0, false, 0, 0, 0, ask.data(), ask.size()))
        {
            std::lock_guard l(g_fileMutex);
            --wait.unanswered;
        }
    std::unique_lock l(g_fileMutex);
    g_fileCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return wait.found || wait.unanswered <= 0; });
    g_fileWaits.erase(std::find(g_fileWaits.begin(), g_fileWaits.end(), &wait));
    if (!wait.found) return 0;
    data = std::move(wait.data);
    return 1;
}

bool RollbackTimeSyncSend(const void* data, size_t size)
{
    const uint64_t peer = g_timeSyncPeer.load();
    return peer && SendPacket(peer, net::Channel::Unreliable, Op::TimeSync, 0, false, 0, 0, 0, static_cast<const uint8_t*>(data), size);
}

namespace {

// Wait (blocking sockets) until `ready` or the timeout; g_m held through `lock`.
template <typename F> bool WaitFor(std::unique_lock<std::mutex>& lock, F ready, int timeoutMs = 30000)
{
    return g_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
}

} // namespace

uint32_t XSocketLastError() { return t_error; }
bool XSocketStartTransport() { return EnsureTransport(); }

// ---- the NetDll socket API (r3 = caller) --------------------------------------------------------------
PPC_FUNC(__imp__NetDll_socket)
{
    KTrace("NetDll_socket");
    const uint32_t af = ctx.r4.u32, type = ctx.r5.u32, protocol = ctx.r6.u32;
    EnsureTransport();
    Socket s;
    if (af != 2) { ctx.r3.u64 = uint32_t(Fail(WSAEPROTONOSUPPORT_)); return; }
    if (type == 1 || (type == 0 && protocol == 6)) { s.kind = Kind::Stream; s.proto = 6; }
    else if (type == 2 || (type == 0 && (protocol == 17 || protocol == 254)))
    {
        s.kind = Kind::Datagram;
        s.proto = protocol == 254 ? 254 : 17;
    }
    else { ctx.r3.u64 = uint32_t(Fail(WSAEPROTONOSUPPORT_)); return; }
    std::lock_guard l(g_m);
    const uint32_t handle = g_nextHandle++;
    g_sockets.emplace(handle, std::move(s));
    if (Trace()) std::fprintf(stderr, "[xsocket] socket(type %u, proto %u) -> %08X\n", type, protocol, handle);
    ctx.r3.u64 = handle;
}

PPC_FUNC(__imp__NetDll_closesocket)
{
    KTrace("NetDll_closesocket");
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s) { ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_)); return; }
    if (s->kind == Kind::Stream && (s->state == TcpState::Connected || s->state == TcpState::PeerClosed) && !s->sendShut)
        if (xsvc::Handler* service = xsvc::Find(s->peer)) service->Close(s->connId);
    if (s->kind == Kind::Stream && s->state == TcpState::Connected && !s->sendShut && !xsvc::Find(s->peer))
        SendPacket(s->peer, net::Channel::Reliable, Op::Fin, 6, !s->connector, s->port, s->peerPort, s->connId, nullptr, 0);
    for (const uint32_t pending : s->accepted) g_sockets.erase(pending);
    g_sockets.erase(ctx.r4.u32);
    g_cv.notify_all();
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_shutdown)
{
    KTrace("NetDll_shutdown");
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s) { ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_)); return; }
    const uint32_t how = ctx.r5.u32;
    if (s->kind == Kind::Stream && s->state == TcpState::Connected && how >= 1 && !s->sendShut)
    {
        if (xsvc::Handler* service = xsvc::Find(s->peer)) service->Close(s->connId);
        else SendPacket(s->peer, net::Channel::Reliable, Op::Fin, 6, !s->connector, s->port, s->peerPort, s->connId, nullptr, 0);
        s->sendShut = true;
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_bind)
{
    KTrace("NetDll_bind");
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s) { ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_)); return; }
    if (!ctx.r5.u32 || ctx.r6.u32 < 16) { ctx.r3.u64 = uint32_t(Fail(WSAEFAULT_)); return; }
    uint32_t addr = 0;
    uint16_t port = 0;
    ReadAddr(base, ctx.r5.u32, addr, port);
    if (!port) port = Ephemeral();
    for (auto& [h, o] : g_sockets)
        if (&o != s && o.bound && o.port == port && o.kind == s->kind && o.proto == s->proto && !(s->reuse && o.reuse))
        { ctx.r3.u64 = uint32_t(Fail(WSAEADDRINUSE_)); return; }
    s->port = port;
    s->bound = true;
    if (Trace()) std::fprintf(stderr, "[xsocket] bind %08X proto %u port %u\n", ctx.r4.u32, s->proto, port);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_listen)
{
    KTrace("NetDll_listen");
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s) { ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_)); return; }
    if (s->kind != Kind::Stream || !s->bound) { ctx.r3.u64 = uint32_t(Fail(WSAEINVAL_)); return; }
    s->state = TcpState::Listening;
    if (Trace()) std::fprintf(stderr, "[xsocket] listen %08X port %u\n", ctx.r4.u32, s->port);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_accept)
{
    KTrace("NetDll_accept");
    const uint32_t handle = ctx.r4.u32, sa = ctx.r5.u32, lenPtr = ctx.r6.u32;
    std::unique_lock l(g_m);
    Socket* s = Find(handle);
    if (!s) { ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_)); return; }
    if (s->state != TcpState::Listening) { ctx.r3.u64 = uint32_t(Fail(WSAEINVAL_)); return; }
    if (s->accepted.empty())
    {
        if (s->nonBlocking) { ctx.r3.u64 = uint32_t(Fail(WSAEWOULDBLOCK_)); return; }
        WaitFor(l, [&] { Socket* x = Find(handle); return !x || !x->accepted.empty(); }, 0x7FFFFFFF);
        s = Find(handle);
        if (!s || s->accepted.empty()) { ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_)); return; }
    }
    const uint32_t child = s->accepted.front();
    s->accepted.pop_front();
    Socket* c = Find(child);
    if (c) WriteAddr(base, sa, lenPtr, xlive::AddrOfPeer(c->peer), c->peerPort);
    if (Trace()) std::fprintf(stderr, "[xsocket] accept %08X -> %08X\n", handle, child);
    ctx.r3.u64 = child;
}

PPC_FUNC(__imp__NetDll_connect)
{
    KTrace("NetDll_connect");
    const uint32_t handle = ctx.r4.u32;
    std::unique_lock l(g_m);
    Socket* s = Find(handle);
    if (!s) { ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_)); return; }
    if (!ctx.r5.u32) { ctx.r3.u64 = uint32_t(Fail(WSAEFAULT_)); return; }
    uint32_t addr = 0;
    uint16_t port = 0;
    ReadAddr(base, ctx.r5.u32, addr, port);
    const uint64_t peer = xlive::PeerOfAddr(addr);
    if (!peer) { ctx.r3.u64 = uint32_t(Fail(WSAEHOSTUNREACH_)); return; }
    if (!s->bound) { s->port = Ephemeral(); s->bound = true; }
    if (s->kind == Kind::Datagram)
    {
        s->peer = peer;                                    // a default destination
        s->peerPort = port;
        ctx.r3.u64 = 0;
        return;
    }
    if (s->state == TcpState::Connected) { ctx.r3.u64 = uint32_t(Fail(WSAEISCONN_)); return; }
    s->peer = peer;
    s->peerPort = port;
    xlive::Random(&s->connId, sizeof(s->connId));
    s->connector = true;
    if (xsvc::Handler* service = xsvc::Find(peer))
    {
        s->state = TcpState::Connected;
        service->Open(s->connId, port);
        if (Trace()) std::fprintf(stderr, "[xsocket] connect %08X -> service %016llX:%u\n", handle, (unsigned long long)peer, port);
        ctx.r3.u64 = 0;
        return;
    }
    s->state = TcpState::Connecting;
    SendPacket(peer, net::Channel::Reliable, Op::Syn, 6, false, s->port, port, s->connId, nullptr, 0);
    if (Trace()) std::fprintf(stderr, "[xsocket] connect %08X -> peer %016llX:%u\n", handle, (unsigned long long)peer, port);
    if (s->nonBlocking) { ctx.r3.u64 = uint32_t(Fail(WSAEWOULDBLOCK_)); return; }
    const bool done = WaitFor(l, [&] { Socket* x = Find(handle); return !x || x->state != TcpState::Connecting; }, 10000);
    s = Find(handle);
    if (!s) { ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_)); return; }
    if (!done) { s->state = TcpState::Refused; ctx.r3.u64 = uint32_t(Fail(WSAETIMEDOUT_)); return; }
    if (s->state != TcpState::Connected) { ctx.r3.u64 = uint32_t(Fail(WSAECONNREFUSED_)); return; }
    ctx.r3.u64 = 0;
}

// send / sendto: streams go out as reliable DATA chunks, datagrams as one unreliable packet.
static int SendCommon(uint8_t* base, uint32_t handle, uint32_t buffer, uint32_t length, uint32_t to)
{
    std::lock_guard l(g_m);
    Socket* s = Find(handle);
    if (!s) return Fail(WSAENOTSOCK_);
    if (length && !buffer) return Fail(WSAEFAULT_);
    if (s->kind == Kind::Stream)
    {
        if (s->state != TcpState::Connected || s->sendShut) return Fail(WSAENOTCONN_);
        TraceBytes("send", handle, xlive::AddrOfPeer(s->peer), s->peerPort, base + buffer, int(length));
        if (xsvc::Handler* service = xsvc::Find(s->peer))
        {
            service->Data(s->connId, base + buffer, length);
            return int(length);
        }
        for (uint32_t done = 0; done < length;)
        {
            const uint32_t chunk = (std::min)(length - done, 16384u);
            SendPacket(s->peer, net::Channel::Reliable, Op::Data, 6, !s->connector, s->port, s->peerPort, s->connId,
                       base + buffer + done, chunk);
            done += chunk;
        }
        return int(length);
    }
    uint64_t peer = s->peer;
    uint16_t port = s->peerPort;
    if (to)
    {
        uint32_t addr = 0;
        ReadAddr(base, to, addr, port);
        if (addr == 0xFFFFFFFFu) return int(length);       // LAN broadcast: no LAN here
        peer = xlive::PeerOfAddr(addr);
    }
    if (!peer || xsvc::Find(peer))
    {
        TraceBytes("sendto(unknown address, dropped)", handle, 0, port, base + buffer, int(length));
        return int(length);                                 // unknown address: lost, as with UDP
    }
    if (length > 1400) return Fail(WSAEMSGSIZE_);
    if (!s->bound) { s->port = Ephemeral(); s->bound = true; }
    SendPacket(peer, net::Channel::Unreliable, Op::Datagram, s->proto, false, s->port, port, 0, base + buffer, length);
    TraceBytes("sendto", handle, xlive::AddrOfPeer(peer), port, base + buffer, int(length));
    return int(length);
}
PPC_FUNC(__imp__NetDll_send)
{
    KTrace("NetDll_send");
    ctx.r3.u64 = uint32_t(SendCommon(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0));
}
bool RollbackSuppressSend();   // rollback.cpp: an online task's send in a re-simulated frame
PPC_FUNC(__imp__NetDll_sendto)
{
    KTrace("NetDll_sendto");
    // r4 socket, r5 buffer, r6 length, r7 flags, r8 to, r9 tolen
    // A rollback re-simulation runs the game's online tasks again: what they send went out when the
    // frame first ran - reported as sent, not sent twice.
    if (RollbackSuppressSend()) { ctx.r3.u64 = ctx.r6.u32; return; }
    ctx.r3.u64 = uint32_t(SendCommon(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r8.u32));
}

// recv / recvfrom
static int RecvCommon(uint8_t* base, uint32_t handle, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t from,
                      uint32_t fromLen)
{
    std::unique_lock l(g_m);
    Socket* s = Find(handle);
    if (!s) return Fail(WSAENOTSOCK_);
    const bool peek = flags & 2;
    if (s->kind == Kind::Datagram)
    {
        if (s->datagrams.empty())
        {
            if (s->nonBlocking) return Fail(WSAEWOULDBLOCK_);
            WaitFor(l, [&] { Socket* x = Find(handle); return !x || !x->datagrams.empty(); }, 0x7FFFFFFF);
            s = Find(handle);
            if (!s || s->datagrams.empty()) return Fail(WSAENOTSOCK_);
        }
        Datagram& d = s->datagrams.front();
        const uint32_t n = (std::min)(length, uint32_t(d.data.size()));
        if (n) std::memcpy(base + buffer, d.data.data(), n);
        WriteAddr(base, from, fromLen, d.fromAddr, d.fromPort);
        TraceBytes("recvfrom", handle, d.fromAddr, d.fromPort, d.data.data(), int(d.data.size()));
        const bool truncated = d.data.size() > length;
        if (!peek) s->datagrams.pop_front();
        if (truncated) return Fail(WSAEMSGSIZE_);
        return int(n);
    }
    if (s->state == TcpState::Listening || s->state == TcpState::Idle || s->state == TcpState::Connecting)
        return Fail(s->state == TcpState::Connecting && s->nonBlocking ? WSAEWOULDBLOCK_ : WSAENOTCONN_);
    if (s->stream.empty())
    {
        if (s->state == TcpState::PeerClosed) return s->pendingError ? Fail(WSAECONNRESET_) : 0;
        if (s->state == TcpState::Refused) return Fail(WSAECONNRESET_);
        if (s->nonBlocking) return Fail(WSAEWOULDBLOCK_);
        WaitFor(l, [&] { Socket* x = Find(handle); return !x || !x->stream.empty() || x->state != TcpState::Connected; }, 0x7FFFFFFF);
        s = Find(handle);
        if (!s) return Fail(WSAENOTSOCK_);
        if (s->stream.empty()) return s->state == TcpState::PeerClosed && !s->pendingError ? 0 : Fail(WSAECONNRESET_);
    }
    const uint32_t n = (std::min)(length, uint32_t(s->stream.size()));
    std::copy(s->stream.begin(), s->stream.begin() + n, base + buffer);
    TraceBytes("recv", handle, xlive::AddrOfPeer(s->peer), s->peerPort, base + buffer, int(n));
    if (!peek) s->stream.erase(s->stream.begin(), s->stream.begin() + n);
    WriteAddr(base, from, fromLen, xlive::AddrOfPeer(s->peer), s->peerPort);
    return int(n);
}
PPC_FUNC(__imp__NetDll_recv)
{
    KTrace("NetDll_recv");
    ctx.r3.u64 = uint32_t(RecvCommon(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, 0, 0));
}
PPC_FUNC(__imp__NetDll_recvfrom)
{
    KTrace("NetDll_recvfrom");
    // r4 socket, r5 buffer, r6 length, r7 flags, r8 from, r9 fromlen
    if (ctx.r8.u32) { std::memset(base + ctx.r8.u32, 0, 16); GWrite16(base, ctx.r8.u32, 2); }
    ctx.r3.u64 = uint32_t(RecvCommon(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32));
}

// select(caller, nfds, read, write, except, timeout): handles are ours; ready ones are kept.
PPC_FUNC(__imp__NetDll_select)
{
    KTrace("NetDll_select");
    const uint32_t sets[3] = { ctx.r5.u32, ctx.r6.u32, ctx.r7.u32 }, timeout = ctx.r8.u32;
    int64_t waitMs = -1;
    if (timeout)
        waitMs = int64_t(int32_t(GRead32(base, timeout))) * 1000 + int32_t(GRead32(base, timeout + 4)) / 1000;
    std::vector<uint32_t> want[3];
    for (int k = 0; k < 3; ++k)
        if (sets[k])
        {
            const uint32_t n = (std::min)(GRead32(base, sets[k]), 64u);
            for (uint32_t i = 0; i < n; ++i) want[k].push_back(GRead32(base, sets[k] + 4 + i * 4));
        }
    std::unique_lock l(g_m);
    std::vector<uint32_t> ready[3];
    auto scan = [&] {
        int total = 0;
        for (int k = 0; k < 3; ++k)
        {
            ready[k].clear();
            for (const uint32_t h : want[k])
            {
                const Socket* s = Find(h);
                if (!s) continue;
                if ((k == 0 && Readable(*s)) || (k == 1 && Writable(*s)) || (k == 2 && Excepted(*s))) ready[k].push_back(h);
            }
            total += int(ready[k].size());
        }
        return total;
    };
    for (int k = 0; k < 3; ++k)
        for (const uint32_t h : want[k])
            if (!Find(h)) { ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_)); return; }
    int count = scan();
    if (!count && waitMs != 0)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs < 0 ? 0x7FFFFFFF : waitMs);
        while (!count && g_cv.wait_until(l, deadline) != std::cv_status::timeout) count = scan();
        if (!count) count = scan();
    }
    for (int k = 0; k < 3; ++k)
        if (sets[k])
        {
            GWrite32(base, sets[k], uint32_t(ready[k].size()));
            for (size_t i = 0; i < ready[k].size(); ++i) GWrite32(base, sets[k] + 4 + uint32_t(i) * 4, ready[k][i]);
        }
    ctx.r3.u64 = uint32_t(count);
}

PPC_FUNC(__imp__NetDll___WSAFDIsSet)
{
    KTrace("NetDll___WSAFDIsSet");
    const uint32_t handle = ctx.r3.u32, set = ctx.r4.u32;
    uint32_t found = 0;
    if (set)
    {
        const uint32_t n = (std::min)(GRead32(base, set), 64u);
        for (uint32_t i = 0; i < n && !found; ++i) found = GRead32(base, set + 4 + i * 4) == handle;
    }
    ctx.r3.u64 = found;
}

// ioctlsocket: FIONBIO 0x8004667E (non-blocking), FIONREAD 0x4004667F (bytes waiting).
PPC_FUNC(__imp__NetDll_ioctlsocket)
{
    KTrace("NetDll_ioctlsocket");
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s) { ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_)); return; }
    const uint32_t cmd = ctx.r5.u32, arg = ctx.r6.u32;
    if (cmd == 0x8004667Eu) { s->nonBlocking = arg && GRead32(base, arg); ctx.r3.u64 = 0; return; }
    if (cmd == 0x4004667Fu)
    {
        const uint32_t n = s->kind == Kind::Stream ? uint32_t(s->stream.size())
                                                   : (s->datagrams.empty() ? 0 : uint32_t(s->datagrams.front().data.size()));
        if (arg) GWrite32(base, arg, n);
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = uint32_t(Fail(WSAEOPNOTSUPP_));
}

// setsockopt(caller, s, level, name, value*, len): kept (SO_REUSEADDR 4 noted; Xbox-only 0x58xx).
PPC_FUNC(__imp__NetDll_setsockopt)
{
    KTrace("NetDll_setsockopt");
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s) { ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_)); return; }
    const uint32_t level = ctx.r5.u32, name = ctx.r6.u32, value = ctx.r7.u32, length = ctx.r8.u32;
    uint32_t v = 0;
    if (value && length >= 4) v = GRead32(base, value);
    else if (value && length >= 1) v = GRead8(base, value);
    s->options[(level << 16) ^ name] = v;
    if (level == 0xFFFF && name == 4) s->reuse = v != 0;
    ctx.r3.u64 = 0;
}

// getsockopt: SO_TYPE 0x1008, SO_ERROR 0x1007, buffer sizes and stored values.
PPC_FUNC(__imp__NetDll_getsockopt)
{
    KTrace("NetDll_getsockopt");
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s) { ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_)); return; }
    const uint32_t level = ctx.r5.u32, name = ctx.r6.u32, value = ctx.r7.u32, lenPtr = ctx.r8.u32;
    uint32_t v = 0;
    if (level == 0xFFFF && name == 0x1008) v = s->kind == Kind::Stream ? 1 : 2;
    else if (level == 0xFFFF && name == 0x1007) { v = s->pendingError; s->pendingError = 0; }
    else if (level == 0xFFFF && (name == 0x1001 || name == 0x1002))
    {
        const auto it = s->options.find((level << 16) ^ name);
        v = it == s->options.end() ? 65536 : it->second;
    }
    else
    {
        const auto it = s->options.find((level << 16) ^ name);
        v = it == s->options.end() ? 0 : it->second;
    }
    if (value) GWrite32(base, value, v);
    if (lenPtr) GWrite32(base, lenPtr, 4);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_getsockname)
{
    KTrace("NetDll_getsockname");
    std::lock_guard l(g_m);
    Socket* s = Find(ctx.r4.u32);
    if (!s) { ctx.r3.u64 = uint32_t(Fail(WSAENOTSOCK_)); return; }
    if (!s->bound) { ctx.r3.u64 = uint32_t(Fail(WSAEINVAL_)); return; }
    WriteAddr(base, ctx.r5.u32, ctx.r6.u32, xlive::AddrOfPeer(net::LocalPeer()), s->port);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_WSAGetLastError) { KTrace("NetDll_WSAGetLastError"); ctx.r3.u64 = t_error; }

// inet_addr("a.b.c.d") (no caller argument: the thunk jumps straight here): network-order
// address; "" = 0, invalid = INADDR_NONE.
PPC_FUNC(__imp__NetDll_inet_addr)
{
    KTrace("NetDll_inet_addr");
    const uint32_t text = ctx.r3.u32;
    if (!text) { ctx.r3.u64 = 0xFFFFFFFFu; return; }
    char s[32] = {};
    for (int i = 0; i < 31 && GRead8(base, text + i); ++i) s[i] = char(GRead8(base, text + i));
    if (!s[0]) { ctx.r3.u64 = 0; return; }
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra = 0;
    if (std::sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
    { ctx.r3.u64 = 0xFFFFFFFFu; return; }
    ctx.r3.u64 = (a << 24) | (b << 16) | (c << 8) | d;
}
