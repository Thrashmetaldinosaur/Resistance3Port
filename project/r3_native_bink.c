/* Native kernels for job image 421 (igjob_00E0A480, the Bink video decoder): the three 8x8
 * dequantise + IDCT + block-output leaf functions, behind r3nj_gate (r3_native_jobs.c).
 *
 *   R3NJ_K_BINK_IDCT_PUT    0xC288  r3 dst, r4 stride, r5 coefs (int16[64]), r6 quant (int32[64], LS address)
 *   R3NJ_K_BINK_IDCT_PUT2X  0xC8C0  the same, 16x16 output (each pixel doubled across and down);
 *                                   quant = 0x16A80 + (r7 - r8) + (r6 << 8)  (r7/r8: the caller's PIC pair)
 *   R3NJ_K_BINK_IDCT_ADD    0xCF30  dst = ref + idct: r7 ref, r8 ref stride (0 allowed), quant = 0x15A80 + (r9 - r10) + (r6 << 8)
 *
 * What the lifted code does (verified byte for byte by scratch/native_bink/test_bink):
 *   pass 1, per column c: s_k = (int32)(coef[k][c] * quant[k][c]) >> 11 (32-bit wrapping product, arithmetic
 *          shift), the Bink 8-point transform down the column, results to an int32[8][8] stack buffer at
 *          r1 - 288 + 32 (big-endian, row j = output j). (The lifted all-AC-zero shortcut gives the same values.)
 *   pass 2, per row: the same transform along the row, pixel = (x + 127) >> 8, low byte stored (no clamp);
 *          the ADD variant stores the low byte of ref + pixel.
 *   The stack frame is r1 - 288 with the caller's r1 quadword stored at its base (the back chain).
 * Only r0/r1/r80+ are callee-visible: the kernels leave every register as it was (r1 is restored on return).
 * A call the kernel cannot reproduce exactly (unaligned operands, overlapping buffers, ranges that wrap the
 * local store) is declined before anything is written, and the lifted code runs.
 * AVX2 builds only; others always decline. */
#include "spu_context.h"
#include "r3_native_jobs.h"
#include <string.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

#define LS_SZ 0x40000u

static int rng_ok(uint32_t a, uint32_t n) { return a < LS_SZ && n <= LS_SZ - a; }
static int rng_ovl(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) { return a < b + bn && b < a + an; }

#if defined(__AVX2__)
static inline __m256i bk_mulsh(__m256i a, int k) { return _mm256_srai_epi32(_mm256_mullo_epi32(a, _mm256_set1_epi32(k)), 11); }

/* Bink 8-point IDCT on 8 lanes at once (s[k] = input k of every lane) */
static inline void bk_idct8(const __m256i* s, __m256i* d)
{
    const __m256i a0 = _mm256_add_epi32(s[0], s[4]), a1 = _mm256_sub_epi32(s[0], s[4]);
    const __m256i a2 = _mm256_add_epi32(s[2], s[6]), a3 = bk_mulsh(_mm256_sub_epi32(s[2], s[6]), 2896);
    const __m256i a4 = _mm256_add_epi32(s[5], s[3]), a5 = _mm256_sub_epi32(s[5], s[3]);
    const __m256i a6 = _mm256_add_epi32(s[1], s[7]), a7 = _mm256_sub_epi32(s[1], s[7]);
    const __m256i b0 = _mm256_add_epi32(a4, a6);
    const __m256i b1 = bk_mulsh(_mm256_add_epi32(a5, a7), 3784);
    const __m256i b2 = _mm256_add_epi32(_mm256_sub_epi32(bk_mulsh(a5, -5352), b0), b1);
    const __m256i b3 = _mm256_sub_epi32(bk_mulsh(_mm256_sub_epi32(a6, a4), 2896), b2);
    const __m256i b4 = _mm256_sub_epi32(_mm256_add_epi32(bk_mulsh(a7, 2217), b3), b1);
    const __m256i p02 = _mm256_add_epi32(a0, a2), m02 = _mm256_sub_epi32(a0, a2);
    const __m256i q1 = _mm256_sub_epi32(_mm256_add_epi32(a1, a3), a2);   /* a1 + a3 - a2 */
    const __m256i r1 = _mm256_add_epi32(_mm256_sub_epi32(a1, a3), a2);   /* a1 - a3 + a2 */
    d[0] = _mm256_add_epi32(p02, b0);  d[7] = _mm256_sub_epi32(p02, b0);
    d[1] = _mm256_add_epi32(q1, b2);   d[6] = _mm256_sub_epi32(q1, b2);
    d[2] = _mm256_add_epi32(r1, b3);   d[5] = _mm256_sub_epi32(r1, b3);
    d[3] = _mm256_sub_epi32(m02, b4);  d[4] = _mm256_add_epi32(m02, b4);
}

static inline void bk_tr8(__m256i* r)
{
    const __m256i t0 = _mm256_unpacklo_epi32(r[0], r[1]), t1 = _mm256_unpackhi_epi32(r[0], r[1]);
    const __m256i t2 = _mm256_unpacklo_epi32(r[2], r[3]), t3 = _mm256_unpackhi_epi32(r[2], r[3]);
    const __m256i t4 = _mm256_unpacklo_epi32(r[4], r[5]), t5 = _mm256_unpackhi_epi32(r[4], r[5]);
    const __m256i t6 = _mm256_unpacklo_epi32(r[6], r[7]), t7 = _mm256_unpackhi_epi32(r[6], r[7]);
    const __m256i u0 = _mm256_unpacklo_epi64(t0, t2), u1 = _mm256_unpackhi_epi64(t0, t2);
    const __m256i u2 = _mm256_unpacklo_epi64(t1, t3), u3 = _mm256_unpackhi_epi64(t1, t3);
    const __m256i u4 = _mm256_unpacklo_epi64(t4, t6), u5 = _mm256_unpackhi_epi64(t4, t6);
    const __m256i u6 = _mm256_unpacklo_epi64(t5, t7), u7 = _mm256_unpackhi_epi64(t5, t7);
    r[0] = _mm256_permute2x128_si256(u0, u4, 0x20); r[1] = _mm256_permute2x128_si256(u1, u5, 0x20);
    r[2] = _mm256_permute2x128_si256(u2, u6, 0x20); r[3] = _mm256_permute2x128_si256(u3, u7, 0x20);
    r[4] = _mm256_permute2x128_si256(u0, u4, 0x31); r[5] = _mm256_permute2x128_si256(u1, u5, 0x31);
    r[6] = _mm256_permute2x128_si256(u2, u6, 0x31); r[7] = _mm256_permute2x128_si256(u3, u7, 0x31);
}

/* kind 0 put 8x8, 1 put 16x16 doubled, 2 add */
static int bk_idct(uint8_t* ls, u128* gpr, int kind)
{
    const uint32_t sp = gpr[1]._u32[0];
    const uint32_t dst = gpr[3]._u32[0], dstr = gpr[4]._u32[0], coef = gpr[5]._u32[0];
    uint32_t q, ref = 0, rstr = 0;
    if (kind == 0) q = gpr[6]._u32[0];
    else if (kind == 1) q = 0x16A80u + (gpr[7]._u32[0] - gpr[8]._u32[0]) + (gpr[6]._u32[0] << 8);
    else { q = 0x15A80u + (gpr[9]._u32[0] - gpr[10]._u32[0]) + (gpr[6]._u32[0] << 8); ref = gpr[7]._u32[0]; rstr = gpr[8]._u32[0]; }

    /* exactness preconditions; anything else: decline, nothing written */
    if ((sp & 15u) || sp < 288u || sp > LS_SZ) return 0;
    const uint32_t frame = sp - 288u;
    if ((coef & 1u) || !rng_ok(coef, 128) || (q & 3u) || !rng_ok(q, 256)) return 0;
    if (rng_ovl(frame, 288, coef, 128) || rng_ovl(frame, 288, q, 256)) return 0;
    const uint32_t rows = kind == 1 ? 16u : 8u, rlen = kind == 1 ? 16u : 8u;
    if (dstr < rlen || dstr > LS_SZ || (kind == 1 && ((dst | dstr) & 3u))) return 0;
    if (!rng_ok(dst, dstr * (rows - 1) + rlen)) return 0;
    if (rng_ovl(frame, 288, dst, dstr * (rows - 1) + rlen)) return 0;
    if (kind == 2) {
        /* any stride works, including 0 (one ref row reused; the 0x5FD8 call site passes r8 = 0): ref is
         * only read, so its rows may overlap each other. A dst row over a ref row is declined, since the
         * lifted per-byte read/write interleave would then decide the result. */
        if (rstr > LS_SZ || !rng_ok(ref, rstr * 7 + 8)) return 0;
        if (rng_ovl(frame, 288, ref, rstr * 7 + 8)) return 0;   /* read after the frame is written */
        for (uint32_t i = 0; i < 8; i++)
            for (uint32_t j = 0; j < 8; j++)
                if (rng_ovl(dst + i * dstr, 8, ref + j * rstr, 8)) return 0;
    }

    const __m256i bs32 = _mm256_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12, 3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12);
    const __m128i bs16 = _mm_setr_epi8(1,0, 3,2, 5,4, 7,6, 9,8, 11,10, 13,12, 15,14);
    __m256i s[8], d[8];
    for (int k = 0; k < 8; k++) {
        const __m256i c = _mm256_cvtepi16_epi32(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + coef + 16u * k)), bs16));
        const __m256i qq = _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(ls + q + 32u * k)), bs32);
        s[k] = _mm256_srai_epi32(_mm256_mullo_epi32(c, qq), 11);
    }
    bk_idct8(s, d);   /* d[j] lane c = temp[j][c] */
    /* back chain, then the int32[8][8] buffer, as the lifted code leaves them */
    for (int w = 0; w < 4; w++) {
        const uint32_t v = __builtin_bswap32(gpr[1]._u32[w]);
        memcpy(ls + frame + 4u * w, &v, 4);
    }
    for (int j = 0; j < 8; j++) _mm256_storeu_si256((__m256i*)(ls + frame + 32u + 32u * j), _mm256_shuffle_epi8(d[j], bs32));
    bk_tr8(d);        /* d[c] lane j = temp[j][c] */
    bk_idct8(d, s);   /* s[i] lane j = row j, pixel i before rounding */
    const __m256i r127 = _mm256_set1_epi32(127);
    for (int i = 0; i < 8; i++) s[i] = _mm256_srai_epi32(_mm256_add_epi32(s[i], r127), 8);
    bk_tr8(s);        /* s[j] lane i = row j, pixel i */
    const __m256i lowb = _mm256_setr_epi8(0,4,8,12, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, 0,4,8,12, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1);
    for (uint32_t j = 0; j < 8; j++) {
        const __m256i b = _mm256_shuffle_epi8(s[j], lowb);
        const __m128i px = _mm_unpacklo_epi32(_mm256_castsi256_si128(b), _mm256_extracti128_si256(b, 1));   /* 8 bytes */
        if (kind == 0) {
            _mm_storel_epi64((__m128i*)(ls + dst + j * dstr), px);
        } else if (kind == 1) {
            const __m128i dbl = _mm_unpacklo_epi8(px, px);
            _mm_storeu_si128((__m128i*)(ls + dst + (2u * j) * dstr), dbl);
            _mm_storeu_si128((__m128i*)(ls + dst + (2u * j + 1u) * dstr), dbl);
        } else {
            const __m128i rp = _mm_loadl_epi64((const __m128i*)(ls + ref + j * rstr));
            _mm_storel_epi64((__m128i*)(ls + dst + j * dstr), _mm_add_epi8(rp, px));
        }
    }
    return 1;
}
int r3nj_bink_built(void) { return 1; }
#else
static int bk_idct(uint8_t* ls, u128* gpr, int kind) { (void)ls; (void)gpr; (void)kind; return 0; }
/* every call would decline: the gate keeps these kernels off ("needs an AVX2 build") */
int r3nj_bink_built(void) { return 0; }
#endif

int r3nj_bink_idct_put(uint8_t* ls, u128* gpr)   { return bk_idct(ls, gpr, 0); }
int r3nj_bink_idct_put2x(uint8_t* ls, u128* gpr) { return bk_idct(ls, gpr, 1); }
int r3nj_bink_idct_add(uint8_t* ls, u128* gpr)   { return bk_idct(ls, gpr, 2); }
