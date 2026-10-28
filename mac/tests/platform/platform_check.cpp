// The test "game" behind apple_game_entry.cpp: does what the port does, from the threads it does it on.
//   1. audio: InitializeHostAudio on the game thread, one AudioOutputClient fed silent frames at the
//      render-credit pace for 0.5 s (a muted check: every sample is 0).
//   2. window (unless PLATFORM_CHECK_NO_WINDOW=1): a separate 8 MiB host thread, like PresentThread,
//      opens the SDL window, creates a Vulkan surface for it on the main thread (what DXVK's SDL3
//      WSI must do on macOS), pumps it for ~1.5 s through host_window::Poll, closes it.
#include "audio_output.h"
#include "host_input_sdl.h"
#include "host_process.h"
#include "host_thread.h"
#include "host_window.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// Stand-ins for the port's kernel pieces that the window code calls.
bool SysUiTextEntryActive() { return false; }
bool SysUiKeyCaptureActive() { return false; }
void SysUiOnKey(int) {}
void SysUiOnChar(uint32_t) {}
void LogDrain(unsigned) {}
namespace host_process { [[noreturn]] void ExitNow(int status) { std::fflush(nullptr); std::_Exit(status); } }

static int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_failures; std::fprintf(stderr, "FAIL " __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)

static uint64_t NowNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void AudioCheck()
{
    CHECK(InitializeHostAudio(), "InitializeHostAudio");
    AudioOutputClient client(0x12345678);
    static uint8_t silence[AUDIO_PCM_BYTES] = {};
    const uint64_t end = NowNs() + 500'000'000ull;
    uint64_t submitted = 0;
    while (NowNs() < end)
    {
        if (client.TryAcquireRenderCredit(NowNs())) { client.Submit(silence); ++submitted; }
        else WaitAudioOutputWake(AudioOutputWakeVersion(), 2'000'000);
    }
    const AudioOutputStats st = client.Stats();
    std::fprintf(stderr, "[check] audio: driver %s, submitted %llu, completed %llu, dropped %llu, started %d failed %d\n",
                 SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(none)",
                 (unsigned long long)submitted, (unsigned long long)st.completed,
                 (unsigned long long)st.dropped, st.started, st.failed);
    CHECK(!st.failed && st.started, "audio device did not start");
    // 0.5 s at 48 kHz is ~94 frames of 256; allow the startup.
    CHECK(st.completed > 50, "audio completed only %llu frames", (unsigned long long)st.completed);
    client.Close();
}

struct SurfaceRequest { SDL_Window* window; VkInstance instance; VkSurfaceKHR surface; bool ok; };

static bool VulkanSurfaceCheck(SDL_Window* window)
{
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!gipa) { std::fprintf(stderr, "[check] no Vulkan loader: %s\n", SDL_GetError()); return false; }
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    Uint32 count = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&count);
    std::vector<const char*> exts(sdlExts, sdlExts + count);
    exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = uint32_t(exts.size());
    info.ppEnabledExtensionNames = exts.data();
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult r = createInstance(&info, nullptr, &instance);
    if (r != VK_SUCCESS) { std::fprintf(stderr, "[check] vkCreateInstance %d\n", int(r)); return false; }
    // What the DXVK SDL3 WSI's createSurface has to do on macOS.
    SurfaceRequest req{window, instance, VK_NULL_HANDLE, false};
    SDL_RunOnMainThread([](void* raw) {
        auto* q = static_cast<SurfaceRequest*>(raw);
        q->ok = SDL_Vulkan_CreateSurface(q->window, q->instance, nullptr, &q->surface);
    }, &req, true);
    bool good = req.ok;
    if (good)
    {
        auto destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(gipa(instance, "vkDestroySurfaceKHR"));
        destroySurface(instance, req.surface, nullptr);
    }
    else std::fprintf(stderr, "[check] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"))(instance, nullptr);
    return good;
}

static std::atomic<bool> g_windowDone{false};

static unsigned PresentLikeThread(void*)
{
    host_window::Window* window = host_window::Open(640, 360);
    CHECK(window, "host_window::Open from a worker thread");
    if (window)
    {
        SDL_Window* handle = host_window::SDLHandle(window);
        int w = 0, h = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(handle, &w, &h);
        SDL_GetWindowSizeInPixels(handle, &pw, &ph);
        const SDL_WindowFlags flags = SDL_GetWindowFlags(handle);
        std::fprintf(stderr, "[check] window %dx%d points, %dx%d pixels, vulkan %d, high density %d, native %p\n",
                     w, h, pw, ph, (flags & SDL_WINDOW_VULKAN) != 0, (flags & SDL_WINDOW_HIGH_PIXEL_DENSITY) != 0,
                     (void*)host_window::GetNativeSurface(window).window);
        CHECK(flags & SDL_WINDOW_VULKAN, "window lacks SDL_WINDOW_VULKAN");
        CHECK(VulkanSurfaceCheck(handle), "Vulkan surface on the window");
        host_window::ApplyMode(window, host_window::Mode::Windowed, 640, 360);
        int polls = 0;
        for (const uint64_t end = NowNs() + 1'500'000'000ull; NowNs() < end; ++polls)
        {
            const host_window::Events e = host_window::Poll(window);
            if (e.close) { std::fprintf(stderr, "[check] close requested\n"); break; }
            host_window::Idle(5);
        }
        const host_input::Snapshot snap = host_input::Read();
        int pads = 0;
        for (const auto& pad : snap.pads) pads += pad.connected;
        std::fprintf(stderr, "[check] %d polls, focused %d, %d gamepads\n", polls, snap.focused, pads);
        host_window::Close(window);
    }
    g_windowDone = true;
    return 0;
}

int ttt2_game_main(int, char**)
{
    std::fprintf(stderr, "[check] game thread is main thread: %d\n", SDL_IsMainThread());
    CHECK(!SDL_IsMainThread(), "the game must not run on the main thread");
    AudioCheck();
    if (!(std::getenv("PLATFORM_CHECK_NO_WINDOW") && *std::getenv("PLATFORM_CHECK_NO_WINDOW") == '1'))
    {
        CHECK(host_thread::StartDetached(8 * 1024 * 1024, PresentLikeThread, nullptr), "present thread");
        while (!g_windowDone) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::fprintf(stderr, g_failures ? "[check] %d FAILURES\n" : "[check] PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
