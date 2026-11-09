# ttt2-server wire protocol, version 2

The game (the PC port's `kernel/community_net.cpp`) and `ttt2-server` talk over one TCP port (the control
connection, TLS) and one UDP port (the public status query for the server list), 28200 by default. The server is
where players find each other (sessions, invites) and what answers the game's own online services (GIN: settings,
news, profiles, leaderboards); the matches themselves never pass through it - the two games reach each other over
Steam P2P (Valve's relays), by the Steam IDs the server lists sessions under. Version 1 also relayed match
traffic; version 2 does not, and a version 1 game is refused (error 1). All integers are little-endian unless noted. Strings are `u16 length + UTF-8`; blobs are `u32 length + bytes`.
Security requirements behind these choices: `port/COMMUNITY_SERVER_PLAN.md` section 7b.

## Control connection: TLS, then frames

TLS 1.3 (1.2 minimum). The server's certificate is self-signed (created on first run, `data/tls.key` +
`data/tls.crt`); the game pins the SHA-256 of its SubjectPublicKeyInfo on the first successful connection
(trust on first use) and refuses the server if that key ever changes. The key's fingerprint is

    fingerprint = SHA-256(SubjectPublicKeyInfo DER)
    identity    = "ttt2-" + base64url (no padding) of the first 19 bytes of fingerprint     (31 characters)

and `identity` is what the game asks Steam a Web API ticket for, so a ticket is worthless at any other server.
Measured with Valve: Steam binds at most 31 characters of an identity (a longer one is cut and then matches
nothing), an identity starting with a short `name:` prefix never matches, and a ticket checked without an
identity parameter passes whatever it was made for - so every server always sends its identity.

Frames: `u32 size` (type byte + payload), `u8 type`, payload. Before WELCOME a frame is at most 16 KiB, after it
256 KiB. The client sends HELLO, then AUTH; anything else before WELCOME closes the connection. The server closes
connections that send nothing for 30 s (the client pings every 5 s) and handshakes that take over 10 s.

| Type | Name | Direction | Payload |
|---|---|---|---|
| 1 | HELLO | C->S | u16 protocol version (2), str game protocol (e.g. `ttt2-port-7-rollback`), str client version |
| 2 | AUTH | C->S | u64 Steam ID, str profile name, blob Steam Web API ticket (hex text, made for `identity`; empty when the game has no Steam), str password (empty if none) |
| 3 | PING | C->S | u64 nonce |
| 4 | SESSION_PUT | C->S | blob session record (`SES2`, below) |
| 5 | SESSION_DEL | C->S | u64 session id |
| 6 | SESSION_LIST | C->S | u32 request id, u32 max results (<= 64), optional: u8 context count (<= 16) + (u32 context id, u32 value), u8 flags (bit 0: only sessions with a free public slot) - a session matches when it has each context named with that value; the answer: the most recently opened matches among all the server's sessions of this game version, one per host |
| 7 | SESSION_GET | C->S | u32 request id, u64 session id |
| 8 | INVITE_SEND | C->S | u64 to peer, u64 session id (one of the sender's sessions) |
| 9 | GIN_REQUEST | C->S | u32 request id, blob: one whole HTTP request of the game's GIN client (at most 256 KiB); only to a server whose WELCOME has flag bit 1 |
| 10 | STATS_WRITE | C->S | u64 session id (0: none), u64 own XUID, u64 subject XUID, u8 view count (1..16) + views: u32 view id, u8 property count (<= 32) + properties: u32 property id, u8 type, u64 value |
| 11 | STATS_BOARD | C->S | u32 request id, u32 view id, u32 first place (1 = top), u32 rows (<= 100), u64 pivot XUID (not 0: the page around that player instead), u8 column count (<= 64) + u16 column ids |
| 12 | STATS_PLAYERS | C->S | u32 request id, u32 view id, u8 column count + u16 column ids, u8 XUID count (<= 100; the first 16 different ones are answered) + u64 XUIDs |
| 13 | ARB_REGISTER | C->S | u64 session id, u64 own XUID, u64 opponent (the Steam ID the game is playing against): the game starts a ranked match in that session |
| 14 | PEER_CHECK | C->S | u32 request id, u64 peer (Steam ID): is that player on this server now? (10 a second, burst 40; beyond that the answer is "ask again") |
| 15 | XUID_NAMES | C->S | u32 request id, u8 count (<= 16) + u64 online XUIDs: the names of those players (the game's gamertag lookups, XUserFindUsers: TEKKEN CHANNEL shows a replay's players by name); 5 a second, burst 20 |
| 16 | MATCH_EVENT | C->S | u64 session id, u8 event (1 started, 2 ended), u8 players at the sender's PC (1..4; 2 for pair play), u8 peer count (0..7) + u64 peers (the Steam IDs of the match's other PCs); only to a server whose WELCOME has flag bit 3 ("Match history") |
| 17 | FILE_PUT | C->S | u64 own XUID, str item (`licence.dat`), blob the file (1..16384 bytes): this player's copy for the others ("Player files"); only to a server whose WELCOME has flag bit 4 |
| 18 | FILE_GET | C->S | u32 request id, u64 owner XUID, str item: another player's file, by the online XUID their game uses (0: the asker's own, on a server whose WELCOME has flag bit 5, after OWN_RANKS - "Ranks"); only to a server whose WELCOME has flag bit 4 |
| 19 | OWN_RANKS | C->S | (empty): the game takes its ranks from this server ("Ranks"); sent right after WELCOME; only to a server whose WELCOME has flag bit 5 |
| 20 | FILE_DEL | C->S | u64 own XUID, str item (`licence.dat`, `penalty.dat`): delete this player's own copy ("Ranks"); after OWN_RANKS only; counted with FILE_PUT |
| 101 | WELCOME | S->C | u64 peer id (= Steam ID), u8 flags (bit 0: the Steam ID was checked with Steam; bit 1: GIN_REQUEST is answered; bit 2: statistics are kept - frames 10..13; bit 3: matches are recorded - frame 16; bit 4: player files are kept - frames 17, 18; bit 5: the ranks are this server's own - frames 19, 20, needs bit 4), str server name, str region, str message of the day |
| 102 | ERROR | S->C | u32 code, str text for the player; the server closes the connection after it |
| 103 | PONG | S->C | u64 nonce |
| 104 | SESSION_LIST_RESULT | S->C | u32 request id, u32 count, count x blob record |
| 105 | SESSION_GET_RESULT | S->C | u32 request id, u8 found, blob record (empty when not found) |
| 106 | INVITE | S->C | u64 from peer, blob record |
| 109 | NOTICE | S->C | str text (admin `say`) |
| 110 | GIN_RESPONSE | S->C | u32 request id, blob: the whole HTTP response (answers come in request order) |
| 111 | STATS_RESULT | S->C | u32 request id, u8 ok (0: no such view, or the server could not answer), u32 players on the board, u16 row count + rows: u64 XUID, u32 place, i64 rating, str name, u8 column count + columns: u16 id, u8 type (0xFF: nothing written), u64 value |
| 112 | PEER_RESULT | S->C | u32 request id, u8 answer: 1 the peer is a player of this server (same game version, not the asker), 0 not, 2 ask again later |
| 113 | XUID_NAMES_RESULT | S->C | u32 request id, u8 count + (u64 XUID, str name: printable ASCII, at most 15 characters) for the players the server has seen with those XUIDs (none on a server that keeps no statistics) |
| 114 | FILE_RESULT | S->C | u32 request id, u8 found (1 yes, 0 no such player or file, 2 ask again later), blob the file (empty unless found) |

A game on a server plays only that server's players: its match transport holds every packet from a peer until
PEER_CHECK says yes (then delivers them in order), drops them on a no (and asks again after 10 s), so neither a
player of another server nor a player on plain Steam can reach it. On plain Steam the game asks nothing.

Error codes: 1 protocol version, 2 game version not accepted, 3 server full, 4 wrong password, 5 Steam check
failed (or the server is busy checking), 6 banned / kicked, 7 Steam ID already connected, 8 malformed or too
fast, 9 maintenance. (A connection over an address's limits is closed before TLS, without a frame.)

Error 6's text says until when, for the game to show as it is: `You are banned from this server until 2026-11-01.`
(the day the ban ends, UTC) or `You are banned from this server for good.`; a kick that keeps the player out (a
timed ban whose reason starts with `kicked`, data/bans.txt) `You were removed from this server. You can come back in
10 minutes.` (`in a minute`, `in N minutes`, from two hours on `in N hours`), a kick that does not `You were removed
from this server.` - each followed by ` Reason: <the admin's reason>` when there is one. The same text refuses the
player's logins while the ban lasts. After the text, error 6 carries `u64 until` (unix seconds; 0 = for good; a kick
that keeps nobody out: the time it was sent): the game leaves the server out of its server list until then (older
games stop reading after the text). Bans are looked up only after the Steam check (on a server that checks Steam
IDs): a ban, its reason and its end are told only to the player it bans, never to someone claiming their Steam ID -
who gets error 5 like any failed check. A banned player's refused logins count as failed ones (five a minute from one
address block it for a minute).

Limits: more than 40 frames per second (burst 80) closes the connection with error 8; beyond that, session updates
(4 per second), lists (2 per second, at most 256 KiB of records) and invites (10 per minute sent, 3 per minute
received) are ignored; 4 hosted sessions; 2 MiB of replies waiting for a game
that does not read. Connections: per IPv4 address or IPv6 /64, at most `max_connections_per_ip` open and 4 in
their handshake (1024 for the whole server); TLS within 5 s, HELLO + AUTH within 10 s more. Five failed logins
(wrong password, failed Steam check, malformed login) from one address within a minute block it for a minute.
Steam checks: at most 16 at a time and 20 per second. On a server that does not check Steam IDs, a second
connection with the Steam ID of an online player is refused (error 7, "already connected"); on a checking server
it replaces the old one (a reconnect).

INVITE is not acted on by the game until the game asks the player (its own Accept / Decline window): on a server
of strangers an invite must never pull a player into a match by itself.

## GIN

The game's own online services spoke Namco's "GIN" web API (the game's client library calls itself GINware/2.2):
plain HTTP/1.1 to `/2.2/ttt2/xbox/<endpoint>`, one request per TCP connection, the response read until the
connection closes. The client carries each request in a GIN_REQUEST and hands the GIN_RESPONSE's bytes back to the
game (rebuilt by its JSON cleaner first), then closes the game's connection. The server answers from what the
player is (the TLS login), never from ids in the request.

Replies (MEASURED 2026-10-22 in the game's library, sub_82A5E990): HTTP 200 or 201, and a JSON `"message"` of
`"OK"` for a request that reads data back or `"Created"` for an upload - anything else is a failed request. An
upload's reply is parsed with a schema of `message` alone: any other object in it (as an empty `data`) is a parse
error, so uploads are answered `{"message":"Created"}` and nothing else. (Until this was found every upload failed
in the game's eyes; a failed match report made it cancel that match's ghost and replay uploads.) Every text value
in a reply is a string, every integer a number, as the game's schemas have them - a number where it expects text
makes it drop the whole answer.

Uploads are the game's events: `{"sessionEventCount", "userId", "titleId", "platform", "sessionId", "payload": {...}}`
with every payload value a string.

- `connect` (form `userid=<online XUID>`; the XUID is recorded): `{"message":"OK","session_id":...}`. The game
  connects again before nearly every request.
- `settings`: `{"message":"OK","data":{...}}` with the message of the day and `news` lines as
  `messages-motd-<lang>` / `messages-news-<lang>` (at most 10 lines of 119 printable characters, no
  `~ % " \ < > &`), `ghost-download-count`, `throttle-uploads-per-hour-{max,ave}`.
- POST `player`: the profile the game uploads on every ONLINE MODE visit, kept (at most 64 KiB).
- POST `match` (after a ranked match) and `leaderboard/` (after a ranked match or an offline record): kept as sent,
  the newest 50 / 20 per player.
- GET `team?count=1&for-players=<XUID>`: the teams of those players (the server's admin makes teams: console
  `team`): `{"message":"OK","data":{"teams":[{"player","team":{...}}],"player-team-ids":[{"player","team"}]}}`.
- Ghosts. After a ranked match the winner's game uploads its fighters' ghosts - only from 1st dan (rank 10) up, at
  most once a day, and not after a match whose replay it uploaded: POST `custom/ghost` with payload
  `{"dan-rank-belt", "tag-ghost-1", "tag-ghost-2"}` or `{"dan-rank-belt", "solo-ghost"}`, a ghost being
  `{player (XUID), fighter, fighter-rank, match-beg, match-end, ghost-data}`. The server keeps one ghost per player,
  belt (0..10) and kind (the newest 300 players' per belt). GHOST BATTLE asks GET `custom/ghost?dan-belts=N` and gets
  `{"message":"OK","ghosts":{"dan-belt-ghosts-N":{"dan-rank-belt":N,"tag-ghost-1":G,"tag-ghost-2":G,
  "solo-ghost":G}}}` - one other player's tag pair and a solo ghost, picked among the newest 32 - or an empty
  `ghosts`.
- Replays. After a ranked match a dan-ranked player won outright, the winner's game uploads the replay (a 1st-7th dan
  player - ranks 10 to 16 - once a session, ranks 17-24 twice, 25-32 ten times, higher always): POST `custom/replay` with the
  replay item as payload `{match, match-type, match-beg, match-end, stage, party-1-player-1 .. party-2-player-2,
  party-1-fighter-1 .. party-2-fighter-2, party-1-fighter-1-rank .., replay-data}`. One replay per match; the
  newest 20 per player and 20000 in all. TEKKEN CHANNEL asks GET
  `custom/replay?page=P&count=N&replay-data=true|false` plus at most one of `&fighter=F`, `&tag-fighter=A,B`,
  `&solo-fighter=F` (each with `&sort-by-dan-rank=true`) or `&match-type=ranked|championship`, and gets
  `{"message":"OK","data":{"replays":[...]}}`: at most 4 items with `replay-data`, 100 without.
- `ghost-data` and `replay-data` are base64 with `+` written `%2B`; kept and sent back as the game wrote them (the
  client's JSON cleaner keeps `%XX` in those two values only). Every other field is checked against the game's
  schemas (the size of the buffer it copies into; text without `~ % " \ < > &`).

Limits: a request at most 256 KiB, an upload at most 200 KiB of JSON; 30 uploads an hour per player (as the
settings tell the game); everything a player uploads at most 8 MiB, all players' at most `max_upload_storage_mb`. At
most 5 requests per second per player (burst 30); beyond that the answer is HTTP 503. A GIN_RESPONSE is at most
1 MiB (a reply with ghosts or replays is up to ~400 KiB).

## Statistics (the leaderboards)

The game keeps its leaderboards in Xbox LIVE statistics: each board is a "view" with a rating it is sorted by
(highest first) and up to eight columns (fighters, their ranks, wins and losses), and the game writes properties
into them with XSessionWriteStats. The views, which property each column takes and how it keeps its value (the
newest write, or the best ever) are the game's own configuration - `internal/stats/views.go`, generated from it.
World Arena boards start again every month (UTC). Values are XUSER_DATA: type 1 int32, 2 int64, 3 double,
5 float, 7 date-time; integers sign-extended, a double's IEEE bits, a float's in the low half. A property whose
type does not match its id (the id's high nibble is its type) makes the whole STATS_WRITE ignored.

Players are kept by Steam ID. The XUID in the frames is the online XUID their game uses: a connection's first one
is its XUID for good (frames with another are dropped); it is recorded for lookups by XUID, and an XUID a player
whose Steam ID was checked already uses is not given to anyone else. Boards show the player's profile name.

- Views that are not arbitrated (CHICKEN, SURVIVAL, TIME ATTACK - offline modes): a player writes only its own
  (subject = own XUID); kept at once.
- Arbitrated views (BATTLE POINTS, RANK, RANKED MATCH WINS, WORLD ARENA) count only from a ranked match. When a
  ranked match starts, both games send ARB_REGISTER naming each other: the host of a live ranked session (its
  record has XSESSION_CREATE flag 0x10) and the player it names, who names the host back with another XUID, are the
  match - registrations nobody pairs with count for nothing (they are given back when the match pairs, or after 10
  minutes). A room can hold one match after another: a registration in a settled match starts the next one. MEASURED: at the end
  of a ranked match each game writes both players' results (its own and the opponent's). A player's results count
  when both games of the pair sent the same - and only then: different reports are discarded and logged, and a
  report the other game has not confirmed 2 minutes after the match's first report is discarded too (a player who
  leaves early loses nothing on the boards here; the game counts disconnects in its own profile). A modified game
  can veto its own match's results, never write any. Resends after a result was settled change nothing (a settled
  match is remembered 10 minutes; a pair that never reports is given back after 30). A disagreement is logged with
  the first view / property the two reports differ in.
- Ratings must fit the game's int32 range; a view or a property twice in one write, or a value whose type does not
  match its property, drops the whole STATS_WRITE.

Boards: the order of each board is kept in memory (read from the database at most every 10 seconds, and a second
after a write to it), so a page anywhere on a board and a player's place are cheap at any size; the rows' values are
read when asked.

When a ranked match settles - both subjects counted or discarded, its reports discarded after the 2 minutes, or
the pair given back after 30 minutes without a report - the match history ("Match history") gets its result.

Limits: 4 STATS_WRITE per second (burst 16), 1 ARB_REGISTER per second (burst 4), at most 4 open registrations per
player (and 16 x max_players matches remembered on the server); a connection's XUID changes (another profile) only
while it has none open; 5 reads per second (burst 20) and 8
statistics reads at once on the whole server - beyond either, the answer is "not ok". Dropped frames are logged at
most a few times a minute per player.

### Session records (`SES2`)

As `port/src/runtime/kernel/matchmaking.cpp` writes them: u32 magic `SES2`, u64 id, u64 host peer, u64 host
XUID, u32 flags, u32 max public, u32 max private, u32 filled public, u32 filled private, 60 bytes XSESSION_INFO
(big-endian: XNKID, then the host's XNADDR), u32 context count (<= 64) + count x (u32 id, u32 value), u32
property count (<= 64) + count x (u32 id, u32 size <= 4096, bytes), optional u32 netcode, nothing after. At most
16 KiB. The server checks all of it and that the host peer is the sender, the XNKID in the info is the id and the
XNADDR in the info names the host (bytes 0x20..0x28 of the info = host peer, big-endian), then passes the record
through unchanged. Sessions of other game protocols are never listed.

## Match history

A server whose WELCOME has flag bit 3 keeps every match its players play (its database: `matches`, one row per
match - session, kind, started, ended, result - and `match_players`, one per PC - Steam ID, the profile name the
server knew then, the players at that PC, won). The server sees no match traffic, so the games say when a match
starts and ends (MATCH_EVENT); a row is what they said. The game sends the frame only to a server that set the bit
(to any other it is an unknown frame type, which closes the connection).

- STARTED: a match is its session's while it is open. If the session has an open match, the sender joins it, and so
  do the peers it names that are players of this server (same game version, not the sender) - with 1 player each
  until their own STARTED gives their count. Else a match opens: kind `ranked` when the session's record has
  XSESSION_CREATE flag 0x10, else `player`. A STARTED from a player whose own STARTED is in the session's open match,
  20 s or more after that match opened, is the next match without an END: the open one is closed `unfinished` and a
  new one opens. A resend changes nothing.
- ENDED from any of the match's players (named ones too) closes it: result `ended` for a player match; a ranked
  match's result comes from its ranked results. An END for a match opened less than 10 s ago is the previous match's,
  arriving late, and is ignored, as is one from a player not in the match.
- Every 30 s: a match open for an hour, or none of whose players has been online with it as their newest match for
  a minute (a reconnect keeps it), is closed `unfinished`. At start-up and at a clean stop the server closes the
  matches still open (`unfinished`, an hour after they started at most).
- Ranked: when its ranked results settle ("Statistics"), the newest row of that session with both players and no
  ranked result yet becomes kind `ranked` with result `agreed` (both games reported the same), `disagreed`,
  `unconfirmed` (a report the other game never confirmed) or `unreported` (paired, nothing reported in 30 minutes);
  without such a row (games that send no MATCH_EVENT) one is added from the pair, from its registration to its
  settling. `won` stays NULL: the reports cannot tell the winner - every property the game writes is a running total
  of its own profile (WINS_COUNT, LOSE, the rank: aggregation Last), and MEASURED a ranked match wrote RANK views only.

Limits: MATCH_EVENT as ARB_REGISTER, 1 per second (burst 4), beyond that ignored; a player opens at most 3 matches
in a burst, then one every 20 s (joining an open match does not count), the server at most 16 x max_players open
ones. Malformed (an event other than 1 / 2, players outside 1..4, more than 7 peers, session 0, bytes missing or
left over): error 8. Matches older than `keep_matches_days` (365 by default; 0 = for ever) are deleted once a day.

## Player files

The game views a player's license (ranks, ranked record) by downloading their `licence.dat` from Xbox LIVE title
storage (per-user title storage, by the owner's XUID); each game writes its own. A server whose WELCOME has flag bit
4 keeps each player's last copy (its database: `player_files`, by Steam ID and item): the game sends it after a login
and whenever it saves it (FILE_PUT, with the XUID the connection uses - another one is claimed as statistics frames
claim it, and refused the same way), and asks for another player's by the XUID their game uses (FILE_GET; the player
is picked as for XUID_NAMES: checked Steam IDs first, then the most recently seen). Only `licence.dat` is kept and
given out (a player's `penalty.dat` stays their own); any other item is dropped / answered "no". The bit needs the
statistics (bit 2): XUIDs are recorded there. Without it the game asks the games it is connected to instead.

Limits: FILE_PUT 1 every 5 s (burst 6), beyond that dropped; FILE_GET 5 a second (burst 20), beyond that answered 2.
Malformed (a file of 0 or more than 16384 bytes, bytes missing or left over): error 8.

## Ranks

Whose ranks count is the server owner's choice (`ranks` in server.toml). A player's ranks live in their license
(`licence.dat`: each character's rank and points, the highest rank, Battle Points, the online record) and in
`penalty.dat` (a ranked match left early, charged at the next visit).

- `ranks = "server"` (the default): the server's own. Its WELCOME has flag bit 5 (with bit 4). The game answers
  OWN_RANKS right after WELCOME and from then on keeps the player's license and penalties on the server, not on the
  player's PC: it reads them with FILE_GET owner 0 at each ONLINE MODE visit (and again when the player switches to
  this server inside ONLINE MODE), writes them with FILE_PUT and deletes them with FILE_DEL. The server keeps them
  apart from the copies other players view (database item `server/licence.dat`, `server/penalty.dat`; FILE_GET of
  another player's `licence.dat` answers their `server/licence.dat`). A player new to the server starts as a beginner
  there: their game makes a beginner's license, never one from their own save. A game that did not send OWN_RANKS (an
  older version) still plays here, but its FILE_PUTs are dropped and its ranked matches do not count (ARB_REGISTER is
  refused). Wiping a player's statistics (`ttt2-server admin wipe`) also deletes these files: they start again.
- `ranks = "player"`: each player's own. No bit 5; OWN_RANKS / FILE_DEL close the connection (error 8). The game keeps
  the license and penalties on the player's PC as on Steam, and gives the server a copy for others to view (FILE_PUT).

The player's PC keeps none of a server's ranks. The game also mirrors the license's fields in its own save: it copies a
license it reads into the save, builds the license it stores from the save, and builds one from the save when none is
found. While a server's own ranks are in use, the game puts the player's own fields back into every save it writes
(the save on the PC keeps their own ranks; the server's apply only while they play there), and their `licence.dat`
is not touched. Switching back to Steam or a `"player"` server reads the PC's license again.

Trust: a game sends its license itself. A modified game could send a made-up one; the server does not check the
license against the ranked results it records.

## UDP: the server list's status query

`u8 kind` first; nothing else is ever sent over UDP.

| Kind | Name | Direction | Payload |
|---|---|---|---|
| 1 | INFO_REQ | C->S | u32 magic `TTSI`, u64 nonce, zero padding to at least 128 bytes |
| 2 | INFO | S->C | u32 magic, u64 nonce, u16 protocol version, u8 flags (bit 0 password required, bit 1 Steam IDs checked, bit 2 the ranks are this server's own - "Ranks"), str name, str region, u16 players online, u16 max players, u16 players in a match, u16 open sessions, str message of the day (cut so the reply is at most 3 x the request), str accepted game protocols (comma separated) |

INFO is for the server list (the answer time is the ping; also broadcast on the LAN). "Players in a match" is
the players in open sessions (hosts and who joined them) - the server sees no match traffic (the admin's `status`
also counts the players in open recorded matches, "Match history"). It is unauthenticated and
shown only as information; requests under 128 bytes get no answer; answers are limited to 10 per second per
address (IPv6: /64) and 500 per second in total, built on a worker of their own and cached for 250 ms.

INFO says nothing about any player: whatever a request carries in its padding is ignored, and every request of the
minimum size is answered from the cache - an anonymous question about a player would let anyone learn who is banned,
and cost the server work per request. Whether a player is banned is told only to that player, at its login (error 6,
after the Steam check).

## Scale

One server is meant for thousands of players at once. Measured (`go test ./internal/hub -run TestScale` with
`HUB_SCALE_PLAYERS`, one process, loopback, Windows desktop): 10000 players log in at about 4000 per second (about
20 KB of memory each); all 10000 then list sessions and ask GIN at the same moment and every answer arrives
within 170 ms. Session lists are built at most every 250 ms and shared by everyone who asks within that window.
The database (`internal/store`, SQLite in WAL mode) takes about 12000 player writes per second.
