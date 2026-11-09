// Package config reads server.toml: a small TOML subset (comments, [sections], key = value with strings,
// integers, booleans and string arrays) - enough for the server's settings without a third-party parser.
package config

import (
	"bufio"
	"fmt"
	"net"
	"net/netip"
	"os"
	"strconv"
	"strings"
)

// The ranks setting's values.
const (
	RanksServer = "server"
	RanksPlayer = "player"
)

// Regions are the game's own region names (GIN `region`).
var Regions = []string{"world", "north-america", "south-america", "europe", "middle-east", "africa", "oceania", "asia"}

// Config is the server's configuration.
type Config struct {
	Name              string
	Region            string
	MOTD              string
	News              []string // the game's online news: at most 10 lines of 119 characters
	MaxPlayers        int
	Password          string
	Port              int
	BindAddress       string
	DataDir           string
	AcceptedProtocols []string
	SteamWebAPIKey    string
	SteamAppID        int
	// Trust the Steam IDs games claim when there is no Web API key (LAN / friends only); must be set explicitly.
	TrustUnverified bool
	MaxConnsPerIP   int
	// The most the server keeps of what games upload (match reports, records, ghosts, replays), in MB.
	MaxUploadStorageMB int
	// How long a kicked player is kept out (minutes; 0 = they can come back at once).
	KickMinutes int
	// How long the match history is kept (days; 0 = for ever).
	KeepMatchesDays int
	// Whose ranks count here (PROTOCOL.md "Ranks"): RanksServer - this server's own record of each player (a player
	// starts here as a beginner and ranks up by playing here), or RanksPlayer - the rank each player brings from their
	// PC, kept in sync with it.
	Ranks string
	// The web admin panel (internal/panel): HTTPS on its own TCP port, off unless turned on.
	AdminPanel     bool
	AdminPanelPort int
	AdminPanelBind string
	// Addresses and CIDR ranges that may reach the panel (empty: any address).
	AdminPanelAllow []string
	// A real certificate's PEM files (chain, key); empty: the server's own identity certificate (internal/tlsid).
	AdminPanelCert, AdminPanelKey string
}

// Default is the configuration a new server starts with.
func Default() Config {
	return Config{
		Name:               "TTT2 Community Server",
		Region:             "world",
		MOTD:               "Welcome!",
		MaxPlayers:         64,
		Port:               28200,
		DataDir:            "data",
		AcceptedProtocols:  []string{"ttt2-port-13-rollback"},
		SteamAppID:         480,
		MaxConnsPerIP:      8,
		MaxUploadStorageMB: 1024,
		KickMinutes:        10,
		KeepMatchesDays:    365,
		Ranks:              RanksServer,
		AdminPanelPort:     28201,
		AdminPanelBind:     "0.0.0.0",
	}
}

// DefaultFile is the text written when no configuration exists yet.
const DefaultFile = `# ttt2-server configuration. Restart the server (or type "reload") after editing.

# Shown in the game's server list.
name = "TTT2 Community Server"
# One of: world, north-america, south-america, europe, middle-east, africa, oceania, asia
region = "world"
# Message of the day, shown when a player looks at the server.
motd = "Welcome!"
# News for the game's online news: up to 10 lines of at most 119 characters, e.g. ["Ranked night: Friday 21:00"].
news = []
max_players = 64
# Leave empty for a public server.
password = ""

# TCP (the games' connections) and UDP (the server list's player count and ping) port. Open both in the firewall /
# router. Matches do not go through the server: players connect to each other over Steam.
port = 28200
# Listen on one address only (e.g. "127.0.0.1" for tests); empty = every address.
bind_address = ""
# Where the server keeps its data (relative to this file).
data_dir = "data"

# Game versions this server accepts (the port's online protocol strings).
accepted_protocols = ["ttt2-port-13-rollback"]

# Players are identified by their Steam ID, checked with Steam at every login. That needs a Steam Web API key:
# https://steamcommunity.com/dev/apikey (keep it secret; this file should be readable by the server only).
steam_web_api_key = ""
steam_app_id = 480
# Without a key the server does not start - unless you set this to true: then it trusts the Steam ID each
# game claims (anyone could pretend to be someone else). Only for a LAN or a group of friends; the game's
# server list marks such a server as unverified.
trust_unverified_steam_ids = false

# Connections from one IP address (players behind one router share an address).
max_connections_per_ip = 8
# The most the server keeps of what games upload (match reports, offline records, ghosts, replays), in MB.
max_upload_storage_mb = 1024
# A kicked player can come back after this many minutes (kick <name> [minutes] picks another; 0 = at once).
kick_minutes = 10
# Every match played here is kept in the database (the matches command lists them) this many days; 0 = for ever.
keep_matches_days = 365

# Whose ranks count here.
#  "server": this server's own. Every player starts here as a beginner and ranks up by playing ranked matches here;
#            the ranks are kept by the server, so a player's own saved rank (or an edited save) never counts here,
#            and what they earn here never changes their rank on Steam or on other servers.
#  "player": the rank each player brings from their PC. Their ranked matches here change their own saved rank.
ranks = "server"

# The web admin panel: players, kicks, bans and matches in a browser (HTTPS), on its own TCP port. Off by default.
# Before turning it on, make its login: "ttt2admin panel-setup" (or "ttt2-server admin panel-setup") shows a password
# and a code for an authenticator app, once. Then open TCP admin_panel_port in the firewall - for your own address
# only, if you can (or list it below). README.md "Admin panel".
admin_panel = false
admin_panel_port = 28201
# Listen on one address only; "0.0.0.0" = every IPv4 address, "" = every address.
admin_panel_bind = "0.0.0.0"
# The only addresses that may reach the panel, e.g. ["203.0.113.7", "198.51.100.0/24"]; empty = any address.
admin_panel_allow = []
# A real certificate for a domain (PEM files, e.g. Let's Encrypt's fullchain.pem and privkey.pem; relative to this
# file). Empty: the server's own self-signed certificate - the browser warns; compare the fingerprint the log shows.
admin_panel_cert = ""
admin_panel_key = ""
`

// Load reads a configuration file; a missing file is created with DefaultFile first.
func Load(path string) (Config, error) {
	if _, err := os.Stat(path); os.IsNotExist(err) {
		// readable by the server's account only: the file will hold the Steam Web API key
		if err := os.WriteFile(path, []byte(DefaultFile), 0o600); err != nil {
			return Config{}, err
		}
	}
	f, err := os.Open(path)
	if err != nil {
		return Config{}, err
	}
	defer f.Close()
	return Parse(bufio.NewScanner(f), path)
}

// Parse reads configuration lines.
func Parse(s *bufio.Scanner, name string) (Config, error) {
	c := Default()
	section := ""
	for line := 1; s.Scan(); line++ {
		text := strings.TrimSpace(stripComment(s.Text()))
		if text == "" {
			continue
		}
		fail := func(format string, args ...any) (Config, error) {
			return Config{}, fmt.Errorf("%s:%d: %s", name, line, fmt.Sprintf(format, args...))
		}
		if strings.HasPrefix(text, "[") {
			if !strings.HasSuffix(text, "]") {
				return fail("bad section %q", text)
			}
			section = strings.TrimSpace(text[1 : len(text)-1])
			continue
		}
		eq := strings.IndexByte(text, '=')
		if eq < 0 {
			return fail("expected key = value")
		}
		key := strings.TrimSpace(text[:eq])
		value := strings.TrimSpace(text[eq+1:])
		if section != "" {
			key = section + "." + key
		}
		var err error
		switch key {
		case "name":
			c.Name, err = parseString(value)
		case "region":
			c.Region, err = parseString(value)
		case "motd":
			c.MOTD, err = parseString(value)
		case "news":
			c.News, err = parseStrings(value)
		case "max_players":
			c.MaxPlayers, err = parseInt(value)
		case "password":
			c.Password, err = parseString(value)
		case "port":
			c.Port, err = parseInt(value)
		case "bind_address":
			c.BindAddress, err = parseString(value)
		case "data_dir":
			c.DataDir, err = parseString(value)
		case "accepted_protocols":
			c.AcceptedProtocols, err = parseStrings(value)
		case "steam_web_api_key":
			c.SteamWebAPIKey, err = parseString(value)
		case "steam_app_id":
			c.SteamAppID, err = parseInt(value)
		case "trust_unverified_steam_ids":
			c.TrustUnverified, err = parseBool(value)
		case "max_connections_per_ip":
			c.MaxConnsPerIP, err = parseInt(value)
		case "max_upload_storage_mb":
			c.MaxUploadStorageMB, err = parseInt(value)
		case "kick_minutes":
			c.KickMinutes, err = parseInt(value)
		case "keep_matches_days":
			c.KeepMatchesDays, err = parseInt(value)
		case "ranks":
			c.Ranks, err = parseString(value)
		case "admin_panel":
			c.AdminPanel, err = parseBool(value)
		case "admin_panel_port":
			c.AdminPanelPort, err = parseInt(value)
		case "admin_panel_bind":
			c.AdminPanelBind, err = parseString(value)
		case "admin_panel_allow":
			c.AdminPanelAllow, err = parseStrings(value)
		case "admin_panel_cert":
			c.AdminPanelCert, err = parseString(value)
		case "admin_panel_key":
			c.AdminPanelKey, err = parseString(value)
		default:
			return fail("unknown setting %q", key)
		}
		if err != nil {
			return fail("%s: %v", key, err)
		}
	}
	if err := s.Err(); err != nil {
		return Config{}, err
	}
	return c, c.Check()
}

// Check validates the values.
func (c Config) Check() error {
	ok := false
	for _, r := range Regions {
		ok = ok || r == c.Region
	}
	switch {
	case !ok:
		return fmt.Errorf("region %q: use one of %s", c.Region, strings.Join(Regions, ", "))
	case c.Name == "" || len(c.Name) > 64 || !printable(c.Name, false):
		return fmt.Errorf("name: 1 to 64 printable characters")
	case len(c.MOTD) > 1000 || !printable(c.MOTD, true):
		return fmt.Errorf(`motd: at most 1000 printable characters (write \n for a new line)`)
	case len(c.News) > MaxNewsLines:
		return fmt.Errorf("news: at most %d lines", MaxNewsLines)
	case !newsOK(c.News):
		return fmt.Errorf("news: each line at most %d printable characters", MaxNewsLine)
	case len(c.Password) > 128:
		return fmt.Errorf("password: at most 128 characters")
	case c.MaxPlayers < 2 || c.MaxPlayers > 10000:
		return fmt.Errorf("max_players %d: 2..10000", c.MaxPlayers)
	case c.Port < 1 || c.Port > 65535:
		return fmt.Errorf("port %d", c.Port)
	case c.MaxConnsPerIP < 1 || c.MaxConnsPerIP > 1000:
		return fmt.Errorf("max_connections_per_ip %d: 1..1000", c.MaxConnsPerIP)
	case c.MaxUploadStorageMB < 0 || c.MaxUploadStorageMB > 1<<20:
		return fmt.Errorf("max_upload_storage_mb %d: 0..1048576", c.MaxUploadStorageMB)
	case c.KickMinutes < 0 || c.KickMinutes > MaxKickMinutes:
		return fmt.Errorf("kick_minutes %d: 0..%d", c.KickMinutes, MaxKickMinutes)
	case c.KeepMatchesDays < 0 || c.KeepMatchesDays > 36500:
		return fmt.Errorf("keep_matches_days %d: 0..36500", c.KeepMatchesDays)
	case c.Ranks != RanksServer && c.Ranks != RanksPlayer:
		return fmt.Errorf(`ranks %q: "server" (this server's own) or "player" (each player's own)`, c.Ranks)
	case len(c.AcceptedProtocols) == 0:
		return fmt.Errorf("accepted_protocols is empty")
	case c.AdminPanelPort < 1 || c.AdminPanelPort > 65535:
		return fmt.Errorf("admin_panel_port %d", c.AdminPanelPort)
	case c.AdminPanel && c.AdminPanelPort == c.Port:
		return fmt.Errorf("admin_panel_port %d is the game's port: pick another", c.AdminPanelPort)
	case c.AdminPanelBind != "" && net.ParseIP(c.AdminPanelBind) == nil:
		return fmt.Errorf(`admin_panel_bind %q: an IP address ("0.0.0.0": every IPv4 address; "": every address)`, c.AdminPanelBind)
	case (c.AdminPanelCert == "") != (c.AdminPanelKey == ""):
		return fmt.Errorf("admin_panel_cert and admin_panel_key: both files, or neither")
	}
	_, err := ParseAllow(c.AdminPanelAllow)
	return err
}

// ParseAllow reads admin_panel_allow: IP addresses ("203.0.113.7", "2001:db8::7") and CIDR ranges
// ("198.51.100.0/24", "2001:db8::/48"). An IPv4 address written as IPv6 ("::ffff:203.0.113.7") is the IPv4 one.
func ParseAllow(list []string) ([]netip.Prefix, error) {
	var out []netip.Prefix
	for _, s := range list {
		p, err := netip.ParsePrefix(s)
		if err != nil {
			a, aerr := netip.ParseAddr(s)
			if aerr != nil || a.Zone() != "" {
				return nil, fmt.Errorf("admin_panel_allow: %q is not an IP address or a CIDR range", s)
			}
			p = netip.PrefixFrom(a, a.BitLen())
		}
		if a := p.Addr(); a.Is4In6() && p.Bits() >= 96 {
			p = netip.PrefixFrom(a.Unmap(), p.Bits()-96)
		}
		out = append(out, p.Masked())
	}
	return out, nil
}

// MaxKickMinutes: a kick keeps a player out for a day at most (longer is a ban).
const MaxKickMinutes = 1440

// The game keeps at most 10 news lines of 119 characters (its GIN settings schema: 10 x char[120]).
const (
	MaxNewsLines = 10
	MaxNewsLine  = 119
)

func newsOK(lines []string) bool {
	for _, l := range lines {
		if len(l) > MaxNewsLine || !printable(l, false) {
			return false
		}
	}
	return true
}

func printable(s string, newlines bool) bool {
	for _, r := range s {
		if r < 0x20 && !(newlines && r == 0x0A) || r == 0x7F {
			return false
		}
	}
	return true
}

// stripComment removes a # comment that is not inside a string.
func stripComment(s string) string {
	in := false
	for i := 0; i < len(s); i++ {
		switch s[i] {
		case '\\':
			if in {
				i++
			}
		case '"':
			in = !in
		case '#':
			if !in {
				return s[:i]
			}
		}
	}
	return s
}

func parseString(v string) (string, error) {
	if len(v) < 2 || v[0] != '"' || v[len(v)-1] != '"' {
		return "", fmt.Errorf("expected a \"quoted\" string")
	}
	s, err := strconv.Unquote(v)
	if err != nil {
		return "", fmt.Errorf("bad string %s", v)
	}
	return s, nil
}

func parseInt(v string) (int, error) { return strconv.Atoi(v) }

func parseBool(v string) (bool, error) {
	switch v {
	case "true":
		return true, nil
	case "false":
		return false, nil
	}
	return false, fmt.Errorf("expected true or false")
}

// CheckIdentity is the start-up rule for Steam IDs: a Web API key, or an explicit choice to trust them.
func (c Config) CheckIdentity() error {
	if c.SteamWebAPIKey == "" && !c.TrustUnverified {
		return fmt.Errorf("set steam_web_api_key (https://steamcommunity.com/dev/apikey) to check players' Steam IDs, " +
			"or set trust_unverified_steam_ids = true for a LAN / friends server")
	}
	return nil
}

func parseStrings(v string) ([]string, error) {
	if len(v) < 2 || v[0] != '[' || v[len(v)-1] != ']' {
		return nil, fmt.Errorf("expected [\"a\", \"b\"]")
	}
	body := strings.TrimSpace(v[1 : len(v)-1])
	var out []string
	for body != "" {
		if body[0] != '"' {
			return nil, fmt.Errorf("expected a string in the list")
		}
		end := 1
		for end < len(body) && body[end] != '"' {
			if body[end] == '\\' {
				end++
			}
			end++
		}
		if end >= len(body) {
			return nil, fmt.Errorf("unterminated string")
		}
		s, err := parseString(body[:end+1])
		if err != nil {
			return nil, err
		}
		out = append(out, s)
		body = strings.TrimSpace(body[end+1:])
		if body != "" {
			if body[0] != ',' {
				return nil, fmt.Errorf("expected , between strings")
			}
			body = strings.TrimSpace(body[1:])
		}
	}
	return out, nil
}
