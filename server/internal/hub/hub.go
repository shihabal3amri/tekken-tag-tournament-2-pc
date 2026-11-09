// Package hub is the server's state and protocol (PROTOCOL.md; security requirements in
// port/COMMUNITY_SERVER_PLAN.md section 7b): connected players (peer id = Steam ID), their hosted sessions, the
// TLS control connections (sessions, GIN) and the public status reply over UDP; matches run over Steam P2P.
package hub

import (
	"crypto/rand"
	"crypto/tls"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"ttt2-server/internal/config"
	"ttt2-server/internal/gin"
	"ttt2-server/internal/record"
	"ttt2-server/internal/tlsid"
	"ttt2-server/internal/wire"
)

// Verifier checks a Steam Web API ticket made for `identity` and returns the Steam ID it belongs to.
type Verifier interface {
	Verify(ticketHex, identity string) (uint64, error)
}

// Records keeps what outlives a restart (internal/store). Called on the player's own connection goroutine,
// never under the hub's lock.
type Records interface {
	PlayerSeen(id uint64, name string, verified bool, at time.Time) error
}

// Timeouts (variables for the tests).
var (
	tlsTimeout   = 5 * time.Second  // the TLS handshake
	loginTimeout = 10 * time.Second // HELLO + AUTH after it (the Steam check has its own)
	idleTimeout  = 30 * time.Second // the game pings every 5 s
)

const (
	maxPending        = 1024 // connections in their handshake, whole server
	maxPendingPrefix  = 4    // ... from one address (IPv6: one /64)
	preAuthFrame      = 16 << 10
	maxFrame          = 256 << 10
	maxQueuedBytes    = 2 << 20 // control frames waiting for a slow game
	maxListBytes      = 256 << 10
	maxNamesAsked     = 16 // XUIDs in one XUID_NAMES
	maxSessionsEach   = 4
	maxListResults    = 64
	sendQueue         = 256
	listCacheTime     = 250 * time.Millisecond // session lists come from a snapshot this old at most
	authFailLimit     = 5                      // failed logins from one prefix ...
	authFailWindow    = time.Minute
	authBlockTime     = time.Minute // ... block it this long
	verifyConcurrency = 16
	verifyPerSecond   = 20
)

// Player is one connected game.
type Player struct {
	Peer     uint64
	Name     string
	Proto    string
	Verified bool
	prefix   string // address (IPv6: /64) for the per-prefix limits
	ip       string
	conn     *tls.Conn
	raw      net.Conn
	failing  atomic.Bool // an ERROR is on its way: input is only drained now
	out      chan []byte // whole frames, written by the player's writer goroutine
	queued   atomic.Int64
	closed   chan struct{}
	once     sync.Once
	since    time.Time

	// rates, used by the player's reader goroutine only
	frames, puts, lists, invites, gins, statWrites, statReadRate, arbs, logs, peerChecks, names bucket
	matchEvents, matchOpens, filePuts, fileGets                                                 bucket
	ownXUID                                                                                     uint64 // the online XUID the player's game reported (statistics), reader goroutine only
	// The ranks are this server's own (config ranks = "server", told in WELCOME: fixed for the connection), and the
	// game said it takes its own license from here (OWN_RANKS; an older game never does) - reader goroutine only.
	serverRanks, ownRanks bool
}

type session struct {
	id          uint64
	host        *Player
	record      []byte
	filled, max uint32
	open        bool              // a public slot is free
	contexts    map[uint32]uint32 // the host's contexts (searches filter on them)
	ranked      bool              // a ranked match (XSESSION_CREATE arbitration): ARB_REGISTER is for these
	created     time.Time         // first published (the lists' order)
	updated     time.Time
}

// listFilter: what a search asks for (SESSION_LIST's optional part).
type listFilter struct {
	contexts map[uint32]uint32 // every one must be the session's value (a session without that context does not match)
	openOnly bool              // only sessions with a free public slot
}

func (f listFilter) match(s *session) bool {
	if f.openOnly && !s.open {
		return false
	}
	for id, want := range f.contexts {
		if have, ok := s.contexts[id]; !ok || have != want {
			return false
		}
	}
	return true
}

type failState struct {
	count        int
	first, until time.Time
}

// Hub is the whole server state.
type Hub struct {
	cfg      atomic.Pointer[config.Config]
	id       tlsid.Identity
	tlsCfg   *tls.Config
	verifier Verifier
	verifies chan struct{} // concurrent Steam checks
	verifyRt bucket        // Steam checks per second (under mu)
	log      *log.Logger
	started  time.Time
	bans     *Bans
	records  Records      // nil: nothing is kept
	gin      *gin.Service // nil: GIN_REQUEST is not answered
	stats    StatsStore   // nil: no statistics (leaderboards)
	// statistics reads in progress, whole server (SetStats): a full one answers "not ok" instead of queueing
	statReads chan struct{}
	ranked    rankedState
	matchDB   MatchStore // nil: matches are not recorded
	matches   matchState
	files     FileStore // nil: the player files (licenses) are not kept

	// mu, then matches.mu, is the only order the two are ever held in
	mu         sync.Mutex
	players    map[uint64]*Player
	sessions   map[uint64]*session
	lists      listCache
	perPrefix  map[string]int
	pendPrefix map[string]int
	pending    int
	fails      map[string]*failState
	inviteRate map[uint64]*bucket // invites received, per target

	udp  *net.UDPConn
	info infoState
}

// listCache: each game version's sessions, newest first, at most a quarter second old. Thousands of players list
// sessions; sorting every session for each request would not scale.
type listCache struct {
	at      time.Time
	byProto map[string][]*session
}

// New makes a hub. A nil verifier means the configuration chose to trust unverified Steam IDs.
func New(cfg config.Config, id tlsid.Identity, verifier Verifier, bans *Bans, logger *log.Logger) *Hub {
	h := &Hub{
		id:         id,
		tlsCfg:     id.ServerConfig(),
		verifier:   verifier,
		verifies:   make(chan struct{}, verifyConcurrency),
		log:        logger,
		started:    time.Now(),
		bans:       bans,
		players:    map[uint64]*Player{},
		sessions:   map[uint64]*session{},
		perPrefix:  map[string]int{},
		pendPrefix: map[string]int{},
		fails:      map[string]*failState{},
		inviteRate: map[uint64]*bucket{},
	}
	if h.bans == nil {
		h.bans = NewBans("")
	}
	h.cfg.Store(&cfg)
	return h
}

// Config is the current configuration.
func (h *Hub) Config() config.Config { return *h.cfg.Load() }

// SetConfig replaces the configuration (admin "reload"); port and Steam key changes need a restart.
func (h *Hub) SetConfig(c config.Config) { h.cfg.Store(&c) }

// SetRecords sets where logins are recorded (before Serve).
func (h *Hub) SetRecords(r Records) { h.records = r }

// SetGIN sets the service answering the game's GIN requests (before Serve).
func (h *Hub) SetGIN(g *gin.Service) { h.gin = g }

// Serve accepts control connections on tcp and serves the UDP socket until the TCP listener is closed.
func (h *Hub) Serve(tcp net.Listener, udp *net.UDPConn) error {
	h.udp = udp
	// room for bursts (many matches at once); the OS may cap it
	udp.SetReadBuffer(8 << 20)
	udp.SetWriteBuffer(8 << 20)
	h.info.start(h)
	go h.udpLoop()
	stop := make(chan struct{})
	defer close(stop)
	go h.rankedSweeper(stop)
	if h.matchDB != nil {
		go h.matchWriter(stop)
		go h.matchSweeper(stop)
	}
	var backoff time.Duration
	for {
		c, err := tcp.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return err
			}
			// out of file handles and the like: wait and keep serving (as net/http does)
			backoff = min(max(2*backoff, 5*time.Millisecond), time.Second)
			h.log.Printf("accept: %v (retrying in %s)", err, backoff)
			time.Sleep(backoff)
			continue
		}
		backoff = 0
		ip, prefix := addressOf(c.RemoteAddr())
		if !h.admit(prefix) {
			c.Close()
			continue
		}
		go h.handle(c, ip, prefix)
	}
}

// addressOf returns the remote IP and the prefix the limits count by: an IPv4 address, or an IPv6 /64 (one
// customer's network).
func addressOf(a net.Addr) (ip, prefix string) {
	var addr net.IP
	switch t := a.(type) {
	case *net.TCPAddr:
		addr = t.IP
	case *net.UDPAddr:
		addr = t.IP
	default:
		host, _, _ := net.SplitHostPort(a.String())
		addr = net.ParseIP(host)
	}
	return addr.String(), prefixOf(addr)
}

func prefixOf(ip net.IP) string {
	if v4 := ip.To4(); v4 != nil {
		return v4.String()
	}
	if len(ip) == net.IPv6len {
		return net.IP(append(append([]byte(nil), ip[:8]...), make([]byte, 8)...)).String() + "/64"
	}
	return ip.String()
}

// admit counts a new connection against its prefix's limits and the pending-handshake limit.
func (h *Hub) admit(prefix string) bool {
	cfg := h.Config()
	now := time.Now()
	h.mu.Lock()
	defer h.mu.Unlock()
	if f := h.fails[prefix]; f != nil && now.Before(f.until) {
		return false
	}
	if h.perPrefix[prefix] >= cfg.MaxConnsPerIP || h.pendPrefix[prefix] >= maxPendingPrefix || h.pending >= maxPending {
		return false
	}
	h.perPrefix[prefix]++
	h.pendPrefix[prefix]++
	h.pending++
	return true
}

func (h *Hub) handshakeDone(prefix string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.pendPrefix[prefix]--; h.pendPrefix[prefix] <= 0 {
		delete(h.pendPrefix, prefix)
	}
	h.pending--
}

func (h *Hub) release(prefix string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.perPrefix[prefix]--; h.perPrefix[prefix] <= 0 {
		delete(h.perPrefix, prefix)
	}
}

// failed counts a refused login (bad password, failed Steam check, malformed login) against its prefix.
func (h *Hub) failed(prefix string) {
	now := time.Now()
	h.mu.Lock()
	defer h.mu.Unlock()
	if len(h.fails) > 100000 {
		for k, f := range h.fails {
			if now.After(f.until) && now.Sub(f.first) > authFailWindow {
				delete(h.fails, k)
			}
		}
	}
	f := h.fails[prefix]
	if f == nil || now.Sub(f.first) > authFailWindow {
		f = &failState{first: now}
		h.fails[prefix] = f
	}
	if f.count++; f.count >= authFailLimit {
		f.until = now.Add(authBlockTime)
		f.count = 0
		f.first = now
		h.log.Printf("too many failed logins from %s: blocked for %s", prefix, authBlockTime)
	}
}

// ---- control connections

// loginError is a refusal; `counts` marks those that count as a failed login.
type loginError struct {
	err    error
	counts bool
}

func (e loginError) Error() string { return e.err.Error() }

func (h *Hub) handle(raw net.Conn, ip, prefix string) {
	defer h.release(prefix)
	pending := true
	defer func() {
		if pending {
			h.handshakeDone(prefix)
		}
	}()
	defer raw.Close()
	raw.SetDeadline(time.Now().Add(tlsTimeout))
	c := tls.Server(raw, h.tlsCfg)
	if err := c.Handshake(); err != nil {
		return // scanners, wrong clients: not worth a log line each
	}
	raw.SetDeadline(time.Now().Add(loginTimeout))
	p, err := h.login(c, raw, ip, prefix)
	if err != nil {
		var le loginError
		if errors.As(err, &le) && le.counts {
			h.failed(prefix)
		}
		h.log.Printf("login from %s refused: %v", ip, err)
		linger(c, raw)
		return
	}
	pending = false
	h.handshakeDone(prefix)
	defer h.leave(p)
	raw.SetDeadline(time.Time{})
	unverified := ""
	if !p.Verified {
		unverified = ", unverified"
	}
	h.log.Printf("player %q (%d%s) joined from %s, %s", p.Name, p.Peer, unverified, ip, p.Proto)
	if h.records != nil {
		if err := h.records.PlayerSeen(p.Peer, p.Name, p.Verified, time.Now()); err != nil {
			h.log.Printf("database: player %q not recorded: %v", p.Name, err)
		}
	}
	for {
		if !p.failing.Load() {
			c.SetReadDeadline(time.Now().Add(idleTimeout))
		}
		typ, payload, err := wire.ReadFrameMax(c, maxFrame)
		if err != nil {
			if !p.failing.Load() && !errors.Is(err, io.EOF) && !errors.Is(err, net.ErrClosed) {
				h.log.Printf("player %q: %v", p.Name, err)
			}
			return
		}
		if p.failing.Load() {
			continue // draining until the game closes or the linger time ends
		}
		if !p.frames.take(40, 80, time.Now()) {
			h.log.Printf("player %q: too many frames", p.Name)
			p.fail(wire.ErrMalformed, "Too many requests.")
			continue
		}
		if err := h.frame(p, typ, payload); err != nil {
			h.log.Printf("player %q: %v", p.Name, err)
			p.fail(wire.ErrMalformed, "The server could not read a request.")
		}
	}
}

// lingerTime bounds how long a closing connection is drained.
const lingerTime = time.Second

// linger ends a connection after its final frame: no more writes (TLS close_notify + TCP FIN), unread input
// discarded for up to lingerTime, so the game receives the frame instead of a reset (closing a socket with
// unread input makes TCP send RST, which can drop data still in flight).
func linger(c *tls.Conn, raw net.Conn) {
	c.SetWriteDeadline(time.Now().Add(lingerTime))
	c.CloseWrite()
	if cw, ok := raw.(interface{ CloseWrite() error }); ok {
		cw.CloseWrite()
	}
	raw.SetReadDeadline(time.Now().Add(lingerTime))
	io.Copy(io.Discard, io.LimitReader(raw, 1<<20))
}

func errorPayload(code uint32, text string) []byte {
	var w wire.Writer
	w.U32(code)
	w.Str(text)
	return w.B
}

// bannedPayload is ERROR 6 for a ban or a kick: the text, then u64 until (unix seconds; 0 = for good; a kick without
// a block: now). Only the player it removes gets it (after the Steam check): its game keeps the server out of its
// server list until then.
func bannedPayload(text string, until, now time.Time) []byte {
	var w wire.Writer
	w.B = errorPayload(wire.ErrBanned, text)
	if until.IsZero() {
		w.U64(0)
	} else {
		w.U64(uint64(max(until.Unix(), now.Unix())))
	}
	return w.B
}

// refuse sends ERROR on a connection that has no Player yet (the caller lingers, then closes).
func refuse(c net.Conn, code uint32, text string, why error, counts bool) error {
	if why == nil {
		why = errors.New(text)
	}
	return refuseWith(c, errorPayload(code, text), why, counts)
}

func refuseWith(c net.Conn, payload []byte, why error, counts bool) error {
	wire.WriteFrame(c, wire.Error, payload)
	return loginError{why, counts}
}

// plausibleTicket: a Steam Web API ticket in hex (they are a few hundred bytes).
func plausibleTicket(t []byte) bool {
	if len(t) < 32 || len(t) > 5120 || len(t)%2 != 0 {
		return false
	}
	for _, c := range t {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F') {
			return false
		}
	}
	return true
}

func (h *Hub) login(c *tls.Conn, raw net.Conn, ip, prefix string) (*Player, error) {
	cfg := h.Config()
	typ, payload, err := wire.ReadFrameMax(c, preAuthFrame)
	if err != nil {
		return nil, err
	}
	if typ != wire.Hello {
		return nil, refuse(c, wire.ErrMalformed, "HELLO expected", nil, true)
	}
	r := wire.Reader{B: payload}
	version, proto, _ := r.U16(), r.Str(), r.Str()
	if r.Err() != nil || version != wire.ProtocolVersion {
		return nil, refuse(c, wire.ErrProtocol, fmt.Sprintf("This server speaks protocol %d.", wire.ProtocolVersion), nil, false)
	}
	accepted := false
	for _, a := range cfg.AcceptedProtocols {
		accepted = accepted || a == proto
	}
	if !accepted {
		return nil, refuse(c, wire.ErrGameVersion, "This server needs game version "+strings.Join(cfg.AcceptedProtocols, " or ")+".",
			fmt.Errorf("game version %q", trim(proto, 40)), false)
	}
	typ, payload, err = wire.ReadFrameMax(c, preAuthFrame)
	if err != nil {
		return nil, err
	}
	if typ != wire.Auth {
		return nil, refuse(c, wire.ErrMalformed, "AUTH expected", nil, true)
	}
	r = wire.Reader{B: payload}
	peer, name, ticket, password := r.U64(), r.Str(), r.Blob(), r.Str()
	if r.Err() != nil || peer == 0 || len(ticket) > 5120 {
		return nil, refuse(c, wire.ErrMalformed, "Malformed login.", nil, true)
	}
	if cfg.Password != "" && !constantTimeEqual(password, cfg.Password) {
		return nil, refuse(c, wire.ErrPassword, "Wrong server password.", nil, true)
	}
	verified := false
	if h.verifier != nil {
		if len(ticket) == 0 {
			return nil, refuse(c, wire.ErrAuth, "This server checks Steam accounts: start Steam and try again.", nil, false)
		}
		if !plausibleTicket(ticket) {
			return nil, refuse(c, wire.ErrAuth, "Steam could not confirm your account.", errors.New("malformed Steam ticket"), true)
		}
		if !h.beginVerify() {
			return nil, refuse(c, wire.ErrAuth, "The server is busy checking accounts. Try again in a moment.", errors.New("Steam checks at their limit"), false)
		}
		raw.SetDeadline(time.Time{}) // the Steam call has its own timeout
		id, err := h.verifier.Verify(string(ticket), h.id.SteamIdentity())
		<-h.verifies
		raw.SetDeadline(time.Now().Add(lingerTime + time.Second))
		if err != nil || id != peer {
			return nil, refuse(c, wire.ErrAuth, "Steam could not confirm your account.",
				fmt.Errorf("Steam check for %d failed: %v (ticket of %d)", peer, err, id), true)
		}
		verified = true
	}
	// Bans only after the Steam check: a ban, its reason and its end are told to the player it bans, never to someone
	// claiming their Steam ID (on a server that does not check Steam IDs anyone can claim any, as everywhere there).
	// A banned player trying again and again counts as failed logins: the address is blocked for a while.
	if e, banned := h.bans.Lookup(peer); banned {
		why := fmt.Errorf("banned %d", peer)
		if e.Kick() {
			why = fmt.Errorf("%d was kicked (kept out until %s)", peer, e.Until.Format(time.DateTime))
		}
		now := time.Now()
		return nil, refuseWith(c, bannedPayload(refusalText(e, now), e.Until, now), why, true)
	}
	p := &Player{
		Peer: peer, Name: CleanName(name), Proto: proto, Verified: verified, ip: ip, prefix: prefix, conn: c, raw: raw,
		out: make(chan []byte, sendQueue), closed: make(chan struct{}), since: time.Now(),
		serverRanks: h.keepsFiles() && cfg.Ranks == config.RanksServer,
	}
	h.mu.Lock()
	old := h.players[peer]
	if old != nil && !verified {
		// Without Steam checks anyone can claim any ID: an online player is never replaced (L3).
		h.mu.Unlock()
		return nil, refuse(c, wire.ErrDuplicate, "This Steam ID is already connected to the server.", nil, false)
	}
	if len(h.players) >= cfg.MaxPlayers && old == nil {
		h.mu.Unlock()
		return nil, refuse(c, wire.ErrFull, "The server is full.", nil, false)
	}
	if old != nil {
		// the same, Steam-checked account again (a reconnect after a dropped connection): the new one wins
		h.removeLocked(old)
	}
	h.players[peer] = p
	h.mu.Unlock()
	if old != nil {
		h.log.Printf("player %q reconnected: closing the old connection", old.Name)
		old.close()
	}
	go p.writer()
	var w wire.Writer
	w.U64(peer)
	flags := uint8(0)
	if verified {
		flags |= 1
	}
	if h.gin != nil {
		flags |= 2 // GIN_REQUEST is answered
	}
	if h.stats != nil {
		flags |= 4 // statistics are kept
	}
	if h.matchDB != nil {
		flags |= 8 // matches are recorded: the game sends MATCH_EVENT
	}
	if h.keepsFiles() {
		flags |= 16 // player files are kept: FILE_PUT, FILE_GET
	}
	if p.serverRanks {
		flags |= 32 // the ranks are this server's own: OWN_RANKS, FILE_DEL ("Ranks")
	}
	w.U8(flags)
	w.Str(cfg.Name)
	w.Str(cfg.Region)
	w.Str(cfg.MOTD)
	p.send(wire.Welcome, w.B)
	return p, nil
}

// beginVerify takes one of the Steam-check slots (concurrency and rate), or reports the server is busy.
func (h *Hub) beginVerify() bool {
	h.mu.Lock()
	ok := h.verifyRt.take(verifyPerSecond, verifyPerSecond, time.Now())
	h.mu.Unlock()
	if !ok {
		return false
	}
	select {
	case h.verifies <- struct{}{}:
		return true
	default:
		return false
	}
}

func suffix(reason string) string {
	if reason == "" {
		return ""
	}
	return " (" + reason + ")"
}

func trim(s string, n int) string {
	if len(s) > n {
		return s[:n]
	}
	return s
}

func constantTimeEqual(a, b string) bool {
	if len(a) != len(b) {
		return false
	}
	var d byte
	for i := 0; i < len(a); i++ {
		d |= a[i] ^ b[i]
	}
	return d == 0
}

// CleanName applies the game's profile-name rules (letters, digits, single spaces, at most 15 characters), so
// no text markup or control characters reach other players' screens.
func CleanName(s string) string {
	var b strings.Builder
	space := false
	for _, r := range s {
		ok := (r >= 'A' && r <= 'Z') || (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9')
		if r == ' ' {
			space = b.Len() > 0
			continue
		}
		if !ok {
			continue
		}
		if b.Len() >= 15 {
			break
		}
		if space && b.Len() < 14 {
			b.WriteByte(' ')
		}
		space = false
		b.WriteRune(r)
	}
	if b.Len() == 0 {
		return "PLAYER"
	}
	return b.String()
}

func (h *Hub) frame(p *Player, typ uint8, payload []byte) error {
	r := wire.Reader{B: payload}
	now := time.Now()
	switch typ {
	case wire.Ping:
		nonce := r.U64()
		if r.Err() != nil {
			return r.Err()
		}
		var w wire.Writer
		w.U64(nonce)
		p.send(wire.Pong, w.B)
	case wire.SessionPut:
		rec := r.Blob()
		if r.Err() != nil || len(r.B) != 0 {
			return errors.New("bad SESSION_PUT")
		}
		if !p.puts.take(4, 8, now) {
			return nil // ignored: the next update carries the same state
		}
		return h.putSession(p, rec)
	case wire.SessionDel:
		id := r.U64()
		if r.Err() != nil {
			return r.Err()
		}
		h.mu.Lock()
		if s := h.sessions[id]; s != nil && s.host == p {
			delete(h.sessions, id)
			h.log.Printf("session %016X of %q removed", id, p.Name)
		}
		h.mu.Unlock()
	case wire.SessionList:
		request, max := r.U32(), r.U32()
		var f listFilter
		if len(r.B) > 0 { // the search's filters (PROTOCOL.md): u8 count + (u32 context, u32 value), u8 flags
			n := r.U8()
			if n > 16 {
				return errors.New("bad SESSION_LIST")
			}
			f.contexts = make(map[uint32]uint32, n)
			for i := uint8(0); i < n; i++ {
				id, value := r.U32(), r.U32()
				f.contexts[id] = value
			}
			f.openOnly = r.U8()&1 != 0
		}
		if r.Err() != nil || len(r.B) != 0 {
			return errors.New("bad SESSION_LIST")
		}
		var list [][]byte
		if p.lists.take(2, 5, now) {
			list = h.listSessions(p, int(min(max, maxListResults)), f)
		}
		var w wire.Writer
		w.U32(request)
		w.U32(uint32(len(list)))
		for _, rec := range list {
			w.Blob(rec)
		}
		p.send(wire.ListResult, w.B)
	case wire.SessionGet:
		request, id := r.U32(), r.U64()
		if r.Err() != nil {
			return r.Err()
		}
		var w wire.Writer
		w.U32(request)
		h.mu.Lock()
		if s := h.sessions[id]; s != nil && s.host.Proto == p.Proto && s.host != p {
			w.U8(1)
			w.Blob(s.record)
		} else {
			w.U8(0)
			w.Blob(nil)
		}
		h.mu.Unlock()
		p.send(wire.GetResult, w.B)
	case wire.GinRequest:
		// one HTTP request of the game's GIN client, answered in order (PROTOCOL.md "GIN")
		request, raw := r.U32(), r.Blob()
		if r.Err() != nil || len(r.B) != 0 || h.gin == nil || len(raw) > gin.MaxRequest {
			return errors.New("bad GIN_REQUEST")
		}
		var w wire.Writer
		w.U32(request)
		if p.gins.take(5, 30, now) {
			caller := gin.Caller{SteamID: p.Peer, Name: p.Name, Verified: p.Verified}
			if h.stats != nil {
				caller.ClaimXUID = func(x uint64) bool { return h.claimXUID(p, x) }
			}
			w.Blob(h.gin.Serve(caller, raw))
		} else {
			w.Blob(gin.Busy())
		}
		p.send(wire.GinResponse, w.B)
	case wire.StatsWrite, wire.StatsBoard, wire.StatsPlayers, wire.ArbRegister:
		if h.stats == nil {
			return errors.New("statistics are not kept here")
		}
		switch typ {
		case wire.StatsWrite:
			if !p.statWrites.take(4, 16, now) {
				h.logPlayer(p, "statistics from %q dropped: too many", p.Name)
				return nil
			}
			h.statsWrite(p, &r, now)
		case wire.StatsBoard:
			return h.statsBoard(p, &r, now)
		case wire.StatsPlayers:
			return h.statsPlayers(p, &r, now)
		case wire.ArbRegister:
			session, own, opponent := r.U64(), r.U64(), r.U64()
			if r.Err() != nil || len(r.B) != 0 {
				return errors.New("bad ARB_REGISTER")
			}
			if !p.arbs.take(1, 4, now) {
				h.logPlayer(p, "ranked registrations from %q dropped: too many", p.Name)
				return nil
			}
			err := errors.New("not the XUID this connection uses")
			if p.serverRanks && !p.ownRanks {
				// an older game plays with the rank on its PC: its ranked results never count here ("Ranks")
				err = errors.New("the game does not take its rank from this server (an older version)")
			} else if h.claimXUID(p, own) {
				err = h.rankedRegister(p, session, opponent, now)
			}
			if err != nil {
				h.logPlayer(p, "ranked match %016X: %q not registered: %v", session, p.Name, err)
			}
		}
	case wire.PeerCheck:
		// a game was reached directly by `peer`: is that a player of this server (same game version)? Games on a
		// server play only with each other - not with Steam players or another server's (PROTOCOL.md).
		request, peer := r.U32(), r.U64()
		if r.Err() != nil || len(r.B) != 0 {
			return errors.New("bad PEER_CHECK")
		}
		answer := uint8(2) // asked too often: ask again
		if p.peerChecks.take(10, 40, now) {
			h.mu.Lock()
			q := h.players[peer]
			here := q != nil && q != p && q.Proto == p.Proto
			h.mu.Unlock()
			answer = 0
			if here {
				answer = 1
			}
		}
		var w wire.Writer
		w.U32(request)
		w.U8(answer)
		p.send(wire.PeerResult, w.B)
	case wire.XuidNames:
		// the names behind online XUIDs, for the game's gamertag lookups (XUserFindUsers: TEKKEN CHANNEL's replays,
		// ghosts): players this server has seen, by the XUID their game reported
		request, count := r.U32(), int(r.U8())
		if r.Err() != nil || count > maxNamesAsked {
			return errors.New("bad XUID_NAMES")
		}
		xuids := make([]uint64, 0, count)
		for i := 0; i < count; i++ {
			xuids = append(xuids, r.U64())
		}
		if r.Err() != nil || len(r.B) != 0 {
			return errors.New("bad XUID_NAMES")
		}
		var found map[uint64]string
		if h.stats != nil && p.names.take(5, 20, now) {
			found, _ = h.stats.NamesOf(xuids) // no answer for a failed lookup: the game shows no name
		}
		var w wire.Writer
		w.U32(request)
		w.U8(uint8(len(found)))
		for x, name := range found {
			w.U64(x)
			w.Str(gamertag(name))
		}
		p.send(wire.NamesResult, w.B)
	case wire.MatchEvent:
		// a match started or ended (the match history, PROTOCOL.md): only sent to a server whose WELCOME says it records
		if h.matchDB == nil {
			return errors.New("matches are not recorded here")
		}
		ev, err := readMatchEvent(&r)
		if err != nil {
			return err
		}
		if !p.matchEvents.take(1, 4, now) {
			h.logPlayer(p, "match events from %q dropped: too many", p.Name)
			return nil
		}
		h.matchEvent(p, ev, now)
	case wire.FilePut, wire.FileGet:
		if !h.keepsFiles() {
			return errors.New("player files are not kept here")
		}
		if typ == wire.FilePut {
			return h.filePut(p, &r, now)
		}
		return h.fileGet(p, &r, now)
	case wire.OwnRanks, wire.FileDel:
		// only to a server whose WELCOME says the ranks are its own (to any other an unknown frame type)
		if !p.serverRanks {
			return errors.New("the ranks are not this server's own")
		}
		if typ == wire.OwnRanks {
			if len(r.B) != 0 {
				return errors.New("bad OWN_RANKS")
			}
			p.ownRanks = true
			return nil
		}
		return h.fileDel(p, &r, now)
	case wire.InviteSend:
		to, id := r.U64(), r.U64()
		if r.Err() != nil {
			return r.Err()
		}
		if !p.invites.take(10.0/60, 10, now) {
			return nil
		}
		h.mu.Lock()
		target, s := h.players[to], h.sessions[id]
		ok := target != nil && target != p && s != nil && s.host == p && target.Proto == p.Proto
		if ok {
			b := h.inviteRate[to]
			if b == nil {
				b = &bucket{}
				h.inviteRate[to] = b
			}
			ok = b.take(3.0/60, 3, now) // a target gets at most 3 invites a minute
		}
		h.mu.Unlock()
		if ok {
			var w wire.Writer
			w.U64(p.Peer)
			w.Blob(s.record)
			target.send(wire.Invite, w.B)
		}
	default:
		return fmt.Errorf("unknown frame type %d", typ)
	}
	return nil
}

func (h *Hub) putSession(p *Player, rec []byte) error {
	info, err := record.Check(rec, p.Peer)
	if err != nil {
		return err
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	if s := h.sessions[info.ID]; s != nil && s.host != p {
		return fmt.Errorf("session %016X belongs to another player", info.ID)
	}
	if h.sessions[info.ID] == nil {
		owned := 0
		for _, s := range h.sessions {
			if s.host == p {
				owned++
			}
		}
		if owned >= maxSessionsEach {
			return errors.New("too many sessions")
		}
		h.log.Printf("session %016X hosted by %q", info.ID, p.Name)
	}
	created := time.Now()
	if old := h.sessions[info.ID]; old != nil {
		created = old.created // republishing (a slot filled) does not move a room to the top of the lists
	}
	h.sessions[info.ID] = &session{id: info.ID, host: p, record: append([]byte(nil), rec...), filled: info.Filled, max: info.Max,
		open: info.OpenPublic, contexts: info.Contexts, ranked: info.Flags&record.Arbitrated != 0, created: created, updated: time.Now()}
	return nil
}

// listSessions: the newest rooms a search asks for, one per host (a player's several rooms cannot crowd the others
// out). The snapshot is filtered outside the hub's lock (sessions are never changed in place, only replaced); the
// picks are checked again under it.
func (h *Hub) listSessions(p *Player, max int, f listFilter) [][]byte {
	now := time.Now()
	h.mu.Lock()
	if h.lists.byProto == nil || now.Sub(h.lists.at) > listCacheTime {
		h.rebuildListsLocked(now)
	}
	snapshot := h.lists.byProto[p.Proto]
	h.mu.Unlock()
	var picks []*session
	hosts := map[*Player]bool{}
	bytes := 0
	for _, s := range snapshot {
		if len(picks) >= max {
			break
		}
		if s.host == p || hosts[s.host] || bytes+len(s.record) > maxListBytes || !f.match(s) {
			continue // its own, that host's already picked, too much, or not asked for
		}
		hosts[s.host] = true
		bytes += len(s.record)
		picks = append(picks, s)
	}
	out := make([][]byte, 0, len(picks))
	h.mu.Lock()
	for _, s := range picks {
		// its current record, if it is still there and still matches (updated since the snapshot: the newer one)
		if cur := h.sessions[s.id]; cur != nil && cur.host == s.host && f.match(cur) {
			out = append(out, cur.record)
		}
	}
	h.mu.Unlock()
	return out
}

// rebuildListsLocked sorts each game version's sessions newest first (h.mu held). All of them: a search filters as it
// goes (a ranked search must find ranked rooms among thousands of others).
func (h *Hub) rebuildListsLocked(now time.Time) {
	byProto := map[string][]*session{}
	for _, s := range h.sessions {
		byProto[s.host.Proto] = append(byProto[s.host.Proto], s)
	}
	for _, list := range byProto {
		sort.Slice(list, func(i, j int) bool { return list[i].created.After(list[j].created) })
	}
	h.lists = listCache{at: now, byProto: byProto}
}

func (h *Hub) leave(p *Player) {
	h.mu.Lock()
	gone := h.players[p.Peer] == p
	if gone {
		h.removeLocked(p)
	}
	h.mu.Unlock()
	p.close()
	if gone {
		h.log.Printf("player %q left (%s online)", p.Name, time.Since(p.since).Round(time.Second))
	}
}

// removeLocked drops a player and their sessions (h.mu held; no network I/O here).
func (h *Hub) removeLocked(p *Player) {
	delete(h.players, p.Peer)
	for id, s := range h.sessions {
		if s.host == p {
			delete(h.sessions, id)
		}
	}
	delete(h.inviteRate, p.Peer)
}

func (p *Player) send(typ uint8, payload []byte) {
	frame := make([]byte, 5+len(payload))
	binary.LittleEndian.PutUint32(frame, uint32(1+len(payload)))
	frame[4] = typ
	copy(frame[5:], payload)
	if p.queued.Add(int64(len(frame))) > maxQueuedBytes {
		p.close() // a game that does not read its control connection is dropped
		return
	}
	select {
	case p.out <- frame:
	case <-p.closed:
	default:
		p.close()
	}
}

// fail sends ERROR; the writer then ends the connection gracefully (linger) and it closes within a second or two.
func (p *Player) fail(code uint32, text string) { p.failWith(errorPayload(code, text)) }

func (p *Player) failWith(payload []byte) {
	if p.failing.Swap(true) {
		return
	}
	p.send(wire.Error, payload)
	time.AfterFunc(2*lingerTime, p.close)
}

func (p *Player) writer() {
	for {
		select {
		case f := <-p.out:
			p.queued.Add(-int64(len(f)))
			p.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
			if _, err := p.conn.Write(f); err != nil {
				p.close()
				return
			}
			if f[4] == wire.Error {
				// the reader keeps draining input until the game closes or the deadline passes
				p.conn.CloseWrite()
				if cw, ok := p.raw.(interface{ CloseWrite() error }); ok {
					cw.CloseWrite()
				}
				p.raw.SetReadDeadline(time.Now().Add(lingerTime))
				return
			}
		case <-p.closed:
			return
		}
	}
}

// close drops the connection at once: the raw socket, so nothing blocks on a TLS close_notify.
func (p *Player) close() {
	p.once.Do(func() {
		close(p.closed)
		p.raw.Close()
	})
}

func randomU32() uint32 {
	var b [4]byte
	rand.Read(b[:])
	return binary.LittleEndian.Uint32(b[:])
}

func randomU64() uint64 {
	var b [8]byte
	rand.Read(b[:])
	return binary.LittleEndian.Uint64(b[:])
}

// ---- admin / status

// Stats is what INFO and the admin console report.
type Stats struct {
	Online, InRooms, Sessions int // InRooms: players in the open sessions (hosts and who joined them)
	InMatches                 int // online players in an open recorded match (MATCH_EVENT; 0 where matches are not recorded)
	Uptime                    time.Duration
}

func (h *Hub) Stats() Stats {
	h.mu.Lock()
	defer h.mu.Unlock()
	s := Stats{Online: len(h.players), Sessions: len(h.sessions), Uptime: time.Since(h.started)}
	for _, ses := range h.sessions {
		s.InRooms += int(min(ses.filled, ses.max))
	}
	s.InMatches = h.matches.playing(func(id uint64) bool { return h.players[id] != nil })
	return s
}

// PlayerInfo is one line of the admin "players" list.
type PlayerInfo struct {
	Peer     uint64
	Name     string
	Verified bool
	IP       string
	Since    time.Duration
}

func (h *Hub) Players() []PlayerInfo {
	h.mu.Lock()
	defer h.mu.Unlock()
	var out []PlayerInfo
	for _, p := range h.players {
		out = append(out, PlayerInfo{p.Peer, p.Name, p.Verified, p.ip, time.Since(p.since).Round(time.Second)})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out
}

// SessionInfo is one line of the admin "sessions" list.
type SessionInfo struct {
	ID          uint64
	Host        string
	Filled, Max uint32
}

func (h *Hub) Sessions() []SessionInfo {
	h.mu.Lock()
	defer h.mu.Unlock()
	var out []SessionInfo
	for _, s := range h.sessions {
		out = append(out, SessionInfo{s.id, s.host.Name, s.filled, s.max})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

// find returns the online players whose name or Steam ID (decimal) is `who`.
func (h *Hub) find(who string) []*Player {
	h.mu.Lock()
	defer h.mu.Unlock()
	var hit []*Player
	for _, p := range h.players {
		if strings.EqualFold(p.Name, who) || fmt.Sprint(p.Peer) == who {
			hit = append(hit, p)
		}
	}
	return hit
}

// Kick removes the online players called `who` (name or Steam ID) and keeps them out for `minutes` (0: they can
// come back at once) - a timed ban whose reason starts with "kicked". It returns how many.
func (h *Hub) Kick(who string, minutes int, reason string) (int, error) {
	hit := h.find(who)
	if len(hit) == 0 {
		return 0, fmt.Errorf("no online player %q", who)
	}
	now := time.Now()
	block := Ban{Reason: kickedReason}
	if reason != "" {
		block.Reason += ": " + reason
	}
	text, until := "You were removed from this server."+reasonText(reason), now // no block: back at once
	if minutes > 0 {
		block.Until = now.Add(time.Duration(minutes) * time.Minute)
		text, until = refusalText(block, now), block.Until
	}
	for _, p := range hit {
		if minutes > 0 {
			if err := h.bans.Block(p.Peer, block.Until, block.Reason); err != nil {
				h.log.Printf("bans not saved: %v", err) // kept in memory: the kick still stands until a restart
			}
		}
		h.log.Printf("admin: kicked %q (%d) for %d minutes%s", p.Name, p.Peer, minutes, suffix(reason))
		p.failWith(bannedPayload(text, until, now))
	}
	return len(hit), nil
}

// Ban bans a Steam ID (decimal) or the online players called `who` for `days` (0 = for good) and removes them.
func (h *Hub) Ban(who string, days int, reason string) (int, error) {
	var ids []uint64
	for _, p := range h.find(who) {
		ids = append(ids, p.Peer)
	}
	if len(ids) == 0 {
		var id uint64
		if _, err := fmt.Sscan(who, &id); err != nil || id == 0 {
			return 0, fmt.Errorf("no online player %q (give a Steam ID to ban someone offline)", who)
		}
		ids = append(ids, id)
	}
	now := time.Now()
	e := Ban{Reason: reason}
	if days > 0 {
		e.Until = now.Add(time.Duration(days) * 24 * time.Hour)
	}
	for _, id := range ids {
		if err := h.bans.Add(id, e.Until, reason); err != nil {
			return 0, err
		}
		h.log.Printf("admin: banned %d (%d days) %s", id, days, reason)
		for _, p := range h.find(fmt.Sprint(id)) {
			p.failWith(bannedPayload(refusalText(e, now), e.Until, now))
		}
	}
	return len(ids), nil
}

// Unban lifts a ban (a kick's block too) by Steam ID (decimal).
func (h *Hub) Unban(id uint64) error { return h.bans.Remove(id) }

// Bans is the bans in force now (kick blocks included), the soonest to end first.
func (h *Hub) Bans() []BanInfo { return h.bans.Active(time.Now()) }

// ReloadBans reads the ban file again and removes online players it bans; it returns how many were removed.
func (h *Hub) ReloadBans() (int, error) {
	if err := h.bans.Reload(); err != nil {
		return 0, err
	}
	type removal struct {
		p *Player
		e Ban
	}
	var gone []removal
	h.mu.Lock()
	for _, p := range h.players {
		if e, banned := h.bans.Lookup(p.Peer); banned {
			gone = append(gone, removal{p, e})
		}
	}
	h.mu.Unlock()
	now := time.Now()
	for _, g := range gone {
		h.log.Printf("player %q (%d) removed: banned", g.p.Name, g.p.Peer)
		g.p.failWith(bannedPayload(refusalText(g.e, now), g.e.Until, now))
	}
	return len(gone), nil
}

// Say sends a NOTICE to everyone; it returns to how many.
func (h *Hub) Say(text string) int {
	var w wire.Writer
	w.Str(trim(text, 200))
	h.mu.Lock()
	all := make([]*Player, 0, len(h.players))
	for _, p := range h.players {
		all = append(all, p)
	}
	h.mu.Unlock()
	for _, p := range all {
		p.send(wire.Notice, w.B)
	}
	return len(all)
}

// Fingerprint is the server key's fingerprint as players see it.
func (h *Hub) Fingerprint() string { return h.id.FingerprintText() }

// gamertag is a name as the game's gamertag buffer holds it: printable ASCII, at most 15 characters.
func gamertag(name string) string {
	out := make([]byte, 0, 15)
	for i := 0; i < len(name) && len(out) < 15; i++ {
		if c := name[i]; c >= 0x20 && c < 0x7F {
			out = append(out, c)
		}
	}
	return string(out)
}
