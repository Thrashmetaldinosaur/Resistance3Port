#!/usr/bin/env python3
"""SPURS job local-store pool (runtime/spu/spurs_job.c, perf final pass).

spu_run_spurs_job used to calloc/free a 256 KB local store per job; it now
reuses up to JOB_LS_POOL buffers. Builds the pool code (pulled verbatim out of
spurs_job.c) into a small host program and checks:
  - a reused buffer is all zero again, like calloc's;
  - a returned slot is reused, concurrent holders get distinct buffers, and
    a job that finds every slot busy falls back to calloc (slot -1);
  - PORT_SPURS_JOB_LS_REUSE=0 always uses calloc/free.
"""
import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#define SPU_LS_SIZE 0x40000u
%s
static int all_zero(const uint8_t* p) { for (uint32_t i = 0; i < SPU_LS_SIZE; i++) if (p[i]) return 0; return 1; }
int main(int argc, char** argv) {
    int fails = 0, s;
    if (argc > 1) {   /* PORT_SPURS_JOB_LS_REUSE=0 */
        uint8_t* a = job_ls_get(&s);
        int ok = a && s == -1 && all_zero(a);
        printf("[%%s] reuse off: calloc'd store (slot %%d)\n", ok ? "PASS" : "FAIL", s);
        job_ls_put(a, s);
        return ok ? 0 : 1;
    }
    uint8_t* a = job_ls_get(&s);
    int ok = a && s == 0 && all_zero(a);
    printf("[%%s] first job: pooled slot %%d, zeroed\n", ok ? "PASS" : "FAIL", s);
    fails += !ok;
    memset(a, 0xCD, SPU_LS_SIZE);
    job_ls_put(a, s);
    uint8_t* b = job_ls_get(&s);
    ok = b == a && s == 0 && all_zero(b);
    printf("[%%s] next job: same buffer reused and zeroed again\n", ok ? "PASS" : "FAIL");
    fails += !ok;
    uint8_t* h[JOB_LS_POOL + 1]; int hs[JOB_LS_POOL + 1];
    h[0] = b; hs[0] = s;
    for (int i = 1; i <= JOB_LS_POOL; i++) h[i] = job_ls_get(&hs[i]);
    ok = 1;
    for (int i = 0; i < JOB_LS_POOL; i++) {
        ok &= hs[i] == i && h[i] != NULL;
        for (int j = 0; j < i; j++) ok &= h[i] != h[j];
    }
    ok &= hs[JOB_LS_POOL] == -1 && h[JOB_LS_POOL] && all_zero(h[JOB_LS_POOL]);
    printf("[%%s] %%d concurrent jobs: distinct pooled buffers, overflow job calloc'd\n", ok ? "PASS" : "FAIL", JOB_LS_POOL + 1);
    fails += !ok;
    for (int i = 0; i <= JOB_LS_POOL; i++) job_ls_put(h[i], hs[i]);
    uint8_t* c = job_ls_get(&s);
    ok = c == a && s == 0;
    printf("[%%s] all returned: slot 0 free again\n", ok ? "PASS" : "FAIL");
    fails += !ok;
    job_ls_put(c, s);
    return fails ? 1 : 0;
}
'''


def main():
    cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not cc:
        print('SKIP: no C compiler')
        return
    src = (ROOT / 'runtime/spu/spurs_job.c').read_text(encoding='utf-8', errors='replace')
    m = re.search(r'#define JOB_LS_POOL .*?\nstatic void job_ls_put\(uint8_t\* ls, int slot\)\s*\{.*?\n\}', src, re.S)
    assert m, 'job LS pool not found in spurs_job.c'
    work = ROOT / 'tools' / '_test_spurs_job_ls_pool'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        c = work / 't.c'
        exe = work / ('t.exe' if os.name == 'nt' else 't')
        c.write_text(HARNESS % m.group(0))
        r = subprocess.run([cc, '-std=c11', '-O1', '-w', str(c), '-o', str(exe)], capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[-2000:])
            raise SystemExit('FAIL: harness did not compile')
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout, end='')
        if r.returncode:
            raise SystemExit('FAIL: SPURS job LS pool')
        env = dict(os.environ, PORT_SPURS_JOB_LS_REUSE='0')
        r = subprocess.run([str(exe), 'off'], capture_output=True, text=True, env=env)
        print(r.stdout, end='')
        if r.returncode:
            raise SystemExit('FAIL: PORT_SPURS_JOB_LS_REUSE=0')
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('PASS: SPURS job LS pool reuses zeroed buffers, falls back to calloc')


if __name__ == '__main__':
    main()
