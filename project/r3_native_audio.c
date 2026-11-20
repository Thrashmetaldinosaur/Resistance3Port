/* R3 native audio-synth kernels (port of R2's r2_native_audio.c): switches,
 * verify mode, capture.
 *
 *   R3_NATIVE_AUDIO_JOB=1        run the native kernels (DEFAULT on, as R2:
 *                                bit-exact with the fuzz + mutation suite in
 *                                scratch/native_audio, 0 MISMATCH / 0 missed)
 *                  =0            opt-out: the gate costs one static load per
 *                                call and the lifted code runs unchanged
 *   R3_NATIVE_AUDIO_MASK=0x7F    kernels to replace (bit k = kernel k, see
 *                                r3_native_audio.h); default all
 *   R3_NATIVE_AUDIO_VERIFY=N     verify every Nth call (1 = all): the lifted
 *                                code runs on the real state, the native one
 *                                on a copy, LS + registers are compared and
 *                                mismatches logged as [naudio]. The lifted
 *                                result is kept. Implies JOB=1. Default 4096
 *                                (under JOB=1; 0 = no sampling): a confirmed
 *                                MISMATCH / DECLINE-WROTE (LS is private to
 *                                the SPU thread, so a verified difference is
 *                                deterministic) clears that kernel's mask bit
 *                                -- [naudio] ... DISABLED, lifted from then on.
 *   R3_NATIVE_AUDIO_CAPTURE=N    write the first N calls of each kernel (and,
 *                                with VERIFY, up to 16 mismatches) as R3AUD001
 *                                files for scratch/native_audio/test_native_audio
 *                                (--cap). Implies the verify path for them.
 *   R3_NATIVE_AUDIO_CAPDIR=dir   capture directory (default scratch\audio_capture)
 *   R3_NATIVE_AUDIO_STATS=1      periodic per-kernel counters on stderr
 *
 * R3AUD001 file: 64-byte header {char magic[8]="R3AUD001"; u32 kernel, entry,
 * native_rc, mismatch, seq, ls_size(0x40000), gpr_count(128), reserved[5]},
 * then pre gpr[128] (u128, native word order), pre LS (big-endian image),
 * post gpr, post LS -- the post state is the LIFTED result. */
#include "spu_context.h"
#include "r3_native_audio.h"
#include "port_native_summary.h"   /* port_native_parse_bool: the same switch rules as the [native] line */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <malloc.h>
#define R3NA_MKDIR(p) _mkdir(p)
#define R3NA_AALLOC(n) _aligned_malloc((n), 64)
#else
#include <sys/stat.h>
#define R3NA_MKDIR(p) mkdir((p), 0777)
static void* R3NA_AALLOC(size_t n) { void* p = 0; return posix_memalign(&p, 64, n) ? 0 : p; }
#endif

extern SPU_THREAD_LOCAL uint32_t g_spu_grp_entry;

static volatile int g_r3na_en = -1;     /* -1 = not read yet */
static volatile uint32_t g_r3na_mask = 0x7Fu;   /* a verified mismatch clears a bit */
#ifndef R3NA_HOOKS
#define R3NA_HOOKS 0x7Fu   /* CMake passes the gate calls it finds in the lifted jobmods */
#endif
uint32_t r3na_hooks_present(void) { return (uint32_t)(R3NA_HOOKS) & 0x7Fu; }
static uint32_t g_r3na_verify = 0;      /* every Nth call, 0 = off */
static uint32_t g_r3na_capture = 0;     /* first N calls per kernel */
static int      g_r3na_stats = 0;
static char     g_r3na_capdir[260] = "scratch\\audio_capture";

static SPU_THREAD_LOCAL int t_r3na_guard;   /* lifted reference running: gate off */

/* stats (racy increments are fine: one synth lane, counters are advisory) */
static volatile uint32_t s_calls[R3NA_K_COUNT], s_native[R3NA_K_COUNT], s_declined[R3NA_K_COUNT],
                         s_vok[R3NA_K_COUNT], s_vbad[R3NA_K_COUNT], s_scratch[R3NA_K_COUNT], s_cap[R3NA_K_COUNT];
static volatile uint32_t s_total, s_bad_logged, s_badcap, s_seq;

/* masks / counts: numbers (strtoul base 0) */
static uint32_t envu(const char* n, uint32_t def) {
    const char* e = getenv(n);
    return (e && *e) ? (uint32_t)strtoul(e, 0, 0) : def;
}
/* on/off switches: the [native] line's rules (1/on/true/yes, 0/off/false/no, numbers, else the default) */
static int envb(const char* n, int def) { return port_native_parse_bool(getenv(n), def); }

static void r3na_init(void)
{
    const int job = envb("R3_NATIVE_AUDIO_JOB", 1);
    g_r3na_mask = envu("R3_NATIVE_AUDIO_MASK", 0x7Fu);
    if (job && (g_r3na_mask & ~r3na_hooks_present()))
        fprintf(stderr, "[naudio] kernels 0x%X: hook missing in the lifted jobmods, they stay off "
                        "(regenerate with tools/r3_regen.py)\n", g_r3na_mask & ~r3na_hooks_present());
    g_r3na_mask &= r3na_hooks_present();
    const char* ve = getenv("R3_NATIVE_AUDIO_VERIFY");
    const int verify_set = ve && *ve;
    g_r3na_verify = envu("R3_NATIVE_AUDIO_VERIFY", job ? 4096u : 0u);
    g_r3na_capture = envu("R3_NATIVE_AUDIO_CAPTURE", 0);
    g_r3na_stats = envb("R3_NATIVE_AUDIO_STATS", 0);
    {
        const char* d = getenv("R3_NATIVE_AUDIO_CAPDIR");
        if (d && *d) { strncpy(g_r3na_capdir, d, sizeof g_r3na_capdir - 1); g_r3na_capdir[sizeof g_r3na_capdir - 1] = 0; }
    }
    const int en = (job || (verify_set && g_r3na_verify) || g_r3na_capture) && g_r3na_mask ? 1 : 0;
    if (en)
        fprintf(stderr, "[naudio] native audio kernels ON mask=0x%X verify=%u capture=%u stats=%d\n",
                g_r3na_mask, g_r3na_verify, g_r3na_capture, g_r3na_stats);
    g_r3na_en = en;
}

uint32_t r3na_effective_mask(void)
{
    if (g_r3na_en < 0) r3na_init();
    return g_r3na_en ? (g_r3na_mask & 0x7Fu) : 0u;
}

static void r3na_stats_tick(void)
{
    if (!g_r3na_stats) return;
    const uint32_t t = ++s_total;
    if (t % 50000u) return;
    for (int k = 0; k < R3NA_K_COUNT; k++)
        if (s_calls[k])
            fprintf(stderr, "[naudio] %-16s calls=%u native=%u declined=%u verify ok=%u BAD=%u scratch-diff=%u cap=%u\n",
                    r3na_kernel_name(k), s_calls[k], s_native[k], s_declined[k], s_vok[k], s_vbad[k], s_scratch[k], s_cap[k]);
}

/* ---- verify / capture --------------------------------------------------- */
typedef struct {
    uint8_t* pre_ls;  uint8_t* nat_ls;
    u128*    pre_gpr; u128*    nat_gpr;
} r3na_buf;
static SPU_THREAD_LOCAL r3na_buf t_buf;

static int r3na_buf_ok(void)
{
    if (t_buf.pre_ls) return 1;
    t_buf.pre_ls  = (uint8_t*)R3NA_AALLOC(R3NA_LS_SIZE);
    t_buf.nat_ls  = (uint8_t*)R3NA_AALLOC(R3NA_LS_SIZE);
    t_buf.pre_gpr = (u128*)R3NA_AALLOC(128 * sizeof(u128));
    t_buf.nat_gpr = (u128*)R3NA_AALLOC(128 * sizeof(u128));
    if (!t_buf.pre_ls || !t_buf.nat_ls || !t_buf.pre_gpr || !t_buf.nat_gpr) { t_buf.pre_ls = 0; return 0; }
    return 1;
}

static void r3na_capture(int k, int nrc, int bad, const spu_context* ctx)
{
    static volatile int mk = 0;
    if (!mk) { mk = 1; R3NA_MKDIR(g_r3na_capdir); }
    const uint32_t seq = s_seq++;
    char path[400];
    snprintf(path, sizeof path, "%s\\k%d_%06u%s.bin", g_r3na_capdir, k, seq, bad ? "_bad" : "");
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t h[16];
    memset(h, 0, sizeof h);
    memcpy(h, "R3AUD001", 8);
    h[2] = (uint32_t)k; h[3] = r3na_kernel_entry(k); h[4] = (uint32_t)nrc; h[5] = (uint32_t)bad;
    h[6] = seq; h[7] = R3NA_LS_SIZE; h[8] = 128;
    fwrite(h, 1, 64, f);
    fwrite(t_buf.pre_gpr, sizeof(u128), 128, f);
    fwrite(t_buf.pre_ls, 1, R3NA_LS_SIZE, f);
    fwrite(ctx->gpr, sizeof(u128), 128, f);
    fwrite(ctx->ls, 1, R3NA_LS_SIZE, f);
    fclose(f);
}

static int r3na_first_ls_diff(const uint8_t* a, const uint8_t* b)
{
    for (uint32_t q = 0; q < R3NA_LS_SIZE; q += 4096)
        if (memcmp(a + q, b + q, 4096))
            for (uint32_t i = q; i < q + 4096; i++) if (a[i] != b[i]) return (int)i;
    return -1;
}

static int r3na_verify(spu_context* ctx, int k, void (*lifted)(spu_context*), int want_cap)
{
    if (!r3na_buf_ok()) return 0;
    memcpy(t_buf.pre_ls, ctx->ls, R3NA_LS_SIZE);
    memcpy(t_buf.pre_gpr, ctx->gpr, 128 * sizeof(u128));
    memcpy(t_buf.nat_ls, t_buf.pre_ls, R3NA_LS_SIZE);
    memcpy(t_buf.nat_gpr, t_buf.pre_gpr, 128 * sizeof(u128));
    const int nrc = r3na_kernel_run(k, t_buf.nat_ls, t_buf.nat_gpr);

    /* reference: the lifted routine on the real state, as its brsl call site
     * runs it (host frame + drain to the link register) */
    const uint32_t ret = ctx->gpr[0]._u32[0];
    const int32_t si = (int32_t)ctx->image_id;
    t_r3na_guard = 1;
    ctx->host_depth++;
    lifted(ctx);
    spu_drain_call(ctx, ret);
    ctx->host_depth--;
    spu_img_restore(ctx, si);
    t_r3na_guard = 0;

    int bad = 0, lsd = -1, rd = -1, scratch = 0;
    if (!nrc) {
        s_declined[k]++;
        /* a decline must not have written anything */
        lsd = r3na_first_ls_diff(t_buf.nat_ls, t_buf.pre_ls);
        if (lsd >= 0 || memcmp(t_buf.nat_gpr, t_buf.pre_gpr, 128 * sizeof(u128))) bad = 2;
    } else {
        const uint64_t outm = r3na_kernel_outregs(k);
        lsd = r3na_first_ls_diff(t_buf.nat_ls, ctx->ls);
        if (lsd >= 0) bad = 1;
        for (int r = 0; r < 128; r++) {
            const int must = r < 2 || r >= 80 || (r < 64 && ((outm >> r) & 1));
            if (memcmp(&t_buf.nat_gpr[r], &ctx->gpr[r], sizeof(u128))) {
                if (must) { bad = 1; if (rd < 0) rd = r; }
                else scratch = 1;
            }
        }
        if (bad) s_vbad[k]++; else s_vok[k]++;
        if (scratch) s_scratch[k]++;
    }
    if (bad && s_bad_logged < 10) {
        s_bad_logged++;
        fprintf(stderr, "[naudio] VERIFY %s %s (call %u): first LS diff 0x%05X native=%02X lifted=%02X, first reg diff r%d",
                r3na_kernel_name(k), bad == 2 ? "DECLINE-WROTE" : "MISMATCH", s_calls[k],
                lsd < 0 ? 0 : lsd, lsd < 0 ? 0 : t_buf.nat_ls[lsd], lsd < 0 ? 0 : ctx->ls[lsd], rd);
        if (rd >= 0)
            fprintf(stderr, " native=%08X.%08X.%08X.%08X lifted=%08X.%08X.%08X.%08X",
                    t_buf.nat_gpr[rd]._u32[0], t_buf.nat_gpr[rd]._u32[1], t_buf.nat_gpr[rd]._u32[2], t_buf.nat_gpr[rd]._u32[3],
                    ctx->gpr[rd]._u32[0], ctx->gpr[rd]._u32[1], ctx->gpr[rd]._u32[2], ctx->gpr[rd]._u32[3]);
        fprintf(stderr, " r3=%08X r4=%08X r5=%08X r6=%08X r8=%08X\n",
                t_buf.pre_gpr[3]._u32[0], t_buf.pre_gpr[4]._u32[0], t_buf.pre_gpr[5]._u32[0],
                t_buf.pre_gpr[6]._u32[0], t_buf.pre_gpr[8]._u32[0]);
    }
    if (bad) {
        const uint32_t bit = 1u << k;
        if (__atomic_fetch_and(&g_r3na_mask, ~bit, __ATOMIC_RELAXED) & bit)
            fprintf(stderr, "[naudio] %s DISABLED after a confirmed mismatch: the lifted code runs from now on\n",
                    r3na_kernel_name(k));
    }
    if (want_cap || (bad && g_r3na_capture && s_badcap < 16)) {
        if (bad && !want_cap) s_badcap++;
        s_cap[k]++;
        r3na_capture(k, nrc, bad, ctx);
    }
    return 1;
}

/* ---- the hook entry ----------------------------------------------------- */
int r3na_gate(spu_context* ctx, int k, void (*lifted)(spu_context*))
{
    int en = g_r3na_en;
    if (__builtin_expect(en == 0, 1)) return 0;
    if (en < 0) { r3na_init(); if (!g_r3na_en) return 0; }
    /* a group re-entry (loop back-edge via trampoline) is not a call */
    if (g_spu_grp_entry != 0 || t_r3na_guard) return 0;
    if ((unsigned)k >= R3NA_K_COUNT || !((g_r3na_mask >> k) & 1u)) return 0;
    const uint32_t n = s_calls[k]++;
    r3na_stats_tick();
    const int cap = g_r3na_capture && s_cap[k] < g_r3na_capture;
    if (cap || (g_r3na_verify && n % g_r3na_verify == 0))
        return r3na_verify(ctx, k, lifted, cap);
    const int rc = r3na_kernel_run(k, ctx->ls, ctx->gpr);
    if (rc) s_native[k]++; else s_declined[k]++;
    return rc;
}
