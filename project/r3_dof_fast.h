/* Resistance 3: "PC fast" aim-down-sights blur (R3_DOF_GPU_APPROX=1).
 * Host approximation of SPU job 419's output, see r3_dof_fast.c. Pure
 * functions only (no guest / SPU state), shared by the runtime glue in
 * r3_native_dof.c and the offline test (scratch\dof_approx\test_fast.c). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define R3DOF_FAST_W 480          /* largest image (pixels); the frame's size is learned (r3dof_fast_geom) */
#define R3DOF_FAST_H 352
/* colour image (and the job's output), tiled, pitch 8192: IO offset 0x30000 of the game's GCM IO
 * region, so 0x30030000 with the usual mapper base and 0x40030000 when the IO region sits at 0x40000000 */
#define R3DOF_FAST_IO_OFF 0x30000u
extern uint32_t g_gcm_io_base; /* cellGcmSys.c: the ioAddress cellGcmInit was given */
#define R3DOF_FAST_EA (g_gcm_io_base + R3DOF_FAST_IO_OFF)
#define R3DOF_FAST_PITCH 8192u
#define R3DOF_FAST_Z_XBYTE 2048u  /* usual depth image column: same rows, byte column 2048 */
#define R3DOF_FAST_REGION 0x2C0000u /* bytes of the tiled region the images may touch */

/* the frame's image: w x h pixels (job 419 kernel 618's per-frame block: 480 x 320
 * at 960x640), depth at byte column zx of the same rows, RSX tile bank */
typedef struct { uint32_t w, h, zx, bank; } r3dof_fast_geom;

/* 1 = usable: w multiple of 8 and h even within the maxima, zx 32-aligned past
 * the colour and inside the pitch, bank < 4, every byte inside the region */
int r3dof_fast_geom_ok(const r3dof_fast_geom* g);

/* lowest region offset of the image starting at byte column xbyte0: where
 * kernel 618's detile op reads it (0x20000 for the depth at column 2048) */
uint32_t r3dof_fast_min_off(const r3dof_fast_geom* g, uint32_t xbyte0);

/* job 419's per-frame parameter block (0xC0 bytes, big-endian floats) */
typedef struct {
    float lin_a, lin_b, lin_c, lin_d, lin_split; /* depth: s = d > split ? d*a + b : d*c + d', x = 1/s (mode 4) */
    float near_a, near_b, near_max, split;       /* CoC near side: x <= split (mode 8) */
    float far_a, far_b, far_max;                 /* CoC far side */
} r3dof_fast_params;

/* 1 = block looks like job 419's parameters (near/far planes sane, depth
 * terms consistent with them); 0 = do not use. */
int r3dof_fast_params_from_block(const uint8_t* blk, r3dof_fast_params* p);

/* byte offset from R3DOF_FAST_EA of image row `row`, byte column `xbyte`
 * (RSX tile region at local 0x30000, pitch 8192, comp factor 32, bank from
 * the tile setup (cellGcmSetTile; R3 uses 2); same formula as rpcs3
 * Common/tiled_dma_copy.hpp). Runs of 32 bytes (xbyte & ~31 .. +31) are
 * contiguous. */
uint32_t r3dof_tile_off(uint32_t bank, uint32_t row, uint32_t xbyte);

/* region = host pointer of R3DOF_FAST_EA. lin = w x h x 4 bytes, pitch w * 4. */
void r3dof_fast_detile(const r3dof_fast_geom* g, const uint8_t* region, uint32_t xbyte0, uint8_t* lin);
void r3dof_fast_encode(const r3dof_fast_geom* g, uint8_t* region, const uint8_t* lin);

/* scratch for r3dof_fast_run (floats) */
#define R3DOF_FAST_SCRATCH ((R3DOF_FAST_W / 2) * (R3DOF_FAST_H / 2) * 3 * 2)

/* C, Z: linear w x h words as the RSX stores them (C bytes A R G B; Z
 * big-endian D24S8). out: bytes A R G B; A = CoC (job 419 mode 9 layout),
 * RGB = colour blended towards a 2x2-downsampled, separable
 * (1 6 15 20 15 6 1)/64 blurred copy by the CoC. */
void r3dof_fast_run(const r3dof_fast_geom* g, const uint8_t* C, const uint8_t* Z, uint8_t* out, const r3dof_fast_params* p, float* scratch);

/* per-pixel pieces (exposed for the tests) */
float r3dof_fast_lin(const r3dof_fast_params* p, uint32_t zword);
float r3dof_fast_coc(const r3dof_fast_params* p, float x);

/* Probe verdict (PC fast vs the PS3 job on one frame). amatch: pixels whose
 * CoC byte is within 2 of the job's, blur: job pixels with CoC byte > 25,
 * psnr: luma PSNR of the composite (colour blended to the output by its CoC)
 * PC fast vs job. The CoC byte check proves the output layout (A = CoC; a
 * wrong layout matches a few %): the job's mode-8 near-field mask term (not
 * reproduced) moves the CoC behind near objects (82.7% within 2 at 69 dB in
 * the 2026-11-14 15:21 test, 57.5% at 59 dB in the 20:5x test).
 *   < 25%: layout wrong (0).
 *   25-60%: only a blurring frame (blur >= 5%) whose composite is identical
 *     to the eye (PSNR >= R3DOF_FAST_PSNR_STRICT and >= minpsnr) proves it;
 *     otherwise 0.
 *   60-90%: the composite must be visually equal (PSNR >= R3DOF_FAST_PSNR_LOOSE
 *     and >= minpsnr) on every frame, else -1.
 *   >= 90%: the min-PSNR check applies to blurring frames only, else -1.
 * Returns 1 good, 0 CoC/output layout, -1 PSNR. */
#define R3DOF_FAST_PSNR_LOOSE 35.0
#define R3DOF_FAST_PSNR_STRICT 45.0
int r3dof_fast_probe_verdict(long amatch, long blur, long npx, double psnr, double minpsnr);

/* R3_DOF_FAST_BLACK_HOLD: permille of every step-th pixel of C (bytes A R G B)
 * with R, G and B all <= R3DOF_FAST_DARK_MAX. */
#define R3DOF_FAST_DARK_MAX 2
#define R3DOF_FAST_BLACK_PM 990   /* input this dark = light buffer read before the blits */
#define R3DOF_FAST_GOOD_PM 900    /* last good frame must have been lit */
int r3dof_fast_dark_permille(const uint8_t* C, uint32_t npx, uint32_t step);

/* 1 = show the last good output instead of this frame's (black input after a
 * lit frame, at most max_run frames in a row; a real fade passes through).
 * gap = frames since the previous CPU fast-path frame: only 0-1 (an older
 * output may be from another scene) */
int r3dof_fast_black_hold(int dark_pm, int good_pm, int have_good, long gap, int run, int max_run);

/* 1 = C (w x h, bytes A R G B) looks like FP16 lighting texels read as ARGB8
 * (light buffer read before the post colour blit: purple + vertical comb).
 * The post colour's A byte is 0; FP16 texels give a non-zero A (a half's high
 * byte) and R/B (low bytes) much rougher than G (high byte), or the reverse. */
int r3dof_fast_fp16_garbled(const uint8_t* C, uint32_t w, uint32_t h);

/* R3_DOF_FIRST_USE_GUARD: permille of every step-th output pixel (bytes A R G B)
 * whose CoC byte is >= R3DOF_FAST_SAT_A. A real aim frame keeps the aim point in
 * focus (78-95% blurred, few at the maximum); garbage depth (light buffer read
 * before the blits) saturates the CoC everywhere: the output replaces the whole
 * scene (first-ADS purple, 2026-11-18 13:40:35 frames 8947-8948). */
#define R3DOF_FAST_SAT_A 240
#define R3DOF_FAST_SAT_PM 995
int r3dof_fast_coc_sat_permille(const uint8_t* out, uint32_t npx, uint32_t step);

/* 1 = FP16-garbled input, 2 = black input (both only when in_known: the input
 * image was read), 3 = output CoC saturated everywhere, 0 = frame looks sane.
 * A bad frame's output is replaced by a neutral one (A = 0: the composite shows
 * the sharp scene) or the last good output. */
int r3dof_fast_bad_frame(int in_known, int in_dark_pm, int in_fp16, int out_sat_pm);

#ifdef __cplusplus
}
#endif
