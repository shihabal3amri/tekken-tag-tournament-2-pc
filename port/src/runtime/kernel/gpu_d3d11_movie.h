#pragma once
// Opt-in, isolated large YUV draw offload. Include after Vertex and Surface in
// gpu_render.cpp, under TTT2_D3D11_MOVIE_BACKEND. Everything else keeps using the
// software renderer. CPU EDRAM is committed only after successful GPU readback.
#include "gpu_adapter.h"
#include "gpu_shader_subset.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <limits>
#include <memory>

namespace ttt2_movie {
using Microsoft::WRL::ComPtr;
struct DeviceFailure : std::runtime_error {using std::runtime_error::runtime_error;};
inline std::atomic<bool> deviceFailed{false};
inline void Check(HRESULT hr,const char* what) {if(FAILED(hr))throw DeviceFailure(std::string(what)+" HRESULT "+std::to_string(uint32_t(hr)));}
struct NativeVertex {float position[4];float attributes[16][4];};
struct Constants {float values[256][4];float alpha;float padding[3];};
struct Plane {Texture info;std::vector<uint8_t> snapshot;ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;};
struct Prepared {
    uint32_t width=0,height=0;
    std::array<Texture,3> planes;
    std::array<NativeVertex,6> vertices;
};
inline bool Prepare(const uint32_t* regs,const std::vector<uint32_t>& code,const Vertex* quad,uint32_t count,const Surface& surface,Prepared& p) {
    if(count!=4 || !quad || code.size()!=33)return false;
    p.width=regs[0x2000]&0x3FFF;p.height=(regs[0x2082]>>16)&0x3FFF;
    if(p.width<640 || p.width>1920 || p.height<360 || p.height>1080)return false;
    if((regs[0x2000]&(3<<16)) || (regs[0x2001]&~0xFFFu) || (regs[0x2200]&3) || (regs[0x2205]&3) || (regs[0x2202]&8))return false;
    if(regs[0x2201]!=0x00010001 || (regs[0x2104]&0xFFFF)!=15 || regs[0x2081] || (regs[0x2082]&0x3FFF)!=p.width)return false;
    if(surface.px.size()<size_t(p.width)*p.height)return false;
    unsigned corner[4],mask=0;
    for(unsigned i=0;i<4;++i) {
        const auto& v=quad[i];
        if(!v.valid || !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(v.invW) || v.invW<=0 || v.z<0 || v.z>1)return false;
        // This pilot accepts exact full-screen corners only. No guessing at
        // clipping, offsets, strip order, arbitrary triangles or partial covers.
        if((v.x!=0 && v.x!=p.width) || (v.y!=0 && v.y!=p.height))return false;
        corner[i]=(v.x!=0?1:0)|(v.y!=0?2:0);mask|=1u<<corner[i];
        for(const auto& a:v.attr)for(float value:a.c)if(!std::isfinite(value))return false;
    }
    if(mask!=15 || (corner[0]^corner[2])!=3 || (corner[1]^corner[3])!=3)return false;
    static constexpr unsigned indices[]={0,1,2,0,2,3};
    for(unsigned i=0;i<6;++i) {
        const auto& v=quad[indices[i]];auto& n=p.vertices[i];float w=1/v.invW;
        if(!std::isfinite(w))return false;
        n.position[0]=(v.x*2/p.width-1)*w;n.position[1]=(1-v.y*2/p.height)*w;n.position[2]=v.z*w;n.position[3]=w;
        for(unsigned a=0;a<16;++a)for(unsigned c=0;c<4;++c)n.attributes[a][c]=v.attr[a][c]*w;
        for(float value:n.position)if(!std::isfinite(value))return false;
        for(const auto& a:n.attributes)for(float value:a)if(!std::isfinite(value))return false;
    }
    for(unsigned i=0;i<3;++i) {
        auto& t=p.planes[i];t=DecodeTexture(regs,i);
        if(!t.valid || t.format!=FMT_8 || t.tiled || t.endian || t.pitch<t.width || t.pitch>8192)return false;
        if(t.width!=(i?p.width/2:p.width) || t.height!=(i?p.height/2:p.height))return false;
        uint64_t size=uint64_t(t.pitch)*(t.height-1)+t.width;
        if(!size || size>16*1024*1024 || uint64_t(t.base)+size>0x100000000ull)return false;
    }
    return true;
}
inline ComPtr<ID3DBlob> Compile(const std::string& text,const char* target) {
    ComPtr<ID3DBlob> bytecode,error;
    HRESULT hr=D3DCompile(text.data(),text.size(),"TTT2 translated movie shader",nullptr,nullptr,"main",target,D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&bytecode,&error);
    if(FAILED(hr) && error)throw DeviceFailure(std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()));
    Check(hr,"D3DCompile");return bytecode;
}
struct Backend {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertices,constants;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11SamplerState> linear,point;
    ComPtr<ID3D11Texture2D> target,staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    std::array<Plane,3> planes;
    std::vector<uint32_t> readback;
    std::string shaderText;
    uint32_t width=0,height=0;
    uint64_t draws=0;
    void Initialize() {
        if(device)return;
        // A failed initialization is discarded as a whole by the caller; a
        // half-initialized device can never be used on the next draw.
        D3D_FEATURE_LEVEL actual;const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        // The same GPU as the renderer (gpu_adapter.h).
        const Microsoft::WRL::ComPtr<IDXGIAdapter1> preferred = PreferredGpuAdapter();
        HRESULT created = D3D11CreateDevice(preferred.Get(), preferred ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                            D3D11_SDK_VERSION, &device, &actual, &context);
        if (FAILED(created) && preferred)
            created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, &actual, &context);
        Check(created, "D3D11CreateDevice");
        auto code=Compile("struct V{float4 p:POSITION;float4 a[16]:TEXCOORD0;};struct O{float4 p:SV_Position;float4 a[16]:TEXCOORD0;};O main(V v){O o;o.p=v.p;[unroll]for(int i=0;i<16;++i)o.a[i]=v.a[i];return o;}","vs_5_0");
        Check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs),"CreateVertexShader");
        D3D11_INPUT_ELEMENT_DESC elements[17]{};elements[0]={"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
        for(unsigned i=0;i<16;++i)elements[i+1]={"TEXCOORD",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16+i*16,D3D11_INPUT_PER_VERTEX_DATA,0};
        Check(device->CreateInputLayout(elements,17,code->GetBufferPointer(),code->GetBufferSize(),&layout),"CreateInputLayout");
        D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(NativeVertex)*6;b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_VERTEX_BUFFER;Check(device->CreateBuffer(&b,nullptr,&vertices),"CreateVertexBuffer");
        b.ByteWidth=sizeof(Constants);b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Check(device->CreateBuffer(&b,nullptr,&constants),"CreateConstants");
        D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;r.CullMode=D3D11_CULL_NONE;r.DepthClipEnable=TRUE;
        Check(device->CreateRasterizerState(&r,&raster),"CreateRasterizer");
        D3D11_DEPTH_STENCIL_DESC d{};d.DepthEnable=FALSE;d.StencilEnable=FALSE;Check(device->CreateDepthStencilState(&d,&depth),"CreateDepthState");
        D3D11_BLEND_DESC blendDesc{};blendDesc.RenderTarget[0].RenderTargetWriteMask=15;
        Check(device->CreateBlendState(&blendDesc,&blend),"CreateBlendState");
        D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;s.MaxLOD=D3D11_FLOAT32_MAX;
        Check(device->CreateSamplerState(&s,&linear),"CreateLinearSampler");s.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;Check(device->CreateSamplerState(&s,&point),"CreatePointSampler");
    }
    void Configure(const Prepared& p,const std::string& hlsl) {
        Initialize();
        if(hlsl!=shaderText) {
            auto code=Compile(hlsl,"ps_5_0");ComPtr<ID3D11PixelShader> next;
            Check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&next),"CreatePixelShader");
            ps=next;shaderText=hlsl; // Includes instruction, format/swizzle and alpha policy.
        }
        if(width!=p.width || height!=p.height) {
            D3D11_TEXTURE2D_DESC t{};t.Width=p.width;t.Height=p.height;t.MipLevels=t.ArraySize=1;t.Format=DXGI_FORMAT_B8G8R8A8_UNORM;t.SampleDesc.Count=1;t.Usage=D3D11_USAGE_DEFAULT;t.BindFlags=D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> color,read;ComPtr<ID3D11RenderTargetView> view;
            Check(device->CreateTexture2D(&t,nullptr,&color),"CreateTarget");Check(device->CreateRenderTargetView(color.Get(),nullptr,&view),"CreateTargetView");
            t.Usage=D3D11_USAGE_STAGING;t.BindFlags=0;t.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Check(device->CreateTexture2D(&t,nullptr,&read),"CreateReadback");
            readback.resize(size_t(p.width)*p.height);target=color;staging=read;rtv=view;width=p.width;height=p.height;
        }
        for(unsigned i=0;i<3;++i) {
            auto& plane=planes[i];const auto& t=p.planes[i];
            if(!plane.texture || plane.info.width!=t.width || plane.info.height!=t.height) {
                D3D11_TEXTURE2D_DESC desc{};desc.Width=t.width;desc.Height=t.height;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;
                Check(device->CreateTexture2D(&desc,nullptr,&texture),"CreatePlane");Check(device->CreateShaderResourceView(texture.Get(),nullptr,&view),"CreatePlaneView");plane.texture=texture;plane.view=view;
            }
            plane.info=t;plane.snapshot.resize(size_t(t.pitch)*(t.height-1)+t.width);
            SIZE_T bytes=0;
            if(!ReadProcessMemory(GetCurrentProcess(),g_base+t.base,plane.snapshot.data(),plane.snapshot.size(),&bytes) || bytes!=plane.snapshot.size())throw std::runtime_error("Movie plane is unreadable");
        }
    }
    bool Draw(const uint32_t* regs,const Prepared& p,const std::string& hlsl,Surface& surface) {
        Configure(p,hlsl);
        Constants c{};std::memcpy(c.values,regs+0x4400,4096);c.alpha=AsFloat(regs[0x210E]);
        context->UpdateSubresource(constants.Get(),0,nullptr,&c,0,0);context->UpdateSubresource(vertices.Get(),0,nullptr,p.vertices.data(),0,0);
        for(auto& plane:planes)context->UpdateSubresource(plane.texture.Get(),0,nullptr,plane.snapshot.data(),plane.info.pitch,0);
        // Even though the accepted state is a full overwrite, uploading current
        // CPU pixels makes untouched samples deterministic if clipping changes.
        context->UpdateSubresource(target.Get(),0,nullptr,surface.px.data(),width*4,0);
        auto view=rtv.Get();context->OMSetRenderTargets(1,&view,nullptr);context->OMSetDepthStencilState(depth.Get(),0);context->OMSetBlendState(blend.Get(),nullptr,~0u);
        context->RSSetState(raster.Get());D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&viewport);
        context->IASetInputLayout(layout.Get());auto vertexBuffer=vertices.Get();UINT stride=sizeof(NativeVertex),offset=0;context->IASetVertexBuffers(0,1,&vertexBuffer,&stride,&offset);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);auto constantBuffer=constants.Get();context->PSSetConstantBuffers(0,1,&constantBuffer);
        ID3D11ShaderResourceView* views[]={planes[0].view.Get(),planes[1].view.Get(),planes[2].view.Get()};context->PSSetShaderResources(0,3,views);
        ID3D11SamplerState* samplers[]={linear.Get(),point.Get()};context->PSSetSamplers(0,2,samplers);
        context->Draw(6,0);context->CopyResource(staging.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"MapMovieReadback");
        uint32_t anyColor=0;
        for(unsigned y=0;y<height;++y) {
            auto row=readback.data()+size_t(y)*width;
            std::memcpy(row,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,width*4);
            for(unsigned x=0;x<width;++x)anyColor|=row[x];
        }
        context->Unmap(staging.Get(),0);
        Check(device->GetDeviceRemovedReason(),"Movie device status");
        // Transaction boundary. There are no throwing operations after this:
        // failure anywhere above leaves all CPU pixels and samplePitch intact.
        std::memcpy(surface.px.data(),readback.data(),readback.size()*4);surface.samplePitch=width;
        if(anyColor&0x00FFFFFF)++g_litDraws;
        ++draws;return true;
    }
};
} // namespace ttt2_movie

inline bool D3D11MovieEnabled() {
    static const bool enabled=[] {const char* p=std::getenv("TTT2_D3D11_MOVIE");return p && std::strcmp(p,"1")==0;}();
    return enabled && !ttt2_movie::deviceFailed.load(std::memory_order_relaxed);
}
inline bool TryD3D11MovieDraw(const uint32_t* regs,const std::vector<uint32_t>& psCode,const Vertex* quad,uint32_t count,Surface& surface) {
    if(!D3D11MovieEnabled())return false;
    ttt2_movie::Prepared prepared;
    if(!ttt2_movie::Prepare(regs,psCode,quad,count,surface,prepared))return false;
    static std::mutex mutex;std::lock_guard lock(mutex);
    static std::unique_ptr<ttt2_movie::Backend> backend;
    static unsigned errors=0;
    try {
        auto translated=ttt2_d3d11::Translate(psCode,regs,true);
        if(translated.operations!=8 || translated.textures!=std::set<unsigned>{0,1,2})return false;
        if(!backend)backend=std::make_unique<ttt2_movie::Backend>();
        bool result=backend->Draw(regs,prepared,translated.hlsl,surface);
        if(backend->draws==1)std::fprintf(stderr,"[d3d11-movie] active %ux%u translated R8 YUV draw\n",prepared.width,prepared.height);
        return result;
    }catch(const ttt2_movie::DeviceFailure& e) {
        backend.reset();ttt2_movie::deviceFailed.store(true,std::memory_order_relaxed);
        std::fprintf(stderr,"[d3d11-movie] disabled after device/compiler failure: %s\n",e.what());
        return false;
    }catch(const std::exception& e) {
        backend.reset();
        if(errors++<4)std::fprintf(stderr,"[d3d11-movie] software fallback: %s\n",e.what());
        return false;
    }
}
