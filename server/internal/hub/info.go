package hub

import (
	"net"
	"time"
	"unicode/utf8"

	"ttt2-server/internal/config"
	"ttt2-server/internal/wire"
)

// UDP carries only the public status reply for the game's server list (INFO_REQ -> INFO): answered on a worker of
// its own, with per-address and total limits, the reply cached for a quarter second. Matches run over Steam P2P,
// not through the server.
const (
	infoPerPrefix = 10 // INFO answers per second per address / IPv6 /64
	infoTotal     = 500
	infoCacheTime = 250 * time.Millisecond
	infoQueue     = 256
)

type bucket struct {
	tokens float64
	last   time.Time
}

func (b *bucket) take(rate, burst float64, now time.Time) bool {
	if b.last.IsZero() {
		b.tokens, b.last = burst, now
	}
	b.tokens = min(burst, b.tokens+rate*now.Sub(b.last).Seconds())
	b.last = now
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

func (h *Hub) udpLoop() {
	buf := make([]byte, 2048)
	for {
		n, from, err := h.udp.ReadFromUDP(buf)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			h.log.Printf("udp: %v", err)
			return
		}
		if n >= 1 && buf[0] == wire.InfoReq {
			h.info.offer(buf[:n], from)
		}
	}
}

type infoRequest struct {
	nonce uint64
	size  int
	from  *net.UDPAddr
}

type infoState struct {
	queue    chan infoRequest
	limits   map[string]*bucket
	total    bucket
	sweep    time.Time
	cached   []byte // the reply for a minimum-size request, nonce bytes zero
	cachedAt time.Time
}

func (s *infoState) start(h *Hub) {
	s.queue = make(chan infoRequest, infoQueue)
	s.limits = map[string]*bucket{}
	go func() {
		for req := range s.queue {
			s.answer(h, req)
		}
	}()
}

// offer queues a request (the UDP goroutine only parses the header; a full queue drops it).
func (s *infoState) offer(b []byte, from *net.UDPAddr) {
	r := wire.Reader{B: b[1:]}
	magic, nonce := r.U32(), r.U64()
	if r.Err() != nil || magic != wire.InfoMagic || len(b) < wire.InfoReqMinSize {
		return
	}
	select {
	case s.queue <- infoRequest{nonce, len(b), from}:
	default:
	}
}

func (s *infoState) answer(h *Hub, req infoRequest) {
	now := time.Now()
	if !s.total.take(infoTotal, infoTotal, now) {
		return
	}
	if now.Sub(s.sweep) > time.Minute {
		for k, b := range s.limits {
			if now.Sub(b.last) > time.Minute {
				delete(s.limits, k)
			}
		}
		s.sweep = now
	}
	key := prefixOf(req.from.IP)
	if b := s.limits[key]; b != nil || len(s.limits) < 100000 {
		if b == nil {
			b = &bucket{}
			s.limits[key] = b
		}
		if !b.take(infoPerPrefix, 2*infoPerPrefix, now) {
			return
		}
	} // a full table (a spoofed flood): only the total limit applies, the server stays visible
	var reply []byte
	if req.size == wire.InfoReqMinSize {
		if s.cached == nil || now.Sub(s.cachedAt) > infoCacheTime {
			s.cached, s.cachedAt = h.infoReply(0, 3*wire.InfoReqMinSize), now
		}
		reply = append([]byte(nil), s.cached...)
		for i := 0; i < 8; i++ {
			reply[5+i] = byte(req.nonce >> (8 * i))
		}
	} else {
		reply = h.infoReply(req.nonce, 3*req.size)
	}
	h.udp.WriteToUDP(reply, req.from)
}

// infoReply is the INFO datagram, at most `limit` bytes (the message of the day is cut to fit).
func (h *Hub) infoReply(nonce uint64, limit int) []byte {
	cfg := h.Config()
	st := h.Stats()
	build := func(motd string) []byte {
		var w wire.Writer
		w.U8(wire.Info)
		w.U32(wire.InfoMagic)
		w.U64(nonce)
		w.U16(wire.ProtocolVersion)
		flags := uint8(0)
		if cfg.Password != "" {
			flags |= 1
		}
		if h.verifier != nil {
			flags |= 2
		}
		if h.keepsFiles() && cfg.Ranks == config.RanksServer {
			flags |= 4 // the ranks are this server's own ("Ranks")
		}
		w.U8(flags)
		w.Str(trim(cfg.Name, 64))
		w.Str(cfg.Region)
		w.U16(uint16(min(st.Online, 0xFFFF)))
		w.U16(uint16(min(cfg.MaxPlayers, 0xFFFF)))
		w.U16(uint16(min(st.InRooms, 0xFFFF)))
		w.U16(uint16(min(st.Sessions, 0xFFFF)))
		w.Str(motd)
		protos := ""
		for i, p := range cfg.AcceptedProtocols {
			if i > 0 {
				protos += ","
			}
			protos += p
		}
		w.Str(trim(protos, 128))
		return w.B
	}
	motd := cfg.MOTD
	b := build(motd)
	for len(b) > limit && motd != "" {
		cut := max(0, len(motd)-(len(b)-limit))
		for cut > 0 && !utf8.RuneStart(motd[cut]) {
			cut--
		}
		motd = motd[:cut]
		b = build(motd)
	}
	return b
}
