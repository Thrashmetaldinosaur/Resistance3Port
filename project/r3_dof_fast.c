/* Resistance 3: "PC fast" aim-down-sights blur (R3_DOF_GPU_APPROX=1).
 *
 * SPU job 419 turns the colour + depth images (480x320 at 960x640; size from
 * kernel 618's per-frame block) the RSX leaves in the
 * tiled region at 0x30030000 into the A8R8G8B8 image the composite pass
 * samples (same address): kernel 618 detiles, 623 mode 4 linearises depth,
 * 620/621/622/619 build a downsampled blurred copy, 623 modes 6/8 make the
 * circle of confusion, 623 mode 9 gathers (16 taps scaled by the CoC) and
 * writes A = CoC, RGB = blurred colour; the image is tiled back in place.
 *
 * This file is the fast approximation: depth linearisation and the CoC ramps
 * use the job's own per-frame parameters (same formulas as modes 4 and 8,
 * without mode 8's near-field mask term); the variable-radius gather is
 * replaced by a blend between the colour and one 2x2-downsampled, separable
 * 7-tap blurred copy. Not pixel-identical to the PS3 (see r3_ads_round.md,
 * round 6, for the offline PSNR/SSIM numbers). */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "r3_dof_fast.h"

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static float bef(const uint8_t* p) { uint32_t u = be32(p); float f; memcpy(&f, &u, 4); return f; }
static int near_rel(float a, float b, float tol) { return fabsf(a - b) <= tol * fabsf(b); }

int r3dof_fast_params_from_block(const uint8_t* blk, r3dof_fast_params* p)
{
    const float zn = bef(blk + 0x10), zf = bef(blk + 0x14);
    p->lin_split = bef(blk + 0x1C);
    p->lin_a = bef(blk + 0x20); p->lin_b = bef(blk + 0x24);
    p->lin_c = bef(blk + 0x28); p->lin_d = bef(blk + 0x2C);
    p->near_a = bef(blk + 0x50); p->near_b = bef(blk + 0x54); p->near_max = bef(blk + 0x58); p->split = bef(blk + 0x5C);
    p->far_a = bef(blk + 0x70); p->far_b = bef(blk + 0x74); p->far_max = bef(blk + 0x78);
    if (!(zn > 1e-4f && zn < 10.f && zf > zn && zf < 1e6f)) return 0;
    /* far-range terms: 1/x = d * (1/zf - 1/zn) / 2^24 + 1/zn */
    if (!near_rel(p->lin_b, 1.f / zn, 0.02f) || !near_rel(p->lin_a, (1.f / zf - 1.f / zn) / 16777216.f, 0.02f)) return 0;
    if (!(p->lin_split > 0.f && p->lin_split <= 16777216.f && p->lin_d > 0.f && p->lin_c < 0.f)) return 0;
    const float* v[] = { &p->near_a, &p->near_b, &p->near_max, &p->split, &p->far_a, &p->far_b, &p->far_max };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) if (!(fabsf(*v[i]) < 1e6f)) return 0;   /* also NaN */
    if (p->near_max < 0.f || p->far_max < 0.f || p->near_max > 64.f || p->far_max > 64.f) return 0;
    return 1;
}

uint32_t r3dof_tile_off(uint32_t tbank, uint32_t row, uint32_t xbyte)
{
    enum { BASE = 0x30000, PITCH = R3DOF_FAST_PITCH, TPR = PITCH / 256 };
    static const uint8_t lut[16] = { 0, 1, 2, 3, 2, 3, 0, 1, 1, 2, 3, 0, 3, 0, 1, 2 };
    const uint32_t ta = row * PITCH + BASE + xbyte;
    const uint32_t texel = (ta - BASE) / 256;
    const uint32_t tx = texel % TPR, ty = texel / TPR / 64;
    const uint32_t tsel = (ty * TPR + tx + (BASE >> 14)) & 0x3ffff;
    const uint32_t rowa = (tsel >> 2) & 0xffff;
    const uint32_t bank = (lut[(tsel & 3) * 4 + (ty & 3)] + tbank) & 3;   /* comp factor >= 4 */
    const uint32_t line = texel / TPR % 64;
    const uint32_t col = ((line >> 3) & 7) << 7 | ((ta >> 5) & 7) << 4 | (line & 3) << 2;
    const uint32_t part = (((line >> 2) & 1) + ((ta >> 6) & 1)) & 1;
    uint32_t t = rowa << 16 | bank << 14 | ((col >> 4) & 0x3f) << 8 | part << 7 | ((col >> 2) & 3) << 5 | (ta & 31);
    t ^= (((t >> 12) ^ ((bank ^ tsel) & 1) ^ (t >> 14)) & 1) << 9;
    t ^= ((t >> 11) & 1) << 10;
    return t - BASE;
}

int r3dof_fast_geom_ok(const r3dof_fast_geom* g)
{
    if (!g || !g->w || !g->h || (g->w & 7u) || (g->h & 1u) || g->w > R3DOF_FAST_W || g->h > R3DOF_FAST_H ||
        (g->zx & 31u) || g->zx < g->w * 4u || g->zx + g->w * 4u > R3DOF_FAST_PITCH || g->bank > 3u)
        return 0;
    for (uint32_t y = 0; y < g->h; y++)
        for (uint32_t x = 0; x < g->w * 4u; x += 32)
            if (r3dof_tile_off(g->bank, y, x) + 32u > R3DOF_FAST_REGION || r3dof_tile_off(g->bank, y, g->zx + x) + 32u > R3DOF_FAST_REGION)
                return 0;
    return 1;
}

uint32_t r3dof_fast_min_off(const r3dof_fast_geom* g, uint32_t xbyte0)
{
    uint32_t m = 0xFFFFFFFFu;
    for (uint32_t y = 0; y < g->h; y++)
        for (uint32_t x = 0; x < g->w * 4u; x += 32) {
            const uint32_t o = r3dof_tile_off(g->bank, y, xbyte0 + x);
            if (o < m) m = o;
        }
    return m;
}

void r3dof_fast_detile(const r3dof_fast_geom* g, const uint8_t* region, uint32_t xbyte0, uint8_t* lin)
{
    const uint32_t lp = g->w * 4u;
    for (uint32_t y = 0; y < g->h; y++)
        for (uint32_t x = 0; x < lp; x += 32)
            memcpy(lin + y * lp + x, region + r3dof_tile_off(g->bank, y, xbyte0 + x), 32);
}

void r3dof_fast_encode(const r3dof_fast_geom* g, uint8_t* region, const uint8_t* lin)
{
    const uint32_t lp = g->w * 4u;
    for (uint32_t y = 0; y < g->h; y++)
        for (uint32_t x = 0; x < lp; x += 32)
            memcpy(region + r3dof_tile_off(g->bank, y, x), lin + y * lp + x, 32);
}

float r3dof_fast_lin(const r3dof_fast_params* p, uint32_t zword)
{
    const float d = (float)(zword >> 8);
    const float s = d > p->lin_split ? d * p->lin_a + p->lin_b : d * p->lin_c + p->lin_d;
    return 1.f / s;
}

static float sat(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
float r3dof_fast_coc(const r3dof_fast_params* p, float x)
{
    return x > p->split ? sat(x * p->far_a + p->far_b) * p->far_max : sat(x * p->near_a + p->near_b) * p->near_max;
}

int r3dof_fast_probe_verdict(long amatch, long blur, long npx, double psnr, double minpsnr)
{
    if (npx <= 0 || amatch * 100 < npx * 25L) return 0;
    if (amatch * 10 < npx * 6L)
        return blur * 20 >= npx && psnr >= R3DOF_FAST_PSNR_STRICT && psnr >= minpsnr ? 1 : 0;
    if (amatch * 10 < npx * 9L) return psnr >= R3DOF_FAST_PSNR_LOOSE && psnr >= minpsnr ? 1 : -1;
    return blur * 20 < npx || psnr >= minpsnr ? 1 : -1;
}

int r3dof_fast_dark_permille(const uint8_t* C, uint32_t npx, uint32_t step)
{
    if (!npx) return 0;
    if (!step) step = 1;
    uint32_t n = 0, dark = 0;
    for (uint32_t i = 0; i < npx; i += step, n++) {
        const uint8_t* p = C + (size_t)i * 4u;
        dark += p[1] <= R3DOF_FAST_DARK_MAX && p[2] <= R3DOF_FAST_DARK_MAX && p[3] <= R3DOF_FAST_DARK_MAX;
    }
    return (int)((uint64_t)dark * 1000u / n);
}

int r3dof_fast_black_hold(int dark_pm, int good_pm, int have_good, long gap, int run, int max_run)
{
    return have_good && gap >= 0 && gap <= 1 && dark_pm >= R3DOF_FAST_BLACK_PM && good_pm >= 0 && good_pm < R3DOF_FAST_GOOD_PM && run < max_run;
}

int r3dof_fast_coc_sat_permille(const uint8_t* out, uint32_t npx, uint32_t step)
{
    if (!npx) return 0;
    if (!step) step = 1;
    uint32_t n = 0, sat = 0;
    for (uint32_t i = 0; i < npx; i += step, n++) sat += out[(size_t)i * 4u] >= R3DOF_FAST_SAT_A;
    return (int)((uint64_t)sat * 1000u / n);
}

int r3dof_fast_bad_frame(int in_known, int in_dark_pm, int in_fp16, int out_sat_pm)
{
    if (in_known && in_fp16) return 1;
    if (in_known && in_dark_pm >= R3DOF_FAST_BLACK_PM) return 2;
    if (out_sat_pm >= R3DOF_FAST_SAT_PM) return 3;
    return 0;
}

int r3dof_fast_fp16_garbled(const uint8_t* C, uint32_t w, uint32_t h)
{
    uint64_t n = 0, anz = 0, rrb = 0, rg = 0;
    for (uint32_t y = 0; y < h; y += 4) {
        const uint8_t* row = C + (size_t)y * w * 4u;
        for (uint32_t x = 0; x + 2 < w; x += 6) {
            const uint8_t* p = row + x * 4u; const uint8_t* q = p + 8;
            anz += p[0] != 0;
            rrb += (uint32_t)abs(p[1] - q[1]) + (uint32_t)abs(p[3] - q[3]);
            rg += 2u * (uint32_t)abs(p[2] - q[2]);
            n++;
        }
    }
    if (!n || anz * 2 < n) return 0;
    /* 1 + mean roughness per channel, x100 */
    const uint64_t a = 100 * n + rrb * 50, b = 100 * n + rg * 50;
    return a * 2 >= b * 3 || b * 2 >= a * 3;
}

void r3dof_fast_run(const r3dof_fast_geom* g, const uint8_t* C, const uint8_t* Z, uint8_t* out, const r3dof_fast_params* p, float* scratch)
{
    const int W = (int)g->w, H = (int)g->h, HW = W / 2, HH = H / 2;
    float* a = scratch;              /* HW x HH x 3 */
    float* b = scratch + HW * HH * 3;
    for (int y = 0; y < HH; y++)
        for (int x = 0; x < HW; x++) {
            const uint8_t* s0 = C + ((2 * y) * W + 2 * x) * 4;
            const uint8_t* s1 = s0 + W * 4;
            for (int c = 0; c < 3; c++)
                a[(y * HW + x) * 3 + c] = (s0[1 + c] + s0[5 + c] + s1[1 + c] + s1[5 + c]) * 0.25f;
        }
    static const float w[4] = { 20.f / 64, 15.f / 64, 6.f / 64, 1.f / 64 };
    for (int y = 0; y < HH; y++)
        for (int x = 0; x < HW; x++)
            for (int c = 0; c < 3; c++) {
                float acc = a[(y * HW + x) * 3 + c] * w[0];
                for (int k = 1; k < 4; k++) {
                    const int xl = x - k < 0 ? 0 : x - k, xr = x + k >= HW ? HW - 1 : x + k;
                    acc += (a[(y * HW + xl) * 3 + c] + a[(y * HW + xr) * 3 + c]) * w[k];
                }
                b[(y * HW + x) * 3 + c] = acc;
            }
    for (int y = 0; y < HH; y++)
        for (int x = 0; x < HW; x++)
            for (int c = 0; c < 3; c++) {
                float acc = b[(y * HW + x) * 3 + c] * w[0];
                for (int k = 1; k < 4; k++) {
                    const int yu = y - k < 0 ? 0 : y - k, yd = y + k >= HH ? HH - 1 : y + k;
                    acc += (b[(yu * HW + x) * 3 + c] + b[(yd * HW + x) * 3 + c]) * w[k];
                }
                a[(y * HW + x) * 3 + c] = acc;
            }
    for (int y = 0; y < H; y++) {
        float fy = (y + 0.5f) * 0.5f - 0.5f; if (fy < 0.f) fy = 0.f;
        int y0 = (int)fy; const int y1 = y0 + 1 < HH ? y0 + 1 : HH - 1; const float ty = fy - (float)y0;
        for (int x = 0; x < W; x++) {
            const uint8_t* c = C + (y * W + x) * 4;
            uint8_t* o = out + (y * W + x) * 4;
            const float coc = r3dof_fast_coc(p, r3dof_fast_lin(p, be32(Z + (y * W + x) * 4)));
            if (!(coc > 0.f)) { o[0] = 0; o[1] = c[1]; o[2] = c[2]; o[3] = c[3]; continue; }
            float fx = (x + 0.5f) * 0.5f - 0.5f; if (fx < 0.f) fx = 0.f;
            const int x0 = (int)fx, x1 = x0 + 1 < HW ? x0 + 1 : HW - 1; const float tx = fx - (float)x0;
            const float k = coc < 1.f ? coc : 1.f;
            for (int ch = 0; ch < 3; ch++) {
                const float t0 = a[(y0 * HW + x0) * 3 + ch] + (a[(y0 * HW + x1) * 3 + ch] - a[(y0 * HW + x0) * 3 + ch]) * tx;
                const float t1 = a[(y1 * HW + x0) * 3 + ch] + (a[(y1 * HW + x1) * 3 + ch] - a[(y1 * HW + x0) * 3 + ch]) * tx;
                const float v = c[1 + ch] + ((t0 + (t1 - t0) * ty) - c[1 + ch]) * k + 0.5f;
                o[1 + ch] = (uint8_t)(v >= 255.f ? 255 : (int)v);
            }
            const float av = coc * 256.f;   /* mode 9: byte(coc) = floor(coc * 256), saturated */
            o[0] = (uint8_t)(av >= 255.f ? 255 : (int)av);
        }
    }
}
