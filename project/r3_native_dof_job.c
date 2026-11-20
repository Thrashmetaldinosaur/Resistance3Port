/* Resistance 3 native DOF job (igjob 419): AVX2 bulk loops for kernel image 623
 * (igpic_00E03580). See r3_native_dof_job.h for the contract.
 *
 * Every lane op here reproduces the spu_* helper the lifted code uses, as
 * compiled for the lifted SPU sources (SPU_X86V, SPU_XF_NO_EXACT, __FMA__):
 *   fma / fnms : clamp each input's magnitude to FLT_MAX (exponent-255 SPU
 *                values), one fused op, +0.0 added, result clamped to +-FLT_MAX
 *                -- run under the SPU threads' MXCSR (FTZ|DAZ|RZ);
 *   fcgt       : integer compare of sign-magnitude keys, exponent-0 -> 0;
 *   cuflt 155  : of a 24-bit integer, the exact int -> float conversion;
 *   frest      : sign | (253 - exp, 255 for exp 0, 0 for exp >= 253) |
 *                spu_frest_fraction_lut[(x >> 18) & 31];
 *   fi         : the ISA's 64-bit fixed-point interpolation (spu_fi), done in
 *                32 bits: m = 2A - p with A = (2^13 | base) << 18, p = step * y;
 *                floor(m / 2) = A - ceil(p / 2) carries every bit fi keeps.
 * The bulk loops do all but the last loop iteration; the transpiled code does
 * the last, so the loop's leftover scratch registers are the lifted code's. */
#include "r3_native_dof_job.h"
#include <string.h>

volatile long long g_r3dofj_stat[16];
volatile int g_r3dofj_level = 1;

#if R3DOFJ_NATIVE
#include <immintrin.h>
#include <intrin.h>

const int g_r3dofj_native = 1;

#define LSM 0x3FFFFu

static inline __m256i bswap32x8(__m256i x)
{
    return _mm256_shuffle_epi8(x, _mm256_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12,
                                                   3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12));
}
/* a u128 register (host word order = SPU word order) duplicated to 8 lanes */
static inline __m256i dup128(u128 v)
{
    __m128i x; memcpy(&x, &v, 16);
    return _mm256_broadcastsi128_si256(x);
}
static inline __m256 xf_in(__m256i x)
{
    const __m256i sm = _mm256_set1_epi32((int)0x80000000u);
    const __m256i mag = _mm256_min_epu32(_mm256_andnot_si256(sm, x), _mm256_set1_epi32(0x7F7FFFFF));
    return _mm256_castsi256_ps(_mm256_or_si256(_mm256_and_si256(x, sm), mag));
}
static inline __m256i xf_out(__m256 r)
{
    const __m256 mx = _mm256_set1_ps(3.40282347e+38f);
    r = _mm256_min_ps(_mm256_max_ps(_mm256_add_ps(r, _mm256_setzero_ps()), _mm256_sub_ps(_mm256_setzero_ps(), mx)), mx);
    return _mm256_castps_si256(r);
}
static inline __m256i x_key(__m256i x)
{
    const __m256i m = _mm256_and_si256(x, _mm256_set1_epi32(0x7FFFFFFF));
    const __m256i den = _mm256_cmpeq_epi32(_mm256_and_si256(x, _mm256_set1_epi32(0x7F800000)), _mm256_setzero_si256());
    const __m256i mag = _mm256_andnot_si256(den, m);
    const __m256i neg = _mm256_srai_epi32(x, 31);
    return _mm256_sub_epi32(_mm256_xor_si256(mag, neg), neg);
}

static inline __m256i x_frest(__m256i x)
{
    static const __attribute__((aligned(32))) uint32_t lut[32] = {
        0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,
        0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,
        0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,
        0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07 };
    const __m256i idx = _mm256_srli_epi32(x, 18);    /* low 3 bits pick within a row of 8 */
    const __m256 t0 = _mm256_castsi256_ps(_mm256_permutevar8x32_epi32(_mm256_load_si256((const __m256i*)(lut + 0)), idx));
    const __m256 t1 = _mm256_castsi256_ps(_mm256_permutevar8x32_epi32(_mm256_load_si256((const __m256i*)(lut + 8)), idx));
    const __m256 t2 = _mm256_castsi256_ps(_mm256_permutevar8x32_epi32(_mm256_load_si256((const __m256i*)(lut + 16)), idx));
    const __m256 t3 = _mm256_castsi256_ps(_mm256_permutevar8x32_epi32(_mm256_load_si256((const __m256i*)(lut + 24)), idx));
    const __m256 b3 = _mm256_castsi256_ps(_mm256_slli_epi32(x, 31 - 21));   /* x bit 21 = idx bit 3 -> sign */
    const __m256 b4 = _mm256_castsi256_ps(_mm256_slli_epi32(x, 31 - 22));   /* x bit 22 = idx bit 4 -> sign */
    const __m256i frac = _mm256_castps_si256(_mm256_blendv_ps(_mm256_blendv_ps(t0, t1, b3), _mm256_blendv_ps(t2, t3, b3), b4));
    const __m256i e = _mm256_and_si256(_mm256_srli_epi32(x, 23), _mm256_set1_epi32(0xFF));
    __m256i re = _mm256_max_epi32(_mm256_sub_epi32(_mm256_set1_epi32(253), e), _mm256_setzero_si256());
    re = _mm256_blendv_epi8(re, _mm256_set1_epi32(255), _mm256_cmpeq_epi32(e, _mm256_setzero_si256()));
    return _mm256_or_si256(_mm256_or_si256(_mm256_and_si256(x, _mm256_set1_epi32((int)0x80000000u)), _mm256_slli_epi32(re, 23)), frac);
}
/* spu_fi(a, b): b = estimate, a supplies Y */
static inline __m256i x_fi(__m256i a, __m256i est)
{
    const __m256i e = _mm256_and_si256(_mm256_srli_epi32(est, 23), _mm256_set1_epi32(0xFF));
    const __m256i A = _mm256_slli_epi32(_mm256_or_si256(_mm256_and_si256(_mm256_srli_epi32(est, 10), _mm256_set1_epi32(0x1FFF)),
                                                        _mm256_set1_epi32(0x2000)), 18);
    const __m256i p = _mm256_mullo_epi32(_mm256_and_si256(est, _mm256_set1_epi32(0x3FF)), _mm256_and_si256(a, _mm256_set1_epi32(0x7FFFF)));
    const __m256i h = _mm256_sub_epi32(A, _mm256_srli_epi32(_mm256_add_epi32(p, _mm256_set1_epi32(1)), 1));
    const __m256i hi = _mm256_srai_epi32(h, 31);                           /* m >= 2^32: no normalising shift */
    const __m256i frac = _mm256_and_si256(_mm256_blendv_epi8(_mm256_srli_epi32(h, 7), _mm256_srli_epi32(h, 8), hi),
                                          _mm256_set1_epi32(0x7FFFFF));
    const __m256i e2 = _mm256_add_epi32(e, _mm256_andnot_si256(hi, _mm256_set1_epi32(-1)));  /* e - 1 when shifted */
    const __m256i zero = _mm256_or_si256(_mm256_cmpeq_epi32(e, _mm256_setzero_si256()), _mm256_cmpeq_epi32(e2, _mm256_setzero_si256()));
    const __m256i r = _mm256_or_si256(_mm256_or_si256(_mm256_and_si256(est, _mm256_set1_epi32((int)0x80000000u)), _mm256_slli_epi32(e2, 23)), frac);
    return _mm256_andnot_si256(zero, r);
}

/* cuflt(cfltu(y, 141), 123) on 4 words, as the helpers compute it (spu__x_cfltu
 * then spu_cuflt + spu__x_dsf): the u32 round trip in between is exact, so it is
 * floor(clamp(y * 2^32, 0, 2^32 - 1)) * 2^-32 truncated to a 24-bit single. */
static inline __m128i m6_conv(__m128i y)
{
    __m256d d = _mm256_mul_pd(spu__x_sd(y), _mm256_set1_pd(4294967296.0));
    d = _mm256_floor_pd(_mm256_min_pd(_mm256_max_pd(d, _mm256_setzero_pd()), _mm256_set1_pd(4294967295.0)));
    d = _mm256_mul_pd(d, _mm256_set1_pd(1.0 / 4294967296.0));
    const __m256d t = _mm256_castsi256_pd(_mm256_and_si256(_mm256_castpd_si256(d), _mm256_set1_epi64x(~0x1FFFFFFFll)));
    const __m256d tiny = _mm256_cmp_pd(_mm256_andnot_pd(_mm256_set1_pd(-0.0), t), _mm256_set1_pd(1.1754943508222875e-38), _CMP_LT_OQ);
    return _mm_castps_si128(_mm256_cvtpd_ps(_mm256_andnot_pd(tiny, t)));
}

/* [a, a+an) and [b, b+bn) in LS, no wrap */
static inline int disjoint(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) { return a + an <= b || b + bn <= a; }

/* Mode 4: for each word w of src, d = cuflt(w >> 8, 155);
 * s = fcgt(d, p10) ? fma(d, p6, p7) : fma(d, p8, p9);
 * e = fi(s, frest(s)); out = fma(fnms(e, s, 1), e, e). */
int r3dofj_m4_bulk(uint8_t* ls, uint32_t src, uint32_t dst, uint32_t top,
                   u128 p6, u128 p7, u128 p8, u128 p9, u128 p10)
{
    const uint32_t s = src & LSM & ~15u, d = dst & LSM & ~15u;
    if (top < 64 || (top & 63) || top > SPU_LS_SIZE ||
        (src & LSM) + top + 64 > SPU_LS_SIZE || (dst & LSM) + top + 64 > SPU_LS_SIZE ||
        (s != d && !disjoint(s, top + 64, d + 64, top))) {
        _InterlockedIncrement64(&g_r3dofj_stat[1]);
        return 0;
    }
    /* Operand conditioning hoisted out of the loop: xf_in of the parameters is
     * loop-invariant; x = cuflt(w >> 8) is a non-negative normal or +0, so
     * xf_in(x) = x and its fcgt key is its own bits; sel and the fnms result
     * come out of xf_out (finite), so xf_in leaves them alone. e = fi(...) can
     * carry exponent 255 and keeps its xf_in. */
    const __m256 F6 = xf_in(dup128(p6)), F7 = xf_in(dup128(p7)), F8 = xf_in(dup128(p8)), F9 = xf_in(dup128(p9));
    const __m256i K10 = x_key(dup128(p10));
    const __m256 one = _mm256_set1_ps(1.0f);
    const uint8_t* sp = ls + s + 64;
    uint8_t* dp = ls + d + 64;
    for (uint32_t o = 0; o < top; o += 32) {
        const __m256i w = bswap32x8(_mm256_loadu_si256((const __m256i*)(sp + o)));
        const __m256 x = _mm256_cvtepi32_ps(_mm256_srli_epi32(w, 8));
        const __m256i a = xf_out(_mm256_fmadd_ps(x, F6, F7)), b = xf_out(_mm256_fmadd_ps(x, F8, F9));
        const __m256i sel = _mm256_blendv_epi8(b, a, _mm256_cmpgt_epi32(_mm256_castps_si256(x), K10));
        const __m256 e = xf_in(x_fi(sel, x_frest(sel)));
        const __m256 n = _mm256_castsi256_ps(xf_out(_mm256_fnmadd_ps(e, _mm256_castsi256_ps(sel), one)));
        const __m256i r = xf_out(_mm256_fmadd_ps(n, e, e));
        _mm256_storeu_si256((__m256i*)(dp + o), bswap32x8(r));
    }
    _InterlockedIncrement64(&g_r3dofj_stat[0]);
    _InterlockedExchangeAdd64(&g_r3dofj_stat[2], top / 64);
    return 1;
}

/* Mode 6: for each word x of src: out = cuflt(cfltu(fma(x, p6, p7), 141), 123) & fcgt(x, p8).
 * cfltu / cuflt: see m6_conv. */
int r3dofj_m6_bulk(uint8_t* ls, uint32_t src, uint32_t dst, uint32_t top,
                   u128 p6, u128 p7, u128 p8)
{
    const uint32_t s = src & LSM & ~15u, d = dst & LSM & ~15u;
    if (top < 128 || (top & 127) || top > SPU_LS_SIZE ||
        (src & LSM) + top + 128 > SPU_LS_SIZE || (dst & LSM) + top + 128 > SPU_LS_SIZE ||
        (s != d && !disjoint(s, top + 128, d + 128, top))) {
        _InterlockedIncrement64(&g_r3dofj_stat[4]);
        return 0;
    }
    const __m256 F6 = xf_in(dup128(p6)), F7 = xf_in(dup128(p7));
    const __m256i K8 = x_key(dup128(p8));
    const uint8_t* sp = ls + s + 128;
    uint8_t* dp = ls + d + 128;
    for (uint32_t o = 0; o < top; o += 32) {
        const __m256i x = bswap32x8(_mm256_loadu_si256((const __m256i*)(sp + o)));
        const __m256i y = xf_out(_mm256_fmadd_ps(xf_in(x), F6, F7));
        const __m256i r = _mm256_setr_m128i(m6_conv(_mm256_castsi256_si128(y)), m6_conv(_mm256_extracti128_si256(y, 1)));
        _mm256_storeu_si256((__m256i*)(dp + o), bswap32x8(_mm256_and_si256(r, _mm256_cmpgt_epi32(x_key(x), K8))));
    }
    _InterlockedIncrement64(&g_r3dofj_stat[3]);
    _InterlockedExchangeAdd64(&g_r3dofj_stat[5], top / 128);
    return 1;
}

static inline int same128(u128 a, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    return a._u32[0] == w0 && a._u32[1] == w1 && a._u32[2] == w2 && a._u32[3] == w3;
}
/* top byte of cfltu(v, 141) (u = sat(floor(v * 2^32))): 0 for v <= 0 or exponent 0,
 * 255 for v >= 1 (exponent 255 included), else floor(v * 256) (v * 256 is exact). */
static inline __m256i x_b141(__m256i v)
{
    const __m256i t = _mm256_cvttps_epi32(_mm256_mul_ps(_mm256_castsi256_ps(v), _mm256_set1_ps(256.0f)));
    const __m256i lo = _mm256_cmpgt_epi32(_mm256_set1_epi32(0x00800000), v);
    const __m256i hi = _mm256_cmpgt_epi32(v, _mm256_set1_epi32(0x3F7FFFFF));
    return _mm256_blendv_epi8(_mm256_andnot_si256(lo, t), _mm256_set1_epi32(255), hi);
}
/* top 16 bits of cfltu(v, 145) (scale 2^28): 0xFFFF for v >= 16, else floor(v * 4096). */
static inline __m256i x_b145(__m256i v)
{
    const __m256i t = _mm256_cvttps_epi32(_mm256_mul_ps(_mm256_castsi256_ps(v), _mm256_set1_ps(4096.0f)));
    const __m256i lo = _mm256_cmpgt_epi32(_mm256_set1_epi32(0x00800000), v);
    const __m256i hi = _mm256_cmpgt_epi32(v, _mm256_set1_epi32(0x417FFFFF));
    return _mm256_blendv_epi8(_mm256_andnot_si256(lo, t), _mm256_set1_epi32(0xFFFF), hi);
}
/* cuflt(cfltu(v, 141), 123) = floor(v * 2^32) * 2^-32 for 0 < v < 1 (it has at most
 * v's 24 significant bits, so the SPU truncation to a single is exact), 1 - 2^-24 for
 * v >= 1, 0 for v <= 0 or exponent 0. */
static inline __m256i x_c141_123(__m256i v)
{
    const __m256 f = _mm256_mul_ps(_mm256_round_ps(_mm256_mul_ps(_mm256_castsi256_ps(v), _mm256_set1_ps(4294967296.0f)),
                                                   _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC),
                                   _mm256_set1_ps(1.0f / 4294967296.0f));
    const __m256i lo = _mm256_cmpgt_epi32(_mm256_set1_epi32(0x00800000), v);
    const __m256i hi = _mm256_cmpgt_epi32(v, _mm256_set1_epi32(0x3F7FFFFF));
    return _mm256_blendv_epi8(_mm256_andnot_si256(lo, _mm256_castps_si256(f)), _mm256_set1_epi32(0x3F7FFFFF), hi);
}

/* Mode 8 (loop 0x1150/0x11E8), per word x of src, m of msk (p = r7..r17):
 * g = fcgt(x, p15); c = g ? fma(x, p7, p8) : fma(x, p9, p10);
 * w = fm(m & g, p12); r = fma(cuflt(cfltu(c, 141), 123), g ? p13 : p11, w);
 * out = top byte of cfltu(fm(x, p16), 141) << 24 | top byte of cfltu(fcgt(w, 0) ? 1 : p17, 141) << 16
 *     | top 16 bits of cfltu(fcgt(r, p14) ? p14 : r, 145).
 * Groups top..64 (64 bytes each) of src / msk -> dst. */
int r3dofj_m8_bulk(uint8_t* ls, uint32_t src, uint32_t msk, uint32_t dst, uint32_t top,
                   const u128* p, u128 s22, u128 s23)
{
    if (g_r3dofj_level < 2) return 0;
    const uint32_t s = src & LSM & ~15u, m = msk & LSM & ~15u, d = dst & LSM & ~15u;
    if (top < 64 || (top & 63) || top > SPU_LS_SIZE - 64 ||
        s + top + 64 > SPU_LS_SIZE || m + top + 64 > SPU_LS_SIZE || d + top + 64 > SPU_LS_SIZE ||
        (d != s && !disjoint(s, top + 64, d, top + 64)) || (d != m && !disjoint(m, top + 64, d, top + 64)) ||
        !same128(s22, 0x80100001u, 0x80140405u, 0x80180809u, 0x801C0C0Du) ||
        !same128(s23, 0x10010203u, 0x14050607u, 0x18090A0Bu, 0x1C0D0E0Fu)) {
        _InterlockedIncrement64(&g_r3dofj_stat[10]);
        return 0;
    }
    const __m256 F7 = xf_in(dup128(p[0])), F8 = xf_in(dup128(p[1])), F9 = xf_in(dup128(p[2])), F10 = xf_in(dup128(p[3]));
    const __m256i P11 = dup128(p[4]), P13 = dup128(p[6]), P14 = dup128(p[7]), P17 = dup128(p[10]);
    const __m256 F11 = xf_in(P11), F13 = xf_in(P13), F12 = xf_in(dup128(p[5])), F16 = xf_in(dup128(p[9]));
    const __m256i K14 = x_key(P14), K15 = x_key(dup128(p[8]));
    const __m256i b17 = x_b141(P17), b1 = _mm256_set1_epi32(255);
    for (uint32_t o = 64; o < top + 64; o += 32) {
        const __m256i x = bswap32x8(_mm256_loadu_si256((const __m256i*)(ls + s + o)));
        const __m256i mm = bswap32x8(_mm256_loadu_si256((const __m256i*)(ls + m + o)));
        const __m256 X = xf_in(x);
        const __m256i g = _mm256_cmpgt_epi32(x_key(x), K15);
        const __m256i a = xf_out(_mm256_fmadd_ps(X, F7, F8)), b = xf_out(_mm256_fmadd_ps(X, F9, F10));
        const __m256 cf = _mm256_castsi256_ps(x_c141_123(_mm256_blendv_epi8(b, a, g)));
        const __m256i w = xf_out(_mm256_mul_ps(xf_in(_mm256_and_si256(mm, g)), F12));
        const __m256i r = xf_out(_mm256_fmadd_ps(cf, _mm256_blendv_ps(F11, F13, _mm256_castsi256_ps(g)), _mm256_castsi256_ps(w)));
        const __m256i rc = _mm256_blendv_epi8(r, P14, _mm256_cmpgt_epi32(x_key(r), K14));
        const __m256i sb = _mm256_blendv_epi8(b17, b1, _mm256_cmpgt_epi32(x_key(w), _mm256_setzero_si256()));
        const __m256i db = x_b141(xf_out(_mm256_mul_ps(X, F16)));
        const __m256i out = _mm256_or_si256(_mm256_or_si256(_mm256_slli_epi32(db, 24), _mm256_slli_epi32(sb, 16)), x_b145(rc));
        _mm256_storeu_si256((__m256i*)(ls + d + o), bswap32x8(out));
    }
    _InterlockedIncrement64(&g_r3dofj_stat[9]);
    _InterlockedExchangeAdd64(&g_r3dofj_stat[11], top / 64);
    return 1;
}

static inline __m256i ld2x128(const uint8_t* ls, uint32_t a, uint32_t b)
{
    return _mm256_setr_m128i(_mm_loadu_si128((const __m128i*)(ls + (a & LSM & ~15u))),
                             _mm_loadu_si128((const __m128i*)(ls + (b & LSM & ~15u))));
}
/* 8 LS quads (lanes l = 0..7) -> their words 0..3 as 8-lane vectors (host order) */
static inline void quads8(const uint8_t* ls, const uint32_t* a, __m256i* w0, __m256i* w1, __m256i* w2, __m256i* w3)
{
    const __m256i A0 = bswap32x8(ld2x128(ls, a[0], a[4])), A1 = bswap32x8(ld2x128(ls, a[1], a[5]));
    const __m256i A2 = bswap32x8(ld2x128(ls, a[2], a[6])), A3 = bswap32x8(ld2x128(ls, a[3], a[7]));
    const __m256i t0 = _mm256_unpacklo_epi32(A0, A1), t1 = _mm256_unpacklo_epi32(A2, A3);
    const __m256i t2 = _mm256_unpackhi_epi32(A0, A1), t3 = _mm256_unpackhi_epi32(A2, A3);
    *w0 = _mm256_unpacklo_epi64(t0, t1); *w1 = _mm256_unpackhi_epi64(t0, t1);
    *w2 = _mm256_unpacklo_epi64(t2, t3); *w3 = _mm256_unpackhi_epi64(t2, t3);
}
/* cflts(x, 173): truncate toward zero, saturating (exponent 0 -> 0) */
static inline __m256i x_cflts0(__m256 x)
{
    const __m256i t = _mm256_cvttps_epi32(x);
    return _mm256_blendv_epi8(t, _mm256_set1_epi32(0x7FFFFFFF), _mm256_castps_si256(_mm256_cmp_ps(x, _mm256_set1_ps(2147483648.0f), _CMP_GE_OQ)));
}
/* mpya(a, b, c) = (int16)a * (int16)b + c */
static inline __m256i x_mpya(__m256i a, __m256i b, __m256i c)
{
    const __m256i lo = _mm256_set1_epi32(0xFFFF);
    return _mm256_add_epi32(_mm256_madd_epi16(_mm256_and_si256(a, lo), _mm256_and_si256(b, lo)), c);
}

/* Mode 9 gather blur (F_017C0, outer loop 0x1810): out[n] (4 packed pixels) from the
 * 4 float pixels at in + 64 n and 16 neighbours each. Per pixel P (words r, g, b, c):
 *   coc = cuflt(c & 0xFFFF, 143), k = cuflt(c >> 16 & 255, 147), dep = c >> 24;
 *   acc = P, wsum = 1; for sample j in 12..15, 8..11, 4..7, 0..3 (table T at tab):
 *     off = mpya(cflts(fma(T[j].y, coc, .5)), stride, mpya(cflts(fma(T[j].x, coc, .5)), 16, lane16));
 *     G = LS quad at in + 64 n + off; rw = cuflt(G.c & 0xFFFF, 143);
 *     w = dep > G.c >> 24 ? fm(rw, k) : rw; wsum = fa(wsum, w); acc = fma(G, w, acc);
 *   e = fi(wsum, frest(wsum)); out word = b(coc) << 24 | b(fm(acc.r, e)) << 16 | b(..g..) << 8 | b(..b..)
 * with b = top byte of cfltu(., 141). A group whose 4 (c & 0xFFFF) are all 0 is
 * skipped: out = the bytes of P's r, g, b and 0.
 * Does groups 0 .. L-1, L = the last group not skipped (the transpiled loop runs
 * from there, so the scratch registers a trailing run of skipped groups keeps are
 * the lifted code's), and returns L (0: nothing done).
 * m: the loop's shuffle constants S_36 S_34 S_33 S_31 S_29 S_27 S_25 S_37. */
uint32_t r3dofj_m9g_bulk(uint8_t* ls, uint32_t in, uint32_t out, uint32_t cnt, uint32_t tab,
                         u128 stride, u128 lane16, const u128* m)
{
    if (g_r3dofj_level < 2) return 0;
    const uint32_t N = cnt / 4, is = in & LSM & ~15u, os = out & LSM & ~15u, ts = tab & LSM & ~15u;
    if (!N || (cnt & 3) || N > SPU_LS_SIZE / 64 || is + 64 * N > SPU_LS_SIZE || os + 16 * N > SPU_LS_SIZE ||
        ts + 256 > SPU_LS_SIZE || !disjoint(os, 16 * N, is, 64 * N) || !disjoint(os, 16 * N, ts, 256) ||
        !same128(m[0], 0x0C0D0E0Fu, 0x1C1D1E1Fu, 0x80808080u, 0x80808080u) ||
        !same128(m[1], 0x80808080u, 0x80808080u, 0x0C0D0E0Fu, 0x1C1D1E1Fu) ||
        !same128(m[2], 0x00010203u, 0x04050607u, 0x08090A0Bu, 0x10111213u) ||
        !same128(m[3], 0x00010203u, 0x04050607u, 0x08090A0Bu, 0x14151617u) ||
        !same128(m[4], 0x00010203u, 0x04050607u, 0x08090A0Bu, 0x18191A1Bu) ||
        !same128(m[5], 0x00010203u, 0x04050607u, 0x08090A0Bu, 0x1C1D1E1Fu) ||
        !same128(m[6], 0x0C000408u, 0x1C101418u, 0x80808080u, 0x80808080u) ||
        !same128(m[7], 0x80808080u, 0x80808080u, 0x0C000408u, 0x1C101418u)) {
        _InterlockedIncrement64(&g_r3dofj_stat[7]);
        return 0;
    }
    /* L = the last group with a nonzero c & 0xFFFF (the inputs are not written: os is disjoint) */
    uint32_t L = N;
    while (L) {
        const uint8_t* q = ls + is + 64 * (L - 1);
        if ((q[14] | q[15] | q[30] | q[31] | q[46] | q[47] | q[62] | q[63]) != 0) break;
        L--;
    }
    if (L) L--;
    if (!L) {
        _InterlockedIncrement64(&g_r3dofj_stat[7]);
        return 0;
    }
    static const uint8_t order[16] = { 12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3 };
    __m256 TX[16], TY[16];
    for (int j = 0; j < 16; j++) {
        const uint8_t* t = ls + ((ts + 16u * order[j]) & LSM);
        TX[j] = xf_in(_mm256_set1_epi32((int)((uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3])));
        TY[j] = xf_in(_mm256_set1_epi32((int)((uint32_t)t[4] << 24 | (uint32_t)t[5] << 16 | (uint32_t)t[6] << 8 | t[7])));
    }
    const __m256i ST = dup128(stride), LN = _mm256_add_epi32(dup128(lane16), _mm256_setr_epi32(0, 0, 0, 0, 64, 64, 64, 64));
    const __m256i FF = _mm256_set1_epi32(0xFFFF);
    const __m256 half = _mm256_set1_ps(0.5f), s12 = _mm256_set1_ps(1.0f / 4096.0f), s8 = _mm256_set1_ps(1.0f / 256.0f);
    __attribute__((aligned(32))) uint32_t ad[8];
    uint32_t g = 0;
    while (g < L) {
        const uint32_t ib = in + 64 * g;
        for (int l = 0; l < 8; l++) ad[l] = ib + 16u * (uint32_t)l;
        __m256i R, G, B, C;
        quads8(ls, ad, &R, &G, &B, &C);
        const __m256i cl = _mm256_and_si256(C, FF);
        const __m256 coc = _mm256_mul_ps(_mm256_cvtepi32_ps(cl), s12);
        const __m256 k = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(C, 16), _mm256_set1_epi32(255))), s8);
        const __m256i dep = _mm256_srli_epi32(C, 24);
        __m256 aR = xf_in(R), aG = xf_in(G), aB = xf_in(B), ws = _mm256_set1_ps(1.0f);
        const __m256i base = _mm256_add_epi32(_mm256_set1_epi32((int)ib), LN);
        const __m256i oq = _mm256_set1_epi32((int)((out + 16 * g) & LSM & ~15u));
        __m256i hz = _mm256_setzero_si256();
        for (int j = 0; j < 16; j++) {
            const __m256i ix = x_cflts0(_mm256_fmadd_ps(TX[j], coc, half));
            const __m256i iy = x_cflts0(_mm256_fmadd_ps(TY[j], coc, half));
            const __m256i a = _mm256_add_epi32(x_mpya(iy, ST, _mm256_madd_epi16(_mm256_and_si256(ix, FF), _mm256_set1_epi32(16))), base);
            const __m256i aq = _mm256_and_si256(a, _mm256_set1_epi32((int)(LSM & ~15u)));
            hz = _mm256_or_si256(hz, _mm256_cmpeq_epi32(aq, oq));
            _mm256_store_si256((__m256i*)ad, a);
            __m256i gR, gG, gB, gC;
            quads8(ls, ad, &gR, &gG, &gB, &gC);
            const __m256 rw = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(gC, FF)), s12);
            const __m256 w = _mm256_blendv_ps(rw, _mm256_mul_ps(rw, k), _mm256_castsi256_ps(_mm256_cmpgt_epi32(dep, _mm256_srli_epi32(gC, 24))));
            ws = _mm256_add_ps(ws, w);
            aR = _mm256_castsi256_ps(xf_out(_mm256_fmadd_ps(xf_in(gR), w, aR)));
            aG = _mm256_castsi256_ps(xf_out(_mm256_fmadd_ps(xf_in(gG), w, aG)));
            aB = _mm256_castsi256_ps(xf_out(_mm256_fmadd_ps(xf_in(gB), w, aB)));
        }
        const __m256i wsi = _mm256_castps_si256(ws);
        const __m256 e = xf_in(x_fi(wsi, x_frest(wsi)));
        const __m256i px = _mm256_or_si256(_mm256_or_si256(_mm256_slli_epi32(x_b141(_mm256_castps_si256(coc)), 24),
                                                           _mm256_slli_epi32(x_b141(xf_out(_mm256_mul_ps(aR, e))), 16)),
                                           _mm256_or_si256(_mm256_slli_epi32(x_b141(xf_out(_mm256_mul_ps(aG, e))), 8),
                                                           x_b141(xf_out(_mm256_mul_ps(aB, e)))));
        const __m256i sk = _mm256_or_si256(_mm256_or_si256(_mm256_slli_epi32(x_b141(R), 16), _mm256_slli_epi32(x_b141(G), 8)), x_b141(B));
        /* per group: skipped when its 4 (c & 0xFFFF) are 0 */
        const __m256i z = _mm256_cmpeq_epi32(cl, _mm256_setzero_si256());
        const int zm = _mm256_movemask_ps(_mm256_castsi256_ps(z));
        const __m256i skm = _mm256_setr_epi32(-((zm & 15) == 15), -((zm & 15) == 15), -((zm & 15) == 15), -((zm & 15) == 15),
                                              -((zm >> 4) == 15), -((zm >> 4) == 15), -((zm >> 4) == 15), -((zm >> 4) == 15));
        const __m256i res = bswap32x8(_mm256_blendv_epi8(px, sk, skm));
        _mm_storeu_si128((__m128i*)(ls + ((out + 16 * g) & LSM & ~15u)), _mm256_castsi256_si128(res));
        /* the second group too, unless it is past L or read the first group's output */
        if (g + 1 < L && !(_mm256_movemask_ps(_mm256_castsi256_ps(hz)) & 0xF0)) {
            _mm_storeu_si128((__m128i*)(ls + ((out + 16 * (g + 1)) & LSM & ~15u)), _mm256_extracti128_si256(res, 1));
            g += 2;
        } else
            g += 1;
    }
    _InterlockedIncrement64(&g_r3dofj_stat[6]);
    _InterlockedExchangeAdd64(&g_r3dofj_stat[8], L);
    return L;
}

#else
/* Built without AVX2+FMA / SPU_XF_NO_EXACT: the helpers the lifted code uses
 * have other semantics here (unfused fma, runtime exact-float switch), so no
 * bulk path: return 0 and the transpiled code runs every iteration. */
const int g_r3dofj_native = 0;

int r3dofj_m4_bulk(uint8_t* ls, uint32_t src, uint32_t dst, uint32_t top,
                   u128 p6, u128 p7, u128 p8, u128 p9, u128 p10)
{
    (void)ls; (void)src; (void)dst; (void)top; (void)p6; (void)p7; (void)p8; (void)p9; (void)p10;
    return 0;
}

int r3dofj_m6_bulk(uint8_t* ls, uint32_t src, uint32_t dst, uint32_t top,
                   u128 p6, u128 p7, u128 p8)
{
    (void)ls; (void)src; (void)dst; (void)top; (void)p6; (void)p7; (void)p8;
    return 0;
}

int r3dofj_m8_bulk(uint8_t* ls, uint32_t src, uint32_t msk, uint32_t dst, uint32_t top,
                   const u128* p, u128 s22, u128 s23)
{
    (void)ls; (void)src; (void)msk; (void)dst; (void)top; (void)p; (void)s22; (void)s23;
    return 0;
}

uint32_t r3dofj_m9g_bulk(uint8_t* ls, uint32_t in, uint32_t out, uint32_t cnt, uint32_t tab,
                         u128 stride, u128 lane16, const u128* m)
{
    (void)ls; (void)in; (void)out; (void)cnt; (void)tab; (void)stride; (void)lane16; (void)m;
    return 0;
}
#endif
