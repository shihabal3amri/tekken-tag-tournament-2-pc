// macOS entry point. AppKit owns the process's main thread: SDL's Cocoa backend creates its video
// device only there (Cocoa_CreateDevice), windows and views must be made and changed there, the
// IOKit / GameController hotplug run loop is the one SDL_InitSubSystem(GAMEPAD) ran on, and only
// that thread may pump events. The port creates its window from the presentation thread and the
// guest's _xstart never returns, so (as on Android) the game's main() runs on a worker while this
// thread initializes SDL video and gamepads, pumps events and runs the window calls that
// host_window_sdl.cpp hands it through SDL_RunOnMainThread.
// Build: compile main.cpp with main=ttt2_game_main and link this file.
#include <SDL3/SDL.h>

#include "apple_import.h"
#include "host_process.h"
#include "host_runtime.h"

#include <mach-o/dyld.h>
#include <pthread.h>
#include <climits>
#include <filesystem>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

int ttt2_game_main(int argc, char** argv);
void LogDrain(unsigned timeoutMs);   // kernel/kernel_shared.h (the log writer thread)

namespace {
struct GameRun
{
    int argc;
    char** argv;
    int result = 1;
    std::atomic<bool> finished{false};
};

void* GameThread(void* raw)
{
    auto* run = static_cast<GameRun*>(raw);
    run->result = ttt2_game_main(run->argc, run->argv);
    run->finished.store(true, std::memory_order_release);
    SDL_Event wake{};
    wake.type = SDL_EVENT_USER;
    SDL_PushEvent(&wake);
    return nullptr;
}

// Tool modes of main.cpp that never open a window: no SDL video, no Dock icon.
bool ToolMode(int argc, char** argv)
{
    if (argc < 2) return false;
    for (const char* mode : {"--steam-bridge", "--translate", "--build-shaders"})
        if (std::strcmp(argv[1], mode) == 0) return true;
    return false;
}

// Inside the .app (mac/make_app.sh) the Vulkan driver is the bundled MoltenVK. The loader would
// also load a Homebrew MoltenVK from /opt/homebrew/etc/vulkan/icd.d: a second copy of the same
// classes in the process and a second, identical adapter. Info.plist's LSEnvironment covers
// launches from Finder; this covers running the executable directly.
void PreferBundledVulkanDriver()
{
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return;
    std::error_code error;
    const auto exe = std::filesystem::canonical(path, error);
    if (error) return;
    const auto manifest = exe.parent_path().parent_path() / "Resources/vulkan/icd.d/MoltenVK_icd.json";
    if (std::filesystem::is_regular_file(manifest, error))
        setenv("VK_LOADER_SEARCH_ONLY_IN_BUNDLE", "1", 0);
}
} // namespace

int main(int argc, char** argv)
{
    PreferBundledVulkanDriver();
    if (ToolMode(argc, argv)) return ttt2_game_main(argc, argv);

    SDL_SetAppMetadata("Tekken Tag Tournament 2", nullptr, "io.github.shihabal3amri.ttt2");
    // Full screen without a separate Space: no animation, and the swap chain sees one resize.
    if (!std::getenv("SDL_VIDEO_MAC_FULLSCREEN_SPACES"))
        SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
    // A launch from Finder has no terminal: Cmd+Q and the window's close button quit, and the
    // game's own menus end it too. SDL's default menu bar supplies Cmd+Q (SDL_EVENT_QUIT).
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        std::fprintf(stderr, "[mac] SDL init (video, gamepad): %s\n", SDL_GetError());

    // No game data yet: the import window (apple_import.mm) first, as ttt2_setup.exe on Windows.
    if (argc <= 1 && host_runtime::InstalledLayout(argc).empty() && !ttt2_mac::ImportGameData())
    {
        SDL_Quit();
        return 0;
    }

    // The guest's call chains are deep: the main thread had 8 MiB; give the game twice that, on the
    // performance cores like the main thread (user-interactive QoS).
    GameRun run{argc, argv};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u * 1024 * 1024);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_t game;
    const int created = pthread_create(&game, &attr, GameThread, &run);
    pthread_attr_destroy(&attr);
    if (created != 0)
    {
        std::fprintf(stderr, "[mac] cannot start the game thread (%d)\n", created);
        return 1;
    }

    while (!run.finished.load(std::memory_order_acquire))
    {
        // Pumping runs queued main-thread calls and fills SDL's queue; the presentation thread
        // drains it (host_window::Poll). Before the window exists nobody drains it, so a quit
        // there (Cmd+Q, Dock > Quit) is handled here.
        SDL_PumpEvents();
        int windows = 0;
        SDL_free(SDL_GetWindows(&windows));
        if (windows == 0)
        {
            SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_QUIT - 1);
            if (SDL_HasEvent(SDL_EVENT_QUIT))
            {
                std::fprintf(stderr, "[mac] quit before the window opened\n");
                LogDrain(300);
                host_process::ExitNow(0);
            }
            SDL_FlushEvents(SDL_EVENT_QUIT + 1, SDL_EVENT_LAST);
        }
        SDL_Delay(2);
    }
    pthread_join(game, nullptr);
    SDL_Quit();
    return run.result;
}
