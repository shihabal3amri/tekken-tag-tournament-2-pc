package hub

import (
	"bytes"
	"errors"
	"time"

	"ttt2-server/internal/wire"
)

// Player files (PROTOCOL.md "Player files"): the game views a player's license (ranks, ranked record) by
// downloading their licence.dat from Xbox LIVE title storage by XUID. Each game uploads its own (FILE_PUT, after a
// login and whenever the game saves it); any player's game reads anyone's by the XUID their game uses (FILE_GET).
// A modified game can only put its own player's license - as on Xbox LIVE, it is the player's game that writes it.
//
// Ranks (PROTOCOL.md "Ranks"): on a server whose ranks are its own (config ranks = "server") the license and the
// disconnect penalties a game plays with here are the copies this server keeps - the game reads its own from here
// at every ONLINE MODE visit (FILE_GET owner 0) and writes them back here, never to its PC. Those copies are kept
// apart from the copies of players' own licenses ("player" ranks: the rank on their PC), so switching the setting
// never turns one into the other.

// FileStore keeps the player files.
type FileStore interface {
	PutPlayerFile(steamID uint64, item string, data []byte, at time.Time) error
	PlayerFile(xuid uint64, item string) ([]byte, bool, error)
	OwnPlayerFile(steamID uint64, item string) ([]byte, bool, error)
	DeletePlayerFile(steamID uint64, item string) error
}

const maxPlayerFile = 16 << 10 // licence.dat is 3420 bytes

// sharedItems are the items players may read of each other (penalty.dat stays the player's own).
var sharedItems = map[string]bool{"licence.dat": true}

// ownItems are the items a server whose ranks are its own keeps for their owner: the license and the penalties the
// game counts against a player who left a ranked match (applied to the license at the next visit).
var ownItems = map[string]bool{"licence.dat": true, "penalty.dat": true}

// serverCopy is the database item of this server's own copy of `item` ("server" ranks).
func serverCopy(item string) string { return "server/" + item }

// SetFiles sets where the player files are kept (before Serve); nil: they are not (WELCOME says so, and FILE_PUT /
// FILE_GET close the connection). They need the statistics too: the XUIDs are recorded there.
func (h *Hub) SetFiles(db FileStore) { h.files = db }

// keepsFiles: WELCOME flag bit 4.
func (h *Hub) keepsFiles() bool { return h.files != nil && h.stats != nil }

// filePut: u64 own XUID, str item, blob data.
func (h *Hub) filePut(p *Player, r *wire.Reader, now time.Time) error {
	own, item, data := r.U64(), r.Str(), r.Blob()
	if r.Err() != nil || len(r.B) != 0 || len(data) == 0 || len(data) > maxPlayerFile {
		return errors.New("bad FILE_PUT")
	}
	key := item
	switch {
	case p.serverRanks && !ownItems[item]:
		h.logPlayer(p, "player file %q of %q dropped: not one this server keeps", trim(item, 32), p.Name)
		return nil
	case p.serverRanks && !p.ownRanks:
		// an older game: what it puts is the license on its PC, which never counts on this server
		h.logPlayer(p, "license of %q dropped: the game does not take its rank from this server (an older version)", p.Name)
		return nil
	case p.serverRanks:
		key = serverCopy(item)
	case !sharedItems[item]:
		h.logPlayer(p, "player file %q of %q dropped: not one players read", trim(item, 32), p.Name)
		return nil
	}
	if !p.filePuts.take(0.2, 6, now) {
		h.logPlayer(p, "player files of %q dropped: too many", p.Name)
		return nil
	}
	if !h.claimXUID(p, own) {
		h.logPlayer(p, "player file of %q dropped: not the XUID this connection uses", p.Name)
		return nil
	}
	if err := h.files.PutPlayerFile(p.Peer, key, bytes.Clone(data), now); err != nil {
		h.logPlayer(p, "player file of %q not kept: %v", p.Name, err)
	}
	return nil
}

// fileGet: u32 request id, u64 owner XUID (0: the asker's own, on a server whose ranks are its own), str item ->
// FILE_RESULT u32 request id, u8 found (1 yes, 0 no, 2 asked too often), blob data.
func (h *Hub) fileGet(p *Player, r *wire.Reader, now time.Time) error {
	request, owner, item := r.U32(), r.U64(), r.Str()
	if r.Err() != nil || len(r.B) != 0 {
		return errors.New("bad FILE_GET")
	}
	found := uint8(2)
	var data []byte
	if p.fileGets.take(5, 20, now) {
		found = 0
		read := func(d []byte, ok bool, err error) {
			switch {
			case err != nil:
				found = 2 // the database did not answer: ask again
				h.logPlayer(p, "player file for %q not read: %v", p.Name, err)
			case ok:
				found, data = 1, d
			}
		}
		switch {
		case owner == 0 && p.serverRanks && p.ownRanks && ownItems[item]:
			read(h.files.OwnPlayerFile(p.Peer, serverCopy(item))) // the player's own, as this server keeps it
		case owner != 0 && sharedItems[item] && p.serverRanks:
			read(h.files.PlayerFile(owner, serverCopy(item)))
		case owner != 0 && sharedItems[item]:
			read(h.files.PlayerFile(owner, item))
		}
	}
	var w wire.Writer
	w.U32(request)
	w.U8(found)
	w.Blob(data)
	p.send(wire.FileResult, w.B)
	return nil
}

// fileDel: u64 own XUID, str item - the game deleted its own item (the penalty file, once a penalty is counted), on a
// server whose ranks are its own (after OWN_RANKS; any other: dropped).
func (h *Hub) fileDel(p *Player, r *wire.Reader, now time.Time) error {
	own, item := r.U64(), r.Str()
	if r.Err() != nil || len(r.B) != 0 {
		return errors.New("bad FILE_DEL")
	}
	switch {
	case !ownItems[item] || !p.ownRanks:
		h.logPlayer(p, "player file %q of %q not deleted: not one this server keeps for it", trim(item, 32), p.Name)
	case !p.filePuts.take(0.2, 6, now):
		h.logPlayer(p, "player files of %q dropped: too many", p.Name)
	case !h.claimXUID(p, own):
		h.logPlayer(p, "player file of %q not deleted: not the XUID this connection uses", p.Name)
	default:
		if err := h.files.DeletePlayerFile(p.Peer, serverCopy(item)); err != nil {
			h.logPlayer(p, "player file of %q not deleted: %v", p.Name, err)
		}
	}
	return nil
}
