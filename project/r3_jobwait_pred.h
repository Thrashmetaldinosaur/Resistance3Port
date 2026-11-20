/* R3_JOBWAIT_WAKE predicate (stubs.cpp r3_jobwait_watch): the exit test of func_0082C190's
 * job-list poll loop (loc_0082C2DC..0082C3F4), read with the same loads. Pure so that
 * project\tests\jobwait_pred_test.cpp can check it against the lifted instructions.
 * h = the 64-bit job-list handle at *r31, base = r30 (0x01338780).
 * Returns 2 = the loop would exit now, 1 = keep waiting with [*lo,*hi) = the words it polls. */
#pragma once
#include <stdint.h>

template <class R16, class R32>
static inline int r3_jobwait_pred(uint64_t h, uint32_t base, R16 rd16, R32 rd32, uint32_t* lo, uint32_t* hi)
{
    if (h == 0) return 2;                                   /* cmpdi h,0; beq -> r3=1 -> exit */
    const uint32_t q = (uint32_t)(h >> 10) & 7u;            /* rldicl h,54,61 */
    const int64_t k0 = (int64_t)(h & 0x3FFu);               /* rlwinm h,0,22,31 */
    if ((h >> 13) & 1) {                                    /* rldicl h,51,63 != 0 -> loc_0082C3F8 */
        const uint32_t ea = base + q * 512u + (uint32_t)k0 * 64u + 0xA5D0u + 0xEu;
        if (rd16(ea) & 2) return 2;                         /* (v & 2) != 0 -> r3=1 -> exit */
        *lo = ea; *hi = ea + 2;
        return 1;
    }
    const uint32_t b0 = base + q * 128u;
    int64_t a = (int64_t)(uint64_t)rd32(b0 + 0xA080u);
    const int64_t b = (int64_t)(uint64_t)rd32(b0 + 0xA300u);
    int64_t k = k0;
    if (b < k) k -= 128;                                    /* cmpd b,k; bge+ */
    if (a > b) a -= 128;                                    /* cmpd cr1 a,b; ble cr1 */
    if (a > k) return 2;                                    /* cmpd a,k; bgt -> r3=1 -> exit */
    *lo = b0 + 0xA080u; *hi = b0 + 0xA304u;
    return 1;
}

/* r7d SPU_QUEUE_PI: igPm job queues are per SPU (func_0082BE20: handle = idx | q << 10). */
#define R3_IGQ_N 5
/* Queue of a job-list handle, -1 = none (func_0082BDF8 accepts q < 5). */
static inline int r3_igq_from_handle(uint64_t h)
{
    const uint32_t q = (uint32_t)(h >> 10) & 7u;
    return h && q < R3_IGQ_N ? (int)q : -1;
}
/* Queue whose completion line (base + 0xA080 + q*128) the ig kernel keeps in r84, -1 = other. */
static inline int r3_igq_from_line(uint32_t ea, uint32_t base)
{
    const uint32_t d = ea - (base + 0xA080u);
    return (d & 127u) == 0 && d < R3_IGQ_N * 128u ? (int)(d >> 7) : -1;
}
