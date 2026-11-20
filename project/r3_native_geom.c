/* R3 native kernel for job 422 (igjob_00E29200, Edge-style geometry job; the
 * same Insomniac source as R2's jobmod_dd413431e518 -- data tables match
 * byte for byte, relocated by -0x4400).
 *
 *   R3NJ_K_GEOM_XFORM4  igjob_00E29200 0x0120: packed-position transform +
 *                       clip code (R2 dd413 0x4030, r2_native_jobs_kern.c
 *                       r2nj_dd413_xform4, here with R3's table addresses).
 *
 * In:  $r3 src (80-byte records, 4 vertices each), $r4 dst (64 bytes per
 *      record), $r5 vertex count, $r6..$r9 matrix rows.
 * LS tables: unpack controls at 0x1700 + ((src & 12) << 5) (+0x00..+0x50),
 *      transposes at 0x3960/0x3970 and 0x1900..0x1930.
 * Verified against the lifted function by scratch\spu_native\test_geom.exe
 * (fuzz + mutation). Off by default until checked on captured game inputs
 * (R3_NATIVE_JOBS_GEOM=1 / R3_NATIVE_JOBS_MASK bit 4; see r3_native_jobs.h). */
#include "r3_native_jobs.h"
#include <immintrin.h>
#include <string.h>

#define G_QM 0x3FFF0u
#define G_T_UNPACK 0x1700u
#define G_T_C00    0x1900u
#define G_T_L60    0x3960u

#if defined(__AVX2__)
static inline __m128i g_wswap128(void)
{ return _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12); }
static inline __m128i g_ldq(const uint8_t* ls, uint32_t a)
{ return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (a & G_QM))), g_wswap128()); }
static inline void g_stq(uint8_t* ls, uint32_t a, __m128i v)
{ _mm_storeu_si128((__m128i*)(ls + (a & G_QM)), _mm_shuffle_epi8(v, g_wswap128())); }
static inline __m256i g_ld2(const uint8_t* ls, uint32_t a0, uint32_t a1)
{
    const __m256i raw = _mm256_inserti128_si256(
        _mm256_castsi128_si256(_mm_loadu_si128((const __m128i*)(ls + (a0 & G_QM)))),
        _mm_loadu_si128((const __m128i*)(ls + (a1 & G_QM))), 1);
    return _mm256_shuffle_epi8(raw, _mm256_broadcastsi128_si256(g_wswap128()));
}
/* shufb with a loop-invariant control, precomputed (see r2_native_jobs_kern.c) */
typedef struct { __m256i idx, sel, spec; } g_shf;
static inline void g_shf_mk(g_shf* s, __m128i c)
{
    const __m128i e0m = _mm_set1_epi8((char)0xE0);
    const __m128i hi = _mm_cmplt_epi8(c, _mm_setzero_si128());
    const __m128i top = _mm_and_si128(c, e0m);
    const __m128i pb = _mm_or_si128(_mm_and_si128(_mm_cmpeq_epi8(top, e0m), _mm_set1_epi8((char)0x80)),
                                    _mm_cmpeq_epi8(top, _mm_set1_epi8((char)0xC0)));
    const __m128i ib = _mm_or_si128(_mm_xor_si128(_mm_and_si128(c, _mm_set1_epi8(15)), _mm_set1_epi8(3)), hi);
    const __m128i bit4 = _mm_set1_epi8(0x10);
    const __m128i sb = _mm_andnot_si128(hi, _mm_cmpeq_epi8(_mm_and_si128(c, bit4), bit4));
    s->idx  = _mm256_broadcastsi128_si256(ib);
    s->sel  = _mm256_broadcastsi128_si256(sb);
    s->spec = _mm256_broadcastsi128_si256(pb);
}
static inline __m256i g_shf_ap(const g_shf* s, __m256i a, __m256i b)
{
    return _mm256_or_si256(_mm256_blendv_epi8(_mm256_shuffle_epi8(a, s->idx),
                                              _mm256_shuffle_epi8(b, s->idx), s->sel), s->spec);
}
/* float ops: spu_helpers.h fast path (clamped; identical with or without SPU_XF_RZ_TRUST under RZ) */
static inline __m256 g_xf_in(__m256i x)
{
    const __m256i sm = _mm256_set1_epi32((int)0x80000000u);
    const __m256i mag = _mm256_min_epu32(_mm256_andnot_si256(sm, x), _mm256_set1_epi32(0x7F7FFFFF));
    return _mm256_castsi256_ps(_mm256_or_si256(_mm256_and_si256(x, sm), mag));
}
static inline __m256 g_xf_out(__m256 r)
{
    const __m256 mx = _mm256_set1_ps(3.40282347e+38f);
    return _mm256_min_ps(_mm256_max_ps(_mm256_add_ps(r, _mm256_setzero_ps()),
                                       _mm256_sub_ps(_mm256_setzero_ps(), mx)), mx);
}
static inline __m256 g_fma(__m256 a, __m256 b, __m256 c)
{
#if defined(__FMA__)
    return g_xf_out(_mm256_fmadd_ps(a, b, c));
#else
    return g_xf_out(_mm256_add_ps(_mm256_mul_ps(a, b), c));
#endif
}
static inline __m256 g_fa(__m256 a, __m256 b) { return g_xf_out(_mm256_add_ps(a, b)); }
static inline __m256i g_key(__m256i x)
{
    const __m256i m = _mm256_and_si256(x, _mm256_set1_epi32(0x7FFFFFFF));
    const __m256i dn = _mm256_cmpeq_epi32(_mm256_and_si256(x, _mm256_set1_epi32(0x7F800000)), _mm256_setzero_si256());
    const __m256i mag = _mm256_andnot_si256(dn, m);
    const __m256i neg = _mm256_srai_epi32(x, 31);
    return _mm256_sub_epi32(_mm256_xor_si256(mag, neg), neg);
}
static inline __m256i g_fcgt(__m256 a, __m256 b)
{ return _mm256_cmpgt_epi32(g_key(_mm256_castps_si256(a)), g_key(_mm256_castps_si256(b))); }
static inline __m128 g_csflt16_half(__m128i v)
{
    const __m256d d = _mm256_mul_pd(_mm256_cvtepi32_pd(v), _mm256_set1_pd(1.0 / 65536.0));
    const __m256d t = _mm256_castsi256_pd(_mm256_and_si256(_mm256_castpd_si256(d), _mm256_set1_epi64x(~0x1FFFFFFFll)));
    const __m256d tiny = _mm256_cmp_pd(_mm256_andnot_pd(_mm256_set1_pd(-0.0), t), _mm256_set1_pd(1.1754943508222875e-38), _CMP_LT_OQ);
    return _mm256_cvtpd_ps(_mm256_andnot_pd(tiny, t));
}
static inline __m256 g_csflt16(__m256i v)
{
    const __m256i small = _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_add_epi32(v, _mm256_set1_epi32(1 << 24)),
                                                              _mm256_set1_epi32((int)0xFE000000u)), _mm256_setzero_si256());
    const __m256i low0 = _mm256_cmpeq_epi32(_mm256_and_si256(v, _mm256_set1_epi32(0xFF)), _mm256_setzero_si256());
    if (_mm256_movemask_epi8(_mm256_or_si256(small, low0)) == -1)
        return _mm256_mul_ps(_mm256_cvtepi32_ps(v), _mm256_set1_ps(1.0f / 65536.0f));
    return _mm256_set_m128(g_csflt16_half(_mm256_extracti128_si256(v, 1)),
                           g_csflt16_half(_mm256_castsi256_si128(v)));
}

int r3nj_geom_xform4(uint8_t* ls, u128* gpr)
{
    const uint32_t src = gpr[3]._u32[0], dst = gpr[4]._u32[0];
    const int32_t n = (int32_t)(gpr[5]._u32[0] + 3u) >> 2;
    if (n <= 0 || n > 4096) return 0;
    const uint64_t in_end = (uint64_t)src + 80ull * (uint64_t)(n + 4) + 16u;   /* read-ahead included */
    const uint64_t out_end = (uint64_t)dst + 64ull * (uint64_t)n;
    if (in_end > R3NJ_LS_SIZE || out_end > R3NJ_LS_SIZE) return 0;
    if (!(dst <= src || (uint64_t)dst >= in_end || out_end <= (src & ~15u))) return 0;

    const uint32_t tb = G_T_UNPACK + ((src & 12u) << 5);
    g_shf T0, T1, T2, T3, T4, T5, L60, L70, C00, C10, C20, C30;
    g_shf_mk(&T0, g_ldq(ls, tb + 0x00)); g_shf_mk(&T1, g_ldq(ls, tb + 0x10));
    g_shf_mk(&T2, g_ldq(ls, tb + 0x20)); g_shf_mk(&T3, g_ldq(ls, tb + 0x30));
    g_shf_mk(&T4, g_ldq(ls, tb + 0x40)); g_shf_mk(&T5, g_ldq(ls, tb + 0x50));
    g_shf_mk(&L60, g_ldq(ls, G_T_L60)); g_shf_mk(&L70, g_ldq(ls, G_T_L60 + 0x10));
    g_shf_mk(&C00, g_ldq(ls, G_T_C00)); g_shf_mk(&C10, g_ldq(ls, G_T_C00 + 0x10));
    g_shf_mk(&C20, g_ldq(ls, G_T_C00 + 0x20)); g_shf_mk(&C30, g_ldq(ls, G_T_C00 + 0x30));

    __m256 M[4][4];
    for (int r = 0; r < 4; r++)
        for (int w = 0; w < 4; w++)
            M[r][w] = g_xf_in(_mm256_set1_epi32((int)gpr[6 + r]._u32[w]));
    const __m256i SIGN = _mm256_set1_epi32((int)0x80000000u);

    for (int32_t k = 0; k < n; k += 2) {
        const uint32_t a = src + 80u * (uint32_t)k;
        const uint32_t b = (k + 1 < n) ? a + 80u : a;
        const __m256i q0 = g_ld2(ls, a + 0x00, b + 0x00), q1 = g_ld2(ls, a + 0x10, b + 0x10);
        const __m256i q2 = g_ld2(ls, a + 0x20, b + 0x20), q3 = g_ld2(ls, a + 0x30, b + 0x30);
        const __m256i q4 = g_ld2(ls, a + 0x40, b + 0x40);
        const __m256i t69 = g_shf_ap(&T0, q0, q2);
        const __m256i t52 = g_shf_ap(&T2, t69, q4);
        const __m256i t53 = g_shf_ap(&T1, q1, q3);
        const __m256 x = g_csflt16(g_shf_ap(&T3, t52, t53));
        const __m256 y = g_csflt16(g_shf_ap(&T4, t52, t53));
        const __m256 z = g_csflt16(g_shf_ap(&T5, t52, t53));
        const __m256 X = g_fma(z, M[0][2], g_fma(y, M[0][1], g_fma(x, M[0][0], M[0][3])));
        const __m256 Y = g_fma(z, M[1][2], g_fma(y, M[1][1], g_fma(x, M[1][0], M[1][3])));
        const __m256 Z = g_fma(z, M[2][2], g_fma(y, M[2][1], g_fma(x, M[2][0], M[2][3])));
        const __m256 W = g_fma(z, M[3][2], g_fma(y, M[3][1], g_fma(x, M[3][0], M[3][3])));
        const __m256i Xi = _mm256_castps_si256(X), Yi = _mm256_castps_si256(Y);
        const __m256i Zi = _mm256_castps_si256(Z), Wi = _mm256_castps_si256(W);
        const __m256i cx = _mm256_or_si256(_mm256_andnot_si256(SIGN, g_fcgt(X, W)),
                                           _mm256_and_si256(SIGN, _mm256_castps_si256(g_fa(X, W))));
        const __m256i cy = _mm256_or_si256(_mm256_andnot_si256(SIGN, g_fcgt(Y, W)),
                                           _mm256_and_si256(SIGN, _mm256_castps_si256(g_fa(Y, W))));
        const __m256i cz = _mm256_or_si256(_mm256_andnot_si256(SIGN, g_fcgt(Z, W)),
                                           _mm256_and_si256(SIGN, Zi));
        const __m256i lo = g_shf_ap(&L60, Xi, Wi), hi = g_shf_ap(&L70, Xi, Wi);
        const __m256i c59 = g_shf_ap(&C10, g_shf_ap(&C00, cx, cy), cz);
        const __m256i yl = g_shf_ap(&L60, Yi, c59), yh = g_shf_ap(&L70, Yi, c59);
        const __m256i o0 = g_shf_ap(&C20, lo, yl), o1 = g_shf_ap(&C30, lo, yl);
        const __m256i o2 = g_shf_ap(&C20, hi, yh), o3 = g_shf_ap(&C30, hi, yh);
        const uint32_t d0 = dst + 64u * (uint32_t)k;
        g_stq(ls, d0 + 0x10, _mm256_castsi256_si128(o1));
        g_stq(ls, d0 + 0x00, _mm256_castsi256_si128(o0));
        g_stq(ls, d0 + 0x20, _mm256_castsi256_si128(o2));
        g_stq(ls, d0 + 0x30, _mm256_castsi256_si128(o3));
        if (k + 1 < n) {
            const uint32_t d1 = d0 + 64u;
            g_stq(ls, d1 + 0x10, _mm256_extracti128_si256(o1, 1));
            g_stq(ls, d1 + 0x00, _mm256_extracti128_si256(o0, 1));
            g_stq(ls, d1 + 0x20, _mm256_extracti128_si256(o2, 1));
            g_stq(ls, d1 + 0x30, _mm256_extracti128_si256(o3, 1));
        }
    }
    return 1;
}
int r3nj_geom_built(void) { return 1; }
#else
int r3nj_geom_xform4(uint8_t* ls, u128* gpr) { (void)ls; (void)gpr; return 0; }
int r3nj_geom_built(void) { return 0; }
#endif
