// Unit test for the R3 ultrawide / shadow-safe FOV / LOD maths (project/r3_fov_math.h; no game, no GPU).
// clang-cl /nologo /O1 /EHsc /std:c++17 /I project project\tests\fov_math_test.cpp /Fe:scratch\fov_math_test.exe /Fo:scratch\fov_math_test.obj
#include "r3_fov_math.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>

static int g_fail, g_checks;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, eps) CHECK(std::fabs((double)(a) - (double)(b)) <= (eps))

using namespace r3fov;
static const double A169 = 16.0 / 9.0;

// one func_00799EF0 call as the hook + the game see it: returns the stored +0x10 (radians, float) and +0xC
struct Stored { float rad, aspect; };
// ratio = the FOV setting's tangent multiplier; vm_call = a viewmodel caller's return address
static Stored setter(Table& tab, uint32_t key, double fov_in, double a_in, double ratio, double t, bool vm_call = false)
{
    Stored s{ (float)((float)fov_in * 0.0174533f), (float)a_in };
    double nf, na;
    if (!frustum_step(tab, key, fov_in, a_in, vm_call, Setting{ t, ratio, 1.0, 1.0, false }, &nf, &na)) return s;
    s.rad = (float)((float)nf * 0.0174533f);
    s.aspect = (float)na;
    return s;
}
static double refed(const Stored& s) { return (float)(s.rad * 57.29578f); }   // callers: +0x10 * 57.29578

int main()
{
    // the game's tan and its inverse
    for (double k : { 1.0, 0.85, 0.7 })
        for (double h = 0.05; h < (k == 1.0 ? 1.3 : 0.78); h += 0.05) NEAR(game_atan(game_tan(h, k), k), h, 1e-9);
    for (double t = 0.05; t < 8.0; t *= 1.3) NEAR(game_tan(game_atan(t, 0.7), 0.7), t, 1e-9 * t);
    NEAR(game_tan(0.5, 1.0), std::tan(0.5), 2e-4);

    // R3_ULTRAWIDE values
    double f;
    CHECK(parse_aspect(nullptr, &f) == 0);
    CHECK(parse_aspect("off", &f) == 0);
    CHECK(parse_aspect("16:9", &f) == 0);
    CHECK(parse_aspect("auto", &f) == 1);
    CHECK(parse_aspect("21:9", &f) == 2); NEAR(f, 64.0 / 27.0, 1e-12);
    CHECK(parse_aspect("32:9", &f) == 2); NEAR(f, 32.0 / 9.0, 1e-12);
    CHECK(parse_aspect("3440x1440", &f) == 2); NEAR(f, 3440.0 / 1440.0, 1e-12);
    CHECK(parse_aspect("2.4", &f) == 2); NEAR(f, 2.4, 1e-12);
    CHECK(parse_aspect("9:16", &f) == 0);
    CHECK(parse_aspect("junk", &f) == 0);

    // FOV setting values
    double sc, k;
    CHECK(parse_fov("", &sc, &k) && sc == 1.0 && k == 1.0);
    CHECK(parse_fov("1.3", &sc, &k) && sc == 1.3 && k == 1.0);
    CHECK(parse_fov("0.7", &sc, &k) && sc == 1.0 && k == 0.7);
    CHECK(!parse_fov("3", &sc, &k));
    CHECK(!parse_fov("0.1", &sc, &k));
    // the legacy constant gives about +55% at a 70 deg FOV; k_for_scale is its inverse
    const double h35 = 35.0 * kPi / 180.0;
    const double r07 = fov_ratio(h35, 1.0, 0.7);
    CHECK(r07 > 1.4 && r07 < 1.7);
    NEAR(k_for_scale(r07), 0.7, 1e-6);
    NEAR(fov_ratio(h35, 1.25, 1.0), 1.25, 0);

    // Hor+: 21:9 keeps the vertical FOV, widens the horizontal one
    double nf, na;
    CHECK(widen(70.0, A169, 1.0, 64.0 / 27.0, 1.0, &nf, &na));
    NEAR(na, 64.0 / 27.0, 1e-12);
    NEAR(game_tan(nf * kPi / 360.0, 1.0) / na, game_tan(70.0 * kPi / 360.0, 1.0) / A169, 1e-9);
    CHECK(nf > 85.0 && nf < 90.0);
    // also with the legacy constant in memory (R3_FOV_SAFE=0)
    CHECK(widen(70.0, A169, 1.0, 32.0 / 9.0, 0.7, &nf, &na));
    NEAR(game_tan(nf * kPi / 360.0, 0.7) / na, game_tan(70.0 * kPi / 360.0, 0.7) / A169, 1e-9);
    // narrower than 16:9: horizontal FOV kept, vertical grows
    CHECK(widen(70.0, A169, 1.0, 1.6, 1.0, &nf, &na)); NEAR(nf, 70.0, 0); NEAR(na, 1.6, 0);
    // 16:9 target / off: nothing
    CHECK(!widen(70.0, A169, 1.0, A169, 1.0, &nf, &na));
    CHECK(!widen(70.0, A169, 1.0, 0.0, 1.0, &nf, &na));
    // shadow / cube frustums (aspect 1) are never touched
    CHECK(!widen(90.0, 1.0, 1.5, 64.0 / 27.0, 1.0, &nf, &na)); NEAR(nf, 90.0, 0); NEAR(na, 1.0, 0);
    // FOV setting: the tangent scales by the ratio, aspect stays (zoom while aiming stays proportional)
    for (double fov : { 70.0, 40.0, 20.0 }) {
        CHECK(widen(fov, A169, 1.3, 0.0, 1.0, &nf, &na)); NEAR(na, A169, 0);
        NEAR(game_tan(nf * kPi / 360.0, 1.0), 1.3 * game_tan(fov * kPi / 360.0, 1.0), 1e-9);
    }
    // both: tangent x ratio x (t / a)
    CHECK(widen(70.0, A169, 1.2, 64.0 / 27.0, 1.0, &nf, &na));
    NEAR(game_tan(nf * kPi / 360.0, 1.0), 1.2 * (64.0 / 27.0) / A169 * game_tan(70.0 * kPi / 360.0, 1.0), 1e-9);
    // clamped below 170 deg
    CHECK(widen(160.0, A169, 2.0, 32.0 / 9.0, 1.0, &nf, &na)); CHECK(nf <= 170.0);

    // re-fed FOV / aspect (func_001B3E28, func_00276BA4): no compounding over many frames
    {
        Table tab;
        const double t = 64.0 / 27.0;
        Stored s = setter(tab, 0x1000, 70.0, A169, 1.2, t);
        const Stored first = s;
        for (int i = 0; i < 200; i++) s = setter(tab, 0x1000, refed(s), s.aspect, 1.2, t);
        NEAR(s.rad, first.rad, 1e-6); NEAR(s.aspect, first.aspect, 0);
        // a fresh value from the camera (zoom while aiming) is widened normally
        s = setter(tab, 0x1000, 40.0, A169, 1.2, t);
        widen(40.0, A169, 1.2, t, 1.0, &nf, &na);
        NEAR(refed(s), nf, 2e-3);
        // copied into another frustum (func_00276BA4: R_31 <- R_27's values)
        const Stored c = setter(tab, 0x2000, refed(s), s.aspect, 1.2, t);
        NEAR(c.rad, s.rad, 1e-6); NEAR(c.aspect, s.aspect, 0);
        // setting back to Original: the stored widened value maps back to the original
        s = setter(tab, 0x1000, refed(s), s.aspect, 1.0, 0.0);
        NEAR(refed(s), 40.0, 2e-3); NEAR(s.aspect, A169, 1e-6);
    }
    // viewmodel (func_003F5478): takes the scene's widened aspect; its FOV is not widened by the setting,
    // and its entry does not catch another frustum's FOV
    {
        Table tab;
        const double t = 64.0 / 27.0;
        const Stored sc = setter(tab, 0x1C00, 70.0, A169, 1.3, t);
        const Stored vm = setter(tab, 0x2280, 55.0, sc.aspect, 1.3, t, true);
        NEAR(vm.aspect, t, 1e-6);
        widen(55.0, A169, 1.0, t, 1.0, &nf, &na);
        NEAR(refed(vm), nf, 2e-3);
        CHECK(tab.orig_fov(0x1C00, refed(vm)) == refed(vm));   // not mapped through the viewmodel entry
        // LOD compensation: the scene frustum's widening, none for unknown values
        const double w = tab.widening(0x1C00, sc.rad, 1.0);
        NEAR(w, 1.3 * t / A169, 2e-4);
        NEAR(tab.widening(0x9999, sc.rad, 1.0), w, 0);       // found by value (another view object)
        NEAR(tab.widening(0x1C00, 1.0, 1.0), 1.0, 0);
    }
    // func_00276BA4 every frame with R3_FOV_VIEWMODEL=0: the scene frustum re-fed (0x276F2C), the viewmodel
    // set from the player FOV (0x3F5608), re-fed (0x276EE8) and copied into another view's +0x2280 (0x276F50).
    // The viewmodel only gets the ultrawide widening, stays stable, and never feeds the scene fallback.
    for (int lr_known = 0; lr_known < 2; lr_known++) {
        Table tab;
        const double t = 64.0 / 27.0;
        const uint32_t S = 0x1C00, V = 0x2280, V2 = 0x7280;
        Stored sc = setter(tab, S, 70.0, A169, 1.3, t);
        Stored v{}, v2{};
        double vw, sw;
        widen(55.0, A169, 1.0, t, 1.0, &vw, &na);
        widen(70.0, A169, 1.3, t, 1.0, &sw, &na);
        for (int frame = 0; frame < 50; frame++) {
            sc = setter(tab, S, refed(sc), sc.aspect, 1.3, t);
            v = setter(tab, V, 55.0, sc.aspect, 1.3, t, true);
            // the re-feed is recognised by the frustum alone when the return address is not known
            v = setter(tab, V, refed(v), v.aspect, 1.3, t, lr_known != 0);
            v2 = setter(tab, V2, refed(v), v.aspect, 1.3, t, true);
            NEAR(refed(v), vw, 2e-3); NEAR(v.aspect, t, 1e-6);
            NEAR(v2.rad, v.rad, 1e-6); NEAR(v2.aspect, v.aspect, 0);
            NEAR(refed(sc), sw, 2e-3);
        }
        CHECK(tab.is_vm(V) && tab.is_vm(V2) && !tab.is_vm(S));
        CHECK(tab.orig_fov(0x9999, refed(v)) == refed(v));   // not in the scene fallback
        NEAR(tab.widening(0x9999, sc.rad, 1.0), 1.3 * t / A169, 2e-4);
        NEAR(tab.widening(0x9999, v.rad, 1.0), 1.0, 0);
        // a copy from another viewmodel finds it by value even before its own entry exists
        const Stored v3 = setter(tab, 0x9280, refed(v), v.aspect, 1.3, t, true);
        NEAR(v3.rad, v.rad, 1e-6);
    }
    // the viewmodel role is not permanent: a reused address re-classifies after kRoleFrames idle frames
    // or a level change, while the value mappings (re-fed widened values) survive both
    {
        const double t = 64.0 / 27.0;
        widen(70.0, A169, 1.3, t, 1.0, &nf, &na);
        const double scene_w = nf;
        for (int how = 0; how < 2; how++) {
            Table tab;
            tab.begin(1000, 1);
            Stored sc = setter(tab, 0x1C00, 70.0, A169, 1.3, t);
            const Stored v = setter(tab, 0x2280, 55.0, sc.aspect, 1.3, t, true);
            CHECK(tab.is_vm(0x2280));
            tab.begin(1000 + Table::kRoleFrames, 1);
            CHECK(tab.is_vm(0x2280));                                   // still within the window
            if (how == 0) tab.begin(1001 + Table::kRoleFrames, 1);      // idle too long
            else          tab.begin(1001, 2);                           // next level
            CHECK(!tab.is_vm(0x2280));
            // the old viewmodel address now holds a scene camera: the FOV setting applies again
            const Stored s2 = setter(tab, 0x2280, 70.0, A169, 1.3, t);
            NEAR(refed(s2), scene_w, 2e-3);
            CHECK(!tab.is_vm(0x2280));
            // a camera that outlived the load still maps its re-fed widened value back
            sc = setter(tab, 0x1C00, refed(sc), sc.aspect, 1.3, t);
            NEAR(refed(sc), scene_w, 2e-3);
            (void)v;
        }
        // a frustum set every frame keeps its role indefinitely
        Table tab;
        Stored sc = setter(tab, 0x1C00, 70.0, A169, 1.3, t), v{};
        for (uint32_t f = 0; f < 3 * Table::kRoleFrames; f++) {
            tab.begin(f, 0);
            sc = setter(tab, 0x1C00, refed(sc), sc.aspect, 1.3, t);
            if (f == 0) v = setter(tab, 0x2280, 55.0, sc.aspect, 1.3, t, true);
            else        v = setter(tab, 0x2280, refed(v), v.aspect, 1.3, t, false);
        }
        CHECK(tab.is_vm(0x2280));
        widen(55.0, A169, 1.0, t, 1.0, &nf, &na);
        NEAR(refed(v), nf, 2e-3);
    }
    // LRU replacement keeps working past 32 frustums
    {
        Table tab;
        for (uint32_t i = 1; i <= 100; i++) tab.record(i * 16, 70.0, 80.0 + i * 0.1);
        NEAR(tab.orig_fov(100 * 16, 80.0 + 100 * 0.1), 70.0, 1e-4);
    }

    std::printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "OK", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
