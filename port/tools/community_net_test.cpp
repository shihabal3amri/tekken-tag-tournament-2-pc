// community_net_test: the community-server client (kernel/community_net.h) against a running ttt2-server whose
// configuration trusts unverified Steam IDs (a test server). Two clients in this process:
//  - TLS with the server key pinned (first use, match, and a changed key refused);
//  - sessions: publish, list, get, gone when the host leaves; invites never accepted for the player;
//  - GIN requests answered through the server;
//  - statistics: an offline board (SURVIVAL) written and read back as a page and by player;
//  - the server list (kernel/server_list.h): servers.ini with an encrypted password, INFO answers, LAN search,
//    removal forgetting the pinned key; refusal codes.
//
//   community_net_test <host:port> [expected key fingerprint]     exit code 0 = everything passed
// (the fingerprint as the server prints it: the game's SChannel re-encoding of the key must hash the same)
#include "community_net.h"
#include "server_list.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace community;

namespace {

int g_failures = 0;
void Check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) ++g_failures;
}

std::string TempFile(const char* name)
{
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    return std::string(dir) + name;
}

// A session record as the port writes it (matchmaking.cpp Serialize): SES2 header, XSESSION_INFO naming the host.
std::vector<uint8_t> Record(uint64_t id, uint64_t host)
{
    std::vector<uint8_t> v;
    auto u32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); };
    auto u64 = [&](uint64_t x) { u32(uint32_t(x)); u32(uint32_t(x >> 32)); };
    u32(0x32534553);
    u64(id);
    u64(host);
    u64(0x0009000000000001ull);
    u32(0x32F);
    u32(2); u32(0); u32(1); u32(0);
    uint8_t info[0x3C] = {};
    for (int i = 0; i < 8; ++i) info[i] = uint8_t(id >> (56 - 8 * i));
    for (int i = 0; i < 8; ++i) info[0x20 + i] = uint8_t(host >> (56 - 8 * i));
    v.insert(v.end(), info, info + sizeof(info));
    u32(1); u32(0x800A); u32(1);
    u32(0);
    u32(0x80000001);
    return v;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: community_net_test <host:port>\n"); return 2; }
    Options base;
    if (!ParseAddress(argv[1], base.host, base.port)) { std::printf("bad address %s\n", argv[1]); return 2; }
    base.gameProtocol = "ttt2-port-7-rollback";
    base.clientVersion = "community_net_test";
    base.knownServersFile = TempFile("ttt2_known_servers_test.txt");
    DeleteFileA(base.knownServersFile.c_str());
    base.timeoutMs = 5000;

    std::printf("address parsing\n");
    {
        std::string h;
        uint16_t p;
        Check(ParseAddress("play.example.net", h, p) && h == "play.example.net" && p == 28200, "host, default port");
        Check(ParseAddress(" 10.0.0.5:3000 ", h, p) && h == "10.0.0.5" && p == 3000, "ip:port, spaces");
        Check(ParseAddress("[::1]:28201", h, p) && h == "::1" && p == 28201, "[ipv6]:port");
        Check(!ParseAddress("host:99999", h, p) && !ParseAddress("bad host", h, p) && !ParseAddress("", h, p) &&
              !ParseAddress("a/b:1", h, p), "malformed addresses refused");
    }

    std::printf("connect + pinning\n");
    Client a, b;
    Welcome wa, wb;
    std::string error;
    Options oa = base, ob = base;
    oa.steamId = 0x0110000100F00001ull;
    oa.profileName = "TEST A";
    ob.steamId = 0x0110000100F00002ull;
    ob.profileName = "TEST B";
    const bool aUp = a.Connect(oa, wa, error);
    Check(aUp, ("A connects (" + error + ")").c_str());
    if (!aUp) return 1;
    std::printf("        server \"%s\", %s, key %s, %s\n", wa.name.c_str(), wa.tls.c_str(), wa.fingerprint.c_str(),
                wa.verified ? "verified" : "unverified");
    Check(b.Connect(ob, wb, error), "B connects (pin matches)");
    Check(wa.fingerprint == wb.fingerprint && !wa.fingerprint.empty(), "same server key seen twice");
    if (argc > 2) Check(wa.fingerprint == argv[2], "the game's key fingerprint equals the server's");
    {
        Options oc = base;
        oc.steamId = 0x0110000100F00003ull;
        oc.knownServersFile = TempFile("ttt2_known_servers_bad.txt");
        FILE* f = std::fopen(oc.knownServersFile.c_str(), "w");
        std::fprintf(f, "%s:%u %s\n", oc.host.c_str(), oc.port, std::string(64, '0').c_str());
        std::fclose(f);
        Client c;
        Welcome wc;
        std::string e;
        Check(!c.Connect(oc, wc, e) && e.find("identity") != std::string::npos, "a changed server key is refused");
        DeleteFileA(oc.knownServersFile.c_str());
    }

    std::printf("sessions\n");
    const uint64_t sid = 0x8000000000777001ull;
    a.PublishSession(Record(sid, a.LocalPeer()));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto list = b.ListSessions(20, 3000);
    Check(list.size() == 1 && list[0] == Record(sid, a.LocalPeer()), "B lists A's session");
    std::vector<uint8_t> rec;
    Check(b.GetSession(sid, rec, 3000) && rec == Record(sid, a.LocalPeer()), "B finds it by id");
    Check(a.ListSessions(20, 3000).empty(), "A does not list its own");
    Check(b.ListSessions(20, 3000, { { 0x800A, 0 } }, true).empty() && b.ListSessions(20, 3000, { { 0x800A, 1 } }, true).size() == 1,
          "a search's filters (GAME_TYPE, free slots) are applied by the server");
    // An invite reaches B but is not taken on B's behalf: on a server of strangers it needs the game's own
    // Accept / Decline window first (community_net.cpp kInvite).
    a.Invite(b.LocalPeer(), sid);                       // only a session's host invites to it
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::vector<uint8_t> invite;
    uint64_t from = 0;
    Check(!b.TakeInvite(invite, from) && b.Connected(), "an invite is not accepted for the player");

    std::printf("GIN through the server\n");
    {
        Check(wa.gin, "the server offers GIN");
        const std::string get = "GET /2.2/ttt2/xbox/settings HTTP/1.1\r\nUser-Agent: GINware/2.2 (xbox)\r\n\r\n";
        std::vector<uint8_t> response;
        const bool ok = a.GinRequest(std::vector<uint8_t>(get.begin(), get.end()), response, 3000);
        const std::string text(response.begin(), response.end());
        Check(ok && text.rfind("HTTP/1.1 200", 0) == 0 && text.find("\"messages-motd-en\"") != std::string::npos,
              "settings answered (HTTP, the game's JSON)");
        const std::string connect = "POST /2.2/ttt2/xbox/connect HTTP/1.1\r\nContent-Length: 22\r\n\r\nuserid=2798379101111979";
        Check(a.GinRequest(std::vector<uint8_t>(connect.begin(), connect.end()), response, 3000) &&
              std::string(response.begin(), response.end()).find("\"session_id\"") != std::string::npos, "GIN login answered");
    }

    std::printf("        server rtt A %d ms\n", a.GetStats().rttMs);

    std::printf("statistics\n");
    if (!wa.stats) Check(false, "the server keeps statistics");
    else
    {
        // SURVIVAL (view 230, not arbitrated): each writes its own rating; the board puts B first
        const uint64_t xa = 0x0009000000F00001ull, xb = 0x0009000000F00002ull;
        auto survival = [](int64_t rating) {
            StatViewWrite w;
            w.view = 230;
            w.props.push_back({ 0x20000001u, 2, uint64_t(rating) });
            w.props.push_back({ 0x1000000Eu, 1, uint64_t(int64_t(-3)) });   // a column (MAIN_CHARA_ID), negative
            return std::vector<StatViewWrite>{ w };
        };
        a.WriteStats(0, xa, xa, survival(25));
        b.WriteStats(0, xb, xb, survival(40));
        b.WriteStats(0, xb, xa, survival(9999));                // about another player: not kept
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        StatPage page;
        const bool ok = a.ReadBoard(230, 1, 10, 0, { 1, 5 }, page, 3000);
        Check(ok && page.rows.size() >= 2 && page.rows[0].xuid == xb && page.rows[0].rating >= 40 && page.rows[0].name == "TEST B",
              "a page of the board, best first, with names");
        bool columns = false;
        for (const StatRow& r : page.rows)
            if (r.xuid == xa)
                columns = r.rating == 25 && r.columns.size() == 2 && r.columns[0].column == 1 && r.columns[0].type == 1 &&
                          int32_t(r.columns[0].bits) == -3 && r.columns[1].type == 0xFF;
        Check(columns, "requested columns, a missing one marked empty");
        StatPage mine;
        Check(b.ReadPlayers(230, { xa, 0x0009FFFFFFFFFFFFull }, { 1 }, mine, 3000) && mine.rows.size() == 1 && mine.rows[0].xuid == xa &&
              mine.rows[0].rank == 2, "a player's row and place");
        StatPage none;
        Check(!a.ReadBoard(4242, 1, 10, 0, {}, none, 3000), "an unknown board answers no");
    }

    std::printf("server list (servers.ini, INFO)\n");
    {
        const std::string ini = TempFile("ttt2_servers_test.ini");
        DeleteFileA(ini.c_str());
        SetEnvironmentVariableA("TTT2_SERVERS_INI", ini.c_str());
        _putenv_s("TTT2_SERVERS_INI", ini.c_str());
        Check(server_list::Save({ argv[1], "", "pass word;=x" }), "a server is saved");
        server_list::SetCurrent(argv[1]);
        std::string text;
        {
            FILE* f = std::fopen(ini.c_str(), "rb");
            char chunk[4096];
            for (size_t n; f && (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) text.append(chunk, n);
            if (f) std::fclose(f);
        }
        Check(text.find("dpapi:") != std::string::npos && text.find("pass word") == std::string::npos, "its password is stored encrypted");
        const auto saved = server_list::List();
        Check(saved.size() == 1 && saved[0].password == "pass word;=x" && server_list::Current() == argv[1], "list, password and current read back");
        Check(server_list::SameAddress("LOCALHOST", "localhost:28200") && !server_list::SameAddress("a:1", "a:2"), "addresses compared");

        server_list::Ask(argv[1]);
        server_list::Live live;
        for (int waited = 0; waited < 3000; waited += 50)
        {
            live = server_list::Get(argv[1]);
            if (live.answers && live.full) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::printf("        \"%s\" (%s) %u/%u players, %u in a match, %u sessions, %d ms, protocols %zu, motd %zu chars\n",
                    live.info.name.c_str(), live.info.region.c_str(), live.info.online, live.info.maxPlayers, live.info.inMatch,
                    live.info.sessions, live.pingMs, live.info.gameProtocols.size(), live.info.motd.size());
        Check(live.state == server_list::State::Answered && live.full && live.info.name == wa.name, "the server answers INFO with its name");
        Check(live.info.online >= 2 && live.info.sessions == 1 && live.pingMs >= 0, "players online, sessions and ping");
        Check(std::find(live.info.gameProtocols.begin(), live.info.gameProtocols.end(), base.gameProtocol) != live.info.gameProtocols.end(),
              "it accepts this game version");
        Check(server_list::List()[0].name == wa.name, "the saved server keeps the name it reported");
        server_list::Watch(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const auto lan = server_list::Lan();
        std::printf("        LAN search: %zu server(s)%s%s\n", lan.size(), lan.empty() ? "" : ", first at ", lan.empty() ? "" : lan[0].address.c_str());
        server_list::Watch(false);

        // answers that are not answers
        const std::vector<uint8_t> request = InfoRequest(42);
        ServerInfo info;
        Check(request.size() == kInfoRequestSize && !ParseInfo(request.data(), request.size(), 42, info), "a request is not an answer");
        std::vector<uint8_t> fake = { 2, 0x54, 0x54, 0x53, 0x49, 7, 0, 0, 0, 0, 0, 0, 0 };
        Check(!ParseInfo(fake.data(), fake.size(), 7, info) && !ParseInfo(fake.data(), fake.size(), 8, info), "truncated or foreign answers refused");

        server_list::Remove(argv[1]);
        Check(server_list::List().empty() && server_list::Current().empty(), "removed (and no longer the network)");
        std::string host;
        uint16_t port = 0;
        ParseAddress(argv[1], host, port);
        ForgetServerKey(base.knownServersFile, host, port);
        std::string pins;
        {
            FILE* f = std::fopen(base.knownServersFile.c_str(), "rb");
            char chunk[4096];
            for (size_t n; f && (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) pins.append(chunk, n);
            if (f) std::fclose(f);
        }
        Check(pins.find(host + ":" + std::to_string(port)) == std::string::npos, "its pinned key is forgotten");
        DeleteFileA(ini.c_str());
    }

    std::printf("refusals\n");
    {
        Options od = base;
        od.steamId = oa.steamId;                         // A is online: a second login with its Steam ID
        Client d;
        Welcome wd;
        std::string e;
        const bool up = d.Connect(od, wd, e);
        std::printf("        duplicate login: %s (%s, code %d)\n", up ? "accepted" : "refused", e.c_str(), d.Refusal());
        Check(up || d.Refusal() != RefusedNone, "a refusal carries the server's code");
        d.Disconnect();
    }

    std::printf("host leaves\n");
    a.Disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    Check(b.ListSessions(20, 3000).empty(), "the session went with its host");
    b.Disconnect();
    DeleteFileA(base.knownServersFile.c_str());
    std::printf(g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
