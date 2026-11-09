// Options -> Display Settings -> LOW LATENCY (display_settings.h): each frame's picture at its own swap.
//
// TTT2 draws through its own render thread (sub_82AD2AF8, NU::Draw::RequestManager): the game thread builds a frame's
// render list (render context 82F02100 +36) during its update and draw, and the render thread turns it into GPU
// commands (+40, one render task after another) - but only during the NEXT frame. The frame kick (sub_82AD1B88) waits
// for the render thread and the GPU (sub_82AD1DE8), hands the new list over (sub_82AD2CC0: +40 = +36, the skinned
// vertex ring's advance, a new list), swaps - showing the list handed over a frame earlier -, waits for the vblank and
// only then releases the render thread (sub_82AD1DD8: +120 monitor = 1). MEASURED 2026-11-01 (a skinned-vertex ring
// probe on the timeline, removed since; Practice): every draw a frame shows reads vertices placed one ring advance
// earlier, a press's first reaction is on screen two swaps after the frame that used it, and the render thread runs
// 0.2-0.5 ms after the vblank on the previous frame's list.
//
// Low latency releases the render thread right after the hand-over and waits for it and for the GPU before the swap:
// the swap shows the list just built, one frame (16.7 ms) less from a press to the screen. MEASURED 2026-11-01
// (Practice, the first picture change after a press): two swaps after its frame -> one, 39-47 ms -> 24-31 ms from the
// press to the presented picture. The list is complete at the hand-over (the draw has waited for the skinning jobs,
// sub_82ACFF90). The cost: the render thread's and the command processor's work on the list no longer runs beside the
// game thread's next frame but inside its own (1.0-1.7 ms here; on the tester's i3-3240 + GT 730 the command
// processor alone takes 7-12 ms of a frame with ~2 ms to spare). Switching costs one picture: on, a list is never
// shown; off, one picture shows twice.
//
// ON and AUTO apply in matches only (TEST 17, InMatch below). OFF is the default since TEST 17 (MEASURED, the tester's
// i3-3240 + GT 730: ON took Paris from 59.5 to 52.5 fps - the command processor's ~10 ms per list no longer overlaps).
// AUTO decides from fights (battle state 4 a round, 21 Practice) at the kick's release (lr 82AD1D3C):
//   off -> on: what the frame had to spare (the kick's vblank waits, rollback.cpp, and its wait for the previous
//     list, sub_82AD1DE8) against what the previous list took from its release until the command processor had read
//     its last command (the render thread's end of a list - sub_82AE43E0(+120, 0), lr 82AD2B88 - gives the ring's
//     write offset then; ConsumeRing reports passing it). On when 177 of 180 fight frames had that time + 2 ms.
//   on -> off: a fight frame whose vblank waits were under 0.5 ms had nothing to spare; more than 4 of 120 -> off,
//     and another try after 60 s (each failure doubles that, up to 16 minutes).
// Re-simulated frames (rollback) keep the game's order (they are never shown); the frame overlap experiment
// (TTT2_FRAME_OVERLAP) turns this off. TTT2_LOWLAT=1|0 forces it on / off (tests).
#include "kernel_shared.h"
#include "display_settings.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

bool RollbackInResimDraw();                                     // rollback.cpp
double RollbackKickVblankMs();
bool GpuRingPublished(uint32_t& offset, uint32_t& size);        // kernel_video.cpp
uint32_t GpuRingReadOffset();
namespace frame_overlap { bool On(); }                          // frame_overlap.cpp
extern "C" PPC_FUNC(__imp__sub_82AD1DD8);

namespace {
using Clock = std::chrono::steady_clock;
int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
double Ms(int64_t ns) { return double(ns) / 1e6; }
constexpr uint32_t kBattleState = 0x82EAC03C;                   // 4 a round of a match, 21 Practice (frame_overlap.cpp)
constexpr double kMarginMs = 2.0, kTightMs = 0.5;
constexpr uint32_t kOnWindow = 180, kOnFits = 177, kOffWindow = 120, kOffTight = 4;
constexpr int kFirstRetryS = 60, kLastRetryS = 960;
constexpr double kSettledWaitMs = 12.0;                        // LowLatencyPaceFlip
constexpr uint32_t kSettledFrames = 120;

int Forced()
{
    static const int v = [] { const char* e = std::getenv("TTT2_LOWLAT"); return !e || !*e ? -1 : *e == '1' ? 1 : 0; }();
    return v;
}

// A list's way: released (the game thread), drawn (the render thread), its last command read (the command processor).
std::atomic<uint32_t> g_released{ 0 };          // lists released (the newest one's number)
std::atomic<int64_t> g_releasedNs{ 0 };
std::atomic<uint32_t> g_watched{ 0 };           // a drawn list whose last command is still to be read (0 none)
std::atomic<uint32_t> g_watchedEnd{ 0 };        // that command's end (ring offset)
std::atomic<uint32_t> g_readList{ 0 };          // the newest list read to its end
std::atomic<int64_t> g_readNs{ 0 };

void ListRead(uint32_t list)
{
    uint32_t expected = list;
    if (!g_watched.compare_exchange_strong(expected, 0)) return;
    g_readNs.store(NowNs(), std::memory_order_relaxed);
    g_readList.store(list, std::memory_order_release);
}

// The game thread's (the frame kick's).
thread_local bool t_earlyWait = false;          // in the wait before the swap (diag_hooks.cpp's sub_82AD1DE8 hook)
bool g_early = false;                           // this frame's list drawn before its swap: the kick's release is done
bool g_wasEarly = false;
bool g_resimSince = false;                      // a re-simulated frame's kick (rollback) released a list since the last live one
double g_earlyMs = 0;                           // this frame's wait before the swap
double g_kickWaitMs = 0;                        // the kick's own waits for the previous list this frame
int g_setting = -1;                             // the choice AUTO's state belongs to
bool g_autoOn = false;
Clock::time_point g_retryAt{}, g_lastStay{}, g_report = Clock::now();
int g_retryS = kFirstRetryS;
uint32_t g_frames = 0, g_fit = 0, g_tight = 0, g_measured = 0;   // AUTO's window
double g_costSum = 0, g_costMax = 0, g_spareSum = 0;
uint32_t g_repFrames = 0, g_repTight = 0;       // the 10-s report while lists are drawn before the swap
double g_repSum = 0, g_repMax = 0;

void ResetWindow() { g_frames = g_fit = g_tight = g_measured = 0; g_costSum = g_costMax = g_spareSum = 0; }

void NoteRelease()
{
    g_releasedNs.store(NowNs(), std::memory_order_relaxed);
    g_released.fetch_add(1, std::memory_order_release);
}

// A match: its rounds with their intros and KOs and the breaks between them (battle states 3 / 4 / 5 / 6, MEASURED:
// 16 loading, 0 / 1 the fighters' intros, then 3 4 5 6 for every round, 8 the match's end, 35 / 37 results; 18 the
// menus, 34 Customize), or Practice (21). Elsewhere the game's order: no fight there needs the frame, and early lists
// there cost the slower PCs fps with nothing to turn AUTO off (MEASURED, log 48: Customize at 45-60 fps with ON). A
// match switches twice - a list never shown at the first round's intro, a picture shown twice after the last KO.
bool InMatch(uint32_t state) { return (state >= 3 && state <= 6) || state == 21; }

bool EarlyThisFrame()
{
    if (frame_overlap::On()) return false;
    if (!InMatch(GRead32(g_base, kBattleState))) return false;
    if (Forced() >= 0) return Forced() == 1;
    const int setting = display_settings::GetLowLatency();
    return setting == display_settings::LowLatencyOn || (setting == display_settings::LowLatencyAuto && g_autoOn);
}

const char* Why()
{
    if (Forced() >= 0) return "TTT2_LOWLAT";
    const int setting = display_settings::GetLowLatency();
    return setting == display_settings::LowLatencyOn ? "ON" : setting == display_settings::LowLatencyOff ? "OFF" : "AUTO";
}

// A live frame at its kick's release: the 10-s report, AUTO's measurements and choice.
void Evaluate(bool early)
{
    const auto now = Clock::now();
    const double vblank = RollbackKickVblankMs(), spare = vblank + g_kickWaitMs;
    g_kickWaitMs = 0;
    const uint32_t state = GRead32(g_base, kBattleState);
    const bool fight = state == 4 || state == 21;
    const int setting = Forced() >= 0 ? -2 : display_settings::GetLowLatency();
    if (setting != g_setting)                   // another choice in the menu: AUTO starts over
    {
        g_setting = setting;
        g_autoOn = false;
        g_retryS = kFirstRetryS;
        g_retryAt = {};
        ResetWindow();
    }
    const bool automatic = setting == display_settings::LowLatencyAuto;
    if (early)
    {
        const bool tight = fight && vblank < kTightMs;
        ++g_repFrames; g_repSum += g_earlyMs; g_repMax = (std::max)(g_repMax, g_earlyMs);
        if (tight) ++g_repTight;
        if (now - g_report >= std::chrono::seconds(10))
        {
            std::fprintf(stderr, "[lowlat] lists drawn before the swap (%s): %.2f ms a frame (longest %.2f) over %u frames, "
                                 "%u fight frames with nothing to spare\n", Why(), g_repSum / g_repFrames, g_repMax, g_repFrames, g_repTight);
            g_repFrames = g_repTight = 0; g_repSum = g_repMax = 0; g_report = now;
        }
        if (!automatic || !fight) return;
        ++g_frames; g_costSum += g_earlyMs; g_costMax = (std::max)(g_costMax, g_earlyMs);
        if (tight) ++g_tight;
        if (g_tight > kOffTight)
        {
            g_autoOn = false;
            g_retryAt = now + std::chrono::seconds(g_retryS);
            std::fprintf(stderr, "[lowlat] AUTO -> off: %u of %u fight frames had nothing to spare (the list before the swap took "
                                 "%.2f ms on average, %.2f at most); another try in %d s\n",
                         g_tight, g_frames, g_costSum / g_frames, g_costMax, g_retryS);
            g_retryS = (std::min)(g_retryS * 2, kLastRetryS);
            ResetWindow();
        }
        else if (g_frames >= kOffWindow) ResetWindow();
        return;
    }
    if (!automatic || !fight || g_autoOn || now < g_retryAt) return;
    // After a rollback the newest list is the last re-simulated frame's, which the command processor only skips over
    // (review 2026-11-02): not what a live list costs, so this frame is left out.
    if (g_resimSince) return;
    // The previous list: released at the previous kick's end, read by the command processor by now (this kick waited
    // for it) - unless the command processor fell a list behind.
    const uint32_t list = g_released.load(std::memory_order_relaxed);
    double cost = -1;
    if (list && g_readList.load(std::memory_order_acquire) == list)
        cost = Ms(g_readNs.load(std::memory_order_relaxed) - g_releasedNs.load(std::memory_order_relaxed));
    ++g_frames;
    if (cost >= 0)
    {
        ++g_measured; g_costSum += cost; g_costMax = (std::max)(g_costMax, cost); g_spareSum += spare;
        if (cost + kMarginMs <= spare) ++g_fit;
    }
    if (g_frames < kOnWindow) return;
    const double avgCost = g_measured ? g_costSum / g_measured : 0, avgSpare = g_measured ? g_spareSum / g_measured : 0;
    if (g_fit >= kOnFits)
    {
        g_autoOn = true;
        std::fprintf(stderr, "[lowlat] AUTO -> on: %u of %u fight frames had the time (a list took %.2f ms on average from its release "
                             "to the command processor, %.2f at most; %.2f ms to spare on average)\n",
                     g_fit, g_frames, avgCost, g_costMax, avgSpare);
    }
    else if (now - g_lastStay >= std::chrono::seconds(30))
    {
        g_lastStay = now;
        std::fprintf(stderr, "[lowlat] AUTO stays off: %u of %u fight frames had the time (%u measured; a list took %.2f ms on average "
                             "from its release to the command processor, %.2f at most; %.2f ms to spare on average)\n",
                     g_fit, g_frames, g_measured, avgCost, g_costMax, avgSpare);
    }
    ResetWindow();
}
} // namespace

// diag_hooks.cpp's sub_82AD1DE8 hook: the wait before the swap is this module's (not the kick's own); the kick's own
// waits for the previous list (what a frame has to spare besides its vblank waits).
bool LowLatencyEarlyWait() { return t_earlyWait; }
// kernel_video.cpp's flip phase guard, at every frame kick's swap (the game thread): `atOnce` = the kick's wait for the
// vblank after the swap has passed already. A list drawn before its swap flips at once. In the game's order only a settled
// late schedule does: every kick for 2 s waiting at least 12 ms for the last frame, so the game thread had 4.7 ms of work
// or less and the PC has room for the earlier schedule (MEASURED 2026-11-05, dev PC, schedule forced late: 15 ms a frame
// and one frame more input lag for the rest of the fight otherwise). The tester's i3-3240 takes 7 ms of processor on that
// thread alone, so it never gets here: its schedule stays the game's, TEST 15's.
bool LowLatencyPaceFlip(bool atOnce)
{
    static uint32_t settled = 0;
    settled = atOnce && g_kickWaitMs >= kSettledWaitMs ? settled + 1 : 0;
    return atOnce && (g_early || settled >= kSettledFrames);
}
void LowLatencyNoteKickWait(double ms) { if (!RollbackInResimDraw()) g_kickWaitMs += ms; }

// diag_hooks.cpp: the frame kick's hand-over of this frame's list (sub_82AD2CC0 returned >= 0, lr 82AD1BD0). Low
// latency on: the render thread released now, and the kick waits for it and for the GPU before it swaps.
void LowLatencyHandOver(PPCContext& ctx, uint8_t* base, uint32_t renderContext)
{
    g_early = false;
    if (RollbackInResimDraw()) return;
    const bool early = EarlyThisFrame();
    if (early != g_wasEarly)
    {
        static int logged = 0;
        if (logged++ < 100)
            std::fprintf(stderr, early ? "[lowlat] each frame's list drawn before its swap (%s, battle state %u)\n"
                                       : "[lowlat] the game's order again (%s, battle state %u)\n",
                         Why(), GRead32(base, kBattleState));
        g_wasEarly = early;
        g_repFrames = g_repTight = 0; g_repSum = g_repMax = 0; g_report = Clock::now();
    }
    if (!early) return;
    const int64_t t0 = NowNs();
    const PPCContext saved = ctx;
    ctx.r3.u64 = renderContext;
    NoteRelease();
    __imp__sub_82AD1DD8(ctx, base);                     // the render thread draws the list just handed over
    ctx.r3.u64 = renderContext;
    ctx.r4.u64 = 1;
    t_earlyWait = true;
    sub_82AD1DE8(ctx, base);                            // wait for it, then for the GPU (fence)
    t_earlyWait = false;
    ctx = saved;
    g_early = true;
    g_earlyMs = Ms(NowNs() - t0);
}

// diag_hooks.cpp: the frame kick's release of the render thread (sub_82AD1DD8, lr 82AD1D3C), after its swap and
// vblank waits. False: this frame's list was drawn before the swap - nothing to release.
bool LowLatencyKickRelease()
{
    const bool early = g_early;
    g_early = false;
    if (RollbackInResimDraw())                  // early is always false here (LowLatencyHandOver)
    {
        g_resimSince = true;
        NoteRelease();
        return true;
    }
    Evaluate(early);
    g_resimSince = false;
    if (!early) NoteRelease();
    return !early;
}

// diag_hooks.cpp: the render thread has drawn a list (sub_82AE43E0(+120, 0), lr 82AD2B88, before it says so); its
// commands are published, so the list is read once the command processor reaches the ring's write offset now.
void LowLatencyListDrawn()
{
    uint32_t end = 0, size = 0;
    const uint32_t list = g_released.load(std::memory_order_acquire);
    if (!list || !GpuRingPublished(end, size)) return;
    g_watchedEnd.store(end, std::memory_order_relaxed);
    // seq_cst here and in LowLatencyRingRead (the command processor stores the read offset seq_cst before it looks):
    // with release / acquire both sides could miss the other's store and the read go unrecorded (review 2026-11-01).
    // ListRead takes the list once if both see it.
    g_watched.store(list, std::memory_order_seq_cst);
    if (GpuRingReadOffset() == end) ListRead(list);     // read already
}

// kernel_video.cpp's ConsumeRing: the command processor has read the ring from `from` to `to`.
void LowLatencyRingRead(uint32_t from, uint32_t to, uint32_t size)
{
    const uint32_t list = g_watched.load(std::memory_order_seq_cst);
    if (!list || !size) return;
    const uint32_t end = g_watchedEnd.load(std::memory_order_relaxed);
    if ((end + size - from) % size <= (to + size - from) % size) ListRead(list);
}
