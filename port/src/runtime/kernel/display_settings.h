// PC display settings chosen in Options -> Display Settings (DISPLAY MODE, RESOLUTION): the
// state, the resolution list and their persistence in %LOCALAPPDATA%\TTT2Recomp\settings.ini.
// The menu natives (menu_ext.cpp) read and write it; the presenter applies it.
#pragma once
#include <cstdint>
#include <vector>

namespace display_settings {

enum Mode : int { Windowed = 0, Borderless = 1, Fullscreen = 2 };

struct Resolution { uint32_t width, height; };

// 16:9 render resolutions offered in the menu (fixed list; index = menu cursor).
const std::vector<Resolution>& Resolutions();

int GetMode();
int GetResolutionIndex();
Resolution GetResolution();
void SetMode(int mode);                 // clamps, persists, notifies the presenter
void SetResolutionIndex(int index);     // clamps, persists, notifies the presenter

// Bumped on every change; the presenter compares it to apply new settings once.
uint32_t Generation();

// Options -> Graphics Settings picture options, applied live (the presenter, the texture samplers):
//   SHARPNESS 0 (OFF) .. 10: contrast adaptive sharpening of the scaled image   [Display] Sharpness
//   ANTI-ALIASING 0 OFF, 1 SMAA 1x (at render resolution), 2 / 3 SSAA 1.5X / 2X: the game drawn at 1.5 / 2 times
//     the RESOLUTION setting (at most 4 times the console's 720p) and scaled down to the window [Display] AntiAliasing
//   TEXTURE FILTERING 0 STANDARD (the game's own filters, as on the console), 1 TRILINEAR (mipmapped textures blend
//     between their levels), 2..5 anisotropic 2x / 4x / 8x / 16x (trilinear too)  [Display] TextureFiltering
//     (the older TextureFilter key: 0 standard, 1..4 anisotropic - carried over, display_settings.cpp)
// All default to 0: the picture as the game draws it. Cheap to read (atomics): the renderer asks
// every frame and every draw.
constexpr int kMaxSharpness = 10, kMaxAntiAliasing = 3, kMaxTextureFilter = 5;   // AA: OFF, SMAA, SSAA 1.5X, SSAA 2X
float SupersampleFactor();              // 1, or SSAA's 1.5 / 2 (a change bumps Generation())
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

// Options -> Display Settings -> BRIGHTNESS: a gamma curve on the presented picture (the presenter's gamma table, after
// the game's own ramp), -5 (darker) .. +5 (brighter); black and white stay. 0 (default): the picture as the game draws
// it. [Display] Brightness=-5..5.
constexpr int kMinBrightness = -5, kMaxBrightness = 5;
int GetBrightness();
void SetBrightness(int level);

// Options -> Display Settings -> LOW LATENCY (low_latency.cpp): when the game's render thread draws a frame. The game
// shows each frame's draw list a frame after it built it (its render thread draws the list during the next frame); ON
// draws it before the frame's own swap in fights - one frame (16.7 ms) less from a press to the screen, with the render
// thread's and the command processor's work on the list then inside the game thread's frame. AUTO: ON while fights
// show the PC has that time to spare, else the game's order. OFF (default since TEST 17; MEASURED: ON took the tester's
// i3-3240 from 59.5 to 52.5 fps in Paris, log 48 vs 50): the game's order. [Display] LowLatencyMode=0|1|2 (TEST 16's
// LowLatency key: only its ON carries over, display_settings.cpp).
enum LowLatency : int { LowLatencyAuto = 0, LowLatencyOn = 1, LowLatencyOff = 2 };
int GetLowLatency();
void SetLowLatency(int mode);

// Options -> Graphics Settings -> SHADOWS: the shadows fighters and stage objects cast (the game's shadow map,
// gpu_hw_d3d11.h). OFF skips drawing them - the picture only; the game runs the same. [Display] Shadows=1|0.
bool GetShadows();
void SetShadows(bool on);

// Options -> Graphics Settings -> DEPTH OF FIELD: the game's depth of field (post_dof.cpp), the blur of what lies
// in front of / behind the focus (the stage behind the fighters, cinematic shots). OFF draws the scene sharp and
// skips its passes - the picture only; the game runs the same. [Display] DepthOfField=1|0.
bool GetDepthOfField();
void SetDepthOfField(bool on);

// Options -> Graphics Settings -> BLOOM: the game's glare (post_dof.cpp), the glow around bright light. OFF skips its
// passes - the picture only. [Display] Bloom=1|0.
bool GetBloom();
void SetBloom(bool on);

// Options -> Graphics Settings -> LIGHT SHAFTS: the game's sun shafts (post_dof.cpp), rays from the sun or a light on
// the stages that have them. OFF skips their pass - the picture only. [Display] LightShafts=1|0.
bool GetLightShafts();
void SetLightShafts(bool on);

// Options -> Graphics Settings -> WEATHER EFFECTS: the stages' particle emitters, falling rain among them (stage_fx.cpp:
// the game's StageParticle emitter jobs). OFF skips those jobs - the picture only. [Display] Weather=1|0.
bool GetWeather();
void SetWeather(bool on);

// Options -> Display Settings -> FPS COUNTER: frames shown per second, drawn over the finished picture by the presenter
// (gpu_hw_d3d11.h DrawFpsCounter, in the game's money box look: gpu_fps_overlay.h). 0 OFF (default), 1..4 its corner: top
// left, top right, bottom left, bottom right. [Display] FpsCounter=0..4.
enum FpsCorner : int { FpsOff = 0, FpsTopLeft = 1, FpsTopRight = 2, FpsBottomLeft = 3, FpsBottomRight = 4 };
int GetFpsCounter();
void SetFpsCounter(int corner);

// Options -> Display Settings -> QUICK START: the game starts on the Press Start screen, without the logos and the intro
// movie (the game's own way to the title, online_2v2_select.cpp's mode factory). [Game] QuickStart=1|0, default 0.
bool GetQuickStart();
void SetQuickStart(bool on);

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

// The rematch menu (the online room's REMATCH MENU row, the room host's; online_rematch.cpp): after a Player Match or
// PAIR PLAY match its players choose REMATCH / STAGE SELECT / CHARACTER SELECT / RETURN TO ROOM. [Online] RematchMenu=1|0
// (default 1).
bool GetRematchMenu();
void SetRematchMenu(bool on);           // persisted

} // namespace display_settings
