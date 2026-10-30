// CPU-side Xenos draw execution: microcode interpreter + rectangle rasterizer.
//
// Encodings come from tools/XenosRecomp/XenosRecomp/shader_code.h (the shipped Xenos ->
// HLSL translator, so its bitfields are ground truth) and were cross-checked against the
// game's own packets: the front-buffer texture fetch constant 8A000002 A44A9006 0059E4FF
// decodes with this layout to "tiled 8_8_8_8 1280x720 at 0xA44A9000", which is exactly the
// surface the present path shows.
#include "kernel_shared.h"
#include "gpu_render.h"
#include <atomic>
#include <emmintrin.h>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
// windows.h only for the SEH guard + GetCurrentThreadId; NOMINMAX or its min/max macros
// clobber std::min/std::max all over this file (the classic session-8 build breaker).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

// Frame inventory sink (kernel_video sets it while a framelog_<n>.txt is open; CP thread only).
void (*g_gpuFrameNote)(const char*) = nullptr;
// diag_hooks.cpp: which CPU skinning job wrote a vertex (frame-log sliver corners).
bool DiagSkinDescribe(uint32_t addr, char* out, size_t size);

namespace
{

// --------------------------------------------------------------------------------------
// small helpers
// --------------------------------------------------------------------------------------
struct V4
{
    float c[4] = { 0, 0, 0, 0 };
    float& operator[](int i) { return c[i]; }
    float operator[](int i) const { return c[i]; }
};

uint32_t PhysToGuest(uint32_t addr)
{
    return (addr && addr < 0x20000000u) ? (addr | 0xA0000000u) : addr;
}

float AsFloat(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

int GpuLogBudget()
{
    static int budget = [] {
        const char* s = std::getenv("TTT2_RENDERLOG");
        return s ? (int)std::strtol(s, nullptr, 0) : 24;
    }();
    return budget;
}
std::atomic<int> g_logged{ 0 };          // bumped from raster workers as well as the CP thread
bool LogOn() { return g_logged.load(std::memory_order_relaxed) < GpuLogBudget(); }
void LogUsed() { g_logged.fetch_add(1, std::memory_order_relaxed); }

// A global budget lets the noisiest primitive type crowd out every other one - the rectangle
// clears alone are hundreds per run. Give each primitive type its own small allowance so one
// run shows what EVERY kind of draw is doing.
int g_loggedPrim[64];
std::atomic<uint64_t> g_litDraws{ 0 };   // draws that produced at least one non-black pixel
int PrimLogBudget()
{
    static int n = [] {
        const char* e = std::getenv("TTT2_PRIMLOG");
        return e ? (int)std::strtol(e, nullptr, 0) : 6;
    }();
    return n;
}
bool LogPrimOn(uint32_t prim) { return prim < 64 && g_loggedPrim[prim] < PrimLogBudget(); }
void LogPrimUsed(uint32_t prim) { if (prim < 64) ++g_loggedPrim[prim]; }

// --------------------------------------------------------------------------------------
// raster worker pool
// --------------------------------------------------------------------------------------
// The rasterizer runs on the command-processor thread and interprets the pixel shader PER
// PIXEL; a full-screen pass is 921,600 invocations. Rows of one triangle never touch the same
// sample (row y owns sample rows y*sampY .. y*sampY+sampY-1 exclusively), so the pixel loop
// fans out across rows while triangles keep running strictly in submission order - which is
// what preserves blending order between overlapping primitives.
//
// Jobs are strictly serialized. A worker JOINS a job only while it is open (both checked and
// counted under m_m); ForRows closes the job after its own drain and then waits only for the
// workers that actually joined. It used to wake every worker for every job and wait until
// all of them had woken and checked in, so each large triangle paid the slowest thread's
// wake-up latency even when the caller had already shaded every row; measured 2026-10-10,
// 14 idle workers burned ~13% of a core each on that churn. The pool is leaked on purpose -
// joining infinitely-looping workers from a static destructor would hang exit.
class RasterPool
{
public:
    static RasterPool& Get()
    {
        static RasterPool* p = new RasterPool;
        return *p;
    }

    unsigned Workers() const { return m_count; }

    // Run fn(y) for every y in [y0, y1); returns when all rows are done. The calling thread
    // works too, so a pool of N gives N+1 lanes.
    void ForRows(int y0, int y1, const std::function<void(int)>& fn)
    {
        {
            std::lock_guard<std::mutex> lk(m_m);
            m_fn = &fn;
            m_cursor.store(y0, std::memory_order_relaxed);
            m_end = y1;
            m_open = true;
            ++m_gen;
        }
        // Wake only as many helpers as there are chunks beyond the caller's own first one.
        const int chunks = (y1 - y0 + RowsPerChunk - 1) / RowsPerChunk;
        const unsigned helpers = chunks > 1 ? std::min<unsigned>(m_count, unsigned(chunks - 1)) : 0;
        if (helpers == m_count) m_cv.notify_all();
        else for (unsigned i = 0; i < helpers; ++i) m_cv.notify_one();
        DrainGuarded();
        std::unique_lock<std::mutex> lk(m_m);
        m_open = false;                          // late wakers no longer join this job
        m_doneCv.wait(lk, [&] { return m_active == 0; });
        m_fn = nullptr;
    }

private:
    RasterPool()
    {
        unsigned n = std::thread::hardware_concurrency();
        m_count = n > 3 ? n - 2 : 1;             // leave lanes for the game's own threads
        for (unsigned i = 0; i < m_count; ++i)
            std::thread([this] { Worker(); }).detach();
    }

    static constexpr int RowsPerChunk = 4;

    void Drain()
    {
        const std::function<void(int)>* fn = m_fn;
        for (;;)
        {
            int y = m_cursor.fetch_add(RowsPerChunk, std::memory_order_relaxed);
            if (y >= m_end) break;
            int e = y + RowsPerChunk < m_end ? y + RowsPerChunk : m_end;
            for (int i = y; i < e; ++i) (*fn)(i);
        }
    }

    // SEH net around the row work: a fault in a worker used to kill the whole process with
    // no log at all (run_user1 died silently mid logo fade). Log-and-drop-the-rows beats
    // vanishing; the CP thread's own guards handle everything else.
    void DrainGuarded()
    {
        __try { Drain(); }
        __except (GuestFaultReport(GetExceptionInformation()))
        {
            std::fputs("[raster] worker faulted; dropping its remaining rows this job\n", stderr);
        }
    }

    void Worker()
    {
        uint64_t seen = 0;
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lk(m_m);
                m_cv.wait(lk, [&] { return m_open && m_gen != seen; });
                seen = m_gen;
                ++m_active;                      // joined: ForRows waits for us now
            }
            DrainGuarded();
            std::lock_guard<std::mutex> lk(m_m);
            if (--m_active == 0) m_doneCv.notify_one();
        }
    }

    unsigned m_count = 1;
    std::mutex m_m;
    std::condition_variable m_cv, m_doneCv;
    uint64_t m_gen = 0;                          // guarded by m_m
    bool m_open = false;                         // guarded by m_m
    int m_active = 0;                            // guarded by m_m: workers inside this job
    const std::function<void(int)>* m_fn = nullptr;
    std::atomic<int> m_cursor{ 0 };
    int m_end = 0;
};

// --------------------------------------------------------------------------------------
// register file accessors (Xenos keeps shader constants in the same file as context regs)
// --------------------------------------------------------------------------------------
constexpr uint32_t REG_ALU_CONST   = 0x4000;   // 512 x float4
constexpr uint32_t REG_FETCH_CONST = 0x4800;   // 32 x 6 dwords == 96 x 2 dwords
constexpr uint32_t REG_BOOL_CONST  = 0x4900;   // 8 dwords == 256 bools
constexpr uint32_t REG_LOOP_CONST  = 0x4908;   // 32 dwords

// The 512-entry float4 ALU constant file is split between the stages: a vertex shader's
// c0..c255 are entries 0..255 and a pixel shader's c0.. start at entry 256
// (XenosRecomp shader_recompiler.cpp:1155/1384 does the same remap).
V4 AluConst(const uint32_t* regs, uint32_t idx, bool pixel)
{
    V4 v;
    // A relative index can wrap negative: never let a pixel shader alias vertex constants.
    if (pixel) { if (idx >= 256) return v; idx += 256; }
    if (idx >= 512) return v;
    for (int i = 0; i < 4; ++i) v[i] = AsFloat(regs[REG_ALU_CONST + idx * 4 + i]);
    return v;
}

// --------------------------------------------------------------------------------------
// microcode decoding
// --------------------------------------------------------------------------------------
enum CfOp
{
    CF_NOP = 0, CF_EXEC = 1, CF_EXEC_END = 2, CF_COND_EXEC = 3, CF_COND_EXEC_END = 4,
    CF_COND_EXEC_PRED = 5, CF_COND_EXEC_PRED_END = 6, CF_LOOP_START = 7, CF_LOOP_END = 8,
    CF_COND_CALL = 9, CF_RETURN = 10, CF_COND_JMP = 11, CF_ALLOC = 12,
    CF_COND_EXEC_PRED_CLEAN = 13, CF_COND_EXEC_PRED_CLEAN_END = 14, CF_MARK_VS_FETCH_DONE = 15
};

// A control-flow instruction is 48 bits: `lo` is the low dword, `hi` the high 16 bits.
struct Cf
{
    uint32_t lo = 0, hi = 0;
    uint32_t op()    const { return (hi >> 12) & 0xF; }
    uint32_t addr()  const { return lo & 0xFFF; }
    uint32_t count() const { return (lo >> 12) & 0x7; }
    uint32_t seq()   const { return (lo >> 16) & 0xFFF; }
    uint32_t boolAddr() const { return (hi >> 2) & 0xFF; }     // CondExec
    uint32_t cond()  const { return (hi >> 10) & 0x1; }
    bool isExec() const
    {
        uint32_t o = op();
        return o == CF_EXEC || o == CF_EXEC_END || o == CF_COND_EXEC || o == CF_COND_EXEC_END ||
               o == CF_COND_EXEC_PRED || o == CF_COND_EXEC_PRED_END ||
               o == CF_COND_EXEC_PRED_CLEAN || o == CF_COND_EXEC_PRED_CLEAN_END;
    }
    bool isEnd() const
    {
        uint32_t o = op();
        return o == CF_EXEC_END || o == CF_COND_EXEC_END || o == CF_COND_EXEC_PRED_END ||
               o == CF_COND_EXEC_PRED_CLEAN_END;
    }
};

void DecodeCfPair(const uint32_t* g, Cf& a, Cf& b)
{
    a.lo = g[0];
    a.hi = g[1] & 0xFFFF;
    b.lo = (g[1] >> 16) | (g[2] << 16);
    b.hi = g[2] >> 16;
}

enum AluVec
{
    V_ADD = 0, V_MUL, V_MAX, V_MIN, V_SEQ, V_SGT, V_SGE, V_SNE, V_FRC, V_TRUNC, V_FLOOR,
    V_MAD, V_CNDEQ, V_CNDGE, V_CNDGT, V_DP4, V_DP3, V_DP2ADD, V_CUBE, V_MAX4,
    V_SETP_EQ_PUSH, V_SETP_NE_PUSH, V_SETP_GT_PUSH, V_SETP_GE_PUSH,
    V_KILL_EQ, V_KILL_GT, V_KILL_GE, V_KILL_NE, V_DST, V_MAXA
};

enum AluScalar
{
    S_ADDS = 0, S_ADDS_PREV, S_MULS, S_MULS_PREV, S_MULS_PREV2, S_MAXS, S_MINS, S_SEQS,
    S_SGTS, S_SGES, S_SNES, S_FRCS, S_TRUNCS, S_FLOORS, S_EXP, S_LOGC, S_LOG, S_RCPC,
    S_RCPF, S_RCP, S_RSQC, S_RSQF, S_RSQ, S_MAXAS, S_MAXASF, S_SUBS, S_SUBS_PREV,
    S_SETP_EQ, S_SETP_NE, S_SETP_GT, S_SETP_GE, S_SETP_INV, S_SETP_POP, S_SETP_CLR,
    S_SETP_RSTR, S_KILLS_EQ, S_KILLS_GT, S_KILLS_GE, S_KILLS_NE, S_KILLS_ONE, S_SQRT,
    S_MULSC0 = 42, S_MULSC1, S_ADDSC0, S_ADDSC1, S_SUBSC0, S_SUBSC1, S_SIN, S_COS,
    S_RETAIN_PREV
};

struct Alu
{
    uint32_t w0 = 0, w1 = 0, w2 = 0;
    uint32_t vectorDest()       const { return w0 & 0x3F; }
    bool     absConstants()     const { return (w0 >> 7) & 1; }
    uint32_t scalarDest()       const { return (w0 >> 8) & 0x3F; }
    bool     scalarDestRelative() const { return (w0 >> 14) & 1; }
    bool     exportData()       const { return (w0 >> 15) & 1; }
    uint32_t vectorWriteMask()  const { return (w0 >> 16) & 0xF; }
    uint32_t scalarWriteMask()  const { return (w0 >> 20) & 0xF; }
    bool     vectorSaturate()   const { return (w0 >> 24) & 1; }
    bool     scalarSaturate()   const { return (w0 >> 25) & 1; }
    uint32_t scalarOpcode()     const { return (w0 >> 26) & 0x3F; }

    uint32_t srcSwizzle(int n)  const { return (w1 >> ((3 - n) * 8)) & 0xFF; }   // n = 1..3
    bool     srcNegate(int n)   const { return (w1 >> (24 + (3 - n))) & 1; }
    bool     predCondition()    const { return (w1 >> 27) & 1; }
    bool     isPredicated()     const { return (w1 >> 28) & 1; }
    bool     carRelative()      const { return (w1 >> 29) & 1; }
    bool     const1Relative()   const { return (w1 >> 30) & 1; }
    bool     const0Relative()   const { return (w1 >> 31) & 1; }

    uint32_t srcRegister(int n) const { return (w2 >> ((3 - n) * 8)) & 0xFF; }   // n = 1..3
    uint32_t vectorOpcode()     const { return (w2 >> 24) & 0x1F; }
    bool     srcSelect(int n)   const { return (w2 >> (29 + (3 - n))) & 1; }
};

struct VFetch
{
    uint32_t w0 = 0, w1 = 0, w2 = 0;
    uint32_t opcode()        const { return w0 & 0x1F; }
    uint32_t srcRegister()   const { return (w0 >> 5) & 0x3F; }
    uint32_t dstRegister()   const { return (w0 >> 12) & 0x3F; }
    uint32_t constIndex()    const { return (w0 >> 20) & 0x1F; }
    uint32_t constIndexSel() const { return (w0 >> 25) & 0x3; }
    uint32_t srcSwizzle()    const { return (w0 >> 30) & 0x3; }
    uint32_t dstSwizzle()    const { return w1 & 0xFFF; }
    uint32_t format()        const { return (w1 >> 16) & 0x3F; }
    bool     numFormatAll()  const { return (w1 >> 13) & 1; }   // 1 = integer, 0 = normalized
    bool     signedRfMode()  const { return (w1 >> 14) & 1; }
    int32_t  expAdjust()     const { return (int32_t)((w1 << 2) & 0xFC000000) >> 26; }
    bool     isMiniFetch()   const { return (w1 >> 30) & 1; }
    uint32_t stride()        const { return w2 & 0xFF; }
    int32_t  offset()        const { return (int32_t)((w2 << 1) & 0xFFFFFF00) >> 9; }
};

struct TFetch
{
    uint32_t w0 = 0, w1 = 0, w2 = 0;
    uint32_t opcode()      const { return w0 & 0x1F; }
    uint32_t srcRegister() const { return (w0 >> 5) & 0x3F; }
    uint32_t dstRegister() const { return (w0 >> 12) & 0x3F; }
    uint32_t constIndex()  const { return (w0 >> 20) & 0x1F; }
    bool     coordDenorm() const { return (w0 >> 25) & 1; }
    uint32_t srcSwizzle()  const { return (w0 >> 26) & 0x3F; }
    uint32_t dstSwizzle()  const { return w1 & 0xFFF; }
    uint32_t magFilter()   const { return (w1 >> 12) & 0x3; }   // 0 point, 1 linear, 3 = fetch constant's
    // dword 1 bit 28 use_computed_lod: the hardware picks the mip level from the coordinate's screen-space
    // derivatives (every TTT2 tfetch but 121 of 23,718; the rest sample level 0).
    bool     useComputedLod() const { return (w1 >> 28) & 1; }
    // FetchOpDimension, dword 2 bits 14-15 (Xenia ucode.h): 0 1D, 1 2D, 2 3D/stacked, 3 cube.
    // TTT2's pixel shaders use 2D (w2 = 0x4000) and cube (0xC000) fetches.
    uint32_t dimension()   const { return (w2 >> 14) & 0x3; }
};

// --------------------------------------------------------------------------------------
// texture / vertex format unpacking
// --------------------------------------------------------------------------------------
enum Fmt
{
    FMT_8 = 2, FMT_1_5_5_5 = 3, FMT_5_6_5 = 4, FMT_6_5_5 = 5, FMT_8_8_8_8 = 6,
    FMT_2_10_10_10 = 7, FMT_8_A = 8, FMT_8_B = 9, FMT_8_8 = 10, FMT_4_4_4_4 = 15,
    FMT_10_11_11 = 16, FMT_11_11_10 = 17, FMT_DXT1 = 18, FMT_DXT2_3 = 19, FMT_DXT4_5 = 20,
    FMT_24_8 = 22, FMT_24_8_FLOAT = 23, FMT_16 = 24, FMT_16_16 = 25, FMT_16_16_16_16 = 26,
    FMT_16_FLOAT = 30, FMT_16_16_FLOAT = 31, FMT_16_16_16_16_FLOAT = 32,
    FMT_32 = 33, FMT_32_32 = 34, FMT_32_32_32_32 = 35,
    FMT_32_FLOAT = 36, FMT_32_32_FLOAT = 37, FMT_32_32_32_32_FLOAT = 38,
    FMT_DXN = 49, FMT_32_32_32_FLOAT = 57, FMT_DXT3A = 58, FMT_DXT5A = 59, FMT_CTX1 = 60
};

// Bits per texel for the non-compressed formats; 0 marks a block-compressed format.
uint32_t FormatBpp(uint32_t f)
{
    switch (f)
    {
    case FMT_8: case FMT_8_A: case FMT_8_B:                                   return 8;
    case FMT_1_5_5_5: case FMT_5_6_5: case FMT_6_5_5: case FMT_4_4_4_4:
    case FMT_8_8: case FMT_16: case FMT_16_FLOAT:                             return 16;
    case FMT_8_8_8_8: case FMT_2_10_10_10: case FMT_10_11_11: case FMT_11_11_10:
    case FMT_16_16: case FMT_16_16_FLOAT: case FMT_32: case FMT_32_FLOAT:     return 32;
    case FMT_16_16_16_16: case FMT_16_16_16_16_FLOAT:
    case FMT_32_32: case FMT_32_32_FLOAT:                                     return 64;
    case FMT_32_32_32_FLOAT:                                                  return 96;
    case FMT_32_32_32_32: case FMT_32_32_32_32_FLOAT:                         return 128;
    default:                                                                  return 0;
    }
}

bool IsBlockCompressed(uint32_t f)
{
    return f == FMT_DXT1 || f == FMT_DXT2_3 || f == FMT_DXT4_5 || f == FMT_DXN ||
           f == FMT_DXT3A || f == FMT_DXT5A || f == FMT_CTX1;
}

float Half2Float(uint16_t h)
{
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    uint32_t out;
    if (exp == 0)
    {
        if (!man) out = sign << 31;
        else
        {
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) { man <<= 1; --exp; }
            man &= 0x3FF;
            out = (sign << 31) | (exp << 23) | (man << 13);
        }
    }
    else if (exp == 31) out = (sign << 31) | 0x7F800000u | (man << 13);
    else out = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
    return AsFloat(out);
}

// Read `format` from guest memory at `addr` into a float4. `normalized` follows the fetch
// instruction's num_format_all bit (0 = normalize integers to [0,1] / [-1,1]).
V4 UnpackFormat(uint32_t addr, uint32_t format, bool normalized, bool isSigned, bool vertexFetch = false)
{
    V4 v;
    auto norm8  = [&](uint32_t u) { return isSigned ? std::max(-1.0f, (float)(int8_t)u / 127.0f)
                                                    : (float)u / 255.0f; };
    auto norm16 = [&](uint32_t u) { return isSigned ? std::max(-1.0f, (float)(int16_t)u / 32767.0f)
                                                    : (float)u / 65535.0f; };
    switch (format)
    {
    case FMT_32_FLOAT:            v[0] = AsFloat(GRead32(g_base, addr)); v[3] = 1.0f; break;
    case FMT_32_32_FLOAT:
        v[0] = AsFloat(GRead32(g_base, addr));
        v[1] = AsFloat(GRead32(g_base, addr + 4)); v[3] = 1.0f; break;
    case FMT_32_32_32_FLOAT:
        for (int i = 0; i < 3; ++i) v[i] = AsFloat(GRead32(g_base, addr + i * 4));
        v[3] = 1.0f; break;
    case FMT_32_32_32_32_FLOAT:
        for (int i = 0; i < 4; ++i) v[i] = AsFloat(GRead32(g_base, addr + i * 4));
        break;
    case FMT_8_8_8_8:
    {
        uint32_t u = GRead32(g_base, addr);
        // Components from the LOW bits, for vertex fetch and textures alike (Xenia's vfetch
        // unpack and its R8G8B8A8 texture format; a k_8_8_8_8 render target and texture are
        // bitwise equivalent). D3DFMT_A8R8G8B8 data (0xAARRGGBB words) therefore fetches as
        // (B, G, R, A) and its fetch-constant swizzle .zyxw (0x60A) makes it RGBA again.
        // MEASURED 2026-10-10 (vertex fetch): the battle characters' VS passes an 8_8_8_8
        // attribute to the skin pixel shader as a per-channel tint; the ARGB order drew every
        // fighter magenta/green. MEASURED 2026-10-12 (textures): the ARGB order showed the HUD's
        // red recoverable-health bar blue and the stage environment cube with blue ground.
        (void)vertexFetch;
        v[0] = norm8(u & 0xFF);
        v[1] = norm8((u >> 8) & 0xFF);
        v[2] = norm8((u >> 16) & 0xFF);
        v[3] = norm8((u >> 24) & 0xFF);
        if (normalized) { for (int i = 0; i < 4; ++i) v[i] *= 255.0f; }
        break;
    }
    case FMT_8:
    {
        uint8_t u = GRead8(g_base, addr);
        v[0] = v[1] = v[2] = norm8(u); v[3] = 1.0f;
        break;
    }
    // 16-bit packed textures: x from the LOW bits as for 8_8_8_8 (Xenia's load shaders
    // R5G6B5ToB5G6R5 / R5G5B5A1ToB5G5R5A1 / RGBA4ToBGRA4 / R5G5B6ToB5G6R5WithRBGASwizzle move
    // the guest's low field into host red). Vertex fetch keeps its high-field-first order, which
    // the hardware VS translation mirrors (gpu_hw_translate.h VFetch_); no TTT2 vertex stream
    // has been seen using these formats.
    case FMT_5_6_5:
    {
        uint16_t u = GRead16(g_base, addr);
        v[0] = ((vertexFetch ? u >> 11 : u) & 0x1F) / 31.0f;
        v[1] = ((u >> 5) & 0x3F) / 63.0f;
        v[2] = ((vertexFetch ? u : u >> 11) & 0x1F) / 31.0f;
        v[3] = 1.0f;
        break;
    }
    case FMT_6_5_5:
    {
        uint16_t u = GRead16(g_base, addr);
        v[0] = (u & 0x1F) / 31.0f;
        v[1] = ((u >> 5) & 0x1F) / 31.0f;
        v[2] = ((u >> 10) & 0x3F) / 63.0f;
        v[3] = 1.0f;
        break;
    }
    case FMT_1_5_5_5:
    {
        uint16_t u = GRead16(g_base, addr);
        v[0] = ((vertexFetch ? u >> 10 : u) & 0x1F) / 31.0f;
        v[1] = ((u >> 5) & 0x1F) / 31.0f;
        v[2] = ((vertexFetch ? u : u >> 10) & 0x1F) / 31.0f;
        v[3] = (u >> 15) ? 1.0f : 0.0f;
        break;
    }
    case FMT_4_4_4_4:
    {
        uint16_t u = GRead16(g_base, addr);
        if (vertexFetch)
        {
            v[0] = ((u >> 8) & 0xF) / 15.0f;
            v[1] = ((u >> 4) & 0xF) / 15.0f;
            v[2] = (u & 0xF) / 15.0f;
            v[3] = ((u >> 12) & 0xF) / 15.0f;
        }
        else
        {
            v[0] = (u & 0xF) / 15.0f;
            v[1] = ((u >> 4) & 0xF) / 15.0f;
            v[2] = ((u >> 8) & 0xF) / 15.0f;
            v[3] = ((u >> 12) & 0xF) / 15.0f;
        }
        break;
    }
    case FMT_8_8:                       // same low-bits-first rule as the 16_16 family above
    {
        uint16_t u = GRead16(g_base, addr);
        v[0] = norm8(u & 0xFF);
        v[1] = norm8((u >> 8) & 0xFF);
        v[3] = 1.0f;
        break;
    }
    // Components packed into ONE 32-bit word are numbered from the LOW bits: the fetch reads a
    // big-endian word out of guest memory and the shader unit sees it as a little-endian
    // register, so the component that comes FIRST in memory ends up in the HIGH half and is
    // therefore component .y, not .x.
    //
    // MEASURED: the full-screen video pass fetches its texcoord as `vfetch r0.yx <- 16_16F`,
    // and that .yx is the compiler undoing exactly this - a 32_32_FLOAT texcoord in the same
    // role fetches with a plain .xy. Reading the halves the other way round cancelled the .yx
    // and swapped u with v, which drew every textured pass TRANSPOSED: the vertex at window
    // (2560,0) carried texcoord (0,2) where it has to carry (2,0).
    case FMT_16_16:
    {
        uint32_t u = GRead32(g_base, addr);
        v[0] = norm16(u & 0xFFFF); v[1] = norm16(u >> 16); v[3] = 1.0f;
        if (normalized) { v[0] *= 65535.0f; v[1] *= 65535.0f; }
        break;
    }
    case FMT_16_16_16_16:
    {
        uint32_t a = GRead32(g_base, addr), b = GRead32(g_base, addr + 4);
        v[0] = norm16(a & 0xFFFF); v[1] = norm16(a >> 16);
        v[2] = norm16(b & 0xFFFF); v[3] = norm16(b >> 16);
        break;
    }
    case FMT_16_16_FLOAT:
    {
        uint32_t u = GRead32(g_base, addr);
        v[0] = Half2Float(uint16_t(u)); v[1] = Half2Float(uint16_t(u >> 16)); v[3] = 1.0f;
        break;
    }
    case FMT_16_16_16_16_FLOAT:
    {
        uint32_t a = GRead32(g_base, addr), b = GRead32(g_base, addr + 4);
        v[0] = Half2Float(uint16_t(a)); v[1] = Half2Float(uint16_t(a >> 16));
        v[2] = Half2Float(uint16_t(b)); v[3] = Half2Float(uint16_t(b >> 16));
        break;
    }
    case FMT_2_10_10_10:
    {
        uint32_t u = GRead32(g_base, addr);
        v[0] = (u & 0x3FF) / 1023.0f;                     // low-bits rule, as 8_8_8_8 above
        v[1] = ((u >> 10) & 0x3FF) / 1023.0f;             // (Xenia: R10G10B10A2_UNORM, 32bpb copy)
        v[2] = ((u >> 20) & 0x3FF) / 1023.0f;
        v[3] = ((u >> 30) & 0x3) / 3.0f;
        break;
    }
    default:
        v[3] = 1.0f;
        break;
    }
    return v;
}

// --------------------------------------------------------------------------------------
// tiled addressing
// --------------------------------------------------------------------------------------
} // namespace

uint32_t GpuTiledOffset2D(uint32_t x, uint32_t y, uint32_t width, uint32_t logBpp)
{
    uint32_t alignedWidth = (width + 31) & ~31u;
    uint32_t macro = ((x >> 5) + (y >> 5) * (alignedWidth >> 5)) << (logBpp + 7);
    uint32_t micro = (((x & 7) + ((y & 6) << 2)) << logBpp);
    uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15u) +
                      ((y & 8) << (3 + logBpp)) + ((y & 1) << 4);
    return (((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) +
            ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3u) << 6)) >> logBpp;
}

namespace
{

// --------------------------------------------------------------------------------------
// texture sampling
// --------------------------------------------------------------------------------------
struct Texture
{
    bool valid = false;
    uint32_t base = 0, width = 1, height = 1, format = 0, pitch = 0;
    bool tiled = false;
    uint32_t swizzle = 0;
    uint32_t endian = 0;
    // Sampler state from the fetch constant: ClampMode per axis (dword 0 bits 10-12 / 13-15),
    // mag filter (dword 3 bits 19-20: 0 point, 1 linear), border colour (dword 5 bits 0-1).
    uint32_t clampX = 2, clampY = 2, magFilter = 1, border = 0;
    uint32_t dimension = 1;          // dword5 bits 9-10: 0 1D, 1 2D, 2 3D/stacked, 3 cube
    // dword0 bits 2-9: per-component sign x,y,z,w (2 bits each): 0 unsigned, 1 signed,
    // 2 unsigned biased, 3 gamma (Xenia TextureSign). Recorded for the format census.
    uint32_t signs = 0;
    // Mip levels and minification (Xenia xenos.h xe_gpu_texture_fetch_t): dword 3 bits 21-22 min
    // filter, 23-24 mip filter (0 point, 1 linear, 2 base level only), 25-27 anisotropic filter
    // (0 off, 1..5 = 1:1..16:1); dword 4 bits 2-5 / 6-9 the first / last level, 12-21 the LOD bias
    // (signed, 1/32 steps); dword 5 bit 11 packed mips, bits 12-31 the mip levels' physical page.
    uint32_t minFilter = 1, mipFilter = 2, aniso = 0, mipMin = 0, mipMax = 0;
    int32_t lodBias = 0;
    bool packedMips = false;
    uint32_t mipBase = 0;
    // Where the image starts inside the storage at `base` (texels): a packed mip level's offset in
    // its tile (TextureMipLevel). 0 for every fetch-constant texture.
    uint32_t originX = 0, originY = 0;
};

// Bytes from one cube face (array slice) to the next: each face is a whole 2D surface - block
// rows/columns padded to 32 when tiled - starting on a 4 KB boundary. Matches TTT2's battle
// cubes back to back in memory (64x64 8_8_8_8 and 128x128 DXT5: 16 KB per face, 96 KB a cube).
uint32_t TextureFaceStride(const Texture& t)
{
    const bool bc = IsBlockCompressed(t.format);
    const uint64_t blockBytes = bc ? ((t.format == FMT_DXT1 || t.format == FMT_DXT3A || t.format == FMT_DXT5A ||
                                       t.format == FMT_CTX1) ? 8 : 16)
                                   : std::max<uint32_t>(FormatBpp(t.format) / 8, 1);
    uint64_t cols = bc ? (t.pitch + 3) / 4 : t.pitch, rows = bc ? (t.height + 3) / 4 : t.height;
    if (t.tiled) { cols = (cols + 31) & ~31ull; rows = (rows + 31) & ~31ull; }
    return uint32_t(std::min<uint64_t>((cols * blockBytes * rows + 4095) & ~4095ull, 64ull << 20));
}

// Xenos ClampMode -> texel index, shared with the hardware translator (XAddr). Returns -1 for
// a border texel. 0 repeat, 1 mirrored repeat, 2 clamp to edge, 3 mirror once, 4 clamp to
// halfway (as clamp), 5 / 7 mirror-clamp to halfway / border (as mirror once, like Xenia's
// D3D12 samplers), 6 clamp to border.
inline int TexAddress(int x, int size, uint32_t mode)
{
    switch (mode)
    {
    case 0: { const int m = x % size; return m < 0 ? m + size : m; }
    case 1: { const int p = 2 * size; int m = x % p; if (m < 0) m += p; return m < size ? m : p - 1 - m; }
    case 3: case 5: case 7: { const int m = x < 0 ? -x - 1 : x; return std::min(m, size - 1); }
    case 6: return (x < 0 || x >= size) ? -1 : x;
    default: return std::clamp(x, 0, size - 1);
    }
}

// Float texel coordinate -> int without undefined behaviour (and as HLSL's clamped ftoi):
// far-outside coordinates lose their wrap phase either way.
inline int TexFloor(float f)
{
    return (int)std::floor(std::clamp(f, -16777216.0f, 16777216.0f));
}

Texture DecodeTexture(const uint32_t* regs, uint32_t slot)
{
    Texture t;
    if (slot >= 32) return t;
    const uint32_t* c = &regs[REG_FETCH_CONST + slot * 6];
    if ((c[0] & 3) != 2) return t;                       // type 2 == texture fetch constant
    t.pitch  = ((c[0] >> 22) & 0x1FF) * 32;
    t.tiled  = (c[0] >> 31) & 1;
    t.format = c[1] & 0x3F;
    t.endian = (c[1] >> 6) & 3;
    // The 20-bit page number is a PHYSICAL address. The front buffer's own fetch constant
    // happens to carry one already in the physical-heap window (0xA44A9), which is why this
    // looked right at first; the game's real textures point at resolve destinations like
    // 0x067F1000, which have to be mapped up into guest space or the sampler reads zeros
    // and every textured pass comes out black.
    t.base   = PhysToGuest((c[1] >> 12) << 12);
    t.width  = (c[2] & 0x1FFF) + 1;
    t.height = ((c[2] >> 13) & 0x1FFF) + 1;
    t.swizzle = (c[3] >> 1) & 0xFFF;
    t.clampX = (c[0] >> 10) & 7;
    t.clampY = (c[0] >> 13) & 7;
    t.magFilter = (c[3] >> 19) & 3;
    t.border = c[5] & 3;
    t.dimension = (c[5] >> 9) & 3;
    t.signs = (c[0] >> 2) & 0xFF;
    t.minFilter = (c[3] >> 21) & 3;
    t.mipFilter = (c[3] >> 23) & 3;
    t.aniso = (c[3] >> 25) & 7;
    t.mipMin = (c[4] >> 2) & 0xF;
    t.mipMax = (c[4] >> 6) & 0xF;
    t.lodBias = int32_t(c[4] << 10) >> 22;
    t.packedMips = (c[5] >> 11) & 1;
    t.mipBase = (c[5] >> 12) ? PhysToGuest((c[5] >> 12) << 12) : 0;
    if (!t.pitch) t.pitch = t.width;
    t.valid = t.base != 0 && t.width > 0 && t.height > 0;
    return t;
}

// Where a mip level is stored (Xenos; the layout Xenia's texture_util.cc GetGuestTextureLayout /
// GetPackedMipOffset describes): level 0 at the base address with the fetch constant's pitch; levels
// 1+ back to back from the mip address, each sized from the base's power-of-two-rounded dimensions
// (tiled: whole 32x32-block tiles), every level 4 KB aligned. With packed mips, the levels from the
// first whose shorter side is 16 texels or less share one level's storage, each at its own texel
// offset in it. `variant` (TTT2_MIP_CENSUS only): 1 no packing, 2 sizes not rounded to powers of
// two, 4 a packed level-0 chain's levels from the mip address.
struct MipLevel { uint32_t base = 0, pitch = 1, rows = 1, x = 0, y = 0, width = 1, height = 1; };   // pitch / rows: the storage, texels
inline uint32_t Log2Ceil(uint32_t v) { return v <= 1 ? 0u : 32u - uint32_t(__builtin_clz(v - 1)); }
inline void PackedMipOffset(uint32_t width, uint32_t height, uint32_t level, uint32_t& x, uint32_t& y)
{
    const uint32_t lw = Log2Ceil(width), lh = Log2Ceil(height), ls = std::min(lw, lh);
    const uint32_t packedBase = ls > 4 ? ls - 4 : 0, p = level - packedBase;
    x = y = 0;
    if (p < 3) { if (lw > lh) y = 16u >> p; else x = 16u >> p; }
    else if (lw > lh) x = (1u << (lw - packedBase)) >> (p - 2);
    else y = (1u << (lh - packedBase)) >> (p - 2);
}
inline uint32_t TextureBlockBytes(uint32_t format)
{
    if (!IsBlockCompressed(format)) return std::max<uint32_t>(FormatBpp(format) / 8, 1);
    return (format == FMT_DXT1 || format == FMT_DXT3A || format == FMT_DXT5A || format == FMT_CTX1) ? 8 : 16;
}
bool TextureMipLevel(const Texture& t, uint32_t level, MipLevel& m, uint32_t variant = 0)
{
    const uint32_t bw = IsBlockCompressed(t.format) ? 4 : 1, bpb = TextureBlockBytes(t.format);
    m.width = std::max(t.width >> level, 1u); m.height = std::max(t.height >> level, 1u);
    const uint32_t ls = Log2Ceil(std::min(t.width, t.height));
    const uint32_t packedLevel = t.packedMips && !(variant & 1) ? (ls > 4 ? ls - 4 : 0) : 99u;
    m.x = m.y = 0;
    if (level >= packedLevel) PackedMipOffset(t.width, t.height, level, m.x, m.y);
    if (level == 0 || (packedLevel == 0 && !(variant & 4))) { m.base = t.base; m.pitch = t.pitch; m.rows = t.height; return true; }
    if (!t.mipBase) return false;
    const uint32_t w2 = (variant & 2) ? t.width : 1u << Log2Ceil(t.width), h2 = (variant & 2) ? t.height : 1u << Log2Ceil(t.height);
    uint32_t addr = t.mipBase;
    const uint32_t last = std::min(level, packedLevel);
    for (uint32_t l = 1; l <= 15; ++l)
    {
        const uint32_t lw = std::max(w2 >> l, 1u), lh = std::max(h2 >> l, 1u);
        uint32_t cols = (lw + bw - 1) / bw, rows = (lh + bw - 1) / bw;
        if (t.tiled) { cols = (cols + 31) & ~31u; rows = (rows + 31) & ~31u; }
        else cols = ((cols * bpb + 255) & ~255u) / bpb;
        if (l == last) { m.base = addr; m.pitch = cols * bw; m.rows = rows * bw; return true; }
        addr += (cols * rows * bpb + 4095) & ~4095u;
    }
    return false;
}

// Normalize one texture dword to the little-endian bit layout consumed by BC
// decoders. Xenos endian modes: none, 8-in-16, 8-in-32, 16-in-32. The UI's BC3
// atlases use 8-in-16: treating selectors as BE32 swaps rows within each block,
// while reading alpha bytes without the same normalization corrupts transparency.
// References: Xenia xenos.h Endian / texture_load_dxt5_rgba8.cs.xesl.
uint32_t TextureWord(uint32_t address, uint32_t endian)
{
    uint32_t word;
    std::memcpy(&word, g_base + address, sizeof(word));
    switch (endian)
    {
    case 1: return ((word & 0x00FF00FFu) << 8) | ((word & 0xFF00FF00u) >> 8);
    case 2: return __builtin_bswap32(word);
    case 3: return (word << 16) | (word >> 16);
    default: return word;
    }
}

V4 SampleTexel(const Texture& t, int x, int y)
{
    V4 v;
    x = TexAddress(x, (int)t.width, t.clampX);
    y = TexAddress(y, (int)t.height, t.clampY);
    if (x < 0 || y < 0)
    {
        // Border texel: black (0,0,0,0) or white (1,1,1,1) before the swizzle.
        for (int i = 0; i < 4; ++i) v[i] = t.border == 1 ? 1.0f : 0.0f;
    }
    else if (IsBlockCompressed(t.format))
    {
        uint32_t bw = (t.pitch + 3) / 4;
        uint32_t bx = x / 4, by = y / 4;
        uint32_t blockBytes = (t.format == FMT_DXT1 || t.format == FMT_DXT3A ||
                               t.format == FMT_DXT5A || t.format == FMT_CTX1) ? 8 : 16;
        uint32_t logBpb = blockBytes == 8 ? 3 : 4;
        uint32_t bi = t.tiled ? GpuTiledOffset2D(bx, by, bw, logBpb) : (by * bw + bx);
        uint32_t addr = t.base + bi * blockBytes;
        uint32_t colorOff = (t.format == FMT_DXT2_3 || t.format == FMT_DXT4_5) ? 8 : 0;
        const uint32_t endpoints = TextureWord(addr + colorOff, t.endian);
        uint16_t c0 = uint16_t(endpoints), c1 = uint16_t(endpoints >> 16);
        uint32_t bits = TextureWord(addr + colorOff + 4, t.endian);
        auto rgb = [](uint16_t c, float* o) {
            o[0] = ((c >> 11) & 0x1F) / 31.0f;
            o[1] = ((c >> 5) & 0x3F) / 63.0f;
            o[2] = (c & 0x1F) / 31.0f;
        };
        float e0[3], e1[3];
        rgb(c0, e0); rgb(c1, e1);
        uint32_t sel = (bits >> (((y & 3) * 4 + (x & 3)) * 2)) & 3;
        float a = 1.0f;
        for (int i = 0; i < 3; ++i)
        {
            float r;
            // BC2/BC3 always have four colour entries; only BC1 has the
            // endpoint-order-dependent transparent/three-colour mode.
            if (c0 > c1 || t.format == FMT_DXT2_3 || t.format == FMT_DXT4_5)
                r = sel == 0 ? e0[i] : sel == 1 ? e1[i]
                  : sel == 2 ? (2 * e0[i] + e1[i]) / 3.0f : (e0[i] + 2 * e1[i]) / 3.0f;
            else
            {
                r = sel == 0 ? e0[i] : sel == 1 ? e1[i]
                  : sel == 2 ? (e0[i] + e1[i]) * 0.5f : 0.0f;
                if (sel == 3 && t.format == FMT_DXT1) a = 0.0f;
            }
            v[i] = r;
        }
        if (t.format == FMT_DXT2_3)
        {
            uint32_t half = TextureWord(addr + ((y & 3) < 2 ? 0 : 4), t.endian);
            uint32_t sh = (((y & 1) * 4) + (x & 3)) * 4;
            a = ((half >> sh) & 0xF) / 15.0f;
        }
        else if (t.format == FMT_DXT4_5)
        {
            const uint32_t alphaLo = TextureWord(addr, t.endian);
            const uint32_t alphaHi = TextureWord(addr + 4, t.endian);
            uint8_t a0 = uint8_t(alphaLo), a1 = uint8_t(alphaLo >> 8);
            uint64_t idx = (uint64_t(alphaHi) << 16) | (alphaLo >> 16);
            uint32_t s = (idx >> (((y & 3) * 4 + (x & 3)) * 3)) & 7;
            if (a0 > a1)
                a = s == 0 ? a0 / 255.0f : s == 1 ? a1 / 255.0f
                  : ((float)((8 - s) * a0 + (s - 1) * a1) / 7.0f) / 255.0f;
            else
                a = s == 0 ? a0 / 255.0f : s == 1 ? a1 / 255.0f : s == 6 ? 0.0f : s == 7 ? 1.0f
                  : ((float)((6 - s) * a0 + (s - 1) * a1) / 5.0f) / 255.0f;
        }
        v[3] = a;
    }
    else
    {
        uint32_t bpp = FormatBpp(t.format);
        if (!bpp) { v[3] = 1.0f; return v; }
        uint32_t bytes = bpp / 8, logBpp = bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2
                                        : bytes == 8 ? 3 : 4;
        uint32_t ti = t.tiled ? GpuTiledOffset2D(x, y, t.pitch, logBpp) : (y * t.pitch + x);
        v = UnpackFormat(t.base + ti * bytes, t.format, false, false);
    }
    // Destination swizzle from the fetch constant (3 bits per component: 0-3 = xyzw,
    // 4 = 0.0, 5 = 1.0).
    V4 o;
    for (int i = 0; i < 4; ++i)
    {
        uint32_t s = (t.swizzle >> (i * 3)) & 7;
        o[i] = s < 4 ? v[s] : s == 4 ? 0.0f : s == 5 ? 1.0f : v[i];
    }
    return o;
}


// TTT2_TEXDUMP=1: write each distinct texture the shaders sample to port/tex/<base>.bmp,
// decoded through the same sampler the rasterizer uses. The presented frame is an exact
// TRANSPOSE of the correct image; tiling round-trips (resolve and present share
// TiledOffset32) and the geometry is right (the full-screen triangle measures 2560x1440,
// not 1440x2560), so the swap is either in the texture read or in the interpolated UVs.
// A dumped texture that is already transposed settles it.
void DumpTextureBmp(const Texture& t)
{
    static const bool on = std::getenv("TTT2_TEXDUMP") != nullptr;
    if (!on || !t.valid || t.width < 4 || t.height < 4 ||
        t.width > 4096 || t.height > 4096) return;
    // Texture fetches now run on raster-pool workers, so everything below (the static try
    // map, the dump files) is shared mutable state reached concurrently - serialize it. The
    // env-off early-out above stays lock-free.
    static std::mutex dumpMutex;
    std::lock_guard<std::mutex> dumpLock(dumpMutex);
    // Keep retrying a texture until a decode that actually has colour is captured - the
    // interesting textures are render targets that are still black the first time a shader
    // samples them - but stop once one is written, and cap the total so this cannot thrash.
    static std::map<uint32_t, int> tries;
    int& n = tries[t.base];
    if (n < 0 || n > 400) return;
    ++n;
    bool any = false;
    for (uint32_t y = 0; y < t.height && !any; y += 7)
        for (uint32_t x = 0; x < t.width; x += 7)
        {
            V4 c = SampleTexel(t, (int)x, (int)y);
            if (c[0] > 0.02f || c[1] > 0.02f || c[2] > 0.02f) { any = true; break; }
        }
    if (!any) return;
    n = -1;                                   // captured; never dump this base again
    std::filesystem::create_directories("C:/ttt2/port/tex");
    char path[160];
    std::snprintf(path, sizeof(path), "C:/ttt2/port/tex/%08X_%ux%u_f%u.bmp", t.base,
                  t.width, t.height, t.format);
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    uint32_t imgSize = t.width * t.height * 4;
    uint8_t hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    *reinterpret_cast<uint32_t*>(hdr + 2) = 54 + imgSize;
    *reinterpret_cast<uint32_t*>(hdr + 10) = 54;
    *reinterpret_cast<uint32_t*>(hdr + 14) = 40;
    *reinterpret_cast<int32_t*>(hdr + 18) = (int32_t)t.width;
    *reinterpret_cast<int32_t*>(hdr + 22) = -(int32_t)t.height;
    *reinterpret_cast<uint16_t*>(hdr + 26) = 1;
    *reinterpret_cast<uint16_t*>(hdr + 28) = 32;
    *reinterpret_cast<uint32_t*>(hdr + 34) = imgSize;
    std::fwrite(hdr, 1, sizeof(hdr), f);
    std::vector<uint32_t> row(t.width);
    for (uint32_t y = 0; y < t.height; ++y)
    {
        for (uint32_t x = 0; x < t.width; ++x)
        {
            V4 c = SampleTexel(t, (int)x, (int)y);
            auto q = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
            row[x] = (q(c[0]) << 16) | (q(c[1]) << 8) | q(c[2]);
        }
        std::fwrite(row.data(), 4, t.width, f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[tex] dumped %s pitch=%u tiled=%d\n", path, t.pitch, (int)t.tiled);
}

V4 SampleTexture(const Texture& t, float u, float vv, bool bilinear)
{
    if (!t.valid) { V4 z; z[3] = 1.0f; return z; }
    float fx = std::clamp(u * t.width - 0.5f, -16777216.0f, 16777216.0f);
    float fy = std::clamp(vv * t.height - 0.5f, -16777216.0f, 16777216.0f);
    if (!bilinear) return SampleTexel(t, TexFloor(u * t.width), TexFloor(vv * t.height));
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    float tx = fx - x0, ty = fy - y0;
    V4 a = SampleTexel(t, x0, y0), b = SampleTexel(t, x0 + 1, y0);
    V4 c = SampleTexel(t, x0, y0 + 1), d = SampleTexel(t, x0 + 1, y0 + 1);
    V4 r;
    for (int i = 0; i < 4; ++i)
        r[i] = (a[i] * (1 - tx) + b[i] * tx) * (1 - ty) + (c[i] * (1 - tx) + d[i] * tx) * ty;
    return r;
}

// --------------------------------------------------------------------------------------
// shader interpreter
// --------------------------------------------------------------------------------------
struct ShaderCtx
{
    V4 r[64];
    const uint32_t* regs = nullptr;
    bool p0 = false;
    int a0 = 0, aL = 0;
    float ps = 0.0f;
    bool killed = false;
    bool pixel = false;
    V4 exports[64];              // 0-15 interpolators / colours, 62 position, 61 depth
    uint32_t exportMask = 0;     // which of exports 0-15 were written
    bool wrotePos = false;
    uint32_t lastStride = 0;     // for mini-fetches
    uint32_t lastFetchSlot = 0;
};

// Temporary register operand index: bit 6 adds aL (Xenia ucode.h is_src_temp_relative).
uint32_t TempIndex(const ShaderCtx& sc, uint32_t reg)
{
    return (reg & 0x40) ? uint32_t((int)(reg & 0x3F) + sc.aL) & 63u : (reg & 0x3F);
}

// Constant operand index with relative addressing. The FIRST constant operand of an ALU
// instruction uses const_0_rel_abs and every later one const_1_rel_abs; bit 29 picks a0
// rather than aL (Xenia ucode.h src_const_is_addressed). Wrapping to a huge index reads zero
// through AluConst, as an out-of-range address does.
uint32_t ConstIndex(const ShaderCtx& sc, const Alu& in, int which /* 1..3 */, uint32_t reg)
{
    bool addressed;
    switch (which)
    {
    case 1:  addressed = in.const0Relative(); break;
    case 2:  addressed = in.srcSelect(1) ? in.const0Relative() : in.const1Relative(); break;
    default: addressed = (in.srcSelect(1) && in.srcSelect(2)) ? in.const0Relative() : in.const1Relative(); break;
    }
    if (!addressed) return reg;
    return uint32_t(int32_t(reg) + (in.carRelative() ? sc.a0 : sc.aL));
}

V4 ReadVecOperand(const ShaderCtx& sc, const Alu& in, int which /* 1..3 */)
{
    uint32_t reg = in.srcRegister(which), swz = in.srcSwizzle(which);
    bool sel = in.srcSelect(which), neg = in.srcNegate(which), abs;
    V4 v;
    if (sel)
    {
        abs = (reg & 0x80) != 0;
        v = sc.r[TempIndex(sc, reg)];
    }
    else
    {
        abs = in.absConstants();
        v = AluConst(sc.regs, ConstIndex(sc, in, which, reg), sc.pixel);
    }
    V4 o;
    for (int i = 0; i < 4; ++i) o[i] = v[((swz >> (i * 2)) + i) & 3];
    if (abs) for (int i = 0; i < 4; ++i) o[i] = std::fabs(o[i]);
    if (neg) for (int i = 0; i < 4; ++i) o[i] = -o[i];
    return o;
}

// The scalar unit's two operands live in src3 (and, for the *sc* opcodes, a constant).
float ReadScalarOperand(const ShaderCtx& sc, const Alu& in, int which /* 0 or 1 */)
{
    uint32_t reg = in.srcRegister(3), swz = in.srcSwizzle(3);
    bool sel = in.srcSelect(3), neg = in.srcNegate(3), abs;
    V4 v;
    if (sel) { abs = (reg & 0x80) != 0; v = sc.r[TempIndex(sc, reg)]; }
    else     { abs = in.absConstants(); v = AluConst(sc.regs, ConstIndex(sc, in, 3, reg), sc.pixel); }
    int comp = which == 0 ? (int)(((swz >> 6) + 3) & 3) : (int)(swz & 3);
    float f = v[comp];
    if (abs) f = std::fabs(f);
    if (neg) f = -f;
    return f;
}

void WriteResult(ShaderCtx& sc, const Alu& in, bool scalarUnit, const V4& value)
{
    uint32_t mask = scalarUnit ? in.scalarWriteMask() : in.vectorWriteMask();
    if (in.exportData())
        mask &= ~(scalarUnit ? in.vectorWriteMask() : in.scalarWriteMask());
    if (!mask) return;
    bool sat = scalarUnit ? in.scalarSaturate() : in.vectorSaturate();
    uint32_t dst = scalarUnit && !in.exportData() ? in.scalarDest() : in.vectorDest();
    V4* target;
    if (in.exportData())
    {
        if (dst >= 64) return;
        target = &sc.exports[dst];
        if (dst < 16) sc.exportMask |= 1u << dst;
        if (dst == 62) sc.wrotePos = true;
    }
    else
    {
        // Register destinations can be aL-relative (vector_dest_rel / scalar_dest_rel).
        const bool relative = scalarUnit ? in.scalarDestRelative() : ((in.w0 >> 6) & 1) != 0;
        target = &sc.r[relative ? uint32_t((int)(dst & 0x3F) + sc.aL) & 63u : (dst & 0x3F)];
    }
    for (int i = 0; i < 4; ++i)
    {
        if (!((mask >> i) & 1)) continue;
        float f = value[i];
        if (sat) f = std::clamp(f, 0.0f, 1.0f);
        (*target)[i] = f;
    }
}

void WriteExportConstants(ShaderCtx& sc, const Alu& in)
{
    if (!in.exportData()) return;
    // Exports encode four choices per component, not two independent writes:
    // vector only, scalar only, both = 1, neither = preserve (or 0 when bit14
    // is set). Both units address vectorDest. This also applies to RetainPrev.
    // Reference: xenia/gpu/ucode.h AluInstruction GetConstant0/1WriteMask;
    // XenosRecomp shader_recompiler.cpp uses the same export-mask rules.
    uint32_t one = in.vectorWriteMask() & in.scalarWriteMask();
    uint32_t zero = in.scalarDestRelative()
        ? (~(in.vectorWriteMask() | in.scalarWriteMask()) & 15) : 0;
    if (!(one | zero)) return;
    uint32_t dst = in.vectorDest();
    if (dst < 16) sc.exportMask |= 1u << dst;
    if (dst == 62) sc.wrotePos = true;
    for (unsigned i = 0; i < 4; ++i)
        if ((one | zero) & (1u << i)) sc.exports[dst][i] = (one & (1u << i)) ? 1.0f : 0.0f;
}

void ExecAlu(ShaderCtx& sc, const Alu& in)
{
    if (in.isPredicated() && sc.p0 != in.predCondition()) return;

    // ---- vector unit ----
    uint32_t vop = in.vectorOpcode();
    V4 a = ReadVecOperand(sc, in, 1), b = ReadVecOperand(sc, in, 2), c = ReadVecOperand(sc, in, 3);
    V4 res;
    bool haveVec = true;
    auto bcast = [](float f) { V4 v; v[0] = v[1] = v[2] = v[3] = f; return v; };
    switch (vop)
    {
    case V_ADD:   for (int i = 0; i < 4; ++i) res[i] = a[i] + b[i]; break;
    case V_MUL:   for (int i = 0; i < 4; ++i) res[i] = a[i] * b[i]; break;
    case V_MAX:   for (int i = 0; i < 4; ++i) res[i] = std::max(a[i], b[i]); break;
    case V_MIN:   for (int i = 0; i < 4; ++i) res[i] = std::min(a[i], b[i]); break;
    case V_SEQ:   for (int i = 0; i < 4; ++i) res[i] = a[i] == b[i] ? 1.0f : 0.0f; break;
    case V_SGT:   for (int i = 0; i < 4; ++i) res[i] = a[i] >  b[i] ? 1.0f : 0.0f; break;
    case V_SGE:   for (int i = 0; i < 4; ++i) res[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
    case V_SNE:   for (int i = 0; i < 4; ++i) res[i] = a[i] != b[i] ? 1.0f : 0.0f; break;
    case V_FRC:   for (int i = 0; i < 4; ++i) res[i] = a[i] - std::floor(a[i]); break;
    case V_TRUNC: for (int i = 0; i < 4; ++i) res[i] = std::trunc(a[i]); break;
    case V_FLOOR: for (int i = 0; i < 4; ++i) res[i] = std::floor(a[i]); break;
    case V_MAD:   for (int i = 0; i < 4; ++i) res[i] = a[i] * b[i] + c[i]; break;
    case V_CNDEQ: for (int i = 0; i < 4; ++i) res[i] = a[i] == 0.0f ? b[i] : c[i]; break;
    case V_CNDGE: for (int i = 0; i < 4; ++i) res[i] = a[i] >= 0.0f ? b[i] : c[i]; break;
    case V_CNDGT: for (int i = 0; i < 4; ++i) res[i] = a[i] >  0.0f ? b[i] : c[i]; break;
    case V_DP4:   res = bcast(a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3]); break;
    case V_DP3:   res = bcast(a[0]*b[0] + a[1]*b[1] + a[2]*b[2]); break;
    case V_DP2ADD:res = bcast(a[0]*b[0] + a[1]*b[1] + c[0]); break;
    case V_MAX4:  res = bcast(std::max(std::max(a[0], a[1]), std::max(a[2], a[3]))); break;
    case V_DST:
        res[0] = 1.0f; res[1] = a[1] * b[1]; res[2] = a[2]; res[3] = b[3]; break;
    case V_MAXA:
        sc.a0 = (int)std::clamp(std::floor(a[3] + 0.5f), -256.0f, 255.0f);
        for (int i = 0; i < 4; ++i) res[i] = std::max(a[i], b[i]);
        break;
    case V_KILL_EQ: case V_KILL_GT: case V_KILL_GE: case V_KILL_NE:
    {
        bool k = false;
        for (int i = 0; i < 4; ++i)
            k |= vop == V_KILL_EQ ? a[i] == b[i] : vop == V_KILL_GT ? a[i] > b[i]
               : vop == V_KILL_GE ? a[i] >= b[i] : a[i] != b[i];
        if (k) sc.killed = true;
        res = bcast(k ? 1.0f : 0.0f);
        break;
    }
    case V_SETP_EQ_PUSH: case V_SETP_NE_PUSH: case V_SETP_GT_PUSH: case V_SETP_GE_PUSH:
    {
        bool cond = vop == V_SETP_EQ_PUSH ? b[0] == 0.0f : vop == V_SETP_NE_PUSH ? b[0] != 0.0f
                  : vop == V_SETP_GT_PUSH ? b[0] >  0.0f : b[0] >= 0.0f;
        sc.p0 = a[3] == 0.0f && cond;
        res = bcast(sc.p0 ? 0.0f : a[0] + 1.0f);
        break;
    }
    case V_CUBE:
    {
        // Operand 1 arrives swizzled .zzxy: x = [2], y = [3], z = [0]. Result: T coordinate,
        // S coordinate, 2 * major axis, face id (Xenia ucode.h kCube / dxbc translator).
        const float x = a[2], y = a[3], z = a[0];
        float tc, scc, ma, id;
        if (std::fabs(z) >= std::fabs(x) && std::fabs(z) >= std::fabs(y))
        { tc = -y; scc = z < 0.0f ? -x : x; ma = 2.0f * z; id = z < 0.0f ? 5.0f : 4.0f; }
        else if (std::fabs(y) >= std::fabs(x))
        { tc = y < 0.0f ? -z : z; scc = x; ma = 2.0f * y; id = y < 0.0f ? 3.0f : 2.0f; }
        else
        { tc = -y; scc = x < 0.0f ? z : -z; ma = 2.0f * x; id = x < 0.0f ? 1.0f : 0.0f; }
        res[0] = tc; res[1] = scc; res[2] = ma; res[3] = id;
        break;
    }
    default:
        haveVec = false;
        break;
    }
    if (haveVec) WriteResult(sc, in, false, res);
    WriteExportConstants(sc, in);

    // ---- scalar unit ----
    uint32_t sop = in.scalarOpcode();
    if (sop == S_RETAIN_PREV)
    {
        // Retain the previous scalar computation, but still honor its write mask.
        V4 previous; previous[0] = previous[1] = previous[2] = previous[3] = sc.ps;
        WriteResult(sc, in, true, previous);
        return;
    }
    float s0, s1;
    if (sop >= S_MULSC0 && sop <= S_SUBSC1)
    {
        // Constant-and-temporary forms: s0 = constant src3 (W slot of its swizzle, constant
        // addressing as the third operand), s1 = temporary register (scalar_opc & 1) |
        // (src3_sel << 1) | (src3_swiz & 0x3C) (X slot). abs_constants and src3's negate apply
        // to both (Xenia ucode.h scalar_const_reg_op_src_temp_reg / shader_translator.cc).
        const uint32_t swz = in.srcSwizzle(3);
        const V4 c = AluConst(sc.regs, ConstIndex(sc, in, 3, in.srcRegister(3)), sc.pixel);
        const uint32_t temp = (sop & 1) | ((in.srcSelect(3) ? 1u : 0u) << 1) | (swz & 0x3C);
        s0 = c[((swz >> 6) + 3) & 3];
        s1 = sc.r[temp & 63][swz & 3];
        if (in.absConstants()) { s0 = std::fabs(s0); s1 = std::fabs(s1); }
        if (in.srcNegate(3)) { s0 = -s0; s1 = -s1; }
    }
    else
    {
        s0 = ReadScalarOperand(sc, in, 0);
        s1 = ReadScalarOperand(sc, in, 1);
    }
    float sr = sc.ps;
    bool haveScalar = true;
    auto safeRcp = [](float f) { return f == 0.0f ? 0.0f : 1.0f / f; };
    switch (sop)
    {
    case S_ADDS:       sr = s0 + s1; break;
    case S_ADDS_PREV:  sr = s0 + sc.ps; break;
    case S_MULS:       sr = s0 * s1; break;
    case S_MULS_PREV:  sr = s0 * sc.ps; break;
    case S_MAXS:       sr = std::max(s0, s1); break;
    case S_MINS:       sr = std::min(s0, s1); break;
    case S_SEQS:       sr = s0 == 0.0f ? 1.0f : 0.0f; break;
    case S_SGTS:       sr = s0 >  0.0f ? 1.0f : 0.0f; break;
    case S_SGES:       sr = s0 >= 0.0f ? 1.0f : 0.0f; break;
    case S_SNES:       sr = s0 != 0.0f ? 1.0f : 0.0f; break;
    case S_FRCS:       sr = s0 - std::floor(s0); break;
    case S_TRUNCS:     sr = std::trunc(s0); break;
    case S_FLOORS:     sr = std::floor(s0); break;
    case S_EXP:        sr = std::exp2(s0); break;
    case S_LOG: case S_LOGC: sr = s0 <= 0.0f ? -3.4e38f : std::log2(s0); break;
    case S_RCP: case S_RCPC: case S_RCPF: sr = safeRcp(s0); break;
    case S_RSQ: case S_RSQC: case S_RSQF:
        sr = s0 <= 0.0f ? 0.0f : 1.0f / std::sqrt(s0); break;
    case S_SQRT:       sr = s0 <= 0.0f ? 0.0f : std::sqrt(s0); break;
    case S_SUBS:       sr = s0 - s1; break;
    case S_SUBS_PREV:  sr = s0 - sc.ps; break;
    case S_SIN:        sr = std::sin(s0); break;
    case S_COS:        sr = std::cos(s0); break;
    case S_MULSC0: case S_MULSC1: sr = s0 * s1; break;
    case S_ADDSC0: case S_ADDSC1: sr = s0 + s1; break;
    case S_SUBSC0: case S_SUBSC1: sr = s0 - s1; break;
    case S_MAXAS: case S_MAXASF:
        sc.a0 = (int)std::clamp(sop == S_MAXAS ? std::floor(s0 + 0.5f) : std::floor(s0),
                                -256.0f, 255.0f);
        sr = std::max(s0, s1);
        break;
    case S_SETP_EQ: sc.p0 = s0 == 0.0f; sr = sc.p0 ? 0.0f : 1.0f; break;
    case S_SETP_NE: sc.p0 = s0 != 0.0f; sr = sc.p0 ? 0.0f : 1.0f; break;
    case S_SETP_GT: sc.p0 = s0 >  0.0f; sr = sc.p0 ? 0.0f : 1.0f; break;
    case S_SETP_GE: sc.p0 = s0 >= 0.0f; sr = sc.p0 ? 0.0f : 1.0f; break;
    case S_SETP_INV:
        sc.p0 = s0 == 1.0f; sr = sc.p0 ? 0.0f : (s0 == 0.0f ? 1.0f : s0); break;
    case S_SETP_CLR: sc.p0 = false; sr = -3.4e38f; break;
    case S_SETP_RSTR: sc.p0 = s0 == 0.0f; sr = s0; break;
    case S_KILLS_EQ: case S_KILLS_GT: case S_KILLS_GE: case S_KILLS_NE: case S_KILLS_ONE:
    {
        bool k = sop == S_KILLS_EQ ? s0 == 0.0f : sop == S_KILLS_GT ? s0 > 0.0f
               : sop == S_KILLS_GE ? s0 >= 0.0f : sop == S_KILLS_NE ? s0 != 0.0f : s0 == 1.0f;
        if (k) sc.killed = true;
        sr = k ? 1.0f : 0.0f;
        break;
    }
    default:
        haveScalar = false;
        break;
    }
    if (!haveScalar) return;
    sc.ps = sr;
    V4 sv; sv[0] = sv[1] = sv[2] = sv[3] = sr;
    WriteResult(sc, in, true, sv);
}

void ExecVertexFetch(ShaderCtx& sc, const VFetch& in)
{
    uint32_t slot = in.isMiniFetch() ? sc.lastFetchSlot
                                     : in.constIndex() * 3 + in.constIndexSel();
    if (slot >= 96) return;
    const uint32_t* fc = &sc.regs[REG_FETCH_CONST + slot * 2];
    if ((fc[0] & 3) != 3) return;                        // type 3 == vertex fetch constant
    uint32_t base = PhysToGuest(fc[0] & ~3u);
    uint32_t stride = in.isMiniFetch() ? sc.lastStride : in.stride();
    if (!stride) stride = sc.lastStride;
    if (!in.isMiniFetch()) { sc.lastFetchSlot = slot; sc.lastStride = stride; }
    float fidx = sc.r[in.srcRegister()][in.srcSwizzle() & 3];
    uint32_t index = (uint32_t)std::max(0.0f, std::floor(fidx + 0.5f));
    uint32_t addr = base + index * stride * 4 + in.offset() * 4;
    {
        // Format census (TTT2_FORMAT_CENSUS=1): first CPU-side vertex fetch of each format.
        static const bool census = [] { const char* e = std::getenv("TTT2_FORMAT_CENSUS"); return e && *e == '1'; }();
        static std::atomic<uint64_t> seen{0};
        const uint64_t bit = 1ull << (in.format() & 63);
        if (census && !(seen.fetch_or(bit) & bit))
            std::fprintf(stderr, "[census] cpu vertex fetch fmt=%u (first: stride %u)\n", in.format(), stride);
    }
    V4 v = UnpackFormat(addr, in.format(), in.numFormatAll(), in.signedRfMode(), true);
    if (in.expAdjust()) { float m = std::exp2((float)in.expAdjust());
                          for (int i = 0; i < 4; ++i) v[i] *= m; }
    V4& dst = sc.r[in.dstRegister()];
    uint32_t dsw = in.dstSwizzle();
    for (int i = 0; i < 4; ++i)
    {
        uint32_t s = (dsw >> (i * 3)) & 7;
        if (s < 4) dst[i] = v[s];
        else if (s == 4) dst[i] = 0.0f;
        else if (s == 5) dst[i] = 1.0f;
    }
}

void ExecTextureFetch(ShaderCtx& sc, const TFetch& in)
{
    if (in.opcode() != 1) return;                        // only plain tfetch for now
    Texture t = DecodeTexture(sc.regs, in.constIndex());
    DumpTextureBmp(t);
    const V4& src = sc.r[in.srcRegister()];
    float u = src[in.srcSwizzle() & 3];
    float v = src[(in.srcSwizzle() >> 2) & 3];
    if (in.dimension() == 3 && t.valid && t.dimension == 3)
    {
        // Cube fetch (107 TTT2 pixel shaders - character/environment lighting). The shader
        // feeds (s, t, face) from CUBE's output scaled by 1/|2ma| and biased by 1.5, so s and t
        // span [1, 2] across the face; faces are 2D surfaces in D3D order (+X -X +Y -Y +Z -Z)
        // sampled clamped at their edges. Mirrored exactly by gpu_hw_translate.h.
        const float f = src[(in.srcSwizzle() >> 4) & 3];
        const int face = std::clamp(TexFloor(f + 0.5f), 0, 5);
        t.base += uint32_t(face) * TextureFaceStride(t);
        t.clampX = t.clampY = 2;
        u -= 1.0f; v -= 1.0f;
    }
    else if (in.coordDenorm()) { u /= (float)t.width; v /= (float)t.height; }
    // Filter 3 defers to the fetch constant (every TTT2 tfetch does); then 1 = bilinear.
    const uint32_t mag = in.magFilter() == 3 ? t.magFilter : in.magFilter();
    V4 s = SampleTexture(t, u, v, mag == 1);
    V4& dst = sc.r[in.dstRegister()];
    uint32_t dsw = in.dstSwizzle();
    for (int i = 0; i < 4; ++i)
    {
        uint32_t sw = (dsw >> (i * 3)) & 7;
        if (sw < 4) dst[i] = s[sw];
        else if (sw == 4) dst[i] = 0.0f;
        else if (sw == 5) dst[i] = 1.0f;
    }
}

// Walk the control-flow program and execute it. `code` is the microcode as host dwords.
void RunShader(ShaderCtx& sc, const std::vector<uint32_t>& code)
{
    if (code.size() < 3) return;
    // The CF block runs until the first instruction any EXEC references: instruction N
    // lives at dword N*3, and CF instructions come in groups of 3 dwords (2 per group).
    uint32_t sizeBytes = (uint32_t)code.size() * 4;
    uint32_t cfBytes = sizeBytes;
    for (uint32_t off = 0; off + 12 <= sizeBytes && off < cfBytes; off += 12)
    {
        Cf a, b;
        DecodeCfPair(&code[off / 4], a, b);
        for (const Cf* p : { &a, &b })
            if (p->isExec() && p->addr()) cfBytes = std::min(cfBytes, p->addr() * 12);
    }

    // Control flow with a program counter over CF instruction indices (2 per 3-dword group).
    // Semantics: Xenia ucode.h / shader.h (IPR2015-00325 sequencer specification):
    //  - COND_EXEC* and COND_EXEC_PRED* ends only when its condition is met;
    //  - COND_EXEC_PRED_CLEAN(_END) (13/14) are conditional on a BOOL CONSTANT, not p0;
    //  - LOOP_START pushes {count, aL}; aL = start unless "repeat"; count 0 skips the loop;
    //  - LOOP_END steps aL and decrements first, then loops back unless the count ran out
    //    or a predicated break fires; leaving restores the outer aL;
    //  - COND_CALL / RETURN use a 4-deep return stack (RETURN outside a call is a no-op);
    //  - COND_JMP jumps to a CF index. Calls, jumps and loops: unconditional, p0- or bool-gated.
    const uint32_t cfCount = cfBytes / 12 * 2;
    auto cfAt = [&](uint32_t index) {
        Cf pair[2];
        DecodeCfPair(&code[(index / 2) * 3], pair[0], pair[1]);
        return pair[index & 1];
    };
    auto boolConst = [&](uint32_t ba) {
        return ((sc.regs[REG_BOOL_CONST + (ba >> 5)] >> (ba & 31)) & 1) != 0;
    };
    struct LoopFrame { int32_t count; int32_t savedAL; };
    LoopFrame loops[4];
    uint32_t loopDepth = 0, callStack[4], callDepth = 0;
    uint32_t pc = 0;
    for (uint32_t steps = 0; pc < cfCount; ++steps)
    {
        if (steps > 65536)                    // a malformed program must not hang the CP
        {
            if (LogOn()) { LogUsed(); std::fprintf(stderr, "[gpu] shader: control-flow step limit, stopping\n"); }
            return;
        }
        const Cf cf = cfAt(pc);
        const uint32_t op = cf.op();
        if (cf.isExec())
        {
            bool run = true;
            if (op == CF_COND_EXEC || op == CF_COND_EXEC_END ||
                op == CF_COND_EXEC_PRED_CLEAN || op == CF_COND_EXEC_PRED_CLEAN_END)
                run = boolConst(cf.boolAddr()) == (cf.cond() != 0);
            else if (op == CF_COND_EXEC_PRED || op == CF_COND_EXEC_PRED_END)
                run = sc.p0 == (cf.cond() != 0);
            if (run)
            {
                uint32_t seq = cf.seq();
                for (uint32_t i = 0; i < cf.count(); ++i)
                {
                    uint32_t idx = cf.addr() + i;
                    if ((idx + 1) * 3 > code.size()) break;
                    const uint32_t* w = &code[idx * 3];
                    if ((seq >> (i * 2)) & 1)
                    {
                        if ((w[0] & 0x1F) == 0) { VFetch f{ w[0], w[1], w[2] }; ExecVertexFetch(sc, f); }
                        else                    { TFetch f{ w[0], w[1], w[2] }; ExecTextureFetch(sc, f); }
                    }
                    else
                    {
                        Alu in{ w[0], w[1], w[2] };
                        ExecAlu(sc, in);
                    }
                }
                if (cf.isEnd()) return;
            }
            ++pc;
            continue;
        }
        const uint32_t target = cf.lo & 0x1FFF;
        switch (op)
        {
        case CF_LOOP_START:
        {
            const uint32_t loopConst = sc.regs[REG_LOOP_CONST + ((cf.lo >> 16) & 0x1F)];
            const int32_t count = int32_t(loopConst & 0xFF);
            if (loopDepth == 4 || count == 0) { pc = target; break; }
            loops[loopDepth++] = { count, sc.aL };
            if (!((cf.lo >> 13) & 1)) sc.aL = int32_t((loopConst >> 8) & 0xFF);
            ++pc;
            break;
        }
        case CF_LOOP_END:
        {
            if (!loopDepth) { ++pc; break; }
            const uint32_t loopConst = sc.regs[REG_LOOP_CONST + ((cf.lo >> 16) & 0x1F)];
            LoopFrame& f = loops[loopDepth - 1];
            sc.aL += int32_t(int8_t(loopConst >> 16));
            --f.count;
            const bool brk = ((cf.lo >> 21) & 1) && sc.p0 == (cf.cond() != 0);
            if (f.count > 0 && !brk) pc = target;
            else { sc.aL = f.savedAL; --loopDepth; ++pc; }
            break;
        }
        case CF_COND_CALL:
        case CF_COND_JMP:
        {
            const bool unconditional = (cf.lo >> 13) & 1, predicated = (cf.lo >> 14) & 1;
            const bool take = unconditional ||
                (predicated ? sc.p0 == (cf.cond() != 0) : boolConst(cf.boolAddr()) == (cf.cond() != 0));
            if (!take) { ++pc; break; }
            if (op == CF_COND_CALL)
            {
                if (callDepth == 4) { ++pc; break; }
                callStack[callDepth++] = pc + 1;
            }
            pc = target;
            break;
        }
        case CF_RETURN:
            pc = callDepth ? callStack[--callDepth] : pc + 1;
            break;
        default:                              // NOP, ALLOC, MARK_VS_FETCH_DONE
            ++pc;
            break;
        }
    }
}

std::vector<uint32_t> LoadUcode(const GpuShader& s)
{
    std::vector<uint32_t> v;
    if (!s.addr || !s.size || s.size > 8192) return v;
    v.resize(s.size);
    for (uint32_t i = 0; i < s.size; ++i) v[i] = GRead32(g_base, s.addr + i * 4);
    return v;
}

// --------------------------------------------------------------------------------------
// EDRAM-lite
// --------------------------------------------------------------------------------------
// EDRAM is a flat sample store, not a per-pass image: a pass may render 640x360 with 4x
// multisampling and a later pass resolve the same bytes as a 1280x720 single-sampled
// surface (that is exactly what TTT2's clear + resolve pair does). So the model is one
// flat array per colour base, indexed by sample position, and every consumer applies its
// own pitch and sample grid to it.
constexpr size_t EDRAM_PIXELS = 1280u * 2048u;      // 10 MiB at 4 bytes per sample

struct Surface
{
    std::vector<uint32_t> px;      // 0xAARRGGBB, flat sample store
    // Samples per row, as the rasterizer that last filled this base laid them out
    // (RB_SURFACE_INFO pitch x the horizontal sample count). A resolve must walk the store
    // with THIS stride, not with its own destination pitch: TTT2's 680-wide 4x-multisampled
    // pass stores 1360 samples per row and resolves to a destination whose pitch is 1376
    // (1360 rounded up to 32), and reading it back at 1376 shears the image by 16 pixels a row.
    uint32_t samplePitch = 0;
};
std::unordered_map<uint32_t, Surface> g_surfaces;

Surface& GetSurface(uint32_t colorBase)
{
    Surface& s = g_surfaces[colorBase];
    if (s.px.empty()) s.px.assign(EDRAM_PIXELS, 0);
    return s;
}

uint32_t PackArgb(const V4& c)
{
    __m128 value = _mm_loadu_ps(c.c);
    if (_mm_movemask_ps(_mm_cmpunord_ps(value, value)))
    {
        // Preserve the CRT's existing NaN conversion rather than changing the
        // resulting packed bits through SIMD saturation.
        auto q = [](float f) { return (uint32_t)std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f); };
        return (q(c[3]) << 24) | (q(c[0]) << 16) | (q(c[1]) << 8) | q(c[2]);
    }
    value = _mm_max_ps(_mm_setzero_ps(), _mm_min_ps(_mm_set1_ps(1.0f), value));
    value = _mm_mul_ps(value, _mm_set1_ps(255.0f));
    __m128i integer = _mm_cvttps_epi32(value);
    // Positive lround is truncate + (fraction >= .5). Adding .5 first can
    // round a value just BELOW a boundary upward; ties-to-even conversion also
    // differs. Subtracting the integer part is exact in this bounded range.
    __m128 fraction = _mm_sub_ps(value, _mm_cvtepi32_ps(integer));
    integer = _mm_sub_epi32(integer, _mm_castps_si128(_mm_cmpge_ps(fraction, _mm_set1_ps(.5f))));
    integer = _mm_shuffle_epi32(integer, _MM_SHUFFLE(3, 0, 1, 2)); // RGBA -> BGRA bytes.
    integer = _mm_packs_epi32(integer, _mm_setzero_si128());
    integer = _mm_packus_epi16(integer, _mm_setzero_si128());
    return uint32_t(_mm_cvtsi128_si32(integer));
}

float BlendFactor(uint32_t f, float src, float dst, float srcA, float dstA, float constC,
                  float constA, bool alphaChannel)
{
    switch (f)
    {
    case 0:  return 0.0f;                       // ZERO
    case 1:  return 1.0f;                       // ONE
    case 4:  return alphaChannel ? srcA : src;  // SRC_COLOR
    case 5:  return 1.0f - (alphaChannel ? srcA : src);
    case 6:  return srcA;                       // SRC_ALPHA
    case 7:  return 1.0f - srcA;
    case 8:  return alphaChannel ? dstA : dst;  // DST_COLOR
    case 9:  return 1.0f - (alphaChannel ? dstA : dst);
    case 10: return dstA;                       // DST_ALPHA
    case 11: return 1.0f - dstA;
    case 12: return alphaChannel ? constA : constC;
    case 13: return 1.0f - (alphaChannel ? constA : constC);
    case 14: return constA;
    case 15: return 1.0f - constA;
    case 16: return alphaChannel ? 1.0f : std::min(srcA, 1.0f - dstA);
    default: return 1.0f;
    }
}

} // namespace

// --------------------------------------------------------------------------------------
// public entry points
// --------------------------------------------------------------------------------------
void GpuFillEdram(uint32_t colorBase, uint32_t pitch, uint32_t height, uint32_t argb)
{
    if (!pitch || !height || pitch > 4096 || height > 4096) return;
    Surface& s = GetSurface(colorBase);
    size_t n = std::min(EDRAM_PIXELS, (size_t)pitch * height);
    std::fill(s.px.begin(), s.px.begin() + n, argb);
}

uint64_t GpuLitDraws() { return g_litDraws.load(std::memory_order_relaxed); }

bool GpuReadEdram(uint32_t colorBase, uint32_t pitch, uint32_t height, uint32_t* out)
{
    auto it = g_surfaces.find(colorBase);
    if (it == g_surfaces.end() || it->second.px.empty()) return false;
    const Surface& s = it->second;
    uint32_t src = s.samplePitch ? s.samplePitch : pitch;
    uint32_t width = std::min(src, pitch);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            size_t i = (size_t)y * src + x;
            out[(size_t)y * pitch + x] = i < s.px.size() ? s.px[i] : 0;
        }
    return true;
}


namespace
{

// Xenos primitive types (VGT_DRAW_INITIATOR bits 0-5).
enum Prim
{
    PRIM_POINT_LIST = 1, PRIM_LINE_LIST = 2, PRIM_LINE_STRIP = 3, PRIM_TRI_LIST = 4,
    PRIM_TRI_FAN = 5, PRIM_TRI_STRIP = 6, PRIM_RECT_LIST = 8,
    PRIM_QUAD_LIST = 0x0D, PRIM_QUAD_STRIP = 0x0E
};

struct Vertex
{
    float x = 0, y = 0, z = 0, invW = 1;    // window space; invW carries 1/w for interpolation
    V4 attr[16];                            // interpolators, already divided by w
    bool valid = false;
};

// Depth lives in its own EDRAM region (RB_DEPTH_INFO base), so it gets its own flat store
// keyed the same way the colour store is.
std::unordered_map<uint32_t, std::vector<float>> g_depth;

std::vector<float>& GetDepth(uint32_t base)
{
    auto& v = g_depth[base];
    if (v.empty()) v.assign(EDRAM_PIXELS, 1.0f);
    return v;
}

bool DepthCompare(uint32_t func, float src, float dst)
{
    switch (func)
    {
    case 0: return false;                 // NEVER
    case 1: return src <  dst;            // LESS
    case 2: return src == dst;            // EQUAL
    case 3: return src <= dst;            // LEQUAL
    case 4: return src >  dst;            // GREATER
    case 5: return src != dst;            // NOTEQUAL
    case 6: return src >= dst;            // GEQUAL
    default: return true;                 // ALWAYS
    }
}

} // namespace

#if defined(TTT2_DRAW_CAPTURE_DIAGNOSTICS)
#include "../../../tools/d3d11_capture.h"
#include "gpu_draw_profile.h"
#endif
#if defined(TTT2_D3D11_MOVIE_BACKEND)
#include "gpu_d3d11_movie.h"
#endif
#if defined(TTT2_GPU_HW_BACKEND)
#include "gpu_hw_d3d11.h"
#endif

// main.cpp --build-shaders: the game's shaders into the disk cache ahead of time (gpu_hw_d3d11.h).
int GpuPrebuildShaders(const std::vector<std::filesystem::path>& files, unsigned threads)
{
#if defined(TTT2_GPU_HW_BACKEND)
    return ttt2_hw::Backend::PrebuildShaders(files, threads);
#else
    (void)files; (void)threads;
    return 3;
#endif
}
// main.cpp --translate vs|ps <file>: the HLSL for one microcode file (host-order dwords).
int GpuTranslateFile(const char* kind, const char* path)
{
#if defined(TTT2_GPU_HW_BACKEND)
    FILE* f = std::fopen(path, "rb");
    if (!f) return 2;
    std::vector<uint32_t> code;
    uint32_t w;
    while (std::fread(&w, 4, 1, f) == 1) code.push_back(w);
    std::fclose(f);
    if (std::strcmp(kind, "vs") == 0)
    {
        const ttt2_hw::VsTranslation t = ttt2_hw::TranslateVertexShader(code);
        if (!t.unsupported.empty()) { std::printf("// unsupported: %s\n", t.unsupported.c_str()); return 1; }
        const std::string hlsl = ttt2_hw::VertexShaderHlsl(t);
        std::printf("// text key %016llX, %zu fetches\n%s", (unsigned long long)ttt2_hw::Fnv(hlsl.data(), hlsl.size()), t.fetches.size(), hlsl.c_str());
    }
    else
    {
        const ttt2_hw::PsTranslation t = ttt2_hw::TranslatePixelShader(code);
        if (!t.unsupported.empty()) { std::printf("// unsupported: %s\n", t.unsupported.c_str()); return 1; }
        const std::string hlsl = ttt2_hw::PixelShaderHlsl(t);
        std::printf("// text key %016llX\n%s", (unsigned long long)ttt2_hw::Fnv(hlsl.data(), hlsl.size()), hlsl.c_str());
    }
    return 0;
#else
    (void)kind; (void)path;
    return 3;
#endif
}

// main.cpp: has this exe's prebuild completed for this user's cache?
bool GpuShaderPrebuildCurrent()
{
#if defined(TTT2_GPU_HW_BACKEND)
    return ttt2_hw::Backend::PrebuildCurrent();
#else
    return true;
#endif
}

bool GpuExecuteDraw(const uint32_t* regs, uint32_t initiator, uint32_t indexAddr,
                    bool index32, const GpuShader& vsRef, const GpuShader& psRef)
{
#if defined(TTT2_DRAW_CAPTURE_DIAGNOSTICS)
    Ttt2CaptureD3D11Draw(regs, initiator, indexAddr, index32, vsRef, psRef);
#endif
    uint32_t prim = initiator & 0x3F;
    uint32_t srcSel = (initiator >> 6) & 3;
    uint32_t numIdx = initiator >> 16;
    if (!numIdx || numIdx > 65535) return false;
    if ((regs[0x2208] & 7) == 6) return false;                    // resolve pass, not geometry
    uint32_t writeMask = regs[0x2104] & 0xF;

    bool tri = prim == PRIM_TRI_LIST || prim == PRIM_TRI_FAN || prim == PRIM_TRI_STRIP ||
               prim == PRIM_QUAD_LIST;
    if (prim != PRIM_RECT_LIST && !tri) return false;             // points and lines: later

    std::vector<uint32_t> vsCode = LoadUcode(vsRef), psCode = LoadUcode(psRef);
    if (vsCode.empty()) return false;
#if defined(TTT2_DRAW_CAPTURE_DIAGNOSTICS)
    GpuDrawProfileScope drawProfile(psCode.data(), psCode.size(), prim, numIdx);
#endif

    uint32_t pitch = regs[0x2000] & 0x3FFF;
    uint32_t colorBase = regs[0x2001] & 0xFFF;
    if (!pitch || pitch > 4096) return false;
    uint32_t msaa = (regs[0x2000] >> 16) & 3;
    uint32_t sampX = msaa == 2 ? 2 : 1, sampY = msaa >= 1 ? 2 : 1;
    uint32_t samplePitch = pitch * sampX;

    // ---- index list ----
    // Vertex index = ((raw + VGT_INDX_OFFSET) & 0xFFFFFF) clamped to [VGT_MIN_VTX_INDX,
    // VGT_MAX_VTX_INDX]; with PA_SU_SC_MODE_CNTL.multi_prim_ib_ena a RAW index equal to
    // VGT_MULTI_PRIM_IB_RESET_INDX cuts strips/fans instead of naming a vertex (Xenia
    // dxbc_shader_translator / primitive processor). Measured 2026-10-10: the battle's skinned
    // tri strips use restarts; drawing 0xFFFF as a vertex fanned spikes from one point.
    const bool restartOn = ((regs[0x2205] >> 21) & 1) && srcSel != 2 && indexAddr;
    const uint32_t restartIdx = index32 ? regs[0x2103] : (regs[0x2103] & 0xFFFF);
    const uint32_t indexOffset = regs[0x2102];
    const uint32_t minIndex = regs[0x2101] & 0xFFFFFF, maxIndex = regs[0x2100] & 0xFFFFFF;
    const bool clampIndex = maxIndex != 0 && maxIndex >= minIndex;
    std::vector<uint32_t> idx(numIdx);
    std::vector<uint8_t> cut(numIdx, 0);
    bool anyCut = false;
    extern void GpuNoteMemory(int kind, uint32_t addr, uint32_t bytes);
    if (srcSel != 2 && indexAddr) GpuNoteMemory(2, indexAddr, numIdx * (index32 ? 4 : 2));
    for (uint32_t i = 0; i < numIdx; ++i)
    {
        uint32_t raw;
        if (srcSel == 2 || !indexAddr) raw = i;                   // auto index
        else if (index32) raw = GRead32(g_base, indexAddr + i * 4);
        else raw = GRead16(g_base, indexAddr + i * 2);
        if (restartOn && raw == restartIdx) { cut[i] = 1; anyCut = true; idx[i] = 0; continue; }
        uint32_t v = (raw + indexOffset) & 0xFFFFFF;
        if (clampIndex) v = std::min(std::max(v, minIndex), maxIndex);
        idx[i] = v;
    }

    // ---- vertex stage (one shader run per distinct index) ----
    uint32_t maxIdx = 0;
    for (uint32_t i = 0; i < numIdx; ++i) if (!cut[i]) maxIdx = std::max(maxIdx, idx[i]);
    if (maxIdx > (1u << 20)) return false;

    uint32_t vte = regs[0x2206];
    float xs = AsFloat(regs[0x210F]), xo = AsFloat(regs[0x2110]);
    float ys = AsFloat(regs[0x2111]), yo = AsFloat(regs[0x2112]);
    float zs = AsFloat(regs[0x2113]), zo = AsFloat(regs[0x2114]);
    uint32_t interpCount = 0;
    int rawPosLogged = 0;

    // Run the vertex shader for one index (pure: reads registers and guest memory only).
    auto shadeVertex = [&](uint32_t index, Vertex& v, uint32_t& interp, bool mayLog) {
        ShaderCtx sc;
        sc.regs = regs;
        sc.r[0][0] = (float)index;
        RunShader(sc, vsCode);
        if (sc.wrotePos)
        {
            const V4& p = sc.exports[62];
            // The raw shader output, before any viewport work - this is what separates "the
            // shader produced nonsense" from "our viewport transform is wrong".
            if (mayLog && rawPosLogged < 4 && (LogOn() || LogPrimOn(prim)))
            {
                ++rawPosLogged;
                std::fprintf(stderr, "[gpu]   vs[%u] oPos=(%g,%g,%g,%g) r0=(%g,%g,%g,%g)\n",
                             index, p[0], p[1], p[2], p[3],
                             sc.r[0][0], sc.r[0][1], sc.r[0][2], sc.r[0][3]);
            }
            // PA_CL_VTE_CNTL (AMD Evergreen register docs, as quoted in Xenia's
            // d3d12_command_processor.cc): bits 0-5 enable the viewport scale/offset terms;
            //   bit 8  VTX_XY_FMT = 1: X,Y already multiplied by 1/W0, else multiply them;
            //   bit 9  VTX_Z_FMT  = 1: Z already multiplied by 1/W0, else multiply it;
            //   bit 10 VTX_W0_FMT = 1: W0 is W - take the reciprocal; 0: W0 already is 1/W0.
            // Corrected 2026-10-10: bit 10 was read the other way round and Z followed the XY
            // flag. The 2D UI (0x300: pre-divided, W0 = 1) never noticed; every 3D draw (0x43F)
            // multiplied by W instead of dividing, which put the whole battle scene off-screen.
            const float w0 = p[3];
            const float inv = (vte & 0x400) ? (w0 == 0.0f ? 1.0f : 1.0f / w0) : w0;
            float x = p[0], y = p[1], z = p[2];
            if (!(vte & 0x100)) { x *= inv; y *= inv; }
            if (!(vte & 0x200)) z *= inv;
            if (vte & 0x01) x *= xs;
            if (vte & 0x02) x += xo;
            if (vte & 0x04) y *= ys;
            if (vte & 0x08) y += yo;
            if (vte & 0x10) z *= zs;
            if (vte & 0x20) z += zo;
            v.x = x; v.y = y; v.z = z;
            v.invW = inv;                         // perspective-correct interpolation uses 1/W0
            for (uint32_t k = 0; k < 16; ++k)
                if (sc.exportMask & (1u << k))
                {
                    interp = std::max(interp, k + 1);
                    for (int c = 0; c < 4; ++c) v.attr[k][c] = sc.exports[k][c] * v.invW;
                }
            v.valid = true;
        }
    };

    // Each distinct index is shaded once, in first-use order, before assembly. Large draws fan
    // the vertex shader out over the raster pool: per-vertex work is independent and its
    // inputs read-only (measured 2026-10-10: the battle submits ~145K triangles a frame and the
    // single-threaded interpreter on the CP thread was the frame's critical path).
    thread_local std::vector<int32_t> slotOfIndex;
    thread_local std::vector<uint32_t> uniqueIdx;
    thread_local std::vector<Vertex> shaded;
    thread_local std::vector<uint8_t> shadedInterp;
    bool shadedReady = false;
    auto ensureShaded = [&] {
        if (shadedReady) return;
        shadedReady = true;
        slotOfIndex.assign(size_t(maxIdx) + 1, -1);
        uniqueIdx.clear();
        for (uint32_t i = 0; i < numIdx; ++i)
        {
            if (cut[i]) continue;
            const uint32_t v = idx[i];
            if (slotOfIndex[v] < 0) { slotOfIndex[v] = (int32_t)uniqueIdx.size(); uniqueIdx.push_back(v); }
        }
        const size_t n = uniqueIdx.size();
        if (shaded.size() < n) shaded.resize(n);
        shadedInterp.assign(n, 0);
        // Bind THIS thread's buffers: inside a lambda running on a pool worker, the names of
        // thread_local variables resolve to the worker's own (empty) instances.
        Vertex* const out = shaded.data();
        uint8_t* const outInterp = shadedInterp.data();
        const uint32_t* const indices = uniqueIdx.data();
        auto one = [&, out, outInterp, indices](size_t i, bool mayLog) {
            out[i] = Vertex{};
            uint32_t ic = 0;
            shadeVertex(indices[i], out[i], ic, mayLog);
            outInterp[i] = (uint8_t)ic;
        };
        if (n >= 128)
        {
            const std::function<void(int)> row = [&](int i) { one(size_t(i), false); };
            RasterPool::Get().ForRows(0, (int)n, row);
        }
        else for (size_t i = 0; i < n; ++i) one(i, true);
        for (size_t i = 0; i < n; ++i) interpCount = std::max<uint32_t>(interpCount, shadedInterp[i]);
    };
    auto fetchVertex = [&](uint32_t index) -> const Vertex& {
        ensureShaded();
        return shaded[size_t(slotOfIndex[index])];
    };

#if defined(TTT2_GPU_HW_BACKEND)
    // Hardware backend: the CPU keeps the vertex stage, assembly and culling; triangles go to
    // the GPU instead of the rasterizer (compare mode does both).
    const ttt2_hw::Mode hwMode = ttt2_hw::ActiveMode();
    const bool hwDraw = hwMode != ttt2_hw::Mode::Off;
    const bool cpuRaster = hwMode != ttt2_hw::Mode::On;
    // Triangles for the GPU as slots into `shaded`; synthesized rectangle corners are appended
    // after the shaded vertices (slot >= uniqueIdx.size()).
    thread_local std::vector<uint32_t> hwSlots;
    thread_local std::vector<Vertex> hwExtra;
    hwSlots.clear();
    hwExtra.clear();
    auto hwSlot = [&](const Vertex& v) -> uint32_t {
        const Vertex* p = &v;
        if (p >= shaded.data() && p < shaded.data() + uniqueIdx.size()) return uint32_t(p - shaded.data());
        hwExtra.push_back(v);
        return uint32_t(uniqueIdx.size() + hwExtra.size() - 1);
    };
#else
    constexpr bool hwDraw = false, cpuRaster = true;
#endif

#if defined(TTT2_D3D11_MOVIE_BACKEND)
    // Keep the game's vertex program and primitive order. The hardware helper
    // accepts only validated movie draws and commits the surface after successful
    // readback; any unsupported state or device failure continues through CPU.
    static const bool movieHardwareBudgetAllowed = std::getenv("TTT2_MAXPIX") == nullptr;
    if (!hwDraw && movieHardwareBudgetAllowed && prim == PRIM_QUAD_LIST && numIdx == 4 &&
        psCode.size() == 33 && D3D11MovieEnabled())
    {
        Vertex quad[4];
        for (uint32_t i = 0; i < 4; ++i) quad[i] = fetchVertex(idx[i]);
        if (TryD3D11MovieDraw(regs, psCode, quad, 4, GetSurface(colorBase))) return true;
    }
#endif

    // ---- fixed state for the pixel stage ----
    uint32_t blend = regs[0x2201];
    uint32_t srcF = blend & 0x1F, combFn = (blend >> 5) & 0x7, dstF = (blend >> 8) & 0x1F;
    uint32_t srcFA = (blend >> 16) & 0x1F, combFnA = (blend >> 21) & 0x7,
             dstFA = (blend >> 24) & 0x1F;
    // RB_BLEND_RED/GREEN/BLUE/ALPHA are four separate registers (0x2105..0x2108); the red
    // value had been serving for all three colour channels.
    float constRGB[4] = { AsFloat(regs[0x2105]), AsFloat(regs[0x2106]),
                          AsFloat(regs[0x2107]), AsFloat(regs[0x2108]) };
    float constA = AsFloat(regs[0x2108]);
    // Factors (1,0) only make blending an overwrite when the combine function is ADD -
    // REVSUBTRACT with (1,0) is -src and MIN/MAX ignore factors altogether.
    bool passthrough = srcF == 1 && dstF == 0 && srcFA == 1 && dstFA == 0 &&
                       combFn == 0 && combFnA == 0;

    uint32_t depthCtl = regs[0x2200];
    // Z_WRITE_ENABLE is effective only while Z_ENABLE is set (Xenia's
    // d3d12/pipeline_cache.cc normalizes depth_write under z_enable too).
    bool zEnable = ((depthCtl >> 1) & 1) != 0, zWrite = zEnable && ((depthCtl >> 2) & 1) != 0;
    uint32_t zFunc = (depthCtl >> 4) & 7;
    uint32_t depthBase = regs[0x2002] & 0xFFF;
    // A depth-only pass still has to run: it is how the game clears Z.
    bool colorPass = writeMask != 0;
    // Stencil (RB_DEPTHCONTROL bit 0; ref / masks in RB_STENCILREFMASK): a stencil-only pass,
    // with neither colour nor depth writes, still marks pixels for later passes.
    const bool stencilEnable = (depthCtl & 1) != 0;
    const bool stencilWrite = stencilEnable && ((regs[0x210D] >> 16) & 0xFF) != 0;
    if (!colorPass && !zWrite && !stencilWrite) return false;

    uint32_t cull = regs[0x2205] & 3;            // PA_SU_SC_MODE_CNTL: 1 = front, 2 = back
    bool frontIsCW = ((regs[0x2205] >> 2) & 1) != 0;

    static Surface noCpuSurface;                 // hardware-only draws never reach shade()
    Surface& surf = cpuRaster ? GetSurface(colorBase) : noCpuSurface;
    if (cpuRaster) surf.samplePitch = samplePitch;
    std::vector<float>* zbuf = (cpuRaster && (zEnable || zWrite)) ? &GetDepth(depthBase) : nullptr;

    // Nothing bounded rasterization in y: the EDRAM store is 2048 rows deep, so the oversized
    // full-screen triangle shaded 1,382,400 pixels into a 1280x720 surface - its whole unclipped
    // area - instead of 921,600. PA_SC_WINDOW_SCISSOR is what bounds it on hardware. Guard the
    // degenerate case: a scissor the game has not set yet reads as all zeros, and honouring that
    // literally would cull every draw.
    int scX0 = 0, scY0 = 0, scX1 = (int)pitch, scY1 = 2047;
    uint32_t scTL = regs[0x2081], scBR = regs[0x2082];
    int brX = (int)(scBR & 0x3FFF), brY = (int)((scBR >> 16) & 0x3FFF);
    int tlX = (int)(scTL & 0x3FFF), tlY = (int)((scTL >> 16) & 0x3FFF);
    if (brX > tlX && brY > tlY)
    {
        scX0 = std::max(scX0, tlX); scY0 = std::max(scY0, tlY);
        scX1 = std::min(scX1, brX); scY1 = std::min(scY1, brY);
    }

    static const uint64_t pixelBudget = [] {
        const char* s = std::getenv("TTT2_MAXPIX");
        return s ? std::strtoull(s, nullptr, 0) : 8ull * 1024 * 1024;
    }();
    // Triangles complete synchronously. Count pixels locally per row, then merge on the
    // command processor before the next triangle/budget check. Sharing per-pixel atomics
    // here serialized the raster workers on the same cache line millions of times a draw.
    struct RowStats
    {
        uint64_t written = 0, covered = 0, nonBlack = 0;
        uint32_t firstColor = 0, firstDst = 0;
        V4 firstShaded;
    };
    std::vector<RowStats> rowStats;
    uint64_t written = 0, covered = 0, nonBlack = 0;
    uint32_t firstColor = 0, firstDst = 0;
    V4 firstShaded;
    // A pixel shader's only per-pixel inputs are its interpolators and its texture fetches, so a
    // draw with neither shades every pixel the same colour. The opening video's composite is
    // exactly that - `MAXv oC0 = c0, c0` with interps=0 - and it covers the whole screen, so
    // running it once instead of 1.4 M times is most of that pass. Decided (and, when constant,
    // evaluated) on the CP thread in drawTriangle, BEFORE the parallel pixel loop - interpCount
    // is final there because the triangle's vertices are already shaded, and deciding it inside
    // shade() would be a data race between rows.
    int psConstState = -1;               // -1 undecided, 0 varies per pixel, 1 constant
    bool psConstKilled = false;
    V4 psConstColor;

    // Alpha test (RB_COLORCONTROL bit 3, func in bits 0..2, reference in RB_ALPHA_REF 0x210E;
    // 0x210F right after it holds PA_CL_VPORT_XSCALE = 640.0, which pins the register map).
    // The game's UI quads run with cc=8700000D = alpha test enabled, func 5 = NOT_EQUAL, ref 0:
    // discard every fully transparent pixel. The rasterizer used to ignore this entirely.
    uint32_t colorCtl = regs[0x2202];
    bool alphaTest = ((colorCtl >> 3) & 1) != 0;
    uint32_t alphaFunc = colorCtl & 7;
    float alphaRef = AsFloat(regs[0x210E]);

    // Shade one pixel from three projected vertices and their barycentric weights.
    auto shade = [&](int x, int y, const Vertex& a, const Vertex& b, const Vertex& c,
                     float wa, float wb, float wc, RowStats& stats)
    {
        float invW = a.invW * wa + b.invW * wb + c.invW * wc;
        if (invW == 0.0f) return;
        float z = a.z * wa + b.z * wb + c.z * wc;
        size_t baseIdx = (size_t)(y * sampY) * samplePitch + (size_t)x * sampX;
        if (baseIdx >= surf.px.size()) return;
        // Depth TEST here, but the depth WRITE waits until alpha test and shader kills have
        // had their say - a discarded fragment must not update Z (alpha-tested UI/foliage
        // depends on that). A depth-only pass shortcuts ONLY when alpha test is off; with it
        // on, the shader must run so the discard can gate the Z write. (A depth-only pass
        // whose shader uses KILL without alpha test still writes early - unobserved so far.)
        if (zbuf && zEnable && !DepthCompare(zFunc, z, (*zbuf)[baseIdx])) return;
        if (!colorPass && !alphaTest)
        {
            if (zbuf && zWrite) (*zbuf)[baseIdx] = z;
            return;
        }

        ShaderCtx ps;
        ps.regs = regs;
        ps.pixel = true;
        float rcp = 1.0f / invW;
        for (uint32_t k = 0; k < interpCount && k < 16; ++k)
            for (int comp = 0; comp < 4; ++comp)
                ps.r[k][comp] = (a.attr[k][comp] * wa + b.attr[k][comp] * wb +
                                 c.attr[k][comp] * wc) * rcp;
        V4 src;
        if (psConstState == 1)
        {
            if (psConstKilled) return;
            src = psConstColor;
        }
        else
        {
            if (!psCode.empty()) RunShader(ps, psCode);
            else ps.exports[0] = ps.r[0];
            if (ps.killed) return;
            src = ps.exports[0];
        }
        if (alphaTest)
        {
            bool pass;
            float a2 = src[3];
            switch (alphaFunc)
            {
            case 0: pass = false; break;               case 1: pass = a2 <  alphaRef; break;
            case 2: pass = a2 == alphaRef; break;      case 3: pass = a2 <= alphaRef; break;
            case 4: pass = a2 >  alphaRef; break;      case 5: pass = a2 != alphaRef; break;
            case 6: pass = a2 >= alphaRef; break;      default: pass = true; break;
            }
            if (!pass) return;
        }
        if (zbuf && zWrite) (*zbuf)[baseIdx] = z;
        if (!colorPass) return;                 // depth-only with alpha test: Z is done
        uint32_t& dstPx = surf.px[baseIdx];
        uint32_t dstBefore = dstPx;         // dstPx aliases the surface; the write below moves it
        V4 out = src;
        if (!passthrough)
        {
            V4 dst;
            dst[0] = ((dstPx >> 16) & 0xFF) / 255.0f;
            dst[1] = ((dstPx >> 8) & 0xFF) / 255.0f;
            dst[2] = (dstPx & 0xFF) / 255.0f;
            dst[3] = ((dstPx >> 24) & 0xFF) / 255.0f;
            for (int i = 0; i < 4; ++i)
            {
                bool alphaCh = i == 3;
                uint32_t sf = alphaCh ? srcFA : srcF, df = alphaCh ? dstFA : dstF;
                uint32_t fn = alphaCh ? combFnA : combFn;
                // D3DBLENDOP encoding (Marathon video.h): 0 ADD, 1 SUBTRACT (s-d), 2 MIN,
                // 3 MAX, 4 REVSUBTRACT (d-s); MIN and MAX ignore the blend factors entirely.
                if (fn == 2) { out[i] = std::min(src[i], dst[i]); continue; }
                if (fn == 3) { out[i] = std::max(src[i], dst[i]); continue; }
                float cc = alphaCh ? constA : constRGB[i];
                float s = src[i] * BlendFactor(sf, src[i], dst[i], src[3], dst[3],
                                               cc, constA, alphaCh);
                float d = dst[i] * BlendFactor(df, src[i], dst[i], src[3], dst[3],
                                               cc, constA, alphaCh);
                out[i] = fn == 1 ? s - d : fn == 4 ? d - s : s + d;
            }
        }
        uint32_t packed = PackArgb(out);
        uint32_t keep = 0;
        if (!(writeMask & 1)) keep |= 0x00FF0000;
        if (!(writeMask & 2)) keep |= 0x0000FF00;
        if (!(writeMask & 4)) keep |= 0x000000FF;
        if (!(writeMask & 8)) keep |= 0xFF000000;
        uint32_t finalPx = (dstPx & keep) | (packed & ~keep);
        for (uint32_t j = 0; j < sampY; ++j)
            for (uint32_t i = 0; i < sampX; ++i)
            {
                size_t o = baseIdx + (size_t)j * samplePitch + i;
                if (o < surf.px.size()) surf.px[o] = finalPx;
            }
        if (stats.written++ == 0)
        {
            stats.firstColor = packed; stats.firstShaded = src; stats.firstDst = dstBefore;
        }
        if (packed & 0x00FFFFFF) ++stats.nonBlack;
    };

    // Rasterize one triangle with edge functions and perspective-correct attributes.
    auto drawTriangle = [&](const Vertex& a, const Vertex& b, const Vertex& c)
    {
        if (!a.valid || !b.valid || !c.valid) return;
#if defined(TTT2_GPU_HW_BACKEND)
        // The GPU culls after clipping (correct for triangles that cross W = 0), so the
        // hardware path receives every assembled triangle and the cull state instead.
        if (hwDraw && a.invW != 0.0f && b.invW != 0.0f && c.invW != 0.0f)
        {
            hwSlots.push_back(hwSlot(a)); hwSlots.push_back(hwSlot(b)); hwSlots.push_back(hwSlot(c));
            // Frame-log diagnostics: report triangles that span far beyond the surface or mix
            // vertices in front of and behind the eye (the source of long thin streaks).
            if (g_gpuFrameNote)
            {
                const float spanX = std::max({ a.x, b.x, c.x }) - std::min({ a.x, b.x, c.x });
                const float spanY = std::max({ a.y, b.y, c.y }) - std::min({ a.y, b.y, c.y });
                const bool mixedW = (a.invW < 0.0f) != (b.invW < 0.0f) || (a.invW < 0.0f) != (c.invW < 0.0f);
                // Slivers: a long edge with almost no area (one vertex far from two close ones)
                // inside the visible surface - the thin lines seen on cloth.
                auto edge = [](const Vertex& p, const Vertex& q) { return std::hypot(p.x - q.x, p.y - q.y); };
                const float eab = edge(a, b), ebc = edge(b, c), eca = edge(c, a);
                const float longest = std::max({ eab, ebc, eca }), shortest = std::min({ eab, ebc, eca });
                const float area2 = std::fabs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
                const bool onScreen = std::min({ a.x, b.x, c.x }) > -64.0f && std::max({ a.x, b.x, c.x }) < float(pitch) + 64.0f &&
                                      std::min({ a.y, b.y, c.y }) > -64.0f && std::max({ a.y, b.y, c.y }) < 784.0f;
                const bool sliver = !mixedW && onScreen && longest > 120.0f && shortest < 0.15f * longest &&
                                    area2 < 0.02f * longest * longest;
                static int streakNotes = 0;
                if (sliver && streakNotes < 4000)
                {
                    ++streakNotes;
                    char note[200];
                    std::snprintf(note, sizeof(note), "    SLIVER len=%.0f vs=%08X/%u ps=%08X/%u (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f)\n", longest,
                                  vsRef.addr, vsRef.size, psRef.addr, psRef.size, a.x, a.y, b.x, b.y, c.x, c.y);
                    g_gpuFrameNote(note);
                    // The draw's index source once per draw: a corner from another mesh's range
                    // (a strip index off by a whole sub-mesh) is told apart from bad vertex data.
                    static uint32_t lastIndexDraw = ~0u;
                    if (lastIndexDraw != indexAddr + numIdx)
                    {
                        lastIndexDraw = indexAddr + numIdx;
                        char in[900];
                        int m = std::snprintf(in, sizeof(in), "      indices: addr=%08X %s n=%u offset=%u min=%u max=%u restart=%d/%X raw:",
                                              indexAddr, index32 ? "32-bit" : "16-bit", numIdx, indexOffset, minIndex, maxIndex,
                                              restartOn ? 1 : 0, restartIdx);
                        for (uint32_t i = 0; i < numIdx && i < 160 && m < 880; ++i)
                        {
                            const uint32_t raw = (srcSel == 2 || !indexAddr) ? i
                                                 : index32 ? GRead32(g_base, indexAddr + i * 4) : GRead16(g_base, indexAddr + i * 2);
                            m += std::snprintf(in + m, sizeof(in) - m, " %u", raw);
                        }
                        if (m < 898) { in[m++] = '\n'; in[m] = 0; }
                        g_gpuFrameNote(in);
                    }
                    // Each vertex fetch of this shader: slot, format, stride/offset and the fetch
                    // constant's endian mode (dword 1 bits 0-1), which the vertex stage ignores.
                    static uint32_t lastVs = 0;
                    if (vsRef.addr != lastVs)
                    {
                        lastVs = vsRef.addr;
                        char fn[400];
                        int m = std::snprintf(fn, sizeof(fn), "      fetches:");
                        const ttt2_hw::VsTranslation vt = ttt2_hw::TranslateVertexShader(vsCode);
                        for (const auto& fe : vt.fetches)
                        {
                            const uint32_t* fc = &regs[REG_FETCH_CONST + fe.slot * 2];
                            if (m < 360)
                                m += std::snprintf(fn + m, sizeof(fn) - m, " [s%u fmt %u stride %u off %d endian %u]",
                                                   fe.slot, fe.format, fe.stride, fe.offset, fc[1] & 3u);
                        }
                        if (m < 398) { fn[m++] = '\n'; fn[m] = 0; }
                        g_gpuFrameNote(fn);
                    }
                    // The corners: vertex index and the first fetch's raw data (for CPU-skinned
                    // meshes, a 64-byte vertex whose first float3 is the position), to tell a
                    // bad CPU-written vertex from a bad transform.
                    static int sliverDumps = 0;
                    const ttt2_hw::VsTranslation vt = ttt2_hw::TranslateVertexShader(vsCode);
                    if (sliverDumps < 400 && !vt.fetches.empty() && vt.fetches[0].stride == 16)
                    {
                        ++sliverDumps;
                        const Vertex* tv[3] = { &a, &b, &c };
                        for (int k = 0; k < 3; ++k)
                        {
                            if (tv[k] < shaded.data() || tv[k] >= shaded.data() + shaded.size()) continue;
                            const uint32_t vindex = uniqueIdx[size_t(tv[k] - shaded.data())];
                            const auto& fe = vt.fetches[0];
                            const uint32_t* fc = &regs[REG_FETCH_CONST + fe.slot * 2];
                            const uint32_t fb = PhysToGuest(fc[0] & ~3u);
                            const uint32_t at = fb + (vindex * fe.stride + uint32_t(fe.offset)) * 4;
                            char cn[200];
                            int m = std::snprintf(cn, sizeof(cn), "      corner %d: vertex %u @%08X:", k, vindex, at);
                            for (uint32_t d = 0; d < std::min<uint32_t>(fe.dwords, 4) && m < 180; ++d)
                            {
                                const uint32_t raw = GRead32(g_base, at + d * 4);
                                float f; std::memcpy(&f, &raw, 4);
                                m += std::snprintf(cn + m, sizeof(cn) - m, " %08X(%.4g)", raw, f);
                            }
                            if (m < 198) { cn[m++] = '\n'; cn[m] = 0; }
                            g_gpuFrameNote(cn);
                            char skin[420];
                            if (DiagSkinDescribe(at, skin, sizeof(skin) - 16))
                            {
                                char line[460];
                                std::snprintf(line, sizeof(line), "        skin: %s\n", skin);
                                g_gpuFrameNote(line);
                            }
                        }
                    }
                }
                if ((spanX > 2.0f * float(pitch) || spanY > 1440.0f || mixedW) && streakNotes < 4000)
                {
                    ++streakNotes;
                    char note[320];
                    std::snprintf(note, sizeof(note), "    STREAK span=%.0fx%.0f%s vs=%08X/%u (%.1f,%.1f,%.4g,w=%.4g) (%.1f,%.1f,%.4g,w=%.4g) (%.1f,%.1f,%.4g,w=%.4g)\n",
                                  spanX, spanY, mixedW ? " mixed-w" : "", vsRef.addr, vsRef.size,
                                  a.x, a.y, a.z, 1.0f / a.invW, b.x, b.y, b.z, 1.0f / b.invW, c.x, c.y, c.z, 1.0f / c.invW);
                    g_gpuFrameNote(note);
                    // Span-only (all in front of the eye): name the outlying vertex and dump its
                    // raw vertex data per fetch, to tell bad CPU-written geometry from the VS.
                    if (!mixedW && streakNotes < 400)
                    {
                        const Vertex* tv[3] = { &a, &b, &c };
                        int farK = 0;
                        float best = -1.0f;
                        const float cx = (a.x + b.x + c.x) / 3.0f, cy = (a.y + b.y + c.y) / 3.0f;
                        for (int k = 0; k < 3; ++k)
                        {
                            const float d = std::fabs(tv[k]->x - cx) + std::fabs(tv[k]->y - cy);
                            if (d > best) { best = d; farK = k; }
                        }
                        const Vertex* fv = tv[farK];
                        if (fv >= shaded.data() && fv < shaded.data() + uniqueIdx.size())
                        {
                            const uint32_t vindex = uniqueIdx[size_t(fv - shaded.data())];
                            const ttt2_hw::VsTranslation vt = ttt2_hw::TranslateVertexShader(vsCode);
                            int m = std::snprintf(note, sizeof(note), "      outlier vertex %u:", vindex);
                            for (const auto& fe : vt.fetches)
                            {
                                const uint32_t* fc = &regs[REG_FETCH_CONST + fe.slot * 2];
                                const uint32_t fb = PhysToGuest(fc[0] & ~3u);
                                const uint32_t at = fb + (vindex * fe.stride + uint32_t(fe.offset)) * 4;
                                m += std::snprintf(note + m, sizeof(note) - m, " [s%u %08X+%u*%u+%d:", fe.slot, fb, vindex, fe.stride, fe.offset);
                                for (uint32_t d = 0; d < fe.dwords && m < 280; ++d)
                                    m += std::snprintf(note + m, sizeof(note) - m, " %08X", GRead32(g_base, at + d * 4));
                                if (m < 300) m += std::snprintf(note + m, sizeof(note) - m, "]");
                                if (m >= 300) break;
                            }
                            if (m < 318) { note[m++] = '\n'; note[m] = 0; }
                            g_gpuFrameNote(note);
                        }
                    }
                }
            }
        }
#endif
        if (!cpuRaster) return;
        float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-6f) return;
        // Window space is y-down, so area > 0 is CLOCKWISE as seen on screen. PA_SU_SC_MODE_CNTL
        // bit 2 (face) selects which winding is front; bits 0/1 cull front/back. (The earlier
        // `cw = area < 0` named the opposite winding; only the uncull'd 2D UI had run here.)
        const bool screenCW = area > 0.0f;
        const bool isFront = screenCW == frontIsCW;
        if ((cull & 1) && isFront) return;                        // cull front faces
        if ((cull & 2) && !isFront) return;                       // cull back faces
        int minX = std::max(scX0, (int)std::floor(std::min(a.x, std::min(b.x, c.x))));
        int maxX = std::min(scX1, (int)std::ceil(std::max(a.x, std::max(b.x, c.x))));
        int minY = std::max(scY0, (int)std::floor(std::min(a.y, std::min(b.y, c.y))));
        int maxY = std::min(scY1, (int)std::ceil(std::max(a.y, std::max(b.y, c.y))));
        if (minX >= maxX || minY >= maxY) return;
        // Decide the constant-shader fast path HERE, on the CP thread: the triangle's vertices
        // are already shaded so interpCount is final, and a constant shader can be evaluated
        // once right now (its inputs cannot depend on the pixel) instead of racing rows to do
        // it. Every instruction slot sits at a 3-dword boundary, so scanning the blob in threes
        // covers them all; control-flow words misread as fetches only ever cost the fast path,
        // never correctness.
        if (psConstState < 0)
        {
            bool hasFetch = false;
            for (size_t i = 0; i + 2 < psCode.size(); i += 3)
                if ((psCode[i] & 0x1F) <= 1 && ((psCode[i] >> 19) & 1)) { hasFetch = true; break; }
            psConstState = (interpCount == 0 && !hasFetch) ? 1 : 0;
            if (psConstState == 1)
            {
                ShaderCtx ps;
                ps.regs = regs;
                ps.pixel = true;
                if (!psCode.empty()) RunShader(ps, psCode);
                else ps.exports[0] = ps.r[0];
                psConstColor = ps.exports[0];
                psConstKilled = ps.killed;
            }
        }
        float inv = 1.0f / area;
        // Top-left fill rule (screen space, y down): a pixel exactly ON an edge belongs to
        // the triangle for which that edge is a top edge (horizontal, interior below) or a
        // left edge (interior to its right). Shared edges are traversed in opposite
        // directions by adjacent triangles, so exactly one of them owns the edge - the
        // inclusive w >= 0 test shaded it in both, which double-blended a seam across every
        // blended rect/quad split and strip join.
        auto topLeft = [&](float ux, float uy, float vx, float vy)
        {
            float dx = vx - ux, dy = vy - uy;
            if (area < 0.0f) { dx = -dx; dy = -dy; }
            return (dy == 0.0f && dx > 0.0f) || dy < 0.0f;
        };
        bool tl0 = topLeft(a.x, a.y, b.x, b.y);      // w0 = edge a->b
        bool tl1 = topLeft(b.x, b.y, c.x, c.y);      // w1 = edge b->c
        bool tl2 = topLeft(c.x, c.y, a.x, a.y);      // w2 = edge c->a
        // Fault guards may abandon rows; never merge an entry from the previous triangle.
        rowStats.assign(maxY - minY, RowStats{});
        auto rowFn = [&](int y)
        {
            RowStats stats;
            for (int x = minX; x < maxX; ++x)
            {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * inv;
                float w1 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) * inv;
                float w2 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) * inv;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                if ((w0 == 0.0f && !tl0) || (w1 == 0.0f && !tl1) ||
                    (w2 == 0.0f && !tl2)) continue;
                ++stats.covered;
                shade(x, y, a, b, c, w1, w2, w0, stats);
            }
            rowStats[y - minY] = stats;
        };
        // Fan the rows out only when there is real work; the sub-4096-pixel triangles that
        // dominate UI draws are cheaper inline than a pool handoff.
        if ((int64_t)(maxY - minY) * (maxX - minX) >= 4096 && maxY - minY >= 8)
            RasterPool::Get().ForRows(minY, maxY, rowFn);
        else
            for (int y = minY; y < maxY; ++y) rowFn(y);
        // Pick the first written pixel in row order for reproducible diagnostics. The old
        // atomic winner could be any row; rendered pixels and totals are unchanged.
        for (const RowStats& stats : rowStats)
        {
            if (!written && stats.written)
            {
                firstColor = stats.firstColor;
                firstShaded = stats.firstShaded;
                firstDst = stats.firstDst;
            }
            written += stats.written;
            covered += stats.covered;
            nonBlack += stats.nonBlack;
        }
    };

#if defined(TTT2_GPU_HW_BACKEND)
    auto makeHwState = [&] {
        ttt2_hw::DrawState st;
        st.pitch = pitch; st.sampX = sampX; st.sampY = sampY;
        st.colorBase = colorBase | (((regs[0x2001] >> 16) & 0xFu) << 12);   // base + format
        st.depthBase = depthBase; st.writeMask = writeMask;
        st.zEnable = zEnable; st.zWrite = zWrite; st.zFunc = zFunc;
        st.stencil = stencilEnable; st.depthCtl = depthCtl;
        st.stencilRefMask = regs[0x210D]; st.stencilRefMaskBf = regs[0x210C];
        st.blend = blend;
        for (int i = 0; i < 4; ++i) st.blendConst[i] = constRGB[i];
        st.scX0 = scX0; st.scY0 = scY0; st.scX1 = scX1; st.scY1 = scY1;
        st.alphaTest = alphaTest; st.alphaFunc = alphaFunc; st.alphaRef = alphaRef;
        st.cull = cull; st.frontIsCW = frontIsCW;
        st.depthClip = (regs[0x2204] & 0x10000u) == 0;
        return st;
    };
    // ---- vertex shader on the GPU (gpu_hw_translate.h TranslateVertexShader): assemble only
    // vertex INDICES, in exactly the order the CPU assembly below emits triangles (restarts,
    // strip winding parity, fans, quads), upload the guest vertex data and let the translated
    // shader do the vertex stage. Rectangle lists need the CPU's corner synthesis from shaded
    // positions; shaders the translator refuses stay on the CPU too (TTT2_GPU_VS=0: all).
    // (A frame being logged keeps the CPU vertex stage so its STREAK diagnostics can run.)
    if (hwDraw && !cpuRaster && prim != PRIM_RECT_LIST && !g_gpuFrameNote)
    {
        ttt2_hw::Backend::VsEntry* vsEntry = nullptr;
        ttt2_hw::Guarded("vs", [&] { vsEntry = ttt2_hw::Get().VertexShader(vsCode); });
        if (vsEntry)
        {
            thread_local std::vector<uint32_t> tri;
            tri.clear();
            auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
                tri.push_back(idx[a]); tri.push_back(idx[b]); tri.push_back(idx[c]);
            };
            if (prim == PRIM_TRI_LIST)
            {
                for (uint32_t i = 0; i + 2 < numIdx; i += 3)
                    if (!(cut[i] | cut[i + 1] | cut[i + 2])) emit(i, i + 1, i + 2);
            }
            else if (prim == PRIM_TRI_STRIP || prim == PRIM_TRI_FAN)
            {
                for (uint32_t s = 0; s < numIdx;)
                {
                    if (cut[s]) { ++s; continue; }
                    uint32_t e = s;
                    while (e < numIdx && !cut[e]) ++e;
                    if (prim == PRIM_TRI_STRIP)
                        for (uint32_t i = s; i + 2 < e; ++i)
                        {
                            if ((i - s) & 1) emit(i + 1, i, i + 2);
                            else             emit(i, i + 1, i + 2);
                        }
                    else
                        for (uint32_t i = s + 1; i + 1 < e; ++i) emit(s, i, i + 1);
                    s = e;
                }
            }
            else if (prim == PRIM_QUAD_LIST)
            {
                for (uint32_t i = 0; i + 3 < numIdx; i += 4)
                {
                    if (cut[i] | cut[i + 1] | cut[i + 2] | cut[i + 3]) continue;
                    emit(i, i + 1, i + 2);
                    emit(i, i + 2, i + 3);
                }
            }
            uint32_t lowest = tri.empty() ? 0 : 0xFFFFFFFFu, highest = 0;
            for (uint32_t v : tri) { lowest = std::min(lowest, v); highest = std::max(highest, v); }
            const ttt2_hw::DrawState st = makeHwState();
            auto result = ttt2_hw::Backend::GpuVsResult::Fallback;
            ttt2_hw::Guarded("draw", [&] {
                result = ttt2_hw::Get().DrawGpuVs(regs, psCode, *vsEntry, tri, lowest, highest, st);
            });
            if (result != ttt2_hw::Backend::GpuVsResult::Fallback)
            {
                if (g_gpuFrameNote)
                {
                    char note[160];
                    std::snprintf(note, sizeof(note), "    hw-vs: tris=%zu index range %u..%u z=%u/%u/%u samp=%ux%u sc=%d,%d..%d,%d clip=%08X\n",
                                  tri.size() / 3, lowest, highest, (unsigned)zEnable, (unsigned)zWrite, zFunc,
                                  sampX, sampY, scX0, scY0, scX1, scY1, regs[0x2204]);
                    g_gpuFrameNote(note);
                }
                if (LogOn() || LogPrimOn(prim)) { LogUsed(); LogPrimUsed(prim); }
                if (!tri.empty()) ++g_litDraws;
                return !tri.empty();
            }
        }
    }
#endif

    if (prim == PRIM_RECT_LIST)
    {
        for (uint32_t i = 0; i + 2 < numIdx; i += 3)
        {
            if (cut[i] | cut[i + 1] | cut[i + 2]) continue;
            const Vertex& v0 = fetchVertex(idx[i]);
            const Vertex& v1 = fetchVertex(idx[i + 1]);
            const Vertex& v2 = fetchVertex(idx[i + 2]);
            if (!v0.valid || !v1.valid || !v2.valid) continue;
            // Three of four corners; which one holds the right angle varies, so find it and
            // synthesise the missing vertex from its two edges.
            const Vertex* v[3] = { &v0, &v1, &v2 };
            int corner = 0;
            float best = 2.0f;
            for (int c = 0; c < 3; ++c)
            {
                const Vertex& p0 = *v[c];
                const Vertex& q0 = *v[(c + 1) % 3];
                const Vertex& r0 = *v[(c + 2) % 3];
                float ax = q0.x - p0.x, ay = q0.y - p0.y;
                float bx = r0.x - p0.x, by = r0.y - p0.y;
                float la = std::sqrt(ax * ax + ay * ay), lb = std::sqrt(bx * bx + by * by);
                if (la < 1e-6f || lb < 1e-6f) continue;
                float cosA = std::fabs((ax * bx + ay * by) / (la * lb));
                if (cosA < best) { best = cosA; corner = c; }
            }
            const Vertex& p = *v[corner];
            const Vertex& q = *v[(corner + 1) % 3];
            const Vertex& r = *v[(corner + 2) % 3];
            Vertex s = q;                                        // q + r - p, attributes too
            s.x = q.x + r.x - p.x;
            s.y = q.y + r.y - p.y;
            s.z = q.z + r.z - p.z;
            s.invW = q.invW + r.invW - p.invW;
            for (uint32_t k = 0; k < interpCount && k < 16; ++k)
                for (int comp = 0; comp < 4; ++comp)
                    s.attr[k][comp] = q.attr[k][comp] + r.attr[k][comp] - p.attr[k][comp];
            drawTriangle(p, q, r);
            drawTriangle(q, s, r);
        }
    }
    else if (prim == PRIM_TRI_LIST)
    {
        for (uint32_t i = 0; i + 2 < numIdx && covered < pixelBudget; i += 3)
            if (!(cut[i] | cut[i + 1] | cut[i + 2]))
                drawTriangle(fetchVertex(idx[i]), fetchVertex(idx[i + 1]), fetchVertex(idx[i + 2]));
    }
    else if (prim == PRIM_TRI_STRIP)
    {
        // Each restart begins a new strip; winding parity counts from the strip's start.
        for (uint32_t s = 0; s < numIdx && covered < pixelBudget;)
        {
            if (cut[s]) { ++s; continue; }
            uint32_t e = s;
            while (e < numIdx && !cut[e]) ++e;
            for (uint32_t i = s; i + 2 < e && covered < pixelBudget; ++i)
            {
                if ((i - s) & 1) drawTriangle(fetchVertex(idx[i + 1]), fetchVertex(idx[i]),
                                              fetchVertex(idx[i + 2]));
                else             drawTriangle(fetchVertex(idx[i]), fetchVertex(idx[i + 1]),
                                              fetchVertex(idx[i + 2]));
            }
            s = e;
        }
    }
    else if (prim == PRIM_TRI_FAN)
    {
        for (uint32_t s = 0; s < numIdx && covered < pixelBudget;)
        {
            if (cut[s]) { ++s; continue; }
            uint32_t e = s;
            while (e < numIdx && !cut[e]) ++e;
            for (uint32_t i = s + 1; i + 1 < e && covered < pixelBudget; ++i)
                drawTriangle(fetchVertex(idx[s]), fetchVertex(idx[i]), fetchVertex(idx[i + 1]));
            s = e;
        }
    }
    else if (prim == PRIM_QUAD_LIST)
    {
        for (uint32_t i = 0; i + 3 < numIdx && covered < pixelBudget; i += 4)
        {
            if (cut[i] | cut[i + 1] | cut[i + 2] | cut[i + 3]) continue;
            drawTriangle(fetchVertex(idx[i]), fetchVertex(idx[i + 1]), fetchVertex(idx[i + 2]));
            drawTriangle(fetchVertex(idx[i]), fetchVertex(idx[i + 2]), fetchVertex(idx[i + 3]));
        }
    }

#if defined(TTT2_GPU_HW_BACKEND)
    if (hwDraw)
    {
        const ttt2_hw::DrawState st = makeHwState();
        if (g_gpuFrameNote)
        {
            // Window-space vertices as submitted (x, y, z, 1/w): shows clear depths and
            // where geometry lands relative to the surface.
            char note[400];
            int n = std::snprintf(note, sizeof(note), "    hw: verts=%zu tris=%zu z=%u/%u/%u samp=%ux%u sc=%d,%d..%d,%d clip=%08X v:",
                                  uniqueIdx.size(), hwSlots.size() / 3, (unsigned)zEnable, (unsigned)zWrite, zFunc,
                                  sampX, sampY, scX0, scY0, scX1, scY1, regs[0x2204]);
            for (size_t k = 0; k < std::min<size_t>(uniqueIdx.size(), 3) && n > 0 && n < 330; ++k)
                n += std::snprintf(note + n, sizeof(note) - n, " (%.1f,%.1f,%.6g,%.4g)", shaded[k].x, shaded[k].y,
                                   shaded[k].z, shaded[k].invW);
            if (n > 0 && n < (int)sizeof(note) - 2) { note[n] = '\n'; note[n + 1] = 0; }
            g_gpuFrameNote(note);
            // The first vertex's interpolators as the pixel shader receives them (true values:
            // attr is stored pre-multiplied by 1/W for perspective-correct rasterisation).
            if (!uniqueIdx.empty() && shaded[0].invW != 0.0f)
                for (uint32_t a = 0; a < std::min<uint32_t>(interpCount, 16); ++a)
                {
                    const float iw = shaded[0].invW;
                    std::snprintf(note, sizeof(note), "      i%u = %.5g %.5g %.5g %.5g\n", a, shaded[0].attr[a][0] / iw,
                                  shaded[0].attr[a][1] / iw, shaded[0].attr[a][2] / iw, shaded[0].attr[a][3] / iw);
                    g_gpuFrameNote(note);
                }
            // Pixel-shader constants c0-c15 (material colours, e.g. the costume tint).
            {
                int m = std::snprintf(note, sizeof(note), "      ps:");
                for (uint32_t row = 0; row < 16 && m > 0 && m < 330; ++row)
                {
                    const uint32_t* c = &regs[REG_ALU_CONST + (256 + row) * 4];
                    m += std::snprintf(note + m, sizeof(note) - m, " c%u=(%.3g %.3g %.3g %.3g)", row, AsFloat(c[0]),
                                       AsFloat(c[1]), AsFloat(c[2]), AsFloat(c[3]));
                    if (row % 4 == 3 && m < 330) { note[m++] = '\n'; note[m] = 0; g_gpuFrameNote(note); m = std::snprintf(note, sizeof(note), "      ps:"); }
                }
            }
            // Vertex constants the menu/fight transforms read (c8-c11 view-projection, c16-c23
            // world rows in TTT2's common skinning-free shaders).
            for (uint32_t row : { 8u, 9u, 10u, 11u, 20u, 21u, 22u, 23u })
            {
                const uint32_t* c = &regs[REG_ALU_CONST + row * 4];
                std::snprintf(note, sizeof(note), "      c%u = %.6g %.6g %.6g %.6g\n", row, AsFloat(c[0]), AsFloat(c[1]),
                              AsFloat(c[2]), AsFloat(c[3]));
                g_gpuFrameNote(note);
            }
        }
        ttt2_hw::Guarded("draw", [&] {
            ttt2_hw::Get().Draw(regs, psCode, shaded.data(), uniqueIdx.size(), hwExtra, hwSlots, interpCount, st);
        });
        if (!cpuRaster)
        {
            // Consume the diagnostic budgets exactly as the CPU path's draw log does; the
            // per-vertex lines in shadeVertex are gated on them (an unconsumed budget logged
            // four lines per draw forever - measured 92% of the CP thread in WriteFile).
            if (LogOn() || LogPrimOn(prim)) { LogUsed(); LogPrimUsed(prim); }
            if (!hwSlots.empty()) ++g_litDraws;   // no readback: count submitted draws
            return !hwSlots.empty();
        }
    }
#endif

    if (LogOn() || LogPrimOn(prim))
    {
        LogUsed();
        LogPrimUsed(prim);
        char line[640];
        int n = std::snprintf(line, sizeof(line),
            "[gpu] draw prim=%u src=%u n=%u base=%03X pitch=%u vte=%08X verts=%zu "
            "interps=%u cull=%u z=%u/%u f=%u covered=%llu px=%llu%s v:",
            prim, srcSel, numIdx, colorBase, pitch, vte, shadedReady ? uniqueIdx.size() : size_t(0), interpCount,
            cull, (unsigned)zEnable, (unsigned)zWrite, zFunc,
            (unsigned long long)covered, (unsigned long long)written,
            colorPass ? "" : " depth-only");
        uint32_t aluRows = 0, fetchSlots = 0;
        for (uint32_t i = 0; i < 512; ++i)
        {
            const uint32_t* c = &regs[REG_ALU_CONST + i * 4];
            if (c[0] | c[1] | c[2] | c[3]) ++aluRows;
        }
        for (uint32_t i = 0; i < 32; ++i)
        {
            const uint32_t* c = &regs[REG_FETCH_CONST + i * 6];
            if (c[0] | c[1] | c[2] | c[3] | c[4] | c[5]) ++fetchSlots;
        }
        n += std::snprintf(line + n, sizeof(line) - n, " alu=%u fetch=%u", aluRows, fetchSlots);
        // The UI shaders transform by c129/c131 and read zero there. If the same rows are
        // populated 256 higher, the vertex/pixel halves of the constant file are the other way
        // round from what the interpreter assumes.
        auto cdump = [&](uint32_t i) {
            const uint32_t* c = &regs[REG_ALU_CONST + i * 4];
            n += std::snprintf(line + n, sizeof(line) - n, " c%u=%08X,%08X", i, c[0], c[1]);
        };
        cdump(129); cdump(131); cdump(385); cdump(387);
        n += std::snprintf(line + n, sizeof(line) - n,
                           " nb=%llu blend=%08X cc=%08X msk=%X dst0=%08X c0=%08X"
                           " src0=(%.3f,%.3f,%.3f,%.3f)",
                           (unsigned long long)nonBlack, blend, regs[0x2202], writeMask,
                           firstDst, firstColor, firstShaded[0], firstShaded[1],
                           firstShaded[2], firstShaded[3]);
        for (uint32_t i = 0; shadedReady && i < numIdx && i < 4; ++i)
        {
            if (cut[i] || slotOfIndex[idx[i]] < 0) continue;
            const Vertex& v = shaded[size_t(slotOfIndex[idx[i]])];
            n += std::snprintf(line + n, sizeof(line) - n, " (%.1f,%.1f,%.3f/%.4f)%s",
                               v.x, v.y, v.z, v.invW, v.valid ? "" : "!");
        }
        // First interpolator of vertex 0, which is where a colour or texcoord shows up.
        if (shadedReady && interpCount && !cut[0] && slotOfIndex[idx[0]] >= 0)
        {
            const Vertex& v0 = shaded[size_t(slotOfIndex[idx[0]])];
            n += std::snprintf(line + n, sizeof(line) - n, " i0=(%.3f,%.3f,%.3f,%.3f)",
                               v0.attr[0][0], v0.attr[0][1], v0.attr[0][2], v0.attr[0][3]);
        }
        std::snprintf(line + n, sizeof(line) - n, "\n");
        std::fputs(line, stderr);
    }
    if (nonBlack) ++g_litDraws;
    return written != 0;
}

// Fetch-constant slots a pixel shader samples (frame inventory diagnostics). Uses the same
// control-flow walk as the hardware translator; 0 when it cannot be analysed.
uint32_t GpuPixelShaderTextureSlots(uint32_t addr, uint32_t sizeDwords)
{
#if defined(TTT2_GPU_HW_BACKEND)
    std::vector<uint32_t> code = LoadUcode(GpuShader{ addr, sizeDwords });
    return ttt2_hw::TranslatePixelShader(code).textureSlots;
#else
    (void)addr; (void)sizeDwords;
    return 0;
#endif
}

// ---- hardware backend entry points (no-ops when it is compiled out or disabled) ----
int GpuHwMode()
{
#if defined(TTT2_GPU_HW_BACKEND)
    return (int)ttt2_hw::ActiveMode();
#else
    return 0;
#endif
}

void GpuHwResolve(uint32_t colorBase, uint32_t destBase, uint32_t pitch, uint32_t height,
                  bool hasLatch, uint32_t latch, bool swapRB, uint32_t srcSelect, uint32_t depthBase, bool depthFloat,
                  uint32_t destFormat)
{
#if defined(TTT2_GPU_HW_BACKEND)
    if (GpuHwMode())
        ttt2_hw::Guarded("resolve", [&] {
            ttt2_hw::Get().Resolve(colorBase, destBase, pitch, height, hasLatch, latch, swapRB, srcSelect, depthBase, depthFloat,
                                   destFormat);
        });
#endif
}

void GpuHwCompareResolve(uint32_t destBase, const uint32_t* cpuLinear, uint32_t pitch, uint32_t height)
{
#if defined(TTT2_GPU_HW_BACKEND)
    if (GpuHwMode())
        ttt2_hw::Guarded("compare", [&] { ttt2_hw::Get().CompareResolve(destBase, cpuLinear, pitch, height); });
#endif
}

bool GpuHwPresent(uint32_t frontBuffer, bool displaySwapRB, const uint32_t* gammaRamp, uint32_t gammaVersion)
{
    bool presented = false;
#if defined(TTT2_GPU_HW_BACKEND)
    if (GpuHwMode())
        ttt2_hw::Guarded("present", [&] { presented = ttt2_hw::Get().Present(frontBuffer, displaySwapRB, gammaRamp, gammaVersion); });
#else
    (void)gammaRamp; (void)gammaVersion;
#endif
    return presented;
}

bool GpuHwIsExclusiveFullscreen()
{
#if defined(TTT2_GPU_HW_BACKEND)
    return GpuHwMode() != 0 && ttt2_hw::Get().IsExclusiveFullscreen();
#else
    return false;
#endif
}

bool GpuHwReadFrontBuffer(uint32_t* bgrx)
{
    bool read = false;
#if defined(TTT2_GPU_HW_BACKEND)
    if (GpuHwMode())
        ttt2_hw::Guarded("readback", [&] { read = ttt2_hw::Get().ReadFront(bgrx); });
#endif
    return read;
}

bool GpuHwDumpResolved(uint32_t destBase, const char* path)
{
    bool ok = false;
#if defined(TTT2_GPU_HW_BACKEND)
    if (GpuHwMode())
        ttt2_hw::Guarded("dump", [&] { ok = ttt2_hw::Get().DumpResolved(destBase, path); });
#endif
    return ok;
}

void GpuHwWriteBackRange(uint32_t start, uint32_t size)
{
#if defined(TTT2_GPU_HW_BACKEND)
    // The backend answers "still copying" instead of waiting under its lock; wait with the lock
    // released - on the GPU fence signalled after the copy (it wakes exactly when the copy is done;
    // polling woke ~2 ms late on average, MEASURED 2026-10-15: fight-intro bake reads 7.7 -> 5.6 ms
    // median), or without one by polling (yield first, then 1 ms sleeps) - and after 2000 rounds let
    // Map wait.
    static const bool timing = [] { const char* e = std::getenv("TTT2_FRAMETIME"); return e && *e == '1'; }();
    const auto t0 = std::chrono::steady_clock::now();
    unsigned polls = 0;
    for (bool pending = GpuHwMode(); pending;)
    {
        const bool block = polls > 2000;
        Microsoft::WRL::ComPtr<ID3D11Fence> fence;
        uint64_t fenceValue = 0;
        if (!ttt2_hw::Guarded("writeback", [&] {
                pending = ttt2_hw::Get().WriteBackRange(start, size, block);
                if (pending) fence = ttt2_hw::Get().PendingFence(fenceValue);
            })) break;
        if (!pending) break;
        ++polls;
        // Wait for exactly the copy (the GPU signals the fence after it); otherwise poll: yield
        // first, then 1 ms sleeps.
        static thread_local HANDLE copied = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence && copied && SUCCEEDED(fence->SetEventOnCompletion(fenceValue, copied))) WaitForSingleObject(copied, 100);
        else if (polls < 64) SwitchToThread();
        else Sleep(1);
    }
    if (timing)
    {
        const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        static std::atomic<int> logged{ 0 };
        if (total > 1.0 && logged++ < 200)
            std::fprintf(stderr, "[frame] cpu-read fault %08X: %.2f ms (%u polls) thread %lu\n", start, total, polls, GetCurrentThreadId());
    }
#else
    (void)start; (void)size;
#endif
}

void GpuHwSetWindow(void* window)
{
#if defined(TTT2_GPU_HW_BACKEND)
    if (GpuHwMode()) ttt2_hw::Get().SetWindow(static_cast<HWND>(window));
#endif
}
