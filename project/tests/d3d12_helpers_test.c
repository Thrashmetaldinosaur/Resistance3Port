/* Unit test for the Direct3D 12 (experimental) renderer's pure helpers (rsx_d3d12_engine.c, built with
 * RSX_D3D12_HELPERS_ONLY: no device, no game).
 * clang-cl /nologo /O1 /DRSX_D3D12_HELPERS_ONLY /I ps3recomp\libs\video /I ps3recomp\include project\tests\d3d12_helpers_test.c /Fe:scratch\d3d12_helpers_test.exe /Fo:scratch\d3d12_helpers_test.obj */
#include "../../ps3recomp/libs/video/rsx_d3d12_engine.c"
#include <stdio.h>

static int g_fail, g_checks;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int lb(uint32_t W, uint32_t H, uint32_t aw, uint32_t ah, uint32_t ex, uint32_t ey, uint32_t ew, uint32_t eh)
{
    uint32_t x, y, w, h;
    rsx_d3d12_letterbox(W, H, aw, ah, &x, &y, &w, &h);
    return x == ex && y == ey && w == ew && h == eh;
}

typedef struct { uint32_t sx, sy, sw, sh, dx, dy, dw, dh; } rect8;
static int clip(uint32_t sW, uint32_t sH, uint32_t dW, uint32_t dH, rect8* r)
{
    return rsx_d3d12_clip_blit(sW, sH, dW, dH, &r->sx, &r->sy, &r->sw, &r->sh, &r->dx, &r->dy, &r->dw, &r->dh);
}

int main(void)
{
    /* blend factors: *_COLOR factors become their alpha twins on the alpha channel */
    CHECK(rsx_d3d12_blend_factor(0x0000, 0) == D3D12_BLEND_ZERO);
    CHECK(rsx_d3d12_blend_factor(0x0001, 1) == D3D12_BLEND_ONE);
    CHECK(rsx_d3d12_blend_factor(0x0300, 0) == D3D12_BLEND_SRC_COLOR);
    CHECK(rsx_d3d12_blend_factor(0x0300, 1) == D3D12_BLEND_SRC_ALPHA);
    CHECK(rsx_d3d12_blend_factor(0x0307, 1) == D3D12_BLEND_INV_DEST_ALPHA);
    CHECK(rsx_d3d12_blend_factor(0x0308, 0) == D3D12_BLEND_SRC_ALPHA_SAT);
    CHECK(rsx_d3d12_blend_factor(0x0308, 1) == D3D12_BLEND_ONE);
    CHECK(rsx_d3d12_blend_factor(0x8001, 0) == D3D12_BLEND_BLEND_FACTOR);
    CHECK(rsx_d3d12_blend_factor(0x8004, 1) == D3D12_BLEND_INV_BLEND_FACTOR);
    CHECK(rsx_d3d12_blend_factor(0x1234, 0) == D3D12_BLEND_ONE);

    /* blend equations, including the RSX signed variants */
    CHECK(rsx_d3d12_blend_op(0x8006) == D3D12_BLEND_OP_ADD);
    CHECK(rsx_d3d12_blend_op(0x8007) == D3D12_BLEND_OP_MIN);
    CHECK(rsx_d3d12_blend_op(0x8008) == D3D12_BLEND_OP_MAX);
    CHECK(rsx_d3d12_blend_op(0x800A) == D3D12_BLEND_OP_SUBTRACT);
    CHECK(rsx_d3d12_blend_op(0x800B) == D3D12_BLEND_OP_REV_SUBTRACT);
    CHECK(rsx_d3d12_blend_op(0xF005) == D3D12_BLEND_OP_REV_SUBTRACT);
    CHECK(rsx_d3d12_blend_op(0xF006) == D3D12_BLEND_OP_ADD);

    /* compare: GL 0x200.. and the bare 0..7 encoding */
    CHECK(rsx_d3d12_compare(0x200) == D3D12_COMPARISON_FUNC_NEVER);
    CHECK(rsx_d3d12_compare(0x201) == D3D12_COMPARISON_FUNC_LESS);
    CHECK(rsx_d3d12_compare(0x203) == D3D12_COMPARISON_FUNC_LESS_EQUAL);
    CHECK(rsx_d3d12_compare(0x206) == D3D12_COMPARISON_FUNC_GREATER_EQUAL);
    CHECK(rsx_d3d12_compare(0x207) == D3D12_COMPARISON_FUNC_ALWAYS);
    CHECK(rsx_d3d12_compare(2) == D3D12_COMPARISON_FUNC_EQUAL);
    CHECK(rsx_d3d12_compare(0x208) == D3D12_COMPARISON_FUNC_ALWAYS);

    /* stencil ops */
    CHECK(rsx_d3d12_stencil_op(0x1E00) == D3D12_STENCIL_OP_KEEP);
    CHECK(rsx_d3d12_stencil_op(0x0000) == D3D12_STENCIL_OP_ZERO);
    CHECK(rsx_d3d12_stencil_op(0x1E01) == D3D12_STENCIL_OP_REPLACE);
    CHECK(rsx_d3d12_stencil_op(0x1E02) == D3D12_STENCIL_OP_INCR_SAT);
    CHECK(rsx_d3d12_stencil_op(0x8507) == D3D12_STENCIL_OP_INCR);
    CHECK(rsx_d3d12_stencil_op(0x150A) == D3D12_STENCIL_OP_INVERT);

    /* wrap codes */
    CHECK(rsx_d3d12_wrap(1) == D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    CHECK(rsx_d3d12_wrap(2) == D3D12_TEXTURE_ADDRESS_MODE_MIRROR);
    CHECK(rsx_d3d12_wrap(3) == D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    CHECK(rsx_d3d12_wrap(4) == D3D12_TEXTURE_ADDRESS_MODE_BORDER);
    CHECK(rsx_d3d12_wrap(5) == D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    CHECK(rsx_d3d12_wrap(7) == D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE);
    CHECK(rsx_d3d12_wrap(0) == D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    /* letterbox / pillarbox */
    CHECK(lb(1920, 1080, 16, 9, 0, 0, 1920, 1080));
    CHECK(lb(1920, 1080, 4, 3, 240, 0, 1440, 1080));
    CHECK(lb(1280, 1024, 16, 9, 0, 152, 1280, 720));
    CHECK(lb(2560, 1080, 16, 9, 320, 0, 1920, 1080));
    CHECK(lb(800, 600, 0, 9, 0, 0, 800, 600));       /* aspect 0: stretch */
    CHECK(lb(0, 0, 16, 9, 0, 0, 0, 0));

    /* blit clipping */
    {   /* identity inside both */
        rect8 r = { 10, 20, 30, 40, 1, 2, 30, 40 };
        CHECK(clip(100, 100, 100, 100, &r) && r.sx == 10 && r.sw == 30 && r.dx == 1 && r.dh == 40);
    }
    {   /* 2x upscale past the destination's right edge: the source shrinks proportionally */
        rect8 r = { 0, 0, 64, 10, 0, 0, 128, 20 };
        CHECK(clip(64, 10, 100, 20, &r) && r.dw == 100 && r.sw == 50 && r.sh == 10 && r.dh == 20);
    }
    {   /* source past its right edge: the destination shrinks */
        rect8 r = { 90, 0, 20, 4, 0, 0, 20, 4 };
        CHECK(clip(100, 4, 200, 4, &r) && r.sx == 90 && r.sw == 10 && r.dw == 10);
    }
    {   /* fully outside */
        rect8 r = { 0, 0, 10, 10, 50, 0, 10, 10 };
        CHECK(!clip(10, 10, 50, 50, &r));
        rect8 z = { 0, 0, 0, 10, 0, 0, 10, 10 };
        CHECK(!clip(10, 10, 50, 50, &z));
    }

    /* RSX_FSR renumbering for the RPCS3 renderer (VKR2Upscaler: 1 FSR 3.1, 2 DLSS, 4 XeSS) */
    {
        int f1 = -1;
        CHECK(rsx_d3d12_fsr_to_rpcs3(3, &f1) == 1 && f1 == 0);
        CHECK(rsx_d3d12_fsr_to_rpcs3(2, &f1) == 2 && f1 == 0);
        CHECK(rsx_d3d12_fsr_to_rpcs3(4, &f1) == 4 && f1 == 0);
        CHECK(rsx_d3d12_fsr_to_rpcs3(1, &f1) == 0 && f1 == 1);
        CHECK(rsx_d3d12_fsr_to_rpcs3(0, &f1) == 0 && f1 == 0);
        CHECK(rsx_d3d12_fsr_to_rpcs3(7, NULL) == 0);
    }

    /* point / wide-line expansion GS I/O, from decompiler-shaped shaders */
    {
        static const char vs[] =
            "struct VSOutput {\n"
            "    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
            "    float4 fog:FOG;\n"
            "    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1;\n"
            "#ifdef RSX_FRAGCOORD_W_IS_RCP\n"
            "    [[vk::builtin(\"PointSize\")]]\n"
            "#endif\n"
            "    float psize:PSIZE;\n"
            "    float4 cd0:SV_ClipDistance0; float2 cd1:SV_ClipDistance1;\n"
            "};\nvoid main() {}\n";
        static const char ps[] =
            "struct PSInput {\n"
            "    float4 position : SV_POSITION; nointerpolation float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
            "    float4 fog : FOG;\n"
            "    centroid float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc8:TEXCOORD8;\n"
            "    bool _rsx_front : SV_IsFrontFace;\n"
            "#ifdef RSX_FRAGCOORD_W_IS_RCP\n"
            "    [[vk::location(14)]] float2 _rsx_pcoord : RSX_PCOORD;\n"
            "#endif\n"
            "};\n"
            "float4 main(PSInput input) : SV_TARGET {\n"
            "#ifdef RSX_FRAGCOORD_W_IS_RCP\n"
            "    input.tc0 = float4(input._rsx_pcoord, 1.0, 0.0);\n"
            "#endif\n"
            "    return input.tc0; }\n";
        char* fx = rsx_d3d12_ps_pcoord_fix(ps);
        CHECK(fx != NULL);
        if (fx) {
            CHECK(!strstr(fx, "vk::location") && !strstr(fx, "RSX_FRAGCOORD_W_IS_RCP"));
            CHECK(strstr(fx, "_rsx_pcoord : RSX_PCOORD") < strstr(fx, "SV_IsFrontFace"));
            CHECK(strstr(fx, "    input.tc0 = float4(input._rsx_pcoord, 1.0, 0.0);\n    return") != NULL);
            static char io[16384];
            const size_t n = rsx_d3d12_gs_io(vs, fx, io, sizeof io);
            CHECK(n > 0 && n == strlen(io));
            CHECK(strstr(io, "nointerpolation float4 col0 : COLOR0;") && strstr(io, "centroid float4 tc0 : TEXCOORD0;"));
            CHECK(strstr(io, "o.tc8 = 0;") && strstr(io, "o.tc1 = v.t1;") && strstr(io, "o.position = v.pos;"));
            CHECK(!strstr(io, "SV_IsFrontFace") && strstr(io, "_rsx_gcd7 : SV_ClipDistance0;"));
            CHECK(strstr(io, "#define GS_PC(o, c) (o)._rsx_pcoord = (c)") && strstr(io, "#define GS_VPSIZE(v) (v).psize"));
            CHECK(strstr(io, "tc8 : TEXCOORD8") < strstr(io, "_rsx_pcoord : RSX_PCOORD"));
            CHECK(rsx_d3d12_gs_io(vs, fx, io, 64) == 0);    /* too small */
            free(fx);
        }
        CHECK(rsx_d3d12_ps_pcoord_fix("float4 main() : SV_TARGET { return 0; }") == NULL);
        static char io2[8192];
        CHECK(rsx_d3d12_gs_io(vs, "struct PSInput {\n    uint s : SV_SampleIndex;\n};\n", io2, sizeof io2) == 0);
        CHECK(rsx_d3d12_gs_io("no struct", ps, io2, sizeof io2) == 0);
        CHECK(rsx_d3d12_gs_body(1) && strstr(rsx_d3d12_gs_body(1), "point VSOutput"));
        CHECK(rsx_d3d12_gs_body(2) && strstr(rsx_d3d12_gs_body(2), "line VSOutput"));
        CHECK(rsx_d3d12_gs_body(0) == NULL);
    /* expanded points / lines face the front for either winding (two-sided colour) */
    CHECK(strstr(rsx_d3d12_gs_body(1), "rsx_gs.w") && strstr(rsx_d3d12_gs_body(2), "rsx_gs.w"));
    }

    /* logic ops as blends */
    {
        D3D12_RENDER_TARGET_BLEND_DESC t;
        memset(&t, 0, sizeof t); t.RenderTargetWriteMask = 15;
        CHECK(rsx_d3d12_logic_blend(0x1500, &t) == 1 && t.BlendEnable && t.SrcBlend == D3D12_BLEND_ZERO && t.DestBlend == D3D12_BLEND_ZERO);
        memset(&t, 0, sizeof t); t.RenderTargetWriteMask = 15;
        CHECK(rsx_d3d12_logic_blend(0x1505, &t) == 1 && t.RenderTargetWriteMask == 0);
        memset(&t, 0, sizeof t); t.BlendEnable = TRUE;
        CHECK(rsx_d3d12_logic_blend(0x1503, &t) == 1 && !t.BlendEnable);
        CHECK(rsx_d3d12_logic_blend(0x1501, &t) == 2 && t.SrcBlend == D3D12_BLEND_DEST_COLOR && t.SrcBlendAlpha == D3D12_BLEND_DEST_ALPHA);
        CHECK(rsx_d3d12_logic_blend(0x1507, &t) == 2 && t.BlendOp == D3D12_BLEND_OP_MAX);
        CHECK(rsx_d3d12_logic_blend(0x1506, &t) == 0);     /* XOR */
    }

    /* 16-bit colour target conversions */
    {
        const uint8_t rgba[8] = { 255, 0, 0, 255, 8, 130, 255, 0 };
        uint8_t p[4], back[8];
        rsx_d3d12_cv_from_rgba(1, rgba, p, 2);
        CHECK(p[0] == 0x00 && p[1] == 0xF8);                /* pure red: 0xF800 */
        rsx_d3d12_cv_to_rgba(1, p, back, 2);
        CHECK(back[0] == 255 && back[1] == 0 && back[3] == 255 && back[7] == 255);
        CHECK(back[4] == 8 && back[5] == 130 && back[6] == 255);
        rsx_d3d12_cv_from_rgba(2, rgba, p, 2);
        CHECK((p[1] & 0x80) && !(p[3] & 0x80));             /* alpha bit */
        rsx_d3d12_cv_to_rgba(2, p, back, 2);
        CHECK(back[0] == 255 && back[3] == 255 && back[7] == 0 && back[6] == 255);
    }

    /* R8 (B8) / R8G8 (G8B8) colour targets in the RGBA8 lanes */
    {
        const uint8_t lanes[8] = { 11, 22, 33, 44, 55, 66, 77, 88 };
        uint8_t p[4], back[8];
        rsx_d3d12_cv_from_rgba(3, lanes, p, 2);
        CHECK(p[0] == 33 && p[1] == 77);                    /* B8 lives in .b */
        rsx_d3d12_cv_to_rgba(3, p, back, 2);
        CHECK(back[0] == 33 && back[1] == 33 && back[2] == 33 && back[3] == 33 && back[7] == 77);
        rsx_d3d12_cv_from_rgba(4, lanes, p, 2);
        CHECK(p[0] == 33 && p[1] == 22 && p[2] == 77 && p[3] == 66);
        rsx_d3d12_cv_to_rgba(4, p, back, 2);
        CHECK(back[0] == 33 && back[1] == 22 && back[2] == 33 && back[3] == 22 && back[4] == 77 && back[5] == 66);
        CHECK(rsx_d3d12_cv_bpp(3) == 1 && rsx_d3d12_cv_bpp(4) == 2 && rsx_d3d12_cv_bpp(1) == 2);
        const UINT id = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        CHECK(rsx_d3d12_cv_map(0, id) == id && rsx_d3d12_cv_map(1, id) == id);
        CHECK(rsx_d3d12_cv_map(3, id) == D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0));
        CHECK(rsx_d3d12_cv_map(4, id) == D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 1, 0, 1));
        /* a guest remap (A,B,G,R + forced 1) composes over the base */
        CHECK(rsx_d3d12_cv_map(4, D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(3, 2, 1, 5)) ==
              D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1, 0, 1, 5));
        const float in[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
        float out[4];
        rsx_d3d12_cv_clear(3, in, out);
        CHECK(out[0] == 0.3f);
        rsx_d3d12_cv_clear(4, in, out);
        CHECK(out[0] == 0.3f && out[1] == 0.2f);
        rsx_d3d12_cv_clear(0, in, out);
        CHECK(out[0] == 0.1f && out[3] == 0.4f);
        CHECK(rsx_d3d12_cv_write_mask(3, 4u) == 0xFu && rsx_d3d12_cv_write_mask(3, 0xBu) == 0u);
        CHECK(rsx_d3d12_cv_write_mask(4, 4u) == 0xDu && rsx_d3d12_cv_write_mask(4, 2u) == 0xEu);
        CHECK(rsx_d3d12_cv_write_mask(4, 9u) == 0u && rsx_d3d12_cv_write_mask(0, 5u) == 5u);
        CHECK(rsx_d3d12_cv_write_mask(1, 1u) == 9u);
    }

    /* render scale (RSX_UPSCALE) */
    {
        CHECK(rsx_d3d12_up_parse(NULL) == 1.0f && rsx_d3d12_up_parse("1") == 1.0f && rsx_d3d12_up_parse("0") == 1.0f);
        CHECK(rsx_d3d12_up_parse("0.5") == 0.5f && rsx_d3d12_up_parse("0.1") == 0.25f && rsx_d3d12_up_parse("x") == 1.0f);
        CHECK(rsx_d3d12_up_dim(1280, 0.5f) == 640 && rsx_d3d12_up_dim(720, 0.667f) == 480 && rsx_d3d12_up_dim(1, 0.25f) == 1);
        CHECK(rsx_d3d12_up_dim(0, 0.5f) == 0 && rsx_d3d12_up_dim(77, 1.0f) == 77);
        uint32_t x = 1, y = 0, w = 1, h = 720;
        rsx_d3d12_up_rect(0.5f, &x, &y, &w, &h);
        CHECK(w == 1 && h == 360 && y == 0);                /* never empty */
        uint32_t ax = 0, ay = 0, aw = 641, ah = 1, bx = 641, by = 0, bw = 639, bh = 1;
        rsx_d3d12_up_rect(0.5f, &ax, &ay, &aw, &ah);
        rsx_d3d12_up_rect(0.5f, &bx, &by, &bw, &bh);
        CHECK(ax + aw == bx && bx + bw == 640);             /* neighbours tile */
        CHECK(rsx_d3d12_up_phases(1.0f) == 8 && rsx_d3d12_up_phases(0.5f) == 32 && rsx_d3d12_up_phases(0.667f) == 18);
        CHECK(rsx_d3d12_up_phases(0.25f) == 64 && rsx_d3d12_up_phases(0.3f) == 64);
        CHECK(rsx_d3d12_up_mip_bias(1.0f) == 0.0f && rsx_d3d12_up_mip_bias(0.5f) == -1.0f && rsx_d3d12_up_mip_bias(0.667f) == -0.5f);
        CHECK(rsx_d3d12_up_xess_quality(1.0f) == 106 && rsx_d3d12_up_xess_quality(1.2f) == 106);
        CHECK(rsx_d3d12_up_xess_quality(1.5f) == 104 && rsx_d3d12_up_xess_quality(2.0f) == 102 && rsx_d3d12_up_xess_quality(4.0f) == 100);
        const char* ps =
            "Texture2D rsx_tex0 : register(t0);\n"
            "TextureCube rsx_tex1 : register(t1);\n"
            "Texture2D rsx_tex5 : register(t5);\n"
            "SamplerState rsx_samp[16] : register(s0);\n"
            "float4 main(float4 a : TEXCOORD0) : SV_Target {\n"
            "  float4 c = rsx_tex0.Sample(rsx_samp[0], (a).xy / (a).w);\n"
            "  c += rsx_tex1.Sample(rsx_samp[1], (a).xyz);\n"
            "  c += rsx_tex5.SampleLevel(rsx_samp[5], f(a.xy, 1), 0.0);\n"
            "  return c; }\n";
        char* r = rsx_d3d12_ps_uv_scale(ps);
        CHECK(r != NULL);
        if (r) {
            CHECK(strstr(r, "SamplerState rsx_samp[16] : register(s0);\ncbuffer D3EUp : register(b3) { float4 d3e_uvk[4]; };\n") != NULL);
            CHECK(strstr(r, "rsx_tex0.Sample(rsx_samp[0], (((a).xy / (a).w) * d3e_uvk[0].x));") != NULL);
            CHECK(strstr(r, "rsx_tex1.Sample(rsx_samp[1], (a).xyz);") != NULL);   /* cube stays */
            CHECK(strstr(r, "rsx_tex5.SampleLevel(rsx_samp[5], ((f(a.xy, 1)) * d3e_uvk[1].y), 0.0);") != NULL);
            free(r);
        }
        CHECK(rsx_d3d12_ps_uv_scale("float4 main() : SV_Target { return 0; }") == NULL);
    }
    {
        /* host MSAA: sample <-> widened pixel, twin scissor, sample counts */
        uint32_t smp = 0;
        CHECK(rsx_d3d12_ms_sample(2, 2, 0, 0) == 0 && rsx_d3d12_ms_sample(2, 2, 1, 0) == 1);
        CHECK(rsx_d3d12_ms_sample(2, 2, 0, 1) == 2 && rsx_d3d12_ms_sample(2, 2, 3, 3) == 3);
        CHECK(rsx_d3d12_ms_sample(2, 1, 5, 7) == 1 && rsx_d3d12_ms_sample(2, 1, 4, 9) == 0);
        CHECK(rsx_d3d12_ms_sample(0, 2, 3, 3) == 0);
        uint32_t x = 3, y = 1, w = 4, h = 2;
        rsx_d3d12_ms_rect(2, 2, &x, &y, &w, &h);
        CHECK(x == 1 && y == 0 && w == 3 && h == 2);        /* [3,7) -> [1,4), [1,3) -> [0,2) */
        x = 0; y = 0; w = 1280; h = 720;
        rsx_d3d12_ms_rect(2, 1, &x, &y, &w, &h);
        CHECK(x == 0 && y == 0 && w == 640 && h == 720);
        x = 5; y = 5; w = 0; h = 0;
        rsx_d3d12_ms_rect(2, 2, &x, &y, &w, &h);
        CHECK(x == 2 && y == 2 && w == 0 && h == 0);
        CHECK(rsx_d3d12_ms_samples(2, 2, &smp) && smp == 4);
        CHECK(rsx_d3d12_ms_samples(2, 1, &smp) && smp == 2);
        CHECK(!rsx_d3d12_ms_samples(1, 1, &smp) && !rsx_d3d12_ms_samples(4, 1, &smp) && !rsx_d3d12_ms_samples(0, 2, &smp));
    }    printf("%s: %d/%d checks passed\n", g_fail ? "FAILED" : "OK", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
