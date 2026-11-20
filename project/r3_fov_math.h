/* Resistance 3 PC port: pure maths of the ultrawide (Hor+), shadow-safe FOV and LOD hooks
 * (r3_patches.cpp). No game memory, no globals: unit-tested by project/tests/fov_math_test.cpp. */
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace r3fov {

constexpr double kPi = 3.14159265358979323846;

/* func_00B6CDE8 (the game's tanf, same as R2's func_00788068) with constant term K
 * (0x00F0608C, 1.0; the old "Extended FOV" patch lowers it). */
inline double game_tan(double x, double k)
{
    const double q = std::floor(x * (2.0 / kPi) + 0.5);
    const double r = x - q * (kPi / 2.0);
    const double r2 = r * r;
    const double num = r + r * r2 * -0.0957823;
    const double den = k + r2 * (-0.42911619 + r2 * 0.0097099366);
    if (((long long)q) & 1) return num != 0.0 ? -den / num : 0.0;
    return den != 0.0 ? num / den : 0.0;
}

/* half angle x in (0, 1.53) with game_tan(x, k) == t. game_tan is increasing on (0, pi/4) and on
 * (pi/4, 1.53); with K != 1 it jumps down at pi/4 (the two branches no longer meet), so the lower
 * branch is searched first. */
inline double game_atan(double t, double k)
{
    double lo = 1e-6, hi = 1.53;
    const double q = kPi / 4.0 - 1e-9;
    if (k != 1.0) {
        if (t <= game_tan(q, k)) hi = q;
        else lo = kPi / 4.0 + 1e-9;
    }
    if (t <= game_tan(lo, k)) return lo;
    if (t >= game_tan(hi, k)) return hi;
    for (int i = 0; i < 60; i++) {
        const double m = 0.5 * (lo + hi);
        if (game_tan(m, k) < t) lo = m; else hi = m;
    }
    return 0.5 * (lo + hi);
}

/* R3_ULTRAWIDE: -> 0 off, 1 follow the window, 2 fixed (*fixed = w/h) */
inline int parse_aspect(const char* e, double* fixed)
{
    *fixed = 0.0;
    if (!e || !*e || !_stricmp(e, "off") || !std::strcmp(e, "0") || !_stricmp(e, "16:9")) return 0;
    if (!_stricmp(e, "auto") || !_stricmp(e, "window") || !_stricmp(e, "on") || !std::strcmp(e, "1")) return 1;
    double a = 0.0, b = 0.0;
    if (!_stricmp(e, "21:9")) a = 64.0, b = 27.0;            /* 2560x1080; 3440x1440 is 43:18 */
    else if (sscanf_s(e, "%lf:%lf", &a, &b) == 2 || sscanf_s(e, "%lfx%lf", &a, &b) == 2) {}
    else { a = std::atof(e); b = 1.0; }
    if (!(a > 0.0 && b > 0.0 && a / b >= 1.0 && a / b <= 5.0)) return 0;
    *fixed = a / b;
    return 2;
}

/* The FOV setting (R2_PATCH_FOV_VALUE, launcher / F10 / Options rows):
 *   ""               original
 *   1.0 < v <= 2.0   tan multiplier (1.2 = the view 20% wider at every zoom level)
 *   0.3 <= v < 1.0   legacy "Extended FOV" constant K (0.85 / 0.7 of older settings)
 * -> false when invalid. *scale = 1, *k = 1 means original. */
inline bool parse_fov(const char* v, double* scale, double* k)
{
    *scale = 1.0; *k = 1.0;
    if (!v || !*v) return true;
    const double x = std::atof(v);
    if (x > 1.0 && x <= 2.0) { *scale = x; return true; }
    if (x >= 0.3 && x < 1.0) { *k = x; return true; }
    return x == 1.0;
}

/* tan multiplier of the setting at half angle h (K: the ratio the constant patch gave) */
inline double fov_ratio(double h, double scale, double k)
{
    if (k != 1.0) {
        const double r = game_tan(h, k) / game_tan(h, 1.0);
        return (r > 1.0 && r < 20.0) ? r : 1.0;
    }
    return scale;
}

/* K that gives the tan multiplier s at a typical first-person half angle (35 deg), for the
 * plain constant patch (R3_FOV_SAFE=0) */
inline double k_for_scale(double s)
{
    const double h = 35.0 * kPi / 180.0, r2 = h * h;
    const double den1 = 1.0 + r2 * (-0.42911619 + r2 * 0.0097099366);
    const double k = den1 / s - (den1 - 1.0);
    return k < 0.3 ? 0.3 : k > 1.0 ? 1.0 : k;
}

inline bool scene_aspect(double a) { return a > 1.70 && a < 1.86; }

/* One call of the frustum setter func_00799EF0 (f3 = horizontal FOV, degrees; f4 = aspect w/h):
 * the 16:9 scene frustum widened by `ratio` (FOV setting, tan multiplier) and to the target aspect
 * `t` (0 = off): wider than 16:9 keeps the vertical FOV and widens the horizontal one (Hor+),
 * narrower keeps the horizontal one. kmem = the K in game memory. -> false = unchanged. */
inline bool widen(double fov, double a, double ratio, double t, double kmem, double* out_fov, double* out_a)
{
    *out_fov = fov; *out_a = a;
    if (!scene_aspect(a) || !(fov > 1.0 && fov < 170.0)) return false;
    double kx = 1.0;
    if (t > 0.0 && std::fabs(t / a - 1.0) >= 0.005) { kx = t / a; *out_a = t; }
    const double m = ratio * (kx > 1.0 ? kx : 1.0);
    if (m != 1.0) {
        const double h = fov * (kPi / 360.0);
        const double nh = game_atan(game_tan(h, kmem) * m, kmem);
        *out_fov = nh * (360.0 / kPi);
        if (*out_fov > 170.0) *out_fov = 170.0;
    }
    return *out_fov != fov || *out_a != a;
}

/* Callers feed the frustum's own stored FOV (+0x10 radians * 57.29578) and aspect (+0xC) back into
 * the setter (func_001B3E28, func_00276BA4, the viewmodel takes the scene's aspect), so a widened
 * frustum would widen again every frame. The table remembers what each frustum was widened from;
 * a value that matches a widened one is mapped back to its original first. */
struct Table {
    /* vm: a first-person viewmodel frustum (its FOV is only matched against other viewmodel entries) */
    struct Entry { uint32_t key; float orig, wide; uint32_t age, frame; bool vm; };
    struct Asp { float orig, wide; };
    /* the viewmodel role is forgotten after this many frames without a call (an address the game
     * reuses for another camera classifies afresh) and on a level change */
    static constexpr uint32_t kRoleFrames = 300;
    Entry e[32] = {};
    Asp asp[4] = {};
    uint32_t clock = 0, now = 0;
    int level = 0;

    /* before each call: the current frame number and level counter. A new level drops every
     * viewmodel role; the value mappings stay, since cameras that outlive the load re-feed their
     * widened values. */
    void begin(uint32_t frame, int level_gen)
    {
        now = frame;
        if (level_gen != level) {
            level = level_gen;
            for (Entry& x : e) x.vm = false;
        }
    }

    static bool same_fov(double x, float w) { return w > 0.0f && std::fabs(x - (double)w) <= 2e-3; }

    double orig_aspect(double a) const
    {
        for (const Asp& p : asp)
            if (p.wide > 0.0f && std::fabs(a - (double)p.wide) <= 1e-4 * (a > 1.0 ? a : 1.0)) return p.orig;
        return a;
    }
    /* own entry first, then (copied from another frustum) any entry of the same kind */
    double orig_fov(uint32_t key, double fov, bool vm = false) const
    {
        for (const Entry& x : e) if (x.key == key && x.key && same_fov(fov, x.wide)) return x.orig;
        for (const Entry& x : e) if (x.key && x.vm == vm && same_fov(fov, x.wide)) return x.orig;
        return fov;
    }
    /* the frustum was set as a viewmodel before (the flag stays with the frustum, whoever calls) */
    bool is_vm(uint32_t key) const
    {
        for (const Entry& x : e) if (x.key == key && x.key) return x.vm && now - x.frame <= kRoleFrames;
        return false;
    }
    void record(uint32_t key, double orig, double wide, bool vm = false)
    {
        Entry* slot = nullptr;
        for (Entry& x : e) if (x.key == key) { slot = &x; break; }
        if (!slot) {
            slot = &e[0];
            for (Entry& x : e) { if (!x.key) { slot = &x; break; } if (x.age < slot->age) slot = &x; }
        }
        slot->key = key; slot->orig = (float)orig; slot->wide = (float)wide; slot->age = ++clock; slot->frame = now; slot->vm = vm;
    }
    void record_aspect(double orig, double wide)
    {
        if (std::fabs(orig - wide) < 1e-6) return;
        for (Asp& p : asp) if (std::fabs((double)p.wide - wide) < 1e-6) { p.orig = (float)orig; return; }
        for (int i = 3; i > 0; i--) asp[i] = asp[i - 1];
        asp[0].orig = (float)orig; asp[0].wide = (float)wide;
    }
    /* the widening a frustum with stored FOV fov_rad has: tan(wide/2) / tan(orig/2) (1 = none) */
    double widening(uint32_t key, double fov_rad, double kmem) const
    {
        const double d = fov_rad * (180.0 / kPi);
        const Entry* hit = nullptr;
        for (const Entry& x : e) if (x.key == key && x.key && same_fov(d, x.wide)) { hit = &x; break; }
        if (!hit) for (const Entry& x : e) if (x.key && !x.vm && same_fov(d, x.wide)) { hit = &x; break; }
        if (!hit || !(hit->orig > 0.5f)) return 1.0;
        const double r = game_tan(hit->wide * (kPi / 360.0), kmem) / game_tan(hit->orig * (kPi / 360.0), kmem);
        return (r > 1.0 && r < 20.0) ? r : 1.0;
    }
};

/* the FOV / ultrawide settings as one setter call sees them */
struct Setting {
    double t;            /* target aspect (0 = off) */
    double scale, k;     /* FOV setting (1, 1 = original; R3_FOV_SAFE=0 passes 1, 1) */
    double kmem;         /* the K constant in memory */
    bool vm_widens;      /* R3_FOV_VIEWMODEL=1 */
};

/* One func_00799EF0 call: maps re-fed widened values back, widens, records. vm_call = the caller is
 * known to set a viewmodel frustum; a frustum once set as a viewmodel stays one (func_00276BA4
 * re-feeds and copies those frustums from other return addresses). false = not a 16:9 frustum. */
inline bool frustum_step(Table& tab, uint32_t key, double fov_in, double a_in, bool vm_call, const Setting& s,
                         double* nf, double* na)
{
    const double a = tab.orig_aspect(a_in);
    if (!scene_aspect(a)) return false;
    const bool vm = vm_call || tab.is_vm(key);
    const double fov = tab.orig_fov(key, fov_in, vm);
    double ratio = 1.0;
    if ((s.scale != 1.0 || s.k != 1.0) && !(vm && !s.vm_widens)) ratio = fov_ratio(fov * (kPi / 360.0), s.scale, s.k);
    widen(fov, a, ratio, s.t, s.kmem, nf, na);
    tab.record(key, fov, (float)*nf, vm);
    tab.record_aspect(a, (float)*na);
    return true;
}

} // namespace r3fov
