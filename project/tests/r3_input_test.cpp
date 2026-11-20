/* R3 dropped-movement fixes: movement-key latch + key -> stick (pad_logic.h)
 * and the game-clock floor (r3_dtfloor.h).
 *
 *   clang-cl /nologo /W4 /I project /I ps3recomp\libs\input project\tests\r3_input_test.cpp
 */
#include <stdio.h>
#include <math.h>
#include "r3_dtfloor.h"
#include "pad_logic.h"

static int s_fail = 0, s_n = 0;
#define CHECK(c) do { s_n++; if (!(c)) { s_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static int near_(double a, double b) { return fabs(a - b) < 1e-9; }

static void test_latch(void)
{
    PadKeyLatch l = { 0 };
    /* held key: reported while held, gone after release + delivery */
    CHECK(padk_latch_sample(&l, PADK_UP, 0) == PADK_UP);
    padk_latch_delivered(&l);
    CHECK(padk_latch_sample(&l, 0, 0) == 0);

    /* down at one sample, up at the next, before the game read either: still reported once */
    padk_latch_delivered(&l);
    CHECK(padk_latch_sample(&l, PADK_LEFT, 0) == PADK_LEFT);
    CHECK(padk_latch_sample(&l, 0, 0) == PADK_LEFT);
    padk_latch_delivered(&l);
    CHECK(padk_latch_sample(&l, 0, 0) == 0);

    /* a tap between two samples (never seen held) */
    CHECK(padk_latch_sample(&l, 0, PADK_RIGHT) == PADK_RIGHT);
    padk_latch_delivered(&l);
    CHECK(padk_latch_sample(&l, 0, 0) == 0);

    /* a latched tap does not hide a key pressed after it */
    CHECK(padk_latch_sample(&l, 0, PADK_DOWN) == PADK_DOWN);
    CHECK(padk_latch_sample(&l, PADK_UP, 0) == (PADK_DOWN | PADK_UP));
    padk_latch_delivered(&l);
    CHECK(padk_latch_sample(&l, PADK_UP, 0) == PADK_UP);
}

static void test_stick(void)
{
    CHECK(padk_axis(0, 0) == 128);
    CHECK(padk_axis(1, 0) == 0);      /* full deflection on the first sample, no ramp */
    CHECK(padk_axis(0, 1) == 255);
    CHECK(padk_axis(1, 1) == 128);    /* opposite keys cancel */
}

static void test_floor(void)
{
    const double g = (double)(1.0f / 75.0f);   /* the game's floor as the float it is */
    /* PS3 rate and below: untouched */
    CHECK(r3_move_floor_s(30.0, g, 1, 0.0) == g);
    CHECK(r3_move_floor_s(60.0, 0.0, 1, 0.0) == 0.0);
    /* 60 fps target: a 60 fps frame (was 1/75) */
    CHECK(near_(r3_move_floor_s(60.0, g, 1, 0.0), 1.0 / 60.0));
    CHECK(near_(r3_move_floor_s(59.94, g, 1, 0.0), 1.0 / 60.0));
    /* below 60: still capped at 1/60 */
    CHECK(near_(r3_move_floor_s(48.0, g, 1, 0.0), 1.0 / 60.0));
    /* 61..75: the frame period, never above it (no slow-down) */
    CHECK(near_(r3_move_floor_s(72.0, g, 1, 0.0), 1.0 / 72.0));
    CHECK(near_(r3_move_floor_s(75.0, g, 1, 0.0), 1.0 / 75.0));
    /* cinematic floor = ceiling = 1/30 is lowered to the same floor (real time) */
    CHECK(near_(r3_move_floor_s(60.0, 1.0 / 30.0, 1, 0.0), 1.0 / 60.0));
    /* a clock with a floor below 1/75 keeps it */
    CHECK(r3_move_floor_s(60.0, 0.001, 1, 0.0) == 0.001);
    /* R3_MOVE_DT_MIN=0: the previous flat 1/75 */
    CHECK(near_(r3_move_floor_s(60.0, g, 1, -1.0), 1.0 / 75.0));
    /* R3_MOVE_DT_MIN=<ms>: clamped to [1/75, frame period] */
    CHECK(near_(r3_move_floor_s(60.0, g, 1, 0.015), 0.015));
    CHECK(near_(r3_move_floor_s(60.0, g, 1, 0.005), 1.0 / 75.0));
    CHECK(near_(r3_move_floor_s(60.0, g, 1, 0.030), 1.0 / 60.0));
    /* R3_MOVE_FIX=0 and > 75 fps / uncapped: half a host frame */
    CHECK(near_(r3_move_floor_s(60.0, g, 0, 0.0), 0.5 / 60.0));
    CHECK(near_(r3_move_floor_s(144.0, g, 1, 0.0), 0.5 / 144.0));
    CHECK(near_(r3_move_floor_s(0.0, g, 1, 0.0), 0.5 / 500.0));
    /* the floor never exceeds the frame period at any target 31..75 */
    for (double f = 31.0; f <= 75.0; f += 0.5)
        CHECK(r3_move_floor_s(f, g, 1, 0.0) <= 1.0 / f + 1e-12 && r3_move_floor_s(f, g, 1, 0.0) >= 1.0 / 75.0 - 1e-12);
}

static void test_focus_and_activity(void)
{
    /* drain at the first focused poll (startup) and on each regain, not while focused */
    int was = 0;
    CHECK(padk_focus_drain(&was, 0) == 0);
    CHECK(padk_focus_drain(&was, 1) == 1);
    CHECK(padk_focus_drain(&was, 1) == 0);
    CHECK(padk_focus_drain(&was, 0) == 0);
    CHECK(padk_focus_drain(&was, 1) == 1);
    int was2 = 0;
    CHECK(padk_focus_drain(&was2, 1) == 1);   /* focused from the very first poll */

    /* a tap alone is not keyboard activity (no device switch), only a latch tap */
    unsigned tap = 0;
    CHECK(padk_kbm_key_activity(0x0001, PADK_UP, &tap) == 0 && tap == PADK_UP);
    CHECK(padk_kbm_key_activity(0x0001, 0, &tap) == 0 && tap == PADK_UP);   /* non-movement key */
    CHECK(padk_kbm_key_activity(0x8000, PADK_LEFT, &tap) == 1 && tap == PADK_UP);
    CHECK(padk_kbm_key_activity(0x8001, PADK_RIGHT, &tap) == 1 && tap == PADK_UP);
    CHECK(padk_kbm_key_activity(0x0000, PADK_DOWN, &tap) == 0 && tap == PADK_UP);
}

static void test_movelock_watch(void)
{
    struct r3_movelock_watch w = {0};
    CHECK(r3_movelock_step(&w, 0, 1, 1, 150) == 0);
    CHECK(r3_movelock_step(&w, 100, 1, 1, 150) == 0);
    CHECK(r3_movelock_step(&w, 120, 0, 1, 150) == 0);   /* stick released: re-arm */
    CHECK(r3_movelock_step(&w, 200, 1, 1, 150) == 0);
    CHECK(r3_movelock_step(&w, 300, 1, 0, 150) == 0);   /* hero moved: re-arm */
    CHECK(r3_movelock_step(&w, 400, 1, 1, 150) == 0);
    CHECK(r3_movelock_step(&w, 549, 1, 1, 150) == 0);
    CHECK(r3_movelock_step(&w, 550, 1, 1, 150) == 1);
    CHECK(r3_movelock_step(&w, 900, 1, 1, 150) == 0);   /* once only */
}
static void test_move_start(void)
{
    /* 30 fps and slower, paused (dt 0) or a stopped timescale: the game's own step */
    CHECK(r3_move_start_boost(1.0 / 30.0, 1.0) == 1.0);
    CHECK(r3_move_start_boost(1.0 / 20.0, 1.0) == 1.0);
    CHECK(r3_move_start_boost(0.0, 1.0) == 1.0);
    CHECK(r3_move_start_boost(1.0 / 60.0, 0.0) == 1.0);
    /* faster: the first step is scaled back to a 30 fps frame */
    CHECK(near_(r3_move_start_boost(1.0 / 60.0, 1.0), 2.0));
    CHECK(near_(r3_move_start_boost(1.0 / 144.0, 1.0), 4.8));
    CHECK(r3_move_start_boost(1e-6, 1.0) == 64.0);
    /* slow motion at 30 fps (scaled dt 1/60, scale 0.5) is still a 30 fps frame */
    CHECK(r3_move_start_boost(1.0 / 60.0, 0.5) == 1.0);
    /* rate 15: starts at 30 fps, fails at 60/144 without the fix, starts with it */
    const double r = 15.0;
    CHECK(r3_move_start_survives(r, 1.0 / 30.0, 1.0, 1.0));
    CHECK(!r3_move_start_survives(r, 1.0 / 60.0, 1.0, 1.0));
    CHECK(!r3_move_start_survives(r, 1.0 / 144.0, 1.0, 1.0));
    for (double f = 30.0; f <= 1000.0; f += 7.0) {
        const double dt = 1.0 / f;
        CHECK(r3_move_start_survives(r, dt, 1.0, r3_move_start_boost(dt, 1.0)));
    }
    /* a rate too small at 30 fps stays too small at any frame rate (no new creep) */
    CHECK(!r3_move_start_survives(8.0, 1.0 / 144.0, 1.0, r3_move_start_boost(1.0 / 144.0, 1.0)));
}

int main(void)
{
    test_latch();
    test_stick();
    test_floor();
    test_move_start();
    test_movelock_watch();
    test_focus_and_activity();
    printf("r3_input_test: %d/%d passed\n", s_n - s_fail, s_n);
    return s_fail ? 1 : 0;
}
