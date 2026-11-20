/* R3_SEMFIX_HEAL (runtime/spu/semfix_heal.h): the 2026-11-17 5 fps pattern.
 * A label-77 store was dropped as an already consumed cycle's, then (FIFO
 * desync) every frame's acquire waited the full 150 ms cap for a store that
 * never came. After 3 caps in a row with no store the label must get the short
 * cap; a store resets the run, a satisfied acquire restores the long cap. */
#include "spu/semfix_heal.h"
#include <stdio.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

int main(void)
{
    const uint64_t cap = 150000u, ok_max = 6000u;   /* us; satisfied waits up to 6 ms */
    const long min_ms = 4;
    semfix_heal_t h = {0, 0};

    /* the lock: 20 frames, each acquire capped, no store */
    uint64_t total = 0;
    int healed_at = -1;
    for (int f = 0; f < 20; f++) {
        const uint64_t c = semfix_heal_pick_cap(cap, h.healed, ok_max, min_ms);
        total += c;
        if (semfix_heal_cap(&h, 3)) healed_at = f;
    }
    CHECK(healed_at == 2);
    CHECK(semfix_heal_pick_cap(cap, h.healed, ok_max, min_ms) == ok_max + ok_max / 4 + 1000u);
    CHECK(total == 3 * cap + 17 * (ok_max + ok_max / 4 + 1000u));   /* 3 s -> ~0.6 s */

    /* healed until an acquire is satisfied */
    semfix_heal_satisfied(&h);
    CHECK(!h.healed && semfix_heal_pick_cap(cap, h.healed, ok_max, min_ms) == cap);

    /* stores in between: a slow but signalled label is never healed */
    for (int f = 0; f < 10; f++) {
        CHECK(!semfix_heal_cap(&h, 3));
        if (f % 2) semfix_heal_store(&h);
    }
    CHECK(!h.healed);

    /* circular-wait latch (spu_channels.c semfix_cap_taken / semfix_circ_note_spu):
     * a store right after a heal short cap must not latch circ; after a long cap it must */
    {
        semfix_heal_t hc = {0, 0};
        int circ = 0, armed = 0;
        for (int f = 0; f < 3; f++) {
            armed = semfix_heal_arms_circ(&hc);   /* long caps, no store */
            semfix_heal_cap(&hc, 3);
        }
        CHECK(hc.healed);
        for (int f = 0; f < 5; f++) {
            CHECK(semfix_heal_pick_cap(cap, circ || hc.healed, ok_max, min_ms) < cap);
            armed = semfix_heal_arms_circ(&hc);   /* heal short cap */
            semfix_heal_cap(&hc, 3);
            CHECK(!armed);
            if (armed) circ = 1;                  /* slower legitimate store lands 20 ms later */
            semfix_heal_store(&hc);
        }
        CHECK(!circ);
        semfix_heal_satisfied(&hc);
        CHECK(semfix_heal_pick_cap(cap, circ || hc.healed, ok_max, min_ms) == cap);
        armed = semfix_heal_arms_circ(&hc);       /* long cap again: detection armed */
        CHECK(armed);
    }

    /* R3_SEMFIX_HEAL=0 */
    semfix_heal_t off = {0, 0};
    for (int f = 0; f < 10; f++) CHECK(!semfix_heal_cap(&off, 0));
    CHECK(!off.healed);

    /* short cap floor and R3_SEMFIX_MIN_MS=0 (always the long cap) */
    CHECK(semfix_heal_pick_cap(cap, 1, 0, min_ms) == 4000u);
    CHECK(semfix_heal_pick_cap(cap, 1, ok_max, 0) == cap);
    CHECK(semfix_heal_pick_cap(5000u, 1, 200000u, min_ms) == 5000u);

    printf(g_fail ? "test_semfix_heal: FAIL\n" : "test_semfix_heal: PASS\n");
    return g_fail;
}
