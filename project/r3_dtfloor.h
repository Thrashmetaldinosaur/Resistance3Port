/* R3 game-clock floor maths (stubs.cpp r3_dt_floor), kept pure for tests.
 *
 * The game clock (func_007EEA44 / 007EEA50) steps by
 *     dt = max(min(real, ceiling), floor)        -- the floor wins
 * with the game's own floor 1/75 s. Starting to walk from a standstill fails
 * when a step is short (RPCS3 "Unlock FPS" note: left stick fails above
 * 75 fps); the guest test is the walk-start threshold below
 * (r3_move_start_boost, R3_MOVE_START_FIX), fixed at every frame rate.
 *
 * r3_move_floor_s() returns the floor for one clock update:
 *   - fps target <= 30 (incl. the PS3's own 30) or a non-positive game floor:
 *     the game's value, untouched;
 *   - move_fix off, or a target above 75 / uncapped: 0.5 / fps (half a host
 *     frame, no slow-down at high fps; movement can still lock there);
 *   - targets 31..75 (R3_MOVE_FIX, default): the frame period clamped to
 *     [1/75, 1/60] -- no step is shorter than a steady 60 fps (or the target's
 *     own) frame, the cadence the engine is known to handle. dt_min_s > 0
 *     overrides it (R3_MOVE_DT_MIN=<ms>), clamped to [1/75, frame period] so
 *     the game never runs slower than real time; dt_min_s < 0 = the previous
 *     floor, 1/75 s flat.
 *   - a game floor below 1/75 (a clock that wants short steps) is kept.
 */
#ifndef R3_DTFLOOR_H
#define R3_DTFLOOR_H

#define R3_GAME_FLOOR_S (1.0 / 75.0)

static inline double r3_move_floor_s(double fps, double game_floor, int move_fix, double dt_min_s)
{
    if ((fps > 0.0 && fps <= 30.0) || !(game_floor > 0.0)) return game_floor;
    if (!move_fix || fps <= 0.0 || fps > 75.0) {
        const double f = fps <= 0.0 || fps > 500.0 ? 500.0 : fps;
        const double m = 0.5 / f;
        return m < game_floor ? m : game_floor;
    }
    double m;
    if (dt_min_s < 0.0) m = R3_GAME_FLOOR_S;
    else {
        m = dt_min_s > 0.0 ? dt_min_s : 1.0 / fps;
        if (dt_min_s == 0.0 && m > 1.0 / 60.0) m = 1.0 / 60.0;   /* auto: never above a 60 fps frame */
        if (m > 1.0 / fps) m = 1.0 / fps;                        /* never above the frame period: no slow-down */
        if (m < R3_GAME_FLOOR_S) m = R3_GAME_FLOOR_S;
    }
    /* the game's own floor (1/75) is raised; a lower special-purpose floor is kept */
    return game_floor < R3_GAME_FLOOR_S * 0.999 ? game_floor : m;
}

/* Walk start from a standstill (ground state 0x2B1 update, func_001B0EC0).
 * Each frame the hero velocity (+0xF0) is reset to the measured one (+0x120);
 * if that is exactly zero, the stick steering func_00187130 moves it toward the
 * target by at most rate * dt, and then
 *     0x001B11E4: if |v| < 0.3 * timescale  ->  v = 0   (0.3 at 0x001B0E74)
 * The 0.3 m/s start threshold is per frame but not scaled by dt: tuned for the
 * PS3's 30 fps, a first step of rate * dt falls below it at higher frame rates
 * and the hero never starts (until a jump lands with a non-zero velocity).
 * The fix gives that first step its 30 fps size: the rates passed to
 * func_00187130 are multiplied by r3_move_start_boost() while the measured
 * velocity is zero (the step is still capped at the stick target).
 * dt_scaled = game timer dt (0x012DAE0C), scale = its time scale (0x012DAE08). */
#define R3_MOVE_START_REF_S (1.0 / 30.0)

static inline double r3_move_start_boost(double dt_scaled, double scale)
{
    if (!(dt_scaled > 0.0) || !(scale > 0.0)) return 1.0;
    const double real = dt_scaled / scale;
    if (!(real < R3_MOVE_START_REF_S)) return 1.0;
    const double k = R3_MOVE_START_REF_S / real;
    return k > 64.0 ? 64.0 : k;
}

/* [move-lock] safety net: fires once when movement input has been held for hold_ms while the
 * hero's measured velocity stays zero (pushing into a wall also qualifies; it is logged once). */
struct r3_movelock_watch { double since_ms; int armed; int fired; };

static inline int r3_movelock_step(struct r3_movelock_watch* w, double now_ms, int held, int vel_zero, double hold_ms)
{
    if (w->fired) return 0;
    if (!held || !vel_zero) { w->armed = 0; return 0; }
    if (!w->armed) { w->armed = 1; w->since_ms = now_ms; return 0; }
    if (now_ms - w->since_ms < hold_ms) return 0;
    w->fired = 1;
    return 1;
}

/* The guest's start test: does a first step at `rate` (m/s^2) survive the threshold? */
static inline int r3_move_start_survives(double rate, double dt_scaled, double scale, double boost)
{
    return rate * dt_scaled * boost >= 0.3 * scale;
}

#endif /* R3_DTFLOOR_H */
