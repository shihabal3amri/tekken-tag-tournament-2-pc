// Package wire holds the byte formats of PROTOCOL.md: little-endian integers, u16-length strings, u32-length
// blobs, and the control connection's frames (u32 size, u8 type, payload).
package wire

import (
	"encoding/binary"
	"errors"
	"io"
)

// Control frame types (PROTOCOL.md).
const (
	Hello        = 1
	Auth         = 2
	Ping         = 3
	SessionPut   = 4
	SessionDel   = 5
	SessionList  = 6
	SessionGet   = 7
	InviteSend   = 8
	GinRequest   = 9
	StatsWrite   = 10
	StatsBoard   = 11
	StatsPlayers = 12
	ArbRegister  = 13
	PeerCheck    = 14
	XuidNames    = 15
	MatchEvent   = 16 // only to a server whose WELCOME has flag bit 3 (matches are recorded)
	FilePut      = 17 // FilePut, FileGet: only to a server whose WELCOME has flag bit 4 (player files are kept)
	FileGet      = 18
	OwnRanks     = 19 // OwnRanks, FileDel: only to a server whose WELCOME has flag bit 5 (the ranks are its own)
	FileDel      = 20
	Welcome      = 101
	Error        = 102
	Pong         = 103
	ListResult   = 104
	GetResult    = 105
	Invite       = 106
	Notice       = 109
	GinResponse  = 110
	StatsResult  = 111
	PeerResult   = 112
	NamesResult  = 113
	FileResult   = 114
	MaxFrameSize = 1 << 20
)

// UDP datagram kinds (only the server list's status request and reply).
const (
	InfoReq = 1
	Info    = 2

	InfoMagic      = 0x49535454 // "TTSI" little-endian
	InfoReqMinSize = 128
)

// ERROR codes.
const (
	ErrProtocol    = 1
	ErrGameVersion = 2
	ErrFull        = 3
	ErrPassword    = 4
	ErrAuth        = 5
	ErrBanned      = 6
	ErrDuplicate   = 7
	ErrMalformed   = 8
	ErrMaintenance = 9
)

// ProtocolVersion is the version HELLO carries. 2: no match relay (matches run over Steam P2P).
const ProtocolVersion = 2

// Writer appends values to a byte slice.
type Writer struct{ B []byte }

func (w *Writer) U8(v uint8)   { w.B = append(w.B, v) }
func (w *Writer) U16(v uint16) { w.B = binary.LittleEndian.AppendUint16(w.B, v) }
func (w *Writer) U32(v uint32) { w.B = binary.LittleEndian.AppendUint32(w.B, v) }
func (w *Writer) U64(v uint64) { w.B = binary.LittleEndian.AppendUint64(w.B, v) }

// Str writes a u16-length string, cut at 65535 bytes.
func (w *Writer) Str(s string) {
	if len(s) > 0xFFFF {
		s = s[:0xFFFF]
	}
	w.U16(uint16(len(s)))
	w.B = append(w.B, s...)
}

// Blob writes u32 length + bytes.
func (w *Writer) Blob(b []byte) {
	w.U32(uint32(len(b)))
	w.B = append(w.B, b...)
}

// Raw appends bytes as they are.
func (w *Writer) Raw(b []byte) { w.B = append(w.B, b...) }

// Reader reads values from a byte slice; after the first short read every value is zero and Err() is set.
type Reader struct {
	B   []byte
	err error
}

var ErrShort = errors.New("wire: truncated")

func (r *Reader) take(n int) []byte {
	if r.err != nil || len(r.B) < n {
		r.err = ErrShort
		return nil
	}
	b := r.B[:n]
	r.B = r.B[n:]
	return b
}

func (r *Reader) U8() uint8 {
	if b := r.take(1); b != nil {
		return b[0]
	}
	return 0
}
func (r *Reader) U16() uint16 {
	if b := r.take(2); b != nil {
		return binary.LittleEndian.Uint16(b)
	}
	return 0
}
func (r *Reader) U32() uint32 {
	if b := r.take(4); b != nil {
		return binary.LittleEndian.Uint32(b)
	}
	return 0
}
func (r *Reader) U64() uint64 {
	if b := r.take(8); b != nil {
		return binary.LittleEndian.Uint64(b)
	}
	return 0
}

// Str reads a u16-length string.
func (r *Reader) Str() string { return string(r.take(int(r.U16()))) }

// Blob reads u32 length + bytes, refusing lengths beyond what is left.
func (r *Reader) Blob() []byte {
	n := r.U32()
	if int64(n) > int64(len(r.B)) {
		r.err = ErrShort
		return nil
	}
	return append([]byte(nil), r.take(int(n))...)
}

// Bytes reads n raw bytes (nil after an error; never more than what is left).
func (r *Reader) Bytes(n int) []byte {
	if n < 0 {
		r.err = ErrShort
		return nil
	}
	return r.take(n)
}

// Rest returns the unread bytes.
func (r *Reader) Rest() []byte {
	b := r.B
	r.B = nil
	return b
}

func (r *Reader) Err() error { return r.err }

// WriteFrame writes one control frame.
func WriteFrame(w io.Writer, typ uint8, payload []byte) error {
	buf := make([]byte, 5+len(payload))
	binary.LittleEndian.PutUint32(buf, uint32(1+len(payload)))
	buf[4] = typ
	copy(buf[5:], payload)
	_, err := w.Write(buf)
	return err
}

var ErrFrameSize = errors.New("wire: bad frame size")

// ReadFrame reads one control frame of at most MaxFrameSize bytes.
func ReadFrame(r io.Reader) (typ uint8, payload []byte, err error) {
	return ReadFrameMax(r, MaxFrameSize)
}

// ReadFrameMax reads one control frame of at most `max` bytes (type + payload); a larger size fails before
// anything is allocated.
func ReadFrameMax(r io.Reader, max uint32) (typ uint8, payload []byte, err error) {
	var head [5]byte
	if _, err = io.ReadFull(r, head[:]); err != nil {
		return 0, nil, err
	}
	size := binary.LittleEndian.Uint32(head[:4])
	if size == 0 || size > max {
		return 0, nil, ErrFrameSize
	}
	payload = make([]byte, size-1)
	if _, err = io.ReadFull(r, payload); err != nil {
		return 0, nil, err
	}
	return head[4], payload, nil
}
