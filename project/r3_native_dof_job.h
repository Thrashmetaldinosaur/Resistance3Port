/* Resistance 3 native DOF job (igjob 419) -- hand-vectorised kernel loops.
 *
 * r3dof_gen_job_623() is a drop-in for r3dof_gen_623() (same signature, same
 * result bits, same final registers and LS). It is the transpiled kernel
 * (r3_native_dof_job_gen_623.c, made by scratch\native_job419\gen_job623.py
 * from r3_native_dof_gen_623.c) with the hot per-pixel loops handed to AVX2
 * bulk routines (r3_native_dof_job.c). The bulk routines do every iteration but
 * the last; the transpiled code runs the last one, so every scratch register the
 * loop leaves behind is the lifted code's. Each bulk routine checks its
 * preconditions (aligned, in-range, non-overlapping buffers, loop counts that
 * terminate) and returns 0 to leave the whole loop to the transpiled code.
 *
 * Must be built with the lifted SPU flags (AVX2/FMA, SPU_XF_NO_EXACT) and run
 * under the SPU threads' MXCSR (FTZ|DAZ|round toward zero). */
#ifndef R3_NATIVE_DOF_JOB_H
#define R3_NATIVE_DOF_JOB_H
#include "r3_native_dof_kern.h"

/* The bulk loops reproduce the spu_* helpers as compiled with AVX2 + FMA
 * (fused fma, SPU_X86V paths) and SPU_XF_NO_EXACT -- the same tests
 * spu_helpers.h makes (clang-cl /arch:AVX2 defines __FMA__; -mavx2 alone or
 * MSVC cl does not, and the helpers then use unfused mul+add). Anything else
 * builds stubs that return 0, so r3dof_gen_job_623 equals r3dof_gen_623. */
#if defined(__AVX2__) && defined(__FMA__) && defined(SPU_XF_NO_EXACT) && \
    !defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
#define R3DOFJ_NATIVE 1
#else
#define R3DOFJ_NATIVE 0
#endif

R3DOF_GEN_DECL(job_623);

/* mode 4 (depth -> 1/CoC, loop 0x628/0x6B0): groups top..64 of src -> dst. */
int r3dofj_m4_bulk(uint8_t* ls, uint32_t src, uint32_t dst, uint32_t top,
                   u128 p6, u128 p7, u128 p8, u128 p9, u128 p10);
/* mode 6 (loop 0xC10/0xCA0): groups top..128 of src -> dst. */
int r3dofj_m6_bulk(uint8_t* ls, uint32_t src, uint32_t dst, uint32_t top,
                   u128 p6, u128 p7, u128 p8);

/* mode 8 (loop 0x1150/0x11E8): groups top..64 of src / msk -> dst; p = r7..r17,
 * s22 / s23 the loop's shuffle constants. */
int r3dofj_m8_bulk(uint8_t* ls, uint32_t src, uint32_t msk, uint32_t dst, uint32_t top,
                   const u128* p, u128 s22, u128 s23);
/* mode 9 gather blur (F_017C0, outer loop 0x1810): the 4-pixel groups before the
 * last one not skipped; returns how many (0: none). m = S_36 S_34 S_33 S_31 S_29
 * S_27 S_25 S_37. */
uint32_t r3dofj_m9g_bulk(uint8_t* ls, uint32_t in, uint32_t out, uint32_t cnt, uint32_t tab,
                         u128 stride, u128 lane16, const u128* m);

/* counters: [0] m4 bulk runs, [1] m4 fallbacks, [2] m4 groups, [3] m6 runs,
 * [4] m6 fallbacks, [5] m6 groups, [6] m9 gather runs, [7] m9 gather fallbacks,
 * [8] m9 gather groups, [9] m8 runs, [10] m8 fallbacks, [11] m8 groups */
extern volatile long long g_r3dofj_stat[16];
/* R3_NATIVE_DOF_JOB level: 1 = modes 4 / 6 only (default), 2 = also modes 8 / 9 */
extern volatile int g_r3dofj_level;
/* 1 when the AVX2 bulk loops are compiled in (R3DOFJ_NATIVE), else 0 */
extern const int g_r3dofj_native;
#endif
