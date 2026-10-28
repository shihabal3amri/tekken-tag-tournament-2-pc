// First launch on macOS: the counterpart of ttt2_setup.exe (port/INSTALLER.md). The app ships no game
// data. When it finds none (host_runtime_mac.cpp FindGame), this window asks for the player's disc
// image and Title Update 3, and the Android importer (setup/mobile_install.cpp: the same SHA-1 checks,
// LZX expansion and one atomic rename) writes them to ~/Library/Application Support/TTT2Recomp/game.
// The game's shaders are then prepared once (`--build-shaders`, as the Windows installer does), and
// the game starts. Runs on the main thread after SDL_Init, so NSApp is SDL's.
#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>

#include "apple_import.h"
#include "setup/game_data.h"
#include "setup/mobile_install.h"
#include "setup/stfs.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;
using namespace ttt2::setup;

namespace {
fs::path Destination()
{
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return fs::path(home) / "Library" / "Application Support" / "TTT2Recomp" / "game";
}

// The player may choose the TU file or the folder holding all three updates (as on Windows): the
// package of this game whose Default.xexp targets 0.0.3.2. Returns its path, or empty and `why`.
fs::path FindTitleUpdate(const fs::path& chosen, std::string& why)
{
    std::vector<fs::path> candidates;
    std::error_code error;
    if (fs::is_directory(chosen, error))
    {
        for (const auto& entry : fs::recursive_directory_iterator(chosen, fs::directory_options::skip_permission_denied, error))
            if (entry.is_regular_file(error) && entry.file_size(error) < (1ull << 30) &&
                entry.path().filename().string().front() != '.')
                candidates.push_back(entry.path());
    }
    else
        candidates.push_back(chosen);
    std::string seen;
    for (const auto& path : candidates)
    {
        StfsPackage package;
        std::string ignored;
        std::vector<uint8_t> xexp;
        if (!package.Open(path, ignored) || package.TitleId() != kTitleId || !package.ReadFile("Default.xexp", xexp))
            continue;
        const std::string version = XexpTargetVersion(xexp);
        if (version == "0.0.3.2") return path;
        seen = version;
    }
    why = !seen.empty() ?
        "That is the title update for game version " + seen + ". The port needs Title Update 3 "
        "(version 0.0.3.2, file name ending in \"O2\")." :
        "No Tekken Tag Tournament 2 title update was found there. Choose the Title Update 3 "
        "file (TU_174Q22P_0000008000000.00000000000O2) or the folder holding it.";
    return {};
}

fs::path Executable()
{
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    return _NSGetExecutablePath(path, &size) == 0 ? fs::path(path) : fs::path{};
}
} // namespace

@interface TTT2Importer : NSObject <NSWindowDelegate>
@property(strong) NSWindow* window;
@property(strong) NSTextField* discField;
@property(strong) NSTextField* updateField;
@property(strong) NSButton* discButton;
@property(strong) NSButton* updateButton;
@property(strong) NSButton* installButton;
@property(strong) NSButton* quitButton;
@property(strong) NSProgressIndicator* bar;
@property(strong) NSTextField* status;
@property(copy) NSString* discPath;
@property(copy) NSString* updatePath;
@property(strong) NSTimer* quitPoll;
- (void)refresh;
- (void)show:(NSString*)text percent:(double)percent;
- (void)install:(id)sender;
@end

@implementation TTT2Importer {
    std::atomic<bool> _cancel;
    std::atomic<pid_t> _shaders;
    BOOL _running;
    BOOL _quitting;
}

- (NSTextField*)label:(NSString*)text frame:(NSRect)frame
{
    NSTextField* field = [NSTextField labelWithString:text];
    field.frame = frame;
    field.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [self.window.contentView addSubview:field];
    return field;
}

- (NSButton*)button:(NSString*)title frame:(NSRect)frame action:(SEL)action
{
    NSButton* button = [NSButton buttonWithTitle:title target:self action:action];
    button.frame = frame;
    [self.window.contentView addSubview:button];
    return button;
}

- (instancetype)init
{
    if (!(self = [super init])) return nil;
    _cancel = false;
    _shaders = -1;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 560, 256)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"Tekken Tag Tournament 2";
    self.window.releasedWhenClosed = NO;
    self.window.delegate = self;
    NSTextField* heading = [self label:@"Game data" frame:NSMakeRect(20, 212, 520, 24)];
    heading.font = [NSFont boldSystemFontOfSize:17];
    [self label:@"The port installs the game from your own Xbox 360 disc image and Title Update 3."
          frame:NSMakeRect(20, 188, 520, 18)];
    [self label:@"Disc image" frame:NSMakeRect(20, 154, 110, 18)];
    self.discField = [self label:@"(none)" frame:NSMakeRect(130, 154, 300, 18)];
    self.discButton = [self button:@"Choose…" frame:NSMakeRect(440, 146, 100, 32) action:@selector(chooseDisc:)];
    [self label:@"Title Update 3" frame:NSMakeRect(20, 122, 110, 18)];
    self.updateField = [self label:@"(none)" frame:NSMakeRect(130, 122, 300, 18)];
    self.updateButton = [self button:@"Choose…" frame:NSMakeRect(440, 114, 100, 32) action:@selector(chooseUpdate:)];
    self.bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 90, 520, 16)];
    self.bar.indeterminate = NO;
    self.bar.minValue = 0;
    self.bar.maxValue = 100;
    [self.window.contentView addSubview:self.bar];
    self.status = [NSTextField wrappingLabelWithString:@""];
    self.status.frame = NSMakeRect(20, 48, 520, 36);
    [self.window.contentView addSubview:self.status];
    self.status.textColor = NSColor.secondaryLabelColor;
    self.quitButton = [self button:@"Quit" frame:NSMakeRect(340, 10, 100, 32) action:@selector(quit:)];
    self.quitButton.keyEquivalent = @"\e";
    self.installButton = [self button:@"Install" frame:NSMakeRect(440, 10, 100, 32) action:@selector(install:)];
    self.installButton.keyEquivalent = @"\r";
    self.installButton.enabled = NO;
    [self.window center];
    // Cmd+Q, Dock > Quit and SIGTERM become SDL_EVENT_QUIT (SDL's app delegate and signal handler).
    self.quitPoll = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer*) {
        if (!SDL_HasEvent(SDL_EVENT_QUIT) || self->_quitting) return;
        self->_quitting = YES;
        [self quit:nil];
    }];
    [[NSRunLoop currentRunLoop] addTimer:self.quitPoll forMode:NSModalPanelRunLoopMode];
    return self;
}

// No type filter: TU files have no extension, and disc images are not always named .iso.
- (NSString*)choose:(BOOL)folders
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = folders;
    panel.allowsMultipleSelection = NO;
    if ([panel runModal] != NSModalResponseOK) return nil;
    return panel.URL.path;
}

- (void)refresh
{
    self.discField.stringValue = self.discPath ? self.discPath.lastPathComponent : @"(none)";
    self.updateField.stringValue = self.updatePath ? self.updatePath.lastPathComponent : @"(none)";
    self.installButton.enabled = !_running && self.discPath && self.updatePath;
}

- (void)chooseDisc:(id)sender
{
    if (NSString* path = [self choose:NO]) self.discPath = path;
    [self refresh];
}

- (void)chooseUpdate:(id)sender
{
    NSString* path = [self choose:YES];
    if (!path) return;
    std::string why;
    const fs::path found = FindTitleUpdate(path.fileSystemRepresentation, why);
    if (found.empty())
    {
        [self show:[NSString stringWithUTF8String:why.c_str()] percent:self.bar.doubleValue];
        return;
    }
    self.updatePath = [NSString stringWithUTF8String:found.c_str()];
    [self show:@"" percent:0];
    [self refresh];
}

- (void)show:(NSString*)text percent:(double)percent
{
    self.status.stringValue = text;
    self.bar.doubleValue = percent;
}

- (BOOL)windowShouldClose:(NSWindow*)sender
{
    [self quit:sender];
    return NO;
}

- (void)quit:(id)sender
{
    if (_running)
    {
        _cancel = true;
        if (pid_t child = _shaders.load(); child > 0) kill(child, SIGTERM);
        [self show:@"Cancelling…" percent:self.bar.doubleValue];
        return;
    }
    [NSApp stopModalWithCode:NSModalResponseCancel];
}

- (void)finish:(BOOL)installed message:(NSString*)message
{
    _running = NO;
    self.quitButton.title = @"Quit";
    self.discButton.enabled = YES;
    self.updateButton.enabled = YES;
    // Cancelled while the shaders were being prepared: the game data stays, and the game prepares
    // them in the background when it next starts.
    if (installed || _quitting)
    {
        [NSApp stopModalWithCode:installed && !_cancel ? NSModalResponseOK : NSModalResponseCancel];
        return;
    }
    [self show:message percent:0];
    [self refresh];
}

- (void)install:(id)sender
{
    _running = YES;
    _cancel = false;
    self.installButton.enabled = NO;
    self.quitButton.title = @"Cancel";
    self.discButton.enabled = NO;
    self.updateButton.enabled = NO;
    const std::string disc = self.discPath.fileSystemRepresentation;
    const std::string update = self.updatePath.fileSystemRepresentation;
    TTT2Importer* importer = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        std::string error;
        const bool installed = [importer installDisc:disc update:update error:error];
        if (installed) [importer prepareShaders];
        NSString* message = [NSString stringWithUTF8String:error.c_str()];
        dispatch_async(dispatch_get_main_queue(), ^{ [importer finish:installed message:message]; });
    });
}

// Worker thread. Progress 0-80 %: the game data.
- (bool)installDisc:(const std::string&)disc update:(const std::string&)update error:(std::string&)error
{
    const fs::path destination = Destination();
    if (destination.empty()) { error = "No home folder."; return false; }
    const int iso = open(disc.c_str(), O_RDONLY | O_CLOEXEC);
    const int tu = open(update.c_str(), O_RDONLY | O_CLOEXEC);
    bool installed = false;
    if (iso < 0 || tu < 0)
        error = "Could not open " + (iso < 0 ? disc : update) + ": " + std::strerror(errno) + ".";
    else
        installed = InstallFromDescriptors(iso, tu, destination, [self](int percent, const std::string& message) {
            NSString* text = [NSString stringWithUTF8String:(message + "…").c_str()];
            dispatch_async(dispatch_get_main_queue(), ^{ [self show:text percent:percent * 0.8]; });
            return !_cancel.load();
        }, error);
    if (iso >= 0) close(iso);
    if (tu >= 0) close(tu);
    return installed;
}

// Worker thread. Progress 80-100 %: the game's pixel shaders into the shader cache, so play never waits
// for one to compile. The game runs itself as `--build-shaders` and prints "[shaders] <done>/<total>".
// Not fatal: a game that starts without them runs the same prebuild in the background.
- (void)prepareShaders
{
    const fs::path exe = Executable();
    int pipeFds[2];
    if (exe.empty() || pipe(pipeFds) != 0) return;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const std::string program = exe.string(), threads = std::to_string(cores > 2 ? cores - 1 : 1);
    char* arguments[] = { const_cast<char*>(program.c_str()), const_cast<char*>("--build-shaders"),
                          const_cast<char*>(""), const_cast<char*>(""), const_cast<char*>(threads.c_str()), nullptr };
    pid_t child = -1;
    const int spawned = posix_spawn(&child, program.c_str(), &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipeFds[1]);
    if (spawned != 0) { close(pipeFds[0]); return; }
    _shaders = child;
    if (_cancel) kill(child, SIGTERM);
    FILE* output = fdopen(pipeFds[0], "r");
    char line[256];
    while (output && std::fgets(line, sizeof(line), output))
    {
        unsigned done = 0, total = 0;
        if (std::sscanf(line, "[shaders] %u/%u", &done, &total) != 2 || total == 0) continue;
        NSString* text = [NSString stringWithFormat:@"Preparing the game's shaders (one time, for smooth play): %u of %u…", done, total];
        const double percent = 80.0 + 20.0 * done / total;
        dispatch_async(dispatch_get_main_queue(), ^{ [self show:text percent:percent]; });
    }
    if (output) std::fclose(output);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    _shaders = -1;
}
@end

namespace ttt2_mac {
bool ImportGameData()
{
    @autoreleasepool {
        TTT2Importer* importer = [[TTT2Importer alloc] init];
        if (@available(macOS 14.0, *)) [NSApp activate];
        else [NSApp activateIgnoringOtherApps:YES];
        [importer.window makeKeyAndOrderFront:nil];
        // Scripted test (as ttt2_setup_probe.exe /silent): both inputs from the environment start at once.
        const char* disc = std::getenv("TTT2_IMPORT_DISC");
        const char* update = std::getenv("TTT2_IMPORT_UPDATE");
        if (disc && *disc && update && *update)
        {
            std::string why;
            const fs::path found = FindTitleUpdate(update, why);
            importer.discPath = [NSString stringWithUTF8String:disc];
            importer.updatePath = found.empty() ? nil : [NSString stringWithUTF8String:found.c_str()];
            [importer refresh];
            if (found.empty()) [importer show:[NSString stringWithUTF8String:why.c_str()] percent:0];
            else [importer performSelector:@selector(install:) withObject:nil afterDelay:0 inModes:@[ NSModalPanelRunLoopMode ]];
        }
        const NSModalResponse response = [NSApp runModalForWindow:importer.window];
        [importer.quitPoll invalidate];
        [importer.window orderOut:nil];
        return response == NSModalResponseOK;
    }
}
} // namespace ttt2_mac
