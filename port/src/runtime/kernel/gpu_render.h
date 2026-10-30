#pragma once
// GPU milestone 2: CPU-side execution of the game's draw packets.
//
// The command processor in kernel_video.cpp decodes PM4 and keeps a Xenos register file;
// this module turns a draw packet into pixels. It interprets the real Xenos shader
// microcode (vertex + pixel) and rasterizes into an "EDRAM-lite" colour surface keyed by
// RB_COLOR_INFO.color_base, which the resolve path then copies into guest memory exactly
// where the game's own front-buffer texture fetch constant points.
#include <cstdint>

// One bound shader: guest address of its microcode plus its length in dwords.
struct GpuShader
{
    uint32_t addr = 0;
    uint32_t size = 0;
};

// Rasterize one draw: rectangle lists, triangle lists/strips/fans and quad lists, with
// auto-index, immediate or DMA index sourcing. `indexAddr` is the guest address of the index
// buffer (0 when the draw is auto-indexed) and `index32` selects 32- vs 16-bit indices.
// Returns false for primitive types that are not handled, so the caller can carry on.
bool GpuExecuteDraw(const uint32_t* regs, uint32_t initiator, uint32_t indexAddr,
                    bool index32, const GpuShader& vs, const GpuShader& ps);

// Copy the EDRAM-lite surface for `colorBase` into `out` (pitch*height ARGB8888 pixels,
// linear, top-down). Returns false when nothing has ever been rasterized into it.
bool GpuReadEdram(uint32_t colorBase, uint32_t pitch, uint32_t height, uint32_t* out);

// Fast-clear latch: fill an EDRAM-lite surface with one colour.
uint64_t GpuLitDraws();
void GpuFillEdram(uint32_t colorBase, uint32_t pitch, uint32_t height, uint32_t argb);

// Hardware (D3D11) backend - gpu_hw_d3d11.h. TTT2_GPU_HW=1 renders on the GPU,
// TTT2_GPU_HW=compare also runs the CPU renderer and compares every resolve.
// GpuHwMode: 0 off (CPU renderer), 1 hardware, 2 compare.
int GpuHwMode();
// srcSelect: RB_COPY_CONTROL.copy_src_select (4 = depth from depthBase, RB_DEPTH_INFO base);
// depthFloat: RB_DEPTH_INFO.depth_format (D24FS8).
void GpuHwResolve(uint32_t colorBase, uint32_t destBase, uint32_t pitch, uint32_t height,
                  bool hasLatch, uint32_t latch, bool swapRB, uint32_t srcSelect = 0,
                  uint32_t depthBase = 0, bool depthFloat = false, uint32_t destFormat = 6);
void GpuHwCompareResolve(uint32_t destBase, const uint32_t* cpuLinear, uint32_t pitch, uint32_t height);
// gammaRamp: the guest's 256-entry display gamma table (DC_LUT_30_COLOR words); gammaVersion
// changes whenever it was written.
bool GpuHwPresent(uint32_t frontBuffer, bool displaySwapRB, const uint32_t* gammaRamp, uint32_t gammaVersion);
bool GpuHwReadFrontBuffer(uint32_t* bgrx1280x720);
// The swap chain is in exclusive fullscreen (display_settings Fullscreen, window in front).
bool GpuHwIsExclusiveFullscreen();
void GpuHwSetWindow(void* window);
bool GpuHwDumpResolved(uint32_t destBase, const char* bmpPath);   // diagnostics
// Write GPU-only resolves overlapping [start, start + size) into guest memory before the CPU
// reads them (callable from any thread; the backend serialises itself).
void GpuHwWriteBackRange(uint32_t start, uint32_t size);

// Fetch-constant slots sampled by a pixel shader (diagnostics; 0 if not analysable).
uint32_t GpuPixelShaderTextureSlots(uint32_t addr, uint32_t sizeDwords);

// Xbox 360 2D tiled-surface texel index (XGAddress2DTiledOffset). `logBpp` is log2 of the
// element size in bytes; for block-compressed formats x/y are block coordinates.
uint32_t GpuTiledOffset2D(uint32_t x, uint32_t y, uint32_t width, uint32_t logBpp);
