// Process services for macOS. The game data folder (what the installer makes:
// tekken.img, data*.bin, ...) is found, in order, at:
//   1. $TTT2_VFS_ROOT, when set (tekken.img in it or in its parent);
//   2. <App>.app/Contents/Resources/game;
//   3. next to the executable or the .app bundle: <dir>/game;
//   4. ~/Library/Application Support/TTT2Recomp/game.
// The disc files are read from <game>/game_files_x when it exists, else <game>.
// Saves and settings live in ~/Library/Application Support/TTT2Recomp
// (host_storage_linux.cpp, host_settings_linux.cpp), caches in
// ~/Library/Caches/TTT2Recomp, and an installed launch logs to
// ~/Library/Logs/TTT2Recomp/log.txt.
#include "host_runtime.h"
#include "host_vm.h"
#include "host_thread.h"
#include "host_posix_compat.h"
#include "host_runtime_posix_diagnostics.h"

#if !defined(__APPLE__)
#error "This runtime host backend requires macOS"
#endif

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace host_runtime {
namespace {
bool g_installed = false;

std::filesystem::path Executable()
{
    char path[PATH_MAX] = {};
    if (!host_posix::ExecutablePath(path, sizeof(path))) return {};
    return path;
}

std::filesystem::path Home()
{
    const char* home = std::getenv("HOME");
    return home && *home && std::filesystem::path(home).is_absolute() ?
        std::filesystem::path(home) : std::filesystem::path{};
}

bool HasImage(const std::filesystem::path& folder)
{
    std::error_code error;
    return !folder.empty() && std::filesystem::is_regular_file(folder / "tekken.img", error) && !error;
}

void SetDefault(const char* key, const std::filesystem::path& value)
{
    if (!value.empty()) setenv(key, value.c_str(), 0);
}

// The folder holding tekken.img, or empty.
std::filesystem::path FindGame()
{
    // TTT2_VFS_ROOT may name the game folder or its game_files_x subfolder.
    if (const char* configured = std::getenv("TTT2_VFS_ROOT"); configured && *configured)
    {
        const std::filesystem::path root(configured);
        if (HasImage(root)) return root;
        return HasImage(root.parent_path()) ? root.parent_path() : std::filesystem::path{};
    }
    const auto executable = Executable();
    if (!executable.empty())
    {
        // .../Name.app/Contents/MacOS/exe
        const auto macos = executable.parent_path();
        const auto contents = macos.parent_path();
        const bool bundled = macos.filename() == "MacOS" && contents.filename() == "Contents" &&
                             contents.parent_path().extension() == ".app";
        if (bundled)
        {
            if (HasImage(contents / "Resources" / "game")) return contents / "Resources" / "game";
            if (HasImage(contents.parent_path().parent_path() / "game"))
                return contents.parent_path().parent_path() / "game";
        }
        if (HasImage(macos / "game")) return macos / "game";
    }
    const auto home = Home();
    if (!home.empty())
    {
        const auto support = home / "Library" / "Application Support" / "TTT2Recomp" / "game";
        if (HasImage(support)) return support;
    }
    return {};
}
} // namespace

std::filesystem::path InstalledLayout(int argc)
{
    // DXVK Native needs the SDL3 window-system driver even when the image path
    // is passed explicitly by a development launcher.
    setenv("DXVK_WSI_DRIVER", "SDL3", 0);
    setenv("TTT2_GPU_HW", "1", 0);
    setenv("TTT2_FBDUMP", "0", 0);
    // New pipelines compile on DXVK's async threads (mac/patches/dxvk-1.9.2b-async-pipelines-macos.patch,
    // DXVK_ASYNC=0 turns it off); let Metal run more of those compiles at once (macOS 13.3+).
    setenv("MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION", "1", 0);
    // A bundle starts in "/", so every cache needs an absolute, writable home.
    const auto home = Home();
    if (!home.empty())
    {
        const auto caches = home / "Library" / "Caches" / "TTT2Recomp";
        std::error_code error;
        std::filesystem::create_directories(caches, error);
        if (!error)
        {
            SetDefault("TTT2_SHADER_CACHE", caches / "shadercache");
            SetDefault("DXVK_STATE_CACHE_PATH", caches);
            SetDefault("DXVK_LOG_PATH", caches);
            // As on Android: keeps MoltenVK's shader conversions between runs.
            SetDefault("DXVK_PIPELINE_CACHE_FILE", caches / "dxvk-1.9.2b-ttt2-v1.vkcache");
        }
    }
    if (argc > 1) return {};
    const auto game = FindGame();
    if (game.empty()) return {};
    // The installer puts the disc files (nrfo.bin, data*.bin, update/) in
    // game/game_files_x beside tekken.img, as Android's runtime expects.
    if (const char* configured = std::getenv("TTT2_VFS_ROOT"); !configured || !*configured)
    {
        std::error_code error;
        const auto files = game / "game_files_x";
        const auto root = std::filesystem::is_directory(files, error) && !error ? files : game;
        setenv("TTT2_VFS_ROOT", root.c_str(), 1);
    }
    g_installed = true;
    return game / "tekken.img";
}

std::filesystem::path VfsRoot()
{
    const char* configured = std::getenv("TTT2_VFS_ROOT");
    return configured && *configured ? std::filesystem::path(configured) :
                                      std::filesystem::path(DefaultVfsRoot());
}

const char* DefaultImagePath() { return "re/decrypted/Default_patched_tu3.bin"; }
const char* DefaultVfsRoot() { return "re/game_files_x"; }

void StartLogWriter()
{
    // A Finder launch has no terminal: keep the log where Console.app finds it.
    // Terminal launches keep stderr.
    if (!g_installed || isatty(STDERR_FILENO)) return;
    const auto home = Home();
    if (home.empty()) return;
    const auto folder = home / "Library" / "Logs" / "TTT2Recomp";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) return;
    const int log = open((folder / "log.txt").c_str(),
                         O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (log < 0) return;
    dup2(log, STDERR_FILENO);
    close(log);
}

void ConfigureTimer() {} // Darwin sleeps already have high-resolution timers.

void InstallFatalHandler()
{
    // Fault recovery is owned by the VM. Keep the optional scheduled diagnostic
    // dump available through the same startup setting as Linux.
    const char* configured = std::getenv("TTT2_STACKDUMP");
    const int seconds = configured ? std::atoi(configured) : 0;
    if (seconds > 0)
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            DumpAllStacks("scheduled thread dump");
            std::fflush(stderr);
            _exit(3);
        }).detach();
}

void InstallGuestGuards()
{
    if (!host_vm::EnableNullPageGuard()) std::abort();
}
} // namespace host_runtime

void LogDrain(unsigned) {} // stderr stays unbuffered in the common entry point.

void host_runtime::StartShaderPrebuild()
{
    const auto executable = Executable();
    if (executable.empty()) return;
    const unsigned cores = host_thread::AvailableProcessors();
    const std::string workers = std::to_string(cores > 3 ? cores / 2 : 1);
    const std::string program = executable.string();
    char* arguments[] = { const_cast<char*>(program.c_str()), const_cast<char*>("--build-shaders"),
                          const_cast<char*>(""), const_cast<char*>(""),
                          const_cast<char*>(workers.c_str()), nullptr };
    // posix_spawn, not fork: other threads may hold libc or Objective-C locks.
    // The child lowers its own priority (setpriority is per process on macOS).
    pid_t child = -1;
    if (posix_spawn(&child, program.c_str(), nullptr, nullptr, arguments, environ) != 0) return;
    setpriority(PRIO_PROCESS, child, 10);
    std::thread([child] {
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            std::fprintf(stderr, "[shaders] background prebuild exited with status %d\n", WEXITSTATUS(status));
    }).detach();
}
