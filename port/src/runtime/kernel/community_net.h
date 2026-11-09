// The community-server client (port/COMMUNITY_SERVER_PLAN.md, wire format server/PROTOCOL.md): a TLS control
// connection to a ttt2-server (server key pinned on first use) for sessions, invites and the game's GIN requests,
// and the server list's INFO exchange. Matches never go through the server: they run over Steam P2P.
// Everything a server sends is bounded and checked before the game sees it (plan 7b S6).
//
// No game-runtime dependencies: tools/community_net_test.cpp drives two clients in one process.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace community {

struct Options
{
    std::string host;
    uint16_t port = 28200;
    uint64_t steamId = 0;                     // the player's Steam ID (or a test id on a trusting server)
    std::string profileName;
    // Asked after the TLS handshake for a Steam Web API ticket (hex) for the server's identity string; "" = none.
    std::function<std::string(const std::string& identity)> ticket;
    std::string password;
    std::string gameProtocol;                 // e.g. "ttt2-port-7-rollback"
    std::string clientVersion;
    std::string knownServersFile;             // pinned server keys (trust on first use)
    int timeoutMs = 8000;
};

// Why a server turned a login away (server/PROTOCOL.md ERROR codes); Client::Refusal() after a failed Connect.
enum Refusal : int
{
    RefusedNone = 0, RefusedProtocol = 1, RefusedGameVersion = 2, RefusedFull = 3, RefusedPassword = 4, RefusedAuth = 5,
    RefusedBanned = 6, RefusedDuplicate = 7, RefusedMalformed = 8, RefusedMaintenance = 9,
};

struct Welcome
{
    uint64_t peer = 0;
    bool verified = false;                    // the server checked the Steam ID with Steam
    bool gin = false;                         // the server answers the game's GIN requests (GinRequest)
    bool stats = false;                       // the server keeps the game's statistics (the leaderboards)
    bool matches = false;                     // the server records the matches played on it (ReportMatch)
    bool files = false;                       // the server keeps the players' license files (PutFile / GetFile)
    bool serverRanks = false;                 // the ranks are the server's own (server/PROTOCOL.md "Ranks")
    std::string name, region, motd;
    std::string fingerprint;                  // the server key, 8 groups of 4 hex digits
    std::string tls;                          // negotiated protocol
};

// The game's Xbox LIVE statistics on a server (server/PROTOCOL.md "Statistics"): written properties, and board
// rows with their columns (type 0xFF: nothing written in that column).
struct StatProperty { uint32_t id = 0; uint8_t type = 0; uint64_t bits = 0; };
struct StatViewWrite { uint32_t view = 0; std::vector<StatProperty> props; };
struct StatValue { uint16_t column = 0; uint8_t type = 0xFF; uint64_t bits = 0; };
struct StatRow { uint64_t xuid = 0; uint32_t rank = 0; int64_t rating = 0; std::string name; std::vector<StatValue> columns; };
struct StatPage { uint32_t total = 0; std::vector<StatRow> rows; };
constexpr size_t kMaxStatRows = 100, kMaxStatColumns = 64, kMaxStatViews = 16, kMaxStatProps = 32;
constexpr size_t kMaxPlayerFile = 16u << 10;     // a player file (licence.dat is 3420 bytes)

class Client
{
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Connects and logs in (blocking, at most opts.timeoutMs). On failure `error` is what the player should read.
    bool Connect(const Options& opts, Welcome& welcome, std::string& error);
    void Disconnect();
    bool Connected() const;
    uint64_t LocalPeer() const;
    // The end of the ban or kick block behind a RefusedBanned (ERROR 6 after its text, told only to the player it bans):
    // unix seconds, 0 = for good; false when the server did not say (an older server).
    bool BanUntil(uint64_t& until) const;
    // The server's refusal code when Connect failed because the server said no, or the ERROR code that ended the
    // connection later (RefusedBanned: kicked or banned while playing); RefusedNone otherwise.
    int Refusal() const;
    // Why the connection ended ("" while connected).
    std::string CloseReason() const;

    // Sessions (the port's SES2 records). Publish / Remove queue a frame; List / Get wait for the answer.
    void PublishSession(const std::vector<uint8_t>& record);
    void RemoveSession(uint64_t id);
    // List: the newest sessions of this game version; with `contexts` only those whose (context id, value) match (a
    // session without that context matches too), with `openOnly` only those with a free public slot - the server
    // filters among all its sessions, so a search finds its kind of room among thousands of others.
    std::vector<std::vector<uint8_t>> ListSessions(uint32_t max, int timeoutMs,
                                                   const std::vector<std::pair<uint32_t, uint32_t>>& contexts = {},
                                                   bool openOnly = false);
    bool GetSession(uint64_t id, std::vector<uint8_t>& record, int timeoutMs);
    // One HTTP request of the game's GIN client and the server's whole response (server/PROTOCOL.md "GIN");
    // false when the server does not answer GIN, the connection ends or the time runs out.
    bool GinRequest(const std::vector<uint8_t>& request, std::vector<uint8_t>& response, int timeoutMs);
    void Invite(uint64_t toPeer, uint64_t sessionId);
    bool TakeInvite(std::vector<uint8_t>& record, uint64_t& fromPeer);

    // Statistics (only to a server whose WELCOME offers them). WriteStats: one XSessionWriteStats of the game,
    // about the player whose online XUID is `subjectXuid`, written by this player (`ownXuid`) in session
    // `sessionId` (0: none; a ranked match's results count once both players' games reported the same).
    // ReadBoard: `count` rows of a view's board from place `rankStart` (1 = first), or the page around
    // `pivotXuid`'s place when it is not 0. ReadPlayers: those players' rows. False when the server has no answer.
    void WriteStats(uint64_t sessionId, uint64_t ownXuid, uint64_t subjectXuid, const std::vector<StatViewWrite>& views);
    // A ranked match starting (the game's XSessionArbitrationRegister): this player takes part in session
    // `sessionId` against `opponent` (the Steam ID its game is connected to) - the host and the player it names, who
    // names it back, are the match; only their results count.
    void RegisterRanked(uint64_t sessionId, uint64_t ownXuid, uint64_t opponent);
    // A match of session `sessionId` started / ended at this game, against the PCs `peers` (Steam IDs), with
    // `localPlayers` players here (2: pair play): the server's match history. Only to a server whose WELCOME says it
    // records matches (bit 3); to any other it is not sent (an unknown frame ends the connection).
    void ReportMatch(uint64_t sessionId, bool started, uint8_t localPlayers, const std::vector<uint64_t>& peers);

    // The players' license files (server/PROTOCOL.md "Player files"; only to a server whose WELCOME has bit 4): the
    // game's own per-user title-storage item `item` (licence.dat: the license other players view) as it uploaded it,
    // and another player's, by the online XUID their game uses. GetFile: 1 found (`data`), 0 the server has none,
    // -1 no answer (a server without them, asked too often, or the connection ended).
    void PutFile(uint64_t ownXuid, const std::string& item, const std::vector<uint8_t>& data);
    int GetFile(uint64_t ownerXuid, const std::string& item, std::vector<uint8_t>& data, int timeoutMs);
    bool KeepsFiles() const;
    // A server whose ranks are its own (server/PROTOCOL.md "Ranks", WELCOME bit 5) keeps the license and penalties
    // this game plays with there: GetFile with owner 0 reads this player's own (licence.dat, penalty.dat), PutFile
    // writes them, DeleteOwnFile removes one.
    bool ServerRanks() const;
    void DeleteOwnFile(uint64_t ownXuid, const std::string& item);
    // The fingerprint of the server's key (64 hex digits; the same server at any address), "" before a login.
    std::string ServerKey() const;

    // Is `peer` (a Steam ID that reached this game directly) a player of this server? 1 yes, 0 no, -1 no answer
    // (asked too often, or the connection ended): games on a server play only with each other.
    int PeerOnServer(uint64_t peer, int timeoutMs);

    // The names of the players whose games use these online XUIDs (at most 16), as this server knows them (players
    // it has seen); false: no answer (a server without statistics, or the connection ended).
    bool NamesOf(const std::vector<uint64_t>& xuids, std::map<uint64_t, std::string>& names, int timeoutMs);
    bool ReadBoard(uint32_t view, uint32_t rankStart, uint32_t count, uint64_t pivotXuid,
                   const std::vector<uint16_t>& columns, StatPage& page, int timeoutMs);
    bool ReadPlayers(uint32_t view, const std::vector<uint64_t>& xuids, const std::vector<uint16_t>& columns,
                     StatPage& page, int timeoutMs);

    // Diagnostics: the round trip to the server (its PING / PONG).
    struct Stats { int rttMs = -1; };
    Stats GetStats() const;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

// "host[:port]" -> host, port (default 28200); false when malformed.
bool ParseAddress(const std::string& text, std::string& host, uint16_t& port);

// The server list's INFO exchange (server/PROTOCOL.md "UDP", unauthenticated, shown only as information): a
// request of `size` bytes (at least 128; the answer may be up to 3 x that, so a bigger request gets more of the
// message of the day) and its answer, bounded and cleaned like everything else a server sends.
struct ServerInfo
{
    uint16_t protocolVersion = 0;
    bool password = false;                    // the server asks for a password
    bool verified = false;                    // the server checks Steam IDs with Steam
    bool ownRanks = false;                    // the ranks there are the server's own (PROTOCOL.md "Ranks")
    std::string name, region, motd;
    uint16_t online = 0, maxPlayers = 0, inMatch = 0, sessions = 0;   // inMatch: players in rooms and their matches
    std::vector<std::string> gameProtocols;   // the game versions it accepts
};
constexpr size_t kInfoRequestSize = 420;
std::vector<uint8_t> InfoRequest(uint64_t nonce, size_t size = kInfoRequestSize);
// False when `data` is not an INFO answer to `nonce`.
bool ParseInfo(const uint8_t* data, size_t size, uint64_t nonce, ServerInfo& info);

// Forgets a pinned server key (the player removed the server from the list: adding it again trusts its key anew).
void ForgetServerKey(const std::string& knownServersFile, const std::string& host, uint16_t port);

} // namespace community
