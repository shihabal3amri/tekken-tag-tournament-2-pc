// Standalone native settings launcher. Uses the same settings implementation as the game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <cstring>
#include <objidl.h>
#include <gdiplus.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "preview_environment.h"
#include "original_launch.h"
#include "update_version.h"
#include "../runtime/kernel/display_settings.h"
#include "../runtime/kernel/display_devices.h"
#include "../runtime/kernel/graphics_registry.h"

namespace {
namespace settings = display_settings;
enum : int { Browse = 100, Save, Play, Mode, Resolution, AA, Sharpness, Filter, VSync, Shadows, Delay, GamePath, InputLayout, SOCD, StickMode, Deadzone, Tabs, Preset, Defaults, GraphicsTab, InputTab, CheatsTab, Overlay, InfiniteHealth, InfiniteMoney, UnlockContent, AdvancedTab, Brightness, Gamma, Contrast, GpuAdapter, Monitor, ColorDefaults, QualityTab, RefreshRate, TextureQuality, RenderScale, PerformanceStats, MotionBlur, CalibrationPattern, PlayerOnePreference, StickDiagonal, PlayOnline };
HWND window;
HFONT font;
int dpi = 96;
std::wstring launcherIni;
std::wstring settingsIni;
bool internalSelection=false,buildingUI=true;
int controlGroup = 0;
std::vector<std::wstring> monitorDevices;
std::vector<int> refreshRates;
std::vector<HWND> graphicsControls, inputControls, cheatControls, advancedControls, qualityControls;
int Px(int value) { return MulDiv(value, dpi, 96); }
#include "launcher_theme.h"
HWND Control(int id) { return GetDlgItem(window, id); }
void Error(const wchar_t* text) { MessageBoxW(window, text, L"TTT2 Launcher", MB_OK | MB_ICONERROR); }
std::wstring Text(HWND control) {
    std::wstring value(GetWindowTextLengthW(control) + 1, L'\0');
    value.resize(GetWindowTextW(control, value.data(), static_cast<int>(value.size())));
    return value;
}
HWND Add(const wchar_t* type, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
    if (!std::wcscmp(type, L"BUTTON")) style = (style & ~0xFu) | BS_OWNERDRAW;
    HWND child = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
        Px(x), Px(y), Px(w), Px(h), window, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (controlGroup == 1) graphicsControls.push_back(child);
    if (controlGroup == 2) inputControls.push_back(child);
    if (controlGroup == 3) cheatControls.push_back(child);
    if (controlGroup == 4) advancedControls.push_back(child);
    if (controlGroup == 5) qualityControls.push_back(child);
    return child;
}
void Combo(int id, const wchar_t* label, int y, const std::vector<std::wstring>& values, int selected) {
    Add(L"STATIC", label, 0, 0, 448, y + 6, 242, 26);
    HWND control = Add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, id, 704, y, 320, 300);
    SetWindowSubclass(control, ComboTheme, 1, 0);
    for (const auto& value : values) SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
    SendMessageW(control, CB_SETITEMHEIGHT, WPARAM(-1), Px(30));
    SendMessageW(control, CB_SETITEMHEIGHT, 0, Px(30));
    SendMessageW(control, CB_SETCURSEL, selected, 0);
}
int Selected(int id) { return static_cast<int>(SendMessageW(Control(id), CB_GETCURSEL, 0, 0)); }
void Select(int id, int value) { const bool previous=internalSelection;internalSelection=true;SendMessageW(Control(id), CB_SETCURSEL, value, 0);internalSelection=previous; }
void ShowTab() {
    const bool graphics = activePage==0;
    for (HWND child : graphicsControls) ShowWindow(child, graphics ? SW_SHOW : SW_HIDE);
    for (HWND child : inputControls) ShowWindow(child, activePage==1 ? SW_SHOW : SW_HIDE);
    for (HWND child : cheatControls) ShowWindow(child, activePage==2 ? SW_SHOW : SW_HIDE);
    for(HWND child:advancedControls) ShowWindow(child,activePage==3?SW_SHOW:SW_HIDE);
    for(HWND child:qualityControls) ShowWindow(child,activePage==4?SW_SHOW:SW_HIDE);
    InvalidateRect(Control(QualityTab),nullptr,TRUE);
    InvalidateRect(Control(AdvancedTab),nullptr,TRUE);
    InvalidateRect(Control(CheatsTab), nullptr, TRUE);
    InvalidateRect(Control(GraphicsTab), nullptr, TRUE);
    InvalidateRect(Control(InputTab), nullptr, TRUE);
}
void UpdateRefreshRates() {
    if(!Control(RefreshRate)||!Control(Resolution)||!Control(Monitor))return;
    const int previous=Selected(RefreshRate);
    const int wanted=previous>=0&&previous<int(refreshRates.size())?refreshRates[previous]:settings::GetRefreshRate();
    refreshRates={0};std::vector<int> rates;
    const int monitor=Selected(Monitor);
    std::wstring device=monitor>=0&&monitor<int(monitorDevices.size())?monitorDevices[monitor]:L"";
    if(device.empty())for(const auto& display:display_devices::Enumerate())if(display.primary)device=display.device;
    const int resolution=Selected(Resolution);if(resolution<0)return;
    const auto dimensions=settings::Resolutions()[resolution];
    DEVMODEW mode{};mode.dmSize=sizeof(mode);
    for(DWORD index=0;EnumDisplaySettingsW(device.empty()?nullptr:device.c_str(),index,&mode);++index)
        if(mode.dmPelsWidth==dimensions.width&&mode.dmPelsHeight==dimensions.height&&mode.dmDisplayFrequency>1&&mode.dmDisplayFrequency<=360)
            rates.push_back(int(mode.dmDisplayFrequency));
    std::sort(rates.begin(),rates.end());rates.erase(std::unique(rates.begin(),rates.end()),rates.end());
    SendMessageW(Control(RefreshRate),CB_RESETCONTENT,0,0);
    SendMessageW(Control(RefreshRate),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Automatic / desktop"));
    int choice=0;for(int rate:rates){refreshRates.push_back(rate);const auto label=std::to_wstring(rate)+L" Hz";SendMessageW(Control(RefreshRate),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));if(rate==wanted)choice=int(refreshRates.size())-1;}
    Select(RefreshRate,choice);
}
int CurrentPreset() {
    for (int preset = 0; preset < 3; ++preset)
        if (Selected(TextureQuality)==(preset==1?1:0) && Selected(RenderScale)==0 &&
            Selected(Brightness)==10 && Selected(Gamma)==10 && Selected(Contrast)==10 && Selected(Resolution) == (preset == 1 ? 1 : preset == 2 ? 4 : 2) &&
            Selected(Sharpness) == (preset == 2 ? 3 : 0) && Selected(AA) == (preset == 2 ? 1 : 0) &&
            Selected(Filter) == (preset == 2 ? 4 : 0) && Selected(Shadows) == (preset == 1 ? 0 : 1)) return preset;
    return 3;
}
void StagePreset(int preset) {
    // Presets only stage actual renderer settings; Save applies them. Display mode,
    // VSync, input bindings and online settings remain the user's choices.
    if (preset < 0 || preset > 2) return;
    Select(TextureQuality,preset==1?1:0);Select(RenderScale,0);
    Select(Brightness,10);Select(Gamma,10);Select(Contrast,10);
    Select(Resolution, preset == 1 ? 1 : preset == 2 ? 4 : 2);
    Select(Sharpness, preset == 2 ? 3 : 0);
    Select(AA, preset == 2 ? 1 : 0);
    Select(Filter, preset == 2 ? 4 : 0);
    Select(Shadows, preset == 1 ? 0 : 1);
    Select(Preset, preset);
    UpdateRefreshRates();
}
void StageGraphicsDefaults() {
    StagePreset(0);
    Select(Mode,0);Select(VSync,1);Select(GpuAdapter,0);Select(Monitor,0);
    Select(PerformanceStats,0);Select(MotionBlur,0);
    UpdateRefreshRates();Select(RefreshRate,0);
}
LRESULT CALLBACK CalibrationProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==WM_KEYDOWN&&wParam==VK_ESCAPE){DestroyWindow(hwnd);return 0;}
    if(message==WM_GETMINMAXINFO){auto* limits=reinterpret_cast<MINMAXINFO*>(lParam);limits->ptMinTrackSize={Px(500),Px(260)};return 0;}
    if(message==WM_PAINT){
        PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);RECT client{};GetClientRect(hwnd,&client);
        HBRUSH background=CreateSolidBrush(RGB(18,18,18));FillRect(dc,&client,background);DeleteObject(background);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(235,235,235));
        HGDIOBJ old=SelectObject(dc,GetStockObject(DEFAULT_GUI_FONT));
        RECT title{20,15,client.right-20,75};
        DrawTextW(dc,L"SDR DISPLAY REFERENCE\nAdjust your display so nearby dark and bright squares remain distinguishable.",-1,&title,DT_LEFT|DT_WORDBREAK);
        const int values[]={0,2,4,8,16,24,32,64,96,128,160,192,224,240,248,252,254,255};
        const int left=20,right=client.right-20,width=(right-left)/18,top=90,bottom=client.bottom-90;
        for(int i=0;i<18;++i){
            const int v=values[i];HBRUSH brush=CreateSolidBrush(RGB(v,v,v));
            RECT cell{left+i*width,top,i==17?right:left+(i+1)*width,bottom};FillRect(dc,&cell,brush);DeleteObject(brush);
            RECT label{cell.left,bottom+8,cell.right,bottom+28};const auto text=std::to_wstring(v);
            DrawTextW(dc,text.c_str(),-1,&label,DT_CENTER|DT_SINGLELINE);
        }
        RECT footer{20,client.bottom-45,client.right-20,client.bottom-10};
        DrawTextW(dc,L"Display reference only: launcher brightness, gamma and contrast apply inside the game. Esc closes.",-1,&footer,DT_LEFT|DT_WORDBREAK);
        SelectObject(dc,old);EndPaint(hwnd,&ps);return 0;
    }
    return DefWindowProcW(hwnd,message,wParam,lParam);
}
void ShowCalibrationPattern(){
    static bool registered=false;
    if(!registered){WNDCLASSW wc{};wc.lpfnWndProc=CalibrationProc;wc.hInstance=GetModuleHandleW(nullptr);wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"TTT2SdrReference";registered=RegisterClassW(&wc)!=0;}
    if(!registered){Error(L"Cannot open the display reference.");return;}
    HWND reference=CreateWindowExW(0,L"TTT2SdrReference",L"SDR display reference",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        CW_USEDEFAULT,CW_USEDEFAULT,Px(940),Px(410),window,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!reference)Error(L"Cannot open the display reference.");
}
bool SteamRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W process{ sizeof(process) };
    bool running = false;
    if (Process32FirstW(snapshot, &process)) do {
        if (!_wcsicmp(process.szExeFile, L"steam.exe")) { running = true; break; }
    } while (Process32NextW(snapshot, &process));
    CloseHandle(snapshot);
    return running;
}
bool GameRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true; // cannot safely rule out a running game
    PROCESSENTRY32W process{ sizeof(process) };
    bool running = false;
    if (!Process32FirstW(snapshot, &process)) running = true;
    else do {
        if (process.th32ProcessID!=GetCurrentProcessId() &&
            (!_wcsicmp(process.szExeFile, L"Tekken Tag Tournament 2.exe") ||
            !_wcsicmp(process.szExeFile, L"tekken_recomp.exe"))) { running = true; break; }
    } while (Process32NextW(snapshot, &process));
    CloseHandle(snapshot);
    return running;
}
bool Apply() {
    if (GameRunning()) { Error(L"Close the game before saving settings or launching another copy."); return false; }
    graphics_registry::Apply(123,Selected(MotionBlur)-1);
    graphics_registry::Apply(41,Selected(TextureQuality));
    graphics_registry::Apply(22,Selected(RenderScale)==0?0:25+Selected(RenderScale)*25);
    graphics_registry::Apply(149,Selected(PerformanceStats));
    graphics_registry::Apply(14,50+Selected(Brightness)*5);
    graphics_registry::Apply(15,50+Selected(Gamma)*5);
    graphics_registry::Apply(16,50+Selected(Contrast)*5);
    graphics_registry::Apply(4,Selected(GpuAdapter)-1);
    if(Selected(RefreshRate)>=0&&Selected(RefreshRate)<int(refreshRates.size()))graphics_registry::Apply(6,refreshRates[Selected(RefreshRate)]);
    if(Selected(Monitor)>=0 && Selected(Monitor)<int(monitorDevices.size()))settings::SetMonitorDevice(monitorDevices[Selected(Monitor)]);
    graphics_registry::Apply(1,Selected(Mode));
    graphics_registry::Apply(2,Selected(Resolution));
    graphics_registry::Apply(122,Selected(AA));
    graphics_registry::Apply(40,Selected(Sharpness));
    graphics_registry::Apply(43,Selected(Filter));
    graphics_registry::Apply(7,Selected(VSync) == 1);
    graphics_registry::Apply(54,Selected(Shadows) == 1);
    settings::SetInputDelayOn(Selected(Delay) == 1);
    if (!WritePrivateProfileStringW(L"Keyboard", L"SeparatePlayer", Selected(InputLayout) == 1 ? L"1" : L"0", settingsIni.c_str())) {
        Error(L"Could not save the input layout."); return false;
    }
    const std::wstring playerPreference=std::to_wstring(Selected(PlayerOnePreference));
    if (!WritePrivateProfileStringW(L"Input",L"PlayerOnePreference",playerPreference.c_str(),settingsIni.c_str())) {
        Error(L"Could not save the player one preference."); return false;
    }
    const std::wstring socd = std::to_wstring(Selected(SOCD));
    if (!WritePrivateProfileStringW(L"Keyboard", L"SOCD", socd.c_str(), settingsIni.c_str())) {
        Error(L"Could not save directional input settings."); return false;
    }
    const std::wstring deadzone = std::to_wstring(Selected(Deadzone));
    const std::wstring diagonal = std::to_wstring(Selected(StickDiagonal));
    if (!WritePrivateProfileStringW(L"Input", L"DPadOnly", Selected(StickMode) == 1 ? L"1" : L"0", settingsIni.c_str()) ||
        !WritePrivateProfileStringW(L"Input", L"StickDeadzone", deadzone.c_str(), settingsIni.c_str()) ||
        !WritePrivateProfileStringW(L"Input", L"StickDiagonal", diagonal.c_str(), settingsIni.c_str())) {
        Error(L"Could not save stick settings."); return false;
    }
    if (!WritePrivateProfileStringW(L"Mods", L"Overlay", Selected(Overlay)==1?L"1":L"0", settingsIni.c_str())) {
        Error(L"Could not save overlay settings."); return false;
    }
    if (!WritePrivateProfileStringW(L"Mods",L"InfiniteMoney",Selected(InfiniteMoney)==1?L"1":L"0",settingsIni.c_str())) {
        Error(L"Could not save money settings."); return false;
    }
    if (!WritePrivateProfileStringW(L"Mods",L"UnlockContent",Selected(UnlockContent)==1?L"1":L"0",settingsIni.c_str())) {
        Error(L"Could not save unlock settings."); return false;
    }
    const auto health=std::to_wstring(Selected(InfiniteHealth));
    if (!WritePrivateProfileStringW(L"Mods",L"InfiniteHealth",health.c_str(),settingsIni.c_str())) {
        Error(L"Could not save health settings."); return false;
    }
    const std::wstring exe = Text(Control(GamePath));
    if (!WritePrivateProfileStringW(L"Launcher", L"GameExecutable", exe.c_str(), launcherIni.c_str())) {
        Error(L"Could not save the launcher configuration."); return false;
    }
    return true;
}
void PickGame() {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{ sizeof(dialog) };
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"Tekken executable (*.exe)\0*.exe\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"Select Tekken Tag Tournament 2.exe";
    if (GetOpenFileNameW(&dialog)) SetWindowTextW(Control(GamePath), path);
}
void Launch() {
    const std::filesystem::path exe(Text(Control(GamePath)));
    std::error_code ec;
    wchar_t self[32768]{};GetModuleFileNameW(nullptr,self,32768);
    if(std::filesystem::equivalent(exe,std::filesystem::path(self),ec)) {
        Error(L"The launcher cannot launch itself. Choose tekken_recomp.exe.");return;
    }
    if (!exe.is_absolute() || !std::filesystem::is_regular_file(exe, ec) ||
        (_wcsicmp(exe.filename().c_str(), L"Tekken Tag Tournament 2.exe") &&
         _wcsicmp(exe.filename().c_str(), L"tekken_recomp.exe"))) {
        Error(L"Select Tekken Tag Tournament 2.exe or tekken_recomp.exe first."); return;
    }
    // Release-layout launches only: a development build needs its own image/VFS arguments.
    if (!std::filesystem::is_regular_file(exe.parent_path() / L"game" / L"tekken.img", ec)) {
        Error(L"The selected executable needs a game folder containing tekken.img beside it."); return;
    }
    if (!Apply()) return;
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
        exe.parent_path().c_str(), &startup, &process)) {
        Error(L"Windows could not start the game. Check the selected executable and its runtime DLLs."); return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    DestroyWindow(window);
}
void LaunchOnline() {
    if(GameRunning()){Error(L"Close the running game before launching another copy.");return;}
    wchar_t self[32768]{};GetModuleFileNameW(nullptr,self,32768);
    const auto root=std::filesystem::path(self).parent_path();
    wchar_t configured[32768]{};
    GetPrivateProfileStringW(L"Launcher",L"OriginalExecutable",L"",configured,32768,launcherIni.c_str());
    const auto exe=*configured?std::filesystem::path(configured):root/L"Tekken Tag Tournament 2 (Original).exe";
    if(!std::filesystem::is_regular_file(exe) || std::filesystem::equivalent(exe,std::filesystem::path(self))) {
        Error(L"The original game executable is missing. Restore it from your update backup.");return;
    }
    PROCESS_INFORMATION process{};
    if(!original_launch::Start(exe,process,root)) {Error(L"Windows could not start the original game.");return;}
    CloseHandle(process.hThread);CloseHandle(process.hProcess);DestroyWindow(window);
}
void BuildUI() {
    font = CreateFontW(-Px(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    LoadArtwork();
    Add(L"BUTTON", L"GRAPHICS", WS_TABSTOP, GraphicsTab, 448, 94, 102, 32);
    Add(L"BUTTON", L"INPUT", WS_TABSTOP, InputTab, 558, 94, 86, 32);
    Add(L"BUTTON", L"CHEATS", WS_TABSTOP, CheatsTab, 652, 94, 116, 32);
    Add(L"BUTTON",L"DISPLAY",WS_TABSTOP,AdvancedTab,776,94,116,32);
    Add(L"BUTTON",L"QUALITY",WS_TABSTOP,QualityTab,900,94,124,32);
    controlGroup=1;
    std::vector<std::wstring> resolutions;
    for (const auto& r : settings::Resolutions()) resolutions.push_back(std::to_wstring(r.width)+L" x "+std::to_wstring(r.height));
    Combo(Mode,L"Display mode",154,{L"Windowed",L"Borderless",L"Fullscreen"},settings::GetMode());
    Combo(Resolution,L"Render resolution",194,resolutions,settings::GetResolutionIndex());
    Combo(AA,L"Anti-aliasing",234,{L"Off",L"FXAA",L"SMAA 1x (High)"},settings::GetAntiAliasing());
    std::vector<std::wstring> levels{L"Off"};
    for(int i=1;i<=settings::kMaxSharpness;++i) levels.push_back(std::to_wstring(i));
    Combo(Sharpness,L"Sharpness",274,levels,settings::GetSharpness());
    Combo(Filter,L"Texture filtering",314,{L"Standard",L"Anisotropic 2x",L"Anisotropic 4x",L"Anisotropic 8x",L"Anisotropic 16x"},settings::GetTextureFilter());
    Combo(VSync,L"VSync",354,{L"Off",L"On"},settings::GetVSync());
    Combo(Shadows,L"Shadows",394,{L"Off",L"On"},settings::GetShadows());
    Combo(Delay,L"Offline input delay",434,{L"Removed",L"Original (2 frames)"},settings::GetInputDelayOn());
    Combo(Preset,L"Graphics preset",474,{L"Original",L"Performance",L"Enhanced",L"Custom"},3);
    Add(L"BUTTON",L"Restore original graphics",WS_TABSTOP,Defaults,448,524,242,30);
    controlGroup=2;
    Combo(InputLayout,L"Player assignment",154,{L"Shared Player 1 (legacy)",L"Start to join (automatic)"},GetPrivateProfileIntW(L"Keyboard",L"SeparatePlayer",1,settingsIni.c_str())==1);
    Combo(SOCD,L"Keyboard SOCD",194,{L"Original",L"Neutral",L"Up priority",L"Last input priority"},
        GetPrivateProfileIntW(L"Keyboard",L"SOCD",0,settingsIni.c_str())<=3?GetPrivateProfileIntW(L"Keyboard",L"SOCD",0,settingsIni.c_str()):0);
    Combo(StickMode,L"Controller directions",234,{L"D-pad + analog",L"D-pad only"},GetPrivateProfileIntW(L"Input",L"DPadOnly",0,settingsIni.c_str())==1);
    std::vector<std::wstring> deadzones; for(int i=0;i<=95;++i) deadzones.push_back(std::to_wstring(i)+L"%");
    const unsigned dz=GetPrivateProfileIntW(L"Input",L"StickDeadzone",0,settingsIni.c_str());
    Combo(Deadzone,L"Left stick deadzone",274,deadzones,dz<=95?int(dz):0);
    const unsigned preference=GetPrivateProfileIntW(L"Input",L"PlayerOnePreference",0,settingsIni.c_str());
    Combo(PlayerOnePreference,L"Player 1 preference",314,{L"First Start (automatic)",L"Keyboard first",L"Controller first"},preference<=2?int(preference):0);
    std::vector<std::wstring> diagonals{L"Original"};
    for(int i=1;i<=100;++i) diagonals.push_back(std::to_wstring(i)+L"% axis ratio");
    const unsigned diagonal=GetPrivateProfileIntW(L"Input",L"StickDiagonal",0,settingsIni.c_str());
    Combo(StickDiagonal,L"Stick diagonal threshold",354,diagonals,diagonal<=100?int(diagonal):0);
    Add(L"STATIC",L"Higher diagonal thresholds require more even stick axes. Digital D-pad buttons stay unchanged.",0,0,448,398,550,44);
    Add(L"STATIC",L"First Start assigns Player 1; a second device joins with Start. A Player 1 preference chooses which device starts first.",0,0,448,452,550,58);
    Add(L"STATIC",L"Close the game before saving input changes. Restart to apply.",0,0,448,520,550,38);
    controlGroup=3;
    Combo(Overlay,L"L3 + R3 overlay",154,{L"Off",L"On (offline only)"},GetPrivateProfileIntW(L"Mods",L"Overlay",0,settingsIni.c_str())==1);
    Combo(InfiniteHealth,L"Infinite health",198,{L"Off",L"Player 1",L"Player 2",L"Both sides"},std::clamp<int>(GetPrivateProfileIntW(L"Mods",L"InfiniteHealth",0,settingsIni.c_str()),0,3));
    Combo(InfiniteMoney,L"Infinite money",242,{L"Off",L"On (wallet refill)"},GetPrivateProfileIntW(L"Mods",L"InfiniteMoney",0,settingsIni.c_str())==1);
    Combo(UnlockContent,L"Unlock content",286,{L"Off",L"On (temporary, offline)"},GetPrivateProfileIntW(L"Mods",L"UnlockContent",0,settingsIni.c_str())==1);
    Add(L"STATIC",L"Press both stick buttons on one controller to open the in-game tools panel. The panel closes and blocks its actions during online sessions.",0,0,448,336,550,60);
    Add(L"STATIC",L"AVAILABLE NOW",0,0,448,402,550,24);
    Add(L"STATIC",L"Health, money, progression rewards, movies, items, LEDs and panels.",0,0,448,438,550,45);
    Add(L"STATIC",L"Unlocks are temporary; normal saves manage equipped items. Missing assets and DLC licences are not supplied. Health and money apply only offline.",0,0,448,494,550,80);
    controlGroup=4;
    std::vector<std::wstring> percentages;for(int v=50;v<=150;v+=5) percentages.push_back(std::to_wstring(v)+L"%");
    Combo(Brightness,L"SDR brightness",154,percentages,(settings::GetBrightness()-50)/5);
    Combo(Contrast,L"SDR contrast",198,percentages,(settings::GetContrast()-50)/5);
    for(int v=155;v<=250;v+=5) percentages.push_back(std::to_wstring(v)+L"%");
    Combo(Gamma,L"SDR gamma adjustment",242,percentages,(settings::GetGamma()-50)/5);
    std::vector<std::wstring> adapters{L"Automatic (high performance)"};
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))for(UINT i=0;i<32;++i){
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;if(FAILED(factory->EnumAdapters1(i,&adapter)))break;
        DXGI_ADAPTER_DESC1 info{};adapter->GetDesc1(&info);adapters.push_back(info.Description);
    }
    const int gpu=settings::GetGpuAdapter()+1;
    Combo(GpuAdapter,L"Graphics adapter (restart)",286,adapters,gpu<int(adapters.size())?gpu:0);
    std::vector<std::wstring> monitors{L"Automatic (current display)"};monitorDevices={L""};int monitorSelection=0;
    for(const auto& display:display_devices::Enumerate()){
        monitorDevices.push_back(display.device);
        monitors.push_back(display.device+(display.primary?L" (primary)":L""));
        if(display.device==settings::GetMonitorDevice())monitorSelection=int(monitors.size())-1;
    }
    Combo(Monitor,L"Monitor",330,monitors,monitorSelection);
    Combo(RefreshRate,L"Exclusive refresh rate",374,{L"Automatic / desktop"},0);UpdateRefreshRates();
    Add(L"BUTTON",L"Reset SDR calibration",WS_TABSTOP,ColorDefaults,448,418,242,30);
    Add(L"BUTTON",L"Show display reference",WS_TABSTOP,CalibrationPattern,704,418,250,30);
    Add(L"STATIC",L"100% keeps the original image. Brightness, contrast and gamma affect the actual SDR presentation pass. HDR is not supported by this renderer.",0,0,448,462,550,62);
    Add(L"STATIC",L"GPU selection applies on launch. Refresh selection affects exclusive fullscreen only. Unsupported refresh modes revert to automatic.",0,0,448,532,550,54);
    controlGroup=5;
    Combo(TextureQuality,L"Mip texture detail",154,{L"Original",L"Reduce 1 mip",L"Reduce 2 mips",L"Reduce 3 mips"},settings::GetTextureMipBias());
    std::vector<std::wstring> scales{L"Follow render resolution"};for(int v=50;v<=200;v+=25)scales.push_back(std::to_wstring(v)+L"% of native 720p");
    Combo(RenderScale,L"Internal render scale",198,scales,settings::GetRenderScale()==0?0:(settings::GetRenderScale()-25)/25);
    Combo(PerformanceStats,L"Performance readout",242,{L"Off",L"Submitted FPS (window title)",L"FPS, timings, memory (+ log)"},settings::GetPerformanceStats());
    Combo(MotionBlur,L"Motion blur",286,{L"Follow saved game setting",L"Off",L"On"},settings::GetMotionBlur()+1);
    Add(L"STATIC",L"Texture detail changes real mip sampling; it does not shrink texture allocations. Render scale changes internal color/depth targets, independently of window size.",0,0,448,336,550,76);
    Add(L"STATIC",L"Readouts appear in the window title. Submitted FPS counts accepted DXGI presents. GPU interval includes command gaps; memory shows this process's local usage and budget. Detailed results also appear in the game log.",0,0,448,420,550,130);
    controlGroup=0;
    Add(L"STATIC",L"GAME LOCATION",0,0,448,594,550,24);
    Add(L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL|ES_READONLY,GamePath,448,624,462,32);
    Add(L"BUTTON",L"Browse",WS_TABSTOP,Browse,920,624,104,32);
    wchar_t netMode[32]{};GetEnvironmentVariableW(L"TTT2_NET",netMode,static_cast<DWORD>(std::size(netMode)));
    const wchar_t* onlineHint=!_wcsicmp(netMode,L"loopback")
        ?L"Offline uses the updated game. Online uses the preserved original. Both use your normal save location."
        :(SteamRunning()?L"Steam is running. Online also requires a Steam-enabled game build.":L"Steam is not running. Start it for online play with a compatible build.");
    Add(L"STATIC",onlineHint,0,0,448,672,576,38);
    Add(L"BUTTON",L"Save settings",WS_TABSTOP,Save,448,722,170,42);
    Add(L"BUTTON",L"PLAY ONLINE",WS_TABSTOP,PlayOnline,630,722,184,42);
    Add(L"BUTTON",L"PLAY OFFLINE",WS_TABSTOP|BS_DEFPUSHBUTTON,Play,828,722,196,42);
    Select(Preset, CurrentPreset());
    ShowTab();
    wchar_t saved[32768]{};
    GetPrivateProfileStringW(L"Launcher", L"GameExecutable", L"", saved, static_cast<DWORD>(std::size(saved)), launcherIni.c_str());
    if (!*saved) {
        GetModuleFileNameW(nullptr, saved, static_cast<DWORD>(std::size(saved)));
        const auto directory = std::filesystem::path(saved).parent_path();
        for (const wchar_t* filename : {L"Tekken Tag Tournament 2.exe", L"tekken_recomp.exe"}) {
            const auto sibling = directory / filename;
            std::error_code ec;
            if (std::filesystem::is_regular_file(sibling, ec)) {
                SetWindowTextW(Control(GamePath), sibling.c_str()); break;
            }
        }
    } else SetWindowTextW(Control(GamePath), saved);
    buildingUI=false;
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: window = hwnd; BuildUI(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};HDC dc=BeginPaint(hwnd,&paint);RECT r{};GetClientRect(hwnd,&r);
        HDC memory=CreateCompatibleDC(dc);HBITMAP buffer=CreateCompatibleBitmap(dc,r.right,r.bottom);
        HGDIOBJ old=SelectObject(memory,buffer);PaintLauncher(memory);
        BitBlt(dc,0,0,r.right,r.bottom,memory,0,0,SRCCOPY);
        SelectObject(memory,old);DeleteObject(buffer);DeleteDC(memory);EndPaint(hwnd,&paint);return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc=reinterpret_cast<HDC>(wp);SetTextColor(dc,RGB(207,214,228));
        const bool field=msg!=WM_CTLCOLORSTATIC || GetDlgCtrlID(reinterpret_cast<HWND>(lp))==GamePath;
        SetBkColor(dc,field?RGB(31,35,44):RGB(19,22,29));return reinterpret_cast<LRESULT>(field?fieldBrush:panelBrush);
    }
    case WM_MEASUREITEM: reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight=Px(30);return TRUE;
    case WM_DRAWITEM: {
        auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if(item->CtlType==ODT_BUTTON)DrawButton(*item);
        else if(item->CtlType==ODT_COMBOBOX)DrawCombo(*item);
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == CBN_SELCHANGE) {
            if(LOWORD(wp)==Resolution||LOWORD(wp)==Monitor)UpdateRefreshRates();
            if (LOWORD(wp) == Preset) StagePreset(Selected(Preset));
            else Select(Preset, CurrentPreset());
        }
        if (HIWORD(wp) == BN_CLICKED) {
            if (LOWORD(wp) == GraphicsTab || LOWORD(wp) == InputTab || LOWORD(wp) == CheatsTab || LOWORD(wp)==AdvancedTab || LOWORD(wp)==QualityTab) {
                activePage=LOWORD(wp)==GraphicsTab?0:LOWORD(wp)==InputTab?1:LOWORD(wp)==CheatsTab?2:LOWORD(wp)==AdvancedTab?3:4;ShowTab();
            }
            if (LOWORD(wp) == Defaults) StageGraphicsDefaults();
            if (LOWORD(wp)==ColorDefaults) {Select(Brightness,10);Select(Gamma,10);Select(Contrast,10);Select(Preset,CurrentPreset());}
            if (LOWORD(wp)==CalibrationPattern) ShowCalibrationPattern();
            if (LOWORD(wp) == Browse) PickGame();
            if (LOWORD(wp) == Save && Apply()) MessageBoxW(window, L"Settings saved.", L"TTT2 Launcher", MB_OK);
            if (LOWORD(wp) == Play) Launch();
            if (LOWORD(wp) == PlayOnline) LaunchOnline();
        }
        return 0;
    case WM_DESTROY: DeleteObject(font);hero.reset();if(heroStream)heroStream->Release();DeleteObject(panelBrush);DeleteObject(fieldBrush);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int show) {
    try {
        wchar_t self[32768]{};
        const DWORD size=GetModuleFileNameW(nullptr,self,32768);
        if(!size || size>=32768)throw std::runtime_error("Cannot locate the launcher.");
        const bool configured=preview_environment::Configure(self);
        // Headless installation acceptance: same startup path and process
        // ownership check as normal launch, without opening a window/game.
        if(arguments && !_wcsicmp(arguments,L"--check-install"))
            return configured && !GameRunning()?0:2;
    } catch(const std::exception& error) {
        MessageBoxA(nullptr,error.what(),"TTT2 Offline Launcher",MB_OK|MB_ICONERROR);return 1;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES}; InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput graphicsStartup;Gdiplus::GdiplusStartup(&graphicsToken,&graphicsStartup,nullptr);
    SetProcessDPIAware();
    HDC screen = GetDC(nullptr); dpi = GetDeviceCaps(screen, LOGPIXELSX); ReleaseDC(nullptr, screen);
    RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    const int fitDpi=(work.bottom-work.top-50)*96/790;
    if(dpi>fitDpi) dpi=fitDpi<72?72:fitDpi;
    wchar_t local[32768]{};
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(std::size(local)));
    if (!size || size >= std::size(local)) { Error(L"LOCALAPPDATA is not available."); return 1; }
    auto dir = std::filesystem::path(local) / L"TTT2Recomp";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) { Error(L"Cannot create the settings folder."); return 1; }
    launcherIni = (dir / L"launcher.ini").wstring();
    wchar_t overridePath[32768]{};
    DWORD overrideSize = GetEnvironmentVariableW(L"TTT2_SETTINGS_INI", overridePath, static_cast<DWORD>(std::size(overridePath)));
    settingsIni = overrideSize && overrideSize < std::size(overridePath) ? overridePath : (dir / L"settings.ini").wstring();
    if (overrideSize && overrideSize < std::size(overridePath))
        launcherIni = (std::filesystem::path(settingsIni).parent_path() / L"launcher.ini").wstring();
    wchar_t launchOverride[32768]{};
    if(GetEnvironmentVariableW(L"TTT2_LAUNCHER_INI",launchOverride,32768))launcherIni=launchOverride;
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc; cls.hInstance = instance; cls.lpszClassName = L"TTT2Launcher";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = nullptr;
    cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(102));
    if (!RegisterClassW(&cls)) return 1;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT rect{ 0, 0, Px(1080), Px(790) }; AdjustWindowRect(&rect, style, FALSE);
    HWND hwnd = CreateWindowW(cls.lpszClassName, L"TTT2 Launcher - v" TTT2_UPDATE_VERSION_W, style, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    BOOL dark=TRUE;DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark));
    ShowWindow(hwnd, show);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    Gdiplus::GdiplusShutdown(graphicsToken);
    return static_cast<int>(msg.wParam);
}
