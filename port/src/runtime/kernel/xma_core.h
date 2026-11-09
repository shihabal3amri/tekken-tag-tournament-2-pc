#pragma once
// XMA hardware decoder emulation core (guest glue: xma.cpp).
//
// The Xbox 360 audio block decodes XMA2 for up to 320 hardware contexts. A context is 64 bytes
// of big-endian dwords in a physical array whose address the title reads from register 0x1800
// of the block at 0x7FEA0000. TTT2 imports only XMACreateContext / XMAReleaseContext and drives
// the rest itself (NUSound sub_82D3DF38 / sub_82D3DFD0 / sub_82D3E120, and the statically
// linked XAudio2 sub_82C295E0 / sub_82C29750): it fills the context, writes one bit per context
// to the kick registers (0x1940 + 4*(index/32)), and on its next audio frame reads the context
// back: consumed input buffers have their valid bit cleared, the output ring's write offset has
// advanced over newly decoded 16-bit big-endian PCM.
//
// Context dwords (LSB-first bit fields, as in Xenia's XMA_CONTEXT_DATA):
//   0: input-0 packet count [0,12) loop count [12,20) (255 = forever) input-0 valid 20,
//      input-1 valid 21, output block count [22,27) (256-byte blocks), output write offset [27,32)
//   1: input-1 packet count [0,12) ... sample rate [27,29) stereo 29, output valid 31
//   2: input read offset [0,26)    3: loop start [0,26)    4: loop end [0,26), current buffer 31
//   5/6: input-0/1 address   7: output address   8: work buffer   9: output read offset [0,5)
// Bit offsets count from the start of the input buffer, packet headers included.
//
// The input is a run of 2048-byte packets; each starts with a 32-bit header (frames beginning
// here [6], bits finishing the previous packet's last frame [15], metadata [3], skip [8]), then
// a bit stream of length-prefixed frames (15-bit length, in bits, counting the prefix; 0x7FFF
// or a header saying nothing continues marks padding). Frames freely cross packet and buffer
// boundaries. This core walks that bit stream exactly as the hardware's read offset does: it
// assembles one frame at a time (carrying a partial frame across a buffer switch, so a buffer
// is released as soon as it has been read), honours loop regions at frame granularity, and
// hands each frame to FFmpeg's XMA2 decoder re-packed alone into its own packet(s), so the
// decoder sees the same frames in the same order as from the original packets. Each frame's
// 512 samples per channel are written to the output ring as whole 256-byte blocks; a kick
// decodes until the ring is full or input runs out, as the hardware does between two of the
// title's audio frames.
//
// Not modelled: sub-frame loop points (loops jump at frame boundaries), multi-stream packet
// skipping (TTT2 contexts are single streams), error/parser status bits.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace xma {

constexpr uint32_t kContextCount = 320;
constexpr uint32_t kContextBytes = 64;
constexpr uint32_t kRegisterBase = 0x7FEA0000u;
constexpr uint32_t kRegContextArray = kRegisterBase + 0x1800;   // read with lwbrx
constexpr uint32_t kRegKick = kRegisterBase + 0x1940;           // 10 words, stwbrx 1 << (index % 32)
constexpr uint32_t kPacketBytes = 2048;
constexpr uint32_t kPacketBits = kPacketBytes * 8;
constexpr uint32_t kPacketHeaderBits = 32;
constexpr uint32_t kPayloadBits = kPacketBits - kPacketHeaderBits;
constexpr uint32_t kFrameLengthBits = 15;
constexpr uint32_t kFramePadding = 0x7FFF;
constexpr uint32_t kBlockBytes = 256;
constexpr unsigned kRates[4] = { 24000, 32000, 44100, 48000 };   // context sample-rate codes

inline uint64_t SteadyMs()
{
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

inline uint32_t Bits(uint32_t v, unsigned lo, unsigned n) { return (v >> lo) & ((1u << n) - 1u); }
inline uint32_t WithBits(uint32_t v, unsigned lo, unsigned n, uint32_t x)
{
    const uint32_t mask = ((1u << n) - 1u) << lo;
    return (v & ~mask) | ((x << lo) & mask);
}
// Bits that finish the previous packet's last frame (the header's 15-bit frame offset).
inline uint32_t PacketContinuation(const uint8_t* packet)
{
    return ((packet[0] & 3u) << 13) | (uint32_t(packet[1]) << 5) | (packet[2] >> 3);
}
inline uint32_t ReadBits(const uint8_t* data, uint32_t bit, uint32_t n)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; ++i, ++bit)
        v = (v << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1u);
    return v;
}

// Copies n bits, most significant first, from src at bit `from` to dst at bit `to`: whole bytes once dst is byte
// aligned. Bit by bit, assembling and repacking frames cost as much as decoding them (~1.5% of the sound thread).
inline void CopyBits(uint8_t* dst, uint32_t to, const uint8_t* src, uint32_t from, uint32_t n)
{
    auto copyBit = [&] {
        const uint8_t mask = uint8_t(0x80u >> (to & 7));
        if ((src[from >> 3] >> (7 - (from & 7))) & 1u) dst[to >> 3] |= mask;
        else dst[to >> 3] &= uint8_t(~mask);
        ++from, ++to, --n;
    };
    while (n && (to & 7)) copyBit();
    const uint32_t bytes = n >> 3, shift = from & 7;
    const uint8_t* s = src + (from >> 3);
    uint8_t* d = dst + (to >> 3);
    if (!shift) std::memcpy(d, s, bytes);
    else
        for (uint32_t i = 0; i < bytes; ++i) d[i] = uint8_t(s[i] << shift | s[i + 1] >> (8 - shift));
    from += bytes * 8, to += bytes * 8, n -= bytes * 8;
    while (n) copyBit();
}

// First frame start at or after `bit` (buffer coordinates), found by walking the length
// prefixes from the first frame of the packet that holds it. Loop starts need not sit exactly
// on a frame: a TTT2 ambience loop gives 0x664B for the frame at 0x664C (its sub-frame fields
// pick sub-frame 2 of that frame, which this core does not model).
inline uint32_t FrameStartAtOrAfter(const uint8_t* data, uint32_t packets, uint32_t bit)
{
    auto toBuffer = [](uint32_t p) { return p / kPayloadBits * kPacketBits + kPacketHeaderBits + p % kPayloadBits; };
    auto continuation = [&](uint32_t packet) { return std::min(PacketContinuation(data + packet * kPacketBytes), kPayloadBits); };
    const uint32_t packet = bit / kPacketBits, total = packets * kPayloadBits;
    if (packet >= packets) return bit;
    const uint32_t inPacket = bit % kPacketBits;
    const uint32_t target = packet * kPayloadBits + (inPacket > kPacketHeaderBits ? inPacket - kPacketHeaderBits : 0);
    uint32_t p = packet * kPayloadBits + continuation(packet);
    for (int guard = 0; guard < 4096 && p + kFrameLengthBits <= total; ++guard)
    {
        if (p >= target) return toBuffer(p);
        const uint32_t pk = p / kPayloadBits;
        uint32_t length = 0;
        for (uint32_t i = 0; i < kFrameLengthBits; ++i)
        {
            const uint32_t b = toBuffer(p + i);
            length = (length << 1) | ((data[b >> 3] >> (7 - (b & 7))) & 1u);
        }
        const bool straddlesIntoPadding = kPayloadBits - p % kPayloadBits < kFrameLengthBits &&
                                          (pk + 1 >= packets || !continuation(pk + 1));
        if (length == kFramePadding || length <= kFrameLengthBits || straddlesIntoPadding)
        {
            if (pk + 1 >= packets) break;
            p = (pk + 1) * kPayloadBits + continuation(pk + 1);
            continue;
        }
        p += length;
    }
    return bit;
}

struct Stats
{
    uint64_t kicks = 0, frames = 0, packets = 0, bytesOut = 0, nonzeroBlocks = 0;
    uint64_t resets = 0, sendErrors = 0, decodeErrors = 0, loops = 0, ringFull = 0;
    uint64_t carries = 0, badFrames = 0;
};

class Decoder
{
public:
    ~Decoder() { for (auto& h : host_) Close(h); }

    // base = host address of guest 0; array = guest address of the 320 x 64-byte context array.
    void Attach(uint8_t* base, uint32_t array) { base_ = base; array_ = array; }
    uint32_t Array() const { return array_; }

    // Index of a context address, or -1.
    int IndexOf(uint32_t guest) const
    {
        if (guest < array_ || guest >= array_ + kContextCount * kContextBytes || (guest - array_) % kContextBytes)
            return -1;
        return int((guest - array_) / kContextBytes);
    }

    // XMACreateContext: first free slot, zeroed. Returns its guest address (0 when full).
    uint32_t Allocate()
    {
        for (uint32_t i = 0; i < kContextCount; ++i)
            if (!host_[i].allocated)
            {
                host_[i].allocated = true;
                ResetHost(host_[i]);
                host_[i].createdMs = SteadyMs();
                host_[i].lastKickMs = host_[i].lastOutputMs = host_[i].totalKicks = 0;
                std::memset(base_ + array_ + i * kContextBytes, 0, kContextBytes);
                return array_ + i * kContextBytes;
            }
        return 0;
    }
    void Release(uint32_t guest)
    {
        const int i = IndexOf(guest);
        if (i < 0) return;
        host_[i].allocated = false;
        ResetHost(host_[i]);
        std::memset(base_ + guest, 0, kContextBytes);
    }

    // One kick of the context at `guest`: decode until its output ring is full or input ends.
    void Kick(uint32_t guest, int engine = 0)
    {
        const int index = IndexOf(guest);
        if (index < 0) return;
        Host& h = host_[index];
        h.index = index;
        h.engine = engine;
        h.lastKickMs = SteadyMs();
        if (!h.winFirstMs) h.winFirstMs = h.lastKickMs, h.winFirstFill = true;
        h.winLastMs = h.lastKickMs;
        ++h.totalKicks;
        ++stats_.kicks;
        uint32_t dw[10];
        uint8_t* raw = base_ + guest;
        for (int i = 0; i < 10; ++i) dw[i] = Load(raw + i * 4);
        // Diagnostics: a stereo voice (TTT2's music streams) taken over by another sound.
        if (logKicks_ && h.haveLast && Bits(h.lastDw[1], 29, 1) && Bits(h.lastDw[1], 27, 3) != Bits(dw[1], 27, 3))
            std::fprintf(stderr, "[xma] voice %d reassigned after %llu ms: was stereo in0=%08X/%u rd=%X valid=%u -> %s %uHz in0=%08X/%u\n",
                         index, (unsigned long long)SteadyMs(), h.lastDw[5], Bits(h.lastDw[0], 0, 12), Bits(h.lastDw[2], 0, 26),
                         Bits(h.lastDw[0], 20, 2), Bits(dw[1], 29, 1) ? "stereo" : "mono", kRates[Bits(dw[1], 27, 2)],
                         dw[5], Bits(dw[0], 0, 12));

        // A new sound (or a seek) on this context: the title rewrote the stream position or the
        // output ring. Drop what the decoder carried over from the old stream. A buffer handed
        // over after the previous one ran dry is NOT a new sound: TTT2 streams music one 64 KB
        // buffer at a time (re-pointing input 0, read offset 32), and the decoder state must
        // carry on across those or each switch loses a frame (measured: music ran 0.39% fast).
        if (h.haveLast && (dw[2] != h.lastReadOffset || Bits(dw[0], 27, 5) != Bits(h.lastDw0, 27, 5)))
        {
            if (logKicks_ && stats_.resets < 40)
                std::fprintf(stderr, "[xma] ctx %d reset: rd %X -> %X, write %u -> %u, in0=%08X/%u in1=%08X/%u valid %u\n",
                             index, h.lastReadOffset, dw[2], Bits(h.lastDw0, 27, 5), Bits(dw[0], 27, 5),
                             dw[5], Bits(dw[0], 0, 12), dw[6], Bits(dw[1], 0, 12), Bits(dw[0], 20, 2));
            ResetHost(h);
            ++stats_.resets;
        }
        if (logKicks_ && h.logged < 6)
        {
            ++h.logged;
            std::fprintf(stderr, "[xma] ctx %d kick dw0=%08X dw1=%08X rd=%X loop=%X..%X dw4=%08X in0=%08X in1=%08X out=%08X dw9=%08X\n",
                         index, dw[0], dw[1], dw[2], dw[3], dw[4] & 0x3FFFFFF, dw[4], dw[5], dw[6], dw[7], dw[9]);
        }
        ++h.periodKicks;
        if (Bits(dw[1], 31, 1))
            Fill(h, dw);
        else
            ++h.periodNotValid;
        std::memcpy(h.lastDw, dw, sizeof(h.lastDw));
        h.haveLast = true;
        h.lastReadOffset = dw[2];
        h.lastDw0 = dw[0];
        for (int i = 0; i < 10; ++i) Store(raw + i * 4, dw[i]);
    }

    const Stats& GetStats() const { return stats_; }

    // One context's lifetime and last kicked state (diagnostics for create/release logging).
    struct ContextInfo
    {
        int index = -1;
        unsigned live = 0;                       // allocated contexts
        uint64_t ageMs = 0, idleMs = 0, silentMs = 0, kicks = 0;   // idle: since last kick; silent: since last output
        uint32_t dw[10] = {};
    };
    ContextInfo Info(uint32_t guest) const
    {
        ContextInfo info;
        for (const Host& h : host_) info.live += h.allocated;
        info.index = IndexOf(guest);
        if (info.index < 0) return info;
        const Host& h = host_[info.index];
        const uint64_t now = SteadyMs();
        info.ageMs = h.createdMs ? now - h.createdMs : 0;
        info.idleMs = h.lastKickMs ? now - h.lastKickMs : info.ageMs;
        info.silentMs = h.lastOutputMs ? now - h.lastOutputMs : info.ageMs;
        info.kicks = h.totalKicks;
        std::memcpy(info.dw, h.lastDw, sizeof(info.dw));
        return info;
    }

    // One line per voice kicked since the last call: kicks, 256-byte blocks written, kicks that
    // found room but no input ("starved"), kicks with "output valid" clear, and its state.
    void ReportVoices(std::FILE* out)
    {
        // A sample on 10+ voices in one period: one-shots piling up instead of ending (NUSound
        // ends one only after frames x 512 samples; a short decoder leaked every voice until the
        // pool ran out and the music was re-voiced). Dump it once as xma_flood_<address>.bin.
        {
            std::vector<std::pair<uint32_t, uint32_t>> inputs;   // (address, packets)
            for (uint32_t i = 0; i < kContextCount; ++i)
                if (host_[i].periodKicks && host_[i].lastDw[5])
                    inputs.push_back({ host_[i].lastDw[5], Bits(host_[i].lastDw[0], 0, 12) });
            std::sort(inputs.begin(), inputs.end());
            for (size_t a = 0; a < inputs.size();)
            {
                size_t b = a;
                while (b < inputs.size() && inputs[b].first == inputs[a].first) ++b;
                static std::vector<uint32_t> dumped;
                if (b - a >= 10 && std::find(dumped.begin(), dumped.end(), inputs[a].first) == dumped.end())
                {
                    dumped.push_back(inputs[a].first);
                    char path[64];
                    std::snprintf(path, sizeof(path), "xma_flood_%08X.bin", inputs[a].first);
                    if (std::FILE* f = std::fopen(path, "wb"))
                    {
                        std::fwrite(base_ + inputs[a].first, kPacketBytes, std::min<uint32_t>(inputs[a].second, 64), f);
                        std::fclose(f);
                    }
                    std::fprintf(out, "[xma] FLOOD sample %08X (%u packets) on %zu voices\n",
                                 inputs[a].first, inputs[a].second, b - a);
                }
                a = b;
            }
        }
        for (uint32_t i = 0; i < kContextCount; ++i)
        {
            Host& h = host_[i];
            if (!h.periodKicks) continue;
            const uint32_t* dw = h.lastDw;
            std::fprintf(out, "[xma] voice %u %s kicks=%llu blocks=%llu starved=%llu notvalid=%llu valid=%u in0=%08X/%u in1=%08X/%u cur=%u rd=%X loops=%u %s %uHz\n",
                         i, h.engine == 1 ? "nus" : h.engine == 2 ? "xa2" : "?", (unsigned long long)h.periodKicks, (unsigned long long)h.periodBlocks,
                         (unsigned long long)h.periodStarved, (unsigned long long)h.periodNotValid,
                         Bits(dw[0], 20, 2), dw[5], Bits(dw[0], 0, 12), dw[6], Bits(dw[1], 0, 12),
                         Bits(dw[4], 31, 1), Bits(dw[2], 0, 26), Bits(dw[0], 12, 8),
                         Bits(dw[1], 29, 1) ? "stereo" : "mono", kRates[Bits(dw[1], 27, 2)]);
            h.periodKicks = h.periodBlocks = h.periodStarved = h.periodNotValid = 0;
        }
    }
    // The music (NUSound's busiest stereo voice since the last call): how fast its stream was decoded against
    // the clock. Decoding stops when the voice's ring is full, so this is how fast the game played it: below
    // 100% the music ran slow (no input in time, or NUSound mixed late: [snd-nus]). Skipped under 5 s of play.
    void ReportMusic(std::FILE* out)
    {
        int best = -1;
        for (uint32_t i = 0; i < kContextCount; ++i)
            if (host_[i].engine == 1 && host_[i].channels == 2 && host_[i].winBlocks &&
                (best < 0 || host_[i].winBlocks > host_[best].winBlocks))
                best = int(i);
        if (best >= 0)
        {
            const Host& h = host_[best];
            const uint64_t spanMs = h.winLastMs - h.winFirstMs;
            const double samples = double(h.winBlocks) * kBlockBytes / 4.0;   // stereo, 16-bit
            const uint32_t rate = kRates[h.rateCode];
            if (spanMs >= 5000)
                std::fprintf(out, "[xma] music: voice %d %u Hz played at %.1f%% of real time over %.1f s (no input at %llu kicks)\n",
                             best, rate, samples * 1000.0 / double(spanMs) / rate * 100.0, spanMs / 1000.0,
                             (unsigned long long)h.winDry);
        }
        for (Host& h : host_) h.winBlocks = h.winDry = h.winFirstMs = h.winLastMs = 0, h.winFirstFill = false;
    }
    void SetLogKicks(bool on) { logKicks_ = on; }

private:
    struct Host
    {
        bool allocated = false;
        AVCodecContext* codec = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        int channels = 0;
        uint32_t rateCode = 0;
        std::vector<uint8_t> pending;   // decoded big-endian PCM not yet in the ring (whole blocks)
        size_t pendingPos = 0;
        // Frame being assembled from the input bit stream (MSB-first bits).
        std::vector<uint8_t> frameBits = std::vector<uint8_t>(4096 + 8);
        uint32_t have = 0, frameLength = 0;
        bool enteringBuffer = false;    // dw2 points at a buffer whose first header is unread
        bool haveLast = false;
        int index = 0;
        uint32_t lastFrameEnd = 0, loopStart = 0, loopEnd = 0, loopCount = 0;   // diagnostics
        uint32_t lastReadOffset = 0, lastDw0 = 0;
        int logged = 0;
        // Per-voice activity since the last ReportVoices (diagnostics).
        uint64_t periodKicks = 0, periodBlocks = 0, periodStarved = 0, periodNotValid = 0;
        // Since the last ReportMusic: blocks written after the first decode (the span runs from the first kick to
        // the last), kicks that found room but no input, first and last kick.
        uint64_t winBlocks = 0, winDry = 0, winFirstMs = 0, winLastMs = 0;
        bool winFirstFill = false;
        int engine = 0;                 // 1 = NUSound kick, 2 = XAudio2 kick
        uint32_t lastDw[10] = {};
        // Lifetime (diagnostics): allocation, last kick, last kick that produced output.
        uint64_t createdMs = 0, lastKickMs = 0, lastOutputMs = 0, totalKicks = 0;
    };

    static uint32_t Load(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
    static void Store(uint8_t* p, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(p, &v, 4); }

    static void Close(Host& h)
    {
        if (h.codec) avcodec_free_context(&h.codec);
        if (h.packet) av_packet_free(&h.packet);
        if (h.frame) av_frame_free(&h.frame);
        h.channels = 0;
    }
    static void ResetHost(Host& h)
    {
        if (h.codec) avcodec_flush_buffers(h.codec);
        h.pending.clear();
        h.pendingPos = 0;
        h.have = h.frameLength = 0;
        h.enteringBuffer = false;
        h.haveLast = false;
    }

    bool Open(Host& h, int channels, uint32_t rateCode)
    {
        Close(h);
        static const int rates[4] = { 24000, 32000, 44100, 48000 };
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
        if (!codec) return false;
        h.codec = avcodec_alloc_context3(codec);
        h.packet = av_packet_alloc();
        h.frame = av_frame_alloc();
        if (!h.codec || !h.packet || !h.frame) { Close(h); return false; }
        av_channel_layout_default(&h.codec->ch_layout, channels);
        h.codec->sample_rate = rates[rateCode & 3];   // selects the codec's band layout
        h.codec->block_align = kPacketBytes;
        // Keep every frame 512 samples: the hardware has no encoder-delay trim, and the ring
        // advances in whole 256-byte blocks.
        h.codec->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
        // XMA2WAVEFORMATEX tail: one stream, mono = front centre, stereo = front left/right.
        uint8_t* extra = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!extra) { Close(h); return false; }
        extra[0] = 1;
        extra[2] = channels == 2 ? 0x03 : 0x04;
        h.codec->extradata = extra;
        h.codec->extradata_size = 34;
        if (avcodec_open2(h.codec, codec, nullptr) < 0) { Close(h); return false; }
        h.channels = channels;
        return true;
    }

    uint8_t* Guest(uint32_t address) const { return base_ + address; }

    // Input buffer `which` fully read: clear its valid bit, continue at the other buffer's start.
    static void ConsumeBuffer(Host& h, uint32_t* dw, uint32_t which)
    {
        dw[0] = WithBits(dw[0], 20 + which, 1, 0);
        dw[4] = WithBits(dw[4], 31, 1, which ^ 1);
        dw[2] = WithBits(dw[2], 0, 26, kPacketHeaderBits);
        h.enteringBuffer = true;
    }

    void AppendBits(Host& h, const uint8_t* data, uint32_t bit, uint32_t n)
    {
        CopyBits(h.frameBits.data(), h.have, data, bit, n);
        h.have += n;
    }

    // Walk the input bit stream to the end of the next frame (into h.frameBits). False when the
    // input runs out first; a partial frame stays in h for the next kick.
    bool AssembleFrame(Host& h, uint32_t* dw)
    {
        for (int guard = 0; guard < 1 << 16; ++guard)
        {
            uint32_t current = Bits(dw[4], 31, 1);
            if (!Bits(dw[0], 20 + current, 1))
            {
                if (!Bits(dw[0], 20 + (current ^ 1), 1)) return false;
                // The title set only the other buffer: continue there.
                current ^= 1;
                dw[4] = WithBits(dw[4], 31, 1, current);
                dw[2] = WithBits(dw[2], 0, 26, kPacketHeaderBits);
                h.enteringBuffer = true;
            }
            const uint32_t count = current ? Bits(dw[1], 0, 12) : Bits(dw[0], 0, 12);
            const uint32_t address = current ? dw[6] : dw[5];
            uint32_t pos = Bits(dw[2], 0, 26);
            if (!count || !address || pos >= count * kPacketBits)
            {
                ConsumeBuffer(h, dw, current);
                continue;
            }
            const uint8_t* data = Guest(address);
            const uint32_t packet = pos / kPacketBits;
            const uint32_t packetEnd = (packet + 1) * kPacketBits;
            if (pos % kPacketBits < kPacketHeaderBits || h.enteringBuffer)
            {
                // Entering a packet: its header gives the bits that finish the frame begun before it.
                h.enteringBuffer = false;
                const uint32_t continuation = std::min(PacketContinuation(data + packet * kPacketBytes), kPayloadBits);
                pos = packet * kPacketBits + kPacketHeaderBits;
                if (h.have && !continuation)
                {
                    h.have = h.frameLength = 0;          // what we held was padding
                }
                else if (!h.have)
                    pos += continuation;                 // the tail of a frame we did not see begin
                dw[2] = WithBits(dw[2], 0, 26, pos);
                continue;
            }
            if (!h.have && packetEnd - pos >= kFrameLengthBits)
            {
                const uint32_t length = ReadBits(data, pos, kFrameLengthBits);
                if (length == kFramePadding || length <= kFrameLengthBits)
                {
                    dw[2] = WithBits(dw[2], 0, 26, packetEnd);   // rest of the packet is padding
                    continue;
                }
            }
            const uint32_t need = h.frameLength ? h.frameLength - h.have : kFrameLengthBits - h.have;
            const uint32_t n = std::min(need, packetEnd - pos);
            AppendBits(h, data, pos, n);
            pos += n;
            dw[2] = WithBits(dw[2], 0, 26, pos);
            if (!h.frameLength && h.have >= kFrameLengthBits)
            {
                h.frameLength = ReadBits(h.frameBits.data(), 0, kFrameLengthBits);
                if (h.frameLength == kFramePadding || h.frameLength <= kFrameLengthBits)
                {
                    h.have = h.frameLength = 0;          // padding whose length straddled a header
                    continue;
                }
            }
            if (h.frameLength && h.have == h.frameLength)
            {
                h.lastFrameEnd = pos;
                h.loopStart = Bits(dw[3], 0, 26), h.loopEnd = Bits(dw[4], 0, 26), h.loopCount = Bits(dw[0], 12, 8);
                // Loop region: the frame that ends at the loop end is followed by the loop start.
                const uint32_t loops = Bits(dw[0], 12, 8), loopEnd = Bits(dw[4], 0, 26);
                if (loops && loopEnd && pos >= loopEnd)
                {
                    dw[2] = WithBits(dw[2], 0, 26, FrameStartAtOrAfter(data, count, Bits(dw[3], 0, 26)));
                    if (loops != 255) dw[0] = WithBits(dw[0], 12, 8, loops - 1);
                    ++stats_.loops;
                }
                return true;
            }
            if (n == 0) return false;
        }
        return false;
    }

    // Hand the assembled frame to the decoder alone in its own packet(s): header (1 frame
    // starting at bit 0, or the bits continuing it), the frame, then padding ones.
    bool SendFrame(Host& h)
    {
        const uint32_t length = h.frameLength;
        for (uint32_t sent = 0; sent < length;)
        {
            const uint32_t n = std::min(length - sent, kPayloadBits);
            av_packet_unref(h.packet);
            if (av_new_packet(h.packet, int(kPacketBytes)) < 0) return false;
            uint8_t* p = h.packet->data;
            std::memset(p, 0xFF, kPacketBytes);
            const uint32_t frames = sent ? 0 : 1, continuation = sent ? std::min(length - sent, 0x7FFFu) : 0;
            const uint32_t header = (frames << 26) | (continuation << 11);
            p[0] = uint8_t(header >> 24); p[1] = uint8_t(header >> 16); p[2] = uint8_t(header >> 8); p[3] = uint8_t(header);
            CopyBits(p, kPacketHeaderBits, h.frameBits.data(), sent, n);
            const int r = avcodec_send_packet(h.codec, h.packet);
            if (r < 0)
            {
                ++stats_.sendErrors;
                if (logKicks_ && stats_.sendErrors <= 12)
                {
                    char text[64] = {};
                    av_strerror(r, text, sizeof(text));
                    std::fprintf(stderr, "[xma] ctx %d send error %d (%s): frame %u bits ending at %X, part at %u, channels %d, rate %d, loop %X..%X x%u\n",
                                 h.index, r, text, length, h.lastFrameEnd, sent, h.channels, h.codec->sample_rate,
                                 h.loopStart, h.loopEnd, h.loopCount);
                    // Diagnostic capture for offline replay: the context and its input buffers.
                    char path[64];
                    std::snprintf(path, sizeof(path), "xma_error_ctx%d.bin", h.index);
                    if (std::FILE* f = std::fopen(path, "wb"))
                    {
                        const uint8_t* ctxBytes = base_ + array_ + h.index * kContextBytes;
                        std::fwrite(ctxBytes, 1, kContextBytes, f);
                        for (int b = 0; b < 2; ++b)
                        {
                            const uint32_t dw0 = Load(ctxBytes), dw1 = Load(ctxBytes + 4);
                            const uint32_t count = b ? Bits(dw1, 0, 12) : Bits(dw0, 0, 12);
                            const uint32_t address = Load(ctxBytes + 20 + 4 * b);
                            const uint32_t n = address ? count : 0;
                            std::fwrite(&n, 4, 1, f);
                            if (n) std::fwrite(base_ + address, kPacketBytes, n, f);
                        }
                        std::fclose(f);
                    }
                }
            }
            ++stats_.packets;
            sent += n;
        }
        return true;
    }

    // Decode one more frame into h.pending. False when no input is left.
    bool DecodeFrame(Host& h, uint32_t* dw)
    {
        for (int guard = 0; guard < 64; ++guard)
        {
            const int r = avcodec_receive_frame(h.codec, h.frame);
            if (r == 0)
            {
                AppendFrame(h);
                return true;
            }
            if (r != AVERROR(EAGAIN))
            {
                ++stats_.decodeErrors;
                avcodec_flush_buffers(h.codec);
            }
            if (!AssembleFrame(h, dw)) return false;
            if (h.have != h.frameLength || h.frameLength > 4096 * 8) ++stats_.badFrames;
            else SendFrame(h);
            h.have = h.frameLength = 0;
        }
        return false;
    }

    void AppendFrame(Host& h)
    {
        AVFrame* f = h.frame;
        const int channels = std::min(h.channels, f->ch_layout.nb_channels);
        const int samples = f->nb_samples;
        if (h.pendingPos == h.pending.size()) { h.pending.clear(); h.pendingPos = 0; }
        const size_t start = h.pending.size();
        size_t bytes = size_t(samples) * h.channels * 2;
        bytes = (bytes + kBlockBytes - 1) / kBlockBytes * kBlockBytes;
        h.pending.resize(start + bytes, 0);
        uint8_t* out = h.pending.data() + start;
        bool nonzero = false;
        for (int s = 0; s < samples; ++s)
            for (int c = 0; c < h.channels; ++c)
            {
                const float v = c < channels ? reinterpret_cast<const float*>(f->extended_data[c])[s] : 0.0f;
                long q = std::lrintf(v * 32768.0f);
                q = q > 32767 ? 32767 : q < -32768 ? -32768 : q;
                const uint16_t u = uint16_t(int16_t(q));
                *out++ = uint8_t(u >> 8);
                *out++ = uint8_t(u);
                nonzero |= q != 0;
            }
        stats_.frames += (samples + 511) / 512;
        if (nonzero) stats_.nonzeroBlocks += bytes / kBlockBytes;
    }

    void Fill(Host& h, uint32_t* dw)
    {
        // The sample rate selects the codec's band layout, so a voice reused for a sound of
        // another rate (TTT2 mixes 24/32/44.1/48 kHz banks) needs a decoder opened for it.
        const int channels = Bits(dw[1], 29, 1) ? 2 : 1;
        const uint32_t rateCode = Bits(dw[1], 27, 2);
        if (!h.codec || h.channels != channels || h.rateCode != rateCode)
        {
            const bool hadDecoder = h.codec != nullptr;
            if (!Open(h, channels, rateCode)) return;
            h.rateCode = rateCode;
            if (hadDecoder) { h.pending.clear(); h.pendingPos = 0; }
        }
        const uint32_t capacity = Bits(dw[0], 22, 5) * kBlockBytes;
        if (!capacity) return;
        uint32_t write = Bits(dw[0], 27, 5) * kBlockBytes;
        const uint32_t read = Bits(dw[9], 0, 5) * kBlockBytes;
        if (write >= capacity || read >= capacity) return;
        uint32_t space = read > write ? read - write : capacity - write + read;
        uint8_t* out = Guest(dw[7]);
        uint32_t written = 0;
        while (space >= kBlockBytes)
        {
            if (h.pendingPos == h.pending.size() && !DecodeFrame(h, dw)) break;
            uint32_t n = uint32_t(std::min<size_t>(space, h.pending.size() - h.pendingPos));
            n -= n % kBlockBytes;
            if (!n) break;
            const uint32_t first = std::min(n, capacity - write);
            std::memcpy(out + write, h.pending.data() + h.pendingPos, first);
            std::memcpy(out, h.pending.data() + h.pendingPos + first, n - first);
            h.pendingPos += n;
            write = (write + n) % capacity;
            space -= n;
            written += n;
        }
        stats_.bytesOut += written;
        h.periodBlocks += written / kBlockBytes;
        if (h.winFirstFill) h.winFirstFill = false;
        else h.winBlocks += written / kBlockBytes;
        if (written) h.lastOutputMs = SteadyMs();
        if (!written && space >= kBlockBytes) ++h.periodStarved, ++h.winDry;   // room in the ring, no input
        dw[0] = WithBits(dw[0], 27, 5, write / kBlockBytes);
        // A full ring reads as write == read; the hardware then drops "output valid" until the
        // title has consumed blocks and set it again.
        if (written && write == read)
        {
            dw[1] = WithBits(dw[1], 31, 1, 0);
            ++stats_.ringFull;
        }
    }

    uint8_t* base_ = nullptr;
    uint32_t array_ = 0;
    Host host_[kContextCount];
    Stats stats_;
    bool logKicks_ = false;
};

} // namespace xma
