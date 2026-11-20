/* R3 native audio-synth kernels, ported from R2's r2_native_audio
 * (default on: R3_NATIVE_AUDIO_JOB=0 = lifted only).
 *
 * Native replacements for the leaf routines of the "nextsynth" WWS job
 * modules that run all game audio on one SPURS lane:
 *   jobmod_08b54bef234a_e80 (synth A, = R2 a8d79dd61f50) and
 *   jobmod_e2afa2e7478f_e80 (synth B, = R2 5e0b41a9ea8c).
 * Each kernel works on a raw LS image (256 KB, big-endian words) plus the
 * 128-entry SPU register file, exactly like the lifted function would, and is
 * bit-exact with it (float ops go through the same spu_helpers.h helpers,
 * compiled with the same flags). A kernel either completes (returns 1, LS and
 * its output registers updated) or declines (returns 0, NOTHING written) --
 * then the lifted code runs. See scratch/cpu_audio_native.md. */
#ifndef R3_NATIVE_AUDIO_H
#define R3_NATIVE_AUDIO_H

#include "spu_context.h"   /* u128 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    R3NA_K_08_RESAMPLE4 = 0,   /* 08b5 0x4800: 4-tap table-interpolated resampler     */
    R3NA_K_08_DEINTERLEAVE4,   /* 08b5 0x4B80: 4-channel deinterleave (word transpose)*/
    R3NA_K_08_INTERLEAVE4,     /* 08b5 0x4C80: 4-channel interleave                   */
    R3NA_K_08_ALLOC_UP,        /* 08b5 0x66D0: arena alloc (grow up) + zero fill      */
    R3NA_K_08_ALLOC_DOWN,      /* 08b5 0x6828: arena alloc (grow down) + zero fill    */
    R3NA_K_E2_ALLOC_UP,        /* e2af 0xD9F8: same code as 08b5 0x66D0              */
    R3NA_K_E2_ALLOC_DOWN,      /* e2af 0xDB50: same code as 08b5 0x6828              */
    R3NA_K_COUNT
};

#define R3NA_LS_SIZE   0x40000u
#define R3NA_QMASK     0x3FFF0u

/* 1 = done (LS + output registers updated), 0 = declined (nothing written). */
int         r3na_kernel_run(int k, uint8_t* ls, u128* gpr);
const char* r3na_kernel_name(int k);
uint32_t    r3na_kernel_entry(int k);
/* Registers (bit n = $rn, n < 64) the kernel defines on return and that a
 * caller may read. All other volatile registers ($r2..$r79) are scratch the
 * lifted code leaves behind (no caller reads them: checked at every brsl site
 * in scratch/cpu_audio_native.md); $r0/$r1/$r80..$r127 are never written. */
uint64_t    r3na_kernel_outregs(int k);

/* Gate called from the hook line at the top of each lifted target function
 * (project/spu/jobmod_{08b54bef234a,e2afa2e7478f}_e80/spu_recomp.c, put there
 * by tools/r3_regen.py from tools/hooks/spu/<jobmod>.json). CMake passes the
 * hooks it finds as R3NA_HOOKS (bit k = kernel k); kernels without one are
 * off. 1 = handled (the hook does SPU_RET). */
int r3na_gate(spu_context* ctx, int k, void (*lifted)(spu_context*));

/* Kernels whose gate call this build has (R3NA_HOOKS), for the [native] line. */
uint32_t r3na_hooks_present(void);
uint32_t r3na_effective_mask(void);   /* kernels that really run native now (switches, mask, hooks, self-check) */

#ifdef __cplusplus
}
#endif
#endif
