#pragma once

#if !(defined(__ANDROID__) || defined(__APPLE__)) || !defined(__aarch64__)
#error "Android ARM64 SIMD compatibility header used on another target"
#endif

#include <x86/sse4.1.h>

using __m128 = simde__m128;
using __m128i = simde__m128i;

#ifndef _MM_SHUFFLE
#define _MM_SHUFFLE(z, y, x, w) SIMDE_MM_SHUFFLE(z, y, x, w)
#endif

#define _mm_castps_si128 simde_mm_castps_si128
#define _mm_cmpge_ps simde_mm_cmpge_ps
#define _mm_cmpunord_ps simde_mm_cmpunord_ps
#define _mm_cvtepi32_ps simde_mm_cvtepi32_ps
#define _mm_cvtsi128_si32 simde_mm_cvtsi128_si32
#define _mm_cvttps_epi32 simde_mm_cvttps_epi32
#define _mm_loadu_ps simde_mm_loadu_ps
#define _mm_max_ps simde_mm_max_ps
#define _mm_min_ps simde_mm_min_ps
#define _mm_movemask_ps simde_mm_movemask_ps
#define _mm_mul_ps simde_mm_mul_ps
#define _mm_packs_epi32 simde_mm_packs_epi32
#define _mm_packus_epi16 simde_mm_packus_epi16
#define _mm_set1_ps simde_mm_set1_ps
#define _mm_setzero_ps simde_mm_setzero_ps
#define _mm_setzero_si128 simde_mm_setzero_si128
#define _mm_shuffle_epi32 simde_mm_shuffle_epi32
#define _mm_sub_epi32 simde_mm_sub_epi32
#define _mm_sub_ps simde_mm_sub_ps
#ifndef _mm_pause
#define _mm_pause() __asm__ __volatile__("yield")
#endif
