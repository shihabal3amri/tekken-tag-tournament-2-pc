#pragma once
// Xenos pixel-shader microcode -> HLSL (Shader Model 5) for the hardware backend.
//
// The semantics deliberately MIRROR the CPU interpreter in gpu_render.cpp (RunShader,
// ExecAlu, ExecTextureFetch), so the interpreter stays a usable oracle
// (port/tools/test_gpu_hw_translate.py runs both on the same inputs). Correctness fixes must
// be made to BOTH, together. Reference for the semantics of both: Xenia's ucode.h, shader.h
// and shader_translator.cc (tools/ref_xenia), i.e. the IPR2015-00325 sequencer specification.
//
// Known shared simplifications: scalar operands are read after the vector write; textures
// sample mip 0; texture fetch opcodes other than plain tfetch are no-ops. Address modes and
// the mag filter come from the fetch constant (TexAddress / XAddr).
//
// Include after the interpreter's Cf/Alu/TFetch/VFetch decoders (inside gpu_render.cpp).
#include <algorithm>
#include <cstdint>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ttt2_hw {

struct PsTranslation
{
    std::string function;        // HLSL: XOut XenosPs(float4 inputs[16])
    uint32_t textureSlots = 0;   // bitmask of fetch-constant slots sampled
    uint32_t cubeSlots = 0;      // of those, slots fetched as cubes (bound as Texture2DArray)
    bool writesDepth = false;    // exports oDepth (export 61, X): the function sets eDepth / wroteDepth
    std::string unsupported;     // non-empty: the shader cannot be translated
};

// Shared declarations for every translated pixel shader. Register map:
//   b0 = 256 pixel ALU constants (register file entries 256..511)
//   b1 = per-draw state (bool + loop constants, texture remap/swizzle/scale, alpha test)
//   t0..t31 = textures by fetch-constant slot, s0..s15 = that slot's sampler (slot & 15)
inline const char* PsPrelude()
{
    return R"(
cbuffer XenosPsConstants : register(b0) { float4 c[256]; };
cbuffer XenosPsState : register(b1)
{
    uint4 boolConsts[2];
    uint4 loopConsts[8];
    uint4 texInfo[32];      // x = fetch-constant swizzle, y = remap mode, z = width, w = height
    float4 texScale[32];    // xy = logical size / allocated size
    uint4 texSampler[32];   // x/y = ClampMode u/v, z = bilinear, w = border colour
                            // texScale.z = array slices bound (6 = a cube texture)
    float alphaRef; uint alphaTest; uint alphaFunc; uint statePad;
};
SamplerState xs[16] : register(s0);
struct XOut { float4 color; bool killed; };
float4 XC(int i) { return (uint)i < 256u ? c[(uint)i] : float4(0, 0, 0, 0); }
bool XBool(uint i) { return ((boolConsts[i >> 7][(i >> 5) & 3] >> (i & 31)) & 1) != 0; }
uint XLoop(uint i) { return loopConsts[i >> 2][i & 3]; }
float XSel(float4 v, uint s, float self) { return s < 4 ? v[s] : (s == 4 ? 0.0 : (s == 5 ? 1.0 : self)); }
float4 XSwz(float4 v, uint sw)
{
    return float4(XSel(v, sw & 7, v.x), XSel(v, (sw >> 3) & 7, v.y),
                  XSel(v, (sw >> 6) & 7, v.z), XSel(v, (sw >> 9) & 7, v.w));
}
// Bring a host texel to the interpreter's UnpackFormat component order before the
// fetch-constant swizzle: 1 = single-channel R8 (k_8 replicates), 2 = 2_10_10_10 order,
// 3 = depth (24_8 / 24_8_FLOAT resolved as R32F; the depth replicates like Xenia's RRRR).
float4 XRemap(float4 f, uint mode)
{
    if (mode == 1) return float4(f.r, f.r, f.r, 1.0);
    if (mode == 2) return f.zyxw;
    if (mode == 3) return f.rrrr;
    return f;
}
// Texel addressing exactly as the interpreter's TexAddress (-1 = border texel).
int XAddr(int x, int size, uint mode)
{
    if (mode == 0) { int m = x % size; return m < 0 ? m + size : m; }
    if (mode == 1) { int p = 2 * size; int m = x % p; if (m < 0) m += p; return m < size ? m : p - 1 - m; }
    if (mode == 3 || mode == 5 || mode == 7) { int m = x < 0 ? -x - 1 : x; return min(m, size - 1); }
    if (mode == 6) return (x < 0 || x >= size) ? -1 : x;
    return clamp(x, 0, size - 1);
}
int XFloor(float f) { return (int)floor(clamp(f, -16777216.0, 16777216.0)); }
// Exact CPU-interpreter filtering (SampleTexture/SampleTexel): used only by parity tests to
// separate translation errors from hardware filtering precision (8-bit bilinear weights).
float4 XTexel(Texture2D<float4> t, int x, int y, uint2 size, uint4 smp)
{
    x = XAddr(x, (int)size.x, smp.x);
    y = XAddr(y, (int)size.y, smp.y);
    if (x < 0 || y < 0) return smp.w == 1 ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);
    return t.Load(int3(x, y, 0));
}
float4 XExactSample(Texture2D<float4> t, float2 uv, uint2 size, uint4 smp)
{
    if (smp.z == 0) return XTexel(t, XFloor(uv.x * size.x), XFloor(uv.y * size.y), size, smp);
    float fx = clamp(uv.x * size.x - 0.5, -16777216.0, 16777216.0);
    float fy = clamp(uv.y * size.y - 0.5, -16777216.0, 16777216.0);
    int x0 = (int)floor(fx), y0 = (int)floor(fy);
    float tx = fx - x0, ty = fy - y0;
    float4 a = XTexel(t, x0, y0, size, smp), b = XTexel(t, x0 + 1, y0, size, smp);
    float4 c2 = XTexel(t, x0, y0 + 1, size, smp), d = XTexel(t, x0 + 1, y0 + 1, size, smp);
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c2 * (1 - tx) + d * tx) * ty;
}
// The same for cube faces bound as Texture2DArray slices.
float4 XTexelA(Texture2DArray<float4> t, int x, int y, int face, uint2 size, uint4 smp)
{
    x = XAddr(x, (int)size.x, smp.x);
    y = XAddr(y, (int)size.y, smp.y);
    if (x < 0 || y < 0) return smp.w == 1 ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);
    return t.Load(int4(x, y, face, 0));
}
float4 XExactSampleA(Texture2DArray<float4> t, float2 uv, int face, uint2 size, uint4 smp)
{
    if (smp.z == 0) return XTexelA(t, XFloor(uv.x * size.x), XFloor(uv.y * size.y), face, size, smp);
    float fx = clamp(uv.x * size.x - 0.5, -16777216.0, 16777216.0);
    float fy = clamp(uv.y * size.y - 0.5, -16777216.0, 16777216.0);
    int x0 = (int)floor(fx), y0 = (int)floor(fy);
    float tx = fx - x0, ty = fy - y0;
    float4 a = XTexelA(t, x0, y0, face, size, smp), b = XTexelA(t, x0 + 1, y0, face, size, smp);
    float4 c2 = XTexelA(t, x0, y0 + 1, face, size, smp), d = XTexelA(t, x0 + 1, y0 + 1, face, size, smp);
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c2 * (1 - tx) + d * tx) * ty;
}
// Hardware sampling: the slot's sampler wraps at the ALLOCATED size, so when the logical
// size differs (block-compressed padding, partial views of a resolve) wrap in normalised
// space first. Bilinear taps across that seam clamp instead of wrapping (approximation).
float XWrap1(float u, uint mode)
{
    if (mode == 0) return frac(u);
    if (mode == 1) { float m = frac(u * 0.5) * 2.0; return m > 1.0 ? 2.0 - m : m; }
    return u;
}
float2 XNativeUV(float2 uv, uint4 smp, float4 scale)
{
    if (scale.x != 1.0) uv.x = XWrap1(uv.x, smp.x);
    if (scale.y != 1.0) uv.y = XWrap1(uv.y, smp.y);
    return uv * scale.xy;
}
)";
}

namespace detail {

inline std::string Num(uint32_t v) { return std::to_string(v); }

inline std::string Swizzle4(uint32_t swz)
{
    std::string s = ".";
    for (int i = 0; i < 4; ++i) s += "xyzw"[((swz >> (i * 2)) + i) & 3];
    return s;
}

inline bool IsScalarConstOp(uint32_t sop) { return sop >= S_MULSC0 && sop <= S_SUBSC1; }

// Source operands each vector opcode reads (Xenia ucode GetAluVectorOpcodeInfo).
inline int VectorOperandCount(uint32_t vop)
{
    switch (vop)
    {
    case V_FRC: case V_TRUNC: case V_FLOOR: case V_MAX4: return 1;
    case V_MAD: case V_CNDEQ: case V_CNDGE: case V_CNDGT: case V_DP2ADD: return 3;
    default: return 2;
    }
}
// Whether the scalar operation reads src3 as an ordinary operand.
inline bool ScalarReadsSrc3(uint32_t sop)
{
    return sop != S_RETAIN_PREV && sop != S_SETP_CLR && !IsScalarConstOp(sop);
}

// Whether an ALU instruction addresses temporary registers relative to aL (sources it
// actually reads, or a register destination). Scalar-constant ops encode a temp register in
// src3's fields instead, so their src3 is never an aL-relative temp.
inline bool UsesRelativeTemps(const Alu& in)
{
    const int vn = VectorOperandCount(in.vectorOpcode());
    for (int n = 1; n <= 3; ++n)
    {
        const bool read = n <= vn || (n == 3 && ScalarReadsSrc3(in.scalarOpcode()));
        if (read && in.srcSelect(n) && (in.srcRegister(n) & 0x40)) return true;
    }
    if (!in.exportData() && (((in.w0 >> 6) & 1) || in.scalarDestRelative())) return true;
    return false;
}

// One vertex fetch of a translated vertex shader: dwords [index * stride + offset, + dwords)
// relative to fetch-constant `slot`'s base are read (the CPU-side upload range; 0 dwords: a format
// not decoded, nothing read). The rest is how the fetch decodes - all of it reaches the shader per draw
// (VsFetchDescriptors), none of it is compiled in.
struct VsFetch
{
    uint32_t slot, stride; int32_t offset; uint32_t dwords, format = 0;
    bool isSigned = false, numFormatAll = false; int32_t expAdjust = 0; uint32_t dstSwizzle = 0;
};
constexpr uint32_t kMaxVsFetches = 32;
// Guest dwords one vertex fetch of this format reads (0: not decoded - the fetch yields 0, 0, 0, 1).
inline uint32_t VsFetchDwords(uint32_t fmt)
{
    switch (fmt)
    {
    case FMT_32_FLOAT: case FMT_8_8_8_8: case FMT_8: case FMT_5_6_5: case FMT_1_5_5_5: case FMT_4_4_4_4:
    case FMT_8_8: case FMT_16_16: case FMT_16_16_FLOAT: case FMT_2_10_10_10: return 1;
    case FMT_32_32_FLOAT: case FMT_16_16_16_16: case FMT_16_16_16_16_FLOAT:  return 2;
    case FMT_32_32_32_FLOAT:                                                    return 3;
    case FMT_32_32_32_32_FLOAT:                                                 return 4;
    default:                                                                    return 0;
    }
}

class Writer
{
public:
    std::ostringstream out;
    std::set<uint32_t> regs;          // temp registers referenced (individual-variable mode)
    uint32_t textures = 0, cubeSlots = 0, flatSlots = 0;
    std::string unsupported;
    bool exactFilter = false;
    int samplerOwner[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    bool arrayRegs = false;           // relative addressing present: registers are r[64]
    // Vertex shaders: exports go to oPos (62) / oI[0..15], all 512 ALU constants are
    // addressable (AluConst), and vertex fetches read the per-draw vertex data.
    bool vertex = false;
    uint32_t constCount = 256;
    std::vector<VsFetch> fetches;
    uint32_t lastSlot = ~0u, lastStride = 0;   // vfetch_mini state, in program order (as RunShader)
    bool r0Written = false;           // the vertex index register may no longer hold the index
    bool depthExport = false;         // pixel shader writes oDepth (export 61)

    std::string R(uint32_t i)
    {
        i &= 63;
        if (arrayRegs) return "r[" + Num(i) + "]";
        regs.insert(i);
        return "r" + Num(i);
    }
    std::string RRel(uint32_t i) { return "r[(" + Num(i & 63) + " + aL) & 63]"; }
    std::string Temp(uint32_t reg) { return (reg & 0x40) ? RRel(reg) : R(reg); }

    // Constant operand with the const_0 / const_1 relative-addressing rule (ConstIndex).
    std::string Const(const Alu& in, int which, uint32_t reg)
    {
        bool addressed;
        switch (which)
        {
        case 1:  addressed = in.const0Relative(); break;
        case 2:  addressed = in.srcSelect(1) ? in.const0Relative() : in.const1Relative(); break;
        default: addressed = (in.srcSelect(1) && in.srcSelect(2)) ? in.const0Relative() : in.const1Relative(); break;
        }
        if (addressed) return std::string("XC(") + Num(reg) + (in.carRelative() ? " + a0)" : " + aL)");
        return reg < constCount ? "c[" + Num(reg) + "]" : "float4(0, 0, 0, 0)";
    }

    std::string VecOperand(const Alu& in, int n)
    {
        uint32_t reg = in.srcRegister(n), swz = in.srcSwizzle(n);
        bool abs;
        std::string v;
        if (in.srcSelect(n)) { abs = (reg & 0x80) != 0; v = Temp(reg); }
        else { abs = in.absConstants(); v = Const(in, n, reg); }
        std::string s = "(" + v + ")" + Swizzle4(swz);
        if (abs) s = "abs(" + s + ")";
        if (in.srcNegate(n)) s = "(-" + s + ")";
        return s;
    }

    std::string ScalarOperand(const Alu& in, int which)
    {
        uint32_t reg = in.srcRegister(3), swz = in.srcSwizzle(3);
        bool abs;
        std::string v;
        if (in.srcSelect(3)) { abs = (reg & 0x80) != 0; v = Temp(reg); }
        else { abs = in.absConstants(); v = Const(in, 3, reg); }
        int comp = which == 0 ? (int)(((swz >> 6) + 3) & 3) : (int)(swz & 3);
        std::string s = "(" + v + ")." + "xyzw"[comp];
        if (abs) s = "abs(" + s + ")";
        if (in.srcNegate(3)) s = "(-" + s + ")";
        return s;
    }

    void WriteResult(const Alu& in, bool scalarUnit, const std::string& value)
    {
        uint32_t mask = scalarUnit ? in.scalarWriteMask() : in.vectorWriteMask();
        if (in.exportData()) mask &= ~(scalarUnit ? in.vectorWriteMask() : in.scalarWriteMask());
        if (!mask) return;
        bool sat = scalarUnit ? in.scalarSaturate() : in.vectorSaturate();
        uint32_t dst = scalarUnit && !in.exportData() ? in.scalarDest() : in.vectorDest();
        std::string target;
        if (in.exportData())
        {
            target = ExportTarget(dst);
            if (target.empty()) return;
        }
        else
        {
            const bool relative = scalarUnit ? in.scalarDestRelative() : ((in.w0 >> 6) & 1) != 0;
            target = relative ? RRel(dst) : R(dst);
            if (relative || (dst & 63) == 0) r0Written = true;
        }
        for (int i = 0; i < 4; ++i)
        {
            if (!((mask >> i) & 1)) continue;
            std::string comp = value + "." + "xyzw"[i];
            out << "        " << target << "." << "xyzw"[i] << " = "
                << (sat ? "saturate(" + comp + ")" : comp) << ";\n";
        }
    }

    // Where an export lands: pixel shaders observe colour export 0 and the depth (61, X - TTT2's
    // half-resolution scene depth for soft particles is written this way); vertex shaders the
    // position (62, which also marks the vertex valid) and interpolators 0-15.
    // Empty = not observable here (point size, memexport, other render targets).
    std::string ExportTarget(uint32_t dst)
    {
        if (!vertex)
        {
            if (dst == 0) return "e0";
            if (dst != 61) return "";
            depthExport = true;
            out << "        wroteDepth = true;\n";
            return "eDepth";
        }
        if (dst == 62) { out << "        wrotePos = true;\n"; return "oPos"; }
        if (dst < 16) return "oI[" + Num(dst) + "]";
        return "";
    }

    void WriteExportConstants(const Alu& in)
    {
        if (!in.exportData()) return;
        uint32_t one = in.vectorWriteMask() & in.scalarWriteMask();
        uint32_t zero = in.scalarDestRelative()
            ? (~(in.vectorWriteMask() | in.scalarWriteMask()) & 15) : 0;
        if (!(one | zero)) return;
        const std::string target = ExportTarget(in.vectorDest());
        if (target.empty()) return;
        for (unsigned i = 0; i < 4; ++i)
            if ((one | zero) & (1u << i))
                out << "        " << target << "." << "xyzw"[i] << " = " << ((one & (1u << i)) ? "1.0" : "0.0") << ";\n";
    }

    void Alu_(const Alu& in)
    {
        out << "    {\n";
        if (in.isPredicated())
            out << "    if (p0 == " << (in.predCondition() ? "true" : "false") << ") {\n";
        const uint32_t vop = in.vectorOpcode();
        const int vn = VectorOperandCount(vop);
        out << "        float4 va = " << VecOperand(in, 1) << ";\n";
        if (vn >= 2) out << "        float4 vb = " << VecOperand(in, 2) << ";\n";
        if (vn >= 3) out << "        float4 vc = " << VecOperand(in, 3) << ";\n";
        std::string res;
        switch (vop)
        {
        case V_ADD:   res = "va + vb"; break;
        case V_MUL:   res = "va * vb"; break;
        case V_MAX:   res = "max(va, vb)"; break;
        case V_MIN:   res = "min(va, vb)"; break;
        case V_SEQ:   res = "float4(va == vb)"; break;
        case V_SGT:   res = "float4(va > vb)"; break;
        case V_SGE:   res = "float4(va >= vb)"; break;
        case V_SNE:   res = "float4(va != vb)"; break;
        case V_FRC:   res = "va - floor(va)"; break;
        case V_TRUNC: res = "trunc(va)"; break;
        case V_FLOOR: res = "floor(va)"; break;
        case V_MAD:   res = "va * vb + vc"; break;
        case V_CNDEQ: res = "(va == 0.0) ? vb : vc"; break;
        case V_CNDGE: res = "(va >= 0.0) ? vb : vc"; break;
        case V_CNDGT: res = "(va > 0.0) ? vb : vc"; break;
        case V_DP4:   res = "(va.x * vb.x + va.y * vb.y + va.z * vb.z + va.w * vb.w).xxxx"; break;
        case V_DP3:   res = "(va.x * vb.x + va.y * vb.y + va.z * vb.z).xxxx"; break;
        case V_DP2ADD:res = "(va.x * vb.x + va.y * vb.y + vc.x).xxxx"; break;
        case V_MAX4:  res = "max(max(va.x, va.y), max(va.z, va.w)).xxxx"; break;
        case V_DST:   res = "float4(1.0, va.y * vb.y, va.z, vb.w)"; break;
        case V_CUBE:
            // Operand 1 arrives swizzled .zzxy (Xenia kCube): x = [2], y = [3], z = [0].
            out << "        float4 cr;\n"
                   "        { float cx = va.z, cy = va.w, cz = va.x;\n"
                   "          if (abs(cz) >= abs(cx) && abs(cz) >= abs(cy)) cr = float4(-cy, cz < 0.0 ? -cx : cx, 2.0 * cz, cz < 0.0 ? 5.0 : 4.0);\n"
                   "          else if (abs(cy) >= abs(cx)) cr = float4(cy < 0.0 ? -cz : cz, cx, 2.0 * cy, cy < 0.0 ? 3.0 : 2.0);\n"
                   "          else cr = float4(-cy, cx < 0.0 ? cz : -cz, 2.0 * cx, cx < 0.0 ? 1.0 : 0.0); }\n";
            res = "cr";
            break;
        case V_MAXA:
            out << "        a0 = (int)clamp(floor(va.w + 0.5), -256.0, 255.0);\n";
            res = "max(va, vb)";
            break;
        case V_KILL_EQ: case V_KILL_GT: case V_KILL_GE: case V_KILL_NE:
        {
            const char* cmp = vop == V_KILL_EQ ? "==" : vop == V_KILL_GT ? ">" : vop == V_KILL_GE ? ">=" : "!=";
            out << "        bool k = any(va " << cmp << " vb);\n        if (k) killed = true;\n";
            res = "(k ? 1.0 : 0.0).xxxx";
            break;
        }
        case V_SETP_EQ_PUSH: case V_SETP_NE_PUSH: case V_SETP_GT_PUSH: case V_SETP_GE_PUSH:
        {
            const char* cmp = vop == V_SETP_EQ_PUSH ? "==" : vop == V_SETP_NE_PUSH ? "!="
                            : vop == V_SETP_GT_PUSH ? ">" : ">=";
            out << "        p0 = va.w == 0.0 && vb.x " << cmp << " 0.0;\n";
            res = "(p0 ? 0.0 : va.x + 1.0).xxxx";
            break;
        }
        default: break;                       // unknown: no vector result (as on CPU)
        }
        if (!res.empty())
        {
            out << "        float4 vr = " << res << ";\n";
            WriteResult(in, false, "vr");
        }
        WriteExportConstants(in);

        const uint32_t sop = in.scalarOpcode();
        if (sop == S_RETAIN_PREV)
        {
            out << "        float4 sv = ps.xxxx;\n";
            WriteResult(in, true, "sv");
        }
        else
        {
            std::string sr;
            switch (sop)
            {
            case S_ADDS:      sr = "s0 + s1"; break;
            case S_ADDS_PREV: sr = "s0 + ps"; break;
            case S_MULS:      sr = "s0 * s1"; break;
            case S_MULS_PREV: sr = "s0 * ps"; break;
            case S_MAXS:      sr = "max(s0, s1)"; break;
            case S_MINS:      sr = "min(s0, s1)"; break;
            case S_SEQS:      sr = "s0 == 0.0 ? 1.0 : 0.0"; break;
            case S_SGTS:      sr = "s0 > 0.0 ? 1.0 : 0.0"; break;
            case S_SGES:      sr = "s0 >= 0.0 ? 1.0 : 0.0"; break;
            case S_SNES:      sr = "s0 != 0.0 ? 1.0 : 0.0"; break;
            case S_FRCS:      sr = "s0 - floor(s0)"; break;
            case S_TRUNCS:    sr = "trunc(s0)"; break;
            case S_FLOORS:    sr = "floor(s0)"; break;
            case S_EXP:       sr = "exp2(s0)"; break;
            case S_LOG: case S_LOGC: sr = "s0 <= 0.0 ? -3.4e38 : log2(s0)"; break;
            case S_RCP: case S_RCPC: case S_RCPF: sr = "s0 == 0.0 ? 0.0 : 1.0 / s0"; break;
            case S_RSQ: case S_RSQC: case S_RSQF: sr = "s0 <= 0.0 ? 0.0 : 1.0 / sqrt(s0)"; break;
            case S_SQRT:      sr = "s0 <= 0.0 ? 0.0 : sqrt(s0)"; break;
            case S_SUBS:      sr = "s0 - s1"; break;
            case S_SUBS_PREV: sr = "s0 - ps"; break;
            case S_SIN:       sr = "sin(s0)"; break;
            case S_COS:       sr = "cos(s0)"; break;
            case S_MULSC0: case S_MULSC1: sr = "s0 * s1"; break;
            case S_ADDSC0: case S_ADDSC1: sr = "s0 + s1"; break;
            case S_SUBSC0: case S_SUBSC1: sr = "s0 - s1"; break;
            case S_MAXAS: case S_MAXASF: sr = "max(s0, s1)"; break;
            case S_SETP_EQ: case S_SETP_NE: case S_SETP_GT: case S_SETP_GE: case S_SETP_INV:
            case S_SETP_CLR: case S_SETP_RSTR:
            case S_KILLS_EQ: case S_KILLS_GT: case S_KILLS_GE: case S_KILLS_NE: case S_KILLS_ONE:
                sr = "special";
                break;
            default: break;                   // not implemented by the interpreter either
            }
            if (!sr.empty())
            {
                if (IsScalarConstOp(sop))
                {
                    // s0 = constant src3 (W slot), s1 = temp (opcode bit 0 | sel << 1 | swizzle
                    // bits 2-5), X slot; abs_constants and src3 negate apply to both.
                    const uint32_t swz = in.srcSwizzle(3);
                    const uint32_t temp = (sop & 1) | ((in.srcSelect(3) ? 1u : 0u) << 1) | (swz & 0x3C);
                    std::string a = "(" + Const(in, 3, in.srcRegister(3)) + ")." + "xyzw"[((swz >> 6) + 3) & 3];
                    std::string b = "(" + R(temp) + ")." + "xyzw"[swz & 3];
                    if (in.absConstants()) { a = "abs(" + a + ")"; b = "abs(" + b + ")"; }
                    if (in.srcNegate(3)) { a = "(-" + a + ")"; b = "(-" + b + ")"; }
                    out << "        float s0 = " << a << ";\n        float s1 = " << b << ";\n";
                }
                else if (ScalarReadsSrc3(sop))
                {
                    // Mirror the interpreter: scalar operands are read AFTER the vector write.
                    out << "        float s0 = " << ScalarOperand(in, 0) << ";\n";
                    out << "        float s1 = " << ScalarOperand(in, 1) << ";\n";
                }
                switch (sop)
                {
                case S_MAXAS:
                    out << "        a0 = (int)clamp(floor(s0 + 0.5), -256.0, 255.0);\n"; break;
                case S_MAXASF:
                    out << "        a0 = (int)clamp(floor(s0), -256.0, 255.0);\n"; break;
                case S_SETP_EQ: out << "        p0 = s0 == 0.0;\n"; sr = "p0 ? 0.0 : 1.0"; break;
                case S_SETP_NE: out << "        p0 = s0 != 0.0;\n"; sr = "p0 ? 0.0 : 1.0"; break;
                case S_SETP_GT: out << "        p0 = s0 > 0.0;\n";  sr = "p0 ? 0.0 : 1.0"; break;
                case S_SETP_GE: out << "        p0 = s0 >= 0.0;\n"; sr = "p0 ? 0.0 : 1.0"; break;
                case S_SETP_INV:
                    out << "        p0 = s0 == 1.0;\n"; sr = "p0 ? 0.0 : (s0 == 0.0 ? 1.0 : s0)"; break;
                case S_SETP_CLR: out << "        p0 = false;\n"; sr = "-3.4e38"; break;
                case S_SETP_RSTR: out << "        p0 = s0 == 0.0;\n"; sr = "s0"; break;
                case S_KILLS_EQ: case S_KILLS_GT: case S_KILLS_GE: case S_KILLS_NE: case S_KILLS_ONE:
                {
                    const char* k = sop == S_KILLS_EQ ? "s0 == 0.0" : sop == S_KILLS_GT ? "s0 > 0.0"
                                  : sop == S_KILLS_GE ? "s0 >= 0.0" : sop == S_KILLS_NE ? "s0 != 0.0"
                                  : "s0 == 1.0";
                    out << "        bool ks = " << k << ";\n        if (ks) killed = true;\n";
                    sr = "ks ? 1.0 : 0.0";
                    break;
                }
                default: break;
                }
                out << "        ps = " << sr << ";\n";
                out << "        float4 sv = ps.xxxx;\n";
                WriteResult(in, true, "sv");
            }
        }
        if (in.isPredicated()) out << "    }\n";
        out << "    }\n";
    }

    void TFetch_(const TFetch& in)
    {
        if (in.opcode() != 1) return;         // only plain tfetch, as on CPU
        const uint32_t slot = in.constIndex();
        textures |= 1u << slot;
        const bool cube = in.dimension() == 3;
        (cube ? cubeSlots : flatSlots) |= 1u << slot;
        if (cubeSlots & flatSlots) unsupported = "slot " + Num(slot) + " fetched both as 2D and as a cube";
        const std::string src = R(in.srcRegister()), t = "tex" + Num(slot);
        const uint32_t sw = in.srcSwizzle();
        out << "    {\n";
        out << "        float2 uv = float2(" << src << "." << "xyzw"[sw & 3] << ", "
            << src << "." << "xyzw"[(sw >> 2) & 3] << ");\n";
        if (cube)
        {
            // As ExecTextureFetch: cube semantics only when a cube texture is bound (6 slices).
            out << "        bool isCube = texScale[" << slot << "].z > 1.5; int face = 0;\n"
                << "        if (isCube) { uv -= 1.0; face = clamp(XFloor(" << src << "." << "xyzw"[(sw >> 4) & 3]
                << " + 0.5), 0, 5); }\n";
            if (in.coordDenorm()) out << "        else uv /= float2(texInfo[" << slot << "].zw);\n";
        }
        else if (in.coordDenorm()) out << "        uv /= float2(texInfo[" << slot << "].zw);\n";
        // Sampler register slot & 15: two used slots sharing one would need two samplers.
        if (samplerOwner[slot & 15] >= 0 && samplerOwner[slot & 15] != int(slot))
            unsupported = "sampler register collision (slots " + Num(samplerOwner[slot & 15]) + "/" + Num(slot) + ")";
        samplerOwner[slot & 15] = int(slot);
        out << "        uint4 smp = texSampler[" << slot << "];\n";
        // Filter 3 (every TTT2 tfetch) defers to the fetch constant; an explicit override
        // is honoured exactly in the parity path, approximated by the slot sampler otherwise.
        if (in.magFilter() != 3) out << "        smp.z = " << (in.magFilter() == 1 ? 1 : 0) << ";\n";
        if (cube) out << "        if (isCube) { smp.x = 2; smp.y = 2; }\n";
        // Computed LOD (the hardware's own mip selection): SampleGrad with the derivatives of the
        // native coordinate before XNativeUV's wrap (a wrap would put a seam of huge derivatives -
        // the smallest level - along it). Level 0 otherwise, and in vertex shaders.
        const bool grad = in.useComputedLod() && !vertex;
        if (grad && !exactFilter) out << "        float2 xg = uv * texScale[" << slot << "].xy, gx = ddx(xg), gy = ddy(xg);\n";
        const char* op = grad ? ".SampleGrad(xs[" : ".SampleLevel(xs[";
        const char* tail = grad ? ", gx, gy);\n" : ", 0);\n";
        if (exactFilter)
            out << "        float4 f = " << (cube ? "XExactSampleA(" : "XExactSample(") << t << ", uv, "
                << (cube ? "face, " : "") << "texInfo[" << slot << "].zw, smp);\n";
        else if (cube)
            out << "        float4 f = " << t << op << (slot & 15) << "], float3(XNativeUV(uv, smp, texScale["
                << slot << "]), face)" << tail;
        else
            out << "        float4 f = " << t << op << (slot & 15) << "], XNativeUV(uv, smp, texScale["
                << slot << "])" << tail;
        out << "        f = XSwz(XRemap(f, texInfo[" << slot << "].y), texInfo[" << slot << "].x);\n";
        const std::string dst = R(in.dstRegister());
        const uint32_t dsw = in.dstSwizzle();
        for (int i = 0; i < 4; ++i)
        {
            uint32_t s = (dsw >> (i * 3)) & 7;
            if (s < 4) out << "        " << dst << "." << "xyzw"[i] << " = f." << "xyzw"[s] << ";\n";
            else if (s == 4) out << "        " << dst << "." << "xyzw"[i] << " = 0.0;\n";
            else if (s == 5) out << "        " << dst << "." << "xyzw"[i] << " = 1.0;\n";
        }
        out << "    }\n";
    }

    // Vertex fetch, mirroring ExecVertexFetch + UnpackFormat(vertexFetch = true): the index is
    // round(src) clamped at 0, the address base + (index * stride + offset) dwords, and the
    // guest data arrives as host-order dwords (each big-endian guest dword swapped on upload),
    // so GRead16/GRead8 at a dword address are the dword's high half / high byte.
    // The fetch constant, stride, offset, format, signedness, number format, exponent adjust and
    // destination swizzle are not compiled in: the game's D3D patches exactly those fields per vertex
    // layout at run time (MEASURED: a vertex shader in the game files and its run-time copies differ
    // only in the vfetch dwords; 568 run-time copies came from far fewer shaders), so one compiled
    // shader serves every layout and can be built ahead of time from the files. The draw passes them
    // per fetch (vfDesc[i], VsFetchDescriptors - i = the fetch's position in program order); XFetch
    // (VsPrelude) decodes. The HLSL text depends only on fields the game never patches.
    void VFetch_(const VFetch& in)
    {
        if (!vertex) { unsupported = "vertex fetch in pixel shader"; return; }
        // The fetch constant is one of the patched fields: a file's shader carries a placeholder
        // (MEASURED: slot 96 - no fetch constant - in some), so a slot past 95 is not a reason to
        // refuse the shader; that fetch reads nothing (VsFetchDescriptors leaves it invalid). A mini
        // fetch before any full one is a structural problem, though.
        if (in.isMiniFetch() && lastSlot == ~0u) { unsupported = "vertex fetch without a fetch constant"; return; }
        const uint32_t slot = in.isMiniFetch() ? lastSlot : in.constIndex() * 3 + in.constIndexSel();
        uint32_t stride = in.isMiniFetch() ? lastStride : in.stride();
        if (!stride) stride = lastStride;
        if (!in.isMiniFetch()) { lastSlot = slot; lastStride = stride; }
        // The CPU-side upload covers the index range of the draw, so the fetch index must be
        // the vertex index itself (r0.x as the shader received it).
        if (in.srcRegister() != 0 || (in.srcSwizzle() & 3) != 0 || r0Written)
        {
            unsupported = "vertex fetch index is not the vertex index";
            return;
        }
        if (fetches.size() >= kMaxVsFetches) { unsupported = "more than 32 vertex fetches"; return; }
        const uint32_t i = uint32_t(fetches.size());
        VsFetch f{ slot, stride, in.offset(), VsFetchDwords(in.format()), in.format() };
        f.isSigned = in.signedRfMode();
        f.numFormatAll = in.numFormatAll();
        f.expAdjust = in.expAdjust();
        f.dstSwizzle = in.dstSwizzle();
        fetches.push_back(f);
        // vfRegion[slot].w = 0: the slot holds no vertex fetch constant - the CPU returns before
        // touching the destination register.
        const std::string dst = R(in.dstRegister());
        // The value itself was decoded at the top (xf[i], TranslateVertexShader): every fetch reads the
        // untouched vertex index (checked above), so where it runs does not change what it reads.
        out << "    if (XFetchOn(" << i << "u)) {\n"
            << "        float4 f = xf[" << i << "];\n"
            << "        uint sw = vfDesc[" << i << "].w >> 16;\n";
        for (int c = 0; c < 4; ++c)
        {
            const std::string d = dst + "." + "xyzw"[c];
            out << "        { uint s = (sw >> " << (3 * c) << ") & 7; if (s < 6) " << d << " = XSel(f, s); }\n";
        }
        out << "    }\n";
        if ((in.dstRegister() & 63) == 0) r0Written = true;
    }

    // The instructions of one exec block.
    bool Exec(const std::vector<uint32_t>& code, const Cf& cf)
    {
        const uint32_t seq = cf.seq();
        for (uint32_t i = 0; i < cf.count(); ++i)
        {
            const uint32_t idx = cf.addr() + i;
            if ((idx + 1) * 3 > code.size()) break;
            const uint32_t* wd = &code[idx * 3];
            if ((seq >> (i * 2)) & 1)
            {
                if ((wd[0] & 0x1F) == 0) VFetch_(VFetch{ wd[0], wd[1], wd[2] });
                else if (vertex) { unsupported = "texture fetch in vertex shader"; return false; }
                else TFetch_(TFetch{ wd[0], wd[1], wd[2] });
                if (!unsupported.empty()) return false;
            }
            else Alu_(Alu{ wd[0], wd[1], wd[2] });
        }
        return true;
    }

    // Bool-constant or predicate condition of a conditional exec, or "" when unconditional.
    static std::string ExecCondition(const Cf& cf)
    {
        const uint32_t op = cf.op();
        const char* want = cf.cond() ? "true" : "false";
        if (op == CF_COND_EXEC || op == CF_COND_EXEC_END || op == CF_COND_EXEC_PRED_CLEAN ||
            op == CF_COND_EXEC_PRED_CLEAN_END)
            return "XBool(" + Num(cf.boolAddr()) + ") == " + want;
        if (op == CF_COND_EXEC_PRED || op == CF_COND_EXEC_PRED_END)
            return std::string("p0 == ") + want;
        return "";
    }
};

inline Cf CfAt(const std::vector<uint32_t>& code, uint32_t index)
{
    Cf pair[2];
    DecodeCfPair(&code[(index / 2) * 3], pair[0], pair[1]);
    return pair[index & 1];
}

} // namespace detail

// The control-flow program of non-empty microcode into w.out (shared by both shader types).
// branching: the program-counter form was used (no break-based wrapper).
inline void EmitProgram(const std::vector<uint32_t>& code, detail::Writer& w, bool& branching, std::string& unsupported)
{
    branching = false;
    if (code.size() < 3 || code.size() > 8192)
        unsupported = "invalid shader length";
    else
    {
        // CF block ends at the first instruction any EXEC references (as RunShader).
        uint32_t sizeBytes = (uint32_t)code.size() * 4, cfBytes = sizeBytes;
        for (uint32_t off = 0; off + 12 <= sizeBytes && off < cfBytes; off += 12)
        {
            Cf a, b;
            DecodeCfPair(&code[off / 4], a, b);
            for (const Cf* p : { &a, &b })
                if (p->isExec() && p->addr()) cfBytes = std::min(cfBytes, p->addr() * 12);
        }
        const uint32_t cfCount = cfBytes / 12 * 2;
        // Pre-scan: branching control flow and relative register addressing.
        branching = false;
        for (uint32_t k = 0; k < cfCount; ++k)
        {
            const Cf cf = detail::CfAt(code, k);
            const uint32_t op = cf.op();
            if (op == CF_LOOP_START || op == CF_LOOP_END || op == CF_COND_CALL || op == CF_RETURN || op == CF_COND_JMP)
                branching = true;
            if (!cf.isExec()) continue;
            for (uint32_t i = 0; i < cf.count(); ++i)
            {
                const uint32_t idx = cf.addr() + i;
                if ((idx + 1) * 3 > code.size()) break;
                if (!((cf.seq() >> (i * 2)) & 1) &&
                    detail::UsesRelativeTemps(Alu{ code[idx * 3], code[idx * 3 + 1], code[idx * 3 + 2] }))
                    w.arrayRegs = true;
            }
        }

        if (!branching)
        {
            // Structured form: execs in order; a conditional END ends only when taken.
            for (uint32_t k = 0; k < cfCount; ++k)
            {
                const Cf cf = detail::CfAt(code, k);
                if (!cf.isExec()) continue;
                const std::string cond = detail::Writer::ExecCondition(cf);
                if (!cond.empty()) w.out << "    if (" << cond << ") {\n";
                if (!w.Exec(code, cf)) break;
                if (cf.isEnd()) w.out << "    break;\n";
                if (!cond.empty()) w.out << "    }\n";
                if (cf.isEnd() && cond.empty()) break;     // nothing after an unconditional end runs
            }
        }
        else
        {
            // Program-counter form (loops / calls / jumps), same semantics as RunShader.
            const std::string end = detail::Num(cfCount);
            w.out << "    uint xpc = 0; uint loopDepth = 0; uint callDepth = 0;\n"
                     "    int loopCount[4] = { 0, 0, 0, 0 }; int loopAL[4] = { 0, 0, 0, 0 }; uint callStack[4] = { 0, 0, 0, 0 };\n"
                     "    uint steps = 0;\n"
                     "    [loop] while (xpc < " << end << " && steps <= 65536) {\n"
                     "    ++steps;\n";
            for (uint32_t k = 0; k < cfCount && w.unsupported.empty(); ++k)
            {
                const Cf cf = detail::CfAt(code, k);
                const uint32_t op = cf.op();
                const std::string next = detail::Num(k + 1), target = detail::Num(cf.lo & 0x1FFF);
                w.out << "    " << (k ? "else " : "") << "if (xpc == " << k << ") {\n";
                if (cf.isExec())
                {
                    const std::string cond = detail::Writer::ExecCondition(cf);
                    w.out << "    xpc = " << next << ";\n";
                    w.out << "    if (" << (cond.empty() ? std::string("true") : cond) << ") {\n";
                    if (!w.Exec(code, cf)) break;
                    if (cf.isEnd()) w.out << "    xpc = " << end << ";\n";
                    w.out << "    }\n";
                }
                else if (op == CF_LOOP_START)
                {
                    const std::string id = detail::Num((cf.lo >> 16) & 0x1F);
                    w.out << "    uint lc = XLoop(" << id << "); int cnt = (int)(lc & 0xFF);\n"
                             "    if (loopDepth == 4 || cnt == 0) xpc = " << target << ";\n"
                             "    else { loopCount[loopDepth] = cnt; loopAL[loopDepth] = aL; ++loopDepth;\n";
                    if (!((cf.lo >> 13) & 1)) w.out << "      aL = (int)((lc >> 8) & 0xFF);\n";
                    w.out << "      xpc = " << next << "; }\n";
                }
                else if (op == CF_LOOP_END)
                {
                    const std::string id = detail::Num((cf.lo >> 16) & 0x1F);
                    const bool predBreak = (cf.lo >> 21) & 1;
                    w.out << "    if (loopDepth == 0) xpc = " << next << ";\n"
                             "    else { uint lc = XLoop(" << id << "); aL += ((int)(lc << 8)) >> 24;\n"
                             "      loopCount[loopDepth - 1] -= 1;\n"
                             "      bool brk = " << (predBreak ? std::string("p0 == ") + (cf.cond() ? "true" : "false") : "false") << ";\n"
                             "      if (loopCount[loopDepth - 1] > 0 && !brk) xpc = " << target << ";\n"
                             "      else { aL = loopAL[loopDepth - 1]; --loopDepth; xpc = " << next << "; } }\n";
                }
                else if (op == CF_COND_CALL || op == CF_COND_JMP)
                {
                    const bool unconditional = (cf.lo >> 13) & 1, predicated = (cf.lo >> 14) & 1;
                    const char* want = cf.cond() ? "true" : "false";
                    const std::string take = unconditional ? "true" : predicated ? std::string("p0 == ") + want
                        : "XBool(" + detail::Num(cf.boolAddr()) + ") == " + want;
                    if (op == CF_COND_CALL)
                        w.out << "    if ((" << take << ") && callDepth < 4) { callStack[callDepth] = " << next
                              << "; ++callDepth; xpc = " << target << "; } else xpc = " << next << ";\n";
                    else
                        w.out << "    xpc = (" << take << ") ? " << target << " : " << next << ";\n";
                }
                else if (op == CF_RETURN)
                    w.out << "    if (callDepth > 0) { --callDepth; xpc = callStack[callDepth]; } else xpc = " << next << ";\n";
                else
                    w.out << "    xpc = " << next << ";\n";
                w.out << "    }\n";
            }
            w.out << "    else xpc = " << end << ";\n    }\n";
        }
        if (!w.unsupported.empty()) unsupported = w.unsupported;
    }
}

// Translate pixel-shader microcode (host-order dwords). Empty code exports r0, as on CPU.
// exactFilter: emulate the interpreter's filtering arithmetic with Load (parity tests only).
inline PsTranslation TranslatePixelShader(const std::vector<uint32_t>& code, bool exactFilter = false)
{
    PsTranslation result;
    detail::Writer w;
    w.exactFilter = exactFilter;
    bool branching = false;            // program-counter form (no break-based wrapper)
    if (code.empty())
        w.out << "    e0 = " << w.R(0) << ";\n";
    else
        EmitProgram(code, w, branching, result.unsupported);
    if (!result.unsupported.empty()) return result;

    std::ostringstream f;
    for (uint32_t slot = 0; slot < 32; ++slot)
        if (w.textures & (1u << slot))
            f << ((w.cubeSlots >> slot) & 1 ? "Texture2DArray<float4> tex" : "Texture2D<float4> tex") << slot
              << " : register(t" << slot << ");\n";
    // Only depth-writing shaders declare these, so every other shader's HLSL (and its disk
    // cache entry) is unchanged.
    if (w.depthExport)
        f << "static float4 eDepth = float4(0, 0, 0, 0);\nstatic bool wroteDepth = false;\n";
    f << "XOut XenosPs(float4 inputs[16])\n{\n";
    if (w.arrayRegs)
        f << "    float4 r[64];\n"
             "    [unroll] for (int ri = 0; ri < 16; ++ri) r[ri] = inputs[ri];\n"
             "    [unroll] for (int rz = 16; rz < 64; ++rz) r[rz] = float4(0, 0, 0, 0);\n";
    else
        for (uint32_t r : w.regs)
            f << "    float4 r" << r << " = " << (r < 16 ? "inputs[" + detail::Num(r) + "]" : std::string("float4(0, 0, 0, 0)")) << ";\n";
    f << "    float4 e0 = float4(0, 0, 0, 0);\n"
         "    int a0 = 0; int aL = 0; bool p0 = false; float ps = 0.0; bool killed = false;\n"
      << (branching ? "    {\n" : "    [loop] do {\n")
      << w.out.str()
      << (branching ? "    }\n" : "    } while (false);\n")
      << "    XOut o; o.color = e0; o.killed = killed; return o;\n}\n";
    result.function = f.str();
    result.textureSlots = w.textures;
    result.cubeSlots = w.cubeSlots;
    result.writesDepth = w.depthExport;
    return result;
}

// Complete pixel shader: translated body + kill + alpha test (RB_COLORCONTROL semantics). A
// depth-exporting shader also outputs SV_Depth: the exported X clamped to 0..1 like Xenia (host
// depth holds the guest's window z), or the rasterized depth on paths that never export it.
inline std::string PixelShaderHlsl(const PsTranslation& t)
{
    return std::string(PsPrelude()) + t.function + R"(
struct PSIn { float4 pos : SV_Position; float4 a[16] : TEXCOORD0; };
)" + (t.writesDepth ? "float4 main(PSIn input, out float oDepth : SV_Depth) : SV_Target" : "float4 main(PSIn input) : SV_Target") + R"(
{
    XOut o = XenosPs(input.a);)" + (t.writesDepth ? "\n    oDepth = wroteDepth ? saturate(eDepth.x) : input.pos.z;" : "") + R"(
    if (o.killed) discard;
    if (alphaTest != 0)
    {
        float a = o.color.a; bool ok;            // ("pass" is reserved in HLSL)
        switch (alphaFunc)
        {
        case 0: ok = false; break;             case 1: ok = a < alphaRef; break;
        case 2: ok = a == alphaRef; break;     case 3: ok = a <= alphaRef; break;
        case 4: ok = a > alphaRef; break;      case 5: ok = a != alphaRef; break;
        case 6: ok = a >= alphaRef; break;     default: ok = true; break;
        }
        if (!ok) discard;
    }
    return o.color;
}
)";
}

// ---------------------------------------------------------------------------------------
// Vertex shaders. The CPU interpreter's vertex stage (GpuExecuteDraw's shadeVertex + the
// backend's window-to-clip conversion) moves to the GPU: same ALU/flow translation, vertex
// fetches read the draw's guest vertex data from a raw buffer (t0), and the PA_CL_VTE
// viewport transform runs in the shader.
//   b0 = 512 ALU constants (a vertex shader can address all of them, AluConst)
//   b1 = bool/loop constants, per fetch-constant-slot data regions, viewport/surface state
// ---------------------------------------------------------------------------------------
struct VsTranslation
{
    std::string function;                 // HLSL: XVs XenosVs(uint index)
    std::vector<detail::VsFetch> fetches; // dword ranges the draw must upload
    std::string unsupported;
};

// Per-draw vertex-stage state (b1). vfRegion[slot] = { vdata dword of guest dword 0 of the
// slot, first and one-past-last guest dword uploaded } (dwords relative to the slot's base).
struct VsStateCB
{
    uint32_t boolConsts[8];
    uint32_t loopConsts[32];
    int32_t vfRegion[96][4];
    uint32_t vte; float xs, xo, ys;
    float yo, zs, zo, pitch;
    float rows; uint32_t noClip, pad[2];
    uint32_t vfDesc[detail::kMaxVsFetches][4];   // the fetches' parameters (VsFetchDescriptors)
};

// The draw's per-fetch parameters for a translated vertex shader (its fetches in program order).
inline void VsFetchDescriptors(const VsTranslation& t, uint32_t desc[detail::kMaxVsFetches][4])
{
    for (uint32_t i = 0; i < detail::kMaxVsFetches; ++i)
    {
        desc[i][0] = desc[i][1] = desc[i][2] = desc[i][3] = 0;
        if (i >= t.fetches.size() || t.fetches[i].slot >= 96) continue;   // no fetch constant: no fetch
        const detail::VsFetch& f = t.fetches[i];
        desc[i][0] = f.slot;
        desc[i][1] = f.stride;
        desc[i][2] = uint32_t(f.offset);
        desc[i][3] = (f.format & 63) | (f.isSigned ? 1u << 6 : 0u) | (f.numFormatAll ? 1u << 7 : 0u) |
                     (uint32_t(f.expAdjust) & 63) << 8 | (f.dstSwizzle & 0xFFF) << 16 | 1u << 31;
    }
}

// vfDesc[i] = fetch i's { slot, stride, offset, format | signed << 6 | number format << 7 |
// exponent adjust (6 bits) << 8 | destination swizzle << 16 | 1 << 31 } (VsFetchDescriptors).
inline std::string VsFetchHlsl()
{
    auto C = [](uint32_t fmt, const char* body) { return "    case " + detail::Num(fmt) + "u: " + body + " break;\n"; };
    return std::string(R"(
bool XFetchOn(uint i) { uint4 d = vfDesc[i]; return (d.w >> 31) != 0 && vfRegion[d.x].w != 0; }
// Destination swizzle component s (0-3 x..w, 4 = 0, 5 = 1), chosen without indexing the vector.
float XSel(float4 f, uint s) { return s == 0 ? f.x : s == 1 ? f.y : s == 2 ? f.z : s == 3 ? f.w : s == 4 ? 0.0 : 1.0; }
float4 XFetch(uint i, float index)
{
    uint4 d = vfDesc[i];
    uint slot = d.x;
    uint vi = (uint)max(0.0, floor(index + 0.5));
    int fa = (int)(vi * d.y) + (int)d.z;
    bool s = ((d.w >> 6) & 1) != 0;
    bool all = ((d.w >> 7) & 1) != 0;
    int ea = ((int)(d.w << 18)) >> 26;
    uint d0 = XVd(slot, fa);
    float4 f = float4(0, 0, 0, 1);
    [branch] switch (d.w & 63)
    {
)") +
        C(FMT_32_FLOAT, "f = float4(asfloat(d0), 0, 0, 1);") +
        C(FMT_32_32_FLOAT, "f = float4(asfloat(d0), asfloat(XVd(slot, fa + 1)), 0, 1);") +
        C(FMT_32_32_32_FLOAT, "f = float4(asfloat(d0), asfloat(XVd(slot, fa + 1)), asfloat(XVd(slot, fa + 2)), 1);") +
        C(FMT_32_32_32_32_FLOAT, "f = float4(asfloat(d0), asfloat(XVd(slot, fa + 1)), asfloat(XVd(slot, fa + 2)), asfloat(XVd(slot, fa + 3)));") +
        C(FMT_8_8_8_8, "f = float4(XN8(d0, s), XN8(d0 >> 8, s), XN8(d0 >> 16, s), XN8(d0 >> 24, s)); if (all) f *= 255.0;") +
        C(FMT_8, "f = float4(XN8(d0 >> 24, s).xxx, 1);") +
        C(FMT_5_6_5, "f = float4(((d0 >> 27) & 31) / 31.0, ((d0 >> 21) & 63) / 63.0, ((d0 >> 16) & 31) / 31.0, 1);") +
        C(FMT_1_5_5_5, "f = float4(((d0 >> 26) & 31) / 31.0, ((d0 >> 21) & 31) / 31.0, ((d0 >> 16) & 31) / 31.0, (d0 >> 31) ? 1.0 : 0.0);") +
        C(FMT_4_4_4_4, "f = float4(((d0 >> 24) & 15) / 15.0, ((d0 >> 20) & 15) / 15.0, ((d0 >> 16) & 15) / 15.0, ((d0 >> 28) & 15) / 15.0);") +
        C(FMT_8_8, "f = float4(XN8(d0 >> 16, s), XN8(d0 >> 24, s), 0, 1);") +
        C(FMT_16_16, "f = float4(XN16(d0, s), XN16(d0 >> 16, s), 0, 1); if (all) f *= float4(65535.0, 65535.0, 1.0, 1.0);") +
        C(FMT_16_16_16_16, "{ uint d1 = XVd(slot, fa + 1); f = float4(XN16(d0, s), XN16(d0 >> 16, s), XN16(d1, s), XN16(d1 >> 16, s)); }") +
        C(FMT_16_16_FLOAT, "f = float4(f16tof32(d0 & 0xFFFF), f16tof32(d0 >> 16), 0, 1);") +
        C(FMT_16_16_16_16_FLOAT, "{ uint d1 = XVd(slot, fa + 1); f = float4(f16tof32(d0 & 0xFFFF), f16tof32(d0 >> 16), f16tof32(d1 & 0xFFFF), f16tof32(d1 >> 16)); }") +
        C(FMT_2_10_10_10, "f = float4((d0 & 0x3FF) / 1023.0, ((d0 >> 10) & 0x3FF) / 1023.0, ((d0 >> 20) & 0x3FF) / 1023.0, ((d0 >> 30) & 3) / 3.0);") +
        R"(    default: break;
    }
    if (ea != 0) f *= exp2((float)ea);
    return f;
}
)";
}

inline const char* VsPrelude()
{
    static const std::string text = std::string(R"(
cbuffer XenosVsConstants : register(b0) { float4 c[512]; };
cbuffer XenosVsState : register(b1)
{
    uint4 boolConsts[2];
    uint4 loopConsts[8];
    int4 vfRegion[96];
    uint vte; float vpXs; float vpXo; float vpYs;
    float vpYo; float vpZs; float vpZo; float surfPitch;
    float surfRows; uint noClip; uint2 vsPad;
    uint4 vfDesc[32];
};
ByteAddressBuffer vdata : register(t0);
float4 XC(int i) { return (uint)i < 512u ? c[(uint)i] : float4(0, 0, 0, 0); }
bool XBool(uint i) { return ((boolConsts[i >> 7][(i >> 5) & 3] >> (i & 31)) & 1) != 0; }
uint XLoop(uint i) { return loopConsts[i >> 2][i & 3]; }
uint XVd(uint slot, int dw)
{
    int4 r = vfRegion[slot];
    if (dw < r.y || dw >= r.z) return 0;
    return vdata.Load((uint)(r.x + dw) * 4);
}
float XN8(uint u, bool s)  { u &= 0xFF;   return s ? max(-1.0, (float)((int)(u << 24) >> 24) / 127.0) : (float)u / 255.0; }
float XN16(uint u, bool s) { u &= 0xFFFF; return s ? max(-1.0, (float)((int)(u << 16) >> 16) / 32767.0) : (float)u / 65535.0; }
struct XVs { float4 pos; float4 o[16]; bool wrotePos; };
)") + VsFetchHlsl();
    return text.c_str();
}

// Translate vertex-shader microcode (host-order dwords). r0.x holds the vertex index.
inline VsTranslation TranslateVertexShader(const std::vector<uint32_t>& code)
{
    VsTranslation result;
    detail::Writer w;
    w.vertex = true;
    w.constCount = 512;
    bool branching = false;
    if (code.empty()) result.unsupported = "empty vertex shader";
    else EmitProgram(code, w, branching, result.unsupported);
    // With loops/calls the program order is not the execution order: a register-0 write could
    // precede a fetch at run time. Only straight-line shaders get the index-range guarantee.
    if (result.unsupported.empty() && branching && w.r0Written &&
        std::any_of(w.fetches.begin(), w.fetches.end(), [](const detail::VsFetch& f) { return f.dwords != 0; }))
        result.unsupported = "vertex fetch after a possible index-register write (branching)";
    if (!result.unsupported.empty()) return result;

    std::ostringstream f;
    f << "XVs XenosVs(uint index)\n{\n";
    if (w.arrayRegs)
        f << "    float4 r[64];\n"
             "    [unroll] for (int rz = 0; rz < 64; ++rz) r[rz] = float4(0, 0, 0, 0);\n"
             "    r[0].x = (float)index;\n";
    else
        for (uint32_t r : w.regs)
            f << "    float4 r" << r << " = " << (r == 0 ? std::string("float4((float)index, 0, 0, 0)") : std::string("float4(0, 0, 0, 0)")) << ";\n";
    // The vertex fetches, decoded in one loop (one copy of XFetch's format switch: D3DCompile spent
    // ~0.94 s per vertex shader with one per fetch - MEASURED, fxc_bench.py).
    if (!w.fetches.empty())
        f << "    float4 xf[" << w.fetches.size() << "];\n"
          << "    [loop] for (uint xfi = 0; xfi < " << w.fetches.size() << "u; ++xfi) xf[xfi] = XFetch(xfi, (float)index);\n";
    f << "    float4 oPos = float4(0, 0, 0, 0); float4 oI[16];\n"
         "    [unroll] for (int oz = 0; oz < 16; ++oz) oI[oz] = float4(0, 0, 0, 0);\n"
         "    bool wrotePos = false;\n"
         "    int a0 = 0; int aL = 0; bool p0 = false; float ps = 0.0; bool killed = false;\n"
      << (branching ? "    {\n" : "    [loop] do {\n")
      << w.out.str()
      << (branching ? "    }\n" : "    } while (false);\n")
      << "    XVs o; o.pos = oPos; o.wrotePos = wrotePos;\n"
         "    [unroll] for (int ok = 0; ok < 16; ++ok) o.o[ok] = oI[ok];\n"
         "    return o;\n}\n";
    result.function = f.str();
    result.fetches = std::move(w.fetches);
    return result;
}

// Vertex output exactly as the CPU path delivers it to the rasterizer: shadeVertex's
// PA_CL_VTE_CNTL transform to window space, then the backend's window-to-clip conversion
// for the sample-space viewport (Backend::Draw). Attributes reach the pixel shader as the
// exported values (the CPU stores them x 1/W and the backend multiplies W back). A vertex
// without a position export, or with 1/W = 0, never forms a triangle on the CPU: NaN drops it.
inline const char* VsMainHlsl()
{
    return R"(
struct VSOut { float4 pos : SV_Position; float4 a[16] : TEXCOORD0; };
VSOut XVsToClip(XVs x)
{
    VSOut o;
    float4 p = x.pos;
    float inv = (vte & 0x400) ? (p.w == 0.0 ? 1.0 : 1.0 / p.w) : p.w;
    float px = p.x, py = p.y, pz = p.z;
    if (!(vte & 0x100)) { px *= inv; py *= inv; }
    if (!(vte & 0x200)) pz *= inv;
    if (vte & 0x01) px *= vpXs;
    if (vte & 0x02) px += vpXo;
    if (vte & 0x04) py *= vpYs;
    if (vte & 0x08) py += vpYo;
    if (vte & 0x10) pz *= vpZs;
    if (vte & 0x20) pz += vpZo;
    float w = 1.0 / inv;
    float wc = (w < 0.0 && noClip != 0) ? -w : w;
    o.pos = float4((px / surfPitch * 2.0 - 1.0) * wc, (1.0 - py / surfRows * 2.0) * wc, pz * wc, wc);
    if (!x.wrotePos || inv == 0.0) o.pos = asfloat(0x7FC00000u).xxxx;
    [unroll] for (int k = 0; k < 16; ++k) o.a[k] = x.o[k];
    return o;
}
VSOut main(uint vid : SV_VertexID) { return XVsToClip(XenosVs(vid)); }
)";
}

inline std::string VertexShaderHlsl(const VsTranslation& t)
{
    return std::string(VsPrelude()) + t.function + VsMainHlsl();
}

// Compute kernel for parity tests: item i runs vertex index i; out[i * 18 + 0] = position as
// the CPU computes it (window space x, y, z and 1/W), [1..16] = interpolators, [17].x = valid.
inline std::string VsParityComputeHlsl(const VsTranslation& t)
{
    return std::string(VsPrelude()) + t.function + R"(
RWStructuredBuffer<float4> parityOut : register(u0);
[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    XVs x = XenosVs(id);
    float4 p = x.pos;
    float inv = (vte & 0x400) ? (p.w == 0.0 ? 1.0 : 1.0 / p.w) : p.w;
    float px = p.x, py = p.y, pz = p.z;
    if (!(vte & 0x100)) { px *= inv; py *= inv; }
    if (!(vte & 0x200)) pz *= inv;
    if (vte & 0x01) px *= vpXs;
    if (vte & 0x02) px += vpXo;
    if (vte & 0x04) py *= vpYs;
    if (vte & 0x08) py += vpYo;
    if (vte & 0x10) pz *= vpZs;
    if (vte & 0x20) pz += vpZo;
    parityOut[id * 18] = float4(px, py, pz, inv);
    [unroll] for (int k = 0; k < 16; ++k) parityOut[id * 18 + 1 + k] = x.o[k];
    parityOut[id * 18 + 17] = float4(x.wrotePos ? 1.0 : 0.0, 0, 0, 0);
}
)";
}

// Guest dword range [lo, hi) per fetch-constant slot read by vertex indices minIndex..maxIndex.
inline void VsFetchRanges(const VsTranslation& t, uint32_t minIndex, uint32_t maxIndex,
                          int64_t lo[96], int64_t hi[96], bool used[96])
{
    for (uint32_t s = 0; s < 96; ++s) used[s] = false;
    for (const detail::VsFetch& f : t.fetches)
    {
        if (!f.dwords) continue;                    // a format not decoded reads nothing
        const int64_t a = int64_t(minIndex) * f.stride + f.offset, b = int64_t(maxIndex) * f.stride + f.offset + f.dwords;
        if (f.slot >= 96) continue;                 // no fetch constant: reads nothing
        if (!used[f.slot]) { lo[f.slot] = a; hi[f.slot] = b; used[f.slot] = true; }
        else { lo[f.slot] = (std::min)(lo[f.slot], a); hi[f.slot] = (std::max)(hi[f.slot], b); }
    }
}

// Compute kernel for parity tests: inputs[16] per item in t32, outputs (color, killed) in u0.
inline std::string ParityComputeHlsl(const PsTranslation& t)
{
    // Compute shaders have no screen-space derivatives: zero gradients sample level 0 (the
    // interpreter's sampling; the harness binds single-level textures anyway).
    return std::string("#define ddx(v) ((v) * 0.0)\n#define ddy(v) ((v) * 0.0)\n") + PsPrelude() + t.function + R"(
StructuredBuffer<float4> parityIn : register(t32);
RWStructuredBuffer<float4> parityOut : register(u0);
[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    float4 inputs[16];
    [unroll] for (int k = 0; k < 16; ++k) inputs[k] = parityIn[id * 16 + k];
    XOut o = XenosPs(inputs);
    parityOut[id * 2] = o.color;
    parityOut[id * 2 + 1] = float4(o.killed ? 1.0 : 0.0, 0, 0, 0);
}
)";
}

} // namespace ttt2_hw
