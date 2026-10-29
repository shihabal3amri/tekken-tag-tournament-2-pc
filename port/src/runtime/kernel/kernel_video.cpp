// Xbox 360 video (Vd*) kernel layer: a "GPU heartbeat" that lets the game's own D3D runtime
// run its swap/interrupt machinery before a real command processor exists.
//
// Contract reverse-engineered from TTT2's D3D runtime (PROJECT_STATUS session 5):
// - The game registers interrupt callback 0x82738AD8 with context = D3D device pointer.
//   source 0: callback reads GPU MMIO 0x7FC86544 (bit0 = swap pending) and runs the
//   swap-complete DPC (flips via MMIO 0x7FC86110, signals the KEVENT at device+24460).
//   source 1: dispatches the game's vblank callback + clears a per-CPU pending mask.
// - Ring progress: the game polls the "GPU completed segment" counter at sysCmdBuf+0
//   (sysCmdBuf = VdSetSystemCommandBufferGpuIdentifierAddress arg - 8) and the rptr
//   writeback dword registered via VdEnableRingBufferRPtrWriteBack.
// - Fences: PM4 EVENT_WRITE_SHD packets (header 0xC0025800: initiator, physAddr, value)
//   expect the GPU to store `value` at `physAddr`. We scan the ring each tick and execute
//   them, replacing the header with a PM4 NOP so they run once.
#include "kernel_shared.h"
#include "guest_heap.h"
#include <immintrin.h>
#include "kernel_trace.h"
#include "gpu_render.h"
#include "gpu_alu_upload.h"
#if defined(TTT2_GEOMETRY_AB_PROBE)
#include "gpu_geometry_probe.h"
#endif
#include "../host_vm.h"
#include "../host_window_sdl.h"
#include "../host_process.h"
#include "../host_window.h"
#include "../host_thread.h"
#include "../host_deadline_wait.h"
#include "host_settings.h"
#ifdef _WIN32
#include "../host_window_win32.h"
#endif
#include "phys_mirror.h"
#include "gpu_ring_reader.h"
#include "bounded_format.h"
#include "timeline.h"
#include "frame_benchmark.h"
#include "frame_pacing_probe.h"
#include "gpu_draw_skip_probe.h"
#include "display_settings.h"
#ifdef _WIN32
#include "display_devices.h"
#include "sys_ui.h"
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// Guest ISR/DPC chains can run deep; give guest-executing host threads big stacks
// (a silent process death with no fault output = stack overflow in a detached thread).
static void StartBigStackThread(host_thread::Procedure fn)
{
    if (!host_thread::StartDetached(8 * 1024 * 1024, fn, nullptr))
        std::fprintf(stderr, "[vd] failed to start presentation worker\n");
}

static std::atomic<uint32_t> g_intrCallback{0};
static std::atomic<uint32_t> g_intrContext{0};
uint32_t VdDeviceGuest() { return g_intrContext.load(); }   // the D3D device (rollback.cpp keeps it out of its state)
static std::atomic<uint32_t> g_ringBase{0};
static std::atomic<uint32_t> g_ringSize{0};
static std::atomic<uint32_t> g_rptrWriteBack{0};
static std::atomic<uint32_t> g_sysCmdBuf{0};
static std::atomic<bool> g_tickStarted{false};

static constexpr uint32_t MMIO_SWAP_STATUS = 0x7FC86544u;   // reg 0x6544, bit0 = swap pending

// GPU progress: bumped after every guest-memory write the GPU side makes - the command
// processor's fences, memory writes and read-pointer writeback, and each guest interrupt
// callback (the vblank / swap DPC tokens). Waiters on GPU-written values sleep on it
// (WaitOnAddress) instead of spinning: guest_monitor.cpp's D3D wait, WaitRegMem below.
std::atomic<uint32_t> g_gpuProgress{0};
static std::mutex g_gpuProgressMutex;
static std::condition_variable g_gpuProgressCv;
void GpuProgress()
{
    {
        std::lock_guard lock(g_gpuProgressMutex);
        g_gpuProgress.fetch_add(1, std::memory_order_release);
    }
    g_gpuProgressCv.notify_all();
}
// Sleep until GpuProgress moves past `seen`, at most `ms` (true: it moved).
bool WaitGpuProgress(uint32_t seen, uint32_t ms)
{
    std::unique_lock lock(g_gpuProgressMutex);
    return g_gpuProgressCv.wait_for(lock, std::chrono::milliseconds(ms), [seen] {
        return g_gpuProgress.load(std::memory_order_acquire) != seen;
    });
}

// The D3D runtime hands the kernel PHYSICAL addresses (virtual & 0x1FFFFFFF). GPU-visible
// allocations come from the physical heap at guest 0xA0000000+, so map them back.
static uint32_t PhysToGuest(uint32_t addr)
{
    return (addr && addr < 0x20000000u) ? (addr | 0xA0000000u) : addr;
}

// Keep the existing diagnostic names under a host-specific debug directory.
static std::string DebugPath(const char* legacy)
{
    constexpr char prefix[] = "C:/ttt2/port/";
    if (std::strncmp(legacy, prefix, sizeof(prefix) - 1) == 0)
        return host_settings::DiagnosticPath(legacy + sizeof(prefix) - 1);
    return legacy;
}
static FILE* DebugOpen(const char* path, const char* mode)
{
    return std::fopen(DebugPath(path).c_str(), mode);
}
static bool DebugExists(const char* path)
{
    std::error_code error;
    return std::filesystem::exists(DebugPath(path), error);
}
static bool DebugRemove(const char* path)
{
    std::error_code error;
    return std::filesystem::remove(DebugPath(path), error);
}
static void DebugDirectory(const char* path)
{
    std::error_code error;
    std::filesystem::create_directories(DebugPath(path), error);
}

// ===================== Present pass: host window + frontbuffer blit =====================
// The swap packet carries the frontbuffer TEXTURE FETCH BLOCK; we decode the surface
// address from it and show the surface in a host window (and periodically dump a BMP
// for headless verification). Even a black frame proves the flip chain end-to-end.
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "synchronization.lib")

static std::atomic<uint32_t> g_frontBufferPhys{0};   // guest address of the displayed surface
// The displayed surface's fetch-constant swizzle (12 bits). TTT2's is .zyx1 (0xA0A): it swaps
// red and blue back after the final resolve's copy_dest_swap.
static std::atomic<uint32_t> g_frontBufferSwizzle{ 0x688 };
static bool SwizzleSwapsRB(uint32_t sw) { return (sw & 7) == 2 && ((sw >> 6) & 7) == 0; }
static std::atomic<bool> g_presentStarted{false};
static constexpr uint32_t FB_W = 1280, FB_H = 720;

// Xbox 360 2D tiled-texture offset (32bpp texels). Standard XGAddress2DTiledOffset.
static uint32_t TiledOffset32(uint32_t x, uint32_t y, uint32_t widthAligned)
{
    constexpr uint32_t logBpp = 2;   // 4 bytes/texel
    uint32_t macro = ((x >> 5) + (y >> 5) * (widthAligned >> 5)) << (logBpp + 7);
    uint32_t micro = (((x & 7) + ((y & 6) << 2)) << logBpp);
    uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15u) +
                      ((y & 8) << (3 + logBpp)) + ((y & 1) << 4);
    return (((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) +
            ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3u) << 6)) >> logBpp;
}

// Convert the guest surface (big-endian words, tiled unless TTT2_FB_LINEAR=1) into a
// host BGRX staging buffer. The word's low byte is the fetched x component, so the image is
// the ARGB reading of the word when the swizzle exchanges red and blue (TTT2's .zyx1).
static void ConvertFrontBuffer(uint32_t fbGuest, uint32_t* out)
{
    static const bool linear = std::getenv("TTT2_FB_LINEAR") != nullptr;
    const bool swapRB = !SwizzleSwapsRB(g_frontBufferSwizzle.load());
    const uint8_t* src = g_base + fbGuest;
    for (uint32_t y = 0; y < FB_H; ++y)
    {
        for (uint32_t x = 0; x < FB_W; ++x)
        {
            uint32_t idx = linear ? (y * FB_W + x) : TiledOffset32(x, y, FB_W);
            uint32_t be;
            std::memcpy(&be, src + idx * 4, 4);
            uint32_t v = __builtin_bswap32(be);              // the guest word
            if (swapRB) v = (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
            out[y * FB_W + x] = v & 0x00FFFFFFu;             // BGRX for BI_RGB 32bpp
        }
    }
}

static void WriteBmpTo(const char* path, const uint32_t* px)
{
    FILE* f = DebugOpen(path, "wb");
    if (!f) return;
    uint32_t imgSize = FB_W * FB_H * 4;
    uint8_t hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    *reinterpret_cast<uint32_t*>(hdr + 2) = 54 + imgSize;
    *reinterpret_cast<uint32_t*>(hdr + 10) = 54;
    *reinterpret_cast<uint32_t*>(hdr + 14) = 40;
    *reinterpret_cast<int32_t*>(hdr + 18) = (int32_t)FB_W;
    *reinterpret_cast<int32_t*>(hdr + 22) = -(int32_t)FB_H;   // top-down
    *reinterpret_cast<uint16_t*>(hdr + 26) = 1;
    *reinterpret_cast<uint16_t*>(hdr + 28) = 32;
    *reinterpret_cast<uint32_t*>(hdr + 34) = imgSize;
    std::fwrite(hdr, 1, sizeof(hdr), f);
    std::fwrite(px, 1, imgSize, f);
    std::fclose(f);
}

static void WriteFrontBufferBmp(const uint32_t* px)
{
    // TTT2_INSTANCE=<tag> gives a second instance its own file (fbdump<tag>.bmp, two-instance tests).
    static const std::string path = [] {
        const char* tag = std::getenv("TTT2_INSTANCE");
        return std::string("C:/ttt2/port/fbdump") + (tag ? tag : "") + ".bmp";
    }();
    WriteBmpTo(path.c_str(), px);
}

// SDL owns the window and event loop. D3D11 receives only the native HWND from
// its Windows adapter; window sizing and input events stay in SDL.
static void PumpWindow(host_window::Window* window)
{
    const host_window::Events events = host_window::Poll(window);
    if (events.close)
    {
        std::fprintf(stderr, "[window] closed - quitting\n");
        std::fflush(stderr);
        LogDrain(); host_process::ExitNow(0);
    }
    if (events.toggleMode)
        display_settings::SetMode(display_settings::GetMode() == display_settings::Windowed
            ? display_settings::Borderless : display_settings::Windowed);
}

static void ApplyWindowMode(host_window::Window* window)
{
    const int mode = display_settings::GetMode();
    const display_settings::Resolution r = display_settings::GetResolution();
#ifdef _WIN32
    // SDL remains the window owner. Position it on the configured native
    // monitor before the shared mode code computes that display's bounds.
    const HWND wnd=host_window_win32::Handle(window);
    MONITORINFO mi{};mi.cbSize=sizeof(mi);
    if(GetMonitorInfoA(display_devices::Resolve(display_settings::GetMonitorDevice(),wnd),&mi)) {
        SDL_SetWindowPosition(host_window::SDLHandle(window),mi.rcMonitor.left+32,mi.rcMonitor.top+32);
        SDL_SyncWindow(host_window::SDLHandle(window));
    }
#endif
    const host_window::Mode hostMode = mode == display_settings::Windowed
        ? host_window::Mode::Windowed
        : (mode == display_settings::Borderless ? host_window::Mode::Borderless
                                                : host_window::Mode::Fullscreen);
    host_window::ApplyMode(window, hostMode, r.width, r.height);
    std::fprintf(stderr, "[window] mode %d, resolution %ux%u\n", mode, r.width, r.height);
}

static unsigned PresentThread(void*)
{
    host_window::Window* window = host_window::Open(FB_W, FB_H);
    if (!window) return 0;
#ifdef _WIN32
    host_window_win32::InstallMessageHook(window);
#endif
    if (!host_window::GetNativeSurface(window).window) { host_window::Close(window); return 0; }
    GpuHwSetWindow(window);
    if (GpuHwMode())
    {
        // The hardware backend presents through a DXGI swap chain from the CP thread at the
        // game's own swap points (HwPresentFrame); this thread owns the window and applies the
        // display mode (the saved one at start, then every change from the menu or Alt+Enter).
        uint32_t applied = ~0u;
        for (;;)
        {
            PumpWindow(window);
            const uint32_t generation = display_settings::Generation();
            // Leaving exclusive fullscreen: the presenter switches back first, or DXGI would
            // restore the window it saved over the new style. While exclusive fullscreen stays on
            // (a new RESOLUTION), DXGI owns the window: the presenter changes the display mode and
            // restyling or moving the window here would knock the swap chain out of it.
            const bool exclusive = GpuHwIsExclusiveFullscreen();
            const bool fullscreen = display_settings::GetMode() == display_settings::Fullscreen;
            if (generation != applied && !(exclusive && !fullscreen))
            {
                if (!exclusive) ApplyWindowMode(window);
                applied = generation;
            }
            host_window::Idle(5);
        }
    }
    static uint32_t staging[FB_W * FB_H];
    uint32_t frames = 0;
    for (;;)
    {
        PumpWindow(window);
        if (uint32_t fb = g_frontBufferPhys.load())
        {
            ConvertFrontBuffer(fb, staging);
            host_window::Blit(window, staging, FB_W, FB_H);
            // TTT2_FBSEQ=<n>: also keep every n-th presented frame as its own numbered file, so a
            // run produces a filmstrip. A single overwritten dump cannot tell a black screen
            // from an animation caught mid-fade.
            static const int seqEvery = [] {
                const char* e = std::getenv("TTT2_FBSEQ");
                return e ? (int)std::strtol(e, nullptr, 0) : 0;
            }();
            if (seqEvery > 0 && frames % seqEvery == 0)
            {
                DebugDirectory("C:/ttt2/port/fb");
                char fp[128];
                BoundedSnprintf(fp, sizeof(fp), "C:/ttt2/port/fb/frame_%04u.bmp",
                              frames / (uint32_t)seqEvery);
                WriteBmpTo(fp, staging);
            }
            if (++frames % 60 == 1)
            {
                WriteFrontBufferBmp(staging);                       // headless verification
                uint32_t nz = 0;
                for (uint32_t i = 0; i < FB_W * FB_H; i += 97) nz += staging[i] != 0;
                std::fprintf(stderr,
                    "[vd] present fb=%08X raw[0]=%08X staging[0]=%08X staging[mid]=%08X nz=%u\n",
                    fb, GRead32(g_base, fb), staging[0],
                    staging[(FB_H / 2) * FB_W + FB_W / 2], nz);
            }
        }
        host_window::Idle(33);
    }
    return 0;
}

// Hardware-backend presentation at the game's swap point (CP thread, in stream order).
// TTT2_FBSEQ / fbdump.bmp keep working: those frames are read back from the GPU.
// Frames presented by the command processor / swaps issued by the game (diagnostics:
// how far the CPU runs ahead of the GPU emulation).
std::atomic<uint64_t> g_cpPresents{0}, g_gameSwaps{0};

// Frame pacing diagnostics (TTT2_FRAMETIME=1): intervals between the game's VdSwap calls
// (guest CPU frame) and between the command processor's presents (GPU emulation + D3D11),
// a 10 s summary per stream (frames, intervals over 17.5/25/33/50 ms, longest) and one line
// per interval over 25 ms. Each timer is used by one thread only.
void HitchRegisterThread(const char* name);   // diag_hooks.cpp (TTT2_HITCH_PROFILE=3)
void HitchFrameGap(double ms);
static bool FrameTimeLog()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_FRAMETIME"); return e && *e == '1'; }();
    return on;
}
struct FrameTimer
{
    const char* name;
    std::chrono::steady_clock::time_point last{}, window{};
    uint64_t frames = 0, over17 = 0, over25 = 0, over33 = 0, over50 = 0;
    double longest = 0;
    void Mark(double workMs = -1)
    {
        const auto now = std::chrono::steady_clock::now();
        if (last.time_since_epoch().count())
        {
            const double ms = std::chrono::duration<double, std::milli>(now - last).count();
            ++frames;
            over17 += ms > 17.5, over25 += ms > 25, over33 += ms > 33.4, over50 += ms > 50;
            longest = (std::max)(longest, ms);
            if (ms > 25 && name[0] == 'p') HitchFrameGap(ms);
            if (ms > 25)
                std::fprintf(stderr, "[frame] %s gap %.1f ms (swaps %llu presents %llu, present call %.1f ms)\n", name, ms,
                             (unsigned long long)g_gameSwaps.load(), (unsigned long long)g_cpPresents.load(),
                             workMs >= 0 ? workMs : 0.0);
        }
        else window = now;
        last = now;
        if (now - window >= std::chrono::seconds(10))
        {
            std::fprintf(stderr, "[frame] %s 10s: %llu frames, >17.5ms %llu, >25ms %llu, >33ms %llu, >50ms %llu, longest %.1f ms\n",
                         name, (unsigned long long)frames, (unsigned long long)over17, (unsigned long long)over25,
                         (unsigned long long)over33, (unsigned long long)over50, longest);
            frames = over17 = over25 = over33 = over50 = 0;
            longest = 0;
            window = now;
        }
    }
};

// ---- Display gamma LUT. The console applies a 256-entry table (DC_LUT_30_COLOR words: blue
// bits 0-9, green 10-19, red 20-29) to the front buffer at scan-out. D3D loads it from the
// display type VdGetCurrentDisplayGamma reports: for 2 (HDTV, also Xenia's default) TTT2 writes
// BT709_OETF(sRGB_EOTF(x)) - MEASURED 2026-10-12: entry 128 = 462/1023 (formula 0.4504), entry
// 16 = 24/1023 (formula 0.0234). Register semantics follow Xenia's
// CommandProcessor::WriteRegister (tools/ref_xenia/command_processor.cc). The present applies the
// table (gpu_hw_d3d11.h Present; TTT2_GAMMA_RAMP=0 shows the unramped frame buffer).
static std::mutex g_gammaMutex;
static std::atomic<uint32_t> g_gammaVersion{ 0 };
static uint32_t g_gammaComponent = 0;                 // DC_LUT_SEQ_COLOR: red, green, blue
static uint32_t* GammaRamp()
{
    static uint32_t ramp[256] = {};
    static const bool init = [] {
        for (uint32_t i = 0; i < 256; ++i) { const uint32_t v = i * 0x3FF / 0xFF; ramp[i] = v | (v << 10) | (v << 20); }
        return true;
    }();
    (void)init;
    return ramp;
}
static void GammaRegisterWritten(uint32_t reg, uint32_t value);

// TTT2_INPUT_TIMING=1: input latency tests log every game swap and present (same clock as the
// [input] / [burst] lines).
static bool InputTimingOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_INPUT_TIMING"); return e && *e == '1'; }();
    return on;
}

static bool EarlyPresentOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_EARLY_PRESENT"); return !(e && *e == '0'); }();
    return on;
}
// Early present: VdSwap numbers the swaps it writes a marker for and records each one's front
// buffer; the CP counts the markers it executes, so the frame a vblank-token wait belongs to is
// known (see the WAIT_REG_MEM packet).
std::atomic<uint32_t> g_markerSwaps{ 0 };               // markers written (VdSwap, main thread)
std::atomic<uint32_t> g_markerFb[16];                   // their front buffer surfaces, by number
static uint32_t g_cpMarkers = 0;                        // markers executed (CP thread)
// Rollback (rollback.cpp): frames re-simulated in one host frame still run the game's draw, so its
// job and render pipeline keeps its rhythm, but the command processor discards their work: no draws,
// no resolves, memory waits (the vblank token) satisfied at once, nothing presented. Frames by
// marker number, [from, to).
static std::atomic<uint32_t> g_discardFrom{ 0 }, g_discardTo{ 0 };
uint32_t GpuCpMarkers() { return g_cpMarkers; }   // rollback.cpp diagnostics (racy read)
void GpuDiscardNextFrames(uint32_t n)
{
    const uint32_t from = g_markerSwaps.load() + 1;
    g_discardTo.store(from + n);
    g_discardFrom.store(from);
}
// The re-simulation is over: the window ends after the frames written so far (fewer than asked when a
// re-simulation stopped early), so the next frame is shown.
void GpuDiscardEndNow() { g_discardTo.store(g_markerSwaps.load() + 1); }
static bool CpDiscarding()                               // CP thread: the frame it is executing
{
    const uint32_t frame = g_cpMarkers + 1;
    return frame >= g_discardFrom.load(std::memory_order_relaxed) && frame < g_discardTo.load(std::memory_order_relaxed);
}
// Before a rewind: frames already handed to the GPU finish now, without waiting for the vblank
// (their memory waits pass at once; early present has shown them already). A restore rewinds the
// D3D runtime's dynamic-buffer bookkeeping, and a frame still in flight would read buffers the
// re-simulated frames then reuse (MEASURED: fighters drawn with garbage vertices).
static std::atomic<uint32_t> g_noPacingThrough{ 0 };
static bool CpUnpaced() { return g_cpMarkers + 1 <= g_noPacingThrough.load(std::memory_order_relaxed); }

// D3D's vblank fence (sub_82740EF0, MEASURED): each frame's swap sets SCRATCH_REG1 = 1, queues a
// flip-queue entry through the swap interrupt (sub_82740610: due = the previous entry's vblank + the
// interval, at the earliest now) and waits until the vblank DPC (sub_827404E0) retires it, which sets
// SCRATCH_REG1 = 0. Discarded and unpaced frames skip that wait, so their entries stayed queued: every
// later frame's vblank moved out by one per rewound frame, and the stale entries retired later, one
// per vblank - one cleared SCRATCH_REG1 right after a live frame set it, and that frame's "== 1" wait
// never passed (MEASURED: [pm4-fault] after ~300 rewinds of 7 frames). Their swaps now complete at
// once, and the vblank schedule stays the live frames'.
static constexpr uint32_t kFlipLock = 17120, kFlipHead = 17108, kFlipTail = 17112, kFlipQueue = 16980,
                          kVblanks = 16952, kLastDue = 16960, kFlips = 16976, kScratchShadow = 11028;
extern "C" PPC_FUNC(__imp__sub_82740610);
PPC_FUNC(sub_82740610)
{
    const uint32_t dev = g_intrContext.load();
    if (!dev || !(CpDiscarding() || CpUnpaced()))
    {
        __imp__sub_82740610(ctx, base);
        return;
    }
    // Due now: the previous entry's vblank one interval before the current one (the ISR takes the
    // later of that + the interval and now, under its lock) and no "too late in this vblank" bump
    // (its threshold, the argument's low byte, above any percentage). Then the live schedule back.
    const uint32_t lastDue = GRead32(base, dev + kLastDue);
    GWrite32(base, dev + kLastDue, GRead32(base, dev + kVblanks) - ((ctx.r3.u32 >> 8) & 0xF));
    ctx.r3.u32 |= 0xFF;
    __imp__sub_82740610(ctx, base);
    GWrite32(base, dev + kLastDue, lastDue);
}

// After the drain: a frame that queued its entry before the drain started and then skipped its wait
// left the entry behind. Retire what is queued now, as the DPC would, under the queue's lock.
static void RetireQueuedFlips()
{
    const uint32_t dev = g_intrContext.load();
    if (!dev) return;
    std::atomic_ref<uint32_t> lock(*reinterpret_cast<uint32_t*>(g_base + dev + kFlipLock));
    while (lock.exchange(1, std::memory_order_acquire) != 0) _mm_pause();
    const uint32_t tail = GRead32(g_base, dev + kFlipTail), shadow = GRead32(g_base, dev + kScratchShadow);
    uint32_t head = GRead32(g_base, dev + kFlipHead);
    for (; head != tail; ++head)
    {
        const uint32_t fb = GRead32(g_base, dev + kFlipQueue + (head & 15) * 8);
        GWrite32(g_base, dev + kFlips, GRead32(g_base, dev + kFlips) + 1);
        if (fb) GWrite32(g_base, 0x7FC86110u, fb);
        else if (shadow) GWrite32(g_base, shadow + 4, 0);
    }
    GWrite32(g_base, dev + kFlipHead, head);
    const uint32_t now = GRead32(g_base, dev + kVblanks);
    if (int32_t(GRead32(g_base, dev + kLastDue) - now) > 0) GWrite32(g_base, dev + kLastDue, now);   // next swap: next vblank
    lock.store(0, std::memory_order_release);
}

bool GpuRingDrained();
uint32_t GpuRingPendingBytes();
// A rewind that does not wait (rollback.cpp, its re-simulation's skinned vertices in scratch memory):
// the frames already handed over run without their vblank waits, as for GpuFinishInFlight, so the
// command buffers they hold free up for the re-simulation's draws at once.
// And the flip queue is retired right after the last of those frames (RetireQueuedFlips), as the
// drain does once they are done: by the command processor when it executes that frame's marker
// (CpRetireAfterUnpaced), or here when it is done already - whichever claims g_retireAt first.
static std::atomic<uint32_t> g_retireAt{ 0 };     // marker after which the flip queue is retired (0 none)
static void CpRetireAfterUnpaced()                 // CP thread, after each marker
{
    std::atomic_thread_fence(std::memory_order_seq_cst);   // the marker count before the claim (vs GpuUnpaceInFlight)
    const uint32_t at = g_retireAt.load(std::memory_order_seq_cst);
    if (at && g_cpMarkers >= at && g_retireAt.exchange(0) != 0) RetireQueuedFlips();
}
void GpuUnpaceInFlight()
{
    const uint32_t through = g_markerSwaps.load();
    g_noPacingThrough.store(through);
    g_retireAt.store(through ? through : 1, std::memory_order_seq_cst);
    GpuProgress();
    if (g_cpMarkers >= through && g_retireAt.exchange(0) != 0) RetireQueuedFlips();
}
// `discardInFlight`: the frames already handed over but not executed yet are dropped instead of drawn -
// the command processor still walks their packets (fences, markers, waits), fast, but executes no draws,
// resolves or presents (as for re-simulated frames); the screen keeps the last presented frame until
// the corrected one. rollback.cpp decides (RewindDropsInFlight).
bool GpuFinishInFlight(int maxMs, bool discardInFlight)
{
    const timeline::Scope drainMark{ timeline::RbDrain };
    if (discardInFlight)
    {
        const uint32_t from = g_cpMarkers + 1, to = g_markerSwaps.load() + 1;
        if (to > from)
        {
            g_discardTo.store(to);
            g_discardFrom.store(from);
        }
    }
    g_noPacingThrough.store(g_markerSwaps.load());
    GpuProgress();
    const auto start = std::chrono::steady_clock::now();
    const uint32_t pendingAtStart = GpuRingPendingBytes();
    const uint32_t markersAtStart = g_cpMarkers, swapsAtStart = g_markerSwaps.load();
    uint32_t sleeps = 0, wokeByProgress = 0;
    bool drained = true;
    while (!GpuRingDrained())
    {
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(maxMs)) { drained = false; break; }
        ++sleeps;
        if (WaitGpuProgress(g_gpuProgress.load(std::memory_order_acquire), 1)) ++wokeByProgress;
    }
    static int traced = 0;
    if (traced++ < 40)
        std::fprintf(stderr, "[drain] %.2f ms: %u ring bytes pending, CP markers %u of %u swaps, %u sleeps (%u woken by progress)\n",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), pendingAtStart,
                     markersAtStart, swapsAtStart, sleeps, wokeByProgress);
    RetireQueuedFlips();
    GpuProgress();
    return drained;
}
static uint32_t g_earlySwap = 0;                        // the marker number presented early (CP)
static bool g_earlyBroken = false;                      // a marker did not match: early present off
// The game's swap: wait for the vblank token (the previous flip done), resolve the frame from EDRAM
// into the front buffer, marker. Early present does that resolve itself when the wait starts, with
// the arguments of the previous frame's front resolve; the game's own repeats it after the wait.
struct FrontResolveArgs
{
    uint32_t key = 0, dest = 0, pitch = 0, height = 0, latch = 0, src = 0, depthBase = 0, format = 0;
    bool hasLatch = false, swap = false, depthFloat = false;
    bool operator==(const FrontResolveArgs& o) const
    {
        return key == o.key && dest == o.dest && pitch == o.pitch && height == o.height && latch == o.latch && src == o.src &&
               depthBase == o.depthBase && format == o.format && hasLatch == o.hasLatch && swap == o.swap && depthFloat == o.depthFloat;
    }
};
static FrontResolveArgs g_frontArgs, g_earlyArgs;       // CP: the last front resolve / the early one
static uint32_t g_frontArgsAt = ~0u;                    // markers executed when the last one ran
static bool g_earlyMismatch = false;                    // this frame's own differed: present at its marker

static void HwPresentFrame(uint32_t fb)
{
    const timeline::Scope presentMark{ timeline::CpPresent, fb };
    g_cpPresents.fetch_add(1, std::memory_order_relaxed);
    const auto presentStart = std::chrono::steady_clock::now();
    if (InputTimingOn())
        std::fprintf(stderr, "[present] #%llu at %.1f ms\n", (unsigned long long)g_cpPresents.load(),
                     std::chrono::duration<double, std::milli>(presentStart.time_since_epoch()).count());
    static uint32_t ramp[256], rampVersion = ~0u;
    if (const uint32_t v = g_gammaVersion.load(); v != rampVersion)
    {
        std::lock_guard<std::mutex> lock(g_gammaMutex);
        std::memcpy(ramp, GammaRamp(), sizeof(ramp));
        rampVersion = v;
    }
    if (!fb || !GpuHwPresent(fb, SwizzleSwapsRB(g_frontBufferSwizzle.load()), ramp, rampVersion)) return;
    frame_benchmark::Present();
    ttt2_hw::draw_skip_probe::Present();
    if (FrameTimeLog())
    {
        static FrameTimer presents{ "present" };
        presents.Mark(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count());
    }
    static uint32_t frames = 0;
    static const int seqEvery = [] {
        const char* e = std::getenv("TTT2_FBSEQ");
        return e ? (int)std::strtol(e, nullptr, 0) : 0;
    }();
    // A burst on request (TTT2_FBDUMP=request): fbdump.request holding "seq <count> <every>" writes
    // <count> frames, one every <every> presents, as port/fb/burst_NNNN.bmp (a short scene such as a
    // fight intro, without TTT2_FBSEQ dumping the whole run).
    static uint32_t burstLeft = 0, burstEvery = 1, burstIndex = 0;
    const bool burst = burstLeft && frames % burstEvery == 0;
    const bool seq = (seqEvery > 0 && frames % (uint32_t)seqEvery == 0) || burst;
    // fbdump.bmp every 60 presents (nav.py's screenshots); TTT2_FBDUMP=0 (installed copies) skips
    // the read-back. TTT2_FBDUMP=request writes it only when fbdump.request appears (nav.py asks),
    // so timing runs have no periodic read-back stall.
    static const int fbdump = [] {
        const char* e = std::getenv("TTT2_FBDUMP");
        return (e && *e == '0') ? 0 : (e && std::strcmp(e, "request") == 0) ? 2 : 1;
    }();
    static const std::string request = [] {
        const char* tag = std::getenv("TTT2_INSTANCE");
        return std::string("C:/ttt2/port/fbdump") + (tag ? tag : "") + ".request";
    }();
    bool dump = fbdump == 1 && frames % 60 == 0;
    if (fbdump == 2 && frames % 10 == 0 && DebugExists(request.c_str()))
    {
        unsigned count = 0, every = 1;
        if (FILE* f = DebugOpen(request.c_str(), "r"))
        {
            if (std::fscanf(f, "seq %u %u", &count, &every) != 2) count = 0;
            std::fclose(f);
        }
        if (count) { burstLeft = count; burstEvery = (std::max)(1u, every); burstIndex = 0; DebugRemove(request.c_str()); }
        else dump = DebugRemove(request.c_str());
    }
    if (seq || dump)
    {
        static std::vector<uint32_t> staging(FB_W * FB_H);
        if (GpuHwReadFrontBuffer(staging.data()))
        {
            if (seq)
            {
                DebugDirectory("C:/ttt2/port/fb");
                char fp[128];
                if (burst)
                {
                    BoundedSnprintf(fp, sizeof(fp), "C:/ttt2/port/fb/burst_%04u.bmp", burstIndex);
                    // When this frame was presented (input latency tests: compare with the
                    // "[input] buttons ... visible" line, same clock).
                    std::fprintf(stderr, "[burst] %04u at %.1f ms\n", burstIndex++,
                                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count());
                    --burstLeft;
                }
                else BoundedSnprintf(fp, sizeof(fp), "C:/ttt2/port/fb/frame_%04u.bmp", frames / (uint32_t)seqEvery);
                WriteBmpTo(fp, staging.data());
            }
            if (dump)
            {
                WriteFrontBufferBmp(staging.data());
                uint32_t nz = 0;
                for (uint32_t i = 0; i < FB_W * FB_H; i += 97) nz += staging[i] != 0;
                std::fprintf(stderr, "[vd] present(hw) fb=%08X frame=%u staging[mid]=%08X nz=%u\n",
                             fb, frames, staging[(FB_H / 2) * FB_W + FB_W / 2], nz);
            }
        }
    }
    ++frames;
}

// --- PM4 command processor (own thread, asynchronous to the vblank ticker). Walks the
// ring from rptr to the doorbell-published wptr, executes indirect buffers, and implements
// the packets the D3D runtime's fence machinery depends on (spec: PROJECT_STATUS session 5):
//   - type-0 register writes; SCRATCH_REG0..7 (0x578-0x57F) mirrored to the guest scratch
//     shadow at [device+11028] (the ISR reads the fence callback fn/arg from there!)
//   - WAIT_REG_MEM (0x3C): poll guest memory or our register file until satisfied
//   - CP_INTERRUPT (0x54): invoke the guest ISR with source=1 for each CPU in the mask
//   - EVENT_WRITE_SHD (0x58): fence value writes (segment retirement via sysCmdBuf+0/+4)
//   - MEM_WRITE (0x3D), INDIRECT_BUFFER (0x3F) / _PFD (0x37), INVALIDATE_STATE etc. ---
static uint32_t g_opcodeSeen[128];
static std::atomic<uint32_t> g_ringRptr{0}; // CP owns writes; vblank observes ring progress
static uint32_t g_regFile[0x8000];     // GPU register file (word index)

// ---- GPU milestone 1: EDRAM fast-clear + resolve-to-memory (CPU-only). With no draws
// rasterized yet, EDRAM content per color_base is fully described by the last clear, so we
// latch clear colors and fill the resolve destination with them. The present window shows
// the result (the game's real clear colors = first non-black pixels). ----
static std::unordered_map<uint32_t, uint32_t> g_edramClear;   // color_base -> RB_COLOR_CLEAR (red in the low byte)
static std::atomic<uint32_t> g_resolvesToFront{ 0 };  // resolves into the presented surface
static uint32_t g_curVs = 0, g_curPs = 0;                     // latched ucode guest addrs
static uint32_t g_curVsSize = 0, g_curPsSize = 0;             // ucode size in dwords

// ---- Bring-up instrumentation (env-gated; a normal run stays quiet) ----------------
// TTT2_PM4TRACE=<n> : dump the first n PM4 packets in stream order with their payload.
// TTT2_DRAWDUMP=<n> : dump the FULL pipeline state at the first n RECTLIST draws
//                     (all constants, fetch constants, viewport, shader ucode).
static int g_pm4Trace = -1;
static int g_drawDump = -1;

static int EnvInt(const char* name, int dflt)
{
    const char* s = std::getenv(name);
    return s ? (int)std::strtol(s, nullptr, 0) : dflt;
}

static void InitGpuTrace()
{
    if (g_pm4Trace < 0) g_pm4Trace = EnvInt("TTT2_PM4TRACE", 0);
    if (g_drawDump < 0) g_drawDump = EnvInt("TTT2_DRAWDUMP", 0);
}

// TTT2_SHADER_DUMP=1: write each new shader's microcode to port/shaders (bring-up). Off by
// default: it hashed every shader load and wrote a file + log line per new shader on the CP
// thread, exactly when a fight starts or a new effect appears.
static void DumpShaderBlob(uint32_t type, uint32_t addr, uint32_t sizeDwords)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_SHADER_DUMP"); return e && *e == '1'; }();
    if (!on) return;
    uint64_t h = 1469598103934665603ull;                      // FNV-1a over the ucode
    for (uint32_t i = 0; i < sizeDwords; ++i)
    {
        h ^= GRead32(g_base, addr + i * 4);
        h *= 1099511628211ull;
    }
    static std::unordered_map<uint64_t, bool> seen;
    if (seen.count(h)) return;
    seen[h] = true;
    DebugDirectory("C:/ttt2/port/shaders");
    char path[128];
    BoundedSnprintf(path, sizeof(path), "C:/ttt2/port/shaders/%s_%016llX.bin",
                  type == 0 ? "vs" : "ps", (unsigned long long)h);
    if (FILE* f = DebugOpen(path, "wb"))
    {
        for (uint32_t i = 0; i < sizeDwords; ++i)
        {
            uint32_t d = GRead32(g_base, addr + i * 4);
            std::fwrite(&d, 4, 1, f);
        }
        std::fclose(f);
    }
    std::fprintf(stderr, "[gpu] shader %s %u dwords -> %s\n", type == 0 ? "VS" : "PS",
                 sizeDwords, path);
}

// TTT2_DUMPCONTAINERS=<seconds>: after that long, sweep the guest heaps for Xbox 360 shader
// CONTAINERS and write each one out for offline XenosRecomp translation. The executable image
// only holds XDK library shaders (video decode / XUI / 2D sprite) - TTT2's own shaders live
// compressed inside the dataNNN.bin archives, so the only place they are ever in container
// form is guest RAM, after the loader has decompressed them. Validity test matches
// tools/XenosRecomp/XenosRecomp/main.cpp:91-94 exactly, and the bytes are written RAW
// (big-endian, as they sit in guest memory) because that is what XenosRecomp parses.
static void SweepShaderContainers()
{
    DebugDirectory("C:/ttt2/port/shaders");
    DebugDirectory("C:/ttt2/port/shaders/containers");
    struct { uint32_t lo, hi; } ranges[] = {
        { VHEAP_BEGIN, VHEAP_END }, { PHEAP_BEGIN, PHEAP_END } };
    uint32_t found = 0;
    for (auto& rg : ranges)
    {
        for (uint32_t a = rg.lo; a + 40 < rg.hi; a += 4)
        {
            if ((GRead32(g_base, a) & 0xFFFFFF00u) != 0x102A1100u) continue;
            uint32_t virt = GRead32(g_base, a + 4), phys = GRead32(g_base, a + 8);
            uint32_t f1C = GRead32(g_base, a + 28), f20 = GRead32(g_base, a + 32);
            uint64_t dataSize = (uint64_t)virt + phys;
            if (f1C || f20 || dataSize < 40 || dataSize > 0x40000 ||
                a + dataSize >= rg.hi) continue;
            char path[160];
            BoundedSnprintf(path, sizeof(path),
                          "C:/ttt2/port/shaders/containers/gc_%08X.bin", a);
            if (FILE* f = DebugOpen(path, "wb"))
            {
                std::fwrite(g_base + a, 1, (size_t)dataSize, f);
                std::fclose(f);
                ++found;
            }
            a += (uint32_t)dataSize - 4;
        }
    }
    std::fprintf(stderr, "[gpu] container sweep: %u written to port/shaders/containers\n", found);
}

// Dump every piece of pipeline state a software rasterizer would need, at a real draw.
// Constant banks live in the same register file as context registers on Xenos:
//   0x4000..0x47FF ALU float constants (512 x float4), 0x4800..0x48BF fetch constants
//   (32 texture x 6 dwords == 96 vertex x 2 dwords), 0x4900.. bool, 0x4908.. loop.
static int g_dumpSkip = -1;            // TTT2_DUMPSKIP: ignore the first n content draws

static void DumpDrawState(uint32_t initiator, uint32_t op)
{
    if (g_drawDump <= 0) return;
    if (g_dumpSkip < 0)
    {
        const char* e = std::getenv("TTT2_DUMPSKIP");
        g_dumpSkip = e ? (int)std::strtol(e, nullptr, 0) : 0;
    }
    if (g_dumpSkip > 0) { --g_dumpSkip; return; }   // let the budget fire late in the run
    --g_drawDump;
    uint32_t prim = initiator & 0x3F, srcSel = (initiator >> 6) & 3;
    uint32_t numIdx = initiator >> 16;
    std::fprintf(stderr,
        "[dd] ===== DRAW op=%02X initiator=%08X prim=%u srcSel=%u numIndices=%u =====\n",
        op, initiator, prim, srcSel, numIdx);

    // Context registers: everything nonzero in the 0x2000 block, in one place.
    for (uint32_t r = 0x2000; r < 0x2400; ++r)
        if (g_regFile[r]) std::fprintf(stderr, "[dd] ctx %04X = %08X\n", r, g_regFile[r]);
    // Non-context / config registers the stream touched.
    for (uint32_t r = 0x0000; r < 0x2000; ++r)
        if (g_regFile[r]) std::fprintf(stderr, "[dd] cfg %04X = %08X\n", r, g_regFile[r]);

    // ALU float constants (report as float4 rows; only nonzero rows).
    uint32_t aluRows = 0;
    for (uint32_t i = 0; i < 512; ++i)
    {
        const uint32_t* c = &g_regFile[0x4000 + i * 4];
        if (!(c[0] | c[1] | c[2] | c[3])) continue;
        if (++aluRows > 400) { std::fprintf(stderr, "[dd] alu ... (more)\n"); break; }
        float f[4];
        for (int k = 0; k < 4; ++k) std::memcpy(&f[k], &c[k], 4);
        std::fprintf(stderr, "[dd] alu c%-3u = %08X %08X %08X %08X  (%g %g %g %g)\n",
                     i, c[0], c[1], c[2], c[3], f[0], f[1], f[2], f[3]);
    }
    // Raw fetch-constant block: print all 192 dwords in rows of 6 (texture-fetch view).
    for (uint32_t i = 0; i < 32; ++i)
    {
        const uint32_t* c = &g_regFile[0x4800 + i * 6];
        if (!(c[0] | c[1] | c[2] | c[3] | c[4] | c[5])) continue;
        std::fprintf(stderr, "[dd] fetch6 slot%-2u = %08X %08X %08X %08X %08X %08X\n",
                     i, c[0], c[1], c[2], c[3], c[4], c[5]);
    }
    for (uint32_t i = 0; i < 96; ++i)
    {
        const uint32_t* c = &g_regFile[0x4800 + i * 2];
        if (!(c[0] | c[1])) continue;
        // A vertex fetch constant is {type(1:0)=3 | address(31:2), endian(1:0) | size(25:2)}.
        uint32_t addr = PhysToGuest(c[0] & ~3u), sz = (c[1] >> 2) & 0xFFFFFF;
        std::fprintf(stderr, "[dd] fetch2 slot%-2u = %08X %08X  type=%u addr=%08X size=%u dw\n",
                     i, c[0], c[1], c[0] & 3, addr, sz);
        // Dump the head of the buffer whatever the size field says - the size is in dwords and
        // is routinely far larger than anything worth printing, but the first few vertices are
        // exactly what tells us whether a fetch is reading real data.
        if ((c[0] & 3) != 3 || !sz || !addr) continue;
        for (uint32_t k = 0; k < sz && k < 32; k += 4)
        {
            char line[200]; int n = 0;
            n += BoundedSnprintf(line + n, sizeof(line) - n, "[dd]   vb+%02u:", k);
            for (uint32_t j = 0; j < 4 && k + j < sz; ++j)
                n += BoundedSnprintf(line + n, sizeof(line) - n, " %08X",
                                   GRead32(g_base, addr + (k + j) * 4));
            n += BoundedSnprintf(line + n, sizeof(line) - n, "  f:");
            for (uint32_t j = 0; j < 4 && k + j < sz; ++j)
            {
                uint32_t u = GRead32(g_base, addr + (k + j) * 4);
                float f; std::memcpy(&f, &u, 4);
                n += BoundedSnprintf(line + n, sizeof(line) - n, " %g", f);
            }
            BoundedSnprintf(line + n, sizeof(line) - n, "\n");
            std::fputs(line, stderr);
        }
    }
    for (uint32_t r = 0x4900; r < 0x4940; ++r)
        if (g_regFile[r]) std::fprintf(stderr, "[dd] boolloop %04X = %08X\n", r, g_regFile[r]);

    // Shader ucode currently bound, with a full hex dump so it can be disassembled offline.
    struct { const char* n; uint32_t a, sz; } sh[2] = {
        { "VS", g_curVs, g_curVsSize }, { "PS", g_curPs, g_curPsSize } };
    for (auto& s : sh)
    {
        std::fprintf(stderr, "[dd] %s ucode @%08X %u dwords\n", s.n, s.a, s.sz);
        for (uint32_t i = 0; i < s.sz && i < 256; i += 6)
        {
            char line[160]; int n = 0;
            n += BoundedSnprintf(line + n, sizeof(line) - n, "[dd] %s+%03u:", s.n, i);
            for (uint32_t k = 0; k < 6 && i + k < s.sz; ++k)
                n += BoundedSnprintf(line + n, sizeof(line) - n, " %08X",
                                   GRead32(g_base, s.a + (i + k) * 4));
            BoundedSnprintf(line + n, sizeof(line) - n, "\n");
            std::fputs(line, stderr);
        }
    }
    std::fprintf(stderr, "[dd] ===== end draw =====\n");
}

// ---- Frame inventory (diagnostics). Create C:/ttt2/port/framelog.trigger while the game
// runs: the next complete frame (swap to swap) is written to C:/ttt2/port/framelog_<n>.txt with
// every draw's render targets, formats, state and sampled texture formats, and every resolve's
// source/destination. Field layouts: Xenia registers.h / xenos.h (tools/ref_xenia).
static FILE* g_frameLog = nullptr;
extern void (*g_gpuFrameNote)(const char*);
static void FrameLogNote(const char* text) { if (g_frameLog) std::fputs(text, g_frameLog); }
static uint32_t g_frameLogDraws = 0;
static void FrameLogSwap()
{
    static uint32_t frame = 0, check = 0;
    ++frame;
    if (g_frameLog)
    {
        std::fprintf(g_frameLog, "== end of frame %u (%u draws)\n", frame - 1, g_frameLogDraws);
        std::fclose(g_frameLog);
        g_frameLog = nullptr;
        g_gpuFrameNote = nullptr;
        std::fprintf(stderr, "[framelog] frame %u written\n", frame - 1);
    }
    if (++check % 30) return;                          // poll the trigger twice a second
    if (!DebugExists("C:/ttt2/port/framelog.trigger")) return;
    DebugRemove("C:/ttt2/port/framelog.trigger");
    char path[96];
    BoundedSnprintf(path, sizeof(path), "C:/ttt2/port/framelog_%u.txt", frame);
    g_frameLog = DebugOpen(path, "w");
    g_frameLogDraws = 0;
    g_gpuFrameNote = g_frameLog ? FrameLogNote : nullptr;
    if (g_frameLog) std::fprintf(g_frameLog, "== frame %u\n", frame);
}
static void FrameLogTexture(uint32_t slot)
{
    const uint32_t* c = &g_regFile[0x4800 + slot * 6];
    if ((c[0] & 3) != 2) { std::fprintf(g_frameLog, "    t%u: not a texture (type %u)\n", slot, c[0] & 3); return; }
    std::fprintf(g_frameLog,
        "    t%u: base=%08X fmt=%u %s endian=%u %ux%u pitch=%u dim=%u swz=%03X clamp=%u/%u/%u "
        "filt mag%u min%u mip%u miplv %u-%u mipaddr=%08X packed=%u sign=%X numfmt=%u expadj=%d\n",
        slot, (c[1] >> 12) << 12, c[1] & 0x3F, (c[0] >> 31) ? "tiled" : "linear", (c[1] >> 6) & 3,
        (c[2] & 0x1FFF) + 1, ((c[2] >> 13) & 0x1FFF) + 1, ((c[0] >> 22) & 0x1FF) * 32, (c[5] >> 9) & 3,
        (c[3] >> 1) & 0xFFF, (c[0] >> 10) & 7, (c[0] >> 13) & 7, (c[0] >> 16) & 7,
        (c[3] >> 19) & 3, (c[3] >> 21) & 3, (c[3] >> 23) & 3, (c[4] >> 2) & 15, (c[4] >> 6) & 15,
        (c[5] >> 12) << 12, (c[5] >> 11) & 1, (c[0] >> 2) & 0xFF, c[3] & 1, ((int32_t)(c[3] << 13)) >> 26);
}
static void FrameLogDraw(uint32_t init)
{
    if (!g_frameLog) return;
    const uint32_t* r = g_regFile;
    ++g_frameLogDraws;
    const uint32_t mode = r[0x2208] & 7, surf = r[0x2000], ci = r[0x2001], di = r[0x2002];
    std::fprintf(g_frameLog,
        "draw %u prim=%u n=%u src=%u mode=%u | surf pitch=%u msaa=%u | c0 base=%03X fmt=%u bias=%d mask=%04X | "
        "c1=%08X c2=%08X c3=%08X | depth base=%03X fmt=%u ctl=%08X | blend0=%08X b1=%08X | cull=%08X vte=%08X | "
        "vs=%08X/%u ps=%08X/%u sqcntl=%08X misc=%08X\n",
        g_frameLogDraws, init & 0x3F, init >> 16, (init >> 6) & 3, mode, surf & 0x3FFF, (surf >> 16) & 3,
        ci & 0xFFF, (ci >> 16) & 15, ((int32_t)(ci << 6)) >> 26, r[0x2104] & 0xFFFF,
        r[0x2003], r[0x2004], r[0x2005], di & 0xFFF, (di >> 16) & 1, r[0x2200], r[0x2201], r[0x2209],
        r[0x2205], r[0x2206], g_curVs, g_curVsSize, g_curPs, g_curPsSize, r[0x2180], r[0x2181]);
    if (g_curPs && g_curPsSize && g_curPsSize < 4096)
    {
        // The pixel shader's microcode hash, as TTT2_SHADER_DUMP names port/shaders/ps_<hash>.bin
        // (written here too, so the frame's shaders can be disassembled with ucode_disasm.py).
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < g_curPsSize; ++i) { h ^= GRead32(g_base, g_curPs + i * 4); h *= 1099511628211ull; }
        std::fprintf(g_frameLog, "    ps hash %016llX\n", (unsigned long long)h);
        char path[128];
        BoundedSnprintf(path, sizeof(path), "C:/ttt2/port/shaders/ps_%016llX.bin", (unsigned long long)h);
        if (!DebugExists(path))
            if (FILE* f = DebugOpen(path, "wb"))
            {
                for (uint32_t i = 0; i < g_curPsSize; ++i)
                {
                    const uint32_t w = GRead32(g_base, g_curPs + i * 4);
                    std::fwrite(&w, 4, 1, f);
                }
                std::fclose(f);
            }
    }
    if (mode == 6)
    {
        const uint32_t cc = r[0x2318], info = r[0x231B];
        std::fprintf(g_frameLog,
            "  RESOLVE src=%u sample=%u clrC=%u clrZ=%u cmd=%u dest=%08X pitch=%u h=%u | dest fmt=%u endian=%u "
            "num=%u bias=%d swap=%u array=%u slice=%u | clear c=%08X z=%08X | win_off=%08X sc=%08X..%08X\n",
            cc & 7, (cc >> 4) & 7, (cc >> 8) & 1, (cc >> 9) & 1, (cc >> 20) & 3, r[0x2319], r[0x231A] & 0x3FFF,
            (r[0x231A] >> 16) & 0x3FFF, (info >> 7) & 0x3F, info & 7, (info >> 13) & 7,
            ((int32_t)(info << 10)) >> 26, (info >> 24) & 1, (info >> 3) & 1, (info >> 4) & 7,
            r[0x231E], r[0x231D], r[0x2080], r[0x2081], r[0x2082]);
        return;
    }
    const uint32_t slots = GpuPixelShaderTextureSlots(g_curPs, g_curPsSize);
    for (uint32_t s = 0; s < 32; ++s)
        if (slots & (1u << s)) FrameLogTexture(s);
}

// Format census (TTT2_FORMAT_CENSUS=1): resolves, gamma LUT writes; textures,
// targets and vertex formats in the hardware backend.
static bool FormatCensusOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_FORMAT_CENSUS"); return e && *e == '1'; }();
    return on;
}

// A draw issued with RB_MODECONTROL edram-mode COPY performs the resolve described by the
// RB_COPY_* registers. copy_command 3 + clear bits = pure fast-clear (latch only).
static uint32_t traceDumpDest = 0;     // resolve-trace: dump this destination after resolving
static void IssueResolve()
{
    uint32_t ctrl = g_regFile[0x2318];
    uint32_t copyCmd = (ctrl >> 20) & 3;
    bool clearColor = (ctrl & (1u << 8)) != 0;
    uint32_t colorBase = g_regFile[0x2001] & 0xFFFu;
    // The hardware backend keys colour surfaces by EDRAM base and colour format.
    const uint32_t hwColorKey = colorBase | (((g_regFile[0x2001] >> 16) & 0xFu) << 12);
    uint32_t clearVal = g_regFile[0x231E];
    uint32_t destBase = PhysToGuest(g_regFile[0x2319] & ~0xFFFu);
    uint32_t pitch = g_regFile[0x231A] & 0x3FFFu;
    uint32_t height = (g_regFile[0x231A] >> 16) & 0x3FFFu;
    uint32_t destInfo = g_regFile[0x231B];
    // The resolve destination is written TILED: the swap packet's own texture fetch block
    // for this exact surface reads 8A000002 A44A9006 ... - dword0 bit31 (tiled) is set,
    // pitch field 40*32 = 1280, format 8_8_8_8 - and the present path un-tiles it.
    // (RB_COPY_DEST_INFO bit3 is copy_dest_array, not a tiling flag.)
    bool tiled = true;
    static int rlog = 0;
    if (rlog < 40)
    {
        ++rlog;
        std::fprintf(stderr,
            "[gpu] resolve cmd=%u ctrl=%08X cbase=%03X clr=%08X dest=%08X %ux%u info=%08X%s\n",
            copyCmd, ctrl, colorBase, clearVal, destBase, pitch, height, destInfo,
            tiled ? " tiled" : "");
    }
    {
        // Resolves of non-8_8_8_8 surfaces or into non-8_8_8_8 destinations (e.g. GPU DXT
        // compression through 64-bpp targets) - the hardware path models only 32-bpp ARGB.
        const uint32_t colorFmt = (g_regFile[0x2001] >> 16) & 0xF, destFmt = (destInfo >> 7) & 0x3F;
        static std::atomic<int> oddLog{ 400 };
        if ((colorFmt != 0 || (destFmt != 6 && (ctrl & 7) != 4)) && oddLog.fetch_sub(1) > 0)
            std::fprintf(stderr, "[resolve-odd] src=%u cbase=%03X colorFmt=%u pitch(surf)=%u msaa=%u dest=%08X %ux%u destFmt=%u endian=%u swap=%u ctrl=%08X\n",
                         ctrl & 7, colorBase, colorFmt, g_regFile[0x2000] & 0x3FFF, (g_regFile[0x2000] >> 16) & 3,
                         destBase, pitch, height, destFmt, destInfo & 7, (destInfo >> 24) & 1, ctrl);
    }
    {
        // Format census (TTT2_FORMAT_CENSUS=1, with the backend's texture/target/vertex census):
        // the first resolve of every source format / destination format / endian / swap.
        const bool census = FormatCensusOn();
        static std::unordered_map<uint32_t, uint32_t> seen;
        const uint32_t key = ((g_regFile[0x2001] >> 16) & 0xF) | (((destInfo >> 7) & 0x3F) << 4) | ((destInfo & 7) << 10) |
                             (((destInfo >> 24) & 1) << 13) | ((ctrl & 7) << 14) | (copyCmd << 17) | ((clearColor ? 1u : 0u) << 19);
        if (census && !seen[key]++)
            std::fprintf(stderr, "[census] resolve src=%u cfmt=%u destFmt=%u endian=%u swap=%u cmd=%u clearC=%u clear=%08X (first: dest=%08X %ux%u)\n",
                         ctrl & 7, (g_regFile[0x2001] >> 16) & 0xF, (destInfo >> 7) & 0x3F, destInfo & 7, (destInfo >> 24) & 1,
                         copyCmd, clearColor ? 1u : 0u, clearVal, destBase, pitch, height);
    }
    {
        // Diagnostics: create C:/ttt2/port/resolvelog.trigger to log the next 400 resolves (e.g.
        // across a loading screen, where the character texture bake renders and resolves).
        static int traceLeft = 0;
        traceDumpDest = 0;
        static uint32_t checkTick = 0;
        if ((++checkTick & 63) == 0 && DebugExists("C:/ttt2/port/resolvelog.trigger"))
        {
            DebugRemove("C:/ttt2/port/resolvelog.trigger");
            traceLeft = 400;
        }
        if (traceLeft > 0)
        {
            --traceLeft;
            std::fprintf(stderr, "[resolve-trace] src=%u cbase=%03X cfmt=%u surf=%u msaa=%u dest=%08X %ux%u destFmt=%u endian=%u swap=%u cmd=%u clr=%u/%u ps=%08X/%u\n",
                         ctrl & 7, colorBase, (g_regFile[0x2001] >> 16) & 0xF, g_regFile[0x2000] & 0x3FFF,
                         (g_regFile[0x2000] >> 16) & 3, destBase, pitch, height, (destInfo >> 7) & 0x3F, destInfo & 7,
                         (destInfo >> 24) & 1, copyCmd, clearColor ? 1u : 0u, (ctrl >> 9) & 1, g_curPs, g_curPsSize);
            traceDumpDest = (pitch == height && (pitch == 512 || pitch == 1024 || pitch == 128)) ? destBase : 0;
        }
    }
    const int hw = GpuHwMode();                                // 0 CPU, 1 GPU, 2 compare
    static std::vector<uint32_t> lin;
    if ((copyCmd == 0 || copyCmd == 1) && hw == 1 &&
        destBase && pitch && height && pitch <= 4096 && height <= 4096)
    {
        // Hardware backend: copy the GPU surface into the GPU texture registered for
        // destBase; guest memory is only written when something needs the bytes. Compare
        // mode resolves after the CPU path below, so the GPU entry also adopts its writes.
        // copy_src_select 4 resolves DEPTH (TTT2's shadow map and scene depth, sampled as
        // 24_8_FLOAT); the CPU path below still copies colour for it (documented divergence).
        auto latch = g_edramClear.find(colorBase);
        FrontResolveArgs args;
        args.key = hwColorKey; args.dest = destBase; args.pitch = pitch; args.height = height;
        args.hasLatch = latch != g_edramClear.end(); args.latch = args.hasLatch ? latch->second : 0;
        args.swap = ((destInfo >> 24) & 1) != 0; args.src = ctrl & 7u;
        args.depthBase = g_regFile[0x2002] & 0xFFFu; args.depthFloat = ((g_regFile[0x2002] >> 16) & 1u) != 0;
        args.format = (destInfo >> 7) & 0x3Fu;
        GpuHwResolve(args.key, args.dest, args.pitch, args.height, args.hasLatch, args.latch, args.swap,
                     args.src, args.depthBase, args.depthFloat, args.format);
        if (destBase == g_frontBufferPhys.load())
        {
            if (g_earlySwap == g_cpMarkers + 1 && !(args == g_earlyArgs)) g_earlyMismatch = true;
            g_frontArgs = args;
            g_frontArgsAt = g_cpMarkers;
        }
        if (traceDumpDest == destBase)
        {
            static uint32_t traceDumps = 0;
            if (traceDumps < 40)
            {
                DebugDirectory("C:/ttt2/port/resolve_trace");
                char path[128];
                BoundedSnprintf(path, sizeof(path), "C:/ttt2/port/resolve_trace/%03u_%08X_%ux%u_ps%u.bmp",
                                traceDumps++, destBase, pitch, height, g_curPsSize);
                GpuHwDumpResolved(destBase, path);
            }
        }
        if (g_frameLog)
        {
            static uint32_t dumpIndex = 0;
            DebugDirectory("C:/ttt2/port/framelog_dump");
            char path[128];
            BoundedSnprintf(path, sizeof(path), "C:/ttt2/port/framelog_dump/%03u_draw%u_src%u_%08X_%ux%u.bmp",
                            dumpIndex++, g_frameLogDraws, ctrl & 7, destBase, pitch, height);
            GpuHwDumpResolved(destBase, path);
        }
    }
    if ((copyCmd == 0 || copyCmd == 1) && hw != 1)              // raw / convert copy (CPU)
    {
        if (destBase && pitch && height && pitch <= 4096 && height <= 4096)
        {
            // Source: the EDRAM-lite surface the rasterizer filled. Falls back to the
            // fast-clear latch when nothing has been rasterized into this colour base.
            lin.assign((size_t)pitch * height, 0);
            bool fromEdram = GpuReadEdram(colorBase, pitch, height, lin.data());
            if (!fromEdram)
            {
                auto it = g_edramClear.find(colorBase);
                uint32_t px = it != g_edramClear.end() ? it->second : 0;
                px = (px & 0xFF00FF00u) | ((px >> 16) & 0xFFu) | ((px & 0xFFu) << 16);   // red low -> ARGB
                std::fill(lin.begin(), lin.end(), px);
            }
            // TTT2_GPUTEST=1: write a recognisable gradient instead, to prove
            // resolve -> tiling -> present -> window end to end independently of the
            // rasterizer (stage 0 of the render bring-up).
            static const bool test = std::getenv("TTT2_GPUTEST") != nullptr;
            uint32_t widthAligned = (pitch + 31) & ~31u;
            // The hardware's k_8_8_8_8 word holds red in the LOW byte (0xAABBGGRR); RB_COPY_DEST_INFO
            // bit 24 copy_dest_swap exchanges red and blue, giving the D3DFMT_A8R8G8B8 word
            // 0xAARRGGBB that the CPU reads and the fetch swizzle .zyxw / .zyx1 samples as RGBA
            // (every battle resolve and the front buffer swap). lin holds 0xAARRGGBB.
            const bool swapRB = (destInfo >> 24) & 1;
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < pitch; ++x)
                {
                    uint32_t off = tiled ? TiledOffset32(x, y, widthAligned) : (y * pitch + x);
                    uint32_t v = lin[(size_t)y * pitch + x];
                    if (!swapRB) v = (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
                    if (test)
                        v = 0xFF000000u | ((x * 255 / pitch) << 16) |
                            ((y * 255 / height) << 8) | (((x ^ y) & 0x20) ? 0xFF : 0x00);
                    GWrite32(g_base, destBase + off * 4, v);
                }
            // TTT2_RESOLVEDUMP=1: the linear EDRAM readback for this pass, before tiling.
            // Shows which stage of the multi-pass chain the transposed image comes from.
            static const bool rdump = std::getenv("TTT2_RESOLVEDUMP") != nullptr;
            // Only dump a pass that actually produced colour, and only twice per
            // source/destination pair: every resolve is a 4 MB write and dumping all of them
            // starves the command processor badly enough that the run never reaches the logo.
            bool worthDumping = false;
            if (rdump)
            {
                for (size_t i = 0; i < lin.size(); i += 17)
                    if (lin[i] & 0x00FFFFFFu) { worthDumping = true; break; }
                static std::unordered_map<uint64_t, int> done;
                uint64_t key = ((uint64_t)colorBase << 32) | destBase;
                if (worthDumping && done[key]++ >= 2) worthDumping = false;
            }
            if (worthDumping)
            {
                DebugDirectory("C:/ttt2/port/tex");
                char rp[160];
                BoundedSnprintf(rp, sizeof(rp), "C:/ttt2/port/tex/resolve_%03X_%08X_%ux%u.bmp",
                              colorBase, destBase, pitch, height);
                if (FILE* rf = DebugOpen(rp, "wb"))
                {
                    uint32_t isz = pitch * height * 4;
                    uint8_t rh[54] = {};
                    rh[0] = 'B'; rh[1] = 'M';
                    *reinterpret_cast<uint32_t*>(rh + 2) = 54 + isz;
                    *reinterpret_cast<uint32_t*>(rh + 10) = 54;
                    *reinterpret_cast<uint32_t*>(rh + 14) = 40;
                    *reinterpret_cast<int32_t*>(rh + 18) = (int32_t)pitch;
                    *reinterpret_cast<int32_t*>(rh + 22) = -(int32_t)height;
                    *reinterpret_cast<uint16_t*>(rh + 26) = 1;
                    *reinterpret_cast<uint16_t*>(rh + 28) = 32;
                    *reinterpret_cast<uint32_t*>(rh + 34) = isz;
                    std::fwrite(rh, 1, sizeof(rh), rf);
                    std::fwrite(lin.data(), 4, (size_t)pitch * height, rf);
                    std::fclose(rf);
                }
            }
            if (rlog < 40)
            {
                uint32_t midOff = TiledOffset32(pitch / 2, height / 2, widthAligned);
                std::fprintf(stderr,
                    "[gpu] resolve src=%s test=%d first=%08X mid=%08X "
                    "wrote[%08X]=%08X wrote[mid]=%08X\n",
                    fromEdram ? "edram" : "clear", (int)test, lin[0],
                    lin[(size_t)(height / 2) * pitch + pitch / 2], destBase,
                    GRead32(g_base, destBase), GRead32(g_base, destBase + midOff * 4));
            }
            if (hw == 2)
            {
                // Diagnostics: the first resolves of each destination-info combination.
                {
                    static std::unordered_map<uint64_t, int> seen;
                    int& n = seen[(uint64_t(destInfo) << 16) ^ pitch];
                    if (n++ < 2)
                        std::fprintf(stderr, "[gpu] resolve dest=%08X %ux%u destInfo=%08X (fmt %u endian %u swap %u) edram fmt %u\n",
                                     destBase, pitch, height, destInfo, (destInfo >> 7) & 0x3Fu, destInfo & 7u,
                                     (destInfo >> 24) & 1u, (g_regFile[0x2001] >> 16) & 0xFu);
                }
                auto latch = g_edramClear.find(colorBase);
                GpuHwResolve(hwColorKey, destBase, pitch, height, latch != g_edramClear.end(),
                             latch != g_edramClear.end() ? latch->second : 0, swapRB,
                             ctrl & 7u, g_regFile[0x2002] & 0xFFFu, ((g_regFile[0x2002] >> 16) & 1u) != 0,
                             (destInfo >> 7) & 0x3Fu);
                if ((ctrl & 7u) != 4u) GpuHwCompareResolve(destBase, lin.data(), pitch, height);
            }
        }
    }
    if (clearColor) g_edramClear[colorBase] = clearVal;
    if (destBase == g_frontBufferPhys.load()) ++g_resolvesToFront;
    // The destination's bytes changed behind the CPU: drop any cached-view copy of them
    // (phys_mirror.cpp). Sized for 64 bpp tiled destinations; closing extra chunks is free.
    if ((copyCmd == 0 || copyCmd == 1) && destBase && pitch && height)
        PhysMirrorInvalidate(destBase, ((pitch + 31) & ~31u) * ((height + 31) & ~31u) * 8u);
}

// SCRATCH_ADDR (reg 0x01DD) holds the PHYSICAL base of the scratch-register memory shadow;
// hardware copies SCRATCH_REG0..7 there for every register whose bit is set in SCRATCH_UMSK
// (reg 0x01DC). The D3D runtime caches the same block's virtual address at device+11028 and
// the CPU side reads AND WRITES it directly: the graphics ISR (sub_82738AD8) clears its own
// CPU bit in [shadow+0] under a spinlock, which is exactly what the "wait scratch0 == 0"
// WAIT_REG_MEM in the fence-arming sequence is waiting for.
static uint32_t ScratchShadow()
{
    uint32_t dev = g_intrContext.load();
    uint32_t s = dev ? GRead32(g_base, dev + 11028) : 0;
    if (s) return s;
    uint32_t phys = g_regFile[0x01DD];
    return phys ? PhysToGuest(phys) : 0;
}

static void MirrorScratch(uint32_t reg, uint32_t value)
{
    // Mirror into the MMIO register aperture too, so a guest read-back of the register
    // sees what the command stream wrote.
    GWrite32(g_base, 0x7FC80000u + reg * 4, value);
    uint32_t shadow = ScratchShadow();
    if (shadow) GWrite32(g_base, shadow + (reg - 0x578) * 4, value);
}

// The guest can also program GPU registers by storing straight into the MMIO aperture,
// which never passes through the command stream. Detect those stores (aperture value
// diverging from what we last wrote) and perform the scratch write-back hardware would.
// One-directional on purpose: values the ISR writes into the shadow must never be
// clobbered by a stale aperture copy, and they cannot be, because the ISR only ever
// touches the shadow.
static void SyncGuestScratchWrites()
{
    uint32_t shadow = ScratchShadow();
    for (uint32_t reg = 0x578; reg <= 0x57F; ++reg)
    {
        uint32_t v = GRead32(g_base, 0x7FC80000u + reg * 4);
        if (v == g_regFile[reg]) continue;
        g_regFile[reg] = v;
        if (shadow) GWrite32(g_base, shadow + (reg - 0x578) * 4, v);
    }
}

// Where do register writes actually land? The shading path needs ALU float constants
// (0x4000..0x47FF) and fetch constants (0x4800..0x48BF); if those buckets stay at zero the
// game is uploading its constants by a route the command processor does not execute.
static uint32_t g_regWrites[4];      // context, ALU const, fetch const, other

// Every primitive type the game actually issues, so the renderer never silently drops one.
static uint32_t g_primSeen[64];

// Which float4 ALU constant rows the game has EVER written. A shader reading a row that is
// never written anywhere means its constant indices are not the absolute register-file indices
// we assume they are.
static uint8_t g_aluEver[512];

// Constant forensics. The game's 2D shader transforms every vertex by c128/c129 (a 2x3 matrix)
// plus c131, and those rows read ZERO at draw time while c132/c133 right next to them are set.
// To tell "never written" from "written, then clobbered" from "written after the draw", stamp
// every write with a sequence number and print both the writes and the draw-time values.
static uint32_t g_regSeq = 0;
static int g_cwBudget = -1;
static bool CwOn()
{
    if (g_cwBudget < 0) g_cwBudget = std::getenv("TTT2_CWATCH")
                                   ? (int)std::strtol(std::getenv("TTT2_CWATCH"), nullptr, 0) : 0;
    return g_cwBudget > 0;
}

static void WriteReg(uint32_t reg, uint32_t value)
{
    ++g_regSeq;
    // Display gamma LUT (DC_LUT_RW_MODE 0x1921 .. DC_LUTA_CONTROL 0x1930, Xenia register_table.inc):
    // a game-loaded ramp is applied at scan-out on the console.
    if (reg >= 0x1920 && reg <= 0x1930 && FormatCensusOn())
    {
        static int logged = 0;
        if (logged++ < 600) std::fprintf(stderr, "[census] gamma lut reg %04X = %08X\n", reg, value);
    }
    if (reg >= 0x4000 + 126 * 4 && reg < 0x4000 + 140 * 4 && CwOn())
    {
        uint32_t row = (reg - 0x4000) / 4, comp = (reg - 0x4000) % 4;
        --g_cwBudget;
        std::fprintf(stderr, "[cw] seq=%u write c%u.%c = %08X\n", g_regSeq, row,
                     "xyzw"[comp], value);
    }
    ++g_regWrites[reg >= 0x4800 && reg < 0x48C0 ? 2
                : reg >= 0x4000 && reg < 0x4800 ? 1
                : reg >= 0x2000 && reg < 0x3000 ? 0 : 3];
    if (reg >= 0x4000 && reg < 0x4800) g_aluEver[(reg - 0x4000) / 4] = 1;
    if (reg < 0x8000)
    {
        g_regFile[reg] = value;
        GWrite32(g_base, 0x7FC80000u + reg * 4, value);   // guest-visible register aperture
    }
    if (reg >= 0x578 && reg <= 0x57F) { MirrorScratch(reg, value); GpuProgress(); }
    if (reg >= 0x1922 && reg <= 0x1925) GammaRegisterWritten(reg, value);
}

static void WriteRegisterRange(uint32_t reg, uint32_t source, uint32_t count)
{
#if defined(TTT2_GEOMETRY_AB_PROBE) && defined(TTT2_ALU_BATCH_EXPERIMENT)
    // Explicitly opted-in diagnostic experiment only: the downstairs Linux
    // comparison showed no throughput gain. All default builds use per-word writes.
    const bool force = (ttt2_hw::geometry_probe::ReferenceMask() & 65536) != 0;
    // Constant-watch diagnostics need the original per-word sequence/logging.
    // A wrapping guest source also needs the original uint32_t address arithmetic.
    if (!force && !CwOn() && uint64_t(source) + uint64_t(count) * 4 <= 0x100000000ull &&
        ttt2_hw::UploadAluConstants(g_regFile, g_aluEver, g_regSeq, g_regWrites[1], reg,
            reinterpret_cast<const uint32_t*>(g_base + source),
            reinterpret_cast<uint32_t*>(g_base + 0x7FC80000u), count))
        return;
#endif
    for (uint32_t i = 0; i < count; ++i)
        WriteReg(reg + i, GRead32(g_base, source + i * 4));
}

static void GammaRegisterWritten(uint32_t reg, uint32_t value)
{
    const auto advance = [] {
        g_gammaComponent = 0;
        g_regFile[0x1922] = (g_regFile[0x1922] & ~0xFFu) | ((g_regFile[0x1922] + 1) & 0xFFu);
    };
    const uint32_t index = g_regFile[0x1922] & 0xFF, enable = g_regFile[0x1927] & 7;
    const bool table = (g_regFile[0x1921] & 1) == 0;     // DC_LUT_RW_MODE: 256-entry table, not PWL
    switch (reg)
    {
    case 0x1922:                                           // DC_LUT_RW_INDEX
        g_gammaComponent = 0;
        break;
    case 0x1923:                                           // DC_LUT_SEQ_COLOR (red, green, blue)
    {
        if (!table) break;
        const uint32_t shift = 20 - 10 * g_gammaComponent;
        if (enable & (1u << (2 - g_gammaComponent)))
        {
            std::lock_guard<std::mutex> lock(g_gammaMutex);
            uint32_t& e = GammaRamp()[index];
            e = (e & ~(0x3FFu << shift)) | ((((value & 0xFFFF) >> 6) & 0x3FF) << shift);
            g_gammaVersion.fetch_add(1);
        }
        if (++g_gammaComponent >= 3) advance();
        break;
    }
    case 0x1924:                                           // DC_LUT_PWL_DATA: 2_10_10_10 front buffers only
    {
        static bool logged = false;
        if (!logged) { logged = true; std::fprintf(stderr, "[gamma] PWL gamma ramp written (not applied: 8_8_8_8 front buffer)\n"); }
        break;
    }
    case 0x1925:                                           // DC_LUT_30_COLOR
    {
        if (!table) break;
        if (enable)
        {
            std::lock_guard<std::mutex> lock(g_gammaMutex);
            uint32_t& e = GammaRamp()[index];
            if (enable & 1) e = (e & ~0x3FFu) | (value & 0x3FFu);
            if (enable & 2) e = (e & ~(0x3FFu << 10)) | (value & (0x3FFu << 10));
            if (enable & 4) e = (e & ~(0x3FFu << 20)) | (value & (0x3FFu << 20));
            g_gammaVersion.fetch_add(1);
        }
        advance();
        break;
    }
    }
}

static void RaiseCpInterrupt(uint32_t cpuMask)
{
    uint32_t cb = g_intrCallback.load(), dev = g_intrContext.load();
    if (!cb) return;
    for (uint8_t cpu = 0; cpu < 6; ++cpu)
    {
        if (!(cpuMask & (1u << cpu))) continue;
        GuestSetCurrentCpu(cpu);
        GuestCall2(cb, 1, dev);        // source 1: the ISR calls [shadow+16]([shadow+20])
    }
    GuestSetCurrentCpu(0);
    GpuProgress();
}

// Command-processor time split for TTT2_FRAMETIME=1 (CP thread only): WAIT_REG_MEM time by
// kind (scratch-register handshakes such as the vblank token, other guest memory, registers),
// and the remainder of ConsumeRing (executing packets) vs. the idle loop.
struct CpTimes
{
    double waitMs[3] = {}, ringMs = 0, idleMs = 0;
    uint64_t waits[3] = {};
    std::unordered_map<uint32_t, std::pair<uint64_t, double>> memoryWaits;   // poll address -> count, ms
};
static CpTimes g_cpTimes;

struct CpWaitClock
{
    int kind;
    uint32_t address;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~CpWaitClock()
    {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        frame_pacing_probe::Wait(unsigned(kind), ms, address);
        g_cpTimes.waitMs[kind] += ms;
        ++g_cpTimes.waits[kind];
        if (kind == 1 && FrameTimeLog())
        {
            auto& w = g_cpTimes.memoryWaits[address];
            ++w.first;
            w.second += ms;
        }
    }
};

// The game's swap sequence (VdSwap: an indirect buffer, then our marker) waits for the next vblank
// on SCRATCH_REG1 (the vblank token: the previous flip done), then resolves frame N from EDRAM into
// the front buffer: frame N is drawn when that wait starts. Resolve and present it then instead of
// at its marker after the wait - MEASURED 2026-10-18: the wait lasted 7-8 ms every frame (the
// console's flip at vblank), half a frame of input latency. The game's own resolve repeats the copy
// and the flip bookkeeping still runs at the marker. Called when the wait starts and again while it
// waits (the marker can be written a moment after the CP gets there; VdSwap wakes the wait).
// Only while the previous frame resolved into this front buffer the same way. TTT2_EARLY_PRESENT=0:
// present at the markers as before.
static void TryEarlyPresent(uint32_t waitInfo, uint32_t pollOn)
{
    if (CpDiscarding()) return;
    const uint32_t pending = g_cpMarkers + 1;
    if (!EarlyPresentOn() || g_earlyBroken || !GpuHwMode() || !(waitInfo & 0x10) || (pollOn & ~3u) != g_regFile[0x01DD] + 4 ||
        g_earlySwap == pending || pending > g_markerSwaps.load())
        return;
    const uint32_t fbSurf = g_markerFb[pending & 15].load();
    if (!fbSurf || g_frontArgsAt + 1 != g_cpMarkers || g_frontArgs.dest != (fbSurf & ~0xFFFu)) return;
    frame_pacing_probe::Ready(pending);
    const FrontResolveArgs& a = g_frontArgs;
    GpuHwResolve(a.key, a.dest, a.pitch, a.height, a.hasLatch, a.latch, a.swap, a.src, a.depthBase, a.depthFloat, a.format);
    g_earlyArgs = a;
    g_earlyMismatch = false;
    g_frontBufferPhys.store(fbSurf & ~0xFFFu);
    g_frontBufferSwizzle.store(fbSurf & 0xFFFu);
    HwPresentFrame(g_frontBufferPhys.load());
    g_earlySwap = pending;
}

// The command processor's work per frame (ms, average): from the end of one vblank-token wait (the
// previous flip done) to the start of the next - the frame's draws. rollback.cpp uses it to judge
// whether a rewind can wait for the frame in flight (GpuCpFrameMs).
static std::atomic<double> g_cpFrameMs{ 4.0 };
static std::chrono::steady_clock::time_point g_cpWaitEnd{};
static bool IsVblankTokenWait(uint32_t waitInfo, uint32_t pollOn)
{
    return (waitInfo & 0x10) && (pollOn & ~3u) == g_regFile[0x01DD] + 4;
}
static void NoteCpFrameWork(uint32_t waitInfo, uint32_t pollOn, bool start)
{
    if (!IsVblankTokenWait(waitInfo, pollOn) || CpDiscarding()) return;
    const auto now = std::chrono::steady_clock::now();
    if (start)
    {
        if (g_cpWaitEnd.time_since_epoch().count())
        {
            const double ms = std::chrono::duration<double, std::milli>(now - g_cpWaitEnd).count();
            if (ms < 100.0) g_cpFrameMs.store(g_cpFrameMs.load() * 0.95 + ms * 0.05);
        }
    }
    else g_cpWaitEnd = now;
}
double GpuCpFrameMs() { return g_cpFrameMs.load(); }

static bool CpWaitYield()
{
    static const bool yield = [] { const char* e = std::getenv("TTT2_CP_WAIT"); return e && std::strcmp(e, "yield") == 0; }();
    return yield;
}
// TTT2_FRAMETIME=1: waits whose value turned up only after a 1 ms sleep ran out (written
// without a GpuProgress bump), counted by address and printed every 600.
static void CpWaitLateWake(uint32_t pollOn)
{
    static std::mutex m;
    static std::unordered_map<uint32_t, uint32_t> counts;
    static uint32_t total = 0;
    if (!FrameTimeLog()) return;
    std::lock_guard l(m);
    ++counts[pollOn];
    if (++total % 600 != 0) return;
    char line[512];
    int n = BoundedSnprintf(line, sizeof(line), "[frame] cp waits woken late (no progress bump), %u so far:", total);
    for (const auto& [a, c] : counts)
        if (n < 440) n += BoundedSnprintf(line + n, sizeof(line) - n, " %08X=%u", a, c);
    BoundedSnprintf(line + n, sizeof(line) - n, "\n");
    std::fputs(line, stderr);
}

// Wait until (value & mask) <func> ref, polling guest memory or the register file.
static bool WaitRegMem(uint32_t waitInfo, uint32_t pollOn, uint32_t ref, uint32_t mask)
{
    struct TimingMark                                   // TTT2_INPUT_TIMING=1: the wait's span
    {
        uint32_t poll;
        uint32_t info = 0;
        double start = InputTimingOn() ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count() : 0;
        ~TimingMark()
        {
            NoteCpFrameWork(info, poll, false);
            if (InputTimingOn())
                std::fprintf(stderr, "[cpwait] %08X from %.1f ms at %.1f ms\n", poll, start,
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }
    } timingMark{ pollOn, waitInfo };
    if ((waitInfo & 0x10) && (CpDiscarding() || CpUnpaced())) return false;   // rollback: no vblank pacing
    const CpWaitClock waitClock{ (waitInfo & 0x10) ? (pollOn < 0x1000 ? 0 : 1) : 2, pollOn };
    const timeline::Scope waitMark{ timeline::CpVblankWait, pollOn };
    bool memSpace = (waitInfo & 0x10) != 0;
    uint32_t func = waitInfo & 0x7;
    // Coherency waits: the stream programs COHER_BASE/SIZE_HOST and then waits for the GPU to
    // acknowledge the cache flush by clearing bits of COHER_STATUS_HOST (0x0A31). We have no
    // caches to flush - acknowledge immediately, the way Xenia's command processor does.
    // Without this the audio-awakened resource-upload paths stalled 3 s per wait, 70+ times a
    // run.
    if (!memSpace && (pollOn & 0x7FFF) == 0x0A31)
    {
        g_regFile[0x0A31] &= ~mask;
        GWrite32(g_base, 0x7FC80000u + 0x0A31u * 4, g_regFile[0x0A31]);
        static int clog = 0;
        if (clog < 6)
        {
            ++clog;
            std::fprintf(stderr, "[vd] coherency ack: status &= ~%08X -> %08X\n",
                         mask, g_regFile[0x0A31]);
        }
        return false;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    // Throttle forensics: the SCRATCH_REG1 wait times out every frame even though the swap
    // DPC provably consumes the vblank-fence token (q16976 == q17112). Log entry and exit of
    // exactly that wait so the run shows what value it saw and for how long.
    static std::atomic<int> wlog{ 200 };
    bool traced = pollOn == 0x13EB4506u && wlog.fetch_sub(1) > 0;
    uint32_t firstSeen = 0xDEADBEEF;
    auto t0 = std::chrono::steady_clock::now();
    bool timedOut = false;
    for (uint64_t spins = 0;; ++spins)
    {
        const uint32_t progress = g_gpuProgress.load(std::memory_order_acquire);
        if (memSpace && CpUnpaced()) return false;    // a rewind's drain started during this wait
        if (memSpace && spins == 0) NoteCpFrameWork(waitInfo, pollOn, true);
        if (memSpace) TryEarlyPresent(waitInfo, pollOn);
        if (memSpace) SyncGuestScratchWrites();
        uint32_t v;
        if (memSpace && pollOn < 0x1000)
        {
            // Tiny "memory" addresses are byte offsets into the scratch-register memory
            // shadow: the fence handshake waits on SCRATCH_REG[off/4]. Poll the SHADOW -
            // the CP writes it via MirrorScratch and the guest ISR acks in it directly.
            uint32_t shadow = ScratchShadow();
            v = shadow ? GRead32(g_base, shadow + (pollOn & ~3u))
                       : GRead32(g_base, 0x7FC80000u + (0x578u + ((pollOn & ~3u) >> 2)) * 4);
        }
        else if (memSpace)
            v = GRead32(g_base, PhysToGuest(pollOn & ~3u));
        else
            v = pollOn < 0x8000 ? GRead32(g_base, 0x7FC80000u + pollOn * 4) : 0;
        v &= mask;
        if (traced && firstSeen == 0xDEADBEEF)
        {
            firstSeen = v;
            std::fprintf(stderr, "[wrm] enter poll=%08X first=%08X\n", pollOn, v);
        }
        bool ok;
        switch (func)
        {
        case 1: ok = v <  ref; break;
        case 2: ok = v <= ref; break;
        case 3: ok = v == ref; break;
        case 4: ok = v != ref; break;
        case 5: ok = v >= ref; break;
        case 6: ok = v >  ref; break;
        default: ok = true; break;
        }
        if (ok)
        {
            if (traced)
            {
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - t0).count();
                std::fprintf(stderr, "[wrm] PASS poll=%08X first=%08X after %lld us\n",
                             pollOn, firstSeen, (long long)us);
            }
            if (timedOut) CpWaitLateWake(pollOn);
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        if (CpWaitYield())
        {
            // TTT2_CP_WAIT=yield: the old loop - yield for up to a frame and a half (Windows
            // sleep_for(500us) really costs ~15 ms), then 1 ms sleeps; real 3 s deadline.
            if (spins < 20000 || now - t0 < std::chrono::milliseconds(25)) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        // The values these waits poll (the vblank token, the fence handshake) are written by
        // the vblank interrupt and the command stream, which bump GpuProgress: sleep on it
        // instead of yielding. MEASURED 2026-10-15: the yield loop cost the CP thread ~250 ms/s
        // of kernel calls in fights. A short pause-spin first catches an imminent write; the
        // 1 ms cap bounds a write nobody announces (a guest store to the register aperture).
        for (int i = 0; i < 200 && g_gpuProgress.load(std::memory_order_acquire) == progress; ++i) _mm_pause();
        timedOut = !WaitGpuProgress(progress, 1);     // the last sleep ran out: no bump announced the write
    }
    uint32_t sh = ScratchShadow();
    std::fprintf(stderr,
        "[vd] WAIT_REG_MEM timeout: info=%X poll=%08X ref=%08X mask=%08X "
        "shadow=%08X sh0=%08X sh4=%08X ap0=%08X ap4=%08X\n",
        waitInfo, pollOn, ref, mask, sh,
        sh ? GRead32(g_base, sh) : 0, sh ? GRead32(g_base, sh + 4) : 0,
        GRead32(g_base, 0x7FC80000u + 0x578u * 4), GRead32(g_base, 0x7FC80000u + 0x579u * 4));
    // Full scratch state: shadow dwords, aperture dwords, and the CP's cached copies. A wait
    // on a scratch-shadow address that times out means one of the three disagrees about who
    // wrote last - print all of them and the disagreement is visible directly.
    if (sh)
    {
        char l3[300]; int n5 = 0;
        n5 += BoundedSnprintf(l3 + n5, sizeof(l3) - n5, "[vd]   scratch sh:");
        for (int i2 = 0; i2 < 8; ++i2)
            n5 += BoundedSnprintf(l3 + n5, sizeof(l3) - n5, " %08X", GRead32(g_base, sh + i2 * 4));
        n5 += BoundedSnprintf(l3 + n5, sizeof(l3) - n5, "  ap:");
        for (int i2 = 0; i2 < 8; ++i2)
            n5 += BoundedSnprintf(l3 + n5, sizeof(l3) - n5, " %08X",
                                GRead32(g_base, 0x7FC80000u + (0x578u + i2) * 4));
        n5 += BoundedSnprintf(l3 + n5, sizeof(l3) - n5, "  rf:");
        for (int i2 = 0; i2 < 8; ++i2)
            n5 += BoundedSnprintf(l3 + n5, sizeof(l3) - n5, " %08X", g_regFile[0x578 + i2]);
        BoundedSnprintf(l3 + n5, sizeof(l3) - n5, "\n");
        std::fputs(l3, stderr);
    }
    return true;
}

struct PacketHistoryEntry { uint32_t address, header, stream, size; int depth; };
static PacketHistoryEntry g_packetHistory[64]{};
static uint64_t g_packetSequence = 0;

static uint32_t LatchImmediateShader(uint32_t stage, uint32_t source, uint32_t words)
{
    // IM_LOAD_IMMEDIATE uploads program bytes, it does not retain a pointer to
    // the command buffer. Primary-ring staging and reusable IBs may change after
    // this packet. The CP executes draws synchronously, so one buffer per stage
    // lasts until that stage's next upload without interfering with the other.
    static uint32_t programs[2]{};
    constexpr uint32_t capacity = 0x10000;
    if (!words || words > capacity / 4 || uint64_t(source) + words * 4ull > 0x100000000ull)
    {
        std::fprintf(stderr, "[pm4-fault] invalid immediate shader upload stage=%u source=%08X words=%u\n",
                     stage, source, words);
        std::fflush(stderr);
        LogDrain(); host_process::ExitNow(6);
        std::abort();
    }
    uint32_t& destination = programs[stage == 0 ? 0 : 1];
    if (!destination)
    {
        void* allocation = g_virtualHeap.Alloc(capacity, 16);
        if (!allocation)
        {
            std::fputs("[pm4-fault] cannot allocate shader program storage\n", stderr);
            std::fflush(stderr);
            LogDrain(); host_process::ExitNow(6);
            std::abort();
        }
        destination = GuestAddr(g_base, allocation);
    }
    std::memcpy(g_base + destination, g_base + source, words * 4);
    return destination;
}

static void CheckPacketBoundary(uint32_t address, uint32_t header, uint32_t start,
                                uint32_t size, uint32_t payloadWords, int depth)
{
    g_packetHistory[g_packetSequence++ % 64] = {address, header, start, size, depth};
    static const bool strict = std::getenv("TTT2_STRICT_GPU") != nullptr;
    bool overflow = uint64_t(address - start) + 4 + uint64_t(payloadWords) * 4 > size;
    uint32_t op = (header >> 8) & 0x7F;
    bool invalidOpcode = strict && (header >> 30) == 3 && (op == 0 || op == 0x7F);
    if (!overflow && !invalidOpcode) return;
    std::fprintf(stderr, "[pm4-fault] %s @%08X h=%08X payload=%u stream=%08X+%X d=%d\n",
                 overflow ? "packet exceeds stream" : "invalid opcode", address, header,
                 payloadWords, start, size, depth);
    for (uint64_t i = g_packetSequence > 64 ? g_packetSequence - 64 : 0;
         i < g_packetSequence; ++i)
    {
        const auto& p = g_packetHistory[i % 64];
        std::fprintf(stderr, "[pm4-history] #%llu d%d @%08X h=%08X stream=%08X+%X\n",
                     (unsigned long long)i, p.depth, p.address, p.header, p.stream, p.size);
    }
    uint32_t first = address - start >= 64 ? address - 64 : start;
    uint64_t limit = std::min<uint64_t>(uint64_t(address) + 96, uint64_t(start) + size);
    for (uint64_t p = first; p + 16 <= limit; p += 16)
        std::fprintf(stderr, "[pm4-bytes] %08X: %08X %08X %08X %08X\n", uint32_t(p),
                     GRead32(g_base, uint32_t(p)), GRead32(g_base, uint32_t(p + 4)),
                     GRead32(g_base, uint32_t(p + 8)), GRead32(g_base, uint32_t(p + 12)));
    std::fflush(stderr);
    LogDrain(); host_process::ExitNow(6);
    std::abort();
}

// TTT2_CP_PROFILE=1 (diagnostics): where the command processor's time goes, per type-3 opcode
// (inclusive of what the opcode calls; INDIRECT_BUFFER itself not counted, its packets are), and the
// swap marker; printed every 600 swaps as ms per frame and us per packet.
static bool CpProfileOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_CP_PROFILE"); return e && *e == '1'; }();
    return on;
}
static int64_t g_cpOpTicks[129] = {};
static uint64_t g_cpOpCount[129] = {};
static int64_t CpQpc() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct CpOpTimer
{
    int op; int64_t t0;
    explicit CpOpTimer(int o) : op(o), t0(CpProfileOn() && o >= 0 ? CpQpc() : 0) {}
    ~CpOpTimer() { if (t0) { g_cpOpTicks[op] += CpQpc() - t0; ++g_cpOpCount[op]; } }
};
static void CpProfileReport()
{
    static uint32_t swaps = 0;
    if (!CpProfileOn() || ++swaps % 600 != 0) return;
    constexpr double ticksPerSecond = 1e9;
    std::vector<std::pair<int64_t, int>> order;
    int64_t total = 0;
    for (int i = 0; i < 129; ++i) if (g_cpOpCount[i]) { order.push_back({ g_cpOpTicks[i], i }); total += g_cpOpTicks[i]; }
    std::sort(order.rbegin(), order.rend());
    char line[1024];
    int n = std::snprintf(line, sizeof(line), "[cp-profile] %.2f ms per frame in packets:", double(total) * 1000.0 / ticksPerSecond / 600.0);
    for (size_t k = 0; k < order.size() && k < 12 && n < 900; ++k)
    {
        const int i = order[k].second;
        n += std::snprintf(line + n, sizeof(line) - n, " %s%02X=%.2fms(%llu/f,%.1fus)", i == 128 ? "swap" : "op", i == 128 ? 0 : i,
                           double(g_cpOpTicks[i]) * 1000.0 / ticksPerSecond / 600.0, (unsigned long long)(g_cpOpCount[i] / 600),
                           double(g_cpOpTicks[i]) * 1e6 / ticksPerSecond / double(g_cpOpCount[i]));
    }
    std::fprintf(stderr, "%s\n", line);
    std::memset(g_cpOpTicks, 0, sizeof(g_cpOpTicks));
    std::memset(g_cpOpCount, 0, sizeof(g_cpOpCount));
}

// ---- TTT2_RB_GPUMEM=1 (diagnostics): guest memory the command processor touches per frame -----------
// Kinds: 0 command buffers (indirect buffers), 1 vertex data, 2 index data, 3 constant loads, 4 writes
// (fences, MEM_WRITE, swap identifiers). Per frame (swap marker to swap marker) the distinct pages of each
// kind and how many rollback.cpp rewinds (RollbackPageRewound); every 600 frames a summary with the
// rewound pages' address ranges.
bool RollbackPageRewound(uint32_t page);   // rollback.cpp: in the hot set, not masked whole
static bool GpuMemOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_RB_GPUMEM"); return e && *e == '1'; }();
    return on;
}
namespace gpumem {
constexpr int kKinds = 6;
const char* const kNames[kKinds] = { "command buffers", "vertex data", "index data", "constant loads", "CP writes", "texture data" };
std::mutex m;
std::vector<uint32_t> epoch[kKinds];      // page -> frame number + 1 it was last counted in
std::vector<uint32_t> pages[kKinds];      // this frame's distinct pages
uint32_t frame = 0;
uint64_t totalPages[kKinds] = {}, rewoundPages[kKinds] = {}, frames = 0;
std::map<uint32_t, uint32_t> rewoundRuns[kKinds];   // first page of a 1 MB block -> times seen rewound
std::vector<uint32_t> lastPages[kKinds];             // the last finished frame's pages
// Rewind measurement: the last frame's pages and their hashes at the rewind's start.
std::vector<std::pair<uint32_t, uint64_t>> rewindHashes[kKinds];
uint64_t rewinds = 0, rewindPages[kKinds] = {}, rewindChanged[kKinds] = {};
std::map<uint32_t, uint32_t> changedRuns[kKinds];   // 1 MB block -> pages seen changed
std::set<uint32_t> changedOutsideRing;               // vertex pages outside the skinned ring, a few
std::map<int, uint32_t> scratchAges;                 // changed scratch pages by age (rewinds)
uint64_t changedAt[2][kKinds] = {};                  // changed after the restore / after the first update
uint64_t HashPage(uint32_t page)
{
    const uint64_t* p = reinterpret_cast<const uint64_t*>(g_base + (uint64_t(page) << 12));
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 512; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
}
void GpuNoteMemory(int kind, uint32_t addr, uint32_t bytes)
{
    if (!GpuMemOn() || !bytes || kind < 0 || kind >= gpumem::kKinds) return;
    std::lock_guard l(gpumem::m);
    auto& ep = gpumem::epoch[kind];
    if (ep.empty()) ep.assign(1u << 20, 0);
    for (uint64_t p = addr >> 12, e = (uint64_t(addr) + bytes + 4095) >> 12; p < e && p < ep.size(); ++p)
        if (ep[p] != gpumem::frame + 1) { ep[p] = gpumem::frame + 1; gpumem::pages[kind].push_back(uint32_t(p)); }
}
static void GpuMemFrameEnd()
{
    if (!GpuMemOn()) return;
    std::lock_guard l(gpumem::m);
    for (int k = 0; k < gpumem::kKinds; ++k)
    {
        for (uint32_t p : gpumem::pages[k])
        {
            ++gpumem::totalPages[k];
            if (RollbackPageRewound(p)) { ++gpumem::rewoundPages[k]; ++gpumem::rewoundRuns[k][p >> 8]; }
        }
        gpumem::lastPages[k].swap(gpumem::pages[k]);
        gpumem::pages[k].clear();
    }
    ++gpumem::frame;
    if (++gpumem::frames % 600 != 0) return;
    for (int k = 0; k < gpumem::kKinds; ++k)
    {
        char line[700];
        int n = std::snprintf(line, sizeof(line), "[gpu-mem] %s: %.1f pages a frame, %.1f of them rewound; rewound in 1 MB blocks:", gpumem::kNames[k],
                              double(gpumem::totalPages[k]) / 600.0, double(gpumem::rewoundPages[k]) / 600.0);
        int shown = 0;
        for (const auto& [blk, c] : gpumem::rewoundRuns[k])
            if (shown++ < 12 && n < 600) n += std::snprintf(line + n, sizeof(line) - n, " %08X(%u)", blk << 20, c);
        std::fprintf(stderr, "%s\n", line);
        gpumem::totalPages[k] = gpumem::rewoundPages[k] = 0;
        gpumem::rewoundRuns[k].clear();
    }
}

// rollback.cpp's session rewind: after the drain (the frame that was in flight is the last finished one)
// and after the re-simulation - which of that frame's pages changed in between, by kind; a summary every
// 50 rewinds with the 1 MB blocks the changed pages lie in.
void GpuMemRewindBegin()
{
    if (!GpuMemOn()) return;
    std::lock_guard l(gpumem::m);
    for (int k = 0; k < gpumem::kKinds; ++k)
    {
        gpumem::rewindHashes[k].clear();
        for (uint32_t p : gpumem::lastPages[k]) gpumem::rewindHashes[k].emplace_back(p, gpumem::HashPage(p));
    }
}
// stage 0: after the restore; 1: after the first re-simulated update (before its draw).
void GpuMemRewindCheck(int stage)
{
    if (!GpuMemOn() || stage < 0 || stage > 1) return;
    std::lock_guard l(gpumem::m);
    for (int k = 0; k < gpumem::kKinds; ++k)
        for (const auto& [p, h] : gpumem::rewindHashes[k])
            if (gpumem::HashPage(p) != h) ++gpumem::changedAt[stage][k];
}
void GpuMemRewindEnd()
{
    if (!GpuMemOn()) return;
    std::lock_guard l(gpumem::m);
    ++gpumem::rewinds;
    for (int k = 0; k < gpumem::kKinds; ++k)
        for (const auto& [p, h] : gpumem::rewindHashes[k])
        {
            ++gpumem::rewindPages[k];
            if (gpumem::HashPage(p) != h)
            {
                ++gpumem::rewindChanged[k]; ++gpumem::changedRuns[k][p >> 8];
                if (k == 1 && (p < 0xA0000 || p >= 0xA2200) && gpumem::changedOutsideRing.size() < 16) gpumem::changedOutsideRing.insert(p);
                extern int ResimScratchPageAge(uint32_t page);
                if (const int age = ResimScratchPageAge(p); age >= 0) ++gpumem::scratchAges[age];
            }
        }
    if (gpumem::rewinds % 50 != 0) return;
    for (int k = 0; k < gpumem::kKinds; ++k)
    {
        char line[700];
        int n = std::snprintf(line, sizeof(line), "[gpu-mem] rewinds: %s of the frame in flight: %.1f pages, %.1f changed by the re-simulation; in 1 MB blocks:",
                              gpumem::kNames[k], double(gpumem::rewindPages[k]) / 50.0, double(gpumem::rewindChanged[k]) / 50.0);
        int shown = 0;
        for (const auto& [blk, c] : gpumem::changedRuns[k])
            if (shown++ < 12 && n < 600) n += std::snprintf(line + n, sizeof(line) - n, " %08X(%u)", blk << 20, c);
        std::fprintf(stderr, "%s (after the restore %.1f, after the first update %.1f)\n", line,
                     double(gpumem::changedAt[0][k]) / 50.0, double(gpumem::changedAt[1][k]) / 50.0);
        gpumem::rewindPages[k] = gpumem::rewindChanged[k] = 0;
        gpumem::changedAt[0][k] = gpumem::changedAt[1][k] = 0;
        gpumem::changedRuns[k].clear();
    }
    std::string pages;
    char b[16];
    for (uint32_t p : gpumem::changedOutsideRing) { std::snprintf(b, sizeof(b), " %08X", p << 12); pages += b; }
    std::fprintf(stderr, "[gpu-mem] rewinds: vertex pages changed outside the skinned ring:%s\n", pages.c_str());
    std::string ages;
    for (const auto& [age, n] : gpumem::scratchAges) { std::snprintf(b, sizeof(b), " %d:%u", age, n); ages += b; }
    if (!ages.empty()) std::fprintf(stderr, "[gpu-mem] rewinds: changed scratch pages by age in rewinds (age:pages):%s\n", ages.c_str());
}

static void ExecPacketStream(uint32_t addr, uint32_t sizeBytes, int depth)
{
    if (depth > 4 || !addr) return;
    if (depth > 0) GpuNoteMemory(0, addr, sizeBytes);
    struct IbMark
    {
        bool on; uint32_t v;
        IbMark(bool o, uint32_t size) : on(o), v(size) { if (on) timeline::Mark(timeline::CpIb, '+', v); }
        ~IbMark() { if (on) timeline::Mark(timeline::CpIb, '-', v); }
    } ibMark{ depth > 0, sizeBytes };
    if (uint64_t(addr) + sizeBytes > UINT32_MAX || ((addr | sizeBytes) & 3))
    {
        std::fprintf(stderr, "[pm4-fault] invalid stream range %08X+%X d=%d\n",
                     addr, sizeBytes, depth);
        std::fflush(stderr);
        LogDrain(); host_process::ExitNow(6);
        std::abort();
    }
    uint32_t start = addr;
    uint32_t end = addr + sizeBytes;
    while (uint64_t(addr) + 4 <= end)
    {
        uint32_t h = GRead32(g_base, addr); addr += 4;
        uint32_t type = h >> 30;
        uint32_t payload = GpuPacketBytes(h) / 4 - 1;
        CheckPacketBoundary(addr - 4, h, start, sizeBytes, payload, depth);
        if (h == 0) continue;                                    // zero padding between packets
        if (h == 0x54535750u)                                    // 'TSWP': our VdSwap marker
        {
            const CpOpTimer swapTimer{ 128 };
            CpProfileReport();
            uint32_t identVal  = GRead32(g_base, addr);
            uint32_t identAddr = GRead32(g_base, addr + 4);
            uint32_t fbtex     = GRead32(g_base, addr + 8);
            uint32_t fbSurf    = GRead32(g_base, addr + 12);
            addr += 16;
            // Swap-packet side effects: report the identifier, then enqueue the flip in the
            // device's swap queue (16 x {fbAddr, dueTick} at dev+16980) - the game's own
            // 60Hz swap DPC (sub_827404E0) drains it and performs the flip bookkeeping.
            if (identAddr) { GWrite32(g_base, identAddr, identVal); GpuNoteMemory(4, identAddr, 4); }
            GpuMemFrameEnd();
            // The flip-queue enqueue that used to live here is GONE, and stays gone: with
            // the VdGlobalDevice import slots fixed, the game's own ISR swap callback
            // (sub_82740610) enqueues the flip and the SCRATCH_REG1 vblank-fence entries
            // itself, under its own spinlock, with its own pacing. Our synthetic entries
            // double-enqueued every flip and cleared the throttle flag at the wrong times.
            // Only the identifier write above and the present-surface latch below remain.
            // The surface was decoded in VdSwap (see there); a zero here just means this
            // flip carried no usable fetch block, so keep showing the previous surface.
            const bool discarded = CpDiscarding();
            if (fbSurf && !discarded)
            {
                g_frontBufferPhys.store(fbSurf & ~0xFFFu);
                g_frontBufferSwizzle.store(fbSurf & 0xFFFu);
            }
            ++g_cpMarkers;
            CpRetireAfterUnpaced();
            const bool early = g_earlySwap && g_earlySwap == g_cpMarkers && !g_earlyMismatch;
            g_earlyMismatch = false;
            if (early && !g_earlyBroken && g_markerFb[g_cpMarkers & 15].load() != fbSurf)
            {
                g_earlyBroken = true;                            // numbering lost: present at markers
                std::fprintf(stderr, "[vd] early present off: marker %u surface %08X, recorded %08X\n",
                             g_cpMarkers, fbSurf, g_markerFb[g_cpMarkers & 15].load());
            }
            if (GpuHwMode() && !early && !discarded)
            {
                frame_pacing_probe::Ready(g_cpMarkers);
                HwPresentFrame(g_frontBufferPhys.load());
            }
            FrameLogSwap();
            PhysMirrorInvalidateAll();                           // frame boundary
            frame_pacing_probe::TraceFrame(g_cpMarkers, frame_pacing_probe::lastTickUs.load());
            static int slog = 0;
            if (slog < 8)
            {
                ++slog;
                std::fprintf(stderr, "[vd] swap-exec ident[%08X]=%08X fb=%08X\n",
                             identAddr, identVal, fbtex);
            }
            continue;
        }
        if (type == 3)
        {
            uint32_t cnt = ((h >> 16) & 0x3FFF) + 1;
            uint32_t op = (h >> 8) & 0x7F;
            if (op < 128) ++g_opcodeSeen[op];
            const CpOpTimer opTimer{ (op == 0x3F || op == 0x37) ? -1 : int(op) };
            if (g_pm4Trace > 0)
            {
                --g_pm4Trace;
                char line[300]; int n = 0;
                n += BoundedSnprintf(line + n, sizeof(line) - n,
                                   "[pm4] d%d @%08X h=%08X T3 op=%02X cnt=%-4u :", depth,
                                   addr - 4, h, op, cnt);
                for (uint32_t i = 0; i < cnt && i < 10; ++i)
                    n += BoundedSnprintf(line + n, sizeof(line) - n, " %08X",
                                       GRead32(g_base, addr + i * 4));
                if (cnt > 10) n += BoundedSnprintf(line + n, sizeof(line) - n, " ...");
                BoundedSnprintf(line + n, sizeof(line) - n, "\n");
                std::fputs(line, stderr);
            }
            // Draw-state inventory (GPU phase bring-up): dump draw-class packets with the
            // register state they depend on, so the host renderer is built from measured
            // data, not guesses. First 48 draws only.
            if (op == 0x2B && cnt >= 3)                          // IM_LOAD_IMMEDIATE (shader ucode)
            {
                // dword0 = shader type (0 = vertex, 1 = pixel), dword1 = start(31:16) |
                // size-in-dwords(15:0), then the ucode itself.
                uint32_t stype = GRead32(g_base, addr);
                uint32_t startSize = GRead32(g_base, addr + 4);
                uint32_t szDw = startSize & 0xFFFF;
                if (!szDw || szDw > cnt - 2) szDw = cnt - 2;
                const uint32_t program = LatchImmediateShader(stype, addr + 8, szDw);
                if (stype == 0) { g_curVs = program; g_curVsSize = szDw; }
                else            { g_curPs = program; g_curPsSize = szDw; }
                DumpShaderBlob(stype, program, szDw);
                static int imlog = 0;
                if (imlog < 24)
                {
                    ++imlog;
                    std::fprintf(stderr, "[gpu] IM_LOAD_IMM type=%u startSize=%08X cnt=%u -> %u dw @%08X\n",
                                 stype, startSize, cnt, szDw, addr + 8);
                }
            }
            else if (op == 0x27 && cnt >= 2)                     // IM_LOAD (ucode from memory)
            {
                uint32_t d0 = GRead32(g_base, addr), d1 = GRead32(g_base, addr + 4);
                uint32_t stype = d0 & 3, src = PhysToGuest(d0 & ~3u), szDw = d1 & 0xFFFF;
                if (stype == 0) { g_curVs = src; g_curVsSize = szDw; }
                else            { g_curPs = src; g_curPsSize = szDw; }
                static int imlog2 = 0;
                if (imlog2 < 24)
                {
                    ++imlog2;
                    std::fprintf(stderr, "[gpu] IM_LOAD type=%u src=%08X startSize=%08X -> %u dw\n",
                                 stype, src, d1, szDw);
                }
                if (szDw && szDw < 4096) DumpShaderBlob(stype, src, szDw);
            }
            else if (op == 0x46 && cnt >= 1)                     // EVENT_WRITE: latch event id
            {
                g_regFile[0x21F9] = GRead32(g_base, addr) & 0x3F;
            }
            else if ((op == 0x2D || op == 0x55 || op == 0x56) && cnt >= 1)
            {
                // SET_CONSTANT (0x2D): dword0 = index(10:0) | bank(23:16); the bank selects
                // the register-file base (0 ALU 0x4000, 1 FETCH 0x4800, 2 BOOL 0x4900,
                // 3 LOOP 0x4908, 4 context registers 0x2000).
                // SET_CONSTANT2 (0x55) / SET_SHADER_CONSTANTS (0x56): dword0 is already an
                // absolute register-file index.
                uint32_t d0 = GRead32(g_base, addr), reg;
                if (op == 0x2D)
                {
                    uint32_t bank = (d0 >> 16) & 0xFF, idx = d0 & 0x7FF;
                    static const uint32_t base[5] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                    reg = (bank < 5 ? base[bank] : 0x2000) + idx;
                }
                else reg = d0 & 0xFFFF;
                WriteRegisterRange(reg, addr + 4, cnt - 1);
                static int sclog = 0;
                if (sclog < 40)
                {
                    ++sclog;
                    std::fprintf(stderr, "[gpu] SET_CONST op=%02X d0=%08X -> reg %04X x%u\n",
                                 op, d0, reg, cnt - 1);
                }
            }
            else if (op == 0x2F && cnt >= 3)                     // LOAD_ALU_CONSTANT (from memory)
            {
                uint32_t src = PhysToGuest(GRead32(g_base, addr) & 0x3FFFFFFCu);
                uint32_t ot  = GRead32(g_base, addr + 4);
                uint32_t n   = GRead32(g_base, addr + 8) & 0xFFF;
                uint32_t bank = (ot >> 16) & 0xFF, idx = ot & 0x7FF;
                GpuNoteMemory(3, src, n * 4);
                static const uint32_t base[5] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                uint32_t reg = (bank < 5 ? base[bank] : 0x2000) + idx;
                WriteRegisterRange(reg, src, n);
                static int lalog = 0;
                if (lalog < 20)
                {
                    ++lalog;
                    std::fprintf(stderr, "[gpu] LOAD_ALU_CONST src=%08X ot=%08X n=%u -> reg %04X\n",
                                 src, ot, n, reg);
                }
            }
            else if (op == 0x45 && cnt >= 6)                     // COND_WRITE
            {
                // Log-only for now (enabling the write is TTT2_CONDWRITE=1): the stream emits
                // 256 of these and we must know what they target before letting them fire.
                uint32_t info = GRead32(g_base, addr);
                uint32_t poll = GRead32(g_base, addr + 4), ref = GRead32(g_base, addr + 8);
                uint32_t mask = GRead32(g_base, addr + 12);
                uint32_t wa   = GRead32(g_base, addr + 16), wd = GRead32(g_base, addr + 20);
                static int cwlog = 0;
                if (cwlog < 24)
                {
                    ++cwlog;
                    std::fprintf(stderr,
                        "[gpu] COND_WRITE info=%08X poll=%08X ref=%08X mask=%08X wr[%08X]=%08X\n",
                        info, poll, ref, mask, wa, wd);
                }
                static const bool doWrite = std::getenv("TTT2_CONDWRITE") != nullptr;
                if (doWrite)
                {
                    bool memPoll = (info & 0x10) != 0, memWrite = (info & 0x100) != 0;
                    uint32_t v = memPoll ? GRead32(g_base, PhysToGuest(poll & ~3u))
                                         : (poll < 0x8000 ? g_regFile[poll] : 0);
                    v &= mask;
                    bool ok;
                    switch (info & 7)
                    {
                    case 1: ok = v <  ref; break;   case 2: ok = v <= ref; break;
                    case 3: ok = v == ref; break;   case 4: ok = v != ref; break;
                    case 5: ok = v >= ref; break;   case 6: ok = v >  ref; break;
                    default: ok = (info & 7) != 0; break;
                    }
                    if (ok)
                    {
                        if (memWrite) GWrite32(g_base, PhysToGuest(wa & ~3u), wd);
                        else WriteReg(wa & 0x7FFF, wd);
                        GpuProgress();
                    }
                }
            }
            if (op == 0x22 || op == 0x36)                        // draws: dispatch on edram mode
            {
                // DRAW_INDX carries the initiator in dword1 and, when the index source is DMA,
                // the index buffer address in dword2; DRAW_INDX_2 has the initiator in dword0
                // and immediate indices inline right after it.
                uint32_t init = GRead32(g_base, addr + (op == 0x22 ? 4 : 0));
                uint32_t srcSel = (init >> 6) & 3;
                bool index32 = ((init >> 8) & 1) != 0;
                uint32_t indexAddr = 0;
                // The index base (VGT_DMA_BASE) is aligned to the index size only: TTT2 packs
                // many 16-bit strips back to back, and half of them start on a 2-byte boundary.
                // Rounding those down to 4 bytes read the previous strip's last index as the
                // first vertex - one triangle per mesh stretched into a neighbouring sub-mesh,
                // the long "streak lines" across characters (measured 2026-10-11: strips at
                // AF1D59B6 / AF1D5BDA / AE21847A were read from ...B4 / ...D8 / ...78).
                if (srcSel == 0 && op == 0x22 && cnt >= 4)
                    indexAddr = PhysToGuest(GRead32(g_base, addr + 8) & (index32 ? ~3u : ~1u));
                else if (srcSel == 1)
                    indexAddr = addr + (op == 0x22 ? 8 : 4);
                // Dump the CONTENT draws: skip the 1-vertex point-list warm-up dummies and
                // the rectangle-list clears/resolves, which arrive first and would otherwise
                // eat the whole dump budget before any real geometry shows up.
                uint32_t dprim = init & 0x3F;
                ++g_primSeen[dprim];
                if (dprim != 1 && dprim != 8 && dprim != 4) DumpDrawState(init, op);
                if (dprim != 1 && dprim != 8 && dprim != 4 && CwOn())
                {
                    --g_cwBudget;
                    char line[400]; int n2 = 0;
                    n2 += BoundedSnprintf(line + n2, sizeof(line) - n2,
                                        "[cw] seq=%u DRAW prim=%u vs=%08X ps=%08X rows:",
                                        g_regSeq, dprim, g_curVs, g_curPs);
                    for (uint32_t row = 126; row < 136; ++row)
                    {
                        const uint32_t* c = &g_regFile[0x4000 + row * 4];
                        n2 += BoundedSnprintf(line + n2, sizeof(line) - n2, " c%u=%08X/%08X",
                                            row, c[0], c[1]);
                    }
                    BoundedSnprintf(line + n2, sizeof(line) - n2, "\n");
                    std::fputs(line, stderr);
                }
                FrameLogDraw(init);
                if (!CpDiscarding())
                {
                    GpuExecuteDraw(g_regFile, init, indexAddr, index32,
                                   GpuShader{ g_curVs, g_curVsSize },
                                   GpuShader{ g_curPs, g_curPsSize });
                    if ((g_regFile[0x2208] & 7u) == 6u) IssueResolve();
                }
            }
            if (op == 0x22 || op == 0x36 || op == 0x34)          // DRAW_INDX / DRAW_INDX_2 / VIZQUERY?
            {
                static int dlog = 0;
                if (dlog < 48)
                {
                    ++dlog;
                    char line[640]; int n = 0;
                    n += BoundedSnprintf(line + n, sizeof(line) - n, "[draw] op=%02X cnt=%u pay:", op, cnt);
                    for (uint32_t i = 0; i < cnt && i < 6; ++i)
                        n += BoundedSnprintf(line + n, sizeof(line) - n, " %08X", GRead32(g_base, addr + i * 4));
                    // key state: RB_SURFACE/COLOR/DEPTH_INFO (2000-2002), COPY ctl (2318-231C),
                    // SQ_PROGRAM_CNTL (2180), PGM starts (2100/2104? per Xenos docs), viewport.
                    static const uint16_t regs[] = { 0x2000, 0x2001, 0x2002, 0x2318, 0x2319,
                                                     0x231A, 0x231B, 0x231C, 0x2180, 0x2100,
                                                     0x2101, 0x210C, 0x2104, 0x2280, 0x2281 };
                    n += BoundedSnprintf(line + n, sizeof(line) - n, " regs:");
                    for (uint16_t rg : regs)
                        n += BoundedSnprintf(line + n, sizeof(line) - n, " %X=%08X", rg, g_regFile[rg]);
                    BoundedSnprintf(line + n, sizeof(line) - n, "\n");
                    std::fputs(line, stderr);
                    if (dlog == 20)
                    {
                        // One-time full register inventory: every nonzero reg the stream wrote.
                        std::fprintf(stderr, "[draw] ==== register inventory ====\n");
                        for (uint32_t rg = 0; rg < 0x6000; ++rg)
                            if (g_regFile[rg])
                                std::fprintf(stderr, "[draw] reg %04X = %08X\n", rg, g_regFile[rg]);
                        std::fprintf(stderr, "[draw] ==== end inventory ====\n");
                    }
                }
            }
            if ((op == 0x3F || op == 0x37) && cnt >= 2)          // INDIRECT_BUFFER / _PFD
            {
                uint32_t ib = PhysToGuest(GRead32(g_base, addr));
                uint32_t n = GRead32(g_base, addr + 4) & 0xFFFFF;
                ExecPacketStream(ib, n * 4, depth + 1);
            }
            else if (op == 0x58 && cnt >= 3)                     // EVENT_WRITE_SHD (fence)
            {
                uint32_t dst = PhysToGuest(GRead32(g_base, addr + 4) & ~3u);
                uint32_t val = GRead32(g_base, addr + 8);
                if (dst) { GWrite32(g_base, dst, val); GpuNoteMemory(4, dst, 4); }
                GpuProgress();
                static int flog = 0;
                if (flog < 24)
                {
                    ++flog;
                    std::fprintf(stderr, "[vd] fence: [%08X]=%08X\n", dst, val);
                }
            }
            else if (op == 0x3D && cnt >= 2)                     // MEM_WRITE
            {
                uint32_t dst = PhysToGuest(GRead32(g_base, addr) & ~3u);
                if (dst) GpuNoteMemory(4, dst, (cnt - 1) * 4);
                for (uint32_t i = 1; i < cnt && dst; ++i)
                    GWrite32(g_base, dst + (i - 1) * 4, GRead32(g_base, addr + i * 4));
                GpuProgress();
            }
            else if (op == 0x3C && cnt >= 5)                     // WAIT_REG_MEM
            {
                TryEarlyPresent(GRead32(g_base, addr), GRead32(g_base, addr + 4));
                if (WaitRegMem(GRead32(g_base, addr), GRead32(g_base, addr + 4),
                               GRead32(g_base, addr + 8), GRead32(g_base, addr + 12)))
            {
                // A timed-out wait is the open SCRATCH_REG1 puzzle: dump the stream around
                // the packet so the arming sequence and what follows are visible. addr points
                // at this packet's payload; the window behind it holds whatever armed the
                // register, the window ahead holds what the game expects to run after.
                static int sdump = 4;
                if (sdump-- > 0)
                {
                    std::fprintf(stderr, "[wrm] STREAM around timed-out wait @%08X:\n", addr - 4);
                    for (int32_t off = -96; off < 64; off += 16)
                    {
                        char l2[160]; int n4 = 0;
                        n4 += BoundedSnprintf(l2 + n4, sizeof(l2) - n4, "[wrm]   %+05d:", off);
                        for (int k2 = 0; k2 < 4; ++k2)
                            n4 += BoundedSnprintf(l2 + n4, sizeof(l2) - n4, " %08X",
                                                GRead32(g_base, addr + off + k2 * 4));
                        BoundedSnprintf(l2 + n4, sizeof(l2) - n4, "\n");
                        std::fputs(l2, stderr);
                    }
                }
                // A failed fence is not permission to execute commands after it.
                // Continuing previously called the intentionally disarmed callback.
                std::fputs("[pm4-fault] fence wait failed; stopping command execution\n", stderr);
                std::fflush(stderr);
                LogDrain(); host_process::ExitNow(6);
                std::abort();
            }
            }
            else if (op == 0x54 && cnt >= 1)                     // CP_INTERRUPT
            {
                uint32_t mask9 = GRead32(g_base, addr);
                static int ilog = 0;
                if (ilog < 12)
                {
                    ++ilog;
                    std::fprintf(stderr, "[vd] CP_INTERRUPT mask=%08X\n", mask9);
                }
                RaiseCpInterrupt(mask9);
            }
            else if (op != 0x10 && op != 0x3F && op != 0x37 && op != 0x3C && op != 0x3D &&
                     op != 0x58 && op != 0x3B && op != 0x48 && op != 0x21 && op != 0x22 &&
                     op != 0x36 && op != 0x34)
            {
                static uint8_t seen[128];
                if (op < 128 && seen[op] < 4)
                {
                    ++seen[op];
                    char line[300]; int n3 = 0;
                    n3 += BoundedSnprintf(line + n3, sizeof(line) - n3,
                                        "[unk] T3 op=%02X cnt=%u :", op, cnt);
                    for (uint32_t i = 0; i < cnt && i < 12; ++i)
                        n3 += BoundedSnprintf(line + n3, sizeof(line) - n3, " %08X",
                                            GRead32(g_base, addr + i * 4));
                    BoundedSnprintf(line + n3, sizeof(line) - n3, "\n");
                    std::fputs(line, stderr);
                }
            }
            addr += cnt * 4;
        }
        else if (type == 0)                                      // register writes
        {
            uint32_t cnt = ((h >> 16) & 0x3FFF) + 1;
            uint32_t reg = h & 0x7FFF;
            bool oneReg = (h & 0x8000) != 0;
            if (g_pm4Trace > 0)
            {
                --g_pm4Trace;
                char line[300]; int n = 0;
                n += BoundedSnprintf(line + n, sizeof(line) - n,
                                   "[pm4] d%d @%08X h=%08X T0 reg=%04X cnt=%-4u one=%d :",
                                   depth, addr - 4, h, reg, cnt, (int)oneReg);
                for (uint32_t i = 0; i < cnt && i < 10; ++i)
                    n += BoundedSnprintf(line + n, sizeof(line) - n, " %08X",
                                       GRead32(g_base, addr + i * 4));
                if (cnt > 10) n += BoundedSnprintf(line + n, sizeof(line) - n, " ...");
                BoundedSnprintf(line + n, sizeof(line) - n, "\n");
                std::fputs(line, stderr);
            }
            // The 2D transform block c128..c133 arrives by type-0 write and lands half-zero.
            // Log WHERE in guest memory the c128 payload sits (the D3D runtime memcpy'd it
            // there from the game's staging buffer) so a TTT2_WATCH on that spot can name the
            // writer - the next hop toward the code that builds the defective matrix.
            if (CwOn() && !oneReg && reg <= 0x4200 && reg + cnt > 0x4200)
            {
                --g_cwBudget;
                std::fprintf(stderr, "[cw] c128 payload at guest %08X (T0 reg=%04X cnt=%u)\n",
                             addr + (0x4200 - reg) * 4, reg, cnt);
            }
            if (!oneReg) WriteRegisterRange(reg, addr, cnt);
            else for (uint32_t i = 0; i < cnt; ++i)
                WriteReg(reg, GRead32(g_base, addr + i * 4));
            addr += cnt * 4;
        }
        else if (type == 1)                                      // two independent registers
        {
            WriteReg(h & 0x7FF, GRead32(g_base, addr));
            WriteReg((h >> 11) & 0x7FF, GRead32(g_base, addr + 4));
            addr += 8;
        }
        // type 2: no payload
    }
}

static void ConsumeRing()
{
    uint32_t rb = g_ringBase, rs = g_ringSize;
    if (!rb || !rs) return;
    // Consume only what the game PUBLISHED via the CP_RB_WPTR doorbell (MMIO 0x7FC80714,
    // dword index; mirrored at device+11088) - the CPU cursor at device+48 races the writes.
    uint32_t doorbell = GRead32(g_base, 0x7FC80714u);
    uint32_t wptrOff;
    if ((uint64_t)doorbell * 4 < rs) wptrOff = doorbell * 4;   // dword index, including zero
    else if (doorbell >= rb && uint64_t(doorbell) < uint64_t(rb) + rs) wptrOff = doorbell - rb;
    else return;
    if (wptrOff == g_ringRptr.load()) return;
    const auto result = WalkGpuRing(rs, g_ringRptr.load(), wptrOff,
        [rb](uint32_t offset) { return GRead32(g_base, rb + offset); },
        [rb](const GpuRingPacket& packet)
        {
            if (!packet.Wraps())
            {
                ExecPacketStream(rb + packet.offset, packet.size, 0);
                return;
            }
            // Only the CP thread uses this stable staging area. A wrapped primary
            // packet (normally an IB reference) must be contiguous for the decoder.
            static uint32_t staging = 0;
            if (!staging)
            {
                void* allocation = g_virtualHeap.Alloc(GpuMaxPacketBytes, 16);
                if (!allocation)
                {
                    std::fputs("[pm4-fault] cannot allocate wrapped-packet staging\n", stderr);
                    std::fflush(stderr);
                    LogDrain(); host_process::ExitNow(6);
                    std::abort();
                }
                staging = GuestAddr(g_base, allocation);
            }
            std::memcpy(g_base + staging, g_base + rb + packet.offset, packet.firstBytes);
            std::memcpy(g_base + staging + packet.firstBytes, g_base + rb,
                        packet.size - packet.firstBytes);
            ExecPacketStream(staging, packet.size, 0);
        });
    if (result.status != GpuRingStatus::Complete)
    {
        std::fprintf(stderr,
            "[pm4-fault] invalid published ring packet @%08X h=%08X required=%u available=%u ring=%08X+%X rptr=%X wptr=%X\n",
            rb + result.offset, result.header, result.required, result.remaining,
            rb, rs, g_ringRptr.load(), wptrOff);
        std::fflush(stderr);
        LogDrain(); host_process::ExitNow(6);
        std::abort();
    }
    g_ringRptr.store(result.offset);
    // Rptr writeback: real hardware reports a LAGGING, block-quantized read pointer
    // (VdEnableRingBufferRPtrWriteBack registers block-size log2 = 6 for this ring, so
    // the value only advances in 0x40-dword steps). Publishing the exact consumption
    // point can land inside a tail-rewrite window (wptr, wptr+n] and wedge the
    // ring-space wait (sub_82738830) forever - hardware never exposes that state.
    if (uint32_t wb = g_rptrWriteBack.load())
        GWrite32(g_base, wb, (result.offset / 4) & ~0x3Fu);
    GpuProgress();
}

// rollback.cpp: the command processor has consumed everything the game published - no frame in
// flight still reads guest memory.
uint32_t GpuRingPendingBytes()
{
    const uint32_t rb = g_ringBase, rs = g_ringSize;
    if (!rb || !rs) return 0;
    const uint32_t doorbell = GRead32(g_base, 0x7FC80714u);
    uint32_t wptrOff;
    if ((uint64_t)doorbell * 4 < rs) wptrOff = doorbell * 4;
    else if (doorbell >= rb && uint64_t(doorbell) < uint64_t(rb) + rs) wptrOff = doorbell - rb;
    else return 0;
    const uint32_t r = g_ringRptr.load();
    return wptrOff >= r ? wptrOff - r : rs - r + wptrOff;
}
bool GpuRingDrained()
{
    const uint32_t rb = g_ringBase, rs = g_ringSize;
    if (!rb || !rs) return true;
    const uint32_t doorbell = GRead32(g_base, 0x7FC80714u);
    uint32_t wptrOff;
    if ((uint64_t)doorbell * 4 < rs) wptrOff = doorbell * 4;
    else if (doorbell >= rb && uint64_t(doorbell) < uint64_t(rb) + rs) wptrOff = doorbell - rb;
    else return true;
    return wptrOff == g_ringRptr.load();
}

// The CP thread: consume as soon as work is published. While frames are flowing the game kicks
// every few milliseconds, so after consuming anything stay awake (yield) for 4 ms: a new kick
// then starts within microseconds instead of after a 1 ms sleep (measured 2026-10-11: with the
// vertex stage on the GPU the main thread spent ~45% spinning on CP progress, much of it this
// latency). Fully idle, fall back to the 1 ms cadence.
static void CommandProcessorThread()
{
    InitGpuTrace();
    HitchRegisterThread("cp");
    auto lastWork = std::chrono::steady_clock::now();
    auto loopEnd = lastWork, report = lastWork;
    for (;;)
    {
        const uint32_t before = g_ringRptr.load();
        const auto ringStart = std::chrono::steady_clock::now();
        const uint64_t ringStartUs = timeline::On() ? timeline::NowUs() : 0;
        ConsumeRing();
        const auto now = std::chrono::steady_clock::now();
        if (g_ringRptr.load() != before)
        {
            lastWork = now;
            if (timeline::On())
            {
                auto& ring = timeline::Ring();
                ring[timeline::Next().fetch_add(1) & (timeline::kEntries - 1)] =
                    timeline::Entry{ ringStartUs, before, timeline::ThreadTag(), uint8_t(timeline::CpRing), uint8_t('+') };
                timeline::Mark(timeline::CpRing, '-', g_ringRptr.load());
            }
        }
        if (now - lastWork < std::chrono::milliseconds(4)) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (FrameTimeLog())
        {
            using ms = std::chrono::duration<double, std::milli>;
            const auto end = std::chrono::steady_clock::now();
            g_cpTimes.ringMs += ms(now - ringStart).count();
            g_cpTimes.idleMs += ms(ringStart - loopEnd).count() + ms(end - now).count();
            loopEnd = end;
            if (end - report >= std::chrono::seconds(10))
            {
                const CpTimes& t = g_cpTimes;
                std::fprintf(stderr, "[frame] cp 10s: executing %.0f ms, scratch waits %.0f ms (%llu), memory waits %.0f ms (%llu), register waits %.0f ms (%llu), idle %.0f ms\n",
                             t.ringMs - t.waitMs[0] - t.waitMs[1] - t.waitMs[2], t.waitMs[0], (unsigned long long)t.waits[0],
                             t.waitMs[1], (unsigned long long)t.waits[1], t.waitMs[2], (unsigned long long)t.waits[2], t.idleMs);
                std::vector<std::pair<double, uint32_t>> top;
                for (const auto& [address, w] : t.memoryWaits) top.push_back({ w.second, address });
                std::sort(top.rbegin(), top.rend());
                char line[512];
                int n = BoundedSnprintf(line, sizeof(line), "[frame] cp memory waits by address:");
                for (size_t i = 0; i < top.size() && i < 6; ++i)
                    n += BoundedSnprintf(line + n, sizeof(line) - n, " %08X=%.0fms/%llu", top[i].second, top[i].first,
                                         (unsigned long long)t.memoryWaits.at(top[i].second).first);
                std::fprintf(stderr, "%s\n", line);
                g_cpTimes = {};
                report = end;
            }
        }
    }
}

// 60 Hz "GPU": complete fences, report ring progress, raise the graphics interrupt.
// Find every copy of the game's ortho projection matrix in guest memory. Fingerprint:
// row0.x = 0x3ACCCCCD (2/1280) and row2.z = 0xB8B67793. The 2D compose uploads rows 128..135
// with row1 (y scale) and row3 (offsets) ZEROED while row0/row2 arrive intact - so somewhere
// there is a complete source matrix and a half-copied shadow. This sweep prints both, so the
// next run can put a write-watch on the shadow's zero row and name the code that builds it.
static void ScanOrthoPattern(uint32_t lo, uint32_t hi, const char* tag)
{
    static const uint8_t pat[4] = { 0x3A, 0xCC, 0xCC, 0xCD };
    uint32_t a = lo;
    int found = 0;
    while (a + 64 <= hi && found < 40)
    {
        // Guest space is committed as a whole, but pages the runtime protected (write-watch,
        // null guard, stack guards) fault on a raw read - and this runs on the vblank thread
        // with no SEH net (measured: the first version killed the thread at exactly the scan
        // tick). Walk regions and only touch plainly readable ones.
        size_t regionBytes = 0;
        bool readable = false;
        if (!host_vm::QueryReadRegion(g_base + a, hi - a, regionBytes, readable)) break;
        uint32_t regEnd = a + static_cast<uint32_t>(regionBytes);
        if (!readable || regEnd <= a + 64) { a = regEnd ? regEnd : hi; continue; }
        const void* f = std::memchr(g_base + a, 0x3A, (size_t)(regEnd - a - 60));
        if (!f) break;
        uint32_t addr = (uint32_t)((const uint8_t*)f - g_base);
        a = addr + 1;
        if (addr & 3) continue;
        if (std::memcmp(g_base + addr, pat, 4) != 0) continue;
        if (GRead32(g_base, addr + 40) != 0xB8B67793u) continue;
        uint32_t r1 = GRead32(g_base, addr + 16) | GRead32(g_base, addr + 20) |
                      GRead32(g_base, addr + 24) | GRead32(g_base, addr + 28);
        ++found;
        std::fprintf(stderr, "[scan] %s ortho@%08X row1=%s row1.y=%08X row3.x=%08X row3.w=%08X\n",
                     tag, addr, r1 ? "OK" : "ZERO", GRead32(g_base, addr + 20),
                     GRead32(g_base, addr + 48), GRead32(g_base, addr + 60));
    }
}

// Rollback sessions' time sync (rollback_session.cpp): microseconds added to the next vblank periods
// while this instance runs ahead of the other one - the game's clock itself goes slower, so the frame
// loop cannot catch the time up (it does after a late frame).
std::atomic<int32_t> g_vblankStretchUs{ 0 };
void VideoSetVblankStretch(int32_t us) { g_vblankStretchUs.store(us < 0 ? 0 : us); }

static void VideoTick()
{
    using clock = std::chrono::steady_clock;
    // Development throughput probe: keep the normal 60 Hz clock unless a
    // caller explicitly requests a higher guest vblank rate. This accelerates
    // guest frame pacing; it does not provide normal-speed high-refresh play.
    int refreshHz = std::clamp(EnvInt("TTT2_VIDEO_HZ", 60), 1, 10000);
    auto period = std::chrono::microseconds((1000000 + refreshHz / 2) / refreshHz);
    auto spinMargin = (std::min)(std::chrono::microseconds(1500), period / 8);
    host_deadline::Waiter deadlineWaiter;
    if (refreshHz != 60)
        std::fprintf(stderr, "[vd] throughput probe: guest vblank %d Hz; simulation pacing may accelerate\n", refreshHz);
    const char* refreshFile = std::getenv("TTT2_VIDEO_HZ_FILE");
    auto next = clock::now(), lastRefreshPoll = next;
    for (;;)
    {
        // Optional live probe control lets menu navigation run at normal speed.
        // Poll by wall time so high refresh rates do not amplify file I/O.
        if (refreshFile && clock::now() - lastRefreshPoll >= std::chrono::seconds(1))
        {
            lastRefreshPoll = clock::now();
            int requested = refreshHz;
            if (FILE* f = std::fopen(refreshFile, "r"))
            {
                if (std::fscanf(f, "%d", &requested) != 1) requested = refreshHz;
                std::fclose(f);
            }
            if (requested >= 1 && requested <= 10000 && requested != refreshHz)
            {
                refreshHz = requested;
                period = std::chrono::microseconds((1000000 + refreshHz / 2) / refreshHz);
                spinMargin = (std::min)(std::chrono::microseconds(1500), period / 8);
                next = clock::now();
                std::fprintf(stderr, "[vd] throughput probe: guest vblank changed to %d Hz\n", refreshHz);
            }
        }
        // Keep a fixed vblank schedule: work and sleep overshoot must not extend
        // the frame period. Learn a conservative sleep margin, then yield until
        // the original deadline.
        next += period + std::chrono::microseconds(g_vblankStretchUs.load(std::memory_order_relaxed));
        const auto pacingDeadline = next;
        deadlineWaiter.Wait(next, spinMargin);
        const auto pacingWoke = clock::now();
        int pacingStalls = 0;
        bool pacingReanchored = false;
        if (FrameTimeLog())
        {
            static FrameTimer vblanks{ "vblank" };
            vblanks.Mark();
        }
        timeline::Mark(timeline::Vblank, '.');
        {
            static unsigned polls = 0;
            if (timeline::On() && ++polls % 30 == 0) timeline::PollDumpTrigger();
        }

        // SYNC-TO-GPU PACING. The vblank is the game's master clock: swap pacing, the DPC
        // tick (dev+16952), and every fence deadline derive from it. Our software renderer
        // cannot yet keep 60 fps, and letting the clock run free while the CP falls seconds
        // behind sent the game's CPU-side fence logic into its timeout paths - it re-armed
        // fences the CP had not reached yet, and the CP then burned 3 s on each stale wait
        // (measured: the run_s11h..s11i death spirals). So when the CP is far behind the
        // doorbell, STALL THE CLOCK: the whole game runs slow-motion but stays in sync, and
        // speeds up as the renderer does. Floor of ~6 Hz so nothing hard-deadlocks.
        {
            int stalls = 0;
            for (; stalls < 10; ++stalls)
            {
                uint32_t rb2 = g_ringBase.load(), rs2 = g_ringSize.load();
                if (!rb2 || !rs2) break;
                uint32_t doorbell = GRead32(g_base, 0x7FC80714u);
                uint32_t wptrOff;
                if ((uint64_t)doorbell * 4 < rs2) wptrOff = doorbell * 4;
                else if (doorbell >= rb2 && doorbell < rb2 + rs2) wptrOff = doorbell - rb2;
                else break;
                uint32_t lag = (wptrOff - g_ringRptr.load()) & (rs2 - 1);
                if (lag < 0x30000) break;              // < 192 KB of unconsumed ring: on pace
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            static std::atomic<uint64_t> stallTicks{ 0 };
            if (stalls) stallTicks += stalls;
            static uint64_t lastStallReport = 0;
            uint64_t st = stallTicks.load();
            if (st - lastStallReport >= 600)
            {
                lastStallReport = st;
                std::fprintf(stderr, "[vd] pacing: %llu vblank-stall slices so far\n",
                             (unsigned long long)st);
            }
            // Re-anchor only after a stall (or if this thread fell more than a frame behind):
            // otherwise keep the fixed schedule.
            pacingStalls = stalls;
            pacingReanchored = stalls || clock::now() - next > period;
            if (pacingReanchored) next = clock::now();
        }

        // Segment retirement and rptr writeback are handled by the CP thread executing
        // the game's own EVENT_WRITE_SHD tail packets - do not fake counters here.
        // Raise the interrupt: swap-complete (source 0, with pending bit set), then vblank.
        static uint64_t ticks = 0;
        ++ticks;
        // The old unconditional heap scans ran on the timing-critical vblank
        // thread and could delay the very fence acknowledgements the CP waits on.
        // Keep this expensive, read-only investigation opt-in and off that thread.
        static const bool orthoScan = std::getenv("TTT2_ORTHO_SCAN") != nullptr;
        if (orthoScan && (ticks == 1500 || ticks == 9000))
        {
            const char* tag = ticks == 1500 ? "t25" : "t150";
            std::thread([tag] {
                ScanOrthoPattern(0x00100000, 0x02000000, tag);
                ScanOrthoPattern(0xA0000000, 0xAA000000, tag);
                ScanOrthoPattern(0xB3C00000, 0xB4100000, tag);
            }).detach();
        }
        if (ticks % 600 == 1)
            std::fprintf(stderr, "[vd] tick-alive #%llu\n", (unsigned long long)ticks);
        if (uint32_t cb = g_intrCallback.load())
        {
            uint32_t ctxArg = g_intrContext.load();
            GWrite32(g_base, MMIO_SWAP_STATUS, 1);
            // Vblank raises ONLY source 0 (swap DPC). Source 1 is the CP interrupt and is
            // raised exclusively by CP_INTERRUPT packets (the ISR calls [shadow+16]
            // unconditionally, which holds 0x0BADF00D between fence sequences).
            GuestSetCurrentCpu(0);
            const auto pacingDispatch = clock::now();
            frame_pacing_probe::Dispatch(pacingDispatch);
            GuestCall2(cb, 0, ctxArg);
            GpuProgress();
            frame_pacing_probe::Tick(pacingDeadline, pacingWoke, pacingDispatch, clock::now(),
                                     refreshHz, pacingStalls, pacingReanchored);
            if (ticks % 600 == 0)
            {
                uint32_t rb2 = g_ringBase.load(), rs2 = g_ringSize.load(), dev2 = g_intrContext.load();
                uint32_t nz = 0, firstNz = 0, firstOff = 0;
                if (rb2 && rs2)
                {
                    uint32_t lim = rs2 > 0x400000 ? 0x400000 : rs2;
                    for (uint32_t off = 0; off < lim; off += 4)
                    {
                        uint32_t v = GRead32(g_base, rb2 + off);
                        if (v) { if (!nz) { firstNz = v; firstOff = off; } ++nz; }
                    }
                }
                {
                    uint32_t everCount = 0, lo = 512, hi = 0;
                    for (uint32_t i = 0; i < 512; ++i)
                        if (g_aluEver[i]) { ++everCount; if (i < lo) lo = i; hi = i; }
                    std::fprintf(stderr,
                        "[vd] aluEver=%u range=%u..%u c129=%u c131=%u c132=%u c133=%u\n",
                        everCount, lo, hi, g_aluEver[129], g_aluEver[131], g_aluEver[132],
                        g_aluEver[133]);
                }
                char prims[192]; int pn = 0;
                pn += BoundedSnprintf(prims + pn, sizeof(prims) - pn, "[vd] prims:");
                for (int pt = 0; pt < 64 && pn < 160; ++pt)
                    if (g_primSeen[pt])
                        pn += BoundedSnprintf(prims + pn, sizeof(prims) - pn, " %u:%u", pt,
                                            g_primSeen[pt]);
                BoundedSnprintf(prims + pn, sizeof(prims) - pn, "\n");
                std::fputs(prims, stderr);
                char ops[256]{}; int on = 0;
                for (int op = 0; op < 128 && on < 200; ++op)
                    if (g_opcodeSeen[op])
                        on += BoundedSnprintf(ops + on, sizeof(ops) - on, " %02X:%u", op, g_opcodeSeen[op]);
                // Screen-flow state: the scene-flow manager's loader machine + job slots
                // (mgr=0x82E6C5B8; layout from the GIN decode workflow). Shows which screen
                // state the game is parked in when no real draws are issued.
                {
                    constexpr uint32_t mgr = 0x82E6C5B8u;
                    static const uint32_t slots[] = { 6840, 6896, 6960, 7016, 7080, 7144,
                                                      7200, 7272, 7328, 7392, 7448 };
                    char fl[512]; int fn2 = 0;
                    fn2 += BoundedSnprintf(fl + fn2, sizeof(fl) - fn2,
                        "[flow] mgr16=%08X mgr40=%08X jobs:",
                        GRead32(g_base, mgr + 16), GRead32(g_base, mgr + 40));
                    for (uint32_t s : slots)
                    {
                        uint32_t job = GRead32(g_base, mgr + s);
                        fn2 += BoundedSnprintf(fl + fn2, sizeof(fl) - fn2, " %X=%08X",
                            s, job && job < 0xFFFFF000u ? GRead32(g_base, job + 24) : job);
                    }
                    BoundedSnprintf(fl + fn2, sizeof(fl) - fn2, "\n");
                    std::fputs(fl, stderr);
                    // BootMode wait1 gate: sound-manager slot load/loaded bytes, the two
                    // check-gate words, and the flow master gate (decode workflow session 6).
                    uint32_t sm = GRead32(g_base, 0x82EE7904u);
                    char sl[384]; int sn = 0;
                    sn += BoundedSnprintf(sl + sn, sizeof(sl) - sn, "[boot] sm=%08X slots:", sm);
                    if (sm && sm < 0xFFFFF000u)
                        for (int k = 0; k < 6; ++k)
                            sn += BoundedSnprintf(sl + sn, sizeof(sl) - sn, " %u/%u",
                                g_base[sm + 16 + 36 * k], g_base[sm + 17 + 36 * k]);
                    sn += BoundedSnprintf(sl + sn, sizeof(sl) - sn,
                        " chk=%08X,%08X gate7496=%08X q:",
                        GRead32(g_base, 0x82F321BCu), GRead32(g_base, 0x82F321C0u),
                        GRead32(g_base, 0x82E6C5B8u + 7496));
                    for (int i = 0; i < 8; ++i)
                        sn += BoundedSnprintf(sl + sn, sizeof(sl) - sn, " %08X",
                            GRead32(g_base, 0x82F2B6DCu + i * 4));
                    // Boot object 0x82E6B558's current state object (+4) and its fields -
                    // the vtable identifies which fsm state the boot is parked in.
                    uint32_t bhead = GRead32(g_base, 0x82E6B558u);
                    sn += BoundedSnprintf(sl + sn, sizeof(sl) - sn, " bhead=%08X/%08X s52=%d s64=%d",
                        bhead, bhead && bhead < 0xFFFFF000u ? GRead32(g_base, bhead + 4) : 0,
                        (int)GRead32(g_base, 0x82E6B558u + 52), (int)GRead32(g_base, 0x82E6B558u + 64));
                    uint32_t bst = GRead32(g_base, 0x82E6B558u + 4);
                    sn += BoundedSnprintf(sl + sn, sizeof(sl) - sn, " bst=%08X [", bst);
                    if (bst && bst < 0xFFFFF000u)
                        for (int i = 0; i < 6; ++i)
                            sn += BoundedSnprintf(sl + sn, sizeof(sl) - sn, " %08X",
                                GRead32(g_base, bst + i * 4));
                    // Sign-in record container [mgr+12]: idx [+4], array [+8]; the boot
                    // stage-3 gate reads [record+12] of the selected record.
                    if (bst && bst < 0xFFFFF000u)
                    {
                        uint32_t cont = GRead32(g_base, bst + 12);
                        uint32_t cidx = cont ? GRead32(g_base, cont + 4) : 0;
                        uint32_t carr = cont ? GRead32(g_base, cont + 8) : 0;
                        uint32_t e0 = carr ? GRead32(g_base, carr + 0) : 0;
                        sn += BoundedSnprintf(sl + sn, sizeof(sl) - sn,
                            " cont=%08X idx=%u arr=%08X e0=%08X", cont, cidx, carr, e0);
                        if (e0 && e0 < 0xFFFFF000u)
                            for (int i = 0; i < 6; ++i)
                                sn += BoundedSnprintf(sl + sn, sizeof(sl) - sn, " %08X",
                                    GRead32(g_base, e0 + i * 4));
                    }
                    // Boot stage-3 gate object = [signinMgr+16]; its vtbl+36 method is the
                    // real gate. Print the object, its vtable, the slot-36 fn, and fields.
                    if (bst && bst < 0xFFFFF000u)
                    {
                        uint32_t o16 = GRead32(g_base, bst + 16);
                        uint32_t v16 = o16 ? GRead32(g_base, o16) : 0;
                        uint32_t f36 = v16 ? GRead32(g_base, v16 + 36) : 0;
                        char ol[320]; int on = 0;
                        on += BoundedSnprintf(ol + on, sizeof(ol) - on,
                            "[gate3] o16=%08X v16=%08X f36=%08X f:", o16, v16, f36);
                        if (o16 && o16 < 0xFFFFF000u)
                            for (int i = 0; i < 10; ++i)
                                on += BoundedSnprintf(ol + on, sizeof(ol) - on, " %08X",
                                    GRead32(g_base, o16 + i * 4));
                        BoundedSnprintf(ol + on, sizeof(ol) - on, "\n");
                        std::fputs(ol, stderr);
                    }
                    // Sign-in manager vtable (runtime): slot +36 gates boot stage 3.
                    sn += BoundedSnprintf(sl + sn, sizeof(sl) - sn, " ] svt:");
                    for (int i = 0; i < 14; ++i)
                        sn += BoundedSnprintf(sl + sn, sizeof(sl) - sn, " %08X",
                            GRead32(g_base, 0x820DFAD4u + i * 4));
                    sn += BoundedSnprintf(sl + sn, sizeof(sl) - sn, "\n");
                    std::fputs(sl, stderr);
                    // BootMode impl fsm: [0x82F30B94] -> impl; dump words (state fn ptrs are
                    // 0x82xxxxxx code addresses identifying the parked state), plus the mode
                    // manager's current mode id ([0x82E5B014]+12).
                    uint32_t bm = GRead32(g_base, 0x82F30B94u);
                    uint32_t mm = GRead32(g_base, 0x82E5B014u);
                    char bl[512]; int bn = 0;
                    bn += BoundedSnprintf(bl + bn, sizeof(bl) - bn, "[bmode] impl=%08X mode=%08X [",
                        bm, mm && mm < 0xFFFFF000u ? GRead32(g_base, mm + 12) : 0xDEAD);
                    if (bm && bm < 0xFFFFF000u)
                        for (int i = 0; i < 16; ++i)
                            bn += BoundedSnprintf(bl + bn, sizeof(bl) - bn, " %08X",
                                GRead32(g_base, bm + i * 4));
                    bn += BoundedSnprintf(bl + bn, sizeof(bl) - bn, " ]\n");
                    std::fputs(bl, stderr);
                }
                uint32_t P2 = dev2 ? GRead32(g_base, dev2 + 11024) : 0;
                std::fprintf(stderr,
                    "[vd] sync dev=%08X P=%08X P0=%08X P60=%08X mir11088=%08X thr15344=%08X "
                    "rbase15300=%08X rmask15304=%08X q17112=%u q16972=%u q16976=%u q16968=%u\n",
                    dev2, P2,
                    P2 ? GRead32(g_base, P2) : 0, P2 ? GRead32(g_base, P2 + 60) : 0,
                    dev2 ? GRead32(g_base, dev2 + 11088) : 0,
                    dev2 ? GRead32(g_base, dev2 + 15344) : 0,
                    dev2 ? GRead32(g_base, dev2 + 15300) : 0,
                    dev2 ? GRead32(g_base, dev2 + 15304) : 0,
                    dev2 ? GRead32(g_base, dev2 + 17112) : 0,
                    dev2 ? GRead32(g_base, dev2 + 16972) : 0,
                    dev2 ? GRead32(g_base, dev2 + 16976) : 0,
                    dev2 ? GRead32(g_base, dev2 + 16968) : 0);
                std::fprintf(stderr,
                    "[vd] tick #%llu scb=%u cur=%u cursor=%08X bell=%08X rptr=%X nz=%u dpcSwaps=%u defSegs=%u pend=%08X rfb=%u lit=%llu regw=%u/%u/%u/%u ops:%s\n",
                    (unsigned long long)ticks,
                    g_sysCmdBuf.load() ? GRead32(g_base, g_sysCmdBuf.load()) : 0,
                    dev2 ? GRead32(g_base, dev2 + 11036) : 0,
                    dev2 ? GRead32(g_base, dev2 + 48) : 0,
                    GRead32(g_base, 0x7FC80714u), g_ringRptr.load(), nz,
                    dev2 ? GRead32(g_base, dev2 + 16952) : 0,
                    dev2 ? GRead32(g_base, dev2 + 11152) : 0,
                    dev2 ? GRead32(g_base, GRead32(g_base, dev2 + 11028) + 16) : 0,
                    g_resolvesToFront.load(), (unsigned long long)GpuLitDraws(),
                    g_regWrites[0], g_regWrites[1], g_regWrites[2], g_regWrites[3], ops);
            }
        }
    }
}

PPC_FUNC(__imp__VdInitializeEngines)
{
    KTrace("VdInitializeEngines");
    std::fprintf(stderr, "[vd] InitializeEngines flags=%08X notify=%08X\n", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u32 = 1;
}

PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    KTrace("VdSetGraphicsInterruptCallback");
    std::fprintf(stderr, "[vd] SetGraphicsInterruptCallback cb=%08X ctx=%08X\n", ctx.r3.u32, ctx.r4.u32);
    g_intrCallback = ctx.r3.u32;
    g_intrContext = ctx.r4.u32;
    // Kernel DATA exports. The XEX loader is supposed to patch the import slots at
    // 0x82000690/0x82000694 with the ADDRESSES of the kernel's exported VdGlobalXamDevice /
    // VdGlobalDevice variables; ours still held the raw ordinals (0x1BF / 0x1BE), so the D3D
    // runtime's ISR swap callback sub_82740610 resolved its device via *(0x1BE) == NULL and
    // every store it made - the flip-queue enqueue, the swap serial, and the SCRATCH_REG1
    // throttle ack at [shadow+4] - was silently erased by the null-page guard. (That is
    // exactly the long-logged `[nullpage] write@0x42E0`: the swap spinlock at NULL+17120.)
    // Point both slots at one real cell and publish the device in it.
    if (uint32_t dev0 = ctx.r4.u32)
    {
        static uint32_t cell = 0;
        if (!cell)
        {
            void* pcell = g_virtualHeap.Alloc(8, 16);
            if (pcell) cell = (uint32_t)(reinterpret_cast<uint8_t*>(pcell) - g_base);
        }
        if (cell)
        {
            GWrite32(g_base, cell, dev0);
            GWrite32(g_base, 0x82000690u, cell);
            GWrite32(g_base, 0x82000694u, cell);
            std::fprintf(stderr, "[vd] VdGlobalDevice import slots -> cell %08X = %08X\n",
                         cell, dev0);
        }
    }
    if (ctx.r3.u32 && !g_tickStarted.exchange(true))
    {
        StartBigStackThread([](void*) -> unsigned { VideoTick(); return 0; });
        StartBigStackThread([](void*) -> unsigned { CommandProcessorThread(); return 0; });
        if (int delay = EnvInt("TTT2_DUMPCONTAINERS", 0))
            StartBigStackThread([](void*) -> unsigned {
                std::this_thread::sleep_for(
                    std::chrono::seconds(EnvInt("TTT2_DUMPCONTAINERS", 60)));
                SweepShaderContainers();
                return 0;
            });
    }
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    KTrace("VdInitializeRingBuffer");
    uint32_t base_ = ctx.r3.u32, len = ctx.r4.u32;
    // len=0xC and the D3D device's own ring mask ([dev+15304] = 0x1FFF DWORDS) pin the
    // decode: ring bytes = 8 << len -> 32 KB. The old 0x1000<<len (16 MB) misparse could not
    // bite until session 11: the stream never WRAPPED before the audio-driven engine ran at
    // full submission rate. When it finally did, the CP walked off the real 32 KB ring into
    // garbage memory, and the junk "packets" included type-0 writes that clobbered
    // CP_RB_WPTR itself (the 13EB92E0 / 0D564F49 doorbell values of run_s11j).
    uint32_t size = (len < 32) ? (8u << len) : len;
    std::fprintf(stderr, "[vd] InitializeRingBuffer base=%08X len=%08X (size=%u)\n", base_, len, size);
    g_ringBase = base_;
    g_ringSize = size;
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    KTrace("VdEnableRingBufferRPtrWriteBack");
    std::fprintf(stderr, "[vd] EnableRingBufferRPtrWriteBack addr=%08X (guest %08X) r4=%08X\n",
                 ctx.r3.u32, PhysToGuest(ctx.r3.u32), ctx.r4.u32);
    g_rptrWriteBack = PhysToGuest(ctx.r3.u32);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress)
{
    KTrace("VdSetSystemCommandBufferGpuIdentifierAddress");
    std::fprintf(stderr, "[vd] SetSysCmdBufGpuIdentifierAddress addr=%08X\n", ctx.r3.u32);
    g_sysCmdBuf = ctx.r3.u32 ? ctx.r3.u32 - 8 : 0;
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdGetSystemCommandBuffer)
{
    KTrace("VdGetSystemCommandBuffer");
    if (ctx.r3.u32) std::memset(g_base + ctx.r3.u32, 0, 148);
    GWrite32(base, ctx.r4.u32, 0);                              // current GPU identifier
    ctx.r3.u32 = 0;
}

// Decode the surface address out of a 6-dword Xenos texture fetch constant, validating it
// so a recycled stack buffer cannot point the window at unrelated memory. Layout confirmed
// against the game's own block 8A000002 A44A9006 0059E4FF ...: dword0 bits 0-1 = type
// (2 = texture), bits 22-30 = pitch/32, bit 31 = tiled; dword1 bits 0-5 = format,
// bits 12-31 = base address >> 12; dword2 = (width-1) | (height-1) << 13.
static uint32_t DecodeFrontBuffer(uint32_t fetchBlock)
{
    if (!fetchBlock) return 0;
    uint32_t fw[6];
    for (int i = 0; i < 6; ++i) fw[i] = GRead32(g_base, fetchBlock + i * 4);
    if ((fw[0] & 3) != 2) return 0;                              // not a texture fetch constant
    uint32_t surf = PhysToGuest((fw[1] >> 12) << 12);
    uint32_t w = (fw[2] & 0x1FFF) + 1, h = ((fw[2] >> 13) & 0x1FFF) + 1;
    static int flog = 0;
    if (flog < 6)
    {
        ++flog;
        std::fprintf(stderr,
            "[vd] fetchblk %08X: %08X %08X %08X %08X %08X %08X -> surf=%08X %ux%u\n",
            fetchBlock, fw[0], fw[1], fw[2], fw[3], fw[4], fw[5], surf, w, h);
    }
    if (!surf || w != FB_W || h != FB_H) return 0;
    // The surface is page-aligned: carry the fetch constant's 12-bit swizzle in the low bits
    // (the front buffer reads .zyx1 = 0xA0A, undoing the final resolve's red/blue swap).
    return surf | ((fw[3] >> 1) & 0xFFFu);
}

// VdSwap(r3=ring wptr+4 where the kernel writes the swap packet, r4=frontbuffer texture
// fetch block, r5=identifier addr, ...). The caller at 0x82741738 reserves 64 dwords,
// passes oldCursor+4, then sets its last-written cursor to oldCursor+256. This owns
// [r3, r3+252] inclusive: all 64 words must be initialized, including the last word
// on a reused command buffer. The flip bookkeeping runs in the game's own DPC.
void (*g_uiFrameHook)(PPCContext& ctx, uint8_t* base) = nullptr;

void DirtyMeasureFrame();   // rollback_probe.cpp (TTT2_DIRTY_MEASURE=1)

bool RollbackInResimDraw();                    // rollback.cpp
void RollbackResimTime(int id, double ms);

PPC_FUNC(__imp__VdSwap)
{
    KTrace("VdSwap");
    struct SwapTimer
    {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~SwapTimer() { if (RollbackInResimDraw()) RollbackResimTime(5, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()); }
    } swapTimer;
    DirtyMeasureFrame();
    g_gameSwaps.fetch_add(1, std::memory_order_relaxed);
    if (InputTimingOn())
        std::fprintf(stderr, "[swap] #%u at %.1f ms\n", uint32_t(g_gameSwaps.load()),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count());
    timeline::Mark(timeline::Swap, '.', uint32_t(g_gameSwaps.load()));
    if (FrameTimeLog())
    {
        static FrameTimer swaps{ "swap" };
        swaps.Mark();
        static const bool once = (std::fprintf(stderr, "[frame] VdSwap thread %u\n", host_thread::CurrentId()), true);
        (void)once;
    }
    static int swaps = 0;
    if (swaps < 5)
        std::fprintf(stderr, "[vd] VdSwap #%d ring=%08X fbtex=%08X ident=%08X\n",
                     ++swaps, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    if (uint32_t surf = DecodeFrontBuffer(ctx.r4.u32))
    {
        g_frontBufferPhys.store(surf & ~0xFFFu);
        g_frontBufferSwizzle.store(surf & 0xFFFu);
        if (!g_presentStarted.exchange(true)) StartBigStackThread(PresentThread);
    }
    if (uint32_t p = ctx.r3.u32)
    {
        // Write a marker block the CP recognizes when it executes this segment (the real
        // kernel writes a GPU swap packet here; the caller advances its cursor 64 dwords).
        // On execution the CP must write the identifier to [r5] and enqueue the flip -
        // that is what lets the present path consider the swap complete.
        GWrite32(base, p + 0,  0x54535750u);        // 'TSWP' marker
        GWrite32(base, p + 4,  ctx.r7.u32);         // identifier value
        GWrite32(base, p + 8,  ctx.r5.u32);         // identifier address
        GWrite32(base, p + 12, ctx.r4.u32);         // frontbuffer texture fetch block
        // The fetch block lives on the CALLER'S STACK, so it is gone by the time the CP
        // executes this packet (it decoded a stale/zero surface and pointed the window at
        // the wrong memory). Decode it here, while it is still valid, and carry the result.
        const uint32_t surface = DecodeFrontBuffer(ctx.r4.u32);
        GWrite32(base, p + 16, surface);
        for (int i = 5; i < 64; ++i) GWrite32(base, p + i * 4, 0x80000000u);
        const uint32_t n = g_markerSwaps.load() + 1;
        g_markerFb[n & 15].store(surface);
        frame_pacing_probe::Published(n);
        g_markerSwaps.store(n);
        GpuProgress();                                  // a CP waiting for the vblank can present it now
    }
    // Port UI actions on the game's main thread, once per frame (menu_ext.cpp: the in-game
    // profile / message-box screens). The guest's registers are restored afterwards.
    if (g_uiFrameHook)
    {
        const PPCContext saved = ctx;
        g_uiFrameHook(ctx, base);
        ctx = saved;
    }
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdQueryVideoMode)
{
    KTrace("VdQueryVideoMode");
    uint32_t p = ctx.r3.u32;
    if (p)
    {
        std::memset(g_base + p, 0, 48);
        GWrite32(base, p + 0,  1280);          // DisplayWidth
        GWrite32(base, p + 4,  720);           // DisplayHeight
        GWrite32(base, p + 8,  0);             // IsInterlaced
        GWrite32(base, p + 12, 1);             // IsWidescreen
        GWrite32(base, p + 16, 1);             // IsHighDefinition
        GWrite32(base, p + 20, 0x42700000);    // RefreshRate = 60.0f
        GWrite32(base, p + 24, 1);             // VideoStandard (NTSC-M)
        GWrite32(base, p + 28, 0x4A);
        GWrite32(base, p + 32, 0x01);
    }
}

PPC_FUNC(__imp__XGetVideoMode)
{
    KTrace("XGetVideoMode");
    __imp__VdQueryVideoMode(ctx, base);
}

PPC_FUNC(__imp__VdQueryVideoFlags)
{
    KTrace("VdQueryVideoFlags"); ctx.r3.u32 = 1; }               // bit0 = widescreen

PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded)
{
    KTrace("VdIsHSIOTrainingSucceeded"); ctx.r3.u32 = 1; }       // nonzero = succeeded

PPC_FUNC(__imp__VdGetCurrentDisplayGamma)
{
    KTrace("VdGetCurrentDisplayGamma");
    GWrite32(base, ctx.r3.u32, 2);                               // gamma type
    GWrite32(base, ctx.r4.u32, 0x400CCCCD);                      // 2.2f
}

PPC_FUNC(__imp__VdGetCurrentDisplayInformation)
{
    KTrace("VdGetCurrentDisplayInformation");
    uint32_t p = ctx.r3.u32;
    if (p)
    {
        // The struct is 88 bytes. Writing more trashes the caller's stack frame: the
        // present-path caller sub_827481E8 keeps its saved r30/r31 right after this
        // buffer, and a larger memset nulls the restored device pointer (boot-stall!).
        std::memset(g_base + p, 0, 88);
        GWrite16(base, p + 72, 1280);
        GWrite16(base, p + 74, 720);
        GWrite16(base, p + 86, 60);
    }
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__VdSetDisplayMode)
{
    KTrace("VdSetDisplayMode"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__VdSetDisplayModeOverride)
{
    KTrace("VdSetDisplayModeOverride"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines)
{
    KTrace("VdCallGraphicsNotificationRoutines"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer)
{
    KTrace("VdInitializeScalerCommandBuffer"); ctx.r3.u32 = 0; } // 0 dwords written
PPC_FUNC(__imp__VdRetrainEDRAM)
{
    KTrace("VdRetrainEDRAM"); ctx.r3.u32 = 0; }                  // 0 = done, no ring pass needed
PPC_FUNC(__imp__VdRetrainEDRAMWorker)
{
    KTrace("VdRetrainEDRAMWorker"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__VdEnableDisableClockGating)
{
    KTrace("VdEnableDisableClockGating"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__VdShutdownEngines)
{
    KTrace("VdShutdownEngines"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__VdPersistDisplay)
{
    KTrace("VdPersistDisplay");
    if (ctx.r3.u32) { GWrite32(base, ctx.r3.u32, 0); GWrite32(base, ctx.r3.u32 + 4, 0); GWrite32(base, ctx.r3.u32 + 8, 0); }
    if (ctx.r4.u32) GWrite32(base, ctx.r4.u32, 0);
    ctx.r3.u32 = 0;                                              // false: nothing persisted
}

// Timeline dump (TTT2_TIMELINE=1): the last 3 seconds of events to C:/ttt2/port/timeline.txt.
void timeline::PollDumpTrigger()
{
    if (!DebugExists("C:/ttt2/port/timeline.trigger")) return;
    DebugRemove("C:/ttt2/port/timeline.trigger");
    const uint64_t end = Next().load(), now = NowUs();
    const uint64_t first = end > kEntries ? end - kEntries : 0;
    FILE* f = DebugOpen("C:/ttt2/port/timeline.txt", "w");
    if (!f) return;
    uint64_t start = first;
    while (start < end && Ring()[start & (kEntries - 1)].us + 3000000 < now) ++start;
    const uint64_t t0 = start < end ? Ring()[start & (kEntries - 1)].us : now;
    for (uint64_t i = start; i < end; ++i)
    {
        const Entry& e = Ring()[i & (kEntries - 1)];
        std::fprintf(f, "%llu t%u %s %c %08X\n", (unsigned long long)(e.us - t0), e.thread, Name(e.event), e.phase, e.value);
    }
    std::fclose(f);
    std::fprintf(stderr, "[timeline] %llu events written to C:/ttt2/port/timeline.txt\n", (unsigned long long)(end - start));
}
