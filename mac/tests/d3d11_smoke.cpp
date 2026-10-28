// D3D11 through DXVK-native on MoltenVK: does the renderer stack work on macOS?
//
//   d3d11_smoke                 offscreen: device, HLSL via vkd3d-shader (native_d3dcompiler.cpp),
//                               draws, readback, and the D3D11 features the game's backend uses
//   d3d11_smoke --window [N]    SDL3 window + DXGI flip swap chain, N frames (default 300), fps
//   d3d11_smoke --window N [--no-vsync] [--thread]   --thread: render on a worker, main thread pumps events
//
// Every offscreen check reads pixels back through a staging texture and compares exact values.
#include <windows.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include "dxvk_native_com_ptr.h"
#include <wsi/native_sdl3.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

// The port's ComPtr (dxvk_native_com_ptr.h) has no GetAddressOf; pass a one-element array instead.
template <typename T> struct One {
    T* p;
    operator T* const*() const { return &p; }
};
template <typename T> static One<T> One1(const ComPtr<T>& c) { return One<T>{c.Get()}; }

static int g_failures = 0;
static void Need(HRESULT hr, const char* what)
{
    if (FAILED(hr)) { std::fprintf(stderr, "FATAL %s: %08x\n", what, unsigned(hr)); std::exit(1); }
}
static void Report(bool ok, const char* name, const std::string& detail = {})
{
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++g_failures;
}

static ComPtr<ID3DBlob> Compile(const char* src, const char* target)
{
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(src, std::strlen(src), "smoke", nullptr, nullptr, "main", target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3DCompile(%s) failed: %s\n", target,
                     errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        std::exit(1);
    }
    return code;
}

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};

    ComPtr<ID3D11VertexShader> Vs(const char* src) {
        auto code = Compile(src, "vs_5_0"); ComPtr<ID3D11VertexShader> s;
        Need(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s), "CreateVertexShader");
        return s;
    }
    ComPtr<ID3D11PixelShader> Ps(const char* src) {
        auto code = Compile(src, "ps_5_0"); ComPtr<ID3D11PixelShader> s;
        Need(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s), "CreatePixelShader");
        return s;
    }
    ComPtr<ID3D11Texture2D> Tex(UINT w, UINT h, DXGI_FORMAT f, UINT bind, const void* init = nullptr, UINT pitch = 0) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA s{init, pitch, 0}; ComPtr<ID3D11Texture2D> t;
        Need(device->CreateTexture2D(&d, init ? &s : nullptr, &t), "CreateTexture2D");
        return t;
    }
    ComPtr<ID3D11RenderTargetView> Rtv(ID3D11Texture2D* t) {
        ComPtr<ID3D11RenderTargetView> v; Need(device->CreateRenderTargetView(t, nullptr, &v), "CreateRenderTargetView"); return v;
    }
    ComPtr<ID3D11ShaderResourceView> Srv(ID3D11Resource* t, const D3D11_SHADER_RESOURCE_VIEW_DESC* d = nullptr) {
        ComPtr<ID3D11ShaderResourceView> v; Need(device->CreateShaderResourceView(t, d, &v), "CreateShaderResourceView"); return v;
    }
    // Copy to a staging texture and return the bytes of the top-left pixel row by row.
    std::vector<uint8_t> Read(ID3D11Texture2D* t, UINT bpp) {
        D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging; Need(device->CreateTexture2D(&d, nullptr, &staging), "staging");
        context->CopyResource(staging.Get(), t);
        D3D11_MAPPED_SUBRESOURCE m;
        // The game polls with DO_NOT_WAIT first (nonblocking readback patch); do the same, then block.
        HRESULT hr; int spins = 0;
        while ((hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)) == DXGI_ERROR_WAS_STILL_DRAWING)
            ++spins;
        Need(hr, "Map staging");
        std::vector<uint8_t> out(size_t(d.Width) * d.Height * bpp);
        for (UINT y = 0; y < d.Height; ++y)
            std::memcpy(&out[size_t(y) * d.Width * bpp], static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(d.Width) * bpp);
        context->Unmap(staging.Get(), 0);
        return out;
    }
    void Viewport(UINT w, UINT h) {
        D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1}; context->RSSetViewports(1, &vp);
    }
};

static const char* kFullscreenVs =
    "float4 main(uint id : SV_VertexID) : SV_Position {"
    " float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }";

static std::string Hex(const uint8_t* p, int n)
{
    std::string s; char b[4];
    for (int i = 0; i < n; ++i) { std::snprintf(b, sizeof b, "%02x", p[i]); s += b; }
    return s;
}

static void TestClearAndConstantColour(Gpu& g)
{
    auto rt = g.Tex(64, 64, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto rtv = g.Rtv(rt.Get());
    auto vs = g.Vs(kFullscreenVs);
    auto ps = g.Ps("cbuffer C : register(b0) { float4 colour; }; float4 main() : SV_Target { return colour; }");
    D3D11_BUFFER_DESC b{}; b.ByteWidth = 16; b.Usage = D3D11_USAGE_DYNAMIC; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> cb; Need(g.device->CreateBuffer(&b, nullptr, &cb), "cb");
    D3D11_MAPPED_SUBRESOURCE m; Need(g.context->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "map cb");
    const float c[4] = {1.0f, 0.5f, 0.25f, 1.0f}; std::memcpy(m.pData, c, 16); g.context->Unmap(cb.Get(), 0);

    const float clear[4] = {0, 0, 1, 1};
    g.context->ClearRenderTargetView(rtv.Get(), clear);
    auto cleared = g.Read(rt.Get(), 4);
    Report(Hex(&cleared[0], 4) == "ff0000ff", "clear B8G8R8A8", Hex(&cleared[0], 4));

    g.context->OMSetRenderTargets(1, One1(rtv), nullptr);
    g.Viewport(64, 64);
    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.context->IASetInputLayout(nullptr);
    g.context->VSSetShader(vs.Get(), nullptr, 0);
    g.context->PSSetShader(ps.Get(), nullptr, 0);
    g.context->PSSetConstantBuffers(0, 1, One1(cb));
    g.context->Draw(3, 0);
    auto px = g.Read(rt.Get(), 4);
    // BGRA bytes of (1, .5, .25, 1): b=0x40, g=0x80, r=0xff, a=0xff
    bool all = true;
    for (size_t i = 0; i < px.size(); i += 4) all &= Hex(&px[i], 4) == "4080ffff";
    Report(all, "draw constant colour (cbuffer, VS SV_VertexID, every pixel)", Hex(&px[0], 4));
}

static void TestBc1AndMirrorOnce(Gpu& g)
{
    // One BC1 block: colour0 = colour1 = pure red (565 0xF800), all indices 0.
    const uint8_t block[8] = {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
    auto bc1 = g.Tex(4, 4, DXGI_FORMAT_BC1_UNORM, D3D11_BIND_SHADER_RESOURCE, block, 8);
    // A 2x1 RGBA8 texture: black, white. MIRROR_ONCE at u=-0.25 lands on 0.25 -> black; at u=-0.75 -> 0.75 -> white.
    const uint32_t texels[2] = {0xff000000u, 0xffffffffu};
    auto two = g.Tex(2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, texels, 8);
    auto rt = g.Tex(4, 1, DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_RENDER_TARGET);
    auto rtv = g.Rtv(rt.Get());
    auto vs = g.Vs(kFullscreenVs);
    auto ps = g.Ps(
        "Texture2D a : register(t0); Texture2D b : register(t1); SamplerState s : register(s0);"
        "float4 main(float4 p : SV_Position) : SV_Target {"
        " if (p.x < 1) return a.Sample(s, float2(0.5, 0.5));"
        " if (p.x < 2) return b.Sample(s, float2(-0.75, 0.5));"
        " if (p.x < 3) return b.Sample(s, float2(-0.25, 0.5));"
        " return float4(0, 0, 1, 1); }");
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER; sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler; Need(g.device->CreateSamplerState(&sd, &sampler), "sampler");
    auto srvA = g.Srv(bc1.Get()), srvB = g.Srv(two.Get());
    ID3D11ShaderResourceView* srvs[2] = {srvA.Get(), srvB.Get()};
    g.context->OMSetRenderTargets(1, One1(rtv), nullptr);
    g.Viewport(4, 1);
    g.context->VSSetShader(vs.Get(), nullptr, 0);
    g.context->PSSetShader(ps.Get(), nullptr, 0);
    g.context->PSSetShaderResources(0, 2, srvs);
    g.context->PSSetSamplers(0, 1, One1(sampler));
    g.context->Draw(3, 0);
    ID3D11ShaderResourceView* none[2] = {};
    g.context->PSSetShaderResources(0, 2, none);
    auto px = g.Read(rt.Get(), 4);
    uint32_t v[4]; std::memcpy(v, px.data(), 16);
    // R10G10B10A2: r = bits 0-9, g = 10-19, b = 20-29, a = 30-31
    Report(v[0] == 0xC00003FFu, "BC1 sample into R10G10B10A2 (red)", Hex(px.data(), 4));
    Report(v[1] == 0xFFFFFFFFu, "MIRROR_ONCE u=-0.75 -> white", Hex(px.data() + 4, 4));
    Report(v[2] == 0xC0000000u, "MIRROR_ONCE u=-0.25 -> black", Hex(px.data() + 8, 4));
}

static void TestDepthStencilAndDepthSrv(Gpu& g)
{
    // The game's depth surfaces: R32G8X24_TYPELESS, DSV D32_FLOAT_S8X24_UINT, SRV R32_FLOAT_X8X24_TYPELESS.
    auto depth = g.Tex(16, 16, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> dsv; Need(g.device->CreateDepthStencilView(depth.Get(), &dd, &dsv), "dsv");
    auto colour = g.Tex(16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    auto crtv = g.Rtv(colour.Get());
    auto vs = g.Vs(
        "cbuffer C : register(b0) { float4 zc; };"
        "float4 main(uint id : SV_VertexID) : SV_Position {"
        " float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), zc.x, 1); }");
    auto ps = g.Ps("cbuffer C : register(b0) { float4 zc; }; float4 main() : SV_Target { return float4(zc.yzw, 1); }");
    D3D11_BUFFER_DESC b{}; b.ByteWidth = 16; b.Usage = D3D11_USAGE_DEFAULT; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb; Need(g.device->CreateBuffer(&b, nullptr, &cb), "cb");
    D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_LESS;
    ComPtr<ID3D11DepthStencilState> less; Need(g.device->CreateDepthStencilState(&ds, &less), "dss");
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> always; Need(g.device->CreateDepthStencilState(&ds, &always), "dss always");
    D3D11_RASTERIZER_DESC rs{}; rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> clipOn; Need(g.device->CreateRasterizerState(&rs, &clipOn), "rs");
    rs.DepthClipEnable = FALSE;
    ComPtr<ID3D11RasterizerState> clipOff; Need(g.device->CreateRasterizerState(&rs, &clipOff), "rs noclip");

    auto draw = [&](float z, float r, float gg, float bb) {
        const float c[4] = {z, r, gg, bb}; g.context->UpdateSubresource(cb.Get(), 0, nullptr, c, 0, 0);
        g.context->Draw(3, 0);
    };
    const float black[4] = {0, 0, 0, 1};
    g.context->ClearRenderTargetView(crtv.Get(), black);
    g.context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    g.context->OMSetRenderTargets(1, One1(crtv), dsv.Get());
    g.Viewport(16, 16);
    g.context->VSSetShader(vs.Get(), nullptr, 0);
    g.context->PSSetShader(ps.Get(), nullptr, 0);
    g.context->VSSetConstantBuffers(0, 1, One1(cb));
    g.context->PSSetConstantBuffers(0, 1, One1(cb));
    g.context->RSSetState(clipOn.Get());
    g.context->OMSetDepthStencilState(less.Get(), 0);
    draw(0.25f, 1, 0, 0);      // red at z=.25
    draw(0.50f, 0, 1, 0);      // green behind: must be rejected
    auto px = g.Read(colour.Get(), 4);
    Report(Hex(px.data(), 4) == "ff0000ff", "depth test LESS on D32_FLOAT_S8X24", Hex(px.data(), 4));

    // Depth clip: z = 1.5 is beyond the far plane. Clip on: nothing drawn. Clip off (the game
    // turns DepthClipEnable off for some passes): clamped and drawn (DXVK uses depthClamp
    // because MoltenVK has no VK_EXT_depth_clip_enable).
    g.context->OMSetDepthStencilState(always.Get(), 0);
    g.context->ClearRenderTargetView(crtv.Get(), black);
    draw(1.5f, 0, 0, 1);
    px = g.Read(colour.Get(), 4);
    Report(Hex(px.data(), 4) == "000000ff", "DepthClipEnable=TRUE clips z=1.5", Hex(px.data(), 4));
    g.context->RSSetState(clipOff.Get());
    draw(1.5f, 0, 0, 1);
    px = g.Read(colour.Get(), 4);
    Report(Hex(px.data(), 4) == "0000ffff", "DepthClipEnable=FALSE draws z=1.5 (clamped)", Hex(px.data(), 4));

    // Put z=.25 back, then read depth through the X8X24 SRV into an R32_FLOAT target.
    g.context->RSSetState(clipOn.Get());
    g.context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    g.context->OMSetDepthStencilState(less.Get(), 0);
    draw(0.25f, 1, 0, 0);
    g.context->OMSetRenderTargets(0, nullptr, nullptr);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    auto dsrv = g.Srv(depth.Get(), &sd);
    auto out = g.Tex(16, 16, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET);
    auto ortv = g.Rtv(out.Get());
    auto rps = g.Ps("Texture2D<float> d : register(t0); float4 main(float4 p : SV_Position) : SV_Target { return d.Load(int3(p.xy, 0)); }");
    g.context->OMSetRenderTargets(1, One1(ortv), nullptr);
    g.context->OMSetDepthStencilState(nullptr, 0);
    g.context->VSSetShader(g.Vs(kFullscreenVs).Get(), nullptr, 0);
    g.context->PSSetShader(rps.Get(), nullptr, 0);
    g.context->PSSetShaderResources(0, 1, One1(dsrv));
    g.context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr; g.context->PSSetShaderResources(0, 1, &none);
    px = g.Read(out.Get(), 4);
    float f; std::memcpy(&f, px.data(), 4);
    Report(f == 0.25f, "depth SRV R32_FLOAT_X8X24 reads back 0.25", std::to_string(f));
    g.context->RSSetState(nullptr);
}

static void TestRawBufferVertexFetch(Gpu& g)
{
    // The backend's VS reads each draw's guest vertex data from a raw (ByteAddressBuffer) ring
    // written with WRITE_DISCARD then WRITE_NO_OVERWRITE.
    D3D11_BUFFER_DESC b{}; b.ByteWidth = 4096; b.Usage = D3D11_USAGE_DYNAMIC; b.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ComPtr<ID3D11Buffer> ring; Need(g.device->CreateBuffer(&b, nullptr, &ring), "raw ring");
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R32_TYPELESS; sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.FirstElement = 0; sd.BufferEx.NumElements = 1024; sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    auto srv = g.Srv(ring.Get(), &sd);
    auto vs = g.Vs(
        "ByteAddressBuffer ring : register(t0); cbuffer C : register(b0) { uint base; uint3 pad; };"
        "struct O { float4 p : SV_Position; float4 c : COLOR; };"
        "O main(uint id : SV_VertexID) { O o; uint a = base + id * 24;"
        " o.p = float4(asfloat(ring.Load2(a)), 0, 1); o.c = float4(asfloat(ring.Load4(a + 8))); return o; }");
    auto ps = g.Ps("float4 main(float4 p : SV_Position, float4 c : COLOR) : SV_Target { return c; }");
    D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_DEFAULT; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb; Need(g.device->CreateBuffer(&cbd, nullptr, &cb), "cb");
    auto rt = g.Tex(8, 8, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET);
    auto rtv = g.Rtv(rt.Get());
    const float clear[4] = {0, 0, 0, 0};
    g.context->ClearRenderTargetView(rtv.Get(), clear);
    g.context->OMSetRenderTargets(1, One1(rtv), nullptr);
    g.Viewport(8, 8);
    g.context->VSSetShader(vs.Get(), nullptr, 0);
    g.context->PSSetShader(ps.Get(), nullptr, 0);
    g.context->VSSetShaderResources(0, 1, One1(srv));
    g.context->VSSetConstantBuffers(0, 1, One1(cb));
    auto tri = [&](D3D11_MAP mode, uint32_t offset, float r, float gg, float bb) {
        D3D11_MAPPED_SUBRESOURCE m; Need(g.context->Map(ring.Get(), 0, mode, 0, &m), "map ring");
        const float v[3][6] = {{-1, 1, r, gg, bb, 1}, {3, 1, r, gg, bb, 1}, {-1, -3, r, gg, bb, 1}};
        std::memcpy(static_cast<uint8_t*>(m.pData) + offset, v, sizeof v);
        g.context->Unmap(ring.Get(), 0);
        const uint32_t c[4] = {offset, 0, 0, 0}; g.context->UpdateSubresource(cb.Get(), 0, nullptr, c, 0, 0);
        g.context->Draw(3, 0);
    };
    tri(D3D11_MAP_WRITE_DISCARD, 0, 1, 0, 0);
    tri(D3D11_MAP_WRITE_NO_OVERWRITE, 256, 0.5f, 0.25f, 2.0f);   // second draw wins; 2.0 checks the float target
    ID3D11ShaderResourceView* none = nullptr; g.context->VSSetShaderResources(0, 1, &none);
    auto px = g.Read(rt.Get(), 8);
    // halves little-endian: 0.5 = 3800, 0.25 = 3400, 2.0 = 4000, 1.0 = 3c00
    Report(Hex(px.data(), 8) == "003800340040003c", "raw-buffer vertex fetch + NO_OVERWRITE into R16G16B16A16_FLOAT", Hex(px.data(), 8));
}

static void TestQueries(Gpu& g)
{
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ComPtr<ID3D11Query> disjoint, t0, t1, event;
    Need(g.device->CreateQuery(&qd, &disjoint), "disjoint");
    qd.Query = D3D11_QUERY_TIMESTAMP; Need(g.device->CreateQuery(&qd, &t0), "ts"); Need(g.device->CreateQuery(&qd, &t1), "ts");
    qd.Query = D3D11_QUERY_EVENT; Need(g.device->CreateQuery(&qd, &event), "event");
    auto rt = g.Tex(256, 256, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    auto rtv = g.Rtv(rt.Get());
    g.context->Begin(disjoint.Get());
    g.context->End(t0.Get());
    const float c[4] = {1, 1, 1, 1};
    for (int i = 0; i < 64; ++i) g.context->ClearRenderTargetView(rtv.Get(), c);
    g.context->End(t1.Get());
    g.context->End(disjoint.Get());
    g.context->End(event.Get());
    g.context->Flush();
    BOOL done = FALSE; auto start = std::chrono::steady_clock::now();
    while (g.context->GetData(event.Get(), &done, sizeof done, 0) != S_OK) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) break;
    }
    Report(done == TRUE, "event query completes");
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{}; UINT64 a = 0, b = 0;
    while (g.context->GetData(disjoint.Get(), &dj, sizeof dj, 0) != S_OK) {}
    while (g.context->GetData(t0.Get(), &a, sizeof a, 0) != S_OK) {}
    while (g.context->GetData(t1.Get(), &b, sizeof b, 0) != S_OK) {}
    Report(dj.Frequency > 0 && b >= a, "timestamp queries",
           "freq " + std::to_string(dj.Frequency) + " disjoint " + std::to_string(dj.Disjoint) +
           " delta " + std::to_string(b - a));
}

static Gpu CreateGpu()
{
    Gpu g;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    Need(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                           D3D11_SDK_VERSION, &g.device, &g.level, &g.context), "D3D11CreateDevice");
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC ad{};
    if (SUCCEEDED(g.device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter))) adapter->GetDesc(&ad);
    char name[128] = {}; std::wcstombs(name, ad.Description, sizeof name - 1);
    std::printf("device: %s, feature level %X\n", name, unsigned(g.level));
    ComPtr<ID3D11Device5> d5; ComPtr<ID3D11DeviceContext1> c1;
    std::printf("ID3D11Device5 (fences): %s, ID3D11DeviceContext1: %s\n",
                SUCCEEDED(g.device.As(&d5)) ? "yes" : "no", SUCCEEDED(g.context.As(&c1)) ? "yes" : "no");
    return g;
}

// Renders into an existing window. With pumpOnMain, this runs on a worker thread while the
// main thread pumps SDL events (the game's arrangement on Apple: apple_game_entry.cpp).
static int RenderWindow(SDL_Window* window, int frames, bool vsync, bool pumpOnMain)
{
    Gpu g = CreateGpu();
    int pw = 0, ph = 0; SDL_GetWindowSizeInPixels(window, &pw, &ph);
    ComPtr<IDXGIDevice> dxgi; Need(g.device.As(&dxgi), "IDXGIDevice");
    ComPtr<IDXGIAdapter> adapter; Need(dxgi->GetAdapter(&adapter), "GetAdapter");
    ComPtr<IDXGIFactory2> factory; Need(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)), "factory");
    DXGI_SWAP_CHAIN_DESC1 d{}; d.Width = UINT(pw); d.Height = UINT(ph); d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; d.Scaling = DXGI_SCALING_STRETCH;
    ComPtr<IDXGISwapChain1> swapchain;
    Need(factory->CreateSwapChainForHwnd(g.device.Get(), dxvk::wsi::toHwnd(window), &d, nullptr, nullptr, &swapchain), "CreateSwapChainForHwnd");
    ComPtr<ID3D11Texture2D> back; Need(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)), "GetBuffer");
    auto rtv = g.Rtv(back.Get());
    auto vs = g.Vs(kFullscreenVs);
    auto ps = g.Ps("cbuffer C : register(b0) { float4 t; };"
                   "float4 main(float4 p : SV_Position) : SV_Target {"
                   " float2 uv = p.xy / t.yz; return float4(0.5 + 0.5 * sin(t.x + uv.x * 6.28), uv.y, 0.5 + 0.5 * cos(t.x), 1); }");
    D3D11_BUFFER_DESC b{}; b.ByteWidth = 16; b.Usage = D3D11_USAGE_DYNAMIC; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> cb; Need(g.device->CreateBuffer(&b, nullptr, &cb), "cb");
    std::printf("swap chain %dx%d, %d frames, vsync %s\n", pw, ph, frames, vsync ? "on" : "off");
    auto start = std::chrono::steady_clock::now();
    double worst = 0; auto last = start;
    int presented = 0;
    for (int i = 0; i < frames; ++i) {
        if (!pumpOnMain) { SDL_Event e; while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) i = frames; }
        D3D11_MAPPED_SUBRESOURCE m; Need(g.context->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "map");
        const float t[4] = {i / 30.0f, float(pw), float(ph), 0}; std::memcpy(m.pData, t, 16); g.context->Unmap(cb.Get(), 0);
        g.context->OMSetRenderTargets(1, One1(rtv), nullptr);
        g.Viewport(UINT(pw), UINT(ph));
        g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.context->VSSetShader(vs.Get(), nullptr, 0);
        g.context->PSSetShader(ps.Get(), nullptr, 0);
        g.context->PSSetConstantBuffers(0, 1, One1(cb));
        g.context->Draw(3, 0);
        if (i == frames / 2) {
            // DXGI ResizeTarget -> dxvk::wsi::resizeWindow -> SDL_SetWindowSize (on the main thread
            // on Apple), then ResizeBuffers to the new pixel size, as the game does on a mode change.
            DXGI_MODE_DESC mode{}; mode.Width = 800; mode.Height = 450; mode.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            Need(swapchain->ResizeTarget(&mode), "ResizeTarget");
            int nw = 0, nh = 0; SDL_GetWindowSizeInPixels(window, &nw, &nh);
            rtv.Reset(); back.Reset();
            Need(swapchain->ResizeBuffers(0, UINT(nw), UINT(nh), DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
            Need(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)), "GetBuffer");
            rtv = g.Rtv(back.Get());
            std::printf("resized: window %dx%d px (was %dx%d)\n", nw, nh, pw, ph);
            pw = nw; ph = nh;
        }
        HRESULT hr = swapchain->Present(vsync ? 1 : 0, 0);
        if (FAILED(hr)) { std::fprintf(stderr, "Present failed %08x at frame %d\n", unsigned(hr), i); break; }
        ++presented;
        auto now = std::chrono::steady_clock::now();
        if (i > 5) worst = std::max(worst, std::chrono::duration<double, std::milli>(now - last).count());
        last = now;
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("presented %d frames in %.2f s: %.1f fps (worst frame %.1f ms)\n", presented, secs, presented / secs, worst);
    rtv.Reset(); back.Reset(); swapchain.Reset();
    g.context->ClearState(); g.context->Flush();
    g.context.Reset(); g.device.Reset();
    return presented == frames ? 0 : 1;
}

static int RunWindow(int frames, bool vsync, bool threaded)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* window = SDL_CreateWindow("d3d11 smoke", 960, 540, SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    int result = 1;
    if (threaded) {
        std::printf("device, swap chain and presents on a worker thread; main thread pumps events\n");
        std::atomic<bool> done{false};
        std::thread worker([&] { result = RenderWindow(window, frames, vsync, true); done = true; });
        while (!done) { SDL_Event e; while (SDL_PollEvent(&e)) {} SDL_Delay(1); }
        worker.join();
    } else {
        result = RenderWindow(window, frames, vsync, false);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--window") == 0) {
        int frames = argc > 2 && argv[2][0] != '-' ? std::atoi(argv[2]) : 300;
        bool vsync = true, threaded = false;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--no-vsync") == 0) vsync = false;
            if (std::strcmp(argv[i], "--thread") == 0) threaded = true;
        }
        return RunWindow(frames, vsync, threaded);
    }
    Gpu g = CreateGpu();
    TestClearAndConstantColour(g);
    TestBc1AndMirrorOnce(g);
    TestDepthStencilAndDepthSrv(g);
    TestRawBufferVertexFetch(g);
    TestQueries(g);
    g.context->ClearState(); g.context->Flush();
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
