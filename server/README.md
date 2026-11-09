# ttt2-server

A community server for the Tekken Tag Tournament 2 PC port. Players add it in the game's server list (ONLINE
MODE > SERVER); it is where they find each other - it lists their sessions and answers the game's own online
services (news, profiles, rankings) - while the matches themselves run directly between the two players over
Steam P2P, never through the server. Nobody runs a central server: every community hosts its own.

One program, no game data, nothing else to install (its database is built in). Windows and Linux (x86-64 and
ARM64). Ship `THIRD_PARTY_NOTICES.txt` with it.

**New to hosting? [HOSTING.md](HOSTING.md) walks through it step by step** (a Windows PC or a Linux VPS, the port,
the settings that matter, getting players on it). This README is the full reference.

## Run it

1. Put `ttt2-server` (Linux) or `ttt2-server.exe` (Windows) in a folder of its own.
2. Start it once. It writes `server.toml` with the default settings and stops.
3. Edit `server.toml`: the server's name, region, message of the day, and **how players are identified**
   (below).
4. Start it again. On the first real start it creates `data/` with the server's key.
5. Open **TCP and UDP port 28200** (or the `port` you chose) in the firewall and, at home, forward both in the
   router to this PC. (UDP only answers the server list's status query; matches never use the server's ports.)

## Capacity

One server handles thousands of players: `max_players` goes up to 10000. Matches cost the server nothing (they
run over Steam P2P), so what grows with players is logins, session lists and the game's requests. Measured on a
desktop PC: 10000 players logged in at about 4000 per second, about 20 KB of memory each, and all of them
asking for the session list and the game's settings at the same moment were answered within 170 ms. The
database takes about 12000 writes per second. A small VPS (1-2 cores, 1 GB of memory) is plenty for a
few thousand players; bandwidth stays small (a ping every 5 s per player, plus lists and requests while players
browse the menus).

## Admin commands

Type them in the server's console, or - when it runs as a service or in a container, with no console - send them
from a shell on the same machine with `ttt2admin <command>` (Linux service, Windows) or
`ttt2-server [-config <the server's server.toml>] admin <command>` (below). The most used ones are also in a web
page: the [admin panel](#admin-panel) (off by default).

- `status`: players online, in rooms (open sessions), in matches (recorded ones), open sessions, players seen, uptime.
- `players` (online now: Steam ID, name, IP address, how long), `sessions`; `seen <name or steam id>` (when a player
  was here).
- `players all [name part or steam id]`: every player the database has seen, online or not, the most recently seen
  first (at most 200; a part of the name, any case, or a Steam ID finds others): Steam ID, name, first and last
  login, logins, `(unverified)` when Steam did not check the last login, `ONLINE` when connected now, and
  `BANNED until <time>` / `BANNED for good` / `BANNED kicked until <time>` when banned now.
- `kick <name or steam id> [minutes] [reason]`: removes the player and keeps them out for `minutes` -
  `kick_minutes` in server.toml when not given (10 by default; 0 = they can come back at once; at most 1440). The
  game tells them, e.g. "You were removed from this server. You can come back in 10 minutes. Reason: griefing", and
  hides the server from their list until then.
- `ban <name or steam id> [days] [reason]` (no days or 0: for good), `unban <steam id>` (a kick's block too),
  `bans` (the bans in force: Steam ID, the name the server knows, until when, reason - a kick's reason starts with
  `kicked`). The player's game is told until when (only that player, after the Steam check) and leaves the server
  out of its list until then.
- `matches [count | name | steam id]`: the newest matches (20, or `count`), of everyone or of one player (by Steam
  ID, the name they have now or the name they played that match under): when, kind (player / ranked), how long,
  result, the players (name, Steam ID, how many at that PC). A player reports a cheater by the name the game showed:
  `matches <that name>` finds the match and the cheater's Steam ID, then `ban <steam id> [days] [reason]`.
- `board <view> [rows]` (the top of a leaderboard), `wipe <steam id>` (a cheater's leaderboard rows, uploads,
  ghosts and replays - ban them too), `team` (below), `say <text>` (a notice to every player), `reload`
  (server.toml and data/bans.txt again), `backup <file>`, `fingerprint`, `quit` (`help` lists them all).
- `panel-setup [force]`, `panel-password`, `panel-logout-all`, `panel-status`: the admin panel's login and sessions
  ([Admin panel](#admin-panel)).

Names with spaces work (`kick Jin Kazama 30 griefing`). Admin actions are logged ("admin: ...").

### Without a console

The server listens for admin commands on `127.0.0.1` only (a random port) and writes `data/admin.token` (the port
and a random token, new at every start, readable by the server's account only). `ttt2-server admin ...` reads that
file - so run it as the server's user, with the server's configuration - sends one command and prints the answer;
whoever can read the file can run every command, as whoever can type in the console can. Its exit status is 1 when
the server is not running.

For the systemd service in `deploy/` (user `ttt2`, in `/opt/ttt2-server`), install the `ttt2admin` wrapper (in the
Linux archives, and `deploy/` here) once:

```bash
sudo install -m 755 ttt2admin /usr/local/bin/ttt2admin
ttt2admin players
```

It runs `ttt2-server admin` as the user `ttt2` with `/opt/ttt2-server/server.toml` (through sudo; as `ttt2` or root it
works too), so every command is short:

```bash
ttt2admin players all jin
ttt2admin kick <name> 30 griefing
ttt2admin ban <steam id> 7 cheating
ttt2admin bans
ttt2admin matches
```

Without it: `sudo -u ttt2 /opt/ttt2-server/ttt2-server -config /opt/ttt2-server/server.toml admin <command>`
(`-config` comes before `admin`; `admin` alone runs `help`). On Windows `ttt2admin.cmd` (in the zip, next to
`ttt2-server.exe`) does the same with the `server.toml` in its folder: `ttt2admin players`.

Teams: the game only shows them (TEAM in ONLINE MODE: name, level, points, leader, members, best ranks); Namco made
them on its website. Here the admin makes them: `team` (the list), `team create <leader steam id> <name>`,
`team add <team name or number> <steam id>`, `team remove <steam id>` (out of their team; a leader's team passes to
the longest member, the last one out ends it), `team delete <team name or number>`. A player is in one team at most,
a team has at most 100 members.

## Admin panel

A web page for the admin work done most, in a browser on a PC or a phone, over HTTPS on its own TCP port (28201).
**It is off by default.** Its pages update themselves while they are open (below); light and dark follow the system.

- **Dashboard**: players online, playing, rooms open, matches in the last 24 hours, bans in force, uptime; the players
  online - what each is doing (in a match and against whom, hosting a room, or online), for how long, their IP
  address - with Kick and Ban; and the activity: players joining and leaving, matches starting and ending, kicks,
  bans and unbans.
- **Players**: everyone the server has seen - search by a part of the name or a Steam ID, show everyone, those online,
  offline, or banned, 50 a page. A **player's page**: when they were first and last seen, the names they played
  matches under, their recent matches, the kicks and bans the panel gave them (from the audit log), their Steam
  profile, and kick / ban / unban.
- **Matches**: the match history - search by a player's name (then or now) or Steam ID, ranked or not, 50 a page;
  when, who against whom (each links to their page), how long, the result.
- **Bans**: the bans and kicks in force, with the time left, unban; ban any Steam ID (online or not).
- **Audit log**: the newest lines of `data/admin_audit.log` (its last 256 KB, at most 500 lines), searchable.
- **Kick** keeps a player out for a while (`kick_minutes` by default; just remove, 10 minutes, an hour, a day, or a
  number of minutes); **ban** is for 1, 7 or 30 days, any number of days, or for good. The reason is shown to the
  player. Each asks first.

### Live updates

While a panel page is open it keeps a connection to the panel (Server-Sent Events: `GET /events`, plain HTTPS on the
panel's own port and session - no WebSocket, nothing else to open) and shows what changes as it happens; the pill
under the server's name says **Live** (or Reconnecting: the browser reconnects by itself). The panel looks at the
server once a second, and only while a page is open (and a minute after, so moving between pages misses nothing); it
sends something only when something changed. The cost is small next to the game server's own work: about a
microsecond a second when nothing changed (64 players online; 13 microseconds with 1000), about 0.06 ms for a change
with 64 players (0.8 ms with 1000) with 32 pages open, plus one short look at the server's state (under 10
microseconds with 60 players). The activity is what the panel saw while a page was open (the last 100 events); the
audit log and the match history keep everything. At most 4 live pages per login and 32 in all; a page left open stays live until its
session ends (an hour without a click, 12 hours after the login, a logout).

### Turning it on

1. Make its login, on the server: `ttt2admin panel-setup` (Windows: in the server's folder; Docker:
   `docker exec ttt2 /ttt2-server -config /srv/ttt2/server.toml admin panel-setup`). It shows - once, never again,
   never in the log - a random password and an authenticator secret (with an `otpauth://` link holding the same).
   Keep the password in a password manager. (Not in the server's console: its output is the log. Lost the password:
   `ttt2admin panel-password` makes a new one, the authenticator stays; lost the phone: `ttt2admin panel-setup force`.)
2. Add the secret to an authenticator app (Google Authenticator, Microsoft Authenticator, Aegis, 1Password,
   Bitwarden, ...): "enter a setup key", time-based, the secret as shown. Don't paste the `otpauth://` link into a
   website to get a QR code: it is the secret.
3. In `server.toml` set `admin_panel = true`, then `reload` (`systemctl reload ttt2-server`) or restart. The log
   shows where it is and its certificate's fingerprint:
   `admin panel on https://<this server's address>:28201/ (...); its certificate's SHA-256 fingerprint is AB:CD:...`
   (`ttt2admin panel-status` shows it again). A panel that cannot start (no login yet, port taken) says why in the
   log; the game server runs on either way.
4. Open **TCP port 28201** (`admin_panel_port`) in the firewall - on AWS or another cloud, in the security group -
   **for your own IP address only** if you can (e.g. `203.0.113.7/32`), and/or list your addresses in
   `admin_panel_allow`: every other address is then turned away before anything else (no page, no TLS). With an
   address that changes, the panel is still protected by its login, but anyone guessing can pause every login for a
   minute at a time (below).
5. Open `https://<the server's address>:28201/` and log in with the password and the app's current code.

Settings (`server.toml`): `admin_panel` (`false`), `admin_panel_port` (`28201`), `admin_panel_bind` (`"0.0.0.0"`:
every IPv4 address; `""`: every address), `admin_panel_allow` (`[]`: any address; e.g.
`["203.0.113.7", "198.51.100.0/24"]`), `admin_panel_cert` / `admin_panel_key` (`""`: the server's own certificate).
`reload` applies them all (it starts, stops or moves the panel; a new `admin_panel_allow` counts at once).

### The certificate warning

Without a certificate of its own the panel uses the server's self-signed one (the key players' games pin), so the
browser warns ("Your connection is not private", "Potential Security Risk Ahead"). Before going past the warning,
check that it is your server: open the certificate (the warning's details, or the "Not secure" sign next to the
address) and compare its **SHA-256 fingerprint** with the one in the server's log or `ttt2admin panel-status` -
the same 32 pairs of hex digits (some browsers write spaces instead of colons). Equal: continue. Different: something
is in between - don't log in.

### A real certificate (a domain)

With a domain name pointing at the server, a real certificate takes the warning away - e.g. Let's Encrypt
(`sudo certbot certonly --standalone -d panel.example.com`, with TCP port 80 open while it runs). Copy the files where
the server's account can read them and name them in `server.toml` (relative to it, or absolute):

```bash
sudo install -d -o ttt2 -m 700 /opt/ttt2-server/panel
sudo install -o ttt2 -m 600 /etc/letsencrypt/live/panel.example.com/fullchain.pem /etc/letsencrypt/live/panel.example.com/privkey.pem /opt/ttt2-server/panel/
```

```toml
admin_panel_cert = "panel/fullchain.pem"
admin_panel_key = "panel/privkey.pem"
```

`reload` reads the files again: after each renewal, copy them again and `systemctl reload ttt2-server` (a certbot
deploy hook can do both). Then open `https://panel.example.com:28201/`. With a real certificate the panel also tells
browsers to reach that name over HTTPS only (HSTS).

### How it is protected

- Off by default. `admin_panel_allow` comes first: other addresses are closed before TLS. TLS 1.2 at least (1.3 when
  the browser has it), HTTPS only; at most 256 connections, 16 per address; short timeouts, small requests.
- The login is not in `server.toml`: `data/admin_panel.json` (the server's account only) keeps the password's hash
  (PBKDF2-HMAC-SHA256, 600000 iterations, a random salt) and the authenticator secret. Logging in takes the password
  and a current 6-digit code (RFC 6238: 30-second steps, one step of clock difference either way); a code works once.
- Guessing: 5 failed logins from one address (IPv6: one /64) in 15 minutes lock it out for 15 minutes; 30 failed
  logins in a minute from anywhere pause every login for a minute; every refused login is answered a second late.
  Failures are logged with the address, never with what was tried.
- Sessions: a random cookie (`__Host-ttt2admin`: Secure, HttpOnly, SameSite=Strict) that ends 12 hours after the
  login or after an hour unused; at most 20 (a 21st login ends the one used least recently). Log out on the page, or
  end them all with `ttt2admin panel-logout-all`; `panel-password` and `panel-setup force` end them all too.
- Every change is a form POST that must come from the panel's own page (its Origin) with the session's CSRF token;
  looking at a page never changes anything. Everything players send (names, reasons) is escaped, in the pages and in
  the live updates. One script, the panel's own (`/panel.js`, built into the server): the Content-Security-Policy
  allows no other script, nothing inline, nothing from another site (no fonts, images or libraries from the
  internet), and no HTML written from text (Trusted Types); every page and action also works with JavaScript off.
  Strict security headers (Content-Security-Policy, no framing, no caching, no referrer to other sites).
- The live updates need the session like the pages, never keep it alive, and end with it.
- Every login, failed login, lockout and action is in the log and in `data/admin_audit.log`
  (`panel: admin from 203.0.113.7 banned 76561198000000001 7 days: cheating`).
- Reach it directly, not through a reverse proxy: its limits and its allowlist go by the connection's address.

## Identifying players

Players are identified by their Steam account. Choose one:

- **Checked (public servers).** Get a Steam Web API key at <https://steamcommunity.com/dev/apikey> and put it
  in `steam_web_api_key`. The server asks Steam to confirm every login. Keep the key secret: never share
  `server.toml`; on Linux, `chmod 600 server.toml`.
- **Trusted (LAN / friends only).** Set `trust_unverified_steam_ids = true`. The server believes the Steam ID
  each game sends, so anyone could pretend to be someone else - and write that player's offline records or play
  their ranked matches. The game's server list shows such servers as unverified.

The server will not start until you choose.

## Ranks

`ranks` in `server.toml` decides whose ranks count on your server:

- **`"server"` (the default): the server's own.** Every player starts here as a beginner (every fighter Beginner, no
  Battle Points) and ranks up by playing ranked matches here. The server keeps each player's license - their rank and
  points per fighter, highest rank, Battle Points, online record - and the penalty for leaving a ranked match early.
  A player's saved rank, or an edited save, never counts here. And what they earn here stays here: the save on their
  PC keeps their own ranks, which are the ones that count on Steam and on `"player"` servers. Needs the game 0.2.0 or
  newer; an older game still plays Player Matches here, but its ranked matches do not count.
- **`"player"`: the rank each player brings from their PC**, as on Steam. Their ranked matches here change their own
  saved rank; the server keeps a copy for other players to see. Someone with an edited save brings its rank.

You can change it later (`reload` or a restart; players online then keep the old setting until they connect again).
The two are kept apart in the database: from `"player"` to `"server"` everyone starts as a beginner; back to
`"player"`, the server's ranks stay in the database, unused, and count again if you switch back. `wipe <steam id>`
deletes a player's ranks here too (they start again as a beginner).

## The server key - back it up

`data/tls.key` is your server's identity. Every player's game remembers it the first time they connect and
refuses the server if it ever changes, so nobody can impersonate your server. **Back up `data/`.** If you lose
the key, players have to remove your server from their list and add it again. The `fingerprint` command (and
the start-up log) shows the key's fingerprint; you can publish it so players can compare.

## What the server keeps

- `data/tls.key`, `data/tls.crt`: the server's identity.
- `data/bans.txt`: bans by Steam ID (one per line: steam id, until (unix time, 0 = permanent), reason); a kick's
  block is a timed ban whose reason starts with `kicked`. Bans that have ended leave the file the next time it is
  saved.
- `data/admin.token`: while the server runs, its admin port and token (Admin commands); removed when it stops.
- `data/admin_panel.json`: the admin panel's login - the password's hash and the authenticator secret (Admin panel).
  `data/admin_audit.log`: every admin panel login, failed login, lockout and action, with the address, and the
  panel's admin commands (never a password or secret).
- `data/server.db`: the database (SQLite). Every player seen - Steam ID, profile name, the game's online ID
  (XUID), whether Steam confirmed the Steam ID at the last login, first and last login, number of logins - and
  the game profile the game uploads (its ranked data, at most 64 KB of JSON per player) - and the leaderboards
  (the game's own boards: battle points, rank per fighter, ranked match wins, World Arena, chicken, survival,
  time attack). Ranked results count only when both players' games report the same result. Each player's license
  (Ranks: with `ranks = "server"` the one that counts here, with `"player"` a copy of theirs for others to see) and,
  with `"server"`, their penalties for ranked matches left early. Also what the game
  uploads, as it sent it: its last match reports (50) and offline records (20) per player; ghosts (GHOST BATTLE
  fights other players' ghosts: one per player, dan belt and kind, the newest 300 players' per belt) and replays
  (TEKKEN CHANNEL: one per match, 20 per player, 20000 in all); teams. 30 uploads an hour at most, 8 MB per player,
  `max_upload_storage_mb` (1 GB by default) in all. Every match played here (the `matches` command): when it
  started and ended, Player Match or ranked, its result (ended / unfinished; a ranked match's: both games agreed,
  disagreed, unconfirmed, unreported) and its players (Steam ID, profile name then, players at that PC) - for
  `keep_matches_days` (365 by default; 0 = for ever). A database made by a newer server version is refused, not
  overwritten.
- The log (standard output): connections with Steam ID, profile name and IP address, sessions, admin actions.
  No passwords, keys or tickets are ever logged. Rotate or delete logs as your local law requires.

Nothing else about players is stored.

### Backups

Back up `data/` regularly. While the server runs, copy the database with the `backup <file>` command (a
consistent copy taken live); copying `server.db` itself is only safe while the server is stopped (recent changes
can still sit in `server.db-wal` next to it). To restore, stop the server and put the copy back as
`data/server.db`.

## Security

- Game connections are encrypted (TLS 1.3, TLS 1.2 minimum). Matches never pass through the server: they go
  directly between the two players over Steam P2P (Valve's relays), so the server never sees match traffic or
  players' IP addresses beyond their own connection to it.
- Limits protect the server: connections per IP address or IPv6 /64 (`max_connections_per_ip`), handshake and
  idle timeouts, a one-minute block after five failed logins (password guessing), request rates per player, and
  size limits on everything.
- Invites never move a player on their own: the game will show them in its own Accept / Decline window (until
  that window is built, the game ignores them).
- The public status reply (the server list's player count and ping) is answered at most 10 times a second per
  address and is never larger than 3x the request, so the server cannot be used to amplify attacks.
- The admin commands answer only on the machine itself (loopback, with a token only the server's account can read).
  The web admin panel is off unless you turn it on ([How it is protected](#how-it-is-protected)).

## Hosting it

### Windows

Unzip `ttt2-server-<version>-windows-amd64.zip` into a folder of its own and run `ttt2-server.exe` from a command
prompt in that folder (the console is its admin console). To keep it running without a window, use the Task
Scheduler ("At startup", "Run whether user is logged on or not") or a service wrapper, and give commands from a
command prompt in that folder as the same Windows account: `ttt2admin players` (`ttt2admin.cmd` runs
`ttt2-server.exe admin` with the `server.toml` next to it). Keep the folder private to that account
(`data\admin.token` opens the admin commands, `data\admin_panel.json` holds the panel's login). Windows Firewall asks
to allow it the first time - allow it on the networks players come from (the admin port is loopback only and needs
no rule; the admin panel's port, when you turn it on, only for the addresses you use it from).

### Linux service (systemd)

`ttt2-server.service` (in the Linux archives, and `deploy/` here) runs the server as its own user with a locked-down
sandbox; its header lists the install commands. `systemctl reload ttt2-server` (SIGHUP) applies `server.toml` again
- name, region, message of the day, news, limits, the admin panel; the port and the Steam settings need a restart.
Logs: `journalctl -u ttt2-server`. Admin commands (`kick`, `ban`, `bans`, `matches`, ...) go through `ttt2admin`
(install it once: `sudo install -m 755 ttt2admin /usr/local/bin/ttt2admin`, then `ttt2admin players`; Admin
commands). Use a port above 1024 (the defaults 28200 and 28201 are fine): it needs no privileges.

### Docker

```bash
docker build -t ttt2-server --build-arg VERSION=0.2.0 .
docker run -it --name ttt2 -p 28200:28200/tcp -p 28200:28200/udp -v /srv/ttt2:/srv/ttt2 ttt2-server
```

The image is distroless (no shell) and runs as a non-root user (uid 65532): `/srv/ttt2` must be writable by it
(`chown 65532 /srv/ttt2`). The first start writes `/srv/ttt2/server.toml` and stops; edit it and start again.
`docker attach ttt2` reaches the console (detach with Ctrl+P Ctrl+Q); without it,
`docker exec ttt2 /ttt2-server -config /srv/ttt2/server.toml admin players` (any command). For the admin panel
(`admin_panel = true`), publish its port too: `-p 28201:28201/tcp`.

On Windows, Docker Desktop publishes the ports on the PC itself: players on that PC use `127.0.0.1:28200`. A server
run directly inside WSL (the Linux build) is reached from Windows at the WSL address (`hostname -I` in WSL): WSL
forwards only IPv6 `localhost` (`[::1]`) to it, so `127.0.0.1` does not work there.

### Releases

`tools/release.sh <version>` builds the three archives (Windows x86-64 zip, Linux x86-64 and ARM64 tar.gz - static,
the version stamped in) with HOSTING.md, this README, PROTOCOL.md, THIRD_PARTY_NOTICES.txt and the admin wrapper
(`ttt2admin.cmd`; on Linux `ttt2admin` and the systemd unit), plus SHA256SUMS, into `dist/<version>/`.

## Building from source

Go 1.27 or newer. One dependency, `modernc.org/sqlite` (pinned in `go.mod`, checked against `go.sum`): SQLite
translated to Go, so no C compiler is needed on any platform.

```bash
CGO_ENABLED=0 go build -trimpath -ldflags "-s -w" -o ttt2-server ./cmd/ttt2-server
```

Cross-compile with `GOOS=linux GOARCH=arm64` (or `amd64`, `windows`). `go test ./...` runs the tests
(unit, attack, soak, database); `go test -race ./...` needs cgo. After changing dependencies, regenerate the
notices with `python tools/notices.py > THIRD_PARTY_NOTICES.txt`. Protocol: [PROTOCOL.md](PROTOCOL.md).
