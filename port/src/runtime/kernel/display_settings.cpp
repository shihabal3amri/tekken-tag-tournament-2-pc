// PC display settings (see display_settings.h). Stored as
//   [Display]
//   Mode=0|1|2            (windowed, borderless, fullscreen)
//   Resolution=1920x1080
//   Sharpness=0..10       (0 = off)
//   AntiAliasing=0|1|2    (off, FXAA, SMAA 1x high)
//   TextureFilter=0..4    (the game's own, anisotropic 2x / 4x / 8x / 16x)
//   VSync=1|0             (frames at the display's refresh / at once)
//   Shadows=1|0           (cast shadows drawn / not)
//   [Game]
//   InputDelay=0|1        (the game's 2-frame offline input delay: removed / original)
//   [Online]
//   Netcode=0..3          (STANDARD, PRIORITIZE RESPONSE, PRIORITIZE GRAPHICS, CUSTOM)
//   CustomDelay=1..6      (CUSTOM's input delay, frames)
//   CustomRollback=1..8   (CUSTOM's prediction limit, frames)
// in the host settings file (host_settings.h), read once at first use.
#include "display_settings.h"
#include "host_settings.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>

namespace display_settings {
namespace {

std::mutex g_mutex;
bool g_loaded = false;
int g_mode = Windowed;
std::wstring g_monitorDevice;
int g_resolution = 0;
bool g_inputDelay = false;
int g_netcode = NetStandard;
int g_customDelay = 1;
int g_customRollback = kMaxRollback;
std::atomic<int> g_sharpness{ 0 }, g_antiAliasing{ 0 }, g_textureFilter{ 0 }, g_vsync{ 1 }, g_shadows{ 1 };
std::atomic<int> g_motionBlur{-1};
std::atomic<int> g_refreshRate{0};
std::atomic<int> g_mipBias{0},g_renderScale{0},g_performanceStats{0};
std::atomic<int> g_brightness{100},g_gamma{100},g_contrast{100},g_gpuAdapter{-1};
std::atomic<bool> g_loadedFlag{ false };
std::atomic<bool> g_calibrationPattern{false};
std::atomic<uint32_t> g_generation{ 0 };

void LoadLocked()
{
    if (g_loaded) return;
    g_loaded = true;
    const auto monitor=host_settings::Read("Display","MonitorDevice","");
    g_monitorDevice.assign(monitor.begin(),monitor.end());
    g_mode = std::clamp<int>(host_settings::ReadInt("Display", "Mode", Windowed), Windowed, Fullscreen);
    const auto res=host_settings::Read("Display","Resolution","1280x720");
    unsigned w = 0, h = 0;
    g_resolution = 2;                                   // 1280x720 unless the file names another
    if (std::sscanf(res.c_str(), "%ux%u", &w, &h) == 2)
    {
        const auto& list = Resolutions();
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].width == w && list[i].height == h) g_resolution = int(i);
    }
    g_inputDelay = host_settings::ReadInt("Game", "InputDelay", 0) != 0;
    g_netcode = std::clamp<int>(host_settings::ReadInt("Online", "Netcode", NetStandard), NetStandard, NetCustom);
    g_customDelay = std::clamp<int>(host_settings::ReadInt("Online", "CustomDelay", 1), kMinDelay, kMaxCustomDelay);
    g_customRollback = std::clamp<int>(host_settings::ReadInt("Online", "CustomRollback", kMaxRollback), 1, kMaxRollback);
    g_sharpness = std::clamp<int>(host_settings::ReadInt("Display", "Sharpness", 0), 0, kMaxSharpness);
    g_antiAliasing = std::clamp<int>(host_settings::ReadInt("Display", "AntiAliasing", 0), 0, kMaxAntiAliasing);
    g_textureFilter = std::clamp<int>(host_settings::ReadInt("Display", "TextureFilter", 0), 0, kMaxTextureFilter);
    g_vsync = host_settings::ReadInt("Display", "VSync", 1) != 0 ? 1 : 0;
    g_shadows = host_settings::ReadInt("Display", "Shadows", 1) != 0 ? 1 : 0;
    g_motionBlur=std::clamp<int>(host_settings::ReadInt("Display","MotionBlur",-1),-1,1);
    g_refreshRate=std::clamp<int>(host_settings::ReadInt("Display","RefreshRate",0),0,360);
    g_mipBias=std::clamp<int>(host_settings::ReadInt("Display","TextureMipBias",0),0,3);
    const int scale=host_settings::ReadInt("Display","RenderScale",0);
    g_renderScale=scale==0?0:std::clamp(scale,50,200);
    g_performanceStats=std::clamp<int>(host_settings::ReadInt("Display","PerformanceStats",0),0,2);
    g_brightness=std::clamp<int>(host_settings::ReadInt("Display","Brightness",100),50,150);
    g_gamma=std::clamp<int>(host_settings::ReadInt("Display","Gamma",100),50,250);
    g_contrast=std::clamp<int>(host_settings::ReadInt("Display","Contrast",100),50,150);
    g_gpuAdapter=std::clamp<int>(host_settings::ReadInt("Display","GpuAdapter",-1),-1,31);
    g_loadedFlag.store(true, std::memory_order_release);
    std::fprintf(stderr, "[display] image: sharpness %d, anti-aliasing %d, texture filtering %d, vsync %s, shadows %s\n",
                 g_sharpness.load(), g_antiAliasing.load(), g_textureFilter.load(), g_vsync.load() ? "on" : "off",
                 g_shadows.load() ? "on" : "off");
    std::fprintf(stderr, "[display] settings: mode=%d resolution=%ux%u input delay %s, online netcode %d (custom: delay %d, rollback %d)\n",
                 g_mode, Resolutions()[g_resolution].width, Resolutions()[g_resolution].height, g_inputDelay ? "on" : "off",
                 g_netcode, g_customDelay, g_customRollback);
}

void SaveLocked()
{
    const std::string monitor(g_monitorDevice.begin(),g_monitorDevice.end());
    host_settings::Write("Display","MonitorDevice",monitor.c_str());
    char value[32];
    const std::pair<const char*,int> extended[]={{"MotionBlur",g_motionBlur.load()},{"RefreshRate",g_refreshRate.load()},{"TextureMipBias",g_mipBias.load()},{"RenderScale",g_renderScale.load()},{"PerformanceStats",g_performanceStats.load()},{"Brightness",g_brightness.load()},{"Gamma",g_gamma.load()},{"Contrast",g_contrast.load()},{"GpuAdapter",g_gpuAdapter.load()}};
    for(const auto& [key,v]:extended){std::snprintf(value,sizeof(value),"%d",v);host_settings::Write("Display",key,value);}

    std::snprintf(value, sizeof(value), "%d", g_mode);
    host_settings::Write("Display", "Mode", value);
    const Resolution r = Resolutions()[g_resolution];
    std::snprintf(value, sizeof(value), "%ux%u", r.width, r.height);
    host_settings::Write("Display", "Resolution", value);
    std::snprintf(value, sizeof(value), "%d", g_sharpness.load());
    host_settings::Write("Display", "Sharpness", value);
    std::snprintf(value, sizeof(value), "%d", g_antiAliasing.load());
    host_settings::Write("Display", "AntiAliasing", value);
    std::snprintf(value, sizeof(value), "%d", g_textureFilter.load());
    host_settings::Write("Display", "TextureFilter", value);
    host_settings::Write("Display", "VSync", g_vsync.load() ? "1" : "0");
    host_settings::Write("Display", "Shadows", g_shadows.load() ? "1" : "0");
    host_settings::Write("Game", "InputDelay", g_inputDelay ? "1" : "0");
    std::snprintf(value, sizeof(value), "%d", g_netcode);
    host_settings::Write("Online", "Netcode", value);
    std::snprintf(value, sizeof(value), "%d", g_customDelay);
    host_settings::Write("Online", "CustomDelay", value);
    std::snprintf(value, sizeof(value), "%d", g_customRollback);
    host_settings::Write("Online", "CustomRollback", value);
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

std::wstring GetMonitorDevice(){std::lock_guard<std::mutex> l(g_mutex);LoadLocked();return g_monitorDevice;}
void SetMonitorDevice(const std::wstring& device){
    std::lock_guard<std::mutex> l(g_mutex);LoadLocked();
    if(device==g_monitorDevice)return;g_monitorDevice=device;SaveLocked();g_generation.fetch_add(1);
}
int GetMotionBlur(){return GetAtomic(g_motionBlur);}
void SetMotionBlur(int mode){SetAtomic(g_motionBlur,mode,-1,1,"motion blur");}
int GetRefreshRate(){return GetAtomic(g_refreshRate);}
void SetRefreshRate(int hz){const int previous=GetRefreshRate();SetAtomic(g_refreshRate,hz,0,360,"exclusive refresh rate");if(previous!=GetRefreshRate())g_generation.fetch_add(1);}
int GetTextureMipBias(){return GetAtomic(g_mipBias);}
int GetRenderScale(){return GetAtomic(g_renderScale);}
int GetPerformanceStats(){return GetAtomic(g_performanceStats);}
void SetTextureMipBias(int v){SetAtomic(g_mipBias,v,0,3,"texture mip bias");}
void SetRenderScale(int v){const int previous=GetRenderScale();SetAtomic(g_renderScale,v==0?0:std::clamp(v,50,200),0,200,"render scale");if(previous!=GetRenderScale())g_generation.fetch_add(1);}
void SetPerformanceStats(int v){SetAtomic(g_performanceStats,v,0,2,"performance statistics");}
bool GetCalibrationPattern(){return g_calibrationPattern.load();}
void SetCalibrationPattern(bool enabled){g_calibrationPattern.store(enabled);}
int GetBrightness(){return GetAtomic(g_brightness);}
int GetGamma(){return GetAtomic(g_gamma);}
int GetContrast(){return GetAtomic(g_contrast);}
int GetGpuAdapter(){return GetAtomic(g_gpuAdapter);}
void SetBrightness(int v){SetAtomic(g_brightness,v,50,150,"SDR brightness");}
void SetGamma(int v){SetAtomic(g_gamma,v,50,250,"SDR gamma");}
void SetContrast(int v){SetAtomic(g_contrast,v,50,150,"SDR contrast");}
void SetGpuAdapter(int v){SetAtomic(g_gpuAdapter,v,-1,31,"GPU adapter (next launch)");}
int GetSharpness() { return GetAtomic(g_sharpness); }
int GetAntiAliasing() { return GetAtomic(g_antiAliasing); }
int GetTextureFilter() { return GetAtomic(g_textureFilter); }
void SetSharpness(int level) { SetAtomic(g_sharpness, level, 0, kMaxSharpness, "sharpness"); }
void SetAntiAliasing(int mode) { SetAtomic(g_antiAliasing, mode, 0, kMaxAntiAliasing, "anti-aliasing"); }
void SetTextureFilter(int mode) { SetAtomic(g_textureFilter, mode, 0, kMaxTextureFilter, "texture filtering"); }
bool GetVSync() { return GetAtomic(g_vsync) != 0; }
void SetVSync(bool on) { SetAtomic(g_vsync, on ? 1 : 0, 0, 1, "vsync"); }
bool GetShadows() { return GetAtomic(g_shadows) != 0; }
void SetShadows(bool on) { SetAtomic(g_shadows, on ? 1 : 0, 0, 1, "shadows"); }

} // namespace display_settings
