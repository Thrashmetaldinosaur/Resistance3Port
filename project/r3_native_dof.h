/* Resistance 3: native replacement of the ADS depth-of-field job kernels
 * (igjob 419). See r3_native_dof.c. */
#pragma once
#include <stdint.h>

#ifndef R3_NATIVE_DOF_DEFAULT
#define R3_NATIVE_DOF_DEFAULT 1
#endif
/* R3_NATIVE_DOF_JOB unset: 4 = native kernel 623 bulk loops + lean / bulk job-419 DMA (AVX2 builds only) */
#ifndef R3_NATIVE_DOF_JOB_DEFAULT
#define R3_NATIVE_DOF_JOB_DEFAULT 4
#endif
/* R3_DOF_GPU_APPROX unset: 1 = "Aim blur: PC fast" */
#ifndef R3_DOF_GPU_APPROX_DEFAULT
#define R3_DOF_GPU_APPROX_DEFAULT 1
#endif

#ifdef __cplusplus
extern "C" {
#endif
struct spu_context;
/* Runs the kernel at (owner image, offset) natively on ctx's LS/registers.
 * Returns 1 if handled (state equals what the lifted kernel leaves at its
 * return), 0 to fall back to the lifted kernel. */
int r3_native_dof_kernel(struct spu_context* ctx, uint32_t owner_img, uint32_t off);
/* Effective R3_NATIVE_DOF_JOB level after start-up (0 when the build has no AVX2 job loops). */
int r3_native_dof_job_level(void);
#ifdef __cplusplus
}
#endif
