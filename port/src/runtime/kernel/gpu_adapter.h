#pragma once
// The GPU the game renders on. Windows hands an application the default adapter - on laptops with
// switchable graphics that is the integrated GPU unless the application asks otherwise (MEASURED in a
// real match: a friend's gaming PC ran the port on "Intel(R) UHD Graphics"). The high-performance
// adapter by DXGI's GPU preference is used instead (the dedicated GPU when there is one; the exe also
// exports NvOptimusEnablement / AmdPowerXpressRequestHighPerformance, main.cpp). TTT2_GPU_ADAPTER=<n>
// picks DXGI's n-th adapter. Every adapter is listed in the log once.
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#pragma comment(lib, "dxgi.lib")

inline Microsoft::WRL::ComPtr<IDXGIAdapter1> PreferredGpuAdapter()
{
    using Microsoft::WRL::ComPtr;
    static bool logged = false;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    ComPtr<IDXGIAdapter1> pick;
    if (const char* e = std::getenv("TTT2_GPU_ADAPTER"); e && *e)
        factory->EnumAdapters1(UINT(std::atoi(e)), &pick);
    ComPtr<IDXGIFactory6> factory6;
    if (!pick && SUCCEEDED(factory.As(&factory6)))
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> a;
            if (FAILED(factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)))) break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            pick = a;
            break;
        }
    if (!logged)
    {
        logged = true;
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            DXGI_ADAPTER_DESC1 p{};
            if (pick) pick->GetDesc1(&p);
            const bool chosen = pick && p.AdapterLuid.LowPart == d.AdapterLuid.LowPart && p.AdapterLuid.HighPart == d.AdapterLuid.HighPart;
            std::fprintf(stderr, "[gpu] adapter %u: %ls, %llu MB dedicated%s%s\n", i, d.Description,
                         (unsigned long long)(d.DedicatedVideoMemory >> 20), (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "",
                         chosen ? " <- rendering here" : "");
        }
    }
    return pick;
}
