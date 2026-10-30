// The saved community servers and their live INFO answers (server_list.h).
//
// servers.ini: "[Network]" with Current=steam|<address>, then one "[Server]" section per saved server (Address=,
// Name=, Password=dpapi:<base64> - CryptProtectData, readable only by this Windows user). Written whole to a
// temporary file and moved over the old one.
//
// Questions go out from one thread with two UDP sockets (IPv4 with broadcast, IPv6); every question has its own
// random nonce, so an answer is matched to what was asked and nothing else. Names are resolved on short threads
// of their own (a dead DNS name never holds the others up).
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include "server_list.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace server_list {
namespace {

std::mutex g_mutex;
std::atomic<uint32_t> g_version{ 1 };
void Changed() { g_version.fetch_add(1); }

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint64_t Random64()
{
    uint64_t v = 0;
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&v), sizeof(v), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return v;
}
std::string Lower(std::string s)
{
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- servers.ini

std::string FilePath()
{
    if (const char* e = std::getenv("TTT2_SERVERS_INI")) return e;
    // Tests with their own save folder keep their own server list there (never the player's).
    if (const char* saves = std::getenv("TTT2_SAVE_ROOT"); saves && *saves) return std::string(saves) + "\\servers.ini";
    const char* local = std::getenv("LOCALAPPDATA");
    return std::string(local && *local ? local : ".") + "\\TTT2Recomp\\servers.ini";
}

std::string Protect(const std::string& plain)
{
    if (plain.empty()) return {};
    DATA_BLOB in{ DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data())) }, out{};
    if (!CryptProtectData(&in, L"TTT2 server password", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    DWORD chars = 0;
    std::string text;
    if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &chars))
    {
        text.resize(chars);
        CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, text.data(), &chars);
        text.resize(chars);
    }
    LocalFree(out.pbData);
    return text.empty() ? std::string() : "dpapi:" + text;
}

std::string Unprotect(const std::string& stored)
{
    if (stored.rfind("dpapi:", 0) != 0) return {};
    const std::string text = stored.substr(6);
    DWORD bytes = 0;
    if (!CryptStringToBinaryA(text.c_str(), DWORD(text.size()), CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr)) return {};
    std::vector<BYTE> blob(bytes);
    if (!CryptStringToBinaryA(text.c_str(), DWORD(text.size()), CRYPT_STRING_BASE64, blob.data(), &bytes, nullptr, nullptr)) return {};
    DATA_BLOB in{ bytes, blob.data() }, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

// One line of text in the file: no line breaks, printable.
std::string OneLine(const std::string& s, size_t maxLen)
{
    std::string out;
    for (unsigned char c : s)
        if (c >= 0x20 && c < 0x7F && out.size() < maxLen) out.push_back(char(c));
    return out;
}

struct Ban { std::string address; uint64_t until = 0; };   // SetBanned
struct Store
{
    bool loaded = false;
    std::vector<Saved> servers;
    std::string current;
    std::vector<Ban> bans;
} g_store;
uint64_t NowUnix() { return uint64_t(std::time(nullptr)); }
bool Active(const Ban& b) { return !b.until || b.until > NowUnix(); }

void Load()
{
    if (g_store.loaded) return;
    g_store.loaded = true;
    std::ifstream in(FilePath());
    std::string line, section;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[')
        {
            section = Lower(line);
            if (section == "[server]" && g_store.servers.size() < kMaxSaved) g_store.servers.push_back({});
            if (section == "[banned]" && g_store.bans.size() < kMaxSaved) g_store.bans.push_back({});
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Lower(line.substr(0, eq)), value = line.substr(eq + 1);
        if (section == "[network]" && key == "current")
            g_store.current = Lower(value) == "steam" ? std::string() : OneLine(value, 260);
        else if (section == "[server]" && !g_store.servers.empty())
        {
            Saved& s = g_store.servers.back();
            if (key == "address") s.address = OneLine(value, 260);
            else if (key == "name") s.name = OneLine(value, 64);
            else if (key == "password") s.password = Unprotect(value);
        }
        else if (section == "[banned]" && !g_store.bans.empty())
        {
            Ban& b = g_store.bans.back();
            if (key == "address") b.address = OneLine(value, 260);
            else if (key == "until") b.until = std::strtoull(value.c_str(), nullptr, 10);
        }
    }
    std::string host;
    uint16_t port;
    g_store.servers.erase(std::remove_if(g_store.servers.begin(), g_store.servers.end(),
                                         [&](const Saved& s) { return !community::ParseAddress(s.address, host, port); }),
                          g_store.servers.end());
    std::erase_if(g_store.bans, [](const Ban& b) { return b.address.empty() || !Active(b); });
    std::fprintf(stderr, "[servers] %zu saved, network %s (%s)\n", g_store.servers.size(),
                 g_store.current.empty() ? "STEAM" : g_store.current.c_str(), FilePath().c_str());
}

void Write()
{
    const std::string path = FilePath();
    const size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos)
        for (size_t at = path.find_first_of("/\\", 3); at != std::string::npos && at <= slash; at = path.find_first_of("/\\", at + 1))
            CreateDirectoryA(path.substr(0, at).c_str(), nullptr);
    std::ostringstream out;
    out << "; TTT2 Recomp - community servers (the game writes this file)\n[Network]\nCurrent="
        << (g_store.current.empty() ? "steam" : g_store.current) << "\n";
    for (const Saved& s : g_store.servers)
    {
        out << "\n[Server]\nAddress=" << OneLine(s.address, 260) << "\nName=" << OneLine(s.name, 64) << "\n";
        if (!s.password.empty()) out << "Password=" << Protect(s.password) << "\n";
    }
    for (const Ban& b : g_store.bans)
        if (Active(b)) out << "\n[Banned]\nAddress=" << OneLine(b.address, 260) << "\nUntil=" << b.until << "\n";
    const std::string temp = path + ".new";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        f << out.str();
        if (!f) { std::fprintf(stderr, "[servers] could not write %s\n", temp.c_str()); return; }
    }
    if (!MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        std::fprintf(stderr, "[servers] could not replace %s (%lu)\n", path.c_str(), GetLastError());
}

// ---- live answers

struct Target
{
    Live live;
    sockaddr_storage addr{};
    int addrLen = 0;
    bool resolving = false, resolved = false, bad = false;
    int64_t resolvedMs = 0, askedMs = 0, answeredMs = 0, lastUsedMs = 0;
    bool longAsk = false;                 // the next question is the long one
};
std::map<std::string, Target> g_targets;  // by Lower(address)
struct Outstanding { std::string key; int64_t sentMs; bool lan; bool full; };
std::map<uint64_t, Outstanding> g_outstanding;
struct LanEntry { Live live; int64_t seenMs = 0; sockaddr_storage addr{}; int addrLen = 0; };
std::map<std::string, LanEntry> g_lan;
bool g_watching = false;
int64_t g_nextLanMs = 0;
bool g_threadStarted = false;
SOCKET g_s4 = INVALID_SOCKET, g_s6 = INVALID_SOCKET;
constexpr int64_t kIntervalMs = 3000, kTimeoutMs = 2000, kLanKeepMs = 10000, kResolveAgainMs = 300000;

std::string KeyOf(const std::string& address) { return Lower(address); }

std::string AddrText(const sockaddr_storage& a)
{
    char host[INET6_ADDRSTRLEN] = {};
    uint16_t port = 0;
    if (a.ss_family == AF_INET)
    {
        const auto& v4 = reinterpret_cast<const sockaddr_in&>(a);
        inet_ntop(AF_INET, &v4.sin_addr, host, sizeof(host));
        port = ntohs(v4.sin_port);
        return port == kDefaultPort ? std::string(host) : std::string(host) + ":" + std::to_string(port);
    }
    const auto& v6 = reinterpret_cast<const sockaddr_in6&>(a);
    inet_ntop(AF_INET6, &v6.sin6_addr, host, sizeof(host));
    port = ntohs(v6.sin6_port);
    return "[" + std::string(host) + "]" + (port == kDefaultPort ? std::string() : ":" + std::to_string(port));
}
bool SameEndpoint(const sockaddr_storage& a, int aLen, const sockaddr_storage& b, int bLen)
{
    return aLen == bLen && aLen > 0 && std::memcmp(&a, &b, size_t(aLen)) == 0;
}

void Resolve(std::string key, std::string address)
{
    std::string host;
    uint16_t port = 0;
    sockaddr_storage addr{};
    int len = 0;
    if (community::ParseAddress(address, host, port))
    {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res)
        {
            const addrinfo* pick = res;
            for (const addrinfo* r = res; r; r = r->ai_next)
                if (r->ai_family == AF_INET) { pick = r; break; }     // IPv4 first, as the TLS connection does
            if (pick->ai_addrlen <= sizeof(addr)) { std::memcpy(&addr, pick->ai_addr, pick->ai_addrlen); len = int(pick->ai_addrlen); }
            freeaddrinfo(res);
        }
    }
    std::lock_guard l(g_mutex);
    auto it = g_targets.find(key);
    if (it == g_targets.end()) return;
    Target& t = it->second;
    t.resolving = false;
    t.resolvedMs = NowMs();
    t.resolved = len > 0;
    t.addr = addr;
    t.addrLen = len;
    if (!t.resolved)
    {
        t.longAsk = false;                                // nothing to ask: offline until a later lookup works
        if (t.live.state != State::Offline) { t.live.state = State::Offline; t.live.pingMs = -1; Changed(); }
    }
}

void SendTo(const sockaddr_storage& addr, int len, const std::vector<uint8_t>& packet)
{
    const SOCKET s = addr.ss_family == AF_INET6 ? g_s6 : g_s4;
    if (s != INVALID_SOCKET)
        sendto(s, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0, reinterpret_cast<const sockaddr*>(&addr), len);
}

// g_mutex held: questions that are due, timeouts.
void Tick(int64_t now)
{
    for (auto it = g_outstanding.begin(); it != g_outstanding.end();)
        it = now - it->second.sentMs > kTimeoutMs ? g_outstanding.erase(it) : std::next(it);
    std::vector<std::string> saved;
    for (const Saved& s : g_store.servers) saved.push_back(KeyOf(s.address));
    for (auto& [key, t] : g_targets)
    {
        const bool wanted = (g_watching && std::find(saved.begin(), saved.end(), key) != saved.end()) || t.longAsk;
        if (t.bad) continue;
        // no answer to the last question: offline (an earlier answer is kept for its name)
        if (t.askedMs && t.askedMs > t.answeredMs && now - t.askedMs > kTimeoutMs && t.live.state != State::Offline)
        {
            t.live.state = State::Offline;
            t.live.pingMs = -1;
            Changed();
        }
        if (!wanted || t.resolving) continue;
        // names are looked up again every 5 minutes, a failed lookup after the question interval
        if (!t.resolvedMs || now - t.resolvedMs > (t.resolved ? kResolveAgainMs : kIntervalMs))
        {
            t.resolving = true;
            std::string address;
            for (const Saved& s : g_store.servers)
                if (KeyOf(s.address) == key) address = s.address;
            if (address.empty()) address = key;
            std::thread(Resolve, key, address).detach();
            continue;
        }
        if (!t.resolved) continue;
        if (!t.longAsk && now - t.askedMs < kIntervalMs) continue;
        const uint64_t nonce = Random64();
        g_outstanding[nonce] = { key, now, false, t.longAsk };
        SendTo(t.addr, t.addrLen, community::InfoRequest(nonce, t.longAsk ? community::kInfoRequestSize : 128));
        t.longAsk = false;
        if (t.askedMs <= t.answeredMs && t.live.state != State::Answered) { t.live.state = State::Waiting; Changed(); }
        t.askedMs = now;
        t.lastUsedMs = now;
    }
    if (g_watching && now >= g_nextLanMs && g_s4 != INVALID_SOCKET)
    {
        g_nextLanMs = now + kIntervalMs;
        const uint64_t nonce = Random64();
        g_outstanding[nonce] = { std::string(), now, true, false };
        sockaddr_in all{};
        all.sin_family = AF_INET;
        all.sin_port = htons(kDefaultPort);
        all.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        // TTT2_LAN_BROADCAST=<IPv4>: the LAN search asks that address instead (tests: a server on 127.0.0.1)
        static const char* testLan = std::getenv("TTT2_LAN_BROADCAST");
        if (testLan) inet_pton(AF_INET, testLan, &all.sin_addr);
        const std::vector<uint8_t> packet = community::InfoRequest(nonce, 128);
        sendto(g_s4, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0, reinterpret_cast<const sockaddr*>(&all), sizeof(all));
    }
    for (auto it = g_lan.begin(); it != g_lan.end();)
        if (now - it->second.seenMs > kLanKeepMs) { it = g_lan.erase(it); Changed(); }
        else ++it;
}

void Answer(const uint8_t* buffer, int n, const sockaddr_storage& from, int fromLen, int64_t now);

// One datagram from a (non-blocking) socket; false when none is waiting.
bool ReceiveOne(SOCKET s)
{
    uint8_t buffer[2048];
    sockaddr_storage from{};
    int fromLen = sizeof(from);
    const int n = recvfrom(s, reinterpret_cast<char*>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n == SOCKET_ERROR) return WSAGetLastError() != WSAEWOULDBLOCK;   // other errors: try the next datagram
    const int64_t now = NowMs();
    if (n >= 13) Answer(buffer, n, from, fromLen, now);
    return true;
}

void Answer(const uint8_t* buffer, int n, const sockaddr_storage& from, int fromLen, int64_t now)
{
    uint64_t nonce = 0;
    std::memcpy(&nonce, buffer + 5, 8);                   // u8 kind, u32 magic, u64 nonce
    std::lock_guard l(g_mutex);
    const auto it = g_outstanding.find(nonce);
    if (it == g_outstanding.end()) return;
    community::ServerInfo info;
    if (!community::ParseInfo(buffer, size_t(n), nonce, info)) return;
    const Outstanding asked = it->second;
    if (!asked.lan) g_outstanding.erase(it);            // a broadcast gets one answer per server
    const int ping = int(std::clamp<int64_t>(now - asked.sentMs, 0, 9999));
    if (asked.lan)
    {
        for (const auto& [key, t] : g_targets)
            if (t.resolved && SameEndpoint(t.addr, t.addrLen, from, fromLen)) return;   // a saved server
        LanEntry& e = g_lan[AddrText(from)];
        e.live.state = State::Answered;
        e.live.info = std::move(info);
        e.live.pingMs = ping;
        e.live.full = false;
        ++e.live.answers;
        e.seenMs = now;
        e.addr = from;
        e.addrLen = fromLen;
        Changed();
        return;
    }
    const auto t = g_targets.find(asked.key);
    if (t == g_targets.end()) return;
    t->second.live.state = State::Answered;
    t->second.live.info = std::move(info);
    t->second.live.pingMs = ping;
    t->second.live.full = asked.full;
    ++t->second.live.answers;
    t->second.answeredMs = now;
    Changed();
    // the name as last reported, kept with a saved server
    for (Saved& s : g_store.servers)
        if (KeyOf(s.address) == asked.key && !t->second.live.info.name.empty() && s.name != t->second.live.info.name)
        {
            s.name = t->second.live.info.name;
            Write();
        }
}

void Loop()
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    // Non-blocking (every waiting answer is read at once), and an ICMP "port unreachable" from a server that is
    // down must not turn into an error on the socket (Windows' UDP connection reset: each one would hide an answer
    // behind it).
    auto prepare = [](SOCKET s) {
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        BOOL off = FALSE;
        DWORD ignored = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ignored, nullptr, nullptr);
    };
    g_s4 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_s4 != INVALID_SOCKET)
    {
        prepare(g_s4);
        BOOL on = TRUE;
        setsockopt(g_s4, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
        sockaddr_in any{};
        any.sin_family = AF_INET;
        bind(g_s4, reinterpret_cast<const sockaddr*>(&any), sizeof(any));
    }
    g_s6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (g_s6 != INVALID_SOCKET)
    {
        prepare(g_s6);
        sockaddr_in6 any{};
        any.sin6_family = AF_INET6;
        bind(g_s6, reinterpret_cast<const sockaddr*>(&any), sizeof(any));
    }
    for (;;)
    {
        {
            std::lock_guard l(g_mutex);
            Tick(NowMs());
        }
        fd_set read;
        FD_ZERO(&read);
        if (g_s4 != INVALID_SOCKET) FD_SET(g_s4, &read);
        if (g_s6 != INVALID_SOCKET) FD_SET(g_s6, &read);
        timeval wait{ 0, 30000 };
        if (select(0, &read, nullptr, nullptr, &wait) > 0)
            for (const SOCKET s : { g_s4, g_s6 })
                if (s != INVALID_SOCKET && FD_ISSET(s, &read))
                    for (int i = 0; i < 256 && ReceiveOne(s); ++i) {}
    }
}

// g_mutex held
Target& TargetOf(const std::string& address)
{
    if (!g_threadStarted) { g_threadStarted = true; std::thread(Loop).detach(); }
    Target& t = g_targets[KeyOf(address)];
    std::string host;
    uint16_t port;
    if (!community::ParseAddress(address, host, port) && !t.bad)
    {
        t.bad = true;
        t.live.state = State::BadAddress;
        Changed();
    }
    return t;
}

} // namespace

std::vector<Saved> List()
{
    std::lock_guard l(g_mutex);
    Load();
    return g_store.servers;
}

bool Save(const Saved& server)
{
    std::string host;
    uint16_t port;
    if (!community::ParseAddress(server.address, host, port)) return false;
    std::lock_guard l(g_mutex);
    Load();
    for (Saved& s : g_store.servers)
        if (SameAddress(s.address, server.address))
        {
            if (!server.name.empty()) s.name = OneLine(server.name, 64);
            s.password = server.password;
            Write();
            Changed();
            return true;
        }
    if (g_store.servers.size() >= kMaxSaved) return false;
    g_store.servers.push_back({ OneLine(server.address, 260), OneLine(server.name, 64), server.password });
    Write();
    Changed();
    std::fprintf(stderr, "[servers] saved %s\n", server.address.c_str());
    return true;
}

void Remove(const std::string& address)
{
    std::lock_guard l(g_mutex);
    Load();
    const size_t before = g_store.servers.size();
    g_store.servers.erase(std::remove_if(g_store.servers.begin(), g_store.servers.end(),
                                         [&](const Saved& s) { return SameAddress(s.address, address); }),
                          g_store.servers.end());
    if (SameAddress(g_store.current, address)) g_store.current.clear();
    if (g_store.servers.size() != before)
    {
        g_targets.erase(KeyOf(address));
        Write();
        Changed();
        std::fprintf(stderr, "[servers] removed %s\n", address.c_str());
    }
}

std::string Current()
{
    std::lock_guard l(g_mutex);
    Load();
    return g_store.current;
}

void SetCurrent(const std::string& address)
{
    std::lock_guard l(g_mutex);
    Load();
    if (g_store.current == address) return;
    g_store.current = address;
    Write();
    Changed();
}

bool SameAddress(const std::string& a, const std::string& b)
{
    std::string ha, hb;
    uint16_t pa = 0, pb = 0;
    if (!community::ParseAddress(a, ha, pa) || !community::ParseAddress(b, hb, pb)) return Lower(a) == Lower(b);
    return Lower(ha) == Lower(hb) && pa == pb;
}

void Watch(bool on)
{
    std::lock_guard l(g_mutex);
    Load();
    if (g_watching == on) return;
    g_watching = on;
    if (on)
    {
        for (const Saved& s : g_store.servers) TargetOf(s.address);
        g_nextLanMs = 0;
    }
}

void SetBanned(const std::string& address, uint64_t until)
{
    std::lock_guard l(g_mutex);
    Load();
    std::erase_if(g_store.bans, [&](const Ban& b) { return SameAddress(b.address, address) || !Active(b); });
    const Ban ban{ address, until };
    if (Active(ban) && g_store.bans.size() < kMaxSaved) g_store.bans.push_back(ban);
    Write();
    Changed();
}

bool Banned(const std::string& address, uint64_t* until)
{
    std::lock_guard l(g_mutex);
    Load();
    for (const Ban& b : g_store.bans)
        if (Active(b) && SameAddress(b.address, address))
        {
            if (until) *until = b.until;
            return true;
        }
    return false;
}

std::vector<std::pair<std::string, uint64_t>> Bans()
{
    std::lock_guard l(g_mutex);
    Load();
    std::vector<std::pair<std::string, uint64_t>> out;
    for (const Ban& b : g_store.bans)
        if (Active(b)) out.emplace_back(b.address, b.until);
    return out;
}

void Ask(const std::string& address)
{
    std::lock_guard l(g_mutex);
    Load();
    Target& t = TargetOf(address);
    t.longAsk = true;
}

Live Get(const std::string& address)
{
    std::lock_guard l(g_mutex);
    const auto it = g_targets.find(KeyOf(address));
    if (it != g_targets.end()) return it->second.live;
    for (const auto& [key, e] : g_lan)                    // a LAN server by the address the list shows
        if (key == address) return e.live;
    return {};
}

std::vector<Found> Lan()
{
    std::lock_guard l(g_mutex);
    std::vector<Found> out;
    for (const auto& [address, e] : g_lan)
    {
        bool saved = false;
        for (const Saved& s : g_store.servers) saved |= SameAddress(s.address, address);
        if (!saved) out.push_back({ address, e.live });
    }
    return out;
}

uint32_t Version() { return g_version.load(); }

} // namespace server_list
