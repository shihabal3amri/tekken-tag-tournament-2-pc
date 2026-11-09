// PC display settings (see display_settings.h). Stored as
//   [Display]
//   Mode=0|1|2            (windowed, borderless, fullscreen)
//   Resolution=1920x1080
//   Sharpness=0..10       (0 = off)
//   AntiAliasing=0-3      (off, SMAA, SSAA 1.5X, SSAA 2X)
//   TextureFiltering=0..5 (the game's own, trilinear, anisotropic 2x / 4x / 8x / 16x)
//   VSync=1|0             (frames at the display's refresh / at once)
//   Brightness=-5..5      (the presented picture's gamma curve; 0 = as the game draws it)
//   LowLatencyMode=0|1|2  (AUTO, ON, OFF: in fights each frame drawn before its own swap / the game's order; OFF default)
//   Shadows=1|0           (cast shadows drawn / not)
//   DepthOfField=1|0      (the game's depth of field drawn / not)
//   Bloom=1|0             (the game's glare drawn / not)
//   LightShafts=1|0       (the game's sun shafts drawn / not)
//   Weather=1|0           (the stages' particle effects, falling rain among them, drawn / not)
// (DynamicResolution=0|1, up to TEST 16: the row is gone - the full width always, post_dof.cpp; a save removes the key.)
// (LowLatency=0|1|2, TEST 16: its AUTO default went into every file it saved, so only its ON carries over, and only
//  while there is no LowLatencyMode; a save removes the key.)
//   [Game]
//   InputDelay=0|1        (the game's 2-frame offline input delay: removed / original)
//   [Online]
//   Netcode=0..3          (STANDARD, PRIORITIZE RESPONSE, PRIORITIZE GRAPHICS, CUSTOM)
//   CustomDelay=1..6      (CUSTOM's input delay, frames)
//   CustomRollback=1..8   (CUSTOM's prediction limit, frames)
//   RematchMenu=1|0       (the rooms' rematch menu after a match: on / off)
// in %LOCALAPPDATA%\TTT2Recomp\settings.ini, read once at first use.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "display_settings.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace display_settings {
namespace {

std::mutex g_mutex;
bool g_loaded = false;
int g_mode = Windowed;
int g_resolution = 0;
bool g_inputDelay = false;
int g_netcode = NetStandard;
int g_customDelay = 1;
int g_customRollback = kMaxRollback;
bool g_rematchMenu = true;
std::atomic<int> g_sharpness{ 0 }, g_antiAliasing{ 0 }, g_textureFilter{ 0 }, g_vsync{ 1 }, g_shadows{ 1 }, g_depthOfField{ 1 };
std::atomic<int> g_bloom{ 1 }, g_lightShafts{ 1 }, g_weather{ 1 }, g_brightness{ 0 }, g_fpsCounter{ 0 }, g_quickStart{ 0 };
std::atomic<int> g_lowLatency{ LowLatencyOff };
std::atomic<bool> g_loadedFlag{ false };
std::atomic<uint32_t> g_generation{ 0 };

std::string IniPath()
{
    if (const char* e = std::getenv("TTT2_SETTINGS_INI"); e && *e) return e;   // test runs
    char dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir));
    std::string path = (n && n < sizeof(dir)) ? std::string(dir) : std::string(".");
    path += "\\TTT2Recomp";
    CreateDirectoryA(path.c_str(), nullptr);
    return path + "\\settings.ini";
}

// TEST 17: whether the file was there when the game started, and since when. Tester logs began several times with
// every setting at its default and a new save (logs 46 / 48 / 50, the shader cache beside them intact): a missing file
// and one holding the defaults looked the same in the log.
std::string FileNote(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info)) return "no such file - the defaults";
    auto when = [](const FILETIME& ft) {
        SYSTEMTIME utc{}, local{};
        char text[32] = "?";
        if (FileTimeToSystemTime(&ft, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
            std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
        return std::string(text);
    };
    char note[96];
    std::snprintf(note, sizeof(note), "%lu bytes, created %s, written %s", info.nFileSizeLow, when(info.ftCreationTime).c_str(),
                  when(info.ftLastWriteTime).c_str());
    return note;
}

void LoadLocked()
{
    if (g_loaded) return;
    g_loaded = true;
    const std::string ini = IniPath();
    const std::string fileNote = FileNote(ini);
    g_mode = std::clamp<int>(GetPrivateProfileIntA("Display", "Mode", Windowed, ini.c_str()), Windowed, Fullscreen);
    char res[32] = {};
    GetPrivateProfileStringA("Display", "Resolution", "1280x720", res, sizeof(res), ini.c_str());
    unsigned w = 0, h = 0;
    g_resolution = 2;                                   // 1280x720 unless the file names another
    if (std::sscanf(res, "%ux%u", &w, &h) == 2)
    {
        const auto& list = Resolutions();
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].width == w && list[i].height == h) g_resolution = int(i);
    }
    g_inputDelay = GetPrivateProfileIntA("Game", "InputDelay", 0, ini.c_str()) != 0;
    g_netcode = std::clamp<int>(GetPrivateProfileIntA("Online", "Netcode", NetStandard, ini.c_str()), NetStandard, NetCustom);
    g_customDelay = std::clamp<int>(GetPrivateProfileIntA("Online", "CustomDelay", 1, ini.c_str()), kMinDelay, kMaxCustomDelay);
    g_customRollback = std::clamp<int>(GetPrivateProfileIntA("Online", "CustomRollback", kMaxRollback, ini.c_str()), 1, kMaxRollback);
    g_rematchMenu = GetPrivateProfileIntA("Online", "RematchMenu", 1, ini.c_str()) != 0;
    g_sharpness = std::clamp<int>(GetPrivateProfileIntA("Display", "Sharpness", 0, ini.c_str()), 0, kMaxSharpness);
    g_antiAliasing = std::clamp<int>(GetPrivateProfileIntA("Display", "AntiAliasing", 0, ini.c_str()), 0, kMaxAntiAliasing);
    // TextureFilter (before TRILINEAR came, TEST 18): 0 standard, 1..4 anisotropic, now 2..5
    const int textureFilterOld = std::clamp<int>(GetPrivateProfileIntA("Display", "TextureFilter", 0, ini.c_str()), 0, 4);
    g_textureFilter = std::clamp<int>(GetPrivateProfileIntA("Display", "TextureFiltering", textureFilterOld ? textureFilterOld + 1 : 0, ini.c_str()), 0, kMaxTextureFilter);
    g_vsync = GetPrivateProfileIntA("Display", "VSync", 1, ini.c_str()) != 0 ? 1 : 0;
    char bright[16] = {};                               // negative: GetPrivateProfileInt documents 0 for those
    GetPrivateProfileStringA("Display", "Brightness", "0", bright, sizeof(bright), ini.c_str());
    g_brightness = std::clamp<int>(int(std::strtol(bright, nullptr, 10)), kMinBrightness, kMaxBrightness);
    const int lowLatencyTest16 = GetPrivateProfileIntA("Display", "LowLatency", LowLatencyOff, ini.c_str()) == LowLatencyOn ? LowLatencyOn : LowLatencyOff;
    g_lowLatency = std::clamp<int>(GetPrivateProfileIntA("Display", "LowLatencyMode", lowLatencyTest16, ini.c_str()), LowLatencyAuto, LowLatencyOff);
    g_shadows = GetPrivateProfileIntA("Display", "Shadows", 1, ini.c_str()) != 0 ? 1 : 0;
    g_depthOfField = GetPrivateProfileIntA("Display", "DepthOfField", 1, ini.c_str()) != 0 ? 1 : 0;
    g_bloom = GetPrivateProfileIntA("Display", "Bloom", 1, ini.c_str()) != 0 ? 1 : 0;
    g_lightShafts = GetPrivateProfileIntA("Display", "LightShafts", 1, ini.c_str()) != 0 ? 1 : 0;
    g_weather = GetPrivateProfileIntA("Display", "Weather", 1, ini.c_str()) != 0 ? 1 : 0;
    g_fpsCounter = std::clamp<int>(GetPrivateProfileIntA("Display", "FpsCounter", FpsOff, ini.c_str()), FpsOff, FpsBottomRight);
    g_quickStart = GetPrivateProfileIntA("Game", "QuickStart", 0, ini.c_str()) != 0 ? 1 : 0;
    g_loadedFlag.store(true, std::memory_order_release);
    std::fprintf(stderr, "[display] image: sharpness %d, anti-aliasing %d, texture filtering %d, vsync %s, shadows %s, depth of field %s, "
                         "bloom %s, light shafts %s, weather %s, brightness %d, low latency %s\n",
                 g_sharpness.load(), g_antiAliasing.load(), g_textureFilter.load(), g_vsync.load() ? "on" : "off",
                 g_shadows.load() ? "on" : "off", g_depthOfField.load() ? "on" : "off", g_bloom.load() ? "on" : "off",
                 g_lightShafts.load() ? "on" : "off", g_weather.load() ? "on" : "off",
                 g_brightness.load(), g_lowLatency.load() == LowLatencyOn ? "on" : g_lowLatency.load() == LowLatencyOff ? "off" : "auto");
    std::fprintf(stderr, "[display] settings: mode=%d resolution=%ux%u input delay %s, online netcode %d (custom: delay %d, rollback %d) (%s: %s)\n",
                 g_mode, Resolutions()[g_resolution].width, Resolutions()[g_resolution].height, g_inputDelay ? "on" : "off",
                 g_netcode, g_customDelay, g_customRollback, ini.c_str(), fileNote.c_str());
}

void SaveLocked()
{
    const std::string ini = IniPath();
    char value[32];
    std::snprintf(value, sizeof(value), "%d", g_mode);
    WritePrivateProfileStringA("Display", "Mode", value, ini.c_str());
    const Resolution r = Resolutions()[g_resolution];
    std::snprintf(value, sizeof(value), "%ux%u", r.width, r.height);
    WritePrivateProfileStringA("Display", "Resolution", value, ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_sharpness.load());
    WritePrivateProfileStringA("Display", "Sharpness", value, ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_antiAliasing.load());
    WritePrivateProfileStringA("Display", "AntiAliasing", value, ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_textureFilter.load());
    WritePrivateProfileStringA("Display", "TextureFiltering", value, ini.c_str());
    WritePrivateProfileStringA("Display", "TextureFilter", nullptr, ini.c_str());         // the older key (above): deleted
    WritePrivateProfileStringA("Display", "VSync", g_vsync.load() ? "1" : "0", ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_brightness.load());
    WritePrivateProfileStringA("Display", "Brightness", value, ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_lowLatency.load());
    WritePrivateProfileStringA("Display", "LowLatencyMode", value, ini.c_str());
    WritePrivateProfileStringA("Display", "LowLatency", nullptr, ini.c_str());            // TEST 16's key (above): deleted
    WritePrivateProfileStringA("Display", "Shadows", g_shadows.load() ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Display", "DepthOfField", g_depthOfField.load() ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Display", "Bloom", g_bloom.load() ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Display", "LightShafts", g_lightShafts.load() ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Display", "DynamicResolution", nullptr, ini.c_str());     // gone (TEST 17): the key deleted
    WritePrivateProfileStringA("Display", "Weather", g_weather.load() ? "1" : "0", ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_fpsCounter.load());
    WritePrivateProfileStringA("Display", "FpsCounter", value, ini.c_str());
    WritePrivateProfileStringA("Game", "QuickStart", g_quickStart.load() ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Game", "InputDelay", g_inputDelay ? "1" : "0", ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_netcode);
    WritePrivateProfileStringA("Online", "Netcode", value, ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_customDelay);
    WritePrivateProfileStringA("Online", "CustomDelay", value, ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_customRollback);
    WritePrivateProfileStringA("Online", "CustomRollback", value, ini.c_str());
    WritePrivateProfileStringA("Online", "RematchMenu", g_rematchMenu ? "1" : "0", ini.c_str());
}

// One clamped int setting: stored and logged when it changes.
void SetInt(int& slot, int v, int lo, int hi, const char* what)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    v = std::clamp(v, lo, hi);
    if (v == slot) return;
    slot = v;
    SaveLocked();
    std::fprintf(stderr, "[display] %s -> %d\n", what, v);
}

// The same for the image options (atomics: the renderer reads them without the lock).
void SetAtomic(std::atomic<int>& slot, int v, int lo, int hi, const char* what)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    v = std::clamp(v, lo, hi);
    if (v == slot.load()) return;
    slot.store(v);
    SaveLocked();
    std::fprintf(stderr, "[display] %s -> %d\n", what, v);
}
int GetAtomic(const std::atomic<int>& slot)
{
    if (!g_loadedFlag.load(std::memory_order_acquire)) { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); }
    return slot.load(std::memory_order_relaxed);
}

} // namespace

const std::vector<Resolution>& Resolutions()
{
    // 640x360 and 960x540 render below the console's 720p (half / three quarters of it): for weak
    // GPUs (integrated graphics, GTX 1050 class at high window sizes). The setting is stored as WxH,
    // so adding entries keeps everyone's choice.
    static const std::vector<Resolution> list = {
        { 640, 360 }, { 960, 540 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
    return list;
}

int GetMode() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_mode; }
int GetResolutionIndex() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_resolution; }
Resolution GetResolution() { return Resolutions()[GetResolutionIndex()]; }
uint32_t Generation() { return g_generation.load(); }

void SetMode(int mode)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    mode = std::clamp(mode, int(Windowed), int(Fullscreen));
    if (mode == g_mode) return;
    g_mode = mode;
    SaveLocked();
    g_generation.fetch_add(1);
    std::fprintf(stderr, "[display] mode -> %d\n", mode);
}

void SetResolutionIndex(int index)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    index = std::clamp(index, 0, int(Resolutions().size()) - 1);
    if (index == g_resolution) return;
    g_resolution = index;
    SaveLocked();
    g_generation.fetch_add(1);
    std::fprintf(stderr, "[display] resolution -> %ux%u\n", Resolutions()[index].width, Resolutions()[index].height);
}

bool GetInputDelayOn() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_inputDelay; }

void SetInputDelayOn(bool on)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    if (on == g_inputDelay) return;
    g_inputDelay = on;
    SaveLocked();
    std::fprintf(stderr, "[display] input delay -> %s\n", on ? "on (the original 2 frames offline)" : "off");
}

int GetNetcode() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_netcode; }
int GetCustomDelay() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_customDelay; }
int GetCustomRollback() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_customRollback; }
void SetNetcode(int netcode) { SetInt(g_netcode, netcode, NetStandard, NetCustom, "online netcode"); }
void SetCustomDelay(int frames) { SetInt(g_customDelay, frames, kMinDelay, kMaxCustomDelay, "custom input delay"); }
void SetCustomRollback(int frames) { SetInt(g_customRollback, frames, 1, kMaxRollback, "custom rollback"); }
bool GetRematchMenu() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_rematchMenu; }
void SetRematchMenu(bool on)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    if (on == g_rematchMenu) return;
    g_rematchMenu = on;
    SaveLocked();
    std::fprintf(stderr, "[display] rematch menu -> %s\n", on ? "on" : "off");
}

int GetSharpness() { return GetAtomic(g_sharpness); }
int GetAntiAliasing() { return GetAtomic(g_antiAliasing); }
int GetTextureFilter() { return GetAtomic(g_textureFilter); }
void SetSharpness(int level) { SetAtomic(g_sharpness, level, 0, kMaxSharpness, "sharpness"); }
void SetAntiAliasing(int mode)
{
    const int before = GetAtomic(g_antiAliasing);
    SetAtomic(g_antiAliasing, mode, 0, kMaxAntiAliasing, "anti-aliasing");
    if (GetAtomic(g_antiAliasing) != before) g_generation.fetch_add(1);   // SSAA changes the render scale
}
float SupersampleFactor()
{
    const int aa = GetAtomic(g_antiAliasing);
    return aa == 3 ? 2.0f : aa == 2 ? 1.5f : 1.0f;
}
void SetTextureFilter(int mode) { SetAtomic(g_textureFilter, mode, 0, kMaxTextureFilter, "texture filtering"); }
bool GetVSync() { return GetAtomic(g_vsync) != 0; }
void SetVSync(bool on) { SetAtomic(g_vsync, on ? 1 : 0, 0, 1, "vsync"); }
int GetBrightness() { return GetAtomic(g_brightness); }
void SetBrightness(int level) { SetAtomic(g_brightness, level, kMinBrightness, kMaxBrightness, "brightness"); }
int GetLowLatency() { return GetAtomic(g_lowLatency); }
void SetLowLatency(int mode) { SetAtomic(g_lowLatency, mode, LowLatencyAuto, LowLatencyOff, "low latency (0 auto, 1 on, 2 off)"); }
bool GetShadows() { return GetAtomic(g_shadows) != 0; }
void SetShadows(bool on) { SetAtomic(g_shadows, on ? 1 : 0, 0, 1, "shadows"); }
bool GetDepthOfField() { return GetAtomic(g_depthOfField) != 0; }
void SetDepthOfField(bool on) { SetAtomic(g_depthOfField, on ? 1 : 0, 0, 1, "depth of field"); }
bool GetBloom() { return GetAtomic(g_bloom) != 0; }
void SetBloom(bool on) { SetAtomic(g_bloom, on ? 1 : 0, 0, 1, "bloom"); }
bool GetLightShafts() { return GetAtomic(g_lightShafts) != 0; }
void SetLightShafts(bool on) { SetAtomic(g_lightShafts, on ? 1 : 0, 0, 1, "light shafts"); }
bool GetWeather() { return GetAtomic(g_weather) != 0; }
void SetWeather(bool on) { SetAtomic(g_weather, on ? 1 : 0, 0, 1, "weather"); }
int GetFpsCounter() { return GetAtomic(g_fpsCounter); }
void SetFpsCounter(int corner) { SetAtomic(g_fpsCounter, corner, FpsOff, FpsBottomRight, "fps counter"); }
bool GetQuickStart() { return GetAtomic(g_quickStart) != 0; }
void SetQuickStart(bool on) { SetAtomic(g_quickStart, on ? 1 : 0, 0, 1, "quick start"); }

} // namespace display_settings
