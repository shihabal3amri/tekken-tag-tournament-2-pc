package hub

import (
	"bytes"
	"crypto/sha256"
	"crypto/tls"
	"encoding/binary"
	"errors"
	"io"
	"log"
	"net"
	"sync"
	"testing"
	"time"

	"ttt2-server/internal/config"
	"ttt2-server/internal/record"
	"ttt2-server/internal/tlsid"
	"ttt2-server/internal/wire"
)

const proto = "ttt2-port-13-rollback"

type server struct {
	h   *Hub
	id  tlsid.Identity
	tcp net.Listener
	udp *net.UDPConn
}

func start(t *testing.T, cfg config.Config, v Verifier) *server { return startWith(t, cfg, v, nil) }

func startWith(t *testing.T, cfg config.Config, v Verifier, rec Records) *server {
	t.Helper()
	return startSetup(t, cfg, v, func(h *Hub) {
		if rec != nil {
			h.SetRecords(rec)
		}
	})
}

// startSetup starts a hub after setup(hub) (what has to be set before Serve).
func startSetup(t *testing.T, cfg config.Config, v Verifier, setup func(*Hub)) *server {
	t.Helper()
	id, err := tlsid.LoadOrCreate(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	tcp, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	udp, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	h := New(cfg, id, v, NewBans(""), log.New(io.Discard, "", 0))
	setup(h)
	go h.Serve(tcp, udp)
	t.Cleanup(func() { tcp.Close(); udp.Close() })
	return &server{h, id, tcp, udp}
}

// dialTLS connects as the game does: any certificate, but its key must be the pinned one.
func (s *server) dialTLS(t *testing.T) *tls.Conn {
	t.Helper()
	pin := s.id.Fingerprint
	c, err := tls.Dial("tcp", s.tcp.Addr().String(), &tls.Config{
		InsecureSkipVerify: true,
		MinVersion:         tls.VersionTLS12,
		VerifyConnection: func(cs tls.ConnectionState) error {
			if len(cs.PeerCertificates) == 0 || sha256.Sum256(cs.PeerCertificates[0].RawSubjectPublicKeyInfo) != pin {
				return errors.New("pin mismatch")
			}
			return nil
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	return c
}

type client struct {
	t    *testing.T
	s    *server
	c    net.Conn
	peer uint64
}

func hello(w *wire.Writer) { w.U16(wire.ProtocolVersion); w.Str(proto); w.Str("test") }
func auth(peer uint64, name string, ticket []byte, password string) func(*wire.Writer) {
	return func(w *wire.Writer) { w.U64(peer); w.Str(name); w.Blob(ticket); w.Str(password) }
}

// handshake sends HELLO and AUTH and returns the first frame back.
func (s *server) handshake(t *testing.T, h func(*wire.Writer), a func(*wire.Writer)) (*tls.Conn, uint8, wire.Reader) {
	t.Helper()
	c := s.dialTLS(t)
	var w wire.Writer
	h(&w)
	wire.WriteFrame(c, wire.Hello, w.B)
	w = wire.Writer{}
	a(&w)
	wire.WriteFrame(c, wire.Auth, w.B)
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	typ, payload, err := wire.ReadFrame(c)
	if err != nil {
		t.Fatal(err)
	}
	return c, typ, wire.Reader{B: payload}
}

func (s *server) udpConn(t *testing.T) *net.UDPConn {
	u, err := net.DialUDP("udp", nil, s.udp.LocalAddr().(*net.UDPAddr))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { u.Close() })
	return u
}

// login logs in as the current game does: on a server whose ranks are its own (WELCOME flag bit 5) it says it takes
// its rank from there (OWN_RANKS).
func (s *server) login(t *testing.T, peer uint64, name string) *client {
	t.Helper()
	cl, flags := s.loginOld(t, peer, name)
	if flags&32 != 0 {
		cl.send(wire.OwnRanks, nil)
	}
	return cl
}

// loginOld logs in as a game from before "Ranks" (it never sends OWN_RANKS); the WELCOME flags.
func (s *server) loginOld(t *testing.T, peer uint64, name string) (*client, uint8) {
	t.Helper()
	c, typ, r := s.handshake(t, hello, auth(peer, name, nil, ""))
	if typ != wire.Welcome {
		t.Fatalf("expected WELCOME, got %d", typ)
	}
	if r.U64() != peer {
		t.Fatal("WELCOME peer")
	}
	flags := r.U8()
	if r.Str() == "" || r.Err() != nil {
		t.Fatal("WELCOME: flags, server name, region, message")
	}
	return &client{t: t, s: s, c: c, peer: peer}, flags
}

func (cl *client) send(typ uint8, build func(*wire.Writer)) {
	var w wire.Writer
	if build != nil {
		build(&w)
	}
	if err := wire.WriteFrame(cl.c, typ, w.B); err != nil {
		cl.t.Fatal(err)
	}
}

func (cl *client) next() (uint8, wire.Reader) {
	cl.c.SetReadDeadline(time.Now().Add(3 * time.Second))
	typ, payload, err := wire.ReadFrame(cl.c)
	if err != nil {
		cl.t.Fatal(err)
	}
	return typ, wire.Reader{B: payload}
}

// closedSoon reports whether the server closes the connection within d (frames before the close are skipped).
func closedSoon(c net.Conn, d time.Duration) bool {
	c.SetReadDeadline(time.Now().Add(d))
	for {
		if _, _, err := wire.ReadFrame(c); err != nil {
			var ne net.Error
			return !(errors.As(err, &ne) && ne.Timeout())
		}
	}
}

func makeRecord(id, host uint64) []byte {
	var w wire.Writer
	w.U32(record.Magic)
	w.U64(id)
	w.U64(host)
	w.U64(0x0009000000000001)
	w.U32(0x32F)
	w.U32(2)
	w.U32(0)
	w.U32(1)
	w.U32(0)
	info := make([]byte, 0x3C)
	binary.BigEndian.PutUint64(info[0:], id)
	binary.BigEndian.PutUint64(info[8+0x18:], host)
	w.Raw(info)
	w.U32(0)
	w.U32(0)
	w.U32(0x80000001)
	return w.B
}

func (cl *client) list() [][]byte {
	cl.send(wire.SessionList, func(w *wire.Writer) { w.U32(7); w.U32(20) })
	typ, r := cl.next()
	if typ != wire.ListResult || r.U32() != 7 {
		cl.t.Fatal("LIST_RESULT")
	}
	var out [][]byte
	for n := r.U32(); n > 0; n-- {
		out = append(out, r.Blob())
	}
	return out
}

func (cl *client) get(id uint64) bool {
	cl.send(wire.SessionGet, func(w *wire.Writer) { w.U32(9); w.U64(id) })
	typ, r := cl.next()
	return typ == wire.GetResult && r.U32() == 9 && r.U8() == 1
}

func (cl *client) sync() {
	cl.send(wire.Ping, func(w *wire.Writer) { w.U64(1) })
	if typ, _ := cl.next(); typ != wire.Pong {
		cl.t.Fatal("PONG")
	}
}

func trusting() config.Config {
	c := config.Default()
	c.TrustUnverified = true
	return c
}

func TestSessions(t *testing.T) {
	s := start(t, trusting(), nil)
	a := s.login(t, 0x0110000100000001, "HOST A")
	b := s.login(t, 0x0110000100000002, "GUEST B")

	rec := makeRecord(0x8000000000000AAA, a.peer)
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(rec) })
	a.sync()
	time.Sleep(listCacheTime) // lists come from a snapshot at most this old
	if l := b.list(); len(l) != 1 || !bytes.Equal(l[0], rec) {
		t.Fatalf("B lists %d sessions", len(l))
	}
	if l := a.list(); len(l) != 0 {
		t.Fatal("a host must not list its own session")
	}
	if !b.get(0x8000000000000AAA) {
		t.Fatal("GET_RESULT")
	}
	if st := s.h.Stats(); st.Sessions != 1 || st.InRooms != 1 {
		t.Fatalf("stats %+v: one session, its host in it", st)
	}

	// the host leaves: its session goes, at once from GET and with the next snapshot from lists
	a.c.Close()
	deadline := time.Now().Add(2 * time.Second)
	for b.get(0x8000000000000AAA) || len(b.list()) != 0 {
		if time.Now().After(deadline) {
			t.Fatal("session outlived its host")
		}
		time.Sleep(600 * time.Millisecond) // the list budget is 2 per second
	}
}

func TestRecordAttacks(t *testing.T) {
	s := start(t, trusting(), nil)
	a := s.login(t, 0x0110000100000001, "A")
	b := s.login(t, 0x0110000100000002, "B")
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(makeRecord(0x8000000000000AAA, a.peer)) })
	a.sync()
	// B tries to take over A's session id, then C to publish a record naming A as host
	b.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(makeRecord(0x8000000000000AAA, b.peer)) })
	if typ, _ := b.next(); typ != wire.Error {
		t.Fatal("hijacked session id accepted")
	}
	c := s.login(t, 0x0110000100000003, "C")
	c.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(makeRecord(0x8000000000000CCC, a.peer)) })
	if typ, _ := c.next(); typ != wire.Error {
		t.Fatal("spoofed host accepted")
	}
	if l := s.login(t, 0x0110000100000004, "D").list(); len(l) != 1 {
		t.Fatalf("D lists %d sessions", len(l))
	}
}

func TestInvites(t *testing.T) {
	s := start(t, trusting(), nil)
	a := s.login(t, 0x0110000100000001, "A")
	b := s.login(t, 0x0110000100000002, "B")
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(makeRecord(0x8000000000000AAA, a.peer)) })
	for i := 0; i < 5; i++ {
		a.send(wire.InviteSend, func(w *wire.Writer) { w.U64(b.peer); w.U64(0x8000000000000AAA) })
	}
	a.sync()
	got := 0
	for {
		b.c.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
		typ, _, err := wire.ReadFrame(b.c)
		if err != nil {
			break
		}
		if typ == wire.Invite {
			got++
		}
	}
	if got != 3 {
		t.Fatalf("a player got %d invites in a burst (limit 3 a minute)", got)
	}
}

func TestRefusals(t *testing.T) {
	cfg := trusting()
	cfg.Password = "secret"
	s := start(t, cfg, nil)
	s.h.bans.Add(66, time.Time{}, "cheating")
	expect := func(name string, code uint32, h func(*wire.Writer), a func(*wire.Writer)) {
		t.Helper()
		c, typ, r := s.handshake(t, h, a)
		if typ != wire.Error || r.U32() != code {
			t.Fatalf("%s: got %d", name, typ)
		}
		if !closedSoon(c, 3*time.Second) {
			t.Fatalf("%s: connection left open", name)
		}
		c.Close() // as the game does after an ERROR (a lingering refusal holds a handshake slot until then)
	}
	expect("version", wire.ErrProtocol, func(w *wire.Writer) { w.U16(99); w.Str(proto); w.Str("t") }, auth(1, "P", nil, "secret"))
	expect("game", wire.ErrGameVersion, func(w *wire.Writer) { w.U16(wire.ProtocolVersion); w.Str("ttt2-port-1"); w.Str("t") }, auth(1, "P", nil, "secret"))
	expect("password", wire.ErrPassword, hello, auth(1, "P", nil, "wrong"))
	expect("banned", wire.ErrBanned, hello, auth(66, "P", nil, "secret"))
	// an unverified server never lets a second connection take over an online Steam ID
	if _, typ, _ := s.handshake(t, hello, auth(0x0110000100000007, "FIRST", nil, "secret")); typ != wire.Welcome {
		t.Fatal("FIRST")
	}
	expect("duplicate", wire.ErrDuplicate, hello, auth(0x0110000100000007, "SECOND", nil, "secret"))
}

func TestPasswordGuessingIsBlocked(t *testing.T) {
	cfg := trusting()
	cfg.Password = "secret"
	cfg.MaxConnsPerIP = 50
	s := start(t, cfg, nil)
	for i := 0; i < authFailLimit; i++ {
		c, typ, _ := s.handshake(t, hello, auth(uint64(100+i), "P", nil, "guess"))
		if typ != wire.Error {
			t.Fatal("a wrong password passed")
		}
		closedSoon(c, 3*time.Second)
		c.Close()
	}
	time.Sleep(100 * time.Millisecond)
	raw, err := net.Dial("tcp", s.tcp.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer raw.Close()
	raw.SetReadDeadline(time.Now().Add(time.Second))
	if _, err := raw.Read(make([]byte, 1)); err == nil {
		t.Fatal("the address that guessed was not blocked")
	}
}

type fakeVerifier struct{ id uint64 }

func (f fakeVerifier) Verify(ticket, identity string) (uint64, error) {
	if ticket == "" || identity == "" {
		return 0, errors.New("no ticket")
	}
	return f.id, nil
}

func TestSteamCheck(t *testing.T) {
	s := start(t, config.Default(), fakeVerifier{id: 0x0110000100000042})
	ticket := bytes.Repeat([]byte("ab"), 40)
	if _, typ, r := s.handshake(t, hello, auth(0x0110000100000042, "ME", ticket, "")); typ != wire.Welcome {
		t.Fatal("a ticket of the claimed account must pass")
	} else {
		r.U64()
		r.U32()
		r.Bytes(32)
		if r.U8()&1 == 0 {
			t.Fatal("WELCOME must say the account was checked")
		}
	}
	if _, typ, _ := s.handshake(t, hello, auth(0x0110000100000043, "ME", ticket, "")); typ != wire.Error {
		t.Fatal("a ticket of another account must fail")
	}
	if _, typ, _ := s.handshake(t, hello, auth(0x0110000100000042, "ME", nil, "")); typ != wire.Error {
		t.Fatal("no ticket must fail on a checking server")
	}
	if _, typ, _ := s.handshake(t, hello, auth(0x0110000100000042, "ME", []byte("not hex!"), "")); typ != wire.Error {
		t.Fatal("a malformed ticket must fail before Steam is asked")
	}
}

type seen struct {
	id       uint64
	name     string
	verified bool
}
type fakeRecords struct {
	mu    sync.Mutex
	seen  []seen
	fails bool
}

func (f *fakeRecords) PlayerSeen(id uint64, name string, verified bool, _ time.Time) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.fails {
		return errors.New("disk full")
	}
	f.seen = append(f.seen, seen{id, name, verified})
	return nil
}
func (f *fakeRecords) all() []seen {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]seen(nil), f.seen...)
}

// Logins are recorded (with whether Steam confirmed the ID); refused ones are not, and a failing database does not
// keep a player out.
func TestLoginsRecorded(t *testing.T) {
	rec := &fakeRecords{}
	s := startWith(t, config.Default(), fakeVerifier{id: 0x0110000100000042}, rec)
	ticket := bytes.Repeat([]byte("ab"), 40)
	if _, typ, _ := s.handshake(t, hello, auth(0x0110000100000042, "ME", ticket, "")); typ != wire.Welcome {
		t.Fatal("login refused")
	}
	if _, typ, _ := s.handshake(t, hello, auth(0x0110000100000043, "OTHER", ticket, "")); typ != wire.Error {
		t.Fatal("a ticket of another account must fail")
	}
	var got []seen
	for wait := 0; wait < 200 && len(got) == 0; wait++ {
		time.Sleep(10 * time.Millisecond)
		got = rec.all()
	}
	time.Sleep(50 * time.Millisecond)
	if got = rec.all(); len(got) != 1 || got[0] != (seen{0x0110000100000042, "ME", true}) {
		t.Fatalf("recorded %+v", got)
	}

	failing := &fakeRecords{fails: true}
	s2 := startWith(t, config.Default(), fakeVerifier{id: 0x0110000100000042}, failing)
	if _, typ, _ := s2.handshake(t, hello, auth(0x0110000100000042, "ME", ticket, "")); typ != wire.Welcome {
		t.Fatal("a failing database must not refuse players")
	}
}

func TestHostileConnections(t *testing.T) {
	oldTLS, oldLogin := tlsTimeout, loginTimeout
	tlsTimeout, loginTimeout = 500*time.Millisecond, 500*time.Millisecond
	defer func() { tlsTimeout, loginTimeout = oldTLS, oldLogin }()
	cfg := trusting()
	cfg.MaxConnsPerIP = 3
	s := start(t, cfg, nil)
	addr := s.tcp.Addr().String()

	// plain bytes instead of TLS
	raw, _ := net.Dial("tcp", addr)
	raw.Write([]byte("GET / HTTP/1.0\r\n\r\n"))
	if !closedSoon(raw, 2*time.Second) {
		t.Fatal("a non-TLS client was kept")
	}
	raw.Close()

	// a client that says nothing is dropped after the handshake deadline
	idle, _ := net.Dial("tcp", addr)
	began := time.Now()
	if !closedSoon(idle, 3*time.Second) || time.Since(began) > 2*time.Second {
		t.Fatal("a silent client was kept")
	}
	idle.Close()

	// an oversized frame before login
	c := s.dialTLS(t)
	var head [5]byte
	binary.LittleEndian.PutUint32(head[:], 1<<20)
	head[4] = wire.Hello
	c.Write(head[:])
	if !closedSoon(c, 2*time.Second) {
		t.Fatal("an oversized pre-login frame was accepted")
	}

	// more connections from one address than allowed: the extra one is closed at once
	var held []net.Conn
	for i := 0; i < cfg.MaxConnsPerIP; i++ {
		x, _ := net.Dial("tcp", addr)
		held = append(held, x)
	}
	time.Sleep(100 * time.Millisecond)
	extra, _ := net.Dial("tcp", addr)
	if !closedSoon(extra, time.Second) {
		t.Fatal("the per-address limit was not applied")
	}
	for _, x := range held {
		x.Close()
	}
	time.Sleep(700 * time.Millisecond) // their handshake deadline passes, the slots free up
	s.login(t, 0x0110000100000009, "AFTER")
}

func TestPrefixes(t *testing.T) {
	for in, want := range map[string]string{
		"192.0.2.7":                 "192.0.2.7",
		"::ffff:192.0.2.7":          "192.0.2.7",
		"2001:db8:1:2:aaaa::1":      "2001:db8:1:2::/64",
		"2001:db8:1:2:ffff:ffff::9": "2001:db8:1:2::/64",
	} {
		if got := prefixOf(net.ParseIP(in)); got != want {
			t.Errorf("prefixOf(%s) = %s, want %s", in, got, want)
		}
	}
}

func TestFrameFlood(t *testing.T) {
	s := start(t, trusting(), nil)
	a := s.login(t, 0x0110000100000001, "A")
	for i := 0; i < 200; i++ {
		var w wire.Writer
		w.U64(uint64(i))
		wire.WriteFrame(a.c, wire.Ping, w.B)
	}
	a.c.SetReadDeadline(time.Now().Add(3 * time.Second))
	for {
		typ, payload, err := wire.ReadFrame(a.c)
		if err != nil {
			t.Fatal("closed without an ERROR:", err)
		}
		if typ == wire.Error {
			if binary.LittleEndian.Uint32(payload) != wire.ErrMalformed {
				t.Fatal("wrong error code")
			}
			return
		}
	}
}

func TestCleanName(t *testing.T) {
	for in, want := range map[string]string{
		"TTT2":                    "TTT2",
		"  Guest   B ":            "Guest B",
		"%F%A%h%vEVIL\x00\n":      "FAhvEVIL",
		"ABCDEFGHIJKLMNOPQRSTUVW": "ABCDEFGHIJKLMNO",
		"!!!":                     "PLAYER",
		"Jin Kazama":              "Jin Kazama",
	} {
		if got := CleanName(in); got != want {
			t.Errorf("CleanName(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestInfo(t *testing.T) {
	cfg := trusting()
	cfg.Name = "TEKKEN ARABIA"
	cfg.MOTD = "Ranked night every Friday!"
	s := start(t, cfg, nil)
	s.login(t, 0x0110000100000001, "A")
	u := s.udpConn(t)
	ask := func(size int, nonce uint64) []byte {
		var w wire.Writer
		w.U8(wire.InfoReq)
		w.U32(wire.InfoMagic)
		w.U64(nonce)
		w.Raw(make([]byte, max(0, size-len(w.B))))
		u.Write(w.B)
		buf := make([]byte, 2048)
		u.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		n, err := u.Read(buf)
		if err != nil {
			return nil
		}
		return buf[:n]
	}
	if ask(20, 1) != nil {
		t.Fatal("a short INFO_REQ must get no answer")
	}
	b := ask(wire.InfoReqMinSize, 0x1234)
	r := wire.Reader{B: b}
	if r.U8() != wire.Info || r.U32() != wire.InfoMagic || r.U64() != 0x1234 || r.U16() != wire.ProtocolVersion {
		t.Fatalf("INFO header %x", b)
	}
	flags := r.U8()
	name, region, online, maxp := r.Str(), r.Str(), r.U16(), r.U16()
	r.U16()
	r.U16()
	motd, protos := r.Str(), r.Str()
	if r.Err() != nil || flags != 0 || name != "TEKKEN ARABIA" || region != "world" || online != 1 || maxp != 64 ||
		motd != cfg.MOTD || protos != proto || len(b) > 3*wire.InfoReqMinSize {
		t.Fatalf("INFO %d %q %q %d %d %q %q (%d bytes)", flags, name, region, online, maxp, motd, protos, len(b))
	}
	// the cached reply still echoes each request's nonce
	if b := ask(wire.InfoReqMinSize, 0x5678); binary.LittleEndian.Uint64(b[5:13]) != 0x5678 {
		t.Fatal("cached INFO with a stale nonce")
	}
	// a long message of the day is cut to keep the answer within 3x the request
	cfg.MOTD = string(bytes.Repeat([]byte("é"), 400))
	s.h.SetConfig(cfg)
	time.Sleep(infoCacheTime)
	if b := ask(wire.InfoReqMinSize, 9); len(b) > 3*wire.InfoReqMinSize || len(b) == 0 {
		t.Fatalf("INFO of %d bytes", len(b))
	}
	// one address asking too fast gets at most its burst
	answered := 0
	for i := 0; i < 60; i++ {
		var w wire.Writer
		w.U8(wire.InfoReq)
		w.U32(wire.InfoMagic)
		w.U64(uint64(i))
		w.Raw(make([]byte, wire.InfoReqMinSize-len(w.B)))
		u.Write(w.B)
	}
	buf := make([]byte, 2048)
	for {
		u.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
		if _, err := u.Read(buf); err != nil {
			break
		}
		answered++
	}
	if answered > 2*infoPerPrefix+2 {
		t.Fatalf("%d INFO answers to one address in a burst", answered)
	}
}

func FuzzCleanName(f *testing.F) {
	f.Add("Jin Kazama")
	f.Add("%F%A\x00 é ")
	f.Fuzz(func(t *testing.T, in string) {
		out := CleanName(in)
		if len(out) == 0 || len(out) > 15 || out[0] == ' ' || out[len(out)-1] == ' ' {
			t.Fatalf("CleanName(%q) = %q", in, out)
		}
		for _, r := range out {
			if !(r == ' ' || r >= 'A' && r <= 'Z' || r >= 'a' && r <= 'z' || r >= '0' && r <= '9') {
				t.Fatalf("CleanName(%q) = %q", in, out)
			}
		}
	})
}
