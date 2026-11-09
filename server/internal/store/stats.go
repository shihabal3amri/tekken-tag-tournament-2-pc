package store

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"sync"
	"time"

	"ttt2-server/internal/stats"
)

// readTimeout bounds every statistics read (a busy database answers "not ok" rather than holding a connection).
const readTimeout = 5 * time.Second

// WriteStats applies one game's statistics writes for a player (one transaction: all of them or none).
func (s *Store) WriteStats(steamID uint64, at time.Time, writes []stats.Write) error {
	if len(writes) > stats.MaxViewsPerWrite {
		return errors.New("too many views")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	for _, w := range writes {
		v, ok := stats.Lookup(w.View)
		if !ok {
			return fmt.Errorf("no view %d", w.View)
		}
		period := v.Period(at)
		var old *stats.Row
		var rating, rated int64
		var cols []byte
		err := tx.QueryRowContext(ctx, `SELECT rating, columns, rated FROM stats WHERE view = ? AND period = ? AND steam_id = ?`,
			int64(w.View), period, int64(steamID)).Scan(&rating, &cols, &rated)
		switch {
		case errors.Is(err, sql.ErrNoRows):
			rated = at.UnixMilli()
		case err != nil:
			return err
		default:
			c, err := stats.DecodeColumns(cols)
			if err != nil {
				return err
			}
			old = &stats.Row{Rating: rating, Columns: c}
		}
		row, err := v.Apply(old, w.Props)
		if err != nil {
			return err
		}
		if old != nil && row.Rating != old.Rating {
			rated = at.UnixMilli()
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO stats (view, period, steam_id, rating, columns, rated, updated) VALUES (?, ?, ?, ?, ?, ?, ?)
			ON CONFLICT (view, period, steam_id) DO UPDATE SET
				rating = excluded.rating, columns = excluded.columns, rated = excluded.rated, updated = excluded.updated`,
			int64(w.View), period, int64(steamID), row.Rating, stats.EncodeColumns(row.Columns), rated, at.Unix()); err != nil {
			return err
		}
	}
	if err := tx.Commit(); err != nil {
		return err
	}
	s.ranks.mu.Lock()
	s.ranks.writes++
	for _, w := range writes {
		if v, ok := stats.Lookup(w.View); ok {
			if idx := s.ranks.indexes[rankKey{w.View, v.Period(at)}]; idx != nil {
				idx.dirty = true
			}
		}
	}
	s.ranks.mu.Unlock()
	return nil
}

// ---- the boards' order in memory

// rankIndexAge: a board's order is read from the database at most this often; a page from deep down a board of a
// hundred thousand players, or a player's place, is then a slice access instead of a table scan. A write to the board
// makes its order stale after rankIndexDirtyAge (new results show almost at once; busy boards rebuild once a second).
var (
	rankIndexAge      = 10 * time.Second
	rankIndexDirtyAge = time.Second
)

// SetBoardFreshness sets how soon a written board's order is read again (tests: 0, every read after a write).
func SetBoardFreshness(dirty time.Duration) { rankIndexDirtyAge = dirty }

type rankKey struct {
	view   uint32
	period int64
}

type rankIndex struct {
	built time.Time
	used  time.Time
	dirty bool             // written since it was built
	ids   []uint64         // Steam IDs, best first
	place map[uint64]int32 // Steam ID -> index in ids
}

type rankIndexes struct {
	mu      sync.Mutex
	build   sync.Mutex // one board is read at a time (no stampede when an index goes stale)
	indexes map[rankKey]*rankIndex
	writes  uint64 // statistics writes so far (under mu): a write during a build makes the new index dirty
}

// order is the board's order, at most rankIndexAge old (indexes not asked for in 10 minutes are dropped).
func (s *Store) order(ctx context.Context, view uint32, period int64) (*rankIndex, error) {
	key := rankKey{view, period}
	now := time.Now()
	get := func() *rankIndex {
		s.ranks.mu.Lock()
		defer s.ranks.mu.Unlock()
		idx := s.ranks.indexes[key]
		if idx != nil && now.Sub(idx.built) < rankIndexAge && (!idx.dirty || now.Sub(idx.built) < rankIndexDirtyAge) {
			idx.used = now
			return idx
		}
		return nil
	}
	if idx := get(); idx != nil {
		return idx, nil
	}
	s.ranks.build.Lock()
	defer s.ranks.build.Unlock()
	if idx := get(); idx != nil { // built while this one waited
		return idx, nil
	}
	s.ranks.mu.Lock()
	writesBefore := s.ranks.writes
	s.ranks.mu.Unlock()
	q, err := s.db.QueryContext(ctx, `SELECT steam_id FROM stats WHERE view = ? AND period = ? ORDER BY rating DESC, rated, steam_id`,
		int64(view), period)
	if err != nil {
		return nil, err
	}
	defer q.Close()
	idx := &rankIndex{built: now, used: now, place: map[uint64]int32{}}
	for q.Next() {
		var id int64
		if err := q.Scan(&id); err != nil {
			return nil, err
		}
		idx.place[uint64(id)] = int32(len(idx.ids))
		idx.ids = append(idx.ids, uint64(id))
	}
	if err := q.Err(); err != nil {
		return nil, err
	}
	s.ranks.mu.Lock()
	if s.ranks.writes != writesBefore {
		idx.dirty = true // a write may have landed after the read began
	}
	if s.ranks.indexes == nil {
		s.ranks.indexes = map[rankKey]*rankIndex{}
	}
	for k, old := range s.ranks.indexes {
		if now.Sub(old.used) > 10*time.Minute {
			delete(s.ranks.indexes, k)
		}
	}
	s.ranks.indexes[key] = idx
	s.ranks.mu.Unlock()
	return idx, nil
}

// rows reads those players' rows of a board (in no particular order; missing ones are left out).
func (s *Store) rows(ctx context.Context, view uint32, period int64, ids []uint64) (map[uint64]stats.Entry, error) {
	out := map[uint64]stats.Entry{}
	if len(ids) == 0 {
		return out, nil
	}
	args := []any{int64(view), period}
	for _, id := range ids {
		args = append(args, int64(id))
	}
	q, err := s.db.QueryContext(ctx, `SELECT s.steam_id, s.rating, s.columns, p.name, p.xuid FROM stats s JOIN players p USING (steam_id)
		WHERE s.view = ? AND s.period = ? AND s.steam_id IN (?`+strings.Repeat(",?", len(ids)-1)+`)`, args...)
	if err != nil {
		return nil, err
	}
	defer q.Close()
	for q.Next() {
		var e stats.Entry
		var sid int64
		var xuid sql.NullInt64
		var cols []byte
		if err := q.Scan(&sid, &e.Rating, &cols, &e.Name, &xuid); err != nil {
			return nil, err
		}
		e.SteamID, e.XUID = uint64(sid), uint64(xuid.Int64)
		if e.Columns, err = stats.DecodeColumns(cols); err != nil {
			return nil, err
		}
		out[e.SteamID] = e
	}
	return out, q.Err()
}

// Board is one page of a view's board (at the period `at` falls in): `count` rows from place `start` (1 = first),
// and how many players the board has. The order is at most rankIndexAge old; the rows' values are current.
func (s *Store) Board(view uint32, at time.Time, start, count int) (total int64, rows []stats.Entry, err error) {
	v, ok := stats.Lookup(view)
	if !ok {
		return 0, nil, fmt.Errorf("no view %d", view)
	}
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	period := v.Period(at)
	idx, err := s.order(ctx, view, period)
	if err != nil {
		return 0, nil, err
	}
	start = max(start, 1)
	count = min(max(count, 0), stats.MaxRows)
	var page []uint64
	if start-1 < len(idx.ids) {
		page = idx.ids[start-1 : min(len(idx.ids), start-1+count)]
	}
	found, err := s.rows(ctx, view, period, page)
	if err != nil {
		return 0, nil, err
	}
	for i, id := range page {
		if e, ok := found[id]; ok {
			e.Rank = int64(start + i)
			rows = append(rows, e)
		}
	}
	return int64(len(idx.ids)), rows, nil
}

// Entries is those players' rows on a view's board with their places (players without one are left out). A row
// newer than the board's order in memory gets its place from the database.
func (s *Store) Entries(view uint32, at time.Time, steamIDs []uint64) (total int64, rows []stats.Entry, err error) {
	v, ok := stats.Lookup(view)
	if !ok {
		return 0, nil, fmt.Errorf("no view %d", view)
	}
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	period := v.Period(at)
	idx, err := s.order(ctx, view, period)
	if err != nil {
		return 0, nil, err
	}
	found, err := s.rows(ctx, view, period, steamIDs)
	if err != nil {
		return 0, nil, err
	}
	for _, id := range steamIDs {
		e, ok := found[id]
		if !ok {
			continue
		}
		if p, ok := idx.place[id]; ok {
			e.Rank = int64(p + 1)
		} else {
			var rated int64
			if err := s.db.QueryRowContext(ctx, `SELECT rated FROM stats WHERE view = ? AND period = ? AND steam_id = ?`,
				int64(view), period, int64(id)).Scan(&rated); err != nil {
				return 0, nil, err
			}
			if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) + 1 FROM stats WHERE view = ? AND period = ? AND
				(rating > ? OR (rating = ? AND (rated < ? OR (rated = ? AND steam_id < ?))))`,
				int64(view), period, e.Rating, e.Rating, rated, rated, int64(id)).Scan(&e.Rank); err != nil {
				return 0, nil, err
			}
		}
		rows = append(rows, e)
	}
	return max(int64(len(idx.ids)), int64(len(rows))), rows, nil
}

// SteamIDsOf maps online XUIDs to the players whose games use them (unknown XUIDs are left out). Should two players
// have reported one, a player whose Steam ID was checked wins, then the one seen last.
func (s *Store) SteamIDsOf(xuids []uint64) (map[uint64]uint64, error) {
	out := map[uint64]uint64{}
	err := s.byXUID(xuids, `steam_id`, func(x uint64, v int64, _ string) { out[x] = uint64(v) })
	return out, err
}

// NamesOf maps online XUIDs to the names of the players whose games use them (unknown XUIDs are left out), as
// SteamIDsOf picks the player.
func (s *Store) NamesOf(xuids []uint64) (map[uint64]string, error) {
	out := map[uint64]string{}
	err := s.byXUID(xuids, `name`, func(x uint64, _ int64, name string) { out[x] = name })
	return out, err
}

// byXUID calls found for each player of the XUIDs, with the column asked for (steam_id or name), checked players
// and then the most recently seen last.
func (s *Store) byXUID(xuids []uint64, column string, found func(xuid uint64, n int64, text string)) error {
	if len(xuids) == 0 {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), readTimeout)
	defer cancel()
	args := make([]any, len(xuids))
	for i, x := range xuids {
		args[i] = int64(x)
	}
	q, err := s.db.QueryContext(ctx, `SELECT xuid, steam_id, name FROM players WHERE xuid IN (?`+strings.Repeat(",?", len(xuids)-1)+`)
		ORDER BY verified, last_seen`, args...)
	if err != nil {
		return err
	}
	defer q.Close()
	for q.Next() {
		var x, id int64
		var name string
		if err := q.Scan(&x, &id, &name); err != nil {
			return err
		}
		found(uint64(x), id, name) // in that order: the last one wins
	}
	return q.Err()
}

// Wipe forgets a player's statistics, uploads, ghosts, replays, profile and license files - on a server whose ranks
// are its own, their rank there (a cheater's rows); the player stays known (bans, seen).
func (s *Store) Wipe(steamID uint64) (int64, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()
	var n int64
	for _, q := range []string{`DELETE FROM stats WHERE steam_id = ?`, `DELETE FROM uploads WHERE steam_id = ?`, `DELETE FROM profiles WHERE steam_id = ?`,
		`DELETE FROM ghosts WHERE steam_id = ?`, `DELETE FROM replays WHERE steam_id = ?`, `DELETE FROM player_files WHERE steam_id = ?`} {
		r, err := tx.ExecContext(ctx, q, int64(steamID))
		if err != nil {
			return 0, err
		}
		k, _ := r.RowsAffected()
		n += k
	}
	if err := tx.Commit(); err != nil {
		return 0, err
	}
	s.recountUploads()
	s.ranks.mu.Lock()
	s.ranks.indexes = nil // every board again from the database
	s.ranks.writes++
	s.ranks.mu.Unlock()
	return n, nil
}
