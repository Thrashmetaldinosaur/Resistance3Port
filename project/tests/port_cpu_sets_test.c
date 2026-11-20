/* port_cpu_sets_test.c -- the P-core selection of project/port_cpu_sets.h (identical in all three ports;
 * scratch\perf_final\run_tests.ps1 builds this against each repo's copy, as C and as C++) over mocked
 * GetSystemCpuSetInformation buffers: hybrid, homogeneous, multi-CCD, multi-group (> 64 CPUs), small hybrid
 * laptops (2P + 4E, 2P + 8E with the core keeper's rest mask) and affinity-restricted processes, and the
 * hot-thread budget by usable P-cores. */
#include <windows.h>
#include <stdio.h>
#include "port_cpu_sets.h"

static int s_fail, s_n;
static void check(const char* what, long long got, long long want)
{
    s_n++;
    if (got != want) { s_fail++; printf("FAIL %s: got %lld, want %lld\n", what, got, want); }
}
static void check_str(const char* what, const char* got, const char* want)
{
    s_n++;
    if ((got == NULL) != (want == NULL) || (got && strcmp(got, want))) {
        s_fail++; printf("FAIL %s: got %s, want %s\n", what, got ? got : "(null)", want ? want : "(null)");
    }
}

static SYSTEM_CPU_SET_INFORMATION s_buf[512];
static int s_nb;
static void add(unsigned id, unsigned group, unsigned lp, unsigned core, unsigned llc, unsigned eff, unsigned alloc)
{
    SYSTEM_CPU_SET_INFORMATION* c = &s_buf[s_nb++];
    memset(c, 0, sizeof *c);
    c->Size = sizeof *c;
    c->Type = CpuSetInformation;
    c->CpuSet.Id = id;
    c->CpuSet.Group = (WORD)group;
    c->CpuSet.LogicalProcessorIndex = (BYTE)lp;
    c->CpuSet.CoreIndex = (BYTE)core;
    c->CpuSet.LastLevelCacheIndex = (BYTE)llc;
    c->CpuSet.EfficiencyClass = (BYTE)eff;
    c->CpuSet.Allocated = (alloc & 1) != 0;
    c->CpuSet.AllocatedToTargetProcess = (alloc & 2) != 0;
}
static void build(unsigned short group, unsigned long long aff, port_pcore_plan* p)
{
    port_pcore_plan_build(s_buf, (unsigned long)(s_nb * sizeof s_buf[0]), group, aff, p);
}

/* p P-cores with SMT (class 1) then e E-cores (class 0), logical CPUs in that order (Intel laptop parts). */
static void mock_small(unsigned np, unsigned ne)
{
    s_nb = 0;
    for (unsigned c = 0; c < np; c++)
        for (unsigned t = 0; t < 2; t++) add(0x100 + c * 2 + t, 0, c * 2 + t, c * 2, 0, 1, 0);
    for (unsigned e = 0; e < ne; e++) add(0x100 + np * 2 + e, 0, np * 2 + e, np * 2 + e, 0, 0, 0);
}
/* hot roles a plan admits, as a bit string: main fifo render raw0 raw1 lane0 lane2 lane3 audio */
static unsigned roles(const port_pcore_plan* p)
{
    return (unsigned)port_pcore_role_ok(p, PORT_PCORE_MAIN, 0) << 8 | (unsigned)port_pcore_role_ok(p, PORT_PCORE_FIFO, 0) << 7 |
           (unsigned)port_pcore_role_ok(p, PORT_PCORE_RENDER, 0) << 6 | (unsigned)port_pcore_role_ok(p, PORT_PCORE_RAW, 0) << 5 |
           (unsigned)port_pcore_role_ok(p, PORT_PCORE_RAW, 1) << 4 | (unsigned)port_pcore_role_ok(p, PORT_PCORE_LANE, 0) << 3 |
           (unsigned)port_pcore_role_ok(p, PORT_PCORE_LANE, 2) << 2 | (unsigned)port_pcore_role_ok(p, PORT_PCORE_LANE, 3) << 1 |
           (unsigned)port_pcore_role_ok(p, PORT_PCORE_AUDIO, 0);
}

/* Intel 12900K-like: 8 P-cores with SMT (LP 0..15, class 1) + 8 E-cores (LP 16..23, class 0). CPU set ids
 * start at 0x100 as on real systems. */
static void mock_hybrid(void)
{
    s_nb = 0;
    for (unsigned c = 0; c < 8; c++)
        for (unsigned t = 0; t < 2; t++) add(0x100 + c * 2 + t, 0, c * 2 + t, c * 2, 0, 1, 0);
    for (unsigned e = 0; e < 8; e++) add(0x110 + e, 0, 16 + e, 16 + e, 0, 0, 0);
}

int main(void)
{
    port_pcore_plan p;
    char buf[512];

    /* hybrid: the 16 P-core logical CPUs, ids in order */
    mock_hybrid();
    build(0, ~0ull, &p);
    check("hybrid.hybrid", p.hybrid, 1);
    check("hybrid.pcores", p.pcores, 8);
    check("hybrid.ecores", p.ecores, 8);
    check("hybrid.n", p.n, 16);
    check("hybrid.usable", p.usable, 8);
    check("hybrid.roles", roles(&p), 0x1FF);
    check("hybrid.id0", p.ids[0], 0x100);
    check("hybrid.id15", p.ids[15], 0x10F);
    check_str("hybrid.why", p.why_off, NULL);
    check("hybrid.llc", p.llc, 1);
    check("hybrid.groups", p.groups, 1);

    /* hybrid, affinity = P-core 0 + E-cores only: one usable P-core -- placing every hot thread on it would crowd
     * it, so nothing is placed... */
    build(0, 0x3ull | 0xFF0000ull, &p);
    check("hybrid-aff.n", p.n, 2);
    check("hybrid-aff.usable", p.usable, 1);
    check_str("hybrid-aff.why", p.why_off, "fewer than 2 usable P-cores");
    /* ...affinity-restricted process (start /affinity: 4 P-cores + the E-cores): no raw SPU 1-3, no overflow lanes */
    build(0, 0xFFull | 0xFF0000ull, &p);
    check("aff4.n", p.n, 8);
    check("aff4.usable", p.usable, 4);
    check("aff4.pcores", p.pcores, 8);   /* machine-wide: log only */
    check_str("aff4.why", p.why_off, NULL);
    check("aff4.roles", roles(&p), 0x1ED);
    /* ...the core keeper on a 12900K (24 logical): main / FIFO / rawSPU0 hard-pinned on P-cores 1-3 (LP 2-7), the
     * rest mask keeps P-cores 0, 4-7 */
    build(0, ~0ull & ~0xFCull & 0xFFFFFFull, &p);
    check("keeper8.usable", p.usable, 5);
    check("keeper8.roles", roles(&p), 0x1ED);
    /* ...or E-cores only: nothing to prefer */
    build(0, 0xFF0000ull, &p);
    check("hybrid-ecore-only.n", p.n, 0);
    check_str("hybrid-ecore-only.why", p.why_off, "no P-core in the process affinity");

    /* 2P + 4E laptop (8 logical; the keeper does not pin below 12): main + FIFO only */
    mock_small(2, 4);
    build(0, 0xFFull, &p);
    check("2p4e.hybrid", p.hybrid, 1);
    check("2p4e.usable", p.usable, 2);
    check("2p4e.n", p.n, 4);
    check_str("2p4e.why", p.why_off, NULL);
    check("2p4e.roles", roles(&p), 0x180);
    port_pcore_describe(&p, buf, sizeof buf);
    check("2p4e.describe", strstr(buf, "hybrid CPU 2 P + 4 E, 2 usable") == buf, 1);
    /* 2P + 8E laptop (12 logical): without the keeper main + FIFO; with it (main pinned to P-core 1, LP 2-3; the
     * FIFO / rawSPU0 cores would be E-cores so they stay unpinned) the rest mask holds one P-core: nothing */
    mock_small(2, 8);
    build(0, 0xFFFull, &p);
    check("2p8e.usable", p.usable, 2);
    check("2p8e.roles", roles(&p), 0x180);
    build(0, 0xFFFull & ~0xCull, &p);
    check("2p8e-keeper.usable", p.usable, 1);
    check("2p8e-keeper.n", p.n, 2);
    check_str("2p8e-keeper.why", p.why_off, "fewer than 2 usable P-cores");
    check("2p8e-keeper.roles", roles(&p), 0);
    /* R1's keeper (main LP 2, FIFO LP 4 = an E-core): the same one P-core left */
    build(0, 0xFFFull & ~0x3Cull, &p);
    check("2p8e-r1keeper.usable", p.usable, 1);
    /* 6P + 8E (13600K, 20 logical) with the keeper: P-cores 0, 4, 5 left: no raw SPU 1-3 / overflow lanes */
    mock_small(6, 8);
    build(0, 0xFFFFFull & ~0xFCull, &p);
    check("6p8e-keeper.usable", p.usable, 3);
    check("6p8e-keeper.roles", roles(&p), 0x180);

    /* CPU sets reserved for another process are skipped; reserved for us are kept */
    mock_hybrid();
    s_buf[0].CpuSet.Allocated = 1;                                                 /* id 0x100: someone else's */
    s_buf[1].CpuSet.Allocated = 1; s_buf[1].CpuSet.AllocatedToTargetProcess = 1;   /* id 0x101: ours */
    build(0, ~0ull, &p);
    check("alloc.n", p.n, 15);
    check("alloc.id0", p.ids[0], 0x101);

    /* homogeneous 8-core SMT (one class): no-op */
    s_nb = 0;
    for (unsigned c = 0; c < 8; c++)
        for (unsigned t = 0; t < 2; t++) add(0x100 + c * 2 + t, 0, c * 2 + t, c * 2, 0, 0, 0);
    build(0, ~0ull, &p);
    check("homog.hybrid", p.hybrid, 0);
    check("homog.n", p.n, 0);
    check("homog.usable", p.usable, 0);
    check("homog.roles", roles(&p), 0);
    check("homog.pcores", p.pcores, 8);
    check("homog.ecores", p.ecores, 0);
    check_str("homog.why", p.why_off, "no hybrid CPU");

    /* AMD 7950X-like: 16 cores, 2 CCDs (LLC 0/1), one class: no-op, 2 L3 domains reported */
    s_nb = 0;
    for (unsigned c = 0; c < 16; c++)
        for (unsigned t = 0; t < 2; t++) add(0x100 + c * 2 + t, 0, c * 2 + t, c * 2, c < 8 ? 0 : 1, 0, 0);
    build(0, ~0ull, &p);
    check("ccd.n", p.n, 0);
    check("ccd.llc", p.llc, 2);
    check_str("ccd.why", p.why_off, "no hybrid CPU");
    port_pcore_describe(&p, buf, sizeof buf);
    check_str("ccd.describe", buf, "P-core placement off (no hybrid CPU; 16 cores, 2 L3 domains, 1 group)");

    /* homogeneous 128 logical CPUs in 2 groups (Threadripper-like): no-op */
    s_nb = 0;
    for (unsigned g = 0; g < 2; g++)
        for (unsigned lp = 0; lp < 64; lp++) add(0x100 + g * 64 + lp, g, lp, lp & ~1u, lp / 16, 0, 0);
    build(0, ~0ull, &p);
    check("mg-homog.n", p.n, 0);
    check("mg-homog.groups", p.groups, 2);
    check("mg-homog.pcores", p.pcores, 64);
    check("mg-homog.llc", p.llc, 8);

    /* hybrid across 2 groups: group 0 = 40 P + 24 E logical, group 1 = 40 P + 24 E logical (hypothetical).
     * Only the thread's group is selected, and the same LP index in the other group never aliases. */
    s_nb = 0;
    for (unsigned g = 0; g < 2; g++) {
        for (unsigned lp = 0; lp < 40; lp++) add(0x100 + g * 64 + lp, g, lp, lp & ~1u, 0, 1, 0);
        for (unsigned lp = 40; lp < 64; lp++) add(0x100 + g * 64 + lp, g, lp, lp, 1, 0, 0);
    }
    build(1, ~0ull, &p);
    check("mg-hybrid.n", p.n, 40);
    check("mg-hybrid.usable", p.usable, 20);
    check("mg-hybrid.id0", p.ids[0], 0x140);
    check("mg-hybrid.pcores", p.pcores, 40);
    check("mg-hybrid.ecores", p.ecores, 48);
    check("mg-hybrid.groups", p.groups, 2);
    build(0, 0x3ull, &p);
    check("mg-hybrid-g0.n", p.n, 2);
    check("mg-hybrid-g0.id1", p.ids[1], 0x101);

    /* no / broken buffer */
    port_pcore_plan_build(NULL, 0, 0, ~0ull, &p);
    check_str("empty.why", p.why_off, "CPU sets unavailable");
    mock_hybrid();
    s_buf[3].Size = 4;   /* malformed entry: the whole buffer is distrusted */
    build(0, ~0ull, &p);
    check("corrupt.n", p.n, 0);
    check_str("corrupt.why", p.why_off, "CPU sets unavailable");

    /* the budget: 6 usable (13600K, no keeper) keeps the overflow lanes off, 8 admits them */
    p.usable = 6;
    check("roles6", roles(&p), 0x1FD);
    p.usable = 8;
    check("roles8", roles(&p), 0x1FF);

    /* switches: PORT_CPU_PCORES wins, PORT_PCORE_HOT is the older name, penv_bool words */
    _putenv("PORT_CPU_PCORES="); _putenv("PORT_PCORE_HOT=");
    check("env.default", port_cpu_pcores_env(), 1);
    _putenv("PORT_PCORE_HOT=0");
    check("env.legacy0", port_cpu_pcores_env(), 0);
    _putenv("PORT_CPU_PCORES=on");
    check("env.new-wins", port_cpu_pcores_env(), 1);
    _putenv("PORT_CPU_PCORES=abc");
    check("env.garbage-falls-back", port_cpu_pcores_env(), 0);
    _putenv("PORT_PCORE_HOT="); _putenv("PORT_CPU_PCORES= OFF ");
    check("env.off", port_cpu_pcores_env(), 0);
    _putenv("PORT_CPU_PCORES=");
    check("prio.default", port_thread_priority_env(), 0);
    _putenv("PORT_THREAD_PRIORITY=yes");
    check("prio.yes", port_thread_priority_env(), 1);
    _putenv("PORT_THREAD_PRIORITY=");

    /* [native] label */
    check_str("label.on", port_pcore_label(buf, sizeof buf, 1, NULL), "PORT_CPU_PCORES");
    check_str("label.nohybrid", port_pcore_label(buf, sizeof buf, 0, "no hybrid CPU"),
              "PORT_CPU_PCORES,overridden: no hybrid CPU");
    _putenv("PORT_CPU_PCORES=off");
    check_str("label.useroff", port_pcore_label(buf, sizeof buf, 0, "PORT_CPU_PCORES=off"), "PORT_CPU_PCORES");
    _putenv("PORT_CPU_PCORES=");

    /* the real machine: whatever it is, the query must not crash and must be consistent */
    port_pcore_query(&p, 0);
    check("real.consistent", (p.why_off == NULL) == (p.hybrid && p.usable >= 2), 1);
    printf("this machine: %s\n", port_pcore_describe(&p, buf, sizeof buf));

    printf("%s: %d/%d checks passed\n", s_fail ? "FAILED" : "OK", s_n - s_fail, s_n);
    return s_fail != 0;
}
