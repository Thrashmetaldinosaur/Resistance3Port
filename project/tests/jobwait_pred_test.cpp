/* R3_JOBWAIT_WAKE: r3_jobwait_pred (project\r3_jobwait_pred.h) against a transliteration of the
 * lifted exit test of func_0082C190 (recompiled\ppu_recomp_002.cpp, loc_0082C2DC..loc_0082C3F4 and
 * loc_0082C3F8..0082C42C, rotate helpers copied from the generated code). Random handles and
 * counter values, including the 128-wrap cases. Also checks that when the predicate says "wait",
 * the words the lifted code read lie inside the returned [lo,hi) range.
 * Build: clang-cl /nologo /O1 /EHsc /std:c++17 /I ..\..\project jobwait_pred_test.cpp */
#include "r3_jobwait_pred.h"
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <random>

static inline uint64_t ppc_rlw64(uint64_t rs, int sh, int mb, int me, uint64_t ra, int insert) {
    uint32_t w = (uint32_t)rs; sh &= 31;
    uint32_t rw = sh ? ((w << sh) | (w >> (32 - sh))) : w;
    uint64_t r = ((uint64_t)rw << 32) | rw;
    int x = mb + 32, y = me + 32;
    uint64_t m = (x <= y) ? ((~0ull >> x) & (~0ull << (63 - y))) : ((~0ull >> x) | (~0ull << (63 - y)));
    return insert ? ((r & m) | (ra & ~m)) : (r & m);
}
static inline uint64_t ppc_rotl64(uint64_t v, int n) { n &= 63; return n ? ((v << n) | (v >> (64 - n))) : v; }
static inline uint64_t ppc_mask64(int mb, int me) {
    uint64_t mask;
    if (mb <= me) { mask = (mb == 0) ? ~0ULL : (~0ULL >> mb); mask &= (me == 63) ? ~0ULL : (~0ULL << (63 - me)); }
    else mask = (~0ULL >> mb) | (~0ULL << (63 - me));
    return mask;
}
static inline uint64_t ppc_rldicl(uint64_t rs, int sh, int mb) { return ppc_rotl64(rs, sh) & ppc_mask64(mb, 63); }

static std::map<uint32_t, uint32_t> g_mem;
static uint32_t g_rd[4]; static int g_nrd;
static uint32_t rd16(uint32_t a) { g_rd[g_nrd++] = a; return g_mem[a] & 0xFFFF; }
static uint32_t rd32(uint32_t a) { g_rd[g_nrd++] = a; return g_mem[a]; }

/* One pass of the lifted test; returns 1 when the loop exits (r3 != 0 at loc_0082C2EC). */
static int lifted(uint64_t h, uint64_t R_30)
{
    uint64_t R_3 = h, R_4, R_5, R_6; uint32_t R_cr = 0, R_xer = 0;
#define CMP(A, B, F) { int64_t a = (A); int64_t b = (B); uint32_t cr_val = ((a < b) ? 8 : (a > b) ? 4 : 2) | ((R_xer >> 31) & 1u); R_cr = (R_cr & ~(0xFu << F)) | (cr_val << F); }
    CMP((int64_t)R_3, 0, 28);
    if (!((R_cr >> 29) & 1)) goto loc_0082C380;
    R_3 = 1;
    goto loc_0082C2EC;
loc_0082C380:
    R_4 = R_3;
    R_5 = ppc_rldicl(R_4, 51, 63);
    R_3 = ppc_rldicl(R_4, 54, 61);
    CMP((int64_t)R_5, 0, 28);
    R_4 = ppc_rlw64(R_4, 0, 22, 31, 0, 0);
    if (!((R_cr >> 29) & 1)) goto loc_0082C3F8;
    R_3 = ppc_rlw64(R_3, 7, 0, 24, 0, 0);
    R_6 = 0xA300; R_5 = 0xA080;
    R_3 = R_30 + R_3;
    R_6 = R_3 + R_6;
    R_3 = R_3 + R_5;
    R_5 = ppc_rldicl(R_6, 0, 32);
    R_3 = ppc_rldicl(R_3, 0, 32);
    R_5 = rd32((uint32_t)R_5);
    R_3 = rd32((uint32_t)R_3);
    CMP((int64_t)R_3, (int64_t)R_5, 24);
    CMP((int64_t)R_5, (int64_t)R_4, 28);
    if (!((R_cr >> 31) & 1)) goto loc_0082C3D8;
    R_4 = R_4 + (uint64_t)(int64_t)(-128);
loc_0082C3D8:
    if (!((R_cr >> 26) & 1)) goto loc_0082C3E0;
    R_3 = R_3 + (uint64_t)(int64_t)(-128);
loc_0082C3E0:
    R_5 = R_3; R_3 = 0;
    CMP((int64_t)R_5, (int64_t)R_4, 28);
    if (!((R_cr >> 30) & 1)) goto loc_0082C2EC;
    R_3 = 1;
    goto loc_0082C2EC;
loc_0082C3F8:
    R_3 = ppc_rlw64(R_3, 9, 0, 22, 0, 0);
    R_4 = ppc_rlw64(R_4, 6, 0, 25, 0, 0);
    R_3 = R_30 + R_3;
    R_5 = 0;
    R_3 = R_3 + R_4;
    R_4 = R_5 | 0xA5D0;
    R_3 = R_3 + R_4;
    R_3 = ppc_rldicl(R_3, 0, 32);
    R_3 = rd16((uint32_t)R_3 + 0xE);
    R_3 = ppc_rlw64(R_3, 0, 30, 30, 0, 0);
    R_3 = (uint32_t)R_3 ? __builtin_clz((uint32_t)R_3) : 32;
    R_3 = ppc_rlw64(R_3, 27, 31, 31, 0, 0);
    R_3 = R_3 ^ 0x1;
loc_0082C2EC:
    CMP((int64_t)(int32_t)R_3, 0, 28);
    return !((R_cr >> 29) & 1);
#undef CMP
}

int main()
{
    const uint32_t base = 0x01338780u;
    std::mt19937_64 rng(12345);
    int fails = 0, waits = 0, exits = 0, n = 0;
    for (int it = 0; it < 200000; it++) {
        uint64_t h = (it % 97 == 0) ? 0 : (rng() & 0x3FFFull) | ((rng() & 1) ? (rng() << 14) : 0);
        g_mem.clear();
        const uint32_t q = (uint32_t)(h >> 10) & 7u, k = (uint32_t)(h & 0x3FF);
        const uint32_t b0 = base + q * 128u;
        g_mem[b0 + 0xA080u] = (uint32_t)(rng() % 260);
        g_mem[b0 + 0xA300u] = (uint32_t)(rng() % 260);
        if (rng() & 1) g_mem[b0 + 0xA080u] = k + (uint32_t)(rng() % 5) - 2;   /* near the boundary */
        g_mem[base + q * 512u + k * 64u + 0xA5D0u + 0xEu] = (uint32_t)rng();
        g_nrd = 0;
        const int ref = lifted(h, base);
        const int nrd = g_nrd; uint32_t rds[4] = { g_rd[0], g_rd[1], g_rd[2], g_rd[3] };
        uint32_t lo = 0, hi = 0;
        g_nrd = 0;
        const int got = r3_jobwait_pred(h, base, rd16, rd32, &lo, &hi);
        n++;
        bool ok = (got == 2) == (ref == 1) && (got == 1 || got == 2);
        if (ok && got == 1)
            for (int i = 0; i < nrd; i++) ok = ok && rds[i] >= lo && rds[i] < hi;
        if (got == 1) waits++; else exits++;
        if (!ok && fails++ < 10)
            printf("FAIL h=%016llX ref_exit=%d pred=%d lo=%08X hi=%08X\n", (unsigned long long)h, ref, got, lo, hi);
    }
    printf("%d cases (%d wait, %d exit): %s\n", n, waits, exits, fails ? "FAILED" : "all match the lifted loop");
    return fails ? 1 : 0;
}
