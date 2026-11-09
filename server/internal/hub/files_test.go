package hub

import (
	"bytes"
	"path/filepath"
	"testing"
	"time"

	"ttt2-server/internal/config"
	"ttt2-server/internal/store"
	"ttt2-server/internal/wire"
)

const peerD, xuidD = 0x0110000100000D04, 0x0009000000000D04

func startFiles(t *testing.T) *server {
	t.Helper()
	s, _ := startFilesWith(t, trusting())
	return s
}

func startFilesWith(t *testing.T, cfg config.Config) (*server, *store.Store) {
	t.Helper()
	db, err := store.Open(filepath.Join(t.TempDir(), "server.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return startSetup(t, cfg, nil, func(h *Hub) {
		h.SetRecords(db)
		h.SetStats(db)
		h.SetFiles(db)
	}), db
}

// waitFile waits until `cl` reads `owner`'s license as `want` (another connection's put arrives in its own time).
func waitFile(t *testing.T, cl *client, request uint32, owner uint64, want []byte) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for {
		found, data := cl.getFile(t, request, owner, "licence.dat")
		if found == 1 && bytes.Equal(data, want) {
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("license of %016X: found %d, %d bytes", owner, found, len(data))
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func (cl *client) delFile(own uint64, item string) {
	cl.send(wire.FileDel, func(w *wire.Writer) {
		w.U64(own)
		w.Str(item)
	})
}

func (cl *client) putFile(own uint64, item string, data []byte) {
	cl.send(wire.FilePut, func(w *wire.Writer) {
		w.U64(own)
		w.Str(item)
		w.Blob(data)
	})
}

func (cl *client) getFile(t *testing.T, request uint32, owner uint64, item string) (uint8, []byte) {
	t.Helper()
	cl.send(wire.FileGet, func(w *wire.Writer) {
		w.U32(request)
		w.U64(owner)
		w.Str(item)
	})
	typ, r := cl.next()
	if typ != wire.FileResult || r.U32() != request {
		t.Fatalf("FILE_RESULT expected, got frame %d", typ)
	}
	found, data := r.U8(), r.Blob()
	if r.Err() != nil || len(r.B) != 0 {
		t.Fatal("malformed FILE_RESULT")
	}
	return found, data
}

func TestPlayerFiles(t *testing.T) {
	s := startFiles(t)
	if flags, _ := welcomeFlags(t, s, peerC); flags&16 == 0 {
		t.Fatal("WELCOME must offer player files")
	}
	a := s.login(t, peerA, "Alice")
	b := s.login(t, peerB, "Bob")
	licA, licB := bytes.Repeat([]byte{0xA1}, 3420), bytes.Repeat([]byte{0xB2}, 3420)
	a.putFile(xuidA, "licence.dat", licA)
	b.putFile(xuidB, "licence.dat", licB)
	b.putFile(xuidB, "penalty.dat", []byte{1, 2, 3}) // Bob's own: never anyone else's to read
	deadline := time.Now().Add(3 * time.Second)
	for {
		found, data := a.getFile(t, 1, xuidB, "licence.dat")
		if found == 1 && bytes.Equal(data, licB) {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("Bob's license: found %d, %d bytes", found, len(data))
		}
		time.Sleep(20 * time.Millisecond)
	}
	if found, data := b.getFile(t, 2, xuidA, "licence.dat"); found != 1 || !bytes.Equal(data, licA) {
		t.Fatalf("Alice's license: found %d, %d bytes", found, len(data))
	}
	if found, _ := a.getFile(t, 3, xuidB, "penalty.dat"); found != 0 {
		t.Fatal("penalty.dat is nobody else's to read")
	}
	if found, _ := a.getFile(t, 4, xuidC, "licence.dat"); found != 0 {
		t.Fatal("an unknown XUID has no license")
	}
	// a newer upload replaces the one before
	licB2 := bytes.Repeat([]byte{0xB3}, 3420)
	time.Sleep(5 * time.Second) // FILE_PUT: one every 5 s after a burst of 6
	b.putFile(xuidB, "licence.dat", licB2)
	deadline = time.Now().Add(3 * time.Second)
	for {
		if found, data := a.getFile(t, 5, xuidB, "licence.dat"); found == 1 && bytes.Equal(data, licB2) {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("the newer license must replace the older")
		}
		time.Sleep(20 * time.Millisecond)
	}
	// too large: a malformed frame
	a.putFile(xuidA, "licence.dat", make([]byte, maxPlayerFile+1))
	if !closedSoon(a.c, 3*time.Second) {
		t.Fatal("a too large player file must close the connection")
	}
}

// "server" ranks (the default): the license and penalties a game plays with here are this server's copies - its own
// read with owner 0, written back here - kept apart from the copies of players' own licenses; an older game (no
// OWN_RANKS) neither puts a license nor registers a ranked match.
func TestServerRanks(t *testing.T) {
	s, db := startFilesWith(t, trusting())
	// a copy of Dave's own license, kept while the server had "player" ranks: never his rank here
	if err := db.PutPlayerFile(peerD, "licence.dat", bytes.Repeat([]byte{0xD4}, 3420), time.Now()); err != nil {
		t.Fatal(err)
	}
	a, flags := s.loginOld(t, peerA, "Alice")
	if flags&48 != 48 {
		t.Fatalf("WELCOME flags %#x: player files and the server's own ranks expected", flags)
	}
	if flags := infoFlags(t, s); flags&4 == 0 {
		t.Fatalf("INFO flags %#x: the server list must tell its ranks are its own", flags)
	}
	a.send(wire.OwnRanks, nil)
	b := s.login(t, peerB, "Bob")
	if found, _ := a.getFile(t, 1, 0, "licence.dat"); found != 0 {
		t.Fatal("a new player has no license here (the game makes a beginner's)")
	}
	licA := bytes.Repeat([]byte{0xA1}, 3420)
	a.putFile(xuidA, "licence.dat", licA)
	if found, data := a.getFile(t, 2, 0, "licence.dat"); found != 1 || !bytes.Equal(data, licA) {
		t.Fatalf("Alice's own license: found %d, %d bytes", found, len(data))
	}
	waitFile(t, b, 3, xuidA, licA)
	// penalties: kept for their owner only, deleted when the game deletes them
	a.putFile(xuidA, "penalty.dat", []byte{1, 2, 3})
	if found, data := a.getFile(t, 4, 0, "penalty.dat"); found != 1 || !bytes.Equal(data, []byte{1, 2, 3}) {
		t.Fatalf("Alice's own penalties: found %d", found)
	}
	if found, _ := b.getFile(t, 5, xuidA, "penalty.dat"); found != 0 {
		t.Fatal("penalties are nobody else's to read")
	}
	a.delFile(xuidA, "penalty.dat")
	if found, _ := a.getFile(t, 6, 0, "penalty.dat"); found != 0 {
		t.Fatal("a deleted penalty file must be gone")
	}
	if found, _ := b.getFile(t, 7, 0, "licence.dat"); found != 0 {
		t.Fatal("Bob has no license here yet: owner 0 is the asker's own, never another player's")
	}
	// Dave's copy from "player" ranks counts for nothing here
	d := s.login(t, peerD, "Dave")
	d.putFile(xuidD, "penalty.dat", []byte{9}) // claims Dave's XUID
	if found, _ := d.getFile(t, 8, 0, "licence.dat"); found != 0 {
		t.Fatal("a copy of a player's own license must never become their rank here")
	}
	if found, _ := b.getFile(t, 9, xuidD, "licence.dat"); found != 0 {
		t.Fatal("other players must not see the copy of Dave's own license here")
	}
	// an older game: its license (the one on its PC) is dropped, its own files are not given, it cannot delete
	c, _ := s.loginOld(t, peerC, "Carol")
	c.putFile(xuidC, "licence.dat", bytes.Repeat([]byte{0xC3}, 3420))
	c.delFile(xuidC, "penalty.dat")
	if found, _ := c.getFile(t, 10, 0, "licence.dat"); found != 0 {
		t.Fatal("an older game has no own license here")
	}
	if found, _ := b.getFile(t, 11, xuidC, "licence.dat"); found != 0 {
		t.Fatal("an older game's license must not be kept on a server whose ranks are its own")
	}
	// the database keeps the two kinds apart
	if data, ok, _ := db.OwnPlayerFile(peerA, "server/licence.dat"); !ok || !bytes.Equal(data, licA) {
		t.Fatal("Alice's license is this server's copy")
	}
	if _, ok, _ := db.OwnPlayerFile(peerA, "licence.dat"); ok {
		t.Fatal("this server's copy must not be kept as a copy of a player's own license")
	}
}

// "player" ranks: every game plays with the license on its PC and puts a copy here for the others; the frames of
// "server" ranks are unknown to such a server.
func TestPlayerRanks(t *testing.T) {
	cfg := trusting()
	cfg.Ranks = config.RanksPlayer
	s, _ := startFilesWith(t, cfg)
	a, flags := s.loginOld(t, peerA, "Alice")
	if flags&16 == 0 || flags&32 != 0 {
		t.Fatalf("WELCOME flags %#x: player files, the players' own ranks", flags)
	}
	if flags := infoFlags(t, s); flags&4 != 0 {
		t.Fatalf("INFO flags %#x: the players' own ranks", flags)
	}
	b := s.login(t, peerB, "Bob")
	licA := bytes.Repeat([]byte{0xA1}, 3420)
	a.putFile(xuidA, "licence.dat", licA)
	waitFile(t, b, 1, xuidA, licA)
	if found, _ := a.getFile(t, 2, 0, "licence.dat"); found != 0 {
		t.Fatal("a game reads its own license from its PC here")
	}
	a.send(wire.OwnRanks, nil)
	if !closedSoon(a.c, 3*time.Second) {
		t.Fatal("OWN_RANKS to a server whose ranks are the players' must close the connection")
	}
}

// infoFlags asks for the server list's INFO and returns its flags byte.
func infoFlags(t *testing.T, s *server) uint8 {
	t.Helper()
	u := s.udpConn(t)
	var w wire.Writer
	w.U8(wire.InfoReq)
	w.U32(wire.InfoMagic)
	w.U64(7)
	w.Raw(make([]byte, wire.InfoReqMinSize-len(w.B)))
	u.Write(w.B)
	buf := make([]byte, 2048)
	u.SetReadDeadline(time.Now().Add(time.Second))
	n, err := u.Read(buf)
	if err != nil {
		t.Fatal("no INFO answer")
	}
	r := wire.Reader{B: buf[:n]}
	if r.U8() != wire.Info || r.U32() != wire.InfoMagic || r.U64() != 7 || r.U16() != wire.ProtocolVersion {
		t.Fatal("INFO header")
	}
	return r.U8()
}

func TestPlayerFilesNeedTheServerToKeepThem(t *testing.T) {
	plain := start(t, trusting(), nil)
	flags, cl := welcomeFlags(t, plain, peerA)
	if flags&16 != 0 {
		t.Fatal("a server without a database keeps no player files")
	}
	if flags := infoFlags(t, plain); flags&4 != 0 {
		t.Fatal("a server without player files has no ranks of its own")
	}
	cl.putFile(xuidA, "licence.dat", []byte{1})
	if !closedSoon(cl.c, 3*time.Second) {
		t.Fatal("FILE_PUT to a server without player files must close the connection")
	}
}

// On a server whose ranks are its own, a ranked match with an older game (which plays with the rank on its PC)
// counts for nobody: that game's registration is refused, so the pair never forms.
func TestServerRanksOlderGameRankedDoesNotCount(t *testing.T) {
	store.SetBoardFreshness(0)
	s, _ := startFilesWith(t, trusting())
	a, _ := s.loginOld(t, peerA, "HOST A")
	b := s.login(t, peerB, "GUEST B")
	const match = 0x8000000000AC0001
	a.send(wire.SessionPut, func(w *wire.Writer) { w.Blob(rankedRecord(match, peerA)) })
	a.sync()
	a.register(match, xuidA, peerB)
	b.register(match, xuidB, peerA)
	for _, cl := range []*client{a, b} {
		own := uint64(xuidA)
		if cl == b {
			own = xuidB
		}
		cl.writeStats(match, own, xuidA, result(600, 1, 0))
		cl.writeStats(match, own, xuidB, result(400, 0, 1))
	}
	if ok, total, _ := b.board(bpView, 1, 10, 0); !ok || total != 0 {
		t.Fatalf("a ranked match with an older game must not count here: %v %d", ok, total)
	}
}
