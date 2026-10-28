// Prints the bit patterns the generated guest code produces for the FP edge cases listed in
// mac/DETERMINISM.md. Built twice by mac/tests/ppc_fp_probe.sh: arm64 (with the mac overlay,
// as mac/build_ppc.sh compiles re/ppc) and x86_64 -march=x86-64-v2 (as their Windows/Linux
// builds); run the x86 one on a real Intel Mac and diff. Every expression below is copied
// from what XenonRecomp emits (tools/XenonRecomp/XenonRecomp/recompiler.cpp).
#include "ppc_config.h"
#include "ppc_context.h"
#include <cfloat>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

PPCFunc* const g_unmappedReporter = nullptr;
uint64_t PPCReadTimebase() { return 0; }

static uint32_t B(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint64_t B(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static float F(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static double D(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
template <class T> static T opaque(T v) { __asm__ volatile("" : "+m"(v)); return v; }

static void v4(const char* name, simde__m128 v) {
    alignas(16) float f[4]; simde_mm_store_ps(f, v);
    printf("%-34s %08X %08X %08X %08X\n", name, B(f[0]), B(f[1]), B(f[2]), B(f[3]));
}
static void i4(const char* name, simde__m128i v) {
    alignas(16) uint32_t u[4]; simde_mm_store_si128((simde__m128i*)u, v);
    printf("%-34s %08X %08X %08X %08X\n", name, u[0], u[1], u[2], u[3]);
}

int main() {
#ifdef PROBE_FPCR_AH
    // Experiment: FEAT_AFP alternate handling (x86-like flushing and default NaN), FIZ in the flush mask.
    { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); v |= 2; __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
#endif
    PPCContext ctx{};
    ctx.fpscr.loadFromHost();
    PPCRegister f1{}, f2{}, f3{}, f0{};

    // 1. vmsum4fp128 / vmsum3fp128 = simde_mm_dp_ps 0xFF / 0xEF (association and signed zero)
    ctx.fpscr.enableFlushMode();
    v4("dp 0xFF cancel (1e8,1,-1e8,1)", simde_mm_dp_ps(simde_mm_setr_ps(1e8f, 1, -1e8f, 1), simde_mm_set1_ps(1), 0xFF));
    v4("dp 0xFF (1,1e8,1,-1e8)", simde_mm_dp_ps(simde_mm_setr_ps(1, 1e8f, 1, -1e8f), simde_mm_set1_ps(1), 0xFF));
    v4("dp 0xFF (1,-1e8,1e8,1)", simde_mm_dp_ps(simde_mm_setr_ps(1, -1e8f, 1e8f, 1), simde_mm_set1_ps(1), 0xFF));
    v4("dp 0xEF (x,-0,-0,-0)", simde_mm_dp_ps(simde_mm_setr_ps(5, -0.f, -0.f, -0.f), simde_mm_set1_ps(1), 0xEF));
    v4("dp 0xFF (-0,-0,-0,-0)", simde_mm_dp_ps(simde_mm_set1_ps(-0.f), simde_mm_set1_ps(1), 0xFF));
    v4("dp 0xFF qnan payloads", simde_mm_dp_ps(simde_mm_setr_ps(opaque(F(0x7FC00001)), 1, opaque(F(0xFFC00002)), 1), simde_mm_set1_ps(1), 0xFF));

    // 2. FTZ/DAZ (VMX path: enableFlushMode) on vector ops
    const float denorm = F(0x00000100), tinyA = F(0x00800001) /* FLT_MIN+ulp */, tinyB = F(0x3F7FFFFF) /* 1-2^-24 */, tinyC = F(0x3F7FFFFE) /* 1-2^-23 */;
    v4("vmulfp denorm*1", simde_mm_mul_ps(simde_mm_set1_ps(opaque(denorm)), simde_mm_set1_ps(1)));
    v4("vaddfp denorm+0", simde_mm_add_ps(simde_mm_set1_ps(opaque(denorm)), simde_mm_setzero_ps()));
    v4("vmulfp FLT_MIN(1-2^-46) tininess", simde_mm_mul_ps(simde_mm_set1_ps(opaque(tinyA)), simde_mm_set1_ps(opaque(tinyC))));
    f1.f64 = double(opaque(tinyA)); f2.f64 = double(opaque(tinyC));
    f0.f64 = double(float(f1.f64 * f2.f64));
    printf("%-34s %016" PRIX64 "\n", "fmuls same, FZ on (VMX left on)", B(f0.f64));
    v4("vmulfp FLT_MIN*(1-2^-24)", simde_mm_mul_ps(simde_mm_set1_ps(opaque(FLT_MIN)), simde_mm_set1_ps(opaque(tinyB))));
    v4("vmaxfp(denorm, -1)", simde_mm_max_ps(simde_mm_set1_ps(opaque(denorm)), simde_mm_set1_ps(-1)));
    v4("vminfp(denorm, 1)", simde_mm_min_ps(simde_mm_set1_ps(opaque(denorm)), simde_mm_set1_ps(1)));
    v4("vmaxfp(qnan, 1) / (1, qnan)", simde_mm_max_ps(simde_mm_setr_ps(opaque(NAN), 1, 0, 0), simde_mm_setr_ps(1, opaque(NAN), 0, 0)));
    v4("vmaxfp(-0,+0)", simde_mm_max_ps(simde_mm_set1_ps(-0.f), simde_mm_set1_ps(0.f)));
    i4("vcmpgtfp denorm>0", simde_mm_castps_si128(simde_mm_cmpgt_ps(simde_mm_set1_ps(opaque(denorm)), simde_mm_setzero_ps())));
    v4("vrefp(denorm)", simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_set1_ps(opaque(denorm))));
    v4("vrsqrtefp(denorm)", simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(simde_mm_set1_ps(opaque(denorm)))));
    i4("vctsxs(denorm)", simde_mm_vctsxs(simde_mm_set1_ps(opaque(denorm))));
    v4("vcsxwfp(0x7FFFFFFF,..)", simde_mm_cvtepi32_ps(simde_mm_setr_epi32(0x7FFFFFFF, 16777217, -16777217, 0)));

    // 3. default NaNs (invalid operations) and NaN propagation
    v4("vaddfp inf-inf", simde_mm_add_ps(simde_mm_set1_ps(opaque(INFINITY)), simde_mm_set1_ps(-INFINITY)));
    v4("vmulfp 0*inf", simde_mm_mul_ps(simde_mm_set1_ps(opaque(0.f)), simde_mm_set1_ps(INFINITY)));
    v4("vsqrt(-1) (vrsqrtefp path)", simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(simde_mm_set1_ps(opaque(-1.f)))));
    v4("vaddfp qnanA+qnanB", simde_mm_add_ps(simde_mm_set1_ps(opaque(F(0x7FC00001))), simde_mm_set1_ps(opaque(F(0xFFC00002)))));
    v4("vaddfp 1+snan", simde_mm_add_ps(simde_mm_set1_ps(1), simde_mm_set1_ps(opaque(F(0x7F800003)))));
    v4("vaddfp qnanA+snanB", simde_mm_add_ps(simde_mm_set1_ps(opaque(F(0x7FC00001))), simde_mm_set1_ps(opaque(F(0xFF800002)))));
    v4("vaddfp snanA+qnanB", simde_mm_add_ps(simde_mm_set1_ps(opaque(F(0x7F800001))), simde_mm_set1_ps(opaque(F(0xFFC00002)))));
    v4("vmulfp qnanA*snanB", simde_mm_mul_ps(simde_mm_set1_ps(opaque(F(0x7FC00001))), simde_mm_set1_ps(opaque(F(0xFF800002)))));
    ctx.fpscr.disableFlushMode();
    f1.f64 = opaque(D(0x7FF8000000000001)); f2.f64 = opaque(D(0xFFF0000000000002));
    f0.f64 = f1.f64 + f2.f64;                               printf("%-34s %016" PRIX64 "\n", "fadd qnanA+snanB", B(f0.f64));
    f1.f64 = opaque(INFINITY); f2.f64 = -INFINITY;
    f0.f64 = f1.f64 + f2.f64;                               printf("%-34s %016" PRIX64 "\n", "fadd inf-inf", B(f0.f64));
    f1.f64 = opaque(-1.0); f0.f64 = sqrt(f1.f64);           printf("%-34s %016" PRIX64 "\n", "fsqrt(-1)", B(f0.f64));
    f1.f64 = opaque(D(0x7FF8000000000001)); f2.f64 = opaque(D(0xFFF8000000000002));
    f0.f64 = f1.f64 * f2.f64;                               printf("%-34s %016" PRIX64 "\n", "fmul qnanA*qnanB", B(f0.f64));
    f1.f64 = opaque(D(0x7FF8000000000001));
    f0.f64 = double(float(f1.f64));                         printf("%-34s %016" PRIX64 "\n", "frsp(qnan payload)", B(f0.f64));

    // 4. float -> int (FCTIWZ, FCTIDZ, FCTID, vctsxs, vctuxs)
    const double cases[] = { 2.5, 3.5, -2.5, -0.5, 2147483647.0, 2147483647.5, -2147483648.0, -3e9, 3e9,
                             9.3e18, -9.3e18, NAN, -NAN, INFINITY, -INFINITY };
    for (double c : cases) {
        f1.f64 = opaque(c);
        f0.s64 = (f1.f64 > double(INT_MAX)) ? INT_MAX : simde_mm_cvttsd_si32(simde_mm_load_sd(&f1.f64));
        uint64_t fctiwz = f0.u64;
        f0.s64 = (f1.f64 > double(LLONG_MAX)) ? LLONG_MAX : simde_mm_cvttsd_si64(simde_mm_load_sd(&f1.f64));
        uint64_t fctidz = f0.u64;
        f0.s64 = (f1.f64 > double(LLONG_MAX)) ? LLONG_MAX : simde_mm_cvtsd_si64(simde_mm_load_sd(&f1.f64));
        uint64_t fctid = f0.u64;
        printf("%-12.10g fctiwz %016" PRIX64 " fctidz %016" PRIX64 " fctid %016" PRIX64 "\n", c, fctiwz, fctidz, fctid);
    }
    ctx.fpscr.storeFromGuest(PPC_ROUND_TOWARD_ZERO);
    f1.f64 = opaque(2.7); f0.s64 = simde_mm_cvtsd_si64(simde_mm_load_sd(&f1.f64));
    printf("%-34s %016" PRIX64 "\n", "fctid 2.7 (mode toward zero)", f0.u64);
    printf("%-34s %u\n", "mffs rounding after mtfsf 1", (unsigned)ctx.fpscr.loadFromHost());
    ctx.fpscr.storeFromGuest(PPC_ROUND_UP);
    printf("%-34s %u\n", "mffs rounding after mtfsf 2", (unsigned)ctx.fpscr.loadFromHost());
    f1.f64 = opaque(1.0); f2.f64 = opaque(3.0); f0.f64 = double(float(f1.f64 / f2.f64));
    printf("%-34s %016" PRIX64 "\n", "fdivs 1/3 (mode +inf)", B(f0.f64));
    ctx.fpscr.storeFromGuest(PPC_ROUND_NEAREST);
    ctx.fpscr.enableFlushMode();
    i4("vctsxs(nan,3e9,-3e9,2.5)", simde_mm_vctsxs(simde_mm_setr_ps(opaque(NAN), 3e9f, -3e9f, 2.5f)));
    i4("vctuxs(nan,5e9,-1,2.5)", simde_mm_vctuxs(simde_mm_setr_ps(opaque(NAN), 5e9f, -1, 2.5f)));
    i4("vctuxs(snan,inf,-inf,4294967040)", simde_mm_vctuxs(simde_mm_setr_ps(opaque(F(0x7F800001)), INFINITY, -INFINITY, 4294967040.f)));
    v4("vcfux(0xFFFFFFFF,0x80000001,..)", simde_mm_cvtepu32_ps_(simde_mm_setr_epi32(-1, int(0x80000001), 0x7FFFFFFF, 16777217)));
    v4("vrfin(0.5,1.5,2.5,-0.5)", simde_mm_round_ps(simde_mm_setr_ps(0.5f, 1.5f, 2.5f, -0.5f), SIMDE_MM_FROUND_TO_NEAREST_INT | SIMDE_MM_FROUND_NO_EXC));

    // 5. libm (vexptefp / vlogefp): host library results
    const float lx[] = { 0.1f, 0.3f, 1.0f / 3, 2.5f, -3.7f, 10.123f, 100.7f, -126.5f };
    for (float x : lx) printf("exp2f %-10.6g %08X  log2f %08X\n", x, B(exp2f(opaque(x))), B(log2f(opaque(fabsf(x)))));
    // the FZ-on path, as the generated code calls them (VMX flush mode on)
    printf("exp2f(-140) FZ on %08X  log2f(denorm) FZ on %08X\n", B(exp2f(opaque(-140.f))), B(log2f(opaque(denorm))));
    ctx.fpscr.disableFlushMode();
    printf("exp2f(-140) FZ off %08X  log2f(denorm) FZ off %08X\n", B(exp2f(opaque(-140.f))), B(log2f(opaque(denorm))));
    return 0;
}
