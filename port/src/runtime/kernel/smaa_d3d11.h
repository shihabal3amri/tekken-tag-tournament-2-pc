#pragma once
// SMAA 1x, high preset. Post-process only; no temporal jitter or simulation changes.
#include <d3d11.h>
#include <d3dcompiler.h>
#if defined(__linux__)
#include "dxvk_native_com_ptr.h"
#else
#include <wrl/client.h>
#endif
#include <stdexcept>
#include <string>
#include <cstring>
#include <cstdio>
#include "../../../third_party/smaa/shader_source.h"
namespace smaa_vendor {
#include "../../../third_party/smaa/AreaTex.h"
#include "../../../third_party/smaa/SearchTex.h"
}
namespace ttt2_smaa {
using Microsoft::WRL::ComPtr;
inline void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string("SMAA: ") + operation);
}
class Pass {
    ComPtr<ID3D11VertexShader> vs_[3];
    ComPtr<ID3D11PixelShader> ps_[3];
    ComPtr<ID3D11SamplerState> samplers_[2];
    ComPtr<ID3D11Buffer> metrics_;
    ComPtr<ID3D11ShaderResourceView> area_, search_;
    struct Target {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
    } edges_, weights_;
    UINT width_ = 0, height_ = 0;
    bool reported_ = false;
    static ComPtr<ID3DBlob> Compile(const std::string& source, const char* entry, const char* target) {
        ComPtr<ID3DBlob> code, error;
        const HRESULT hr = D3DCompile(source.data(), source.size(), "SMAA 1x", nullptr, nullptr,
            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &error);
        if (FAILED(hr)) throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer()) : "SMAA compile failed");
        return code;
    }
    static void Lookup(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format,
                       const void* bytes, UINT pitch, ID3D11ShaderResourceView** out) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = format; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{bytes, pitch, 0};
        ComPtr<ID3D11Texture2D> texture;
        Check(device->CreateTexture2D(&desc, &data, &texture), "lookup texture");
        Check(device->CreateShaderResourceView(texture.Get(), nullptr, out), "lookup view");
    }
    static Target MakeTarget(ID3D11Device* device, UINT width, UINT height) {
        Target target;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&desc, nullptr, &target.texture), "scratch texture");
        Check(device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.rtv), "scratch RTV");
        Check(device->CreateShaderResourceView(target.texture.Get(), nullptr, &target.srv), "scratch SRV");
        return target;
    }
    void Init(ID3D11Device* device) {
        if (metrics_) return;
        const std::string source = std::string(
            "cbuffer Metrics : register(b0) { float4 rtMetrics; };\n"
            "#define SMAA_RT_METRICS rtMetrics\n#define SMAA_HLSL_4 1\n#define SMAA_PRESET_HIGH 1\n") + smaa_vendor::Shader + R"HLSL(
Texture2D tex0 : register(t0); Texture2D tex1 : register(t1); Texture2D tex2 : register(t2);
struct V { float4 position:SV_Position; float2 uv:TEXCOORD0; float2 pixel:TEXCOORD1; float4 offset[3]:TEXCOORD2; };
V Base(uint id) { V v=(V)0; v.uv=float2((id<<1)&2,id&2); v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1); return v; }
V EdgeVS(uint id:SV_VertexID) { V v=Base(id); SMAAEdgeDetectionVS(v.uv,v.offset); return v; }
V WeightVS(uint id:SV_VertexID) { V v=Base(id); SMAABlendingWeightCalculationVS(v.uv,v.pixel,v.offset); return v; }
V BlendVS(uint id:SV_VertexID) { V v=Base(id); SMAANeighborhoodBlendingVS(v.uv,v.offset[0]); return v; }
float4 EdgePS(V v):SV_Target { return float4(SMAALumaEdgeDetectionPS(v.uv,v.offset,tex0),0,0); }
float4 WeightPS(V v):SV_Target { return SMAABlendingWeightCalculationPS(v.uv,v.pixel,v.offset,tex0,tex1,tex2,0); }
float4 BlendPS(V v):SV_Target { return SMAANeighborhoodBlendingPS(v.uv,v.offset[0],tex0,tex1); }
)HLSL";
        const char* vertex[] = {"EdgeVS", "WeightVS", "BlendVS"};
        const char* pixel[] = {"EdgePS", "WeightPS", "BlendPS"};
        for (int i = 0; i < 3; ++i) {
            auto v = Compile(source, vertex[i], "vs_5_0");
            Check(device->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs_[i]), "vertex shader");
            auto p = Compile(source, pixel[i], "ps_5_0");
            Check(device->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps_[i]), "pixel shader");
        }
        Lookup(device, AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM,
               smaa_vendor::areaTexBytes, AREATEX_PITCH, &area_);
        Lookup(device, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM,
               smaa_vendor::searchTexBytes, SEARCHTEX_PITCH, &search_);
        for (int i = 0; i < 2; ++i) {
            D3D11_SAMPLER_DESC desc{};
            desc.Filter = i ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.MaxLOD = D3D11_FLOAT32_MAX;
            Check(device->CreateSamplerState(&desc, &samplers_[i]), "sampler");
        }
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 16; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        // Commit the initialization sentinel only after all other resources succeed.
        Check(device->CreateBuffer(&desc, nullptr, &metrics_), "metrics");
    }
public:
    // Caller supplies a neutral blend/depth/raster state. This method changes VS/PS,
    // b0, s0-s1, t0-t2, RTV, viewport, scissor and input layout; caller must rebind them.
    void Run(ID3D11Device* device, ID3D11DeviceContext* context,
             ID3D11ShaderResourceView* input, ID3D11RenderTargetView* output, UINT width, UINT height) {
        Init(device);
        if (width != width_ || height != height_) {
            auto edges = MakeTarget(device, width, height);
            auto weights = MakeTarget(device, width, height);
            edges_ = std::move(edges); weights_ = std::move(weights); width_ = width; height_ = height;
        }
        const float metrics[] = {1.0f/width, 1.0f/height, float(width), float(height)};
        context->UpdateSubresource(metrics_.Get(), 0, nullptr, metrics, 0, 0);
        ID3D11Buffer* cb = metrics_.Get();
        context->VSSetConstantBuffers(0, 1, &cb); context->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* samplers[] = {samplers_[0].Get(),samplers_[1].Get()};
        context->PSSetSamplers(0, 2, samplers);
        context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};
        D3D11_RECT scissor{0,0,LONG(width),LONG(height)};
        context->RSSetViewports(1, &viewport); context->RSSetScissorRects(1, &scissor);
        ID3D11ShaderResourceView* empty[3]{};
        context->PSSetShaderResources(0, 3, empty);
        const float clear[4]{};
        context->ClearRenderTargetView(edges_.rtv.Get(), clear);
        context->ClearRenderTargetView(weights_.rtv.Get(), clear);
        for (int pass = 0; pass < 3; ++pass) {
            context->PSSetShaderResources(0, 3, empty);
            ID3D11RenderTargetView* target = pass==0 ? edges_.rtv.Get() : pass==1 ? weights_.rtv.Get() : output;
            context->OMSetRenderTargets(1, &target, nullptr);
            ID3D11ShaderResourceView* resources[3] = {pass==1 ? edges_.srv.Get() : input,
                pass==1 ? area_.Get() : pass==2 ? weights_.srv.Get() : nullptr,
                pass==1 ? search_.Get() : nullptr};
            context->PSSetShaderResources(0, 3, resources);
            context->VSSetShader(vs_[pass].Get(), nullptr, 0); context->PSSetShader(ps_[pass].Get(), nullptr, 0);
            context->Draw(3, 0);
        }
        context->PSSetShaderResources(0, 3, empty);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        if (!reported_) {
            std::fprintf(stderr, "[display] SMAA 1x HIGH: all three passes active at %ux%u\n", width, height);
            reported_ = true;
        }
    }
};
}
