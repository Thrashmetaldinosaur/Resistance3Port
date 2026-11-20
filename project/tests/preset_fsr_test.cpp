// R2_PRESET's RSX_FSR under R2_RSX=d3d12 and the D3D12 -> RPCS3 fallback renumbering.
// Build (with rsx_d3d12_engine.c in helpers-only mode):
//   clang-cl /DRSX_D3D12_HELPERS_ONLY /I ps3recomp\libs\video /I ps3recomp\include
//            project\tests\preset_fsr_test.cpp ps3recomp\libs\video\rsx_d3d12_engine.c
#include "../r2_autoconfig.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int rsx_d3d12_fsr_to_rpcs3(int fsr, int* fsr1_filter);

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

// The preset value written for `rsx`, then what the fallback hands the RPCS3 renderer.
static int fallback(const char* preset_fsr, const char* rsx)
{
    const char* w = r2ac::fsr_for_renderer(preset_fsr, rsx);
    return rsx_d3d12_fsr_to_rpcs3(std::atoi(w), nullptr);
}

int main()
{
    using r2ac::fsr_for_renderer;
    CHECK(!std::strcmp(fsr_for_renderer("1", "d3d12"), "3"));    // FSR 3.1
    CHECK(!std::strcmp(fsr_for_renderer("1", "D3D12"), "3"));
    CHECK(!std::strcmp(fsr_for_renderer("2", "d3d12"), "2"));    // DLSS
    CHECK(!std::strcmp(fsr_for_renderer("0", "d3d12"), "0"));
    CHECK(!std::strcmp(fsr_for_renderer("1", "rpcs3"), "1"));
    CHECK(!std::strcmp(fsr_for_renderer("1", nullptr), "1"));
    // preset under d3d12, then the late fallback (relaunch_compatible): the RPCS3 value is the preset's own
    CHECK(fallback("1", "d3d12") == 1);
    CHECK(fallback("2", "d3d12") == 2);
    CHECK(fallback("0", "d3d12") == 0);
    std::printf("%d/%d checks passed\n", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
