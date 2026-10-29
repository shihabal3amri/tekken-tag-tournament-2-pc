#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
namespace display_devices {
struct Display { HMONITOR handle; std::wstring device; RECT bounds; bool primary; };
inline std::vector<Display> Enumerate() {
    std::vector<Display> displays;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM context)->BOOL {
        MONITORINFOEXW info{};info.cbSize=sizeof(info);
        if(GetMonitorInfoW(monitor,&info))reinterpret_cast<std::vector<Display>*>(context)->push_back(
            {monitor,info.szDevice,info.rcMonitor,(info.dwFlags&MONITORINFOF_PRIMARY)!=0});
        return TRUE;
    },reinterpret_cast<LPARAM>(&displays));
    return displays;
}
inline HMONITOR Resolve(const std::wstring& device,HWND window) {
    if(!device.empty())for(const auto& display:Enumerate())if(display.device==device)return display.handle;
    return MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
}
}
