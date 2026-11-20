/* R3 native SPU job kernels: switches, engage log, runtime self-check with
 * automatic fallback (R2's r2_native_jobs.c scheme, without the profiler).
 * Switches documented in r3_native_jobs.h. */
#include "spu_context.h"
#include "r3_native_jobs.h"
#include "native_edge_zlib.h"
#include "port_native_summary.h"   /* port_native_parse_bool: the same switch rules as the [native] line */
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#define R3NJ_AALLOC(n) _aligned_malloc((n), 64)
#else
static void* R3NJ_AALLOC(size_t n) { void* p = 0; return posix_memalign(&p, 64, n) ? 0 : p; }
#endif

extern SPU_THREAD_LOCAL uint32_t g_spu_grp_entry;

#define R3NJ_DEFAULT_MASK 0xFu      /* kernels proven byte-exact offline AND on game data (0-3) */
#define R3NJ_GEOM_BIT (1u << R3NJ_K_GEOM_XFORM4)   /* fuzz-verified only: opt in (R3_NATIVE_JOBS_GEOM=1) */
#define R3NJ_FIRST_CHECKS 8u
/* bit k: the lifted code of this build has kernel k's gate call (CMake scans
 * the lifted sources; the tools/hooks/spu JSON files, applied by tools/r3_regen.py).
 * Without its hook a kernel never runs, so it reports off. */
#ifndef R3NJ_HOOKS
#define R3NJ_HOOKS 0xFu
#endif

static volatile int g_r3nj_en = -1;          /* -1 = not read yet */
static uint32_t g_r3nj_mask = R3NJ_DEFAULT_MASK;
static uint32_t g_r3nj_off_env;              /* bit k: forced off by its own old switch */
static uint32_t g_r3nj_unbuilt;              /* bit k: kernel compiled out (no AVX2): always declines */
static uint32_t g_r3nj_check = 4096;          /* every Nth call (after the first few) */
static int      g_r3nj_stats = 0;
static volatile uint32_t g_r3nj_dead;         /* bit k: kernel k disabled after a mismatch */

static SPU_THREAD_LOCAL int t_r3nj_guard;    /* lifted reference running: gate off */

static volatile uint32_t s_calls[R3NJ_K_COUNT], s_native[R3NJ_K_COUNT], s_declined[R3NJ_K_COUNT],
                         s_vok[R3NJ_K_COUNT], s_vbad[R3NJ_K_COUNT], s_logged[R3NJ_K_COUNT],
                         s_races[R3NJ_K_COUNT];
static volatile uint32_t s_total;

/* ---- kernels ------------------------------------------------------------- */
/* spuelf0 0x8100 -- Edge zlib inflate_fast (exact emulation in
 * native_edge_zlib.c; verified by scratch\edge_zlib_native). Writes LS only;
 * the lifted routine restores every register its caller reads. */
int r3nj_edge_zlib(uint8_t* ls, u128* gpr)
{
    if (!native_edge_zlib_ls_matches(ls, &native_edge_zlib_r3)) return 0;
    native_edge_zlib_inflate_fast_ls(ls, gpr[3]._u32[0], gpr[4]._u32[0], &native_edge_zlib_r3);
    return 1;
}

#ifndef R3NJ_EXTERNAL_KERNEL_RUN   /* the harness supplies its own (bug injection) */
int r3nj_kernel_run(int k, uint8_t* ls, u128* gpr)
{
    switch (k) {
    case R3NJ_K_EDGE_ZLIB: return r3nj_edge_zlib(ls, gpr);
    case R3NJ_K_BINK_IDCT_PUT: return r3nj_bink_idct_put(ls, gpr);
    case R3NJ_K_BINK_IDCT_PUT2X: return r3nj_bink_idct_put2x(ls, gpr);
    case R3NJ_K_BINK_IDCT_ADD: return r3nj_bink_idct_add(ls, gpr);
    case R3NJ_K_GEOM_XFORM4: return r3nj_geom_xform4(ls, gpr);
    default: return 0;
    }
}
#endif
const char* r3nj_kernel_name(int k)
{
    static const char* const n[R3NJ_K_COUNT] = { "edge_zlib", "bink_idct_put", "bink_idct_put2x", "bink_idct_add", "geom_xform4" };
    return (unsigned)k < R3NJ_K_COUNT ? n[k] : "?";
}
uint32_t r3nj_kernel_entry(int k)
{
    static const uint32_t e[R3NJ_K_COUNT] = { 0x8100u, 0xC288u, 0xC8C0u, 0xCF30u, 0x120u };
    return (unsigned)k < R3NJ_K_COUNT ? e[k] : 0;
}

/* ---- switches ------------------------------------------------------------ */
/* masks / counts: numbers (strtoul base 0) */
static uint32_t envu(const char* n, uint32_t def)
{
    const char* e = getenv(n);
    return (e && *e) ? (uint32_t)strtoul(e, 0, 0) : def;
}
/* on/off switches: the [native] line's rules (1/on/true/yes, 0/off/false/no, numbers, else the default) */
static int envb(const char* n, int def) { return port_native_parse_bool(getenv(n), def); }
static uint32_t r3nj_default_mask(void)
{ return R3NJ_DEFAULT_MASK | (envb("R3_NATIVE_JOBS_GEOM", 0) ? R3NJ_GEOM_BIT : 0u); }

static void r3nj_init(void)
{
    const int on = envb("R3_NATIVE_JOBS", 1);
    g_r3nj_mask = envu("R3_NATIVE_JOBS_MASK", r3nj_default_mask());
    g_r3nj_check = envu("R3_NATIVE_JOBS_CHECK", 4096);
    if (envb("R3_NATIVE_JOBS_VERIFY", 0)) g_r3nj_check = 1;
    g_r3nj_stats = envb("R3_NATIVE_JOBS_STATS", 0);
    /* the zlib kernel's original switch still turns it off */
    if (!envb("PORT_NATIVE_EDGE_ZLIB", 1)) g_r3nj_off_env |= 1u << R3NJ_K_EDGE_ZLIB;
    if (!r3nj_bink_built())
        g_r3nj_unbuilt = (1u << R3NJ_K_BINK_IDCT_PUT) | (1u << R3NJ_K_BINK_IDCT_PUT2X) | (1u << R3NJ_K_BINK_IDCT_ADD);
    if (!r3nj_geom_built()) g_r3nj_unbuilt |= R3NJ_GEOM_BIT;
    if (on)
        for (int k = 0; k < R3NJ_K_COUNT; k++)
            if (((g_r3nj_mask & ~g_r3nj_off_env) >> k) & 1u && !((R3NJ_HOOKS >> k) & 1u))
                fprintf(stderr, "[native-jobs] %s (LS 0x%X) native OFF: hook missing in the lifted code "
                                "(regenerate with tools/r3_regen.py)\n", r3nj_kernel_name(k), r3nj_kernel_entry(k));
    g_r3nj_mask &= ~g_r3nj_off_env & ~g_r3nj_unbuilt & (uint32_t)(R3NJ_HOOKS);
    g_r3nj_en = (on && g_r3nj_mask) ? 1 : 0;
}

int r3nj_kernel_active(int k)
{
    if (g_r3nj_en < 0) r3nj_init();
    return g_r3nj_en && (unsigned)k < R3NJ_K_COUNT && ((g_r3nj_mask >> k) & 1u) && !((g_r3nj_dead >> k) & 1u);
}
const char* r3nj_kernel_why_off(int k)
{
    if (r3nj_kernel_active(k)) return 0;
    if ((unsigned)k >= R3NJ_K_COUNT) return "no such kernel";
    if ((g_r3nj_off_env >> k) & 1u) return "own switch";
    if (!envb("R3_NATIVE_JOBS", 1)) return "R3_NATIVE_JOBS=0";
    if (!((envu("R3_NATIVE_JOBS_MASK", r3nj_default_mask()) >> k) & 1u))
        return k == R3NJ_K_GEOM_XFORM4 ? "off by default (R3_NATIVE_JOBS_GEOM=1)" : "R3_NATIVE_JOBS_MASK";
    if (!((R3NJ_HOOKS >> k) & 1u)) return "hook missing";
    if ((g_r3nj_unbuilt >> k) & 1u) return "needs an AVX2 build";
    return "disabled by self-check";
}
uint32_t r3nj_effective_mask(void)
{
    uint32_t m = 0;
    for (int k = 0; k < R3NJ_K_COUNT; k++)
        if (r3nj_kernel_active(k)) m |= 1u << k;
    return m;
}
void r3nj_kernel_counts(int k, uint32_t out[5])
{
    out[0] = s_calls[k]; out[1] = s_native[k]; out[2] = s_vok[k]; out[3] = s_vbad[k]; out[4] = s_races[k];
}

static void r3nj_stats_tick(void)
{
    if (!g_r3nj_stats) return;
    const uint32_t t = ++s_total;
    if (t % 20000u) return;
    for (int k = 0; k < R3NJ_K_COUNT; k++)
        if (s_calls[k])
            fprintf(stderr, "[native-jobs] %-14s calls=%u native=%u declined=%u check ok=%u BAD=%u races=%u%s\n",
                    r3nj_kernel_name(k), s_calls[k], s_native[k], s_declined[k], s_vok[k], s_vbad[k], s_races[k],
                    (g_r3nj_dead >> k) & 1u ? " (disabled)" : "");
}

/* ---- self-check ---------------------------------------------------------- */
typedef struct { uint8_t* pre_ls; uint8_t* nat_ls; uint8_t* rep_ls; u128* pre_gpr; u128* nat_gpr; } r3nj_buf;
static SPU_THREAD_LOCAL r3nj_buf t_buf;

static int r3nj_buf_ok(void)
{
    if (t_buf.pre_ls) return 1;
    t_buf.pre_ls  = (uint8_t*)R3NJ_AALLOC(R3NJ_LS_SIZE);
    t_buf.nat_ls  = (uint8_t*)R3NJ_AALLOC(R3NJ_LS_SIZE);
    t_buf.rep_ls  = (uint8_t*)R3NJ_AALLOC(R3NJ_LS_SIZE);
    t_buf.pre_gpr = (u128*)R3NJ_AALLOC(128 * sizeof(u128));
    t_buf.nat_gpr = (u128*)R3NJ_AALLOC(128 * sizeof(u128));
    if (!t_buf.pre_ls || !t_buf.nat_ls || !t_buf.rep_ls || !t_buf.pre_gpr || !t_buf.nat_gpr) { t_buf.pre_ls = 0; return 0; }
    return 1;
}

static int r3nj_first_ls_diff(const uint8_t* a, const uint8_t* b)
{
    for (uint32_t q = 0; q < R3NJ_LS_SIZE; q += 4096)
        if (memcmp(a + q, b + q, 4096))
            for (uint32_t i = q; i < q + 4096; i++) if (a[i] != b[i]) return (int)i;
    return -1;
}
/* registers a kernel must reproduce: r0/r1 and r80..r127 (non-volatile) */
static int r3nj_reg_diff(const u128* a, const u128* b)
{
    for (int r = 0; r < 128; r++)
        if ((r < 2 || r >= 80) && memcmp(&a[r], &b[r], sizeof(u128))) return r;
    return -1;
}

/* the lifted routine as its brsl call site runs it (host frame + drain to
 * the link register), with the gate off */
static void r3nj_run_lifted(spu_context* ctx, void (*lifted)(spu_context*))
{
    const uint32_t ret = ctx->gpr[0]._u32[0];
    const int32_t si = (int32_t)ctx->image_id;
    t_r3nj_guard = 1;
    ctx->host_depth++;
    lifted(ctx);
    spu_drain_call(ctx, ret);
    ctx->host_depth--;
    spu_img_restore(ctx, si);
    t_r3nj_guard = 0;
}

/* A sampled call whose native result differs from the lifted one: run both
 * once more on copies of the same input (pre). 1 = confirmed (each side
 * repeats its own result: a kernel bug), 0 = a side did not repeat (its input
 * changed under it, e.g. a concurrent LS writer: race). The lifted repeat runs
 * on a private clone of the context whose ls is rep_ls: the live context is
 * never touched, so peers that write its LS meanwhile (spu_ls_window_transfer,
 * sys_spu_thread_write_ls) still land there. */
static SPU_THREAD_LOCAL spu_context* t_clone;

static int r3nj_confirm(spu_context* ctx, int k, void (*lifted)(spu_context*), int nrc1)
{
    if (!t_clone && !(t_clone = (spu_context*)R3NJ_AALLOC(sizeof(spu_context)))) return 0;
    spu_context* const c = t_clone;
    memcpy(c, ctx, offsetof(spu_context, ls_store));
    memcpy((uint8_t*)c + offsetof(spu_context, ls), (const uint8_t*)ctx + offsetof(spu_context, ls),
           sizeof(spu_context) - offsetof(spu_context, ls));
    memcpy(t_buf.rep_ls, t_buf.pre_ls, R3NJ_LS_SIZE);
    memcpy(c->gpr, t_buf.pre_gpr, 128 * sizeof(u128));
    c->ls = t_buf.rep_ls;
    c->int_enable = 0;   /* no interrupt handler on the copy */
    r3nj_run_lifted(c, lifted);
    const int lif_same = r3nj_first_ls_diff(t_buf.rep_ls, ctx->ls) < 0 && r3nj_reg_diff(c->gpr, ctx->gpr) < 0;
    const int nrc2 = r3nj_kernel_run(k, t_buf.pre_ls, t_buf.pre_gpr);
    const int nat_same = nrc2 == nrc1 && r3nj_first_ls_diff(t_buf.pre_ls, t_buf.nat_ls) < 0 &&
                         r3nj_reg_diff(t_buf.pre_gpr, t_buf.nat_gpr) < 0;
    return lif_same && nat_same;
}

/* 0 = could not check (caller runs native normally), 1 = checked: the lifted
 * routine already ran on the real state */
static int r3nj_check(spu_context* ctx, int k, void (*lifted)(spu_context*))
{
    if (!r3nj_buf_ok()) return 0;
    memcpy(t_buf.pre_ls, ctx->ls, R3NJ_LS_SIZE);
    memcpy(t_buf.pre_gpr, ctx->gpr, 128 * sizeof(u128));
    memcpy(t_buf.nat_ls, t_buf.pre_ls, R3NJ_LS_SIZE);
    memcpy(t_buf.nat_gpr, t_buf.pre_gpr, 128 * sizeof(u128));
    const int nrc = r3nj_kernel_run(k, t_buf.nat_ls, t_buf.nat_gpr);
    r3nj_run_lifted(ctx, lifted);

    int bad = 0, lsd = -1, rd = -1;
    if (!nrc) {
        s_declined[k]++;
        lsd = r3nj_first_ls_diff(t_buf.nat_ls, t_buf.pre_ls);
        if (lsd >= 0 || memcmp(t_buf.nat_gpr, t_buf.pre_gpr, 128 * sizeof(u128))) bad = 2;
    } else {
        lsd = r3nj_first_ls_diff(t_buf.nat_ls, ctx->ls);
        if (lsd >= 0) bad = 1;
        else if ((rd = r3nj_reg_diff(t_buf.nat_gpr, ctx->gpr)) >= 0) bad = 1;
    }
    if (bad && !r3nj_confirm(ctx, k, lifted, nrc)) {
        if (!s_races[k]++)
            fprintf(stderr, "[native-jobs] %s self-check differed on call %u but did not repeat (input changed "
                            "during the check: race, not a kernel bug); kernel stays on\n",
                    r3nj_kernel_name(k), s_calls[k]);
        return 1;
    }
    if (bad) {
        s_vbad[k]++;
        g_r3nj_dead |= 1u << k;
        if (!s_logged[k]++)
            fprintf(stderr, "[native-jobs] %s %s on call %u (first LS diff 0x%05X native=%02X lifted=%02X, reg r%d; "
                            "r3=%08X r4=%08X; repeated): kernel DISABLED, lifted code from now on\n",
                    r3nj_kernel_name(k), bad == 2 ? "declined but wrote" : "MISMATCH", s_calls[k],
                    lsd < 0 ? 0 : lsd, lsd < 0 ? 0 : t_buf.nat_ls[lsd], lsd < 0 ? 0 : ctx->ls[lsd], rd,
                    t_buf.pre_gpr[3]._u32[0], t_buf.pre_gpr[4]._u32[0]);
    } else if (nrc) {
        s_vok[k]++;
    }
    return 1;
}

/* ---- the hook entry ------------------------------------------------------ */
int r3nj_gate(spu_context* ctx, int k, void (*lifted)(spu_context*))
{
    int en = g_r3nj_en;
    if (__builtin_expect(en == 0, 0)) return 0;
    if (en < 0) { r3nj_init(); if (!g_r3nj_en) return 0; }
    /* a group re-entry (loop back-edge via trampoline) is not a call */
    if (g_spu_grp_entry != 0 || t_r3nj_guard) return 0;
    if ((unsigned)k >= R3NJ_K_COUNT) return 0;
    /* a pending SPU interrupt is taken by the lifted code's first check */
    if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) return 0;
    if (!((g_r3nj_mask >> k) & 1u) || ((g_r3nj_dead >> k) & 1u)) return 0;
    const uint32_t n = s_calls[k]++;
    r3nj_stats_tick();
    if (n == 0)
        fprintf(stderr, "[native-jobs] %s (LS 0x%X) native ON (self-check: first %u calls, then 1/%u)\n",
                r3nj_kernel_name(k), r3nj_kernel_entry(k), R3NJ_FIRST_CHECKS, g_r3nj_check);
    if (g_r3nj_check && (n < R3NJ_FIRST_CHECKS || n % g_r3nj_check == 0))
        if (r3nj_check(ctx, k, lifted)) return 1;
    const int rc = r3nj_kernel_run(k, ctx->ls, ctx->gpr);
    if (rc) s_native[k]++; else s_declined[k]++;
    return rc;
}
