// PC display settings chosen in Options -> Display Settings (DISPLAY MODE, RESOLUTION): the
// state, the resolution list and their persistence in %LOCALAPPDATA%\TTT2Recomp\settings.ini.
// The menu natives (menu_ext.cpp) read and write it; the presenter applies it.
#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace display_settings {

enum Mode : int { Windowed = 0, Borderless = 1, Fullscreen = 2 };

struct Resolution { uint32_t width, height; };

// 16:9 render resolutions offered in the menu (fixed list; index = menu cursor).
const std::vector<Resolution>& Resolutions();

std::wstring GetMonitorDevice();
void SetMonitorDevice(const std::wstring& device);
int GetMode();
int GetResolutionIndex();
Resolution GetResolution();
void SetMode(int mode);                 // clamps, persists, notifies the presenter
void SetResolutionIndex(int index);     // clamps, persists, notifies the presenter

// Bumped on every change; the presenter compares it to apply new settings once.
uint32_t Generation();

// Options -> Display Settings image options, applied live (the presenter, the texture samplers):
//   SHARPNESS 0 (OFF) .. 10: contrast adaptive sharpening of the scaled image   [Display] Sharpness
//   ANTI-ALIASING 0 OFF, 1 FXAA, 2 SMAA 1x (at render resolution)                          [Display] AntiAliasing
//   TEXTURE FILTERING 0 STANDARD (the game's own filters, as on the console),
//     1..4 anisotropic 2x / 4x / 8x / 16x for mipmapped textures             [Display] TextureFilter
// All default to 0: the picture as the game draws it. Cheap to read (atomics): the renderer asks
// every frame and every draw.
constexpr int kMaxSharpness = 10, kMaxAntiAliasing = 2, kMaxTextureFilter = 4;
// SDR calibration percentages, neutral 100. GPU selection applies next launch (-1 automatic).
int GetMotionBlur();void SetMotionBlur(int mode);
int GetRefreshRate();void SetRefreshRate(int hz);
int GetTextureMipBias(); int GetRenderScale(); int GetPerformanceStats();
void SetTextureMipBias(int value); void SetRenderScale(int percent); void SetPerformanceStats(int mode);
int GetBrightness(); int GetGamma(); int GetContrast(); int GetGpuAdapter();
void SetBrightness(int value); void SetGamma(int value); void SetContrast(int value); void SetGpuAdapter(int index);
// Transient in-game reference panels, never persisted or enabled at startup.
bool GetCalibrationPattern();
void SetCalibrationPattern(bool enabled);
int GetSharpness();
int GetAntiAliasing();
int GetTextureFilter();
void SetSharpness(int level);
void SetAntiAliasing(int mode);
void SetTextureFilter(int mode);

// Options -> Display Settings -> VSYNC: when a finished frame reaches the screen (the presenter). The game
// runs at its own 60 frames a second either way. ON (default): at the display's next refresh, never torn.
// OFF: at once - less input lag, the picture may tear (none on a variable refresh rate display).
// [Display] VSync=1|0.
bool GetVSync();
void SetVSync(bool on);

// Options -> Display Settings -> SHADOWS: the shadows fighters and stage objects cast (the game's shadow map,
// gpu_hw_d3d11.h). OFF skips drawing them - the picture only; the game runs the same. [Display] Shadows=1|0.
bool GetShadows();
void SetShadows(bool on);

// Options -> Game Options -> INPUT DELAY: the game's own 2-frame input delay in offline modes,
// ON = the original, OFF = removed (input_delay.cpp). [Game] InputDelay=1|0, default 0 (OFF).
bool GetInputDelayOn();
void SetInputDelayOn(bool on);

// Options -> Game Options -> ONLINE NETCODE (rollback sessions, rollback_session.cpp), like Tekken 8's
// rollback setting: how much input delay the host picks from the ping (the host's choice applies to
// both players) and how many frames a session may predict before it waits.
//   STANDARD (the game's own delay for the ping: 3 frames below 99 ms) / PRIORITIZE RESPONSE (one frame
//   less, more rollback) / PRIORITIZE GRAPHICS (one frame more, fewer rollbacks), up to 8 frames predicted;
//   CUSTOM: CUSTOM INPUT DELAY (1..6 frames, fixed) and CUSTOM ROLLBACK (1..8 frames).
// [Online] Netcode=0..3, CustomDelay=1..6 (default 1), CustomRollback=1..8 (default 8).
// No 0: NACE's in-band delay change takes a request for 0 and never applies it (MEASURED 2026-10-19,
// the battle stayed at 4); 1 applies.
enum Netcode : int { NetStandard = 0, NetResponse = 1, NetGraphics = 2, NetCustom = 3 };
constexpr int kMinDelay = 1, kMaxCustomDelay = 6, kMaxRollback = 8;
int GetNetcode();
int GetCustomDelay();
int GetCustomRollback();
void SetNetcode(int netcode);           // clamped, persisted
void SetCustomDelay(int frames);
void SetCustomRollback(int frames);

} // namespace display_settings
