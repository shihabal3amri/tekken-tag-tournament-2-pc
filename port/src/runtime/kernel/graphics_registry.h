#pragma once
#include "display_settings.h"
#include <algorithm>
#include <span>
#include <string_view>
// Scalar settings exposed by this D3D11 build. Dynamic monitor/GPU/output lists
// are enumerated by display_devices.h/DXGI; unsupported inventory IDs are tracked
// separately in docs/graphics/FEATURE_MATRIX.md and are never writable controls.
namespace graphics_registry {
struct Setting {
    int id;
    std::string_view name, key, group, units, impact, help;
    int minimum,maximum,defaultValue,step;
    bool restart;
    int (*read)();
    void (*write)(int);
};
inline std::span<const Setting> Settings() {
    using namespace display_settings;
    static const Setting settings[]={
        {1,"Display mode","Mode","Display","enum","display","Windowed, borderless or exclusive fullscreen",0,2,0,1,false,GetMode,SetMode},
        {2,"Resolution","Resolution","Display","list","GPU/VRAM","Output dimensions; also internal dimensions when scale follows resolution",0,6,2,1,false,GetResolutionIndex,SetResolutionIndex},
        {4,"GPU adapter","GpuAdapter","Display","DXGI index","GPU","-1 chooses high performance; missing adapter falls back automatically",-1,31,-1,1,true,GetGpuAdapter,SetGpuAdapter},
        {6,"Exclusive refresh","RefreshRate","Display","Hz","display","0 automatic; choices enumerated for the selected monitor and resolution",0,360,0,1,false,GetRefreshRate,SetRefreshRate},
        {7,"VSync","VSync","Display","boolean","latency","Presentation behavior only; never changes 60 Hz simulation",0,1,1,1,false,+[]{return int(GetVSync());},+[](int v){SetVSync(v!=0);}},
        {14,"SDR brightness","Brightness","Calibration","percent","GPU","100 preserves the original SDR brightness",50,150,100,5,false,GetBrightness,SetBrightness},
        {15,"SDR gamma","Gamma","Calibration","percent","GPU","100 preserves original transfer; applied after guest gamma",50,250,100,5,false,GetGamma,SetGamma},
        {16,"SDR contrast","Contrast","Calibration","percent","GPU","Contrast around middle gray; output remains bounded SDR",50,150,100,5,false,GetContrast,SetContrast},
        {22,"Internal render scale","RenderScale","Quality","percent of native 720p","GPU/VRAM","0 follows resolution; otherwise 50 through 200 percent",0,200,0,25,false,GetRenderScale,SetRenderScale},
        {40,"Sharpening","Sharpness","Quality","level","GPU","Contrast adaptive sharpening of scaled output",0,10,0,1,false,GetSharpness,SetSharpness},
        {41,"Mip detail reduction","TextureMipBias","Quality","mip levels","GPU","Positive mip bias; does not shrink texture allocations",0,3,0,1,false,GetTextureMipBias,SetTextureMipBias},
        {43,"Texture filtering","TextureFilter","Quality","enum","GPU","Original, anisotropic 2x, 4x, 8x or 16x on mipmapped textures",0,4,0,1,false,GetTextureFilter,SetTextureFilter},
        {54,"Cast shadows","Shadows","Quality","boolean","GPU","Binary caster toggle; no shadow-resolution quality levels",0,1,1,1,false,+[]{return int(GetShadows());},+[](int v){SetShadows(v!=0);}},
        {122,"Anti-aliasing","AntiAliasing","Quality","enum","GPU/VRAM","Off, FXAA or SMAA 1x HIGH; no temporal reconstruction",0,2,0,1,false,GetAntiAliasing,SetAntiAliasing},
        {123,"Motion blur","MotionBlur","Quality","enum","GPU","-1 follows the save; 0 off, 1 on via original guest option setter",-1,1,-1,1,false,GetMotionBlur,SetMotionBlur},
        {149,"Performance readout","PerformanceStats","Diagnostics","enum","CPU","Off, submitted FPS, or timing/memory details in window title and log",0,2,0,1,false,GetPerformanceStats,SetPerformanceStats},
    };
    return settings;
}
inline const Setting* Find(int id) {
    for(const auto& setting:Settings())if(setting.id==id)return &setting;
    return nullptr;
}
inline bool Apply(int id,int value) {
    const Setting* setting=Find(id);
    if(!setting)return false;
    setting->write(std::clamp(value,setting->minimum,setting->maximum));
    return true;
}
}
