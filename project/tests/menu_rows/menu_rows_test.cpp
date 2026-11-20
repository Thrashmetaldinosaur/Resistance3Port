/* Offline test for project/r3_menurows.cpp (R3: the PC category tabs in the game's Options menu).
 * Models GFx arrays in a host table (GFxValue +8 = array id, type 0x47) on a fake guest memory, and
 * the Options menu's AS (tabs, optionArrays, tab locks, the selected row) behind a fake menu Invoke.
 *   menu_rows_test unit       the tabs: PC DISPLAY / GRAPHICS / EFFECTS / CONTROLS & HUD added once
 *                             before MOVE (a category without rows left out), each with its own list,
 *                             MOVE stays locked, selection kept, registers / caller frame untouched,
 *                             changes applied; queued menu and tabs that will not take fall back to CONTROLS
 *   menu_rows_test frontend   the main menu's Options (tabs CONTROLS, AUDIO, VISUAL, 3D, MOVE; 3D
 *                             locked without a 3D TV): the tabs go in after 3D, 3D and MOVE stay locked
 *   menu_rows_test controls   R3_MENU_ROWS=2: the rows at the end of the CONTROLS list under section headers
 *   menu_rows_test off        R3_MENU_ROWS=0: nothing appended, changes not taken */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <map>
#include <string>
#include <vector>
#include "ppu_recomp.h"

/* ---- fake guest memory ---- */
static const uint32_t kMem = 64u << 20;
extern "C" {
uint8_t* vm_base = nullptr;
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*) = nullptr;
uint32_t vm_read32(uint64_t a) { uint32_t v; memcpy(&v, vm_base + (uint32_t)a, 4); return _byteswap_ulong(v); }
uint64_t vm_read64(uint64_t a) { uint64_t v; memcpy(&v, vm_base + (uint32_t)a, 8); return _byteswap_uint64(v); }
void vm_write32(uint64_t a, uint32_t v) { v = _byteswap_ulong(v); memcpy(vm_base + (uint32_t)a, &v, 4); }
void vm_write64(uint64_t a, uint64_t v) { v = _byteswap_uint64(v); memcpy(vm_base + (uint32_t)a, &v, 8); }
static uint32_t s_heap = 0x02000000u;
uint32_t ps3_hle_heap_alloc(uint32_t size) { const uint32_t a = s_heap; s_heap += (size + 15) & ~15u; return a; }
}

/* ---- fake GFx ---- */
struct Val { int type; double num; std::string str; int arr; };
static std::vector<std::vector<Val>> s_arrays;
static std::vector<int> s_refs;
static int s_bad = 0;
static void clobber(ppu_context* c)
{
    for (int i = 0; i < 13; i++) if (i != 1) c->gpr[i] = 0xDEADBEEF00000000ull + i;
    for (int i = 0; i < 14; i++) c->fpr[i] = -1234.5;
    memset(c->vr, 0x5A, sizeof c->vr[0] * 20);
    c->cr = 0x12345678; c->ctr = 0x777; c->xer = 0x20000000;
    if ((uint32_t)c->gpr[1] & 15) s_bad++;
    vm_write64(c->gpr[1] + 16, 0x1111);            /* a callee saves LR in its caller's frame */
}
static int new_array(void) { s_arrays.push_back({}); s_refs.push_back(1); return (int)s_arrays.size() - 1; }
static void put_array(uint32_t v, int id) { vm_write32(v, 0xABC0); vm_write32(v + 4, 0x47); vm_write32(v + 8, id); vm_write32(v + 12, 0); }
static Val read_val(uint32_t v)
{
    Val x{};
    x.type = vm_read32(v + 4) & 0x8F;
    if (x.type == 3) { const uint64_t b = vm_read64(v + 8); memcpy(&x.num, &b, 8); }
    else if (x.type == 4) x.str = (const char*)vm_base + vm_read32(v + 8);
    else if (x.type == 7) x.arr = (int)vm_read32(v + 8);
    else s_bad++;
    return x;
}
void func_0032F0B4(ppu_context* c)
{
    const uint32_t v = (uint32_t)c->gpr[3];
    if (vm_read32(v) || vm_read32(v + 4)) s_bad++;  /* the caller zeroes +0/+4 */
    put_array(v, new_array());
    clobber(c);
}
void func_009FCAE0(ppu_context* c)
{
    const uint32_t a = (uint32_t)c->gpr[4], v = (uint32_t)c->gpr[5];
    if ((uint32_t)c->gpr[3] != 0xABC0 || a >= s_arrays.size()) { s_bad++; return; }
    Val x = read_val(v);
    if (x.type == 7) s_refs[x.arr]++;
    s_arrays[a].push_back(x);
    clobber(c);
}
void func_009FBC6C(ppu_context* c)
{
    const uint32_t v = (uint32_t)c->gpr[4];
    if ((uint32_t)c->gpr[3] != vm_read32(v) || (uint32_t)c->gpr[5] != vm_read32(v + 8)) s_bad++;
    if ((vm_read32(v + 4) & 0x8F) == 7) s_refs[vm_read32(v + 8)]--;
    clobber(c);
}

/* ---- fake Options menu (lobby.Options AS state) behind the menu Invoke ---- */
static const uint32_t kMenu = 0x00900000u;
struct Ui {
    std::vector<std::string> tabs;                 /* categories.listArray labels */
    int tab_clips = 0;                             /* tabs the categories List shows (its _totalI + 1) */
    std::vector<int> opt;                          /* optionArrays: array ids */
    std::vector<bool> locked;                      /* categories.option<i>.locked */
    int cat = 0, sel = 0, shown = -1;              /* categories._I, options._I, options.listArray */
    bool broken = false;                           /* Array methods through a path do nothing */
    int game_refreshes = 0, fallback_rows = 0;
    std::vector<std::string> calls;
} s_ui;
static int s_move = -1, s_game_rows[4] = { -1, -1, -1, -1 };
static bool s_front = false;                       /* the front end's Options: a 3D tab, locked */
static void ui_show(void)                          /* Options.SetOptionsList */
{
    if (s_ui.cat >= (int)s_ui.opt.size()) return;   /* optionArrays[_I] undefined: shows nothing */
    s_ui.shown = s_ui.opt[s_ui.cat];
    const int n = (int)s_arrays[s_ui.shown].size();
    if (s_ui.sel > n - 1) s_ui.sel = n - 1;        /* List.FixIndex */
}
static void ui_lock_last(bool on) { s_ui.locked[s_ui.opt.size() - 1] = on; }
/* the game's Refresh(rows): fresh optionArrays [CONTROLS, AUDIO, VISUAL, (3D,) MOVE], show, lock MOVE
 * (and 3D in the front end) */
static void game_refresh(int outer)
{
    s_ui.game_refreshes++;
    s_ui.opt.clear();
    for (int i = 0; i < (s_front ? 4 : 3); i++) s_ui.opt.push_back(s_game_rows[i]);
    for (auto& r : s_arrays[outer]) if (r.type == 7 && s_arrays[r.arr].size() > 2 && s_arrays[r.arr][0].num >= 90) s_ui.fallback_rows++;
    s_ui.opt.push_back(s_move);
    ui_show();
    ui_lock_last(true);
    if (s_front) s_ui.locked[3] = true;
}
static void ui_intro(void)
{
    if (s_front) s_ui.tabs = { "CONTROLS", "AUDIO", "VISUAL", "3D", "MOVE" };
    else s_ui.tabs = { "CONTROLS", "AUDIO", "VISUAL", "MOVE" };
    s_ui.tab_clips = (int)s_ui.tabs.size();
    s_ui.locked.assign(12, false);
    s_ui.cat = 0; s_ui.sel = 0;
}
static void result_num(uint32_t r, double d) { if (!r) return; uint64_t b; memcpy(&b, &d, 8); vm_write32(r, 0); vm_write32(r + 4, 3); vm_write64(r + 8, b); }
static void result_str(uint32_t r, const std::string& s)
{
    if (!r) return;
    const uint32_t chars = ps3_hle_heap_alloc((uint32_t)s.size() + 1), node = ps3_hle_heap_alloc(16);
    memcpy(vm_base + chars, s.c_str(), s.size() + 1);
    vm_write32(node, chars);
    vm_write32(r, 0xABC0); vm_write32(r + 4, 0x44); vm_write32(r + 8, node); vm_write32(r + 12, 0);
}
void func_0032D788(ppu_context* c)
{
    const std::string name = (const char*)vm_base + (uint32_t)c->gpr[4];
    const uint32_t args = (uint32_t)c->gpr[5], res = (uint32_t)c->gpr[7];
    const int n = (int)c->gpr[6];
    if ((uint32_t)c->gpr[3] != kMenu || c->fpr[1] != 0.25 || c->gpr[9] || c->gpr[10]) s_bad++;
    if (res && (vm_read32(res) || vm_read32(res + 4))) s_bad++;
    auto arg = [&](int i) { return read_val(args + 16u * (uint32_t)i); };
    s_ui.calls.push_back(name);
    if (name == "categories.listArray.toString" && n == 0) {
        std::string s;
        for (auto& t : s_ui.tabs) s += (s.empty() ? "" : ",") + t + ",1";
        result_str(res, s);
    } else if (name == "optionArrays.push" && n == 0) result_num(res, (double)s_ui.opt.size());
    else if (name == "categories.listArray.push" && n == 0) result_num(res, (double)s_ui.tabs.size());
    else if (name == "categories._I.valueOf" && n == 0) result_num(res, s_ui.cat);
    else if (name == "options._I.valueOf" && n == 0) result_num(res, s_ui.sel);
    else if (name == "categories.CheckLocked" && n == 1) {
        vm_write32(res, 0); vm_write32(res + 4, 2); vm_write32(res + 8, 0); vm_write32(res + 12, 0);
        vm_base[res + 8] = s_ui.locked[(int)arg(0).num] ? 1 : 0;
    } else if (name == "LockPSMoveRelatedOptions" && n == 1) ui_lock_last(arg(0).num != 0);
    else if (name == "categories.listArray.splice" && n == 3) {
        const Val e = arg(2);
        if (arg(0).num != -1 || arg(1).num != 0 || e.type != 7 || s_arrays[e.arr].size() != 2 || s_arrays[e.arr][1].num != 1) s_bad++;
        else if (!s_ui.broken) s_ui.tabs.insert(s_ui.tabs.end() - 1, s_arrays[e.arr][0].str);
    } else if (name == "categories.listArray.concat" && n == 0) {
        if (s_ui.broken) return;                   /* undefined */
        const int a = new_array();
        for (auto& t : s_ui.tabs) s_arrays[a].push_back({ 4, 0, t, 0 });
        put_array(res, a);
    } else if (name == "categories.Refresh" && n == 1) {
        const Val a = arg(0);
        if (a.type != 7) { s_bad++; return; }
        s_ui.tab_clips = (int)s_arrays[a.arr].size();
    } else if (name == "optionArrays.splice" && n == 3) {
        const Val p = arg(2);
        if (arg(0).num != -1 || arg(1).num != 0 || p.type != 7) s_bad++;
        else if (!s_ui.broken) { s_ui.opt.insert(s_ui.opt.end() - 1, p.arr); s_refs[p.arr]++; }
    } else if (name == "SetOptionsList" && n == 0) ui_show();
    else if (name == "options.Set" && n == 1) s_ui.sel = (int)arg(0).num;
    else if (name == "Refresh" && n == 1) game_refresh(arg(0).arr);
    else s_bad++;
    clobber(c);
}

/* ---- fake settings ---- */
struct FOpt { const char* key; std::vector<const char*> ch; int sel; };
static FOpt s_pcm[] = { { "fps", { "30 fps", "60 fps", "120 fps" }, 1 }, { "mouse_sens", { "0.50x", "1.00x" }, 1 },
                        { "fov", { "Original", "Wider" }, 0 },
                        { "aspect", { "Off", "Auto (window)", "21:9", "32:9" }, 0 } };
static FOpt s_pcx[] = { { "fx_dof", { "On", "Off", "Off while aiming" }, 0 }, { "fx_motionblur", { "On", "Off" }, 0 },
                        { "gamma", { "Game default", "Brighter", "Brightest", "Darker", "Darkest" }, 0 } };
static std::map<std::string, std::string> s_ini, s_bat;
static std::vector<std::string> s_sets;
template <size_t N> static int ffind(FOpt (&o)[N], const char* k) { for (size_t i = 0; i < N; i++) if (!strcmp(o[i].key, k)) return (int)i; return -1; }
extern "C" {
int pcm_opt_find(const char* k) { return ffind(s_pcm, k); }
int pcm_opt_nch(int i) { return (int)s_pcm[i].ch.size(); }
int pcm_opt_choice(int i, int k, char* out, int n) { snprintf(out, n, "%s", s_pcm[i].ch[k]); return 1; }
int pcm_opt_sel(int i) { return s_pcm[i].sel; }
void pcm_opt_set(int i, int k) { s_pcm[i].sel = k; s_sets.push_back(std::string("pcm:") + s_pcm[i].key + "=" + std::to_string(k)); }
int pcm_ini_get(const char* key, char* out, int n) { auto it = s_ini.find(key); if (it == s_ini.end()) return 0; snprintf(out, n, "%s", it->second.c_str()); return 1; }
void pcm_ini_set(const char* key, const char* v) { s_ini[key] = v; }
void pcm_bat_set(const char* env, const char* v) { s_bat[env] = v; }
int pcx_opt_find(const char* k) { return ffind(s_pcx, k); }
int pcx_opt_nch(int i) { return (int)s_pcx[i].ch.size(); }
int pcx_opt_choice(int i, int k, char* out, int n) { snprintf(out, n, "%s", s_pcx[i].ch[k]); return 1; }
int pcx_opt_sel(int i) { return s_pcx[i].sel; }
void pcx_opt_set(int i, int k) { s_pcx[i].sel = k; s_sets.push_back(std::string("pcx:") + s_pcx[i].key + "=" + std::to_string(k)); }
uint32_t r3_menu_rows_append(ppu_context* ctx);
void r3_menu_rows_after(ppu_context* ctx, uint32_t menu);
int r3_menu_rows_change(ppu_context* ctx);
}

static int s_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

static const uint32_t kStackTop = 0x01000000u, kName = 0x00800000u, kOuter = 0x00800100u, kArgs = 0x00800200u;
static const uint32_t kHash = 0x011508B8u;

/* the Refresh call: r3 menu, r4 "Refresh", r5 the outer array, r6 1, lr 0x383D74 */
static void make_refresh(ppu_context& c, int outer_id, uint32_t lr = 0x00383D74u, const char* name = "Refresh")
{
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 32; i++) c.gpr[i] = 0x1000 + i;
    for (int i = 0; i < 32; i++) c.fpr[i] = i * 1.5;
    for (int i = 0; i < 32; i++) memset(&c.vr[i], i, 16);
    c.gpr[1] = kStackTop; c.gpr[3] = kMenu; c.gpr[4] = kName; c.gpr[5] = kOuter; c.gpr[6] = 1;
    c.gpr[7] = 0; c.gpr[9] = 0; c.gpr[10] = 0; c.fpr[1] = 0.25; c.lr = lr; c.cr = 0x42; c.ctr = 9; c.xer = 0;
    strcpy((char*)vm_base + kName, name);
    put_array(kOuter, outer_id);
}
static bool regs_equal(const ppu_context& a, const ppu_context& b)
{
    return !memcmp(a.gpr, b.gpr, sizeof a.gpr) && !memcmp(a.fpr, b.fpr, sizeof a.fpr) && !memcmp(a.vr, b.vr, sizeof a.vr) &&
           a.cr == b.cr && a.lr == b.lr && a.ctr == b.ctr && a.xer == b.xer;
}
static void fill_frame(void) { for (uint32_t a = kStackTop; a < kStackTop + 0x100; a++) vm_base[a] = (uint8_t)(a * 7); }
static bool frame_ok(void)
{
    bool ok = true;
    for (uint32_t a = kStackTop; a < kStackTop + 0x100; a++) ok &= vm_base[a] == (uint8_t)(a * 7);
    return ok;
}
static void set_state(uint16_t st) { vm_base[kMenu + 0x40] = (uint8_t)(st >> 8); vm_base[kMenu + 0x41] = (uint8_t)st; }
/* one options rebuild as the hooked wrapper runs it: our entry hook, the game's AS Refresh, our exit hook */
static uint32_t full_refresh(int outer, bool check_regs = true)
{
    fill_frame();
    ppu_context c; make_refresh(c, outer);
    const ppu_context before = c;
    const uint32_t m = r3_menu_rows_append(&c);
    if (check_regs) CHECK(regs_equal(c, before));
    game_refresh(outer);
    if (m) r3_menu_rows_after(&c, m);
    if (check_regs) CHECK(regs_equal(c, before));
    CHECK(frame_ok());
    return m;
}
static int change(int id, double v, uint32_t hash = 0)
{
    ppu_context c; memset(&c, 0, sizeof c);
    c.gpr[1] = kStackTop; c.gpr[5] = hash ? hash : vm_read32(kHash); c.gpr[6] = kArgs; c.gpr[7] = 2;
    uint64_t b; const double did = (double)id;
    vm_write32(kArgs, 0); vm_write32(kArgs + 4, 3); memcpy(&b, &did, 8); vm_write64(kArgs + 8, b);
    vm_write32(kArgs + 16, 0); vm_write32(kArgs + 20, 3); memcpy(&b, &v, 8); vm_write64(kArgs + 24, b);
    return r3_menu_rows_change(&c);
}
static int row_id(const std::vector<Val>& list, const char* label)
{
    for (auto& r : list) if (r.type == 7 && s_arrays[r.arr].size() > 2 && s_arrays[r.arr][2].str == label) return (int)s_arrays[r.arr][0].num;
    return -1;
}
static int count_pc_rows(const std::vector<Val>& list)
{
    int n = 0;
    for (auto& r : list) if (r.type == 7 && !s_arrays[r.arr].empty() && s_arrays[r.arr][0].num >= 90) n++;
    return n;
}
static void check_row(const Val& v)
{
    CHECK(v.type == 7);
    const auto& r = s_arrays[v.arr];
    CHECK(r.size() == 5);
    if (r.size() != 5) return;
    CHECK(r[0].type == 3 && r[0].num >= 90);
    CHECK(r[1].type == 3 && r[1].num == 1.0);
    CHECK(r[2].type == 4 && !r[2].str.empty());
    CHECK(r[3].type == 3);
    CHECK(r[4].type == 7 && !s_arrays[r[4].arr].empty() && r[3].num < s_arrays[r[4].arr].size());
    CHECK(s_refs[v.arr] == 1 && s_refs[r[4].arr] == 1);   /* ours released, the parent's kept */
}
static std::vector<std::string> labels(const std::vector<Val>& o, size_t first = 0)
{
    std::vector<std::string> out;
    for (size_t i = first; i < o.size(); i++) if (o[i].type == 7 && s_arrays[o[i].arr].size() > 2) out.push_back(s_arrays[o[i].arr][2].str);
    return out;
}
/* the fake menus offer fps, fov, aspect, mouse_sens (pcm), fx_dof, fx_motionblur, gamma (pcx) + upscale
 * (local): 4 of the 5 categories (no audio row) */
static const std::vector<std::string> kTabsPc = { "PC DISPLAY", "PC GRAPHICS", "PC EFFECTS", "PC CONTROLS & HUD" };
static const std::vector<std::vector<std::string>> kCatRows = {
    { "Frame rate (restart)", "Field of view", "Ultrawide / aspect ratio (restart)", "Brightness (restart)" },
    { "Upscaling (restart)" },
    { "Depth of field", "Motion blur" },
    { "Mouse sensitivity" },
};
/* the category tabs' lists, optionArrays[first .. first + 3] */
static void check_pc_tabs(size_t first)
{
    CHECK(s_ui.opt.size() >= first + kCatRows.size());
    if (s_ui.opt.size() < first + kCatRows.size()) return;
    for (size_t c = 0; c < kCatRows.size(); c++) {
        const auto& o = s_arrays[s_ui.opt[first + c]];
        for (auto& v : o) check_row(v);
        CHECK(labels(o) == kCatRows[c]);
        for (auto& v : o) CHECK(s_arrays[v.arr][0].num >= 100);   /* no header rows on a tab */
    }
    const auto& d = s_arrays[s_ui.opt[first]];
    CHECK(s_arrays[s_arrays[d[0].arr][4].arr][2].str == "120 fps");
    CHECK(row_id(d, "Ultrawide / aspect ratio (restart)") == row_id(d, "Field of view") + 1);
}
/* the CONTROLS layout: a section header per category, then its rows */
static void check_pc_list(const std::vector<Val>& o, size_t first)
{
    std::vector<std::string> want;
    for (size_t c = 0; c < kCatRows.size(); c++) {
        want.push_back("--- " + kTabsPc[c] + " ---");
        for (auto& l : kCatRows[c]) want.push_back(l);
    }
    CHECK(labels(o, first) == want);
    for (size_t i = first; i < o.size(); i++) check_row(o[i]);
    if (o.size() < first + 2) return;
    CHECK(s_arrays[o[first].arr][0].num == 90);      /* PC DISPLAY's header */
    for (size_t i = first; i < o.size(); i++)                /* no F10 hint in the headers' value */
        if (s_arrays[o[i].arr][0].num >= 90 && s_arrays[o[i].arr][0].num < 100) CHECK(s_arrays[s_arrays[o[i].arr][4].arr][0].str.empty());
    CHECK(row_id(o, "Anti-aliasing (restart)") < 0);      /* not offered by the fake menus: skipped */
    CHECK(row_id(o, "--- PC GRAPHICS ---") == 91 && row_id(o, "--- PC CONTROLS & HUD ---") == 93);
    CHECK(row_id(o, "--- PC AUDIO & LANGUAGE ---") < 0);  /* a category without rows: left out */
}

int main(int argc, char** argv)
{
    vm_base = (uint8_t*)calloc(kMem, 1);
    vm_write32(kHash, 0x5EED1234u);
    const std::string mode = argc > 1 ? argv[1] : "unit";
    const int outer = new_array();
    s_arrays[outer].push_back({ 3, 7.0, "", 0 });     /* a game row stands in */
    s_front = mode == "frontend";
    for (int i = 0; i < 4; i++) { s_game_rows[i] = new_array(); s_arrays[s_game_rows[i]].push_back({ 3, (double)i, "", 0 }); }
    s_move = new_array();
    for (int i = 0; i < 3; i++) s_arrays[s_move].push_back({ 3, 20.0 + i, "", 0 });   /* MOVE: 3 rows */
    ui_intro();
    set_state(3);

    if (mode == "off") {
        CHECK(full_refresh(outer) == 0);
        CHECK(s_arrays[outer].size() == 1 && s_ui.tabs.size() == 4 && s_ui.calls.empty());
        CHECK(change(100, 1.0) == 0);
        CHECK(s_sets.empty());
        printf(s_fail ? "menu_rows off: %d failures\n" : "menu_rows off: ok\n", s_fail);
        return s_fail ? 1 : 0;
    }
    if (mode == "frontend") {
        /* CONTROLS AUDIO VISUAL 3D [4 PC tabs] MOVE */
        CHECK(full_refresh(outer) == kMenu);
        CHECK(s_arrays[outer].size() == 1);
        std::vector<std::string> want = { "CONTROLS", "AUDIO", "VISUAL", "3D" };
        want.insert(want.end(), kTabsPc.begin(), kTabsPc.end());
        want.push_back("MOVE");
        CHECK(s_ui.tabs == want);
        CHECK(s_ui.tab_clips == 9);
        CHECK(s_ui.opt.size() == 9 && s_ui.opt[3] == s_game_rows[3] && s_ui.opt[8] == s_move);
        CHECK(s_ui.locked[3] && s_ui.locked[8]);    /* 3D and MOVE locked, PC open */
        for (int i = 4; i < 8; i++) CHECK(!s_ui.locked[i]);
        check_pc_tabs(4);
        const int id_dof = row_id(s_arrays[s_ui.opt[6]], "Depth of field");
        s_ui.cat = 6; ui_show(); s_ui.sel = 1;
        CHECK(change(id_dof, 1.0) == 1 && s_pcx[0].sel == 1);
        CHECK(full_refresh(outer) == kMenu);
        CHECK(s_ui.tabs.size() == 9 && s_ui.opt.size() == 9 && s_ui.opt[8] == s_move);
        CHECK(s_ui.shown == s_ui.opt[6] && s_ui.sel == 1);   /* still on PC EFFECTS, same row */
        CHECK(s_ui.locked[3] && !s_ui.locked[4] && s_ui.locked[8]);
        ui_intro();                                  /* closed and opened again */
        CHECK(full_refresh(outer) == kMenu);
        CHECK(s_ui.tabs == want && s_ui.locked[8]);
        CHECK(s_bad == 0);
        printf(s_fail ? "menu_rows frontend: %d failures\n" : "menu_rows frontend: ok\n", s_fail);
        return s_fail ? 1 : 0;
    }
    if (mode == "controls") {
        CHECK(full_refresh(outer) == 0);
        CHECK(s_ui.calls.empty() && s_ui.tabs.size() == 4 && s_ui.opt.size() == 4);
        check_pc_list(s_arrays[outer], 1);
        CHECK(s_ui.fallback_rows == 12 && s_bad == 0);   /* 4 section headers + 8 settings */
        CHECK(change(91, 0.0) == 1 && s_sets.empty());    /* a header: swallowed */
        printf(s_fail ? "menu_rows controls: %d failures\n" : "menu_rows controls: ok\n", s_fail);
        return s_fail ? 1 : 0;
    }

    /* not the options builder's Refresh: untouched */
    { ppu_context c; make_refresh(c, outer, 0x00383D78u); CHECK(r3_menu_rows_append(&c) == 0); CHECK(s_arrays[outer].size() == 1); }
    { ppu_context c; make_refresh(c, outer, 0x00383D74u, "AddMessage"); CHECK(r3_menu_rows_append(&c) == 0); CHECK(s_arrays[outer].size() == 1); }
    { ppu_context c; make_refresh(c, outer); c.gpr[6] = 2; CHECK(r3_menu_rows_append(&c) == 0); CHECK(s_arrays[outer].size() == 1); }
    CHECK(s_ui.calls.empty());

    /* the menu opens: the category tabs go in before MOVE, each with its own list */
    CHECK(full_refresh(outer) == kMenu);
    CHECK(s_bad == 0);
    CHECK(s_arrays[outer].size() == 1);              /* nothing in the game's list (CONTROLS) */
    std::vector<std::string> want_tabs = { "CONTROLS", "AUDIO", "VISUAL" };
    want_tabs.insert(want_tabs.end(), kTabsPc.begin(), kTabsPc.end());
    want_tabs.push_back("MOVE");
    CHECK(s_ui.tabs == want_tabs);
    CHECK(s_ui.tab_clips == 8);
    CHECK(s_ui.opt.size() == 8 && s_ui.opt[7] == s_move && s_ui.opt[0] == s_game_rows[0]);
    for (int i = 3; i < 7; i++) CHECK(!s_ui.locked[i]);   /* PC tabs unlocked */
    CHECK(s_ui.locked[7]);                           /* MOVE locked again */
    CHECK(s_ui.shown == s_ui.opt[0]);                /* still on CONTROLS */
    check_pc_tabs(3);
    for (int i = 3; i < 7; i++) CHECK(s_refs[s_ui.opt[i]] == 1);
    const auto& disp = s_arrays[s_ui.opt[3]];
    const int id_dof = row_id(s_arrays[s_ui.opt[5]], "Depth of field"), id_gamma = row_id(disp, "Brightness (restart)");
    const int id_up = row_id(s_arrays[s_ui.opt[4]], "Upscaling (restart)"), id_fps = row_id(disp, "Frame rate (restart)");
    const int id_aspect = row_id(disp, "Ultrawide / aspect ratio (restart)");
    CHECK(id_dof >= 100 && id_gamma >= 100 && id_up >= 100 && id_fps >= 100);

    /* the player goes to PC EFFECTS, row 1, and changes a setting: the game rebuilds the list */
    s_ui.cat = 5; ui_show(); s_ui.sel = 1;
    CHECK(change(id_dof, 2.0) == 1);
    CHECK(s_pcx[0].sel == 2 && s_sets.back() == "pcx:fx_dof=2");
    CHECK(full_refresh(outer) == kMenu);
    CHECK(s_ui.tabs.size() == 8 && s_ui.tab_clips == 8);   /* no second set of tabs */
    CHECK(s_ui.opt.size() == 8 && s_ui.opt[7] == s_move && !s_ui.locked[3] && s_ui.locked[7]);
    CHECK(s_ui.shown == s_ui.opt[5] && s_ui.sel == 1);   /* still on our tab and row */
    for (auto& r : s_arrays[s_ui.opt[5]]) if (s_arrays[r.arr][2].str == "Depth of field") CHECK(s_arrays[r.arr][3].num == 2);
    /* the first PC tab keeps its row too */
    s_ui.cat = 3; ui_show(); s_ui.sel = 3;
    CHECK(full_refresh(outer) == kMenu);
    CHECK(s_ui.shown == s_ui.opt[3] && s_ui.sel == 3);
    CHECK(s_bad == 0);

    /* changes */
    CHECK(change(id_fps, 0.0) == 1 && s_pcm[0].sel == 0);
    const size_t nsets = s_sets.size();
    CHECK(change(id_fps, 0.0) == 1 && s_sets.size() == nsets);   /* same value: nothing written */
    CHECK(change(id_fps, 9.0) == 1 && s_pcm[0].sel == 0);        /* out of range: ignored */
    /* brightness goes through the More PC Options row (the F10 menu's own state), not ini / bat directly */
    CHECK(change(id_gamma, 2.0) == 1 && s_pcx[2].sel == 2 && s_sets.back() == "pcx:gamma=2" && !s_ini.count("gamma"));
    CHECK(change(id_gamma, 0.0) == 1 && s_pcx[2].sel == 0 && s_sets.back() == "pcx:gamma=0" && !s_bat.count("PS3_DISPLAY_GAMMA"));
    s_pcx[2].sel = 4;                                /* changed in F10: the row shows it */
    CHECK(full_refresh(outer) == kMenu);
    for (auto& r : s_arrays[s_ui.opt[3]]) if (s_arrays[r.arr][2].str == "Brightness (restart)") CHECK(s_arrays[r.arr][3].num == 4);
    s_pcx[2].sel = 0;
    s_ini["fsr"] = "0";
    CHECK(change(id_up, 3.0) == 1 && s_ini["upscale"] == "0.5" && s_bat["RSX_UPSCALE"] == "1");   /* no upscaler: native */
    s_ini["fsr"] = "2";
    CHECK(change(id_up, 1.0) == 1 && s_ini["upscale"] == "0.667" && s_bat["RSX_UPSCALE"] == "0.667");
    CHECK(change(id_aspect, 2.0) == 1 && s_pcm[3].sel == 2 && s_sets.back() == "pcm:aspect=2");
    CHECK(change(90, 0.0) == 1 && change(94, 0.0) == 1);   /* section headers: swallowed */
    CHECK(change(99, 0.0) == 0);                     /* no such row any more */
    CHECK(change(5, 1.0) == 0);                      /* a game row */
    CHECK(change(id_dof, 1.0, 0x0BADu) == 0 && s_pcx[0].sel == 2);   /* another method */

    /* the menu is closed and opened again (Intro resets the tabs): the tabs come back once */
    ui_intro();
    CHECK(full_refresh(outer) == kMenu);
    CHECK(s_ui.tabs == want_tabs && s_ui.locked[7] && !s_ui.locked[3] && !s_ui.locked[6]);
    for (int i = 3; i < 7; i++) for (auto& r : s_arrays[s_ui.opt[i]]) {
        const auto& a = s_arrays[r.arr];
        if (a[2].str == "Upscaling (restart)") CHECK(a[3].num == 1);
        if (a[2].str == "Brightness (restart)") CHECK(a[3].num == 0);
        if (a[2].str == "Ultrawide / aspect ratio (restart)") CHECK(a[3].num == 2);
    }

    /* the menu still queues its calls: the rows go to CONTROLS this time, nothing is invoked */
    {
        set_state(2);
        const int q = new_array();
        const size_t ncalls = s_ui.calls.size();
        ppu_context c; make_refresh(c, q);
        CHECK(r3_menu_rows_append(&c) == 0);
        CHECK(s_ui.calls.size() == ncalls);
        check_pc_list(s_arrays[q], 0);
        set_state(3);
    }

    /* Array methods through a path do nothing: fall back to the CONTROLS rows, now and from then on */
    {
        ui_intro();
        s_ui.broken = true;
        const int f = new_array();
        s_arrays[f].push_back({ 3, 7.0, "", 0 });
        const int before = s_ui.game_refreshes;
        CHECK(full_refresh(f) == kMenu);
        CHECK(s_ui.game_refreshes == before + 2);    /* the game's, then ours with the rows */
        CHECK(s_ui.tabs.size() == 4 && s_ui.locked[3]);   /* MOVE locked as the game left it */
        check_pc_list(s_arrays[f], 1);
        const int g = new_array();
        const size_t ncalls = s_ui.calls.size();
        CHECK(full_refresh(g) == 0);
        CHECK(s_ui.calls.size() == ncalls);
        check_pc_list(s_arrays[g], 0);
    }
    CHECK(s_bad == 0);
    printf(s_fail ? "menu_rows unit: %d failures\n" : "menu_rows unit: ok\n", s_fail);
    return s_fail ? 1 : 0;
}
