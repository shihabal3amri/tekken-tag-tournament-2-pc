// The community-server client (community_net.h).
//
// Threads: `reader` reads control frames (TLS), `writer` sends queued control frames, `timer` pings the server every
// 5 s. Matches do not touch the server (they run over Steam P2P); UDP is used only for the server list's INFO.
#include "community_net.h"
#include "community_tls.h"
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <bcrypt.h>

namespace community {
namespace {

// server/PROTOCOL.md
enum : uint8_t { kHello = 1, kAuth = 2, kPing = 3, kSessionPut = 4, kSessionDel = 5, kSessionList = 6, kSessionGet = 7, kInviteSend = 8,
                 kGinRequest = 9, kStatsWrite = 10, kStatsBoard = 11, kStatsPlayers = 12, kArbRegister = 13, kPeerCheck = 14, kXuidNames = 15,
                 kMatchEvent = 16, kFilePut = 17, kFileGet = 18,
                 kWelcome = 101, kError = 102, kPong = 103, kListResult = 104, kGetResult = 105, kInvite = 106,
                 kNotice = 109, kGinResponse = 110, kStatsResult = 111, kPeerResult = 112, kNamesResult = 113,
                 kFileResult = 114 };
enum : uint8_t { kInfoReq = 1, kInfo = 2 };
constexpr uint32_t kInfoMagic = 0x49535454;     // "TTSI" little-endian
constexpr size_t kInfoReqMinSize = 128;
constexpr uint16_t kProtocolVersion = 2;       // 2: no match relay
// a frame from the server: at most the protocol's 1 MB (a GIN reply with ghosts or replays is up to ~400 KB); to the
// server: at most what it reads (256 KB)
constexpr uint32_t kMaxFrame = 1u << 20;
constexpr uint32_t kMaxSentFrame = 256u << 10;
constexpr size_t kMaxRecord = 16u << 10;
constexpr size_t kMaxListResults = 64;

int64_t NowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Writer
{
    std::vector<uint8_t> b;
    void U8(uint8_t v) { b.push_back(v); }
    void U16(uint16_t v) { for (int i = 0; i < 2; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void U32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void U64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void Raw(const void* p, size_t n) { b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
    void Str(const std::string& s) { const size_t n = (std::min)(s.size(), size_t(0xFFFF)); U16(uint16_t(n)); Raw(s.data(), n); }
    void Blob(const std::vector<uint8_t>& v) { U32(uint32_t(v.size())); Raw(v.data(), v.size()); }
};

// Bounded reader: after a short read everything is zero / empty and ok() is false.
struct Reader
{
    const uint8_t* p;
    size_t n;
    bool good = true;
    Reader(const uint8_t* data, size_t size) : p(data), n(size) {}
    bool Take(size_t k) { if (!good || n < k) { good = false; return false; } return true; }
    uint64_t Int(size_t k)
    {
        if (!Take(k)) return 0;
        uint64_t v = 0;
        for (size_t i = 0; i < k; ++i) v |= uint64_t(p[i]) << (8 * i);
        p += k;
        n -= k;
        return v;
    }
    uint8_t U8() { return uint8_t(Int(1)); }
    uint16_t U16() { return uint16_t(Int(2)); }
    uint32_t U32() { return uint32_t(Int(4)); }
    uint64_t U64() { return Int(8); }
    std::string Str(size_t maxLen)
    {
        const size_t len = U16();
        if (len > maxLen || !Take(len)) { good = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p), len);
        p += len;
        n -= len;
        return s;
    }
    std::vector<uint8_t> Blob(size_t maxLen)
    {
        const size_t len = U32();
        if (len > maxLen || !Take(len)) { good = false; return {}; }
        std::vector<uint8_t> v(p, p + len);
        p += len;
        n -= len;
        return v;
    }
    bool ok() const { return good; }
};

// Text from a server shown in logs / the game: printable ASCII only, cut.
std::string Clean(const std::string& s, size_t maxLen)
{
    std::string out;
    for (unsigned char c : s)
    {
        if (out.size() >= maxLen) break;
        if (c >= 0x20 && c < 0x7F) out.push_back(char(c));
        else if (c == '\n' && maxLen > 64) out.push_back('\n');
    }
    return out;
}

// base64url without padding (RFC 4648 section 5)
std::string Base64Url(const uint8_t* p, size_t n)
{
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string s;
    for (size_t i = 0; i < n; i += 3)
    {
        const uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        s.push_back(a[v >> 18 & 63]);
        s.push_back(a[v >> 12 & 63]);
        if (i + 1 < n) s.push_back(a[v >> 6 & 63]);
        if (i + 2 < n) s.push_back(a[v & 63]);
    }
    return s;
}

std::string Hex(const uint8_t* p, size_t n)
{
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s.push_back(digits[p[i] >> 4]); s.push_back(digits[p[i] & 15]); }
    return s;
}

// Known servers: "host:port <64 hex fingerprint>" per line (trust on first use).
std::mutex g_pinMutex;
enum class Pin { New, Match, Mismatch };
Pin CheckPin(const std::string& file, const std::string& server, const std::string& fp)
{
    std::lock_guard l(g_pinMutex);
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream fields(line);
        std::string who, pinned;
        if (fields >> who >> pinned && who == server) return pinned == fp ? Pin::Match : Pin::Mismatch;
    }
    return Pin::New;
}
void SavePin(const std::string& file, const std::string& server, const std::string& fp)
{
    std::lock_guard l(g_pinMutex);
    const size_t slash = file.find_last_of("/\\");
    if (slash != std::string::npos)
    {
        // create the folders on the way
        for (size_t at = file.find_first_of("/\\", 3); at != std::string::npos && at <= slash; at = file.find_first_of("/\\", at + 1))
            CreateDirectoryA(file.substr(0, at).c_str(), nullptr);
    }
    std::ofstream(file, std::ios::app) << server << " " << fp << "\n";
}
void ForgetPin(const std::string& file, const std::string& server)
{
    std::lock_guard l(g_pinMutex);
    std::string kept, line;
    bool found = false;
    {
        std::ifstream in(file);
        while (std::getline(in, line))
        {
            std::istringstream fields(line);
            std::string who;
            if (fields >> who && who == server) { found = true; continue; }
            kept += line + "\n";
        }
    }
    if (found) std::ofstream(file, std::ios::trunc) << kept;
}

} // namespace

struct Client::Impl
{
    Options opts;
    TlsStream tls;
    uint64_t peer = 0;
    std::atomic<bool> connected{ false }, stopping{ false };
    std::atomic<int> refusal{ RefusedNone };
    bool used = false;                          // Connect runs once per client

    std::thread reader, writer, timer;
    std::mutex outMutex;
    std::condition_variable outCv;
    std::deque<std::vector<uint8_t>> outQueue;

    std::mutex reqMutex;
    std::condition_variable reqCv;
    uint32_t nextRequest = 1;
    std::map<uint32_t, std::vector<std::vector<uint8_t>>> lists;
    std::map<uint32_t, std::pair<bool, std::vector<uint8_t>>> gets;
    std::map<uint32_t, std::vector<uint8_t>> gins;  // GIN responses by request
    std::map<uint32_t, std::pair<bool, StatPage>> statPages;   // statistics answers by request
    std::map<uint32_t, uint8_t> peerAnswers;   // PEER_RESULT by request
    std::map<uint32_t, std::map<uint64_t, std::string>> nameAnswers;   // XUID_NAMES_RESULT by request
    std::map<uint32_t, std::pair<uint8_t, std::vector<uint8_t>>> fileAnswers;   // FILE_RESULT by request
    bool gin = false;                          // the server answers GIN (WELCOME flag bit 1)
    bool stats = false;                        // ... keeps statistics (bit 2)
    bool matches = false;                      // ... records matches (bit 3)
    bool files = false;                        // ... keeps the players' license files (bit 4)
    std::atomic<uint64_t> banUntil{ ~0ull };   // ERROR 6's until (~0: not said)
    std::set<uint32_t> outstanding;            // requests still waited for (answers to others are dropped)
    std::deque<std::pair<uint64_t, std::vector<uint8_t>>> invites;
    std::string closeReason;

    std::atomic<int> rttMs{ -1 };
    std::atomic<int64_t> pingSentUs{ 0 };

    void Queue(uint8_t type, const std::vector<uint8_t>& payload)
    {
        std::vector<uint8_t> frame(5 + payload.size());
        const uint32_t size = uint32_t(1 + payload.size());
        std::memcpy(frame.data(), &size, 4);
        frame[4] = type;
        if (!payload.empty()) std::memcpy(frame.data() + 5, payload.data(), payload.size());
        {
            std::lock_guard l(outMutex);
            if (outQueue.size() > 256) return;          // a stuck connection: the reader will notice
            outQueue.push_back(std::move(frame));
        }
        outCv.notify_one();
    }

    // ---- control
    void Close(const std::string& why)
    {
        bool was = connected.exchange(false);
        {
            std::lock_guard l(reqMutex);
            if (closeReason.empty()) closeReason = why;
        }
        reqCv.notify_all();
        if (was) std::fprintf(stderr, "[community] disconnected: %s\n", why.c_str());
    }

    void Frame(uint8_t type, const uint8_t* p, size_t n)
    {
        Reader r(p, n);
        switch (type)
        {
        case kPong:
            if (r.U64() == 1 && pingSentUs.load()) rttMs = int((NowUs() - pingSentUs.load()) / 1000);
            break;
        case kListResult:
        {
            const uint32_t request = r.U32(), count = r.U32();
            std::vector<std::vector<uint8_t>> records;
            for (uint32_t i = 0; r.ok() && i < count && i < kMaxListResults; ++i)
            {
                std::vector<uint8_t> rec = r.Blob(kMaxRecord);
                if (r.ok()) records.push_back(std::move(rec));
            }
            if (!r.ok() || count > kMaxListResults) { Close("malformed session list from the server"); return; }
            std::lock_guard l(reqMutex);
            if (!outstanding.count(request)) break;
            lists[request] = std::move(records);
            reqCv.notify_all();
            break;
        }
        case kGetResult:
        {
            const uint32_t request = r.U32();
            const bool found = r.U8() != 0;
            std::vector<uint8_t> rec = r.Blob(kMaxRecord);
            if (!r.ok()) { Close("malformed session from the server"); return; }
            std::lock_guard l(reqMutex);
            if (!outstanding.count(request)) break;
            gets[request] = { found, std::move(rec) };
            reqCv.notify_all();
            break;
        }
        case kGinResponse:
        {
            const uint32_t request = r.U32();
            std::vector<uint8_t> response = r.Blob(kMaxFrame);
            if (!r.ok()) { Close("malformed GIN response from the server"); return; }
            std::lock_guard l(reqMutex);
            if (!outstanding.count(request)) break;
            gins[request] = std::move(response);
            reqCv.notify_all();
            break;
        }
        case kNamesResult:
        {
            const uint32_t request = r.U32();
            std::map<uint64_t, std::string> names;
            for (unsigned n = r.U8(); n > 0 && r.ok(); --n)
            {
                const uint64_t xuid = r.U64();
                names[xuid] = r.Str(15);
            }
            if (!r.ok()) { Close("malformed answer from the server"); return; }
            std::lock_guard l(reqMutex);
            if (!outstanding.count(request)) break;
            nameAnswers[request] = std::move(names);
            reqCv.notify_all();
            break;
        }
        case kFileResult:
        {
            const uint32_t request = r.U32();
            const uint8_t found = r.U8();
            std::vector<uint8_t> data = r.Blob(kMaxPlayerFile);
            if (!r.ok()) { Close("malformed answer from the server"); return; }
            std::lock_guard l(reqMutex);
            if (!outstanding.count(request)) break;
            fileAnswers[request] = { found, std::move(data) };
            reqCv.notify_all();
            break;
        }
        case kPeerResult:
        {
            const uint32_t request = r.U32();
            const uint8_t answer = r.U8();
            if (!r.ok()) { Close("malformed answer from the server"); return; }
            std::lock_guard l(reqMutex);
            if (!outstanding.count(request)) break;
            peerAnswers[request] = answer;
            reqCv.notify_all();
            break;
        }
        case kStatsResult:
        {
            const uint32_t request = r.U32();
            const bool ok = r.U8() != 0;
            StatPage page;
            page.total = r.U32();
            const uint16_t rows = r.U16();
            if (rows > kMaxStatRows) { Close("malformed statistics from the server"); return; }
            for (uint16_t i = 0; i < rows && r.ok(); ++i)
            {
                StatRow row;
                row.xuid = r.U64();
                row.rank = r.U32();
                row.rating = int64_t(r.U64());
                row.name = Clean(r.Str(64), 15);
                const uint8_t columns = r.U8();
                if (columns > kMaxStatColumns) { Close("malformed statistics from the server"); return; }
                for (uint8_t c = 0; c < columns && r.ok(); ++c)
                {
                    StatValue v;
                    v.column = r.U16();
                    v.type = r.U8();
                    v.bits = r.U64();
                    row.columns.push_back(v);
                }
                page.rows.push_back(std::move(row));
            }
            if (!r.ok()) { Close("malformed statistics from the server"); return; }
            std::lock_guard l(reqMutex);
            if (!outstanding.count(request)) break;
            statPages[request] = { ok, std::move(page) };
            reqCv.notify_all();
            break;
        }
        case kInvite:
        {
            // Not accepted on the player's behalf: an invite needs the game's own Accept / Decline window
            // (COMMUNITY_SERVER_PLAN.md phase 2); until then it is only logged.
            const uint64_t from = r.U64();
            if (r.ok()) std::fprintf(stderr, "[community] invite from %016llX ignored (no in-game prompt yet)\n", (unsigned long long)from);
            break;
        }
        case kNotice:
            std::fprintf(stderr, "[community] server notice: %s\n", Clean(r.Str(400), 200).c_str());
            break;
        case kError:
        {
            const uint32_t code = r.U32();
            const std::string text = Clean(r.Str(400), 200);
            refusal = int((std::min)(code, 0xFFFFu));      // e.g. 6: kicked or banned while playing
            if (code == RefusedBanned && r.ok() && r.n >= 8) banUntil = r.U64();
            Close("server error " + std::to_string(code) + ": " + text);
            break;
        }
        default:
            break;                                       // newer servers may send more: ignored
        }
    }

    // Reads frames until the connection ends. `first` (login) returns after one frame.
    bool ReadFrame(std::vector<uint8_t>& pending, uint8_t& type, std::vector<uint8_t>& payload)
    {
        uint8_t chunk[16384];
        for (;;)
        {
            if (pending.size() >= 5)
            {
                uint32_t size;
                std::memcpy(&size, pending.data(), 4);
                if (size == 0 || size > kMaxFrame) return false;
                if (pending.size() >= 4 + size_t(size))
                {
                    type = pending[4];
                    payload.assign(pending.begin() + 5, pending.begin() + 4 + size);
                    pending.erase(pending.begin(), pending.begin() + 4 + size);
                    return true;
                }
            }
            const int n = tls.Recv(chunk, sizeof(chunk));
            if (n <= 0) return false;
            pending.insert(pending.end(), chunk, chunk + n);
        }
    }

    std::vector<uint8_t> pendingIn;

    void ReaderLoop()
    {
        uint8_t type;
        std::vector<uint8_t> payload;
        while (!stopping && ReadFrame(pendingIn, type, payload)) Frame(type, payload.data(), payload.size());
        Close(stopping ? "left the server" : "the server closed the connection");
    }

    void WriterLoop()
    {
        for (;;)
        {
            std::vector<uint8_t> frame;
            {
                std::unique_lock l(outMutex);
                outCv.wait(l, [&] { return stopping || !outQueue.empty(); });
                if (stopping) return;
                frame = std::move(outQueue.front());
                outQueue.pop_front();
            }
            if (!tls.Send(frame.data(), frame.size())) { Close("could not send to the server"); return; }
        }
    }

    void TimerLoop()
    {
        int64_t nextKeepAlive = 0;
        while (!stopping)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const int64_t now = NowUs();
            if (now >= nextKeepAlive && connected)
            {
                nextKeepAlive = now + 5'000'000;     // the server drops a connection quiet for 30 s
                Writer w;
                w.U64(1);
                pingSentUs = now;
                Queue(kPing, w.b);
            }
        }
    }
};

bool ParseAddress(const std::string& text, std::string& host, uint16_t& port)
{
    std::string t = text;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    port = 28200;
    if (t.empty() || t.size() > 253) return false;
    std::string portText;
    if (t.front() == '[')                                // [ipv6]:port
    {
        const size_t close = t.find(']');
        if (close == std::string::npos) return false;
        host = t.substr(1, close - 1);
        if (close + 1 < t.size())
        {
            if (t[close + 1] != ':') return false;
            portText = t.substr(close + 2);
        }
    }
    else if (std::count(t.begin(), t.end(), ':') == 1)
    {
        host = t.substr(0, t.find(':'));
        portText = t.substr(t.find(':') + 1);
    }
    else host = t;
    if (host.empty()) return false;
    for (char c : host)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':')) return false;
    if (!portText.empty())
    {
        if (portText.size() > 5 || portText.find_first_not_of("0123456789") != std::string::npos) return false;
        const int v = std::stoi(portText);
        if (v < 1 || v > 65535) return false;
        port = uint16_t(v);
    }
    return true;
}

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() { Disconnect(); }

bool Client::Connect(const Options& opts, Welcome& welcome, std::string& error)
{
    Impl& m = *impl_;
    if (m.used) { error = "a client connects once"; return false; }
    m.used = true;
    m.opts = opts;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const std::string server = opts.host + ":" + std::to_string(opts.port);
    const int64_t deadline = NowUs() + int64_t(opts.timeoutMs) * 1000;   // one deadline for everything below

    // TLS, with the server's key checked against the known-servers file. A connection the server turns away
    // at once (its limit of handshakes per address - several players behind one router) is tried again.
    std::string fingerprint;
    uint8_t fingerprintBytes[32] = {};
    Pin pin = Pin::New;
    for (int attempt = 0;; ++attempt)
    {
        m.tls.SetDeadline(deadline);
        if (m.tls.Connect(opts.host, opts.port, opts.timeoutMs,
                          [&](const uint8_t (&fp)[32]) {
                              fingerprint = Hex(fp, 32);
                              std::memcpy(fingerprintBytes, fp, 32);
                              pin = opts.knownServersFile.empty() ? Pin::New : CheckPin(opts.knownServersFile, server, fingerprint);
                              return pin != Pin::Mismatch;
                          },
                          error))
            break;
        if (pin == Pin::Mismatch)
        {
            error = "This server's identity has changed since you last played on it. It may be a different server - "
                    "if its host replaced its key, remove it and add it again.";
            return false;
        }
        uint32_t jitter = 0;
        BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&jitter), sizeof(jitter), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        const int64_t wait = 250'000 + int64_t(jitter % 500'000);
        if (attempt >= 3 || error.find("during the TLS handshake") == std::string::npos || NowUs() + wait >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(wait));
    }
    for (size_t i = 0; i < 32; i += 4)
        welcome.fingerprint += (i ? "-" : "") + fingerprint.substr(i, 4);
    welcome.tls = m.tls.ProtocolName();
    // The Steam ticket's identity (server/PROTOCOL.md): "ttt2-" + 152 bits of the key, 31 characters - the most
    // Steam binds a ticket to, and no "name:" prefix (Steam never matches those).
    const std::string identity = "ttt2-" + Base64Url(fingerprintBytes, 19);

    Writer hello;
    hello.U16(kProtocolVersion);
    hello.Str(opts.gameProtocol);
    hello.Str(opts.clientVersion);
    Writer auth;
    auth.U64(opts.steamId);
    auth.Str(opts.profileName);
    const std::string ticket = opts.ticket ? opts.ticket(identity) : std::string();
    auth.Blob(std::vector<uint8_t>(ticket.begin(), ticket.end()));
    auth.Str(opts.password);
    auto frame = [](uint8_t type, const std::vector<uint8_t>& p) {
        std::vector<uint8_t> f(5 + p.size());
        const uint32_t size = uint32_t(1 + p.size());
        std::memcpy(f.data(), &size, 4);
        f[4] = type;
        if (!p.empty()) std::memcpy(f.data() + 5, p.data(), p.size());
        return f;
    };
    std::vector<uint8_t> out = frame(kHello, hello.b), a = frame(kAuth, auth.b);
    out.insert(out.end(), a.begin(), a.end());
    if (!m.tls.Send(out.data(), out.size())) { error = "the server closed the connection"; m.tls.Close(); return false; }

    uint8_t type = 0;
    std::vector<uint8_t> payload;
    if (!m.ReadFrame(m.pendingIn, type, payload)) { error = "the server did not answer"; m.tls.Close(); return false; }
    m.tls.SetDeadline(0);                        // the reader thread waits in Recv from now on; Close unblocks it
    Reader r(payload.data(), payload.size());
    if (type == kError)
    {
        const uint32_t code = r.U32();
        m.refusal = int((std::min)(code, 0xFFFFu));
        error = Clean(r.Str(400), 200);
        if (code == RefusedBanned && r.ok() && r.n >= 8) m.banUntil = r.U64();   // until when (newer servers)
        if (error.empty()) error = "the server refused the connection (" + std::to_string(code) + ")";
        m.tls.Close();
        return false;
    }
    welcome.peer = r.U64();
    const uint8_t flags = r.U8();
    welcome.verified = (flags & 1) != 0;
    welcome.gin = (flags & 2) != 0;
    welcome.stats = (flags & 4) != 0;
    welcome.matches = (flags & 8) != 0;
    welcome.files = (flags & 16) != 0;
    m.gin = welcome.gin;
    m.stats = welcome.stats;
    m.matches = welcome.matches;
    m.files = welcome.files;
    welcome.name = Clean(r.Str(256), 64);
    welcome.region = Clean(r.Str(64), 32);
    welcome.motd = Clean(r.Str(4096), 1000);
    if (type != kWelcome || !r.ok() || welcome.peer != opts.steamId)
    {
        error = "the server's answer was not understood";
        m.tls.Close();
        return false;
    }
    if (pin == Pin::New && !opts.knownServersFile.empty()) SavePin(opts.knownServersFile, server, fingerprint);
    m.peer = welcome.peer;
    m.connected = true;
    m.reader = std::thread([&m] { m.ReaderLoop(); });
    m.writer = std::thread([&m] { m.WriterLoop(); });
    m.timer = std::thread([&m] { m.TimerLoop(); });
    std::fprintf(stderr, "[community] connected to %s \"%s\" (%s, %s, key %s, %s)\n", server.c_str(), welcome.name.c_str(),
                 welcome.region.c_str(), welcome.tls.c_str(), welcome.fingerprint.c_str(),
                 welcome.verified ? "Steam ID checked" : "Steam ID NOT checked");
    return true;
}

void Client::Disconnect()
{
    if (!impl_) return;
    Impl& m = *impl_;
    {
        std::lock_guard l(m.outMutex);              // the writer cannot miss this wake-up
        m.stopping = true;
    }
    m.Close("left the server");
    // Unblock the threads, wait for them, and only then release the socket: a released handle can be reused at
    // once by another socket, which a late receive would then read.
    m.tls.Shutdown();
    m.outCv.notify_all();
    for (std::thread* t : { &m.reader, &m.writer, &m.timer })
        if (t->joinable()) t->join();
    m.tls.Close();
}

bool Client::Connected() const { return impl_ && impl_->connected; }
uint64_t Client::LocalPeer() const { return impl_ ? impl_->peer : 0; }
int Client::Refusal() const { return impl_->refusal; }

bool Client::BanUntil(uint64_t& until) const
{
    const uint64_t u = impl_->banUntil.load();
    if (u == ~0ull) return false;
    until = u;
    return true;
}
std::string Client::CloseReason() const
{
    if (Connected()) return {};
    std::lock_guard l(impl_->reqMutex);
    return impl_->closeReason;
}

void Client::PublishSession(const std::vector<uint8_t>& record)
{
    if (!Connected() || record.size() > kMaxRecord) return;
    Writer w;
    w.Blob(record);
    impl_->Queue(kSessionPut, w.b);
}

void Client::RemoveSession(uint64_t id)
{
    if (!Connected()) return;
    Writer w;
    w.U64(id);
    impl_->Queue(kSessionDel, w.b);
}

std::vector<std::vector<uint8_t>> Client::ListSessions(uint32_t max, int timeoutMs,
                                                       const std::vector<std::pair<uint32_t, uint32_t>>& contexts, bool openOnly)
{
    if (!Connected()) return {};
    Impl& m = *impl_;
    uint32_t request;
    {
        std::lock_guard l(m.reqMutex);
        request = m.nextRequest++;
        m.outstanding.insert(request);
    }
    Writer w;
    w.U32(request);
    w.U32((std::min)(max, uint32_t(kMaxListResults)));
    if (!contexts.empty() || openOnly)
    {
        const size_t n = (std::min)(contexts.size(), size_t(16));
        w.U8(uint8_t(n));
        for (size_t i = 0; i < n; ++i) { w.U32(contexts[i].first); w.U32(contexts[i].second); }
        w.U8(openOnly ? 1 : 0);
    }
    m.Queue(kSessionList, w.b);
    std::unique_lock l(m.reqMutex);
    m.reqCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return m.lists.count(request) || !m.connected; });
    m.outstanding.erase(request);
    auto it = m.lists.find(request);
    if (it == m.lists.end()) return {};
    std::vector<std::vector<uint8_t>> out = std::move(it->second);
    m.lists.erase(it);
    return out;
}

bool Client::GetSession(uint64_t id, std::vector<uint8_t>& record, int timeoutMs)
{
    if (!Connected()) return false;
    Impl& m = *impl_;
    uint32_t request;
    {
        std::lock_guard l(m.reqMutex);
        request = m.nextRequest++;
        m.outstanding.insert(request);
    }
    Writer w;
    w.U32(request);
    w.U64(id);
    m.Queue(kSessionGet, w.b);
    std::unique_lock l(m.reqMutex);
    m.reqCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return m.gets.count(request) || !m.connected; });
    m.outstanding.erase(request);
    auto it = m.gets.find(request);
    if (it == m.gets.end()) return false;
    const bool found = it->second.first;
    record = std::move(it->second.second);
    m.gets.erase(it);
    return found;
}

bool Client::GinRequest(const std::vector<uint8_t>& request, std::vector<uint8_t>& response, int timeoutMs)
{
    if (!Connected() || !impl_->gin || request.size() > (kMaxSentFrame - 64)) return false;
    Impl& m = *impl_;
    uint32_t id;
    {
        std::lock_guard l(m.reqMutex);
        id = m.nextRequest++;
        m.outstanding.insert(id);
    }
    Writer w;
    w.U32(id);
    w.Blob(request);
    m.Queue(kGinRequest, w.b);
    std::unique_lock l(m.reqMutex);
    m.reqCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return m.gins.count(id) || !m.connected; });
    m.outstanding.erase(id);
    const auto it = m.gins.find(id);
    if (it == m.gins.end()) return false;
    response = std::move(it->second);
    m.gins.erase(it);
    return true;
}

void Client::WriteStats(uint64_t sessionId, uint64_t ownXuid, uint64_t subjectXuid, const std::vector<StatViewWrite>& views)
{
    if (!Connected() || !impl_->stats || views.empty() || views.size() > kMaxStatViews) return;
    Writer w;
    w.U64(sessionId);
    w.U64(ownXuid);
    w.U64(subjectXuid);
    w.U8(uint8_t(views.size()));
    for (const StatViewWrite& v : views)
    {
        const size_t n = (std::min)(v.props.size(), kMaxStatProps);
        w.U32(v.view);
        w.U8(uint8_t(n));
        for (size_t i = 0; i < n; ++i)
        {
            w.U32(v.props[i].id);
            w.U8(v.props[i].type);
            w.U64(v.props[i].bits);
        }
    }
    impl_->Queue(kStatsWrite, w.b);
}

int Client::PeerOnServer(uint64_t peer, int timeoutMs)
{
    if (!Connected()) return -1;
    Impl& m = *impl_;
    uint32_t id;
    {
        std::lock_guard l(m.reqMutex);
        id = m.nextRequest++;
        m.outstanding.insert(id);
    }
    Writer w;
    w.U32(id);
    w.U64(peer);
    m.Queue(kPeerCheck, w.b);
    std::unique_lock l(m.reqMutex);
    m.reqCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return m.peerAnswers.count(id) || !m.connected; });
    m.outstanding.erase(id);
    const auto it = m.peerAnswers.find(id);
    if (it == m.peerAnswers.end()) return -1;
    const int answer = it->second == 1 ? 1 : it->second == 0 ? 0 : -1;
    m.peerAnswers.erase(it);
    return answer;
}

bool Client::NamesOf(const std::vector<uint64_t>& xuids, std::map<uint64_t, std::string>& names, int timeoutMs)
{
    if (!Connected() || !impl_->stats || xuids.empty() || xuids.size() > 16) return false;
    Impl& m = *impl_;
    uint32_t id;
    {
        std::lock_guard l(m.reqMutex);
        id = m.nextRequest++;
        m.outstanding.insert(id);
    }
    Writer w;
    w.U32(id);
    w.U8(uint8_t(xuids.size()));
    for (const uint64_t x : xuids) w.U64(x);
    m.Queue(kXuidNames, w.b);
    std::unique_lock l(m.reqMutex);
    m.reqCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return m.nameAnswers.count(id) || !m.connected; });
    m.outstanding.erase(id);
    const auto it = m.nameAnswers.find(id);
    if (it == m.nameAnswers.end()) return false;
    for (auto& [xuid, name] : it->second)
        if (std::find(xuids.begin(), xuids.end(), xuid) != xuids.end()) names[xuid] = name;
    m.nameAnswers.erase(it);
    return true;
}

bool Client::KeepsFiles() const { return Connected() && impl_->files; }

void Client::PutFile(uint64_t ownXuid, const std::string& item, const std::vector<uint8_t>& data)
{
    if (!KeepsFiles() || !ownXuid || item.empty() || item.size() > 32 || data.empty() || data.size() > kMaxPlayerFile) return;
    Writer w;
    w.U64(ownXuid);
    w.Str(item);
    w.Blob(data);
    impl_->Queue(kFilePut, w.b);
}

int Client::GetFile(uint64_t ownerXuid, const std::string& item, std::vector<uint8_t>& data, int timeoutMs)
{
    if (!KeepsFiles() || !ownerXuid || item.empty() || item.size() > 32) return -1;
    Impl& m = *impl_;
    uint32_t id;
    {
        std::lock_guard l(m.reqMutex);
        id = m.nextRequest++;
        m.outstanding.insert(id);
    }
    Writer w;
    w.U32(id);
    w.U64(ownerXuid);
    w.Str(item);
    m.Queue(kFileGet, w.b);
    std::unique_lock l(m.reqMutex);
    m.reqCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return m.fileAnswers.count(id) || !m.connected; });
    m.outstanding.erase(id);
    const auto it = m.fileAnswers.find(id);
    if (it == m.fileAnswers.end()) return -1;
    const uint8_t found = it->second.first;
    if (found == 1) data = std::move(it->second.second);
    m.fileAnswers.erase(it);
    return found == 1 && !data.empty() ? 1 : found == 0 ? 0 : -1;   // 2: asked too often
}

void Client::RegisterRanked(uint64_t sessionId, uint64_t ownXuid, uint64_t opponent)
{
    if (!Connected() || !impl_->stats) return;
    Writer w;
    w.U64(sessionId);
    w.U64(ownXuid);
    w.U64(opponent);
    impl_->Queue(kArbRegister, w.b);
}

void Client::ReportMatch(uint64_t sessionId, bool started, uint8_t localPlayers, const std::vector<uint64_t>& peers)
{
    if (!Connected() || !impl_->matches || !sessionId) return;
    Writer w;
    w.U64(sessionId);
    w.U8(started ? 1 : 2);
    w.U8(uint8_t((std::clamp)(int(localPlayers), 1, 4)));
    const size_t n = (std::min)(peers.size(), size_t(7));
    w.U8(uint8_t(n));
    for (size_t i = 0; i < n; ++i) w.U64(peers[i]);
    impl_->Queue(kMatchEvent, w.b);
}

namespace {
bool StatsRequest(Client::Impl& m, uint8_t type, const std::vector<uint8_t>& body, StatPage& page, int timeoutMs)
{
    uint32_t id;
    {
        std::lock_guard l(m.reqMutex);
        id = m.nextRequest++;
        m.outstanding.insert(id);
    }
    Writer w;
    w.U32(id);
    w.b.insert(w.b.end(), body.begin(), body.end());
    m.Queue(type, w.b);
    std::unique_lock l(m.reqMutex);
    m.reqCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return m.statPages.count(id) || !m.connected; });
    m.outstanding.erase(id);
    const auto it = m.statPages.find(id);
    if (it == m.statPages.end()) return false;
    const bool ok = it->second.first;
    page = std::move(it->second.second);
    m.statPages.erase(it);
    return ok;
}
} // namespace

bool Client::ReadBoard(uint32_t view, uint32_t rankStart, uint32_t count, uint64_t pivotXuid,
                       const std::vector<uint16_t>& columns, StatPage& page, int timeoutMs)
{
    if (!Connected() || !impl_->stats || columns.size() > kMaxStatColumns) return false;
    Writer w;
    w.U32(view);
    w.U32(rankStart);
    w.U32(uint32_t((std::min)(size_t(count), kMaxStatRows)));
    w.U64(pivotXuid);
    w.U8(uint8_t(columns.size()));
    for (uint16_t c : columns) w.U16(c);
    return StatsRequest(*impl_, kStatsBoard, w.b, page, timeoutMs);
}

bool Client::ReadPlayers(uint32_t view, const std::vector<uint64_t>& xuids, const std::vector<uint16_t>& columns,
                         StatPage& page, int timeoutMs)
{
    if (!Connected() || !impl_->stats || columns.size() > kMaxStatColumns || xuids.size() > kMaxStatRows) return false;
    Writer w;
    w.U32(view);
    w.U8(uint8_t(columns.size()));
    for (uint16_t c : columns) w.U16(c);
    w.U8(uint8_t(xuids.size()));
    for (uint64_t x : xuids) w.U64(x);
    return StatsRequest(*impl_, kStatsPlayers, w.b, page, timeoutMs);
}

void Client::Invite(uint64_t toPeer, uint64_t sessionId)
{
    if (!Connected()) return;
    Writer w;
    w.U64(toPeer);
    w.U64(sessionId);
    impl_->Queue(kInviteSend, w.b);
}

bool Client::TakeInvite(std::vector<uint8_t>& record, uint64_t& fromPeer)
{
    Impl& m = *impl_;
    std::lock_guard l(m.reqMutex);
    if (m.invites.empty()) return false;
    fromPeer = m.invites.front().first;
    record = std::move(m.invites.front().second);
    m.invites.pop_front();
    return true;
}

Client::Stats Client::GetStats() const
{
    Stats s;
    s.rttMs = impl_->rttMs;
    return s;
}

std::vector<uint8_t> InfoRequest(uint64_t nonce, size_t size)
{
    Writer w;
    w.U8(kInfoReq);
    w.U32(kInfoMagic);
    w.U64(nonce);
    w.b.resize((std::max)(size, kInfoReqMinSize), 0);
    return w.b;
}

bool ParseInfo(const uint8_t* data, size_t size, uint64_t nonce, ServerInfo& info)
{
    Reader r(data, size);
    if (r.U8() != kInfo || r.U32() != kInfoMagic || r.U64() != nonce) return false;
    ServerInfo out;
    out.protocolVersion = r.U16();
    const uint8_t flags = r.U8();
    out.password = (flags & 1) != 0;
    out.verified = (flags & 2) != 0;
    out.name = Clean(r.Str(256), 64);
    out.region = Clean(r.Str(256), 32);
    out.online = r.U16();
    out.maxPlayers = r.U16();
    out.inMatch = r.U16();
    out.sessions = r.U16();
    out.motd = Clean(r.Str(4096), 1000);
    const std::string protocols = Clean(r.Str(512), 256);
    if (!r.ok()) return false;
    for (size_t at = 0; at <= protocols.size();)
    {
        size_t comma = protocols.find(',', at);
        if (comma == std::string::npos) comma = protocols.size();
        if (comma > at && out.gameProtocols.size() < 16) out.gameProtocols.push_back(protocols.substr(at, comma - at));
        at = comma + 1;
    }
    info = std::move(out);
    return true;
}

void ForgetServerKey(const std::string& knownServersFile, const std::string& host, uint16_t port)
{
    if (!knownServersFile.empty()) ForgetPin(knownServersFile, host + ":" + std::to_string(port));
}

} // namespace community
