/* r7d SPU_QUEUE_PI: r3_igq_from_handle / r3_igq_from_line (project\r3_jobwait_pred.h) against the
 * igPm ring layout: func_0082BE20 returns idx | q << 10 | gen << 14, and the queue's completion
 * line (the words r3_jobwait_pred polls; the ig kernel keeps its EA in r84) is base + 0xA080 + q*128.
 * Build: clang-cl /nologo /O1 /EHsc /std:c++17 /I ..\..\project igq_test.cpp */
#include "r3_jobwait_pred.h"
#include <stdio.h>
#include <random>

static const uint32_t kBase = 0x01338780u;
static uint32_t rd16(uint32_t) { return 0; }
static uint32_t rd32(uint32_t a) { return (a - kBase - 0xA300u) % 128u == 0 && a >= kBase + 0xA300u ? 5u : 0u; }

int main()
{
    int fail = 0, n = 0;
    std::mt19937 rng(7);
    for (uint32_t q = 0; q < 8; q++)
        for (uint32_t idx = 0; idx < 128; idx++) {
            const uint64_t h = idx | (uint64_t)q << 10 | (uint64_t)(rng() & 0x3FFFF) << 14;
            const int want = q < R3_IGQ_N ? (int)q : -1;
            n++;
            if (r3_igq_from_handle(h) != want) { fail++; printf("handle %llX: q %d want %d\n", (unsigned long long)h, r3_igq_from_handle(h), want); }
            if (q < R3_IGQ_N && idx < 3) {
                /* the line the PPU wait polls for this handle is the one the lane maps back to q */
                uint32_t lo = 0, hi = 0;
                const int r = r3_jobwait_pred(h, kBase, rd16, rd32, &lo, &hi);
                if (r != 1 || r3_igq_from_line(lo, kBase) != (int)q) { fail++; printf("pred line %08X q %d want %u\n", lo, r3_igq_from_line(lo, kBase), q); }
            }
        }
    for (uint32_t q = 0; q < R3_IGQ_N; q++) {
        const uint32_t line = kBase + 0xA080u + q * 128u;
        n += 4;
        if (r3_igq_from_line(line, kBase) != (int)q) { fail++; printf("line %08X\n", line); }
        if (r3_igq_from_line(line + 4, kBase) != -1) { fail++; printf("unaligned %08X\n", line + 4); }
        if (r3_igq_from_line(kBase + 0xA300u + q * 128u, kBase) != -1) { fail++; printf("producer line q%u\n", q); }
        if (r3_igq_from_line(line - 0x1000u, kBase) != -1) { fail++; printf("below %08X\n", line - 0x1000u); }
    }
    n += 3;
    if (r3_igq_from_line(kBase + 0xA080u + R3_IGQ_N * 128u, kBase) != -1) { fail++; printf("q5 line\n"); }
    if (r3_igq_from_line(kBase + 0xA080u - 128u, kBase) != -1) { fail++; printf("line before q0\n"); }
    if (r3_igq_from_handle(0) != -1) { fail++; printf("null handle\n"); }
    printf("igq_test: %d checks, %d failures\n", n, fail);
    return fail != 0;
}
