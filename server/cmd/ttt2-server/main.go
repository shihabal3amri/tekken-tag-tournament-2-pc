// ttt2-server: a community server for the Tekken Tag Tournament 2 PC port (port/COMMUNITY_SERVER_PLAN.md).
// Players add it in the game's server list; it lets them find each other (their sessions - the matches themselves
// run over Steam P2P) and keeps their online data. Hosts run it on Windows or Linux; it needs one open TCP + UDP
// port (default 28200).
//
//	ttt2-server [-config server.toml]                     the server
//	ttt2-server [-config server.toml] admin <command...>  one console command in the running server (admin.go)
//
// Console commands: status, players [all], sessions, seen, kick, ban, unban, bans, matches, say, reload, board, wipe,
// team, backup, fingerprint, panel-setup, panel-password, panel-logout-all, panel-status, quit (help). The web admin
// panel (internal/panel, off unless server.toml turns it on) gives the same players, kicks, bans and matches.
package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"ttt2-server/internal/config"
	"ttt2-server/internal/gin"
	"ttt2-server/internal/hub"
	"ttt2-server/internal/panel"
	"ttt2-server/internal/steamauth"
	"ttt2-server/internal/store"
	"ttt2-server/internal/tlsid"
)

var version = "dev"

func main() {
	configPath := flag.String("config", "server.toml", "configuration file (created with defaults when missing)")
	flag.Parse()
	if flag.NArg() > 0 {
		if flag.Arg(0) != "admin" {
			fmt.Fprintln(os.Stderr, "usage: ttt2-server [-config server.toml] [admin <command>]")
			os.Exit(2)
		}
		os.Exit(adminClient(*configPath, flag.Args()[1:], os.Stdout, os.Stderr))
	}
	logger := log.New(os.Stdout, "", log.LstdFlags)
	_, statErr := os.Stat(*configPath)
	cfg, err := config.Load(*configPath)
	if err != nil {
		logger.Fatalf("configuration: %v", err)
	}
	if os.IsNotExist(statErr) {
		logger.Printf("created %s with default settings - edit it, then start the server again", *configPath)
	}
	if err := cfg.CheckIdentity(); err != nil {
		logger.Fatalf("configuration %s: %v", *configPath, err)
	}
	warnPermissions(*configPath, cfg, logger)

	dataDir := dataDirOf(*configPath, cfg)
	if err := os.MkdirAll(dataDir, 0o700); err != nil {
		logger.Fatalf("data folder: %v", err)
	}
	identity, err := tlsid.LoadOrCreate(dataDir)
	if err != nil {
		logger.Fatalf("server identity: %v", err)
	}
	bans, err := hub.LoadBans(filepath.Join(dataDir, "bans.txt"))
	if err != nil {
		logger.Fatalf("bans: %v", err)
	}
	var verifier hub.Verifier
	if cfg.SteamWebAPIKey != "" {
		verifier = steamauth.WebAPI{Key: cfg.SteamWebAPIKey, AppID: cfg.SteamAppID}
	}
	db, err := store.Open(filepath.Join(dataDir, "server.db"))
	if err != nil {
		logger.Fatalf("%v", err)
	}
	h := hub.New(cfg, identity, verifier, bans, logger)
	h.SetRecords(db)
	h.SetStats(db)   // the leaderboards
	h.SetMatches(db) // the match history
	h.SetFiles(db)   // the players' licenses
	db.SetUploadQuota(int64(cfg.MaxUploadStorageMB) << 20)
	// the game's GIN web requests: its message of the day and news from the configuration (reload applies them)
	h.SetGIN(gin.New(db, func() gin.Settings {
		c := h.Config()
		return gin.Settings{MOTD: c.MOTD, News: c.News}
	}, logger))
	pnl := panel.New(h, db, identity, dataDir, filepath.Dir(*configPath), logger)
	cmds := &commands{h: h, db: db, panel: pnl, configPath: *configPath, logger: logger, console: os.Stdout}
	tokenPath := filepath.Join(dataDir, adminTokenFile)
	// Ctrl+C, a service stop, quit: the admin token goes, the match history's last writes land and the database is
	// closed before the process ends
	var stopping sync.Once
	stopServer := func() {
		stopping.Do(func() {
			logger.Printf("stopping")
			os.Remove(tokenPath)
			h.FlushMatches()
			if _, err := db.CloseOpenMatches(time.Now()); err != nil {
				logger.Printf("database: open matches not closed: %v", err)
			}
			db.Close()
			os.Exit(0)
		})
	}
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-stop
		stopServer()
	}()

	listenAddr := net.JoinHostPort(cfg.BindAddress, fmt.Sprint(cfg.Port))
	tcp, err := net.Listen("tcp", listenAddr)
	if err != nil {
		logger.Fatalf("TCP port %d: %v", cfg.Port, err)
	}
	udpAddr, err := net.ResolveUDPAddr("udp", listenAddr)
	if err != nil {
		logger.Fatalf("bind_address %q: %v", cfg.BindAddress, err)
	}
	udp, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		logger.Fatalf("UDP port %d: %v", cfg.Port, err)
	}
	// the game's ports are this server's now (a second server on the same data folder stopped above): the matches a
	// stopped server left open are closed, and the admin port's token is this server's
	if n, err := db.CloseOpenMatches(time.Now()); err != nil {
		logger.Printf("database: open matches not closed: %v", err)
	} else if n > 0 {
		logger.Printf("database: %d matches the last run left open closed (unfinished)", n)
	}
	if err := startAdmin(tokenPath, cmds, stopServer, logger); err != nil {
		logger.Printf("WARNING: admin port: %v (ttt2-server admin will not work; the console still does)", err)
	}
	check := "Steam IDs are checked with Steam"
	if verifier == nil {
		check = "WARNING: Steam IDs are NOT checked (trust_unverified_steam_ids = true): LAN / friends use only"
	}
	logger.Printf("ttt2-server %s: %q (%s), port %d TCP+UDP, up to %d players, accepts %s",
		version, cfg.Name, cfg.Region, cfg.Port, cfg.MaxPlayers, strings.Join(cfg.AcceptedProtocols, ", "))
	logger.Printf("%s", check)
	logger.Printf("ranks: %s", ranksText(cfg.Ranks))
	logger.Printf("server key fingerprint %s, Steam ticket identity %s (players' games remember the key; back up %s - a new key makes every game refuse this server)",
		identity.FingerprintText(), identity.SteamIdentity(), filepath.Join(dataDir, "tls.key"))
	known, _ := db.PlayerCount()
	logger.Printf("database %s (schema %d, %d players seen)", db.Path(), db.Version(), known)
	applyPanel(pnl, cfg, logger.Printf) // the web admin panel, when server.toml turns it on
	// SIGHUP (systemctl reload): server.toml again
	hup := make(chan os.Signal, 1)
	signal.Notify(hup, syscall.SIGHUP)
	go func() {
		for range hup {
			reload(h, db, pnl, *configPath, logger.Printf)
		}
	}()
	go console(cmds, stopServer)
	logger.Fatal(h.Serve(tcp, udp))
}

// dataDirOf is the configuration's data folder (a relative one is relative to the configuration file).
func dataDirOf(configPath string, cfg config.Config) string {
	if filepath.IsAbs(cfg.DataDir) {
		return cfg.DataDir
	}
	return filepath.Join(filepath.Dir(configPath), cfg.DataDir)
}

// warnPermissions: on Linux / macOS a configuration holding a Steam Web API key should be readable by the
// server's account only.
func warnPermissions(path string, cfg config.Config, logger *log.Logger) {
	if runtime.GOOS == "windows" || cfg.SteamWebAPIKey == "" {
		return
	}
	if st, err := os.Stat(path); err == nil && st.Mode().Perm()&0o077 != 0 {
		logger.Printf("WARNING: %s holds the Steam Web API key but other users can read it (chmod 600 %s)", path, path)
	}
}

// applyPanel starts, stops or restarts the web admin panel as the configuration says (at start and at reload); a
// panel that cannot run says why, and the game server runs on. `note` logs (and answers).
func applyPanel(p *panel.Panel, cfg config.Config, note func(format string, args ...any)) {
	if err := p.Apply(cfg); err != nil {
		note("WARNING: admin panel not running: %v", err)
	}
}

// reload applies server.toml and data/bans.txt again (the console's reload, SIGHUP / systemctl reload): name, region,
// MOTD, news, limits, bans, the admin panel (started, stopped or moved; a renewed certificate); the game's port and
// the Steam settings only change with a restart. `note` logs (and answers).
func reload(h *hub.Hub, db *store.Store, pnl *panel.Panel, configPath string, note func(format string, args ...any)) {
	cfg, err := config.Load(configPath)
	if err == nil {
		err = cfg.CheckIdentity()
	}
	if err != nil {
		note("configuration not reloaded: %v", err)
		return
	}
	old := h.Config()
	if cfg.Port != old.Port || cfg.SteamWebAPIKey != old.SteamWebAPIKey || cfg.TrustUnverified != old.TrustUnverified {
		note("port and Steam settings change only with a restart")
	}
	if cfg.Ranks != old.Ranks {
		note("ranks: %s from now on (players online now keep the old setting until they connect again)", ranksText(cfg.Ranks))
	}
	h.SetConfig(cfg)
	db.SetUploadQuota(int64(cfg.MaxUploadStorageMB) << 20)
	applyPanel(pnl, cfg, note)
	if n, err := h.ReloadBans(); err != nil {
		note("bans not reloaded: %v", err)
	} else {
		note("configuration and bans reloaded (%d banned players removed)", n)
	}
}

// ranksText says whose ranks count (config ranks).
func ranksText(ranks string) string {
	if ranks == config.RanksPlayer {
		return "each player's own (the rank on their PC)"
	}
	return "this server's own (players start as beginners here)"
}

// console reads commands from the server's standard input (a service has none: `ttt2-server admin` reaches the same
// commands through the admin port).
func console(c *commands, stop func()) {
	in := bufio.NewScanner(os.Stdin)
	for in.Scan() {
		if c.run(in.Text(), os.Stdout) {
			stop()
		}
	}
}

// commands are the admin commands - typed in the console, or sent by `ttt2-server admin` (admin.go) - each answering
// to the writer it came from.
type commands struct {
	h          *hub.Hub
	db         *store.Store
	panel      *panel.Panel
	configPath string
	logger     *log.Logger
	console    io.Writer // the console's output, which shows the log too
}

const helpText = "status | players [all [name part or steam id]] | sessions | seen <name or steam id> |\n" +
	"kick <name or steam id> [minutes] [reason] | ban <name or steam id> [days] [reason] | unban <steam id> | bans |\n" +
	"matches [count | name | steam id] | say <text> | reload | board [view] [rows] | wipe <steam id> |\n" +
	"team [create|add|remove|delete ...] | backup <file> | fingerprint |\n" +
	"panel-setup [force] | panel-password | panel-logout-all | panel-status | quit"

// run runs one command line with its answer to out; true: it was quit (the caller stops the server).
func (c *commands) run(line string, out io.Writer) (quit bool) {
	cmd, arg, _ := strings.Cut(strings.TrimSpace(line), " ")
	arg = strings.TrimSpace(arg)
	switch strings.ToLower(cmd) {
	case "":
	case "help":
		fmt.Fprintln(out, helpText)
	case "status":
		st := c.h.Stats()
		known, _ := c.db.PlayerCount()
		fmt.Fprintf(out, "%d players online, %d in rooms, %d in matches, %d open sessions, %d players seen, up %s; ranks: %s\n",
			st.Online, st.InRooms, st.InMatches, st.Sessions, known, st.Uptime.Round(time.Second), ranksText(c.h.Config().Ranks))
	case "players":
		if sub, search, _ := strings.Cut(arg, " "); strings.EqualFold(sub, "all") {
			c.allPlayers(out, strings.TrimSpace(search))
			break
		}
		for _, p := range c.h.Players() {
			v := ""
			if !p.Verified {
				v = " (unverified)"
			}
			fmt.Fprintf(out, "%-20d %-16s %-15s %s%s\n", p.Peer, p.Name, p.IP, p.Since, v)
		}
	case "sessions":
		for _, s := range c.h.Sessions() {
			fmt.Fprintf(out, "%016X  host %-16s  %d/%d\n", s.ID, s.Host, s.Filled, s.Max)
		}
	case "seen":
		c.seen(out, arg)
	case "board":
		c.board(out, arg)
	case "team":
		c.teamCommand(out, arg)
	case "wipe":
		c.wipe(out, arg)
	case "backup":
		// a consistent copy of the database while the server runs (copying server.db itself can miss recent changes)
		if arg == "" {
			fmt.Fprintln(out, "backup <new file>")
		} else if err := c.db.Backup(arg); err != nil {
			fmt.Fprintln(out, err)
		} else {
			c.note(out, "database backed up to %s", arg)
		}
	case "kick":
		c.kick(out, arg)
	case "ban":
		c.ban(out, arg)
	case "unban":
		id, err := strconv.ParseUint(arg, 10, 64)
		if err == nil {
			err = c.h.Unban(id)
		}
		if err != nil {
			fmt.Fprintln(out, err)
		} else {
			c.note(out, "admin: unbanned %d", id)
		}
	case "bans":
		c.bans(out)
	case "matches":
		c.matches(out, arg)
	case "say":
		fmt.Fprintf(out, "sent to %d players\n", c.h.Say(arg))
	case "reload":
		reload(c.h, c.db, c.panel, c.configPath, func(format string, args ...any) { c.note(out, format, args...) })
	case "fingerprint":
		fmt.Fprintln(out, c.h.Fingerprint())
	case "panel-setup", "panel-password":
		c.panelLogin(out, strings.ToLower(cmd), arg)
	case "panel-logout-all":
		c.audit(out, "panel: %d sessions ended (admin command)", c.panel.LogoutAll())
	case "panel-status":
		fmt.Fprint(out, c.panel.Status())
	case "quit", "exit":
		return true
	default:
		fmt.Fprintln(out, "unknown command (help lists them)")
	}
	return false
}

// note logs an admin action; an admin connection's answer gets the line too (the console shows the log already).
func (c *commands) note(out io.Writer, format string, args ...any) {
	c.logger.Printf(format, args...)
	if out != c.console {
		fmt.Fprintf(out, format+"\n", args...)
	}
}

// audit is note for the admin panel's events: logged, appended to data/admin_audit.log, and answered.
func (c *commands) audit(out io.Writer, format string, args ...any) {
	c.panel.Audit(format, args...)
	if out != c.console {
		fmt.Fprintf(out, format+"\n", args...)
	}
}

// allPlayers: players all [name part or steam id] - every player the database has seen, online or not, the most
// recently seen first (200 at most), marked ONLINE when connected now and BANNED (until when) when banned now.
func (c *commands) allPlayers(out io.Writer, search string) {
	rows, err := panel.AllPlayers(c.h, c.db, search)
	if err != nil {
		fmt.Fprintln(out, err)
		return
	}
	if len(rows) == 0 {
		fmt.Fprintln(out, "no player found")
	}
	for _, r := range rows {
		marks := ""
		if !r.Verified {
			marks += " (unverified)"
		}
		if r.Online {
			marks += "  ONLINE"
		}
		if r.Banned {
			marks += "  BANNED " + r.BanText()
		}
		fmt.Fprintf(out, "%-20d %-16s first %s, last %s, %d logins%s\n", r.SteamID, r.Name,
			r.FirstSeen.Format(time.DateTime), r.LastSeen.Format(time.DateTime), r.Logins, marks)
	}
	if len(rows) == panel.MaxPlayerRows {
		fmt.Fprintf(out, "(the %d seen last: players all <name part or steam id> finds others)\n", panel.MaxPlayerRows)
	}
}

// panelLogin: panel-setup [force] (the web admin panel's login: a new random password and a new authenticator
// secret) and panel-password (a new password, the same secret). Both end every session. The secrets go to this
// command's answer only, once - never to the log, so never to the console, whose output is the log.
func (c *commands) panelLogin(out io.Writer, cmd, arg string) {
	if out == c.console {
		fmt.Fprintf(out, "%s shows a password: run it from another terminal, as ttt2admin %s (or ttt2-server admin %s) - "+
			"the console's output is the log\n", cmd, cmd, cmd)
		return
	}
	var password, secret string
	var err error
	switch {
	case cmd == "panel-password" && arg == "":
		password, err = c.panel.NewPassword()
	case cmd == "panel-setup" && (arg == "" || strings.EqualFold(arg, "force")):
		password, secret, err = c.panel.Setup(arg != "")
	default:
		fmt.Fprintln(out, "panel-setup [force] | panel-password")
		return
	}
	if err != nil {
		fmt.Fprintln(out, err)
		return
	}
	if secret != "" {
		c.panel.Audit("panel: new login made (admin command); every session ended")
	} else {
		c.panel.Audit("panel: new password made (admin command); every session ended")
	}
	fmt.Fprintf(out, "The admin panel's login - shown only now, keep it safe:\n  user:     admin\n  password: %s\n", password)
	if secret != "" {
		fmt.Fprintf(out, "  authenticator secret: %s\n", secret)
		fmt.Fprintf(out, "Add it to an authenticator app (time-based, 6 digits, 30 s, SHA1): type the secret in as a setup key, or\n"+
			"give the app this link: %s\n", panel.OTPAuthURI(secret))
	}
	// a panel server.toml turns on waited for its login: it starts now
	if cfg := c.h.Config(); cfg.AdminPanel && !c.panel.Running() {
		applyPanel(c.panel, cfg, func(format string, args ...any) { c.note(out, format, args...) })
	}
	fmt.Fprint(out, c.panel.Status())
}

// seen: the players the server has seen by that Steam ID or name.
func (c *commands) seen(out io.Writer, who string) {
	found, err := c.db.FindPlayers(who)
	if err != nil {
		fmt.Fprintln(out, err)
		return
	}
	if len(found) == 0 {
		fmt.Fprintln(out, "never seen")
	}
	for _, p := range found {
		v := ""
		if !p.Verified {
			v = " (unverified)"
		}
		fmt.Fprintf(out, "%-20d %-16s first %s, last %s, %d logins%s\n", p.SteamID, p.Name,
			p.FirstSeen.Format(time.DateTime), p.LastSeen.Format(time.DateTime), p.Logins, v)
	}
}

// board: the top of a leaderboard (the game's own view numbers).
func (c *commands) board(out io.Writer, arg string) {
	fields := strings.Fields(arg)
	if len(fields) == 0 {
		fmt.Fprintln(out, "board <view> [rows]: 9 battle points, 14 rank (all fighters; 15-73 one fighter each), "+
			"77 ranked match wins, 152 world arena, 228 chicken, 230 survival, 234 time attack")
		return
	}
	view, err := strconv.ParseUint(fields[0], 10, 32)
	rows := 10
	if len(fields) > 1 {
		rows, _ = strconv.Atoi(fields[1])
	}
	if err != nil {
		fmt.Fprintln(out, "board <view> [rows]")
		return
	}
	total, entries, err := c.db.Board(uint32(view), time.Now(), 1, rows)
	if err != nil {
		fmt.Fprintln(out, err)
		return
	}
	fmt.Fprintf(out, "view %d: %d players\n", view, total)
	for _, e := range entries {
		fmt.Fprintf(out, "%5d  %-16s %-20d rating %d\n", e.Rank, e.Name, e.SteamID, e.Rating)
	}
}

// wipe: a cheater's leaderboard rows, uploads and profile (ban them too, or they come back).
func (c *commands) wipe(out io.Writer, arg string) {
	id, err := strconv.ParseUint(arg, 10, 64)
	if err != nil || id == 0 {
		fmt.Fprintln(out, "wipe <steam id>")
		return
	}
	n, err := c.db.Wipe(id)
	if err != nil {
		fmt.Fprintln(out, err)
		return
	}
	c.note(out, "admin: wiped %d (%d rows)", id, n)
}

// splitWho splits "<name or steam id> [rest]": the longest run of leading words that is an online player's name
// (names can have spaces: "kick Jin Kazama 30 griefing"), else the first word.
func (c *commands) splitWho(arg string) (who, rest string) {
	words := strings.Fields(arg)
	if len(words) == 0 {
		return "", ""
	}
	online := map[string]bool{}
	for _, p := range c.h.Players() {
		online[strings.ToLower(p.Name)] = true
	}
	for n := len(words); n > 1; n-- {
		if name := strings.Join(words[:n], " "); online[strings.ToLower(name)] {
			return name, strings.Join(words[n:], " ")
		}
	}
	return words[0], strings.Join(words[1:], " ")
}

// leadingNumber splits "[number] [text]": the number when the first word is one (ok), and the text after it.
func leadingNumber(rest string) (n int, text string, ok bool) {
	first, more, _ := strings.Cut(rest, " ")
	n, err := strconv.Atoi(first)
	if err != nil {
		return 0, rest, false
	}
	return n, strings.TrimSpace(more), true
}

// kick: kick <name or steam id> [minutes] [reason] - removed, and kept out `minutes` (kick_minutes when not given;
// 0: they can come back at once).
func (c *commands) kick(out io.Writer, arg string) {
	who, rest := c.splitWho(arg)
	if who == "" {
		fmt.Fprintln(out, "kick <name or steam id> [minutes] [reason]")
		return
	}
	minutes, reason, given := leadingNumber(rest)
	if !given {
		minutes = c.h.Config().KickMinutes
	} else if minutes < 0 || minutes > config.MaxKickMinutes {
		fmt.Fprintf(out, "kick: minutes 0..%d (longer: ban <name or steam id> <days>)\n", config.MaxKickMinutes)
		return
	}
	n, err := c.h.Kick(who, minutes, reason)
	if err != nil {
		fmt.Fprintln(out, err)
		return
	}
	fmt.Fprintf(out, "%d kicked\n", n)
}

// ban: ban <name or steam id> [days] [reason] (0 days or none: for good).
func (c *commands) ban(out io.Writer, arg string) {
	who, rest := c.splitWho(arg)
	if who == "" {
		fmt.Fprintln(out, "ban <name or steam id> [days] [reason]")
		return
	}
	days, reason, given := leadingNumber(rest)
	if !given || days < 0 {
		days, reason = 0, rest
	}
	n, err := c.h.Ban(who, days, reason)
	if err != nil {
		fmt.Fprintln(out, err)
		return
	}
	fmt.Fprintf(out, "%d banned\n", n)
}

// bans: the bans in force (kicks' blocks too, their reason starts with "kicked"), with the name the database knows
// for each Steam ID.
func (c *commands) bans(out io.Writer) {
	list := c.h.Bans()
	if len(list) == 0 {
		fmt.Fprintln(out, "nobody is banned")
	}
	for _, b := range list {
		name := "-"
		if found, err := c.db.FindPlayers(strconv.FormatUint(b.SteamID, 10)); err == nil && len(found) > 0 {
			name = found[0].Name
		}
		until := "for good"
		if !b.Until.IsZero() {
			until = "until " + b.Until.Format(time.DateTime)
		}
		fmt.Fprintf(out, "%-20d %-16s %-25s %s\n", b.SteamID, name, until, b.Reason)
	}
}

// matches: matches [count | name | steam id] - the newest matches (20, or `count`; a number up to 1000 is a count),
// of everyone or of one player (by Steam ID, or the name they have now).
func (c *commands) matches(out io.Writer, arg string) {
	who, count := arg, 20
	if n, err := strconv.Atoi(arg); err == nil && n > 0 && n <= 1000 {
		who, count = "", n
	}
	list, err := c.db.RecentMatches(who, count)
	if err != nil {
		fmt.Fprintln(out, err)
		return
	}
	if len(list) == 0 {
		fmt.Fprintln(out, "no matches recorded")
	}
	for _, m := range list {
		took := "open"
		if !m.Ended.IsZero() {
			took = m.Ended.Sub(m.Started).Round(time.Second).String()
		}
		result := m.Result
		if result == "" {
			result = "-"
		}
		var players []string
		for _, p := range m.Players {
			s := fmt.Sprintf("%s (%d) x%d", p.Name, p.SteamID, p.LocalPlayers)
			switch p.Won {
			case 1:
				s += " won"
			case 0:
				s += " lost"
			}
			players = append(players, s)
		}
		fmt.Fprintf(out, "%s  %-6s %-9s %-11s %s\n", m.Started.Format(time.DateTime), m.Kind, took, result, strings.Join(players, ", "))
	}
}

// teamCommand: the server's teams (the game shows them in TEAM; there is no way to make one in the game).
//
//	team                              the teams
//	team create <leader steam id> <name>
//	team add <team name or number> <steam id>
//	team remove <steam id>            (out of their team)
//	team delete <team name or number>
func (c *commands) teamCommand(out io.Writer, arg string) {
	db := c.db
	sub, rest, _ := strings.Cut(arg, " ")
	rest = strings.TrimSpace(rest)
	id := func(text string) uint64 {
		n, err := strconv.ParseUint(strings.TrimSpace(text), 10, 64)
		if err != nil {
			return 0
		}
		return n
	}
	var err error
	switch strings.ToLower(sub) {
	case "":
		teams, e := db.Teams()
		if e != nil {
			fmt.Fprintln(out, e)
			return
		}
		if len(teams) == 0 {
			fmt.Fprintln(out, "no teams (team create <leader steam id> <name>)")
		}
		for _, t := range teams {
			fmt.Fprintf(out, "%4d  %-32s %3d members, leader %d\n", t.ID, t.Name, len(t.Members), t.Leader)
		}
		return
	case "create":
		leader, name, _ := strings.Cut(rest, " ")
		var n int64
		if n, err = db.CreateTeam(name, id(leader), time.Now()); err == nil {
			c.note(out, "admin: team %d %q made, led by %s", n, strings.TrimSpace(name), leader)
		}
	case "add":
		i := strings.LastIndex(rest, " ")
		if i < 0 {
			err = fmt.Errorf("team add <team name or number> <steam id>")
			break
		}
		var team int64
		if team, err = db.TeamID(strings.TrimSpace(rest[:i])); err == nil {
			if err = db.JoinTeam(team, id(rest[i+1:]), time.Now()); err == nil {
				c.note(out, "admin: %s joined team %d", rest[i+1:], team)
			}
		}
	case "remove":
		if err = db.LeaveTeam(id(rest)); err == nil {
			c.note(out, "admin: %s left their team", rest)
		}
	case "delete":
		var team int64
		if team, err = db.TeamID(rest); err == nil {
			if err = db.DeleteTeam(team); err == nil {
				c.note(out, "admin: team %d deleted", team)
			}
		}
	default:
		err = fmt.Errorf("team [create <leader steam id> <name> | add <team> <steam id> | remove <steam id> | delete <team>]")
	}
	if err != nil {
		fmt.Fprintln(out, err)
	}
}
