#pragma once
// Hardware (Direct3D 11) backend, stage 1.
//
// The CPU keeps what is cheap and already validated: PM4 decoding, the vertex shader
// (interpreter), primitive assembly, rectangle synthesis and culling. The GPU does everything
// per pixel: rasterization, the translated pixel shader, alpha test, depth, blending, EDRAM
// surfaces, resolves and presentation. Nothing is read back in the steady state.
//
// EDRAM model: the same as the CPU renderer's "EDRAM-lite" (one store per colour/depth base,
// sample-space layout, pitch * samples-per-row), held in fixed-size GPU textures.
// Resolves copy into GPU textures registered by destination address; texture fetches of that
// address use them directly. Guest memory under a resolve is NOT written unless something
// needs the bytes (a mismatched fetch, TTT2_GPU_HW_WRITEBACK=1, or compare mode).
// CPU-written textures are decoded from guest memory and invalidated with Windows write
// watch (guest memory is allocated MEM_WRITE_WATCH in main.cpp).
//
// TTT2_GPU_HW=1 enables it; TTT2_GPU_HW=compare also runs the CPU renderer and compares every
// resolve. Include inside gpu_render.cpp after Vertex/Surface/Texture and the interpreter.
#include "gpu_hw_translate.h"
#include "gpu_adapter.h"
#include "display_settings.h"
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <condition_variable>
#include <deque>
#include <cmath>
#include <limits>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <intrin.h>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

extern void (*g_gpuFrameNote)(const char*);   // gpu_render.cpp: the frame log (framelog.trigger)
extern HANDLE g_mainThreadHandle;               // main.cpp: the game's main thread (frame cost reports)
void GpuNoteMemory(int kind, uint32_t addr, uint32_t bytes);   // kernel_video.cpp (TTT2_RB_GPUMEM diagnostics)
bool DirtyMeasureOn();                          // rollback_probe.cpp: the write-watch bits are its
void WriteWatchPublish(int self, const PVOID* pages, size_t count);                          // write_watch.cpp
void WriteWatchDrain(int self, uint32_t firstPage, uint32_t lastPage, std::vector<uint32_t>& out);

namespace ttt2_hw {
using Microsoft::WRL::ComPtr;

enum class Mode { Off = 0, On = 1, Compare = 2 };

struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
// TTT2_D3D_CALLS=1 (diagnostics): every checked D3D call counted by its label, printed with the
// 5-second stats (which creations / maps a frame makes).
inline bool CallCountOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_D3D_CALLS"); return e && *e == '1'; }();
    return on;
}
inline std::mutex g_callCountMutex;
inline std::map<std::string, uint64_t> g_callCounts;
inline void Check(HRESULT hr, const char* what)
{
    if (CallCountOn())
    {
        std::lock_guard l(g_callCountMutex);
        ++g_callCounts[what];
    }
    if (FAILED(hr))
    {
        char text[160];
        std::snprintf(text, sizeof(text), "%s failed: HRESULT %08X", what, (unsigned)hr);
        throw Failure(text);
    }
}

inline uint64_t Fnv(const void* data, size_t size, uint64_t h = 1469598103934665603ull)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

inline Mode RequestedMode()
{
    static const Mode mode = [] {
        const char* e = std::getenv("TTT2_GPU_HW");
        if (!e) return Mode::Off;
        if (std::strcmp(e, "1") == 0) return Mode::On;
        if (std::strcmp(e, "compare") == 0) return Mode::Compare;
        return Mode::Off;
    }();
    return mode;
}
inline std::atomic<bool> g_failed{ false };
inline Mode ActiveMode() { return g_failed.load(std::memory_order_relaxed) ? Mode::Off : RequestedMode(); }

// ---------------------------------------------------------------------------------------
// Write-watch based dirty tracking. Every Sync consumes the OS write-watch bits of a range
// and stamps the written pages with a fresh sequence number, so several consumers can ask
// "written since sequence N?" about overlapping ranges without stealing each other's bits.
// rollback.cpp reads the bits too: write_watch.cpp passes each side the pages the other read.
// Measured cost (2026-10-10, this machine): first guest write to a page after a reset
// ~0.45 us, a 4 MB range query ~11 us. Only ranges actually used as textures are reset.
// ---------------------------------------------------------------------------------------
class DirtyTracker
{
public:
    uint32_t Sync(uint32_t start, uint32_t size)
    {
        if (!size) return seq_;
        const uint64_t first = start >> 12, last = (uint64_t(start) + size + 4095) >> 12;
        if (pageSeq_.empty()) pageSeq_.assign(1u << 20, 0);
        const size_t pages = size_t(last - first);
        if (addresses_.size() < pages) addresses_.resize(pages);
        ULONG_PTR count = pages;
        DWORD granularity = 0;
        if (!usable_ || DirtyMeasureOn()) { MarkAll(first, last); return seq_; }
        UINT r = GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_base + (first << 12), pages << 12,
                               addresses_.data(), &count, &granularity);
        if (r != 0)
        {
            usable_ = false;      // guest memory without MEM_WRITE_WATCH: treat as always dirty
            std::fprintf(stderr, "[gpu-hw] write watch unavailable (%lu); textures re-upload on every use\n",
                         GetLastError());
            MarkAll(first, last);
            return seq_;
        }
        WriteWatchPublish(0, addresses_.data(), count);       // consumer 0: the GPU backend
        shared_.clear();
        WriteWatchDrain(0, uint32_t(first), uint32_t(last), shared_);   // pages rollback.cpp read
        if (count || !shared_.empty())
        {
            ++seq_;
            for (ULONG_PTR i = 0; i < count; ++i)
            {
                const uint64_t page = (static_cast<uint8_t*>(addresses_[i]) - g_base) >> 12;
                if (page < pageSeq_.size()) pageSeq_[page] = seq_;
            }
            for (uint32_t page : shared_)
                if (page < pageSeq_.size()) pageSeq_[page] = seq_;
        }
        return seq_;
    }
    // Highest write sequence on any page of the range (Sync it first).
    uint32_t Newest(uint32_t start, uint32_t size) const
    {
        if (pageSeq_.empty() || !size) return 0;
        uint32_t newest = 0;
        for (uint64_t p = start >> 12, e = (uint64_t(start) + size + 4095) >> 12; p < e && p < pageSeq_.size(); ++p)
            newest = std::max(newest, pageSeq_[p]);
        return newest;
    }
    uint32_t Current() const { return seq_; }
private:
    void MarkAll(uint64_t first, uint64_t last)
    {
        ++seq_;
        for (uint64_t p = first; p < last && p < pageSeq_.size(); ++p) pageSeq_[p] = seq_;
    }
    std::vector<uint32_t> pageSeq_;
    std::vector<PVOID> addresses_;
    std::vector<uint32_t> shared_;
    uint32_t seq_ = 1;
    bool usable_ = true;
};

// One CPU-assembled vertex in clip space plus the 16 interpolators (vertex-shader exports).
struct HwVertex { float pos[4]; float attr[16][4]; };

struct DrawState
{
    // colorBase: EDRAM base (RB_COLOR_INFO bits 0-11) | colour format (bits 16-19) << 12.
    uint32_t pitch = 0, sampX = 1, sampY = 1, colorBase = 0, depthBase = 0, writeMask = 0;
    bool zEnable = false, zWrite = false;
    uint32_t zFunc = 7;
    uint32_t blend = 0x00010001;
    float blendConst[4] = { 0, 0, 0, 0 };
    int scX0 = 0, scY0 = 0, scX1 = 0, scY1 = 0;
    bool alphaTest = false;
    uint32_t alphaFunc = 7;
    float alphaRef = 0;
    uint32_t cull = 0;            // PA_SU_SC_MODE_CNTL bits 0/1: cull front/back
    bool frontIsCW = false;       // bit 2: clockwise (on screen) is the front face
    bool depthClip = false;       // PA_CL_CLIP_CNTL clipping enabled: clip 0 <= z <= w like Xenos
    // Stencil: RB_DEPTHCONTROL (0x2200) bit 0 enable, bit 7 separate back-face state, funcs and
    // ops in bits 8-31; RB_STENCILREFMASK (0x210D) and _BF (0x210C): ref, read mask, write mask.
    bool stencil = false;
    uint32_t depthCtl = 0, stencilRefMask = 0, stencilRefMaskBf = 0;
};

// The logical EDRAM surface space in samples (the vertex transform's frame): wide enough for
// pitch 2048 at 2x horizontal samples, as tall as the CPU store's 2048 sample rows. Textures
// hold only the part in use, times the render scale (Backend::Grow).
constexpr UINT SurfaceW = 4096, SurfaceH = 2048;

struct PsStateCB
{
    uint32_t boolConsts[8];
    uint32_t loopConsts[32];
    uint32_t texInfo[32][4];
    float texScale[32][4];
    uint32_t texSampler[32][4];      // ClampMode u, v; bilinear; border colour (XAddr / TexAddress)
    float alphaRef; uint32_t alphaTest, alphaFunc, pad;
};

// Fetch-constant sampler state in the translator's texSampler layout.
inline void SamplerInfo(const Texture& t, uint32_t out[4])
{
    out[0] = t.valid ? t.clampX : 2u; out[1] = t.valid ? t.clampY : 2u;
    out[2] = t.valid && t.magFilter == 1 ? 1u : 0u; out[3] = t.valid ? t.border : 0u;
}

// The fetch constant's minification state for the D3D11 sampler (the tfetch instructions all defer to
// it): bit 0 min filter linear, 1-2 mip filter (0 point, 1 linear, 2 none), 3-5 anisotropic filter
// (0 off, 1..5 = 1:1..16:1), 6-9 / 10-13 first / last level (the last clamped to the levels the texture
// holds), 14-23 the LOD bias in 1/32 steps (signed; Xenia's sampler MipLODBias = lod_bias / 32).
inline uint32_t MipSampling(const Texture& t, uint32_t levels)
{
    const uint32_t maxLevel = levels > 1 && t.mipFilter != 2 ? std::min<uint32_t>(t.mipMax, levels - 1) : 0;
    return (t.minFilter == 0 ? 0u : 1u) | (std::min<uint32_t>(t.mipFilter, 2) << 1) | (std::min<uint32_t>(t.aniso, 5) << 3) |
           (std::min(t.mipMin, maxLevel) << 6) | (maxLevel << 10) | ((uint32_t(t.lodBias) & 0x3FF) << 14);
}

// The D3D11 sampler for that state (mirror-clamp-to-halfway/border as mirror once, as Xenia).
// userAniso: Options -> Display Settings' anisotropic filtering (0 off), for linear-minified mipped textures.
inline D3D11_SAMPLER_DESC SamplerDesc(const uint32_t info[4], uint32_t mip = 0, uint32_t userAniso = 0)
{
    auto mode = [](uint32_t m) {
        switch (m)
        {
        case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
        case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case 3: case 5: case 7: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
        case 6: return D3D11_TEXTURE_ADDRESS_BORDER;
        default: return D3D11_TEXTURE_ADDRESS_CLAMP;
        }
    };
    D3D11_SAMPLER_DESC s{};
    const bool minLinear = mip & 1;
    const uint32_t mipMode = (mip >> 1) & 3, aniso = (mip >> 3) & 7, minLevel = (mip >> 6) & 0xF, maxLevel = (mip >> 10) & 0xF;
    const int32_t bias = int32_t(mip << 8) >> 22;
    s.Filter = D3D11_ENCODE_BASIC_FILTER(minLinear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                         info[2] ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                         mipMode == 1 ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                         D3D11_FILTER_REDUCTION_TYPE_STANDARD);
    const uint32_t maxAniso = std::max<uint32_t>(aniso ? 1u << (aniso - 1) : 1u, userAniso);
    if (maxAniso > 1 && maxLevel > 0 && minLinear) { s.Filter = D3D11_FILTER_ANISOTROPIC; s.MaxAnisotropy = std::min<uint32_t>(maxAniso, 16); }
    s.AddressU = mode(info[0]); s.AddressV = mode(info[1]); s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MinLOD = float(std::min(minLevel, maxLevel)); s.MaxLOD = float(maxLevel);
    s.MipLODBias = std::clamp(float(bias) / 32.0f, -16.0f, 15.99f);
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    for (float& c : s.BorderColor) c = info[3] == 1 ? 1.0f : 0.0f;
    return s;
}

class Backend
{
    struct ColorSurface;
public:
    // ----------------------------------------------------------------- lifetime
    void Initialize()
    {
        if (device_) return;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (const char* d = std::getenv("TTT2_GPU_HW_DEBUG"); d && std::strcmp(d, "1") == 0)
            flags |= D3D11_CREATE_DEVICE_DEBUG;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL actual{};
        const ComPtr<IDXGIAdapter1> preferred = PreferredGpuAdapter();
        HRESULT created = D3D11CreateDevice(preferred.Get(), preferred ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                            levels, 2, D3D11_SDK_VERSION, &device_, &actual, &context_);
        if (FAILED(created) && preferred)               // that adapter refused: the default one, as before
            created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device_, &actual, &context_);
        Check(created, "D3D11CreateDevice");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adesc{};
        if (SUCCEEDED(device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
            adapter->GetDesc(&adesc);
        char name[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
        std::fprintf(stderr, "[gpu-hw] active on %s (feature level %X), mode=%s\n", name, (unsigned)actual,
                     RequestedMode() == Mode::Compare ? "compare" : "on");
        ComPtr<ID3D11Device5> device5;
        const char* fenceEnv = std::getenv("TTT2_READBACK_FENCE");
        if (!(fenceEnv && *fenceEnv == '0') && SUCCEEDED(device_.As(&device5)) && SUCCEEDED(context_.As(&context4_)) &&
            SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&readbackFence_))))
            std::fprintf(stderr, "[gpu-hw] CPU readbacks wait on a GPU fence\n");
        else { context4_.Reset(); readbackFence_.Reset(); }

        D3D11_BUFFER_DESC b{};
        b.ByteWidth = VertexRingBytes; b.Usage = D3D11_USAGE_DYNAMIC; b.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device_->CreateBuffer(&b, nullptr, &vertexRing_), "CreateVertexRing");
        b.ByteWidth = IndexRingBytes; b.BindFlags = D3D11_BIND_INDEX_BUFFER;
        Check(device_->CreateBuffer(&b, nullptr, &indexRing_), "CreateIndexRing");
        b.ByteWidth = 256 * 16; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device_->CreateBuffer(&b, nullptr, &psConstants_), "CreatePsConstants");
        b.ByteWidth = sizeof(PsStateCB);
        Check(device_->CreateBuffer(&b, nullptr, &psState_), "CreatePsState");
        // GPU vertex stage: 512 vertex ALU constants, per-draw state, and a raw ring holding
        // each draw's guest vertex data (host-order dwords).
        b.ByteWidth = 512 * 16;
        Check(device_->CreateBuffer(&b, nullptr, &vsConstants_), "CreateVsConstants");
        b.ByteWidth = sizeof(VsStateCB);
        Check(device_->CreateBuffer(&b, nullptr, &vsState_), "CreateVsState");
        b.ByteWidth = VdataRingBytes; b.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        Check(device_->CreateBuffer(&b, nullptr, &vdataRing_), "CreateVertexDataRing");
        b.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC rv{};
        rv.Format = DXGI_FORMAT_R32_TYPELESS; rv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        rv.BufferEx.NumElements = VdataRingBytes / 4; rv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        Check(device_->CreateShaderResourceView(vdataRing_.Get(), &rv, &vdataSrv_), "CreateVertexDataSrv");
        {
            // NO_OVERWRITE on a dynamic buffer read through an SRV needs this D3D11.1 option;
            // without it every draw's vertex data is written with DISCARD at offset 0.
            D3D11_FEATURE_DATA_D3D11_OPTIONS o{};
            vdataNoOverwrite_ = SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o))) &&
                                o.MapNoOverwriteOnDynamicBufferSRV;
        }
        static const bool gpuVs = [] { const char* e = std::getenv("TTT2_GPU_VS"); return !e || std::strcmp(e, "0") != 0; }();
        gpuVsEnabled_ = gpuVs;
        std::fprintf(stderr, "[gpu-hw] vertex shaders on the %s (vertex-data ring %s)\n", gpuVs ? "GPU" : "CPU (TTT2_GPU_VS=0)",
                     vdataNoOverwrite_ ? "no-overwrite" : "discard per draw");

        D3D11_RASTERIZER_DESC r{};
        r.FillMode = D3D11_FILL_SOLID; r.CullMode = D3D11_CULL_NONE;   // culled on the CPU
        r.DepthClipEnable = FALSE;                                     // the CPU path never clips z
        r.ScissorEnable = TRUE;
        Check(device_->CreateRasterizerState(&r, &raster_), "CreateRasterizerState");

        D3D11_SAMPLER_DESC s{};
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX; s.ComparisonFunc = D3D11_COMPARISON_NEVER;
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        Check(device_->CreateSamplerState(&s, &pointClamp_), "CreatePointSampler");
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        Check(device_->CreateSamplerState(&s, &linearClamp_), "CreateLinearSampler");

        // The interpreter's answer for an invalid texture fetch constant: (0, 0, 0, 1).
        const float black[4] = { 0, 0, 0, 1 };
        D3D11_TEXTURE2D_DESC t{};
        t.Width = t.Height = 1; t.MipLevels = t.ArraySize = 1; t.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_IMMUTABLE; t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{ black, 16, 16 };
        ComPtr<ID3D11Texture2D> tex;
        Check(device_->CreateTexture2D(&t, &init, &tex), "CreateDefaultTexture");
        Check(device_->CreateShaderResourceView(tex.Get(), nullptr, &defaultSrv_), "CreateDefaultSrv");
        {
            const float white[4] = { 1, 1, 1, 1 };
            D3D11_SUBRESOURCE_DATA wi{ white, 16, 16 };
            ComPtr<ID3D11Texture2D> wt;
            Check(device_->CreateTexture2D(&t, &wi, &wt), "CreateWhiteTexture");
            Check(device_->CreateShaderResourceView(wt.Get(), nullptr, &whiteSrv_), "CreateWhiteSrv");
            D3D11_SHADER_RESOURCE_VIEW_DESC wv{};
            wv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; wv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            wv.Texture2DArray.MipLevels = 1; wv.Texture2DArray.ArraySize = 1;
            Check(device_->CreateShaderResourceView(wt.Get(), &wv, &whiteArraySrv_), "CreateWhiteArraySrv");
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC av{};
        av.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; av.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        av.Texture2DArray.MipLevels = 1; av.Texture2DArray.ArraySize = 1;
        Check(device_->CreateShaderResourceView(tex.Get(), &av, &defaultArraySrv_), "CreateDefaultArraySrv");

        shaderCacheDir_ = ShaderCacheDirectory();
        lastStats_ = std::chrono::steady_clock::now();
        StartShaderWarmup();
    }

    // ----------------------------------------------------------------- draw
    // verts[0..vertCount) are the draw's shaded vertices, extras follow them; slots index both
    // (three per triangle, already culled on the CPU). Only `interps` interpolators are sent.
    bool Draw(const uint32_t* regs, const std::vector<uint32_t>& psCode, const Vertex* verts, size_t vertCount,
              const std::vector<Vertex>& extras, const std::vector<uint32_t>& slots, uint32_t interps,
              const DrawState& st)
    {
        Initialize();
        UpdateScale();
        if (ShadowCaster(st)) { ++stats_.shadowSkips; return true; }
        ++stats_.draws;
        ColorSurface& color = Color(st.colorBase);
        CensusTarget(st.colorBase);
        color.touched = true;
        color.samplePitch = st.pitch * st.sampX;
        if (slots.empty()) { ++stats_.emptyDraws; return true; }

        ID3D11PixelShader* ps = PixelShader(psCode);
        if (!ps) { ++stats_.skippedDraws; return false; }
        const uint32_t textureSlots = lastSlots_;

        // ---- vertices: window space -> clip space for the sample-space viewport
        interps = std::min<uint32_t>(interps, 16);
        const size_t stride = 16 + 16 * size_t(interps);
        const size_t total = vertCount + extras.size();
        const size_t vbBytes = total * stride, ibBytes = slots.size() * 4;
        if (vbBytes + stride > VertexRingBytes || ibBytes > IndexRingBytes) { ++stats_.skippedDraws; return false; }
        // Keep each draw's vertices stride-aligned so the base vertex is an integer.
        size_t vbOffset = (ringOffset_ + stride - 1) / stride * stride;
        D3D11_MAP vbMap = D3D11_MAP_WRITE_NO_OVERWRITE;
        const bool vbWrap = vbOffset + vbBytes > VertexRingBytes;
        if (vbWrap) { vbOffset = 0; if (RingDiscard()) vbMap = D3D11_MAP_WRITE_DISCARD; }
        if (!RingDiscard()) RingWrite(vertexFence_, VertexRingBytes, vbOffset, vbBytes, vbWrap);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context_->Map(vertexRing_.Get(), 0, vbMap, 0, &mapped), "MapVertexRing");
        uint8_t* out = static_cast<uint8_t*>(mapped.pData) + vbOffset;
        const float hpx = float(SurfaceH / st.sampY), pitch = float(st.pitch);
        const bool noClip = (regs[0x2204] & 0x10000u) != 0;
        auto convert = [&](size_t i) {
            const Vertex& v = i < vertCount ? verts[i] : extras[i - vertCount];
            float* h = reinterpret_cast<float*>(out + i * stride);
            const float w = v.invW != 0.0f ? 1.0f / v.invW : 1.0f;
            // PA_CL_CLIP_CNTL.clip_disable: Xenos rasterises negative-W vertices unclipped; D3D
            // would clip them all, so submit the same NDC with a positive W (perspective weights
            // use |W|; attributes keep their true value). With clipping enabled a negative W is
            // clipped on Xenos as well, so it is left for D3D to clip.
            const float wc = (w < 0.0f && noClip) ? -w : w;
            h[0] = (v.x / pitch * 2.0f - 1.0f) * wc;
            h[1] = (1.0f - v.y / hpx * 2.0f) * wc;
            h[2] = v.z * wc;
            h[3] = wc;
            for (uint32_t a = 0; a < interps; ++a)
                for (int c = 0; c < 4; ++c) h[4 + a * 4 + c] = v.attr[a][c] * w;
        };
        if (total >= 2048)
        {
            const std::function<void(int)> row = [&](int i) { convert(size_t(i)); };
            RasterPool::Get().ForRows(0, (int)total, row);
        }
        else for (size_t i = 0; i < total; ++i) convert(i);
        context_->Unmap(vertexRing_.Get(), 0);
        const UINT baseVertex = UINT(vbOffset / stride);
        ringOffset_ = vbOffset + vbBytes;
        // How far right / down the draw can reach (logical pixels), for surface sizing: the
        // vertices' window positions bound every triangle unless one is clipped at W = 0
        // (negative W with clipping on). TTT2's EDRAM clears are unscissored screen-space quads.
        float reachX = std::numeric_limits<float>::infinity(), reachY = reachX;
        {
            float mx = 0.0f, my = 0.0f;
            bool bounded = true;
            for (size_t i = 0; i < total && bounded; ++i)
            {
                const Vertex& v = i < vertCount ? verts[i] : extras[i - vertCount];
                const float w = v.invW != 0.0f ? 1.0f / v.invW : 1.0f;
                bounded = (w > 0.0f || noClip) && std::isfinite(v.x) && std::isfinite(v.y);
                mx = std::max(mx, v.x); my = std::max(my, v.y);
            }
            if (bounded) { reachX = std::ceil(mx); reachY = std::ceil(my); }
        }

        D3D11_MAP ibMap = D3D11_MAP_WRITE_NO_OVERWRITE;
        const bool ibWrap = indexOffset_ + ibBytes > IndexRingBytes;
        if (ibWrap) { indexOffset_ = 0; if (RingDiscard()) ibMap = D3D11_MAP_WRITE_DISCARD; }
        if (!RingDiscard()) RingWrite(indexFence_, IndexRingBytes, indexOffset_, ibBytes, ibWrap);
        Check(context_->Map(indexRing_.Get(), 0, ibMap, 0, &mapped), "MapIndexRing");
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + indexOffset_, slots.data(), ibBytes);
        context_->Unmap(indexRing_.Get(), 0);
        const UINT firstIndex = UINT(indexOffset_ / 4);
        indexOffset_ += ibBytes;

        PassthroughVs& pvs = Passthrough(interps);
        context_->IASetInputLayout(pvs.layout.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* vb = vertexRing_.Get(); UINT vbStride = UINT(stride), vbOffsetBytes = 0;
        context_->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffsetBytes);
        context_->IASetIndexBuffer(indexRing_.Get(), DXGI_FORMAT_R32_UINT, 0);
        context_->VSSetShader(pvs.vs.Get(), nullptr, 0);
        PixelStageAndDraw(regs, psCode, ps, textureSlots, color, st, UINT(slots.size()), firstIndex, INT(baseVertex), reachX, reachY);
        return true;
    }

    // ----------------------------------------------------------------- GPU vertex stage
    struct VsEntry { ComPtr<ID3D11VertexShader> vs; VsTranslation t; bool failed = false; };

    // The translated vertex shader for this microcode, or null when it cannot run on the GPU
    // (the caller keeps the CPU vertex stage for it).
    VsEntry* VertexShader(const std::vector<uint32_t>& code)
    {
        Initialize();
        if (!gpuVsEnabled_ || code.empty()) return nullptr;
        const uint64_t key = VsKey(code);
        if (auto it = vertexShaders_.find(key); it != vertexShaders_.end()) return it->second.failed ? nullptr : &it->second;
        VsEntry e;
        if (TakePrepared(preparedVs_, key, e) || SharedVertexShader(code, e)) ++stats_.warmHits;
        else
        {
            Built<VsEntry> b;
            if (!AwaitBuild(pendingVs_, key, b, [this, code] {
                    Built<VsEntry> r;
                    r.result = BuildVertexShader(code, r.entry, r.counts, r.why, true);
                    return r;
                }, false))
                return nullptr;                 // still compiling: the CPU vertex stage draws it meanwhile
            stats_.diskHits += b.counts.diskHits; stats_.compiles += b.counts.compiles;
            if (b.result != BuildResult::Ready)
            {
                std::fprintf(stderr, b.result == BuildResult::Untranslatable ? "[gpu-hw] vertex shader %016llX (%zu dwords) stays on the CPU: %s\n"
                                                                             : "[gpu-hw] vertex shader %016llX (%zu dwords) failed to compile - it stays on the CPU:\n%s\n",
                             (unsigned long long)key, code.size(), b.why.c_str());
                vertexShaders_[key].failed = true;
                return nullptr;
            }
            e = std::move(b.entry);
            RecordMicrocode("vs", key, code);
        }
        VsEntry& slot = vertexShaders_[key];
        slot = std::move(e);
        return &slot;
    }

    enum class GpuVsResult { Drawn, Skipped, Fallback };

    // Draw with the vertex shader on the GPU. `indices` are vertex indices, three per triangle
    // in the CPU assembly order (restarts already applied); minIndex..maxIndex bound the guest
    // vertex data uploaded. Fallback: nothing was changed, run the CPU vertex stage instead.
    GpuVsResult DrawGpuVs(const uint32_t* regs, const std::vector<uint32_t>& psCode, VsEntry& vs,
                          const std::vector<uint32_t>& indices, uint32_t minIndex, uint32_t maxIndex, const DrawState& st)
    {
        Initialize();
        UpdateScale();
        int64_t lo[96], hi[96];
        bool used[96];
        VsFetchRanges(vs.t, minIndex, maxIndex, lo, hi, used);
        CensusVertex(vs.t.fetches);
        uint64_t totalDwords = 0;
        for (uint32_t s = 0; s < 96; ++s)
            if (used[s])
            {
                if (hi[s] - lo[s] > int64_t(16u << 20) / 4) return GpuVsResult::Fallback;
                totalDwords += uint64_t(hi[s] - lo[s]);
            }
        if (totalDwords * 4 + 64 > VdataRingBytes || indices.size() * 4 > IndexRingBytes) return GpuVsResult::Fallback;
        if (toggle_ & 2) return GpuVsResult::Drawn;
        if (ShadowCaster(st)) { ++stats_.shadowSkips; return GpuVsResult::Drawn; }

        ++stats_.draws;
        ++stats_.gpuVsDraws;
        ColorSurface& color = Color(st.colorBase);
        CensusTarget(st.colorBase);
        color.touched = true;
        color.samplePitch = st.pitch * st.sampX;
        if (indices.empty()) { ++stats_.emptyDraws; return GpuVsResult::Drawn; }
        ID3D11PixelShader* ps = PixelShader(psCode);
        if (!ps) { ++stats_.skippedDraws; return GpuVsResult::Skipped; }
        const uint32_t textureSlots = lastSlots_;

        // ---- vertex data: each fetched range of guest memory as host-order dwords
        VsStateCB vst{};
        if (totalDwords)
        {
            D3D11_MAP map = D3D11_MAP_WRITE_NO_OVERWRITE;
            size_t start = (vdataOffset_ + 15) & ~size_t(15);
            if (!vdataNoOverwrite_) { start = 0; map = D3D11_MAP_WRITE_DISCARD; }
            else
            {
                const bool wrap = start + totalDwords * 4 > VdataRingBytes;
                if (wrap) { start = 0; if (RingDiscard()) map = D3D11_MAP_WRITE_DISCARD; }
                if (!RingDiscard()) RingWrite(vdataFence_, VdataRingBytes, start, size_t(totalDwords) * 4, wrap);
            }
            D3D11_MAPPED_SUBRESOURCE m{};
            Check(context_->Map(vdataRing_.Get(), 0, map, 0, &m), "MapVertexData");
            uint32_t* out = static_cast<uint32_t*>(m.pData) + start / 4;
            uint64_t cursor = start / 4;
            for (uint32_t s = 0; s < 96; ++s)
            {
                if (!used[s]) continue;
                const uint32_t fc0 = regs[0x4800 + s * 2];
                if ((fc0 & 3) != 3) continue;                    // not a vertex fetch constant: w = 0
                const uint32_t phys = fc0 & ~3u;
                const uint64_t guest = (phys && phys < 0x20000000u) ? (phys | 0xA0000000u) : phys;
                const int64_t first = int64_t(guest) + lo[s] * 4, count = hi[s] - lo[s];
                vst.vfRegion[s][3] = 1;
                if (first < 0x10000 || uint64_t(first) + uint64_t(count) * 4 > 0x100000000ull)
                {
                    vst.vfRegion[s][1] = vst.vfRegion[s][2] = 0;     // unreadable: fetches read 0
                    continue;
                }
                const uint32_t* src = reinterpret_cast<const uint32_t*>(g_base + first);
                for (int64_t i = 0; i < count; ++i) out[i] = __builtin_bswap32(src[i]);
                GpuNoteMemory(1, uint32_t(first), uint32_t(count) * 4);
                if (VcacheStatsOn()) VcacheStat(uint32_t(first), uint32_t(count) * 4);
                vst.vfRegion[s][0] = int32_t(int64_t(cursor) - lo[s]);
                vst.vfRegion[s][1] = int32_t(lo[s]);
                vst.vfRegion[s][2] = int32_t(hi[s]);
                out += count;
                cursor += uint64_t(count);
            }
            context_->Unmap(vdataRing_.Get(), 0);
            vdataOffset_ = size_t(cursor) * 4;
            stats_.vertexDataBytes += totalDwords * 4;
        }

        // ---- vertex constants and state
        if (std::memcmp(lastVsConstants_.data(), regs + 0x4000, 512 * 16) != 0 || !vsConstantsValid_)
        {
            std::memcpy(lastVsConstants_.data(), regs + 0x4000, 512 * 16);
            Upload(vsConstants_.Get(), lastVsConstants_.data(), 512 * 16);
            vsConstantsValid_ = true;
        }
        std::memcpy(vst.boolConsts, regs + 0x4900, sizeof(vst.boolConsts));
        std::memcpy(vst.loopConsts, regs + 0x4908, sizeof(vst.loopConsts));
        vst.vte = regs[0x2206];
        vst.xs = AsFloat(regs[0x210F]); vst.xo = AsFloat(regs[0x2110]);
        vst.ys = AsFloat(regs[0x2111]); vst.yo = AsFloat(regs[0x2112]);
        vst.zs = AsFloat(regs[0x2113]); vst.zo = AsFloat(regs[0x2114]);
        vst.pitch = float(st.pitch);
        vst.rows = float(SurfaceH / st.sampY);
        vst.noClip = (regs[0x2204] >> 16) & 1;
        VsFetchDescriptors(vs.t, vst.vfDesc);
        Upload(vsState_.Get(), &vst, sizeof(vst));

        // ---- indices
        const size_t ibBytes = indices.size() * 4;
        D3D11_MAP ibMap = D3D11_MAP_WRITE_NO_OVERWRITE;
        const bool ibWrap = indexOffset_ + ibBytes > IndexRingBytes;
        if (ibWrap) { indexOffset_ = 0; if (RingDiscard()) ibMap = D3D11_MAP_WRITE_DISCARD; }
        if (!RingDiscard()) RingWrite(indexFence_, IndexRingBytes, indexOffset_, ibBytes, ibWrap);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context_->Map(indexRing_.Get(), 0, ibMap, 0, &mapped), "MapIndexRing");
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + indexOffset_, indices.data(), ibBytes);
        context_->Unmap(indexRing_.Get(), 0);
        const UINT firstIndex = UINT(indexOffset_ / 4);
        indexOffset_ += ibBytes;

        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* noVb = nullptr; UINT zero = 0;
        context_->IASetVertexBuffers(0, 1, &noVb, &zero, &zero);
        context_->IASetIndexBuffer(indexRing_.Get(), DXGI_FORMAT_R32_UINT, 0);
        context_->VSSetShader(vs.vs.Get(), nullptr, 0);
        ID3D11Buffer* vcbs[2] = { vsConstants_.Get(), vsState_.Get() };
        context_->VSSetConstantBuffers(0, 2, vcbs);
        ID3D11ShaderResourceView* vsrv = vdataSrv_.Get();
        context_->VSSetShaderResources(0, 1, &vsrv);
        PixelStageAndDraw(regs, psCode, ps, textureSlots, color, st, UINT(indices.size()), firstIndex, 0,
                          std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
        return GpuVsResult::Drawn;
    }

    // Shared tail of both vertex paths (vertex input already bound): pixel-shader constants,
    // textures and samplers, output merger, viewport, scissor, rasterizer state, the draw.
    // reachX / reachY: how far the geometry can reach in logical pixels (infinity: unknown).
    void PixelStageAndDraw(const uint32_t* regs, const std::vector<uint32_t>& psCode, ID3D11PixelShader* ps,
                           uint32_t textureSlots, ColorSurface& color, const DrawState& st,
                           UINT indexCount, UINT firstIndex, INT baseVertex, float reachX, float reachY)
    {
        // ---- constants and per-draw shader state
        if (std::memcmp(lastPsConstants_.data(), regs + 0x4000 + 256 * 4, 4096) != 0 || !constantsValid_)
        {
            std::memcpy(lastPsConstants_.data(), regs + 0x4000 + 256 * 4, 4096);
            Upload(psConstants_.Get(), lastPsConstants_.data(), 4096);
            constantsValid_ = true;
        }
        PsStateCB cb{};
        std::memcpy(cb.boolConsts, regs + 0x4900, sizeof(cb.boolConsts));
        std::memcpy(cb.loopConsts, regs + 0x4908, sizeof(cb.loopConsts));
        cb.alphaRef = st.alphaRef; cb.alphaTest = st.alphaTest ? 1 : 0; cb.alphaFunc = st.alphaFunc;
        ID3D11ShaderResourceView* srvs[32] = {};
        ID3D11SamplerState* samplers[16] = {};
        for (uint32_t slot = 0; slot < 32; ++slot)
        {
            if (!(textureSlots & (1u << slot))) continue;
            Bound b = BindTexture(regs, slot, ((lastCubeSlots_ >> slot) & 1) != 0);
            srvs[slot] = b.srv;
            cb.texInfo[slot][0] = b.swizzle; cb.texInfo[slot][1] = b.mode;
            cb.texInfo[slot][2] = b.width; cb.texInfo[slot][3] = b.height;
            cb.texScale[slot][0] = b.scaleX; cb.texScale[slot][1] = b.scaleY; cb.texScale[slot][2] = float(b.slices);
            const Texture ft = DecodeTexture(regs, slot);
            SamplerInfo(ft, cb.texSampler[slot]);
            if (b.slices > 1) cb.texSampler[slot][0] = cb.texSampler[slot][1] = 2;   // cube faces clamp
            // Bit 30: every depth-tested (3D) draw; bit 31: every draw; second token: shader size.
            if (((overrideMask_ >> slot) & 1) &&
                (overridePsDwords_ ? psCode.size() == overridePsDwords_
                 : ((overrideMask_ >> 30) & 1) ? st.zEnable : (lastCubeSlots_ || (overrideMask_ >> 31))))
            {
                // Diagnostics (C:/ttt2/port/hw_override.txt): this slot samples opaque white in
                // shaders that fetch a cube (character/environment materials), or in every
                // shader when bit 31 is set.
                srvs[slot] = ((lastCubeSlots_ >> slot) & 1) ? whiteArraySrv_.Get() : whiteSrv_.Get();
                cb.texInfo[slot][0] = 0x688; cb.texInfo[slot][1] = 0; cb.texInfo[slot][2] = cb.texInfo[slot][3] = 1;
                cb.texScale[slot][0] = cb.texScale[slot][1] = cb.texScale[slot][2] = 1.0f;
            }
            samplers[slot & 15] = Sampler(cb.texSampler[slot], MipSampling(ft, b.levels));
        }
        if (std::memcmp(&cb, &lastState_, sizeof(cb)) != 0 || !stateValid_)
        {
            lastState_ = cb; stateValid_ = true;
            Upload(psState_.Get(), &cb, sizeof(cb));
        }

        // ---- output merger: the surfaces cover the scissored area (logical samples); a bound
        // colour / depth pair is grown to one size (D3D11 binds only equal-size views).
        const int64_t x1 = std::clamp<int64_t>(int64_t(st.scX1) * st.sampX, 0, SurfaceW);
        const int64_t y1 = std::clamp<int64_t>(int64_t(st.scY1) * st.sampY, 0, SurfaceH);
        DepthSurface* depth = nullptr;
        if (st.zEnable || st.stencil)
        {
            depth = &Depth(st.depthBase);
            depth->touched = true; depth->sampX = st.sampX; depth->sampY = st.sampY;
        }
        if (CensusOn()) CensusSurfaceUse(regs, st, reachY);
        auto reach = [](int64_t scissor, float geometry, uint32_t samples) {
            return UINT(std::isfinite(geometry) ? std::clamp<int64_t>(int64_t(std::min(geometry, 65536.0f)) * samples, 0, scissor)
                                                : scissor);
        };
        UINT needW = reach(x1, reachX, st.sampX), needH = reach(y1, reachY, st.sampY);
        if (st.writeMask && depth)
        {
            needW = std::max({ needW, color.extW, depth->extW });
            needH = std::max({ needH, color.extH, depth->extH });
        }
        // Native-size pair (a CPU-read bake surface and its depth) or the render scale. The game reuses
        // EDRAM ranges: a draw into a bake's colour or depth base at another sample pitch than the
        // bake's, or bigger than a bake (1024x1024), is something else drawn there (the win screen's
        // depth of field draws 640x180 into the fight intro bake's 4C0, MEASURED 2026-10-20) - back
        // to the render scale for good (UpdateNative flags it again at the next bake).
        bool native = (st.writeMask && nativeColors_.count(st.colorBase)) || (depth && nativeDepths_.count(st.depthBase));
        static const bool sceneFix = [] { const char* e = std::getenv("TTT2_NATIVE_SCENE_FIX"); return !(e && *e == '0'); }();
        const uint32_t samplePitch = st.pitch * st.sampX;
        const auto nc = st.writeMask ? nativeColors_.find(st.colorBase) : nativeColors_.end();
        const auto nd = depth ? nativeDepths_.find(st.depthBase) : nativeDepths_.end();
        const bool otherUse = (nc != nativeColors_.end() && nc->second != samplePitch) || (nd != nativeDepths_.end() && nd->second != samplePitch);
        if (native && (otherUse || needW > 1024 || needH > 1024))
        {
            static int logged = 0;
            if (logged++ < 32)
                std::fprintf(stderr, "[gpu-hw] native-size surface %05X / depth %03X drawn at sample pitch %u, %ux%u samples%s\n",
                             st.colorBase, depth ? st.depthBase : 0xFFFu, samplePitch, needW, needH, sceneFix ? ": back to the render scale" : "");
            if (sceneFix)
            {
                nativeColors_.erase(st.colorBase);
                if (depth) nativeDepths_.erase(st.depthBase);
                const auto d = depthWithColor_.find(st.colorBase);
                if (d != depthWithColor_.end()) nativeDepths_.erase(d->second);
                native = false;
            }
        }
        const float sc = native ? 1.0f : scale_;
        if (st.writeMask && depth) depthWithColor_[st.colorBase] = st.depthBase;
        if (st.writeMask) Grow(color, st.colorBase, needW, needH, sc);
        if (depth) Grow(*depth, st.depthBase, needW, needH, sc);
        ID3D11RenderTargetView* rtv = st.writeMask ? color.rtv.Get() : nullptr;
        ID3D11DepthStencilView* dsv = depth ? depth->dsv.Get() : nullptr;
        context_->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, dsv);
        float factor[4];
        context_->OMSetBlendState(BlendState(st, factor), factor, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(DepthState(st), StencilRef(st));
        // The sample-space viewport and scissor, times the render scale (vertices stay logical).
        D3D11_VIEWPORT vp{ 0, 0, float(st.pitch * st.sampX) * sc, float(SurfaceH) * sc, 0, 1 };
        context_->RSSetViewports(1, &vp);
        D3D11_RECT scissor{ LONG(Px(std::max(st.scX0, 0) * int(st.sampX), sc)), LONG(Px(std::max(st.scY0, 0) * int(st.sampY), sc)),
                            LONG(Px(double(x1), sc)), LONG(Px(double(y1), sc)) };
        context_->RSSetScissorRects(1, &scissor);
        if ((st.cull & 3) == 3) { ++stats_.emptyDraws; return; }          // both faces culled
        context_->RSSetState(RasterState(st));
        context_->PSSetShader(ps, nullptr, 0);
        ID3D11Buffer* cbs[2] = { psConstants_.Get(), psState_.Get() };
        context_->PSSetConstantBuffers(0, 2, cbs);
        context_->PSSetShaderResources(0, 32, srvs);
        context_->PSSetSamplers(0, 16, samplers);
        if (!(toggle_ & 1)) context_->DrawIndexed(indexCount, firstIndex, baseVertex);
        stats_.triangles += indexCount / 3;
        MaybeReportStats();
    }

    // ----------------------------------------------------------------- resolve
    // srcSelect is RB_COPY_CONTROL.copy_src_select: 0-3 colour render targets, 4 depth/stencil.
    // A depth resolve copies the depth surface for depthBase into an R32F texture (the value the
    // guest's 24_8 / 24_8_FLOAT fetch returns); depthFloat is RB_DEPTH_INFO.depth_format (20e4).
    // destFormat is RB_COPY_DEST_INFO.copy_dest_format: a 16_16_FLOAT destination (31) keeps the
    // source's red and green as half floats (an R16G16_FLOAT texture the 16_16_FLOAT fetch
    // samples directly); every other colour destination is modelled as 8_8_8_8.
    void Resolve(uint32_t colorBase, uint32_t dest, uint32_t pitch, uint32_t height, bool hasLatch, uint32_t latch,
                 bool swapRB, uint32_t srcSelect, uint32_t depthBase, bool depthFloat, uint32_t destFormat = 6)
    {
        Initialize();
        UpdateScale();
        FinishSmallWritebacks();
        ++stats_.resolves;
        const bool depth = srcSelect == 4;
        const bool rg16f = !depth && destFormat == 31;
        if (depth) NoteShadowResolve(depthBase, pitch, height);
        Resolved& r = resolved_[dest];
        float sourceScale = scale_;
        if (depth)
        {
            auto d = depths_.find(depthBase);
            if (d != depths_.end() && d->second.tex) sourceScale = d->second.scale;
        }
        else if (auto c = colors_.find(colorBase); c != colors_.end() && c->second.tex)
            sourceScale = c->second.scale;
        if (!r.tex || r.pitch != pitch || r.height != height || r.depth != depth || r.rg16f != rg16f || r.scale != sourceScale)
        {
            const bool hot = r.readbackHot;
            RetireResolvedTexture(r);
            r = Resolved{};
            r.pitch = pitch; r.height = height; r.depth = depth; r.rg16f = rg16f;
            r.readbackHot = hot;
            MakeResolvedTexture(r, sourceScale);
        }
        r.sourceKey = depth ? ~0u : colorBase;
        if (!depth) { const auto c = colors_.find(colorBase); r.sourcePitch = c != colors_.end() ? c->second.samplePitch : 0; }
        static const bool scaleLog = [] { const char* e = std::getenv("TTT2_RESOLVE_SCALE_LOG"); return e && *e == '1'; }();
        if (scaleLog && !censusSeen_[(5ull << 60) ^ (uint64_t(colorBase) << 32) ^ (uint64_t(dest) >> 4) ^ (uint64_t(pitch) << 44) ^ (uint64_t(height) << 20)]++)
        {
            const auto c = colors_.find(colorBase);
            std::fprintf(stderr, "[resolve-scale] %s %05X/%03X %ux%u -> %08X: source scale %.2f, texture scale %.2f, surface ext %ux%u native %d/%d\n",
                         depth ? "depth" : "colour", colorBase, depthBase, pitch, height, dest, sourceScale, r.scale,
                         c != colors_.end() ? c->second.extW : 0u, c != colors_.end() ? c->second.extH : 0u,
                         int(nativeColors_.count(colorBase)), int(nativeDepths_.count(depthBase)));
        }
        if (!depth)
        {
            auto& dests = resolveDests_[colorBase];
            auto [entry, added] = dests.try_emplace(dest, r.readbackHot);
            if (added || entry->second != r.readbackHot) { entry->second = r.readbackHot; UpdateNative(colorBase); }
        }
        r.depthFloat = depthFloat;
        if (CensusOn() && !censusSeen_[(4ull << 60) ^ (uint64_t(colorBase) << 32) ^ (uint64_t(pitch) << 16) ^ height ^
                                       (uint64_t(srcSelect) << 28) ^ (uint64_t(depthBase) << 44)]++)
            std::fprintf(stderr, "[census] resolve from %s %05X/%03X %ux%u -> %08X\n", depth ? "depth" : "colour",
                         colorBase, depthBase, pitch, height, dest);
        auto it = colors_.find(colorBase);
        if (depth)
        {
            ++stats_.depthResolves;
            auto d = depths_.find(depthBase);
            if (toggle_ & 4) {}
            else if (ShadowMapOff(depthBase, pitch, height))
            {
                const float lit[4] = { 0, 0, 0, 0 };     // SHADOWS OFF: no occluder anywhere (see ShadowCaster)
                context_->ClearRenderTargetView(r.rtv.Get(), lit);
            }
            else if (d != depths_.end() && d->second.touched) BlitDepth(d->second, r);
            else
            {
                const float farDepth[4] = { 1, 1, 1, 1 };                 // depth surfaces start at 1.0
                context_->ClearRenderTargetView(r.rtv.Get(), farDepth);
            }
            r.copiedWidth = pitch;
        }
        else if (it == colors_.end() || !it->second.touched)
        {
            // Never rasterized: the CPU path fills the fast-clear latch (or zero). RB_COLOR_CLEAR
            // holds the 8_8_8_8 pixel itself, red in the low byte (Xenia's k_8_8_8_8 clear).
            const uint32_t px = hasLatch ? latch : 0;
            const float c[4] = { (px & 0xFF) / 255.0f, ((px >> 8) & 0xFF) / 255.0f,
                                 ((px >> 16) & 0xFF) / 255.0f, ((px >> 24) & 0xFF) / 255.0f };
            context_->ClearRenderTargetView(r.rtv.Get(), c);
            r.copiedWidth = pitch;
        }
        else
        {
            ColorSurface& s = it->second;
            const uint32_t width = std::min({ s.samplePitch ? s.samplePitch : pitch, pitch, SurfaceW });
            const uint32_t rows = std::min<UINT>(height, SurfaceH);
            // Nothing was ever drawn beyond the surface's extent (it covers every draw's reach):
            // that part reads as the cleared store, zero - the shader blits' out-of-range loads.
            const float k = r.scale;                // == s.scale: texel for texel
            const UINT copyW = std::min(Px(width, k), AllocPx(r.pitch, k)), copyH = std::min(Px(rows, k), AllocPx(r.height, k));
            const UINT haveW = s.tex ? std::min(copyW, AllocPx(s.extW, k)) : 0, haveH = s.tex ? std::min(copyH, AllocPx(s.extH, k)) : 0;
            if ((width != r.copiedWidth && width < pitch) || haveW < copyW || haveH < copyH)
            {
                const float zero[4] = { 0, 0, 0, 0 };
                context_->ClearRenderTargetView(r.rtv.Get(), zero);
            }
            if (!s.tex || (toggle_ & 4)) {}
            else if (r.rg16f) BlitColor(s, r, width, rows, true);
            else if (s.format != DXGI_FORMAT_B8G8R8A8_UNORM) BlitColor(s, r, width, rows, false);   // convert
            else
            {
                D3D11_BOX box{ 0, 0, 0, haveW, haveH, 1 };
                context_->CopySubresourceRegion(r.tex.Get(), 0, 0, 0, 0, s.tex.Get(), 0, &box);
            }
            r.copiedWidth = width;
        }
        // The resolve owns its destination from now on; any older decode of these bytes is
        // stale. Consume earlier CPU writes so only LATER ones invalidate this entry.
        r.size = ResolveBytes(pitch, height);
        dirty_.Sync(dest, r.size);
        r.seq = dirty_.Current();
        r.valid = true;
        r.cpuCoherent = false;
        r.swapRB = swapRB;          // copy_dest_swap: the guest bytes hold red/blue exchanged
        DropTexturesOverlapping(dest, r.size);
        static const bool writeback = [] { const char* e = std::getenv("TTT2_GPU_HW_WRITEBACK"); return e && std::strcmp(e, "1") == 0; }();
        // Tiny resolves are CPU readbacks (the game's auto exposure reads its 32x1 average-colour
        // resolve and turns it into the tone pass's exposure constant) - and the CPU reads them
        // without passing through the cached view, so nothing else makes them coherent. MEASURED
        // 2026-10-12 on Odeum of Illusions: without this, exposure 3.4 instead of ~1.5, luma 190 /
        // 25% white vs 119-143 / 1-3% on real-hardware footage; with it, luma 124-129, near-white
        // 2-3%, saturation 0.51-0.55 (reference 0.57), 60 fps. TTT2_SMALL_WRITEBACK=<pixels>
        // changes the limit (0 = off).
        static const uint32_t smallWriteback = [] {
            const char* e = std::getenv("TTT2_SMALL_WRITEBACK");
            return e ? uint32_t(std::strtoul(e, nullptr, 0)) : 32u;
        }();
        // Small ones are staged and written into guest memory once the GPU has done the copy
        // (FinishSmallWritebacks, at the next resolve or present) instead of waiting here: the
        // exposure readback came once per frame, and its blocking Map (with a new staging texture
        // each time) made the CP thread wait for the GPU every frame - MEASURED 2026-10-15 on the
        // TEKKEN TUNES page: 25-45 ms frames, present calls of 6-27 ms. TTT2_SMALL_WRITEBACK_SYNC=1
        // waits as before.
        static const bool smallSync = [] { const char* e = std::getenv("TTT2_SMALL_WRITEBACK_SYNC"); return e && *e == '1'; }();
        if (writeback || (pitch * height <= smallWriteback && smallSync)) WriteBack(dest, r);
        else if (pitch * height <= smallWriteback) { if (!(toggle_ & 8)) { StageReadback(r); smallPending_.insert(dest); } }
        else if (r.readbackHot) StageReadback(r);
        MaybeReportStats();
    }

    // The CPU is about to read guest bytes in [start, start + size) - a first access through
    // the cached physical view (phys_mirror.cpp). Make every GPU-only resolve overlapping them
    // coherent first, as the console's shared memory would be. Returns true while a copy is still
    // on its way: the caller (GpuHwWriteBackRange) calls again WITHOUT the backend lock in between,
    // so the command processor keeps drawing and presenting while this thread waits for the GPU
    // (it used to hold the lock for the whole wait: the fight intro's reads stalled presents by
    // up to ~15 ms each, MEASURED 2026-10-14). `block` waits in Map instead (the caller's last resort).
    bool WriteBackRange(uint32_t start, uint32_t size, bool block = false)
    {
        bool pending = false;
        pendingFence_ = 0;
        for (auto& [rs, r] : resolved_)
        {
            if (!r.valid || r.cpuCoherent || !r.tex) continue;
            if (!(uint64_t(start) < uint64_t(rs) + r.size && uint64_t(rs) < uint64_t(start) + size)) continue;
            dirty_.Sync(rs, r.size);
            if (dirty_.Newest(rs, r.size) > r.seq) { r.valid = false; continue; }   // the CPU overwrote it
            if (!r.readbackHot && r.sourceKey != ~0u)
            {
                r.readbackHot = true;
                resolveDests_[r.sourceKey][rs] = true;
                UpdateNative(r.sourceKey);
            }
            else if (r.sourceKey != ~0u && !nativeColors_.count(r.sourceKey)) UpdateNative(r.sourceKey);   // a bake again
            r.readbackHot = true;                     // its next resolves are staged for the CPU
            if (!r.staged || r.stagedSeq != r.seq)
            {
                StageReadback(r);                     // not staged yet (a first read): copy now, map later
                if (!block) { pending = true; pendingFence_ = std::max(pendingFence_, r.stagedFence); continue; }
            }
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<uint32_t> px;
            if (!ReadStaging(r.staging.Get(), r.pitch, r.height, px, !block))
            {
                pending = true;
                pendingFence_ = std::max(pendingFence_, r.stagedFence);
                continue;
            }
            r.staged = false;
            lastStagedAgeMs_ = std::chrono::duration<double, std::milli>(t0 - r.stagedAt).count();
            ++writebackCauses_["cpu read through the cached physical view"];
            static int logged = 0;
            if (logged < 40)
            {
                ++logged;
                std::fprintf(stderr, "[gpu-hw] cpu-read writeback dest=%08X %ux%u swapRB=%d rg16f=%d depth=%d\n",
                             rs, r.pitch, r.height, int(r.swapRB), int(r.rg16f), int(r.depth));
            }
            Scatter(rs, r, px, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        return pending;
    }

    // After a pending WriteBackRange: the fence value its copies complete at (0: poll instead).
    ComPtr<ID3D11Fence> PendingFence(uint64_t& value) const { value = pendingFence_; return value ? readbackFence_ : nullptr; }

    // Compare the GPU resolve with the CPU renderer's linear result (compare mode).
    void CompareResolve(uint32_t dest, const uint32_t* cpu, uint32_t pitch, uint32_t height)
    {
        auto it = resolved_.find(dest);
        if (it == resolved_.end() || !it->second.tex) return;
        std::vector<uint32_t> gpu;
        ReadLogical(it->second.tex.Get(), it->second.pitch, it->second.height, pitch, height, !it->second.depth, gpu);
        uint64_t differing = 0, sumDiff = 0;
        uint32_t maxDiff = 0;
        for (size_t i = 0; i < size_t(pitch) * height; ++i)
        {
            uint32_t a = cpu[i], b = gpu[i], worst = 0;
            for (int s = 0; s < 32; s += 8)
            {
                const uint32_t d = (uint32_t)std::abs(int((a >> s) & 0xFF) - int((b >> s) & 0xFF));
                worst = std::max(worst, d);
                sumDiff += d;
            }
            maxDiff = std::max(maxDiff, worst);
            differing += worst > 2;
        }
        const double pct = 100.0 * double(differing) / double(std::max<size_t>(size_t(pitch) * height, 1));
        ++compares_;
        if (compares_ <= 40 || compares_ % 120 == 0 || pct > 5.0)
            std::fprintf(stderr, "[gpu-hw-cmp] #%llu dest=%08X %ux%u differing(>2)=%llu (%.3f%%) max=%u mean=%.4f\n",
                         (unsigned long long)compares_, dest, pitch, height, (unsigned long long)differing, pct,
                         maxDiff, double(sumDiff) / double(std::max<size_t>(size_t(pitch) * height * 4, 1)));
        if (pct > 5.0 && dumps_ < 6)
        {
            ++dumps_;
            std::filesystem::create_directories("C:/ttt2/port/gpu_cmp");
            char path[160];
            std::snprintf(path, sizeof(path), "C:/ttt2/port/gpu_cmp/%03u_%08X_cpu.bmp", dumps_, dest);
            WriteBmp(path, cpu, pitch, height);
            std::snprintf(path, sizeof(path), "C:/ttt2/port/gpu_cmp/%03u_%08X_gpu.bmp", dumps_, dest);
            WriteBmp(path, gpu.data(), pitch, height);
        }
    }

    // Diagnostics: write the GPU texture registered for a resolve destination as a BMP.
    bool DumpResolved(uint32_t dest, const char* path)
    {
        auto it = resolved_.find(dest);
        if (it == resolved_.end() || !it->second.tex) return false;
        std::vector<uint32_t> px;
        ReadLogical(it->second.tex.Get(), it->second.pitch, it->second.height, it->second.pitch, it->second.height,
                    !it->second.depth, px);
        if (it->second.depth)
            for (uint32_t& v : px)
            {
                float d; std::memcpy(&d, &v, 4);
                const uint32_t g = uint32_t(std::clamp(d, 0.0f, 1.0f) * 255.0f + 0.5f);
                v = 0xFF000000u | (g << 16) | (g << 8) | g;
            }
        WriteBmp(path, px.data(), it->second.pitch, it->second.height);
        return true;
    }

    // ----------------------------------------------------------------- present
    void SetWindow(HWND w) { window_.store(w); }

    // displaySwapRB: the front buffer's fetch-constant swizzle exchanges red and blue (TTT2's
    // .zyx1). The image shown is swizzle(stored bytes); a GPU-resolved front buffer holds the
    // unswapped render, so swap only when exactly one of the two exchanges applies.
    bool Present(uint32_t frontBuffer, bool displaySwapRB, const uint32_t* gammaRamp, uint32_t gammaVersion)
    {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        Initialize();
        UpdateScale();
        FollowMipToggle();
        FinishSmallWritebacks();
        UpdateGamma(gammaRamp, gammaVersion);
        bool storedSwap = false;
        uint32_t logicalW = 1280, logicalH = 720;
        ID3D11Texture2D* source = FrontTexture(frontBuffer, storedSwap, logicalW, logicalH);
        if (!source) return false;
        HWND w = window_.load();
        if (!w) return false;
        if (!swapchain_)
        {
            ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; ComPtr<IDXGIFactory2> factory;
            Check(device_.As(&dxgi), "QueryDxgiDevice");
            Check(dxgi->GetAdapter(&adapter), "GetAdapter");
            Check(adapter->GetParent(IID_PPV_ARGS(&factory)), "GetFactory2");
            RECT rc{};
            GetClientRect(w, &rc);
            DXGI_SWAP_CHAIN_DESC1 d{};
            d.Width = std::max<LONG>(1, rc.right - rc.left); d.Height = std::max<LONG>(1, rc.bottom - rc.top);
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
            // TTT2_SWAP_BUFFERS=3/4 (experiment): MEASURED 2026-10-15 over 5 min of real play,
            // three buffers neither shortened the fight intro's readback waits (median 7.6 ms
            // either way) nor reduced dropped frames, so two stay the default.
            static const UINT buffers = [] { const char* e = std::getenv("TTT2_SWAP_BUFFERS"); const int n = e ? std::atoi(e) : 2; return UINT(std::clamp(n, 2, 4)); }();
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = buffers;
            d.Scaling = DXGI_SCALING_STRETCH; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            // VSYNC OFF presents with tearing allowed, which needs the swap chain flag: set whenever
            // the system supports it (ResizeBuffers keeps it, SwapChainFlags). TTT2_NO_TEARING=1
            // leaves it out (VSYNC OFF then only changes exclusive fullscreen).
            ComPtr<IDXGIFactory5> factory5;
            BOOL tearing = FALSE;
            static const bool noTearing = [] { const char* e = std::getenv("TTT2_NO_TEARING"); return e && *e == '1'; }();
            tearing_ = !noTearing && SUCCEEDED(factory.As(&factory5)) &&
                       SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) && tearing;
            d.Flags = SwapChainFlags();
            Check(factory->CreateSwapChainForHwnd(device_.Get(), w, &d, nullptr, nullptr, &swapchain_), "CreateSwapChainForHwnd");
            factory->MakeWindowAssociation(w, DXGI_MWA_NO_ALT_ENTER);       // the window thread owns Alt+Enter
            bufW_ = d.Width; bufH_ = d.Height;
            refreshHz_ = DisplayHz(w);
            std::fprintf(stderr, "[gpu-hw] presenting through a DXGI flip swap chain (%ux%u), tearing %s, display %u Hz, %s\n",
                         bufW_, bufH_, tearing_ ? "supported" : "not supported", refreshHz_, OutputInfo().c_str());
        }
        const auto t1 = clock::now();
        ApplyDisplay(w);
        ComPtr<ID3D11Texture2D> back;
        Check(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBackBuffer");
        const bool swap = storedSwap != displaySwapRB;
        const auto t2 = clock::now();
        PresentScaled(source, back.Get(), swap);
        lastSwap_ = swap;
        if (NativeDumpOn() && (stats_.presents + 1) % 60 == 0)      // the same present as fb_native.bmp
        {
            // Diagnostics (TTT2_FB_NATIVE=1): what the window shows, after the image options.
            std::vector<uint32_t> px;
            ReadTexture(back.Get(), bufW_, bufH_, px);
            for (uint32_t& v : px) v &= 0x00FFFFFFu;
            WriteBmp("C:/ttt2/port/fb_back.bmp", px.data(), bufW_, bufH_);
        }
        const auto t3 = clock::now();
        static const bool noWait = [] { const char* e = std::getenv("TTT2_PRESENT_NOWAIT"); return e && *e == '1'; }();
        static const bool diag = [] { const char* e = std::getenv("TTT2_PRESENT_DIAG"); return e && *e == '1'; }();
        if (diag)
        {
            // Diagnostics: the flush, then the GPU draining everything queued, timed apart from Present.
            using ms = std::chrono::duration<double, std::milli>;
            if (!diagQuery_)
            {
                D3D11_QUERY_DESC q{ D3D11_QUERY_EVENT, 0 };
                device_->CreateQuery(&q, &diagQuery_);
            }
            const auto a = clock::now();
            context_->Flush();
            const auto b = clock::now();
            context_->End(diagQuery_.Get());
            BOOL done = FALSE;
            while (context_->GetData(diagQuery_.Get(), &done, sizeof(done), 0) != S_OK || !done) YieldProcessor();
            const auto c = clock::now();
            diagFlushMs_ += ms(b - a).count();
            diagDrainMs_ += ms(c - b).count();
            diagDrainMax_ = (std::max)(diagDrainMax_, ms(c - b).count());
        }
        GpuTimerFrameEnd();
        const auto [interval, presentFlags] = PresentMode(noWait);
        HRESULT hr = (toggle_ & 16) ? S_OK : swapchain_->Present(interval, presentFlags);
        GpuTimerFrameBegin();
        const auto t4 = clock::now();
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { ++presentHist_[7]; hr = S_OK; }
        Probe("present");
        {
            // TTT2_FRAMETIME=1: where a slow present call goes.
            static const bool timing = [] { const char* e = std::getenv("TTT2_FRAMETIME"); return e && *e == '1'; }();
            using ms = std::chrono::duration<double, std::milli>;
            auto printHist = [&] {
                const uint32_t n = (std::max)(presentHist_[6], 1u);
                std::fprintf(stderr, "[frame] DXGI Present over %u calls (toggle %X): <1 ms %u, 1-3 %u, 3-6 %u, 6-10 %u, 10-20 %u, >20 %u, mean %.2f ms, would block %u; flush mean %.2f ms, GPU drain mean %.2f max %.2f ms\n",
                             presentHist_[6], toggle_, presentHist_[0], presentHist_[1], presentHist_[2], presentHist_[3], presentHist_[4], presentHist_[5],
                             presentHistMs_ / n, presentHist_[7], diagFlushMs_ / n, diagDrainMs_ / n, diagDrainMax_);
                presentHist_ = {};
                presentHistMs_ = diagFlushMs_ = diagDrainMs_ = diagDrainMax_ = 0;
            };
            // TTT2_HW_TOGGLE=1 (diagnostics): C:/ttt2/port/hw_toggle.txt, a hex mask re-read every 30
            // presents, switches parts of each frame off live (the picture breaks): 1 draw calls,
            // 2 GPU-VS draws whole (vertex data too), 4 resolve copies, 8 staged readbacks,
            // 16 DXGI Present, 32 constant uploads.
            static const bool toggleFile = [] { const char* e = std::getenv("TTT2_HW_TOGGLE"); return e && *e == '1'; }();
            if (toggleFile && stats_.presents % 30 == 0)
            {
                uint32_t mask = 0;
                if (FILE* f = std::fopen("C:/ttt2/port/hw_toggle.txt", "r")) { if (std::fscanf(f, "%x", &mask) < 1) mask = 0; std::fclose(f); }
                if (mask != toggle_)
                {
                    if (timing) printHist();
                    toggle_ = mask;
                }
            }
            if (timing)
            {
                const double d = ms(t4 - t3).count();
                ++presentHist_[d < 1 ? 0 : d < 3 ? 1 : d < 6 ? 2 : d < 10 ? 3 : d < 20 ? 4 : 5];
                presentHistMs_ += d;
                if (++presentHist_[6] == 600) printHist();
            }
            static int logged = 0;
            if (timing && ms(t4 - t0).count() > 10.0 && logged++ < 300)
                std::fprintf(stderr, "[frame] present call %.1f ms: front texture %.1f, display %.1f, scaled copy %.1f, DXGI Present %.1f\n",
                             ms(t4 - t0).count(), ms(t1 - t0).count(), ms(t2 - t1).count(), ms(t3 - t2).count(), ms(t4 - t3).count());
        }
        buildBudgetMs_ = 24;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) Check(hr, "Present");
        if (hr != lastPresentHr_)
        {
            // A status (e.g. DXGI_STATUS_OCCLUDED 087A0001) means the frame did not reach the screen.
            static int logged = 0;
            if (logged++ < 64)
                std::fprintf(stderr, "[gpu-hw] present result %08X -> %08X (back buffer %ux%u, exclusive %d)\n",
                             static_cast<unsigned>(lastPresentHr_), static_cast<unsigned>(hr), bufW_, bufH_, int(exclusive_.load()));
            if (hr == DXGI_STATUS_OCCLUDED) LogWindowsAbove(w, "present occluded");
            lastPresentHr_ = hr;
        }
        lastFront_ = source; lastFrontW_ = logicalW; lastFrontH_ = logicalH;
        ++stats_.presents;
        if (stats_.presents % 60 == 0 && NativeDumpOn())
        {
            // Diagnostics (TTT2_FB_NATIVE=1): the presented image at its rendered size, once a second.
            D3D11_TEXTURE2D_DESC sd{};
            source->GetDesc(&sd);
            std::vector<uint32_t> px;
            ReadTexture(source, sd.Width, sd.Height, px);
            for (uint32_t& v : px) v = DisplayPixel(v);
            WriteBmp("C:/ttt2/port/fb_native.bmp", px.data(), sd.Width, sd.Height);
        }
        static const bool overrideFile = [] { const char* e = std::getenv("TTT2_HW_OVERRIDE"); return e && *e == '1'; }();
        if (overrideFile && stats_.presents % 30 == 0)
        {
            // Diagnostics (TTT2_HW_OVERRIDE=1): C:/ttt2/port/hw_override.txt = hex mask of texture slots to force white.
            // Optional second token: only pixel shaders of that many dwords.
            uint32_t mask = 0, dwords = 0;
            if (FILE* f = std::fopen("C:/ttt2/port/hw_override.txt", "r"))
            { if (std::fscanf(f, "%x %u", &mask, &dwords) < 1) mask = 0; std::fclose(f); }
            if (mask != overrideMask_ || dwords != overridePsDwords_)
                std::fprintf(stderr, "[gpu-hw] texture override mask %08X ps=%u dwords\n", mask, dwords);
            overrideMask_ = mask; overridePsDwords_ = dwords;
        }
        return true;
    }

    // Copy of the last presented image as BGRX (alpha cleared), for BMP filmstrips.
    bool ReadFront(uint32_t* out)
    {
        if (!lastFront_) return false;
        std::vector<uint32_t> px;
        ReadLogical(lastFront_.Get(), lastFrontW_, lastFrontH_, 1280, 720, true, px);
        for (size_t i = 0; i < px.size(); ++i) out[i] = DisplayPixel(px[i]);
        return true;
    }

    // A stored front-buffer texel as shown: red / blue as presented, the gamma ramp, alpha cleared.
    uint32_t DisplayPixel(uint32_t v) const
    {
        if (lastSwap_) v = (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
        if (gammaApplied_)
            v = (uint32_t(gamma8_[0][(v >> 16) & 0xFF]) << 16) | (uint32_t(gamma8_[1][(v >> 8) & 0xFF]) << 8) |
                gamma8_[2][v & 0xFF];
        return v & 0x00FFFFFFu;
    }
    static bool NativeDumpOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_FB_NATIVE"); return e && *e == '1'; }();
        return on;
    }

private:
    // ---- display gamma: the guest's 256-entry DC_LUT table (kernel_video.cpp), applied to the
    // presented image as the console's scan-out and Xenia's apply-gamma pass do (8 bpc: each
    // channel indexes the table). Skipped while the table is the identity. TTT2_GAMMA_RAMP=0
    // presents the frame buffer unramped.
    static bool GammaRampEnabled()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_GAMMA_RAMP"); return !e || std::strcmp(e, "0") != 0; }();
        return on;
    }
    ComPtr<ID3D11Texture2D> gammaTex_;
    ComPtr<ID3D11ShaderResourceView> gammaSrv_;
    uint32_t gammaVersion_ = ~0u;
    bool gammaApplied_ = false;
    uint8_t gamma8_[3][256] = {};
    void UpdateGamma(const uint32_t* ramp, uint32_t version)
    {
        if (!ramp || version == gammaVersion_) return;
        gammaVersion_ = version;
        float table[256][4];
        bool identity = true;
        for (uint32_t i = 0; i < 256; ++i)
        {
            const uint32_t e = ramp[i], identityValue = i * 0x3FF / 0xFF;
            const uint32_t c[3] = { (e >> 20) & 0x3FF, (e >> 10) & 0x3FF, e & 0x3FF };   // red, green, blue
            for (int k = 0; k < 3; ++k)
            {
                table[i][k] = float(c[k]) / 1023.0f;
                gamma8_[k][i] = uint8_t((c[k] * 255 + 511) / 1023);
                identity = identity && c[k] == identityValue;
            }
            table[i][3] = 1.0f;
        }
        gammaApplied_ = GammaRampEnabled() && !identity;
        static int logged = 0;
        if (logged++ < 4)
            std::fprintf(stderr, "[gpu-hw] display gamma ramp %s (entry 128 = %.4f, identity %.4f)%s\n",
                         identity ? "identity" : "loaded", table[128][0], 128.0f / 255.0f,
                         !identity && !GammaRampEnabled() ? " - not applied (TTT2_GAMMA_RAMP=0)" : "");
        if (!gammaApplied_) return;
        if (!gammaTex_)
        {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = 256; d.Height = 1; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{ table, sizeof(table), 0 };
            Check(device_->CreateTexture2D(&d, &init, &gammaTex_), "CreateGammaTexture");
            Check(device_->CreateShaderResourceView(gammaTex_.Get(), nullptr, &gammaSrv_), "CreateGammaSrv");
        }
        else context_->UpdateSubresource(gammaTex_.Get(), 0, nullptr, table, sizeof(table), 0);
    }
    // ---- window modes (display_settings): the window thread (kernel_video.cpp) gives the window
    // its style and size; this thread follows its client size with the swap chain buffers and
    // owns exclusive fullscreen (entered at the chosen resolution, re-entered only while the
    // window is in the foreground so Alt+Tab is not fought).
    UINT bufW_ = 0, bufH_ = 0;
    bool tearing_ = false;                        // the system supports presents with tearing (VSYNC OFF)
    uint32_t refreshHz_ = 0;                      // the window's display, when the swap chain was made
    UINT SwapChainFlags() const
    {
        return DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    }
    static uint32_t DisplayHz(HWND w)
    {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi) ||
            !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
        return dm.dmDisplayFrequency;
    }
    // Diagnostics: the swap chain's output - name, colour space (HDR or not), bits per colour.
    std::string OutputInfo() const
    {
        ComPtr<IDXGIOutput> out;
        ComPtr<IDXGIOutput6> out6;
        DXGI_OUTPUT_DESC1 d{};
        if (!swapchain_ || FAILED(swapchain_->GetContainingOutput(&out)) || FAILED(out.As(&out6)) || FAILED(out6->GetDesc1(&d)))
            return "output unknown";
        char s[200];
        std::snprintf(s, sizeof(s), "output %ls, colour space %d%s, %u bits per colour", d.DeviceName, int(d.ColorSpace),
                      d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? " (HDR on)" : "", d.BitsPerColor);
        return s;
    }
    // Diagnostics: the visible windows above the game's on its monitor (front first) - what covers an
    // exclusive fullscreen swap chain when DXGI reports it occluded or drops it. At most 12 reports.
    static void LogWindowsAbove(HWND w, const char* why)
    {
        static int reports = 0;
        if (reports++ >= 12) return;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
        std::fprintf(stderr, "[gpu-hw] %s: windows above the game (foreground %p, game %p):\n", why,
                     static_cast<void*>(GetForegroundWindow()), static_cast<void*>(w));
        int n = 0;
        for (HWND h = GetWindow(w, GW_HWNDPREV); h && n < 16; h = GetWindow(h, GW_HWNDPREV))
        {
            RECT r{}, x{};
            if (!IsWindowVisible(h) || !GetWindowRect(h, &r) || !IntersectRect(&x, &r, &mi.rcMonitor)) continue;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            char exe[MAX_PATH] = "?", cls[96] = "?", title[96] = "";
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
            {
                DWORD len = sizeof(exe);
                QueryFullProcessImageNameA(p, 0, exe, &len);
                CloseHandle(p);
            }
            GetClassNameA(h, cls, sizeof(cls));
            GetWindowTextA(h, title, sizeof(title));
            const LONG ex = GetWindowLongA(h, GWL_EXSTYLE);
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            std::fprintf(stderr, "[gpu-hw]   %s pid %lu class \"%s\" title \"%s\" %ld,%ld %ldx%ld%s%s%s%s\n",
                         std::strrchr(exe, '\\') ? std::strrchr(exe, '\\') + 1 : exe, pid, cls, title, r.left, r.top,
                         r.right - r.left, r.bottom - r.top, (ex & WS_EX_TOPMOST) ? " topmost" : "",
                         (ex & WS_EX_LAYERED) ? " layered" : "", (ex & WS_EX_TRANSPARENT) ? " click-through" : "",
                         cloaked ? " cloaked" : "");
            ++n;
        }
        if (!n) std::fprintf(stderr, "[gpu-hw]   (none)\n");
    }
    // VSYNC (Options -> Display Settings, display_settings.h). The game keeps its own 60 Hz pace either
    // way (the vblank ticker, kernel_video.cpp); this only decides when a finished frame is shown.
    //   Windowed / borderless: sync interval 0. ON without tearing - a newer frame replaces a queued
    //     one, so presents never wait for the display or queue up behind it; OFF with
    //     DXGI_PRESENT_ALLOW_TEARING, shown at once wherever Windows lets the swap chain flip by itself
    //     (independent flip). A window DWM composes waits for its next composition either way
    //     (MEASURED 2026-10-20: a borderless window in front was composed on the test PC).
    //   Exclusive fullscreen: sync interval 0 flips at once (tears) there, so ON takes interval 1, with
    //     DXGI_PRESENT_DO_NOT_WAIT: a frame that would wait for the display is dropped instead, so the
    //     display never holds the game under 60 frames a second (a 50 Hz mode shows 50 of them).
    // TTT2_PRESENT_INTERVAL=<n> (experiment) forces the sync interval.
    std::pair<UINT, UINT> PresentMode(bool noWait) const
    {
        static const int forced = [] { const char* e = std::getenv("TTT2_PRESENT_INTERVAL"); return e && *e ? std::clamp(std::atoi(e), 0, 4) : -1; }();
        const bool vsync = display_settings::GetVSync(), exclusive = exclusive_.load();
        UINT interval = exclusive && vsync ? 1u : 0u;
        if (forced >= 0) interval = UINT(forced);
        UINT flags = noWait || interval ? DXGI_PRESENT_DO_NOT_WAIT : 0u;
        if (!vsync && tearing_ && interval == 0 && !exclusive) flags |= DXGI_PRESENT_ALLOW_TEARING;
        return { interval, flags };
    }
    std::atomic<bool> exclusive_{ false };
    std::chrono::steady_clock::time_point lastExclusiveTry_{};
    display_settings::Resolution exclusiveRes_{ 0, 0 };     // display mode set for exclusive fullscreen
    HRESULT lastPresentHr_ = S_OK;
    uint32_t toggle_ = 0;                         // TTT2_HW_TOGGLE
    std::array<uint32_t, 8> presentHist_{};      // TTT2_FRAMETIME: DXGI Present durations, calls, would-block
    double presentHistMs_ = 0;
    ComPtr<ID3D11Query> diagQuery_;              // TTT2_PRESENT_DIAG
    double diagFlushMs_ = 0, diagDrainMs_ = 0, diagDrainMax_ = 0;
    bool resizeNeeded_ = false;                               // resize the buffers at the next present
    std::chrono::steady_clock::time_point lastInvalidRetry_{};
    // Exclusive fullscreen that Windows takes back at once while the game is still in front is retried a
    // few times, then left alone: the window stays a borderless popup over the monitor until the game is
    // switched back to or a display setting changes. MEASURED 2026-10-20 on a PC running Sunshine, Parsec,
    // Virtual Desktop and MuMuPlayer: DXGI kept the swap chain composed by DWM after SetFullscreenState,
    // the first present came back DXGI_STATUS_OCCLUDED and DXGI left fullscreen - and re-entering once a
    // second blinked the screen every second (v0.1.4 to v0.1.6 alike).
    int exclusiveFailures_ = 0;                               // taken back in front of the player, in a row
    bool exclusiveBlocked_ = false;
    bool wasForeground_ = false;
    uint32_t exclusiveGeneration_ = 0;                        // display_settings::Generation() last seen
    std::chrono::steady_clock::time_point exclusiveSince_{};
    void ApplyDisplay(HWND w)
    {
        BOOL fs = FALSE;
        swapchain_->GetFullscreenState(&fs, nullptr);
        const bool want = display_settings::GetMode() == display_settings::Fullscreen;
        const display_settings::Resolution r = display_settings::GetResolution();
        DXGI_MODE_DESC m{};
        m.Width = r.width; m.Height = r.height; m.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        const bool foreground = GetForegroundWindow() == w;
        const uint32_t generation = display_settings::Generation();
        if ((foreground && !wasForeground_) || generation != exclusiveGeneration_)
        {
            const bool settingsChanged = generation != exclusiveGeneration_;
            if (exclusiveBlocked_)
                std::fprintf(stderr, "[gpu-hw] exclusive fullscreen: trying again (%s)\n",
                             settingsChanged ? "display settings changed" : "the game is in front again");
            // Back in front after a block: one try (a failure blocks again at once); new settings: three.
            exclusiveFailures_ = exclusiveBlocked_ && !settingsChanged ? 2 : 0;
            exclusiveBlocked_ = false;
        }
        wasForeground_ = foreground;
        exclusiveGeneration_ = generation;
        if (want && !fs && foreground && !exclusiveBlocked_ &&
            std::chrono::steady_clock::now() - lastExclusiveTry_ > std::chrono::seconds(1))
        {
            lastExclusiveTry_ = std::chrono::steady_clock::now();
            // DXGI picks the fullscreen mode from the buffers' size: a swap chain made while the window was
            // still 1280x720 switched the monitor to 1280x720 first, then to the chosen mode (MEASURED
            // 2026-10-20) - so the buffers take the chosen size before the switch.
            if (bufW_ != r.width || bufH_ != r.height) ResizeSwapBuffers(r.width, r.height);
            swapchain_->ResizeTarget(&m);
            const HRESULT hr = swapchain_->SetFullscreenState(TRUE, nullptr);
            std::fprintf(stderr, "[gpu-hw] exclusive fullscreen %ux%u: %s (%08X)\n", r.width, r.height,
                         SUCCEEDED(hr) ? "on" : "refused", static_cast<unsigned>(hr));
            if (SUCCEEDED(hr))
            {
                m.RefreshRate = {}; swapchain_->ResizeTarget(&m); fs = TRUE; exclusiveRes_ = r;
                exclusiveSince_ = std::chrono::steady_clock::now();
                refreshHz_ = DisplayHz(w);
                std::fprintf(stderr, "[gpu-hw] exclusive fullscreen refresh %u Hz\n", refreshHz_);
            }
        }
        else if (want && fs && (r.width != exclusiveRes_.width || r.height != exclusiveRes_.height))
        {
            // A new RESOLUTION while exclusive: switch the display mode in place (DXGI then resizes
            // the window; the buffers follow below).
            const HRESULT hr = swapchain_->ResizeTarget(&m);
            std::fprintf(stderr, "[gpu-hw] exclusive fullscreen mode -> %ux%u (%08X)\n", r.width, r.height, static_cast<unsigned>(hr));
            exclusiveRes_ = r;
            resizeNeeded_ = true;
        }
        else if (!want && fs)
        {
            swapchain_->SetFullscreenState(FALSE, nullptr);
            fs = FALSE;
            std::fprintf(stderr, "[gpu-hw] exclusive fullscreen off\n");
        }
        // A flip-model swap chain must resize its buffers after every fullscreen transition (ours,
        // or DXGI's own on Alt+Tab) even at an unchanged size: until then every Present fails
        // with DXGI_ERROR_INVALID_CALL and the screen keeps the last frame. MEASURED 2026-10-12:
        // borderless 2560x1440 -> fullscreen 2560x1440 froze the picture. A failing Present also
        // retries the resize (at most once a second).
        if ((fs != FALSE) != exclusive_.load() || resizeNeeded_)
        {
            resizeNeeded_ = true;
            refreshHz_ = DisplayHz(w);          // VSYNC in exclusive fullscreen follows the mode's refresh
        }
        if (lastPresentHr_ == DXGI_ERROR_INVALID_CALL &&
            std::chrono::steady_clock::now() - lastInvalidRetry_ > std::chrono::seconds(1))
        {
            lastInvalidRetry_ = std::chrono::steady_clock::now();
            resizeNeeded_ = true;
        }
        if ((fs != FALSE) != exclusive_.load())
        {
            static int logged = 0;
            if (logged++ < 40)
                std::fprintf(stderr, "[gpu-hw] %.1f ms: exclusive fullscreen state %d -> %d (wanted %d, foreground %d), %s\n",
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                             int(exclusive_.load()), int(fs != FALSE), int(want), int(GetForegroundWindow() == w), OutputInfo().c_str());
            if (!fs && want && foreground)
            {
                // Taken back while the game is in front: not an Alt+Tab. Three quick ones in a row and
                // the game stops asking (see exclusiveBlocked_).
                LogWindowsAbove(w, "exclusive fullscreen lost");
                exclusiveFailures_ = std::chrono::steady_clock::now() - exclusiveSince_ < std::chrono::seconds(10) ? exclusiveFailures_ + 1 : 1;
                if (exclusiveFailures_ >= 3 && !exclusiveBlocked_)
                {
                    exclusiveBlocked_ = true;
                    std::fprintf(stderr, "[gpu-hw] exclusive fullscreen: Windows keeps taking it back while the game is in "
                                         "front (another program capturing or covering the screen?) - staying a borderless "
                                         "window over the whole monitor until the game is switched back to\n");
                }
            }
        }
        exclusive_ = fs != FALSE;
        RECT rc{};
        GetClientRect(w, &rc);
        const UINT cw = std::max<LONG>(1, rc.right - rc.left), ch = std::max<LONG>(1, rc.bottom - rc.top);
        if (cw != bufW_ || ch != bufH_ || resizeNeeded_)
        {
            resizeNeeded_ = false;
            ResizeSwapBuffers(cw, ch);
        }
    }
    void ResizeSwapBuffers(UINT cw, UINT ch)
    {
        backRtv_.Reset();
        backRtvFor_ = nullptr;
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->Flush();
        const HRESULT hr = swapchain_->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, SwapChainFlags());
        if (SUCCEEDED(hr)) { bufW_ = cw; bufH_ = ch; }
        std::fprintf(stderr, "[gpu-hw] back buffer %ux%u%s\n", cw, ch, SUCCEEDED(hr) ? "" : " (resize failed)");
    }

    // The game image into the back buffer: letterboxed to its own aspect, bilinear, red/blue
    // exchanged when the stored order differs from the display swizzle, then the guest's gamma
    // ramp - one pass. Variants: [swap | gamma << 1].
    // ---- the presented image: the front buffer (render resolution) into the window's 16:9 area.
    // Passes (Options -> Display Settings, display_settings.h):
    //   1. display: the red / blue exchange and the game's gamma ramp, texel for texel
    //   2. ANTI-ALIASING FXAA, at render resolution (the edge search of FXAA 3.11's quality preset)
    //   3. scale to the window area: texel for texel when the sizes match; Catmull-Rom bicubic when
    //      enlarging; an area filter when shrinking (bilinear taps under a texel apart across the
    //      pixel's footprint - plain bilinear skipped texels: a 1080p frame in a window a little
    //      smaller rippled, and a resolution above the screen's aliased)
    //   4. SHARPNESS: contrast adaptive sharpening (the filter of AMD FidelityFX CAS, MIT) at the
    //      window's resolution.
    // Sizes equal and both options off: step 1 alone writes the back buffer.
    struct PresentCB { float srcW, srcH, dstW, dstH, originX, originY, sharp; uint32_t mode; };
    ComPtr<ID3D11VertexShader> presentVs_;
    ComPtr<ID3D11PixelShader> displayPs_[4], fxaaPs_, scalePs_, casPs_;
    ComPtr<ID3D11SamplerState> presentSampler_;
    ComPtr<ID3D11Buffer> presentCb_;
    struct PostTarget { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11RenderTargetView> rtv; ComPtr<ID3D11ShaderResourceView> srv; UINT w = 0, h = 0; };
    PostTarget post_[3];
    PostTarget& Post(int i, UINT w, UINT h)
    {
        PostTarget& p = post_[i];
        if (p.tex && p.w == w && p.h == h) return p;
        p = PostTarget{};
        D3D11_TEXTURE2D_DESC t{};
        t.Width = w; t.Height = h; t.MipLevels = t.ArraySize = 1; t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(device_->CreateTexture2D(&t, nullptr, &p.tex), "CreatePostTexture");
        Check(device_->CreateRenderTargetView(p.tex.Get(), nullptr, &p.rtv), "CreatePostRtv");
        Check(device_->CreateShaderResourceView(p.tex.Get(), nullptr, &p.srv), "CreatePostSrv");
        p.w = w; p.h = h;
        return p;
    }
    ComPtr<ID3D11PixelShader> PresentShader(const std::string& body, const char* name)
    {
        const std::string src =
            "cbuffer P : register(b0) { float2 srcSize; float2 dstSize; float2 origin; float sharp; uint mode; };\n"
            "Texture2D<float4> src : register(t0); Texture2D<float4> lut : register(t1); SamplerState lin : register(s0);\n" + body;
        auto code = Compile(src, "ps_5_0", name);
        ComPtr<ID3D11PixelShader> ps;
        Check(device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps), name);
        return ps;
    }
    void PresentShaders()
    {
        if (presentVs_) return;
        auto vs = Compile("struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };"
                          " O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);"
                          " o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = t; return o; }",
                          "vs_5_0", "ttt2 present vs");
        Check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &presentVs_), "CreatePresentVs");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        Check(device_->CreateSamplerState(&sd, &presentSampler_), "CreatePresentSampler");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(PresentCB); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device_->CreateBuffer(&bd, nullptr, &presentCb_), "CreatePresentCb");
        for (int v = 0; v < 4; ++v)
        {
            std::string body =
                "float4 main(float4 pos : SV_Position) : SV_Target {\n"
                "  float4 c = src.Load(int3(int2(pos.xy - origin), 0))";
            body += (v & 1) ? ".bgra;\n" : ";\n";
            if (v & 2)
                body += "  uint3 i = (uint3)round(saturate(c.rgb) * 255.0);\n"
                        "  c.rgb = float3(lut.Load(int3(i.r, 0, 0)).r, lut.Load(int3(i.g, 0, 0)).g, lut.Load(int3(i.b, 0, 0)).b);\n";
            body += "  return float4(c.rgb, 1); }\n";
            displayPs_[v] = PresentShader(body, "ttt2 present display ps");
        }
        // FXAA 3.11 quality (preset 39's steps), luma from the colour.
        fxaaPs_ = PresentShader(
            "float L(float2 p) { return dot(src.SampleLevel(lin, p, 0).rgb, float3(0.299, 0.587, 0.114)); }\n"
            "float4 main(float4 pos : SV_Position) : SV_Target {\n"
            "  float2 rf = 1.0 / srcSize; float2 posM = (pos.xy - origin) * rf;\n"
            "  float4 rgbyM = src.SampleLevel(lin, posM, 0); float lumaM = dot(rgbyM.rgb, float3(0.299, 0.587, 0.114));\n"
            "  float lumaS = L(posM + float2(0, 1) * rf), lumaE = L(posM + float2(1, 0) * rf);\n"
            "  float lumaN = L(posM + float2(0, -1) * rf), lumaW = L(posM + float2(-1, 0) * rf);\n"
            "  float rangeMax = max(max(max(lumaS, lumaM), lumaE), max(lumaN, lumaW));\n"
            "  float rangeMin = min(min(min(lumaS, lumaM), lumaE), min(lumaN, lumaW));\n"
            "  float range = rangeMax - rangeMin;\n"
            "  if (range < max(0.0833, rangeMax * 0.166)) return float4(rgbyM.rgb, 1);\n"
            "  float lumaNW = L(posM + float2(-1, -1) * rf), lumaSE = L(posM + float2(1, 1) * rf);\n"
            "  float lumaNE = L(posM + float2(1, -1) * rf), lumaSW = L(posM + float2(-1, 1) * rf);\n"
            "  float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;\n"
            "  float subpixRcpRange = 1.0 / range, subpixNSWE = lumaNS + lumaWE;\n"
            "  float edgeHorz1 = -2.0 * lumaM + lumaNS, edgeVert1 = -2.0 * lumaM + lumaWE;\n"
            "  float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;\n"
            "  float edgeHorz2 = -2.0 * lumaE + lumaNESE, edgeVert2 = -2.0 * lumaN + lumaNWNE;\n"
            "  float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;\n"
            "  float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2), edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);\n"
            "  float edgeHorz3 = -2.0 * lumaW + lumaNWSW, edgeVert3 = -2.0 * lumaS + lumaSWSE;\n"
            "  float edgeHorz = abs(edgeHorz3) + edgeHorz4, edgeVert = abs(edgeVert3) + edgeVert4;\n"
            "  float subpixNWSWNESE = lumaNWSW + lumaNESE;\n"
            "  bool horzSpan = edgeHorz >= edgeVert;\n"
            "  float lengthSign = horzSpan ? rf.y : rf.x;\n"
            "  float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;\n"
            "  if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }\n"
            "  float subpixB = subpixA * (1.0 / 12.0) - lumaM;\n"
            "  float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;\n"
            "  float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;\n"
            "  bool pairN = abs(gradientN) >= abs(gradientS);\n"
            "  float gradient = max(abs(gradientN), abs(gradientS));\n"
            "  if (pairN) lengthSign = -lengthSign;\n"
            "  float subpixC = saturate(abs(subpixB) * subpixRcpRange);\n"
            "  float2 posB = posM;\n"
            "  float2 offNP = horzSpan ? float2(rf.x, 0) : float2(0, rf.y);\n"
            "  if (!horzSpan) posB.x += lengthSign * 0.5; else posB.y += lengthSign * 0.5;\n"
            "  float2 posN = posB - offNP, posP = posB + offNP;\n"
            "  float subpixD = -2.0 * subpixC + 3.0, subpixE = subpixC * subpixC;\n"
            "  float lumaEndN = L(posN), lumaEndP = L(posP);\n"
            "  if (!pairN) lumaNN = lumaSS;\n"
            "  float gradientScaled = gradient * 0.25, lumaMM = lumaM - lumaNN * 0.5, subpixF = subpixD * subpixE;\n"
            "  bool lumaMLTZero = lumaMM < 0.0;\n"
            "  lumaEndN -= lumaNN * 0.5; lumaEndP -= lumaNN * 0.5;\n"
            "  bool doneN = abs(lumaEndN) >= gradientScaled, doneP = abs(lumaEndP) >= gradientScaled;\n"
            "  static const float steps[11] = { 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };\n"
            "  [loop] for (int k = 0; k < 11 && !(doneN && doneP); ++k) {\n"
            "    if (!doneN) posN -= offNP * steps[k];\n"
            "    if (!doneP) posP += offNP * steps[k];\n"
            "    if (!doneN) { lumaEndN = L(posN) - lumaNN * 0.5; doneN = abs(lumaEndN) >= gradientScaled; }\n"
            "    if (!doneP) { lumaEndP = L(posP) - lumaNN * 0.5; doneP = abs(lumaEndP) >= gradientScaled; }\n"
            "  }\n"
            "  float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;\n"
            "  float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;\n"
            "  bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero, goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;\n"
            "  float spanLengthRcp = 1.0 / (dstP + dstN);\n"
            "  bool directionN = dstN < dstP;\n"
            "  float dst = min(dstN, dstP);\n"
            "  bool goodSpan = directionN ? goodSpanN : goodSpanP;\n"
            "  float subpixG = subpixF * subpixF;\n"
            "  float pixelOffset = dst * -spanLengthRcp + 0.5;\n"
            "  float subpixH = subpixG * 0.75;\n"
            "  float pixelOffsetSubpix = max(goodSpan ? pixelOffset : 0.0, subpixH);\n"
            "  if (!horzSpan) posM.x += pixelOffsetSubpix * lengthSign; else posM.y += pixelOffsetSubpix * lengthSign;\n"
            "  return float4(src.SampleLevel(lin, posM, 0).rgb, 1); }\n",
            "ttt2 present fxaa ps");
        // Scale: mode 0 texel for texel, 1 Catmull-Rom (9 bilinear taps), 2 area filter.
        scalePs_ = PresentShader(
            "float4 main(float4 pos : SV_Position) : SV_Target {\n"
            "  float2 p = pos.xy - origin;\n"
            "  if (mode == 0) return float4(src.Load(int3(int2(p), 0)).rgb, 1);\n"
            "  float2 s = p / dstSize * srcSize;\n"
            "  if (mode == 1) {\n"
            "    float2 t1 = floor(s - 0.5) + 0.5, f = s - t1;\n"
            "    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
            "    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
            "    float2 w12 = w1 + w2, t12 = (t1 + w2 / w12) / srcSize, t0 = (t1 - 1.0) / srcSize, t3 = (t1 + 2.0) / srcSize;\n"
            "    float3 r = src.SampleLevel(lin, float2(t0.x, t0.y), 0).rgb * w0.x * w0.y\n"
            "             + src.SampleLevel(lin, float2(t12.x, t0.y), 0).rgb * w12.x * w0.y\n"
            "             + src.SampleLevel(lin, float2(t3.x, t0.y), 0).rgb * w3.x * w0.y\n"
            "             + src.SampleLevel(lin, float2(t0.x, t12.y), 0).rgb * w0.x * w12.y\n"
            "             + src.SampleLevel(lin, float2(t12.x, t12.y), 0).rgb * w12.x * w12.y\n"
            "             + src.SampleLevel(lin, float2(t3.x, t12.y), 0).rgb * w3.x * w12.y\n"
            "             + src.SampleLevel(lin, float2(t0.x, t3.y), 0).rgb * w0.x * w3.y\n"
            "             + src.SampleLevel(lin, float2(t12.x, t3.y), 0).rgb * w12.x * w3.y\n"
            "             + src.SampleLevel(lin, float2(t3.x, t3.y), 0).rgb * w3.x * w3.y;\n"
            "    return float4(saturate(r), 1);\n"
            "  }\n"
            "  float2 ratio = srcSize / dstSize;\n"
            "  float2 a = (floor(p) + 0.0) * ratio, b = a + ratio;   // the pixel's footprint, source texels\n"
            "  int2 i0 = int2(floor(a)), i1 = min(int2(ceil(b)), int2(srcSize)) - 1;\n"
            "  float3 sum = 0; float wsum = 0;\n"
            "  [loop] for (int y = i0.y; y <= i1.y && y < i0.y + 12; ++y) {\n"
            "    float wy = min(b.y, y + 1.0) - max(a.y, (float)y);\n"
            "    [loop] for (int x = i0.x; x <= i1.x && x < i0.x + 12; ++x) {\n"
            "      float w = wy * (min(b.x, x + 1.0) - max(a.x, (float)x));\n"
            "      sum += src.Load(int3(x, y, 0)).rgb * w; wsum += w;\n"
            "    }\n"
            "  }\n"
            "  return float4(sum / max(wsum, 1e-6), 1); }\n",
            "ttt2 present scale ps");
        // Contrast adaptive sharpening (AMD FidelityFX CAS, no scaling): sharp 0..1.
        casPs_ = PresentShader(
            "float3 T(int2 p) { return src.Load(int3(clamp(p, int2(0, 0), int2(dstSize) - 1), 0)).rgb; }\n"
            "float4 main(float4 pos : SV_Position) : SV_Target {\n"
            "  int2 p = int2(pos.xy - origin);\n"
            "  float3 a = T(p + int2(-1, -1)), b = T(p + int2(0, -1)), c = T(p + int2(1, -1));\n"
            "  float3 d = T(p + int2(-1, 0)), e = T(p), f = T(p + int2(1, 0));\n"
            "  float3 g = T(p + int2(-1, 1)), h = T(p + int2(0, 1)), i = T(p + int2(1, 1));\n"
            "  float3 mn = min(min(min(d, e), min(f, b)), h);\n"
            "  float3 mn2 = min(mn, min(min(a, c), min(g, i)));\n"
            "  mn += mn2;\n"
            "  float3 mx = max(max(max(d, e), max(f, b)), h);\n"
            "  float3 mx2 = max(mx, max(max(a, c), max(g, i)));\n"
            "  mx += mx2;\n"
            "  float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));\n"
            "  float3 w = amp * (-1.0 / lerp(8.0, 5.0, sharp));\n"
            "  float3 o = saturate((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w));\n"
            "  return float4(o, 1); }\n",
            "ttt2 present cas ps");
    }
    void PostPass(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv,
                  const PresentCB& cb)
    {
        context_->UpdateSubresource(presentCb_.Get(), 0, nullptr, &cb, 0, 0);
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->RSSetViewports(1, &vp);
        const D3D11_RECT sc{ LONG(vp.TopLeftX), LONG(vp.TopLeftY), LONG(vp.TopLeftX + vp.Width), LONG(vp.TopLeftY + vp.Height) };
        context_->RSSetScissorRects(1, &sc);
        context_->PSSetShader(ps, nullptr, 0);
        ID3D11ShaderResourceView* v[2] = { srv, gammaSrv_.Get() };
        context_->PSSetShaderResources(0, 2, v);
        ID3D11Buffer* b = presentCb_.Get();
        context_->PSSetConstantBuffers(0, 1, &b);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = { nullptr, nullptr };
        context_->PSSetShaderResources(0, 2, none);
    }
    void PresentScaled(ID3D11Texture2D* source, ID3D11Texture2D* back, bool swap)
    {
        PresentShaders();
        if (backRtvFor_ != back)
        {
            backRtv_.Reset();
            Check(device_->CreateRenderTargetView(back, nullptr, &backRtv_), "CreateBackRtv");
            backRtvFor_ = back;
        }
        D3D11_TEXTURE2D_DESC sd{};
        source->GetDesc(&sd);
        // The window's 16:9 area, whole pixels (texel for texel when it matches the render size).
        const float scale = std::min(float(bufW_) / 1280.0f, float(bufH_) / 720.0f);
        const UINT vw = std::max(1u, UINT(std::lround(1280.0f * scale))), vh = std::max(1u, UINT(std::lround(720.0f * scale)));
        const UINT vx = (bufW_ - std::min(vw, bufW_)) / 2, vy = (bufH_ - std::min(vh, bufH_)) / 2;
        const D3D11_VIEWPORT out{ float(vx), float(vy), float(vw), float(vh), 0, 1 };
        const float black[4] = { 0, 0, 0, 1 };
        ID3D11RenderTargetView* rtv = backRtv_.Get();
        context_->ClearRenderTargetView(rtv, black);
        ComPtr<ID3D11ShaderResourceView> srv;
        Check(device_->CreateShaderResourceView(source, nullptr, &srv), "CreatePresentSrv");
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(nullptr, 0);
        context_->RSSetState(raster_.Get());
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(presentVs_.Get(), nullptr, 0);
        ID3D11SamplerState* s = presentSampler_.Get();
        context_->PSSetSamplers(0, 1, &s);
        ID3D11PixelShader* display = displayPs_[(swap ? 1 : 0) | (gammaApplied_ ? 2 : 0)].Get();
        const UINT sw = sd.Width, sh = sd.Height;
        const int sharpness = display_settings::GetSharpness(), aa = display_settings::GetAntiAliasing();
        const bool resample = sw != vw || sh != vh;
        PresentCB cb{ float(sw), float(sh), float(vw), float(vh), float(vx), float(vy), 0.0f, 0 };
        if (!resample && !aa && !sharpness) { PostPass(rtv, out, display, srv.Get(), cb); return; }
        const D3D11_VIEWPORT full{ 0, 0, float(sw), float(sh), 0, 1 };
        PresentCB at0{ float(sw), float(sh), float(sw), float(sh), 0, 0, 0.0f, 0 };
        PostTarget& p0 = Post(0, sw, sh);
        PostPass(p0.rtv.Get(), full, display, srv.Get(), at0);
        ID3D11ShaderResourceView* cur = p0.srv.Get();
        if (aa)
        {
            PostTarget& p1 = Post(1, sw, sh);
            PostPass(p1.rtv.Get(), full, fxaaPs_.Get(), cur, at0);
            cur = p1.srv.Get();
        }
        cb.mode = !resample ? 0u : vw > sw ? 1u : 2u;
        if (sharpness)
        {
            if (resample)
            {
                PostTarget& p2 = Post(2, vw, vh);
                const D3D11_VIEWPORT area{ 0, 0, float(vw), float(vh), 0, 1 };
                PresentCB toArea = cb; toArea.originX = toArea.originY = 0;
                PostPass(p2.rtv.Get(), area, scalePs_.Get(), cur, toArea);
                cur = p2.srv.Get();
            }
            PresentCB sharp{ float(vw), float(vh), float(vw), float(vh), float(vx), float(vy), float(sharpness - 1) / 9.0f, 0 };
            PostPass(rtv, out, casPs_.Get(), cur, sharp);
        }
        else PostPass(rtv, out, scalePs_.Get(), cur, cb);
    }
public:
    bool IsExclusiveFullscreen() const { return exclusive_.load(); }

private:
    static constexpr UINT VertexRingBytes = 64u << 20, IndexRingBytes = 16u << 20, VdataRingBytes = 64u << 20;

    // Pass-through vertex shaders by interpolator count: the CPU already produced clip-space
    // vertices; the pixel shader always receives 16 interpolators (the unsent ones are zero,
    // exactly what the interpreter sees for interpolators a vertex shader never exported).
    struct PassthroughVs { ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11InputLayout> layout; };
    std::array<PassthroughVs, 17> passthrough_;
    PassthroughVs& Passthrough(uint32_t n)
    {
        PassthroughVs& p = passthrough_[n];
        if (p.vs) return p;
        std::string src = "struct V { float4 p : POSITION;";
        if (n) src += " float4 a[" + std::to_string(n) + "] : TEXCOORD0;";
        src += " };\nstruct O { float4 p : SV_Position; float4 a[16] : TEXCOORD0; };\n"
               "O main(V v) { O o; o.p = v.p; [unroll] for (int i = 0; i < 16; ++i) o.a[i] = float4(0, 0, 0, 0);\n";
        if (n) src += "  [unroll] for (int k = 0; k < " + std::to_string(n) + "; ++k) o.a[k] = v.a[k];\n";
        src += "  return o; }\n";
        auto code = Compile(src, "vs_5_0", "ttt2 passthrough vs");
        Check(device_->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &p.vs), "CreateVertexShader");
        D3D11_INPUT_ELEMENT_DESC elements[17]{};
        elements[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
        for (UINT i = 0; i < n; ++i)
            elements[i + 1] = { "TEXCOORD", i, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16 + i * 16, D3D11_INPUT_PER_VERTEX_DATA, 0 };
        Check(device_->CreateInputLayout(elements, n + 1, code->GetBufferPointer(), code->GetBufferSize(), &p.layout), "CreateInputLayout");
        return p;
    }

    // extW / extH: the logical area held, in samples (the texture is `scale` times that: the
    // render scale, or 1 for a surface rendered at native size - see NativeSize).
    struct ColorSurface { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11RenderTargetView> rtv; ComPtr<ID3D11ShaderResourceView> srv;
                          bool touched = false; uint32_t samplePitch = 0; DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
                          UINT extW = 0, extH = 0; float scale = 1.0f; };
    struct DepthSurface { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11DepthStencilView> dsv; ComPtr<ID3D11ShaderResourceView> srv;
                          bool touched = false; uint32_t sampX = 1, sampY = 1; UINT extW = 0, extH = 0; float scale = 1.0f; };
    struct Resolved
    {
        ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11RenderTargetView> rtv;
        uint32_t pitch = 0, height = 0, copiedWidth = 0, size = 0, seq = 0;   // logical (guest) pixels
        bool valid = false, cpuCoherent = false, swapRB = false, depth = false, depthFloat = false;
        bool rg16f = false;          // 16_16_FLOAT destination (R16G16_FLOAT texture)
        float scale = 1.0f;          // render scale the texture was made at (its source surface's)
        uint32_t sourceKey = ~0u, sourceDepth = ~0u;   // colour surface key / depth base it came from
        uint32_t sourcePitch = 0;                      // that surface's sample pitch at the resolve (UpdateNative)
        // CPU readback (WriteBack). Once the CPU has read this destination, each later resolve
        // starts its copy to `staging` at once (StageReadback), so the read finds it done instead
        // of waiting for the GPU: the fight intro reads its 1024x1024 bake ~100 times, 9.4 ms each
        // (MEASURED 2026-10-14, every ~4th frame late).
        bool readbackHot = false, staged = false;
        uint32_t stagedSeq = 0;
        std::chrono::steady_clock::time_point stagedAt{};   // when the staged copy was queued
        uint64_t stagedFence = 0;                            // readbackFence_ value after that copy
        ComPtr<ID3D11Texture2D> logical;          // scaled: the logical-size copy the CPU reads
        ComPtr<ID3D11RenderTargetView> logicalRtv;
        ComPtr<ID3D11Texture2D> staging;
    };
    struct TexKey
    {
        uint32_t base, format, width, height, pitch, tiled, endian, swizzle, slices;   // slices: count | 0x100 if array
        uint32_t mipBase, levels;                                                      // levels | 0x100 if packed
        bool operator==(const TexKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
    };
    struct TexKeyHash { size_t operator()(const TexKey& k) const { return (size_t)Fnv(&k, sizeof(k)); } };
    struct CachedTexture
    {
        ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv;
        uint32_t mode = 0, rangeStart = 0, rangeSize = 0, seq = 0, allocW = 1, allocH = 1, slices = 1;
        uint32_t levels = 1, mipStart = 0, mipSize = 0;     // mip levels held; their guest bytes (levels 1+)
        bool genericSwizzled = false;
    };
    struct Bound { ID3D11ShaderResourceView* srv; uint32_t swizzle, mode, width, height; float scaleX, scaleY; uint32_t slices = 1, levels = 1; };
    struct Stats { uint64_t draws = 0, triangles = 0, skippedDraws = 0, emptyDraws = 0, resolves = 0, presents = 0,
                   compiles = 0, diskHits = 0, uploads = 0, uploadBytes = 0, resolvedHits = 0, writebacks = 0,
                   depthResolves = 0, depthHits = 0, gpuVsDraws = 0, vertexDataBytes = 0, warmHits = 0, pendingSkips = 0, stagedReadbacks = 0, ringWaits = 0, resolveCreates = 0, resolvePoolHits = 0, shadowSkips = 0; double ringWaitMs = 0; };
    // ---- SHADOWS OFF (display_settings::GetShadows): the game's cast shadows come from square depth maps - about
    // 50 depth-only draws per frame (colour writes off, depth written, compare GEQUAL; cleared by draws with compare
    // ALWAYS) into an EDRAM depth surface, resolved as a 512 / 1024 / 2048 square depth texture, turned into an
    // 8_8_8_8 shadow texture the scene samples (MEASURED 2026-10-22 in frame traces; every other depth resolve -
    // depth of field, soft particles - is screen-shaped). The square resolve names the surface. With SHADOWS OFF
    // its caster draws are skipped (the GPU saves the pass) and its resolve writes 0.0 everywhere: no occluder
    // (MEASURED, same stage and pose: a map left with only its clear came out as shadow over most of the floor,
    // 1.0 shadowed all of it, 0.0 lit all of it - fighters unchanged). Drawing only - the game never reads it back.
    struct ShadowSurface { uint32_t base = 0, size = 0; uint64_t seen = 0; };
    ShadowSurface shadowSurfaces_[4];
    void NoteShadowResolve(uint32_t depthBase, uint32_t pitch, uint32_t height)
    {
        if (pitch != height || (pitch != 512 && pitch != 1024 && pitch != 2048)) return;
        ShadowSurface* slot = nullptr;
        for (ShadowSurface& e : shadowSurfaces_)
            if (e.size && e.base == depthBase && e.size == pitch) { slot = &e; break; }
        if (!slot)
        {
            slot = &shadowSurfaces_[0];
            for (ShadowSurface& e : shadowSurfaces_) if (e.seen < slot->seen) slot = &e;   // the oldest (or a free one)
            if (!slot->size || slot->base != depthBase)
                std::fprintf(stderr, "[gpu-hw] shadow map: depth surface %03X, %ux%u\n", depthBase, pitch, pitch);
            slot->base = depthBase;
            slot->size = pitch;
        }
        slot->seen = stats_.presents + 1;
    }
    // A caster: a depth-only draw that writes depth with a real compare into a shadow map's surface (its surface
    // pitch is the map's size or a little more), seen resolved in the last 10 s.
    bool ShadowMapOff(uint32_t depthBase, uint32_t pitch, uint32_t height) const
    {
        if (display_settings::GetShadows() || pitch != height) return false;
        for (const ShadowSurface& e : shadowSurfaces_)
            if (e.size == pitch && e.base == depthBase) return true;
        return false;
    }
    bool ShadowCaster(const DrawState& st) const
    {
        if (st.writeMask || !st.zEnable || !st.zWrite || st.zFunc == 7 || display_settings::GetShadows()) return false;
        for (const ShadowSurface& e : shadowSurfaces_)
            if (e.size && e.base == st.depthBase && st.pitch >= e.size && st.pitch <= e.size + 64 && stats_.presents < e.seen + 600)
                return true;
        return false;
    }
public:
    // Device-removed diagnostics for the failure log (DXGI reason code).
    HRESULT RemovedReason() const { return device_ ? device_->GetDeviceRemovedReason() : S_OK; }
private:
    // Per-format upload accounting (format -> {uploads, bytes}) and writeback causes, reported
    // with the periodic stats so the next fast path is chosen from measured data.
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> uploadByFormat_;
    std::map<std::string, uint64_t> writebackCauses_;
    std::map<uint32_t, uint64_t> undecodable_;         // texture format -> fetch binds

    // Format census (TTT2_FORMAT_CENSUS=1): the first use of every texture format / swizzle /
    // endian / decode path, colour-target format and vertex-fetch format is logged as
    // "[census] ...", so each one can be checked against a hardware-verified component order
    // (the 8_8_8_8 red/blue mix-up of 2026-10-12 hid in formats nobody had listed).
    static bool CensusOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_FORMAT_CENSUS"); return e && *e == '1'; }();
        return on;
    }
    enum CensusPath : uint32_t { CensusResolved, CensusDepth, CensusUndecodable, CensusUpload, CensusResolveBytes };
    std::map<uint64_t, uint64_t> censusSeen_;
    void CensusTexture(const Texture& t, uint32_t path)
    {
        static const char* names[] = { "gpu-resolve", "depth-resolve", "undecodable", "upload", "upload-of-resolve-bytes" };
        const uint64_t key = uint64_t(t.format) | uint64_t(t.swizzle) << 8 | uint64_t(t.endian) << 20 |
                             uint64_t(t.tiled ? 1 : 0) << 22 | uint64_t(t.dimension) << 23 | uint64_t(path) << 26 |
                             uint64_t(t.signs) << 32;
        if (censusSeen_[key]++) return;
        std::fprintf(stderr, "[census] texture fmt=%u swz=%03X endian=%u %s dim=%u signs=%02X%s path=%s (first: base=%08X %ux%u)\n",
                     t.format, t.swizzle, t.endian, t.tiled ? "tiled" : "linear", t.dimension, t.signs,
                     (t.signs & 0x3F) == 0x3F ? " GAMMA" : "", names[path], t.base, t.width, t.height);
    }
    // TTT2_MIP_CENSUS=1 (diagnostics): per distinct uploaded texture its mip / filter fields, a running
    // summary, and for the first textures with mips each level decoded where TextureMipLevel says
    // (variants 0, 1, 2, 4) against the 2x2 box of the level above (mean absolute difference, 0..1),
    // with the same against the box shifted half a level away (what unrelated texels give).
    static bool MipCensusOn() { static const bool on = [] { const char* e = std::getenv("TTT2_MIP_CENSUS"); return e && *e == '1'; }(); return on; }
    std::set<uint64_t> mipSeen_;
    uint32_t mipChecked_ = 0;
    uint64_t mipCount_[8] = {}, mipFilterHist_[4] = {}, minFilterHist_[4] = {}, magFilterHist_[4] = {}, anisoHist_[8] = {};
    static std::vector<V4> MipDecode(const Texture& t, const MipLevel& m)
    {
        Texture lt = t;
        lt.base = m.base; lt.pitch = m.pitch; lt.width = m.x + m.width; lt.height = m.y + m.height;
        lt.clampX = lt.clampY = 2;
        std::vector<V4> out(size_t(m.width) * m.height);
        for (uint32_t y = 0; y < m.height; ++y)
            for (uint32_t x = 0; x < m.width; ++x) out[size_t(y) * m.width + x] = SampleTexel(lt, int(m.x + x), int(m.y + y));
        return out;
    }
    static double MipError(const std::vector<V4>& up, uint32_t uw, uint32_t uh, const std::vector<V4>& lo, uint32_t lw, uint32_t lh, bool shifted)
    {
        double sum = 0;
        for (uint32_t y = 0; y < lh; ++y)
            for (uint32_t x = 0; x < lw; ++x)
            {
                const uint32_t sx = shifted ? (x + lw / 2) % lw : x, sy = shifted ? (y + lh / 2) % lh : y;
                for (int c = 0; c < 4; ++c)
                {
                    float b = 0;
                    for (uint32_t k = 0; k < 4; ++k)
                    {
                        const uint32_t ux = std::min(2 * sx + (k & 1), uw - 1), uy = std::min(2 * sy + (k >> 1), uh - 1);
                        b += up[size_t(uy) * uw + ux][c];
                    }
                    sum += std::fabs(lo[size_t(y) * lw + x][c] - b * 0.25f);
                }
            }
        return sum / (double(lw) * lh * 4);
    }
    void MipCensus(const Texture& t)
    {
        const uint64_t key = uint64_t(t.base) << 32 ^ uint64_t(t.width) << 18 ^ uint64_t(t.height) << 4 ^ t.format;
        if (!mipSeen_.insert(key).second) return;
        ++mipCount_[0];
        if (t.mipMax > 0) ++mipCount_[1];
        if (t.mipBase) ++mipCount_[2];
        if (t.packedMips) ++mipCount_[3];
        if (t.mipMin > 0) ++mipCount_[4];
        if (t.lodBias) ++mipCount_[5];
        if (t.tiled) ++mipCount_[6];
        if (IsBlockCompressed(t.format)) ++mipCount_[7];
        ++mipFilterHist_[t.mipFilter]; ++minFilterHist_[t.minFilter]; ++magFilterHist_[t.magFilter & 3]; ++anisoHist_[t.aniso];
        const uint64_t n = mipCount_[0];
        if (n == 25 || n == 50 || n == 100 || n == 200 || n == 400 || n == 800 || n == 1600)
            std::fprintf(stderr, "[mip] %llu textures: %llu with levels past 0 (mip max), %llu with a mip address, %llu packed, %llu first level > 0, %llu LOD bias, "
                         "%llu tiled, %llu block-compressed; mip filter point/linear/base/fc %llu/%llu/%llu/%llu, min %llu/%llu/%llu/%llu, mag %llu/%llu/%llu/%llu, "
                         "aniso 0..7 %llu %llu %llu %llu %llu %llu %llu %llu\n",
                         (unsigned long long)n, (unsigned long long)mipCount_[1], (unsigned long long)mipCount_[2], (unsigned long long)mipCount_[3],
                         (unsigned long long)mipCount_[4], (unsigned long long)mipCount_[5], (unsigned long long)mipCount_[6], (unsigned long long)mipCount_[7],
                         (unsigned long long)mipFilterHist_[0], (unsigned long long)mipFilterHist_[1], (unsigned long long)mipFilterHist_[2], (unsigned long long)mipFilterHist_[3],
                         (unsigned long long)minFilterHist_[0], (unsigned long long)minFilterHist_[1], (unsigned long long)minFilterHist_[2], (unsigned long long)minFilterHist_[3],
                         (unsigned long long)magFilterHist_[0], (unsigned long long)magFilterHist_[1], (unsigned long long)magFilterHist_[2], (unsigned long long)magFilterHist_[3],
                         (unsigned long long)anisoHist_[0], (unsigned long long)anisoHist_[1], (unsigned long long)anisoHist_[2], (unsigned long long)anisoHist_[3],
                         (unsigned long long)anisoHist_[4], (unsigned long long)anisoHist_[5], (unsigned long long)anisoHist_[6], (unsigned long long)anisoHist_[7]);
        if (t.mipMax == 0 || mipChecked_ >= 60 || t.width * t.height > 1024 * 1024 || (!FormatBpp(t.format) && !IsBlockCompressed(t.format))) return;
        ++mipChecked_;
        const uint32_t levels = std::min<uint32_t>(t.mipMax, Log2Ceil(std::max(t.width, t.height)));
        std::string line;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "[mip] check fmt=%u %ux%u p%u %s%s levels %u..%u base %08X mips %08X min/mip/aniso %u/%u/%u bias %d:",
                      t.format, t.width, t.height, t.pitch, t.tiled ? "tiled" : "linear", t.packedMips ? " packed" : "",
                      t.mipMin, t.mipMax, t.base, t.mipBase, t.minFilter, t.mipFilter, t.aniso, t.lodBias);
        line = buf;
        static const uint32_t variants[] = { 0, 1, 2, 4 };
        for (int vi = 0; vi < 5; ++vi)
        {
            const bool baseline = vi == 4;
            const uint32_t variant = baseline ? 0 : variants[vi];
            std::snprintf(buf, sizeof(buf), baseline ? " | shifted" : " | v%u", variant);
            line += buf;
            MipLevel m0;
            TextureMipLevel(t, 0, m0, variant);
            std::vector<V4> up = MipDecode(t, m0);
            uint32_t uw = m0.width, uh = m0.height;
            for (uint32_t l = 1; l <= levels; ++l)
            {
                MipLevel m;
                if (!TextureMipLevel(t, l, m, variant)) { line += " -"; break; }
                std::vector<V4> lo = MipDecode(t, m);
                std::snprintf(buf, sizeof(buf), " %.3f", MipError(up, uw, uh, lo, m.width, m.height, baseline));
                line += buf;
                up.swap(lo); uw = m.width; uh = m.height;
            }
        }
        std::fprintf(stderr, "%s\n", line.c_str());
        // Where are the levels past the 4x4 one really? Search the packed level's first tile.
        const uint32_t ls = Log2Ceil(std::min(t.width, t.height));
        const uint32_t packedLevel = ls > 4 ? ls - 4 : 0;
        if (!t.packedMips || !IsBlockCompressed(t.format) || levels < packedLevel + 3 || mipSearched_ >= 12) return;
        ++mipSearched_;
        MipLevel pm;
        TextureMipLevel(t, packedLevel, pm);
        if (dumpedTile_.insert(t.format).second)
        {
            // The packed tile: 32x32 blocks (128x128 texels) from the packed level's storage, 4x enlarged.
            Texture lt = t;
            lt.base = pm.base; lt.pitch = pm.pitch; lt.width = 128; lt.height = 128; lt.clampX = lt.clampY = 2; lt.swizzle = 0x688;
            std::vector<uint32_t> px(512 * 512);
            for (uint32_t y = 0; y < 512; ++y)
                for (uint32_t x = 0; x < 512; ++x)
                {
                    const V4 v = SampleTexel(lt, int(x / 4), int(y / 4));
                    auto b = [](float f) { return uint32_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
                    px[size_t(y) * 512 + x] = (b(v[0]) << 16) | (b(v[1]) << 8) | b(v[2]);
                }
            char path[96];
            std::snprintf(path, sizeof(path), "C:/ttt2/port/miptile_%u.bmp", t.format);
            WriteBmp(path, px.data(), 512, 512);
            std::fprintf(stderr, "[mip] packed tile of fmt=%u %ux%u (level %u at %08X pitch %u) -> %s\n", t.format, t.width, t.height, packedLevel, pm.base, pm.pitch, path);
        }
        MipLevel um;
        TextureMipLevel(t, packedLevel + 2, um);
        std::vector<V4> up = MipDecode(t, um);
        for (uint32_t l = packedLevel + 3; l <= levels; ++l)
        {
            MipLevel m;
            TextureMipLevel(t, l, m);
            struct Hit { double e; uint32_t bx, by; };
            std::vector<Hit> hits;
            for (uint32_t by = 0; by < 32; ++by)
                for (uint32_t bx = 0; bx < pm.pitch / 4; ++bx)
                {
                    MipLevel c = m;
                    c.base = pm.base; c.pitch = pm.pitch; c.x = bx * 4; c.y = by * 4;
                    hits.push_back({ MipError(up, um.width, um.height, MipDecode(t, c), m.width, m.height, false), bx, by });
                }
            std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.e < b.e; });
            std::string s;
            char b2[64];
            for (size_t i = 0; i < 5 && i < hits.size(); ++i) { std::snprintf(b2, sizeof(b2), " (%u,%u) %.3f", hits[i].bx, hits[i].by, hits[i].e); s += b2; }
            std::fprintf(stderr, "[mip] search fmt=%u %ux%u level %u (%ux%u): layout says block (%u,%u); best%s\n",
                         t.format, t.width, t.height, l, m.width, m.height, m.x / 4, m.y / 4, s.c_str());
            MipLevel best = m; best.base = pm.base; best.pitch = pm.pitch; best.x = hits[0].bx * 4; best.y = hits[0].by * 4;
            up = MipDecode(t, best); um = best;
        }
    }
    uint32_t mipSearched_ = 0;
    static bool MipVerifyOn() { static const bool on = [] { const char* e = std::getenv("TTT2_MIP_VERIFY"); return e && *e == '1'; }(); return on; }
    std::set<uint64_t> mipVerified_;
    uint32_t mipVerifyCount_ = 0, mipVerifyBad_ = 0;
    void MipVerify(const Texture& t, uint32_t levels)
    {
        const uint64_t key = uint64_t(t.base) << 32 ^ uint64_t(t.mipBase) ^ uint64_t(t.format) << 24 ^ uint64_t(t.width) << 12 ^ t.height;
        if (!mipVerified_.insert(key).second || t.width * t.height > 2048 * 2048) return;
        ++mipVerifyCount_;
        std::vector<std::vector<V4>> lv(levels);
        std::vector<MipLevel> ml(levels);
        for (uint32_t l = 0; l < levels; ++l) { TextureMipLevel(t, l, ml[l]); lv[l] = MipDecode(t, ml[l]); }
        bool bad = false;
        std::string errs;
        char b[48];
        for (uint32_t l = 1; l < levels; ++l)
        {
            const double e = MipError(lv[l - 1], ml[l - 1].width, ml[l - 1].height, lv[l], ml[l].width, ml[l].height, false);
            const double s = MipError(lv[l - 1], ml[l - 1].width, ml[l - 1].height, lv[l], ml[l].width, ml[l].height, true);
            std::snprintf(b, sizeof(b), " %.3f/%.3f", e, s);
            errs += b;
            if (e > 0.06 && e > 0.5 * s) bad = true;
        }
        if (bad) ++mipVerifyBad_;
        if (bad || mipVerifyCount_ % 50 == 0)
            std::fprintf(stderr, "[mip-verify] %s fmt=%u %ux%u p%u %s%s endian %u base %08X mips %08X levels %u (error/shifted per level:%s); %u checked, %u off\n",
                         bad ? "OFF" : "ok", t.format, t.width, t.height, t.pitch, t.tiled ? "tiled" : "linear", t.packedMips ? " packed" : "",
                         t.endian, t.base, t.mipBase, levels, errs.c_str(), mipVerifyCount_, mipVerifyBad_);
        if (!bad || mipVerifyBad_ > 40) return;
        // The chain side by side, every level enlarged (nearest) to the base size, alpha ignored.
        const uint32_t W = ml[0].width, H = ml[0].height;
        std::vector<uint32_t> px(size_t(W) * levels * H);
        for (uint32_t l = 0; l < levels; ++l)
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                {
                    const V4& v = lv[l][size_t(y * ml[l].height / H) * ml[l].width + x * ml[l].width / W];
                    auto q = [](float f) { return uint32_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
                    px[size_t(y) * W * levels + l * W + x] = (q(v[0]) << 16) | (q(v[1]) << 8) | q(v[2]);
                }
        CreateDirectoryA("C:/ttt2/port/mipv", nullptr);
        char path[96];
        std::snprintf(path, sizeof(path), "C:/ttt2/port/mipv/%08X_%u.bmp", t.base, t.format);
        WriteBmp(path, px.data(), W * levels, H);
    }
    std::set<uint32_t> dumpedTile_;
    void CensusTarget(uint32_t colorKey)
    {
        if (!CensusOn() || censusSeen_[(1ull << 40) | (colorKey >> 12)]++) return;
        std::fprintf(stderr, "[census] colour target fmt=%u (first: edram base %03X)\n", colorKey >> 12, colorKey & 0xFFF);
    }
    // Render-scale sizing data: the first draw of every target / depth / pitch / MSAA / scissor /
    // viewport combination (how far down each surface really is drawn).
    void CensusSurfaceUse(const uint32_t* regs, const DrawState& st, float reachY)
    {
        const float ys = AsFloat(regs[0x2111]), yo = AsFloat(regs[0x2112]);
        const uint32_t vte = regs[0x2206];
        const uint64_t key = (3ull << 60) ^ Fnv(&st.colorBase, 4) ^ (uint64_t(st.depthBase) << 20) ^ (uint64_t(st.pitch) << 32) ^
                             (uint64_t(st.sampX * 4 + st.sampY) << 46) ^ (uint64_t(st.scY1) << 48) ^ (uint64_t(st.zEnable) << 59) ^
                             (uint64_t(regs[0x2082]) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(int(yo + std::fabs(ys))) << 36);
        if (censusSeen_[key]++) return;
        std::fprintf(stderr, "[census] surface use colour=%05X%s depth=%03X%s pitch=%u samp=%ux%u scissor=(%d,%d)-(%d,%d) "
                     "win_tl=%08X win_br=%08X win_off=%08X screen_br=%08X vte=%03X vp_y=%.1f..%.1f clip_off=%u reach_y=%.0f\n",
                     st.colorBase, st.writeMask ? "" : "(masked)", st.depthBase, st.zEnable ? "" : "(off)", st.pitch, st.sampX, st.sampY,
                     st.scX0, st.scY0, st.scX1, st.scY1, regs[0x2081], regs[0x2082], regs[0x2080], regs[0x200F], vte,
                     yo - std::fabs(ys), yo + std::fabs(ys), (regs[0x2204] >> 16) & 1, std::isfinite(reachY) ? reachY : -1.0f);
    }
    // TTT2_VCACHE_STATS=1: per distinct uploaded range (address, bytes), whether it changed since its last
    // upload (write watch, synced once per frame per range); printed with the 5 s stats.
    static bool VcacheStatsOn() { static const bool on = [] { const char* e = std::getenv("TTT2_VCACHE_STATS"); return e && *e == '1'; }(); return on; }
    struct VcacheSeen { uint32_t seq = 0, frame = ~0u; };
    std::unordered_map<uint64_t, VcacheSeen> vcacheSeen_;
    uint64_t vcacheSame_ = 0, vcacheChanged_ = 0, vcacheNew_ = 0, vcacheSyncs_ = 0;
    void VcacheStat(uint32_t addr, uint32_t bytes)
    {
        const uint64_t key = uint64_t(addr) << 32 | bytes;
        VcacheSeen& s = vcacheSeen_[key];
        const uint32_t frame = uint32_t(stats_.presents);
        if (s.frame != frame) { dirty_.Sync(addr, bytes); ++vcacheSyncs_; }
        const uint32_t newest = dirty_.Newest(addr, bytes);
        if (s.frame == ~0u) vcacheNew_ += bytes;
        else if (newest <= s.seq) vcacheSame_ += bytes;
        else vcacheChanged_ += bytes;
        s.seq = dirty_.Current();
        s.frame = frame;
    }
    void CensusVertex(const std::vector<detail::VsFetch>& fetches)
    {
        if (!CensusOn()) return;
        for (const auto& f : fetches)
            if (!censusSeen_[(2ull << 40) | f.format]++)
                std::fprintf(stderr, "[census] vertex fetch fmt=%u (first: slot %u stride %u)\n", f.format, f.slot, f.stride);
    }
    uint64_t textureBytes_ = 0;
    static constexpr uint64_t TextureBudgetBytes = 1536ull << 20;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    // CPU-read readbacks: a fence signalled after each staged copy, so the reading thread can wait
    // for exactly that copy without the context or the backend lock (ID3D11Fence is free-threaded).
    ComPtr<ID3D11DeviceContext4> context4_;
    ComPtr<ID3D11Fence> readbackFence_;
    uint64_t readbackFenceValue_ = 0, pendingFence_ = 0;
    // TTT2_GPU_PROBE=1 (diagnostics): the fence also marks every present and staged copy; a thread
    // logs each one the GPU passes more than 4 ms after it was queued (what holds the GPU queue?).
    struct ProbeMark { uint64_t value; const char* label; std::chrono::steady_clock::time_point queued; };
    std::mutex probeMutex_;
    std::condition_variable probeWake_;
    std::deque<ProbeMark> probeMarks_;
    void Probe(const char* label)
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_GPU_PROBE"); return e && *e == '1'; }();
        if (!on || !readbackFence_) return;
        static std::once_flag started;
        std::call_once(started, [this] {
            std::thread([this] {
                HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                int logged = 0;
                for (;;)
                {
                    ProbeMark m;
                    {
                        std::unique_lock l(probeMutex_);
                        probeWake_.wait(l, [this] { return !probeMarks_.empty(); });
                        m = probeMarks_.front();
                        probeMarks_.pop_front();
                    }
                    if (FAILED(readbackFence_->SetEventOnCompletion(m.value, done))) continue;
                    WaitForSingleObject(done, 2000);
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m.queued).count();
                    if (ms > 4.0 && logged++ < 2000) std::fprintf(stderr, "[gpu-probe] %s passed %.2f ms after it was queued\n", m.label, ms);
                }
            }).detach();
        });
        context4_->Signal(readbackFence_.Get(), ++readbackFenceValue_);
        {
            std::lock_guard l(probeMutex_);
            probeMarks_.push_back({ readbackFenceValue_, label, std::chrono::steady_clock::now() });
        }
        probeWake_.notify_one();
    }
    ComPtr<ID3D11Buffer> vertexRing_, indexRing_, psConstants_, psState_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11SamplerState> pointClamp_, linearClamp_;
    std::unordered_map<uint64_t, ComPtr<ID3D11SamplerState>> samplers_;
    ID3D11SamplerState* Sampler(const uint32_t info[4], uint32_t mip = 0)
    {
        static const uint32_t anisoFor[5] = { 0, 2, 4, 8, 16 };
        const uint32_t userAniso = anisoFor[std::clamp(display_settings::GetTextureFilter(), 0, 4)];
        const uint64_t key = info[0] | (info[1] << 3) | (info[2] << 6) | (info[3] << 7) | (uint64_t(mip) << 9) | (uint64_t(userAniso) << 33);
        ComPtr<ID3D11SamplerState>& s = samplers_[key];
        if (!s)
        {
            const D3D11_SAMPLER_DESC d = SamplerDesc(info, mip, userAniso);
            Check(device_->CreateSamplerState(&d, &s), "CreateSamplerState");
        }
        return s.Get();
    }
    ComPtr<ID3D11ShaderResourceView> defaultSrv_, defaultArraySrv_, whiteSrv_, whiteArraySrv_;
    uint32_t overrideMask_ = 0;       // texture slots forced to white (diagnostics)
    uint32_t overridePsDwords_ = 0;   // ...only in pixel shaders of this size (0 = cube shaders)
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11Texture2D> cpuFront_, lastFront_;
    uint32_t lastFrontW_ = 1280, lastFrontH_ = 720;     // guest size lastFront_ holds
    std::atomic<HWND> window_{ nullptr };
    bool lastSwap_ = false;
    ComPtr<ID3D11VertexShader> blitVs_;
    ComPtr<ID3D11RenderTargetView> backRtv_;
    ID3D11Texture2D* backRtvFor_ = nullptr;

    // Depth resolve: D3D11 cannot copy a sub-rectangle of a depth-stencil resource, so read the
    // typeless surface's R32F view in a full-screen pass (sample 0 of each pixel).
    // A resolve through a shader: redGreen keeps the colour surface's red and green (a
    // 16_16_FLOAT destination, R16G16_FLOAT texture); otherwise the texel converts as is (a
    // 16_16_FLOAT or other non-8888 surface resolved into an 8_8_8_8 destination).
    ComPtr<ID3D11PixelShader> redGreenBlitPs_, colorBlitPs_;
    void BlitColor(ColorSurface& s, Resolved& r, UINT w, UINT h, bool redGreen)
    {
        ComPtr<ID3D11PixelShader>& blitPs = redGreen ? redGreenBlitPs_ : colorBlitPs_;
        if (!blitPs)
        {
            BlitVs();
            auto code = Compile(redGreen
                                    ? "Texture2D<float4> src : register(t0);"
                                      " float4 main(float4 pos : SV_Position) : SV_Target { float4 c = src.Load(int3(int2(pos.xy), 0));"
                                      " return float4(c.r, c.g, 0, 0); }"
                                    : "Texture2D<float4> src : register(t0);"
                                      " float4 main(float4 pos : SV_Position) : SV_Target { return src.Load(int3(int2(pos.xy), 0)); }",
                                "ps_5_0", redGreen ? "ttt2 16_16_float resolve ps" : "ttt2 colour convert resolve ps");
            Check(device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &blitPs), "CreateColorBlitPs");
        }
        if (!s.srv) Check(device_->CreateShaderResourceView(s.tex.Get(), nullptr, &s.srv), "CreateColorSrv");
        ID3D11ShaderResourceView* nulls[32] = {};
        context_->PSSetShaderResources(0, 32, nulls);
        ID3D11RenderTargetView* rtv = r.rtv.Get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(nullptr, 0);
        w = Px(w, s.scale); h = Px(h, s.scale);     // texel for texel: both sides carry the scale
        D3D11_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(raster_.Get());
        D3D11_RECT sc{ 0, 0, LONG(w), LONG(h) };
        context_->RSSetScissorRects(1, &sc);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(blitVs_.Get(), nullptr, 0);
        context_->PSSetShader(blitPs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* v = s.srv.Get();
        context_->PSSetShaderResources(0, 1, &v);
        context_->Draw(3, 0);
        context_->PSSetShaderResources(0, 1, nulls);
    }
    std::map<uint32_t, ComPtr<ID3D11PixelShader>> depthBlitPs_;
    void BlitDepth(DepthSurface& s, Resolved& r)
    {
        const uint32_t sx = std::clamp<uint32_t>(s.sampX, 1, 4), sy = std::clamp<uint32_t>(s.sampY, 1, 4);
        ComPtr<ID3D11PixelShader>& ps = depthBlitPs_[(sx << 4) | sy];
        if (!ps)
        {
            BlitVs();
            // Beyond the surface's extent nothing was drawn: the cleared store's far depth.
            const std::string src = "Texture2D<float> src : register(t0);"
                " float main(float4 pos : SV_Position) : SV_Target { int2 p = int2(pos.xy) * int2(" +
                std::to_string(sx) + ", " + std::to_string(sy) + "); uint w, h; src.GetDimensions(w, h);"
                " return (uint(p.x) < w && uint(p.y) < h) ? src.Load(int3(p, 0)) : 1.0; }";
            auto code = Compile(src, "ps_5_0", "ttt2 depth resolve ps");
            Check(device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps), "CreateDepthBlitPs");
        }
        // The shader's integer sample step holds at any scale (pixel p of the scaled resolve =
        // sample p x samp of the scaled surface).
        const UINT lw = std::min<UINT>(r.pitch, SurfaceW / sx), lh = std::min<UINT>(r.height, SurfaceH / sy);
        ID3D11ShaderResourceView* nulls[32] = {};
        context_->PSSetShaderResources(0, 32, nulls);              // the target may still be bound as input
        ID3D11RenderTargetView* rtv = r.rtv.Get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(nullptr, 0);
        const UINT w = std::min(Px(lw, s.scale), AllocPx(r.pitch, r.scale)), h = std::min(Px(lh, s.scale), AllocPx(r.height, r.scale));
        D3D11_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(raster_.Get());
        D3D11_RECT sc{ 0, 0, LONG(w), LONG(h) };
        context_->RSSetScissorRects(1, &sc);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(blitVs_.Get(), nullptr, 0);
        context_->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* v = s.srv.Get();
        context_->PSSetShaderResources(0, 1, &v);
        context_->Draw(3, 0);
        context_->PSSetShaderResources(0, 1, nulls);
    }
    void BlitVs()
    {
        if (blitVs_) return;
        auto vs = Compile("float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2);"
                          " return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }", "vs_5_0", "ttt2 blit vs");
        Check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &blitVs_), "CreateBlitVs");
    }
    size_t ringOffset_ = 0, indexOffset_ = 0, vdataOffset_ = 0;
    // The dynamic rings wrap without DISCARD. MEASURED 2026-10-15 on Sakura Schoolyard (7 MB of
    // guest vertex data a frame): DISCARD at each wrap had the driver rename the 64 MB ring every
    // ~9 frames - a fresh allocation the kernel pages on the copy engine (the System process at
    // ~10% copy) - and presents across the whole desktop (other programs' too) blocked 10-20 ms on
    // a quarter of the frames. Instead each ring is eight chunks, an event query marking the last
    // command that read each one; writing into a chunk again first waits for its query - long
    // done in practice, the ring holding several frames. TTT2_RING_DISCARD=1: the old wraps.
    struct RingFence
    {
        static constexpr size_t Chunks = 8;
        size_t chunk = 0;                         // bytes per chunk
        ComPtr<ID3D11Query> done[Chunks];
        bool issued[Chunks] = {};                 // query ended for the chunk's previous cycle
        size_t cur = SIZE_MAX, ended = 0;         // furthest chunk written this cycle; chunks [0, ended) marked
    };
    RingFence vertexFence_, indexFence_, vdataFence_;
    static bool RingDiscard()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_RING_DISCARD"); return e && *e == '1'; }();
        return on;
    }
    // Before writing [start, start + bytes) of a ring of `ringBytes`; `wrapped`: the write restarts it at 0.
    void RingWrite(RingFence& f, size_t ringBytes, size_t start, size_t bytes, bool wrapped)
    {
        if (!f.chunk)
        {
            f.chunk = ringBytes / RingFence::Chunks;
            for (auto& q : f.done)
            {
                D3D11_QUERY_DESC d{ D3D11_QUERY_EVENT, 0 };
                Check(device_->CreateQuery(&d, &q), "CreateRingQuery");
            }
        }
        auto mark = [&](size_t c) { context_->End(f.done[c].Get()); f.issued[c] = true; };
        if (wrapped)
        {
            if (f.cur != SIZE_MAX)
                for (size_t c = f.ended; c <= f.cur; ++c) mark(c);
            f.cur = SIZE_MAX;
            f.ended = 0;
        }
        // Chunks before this write's start: every command reading them is already queued.
        const size_t first = start / f.chunk, last = std::min(RingFence::Chunks - 1, (start + std::max<size_t>(bytes, 1) - 1) / f.chunk);
        for (; f.ended < first; ++f.ended) mark(f.ended);
        for (size_t c = f.cur == SIZE_MAX ? 0 : f.cur + 1; c <= last; ++c)
        {
            if (!f.issued[c]) continue;
            BOOL done = FALSE;
            if (context_->GetData(f.done[c].Get(), &done, sizeof(done), 0) != S_OK)
            {
                const auto t0 = std::chrono::steady_clock::now();
                while (context_->GetData(f.done[c].Get(), &done, sizeof(done), 0) != S_OK) YieldProcessor();
                ++stats_.ringWaits;
                stats_.ringWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
            f.issued[c] = false;
        }
        if (f.cur == SIZE_MAX || last > f.cur) f.cur = last;
    }
    std::array<uint32_t, 1024> lastPsConstants_{};
    std::array<uint32_t, 2048> lastVsConstants_{};
    bool vsConstantsValid_ = false, vdataNoOverwrite_ = false, gpuVsEnabled_ = true;
    ComPtr<ID3D11Buffer> vsConstants_, vsState_, vdataRing_;
    ComPtr<ID3D11ShaderResourceView> vdataSrv_;
    std::unordered_map<uint64_t, VsEntry> vertexShaders_;
    bool constantsValid_ = false, stateValid_ = false;
    PsStateCB lastState_{};
    uint32_t lastSlots_ = 0, lastCubeSlots_ = 0;
    std::unordered_map<uint32_t, ColorSurface> colors_;
    std::unordered_map<uint32_t, DepthSurface> depths_;
    std::unordered_map<uint32_t, Resolved> resolved_;
    std::unordered_map<TexKey, CachedTexture, TexKeyHash> textures_;
    struct PsEntry { ComPtr<ID3D11PixelShader> ps; uint32_t slots = 0, cubeSlots = 0; bool failed = false; };
    std::unordered_map<uint64_t, PsEntry> pixelShaders_;
    std::mutex preparedMutex_;                          // guards prepared*_ (warm-up thread)
    std::unordered_map<uint64_t, VsEntry> preparedVs_;
    std::unordered_map<uint64_t, PsEntry> preparedPs_;
    mutable std::mutex vsTextMutex_;                    // guards vsByText_ (build threads, warm-up)
    mutable std::unordered_map<uint64_t, ComPtr<ID3D11VertexShader>> vsByText_;   // HLSL text key -> compiled
    double buildBudgetMs_ = 24;                         // per frame, for waiting on async builds
    std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> blendStates_;
    std::unordered_map<uint64_t, ComPtr<ID3D11DepthStencilState>> depthStates_;
    std::unordered_map<uint32_t, ComPtr<ID3D11RasterizerState>> rasterStates_;
    DirtyTracker dirty_;
    Stats stats_, reported_;
    std::chrono::steady_clock::time_point lastStats_;
    std::filesystem::path shaderCacheDir_;
    uint64_t compares_ = 0;
    uint32_t dumps_ = 0;
    double lastStagedAgeMs_ = 0;                        // the last staged readback's copy: queued this long before its read
    double lastMapWaitMs_ = 0;                          // the last ReadTexture's wait for the GPU (TTT2_FRAMETIME)

    // ----------------------------------------------------------------- helpers
    // D3DCompile behind a structured-exception guard: a crash inside the compiler (a compiler bug on
    // some input) fails that one shader - drawn by the CPU path or skipped - instead of the game.
    static HRESULT GuardedCompile(const char* text, size_t size, const char* target, const char* name, UINT flags,
                                  ID3DBlob** code, ID3DBlob** errors)
    {
        __try { return D3DCompile(text, size, name, nullptr, nullptr, "main", target, flags, 0, code, errors); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
    }
    static ComPtr<ID3DBlob> Compile(const std::string& text, const char* target, const char* name,
                                    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL1)
    {
        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = GuardedCompile(text.data(), text.size(), target, name, flags, &code, &errors);
        if (hr == E_UNEXPECTED && !code) throw std::runtime_error("the shader compiler crashed on this shader");
        if (FAILED(hr))
        {
            std::string why = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "D3DCompile failed";
            throw std::runtime_error(why);
        }
        return code;
    }

    // %LOCALAPPDATA%\TTT2Recomp\shadercache, or TTT2_SHADER_CACHE (tests start from an empty one).
    static std::filesystem::path ShaderCacheDirectory()
    {
        char buffer[MAX_PATH] = {};
        std::filesystem::path p;
        DWORD n = GetEnvironmentVariableA("TTT2_SHADER_CACHE", buffer, MAX_PATH);
        if (n && n < MAX_PATH) p = buffer;
        else
        {
            n = GetEnvironmentVariableA("LOCALAPPDATA", buffer, MAX_PATH);
            if (!n || n >= MAX_PATH) return {};
            p = std::filesystem::path(buffer) / "TTT2Recomp" / "shadercache";
        }
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return ec ? std::filesystem::path{} : p;
    }

    void Upload(ID3D11Buffer* buffer, const void* data, size_t size)
    {
        if (toggle_ & 32) return;
        D3D11_MAPPED_SUBRESOURCE m{};
        Check(context_->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "MapConstants");
        std::memcpy(m.pData, data, size);
        context_->Unmap(buffer, 0);
    }

    ID3D11PixelShader* PixelShader(const std::vector<uint32_t>& code)
    {
        const uint64_t key = PsKey(code);
        auto it = pixelShaders_.find(key);
        if (it != pixelShaders_.end())
        {
            lastSlots_ = it->second.slots; lastCubeSlots_ = it->second.cubeSlots;
            return it->second.failed ? nullptr : it->second.ps.Get();
        }
        PsEntry e;
        if (TakePrepared(preparedPs_, key, e)) ++stats_.warmHits;
        else
        {
            Built<PsEntry> b;
            if (!AwaitBuild(pendingPs_, key, b, [this, code] {
                    Built<PsEntry> r;
                    r.result = BuildPixelShader(code, r.entry, r.counts, r.why, true);
                    return r;
                }))
            {
                ++stats_.pendingSkips;          // still compiling: its draws wait a frame or two
                lastSlots_ = 0;
                return nullptr;
            }
            stats_.diskHits += b.counts.diskHits; stats_.compiles += b.counts.compiles;
            if (b.result != BuildResult::Ready)
            {
                std::fprintf(stderr, b.result == BuildResult::Untranslatable ? "[gpu-hw] pixel shader %016llX (%zu dwords) not translatable: %s - its draws are skipped\n"
                                                                             : "[gpu-hw] pixel shader %016llX (%zu dwords) failed to compile - its draws are skipped:\n%s\n",
                             (unsigned long long)key, code.size(), b.why.c_str());
                pixelShaders_[key].failed = true;
                lastSlots_ = 0;
                return nullptr;
            }
            e = std::move(b.entry);
            RecordMicrocode("ps", key, code);
        }
        PsEntry& slot = pixelShaders_[key];
        slot = std::move(e);
        lastSlots_ = slot.slots; lastCubeSlots_ = slot.cubeSlots;
        return slot.ps.Get();
    }

    // ----------------------------------------------------------------- shader builds
    // Building a shader (translate the microcode, take its bytecode from the disk cache or
    // compile it, create the driver object) costs milliseconds, D3DCompile far more, and a
    // scene's first frame needs dozens at once: the fight's opening "Ready" stalled ~460 ms on
    // 46 of them. So every shader built is also recorded as microcode
    // (shadercache/ucode/{vs,ps}_<key>.bin), and a thread started with the device builds all
    // recorded ones (prepared*_) while the game boots; the command processor adopts a
    // prepared entry on first use instead of building it. TTT2_SHADER_WARM=0 turns this off.
    enum class BuildResult { Ready, Untranslatable, CompileFailed };
    struct BuildCounts { uint64_t diskHits = 0, compiles = 0; };

    // ----------------------------------------------------------------- asynchronous builds
    // A shader that is neither cached nor prepared is built on its own thread. The frame that
    // needs it waits for it only within a per-frame budget (reset at every present); past that
    // the draw is skipped (pixel shader) or drawn by the CPU vertex stage (vertex shader) until
    // the build lands, instead of the whole game stalling on D3DCompile (one TTT2 pixel shader
    // takes 4.4 s even unoptimised; a cold cache stalled the fight start 3.5 s).
    // TTT2_SHADER_ASYNC=0: wait for every build.
    template <class Entry> struct Built { BuildResult result = BuildResult::CompileFailed; Entry entry; BuildCounts counts; std::string why; };
    std::unordered_map<uint64_t, std::future<Built<VsEntry>>> pendingVs_;
    std::unordered_map<uint64_t, std::future<Built<PsEntry>>> pendingPs_;
    // `wait` false: never wait (a vertex shader: the CPU vertex stage draws meanwhile - no frame
    // held for a build that is all but never needed now that the cache is built ahead of time).
    template <class Entry, class Build>
    bool AwaitBuild(std::unordered_map<uint64_t, std::future<Built<Entry>>>& pending, uint64_t key, Built<Entry>& out, Build build,
                    bool wait = true)
    {
        static const bool async = [] { const char* e = std::getenv("TTT2_SHADER_ASYNC"); return !e || std::strcmp(e, "0") != 0; }();
        auto it = pending.find(key);
        if (it == pending.end()) it = pending.emplace(key, std::async(std::launch::async, build)).first;
        const auto start = std::chrono::steady_clock::now();
        const bool ready = !async ? (it->second.wait(), true)
                                  : it->second.wait_for(std::chrono::duration<double, std::milli>(wait ? std::max(0.0, buildBudgetMs_) : 0.0)) ==
                                        std::future_status::ready;
        buildBudgetMs_ -= std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!ready) return false;
        out = it->second.get();
        pending.erase(it);
        return true;
    }


    static uint64_t VsKey(const std::vector<uint32_t>& code) { return Fnv(code.data(), code.size() * 4, 0x5EEDull + code.size()); }
    static uint64_t PsKey(const std::vector<uint32_t>& code) { return Fnv(code.data(), code.size() * 4, 0xC0FFEEull + code.size()); }

    // Safe off the command-processor thread: uses only the (free-threaded) device and the
    // cache directory, both fixed after Initialize.
    // Vertex shaders are compiled per HLSL text: the game's run-time copies of one shader differ only in
    // their vertex fetches' parameters, which reach the shader per draw (VsFetchDescriptors), so they
    // translate to the same text and share one compiled shader (vsByText_; the warm-up loads every
    // compiled one, vg_<text key>.dxbc, at boot).
    BuildResult BuildVertexShader(const std::vector<uint32_t>& code, VsEntry& e, BuildCounts& n, std::string& why,
                                  bool inGame = false) const
    {
        e.t = TranslateVertexShader(code);
        if (!e.t.unsupported.empty()) { why = e.t.unsupported; return BuildResult::Untranslatable; }
        const std::string hlsl = VertexShaderHlsl(e.t);
        const uint64_t textKey = Fnv(hlsl.data(), hlsl.size());
        {
            std::lock_guard lock(vsTextMutex_);
            if (auto it = vsByText_.find(textKey); it != vsByText_.end()) { e.vs = it->second; ++n.diskHits; return BuildResult::Ready; }
        }
        const BuildResult r = CreateFromHlsl(hlsl, true, inGame, n, why,
                                             [&](const void* p, size_t size) { return device_->CreateVertexShader(p, size, nullptr, &e.vs); });
        if (r == BuildResult::Ready)
        {
            std::lock_guard lock(vsTextMutex_);
            vsByText_.emplace(textKey, e.vs);
        }
        return r;
    }
    // A run-time copy of a shader whose compiled form is already loaded: ready at once (the translation
    // only, well under a millisecond), no build thread, no frame waiting.
    bool SharedVertexShader(const std::vector<uint32_t>& code, VsEntry& e)
    {
        VsTranslation t = TranslateVertexShader(code);
        if (!t.unsupported.empty()) return false;
        const std::string hlsl = VertexShaderHlsl(t);
        const uint64_t textKey = Fnv(hlsl.data(), hlsl.size());
        std::lock_guard lock(vsTextMutex_);
        auto it = vsByText_.find(textKey);
        if (it == vsByText_.end()) return false;
        e.vs = it->second;
        e.t = std::move(t);
        return true;
    }
    BuildResult BuildPixelShader(const std::vector<uint32_t>& code, PsEntry& e, BuildCounts& n, std::string& why,
                                 bool inGame = false) const
    {
        const PsTranslation t = TranslatePixelShader(code);
        if (!t.unsupported.empty()) { why = t.unsupported; return BuildResult::Untranslatable; }
        e.slots = t.textureSlots; e.cubeSlots = t.cubeSlots;
        return CreateFromHlsl(PixelShaderHlsl(t), false, inGame, n, why,
                              [&](const void* p, size_t size) { return device_->CreatePixelShader(p, size, nullptr, &e.ps); });
    }

    // A shader first met in game (not in the corpus yet) stalls the frame that needs it, and an
    // optimised D3DCompile of a large TTT2 shader takes a couple of hundred ms (measured: a 426
    // ms fight-start frame on two of them). In game it is compiled without optimisation (a
    // fraction of the time) and the optimised bytecode is compiled for the cache on a background
    // thread, so the next launch's warm-up loads that. TTT2_SHADER_FAST=0: optimise in game too.
    template <class Create>
    BuildResult CreateFromHlsl(const std::string& hlsl, bool vertex, bool inGame, BuildCounts& n, std::string& why,
                               Create create) const
    {
        const uint64_t textKey = Fnv(hlsl.data(), hlsl.size());
        std::vector<char> blob;
        std::filesystem::path cached;
        if (!shaderCacheDir_.empty())
        {
            char name[64];
            std::snprintf(name, sizeof(name), "%s_%016llX.dxbc", vertex ? "vg" : "ps", (unsigned long long)textKey);
            cached = shaderCacheDir_ / name;
            std::ifstream f(cached, std::ios::binary);
            if (f) blob.assign(std::istreambuf_iterator<char>(f), {});
        }
        if (!blob.empty() && SUCCEEDED(create(blob.data(), blob.size()))) { ++n.diskHits; return BuildResult::Ready; }
        static const bool fastInGame = [] { const char* e = std::getenv("TTT2_SHADER_FAST"); return !e || std::strcmp(e, "0") != 0; }();
        // Vertex shaders always compile optimised: they are small (milliseconds either way), and an
        // unoptimised compile crashed D3DCompile itself (MEASURED: a vertex fetch into r0 inside the
        // program's do / while (false), d3dcompiler_47 +77EBC reading address -1; optimised: fine).
        const bool fast = inGame && fastInGame && !vertex;
        const char* target = vertex ? "vs_5_0" : "ps_5_0";
        const char* label = vertex ? "ttt2 translated vs" : "ttt2 translated ps";
        try
        {
            const auto start = std::chrono::steady_clock::now();
            if (const char* trace = std::getenv("TTT2_SHADER_TRACE"))
            {
                char name[64];
                std::snprintf(name, sizeof(name), "/%s_%016llX_%s.hlsl", vertex ? "vg" : "ps", (unsigned long long)textKey, fast ? "fast" : "opt");
                if (FILE* f = std::fopen((std::string(trace) + name).c_str(), "wb")) { std::fwrite(hlsl.data(), 1, hlsl.size(), f); std::fclose(f); }
                std::fprintf(stderr, "[gpu-hw] compiling %s\n", name + 1);
            }
            auto code5 = Compile(hlsl, target, label, fast ? D3DCOMPILE_SKIP_OPTIMIZATION : D3DCOMPILE_OPTIMIZATION_LEVEL1);
            Check(create(code5->GetBufferPointer(), code5->GetBufferSize()), vertex ? "CreateVertexShader" : "CreatePixelShader");
            ++n.compiles;
            if (inGame)
                std::fprintf(stderr, "[gpu-hw] %s shader compiled in game in %.0f ms (%s)\n", vertex ? "vertex" : "pixel",
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                             fast ? "unoptimised; optimised copy cached in the background" : "optimised");
            if (cached.empty()) return BuildResult::Ready;
            if (!fast) WriteCacheFile(cached, code5->GetBufferPointer(), code5->GetBufferSize());
            else
                std::thread([hlsl, cached, target, label] {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    try
                    {
                        auto optimised = Compile(hlsl, target, label);
                        WriteCacheFile(cached, optimised->GetBufferPointer(), optimised->GetBufferSize());
                    }
                    catch (...) {}
                }).detach();
            return BuildResult::Ready;
        }
        catch (const Failure&) { throw; }
        catch (const std::exception& ex) { why = ex.what(); return BuildResult::CompileFailed; }
    }

    // Write through a per-thread temporary and rename, so a reader never sees half a file.
    static void WriteCacheFile(const std::filesystem::path& path, const void* data, size_t size)
    {
        std::filesystem::path temp = path;
        temp += ".tmp" + std::to_string(GetCurrentThreadId());
        {
            std::ofstream f(temp, std::ios::binary);
            f.write(static_cast<const char*>(data), (std::streamsize)size);
            if (!f) return;
        }
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        if (ec) std::filesystem::remove(temp, ec);
    }

    void RecordMicrocode(const char* kind, uint64_t key, const std::vector<uint32_t>& code)
    {
        if (shaderCacheDir_.empty()) return;
        char name[64];
        std::snprintf(name, sizeof(name), "%s_%016llX.bin", kind, (unsigned long long)key);
        const std::filesystem::path dir = shaderCacheDir_ / "ucode", path = dir / name;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) return;
        std::filesystem::create_directories(dir, ec);
        WriteCacheFile(path, code.data(), code.size() * 4);
    }

    template <class Entry>
    bool TakePrepared(std::unordered_map<uint64_t, Entry>& prepared, uint64_t key, Entry& out)
    {
        std::lock_guard lock(preparedMutex_);
        auto it = prepared.find(key);
        if (it == prepared.end()) return false;
        out = std::move(it->second);
        prepared.erase(it);
        return true;
    }

public:
    // --build-shaders (main.cpp): every shader in the game's own files built into the disk cache
    // ahead of time, so play never waits for D3DCompile (MEASURED in a real match on a cold cache: 267
    // builds, ~2,400 skipped draws, 28-36 fps in the worst 5 s). The player's copy is the only source:
    // nothing of the game ships with the port. Xbox 360 compiled-shader containers (magic 102A11xx;
    // bit 0 set = vertex) hold the microcode at container + h[1] + w0, w1 bytes (w0 / w1 = the two words
    // at container + h[6]; big-endian). MEASURED: 5,722 distinct pixel shaders in data002.bin,
    // data003.bin and the game image - 1,100 of the 1,125 seen in play (the others are slices of a
    // container's microcode; the game builds and records those itself, as before). Vertex shaders
    // (557 distinct) build too: the game's D3D patches only their fetch parameters, which the
    // translation leaves to the draw (VsFetchDescriptors), so a file's shader serves every copy.
    // Writes the same files the game does (shadercache/ps_<HLSL key>.dxbc and vg_<HLSL key>.dxbc,
    // compiled optimised, and ucode/ps_<key>.bin, which the warm-up builds at every boot); work
    // already there is skipped.
    // Prints "[shaders] <done>/<total>" lines (the installer's progress). Returns 0 when done.
    // The prebuild's stamp: this exe's link time (every build's translator may differ). A completed
    // prebuild writes it to shadercache/prebuilt.txt; an installed game compares at boot (main.cpp).
    static std::string ShaderBuildStamp()
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
        char s[32];
        std::snprintf(s, sizeof(s), "ps+vs-%08lX", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
        return s;
    }
    static bool PrebuildCurrent()
    {
        const std::filesystem::path dir = ShaderCacheDirectory();
        if (dir.empty()) return true;                   // nowhere to build into: nothing to do
        std::ifstream f(dir / "prebuilt.txt");
        std::string stamp;
        return f && std::getline(f, stamp) && stamp == ShaderBuildStamp();
    }

    static int PrebuildShaders(const std::vector<std::filesystem::path>& files, unsigned threads)
    {
        const std::filesystem::path dir = ShaderCacheDirectory();
        if (dir.empty()) { std::fprintf(stderr, "[shaders] no cache directory\n"); return 2; }
        std::error_code ec;
        std::filesystem::create_directories(dir / "ucode", ec);
        const auto start = std::chrono::steady_clock::now();
        auto be32 = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
        std::unordered_map<uint64_t, std::vector<uint32_t>> found, foundVs;
        size_t containers = 0;
        for (const auto& path : files)
        {
            const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (f == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "[shaders] cannot open %ls\n", path.c_str()); continue; }
            LARGE_INTEGER size{};
            GetFileSizeEx(f, &size);
            const HANDLE map = size.QuadPart > 64 ? CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
            const uint8_t* m = map ? static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0)) : nullptr;
            const size_t n = size_t(size.QuadPart);
            size_t here = 0;
            for (size_t s = 0; m && s + 32 <= n; s += 4)
            {
                if (m[s] != 0x10 || m[s + 1] != 0x2A || m[s + 2] != 0x11) continue;
                uint32_t h[8];
                for (int k = 0; k < 8; ++k) h[k] = be32(m + s + 4 * k);
                if (h[1] == 0 || h[1] > 0x100000 || h[6] == 0 || h[6] >= h[1] || s + h[6] + 8 > n) continue;
                const uint32_t w0 = be32(m + s + h[6]), w1 = be32(m + s + h[6] + 4);
                if (w1 == 0 || (w1 & 3) || (w0 & 3) || w0 > 0x10000 || w1 > 0x40000) continue;
                const size_t u0 = s + h[1] + w0;
                if (u0 + w1 > n || w0 + w1 > h[2]) continue;
                ++here;
                std::vector<uint32_t> code(w1 / 4);
                for (size_t k = 0; k < code.size(); ++k) code[k] = be32(m + u0 + 4 * k);
                // Bit 0 of the magic: a vertex shader (MEASURED: 102A1101 and 102A1121 hold vertex
                // shaders, 102A1100 pixel shaders).
                if (h[0] & 1) foundVs.emplace(VsKey(code), std::move(code));
                else found.emplace(PsKey(code), std::move(code));
            }
            if (m) UnmapViewOfFile(m);
            if (map) CloseHandle(map);
            CloseHandle(f);
            containers += here;
        }
        // Pixel shaders first (most of the work), then vertex shaders (key | 1 << 63 marks them here).
        std::vector<std::pair<uint64_t, std::vector<uint32_t>>> list(found.begin(), found.end());
        const size_t pixelCount = list.size();
        for (auto& [k, code] : foundVs) list.emplace_back(k, std::move(code));
        found.clear();
        foundVs.clear();
        std::fprintf(stdout, "[shaders] %zu containers, %zu distinct pixel and %zu vertex shaders\n", containers, pixelCount,
                     list.size() - pixelCount);
        std::fflush(stdout);
        std::atomic<size_t> next{ 0 }, done{ 0 }, compiled{ 0 }, untranslatable{ 0 }, failed{ 0 };
        auto worker = [&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;)
            {
                const size_t i = next++;
                if (i >= list.size()) return;
                const auto& [key, code] = list[i];
                try
                {
                    if (i >= pixelCount)
                    {
                        // Vertex: compiled per HLSL text (BuildVertexShader); the microcode is not recorded
                        // - the game's copies differ in their fetches and find the text's shader at boot.
                        const VsTranslation t = TranslateVertexShader(code);
                        if (!t.unsupported.empty()) { ++untranslatable; ++done; continue; }
                        const std::string hlsl = VertexShaderHlsl(t);
                        char name[64];
                        std::snprintf(name, sizeof(name), "vg_%016llX.dxbc", (unsigned long long)Fnv(hlsl.data(), hlsl.size()));
                        std::error_code e2;
                        if (!std::filesystem::exists(dir / name, e2))
                        {
                            auto blob = Compile(hlsl, "vs_5_0", "ttt2 translated vs");
                            WriteCacheFile(dir / name, blob->GetBufferPointer(), blob->GetBufferSize());
                            ++compiled;
                        }
                        ++done;
                        continue;
                    }
                    const PsTranslation t = TranslatePixelShader(code);
                    if (!t.unsupported.empty()) { ++untranslatable; ++done; continue; }
                    const std::string hlsl = PixelShaderHlsl(t);
                    char name[64];
                    std::snprintf(name, sizeof(name), "ps_%016llX.dxbc", (unsigned long long)Fnv(hlsl.data(), hlsl.size()));
                    const std::filesystem::path dxbc = dir / name;
                    std::error_code e2;
                    if (!std::filesystem::exists(dxbc, e2))
                    {
                        static const bool fastPs = [] { const char* e = std::getenv("TTT2_PREBUILD_FAST"); return e && *e == '1'; }();
                        auto blob = Compile(hlsl, "ps_5_0", "ttt2 translated ps", fastPs ? D3DCOMPILE_SKIP_OPTIMIZATION : D3DCOMPILE_OPTIMIZATION_LEVEL1);
                        WriteCacheFile(dxbc, blob->GetBufferPointer(), blob->GetBufferSize());
                        ++compiled;
                    }
                    std::snprintf(name, sizeof(name), "ps_%016llX.bin", (unsigned long long)key);
                    const std::filesystem::path ucode = dir / "ucode" / name;
                    if (!std::filesystem::exists(ucode, e2)) WriteCacheFile(ucode, code.data(), code.size() * 4);
                }
                catch (...) { ++failed; }
                ++done;
            }
        };
        if (!threads) threads = (std::max)(1u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u);
        std::vector<std::thread> pool;
        for (unsigned k = 0; k < threads; ++k) pool.emplace_back(worker);
        size_t shown = ~size_t(0);
        while (done.load() < list.size())
        {
            const size_t d = done.load();
            if (d != shown) { std::fprintf(stdout, "[shaders] %zu/%zu\n", d, list.size()); std::fflush(stdout); shown = d; }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        for (auto& t : pool) t.join();
        std::fprintf(stdout, "[shaders] %zu/%zu\n[shaders] done in %.1f s on %u threads: %zu compiled, %zu already cached, %zu not translatable, %zu failed\n",
                     list.size(), list.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), threads,
                     compiled.load(), list.size() - compiled.load() - untranslatable.load() - failed.load(), untranslatable.load(), failed.load());
        std::fflush(stdout);
        if (failed.load() || list.empty()) return 1;
        const std::string stamp = ShaderBuildStamp() + "\n" + std::to_string(pixelCount) + " pixel and " + std::to_string(list.size() - pixelCount) +
                                  " vertex shaders\n";
        WriteCacheFile(dir / "prebuilt.txt", stamp.data(), stamp.size());
        return 0;
    }
private:

    void StartShaderWarmup()
    {
        if (shaderCacheDir_.empty()) return;
        if (const char* e = std::getenv("TTT2_SHADER_WARM"); e && std::strcmp(e, "0") == 0) return;
        std::thread([this] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            const auto start = std::chrono::steady_clock::now();
            std::error_code ec;
            // Every compiled vertex shader (vg_<HLSL text key>.dxbc): run-time copies of one shader
            // share it (BuildVertexShader / SharedVertexShader).
            unsigned shared = 0;
            if (gpuVsEnabled_)
                for (const auto& file : std::filesystem::directory_iterator(shaderCacheDir_, ec))
                {
                    const std::string name = file.path().filename().string();
                    if (name.size() != 24 || name.rfind("vg_", 0) != 0 || file.path().extension() != ".dxbc") continue;
                    const uint64_t textKey = std::strtoull(name.substr(3, 16).c_str(), nullptr, 16);
                    std::ifstream f(file.path(), std::ios::binary);
                    const std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
                    ComPtr<ID3D11VertexShader> vs;
                    if (bytes.empty() || FAILED(device_->CreateVertexShader(bytes.data(), bytes.size(), nullptr, &vs))) continue;
                    std::lock_guard lock(vsTextMutex_);
                    vsByText_.emplace(textKey, vs);
                    ++shared;
                }
            // Every recorded microcode (ucode/{vs,ps}_<key>.bin, the prebuild's and the game's own),
            // built on a few threads (the bytecode comes from the disk cache).
            std::vector<std::filesystem::path> files;
            for (const auto& file : std::filesystem::directory_iterator(shaderCacheDir_ / "ucode", ec))
            {
                const std::string name = file.path().filename().string();
                const bool isVs = name.rfind("vs_", 0) == 0, isPs = name.rfind("ps_", 0) == 0;
                if ((!isVs && !isPs) || file.path().extension() != ".bin" || (isVs && !gpuVsEnabled_)) continue;
                files.push_back(file.path());
            }
            std::atomic<size_t> next{ 0 };
            std::atomic<unsigned> vs{ 0 }, ps{ 0 }, failed{ 0 };
            std::mutex countMutex;
            BuildCounts total;
            auto worker = [&] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                BuildCounts n;
                for (size_t i; (i = next++) < files.size();)
                {
                    const bool isVs = files[i].filename().string().rfind("vs_", 0) == 0;
                    std::ifstream f(files[i], std::ios::binary);
                    const std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
                    if (bytes.empty() || bytes.size() % 4) continue;
                    std::vector<uint32_t> code(bytes.size() / 4);
                    std::memcpy(code.data(), bytes.data(), bytes.size());
                    std::string why;
                    try
                    {
                        if (isVs)
                        {
                            VsEntry e;
                            if (BuildVertexShader(code, e, n, why) != BuildResult::Ready) { ++failed; continue; }
                            std::lock_guard lock(preparedMutex_);
                            preparedVs_.emplace(VsKey(code), std::move(e));
                            ++vs;
                        }
                        else
                        {
                            PsEntry e;
                            if (BuildPixelShader(code, e, n, why) != BuildResult::Ready) { ++failed; continue; }
                            std::lock_guard lock(preparedMutex_);
                            preparedPs_.emplace(PsKey(code), std::move(e));
                            ++ps;
                        }
                    }
                    catch (...) { ++failed; }
                }
                std::lock_guard l(countMutex);
                total.diskHits += n.diskHits;
                total.compiles += n.compiles;
            };
            const unsigned cores = std::thread::hardware_concurrency();
            const unsigned threads = cores >= 8 ? 4 : cores >= 4 ? 2 : 1;
            std::vector<std::thread> pool;
            for (unsigned k = 1; k < threads; ++k) pool.emplace_back(worker);
            worker();
            for (auto& t : pool) t.join();
            std::fprintf(stderr, "[gpu-hw] shader warm-up: %u compiled vertex shaders loaded, %u vertex + %u pixel shaders ready in %.0f ms on %u threads (%llu from the bytecode cache, %llu compiled, %u failed)\n",
                         shared, vs.load(), ps.load(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                         threads, (unsigned long long)total.diskHits, (unsigned long long)total.compiles, failed.load());
        }).detach();
    }

    // EDRAM colour formats (RB_COLOR_INFO.color_format) as D3D11 render targets. TTT2 renders
    // 8_8_8_8 (0) and, for its motion-blur velocity pass, 16_16_FLOAT (6): signed half floats
    // that an 8-bit unorm target clamped to [0, 1], which smeared every moving character.
    static DXGI_FORMAT ColorDxgiFormat(uint32_t format)
    {
        switch (format)
        {
        case 2: case 10:  return DXGI_FORMAT_R10G10B10A2_UNORM;      // 2_10_10_10
        case 4: case 6:   return DXGI_FORMAT_R16G16_FLOAT;           // 16_16 (+-32 fixed), 16_16_FLOAT
        case 3: case 5: case 7: case 12:
                          return DXGI_FORMAT_R16G16B16A16_FLOAT;     // 2_10_10_10_FLOAT, 16_16_16_16(_FLOAT)
        case 14:          return DXGI_FORMAT_R32_FLOAT;
        case 15:          return DXGI_FORMAT_R32G32_FLOAT;
        default:          return DXGI_FORMAT_B8G8R8A8_UNORM;         // 8_8_8_8, 8_8_8_8_GAMMA
        }
    }

    // `key` is the EDRAM base | colour format << 12: each format gets its own surface (a base
    // reused with another format is not reinterpreted bit for bit, as EDRAM would). The texture
    // is made by Grow, sized to the area actually used.
    ColorSurface& Color(uint32_t key)
    {
        auto [it, added] = colors_.try_emplace(key);
        if (added) it->second.format = ColorDxgiFormat((key >> 12) & 0xF);
        return it->second;
    }

    DepthSurface& Depth(uint32_t base) { return depths_[base]; }

    // ---- render scale. Surfaces and resolved textures hold scale_ x their logical (guest)
    // size; everything the guest sees stays logical. Px: one rounding for every size and edge.
    float scale_ = 1.0f;
    uint32_t scaleGeneration_ = ~0u;
    UINT Px(double logical) const { return Px(logical, scale_); }
    UINT AllocPx(double logical) const { return AllocPx(logical, scale_); }
    static UINT Px(double logical, float scale) { return UINT(std::floor(logical * scale + 0.5)); }
    static UINT AllocPx(double logical, float scale) { return std::clamp<UINT>(Px(logical, scale), 1, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION); }

    // ---- native-size surfaces. The fight intro bakes each fighter's textures: draws into a
    // 1024x1024 surface, a resolve, and the CPU reads it back and recompresses it. At render scale
    // 2 every bake drew 2048x2048 and the GPU took ~6 ms per bake (CPU waiting; MEASURED
    // 2026-10-15: copy latency median 5.6 ms at scale 2, 3.1 ms at scale 1, ~0.9 s per intro). Extra
    // resolution there buys nothing - the CPU reads the logical size - so such a bake surface
    // (UpdateNative) renders at scale 1, with its depth surface.
    // TTT2_NATIVE_READBACK=0 keeps them scaled.
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, bool>> resolveDests_;   // colour key -> dest -> CPU-read
    std::unordered_map<uint32_t, uint32_t> depthWithColor_;                          // colour key -> depth base last drawn with it
    std::unordered_map<uint32_t, uint32_t> nativeColors_, nativeDepths_;   // EDRAM base -> the bake's sample pitch
    static bool NativeReadbackOn()
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_NATIVE_READBACK"); return !(e && *e == '0'); }();
        return on;
    }
    // Re-evaluate colour surface `key` after its resolves changed (a new destination, or one the
    // CPU read).
    void UpdateNative(uint32_t key)
    {
        if (!NativeReadbackOn() || key == ~0u) return;
        auto it = resolveDests_.find(key);
        // A texture bake: the CPU reads a square power-of-two resolve of it (1024x1024, 512x512), and
        // the surface is no bigger than that - never a scene-sized one (1280x720 and up), whose
        // resolves the GPU samples. Its other resolves are the bake's own intermediate layers
        // (MEASURED: 004C0 -> A751B000 read by the CPU, A7B6B000 sampled back into the bake).
        bool cpuOnly = false;
        uint32_t bakePitch = 0;
        const auto surface = colors_.find(key);
        if (it != resolveDests_.end() && surface != colors_.end() && surface->second.extW <= 1024 && surface->second.extH <= 1024)
            for (const auto& [dest, cpu] : it->second)
            {
                const auto r = resolved_.find(dest);
                if (cpu && r != resolved_.end() && r->second.pitch == r->second.height && r->second.pitch >= 256 &&
                    (r->second.pitch & (r->second.pitch - 1)) == 0)
                {
                    cpuOnly = true;
                    bakePitch = r->second.sourcePitch;
                }
            }
        const bool was = nativeColors_.count(key) != 0;
        if (cpuOnly == was) return;
        const auto d = depthWithColor_.find(key);
        if (cpuOnly)
        {
            const uint32_t pitch = bakePitch ? bakePitch : surface->second.samplePitch;
            nativeColors_[key] = pitch;
            if (d != depthWithColor_.end()) nativeDepths_[d->second] = pitch;
        }
        else
        {
            nativeColors_.erase(key);
            if (d != depthWithColor_.end()) nativeDepths_.erase(d->second);
        }
        static int logged = 0;
        if (logged++ < 32)
            std::fprintf(stderr, "[gpu-hw] colour surface %05X (depth %03X) now renders at %s (%s)\n", key,
                         d != depthWithColor_.end() ? d->second : 0xFFFu, cpuOnly ? "native size" : "the render scale",
                         cpuOnly ? "a texture bake the CPU reads back" : "no longer a texture bake");
    }
    static float& ScaleOverride() { static float s = 0; return s; }   // TTT2_IMAGE_TOGGLE tests
    static float RequestedScale()
    {
        if (ScaleOverride() > 0) return std::clamp(ScaleOverride(), 0.5f, 4.0f);
        if (const char* e = std::getenv("TTT2_RENDER_SCALE"); e && *e)
            return std::clamp(float(std::atof(e)), 0.5f, 4.0f);
        const display_settings::Resolution r = display_settings::GetResolution();
        return std::clamp(std::min(float(r.width) / 1280.0f, float(r.height) / 720.0f), 0.5f, 4.0f);
    }
    // Follow the RESOLUTION setting (checked once per change of the settings generation).
    void UpdateScale()
    {
        const uint32_t g = display_settings::Generation();
        if (g == scaleGeneration_) return;
        scaleGeneration_ = g;
        const float s = RequestedScale();
        if (s != scale_) Rescale(s);
    }

    // Surface extents grow in steps of 128 x 64 samples (fewer re-creations), never past the
    // EDRAM store's SurfaceW x SurfaceH.
    static UINT Extent(UINT need, UINT have, UINT step, UINT limit)
    {
        need = std::min(std::max(need, 1u), limit);
        return need <= have ? have : std::min((need + step - 1) / step * step, limit);
    }
    void MakeColorTexture(ColorSurface& s, float scale)
    {
        s.scale = scale;
        D3D11_TEXTURE2D_DESC t{};
        t.Width = AllocPx(s.extW, scale); t.Height = AllocPx(s.extH, scale); t.MipLevels = t.ArraySize = 1;
        t.Format = s.format; t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        s.tex.Reset(); s.rtv.Reset(); s.srv.Reset();
        Check(device_->CreateTexture2D(&t, nullptr, &s.tex), "CreateColorSurface");
        Check(device_->CreateRenderTargetView(s.tex.Get(), nullptr, &s.rtv), "CreateColorRtv");
        Check(device_->CreateShaderResourceView(s.tex.Get(), nullptr, &s.srv), "CreateColorSrv");
        const float zero[4] = { 0, 0, 0, 0 };          // the CPU store starts zeroed
        context_->ClearRenderTargetView(s.rtv.Get(), zero);
    }
    void MakeDepthTexture(DepthSurface& s, float scale)
    {
        s.scale = scale;
        D3D11_TEXTURE2D_DESC t{};
        t.Width = AllocPx(s.extW, scale); t.Height = AllocPx(s.extH, scale); t.MipLevels = t.ArraySize = 1;
        // Typeless so depth resolves can read it as R32_FLOAT (shader copy: D3D11 cannot
        // CopySubresourceRegion a sub-rectangle of a depth-stencil resource). 32-bit float depth
        // and the 8-bit stencil the console's D24FS8 / D24S8 has (TTT2's fight-end depth of field
        // marks the characters in stencil; without it the stage blurred into a flat colour).
        t.Format = DXGI_FORMAT_R32G8X24_TYPELESS; t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        s.tex.Reset(); s.dsv.Reset(); s.srv.Reset();
        Check(device_->CreateTexture2D(&t, nullptr, &s.tex), "CreateDepthSurface");
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        Check(device_->CreateDepthStencilView(s.tex.Get(), &dv, &s.dsv), "CreateDsv");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        Check(device_->CreateShaderResourceView(s.tex.Get(), &sv, &s.srv), "CreateDepthSrv");
        context_->ClearDepthStencilView(s.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);   // CPU store starts at 1.0
    }
    void LogGrowth(const char* kind, uint32_t key, UINT w, UINT h, float scale)
    {
        static int logged = 0;
        if (logged++ < 64)
            std::fprintf(stderr, "[gpu-hw] %s surface %05X covers %ux%u samples (%ux%u at scale %.2f)\n",
                         kind, key, w, h, AllocPx(w, scale), AllocPx(h, scale), scale);
    }
    // Make the surface cover logical samples [0, w) x [0, h): created, or re-created larger with
    // its contents copied (the new area starts cleared, as the CPU store does).
    // ... at `scale` (a change of scale resamples the contents).
    void Grow(ColorSurface& s, uint32_t key, UINT w, UINT h, float scale)
    {
        w = Extent(w, s.extW, 128, SurfaceW); h = Extent(h, s.extH, 64, SurfaceH);
        if (s.tex && w == s.extW && h == s.extH && s.scale == scale) return;
        ComPtr<ID3D11Texture2D> old = s.tex;
        ComPtr<ID3D11ShaderResourceView> oldSrv = s.srv;
        const float oldScale = s.scale;
        const UINT oldW = AllocPx(s.extW, oldScale), oldH = AllocPx(s.extH, oldScale);
        const UINT oldExtW = s.extW, oldExtH = s.extH;
        s.extW = w; s.extH = h;
        MakeColorTexture(s, scale);
        if (old && oldScale == scale)
        {
            D3D11_BOX box{ 0, 0, 0, oldW, oldH, 1 };
            context_->CopySubresourceRegion(s.tex.Get(), 0, 0, 0, 0, old.Get(), 0, &box);
        }
        else if (old)
            Resample(oldSrv.Get(), s.rtv.Get(), nullptr, AllocPx(oldExtW, scale), AllocPx(oldExtH, scale), 1.0f / float(AllocPx(oldExtW, scale)),
                     1.0f / float(AllocPx(oldExtH, scale)), Filterable(s.format));
        LogGrowth("colour", key, w, h, scale);
    }
    void Grow(DepthSurface& s, uint32_t key, UINT w, UINT h, float scale)
    {
        w = Extent(w, s.extW, 128, SurfaceW); h = Extent(h, s.extH, 64, SurfaceH);
        if (s.tex && w == s.extW && h == s.extH && s.scale == scale) return;
        ComPtr<ID3D11ShaderResourceView> old = s.srv;
        const UINT oldW = AllocPx(s.extW, s.scale), oldH = AllocPx(s.extH, s.scale);
        const UINT oldExtW = s.extW, oldExtH = s.extH;
        const float oldScale = s.scale;
        s.extW = w; s.extH = h;
        MakeDepthTexture(s, scale);
        if (old && oldScale == scale) Resample(old.Get(), nullptr, s.dsv.Get(), oldW, oldH, 1.0f / float(oldW), 1.0f / float(oldH), false);
        else if (old)
            Resample(old.Get(), nullptr, s.dsv.Get(), AllocPx(oldExtW, scale), AllocPx(oldExtH, scale), 1.0f / float(AllocPx(oldExtW, scale)),
                     1.0f / float(AllocPx(oldExtH, scale)), false);
        LogGrowth("depth", key, w, h, scale);
    }

    // Draw into `dst` (an RTV, or a DSV through SV_Depth) over [0, w) x [0, h), reading `src`
    // at uv = pixel centre * k: copies, rescales, and logical-size reads for the CPU.
    ComPtr<ID3D11PixelShader> resamplePs_, resampleDepthPs_;
    ComPtr<ID3D11Buffer> resampleCb_;
    ComPtr<ID3D11DepthStencilState> depthAlways_;
    void Resample(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv,
                  UINT w, UINT h, float kx, float ky, bool linear)
    {
        if (!resamplePs_)
        {
            BlitVs();
            const char* common = "cbuffer K : register(b0) { float4 k; }; SamplerState smp : register(s0);\n";
            auto ps = Compile(std::string(common) + "Texture2D<float4> src : register(t0);"
                              " float4 main(float4 pos : SV_Position) : SV_Target { return src.SampleLevel(smp, pos.xy * k.xy, 0); }",
                              "ps_5_0", "ttt2 resample ps");
            Check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &resamplePs_), "CreateResamplePs");
            auto dps = Compile(std::string(common) + "Texture2D<float> src : register(t0);"
                               " float main(float4 pos : SV_Position) : SV_Depth { return src.SampleLevel(smp, pos.xy * k.xy, 0); }",
                               "ps_5_0", "ttt2 resample depth ps");
            Check(device_->CreatePixelShader(dps->GetBufferPointer(), dps->GetBufferSize(), nullptr, &resampleDepthPs_), "CreateResampleDepthPs");
            D3D11_BUFFER_DESC b{};
            b.ByteWidth = 16; b.Usage = D3D11_USAGE_DYNAMIC; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            Check(device_->CreateBuffer(&b, nullptr, &resampleCb_), "CreateResampleCb");
            D3D11_DEPTH_STENCIL_DESC d{};
            d.DepthEnable = TRUE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; d.DepthFunc = D3D11_COMPARISON_ALWAYS;
            Check(device_->CreateDepthStencilState(&d, &depthAlways_), "CreateDepthAlways");
        }
        const float k[4] = { kx, ky, 0, 0 };
        Upload(resampleCb_.Get(), k, sizeof(k));
        ID3D11ShaderResourceView* nulls[32] = {};
        context_->PSSetShaderResources(0, 32, nulls);              // the target may still be bound as input
        context_->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, dsv);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(dsv ? depthAlways_.Get() : nullptr, 0);
        D3D11_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(raster_.Get());
        D3D11_RECT sc{ 0, 0, LONG(w), LONG(h) };
        context_->RSSetScissorRects(1, &sc);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(blitVs_.Get(), nullptr, 0);
        context_->PSSetShader(rtv ? resamplePs_.Get() : resampleDepthPs_.Get(), nullptr, 0);
        ID3D11Buffer* cb = resampleCb_.Get();
        context_->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* smp = linear ? linearClamp_.Get() : pointClamp_.Get();
        context_->PSSetSamplers(0, 1, &smp);
        context_->PSSetShaderResources(0, 1, &src);
        context_->Draw(3, 0);
        context_->PSSetShaderResources(0, 1, nulls);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    // Formats that may not filter on every feature-level-11 device: resample with point reads.
    static bool Filterable(DXGI_FORMAT f) { return f != DXGI_FORMAT_R32_FLOAT && f != DXGI_FORMAT_R32G32_FLOAT; }

    // A new RESOLUTION: every live surface and resolved texture is resampled to the new scale,
    // so what the game rendered once (and samples later) survives the change.
    void Rescale(float s)
    {
        const float old = scale_;
        scale_ = s;
        for (auto& [key, c] : colors_)
        {
            const float k = nativeColors_.count(key) ? 1.0f : s;
            if (!c.tex || c.scale == k) continue;
            ComPtr<ID3D11ShaderResourceView> src = c.srv;
            MakeColorTexture(c, k);
            Resample(src.Get(), c.rtv.Get(), nullptr, AllocPx(c.extW, k), AllocPx(c.extH, k),
                     1.0f / float(AllocPx(c.extW, k)), 1.0f / float(AllocPx(c.extH, k)), Filterable(c.format));
        }
        for (auto& [key, d] : depths_)
        {
            const float k = nativeDepths_.count(key) ? 1.0f : s;
            if (!d.tex || d.scale == k) continue;
            ComPtr<ID3D11ShaderResourceView> src = d.srv;
            MakeDepthTexture(d, k);
            Resample(src.Get(), nullptr, d.dsv.Get(), AllocPx(d.extW, k), AllocPx(d.extH, k),
                     1.0f / float(AllocPx(d.extW, k)), 1.0f / float(AllocPx(d.extH, k)), false);
        }
        for (auto& [dest, r] : resolved_)
        {
            if (!r.tex || r.scale != old) continue;         // native-size resolves stay at 1
            ComPtr<ID3D11ShaderResourceView> src = r.srv;
            MakeResolvedTexture(r, s);
            Resample(src.Get(), r.rtv.Get(), nullptr, AllocPx(r.pitch, s), AllocPx(r.height, s),
                     1.0f / float(AllocPx(r.pitch, s)), 1.0f / float(AllocPx(r.height, s)), !r.depth);
        }
        lastFront_.Reset();
        resolvePool_.clear();                               // sized for the old scale
        std::fprintf(stderr, "[gpu-hw] render scale %.2f -> %.2f (%zu colour, %zu depth surfaces, %zu resolved textures resampled; %.0f MB)\n",
                     old, s, colors_.size(), depths_.size(), resolved_.size(), SurfaceMegabytes());
    }
    // Resolve textures a destination gave up when it changed shape, by allocation size and format,
    // for the next destination of that shape. MEASURED 2026-10-15 on Sakura Schoolyard: ~13
    // destinations a frame alternated between shapes (one scratch address resolved at several
    // sizes) with a CreateTexture2D each time - 630 kernel allocations a second, each zero-filled
    // on the copy engine (the System process at ~10% copy) - and presents across the whole desktop,
    // other programs' too, blocked 10-20 ms on a quarter of the frames.
    struct PooledResolve { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11RenderTargetView> rtv; };
    std::unordered_map<uint64_t, std::vector<PooledResolve>> resolvePool_;
    static uint64_t ResolvePoolKey(UINT w, UINT h, DXGI_FORMAT f) { return (uint64_t(w) << 40) | (uint64_t(h) << 16) | uint64_t(f); }
    void RetireResolvedTexture(Resolved& r)
    {
        static const bool on = [] { const char* e = std::getenv("TTT2_RESOLVE_POOL"); return !(e && *e == '0'); }();
        if (!on || !r.tex || !r.srv || !r.rtv || r.tex == lastFront_) return;     // the screenshot source keeps its content
        D3D11_TEXTURE2D_DESC d{};
        r.tex->GetDesc(&d);
        auto& v = resolvePool_[ResolvePoolKey(d.Width, d.Height, d.Format)];
        if (v.size() < 4) v.push_back({ r.tex, r.srv, r.rtv });
    }
    void MakeResolvedTexture(Resolved& r, float scale)
    {
        D3D11_TEXTURE2D_DESC t{};
        t.Width = AllocPx(r.pitch, scale); t.Height = AllocPx(r.height, scale); t.MipLevels = t.ArraySize = 1;
        t.Format = r.depth ? DXGI_FORMAT_R32_FLOAT : r.rg16f ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        r.tex.Reset(); r.srv.Reset(); r.rtv.Reset();
        auto pooled = resolvePool_.find(ResolvePoolKey(t.Width, t.Height, t.Format));
        if (pooled != resolvePool_.end() && !pooled->second.empty())
        {
            PooledResolve& p = pooled->second.back();
            r.tex = std::move(p.tex); r.srv = std::move(p.srv); r.rtv = std::move(p.rtv);
            pooled->second.pop_back();
            ++stats_.resolvePoolHits;
        }
        else
        {
            Check(device_->CreateTexture2D(&t, nullptr, &r.tex), "CreateResolveTexture");
            Check(device_->CreateShaderResourceView(r.tex.Get(), nullptr, &r.srv), "CreateResolveSrv");
            Check(device_->CreateRenderTargetView(r.tex.Get(), nullptr, &r.rtv), "CreateResolveRtv");
            ++stats_.resolveCreates;
        }
        const float zero[4] = { 0, 0, 0, 0 };
        context_->ClearRenderTargetView(r.rtv.Get(), zero);
        r.scale = scale;
    }
    double SurfaceMegabytes() const
    {
        auto bpp = [](DXGI_FORMAT f) {
            return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32_FLOAT ? 8.0 : 4.0;
        };
        double bytes = 0;
        for (const auto& [k, c] : colors_) if (c.tex) bytes += double(AllocPx(c.extW, c.scale)) * AllocPx(c.extH, c.scale) * bpp(c.format);
        for (const auto& [k, d] : depths_) if (d.tex) bytes += double(AllocPx(d.extW, d.scale)) * AllocPx(d.extH, d.scale) * 8.0;
        for (const auto& [k, r] : resolved_) if (r.tex) bytes += double(AllocPx(r.pitch, r.scale)) * AllocPx(r.height, r.scale) * 4.0;
        return bytes / 1048576.0;
    }

    ID3D11BlendState* BlendState(const DrawState& st, float factor[4])
    {
        const uint32_t b = st.blend;
        const uint32_t srcF = b & 0x1F, op = (b >> 5) & 7, dstF = (b >> 8) & 0x1F;
        const uint32_t srcFA = (b >> 16) & 0x1F, opA = (b >> 21) & 7, dstFA = (b >> 24) & 0x1F;
        const bool passthrough = srcF == 1 && dstF == 0 && srcFA == 1 && dstFA == 0 && op == 0 && opA == 0;
        // Constant ALPHA as a colour-channel factor has no D3D11 equivalent unless the colour
        // channels do not also use the constant colour: then feed (a, a, a, a) as the factor.
        auto usesConstColor = [](uint32_t f) { return f == 12 || f == 13; };
        auto usesConstAlpha = [](uint32_t f) { return f == 14 || f == 15; };
        const bool alphaAsFactor = (usesConstAlpha(srcF) || usesConstAlpha(dstF)) &&
                                   !usesConstColor(srcF) && !usesConstColor(dstF);
        for (int i = 0; i < 4; ++i) factor[i] = alphaAsFactor ? st.blendConst[3] : st.blendConst[i];
        const uint64_t key = (uint64_t(passthrough ? 0 : b) << 8) | (st.writeMask & 15) | (alphaAsFactor ? 16 : 0);
        auto it = blendStates_.find(key);
        if (it != blendStates_.end()) return it->second.Get();
        auto map = [](uint32_t f, bool alpha) -> D3D11_BLEND {
            switch (f)
            {
            case 0: return D3D11_BLEND_ZERO;
            case 1: return D3D11_BLEND_ONE;
            case 4: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
            case 5: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
            case 6: return D3D11_BLEND_SRC_ALPHA;
            case 7: return D3D11_BLEND_INV_SRC_ALPHA;
            case 8: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
            case 9: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
            case 10: return D3D11_BLEND_DEST_ALPHA;
            case 11: return D3D11_BLEND_INV_DEST_ALPHA;
            case 12: case 14: return D3D11_BLEND_BLEND_FACTOR;
            case 13: case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
            case 16: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
            default: return D3D11_BLEND_ONE;               // CPU BlendFactor default
            }
        };
        auto mapOp = [](uint32_t o) -> D3D11_BLEND_OP {
            switch (o)
            {
            case 1: return D3D11_BLEND_OP_SUBTRACT;
            case 2: return D3D11_BLEND_OP_MIN;
            case 3: return D3D11_BLEND_OP_MAX;
            case 4: return D3D11_BLEND_OP_REV_SUBTRACT;
            default: return D3D11_BLEND_OP_ADD;
            }
        };
        D3D11_BLEND_DESC d{};
        auto& rt = d.RenderTarget[0];
        rt.BlendEnable = passthrough ? FALSE : TRUE;
        rt.SrcBlend = map(srcF, false); rt.DestBlend = map(dstF, false); rt.BlendOp = mapOp(op);
        rt.SrcBlendAlpha = map(srcFA, true); rt.DestBlendAlpha = map(dstFA, true); rt.BlendOpAlpha = mapOp(opA);
        rt.RenderTargetWriteMask = UINT8(st.writeMask & 15);
        ComPtr<ID3D11BlendState> state;
        Check(device_->CreateBlendState(&d, &state), "CreateBlendState");
        return (blendStates_[key] = state).Get();
    }

    // Viewport y is flipped twice (window y-down -> NDC y-up -> render target y-down), so the
    // winding D3D sees on the render target is the game's on-screen winding.
    ID3D11RasterizerState* RasterState(const DrawState& st)
    {
        const uint32_t key = (st.cull & 3) | (st.frontIsCW ? 4u : 0u) | (st.depthClip ? 8u : 0u);
        ComPtr<ID3D11RasterizerState>& state = rasterStates_[key];
        if (!state)
        {
            D3D11_RASTERIZER_DESC r{};
            r.FillMode = D3D11_FILL_SOLID;
            r.CullMode = (st.cull & 1) ? D3D11_CULL_FRONT : (st.cull & 2) ? D3D11_CULL_BACK : D3D11_CULL_NONE;
            r.FrontCounterClockwise = st.frontIsCW ? FALSE : TRUE;
            // Xenos clips 0 <= z <= w unless PA_CL_CLIP_CNTL.clip_disable; without it, triangles
            // reaching behind the camera plane rasterised as long slivers across the battle floor.
            r.DepthClipEnable = st.depthClip ? TRUE : FALSE;
            r.ScissorEnable = TRUE;
            Check(device_->CreateRasterizerState(&r, &state), "CreateRasterizerState");
        }
        return state.Get();
    }

    // The stencil reference/mask register for a draw: the back-face one only when the draw
    // can show nothing but back faces (front faces culled), as Xenia's D3D12 path picks it -
    // D3D11, like D3D12, has one reference and one pair of masks for both faces.
    uint32_t StencilRefMask(const DrawState& st) const
    {
        const bool backOnly = ((st.depthCtl >> 7) & 1) && (st.cull & 3) == 1;
        return backOnly ? st.stencilRefMaskBf : st.stencilRefMask;
    }
    UINT StencilRef(const DrawState& st) const { return st.stencil ? (StencilRefMask(st) & 0xFF) : 0; }

    // Xenos compare functions (never, less, equal, ...) and stencil ops (keep, zero, replace,
    // incr-clamp, decr-clamp, invert, incr-wrap, decr-wrap) are in D3D11's order.
    ID3D11DepthStencilState* DepthState(const DrawState& st)
    {
        uint64_t key = (st.zEnable ? 1u : 0u) | (st.zWrite ? 2u : 0u) | (st.zFunc << 2);
        if (st.stencil)
        {
            const uint32_t masks = (StencilRefMask(st) >> 8) & 0xFFFF;          // read mask | write mask << 8
            key |= (1ull << 5) | (uint64_t(st.depthCtl >> 7) << 6) | (uint64_t(masks) << 31);
        }
        auto it = depthStates_.find(key);
        if (it != depthStates_.end()) return it->second.Get();
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = st.zEnable ? TRUE : FALSE;
        d.DepthWriteMask = st.zWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = D3D11_COMPARISON_FUNC(D3D11_COMPARISON_NEVER + (st.zFunc & 7));
        if (st.stencil)
        {
            const uint32_t c = st.depthCtl, rm = StencilRefMask(st);
            d.StencilEnable = TRUE;
            d.StencilReadMask = UINT8((rm >> 8) & 0xFF);
            d.StencilWriteMask = UINT8((rm >> 16) & 0xFF);
            auto face = [](uint32_t bits) {
                D3D11_DEPTH_STENCILOP_DESC f{};
                f.StencilFunc = D3D11_COMPARISON_FUNC(D3D11_COMPARISON_NEVER + (bits & 7));
                f.StencilFailOp = D3D11_STENCIL_OP(D3D11_STENCIL_OP_KEEP + ((bits >> 3) & 7));
                f.StencilPassOp = D3D11_STENCIL_OP(D3D11_STENCIL_OP_KEEP + ((bits >> 6) & 7));
                f.StencilDepthFailOp = D3D11_STENCIL_OP(D3D11_STENCIL_OP_KEEP + ((bits >> 9) & 7));
                return f;
            };
            d.FrontFace = face(c >> 8);
            d.BackFace = ((c >> 7) & 1) ? face(c >> 20) : d.FrontFace;
        }
        ComPtr<ID3D11DepthStencilState> state;
        Check(device_->CreateDepthStencilState(&d, &state), "CreateDepthStencilState");
        return (depthStates_[key] = state).Get();
    }

    static uint32_t ResolveBytes(uint32_t pitch, uint32_t height)
    {
        return ((pitch + 31) & ~31u) * ((height + 31) & ~31u) * 4;
    }

    // Guest bytes covered by a texture fetch constant (conservative for tiled layouts).
    static uint32_t TextureBytes(const Texture& t)
    {
        uint64_t elemBytes, cols, rows;
        if (IsBlockCompressed(t.format))
        {
            elemBytes = (t.format == FMT_DXT1 || t.format == FMT_DXT3A || t.format == FMT_DXT5A || t.format == FMT_CTX1) ? 8 : 16;
            cols = (t.pitch + 3) / 4; rows = (t.height + 3) / 4;
        }
        else
        {
            elemBytes = std::max<uint32_t>(FormatBpp(t.format) / 8, 1);
            cols = t.pitch; rows = t.height;
        }
        if (t.tiled) { cols = (cols + 31) & ~31ull; rows = (rows + 31) & ~31ull; }
        return (uint32_t)std::min<uint64_t>(cols * rows * elemBytes, 64ull << 20);
    }

    // Mip levels uploaded with a 2D texture: down to the last whose longer side is 4 texels or more.
    // TextureMipLevel's layout is MEASURED against the data that far (TTT2_MIP_CENSUS, fights: each
    // level matches the 2x2 box of the one above, mean difference 0.003-0.03 against 0.07-0.4 for
    // unrelated texels); the 2x2 / 1x1 levels of 8-byte-block formats sit elsewhere, and a whole
    // texture under 4 texels on screen needs none. 1 (the base alone): no mips, the mip filter "base
    // map", a cube / array binding, or sides not powers of two (none seen with mips).
    // A/B tests: TTT2_MIPS=0 uploads the base level only; TTT2_MIP_TOGGLE=1 follows the file
    // C:/ttt2/port/mips.off at run time (checked every 30 presents; the texture cache is flushed).
    static std::atomic<bool>& MipsOffFlag() { static std::atomic<bool> f{ [] { const char* e = std::getenv("TTT2_MIPS"); return e && *e == '0'; }() }; return f; }
    static bool MipsOff() { return MipsOffFlag().load(std::memory_order_relaxed); }
    void FollowMipToggle()
    {
        static const bool image = [] { const char* e = std::getenv("TTT2_IMAGE_TOGGLE"); return e && *e == '1'; }();
        if (image && stats_.presents % 30 == 0)
            if (FILE* f = std::fopen("C:/ttt2/port/image.txt", "r"))
            {
                int s = 0, a = 0, t = 0;
                float sc = 0;
                const int got = std::fscanf(f, "%d %d %d %f", &s, &a, &t, &sc);
                if (got >= 3)
                {
                    display_settings::SetSharpness(s);
                    display_settings::SetAntiAliasing(a);
                    display_settings::SetTextureFilter(t);
                }
                std::fclose(f);
                if (got == 4 && sc != ScaleOverride())
                {
                    ScaleOverride() = sc;
                    const float want = RequestedScale();
                    if (want != scale_) Rescale(want);
                }
            }
        static const bool on = [] { const char* e = std::getenv("TTT2_MIP_TOGGLE"); return e && *e == '1'; }();
        if (!on || stats_.presents % 30 != 0) return;
        const bool off = GetFileAttributesA("C:/ttt2/port/mips.off") != INVALID_FILE_ATTRIBUTES;
        if (off == MipsOff()) return;
        MipsOffFlag().store(off);
        textures_.clear();
        textureBytes_ = 0;
        std::fprintf(stderr, "[mip] mip levels %s (texture cache flushed)\n", off ? "off" : "on");
    }
    static uint32_t MipLevelsFor(const Texture& t, bool array)
    {
        if (MipsOff() || array || t.mipMax == 0 || t.mipFilter == 2) return 1;
        if ((t.width & (t.width - 1)) || (t.height & (t.height - 1))) return 1;
        uint32_t levels = 1;
        for (uint32_t l = 1; l <= t.mipMax && l < 14; ++l)
        {
            MipLevel m;
            if (std::max(t.width >> l, t.height >> l) < 4 || !TextureMipLevel(t, l, m)) break;
            levels = l + 1;
        }
        return levels;
    }
    // The guest bytes of levels 1+ outside the base's storage: the mip address up to the end of the
    // last uploaded level's storage (none when the levels are packed into the base's tile).
    static void MipRange(const Texture& t, uint32_t levels, uint32_t& start, uint32_t& size)
    {
        start = size = 0;
        MipLevel m;
        if (levels <= 1 || !TextureMipLevel(t, levels - 1, m) || m.base == t.base || m.base < t.mipBase) return;
        const uint32_t bw = IsBlockCompressed(t.format) ? 4 : 1;
        start = t.mipBase;
        size = uint32_t(std::min<uint64_t>(uint64_t(m.base - t.mipBase) + uint64_t(m.pitch / bw) * ((m.rows + bw - 1) / bw) * TextureBlockBytes(t.format), 64ull << 20));
    }

    void DropTexturesOverlapping(uint32_t start, uint32_t size)
    {
        for (auto it = textures_.begin(); it != textures_.end();)
        {
            const CachedTexture& c = it->second;
            const bool overlap = (uint64_t(c.rangeStart) < uint64_t(start) + size && uint64_t(start) < uint64_t(c.rangeStart) + c.rangeSize) ||
                                 (c.mipSize && uint64_t(c.mipStart) < uint64_t(start) + size && uint64_t(start) < uint64_t(c.mipStart) + c.mipSize);
            it = overlap ? textures_.erase(it) : std::next(it);
        }
    }

    // The frame log (framelog.trigger) notes where each bound texture came from.
    Bound BindTexture(const uint32_t* regs, uint32_t slot, bool array = false)
    {
        const Bound b = BindTextureFrom(regs, slot, array);
        if (g_gpuFrameNote)
        {
            const char* kind = "guest bytes";
            if (b.srv == defaultSrv_.Get() || b.srv == defaultArraySrv_.Get()) kind = "fallback";
            else for (const auto& [dest, r] : resolved_) if (r.srv.Get() == b.srv) { kind = r.depth ? "depth resolve" : "resolve"; break; }
            const Texture t = DecodeTexture(regs, slot);
            char line[160];
            std::snprintf(line, sizeof(line), "    hw t%u: base=%08X fmt=%u %ux%u -> %s\n", slot, t.base, t.format, t.width, t.height, kind);
            g_gpuFrameNote(line);
        }
        return b;
    }
    Bound BindTextureFrom(const uint32_t* regs, uint32_t slot, bool array)
    {
        Texture t = DecodeTexture(regs, slot);
        ID3D11ShaderResourceView* fallback = array ? defaultArraySrv_.Get() : defaultSrv_.Get();
        if (!t.valid || t.width > 8192 || t.height > 8192)
            return { fallback, 0x688, 0, 1, 1, 1.0f, 1.0f };
        // Depth textures (24_8 / 24_8_FLOAT) over a GPU depth resolve: the R32F copy, depth
        // replicated (mode 3). TTT2's shadow map and scene-depth passes read these.
        if (!array && (t.format == FMT_24_8 || t.format == FMT_24_8_FLOAT))
        {
            auto it = resolved_.find(t.base);
            if (it != resolved_.end() && it->second.valid && it->second.depth && t.tiled &&
                ((t.pitch + 31) & ~31u) == ((it->second.pitch + 31) & ~31u) &&
                t.width <= it->second.pitch && t.height <= it->second.height)
            {
                Resolved& r = it->second;
                dirty_.Sync(t.base, r.size);
                if (dirty_.Newest(t.base, r.size) <= r.seq)
                {
                    ++stats_.depthHits;
                    if (CensusOn()) CensusTexture(t, CensusDepth);
                    return { r.srv.Get(), t.swizzle, 3u, t.width, t.height,
                             float(t.width) / float(r.pitch), float(t.height) / float(r.height) };
                }
                r.valid = false;
            }
        }
        // Formats the interpreter cannot decode sample as unswizzled (0, 0, 0, 1) on the CPU
        // (SampleTexel's early return) - e.g. the 24_8 depth formats without a GPU depth resolve.
        // Same answer, no guest bytes needed: never write back or upload for them.
        if (!FormatBpp(t.format) && !IsBlockCompressed(t.format))
        {
            ++undecodable_[t.format];
            if (CensusOn()) CensusTexture(t, CensusUndecodable);
            return { fallback, 0x688, 0, t.width, t.height, 1.0f, 1.0f };
        }
        const uint32_t slices = array && t.dimension == 3 ? 6u : 1u;
        const uint32_t bytes = slices > 1 ? std::min<uint32_t>(slices * TextureFaceStride(t), 384u << 20) : TextureBytes(t);
        const uint32_t levels = MipLevelsFor(t, array);
        uint32_t mipStart = 0, mipSize = 0;
        MipRange(t, levels, mipStart, mipSize);

        // GPU-resolved data at this address?
        bool resolveBytes = false;
        for (auto it = resolved_.begin(); it != resolved_.end(); ++it)
        {
            Resolved& r = it->second;
            if (!r.valid) continue;
            const uint32_t rs = it->first;
            if (!(uint64_t(t.base) < uint64_t(rs) + r.size && uint64_t(rs) < uint64_t(t.base) + bytes) &&
                !(mipSize && uint64_t(mipStart) < uint64_t(rs) + r.size && uint64_t(rs) < uint64_t(mipStart) + mipSize)) continue;
            dirty_.Sync(rs, r.size);
            if (dirty_.Newest(rs, r.size) > r.seq) { r.valid = false; continue; }   // the CPU overwrote it
            if (!array && t.base == rs && (r.rg16f ? t.format == FMT_16_16_FLOAT : t.format == FMT_8_8_8_8) && t.tiled &&
                ((t.pitch + 31) & ~31u) == ((r.pitch + 31) & ~31u) && t.width <= r.pitch && t.height <= r.height)
            {
                ++stats_.resolvedHits;
                if (CensusOn()) CensusTexture(t, CensusResolved);
                return { r.srv.Get(), t.swizzle, (r.swapRB && !r.rg16f) ? 2u : 0u, t.width, t.height,
                         float(t.width) / float(r.pitch), float(t.height) / float(r.height) };
            }
            // Any other view of resolved bytes reads guest memory: make it coherent once.
            resolveBytes = true;
            if (!r.cpuCoherent)
            {
                char cause[96];
                std::snprintf(cause, sizeof(cause), "fmt%u %s%ux%u p%u vs resolve %ux%u%s", t.format,
                              t.tiled ? "tiled " : "", t.width, t.height, t.pitch, r.pitch, r.height,
                              t.base == rs ? "" : " offset");
                ++writebackCauses_[cause];
                WriteBack(rs, r);
            }
        }

        const bool fast = t.format == FMT_8_8_8_8 || t.format == FMT_8 ||
                          t.format == FMT_DXT1 || t.format == FMT_DXT2_3 || t.format == FMT_DXT4_5;
        TexKey key{ t.base, t.format, t.width, t.height, t.pitch, t.tiled ? 1u : 0u, t.endian, fast ? 0xFFFFFFFFu : t.swizzle,
                    slices | (array ? 0x100u : 0u), levels > 1 ? t.mipBase : 0u, levels | (levels > 1 && t.packedMips ? 0x100u : 0u) };
        dirty_.Sync(t.base, bytes);
        if (mipSize) dirty_.Sync(mipStart, mipSize);
        GpuNoteMemory(5, t.base, bytes);
        if (mipSize) GpuNoteMemory(5, mipStart, mipSize);
        if (CensusOn()) CensusTexture(t, resolveBytes ? CensusResolveBytes : CensusUpload);
        if (MipCensusOn() && !array) MipCensus(t);
        auto it = textures_.find(key);
        if (it != textures_.end() && dirty_.Newest(t.base, bytes) <= it->second.seq &&
            (!mipSize || dirty_.Newest(mipStart, mipSize) <= it->second.seq))
        {
            const CachedTexture& c = it->second;
            return { c.srv.Get(), c.genericSwizzled ? 0x688u : t.swizzle, c.mode, t.width, t.height,
                     float(t.width) / float(c.allocW), float(t.height) / float(c.allocH), slices, c.levels };
        }
        if (textures_.size() > 4096 || textureBytes_ > TextureBudgetBytes)
        {
            std::fprintf(stderr, "[gpu-hw] texture cache flushed: %zu entries, %.1f MB\n",
                         textures_.size(), double(textureBytes_) / 1048576.0);
            textures_.clear();
            textureBytes_ = 0;
        }
        if (levels > 1 && MipVerifyOn()) MipVerify(t, levels);
        CachedTexture& c = textures_[key];
        UploadTexture(t, c, slices, array, levels);
        c.rangeStart = t.base; c.rangeSize = bytes; c.mipStart = mipStart; c.mipSize = mipSize; c.seq = dirty_.Current();
        return { c.srv.Get(), c.genericSwizzled ? 0x688u : t.swizzle, c.mode, t.width, t.height,
                 float(t.width) / float(c.allocW), float(t.height) / float(c.allocH), slices, c.levels };
    }

    // Decode one 2D surface (a texture, or one cube face) into host layout and DXGI format.
    void DecodeForUpload(const Texture& t, CachedTexture& c, DXGI_FORMAT& format, UINT& allocW, UINT& allocH,
                         UINT& rowPitch, std::vector<uint8_t>& data)
    {
        allocW = t.width; allocH = t.height;
        c.mode = 0; c.genericSwizzled = false;
        if (t.format == FMT_8_8_8_8)
        {
            // R8G8B8A8: the guest word's low byte is the fetched x (the interpreter's UnpackFormat).
            format = DXGI_FORMAT_R8G8B8A8_UNORM; rowPitch = allocW * 4;
            data.resize(size_t(rowPitch) * allocH);
            auto* px = reinterpret_cast<uint32_t*>(data.data());
            for (uint32_t y = 0; y < t.height; ++y)
                for (uint32_t x = 0; x < t.width; ++x)
                {
                    const uint32_t sx = x + t.originX, sy = y + t.originY;
                    const uint32_t ti = t.tiled ? GpuTiledOffset2D(sx, sy, t.pitch, 2) : (sy * t.pitch + sx);
                    px[size_t(y) * allocW + x] = GRead32(g_base, t.base + ti * 4);
                }
        }
        else if (t.format == FMT_8)
        {
            format = DXGI_FORMAT_R8_UNORM; rowPitch = allocW;
            c.mode = 1;
            data.resize(size_t(rowPitch) * allocH);
            for (uint32_t y = 0; y < t.height; ++y)
            {
                if (!t.tiled)
                    std::memcpy(&data[size_t(y) * allocW], g_base + t.base + size_t(y + t.originY) * t.pitch + t.originX, t.width);
                else
                    for (uint32_t x = 0; x < t.width; ++x)
                        data[size_t(y) * allocW + x] = GRead8(g_base, t.base + GpuTiledOffset2D(x + t.originX, y + t.originY, t.pitch, 0));
            }
        }
        else if (t.format == FMT_DXT1 || t.format == FMT_DXT2_3 || t.format == FMT_DXT4_5)
        {
            const uint32_t blockBytes = t.format == FMT_DXT1 ? 8 : 16, logBpb = blockBytes == 8 ? 3 : 4;
            format = t.format == FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM : t.format == FMT_DXT2_3 ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
            allocW = (t.width + 3) & ~3u; allocH = (t.height + 3) & ~3u;
            const uint32_t blocksX = allocW / 4, blocksY = allocH / 4, bw = (t.pitch + 3) / 4;
            rowPitch = blocksX * blockBytes;
            data.resize(size_t(rowPitch) * blocksY);
            for (uint32_t by = 0; by < blocksY; ++by)
                for (uint32_t bx = 0; bx < blocksX; ++bx)
                {
                    const uint32_t sx = bx + t.originX / 4, sy = by + t.originY / 4;
                    const uint32_t bi = t.tiled ? GpuTiledOffset2D(sx, sy, bw, logBpb) : (sy * bw + sx);
                    const uint32_t addr = t.base + bi * blockBytes;
                    auto* dst = reinterpret_cast<uint32_t*>(&data[size_t(by) * rowPitch + size_t(bx) * blockBytes]);
                    for (uint32_t k = 0; k < blockBytes / 4; ++k) dst[k] = TextureWord(addr + k * 4, t.endian);
                }
        }
        else
        {
            // Every other format: the interpreter's own sampler, swizzle included (exact parity).
            format = DXGI_FORMAT_R32G32B32A32_FLOAT; rowPitch = allocW * 16;
            c.genericSwizzled = true;
            data.resize(size_t(rowPitch) * allocH);
            auto* px = reinterpret_cast<float*>(data.data());
            Texture st = t;
            st.width += t.originX; st.height += t.originY;
            for (uint32_t y = 0; y < t.height; ++y)
                for (uint32_t x = 0; x < t.width; ++x)
                {
                    V4 v = SampleTexel(st, int(x + t.originX), int(y + t.originY));
                    std::memcpy(&px[(size_t(y) * allocW + x) * 4], v.c, 16);
                }
        }
    }

    // Upload a texture. `array` binds it as Texture2DArray (cube-fetch slots); a cube texture
    // then has 6 slices, its faces in D3D order at TextureFaceStride intervals.
    void UploadTexture(const Texture& t, CachedTexture& c, uint32_t slices, bool array, uint32_t levels = 1)
    {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT allocW = 1, allocH = 1, rowPitch = 0;
        std::vector<std::vector<uint8_t>> faces(slices);
        const uint32_t stride = slices > 1 ? TextureFaceStride(t) : 0;
        for (uint32_t s = 0; s < slices; ++s)
        {
            Texture face = t;
            face.base += s * stride;
            DecodeForUpload(face, c, format, allocW, allocH, rowPitch, faces[s]);
        }
        size_t total = 0;
        for (const auto& f : faces) total += f.size();
        // Levels 1+ (2D textures only, MipLevelsFor): each decoded like the base from where
        // TextureMipLevel puts it; D3D's level sizes (allocW >> l) are the guest's (powers of two).
        if (slices != 1) levels = 1;
        std::vector<std::vector<uint8_t>> mips(levels - 1);
        std::vector<UINT> mipPitch(levels - 1);
        for (uint32_t l = 1; l < levels; ++l)
        {
            MipLevel m;
            TextureMipLevel(t, l, m);
            Texture lt = t;
            lt.base = m.base; lt.pitch = m.pitch; lt.width = m.width; lt.height = m.height; lt.originX = m.x; lt.originY = m.y;
            CachedTexture scratch;
            DXGI_FORMAT lf = format;
            UINT lw = 1, lh = 1;
            DecodeForUpload(lt, scratch, lf, lw, lh, mipPitch[l - 1], mips[l - 1]);
            total += mips[l - 1].size();
        }
        D3D11_TEXTURE2D_DESC d{};
        d.Width = allocW; d.Height = allocH; d.MipLevels = levels; d.ArraySize = slices; d.Format = format;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (!c.tex || c.allocW != allocW || c.allocH != allocH || c.slices != slices || c.levels != levels)
        {
            c.tex.Reset(); c.srv.Reset();
            std::vector<D3D11_SUBRESOURCE_DATA> init(size_t(slices) * levels);
            for (uint32_t s = 0; s < slices; ++s) init[size_t(s) * levels] = { faces[s].data(), rowPitch, 0 };
            for (uint32_t l = 1; l < levels; ++l) init[l] = { mips[l - 1].data(), mipPitch[l - 1], 0 };
            Check(device_->CreateTexture2D(&d, init.data(), &c.tex), "CreateTexture");
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = format;
            if (array)
            {
                v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                v.Texture2DArray.MipLevels = levels; v.Texture2DArray.ArraySize = slices;
            }
            else { v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = levels; }
            Check(device_->CreateShaderResourceView(c.tex.Get(), &v, &c.srv), "CreateTextureSrv");
            textureBytes_ += total;
        }
        else
        {
            for (uint32_t s = 0; s < slices; ++s)
                context_->UpdateSubresource(c.tex.Get(), D3D11CalcSubresource(0, s, levels), nullptr, faces[s].data(), rowPitch, 0);
            for (uint32_t l = 1; l < levels; ++l)
                context_->UpdateSubresource(c.tex.Get(), D3D11CalcSubresource(l, 0, levels), nullptr, mips[l - 1].data(), mipPitch[l - 1], 0);
        }
        c.allocW = allocW; c.allocH = allocH; c.slices = slices; c.levels = levels;
        ++stats_.uploads;
        stats_.uploadBytes += total;
        auto& f = uploadByFormat_[t.format | (t.tiled ? 0x100u : 0u) | (slices > 1 ? 0x200u : 0u)];
        ++f.first; f.second += total;
    }

    // Guest word for a resolved depth texel: 24-bit depth in the high bits, stencil 0. The
    // float format is Xenos 20e4 (Xenia's Float32To20e4, round to nearest even).
    static uint32_t DepthWord(uint32_t f32, bool depthFloat)
    {
        float d; std::memcpy(&d, &f32, 4);
        if (!depthFloat)
            return uint32_t(std::clamp(d, 0.0f, 1.0f) * 16777215.0f + 0.5f) << 8;
        if (int32_t(f32) <= 0) return 0;
        if (f32 >= 0x3FFFFFF8u) return 0xFFFFFFu << 8;
        if (f32 < 0x38800000u)
        {
            const uint32_t shift = std::min<uint32_t>(113u - (f32 >> 23), 24u);
            f32 = (0x800000u | (f32 & 0x7FFFFFu)) >> shift;
        }
        else f32 += 0xC8000000u;
        f32 += 3u + ((f32 >> 3) & 1u);
        return ((f32 >> 3) & 0xFFFFFFu) << 8;
    }

    // Write a resolved texture into guest memory exactly as the CPU resolve does (tiled,
    // big-endian words in the hardware's order: red in the low byte, or - copy_dest_swap -
    // the A8R8G8B8 word 0xAARRGGBB), then accept those bytes as the entry's own. MEASURED
    // 2026-10-12: the character texture bake (CPU recompressors reading its 1024x1024 swapped
    // resolve) drew Devil Jin's red flames blue with the two orders exchanged.
    // Start a resolve's copy to its staging texture at the logical size WriteBack reads (scaled
    // surfaces are resampled first, as ReadLogical does). The GPU runs it in order with the rest of
    // the frame and the flush hands it over now, so a later Map waits for this copy at most.
    void StageReadback(Resolved& r)
    {
        D3D11_TEXTURE2D_DESC d{};
        r.tex->GetDesc(&d);
        ID3D11Texture2D* source = r.tex.Get();
        if (d.Width != r.pitch || d.Height != r.height)
        {
            if (!r.logical)
            {
                D3D11_TEXTURE2D_DESC t{};
                t.Width = r.pitch; t.Height = r.height; t.MipLevels = t.ArraySize = 1; t.Format = d.Format;
                t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_RENDER_TARGET;
                Check(device_->CreateTexture2D(&t, nullptr, &r.logical), "CreateReadbackLogical");
                Check(device_->CreateRenderTargetView(r.logical.Get(), nullptr, &r.logicalRtv), "CreateReadbackLogicalRtv");
            }
            Resample(r.srv.Get(), r.logicalRtv.Get(), nullptr, r.pitch, r.height, 1.0f / float(r.pitch), 1.0f / float(r.height),
                     !r.depth && Filterable(d.Format));
            source = r.logical.Get();
        }
        if (!r.staging)
        {
            D3D11_TEXTURE2D_DESC s{};
            s.Width = r.pitch; s.Height = r.height; s.MipLevels = s.ArraySize = 1; s.Format = d.Format;
            s.SampleDesc.Count = 1; s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Check(device_->CreateTexture2D(&s, nullptr, &r.staging), "CreateReadbackStaging");
        }
        D3D11_BOX box{ 0, 0, 0, r.pitch, r.height, 1 };
        context_->CopySubresourceRegion(r.staging.Get(), 0, 0, 0, 0, source, 0, &box);
        if (readbackFence_) context4_->Signal(readbackFence_.Get(), r.stagedFence = ++readbackFenceValue_);
        Probe("staged copy");
        context_->Flush();
        r.staged = true;
        r.stagedSeq = r.seq;
        r.stagedAt = std::chrono::steady_clock::now();
        ++stats_.stagedReadbacks;
    }

    // Small resolves staged for the CPU (the exposure readback): into guest memory as soon as the
    // GPU has done their copy - never waiting for it.
    std::unordered_set<uint32_t> smallPending_;
    void FinishSmallWritebacks()
    {
        for (auto it = smallPending_.begin(); it != smallPending_.end();)
        {
            auto f = resolved_.find(*it);
            if (f == resolved_.end() || !f->second.valid || !f->second.staged || f->second.stagedSeq != f->second.seq)
            {
                it = smallPending_.erase(it);
                continue;
            }
            Resolved& r = f->second;
            if (readbackFence_ && readbackFence_->GetCompletedValue() < r.stagedFence) { ++it; continue; }
            std::vector<uint32_t> px;
            if (!ReadStaging(r.staging.Get(), r.pitch, r.height, px, true)) { ++it; continue; }
            r.staged = false;
            Scatter(*it, r, px, 0);
            it = smallPending_.erase(it);
        }
    }

    // A resolve back into guest memory now, waiting for the GPU (TTT2_GPU_HW_WRITEBACK=1, and the
    // small ones with TTT2_SMALL_WRITEBACK_SYNC=1).
    void WriteBack(uint32_t dest, Resolved& r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<uint32_t> px;
        if (r.staged && r.stagedSeq == r.seq) ReadStaging(r.staging.Get(), r.pitch, r.height, px, false);   // copied at resolve time
        else ReadLogical(r.tex.Get(), r.pitch, r.height, r.pitch, r.height, !r.depth, px);
        r.staged = false;
        Scatter(dest, r, px, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }

    // The logical-size pixels `px` of resolve `r` into guest memory (tiled, in the guest's byte and
    // channel order); the resolve is CPU-coherent from now on.
    void Scatter(uint32_t dest, Resolved& r, const std::vector<uint32_t>& px, double readMs)
    {
        using clock = std::chrono::steady_clock;
        const auto t1 = clock::now();
        const uint32_t aligned = (r.pitch + 31) & ~31u;
        auto rows = [&](uint32_t y0, uint32_t y1) {
            for (uint32_t y = y0; y < y1; ++y)
                for (uint32_t x = 0; x < r.pitch; ++x)
                {
                    uint32_t v = px[size_t(y) * r.pitch + x];
                    if (r.depth) v = DepthWord(v, r.depthFloat);
                    else if (r.rg16f) {}       // R16G16_FLOAT texel = red half | green half << 16, as fetched
                    else if (!r.swapRB) v = (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
                    GWrite32(g_base, dest + GpuTiledOffset2D(x, y, aligned, 2) * 4, v);
                }
        };
        // Large ones (the fight intro's 1024x1024 character texture bakes, ~2 ms each with a game
        // thread waiting) in four bands at once: every texel has its own guest address.
        if (size_t(r.pitch) * r.height >= 256 * 1024 && r.height >= 64)
        {
            const uint32_t band = ((r.height + 3) / 4 + 31) & ~31u;    // 4 bands cover every row
            std::thread helpers[3];
            for (uint32_t i = 0; i < 3; ++i)
                helpers[i] = std::thread(rows, std::min(r.height, band * (i + 1)), std::min(r.height, band * (i + 2)));
            rows(0, std::min(r.height, band));
            for (auto& t : helpers) t.join();
        }
        else rows(0, r.height);
        dirty_.Sync(dest, r.size);
        r.seq = dirty_.Current();
        r.cpuCoherent = true;
        ++stats_.writebacks;
        static const bool timing = [] { const char* e = std::getenv("TTT2_FRAMETIME"); return e && *e == '1'; }();
        if (timing && r.pitch * r.height > 64)
        {
            static int logged = 0;
            const auto t2 = clock::now();
            if (logged++ < 200)
                std::fprintf(stderr, "[frame] writeback %08X %ux%u: read %.2f ms (map wait %.2f ms), scatter %.2f ms, copy queued %.2f ms before\n",
                             dest, r.pitch, r.height, readMs, lastMapWaitMs_, std::chrono::duration<double, std::milli>(t2 - t1).count(),
                             lastStagedAgeMs_);
        }
        if (CensusOn() && r.pitch * r.height <= 64)
        {
            // Diagnostics: small resolves the CPU reads back (exposure / luminance readbacks):
            // destination and the first guest words as the CPU now sees them.
            static std::map<uint32_t, uint32_t> seen;
            if (seen[dest]++ % 120 == 0)
                std::fprintf(stderr, "[census] cpu reads resolve %08X %ux%u swap=%d: %08X %08X %08X %08X (read #%u)\n",
                             dest, r.pitch, r.height, int(r.swapRB), GRead32(g_base, dest), GRead32(g_base, dest + 4),
                             GRead32(g_base, dest + 8), GRead32(g_base, dest + 12), seen[dest]);
        }
    }

    // A staged readback (StageReadback): map the staging texture, its copy already queued. With
    // `noWait`, false (nothing read) while the GPU has not finished the copy yet.
    bool ReadStaging(ID3D11Texture2D* staging, uint32_t width, uint32_t height, std::vector<uint32_t>& out, bool noWait)
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        const auto mapStart = std::chrono::steady_clock::now();
        const HRESULT hr = context_->Map(staging, 0, D3D11_MAP_READ, noWait ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0, &m);
        if (noWait && hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
        Check(hr, "MapReadbackStaging");
        lastMapWaitMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mapStart).count();
        out.resize(size_t(width) * height);
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(&out[size_t(y) * width], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, width * 4);
        context_->Unmap(staging, 0);
        return true;
    }

    void ReadTexture(ID3D11Texture2D* tex, uint32_t width, uint32_t height, std::vector<uint32_t>& out)
    {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        d.Width = width; d.Height = height; d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        Check(device_->CreateTexture2D(&d, nullptr, &staging), "CreateStaging");
        D3D11_BOX box{ 0, 0, 0, width, height, 1 };
        context_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        const auto mapStart = std::chrono::steady_clock::now();
        Check(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "MapStaging");
        lastMapWaitMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mapStart).count();
        out.resize(size_t(width) * height);
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(&out[size_t(y) * width], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, width * 4);
        context_->Unmap(staging.Get(), 0);
    }

    // The first w x h pixels of `tex` - whose whole extent is logicalW x logicalH guest pixels -
    // at logical size, for CPU readers: a plain copy at scale 1; otherwise read on the GPU at
    // logical pixel centres (bilinear: a 2x2 box at 2x, the middle texel at 3x; point reads for
    // depth and R32 formats - always a texel inside the logical pixel).
    void ReadLogical(ID3D11Texture2D* tex, uint32_t logicalW, uint32_t logicalH, uint32_t w, uint32_t h,
                     bool linear, std::vector<uint32_t>& out)
    {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        if (d.Width == logicalW && d.Height == logicalH) { ReadTexture(tex, w, h, out); return; }
        D3D11_TEXTURE2D_DESC t{};
        t.Width = w; t.Height = h; t.MipLevels = t.ArraySize = 1; t.Format = d.Format;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> logical;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
        Check(device_->CreateTexture2D(&t, nullptr, &logical), "CreateLogicalRead");
        Check(device_->CreateRenderTargetView(logical.Get(), nullptr, &rtv), "CreateLogicalReadRtv");
        Check(device_->CreateShaderResourceView(tex, nullptr, &srv), "CreateLogicalReadSrv");
        Resample(srv.Get(), rtv.Get(), nullptr, w, h, 1.0f / float(logicalW), 1.0f / float(logicalH), linear && Filterable(d.Format));
        ReadTexture(logical.Get(), w, h, out);
    }

    // logicalW / logicalH: the guest size the returned texture holds (scaled or not).
    ID3D11Texture2D* FrontTexture(uint32_t fb, bool& storedSwap, uint32_t& logicalW, uint32_t& logicalH)
    {
        storedSwap = false;
        if (!fb) return nullptr;
        auto it = resolved_.find(fb);
        if (it != resolved_.end() && it->second.valid && it->second.pitch >= 1280 && it->second.height >= 720)
        {
            dirty_.Sync(fb, it->second.size);
            if (dirty_.Newest(fb, it->second.size) <= it->second.seq)
            {
                storedSwap = it->second.swapRB;
                logicalW = it->second.pitch; logicalH = it->second.height;
                return it->second.tex.Get();
            }
            it->second.valid = false;
        }
        logicalW = 1280; logicalH = 720;
        // Not GPU-resolved (or overwritten by the CPU): decode guest memory like the GDI path.
        // The B8G8R8A8 view reads the word as ARGB, red and blue exchanged against the fetch
        // order (low byte = x), the same as a copy_dest_swap resolve.
        storedSwap = true;
        if (!cpuFront_)
        {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = 1280; d.Height = 720; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            Check(device_->CreateTexture2D(&d, nullptr, &cpuFront_), "CreateCpuFront");
        }
        static std::vector<uint32_t> px(1280 * 720);
        for (uint32_t y = 0; y < 720; ++y)
            for (uint32_t x = 0; x < 1280; ++x)
                px[size_t(y) * 1280 + x] = GRead32(g_base, fb + GpuTiledOffset2D(x, y, 1280, 2) * 4);
        context_->UpdateSubresource(cpuFront_.Get(), 0, nullptr, px.data(), 1280 * 4, 0);
        return cpuFront_.Get();
    }

    static void WriteBmp(const char* path, const uint32_t* px, uint32_t w, uint32_t h)
    {
        FILE* f = std::fopen(path, "wb");
        if (!f) return;
        uint8_t hdr[54] = {};
        const uint32_t size = w * h * 4;
        hdr[0] = 'B'; hdr[1] = 'M';
        *reinterpret_cast<uint32_t*>(hdr + 2) = 54 + size;
        *reinterpret_cast<uint32_t*>(hdr + 10) = 54;
        *reinterpret_cast<uint32_t*>(hdr + 14) = 40;
        *reinterpret_cast<int32_t*>(hdr + 18) = (int32_t)w;
        *reinterpret_cast<int32_t*>(hdr + 22) = -(int32_t)h;
        *reinterpret_cast<uint16_t*>(hdr + 26) = 1;
        *reinterpret_cast<uint16_t*>(hdr + 28) = 32;
        *reinterpret_cast<uint32_t*>(hdr + 34) = size;
        std::fwrite(hdr, 1, sizeof(hdr), f);
        std::fwrite(px, 4, size_t(w) * h, f);
        std::fclose(f);
    }

    // ---- frame cost (the 5 s stats): GPU time from one present to the next (timestamp queries; read
    // three frames later without waiting - GPU work plus any gaps the GPU waited for commands, so an
    // upper bound of its busy time), CPU time per frame of the game's main thread and of this command-
    // processor thread (thread cycle counters), and the whole process's CPU use.
    struct GpuTimer { ComPtr<ID3D11Query> disjoint, begin, end; bool open = false, pending = false; };
    std::array<GpuTimer, 4> gpuTimers_{};
    uint32_t gpuTimerAt_ = 0;
    std::vector<float> gpuFrameMs_;
    void GpuTimerFrameEnd()
    {
        GpuTimer& t = gpuTimers_[gpuTimerAt_ & 3];
        if (!t.open) return;
        context_->End(t.end.Get());
        context_->End(t.disjoint.Get());
        t.open = false;
        t.pending = true;
    }
    void GpuTimerFrameBegin()
    {
        for (GpuTimer& t : gpuTimers_)
        {
            if (!t.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 a = 0, b = 0;
            if (context_->GetData(t.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            if (context_->GetData(t.begin.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                context_->GetData(t.end.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            t.pending = false;
            if (!dj.Disjoint && dj.Frequency && b > a && gpuFrameMs_.size() < 4096) gpuFrameMs_.push_back(float(double(b - a) * 1000.0 / double(dj.Frequency)));
        }
        GpuTimer& t = gpuTimers_[++gpuTimerAt_ & 3];
        if (t.pending || t.open) return;                // still unread: this frame goes untimed
        if (!t.disjoint)
        {
            D3D11_QUERY_DESC q{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            D3D11_QUERY_DESC s{ D3D11_QUERY_TIMESTAMP, 0 };
            if (FAILED(device_->CreateQuery(&q, &t.disjoint)) || FAILED(device_->CreateQuery(&s, &t.begin)) || FAILED(device_->CreateQuery(&s, &t.end)))
            { t.disjoint.Reset(); return; }
        }
        context_->Begin(t.disjoint.Get());
        context_->End(t.begin.Get());
        t.open = true;
    }
    uint64_t costCpCycles_ = 0, costMainCycles_ = 0, costTsc_ = 0, costProc100ns_ = 0;
    void ReportFrameCost(double seconds, uint64_t presents)
    {
        ULONG64 cp = 0, mainCycles = 0;
        QueryThreadCycleTime(GetCurrentThread(), &cp);
        if (g_mainThreadHandle) QueryThreadCycleTime(g_mainThreadHandle, &mainCycles);
        const uint64_t tsc = __rdtsc();
        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        const uint64_t proc = (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
        if (costTsc_ && presents)
        {
            const double tscPerMs = double(tsc - costTsc_) / (seconds * 1000.0);
            const double cpMs = double(cp - costCpCycles_) / tscPerMs / double(presents);
            const double mainMs = double(mainCycles - costMainCycles_) / tscPerMs / double(presents);
            const double cores = double(proc - costProc100ns_) / (seconds * 1e7);
            std::sort(gpuFrameMs_.begin(), gpuFrameMs_.end());
            double sum = 0;
            for (float v : gpuFrameMs_) sum += v;
            const size_t n = gpuFrameMs_.size();
            std::fprintf(stderr, "[gpu-hw]   frame cost: gpu %.2f ms between presents (median %.2f, p95 %.2f, max %.2f; %zu frames), "
                         "cpu per frame: game main thread %.2f ms, command processor %.2f ms; process %.2f cores\n",
                         n ? sum / double(n) : 0.0, n ? gpuFrameMs_[n / 2] : 0.0f, n ? gpuFrameMs_[n * 95 / 100] : 0.0f, n ? gpuFrameMs_[n - 1] : 0.0f, n,
                         mainMs, cpMs, cores);
        }
        gpuFrameMs_.clear();
        costCpCycles_ = cp; costMainCycles_ = mainCycles; costTsc_ = tsc; costProc100ns_ = proc;
    }

    void MaybeReportStats()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastStats_ < std::chrono::seconds(5)) return;
        const double s = std::chrono::duration<double>(now - lastStats_).count();
        lastStats_ = now;
        const Stats d{ stats_.draws - reported_.draws, stats_.triangles - reported_.triangles,
                       stats_.skippedDraws - reported_.skippedDraws, stats_.emptyDraws - reported_.emptyDraws,
                       stats_.resolves - reported_.resolves, stats_.presents - reported_.presents,
                       stats_.compiles - reported_.compiles, stats_.diskHits - reported_.diskHits,
                       stats_.uploads - reported_.uploads, stats_.uploadBytes - reported_.uploadBytes,
                       stats_.resolvedHits - reported_.resolvedHits, stats_.writebacks - reported_.writebacks,
                       stats_.depthResolves - reported_.depthResolves, stats_.depthHits - reported_.depthHits,
                       stats_.gpuVsDraws - reported_.gpuVsDraws, stats_.vertexDataBytes - reported_.vertexDataBytes };
        const uint64_t warmHits = stats_.warmHits - reported_.warmHits, pendingSkips = stats_.pendingSkips - reported_.pendingSkips;
        const uint64_t staged = stats_.stagedReadbacks - reported_.stagedReadbacks;
        const uint64_t ringWaits = stats_.ringWaits - reported_.ringWaits;
        const double ringWaitMs = stats_.ringWaitMs - reported_.ringWaitMs;
        const uint64_t resolveCreates = stats_.resolveCreates - reported_.resolveCreates;
        const uint64_t resolvePoolHits = stats_.resolvePoolHits - reported_.resolvePoolHits;
        const uint64_t shadowSkips = stats_.shadowSkips - reported_.shadowSkips;
        reported_ = stats_;
        if (shadowSkips) std::fprintf(stderr, "[gpu-hw]   shadows off: %llu shadow-map draws skipped\n", (unsigned long long)shadowSkips);
        std::fprintf(stderr,
            "[gpu-hw] %.1fs: draws=%llu tris=%llu skipped=%llu empty=%llu resolves=%llu presents=%llu (%.1f/s) "
            "ps_compiles=%llu ps_disk=%llu shader_warm=%llu shader_pending_skips=%llu tex_uploads=%llu (%.1f MB) resolved_hits=%llu writebacks=%llu (staged %llu) "
            "depth_resolves=%llu depth_hits=%llu shaders=%zu textures=%zu gpu_vs_draws=%llu (%.1f MB vertex data, %zu vs) ring_waits=%llu (%.1f ms) resolve_textures=%llu new %llu reused\n",
            s, (unsigned long long)d.draws, (unsigned long long)d.triangles, (unsigned long long)d.skippedDraws,
            (unsigned long long)d.emptyDraws, (unsigned long long)d.resolves, (unsigned long long)d.presents,
            double(d.presents) / s, (unsigned long long)d.compiles, (unsigned long long)d.diskHits, (unsigned long long)warmHits, (unsigned long long)pendingSkips,
            (unsigned long long)d.uploads, double(d.uploadBytes) / 1048576.0, (unsigned long long)d.resolvedHits,
            (unsigned long long)d.writebacks, (unsigned long long)staged, (unsigned long long)d.depthResolves, (unsigned long long)d.depthHits,
            pixelShaders_.size(), textures_.size(), (unsigned long long)d.gpuVsDraws,
            double(d.vertexDataBytes) / 1048576.0, vertexShaders_.size(), (unsigned long long)ringWaits, ringWaitMs,
            (unsigned long long)resolveCreates, (unsigned long long)resolvePoolHits);
        ReportFrameCost(s, d.presents);
        if (VcacheStatsOn())
        {
            std::fprintf(stderr, "[gpu-hw]   vertex uploads: %.1f MB unchanged since their last upload, %.1f MB changed, %.1f MB first seen; %llu ranges synced (%zu known)\n",
                         vcacheSame_ / 1048576.0, vcacheChanged_ / 1048576.0, vcacheNew_ / 1048576.0, (unsigned long long)vcacheSyncs_, vcacheSeen_.size());
            vcacheSame_ = vcacheChanged_ = vcacheNew_ = vcacheSyncs_ = 0;
        }
        if (CallCountOn())
        {
            std::map<std::string, uint64_t> counts;
            {
                std::lock_guard l(g_callCountMutex);
                counts.swap(g_callCounts);
            }
            std::string line = "[gpu-hw]   d3d calls:";
            for (const auto& [label, n] : counts) line += " " + label + "=" + std::to_string(n);
            std::fprintf(stderr, "%s\n", line.c_str());
        }
        if (!uploadByFormat_.empty())
        {
            char line[512]; int n = std::snprintf(line, sizeof(line), "[gpu-hw]   uploads by format:");
            for (const auto& [format, v] : uploadByFormat_)
                if (n < 440)
                    n += std::snprintf(line + n, sizeof(line) - n, " %u%s:%llu/%.1fMB", format & 0xFF,
                                       (format & 0x100) ? "t" : "", (unsigned long long)v.first, double(v.second) / 1048576.0);
            std::fprintf(stderr, "%s cache=%.1fMB colour_surfaces=%zu depth_surfaces=%zu resolved=%zu scale=%.2f surfaces=%.0fMB\n", line,
                         double(textureBytes_) / 1048576.0, colors_.size(), depths_.size(), resolved_.size(), scale_,
                         SurfaceMegabytes());
            uploadByFormat_.clear();
        }
        for (const auto& [cause, count] : writebackCauses_)
            std::fprintf(stderr, "[gpu-hw]   writeback x%llu: %s\n", (unsigned long long)count, cause.c_str());
        writebackCauses_.clear();
        for (const auto& [format, count] : undecodable_)
            std::fprintf(stderr, "[gpu-hw]   undecodable texture format %u bound x%llu (constant 0,0,0,1 as on CPU)\n",
                         format, (unsigned long long)count);
        undecodable_.clear();
    }
};

inline Backend& Get()
{
    static Backend* b = new Backend;     // leaked on purpose: no device teardown at exit
    return *b;
}

// Serialises backend use: the command processor thread drives it, but a guest thread's first
// read of a cached-view page asks it for a writeback (WriteBackRange) from that thread.
inline std::recursive_mutex g_backendMutex;

// Run a backend operation; a device-level failure disables the backend for the rest of the
// run (the CPU renderer takes over from the next draw) instead of killing the process.
template <typename F>
inline bool Guarded(const char* what, F&& f)
{
    std::lock_guard<std::recursive_mutex> lock(g_backendMutex);
    try { f(); return true; }
    catch (const std::exception& e)
    {
        g_failed.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "[gpu-hw] DISABLED after failure in %s: %s (device removed reason %08X)\n",
                     what, e.what(), (unsigned)Get().RemovedReason());
        return false;
    }
}

} // namespace ttt2_hw
