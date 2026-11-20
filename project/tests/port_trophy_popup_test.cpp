// port_trophy_popup.h: rendering, TRP icon reading and the animation curve (one test, each repo's header copy;
// run by scratch\perf_final\run_tests.ps1).
//   port_trophy_popup_test.exe                       checks only
//   port_trophy_popup_test.exe <dir>                 + writes trophy_preview_<grade>.png (drawn cup) to dir
//   port_trophy_popup_test.exe <dir> <TROPHY.TRP> <id> <grade> <name> [...]
//                                                    + trophy_preview_icon_<id>.png with that trophy's own icon
#include "port_trophy_popup.h"
#include <cstdio>
#include <string>

static int s_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

static bool save_png(const ptp::Image& img, const wchar_t* path)
{
    IWICImagingFactory* f = nullptr; IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* pb = nullptr;
    bool ok = false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, &pb)) &&
        SUCCEEDED(fr->Initialize(pb)) && SUCCEEDED(fr->SetSize(img.w, img.h)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
        IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
        SUCCEEDED(fr->WritePixels(img.h, img.w * 4, img.w * img.h * 4, (BYTE*)img.px.data())) &&
        SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit()))
        ok = true;
    if (pb) pb->Release(); if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release(); if (f) f->Release();
    return ok;
}

// The pop-up over a mid-grey game frame, as it would look in game (straight alpha over opaque).
static ptp::Image over_grey(const ptp::Image& img)
{
    ptp::Image o;
    const int m = 24;
    o.w = img.w + 2 * m; o.h = img.h + 2 * m;
    o.px.resize((size_t)o.w * o.h);
    for (int y = 0; y < o.h; y++)
        for (int x = 0; x < o.w; x++) {
            const int v = 70 + (x + y) % 40;   // a little texture so the translucency shows
            o.px[(size_t)y * o.w + x] = 0xFF000000u | v << 16 | (v + 8) << 8 | (v + 20);
        }
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++) {
            const uint32_t p = img.px[(size_t)y * img.w + x];
            ptp::blend(o.px[(size_t)(y + m) * o.w + x + m], { (float)(p >> 16 & 255), (float)(p >> 8 & 255), (float)(p & 255) }, (p >> 24) / 255.f);
        }
    return o;
}

static uint32_t at(const ptp::Image& i, int x, int y) { return i.px[(size_t)y * i.w + x]; }

// A minimal TROPHY.TRP: header, one entry table with TROP.SFM and TROP003.PNG.
static bool write_trp(const wchar_t* path, const std::vector<unsigned char>& png)
{
    auto be32 = [](unsigned char* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (unsigned char)v; };
    auto be64 = [&](unsigned char* p, uint64_t v) { be32(p, (uint32_t)(v >> 32)); be32(p + 4, (uint32_t)v); };
    const uint32_t esz = 64, n = 2;
    const char sfm[] = "<trophyconf></trophyconf>";
    std::vector<unsigned char> d(64 + esz * n);
    be32(&d[0], 0xDCA24D00u); be32(&d[16], n); be32(&d[20], esz);
    const uint64_t off_sfm = d.size(), off_png = off_sfm + sizeof sfm;
    memcpy(&d[64], "TROP.SFM", 8); be64(&d[64 + 32], off_sfm); be64(&d[64 + 40], sizeof sfm);
    memcpy(&d[64 + esz], "TROP003.PNG", 11); be64(&d[64 + esz + 32], off_png); be64(&d[64 + esz + 40], png.size());
    d.insert(d.end(), sfm, sfm + sizeof sfm);
    d.insert(d.end(), png.begin(), png.end());
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    const bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
    fclose(f);
    return ok;
}

int wmain(int argc, wchar_t** argv)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetProcessDPIAware();
    const std::wstring dir = argc > 1 ? argv[1] : L"";
    const char grades[] = { 'B', 'S', 'G', 'P' };
    const wchar_t* names[] = { L"Bronze sample trophy", L"Silver sample trophy", L"Gold sample trophy", L"Platinum sample trophy" };

    // sizes follow the window height; an explicit UI scale multiplies
    CHECK(ptp::panel_height(1080, 0) == 73);
    CHECK(ptp::panel_height(2160, 0) == 147);
    CHECK(ptp::panel_height(480, 0) == 44);
    CHECK(ptp::panel_height(1080, 150) == 109);

    ptp::Rgb cup_mid[4];
    for (int i = 0; i < 4; i++) {
        ptp::Popup p;
        p.line1 = L"You have earned a trophy.";
        p.name = names[i];
        p.grade = grades[i];
        ptp::Image img;
        const int H = ptp::panel_height(1080, 0);
        CHECK(ptp::render(p, H, 0, img));
        CHECK(img.h == H && img.w >= H * 42 / 10 && img.w <= H * 7);
        CHECK(at(img, 0, 0) >> 24 == 0);                       // rounded corner: transparent
        CHECK(at(img, img.w - 1, img.h - 1) >> 24 == 0);
        const uint32_t mid = at(img, img.w - H / 2, H / 2);     // translucent box
        CHECK((mid >> 24) >= 200 && (mid >> 24) < 255);
        // the big cup's bowl, left of its centre (the highlight)
        const int pad = (H * 14 + 50) / 100, icon = H - 2 * pad, inset = icon / 8, cs = icon - 2 * inset;
        const uint32_t c = at(img, pad + inset + cs * 45 / 100, pad + inset + cs * 30 / 100);
        cup_mid[i] = { (float)(c >> 16 & 255), (float)(c >> 8 & 255), (float)(c & 255) };
        // text was drawn: some near-white pixels right of the icon
        int white = 0;
        for (int y = 0; y < H; y++) for (int x = pad + icon; x < img.w; x++) { const uint32_t q = at(img, x, y); if ((q >> 16 & 255) > 200 && (q & 255) > 200) white++; }
        CHECK(white > 30);
        if (!dir.empty()) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%ls\\trophy_preview_%lc.png", dir.c_str(), (wchar_t)grades[i]);
            CHECK(save_png(over_grey(img), path));
        }
    }
    // grade tints: bronze / gold warm, silver neutral, platinum cool
    CHECK(cup_mid[0].r > cup_mid[0].b + 40);
    CHECK(cup_mid[2].r > cup_mid[2].b + 60 && cup_mid[2].g > cup_mid[0].g);
    CHECK(std::fabs(cup_mid[1].r - cup_mid[1].b) < 30);
    CHECK(cup_mid[3].b > cup_mid[3].r);

    // long names are cut, max_w is respected (Resistance 1's OSD sprite limit)
    {
        ptp::Popup p; p.line1 = L"You have earned a trophy."; p.grade = 'G';
        p.name = L"An extremely long trophy name that goes on and on and on and does not stop anywhere soon";
        ptp::Image img;
        CHECK(ptp::render(p, 147, 768, img));
        CHECK(img.w == 768 && img.h == 147);
    }

    // premultiply
    {
        ptp::Image i; i.w = 2; i.h = 1; i.px = { 0x80FF8000u, 0xFF102030u };
        ptp::premultiply(i);
        CHECK(i.px[0] == 0x80804000u && i.px[1] == 0xFF102030u);
    }

    // animation: slides in from the outer edge, fades out at the end
    {
        int dx = 0;
        CHECK(ptp::anim(0, ptp::LIFE_MS, 73, &dx) == 0.f && dx > 0);
        CHECK(ptp::anim(ptp::IN_MS / 2, ptp::LIFE_MS, 73, &dx) > 0.5f && dx > 0);
        CHECK(ptp::anim(2000, ptp::LIFE_MS, 73, &dx) == 1.f && dx == 0);
        const float a = ptp::anim(ptp::LIFE_MS - ptp::OUT_MS / 2, ptp::LIFE_MS, 73, &dx);
        CHECK(a > 0.4f && a < 0.6f && dx == 0);
        CHECK(ptp::anim(ptp::LIFE_MS, ptp::LIFE_MS, 73, &dx) == 0.f);
    }

    // style switch
    {
        _putenv_s("PORT_TROPHY_STYLE", ""); _putenv_s("R9_TROPHY_STYLE", "");
        CHECK(!ptp::style_old("R9"));
        _putenv_s("R9_TROPHY_STYLE", "old");
        CHECK(ptp::style_old("R9"));
        _putenv_s("PORT_TROPHY_STYLE", "ps3");                 // PORT_ wins over the game prefix
        CHECK(!ptp::style_old("R9"));
        _putenv_s("PORT_TROPHY_STYLE", "Classic");
        CHECK(ptp::style_old(nullptr));
        _putenv_s("PORT_TROPHY_STYLE", ""); _putenv_s("R9_TROPHY_STYLE", "");
    }

    // TRP icon: a synthetic package with a 4x4 PNG (red with a transparent pixel) as TROP003.PNG
    {
        wchar_t tmpdir[MAX_PATH], png_path[MAX_PATH], trp_path[MAX_PATH];
        GetTempPathW(MAX_PATH, tmpdir);
        swprintf(png_path, MAX_PATH, L"%lsptp_test_%lu.png", tmpdir, GetCurrentProcessId());
        swprintf(trp_path, MAX_PATH, L"%lsptp_test_%lu.trp", tmpdir, GetCurrentProcessId());
        ptp::Image src; src.w = 4; src.h = 4; src.px.assign(16, 0xFFE02010u); src.px[5] = 0;
        CHECK(save_png(src, png_path));
        std::vector<unsigned char> png;
        if (FILE* f = _wfopen(png_path, L"rb")) {
            unsigned char b[4096]; size_t n;
            while ((n = fread(b, 1, sizeof b, f)) > 0) png.insert(png.end(), b, b + n);
            fclose(f);
        }
        CHECK(!png.empty() && write_trp(trp_path, png));
        char trp_a[MAX_PATH * 2];
        WideCharToMultiByte(CP_ACP, 0, trp_path, -1, trp_a, sizeof trp_a, nullptr, nullptr);
        ptp::Image icon;
        CHECK(ptp::trp_icon(trp_a, 3, icon));
        CHECK(icon.w == 4 && icon.h == 4 && icon.px[0] == 0xFFE02010u && (icon.px[5] >> 24) == 0);
        CHECK(!ptp::trp_icon(trp_a, 4, icon) && icon.empty());   // no such entry
        CHECK(!ptp::trp_icon("no_such_file.trp", 3, icon));
        DeleteFileW(png_path);
        DeleteFileW(trp_path);
    }

    // previews with a trophy's own icon from the user's TROPHY.TRP
    for (int i = 2; i + 3 < argc; i += 4) {
        char trp[MAX_PATH * 2];
        WideCharToMultiByte(CP_ACP, 0, argv[i], -1, trp, sizeof trp, nullptr, nullptr);
        const int id = _wtoi(argv[i + 1]);
        ptp::Popup p;
        p.line1 = L"You have earned a trophy.";
        p.name = argv[i + 3];
        p.grade = (char)argv[i + 2][0];
        if (!ptp::trp_icon(trp, id, p.icon)) { printf("no icon %d in %s\n", id, trp); s_fail++; continue; }
        ptp::Image img;
        CHECK(ptp::render(p, ptp::panel_height(1080, 0), 0, img));
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\trophy_preview_icon_%03d.png", dir.c_str(), id);
        CHECK(save_png(over_grey(img), path));
    }

    printf("port_trophy_popup_test: %s (%d failure%s)\n", s_fail ? "FAILED" : "OK", s_fail, s_fail == 1 ? "" : "s");
    return s_fail ? 1 : 0;
}
