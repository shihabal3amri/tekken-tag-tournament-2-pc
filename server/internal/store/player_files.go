package store

import (
	"context"
	"database/sql"
	"errors"
	"time"
)

// Player files (PROTOCOL.md "Player files"): each player's last upload of the game's per-user title-storage items
// other players read - the license (licence.dat: ranks and ranked record), which the game views by the owner's XUID.

// MaxPlayerFile is the largest player file kept (licence.dat is 3420 bytes).
const MaxPlayerFile = 16 << 10

// PutPlayerFile keeps a player's item, replacing the one before.
func (s *Store) PutPlayerFile(steamID uint64, item string, data []byte, at time.Time) error {
	if len(data) == 0 || len(data) > MaxPlayerFile {
		return errors.New("player file: 1 to 16384 bytes")
	}
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	_, err := s.db.ExecContext(ctx, `INSERT INTO player_files (steam_id, item, data, updated) VALUES (?, ?, ?, ?)
		ON CONFLICT (steam_id, item) DO UPDATE SET data = excluded.data, updated = excluded.updated`,
		int64(steamID), item, data, at.Unix())
	return err
}

// PlayerFile is the item of the player whose game uses the online XUID `xuid` (the player SteamIDsOf picks); false
// when there is no such player or they never uploaded it.
func (s *Store) PlayerFile(xuid uint64, item string) ([]byte, bool, error) {
	owners, err := s.SteamIDsOf([]uint64{xuid})
	if err != nil {
		return nil, false, err
	}
	steamID, ok := owners[xuid]
	if !ok {
		return nil, false, nil
	}
	return s.OwnPlayerFile(steamID, item)
}

// OwnPlayerFile is a player's item by their Steam ID; false when they never uploaded it.
func (s *Store) OwnPlayerFile(steamID uint64, item string) ([]byte, bool, error) {
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	var data []byte
	err := s.db.QueryRowContext(ctx, `SELECT data FROM player_files WHERE steam_id = ? AND item = ?`, int64(steamID), item).Scan(&data)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, false, nil
	}
	if err != nil {
		return nil, false, err
	}
	return data, true, nil
}

// DeletePlayerFile removes a player's item (none: nothing happens).
func (s *Store) DeletePlayerFile(steamID uint64, item string) error {
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	_, err := s.db.ExecContext(ctx, `DELETE FROM player_files WHERE steam_id = ? AND item = ?`, int64(steamID), item)
	return err
}
