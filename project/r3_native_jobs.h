/* R3 native SPU job kernels (native_hot round, see files/native_hot_r3.md).
 *
 * Native host replacements for hot leaf routines of lifted SPU code, run
 * through the same gate / self-check scheme as R2 (project/r2_native_jobs.c).
 * A kernel works on a raw LS image (256 KB, big-endian words) plus the
 * 128-entry SPU register file exactly like the lifted function would, and
 * either completes (returns 1) or declines (returns 0: NOTHING written) --
 * then the lifted code runs.
 *
 *   R3NJ_K_EDGE_ZLIB   spuelf0 0x8100: Edge zlib inflate_fast
 *                      (ps3recomp/runtime/spu/native_edge_zlib.c)
 *   R3NJ_K_BINK_IDCT_PUT    igjob_00E0A480 (job 421, Bink video) 0xC288,
 *   R3NJ_K_BINK_IDCT_PUT2X  0xC8C0, R3NJ_K_BINK_IDCT_ADD 0xCF30: 8x8 dequant +
 *                      IDCT + block put / 2x put / add (r3_native_bink.c, AVX2)
 *   R3NJ_K_GEOM_XFORM4 igjob_00E29200 (job 422, Edge-style geometry) 0x120:
 *                      packed-position transform + clip codes (r3_native_geom.c,
 *                      AVX2; port of R2 dd413 0x4030). Fuzz-verified offline
 *                      (scratch\spu_native\test_geom) but NOT yet on captured game
 *                      inputs, so OFF by default: R3_NATIVE_JOBS_GEOM=1 (or mask
 *                      bit 4) turns it on.
 *
 * Switches (read once):
 *   R3_NATIVE_JOBS=1         native kernels on (DEFAULT). 0 = lifted only.
 *   R3_NATIVE_JOBS_MASK=0xF  kernels to replace (bit k = kernel k). Default =
 *                            the kernels proven byte-exact offline.
 *   R3_NATIVE_JOBS_CHECK=N   runtime self-check: the first 8 calls of each
 *                            kernel and then every Nth (default 4096; 0 = off)
 *                            run lifted on the real state and native on a copy;
 *                            LS + r0/r1/r80..r127 are compared. A mismatch is
 *                            re-run (lifted and native again on copies of the
 *                            same input); only a repeatable one disables the
 *                            kernel for the rest of the run (logged once; else
 *                            counted as a race). The lifted result is kept, so
 *                            the game never sees the bad output of a checked call.
 *   R3_NATIVE_JOBS_VERIFY=1  check every call (slow; for test runs).
 *   R3_NATIVE_JOBS_STATS=1   periodic per-kernel counters on stderr.
 *   PORT_NATIVE_EDGE_ZLIB=0  still forces the zlib kernel off (old switch).
 *   R3_NATIVE_JOBS_GEOM=1    adds kernel 4 (job 422 xform) to the default mask.
 *
 * A kernel only runs where its gate call is in the lifted code: the hooks are
 * tools/hooks/spu/<module>.json, applied by tools/r3_regen.py (the player's
 * setup) and checked by tools/test_spu_hooks.py. CMake passes the hooks it
 * finds as R3NJ_HOOKS; a kernel without one logs "hook missing" and the
 * [native] start-up line shows it off. */
#ifndef R3_NATIVE_JOBS_H
#define R3_NATIVE_JOBS_H

#include "spu_context.h"   /* u128 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    R3NJ_K_EDGE_ZLIB = 0,
    R3NJ_K_BINK_IDCT_PUT = 1,
    R3NJ_K_BINK_IDCT_PUT2X = 2,
    R3NJ_K_BINK_IDCT_ADD = 3,
    R3NJ_K_GEOM_XFORM4 = 4,
    R3NJ_K_COUNT
};

#define R3NJ_LS_SIZE 0x40000u

int         r3nj_kernel_run(int k, uint8_t* ls, u128* gpr);
const char* r3nj_kernel_name(int k);
uint32_t    r3nj_kernel_entry(int k);

/* individual kernels (harness entry points) */
int r3nj_edge_zlib(uint8_t* ls, u128* gpr);
int r3nj_bink_idct_put(uint8_t* ls, u128* gpr);
int r3nj_bink_idct_put2x(uint8_t* ls, u128* gpr);
int r3nj_bink_idct_add(uint8_t* ls, u128* gpr);
int r3nj_bink_built(void);   /* 0: built without AVX2, the Bink kernels always decline (kept off) */
int r3nj_geom_xform4(uint8_t* ls, u128* gpr);
int r3nj_geom_built(void);   /* 0: built without AVX2 */

/* Gate called from the hook line at the top of each lifted target function
 * (tools/hooks/spu/<module>.json). 1 = handled (the hook does SPU_RET). */
int r3nj_gate(spu_context* ctx, int k, void (*lifted)(spu_context*));

/* Is kernel k on right now (switch + mask + hook compiled in, not disabled by
 * a self-check)? If not, a short reason ("hook missing", "R3_NATIVE_JOBS=0",
 * "R3_NATIVE_JOBS_MASK", "own switch", "disabled by self-check"); 0 if on.
 * Also counters for the harness: calls, native, checks ok, checks bad, races. */
int         r3nj_kernel_active(int k);
const char* r3nj_kernel_why_off(int k);
uint32_t    r3nj_effective_mask(void);   /* bit k = kernel k really runs native now (the [native] line) */
void        r3nj_kernel_counts(int k, uint32_t out[5]);

#ifdef __cplusplus
}
#endif
#endif
