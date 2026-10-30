// XMA hardware emulation, guest side (decoder: xma_core.h).
//
// TTT2 talks to the audio block's registers directly instead of through kernel calls, so the
// port answers in three places:
//  - XmaInit puts the context array in physical memory and its address in register 0x1800,
//    which the title's sound engines read once (sub_82C28D58 for XAudio2, NUSound likewise)
//    to turn a context address into its hardware index.
//  - XMACreateContext / XMAReleaseContext hand out and return array slots.
//  - The two functions that kick contexts are wrapped: after the original has written the
//    voices' context data and set their kick bits, every context of its voice list is decoded
//    until its output ring is full. Both run once per audio frame from the title's mixer, which
//    reads the contexts back (consumed buffers, new write offset) on its next frame.
// TTT2_XMA=0 leaves contexts undecoded (silence, the previous behaviour); TTT2_XMA_LOG=1 logs
// the first kicks of each context. Totals go to the log every 10 s ([xma] kicks=...).
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "guest_heap.h"
#include "xma.h"
#include "xma_core.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" PPC_FUNC(__imp__sub_82D3E120);   // NUSound: copy voice contexts out, kick them
extern "C" PPC_FUNC(__imp__sub_82C29750);   // XAudio2: copy dirty context parts out, kick them

namespace {
constexpr uint32_t STATUS_NO_MEMORY_ = 0xC0000017u;
constexpr uint32_t VOICE_ENTRY_BYTES = 96;    // voice list entry; +64 = context address
constexpr uint32_t VOICE_CONTEXT_OFFSET = 64;

std::mutex g_xmaMutex;
xma::Decoder g_xma;
bool g_xmaDecode = true;
std::chrono::steady_clock::time_point g_nextReport{};

bool EnvIs(const char* name, const char* value)
{
    const char* e = std::getenv(name);
    return e && std::strcmp(e, value) == 0;
}

void Report()
{
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextReport) return;
    g_nextReport = now + std::chrono::seconds(10);
    const auto& s = g_xma.GetStats();
    std::fprintf(stderr, "[xma] kicks=%llu frames=%llu out_kb=%llu nonzero_blocks=%llu resets=%llu ring_full=%llu loops=%llu errors=%llu/%llu/%llu\n",
                 (unsigned long long)s.kicks, (unsigned long long)s.frames,
                 (unsigned long long)(s.bytesOut >> 10), (unsigned long long)s.nonzeroBlocks,
                 (unsigned long long)s.resets, (unsigned long long)s.ringFull, (unsigned long long)s.loops,
                 (unsigned long long)s.sendErrors, (unsigned long long)s.decodeErrors,
                 (unsigned long long)s.badFrames);
    static const bool voices = EnvIs("TTT2_XMA_LOG", "1");
    if (voices) g_xma.ReportVoices(stderr);
}

// Guest return addresses up the stack from r1 (each frame's saved LR sits 8 bytes below the
// back chain it links to), appended to `line` (diagnostics).
int AppendBacktrace(const PPCContext& ctx, uint8_t* base, char* line, int n, int size)
{
    uint32_t frame = ctx.r1.u32;
    for (int i = 0; i < 12 && frame && frame < 0xFFFFFFF0u && n < size - 12; ++i)
    {
        const uint32_t prev = GRead32(base, frame);
        if (!prev || prev <= frame || prev - frame > 0x10000) break;
        n += std::snprintf(line + n, size - n, " %08X", GRead32(base, prev - 8));
        frame = prev;
    }
    return n;
}

// TTT2_XMA_LOG=1: one line per XMACreateContext / XMAReleaseContext with the live count, the
// context's lifetime (age, time since its last kick and last output) and the guest backtrace.
void LogContextEvent(const char* what, uint32_t context, const PPCContext& ctx, uint8_t* base)
{
    static const bool log = EnvIs("TTT2_XMA_LOG", "1");
    if (!log) return;
    const auto info = g_xma.Info(context);
    const uint32_t* dw = info.dw;
    char line[640];
    int n = std::snprintf(line, sizeof(line),
                          "[xma] %s ctx %d live=%u t=%llu age=%llu idle=%llu silent=%llu kicks=%llu %s %uHz in0=%08X/%u valid=%u lr=%08X bt:",
                          what, info.index, info.live, (unsigned long long)xma::SteadyMs(),
                          (unsigned long long)info.ageMs, (unsigned long long)info.idleMs,
                          (unsigned long long)info.silentMs, (unsigned long long)info.kicks,
                          xma::Bits(dw[1], 29, 1) ? "stereo" : "mono", xma::kRates[xma::Bits(dw[1], 27, 2)],
                          dw[5], xma::Bits(dw[0], 0, 12), xma::Bits(dw[0], 20, 2), uint32_t(ctx.lr));
    n = AppendBacktrace(ctx, base, line, n, int(sizeof(line)));
    std::snprintf(line + n, sizeof(line) - n, "\n");
    std::fputs(line, stderr);
}

// Decode every context of a voice list ({count, flags, entries}) the title just kicked.
void KickVoiceList(uint8_t* base, uint32_t list, int engine)
{
    if (!g_xmaDecode || !list) return;
    const uint32_t count = GRead32(base, list);
    const uint32_t entries = GRead32(base, list + 8);
    if (!entries) return;
    std::lock_guard lock(g_xmaMutex);
    for (uint32_t i = 0; i < count && i < xma::kContextCount; ++i)
        g_xma.Kick(GRead32(base, entries + i * VOICE_ENTRY_BYTES + VOICE_CONTEXT_OFFSET), engine);
    Report();
}
}

void XmaInit(uint8_t* base)
{
    void* array = g_physicalHeap.Alloc(xma::kContextCount * xma::kContextBytes, 0x1000);
    if (!array)
    {
        std::fprintf(stderr, "[xma] context array allocation failed; audio stays silent\n");
        g_xmaDecode = false;
        return;
    }
    std::memset(array, 0, xma::kContextCount * xma::kContextBytes);
    const uint32_t guest = GuestAddr(base, array);
    // The title reads this register with lwbrx (byte-reversed load): keep it in host order.
    std::memcpy(base + xma::kRegContextArray, &guest, 4);
    g_xma.Attach(base, guest);
    g_xma.SetLogKicks(EnvIs("TTT2_XMA_LOG", "1"));
    g_xmaDecode = !EnvIs("TTT2_XMA", "0");
    av_log_set_level(AV_LOG_QUIET);   // decode problems are counted in the [xma] stats
    std::fprintf(stderr, "[xma] context array %08X (%u contexts), decoding %s\n",
                 guest, xma::kContextCount, g_xmaDecode ? "on (FFmpeg XMA2)" : "off (TTT2_XMA=0)");
}

// XMACreateContext(r3 = &context OUT) -> status.
PPC_FUNC(__imp__XMACreateContext)
{
    KTrace("XMACreateContext");
    uint32_t context = 0;
    {
        std::lock_guard lock(g_xmaMutex);
        if (g_xma.Array()) context = g_xma.Allocate();
        LogContextEvent("create", context, ctx, base);
    }
    if (ctx.r3.u32) GWrite32(base, ctx.r3.u32, context);
    ctx.r3.u32 = context ? STATUS_SUCCESS : STATUS_NO_MEMORY_;
}

// XMAReleaseContext(r3 = context).
PPC_FUNC(__imp__XMAReleaseContext)
{
    KTrace("XMAReleaseContext");
    std::lock_guard lock(g_xmaMutex);
    LogContextEvent("release", ctx.r3.u32, ctx, base);
    g_xma.Release(ctx.r3.u32);
}

PPC_FUNC(sub_82D3E120)
{
    const uint32_t list = ctx.r3.u32;
    __imp__sub_82D3E120(ctx, base);
    const uint32_t result = ctx.r3.u32;
    KickVoiceList(base, list, 1);   // NUSound
    ctx.r3.u32 = result;
}

PPC_FUNC(sub_82C29750)
{
    const uint32_t list = ctx.r3.u32;
    __imp__sub_82C29750(ctx, base);
    KickVoiceList(base, list, 2);   // XAudio2
}

// Diagnostics (TTT2_XMA_TRACE=<hex guest address of a sound's XMA data>): NUSound's XMA plugin
// update sub_82D089A0(voice r3, samples wanted r4, channel buffers r5, out produced r6,
// out position r7, out flags r8) for voices playing that data, with the fields that decide
// "finished" (flags |= 1 once position +12 reaches the sample count +32 with no loop
// region, +40 == +44). One line whenever position, flags or output change, capped.
extern "C" PPC_FUNC(__imp__sub_82D089A0);
PPC_FUNC(sub_82D089A0)
{
    static const uint32_t trace = [] {
        const char* e = std::getenv("TTT2_XMA_TRACE");
        return e ? uint32_t(std::strtoul(e, nullptr, 16)) : 0u;
    }();
    const uint32_t voice = ctx.r3.u32;
    if (!trace || !voice || GRead32(base, voice) + GRead32(base, voice + 8) != trace)
    {
        __imp__sub_82D089A0(ctx, base);
        return;
    }
    const uint32_t want = ctx.r4.u32, outProduced = ctx.r6.u32, outFlags = ctx.r8.u32;
    const uint32_t before12 = GRead32(base, voice + 12);
    __imp__sub_82D089A0(ctx, base);
    static int lines = 0;
    static uint32_t lastVoice = 0, last12 = ~0u, lastFlags = ~0u, lastProduced = ~0u;
    const uint32_t flags = outFlags ? GRead32(base, outFlags) : 0, produced = outProduced ? GRead32(base, outProduced) : 0;
    if (lines >= 2000 || (voice == lastVoice && GRead32(base, voice + 12) == last12 && flags == lastFlags && produced == lastProduced))
        return;
    ++lines;
    lastVoice = voice, last12 = GRead32(base, voice + 12), lastFlags = flags, lastProduced = produced;
    const uint32_t list = GRead32(base, voice + 52);
    const uint32_t entries = list ? GRead32(base, list + 8) : 0;
    const uint32_t context = entries ? GRead32(base, entries + VOICE_CONTEXT_OFFSET) : 0;
    std::fprintf(stderr,
                 "[xma-trace] t=%llu voice=%08X ctx=%08X want=%u ret=%08X produced=%u pos=%u->%u flags=%X | +20=%X +24=%u +28=%u +32=%u +40=%u +44=%u +48=%u +56=%u +64=%X +88=%u +92=%u +108=%X +116=%u +120=%X +128=%u +132=%X +140=%X b68-71=%02X%02X%02X%02X b144-147=%02X%02X%02X%02X | hw dw0=%08X dw1=%08X rd=%X\n",
                 (unsigned long long)xma::SteadyMs(), voice, context, want, ctx.r3.u32, produced, before12,
                 GRead32(base, voice + 12), flags,
                 GRead32(base, voice + 20), GRead32(base, voice + 24), GRead32(base, voice + 28), GRead32(base, voice + 32),
                 GRead32(base, voice + 40), GRead32(base, voice + 44), GRead32(base, voice + 48), GRead32(base, voice + 56),
                 GRead32(base, voice + 64), GRead32(base, voice + 88), GRead32(base, voice + 92), GRead32(base, voice + 108),
                 GRead32(base, voice + 116), GRead32(base, voice + 120), GRead32(base, voice + 128), GRead32(base, voice + 132),
                 GRead32(base, voice + 140),
                 base[voice + 68], base[voice + 69], base[voice + 70], base[voice + 71],
                 base[voice + 144], base[voice + 145], base[voice + 146], base[voice + 147],
                 context ? GRead32(base, context) : 0, context ? GRead32(base, context + 4) : 0,
                 context ? GRead32(base, context + 8) : 0);
}

// Diagnostics (TTT2_XMA_LOG=1): NUSound re-initialises a voice's context for a new sound in
// sub_82D3E488(list r3, entry index r4; entry = list->entries + index * 96, context at +64).
// Report when the context being re-initialised was playing a stereo stream (the music), with
// the guest backtrace that took the voice.
extern "C" PPC_FUNC(__imp__sub_82D3E488);
PPC_FUNC(sub_82D3E488)
{
    static const bool log = EnvIs("TTT2_XMA_LOG", "1");
    if (log && ctx.r3.u32)
    {
        const uint32_t entries = GRead32(base, ctx.r3.u32 + 8);
        const uint32_t context = entries ? GRead32(base, entries + ctx.r4.u32 * VOICE_ENTRY_BYTES + VOICE_CONTEXT_OFFSET) : 0;
        if (context && (GRead32(base, context + 4) >> 29) & 1)
        {
            char line[512];
            int n = std::snprintf(line, sizeof(line), "[xma] music voice context %08X (entry %u) re-initialised: dw0=%08X dw1=%08X rd=%X lr=%08X bt:",
                                  context, ctx.r4.u32, GRead32(base, context), GRead32(base, context + 4),
                                  GRead32(base, context + 8), uint32_t(ctx.lr));
            n = AppendBacktrace(ctx, base, line, n, int(sizeof(line)));
            std::snprintf(line + n, sizeof(line) - n, "\n");
            std::fputs(line, stderr);
        }
    }
    __imp__sub_82D3E488(ctx, base);
}
