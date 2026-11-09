# Hosting a community server, step by step

A community server is where players find each other for Ranked Match, leaderboards, teams, replays and ghosts. It
is one small program (`ttt2-server`, no game data). The fights themselves never go through it: they run directly
between the two players over Steam, so a cheap machine is enough.

This guide gets a server running and players on it. [README.md](README.md) covers every setting and admin command.

## 1. Pick where it runs

| | Your own Windows PC | A rented Linux server (VPS) |
|---|---|---|
| Good for | friends, a small group, trying it out | a public server, open all day |
| Cost | free | a few dollars a month (1 CPU and 1 GB of memory hold thousands of players) |
| Online when | your PC is on and the server is running | always |
| Hard part | opening a port in your router | a few Linux commands |

Either way you need **TCP and UDP port 28200** reachable from the internet. Players only need the game and Steam.

## 2. Download it

From the [latest release](https://github.com/shihabal3amri/tekken-tag-tournament-2-pc/releases/latest):

- Windows: `ttt2-server-<version>-windows-amd64.zip`
- Linux: `ttt2-server-<version>-linux-amd64.tar.gz` (most servers), or `-linux-arm64.tar.gz` (ARM servers)

`SHA256SUMS` in the same release lets you check the download.

## 3a. On a Windows PC

1. Unzip it into a folder of its own, e.g. `C:\ttt2-server`.
2. Open a command prompt in that folder (in File Explorer, type `cmd` in the address bar and press Enter) and run
   `ttt2-server.exe`. The first time it writes `server.toml` and stops.
3. Edit `server.toml` in Notepad (step 4 below), then run `ttt2-server.exe` again. When Windows Firewall asks,
   allow it on **public and private networks**.
4. **Open the port in your router.** In the router's settings (often <http://192.168.1.1> or
   <http://192.168.0.1>), find "Port forwarding" (or "Virtual server" / "NAT") and forward **port 28200, TCP and
   UDP**, to this PC's address (`ipconfig` in a command prompt shows it as "IPv4 Address", e.g. 192.168.1.20).
5. Your server's address is your public IP address (search "what is my IP" in a browser).

The console window is the server: keep it open, and type admin commands into it (`players`, `kick`, `ban`, `help`).
Closing it stops the server.

> If the WAN address your router shows is not your public IP address (often 100.64.x.x to 100.127.x.x), your internet
> provider shares one address between customers (CGNAT) and port forwarding cannot work. Ask the provider for a
> public IP address, or use a VPS.

## 3b. On a Linux server (VPS)

Any provider and any recent Linux (Ubuntu, Debian, ...). Copy the `.tar.gz` to the server, then:

```bash
tar xzf ttt2-server-*-linux-amd64.tar.gz
cd ttt2-server-*-linux-amd64
sudo useradd --system --home /opt/ttt2-server ttt2
sudo mkdir -p /opt/ttt2-server/data
sudo cp ttt2-server /opt/ttt2-server/
sudo -u ttt2 /opt/ttt2-server/ttt2-server -config /opt/ttt2-server/server.toml
```

That last command writes `/opt/ttt2-server/server.toml` and stops. Edit it (step 4 below):

```bash
sudo nano /opt/ttt2-server/server.toml
```

Then start it as a service, which also starts it with the machine:

```bash
sudo chown -R ttt2:ttt2 /opt/ttt2-server
sudo chmod 600 /opt/ttt2-server/server.toml
sudo cp ttt2-server.service /etc/systemd/system/
sudo systemctl enable --now ttt2-server
sudo install -m 755 ttt2admin /usr/local/bin/ttt2admin
```

**Open the port**: TCP and UDP 28200, in the provider's firewall (often called a "security group" or "firewall
rules" in their web console) and, if the machine runs one, in its own (`sudo ufw allow 28200` on Ubuntu with ufw).
The server's address is the VPS's public IP address.

- See what it is doing: `journalctl -u ttt2-server -f`
- Admin commands: `ttt2admin players`, `ttt2admin kick <name> 30 griefing`, `ttt2admin help`
- After editing `server.toml`: `sudo systemctl reload ttt2-server` (a new port or Steam setting needs
  `sudo systemctl restart ttt2-server`)

Docker works too: README.md, "Docker".

## 4. Set it up (`server.toml`)

Most settings can stay as they are. Set these:

```toml
name = "My TTT2 Server"          # shown in the game's server list
region = "europe"                # world, north-america, south-america, europe, middle-east, africa, oceania, asia
motd = "Welcome! Be nice."       # shown when a player looks at the server
```

**How players are identified** - the server will not start until you choose one:

- A public server: get a free **Steam Web API key** at <https://steamcommunity.com/dev/apikey> (any domain name will
  do; Steam does not give keys to limited accounts that never bought anything) and put it in:

  ```toml
  steam_web_api_key = "your key"
  ```

  The server then asks Steam to confirm every player. Keep the key secret: never share `server.toml` or screenshots
  of it.
- Friends only, no key: `trust_unverified_steam_ids = true`. The server believes whoever players say they are, so
  anyone could pretend to be someone else. The game's server list marks such a server as unverified.

**Whose ranks count** (README.md, "Ranks"):

```toml
ranks = "server"   # the default: everyone starts as a beginner here and ranks up by playing here
#ranks = "player"  # players bring the rank saved on their PC (as on Steam) - an edited save brings its rank too
```

With `"server"`, ranks earned on your server stay on your server, and a player's own saved rank stays theirs for
Steam and other servers. Pick `"player"` if you trust your players and want their ranks to carry over.

A private server: set `password = "..."`; players type it when they add the server.

## 5. Get players on it

Give players your server's address: the public IP address (or a domain name pointing to it), with `:port` if you
changed the port from 28200. In the game:

1. `ONLINE MODE` -> `SERVER` -> `ADD SERVER`, type the address (`Ctrl` + `V` pastes).
2. Pick it with `A`. The game remembers it and connects to it each time ONLINE MODE opens.

Players on your own network see it in that list without adding it. On the PC running the server, add `127.0.0.1`.

**Check it works:** the list shows your server with its ping and players, and `Y` shows its region and message. Ask
someone outside your network to add it: if they can, the port is open.

## 6. Keep it running

- **Back up `data/`**, above all `data/tls.key`: it is your server's identity. Players' games remember it, and refuse
  the server if it changes (they would have to remove the server and add it again). `backup <file>` copies the
  database while the server runs.
- **Bad players**: `matches <the name they played under>` finds a match and their Steam ID; `kick <name> [minutes]`,
  `ban <steam id> [days] [reason]`, `unban <steam id>`, `wipe <steam id>` (removes their records - ban them too).
  README.md "Admin panel" sets up the same in a browser.
- **Updating the server**: stop it, replace `ttt2-server` (or `ttt2-server.exe`) with the new one, start it again.
  `server.toml` and `data/` stay as they are. When a game update changes its online version, its release notes say
  what to change in `accepted_protocols`. For game 0.2.0 that is `accepted_protocols = ["ttt2-port-13-rollback"]`:
  the new server only uses it by itself in a new `server.toml`, an old file keeps the line it has, and until you
  change it 0.2.0 players see your server as OTHER VERSION.

## When something goes wrong

| What happens | What to check |
|---|---|
| It stops at once, saying to choose how players are identified | Step 4: set `steam_web_api_key`, or `trust_unverified_steam_ids = true` |
| Players outside your network cannot reach it | The port: TCP and UDP 28200 forwarded in the router (or open in the VPS firewall), Windows Firewall allowed, and no CGNAT (3a) |
| You can reach it at home, others cannot | You are probably using your local address. Others need the public one |
| A player's game refuses the server and says its key changed | `data/tls.key` was lost or replaced: put the backup back. Otherwise every player removes the server and adds it again |
| A player cannot log in ("game version") | Their game is older or newer than `accepted_protocols` allows: they update the game, or you update the server |
| Ranked Match never finds anyone | Both players have to be on this server. Their games also turn away big rank gaps unless both set `RANK LIMITS` to No Restrictions |
| Ranked results do not count | With `ranks = "server"`, players need game 0.2.0 or newer |
