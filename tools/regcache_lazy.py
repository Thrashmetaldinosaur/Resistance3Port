#!/usr/bin/env python3
"""Lazy register write-back for the lifted PPU code (post-pass after
regcache.py v141, ppu_fastmem.py v163 and the port hooks).

regcache v141 keeps each guest register in a local (R_n) but writes every
update through to ppu_context as well:  ctx->gpr[n] = R_n = expr;
That is one memory store per lifted instruction, and it keeps every
intermediate CR / XER value alive. Only code that looks at ctx needs ctx to
be current, and within a function that is exactly: a statement that hands
ctx to something else (calls, syscalls, HLE, indirect calls, trampolines,
DRAIN_TRAMPOLINE, hooks), a statement that reads or addresses a cached ctx
field directly, and leaving the function (return / the closing brace).

This pass removes the write-through on every other line and, in front of
each of those points, stores the registers that may differ from ctx there
("dirty" = written on some path since the last point where ctx was made
current). The dirty sets come from a forward may-dataflow over the
function's lines (fallthrough to the next line is always assumed, plus every
goto edge), so an over-approximation only costs a redundant store of the
current value -- never a wrong value: R_n always holds the guest register.
Lines that hand ctx off keep their write-throughs as they are.

A function is left as it is unless every body line is a complete, balanced
statement or a label, every goto target is a label of the function, and the
function was produced by regcache (it has its _RCX_RL macro).

  regcache_lazy.py [--lazy-wb] [--fpr-cache [--lfs-barrier]] in.cpp out.cpp

--fpr-cache also keeps ctx->fpr[n] in double F_n locals (stored/reloaded at
the same points). See scratch\recomp_codegen.md for the measurements: neither
option is a measurable speedup, so the shipped output does not use them.

--reglocals=N (needs --lazy-wb; default off = output unchanged) keeps guest
registers in the locals ACROSS calls instead of re-reading every one of them
from ctx after each call (_RCX_RL). See scratch\cpu_reglocals.md.
  1  liveness: after a call reload only the registers that are live there
     (read before being redefined, or stored at a later flush point); and a
     plain `return;` does not store a register that provably holds its
     entry value while ctx still holds it too. Exact: no ABI assumption.
  2  + a direct callee whose summary proves it preserves rN (r2, r13,
     r14-r31) does not force a reload of R_N.
  3  + rN is not even stored before a direct call when the callee both
     preserves it and never reads its incoming value (XenonRecomp-style
     non-volatile locals; the guest stack save slot then holds a stale value).
Levels 2/3 need --summary=FILE, written by
  regcache_lazy.py --summarize FILE chunk1.cpp chunk2.cpp ...
(callee summaries, fixpoint over the whole program; a callee that is not in
the file is treated as reading and clobbering everything). --abi-trust skips
the summaries and assumes the PPC64 ABI for every call (r1, r2, r13-r31 and
cr2-cr4 preserved; r14-r31 never read): an experiment, not a safe mode.
"""
import re, sys

TAG = '/* lazywb v1 */'
FUNC_RE = re.compile(r'^void (func_[0-9A-F]+)\(ppu_context\* ctx\) \{$')
LABEL_RE = re.compile(r'^\s*(loc_[0-9A-F]+):\s*$')
GOTO_RE = re.compile(r'\bgoto\s+(loc_[0-9A-F]+)\s*;')
DECL_RE = re.compile(r'^\s*uint(32|64)_t (R_(\d+|cr|lr|ctr|xer)) = ctx->(gpr\[(\d+)\]|cr|lr|ctr|xer);$')
FIELD = r'(gpr\[(\d+)\]|cr|lr|ctr|xer)'
ANY_FIELD_RE = re.compile(r'\bctx->' + FIELD + r'(?![\w\[])')
BARE_RE = re.compile(r'\bctx\b(?!->)')
CTX_RE = re.compile(r'\bctx->(\w+)')
# fields that are never cached in locals: reading them needs no flush
SAFE_FIELDS = {'fpr', 'vr', 'vscr', 'fpscr', 'reserve_addr', 'reserve_value'}
ASSIGN_OP = r'(?:=(?!=)|\+=|-=|\*=|/=|%=|\|=|&=|\^=|<<=|>>=)'


FPR_RE = re.compile(r'\bctx->fpr\[(\d+)\]')
FPR_ODD_RE = re.compile(r'\bctx->fpr\b(?!\[\d+\])')


def lname(f):
    m = re.match(r'(gpr|fpr)\[(\d+)\]', f)
    if m: return ('R_%s' if m.group(1) == 'gpr' else 'F_%s') % m.group(2)
    return 'R_' + f


def wt_regex(field):
    """ctx->FIELD = followed by an assignment to its local (write-through)"""
    ln = re.escape(lname(field))
    return re.compile(r'\bctx->' + re.escape(field) + r' = (?=(?:(?:--|\+\+)' + ln + r'\b)|(?:' + ln +
                      r'\s*' + ASSIGN_OP + r'))')


def strip_comments(t):
    return re.sub(r'/\*.*?\*/', '', t).strip()


def balanced(t):
    d = 0
    for ch in t:
        if ch in '({[': d += 1
        elif ch in ')}]':
            d -= 1
            if d < 0: return False
    return d == 0


MUTATE = 0   # test-only: deliberately broken variants to prove the differential tester sees errors
FPR_CACHE = False
LFS_BARRIER = False  # --lfs-barrier: force cvtss2sd after lfs (quiets SNaN); off = matches the old build better


def xform(lines, fields, ndecl=0, fprs=()):
    """lines: body lines (without the header / closing brace); the first ndecl
    are the regcache declarations. fprs: FPR numbers to cache in F_n locals
    (empty = FPRs stay in ctx).
    returns (new_lines, why)"""
    wts = {f: wt_regex(f) for f in fields}
    ffields = ['fpr[%d]' % k for k in fprs]
    allf = list(fields) + ffields
    reload_f = ' '.join('F_%d = ctx->fpr[%d];' % (k, k) for k in fprs) if MUTATE != 4 else ''
    n = len(lines)
    labels = {}
    kind = []      # 'label' | 'flush' | 'plain'
    writes = []    # fields written (stripped) on plain lines
    newtext = []
    for i, l in enumerate(lines):
        t = l.strip()
        if i < ndecl:
            kind.append('decl'); writes.append(()); newtext.append(l); continue
        m = LABEL_RE.match(l)
        if m:
            labels[m.group(1)] = i
            kind.append('label'); writes.append(()); newtext.append(l); continue
        c = strip_comments(t)
        if c == '':
            kind.append('plain'); writes.append(()); newtext.append(l); continue
        if '"' in c or "'" in c or c.startswith('#') or c.startswith('else') or c.startswith('case ') \
                or c.startswith('default'):
            return None, 'odd line'
        if not balanced(c) or not (c.endswith(';') or c.endswith('}')):
            return None, 'incomplete statement'
        # flush points
        flush = bool(BARE_RE.search(c)) or (MUTATE != 3 and bool(re.search(r'\breturn\b', c)))
        if not flush:
            for m in CTX_RE.finditer(c):
                if m.group(1) not in SAFE_FIELDS and m.group(1) != 'gpr' and m.group(1) not in fields \
                        and m.group(1) not in ('cr', 'lr', 'ctr', 'xer'):
                    flush = True; break
        if not flush:
            s = l; w = []
            for f, rx in wts.items():
                s2, k = rx.subn('', s)
                if k: w.append(f); s = s2
            if ANY_FIELD_RE.search(strip_comments(s.strip())) or re.search(r'\bctx->gpr\b', strip_comments(s)):
                flush = True     # a read / address of a cached field
            elif fprs and FPR_ODD_RE.search(strip_comments(s)):
                flush = True     # ctx->fpr used other than as ctx->fpr[n]
            else:
                if fprs:
                    used = set(int(k) for k in FPR_RE.findall(s))
                    s = FPR_RE.sub(lambda mm: 'F_' + mm.group(1), s)
                    # lfs: keep the float->double conversion opaque. Otherwise clang
                    # folds a later (float)F_n back to ftmp and a signalling NaN is
                    # stored unquieted, where the ctx round trip ran cvtss2sd.
                    if LFS_BARRIER:
                        s = re.sub(r'\b(F_\d+) = ftmp;', r'\1 = ftmp; __asm__("" : "+x"(\1));', s)
                    for k in sorted(used):
                        if re.search(r'\bF_%d\s*%s' % (k, ASSIGN_OP), s) or re.search(r'&\s*F_%d\b' % k, s) \
                                or re.search(r'(--|\+\+)\s*F_%d\b|\bF_%d\s*(--|\+\+)' % (k, k), s):
                            w.append('fpr[%d]' % k)
                kind.append('plain'); writes.append(tuple(w)); newtext.append(s); continue
        if fprs and re.search(r'\bgoto\b', c):
            return None, 'fpr: goto on a flush line'
        kind.append('flush'); writes.append(()); newtext.append(l)
    succ = [[] for _ in range(n)]
    for i, l in enumerate(lines):
        if i + 1 < n: succ[i].append(i + 1)
        for m in GOTO_RE.finditer(l):
            if m.group(1) not in labels: return None, 'goto outside'
            succ[i].append(labels[m.group(1)])
        if re.search(r'\bgoto\b', l) and not GOTO_RE.search(l): return None, 'odd goto'
    if RL is not None and RL_FN_OK and not fprs:
        return rl_xform(kind, newtext, succ, list(fields), writes)
    # forward may-dirty dataflow (bitmask over fields)
    fidx = {f: k for k, f in enumerate(allf)}
    gen = [0] * n
    for i in range(n):
        for f in writes[i]: gen[i] |= 1 << fidx[f]
    din = [0] * n
    dout = [-1] * n
    work = list(range(n - 1, -1, -1))
    inq = [True] * n
    while work:
        i = work.pop()
        inq[i] = False
        o = 0 if kind[i] in ('flush', 'decl') else (din[i] | gen[i])
        if o != dout[i]:
            dout[i] = o
            for s in succ[i]:
                nd = din[s] | o
                if nd != din[s]:
                    din[s] = nd
                    if not inq[s]: inq[s] = True; work.append(s)
    def stores(mask):
        return ' '.join('ctx->%s = %s;' % (f, lname(f)) for f in allf if mask >> fidx[f] & 1
                        and not (MUTATE == 2 and f == 'cr') and not (MUTATE == 5 and f.startswith('fpr')))
    out = []
    nflush = 0
    for i in range(n):
        l = newtext[i]
        if kind[i] == 'flush':
            if din[i]:
                ind = l[:len(l) - len(l.lstrip())]
                l = ind + stores(din[i]) + ' ' + l.lstrip()
                nflush += 1
            if reload_f:
                l = l + ' ' + reload_f     # the statement may have changed ctx->fpr
        if kind[i] == 'decl' and i == ndecl - 1 and fprs:
            l = l + '\n' + '\n'.join('        double F_%d = ctx->fpr[%d];' % (k, k) for k in fprs)
        out.append(l)
    end_mask = dout[n - 1] if n else 0
    if end_mask > 0 and MUTATE != 1:
        out.append('        ' + stores(end_mask))
    return out, 'ok'


# ---------------------------------------------------------------- reglocals
# Everything below is only reached with --reglocals=N / --summarize (RL set).
RL = None          # {'level', 'trust', 'P', 'L'} or {'collect': True, 'funcs': {}}
RL_FN_OK = True    # this function's _RCX_RL reloads exactly its declared fields
RLSTATS = {}
NV_MASK = (1 << 2) | (1 << 13) | sum(1 << k for k in range(14, 32))   # r1 is restored arithmetically: never trusted
ALL32 = (1 << 32) - 1
TRUST_P = NV_MASK | (1 << 1)
TRUST_SKIP = sum(1 << k for k in range(14, 32))
TOK = r'(\d+|cr|lr|ctr|xer)'
REG_TOK = re.compile(r'\bR_' + TOK + r'\b')
CALL_RE = re.compile(r'^(?P<lr>ctx->lr = R_lr = 0x[0-9A-Fa-f]+;\s*)?(?P<cond>if \((?P<c>[^{}]*)\) \{\s*)?'
                     r'(?:(?P<fn>func_[0-9A-F]+)\(ctx\);|ps3_indirect_call\(ctx\);|lv2_syscall\(ctx\);)\s*'
                     r'(?:DRAIN_TRAMPOLINE\(ctx\);\s*)?_RCX_RL;(?(cond)\s*\})$')
TRAMP_RE = re.compile(r'^(?:if \((?P<c>[^{}]*)\) )?\{ g_trampoline_fn = \(void\(\*\)\(void\*\)\)(?P<fn>func_[0-9A-F]+); return; \}$')
DEF_RE = re.compile(r'^R_' + TOK + r' = ([^;]*);$')
WRITE_RE = re.compile(r'(?:\+\+|--)\s*R_' + TOK + r'\b|\bR_' + TOK + r'\s*(?:' + ASSIGN_OP + r'|\+\+|--)'
                      r'|(?:^|[(,=]\s*)&\s*R_' + TOK + r'\b')
CS_RE = re.compile(r'^uint64_t _cs_(\d+) = R_(\d+);$')
RESTORE_RE = re.compile(r'^R_(\d+) = _cs_(\d+);$')
NUM = r'(-?(?:0x[0-9A-Fa-f]+|\d+))'
SAVE_RE = re.compile(r'^vm_write64(?:_stk)?\(R_1 \+ ' + NUM + r', R_(\d+)\);$')
SLOT_RE = re.compile(r'\bvm_read\w*\(R_1 \+ ' + NUM + r'\)')


def popc(m): return bin(m).count('1')


def r1_escapes(t):
    """offsets X of r1+X values that leave the line other than as a load/store
    address (None = not a constant offset)"""
    s = re.sub(r'\bvm_write64(?:_stk)?\(R_1 \+ [^,]+, R_1\)', '', t)     # stdu back chain
    s = re.sub(r'\bvm_\w+\(R_1 \+ [^,)]+', 'vm(', s)
    s = re.sub(r'(?<![\w>.])R_1 (?:\+=|=(?!=)) [^;]*;', '', s)             # r1's own update
    return [int(m.group(2), 0) if m.group(2) else None
            for m in re.finditer(r'\bR_1\b(\s*\+\s*' + NUM + r'(?![\w(]))?', s)]


def rl_facts(kind, newtext, fields):
    tok2k = {lname(f)[2:]: k for k, f in enumerate(fields)}
    fg = [int(f[4:-1]) if f.startswith('gpr[') else -1 for f in fields]

    def tm(s):
        m = 0
        for t in REG_TOK.findall(s):
            k = tok2k.get(t)
            if k is not None: m |= 1 << k
        return m

    def g(m): return sum(1 << fg[k] for k in range(len(fg)) if m >> k & 1 and fg[k] >= 0)
    text = [strip_comments(l.strip()) for l in newtext]
    cs = set()
    for t in text:
        m = CS_RE.match(t)
        if m and m.group(1) == m.group(2): cs.add(int(m.group(1)))
    cs = {k for k in cs if not any(re.search(r'\b_cs_%d\s*%s' % (k, ASSIGN_OP), t) and not CS_RE.match(t)
                                   for t in text)}
    F = []
    for i, t in enumerate(text):
        d = {'sub': None, 'fn': None, 'cond': False, 'use': 0, 'def': 0, 'wr': 0, 'rst': 0, 'unc': False,
             'csn': None, 'save': None, 'slots': (), 'esc': (), 'ctxw': 0, 'falls': True,
             'use_g': 0, 'def_g': 0, 'wr_g': 0, 'rst_g': 0}
        if kind[i] in ('decl', 'label'):
            F.append(d); continue
        if kind[i] == 'flush':
            m = CALL_RE.match(t)
            mt = TRAMP_RE.match(t)
            if m:
                d.update(sub='call', fn=m.group('fn'), cond=bool(m.group('cond')), use=tm(m.group('c') or ''))
                if m.group('lr') and 'lr' in tok2k: d['def'] = 1 << tok2k['lr']
            elif mt:
                d.update(sub='tramp', fn=mt.group('fn'), cond=bool(mt.group('c')), use=tm(mt.group('c') or ''))
            elif re.search(r'\breturn\b', t) and not BARE_RE.search(t) and 'g_trampoline_fn' not in t \
                    and '_RCX_RL' not in t:
                d.update(sub='ret', use=tm(t), unc=newtext[i].strip() == 'return;')
            elif not BARE_RE.search(t) and not re.search(r'\breturn\b|\bgoto\b|&\s*ctx->gpr|\bctx->gpr\b(?!\[)', t) and \
                    all(f in ('xer', 'cr', 'lr', 'ctr') or f in SAFE_FIELDS or f == 'gpr'
                        for f in CTX_RE.findall(t)):
                # e.g. ppc_srad(&ctx->xer, ...): touches no other part of ctx
                d.update(sub='fobs', use=tm(t), wr=tm(' '.join('R_' + (a or b or c) for a, b, c in WRITE_RE.findall(t))))
            else:
                d.update(sub='obs', use=tm(t))
            if d['sub'] != 'obs':
                d['ctxw'] = sum(1 << int(x) for x in re.findall(r'\bctx->gpr\[(\d+)\] = ', t))
        else:
            d['sub'] = 'plain'
            m = DEF_RE.match(t)
            if m and not re.search(r'\bR_%s\b' % m.group(1), m.group(2)):
                d['def'] = tm('R_' + m.group(1)); d['use'] = tm(m.group(2))
            else:
                d['use'] = tm(t)
                if not re.search(r'\b(?:if|while|for|goto|switch)\b', t):
                    # straight-line block: a register that only ever appears as `R_x = ` is a def
                    for x in set(REG_TOK.findall(t)):
                        if len(re.findall(r'\bR_%s\b' % x, t)) == len(re.findall(r'\bR_%s = (?!=)' % x, t)):
                            d['def'] |= tm('R_' + x); d['use'] &= ~tm('R_' + x)
            d['wr'] = tm(' '.join('R_' + (a or b or c) for a, b, c in WRITE_RE.findall(t))) | d['def']
            m = RESTORE_RE.match(t)
            if m and m.group(1) == m.group(2) and int(m.group(1)) in cs: d['rst'] = tm('R_' + m.group(1))
            m = CS_RE.match(t)
            if m and m.group(1) == m.group(2): d['csn'] = int(m.group(1))
            m = SAVE_RE.match(t)
            if m: d['save'] = (int(m.group(1), 0), int(m.group(2)))
            d['slots'] = tuple(int(x, 0) for x in SLOT_RE.findall(t))
            if re.search(r'\bR_1\b', t): d['esc'] = tuple(r1_escapes(t))
        if d['sub'] == 'fobs':
            d['slots'] = tuple(int(x, 0) for x in SLOT_RE.findall(t))
            if re.search(r'\bR_1\b', t): d['esc'] = tuple(r1_escapes(t))
        for k in ('use', 'def', 'wr', 'rst'): d[k + '_g'] = g(d[k])
        # can control run off the end of this line? (the dataflow assumes it always can)
        d['falls'] = not (re.fullmatch(r'goto loc_[0-9A-F]+;', t) or t == 'return;' or
                          (d['sub'] == 'tramp' and not d['cond']))
        F.append(d)
    return F, fg, cs


def rl_flow(n, succ, entry, tf, must):
    """forward dataflow over the line CFG; None = unreachable (top for must)"""
    IN = [None] * n; OUT = [None] * n
    if not n: return IN, OUT
    IN[0] = entry
    work = [0]
    while work:
        i = work.pop()
        o = tf(i, IN[i])
        if o == OUT[i]: continue
        OUT[i] = o
        for s in succ[i]:
            v = o if IN[s] is None else (IN[s] & o if must else IN[s] | o)
            if v != IN[s]:
                IN[s] = v; work.append(s)
    return IN, OUT


def rl_e_tf(F, pcall):
    """'the guest register still holds its value from function entry' (must)"""
    def tf(i, e):
        d = F[i]; s = d['sub']
        if s == 'plain': return (e & ~d['wr_g']) | d['rst_g']
        if s == 'call': return e & pcall(d['fn'])
        if s == 'fobs': return e & ~d['wr_g']
        if s == 'obs': return 0
        return e
    return tf


def rl_summarize_fn(fd, Pm, Lm):
    """(P, L, deps): P = NV registers whose ctx value at exit equals the value
    at entry, L = NV registers whose incoming value may be read (by this
    function or a callee), deps = direct callees used"""
    F, succ, fg, cs = fd
    n = len(F)
    if any(d['sub'] == 'obs' for d in F): return 0, NV_MASK, set()
    deps = set()

    def pc(fn):
        if fn is None: return 0
        deps.add(fn); return Pm.get(fn, 0)

    def lc(fn):
        if fn is None: return NV_MASK
        deps.add(fn); return Lm.get(fn, NV_MASK)
    EIN, EOUT = rl_flow(n, succ, ALL32, rl_e_tf(F, pc), True)
    P = NV_MASK
    for i, d in enumerate(F):
        if EIN[i] is None: continue
        if d['sub'] == 'ret': P &= EIN[i]
        elif d['sub'] == 'tramp': P &= EIN[i] & pc(d['fn'])
    if n and EOUT[n - 1] is not None and F[n - 1]['falls']: P &= EOUT[n - 1]

    def utf(i, u):
        d = F[i]
        return (u & ~d['def_g']) | d['rst_g'] if d['sub'] == 'plain' else u
    UIN, _ = rl_flow(n, succ, NV_MASK, utf, False)
    L = 0
    saves = {}; slots = []; escs = []
    for i, d in enumerate(F):
        u = UIN[i] or 0
        s = d['sub']
        if s == 'plain':
            ug = d['use_g']
            if d['csn'] is not None: ug &= ~(1 << d['csn'])
            if d['save'] and d['save'][1] in cs:
                ug &= ~(1 << d['save'][1]); saves[d['save'][1]] = d['save'][0]
            L |= ug & u
            slots.extend(d['slots']); escs.extend(d['esc'])
        elif s in ('call', 'tramp'): L |= u & (d['use_g'] | lc(d['fn']))
        elif s is not None:
            # ret, fobs (carry-setting shifts such as ppc_sraw(&ctx->xer, R_30, ..)) and
            # anything else: every register the line reads counts
            L |= u & d['use_g']
            slots.extend(d['slots']); escs.extend(d['esc'])
    # the save slot of an exempted prologue store is only dead if nothing reads it
    for r, off in saves.items():
        if any(off <= x < off + 8 for x in slots): L |= 1 << r
    if saves and any(x is None or x + 0x100 > min(saves.values()) for x in escs):
        for r in saves: L |= 1 << r       # frame address escapes near the save area
    return P & NV_MASK, L & NV_MASK, deps


def rl_fixpoint(funcs):
    P = {f: NV_MASK for f in funcs}   # greatest fixpoint
    L = {f: 0 for f in funcs}         # least fixpoint
    callers = {}
    work = list(funcs); inq = set(funcs)
    rounds = 0
    while work:
        f = work.pop(); inq.discard(f); rounds += 1
        p, l, deps = rl_summarize_fn(funcs[f], P, L)
        for gname in deps: callers.setdefault(gname, set()).add(f)
        p &= P[f]; l |= L[f]
        if (p, l) != (P[f], L[f]):
            P[f], L[f] = p, l
            for c in callers.get(f, ()):
                if c not in inq: inq.add(c); work.append(c)
    return P, L, rounds


def rl_xform(kind, newtext, succ, fields, writes):
    F, fg, cs = rl_facts(kind, newtext, fields)
    if RL.get('collect'): return (F, succ, fg, cs), 'ok'
    n = len(F)
    level, trust, Pm, Lm = RL['level'], RL['trust'], RL['P'], RL['L']
    nf = len(fields); ALLF = (1 << nf) - 1
    fidx = {f: k for k, f in enumerate(fields)}
    gk = {fg[k]: k for k in range(nf) if fg[k] >= 0}

    def g2f(m): return sum(1 << k for r, k in gk.items() if m >> r & 1)

    def f2g(m): return sum(1 << fg[k] for k in range(nf) if m >> k & 1 and fg[k] >= 0)

    def pcall(fn):
        if trust: return TRUST_P
        return Pm.get(fn, 0) if level >= 2 and fn else 0
    C = [0] * n; skip = [0] * n
    for i, d in enumerate(F):
        if d['sub'] != 'call': continue
        C[i] = ALLF & ~g2f(pcall(d['fn']))
        if trust: skip[i] = g2f(TRUST_SKIP)
        elif level >= 3 and d['fn'] in Pm:
            skip[i] = g2f(Pm[d['fn']] & ~Lm.get(d['fn'], NV_MASK) & NV_MASK)
        skip[i] &= ~C[i]
    # dirty: as in xform, except that a register whose store was skipped stays dirty
    gen = [0] * n
    for i in range(n):
        for f in writes[i]: gen[i] |= 1 << fidx[f]

    def dtf(i, x):
        x = x or 0
        if kind[i] == 'decl': return 0
        if kind[i] == 'flush': return x & skip[i]
        return x | gen[i]
    din, dout = rl_flow(n, succ, 0, dtf, False)
    din = [x or 0 for x in din]
    st = [din[i] & ~skip[i] if kind[i] == 'flush' else 0 for i in range(n)]
    # exact elision of stores at `return;`: guest value == entry value == ctx value
    EIN, _ = rl_flow(n, succ, ALL32, rl_e_tf(F, pcall), True)

    def ctf(i, c):
        d = F[i]
        if kind[i] == 'flush':
            sg = f2g(st[i]); e = ALL32 if EIN[i] is None else EIN[i]
            c = (c & ~sg) | (sg & e)
            c &= ~d['ctxw']
            if d['sub'] == 'call': c &= pcall(d['fn'])
            elif d['sub'] == 'obs': c = 0
        return c
    CIN, _ = rl_flow(n, succ, ALL32, ctf, True)
    elided = 0
    if not any(d['sub'] == 'obs' for d in F):
        for i, d in enumerate(F):
            if d['sub'] == 'ret' and d['unc'] and EIN[i] is not None and CIN[i] is not None:
                el = st[i] & g2f(EIN[i] & CIN[i])
                st[i] &= ~el; elided += popc(el)
    # liveness (backward may); a register is live where it is read or stored
    end_mask = (dout[n - 1] or 0) if n else 0
    preds = [[] for _ in range(n)]
    for i in range(n):
        for s in succ[i]: preds[s].append(i)
    lin = [0] * n; lout = [0] * n
    work = list(range(n)); inq = [True] * n
    while work:
        i = work.pop(); inq[i] = False
        lo = end_mask if i == n - 1 else 0
        for s in succ[i]: lo |= lin[s]
        lout[i] = lo
        d = F[i]; s_ = d['sub']
        if s_ in (None,): li = lo
        elif s_ == 'plain': li = d['use'] | (lo & ~d['def'])
        elif s_ == 'call': li = st[i] | d['use'] | ((lo & ~(0 if d['cond'] else C[i])) & ~d['def'])
        else: li = st[i] | d['use'] | lo
        if li != lin[i]:
            lin[i] = li
            for p in preds[i]:
                if not inq[p]: inq[p] = True; work.append(p)
    out = []
    S = RLSTATS
    for i in range(n):
        l = newtext[i]
        if kind[i] == 'flush':
            if F[i]['sub'] == 'call':
                R = lout[i] & C[i]
                S['calls'] = S.get('calls', 0) + 1
                S['reloads_before'] = S.get('reloads_before', 0) + nf
                S['reloads_after'] = S.get('reloads_after', 0) + popc(R)
                S['stores_skipped'] = S.get('stores_skipped', 0) + popc(din[i] & skip[i])
                if R == ALLF: rl = '_RCX_RL;'
                else: rl = ' '.join('%s = ctx->%s;' % (lname(f), f) for f in fields if R >> fidx[f] & 1)
                l = l.replace(' _RCX_RL;', (' ' + rl) if rl else '', 1)
            if st[i]:
                ind = l[:len(l) - len(l.lstrip())]
                l = ind + ' '.join('ctx->%s = %s;' % (f, lname(f)) for f in fields if st[i] >> fidx[f] & 1) \
                    + ' ' + l.lstrip()
        out.append(l)
    if end_mask:
        out.append('        ' + ' '.join('ctx->%s = %s;' % (f, lname(f)) for f in fields if end_mask >> fidx[f] & 1))
    S['stores_elided'] = S.get('stores_elided', 0) + elided
    return out, 'ok'


def process(src):
    global RL_FN_OK
    lines = src.split('\n')
    out = []; stats = {}; i = 0
    while i < len(lines):
        m = FUNC_RE.match(lines[i])
        k = i - 1
        while k > 0 and lines[k].startswith('extern \"C\" '): k -= 1   # hook declarations
        if not m or not (k >= 0 and lines[k].startswith('#define _RCX_RL ')):
            out.append(lines[i]); i += 1; continue
        j = i + 1
        while j < len(lines) and lines[j] != '}': j += 1
        body = lines[i + 1:j]
        fields = []
        for l in body:
            d = DECL_RE.match(l)
            if not d: break
            fields.append(d.group(4))
        if RL is not None:
            RL_FN_OK = sorted(re.findall(r'\bR_\w+ = ctx->(gpr\[\d+\]|cr|lr|ctr|xer);', lines[k])) == sorted(fields)
        if not fields:
            why = 'no decls'; nb = None
        else:
            fprs = sorted(set(int(k) for l in body for k in FPR_RE.findall(l))) if FPR_CACHE else []
            nb, why = xform(body, fields, len(fields), fprs)
            if nb is None and fprs:
                nb, why2 = xform(body, fields, len(fields), [])
                why = why + ' -> gpr only ' + why2
            elif fprs:
                why = 'ok+fpr'
        if RL is not None and RL.get('collect'):
            if nb is not None and RL_FN_OK: RL['funcs'][m.group(1)] = nb
            nb = None
        stats[why] = stats.get(why, 0) + 1
        out.append(lines[i])
        out.extend(nb if nb is not None else body)
        if j < len(lines): out.append(lines[j])
        i = j + 1
    return '\n'.join(out), stats


def main(argv):
    global MUTATE, FPR_CACHE, LFS_BARRIER, RL
    opts = [a for a in argv if a.startswith('--')]
    for o in opts:
        if o.startswith('--mutate='): MUTATE = int(o[9:])
    FPR_CACHE = '--fpr-cache' in opts
    LFS_BARRIER = '--lfs-barrier' in opts
    args = [a for a in argv if not a.startswith('--')]
    if '--summarize' in opts:
        import json
        RL = {'collect': True, 'funcs': {}}
        for p in args[1:]:
            process(open(p, encoding='utf-8', errors='surrogateescape').read())
        P, L, rounds = rl_fixpoint(RL['funcs'])
        json.dump({'P': {f: P[f] for f in sorted(P)}, 'L': {f: L[f] for f in sorted(L)}}, open(args[0], 'w'), indent=0)
        print(args[0], {'functions': len(P), 'evaluations': rounds,
                        'avg_preserved': round(sum(popc(v) for v in P.values()) / max(1, len(P)), 2),
                        'avg_read_nv': round(sum(popc(v) for v in L.values()) / max(1, len(L)), 2)})
        return
    level = 0
    for o in opts:
        if o.startswith('--reglocals='): level = int(o[12:])
    if level:
        if '--lazy-wb' not in opts or FPR_CACHE: sys.exit('--reglocals needs --lazy-wb (and no --fpr-cache)')
        RL = {'level': level, 'trust': '--abi-trust' in opts, 'P': {}, 'L': {}}
        summ = [o[10:] for o in opts if o.startswith('--summary=')]
        if summ:
            import json
            js = json.load(open(summ[0]))
            RL['P'], RL['L'] = js['P'], js['L']
        elif level >= 2 and not RL['trust']:
            sys.exit('--reglocals=%d needs --summary=FILE (or --abi-trust)' % level)
    src = open(args[0], encoding='utf-8', errors='surrogateescape').read()
    if TAG in src or '--lazy-wb' not in opts:
        if TAG in src: print(args[0], 'already processed')
        open(args[1], 'w', encoding='utf-8', errors='surrogateescape', newline='').write(src); return
    res, stats = process(src)
    tag = TAG + (' /* fprcache v2 */' if FPR_CACHE else '')
    if RL is not None:
        tag += ' /* reglocals v1 L%d%s */' % (level, ' abi-trust' if RL['trust'] else '')
    res = res.replace('#include "ppu_recomp.h"', '#include "ppu_recomp.h"\n' + tag, 1)
    open(args[1], 'w', encoding='utf-8', errors='surrogateescape', newline='').write(res)
    if RL is None: print(args[0], stats)
    else: print(args[0], stats, RLSTATS)


if __name__ == '__main__':
    main(sys.argv[1:])
