package config

import (
	"bufio"
	"strings"
	"testing"
)

func parse(t *testing.T, text string) (Config, error) {
	t.Helper()
	return Parse(bufio.NewScanner(strings.NewReader(text)), "test.toml")
}

func TestDefaultFileParsesToDefault(t *testing.T) {
	c, err := parse(t, DefaultFile)
	if err != nil {
		t.Fatal(err)
	}
	d := Default()
	if c.Name != d.Name || c.Region != d.Region || c.Port != d.Port || c.MaxPlayers != d.MaxPlayers ||
		len(c.AcceptedProtocols) != 1 || c.AcceptedProtocols[0] != "ttt2-port-13-rollback" || c.SteamAppID != 480 ||
		c.KickMinutes != 10 || d.KickMinutes != 10 || c.KeepMatchesDays != 365 || d.KeepMatchesDays != 365 {
		t.Fatalf("%+v", c)
	}
}

// The admin panel is off unless server.toml turns it on.
func TestAdminPanelDefaults(t *testing.T) {
	c, err := parse(t, DefaultFile)
	if err != nil {
		t.Fatal(err)
	}
	for _, x := range []Config{c, Default()} {
		if x.AdminPanel || x.AdminPanelPort != 28201 || x.AdminPanelBind != "0.0.0.0" || len(x.AdminPanelAllow) != 0 ||
			x.AdminPanelCert != "" || x.AdminPanelKey != "" {
			t.Fatalf("%+v", x)
		}
	}
	if !strings.Contains(DefaultFile, "\nadmin_panel = false\n") {
		t.Fatal("the example configuration shows the panel off")
	}
}

func TestAdminPanelValues(t *testing.T) {
	c, err := parse(t, `
admin_panel = true
admin_panel_port = 8443
admin_panel_bind = ""
admin_panel_allow = ["203.0.113.7", "198.51.100.9/24", "2001:db8::/48", "::ffff:192.0.2.1"]
admin_panel_cert = "fullchain.pem"
admin_panel_key = "/etc/ttt2/privkey.pem"
`)
	if err != nil {
		t.Fatal(err)
	}
	if !c.AdminPanel || c.AdminPanelPort != 8443 || c.AdminPanelBind != "" || len(c.AdminPanelAllow) != 4 ||
		c.AdminPanelCert != "fullchain.pem" || c.AdminPanelKey != "/etc/ttt2/privkey.pem" {
		t.Fatalf("%+v", c)
	}
	nets, err := ParseAllow(c.AdminPanelAllow)
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, n := range nets {
		got = append(got, n.String())
	}
	if strings.Join(got, " ") != "203.0.113.7/32 198.51.100.0/24 2001:db8::/48 192.0.2.1/32" {
		t.Fatalf("%v", got)
	}
	// the panel's port is free to be the game's while the panel is off
	if _, err := parse(t, "admin_panel_port = 28200\n"); err != nil {
		t.Fatal(err)
	}
}

func TestKickAndMatchKeeping(t *testing.T) {
	c, err := parse(t, "kick_minutes = 0\nkeep_matches_days = 0\n")
	if err != nil || c.KickMinutes != 0 || c.KeepMatchesDays != 0 {
		t.Fatalf("%+v %v", c, err)
	}
	if c, err = parse(t, "kick_minutes = 1440\nkeep_matches_days = 30\n"); err != nil || c.KickMinutes != 1440 || c.KeepMatchesDays != 30 {
		t.Fatalf("%+v %v", c, err)
	}
}

func TestRanks(t *testing.T) {
	if Default().Ranks != RanksServer {
		t.Fatal(`a server's ranks are its own unless the owner says "player"`)
	}
	for text, want := range map[string]string{"": RanksServer, `ranks = "server"`: RanksServer, `ranks = "player"`: RanksPlayer} {
		if c, err := parse(t, text); err != nil || c.Ranks != want {
			t.Fatalf("%q: %+v %v", text, c, err)
		}
	}
}

func TestValues(t *testing.T) {
	c, err := parse(t, `
name = "TEKKEN ARABIA # 1"   # a comment after a value
region = "middle-east"
motd = "Ranked night \"every\" Friday"
accepted_protocols = ["ttt2-port-7-rollback", "ttt2-port-1"]
news = ["Line one", "Line two"]
`)
	if err != nil {
		t.Fatal(err)
	}
	if c.Name != "TEKKEN ARABIA # 1" || c.Region != "middle-east" || c.MOTD != `Ranked night "every" Friday` ||
		len(c.AcceptedProtocols) != 2 || c.AcceptedProtocols[1] != "ttt2-port-1" || len(c.News) != 2 || c.News[1] != "Line two" {
		t.Fatalf("%+v", c)
	}
}

func TestErrors(t *testing.T) {
	for _, text := range []string{
		`region = "mars"`,
		`name = unquoted`,
		`max_players = 1`,
		`colour = "red"`,
		`accepted_protocols = []`,
		`news = ["` + strings.Repeat("x", 120) + `"]`,
		`news = ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"]`,
		`kick_minutes = -1`,
		`kick_minutes = 1441`,
		`keep_matches_days = -5`,
		`keep_matches_days = 36501`,
		`ranks = "players"`,
		`ranks = true`,
		`admin_panel = yes`,
		`admin_panel_port = 0`,
		`admin_panel_port = 65536`,
		"admin_panel = true\nadmin_panel_port = 28200",
		`admin_panel_bind = "localhost"`,
		`admin_panel_allow = ["203.0.113.0/33"]`,
		`admin_panel_allow = ["example.com"]`,
		`admin_panel_allow = ["fe80::1%eth0"]`,
		`admin_panel_allow = "203.0.113.7"`,
		`admin_panel_cert = "fullchain.pem"`,
		`admin_panel_key = "privkey.pem"`,
		`[test]
relay_loss_percent = 3`,
	} {
		if _, err := parse(t, text); err == nil {
			t.Errorf("accepted %q", text)
		}
	}
}

func FuzzParse(f *testing.F) {
	f.Add(DefaultFile)
	f.Add("accepted_protocols = [\"a\", \"b\\\"c\"]\n[test]\nrelay_delay_ms = 5")
	f.Fuzz(func(t *testing.T, text string) {
		c, err := Parse(bufio.NewScanner(strings.NewReader(text)), "fuzz")
		if err == nil {
			if cerr := c.Check(); cerr != nil {
				t.Fatalf("Parse accepted a configuration Check refuses: %v", cerr)
			}
		}
	})
}
