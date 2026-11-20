/* Native host replacement for the Edge zlib inflate_fast() kernel of the
 * Resistance 2 / Resistance 3 "edgezlib_inflate_task" SPU task (lifted image
 * spuelf0 in both ports: zlib "1.2.3.0-PS3-SPU-EDGE").
 *
 * Only the hot inner decoder (zlib's inflate_fast, R3 LS 0x8100..0x8D40,
 * R2 0x8240..0x8E80) is replaced. The task framework, inflate(), DMA, the
 * queue and completion signalling stay lifted. The native code emulates the
 * SPU routine's exact externally visible behaviour against local store:
 * every LS store it makes (output bytes, the deferred-literal quadword
 * stores incl. the scratch "dummy" quadword, the z_stream / inflate_state
 * write-back, error msg/mode), in the same order, with the same values,
 * including corrupt/hostile streams and states. Registers r0/r1/r80..r127
 * are untouched; r2..r79 are volatile in the SPU ABI and dead at the call's
 * return (the caller re-derives everything it uses).
 *
 * Hook: the first statement of the lifted thunk (R3 spuelf0_spu_func_00008100,
 * R2 spuelf0_spu_func_00008240) calls the native-jobs gate (R3 project/
 * r3_native_jobs.c r3nj_gate, R2 r2_native_jobs.c r2nj_gate). The gate runs this
 * kernel with a sampled self-check against the lifted code and turns it off for
 * good on a repeatable mismatch. Switches: R3_NATIVE_JOBS / R2_NATIVE_JOBS;
 * PORT_NATIVE_EDGE_ZLIB=0 forces the kernel off. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct native_edge_zlib_desc {
    const char* name;
    uint32_t thunk_pc;      /* lifted function the gate sits in            */
    uint32_t dummy_ls;      /* `ila $r21, X`: scratch quad for dead stores */
    uint32_t shuf_ls;       /* `lqr $r7, X`: byte-reverse shufb constant   */
    uint32_t msg_dist;      /* "invalid distance code"                      */
    uint32_t msg_lit;       /* "invalid literal/length code"                */
    uint32_t msg_far;       /* "invalid distance too far back"              */
} native_edge_zlib_desc;

extern const native_edge_zlib_desc native_edge_zlib_r3;
extern const native_edge_zlib_desc native_edge_zlib_r2;

/* Core: run inflate_fast(strm, start) against a 256 KB local store image.
 * strm/start are the raw SPU argument values ($r3/$r4 at the thunk). */
void native_edge_zlib_inflate_fast_ls(uint8_t* ls, uint32_t strm, uint32_t start,
                                      const native_edge_zlib_desc* d);

/* 1 when the LS image holds the expected constants for `d` (the data the
 * SPU routine reads besides its arguments). */
int native_edge_zlib_ls_matches(const uint8_t* ls, const native_edge_zlib_desc* d);

#ifdef __cplusplus
}
#endif
