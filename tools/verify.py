#!/usr/bin/env python3
"""Batch verification of written-but-unproven (DIFF) decomp functions against the gameplay
recordings (docs/verify.md).

  tools/verify.py plan   [--units U ...] [--min-size N] [--max N] [--match]
                          candidates from work/verify/scan.txt (hybridize scan -v --diff) -> work/verify/plan.json
  tools/verify.py build  [--jobs 2]
                          compile-check (bisects failing units) -> work/verify/clean.json; targets = targets.txt +
                          candidates as mod= groups -> work/verify/targets.txt; one regeneration into work/verify/gen
                          and a build in build-verify/
  tools/verify.py run    [--fixtures DIR ...] [--calls N] [--jobs 2]
                          per fixture: a noise run (the original twice) and a dual-execution run (original, then the
                          native version on the same memory, every byte of RAM compared, original's outcome kept)
                          -> work/verify/results.json
  tools/verify.py confirm [--fixtures DIR ...] [--jobs 2]
                          every candidate that was identical on every call runs NATIVE (outcome kept) through every
                          fixture; a divergence is peeled down to its culprit(s)
  tools/verify.py report [--write-overrides]
                          table; --write-overrides appends REPLAY_VERIFIED rows with evidence to
                          pcj-decomp status/verify_overrides.csv (tools/coverage.py counts them)
  tools/verify.py status  short summary of the state files

Every step is resumable from the files under work/verify. No git commits are made.
"""
import argparse, collections, concurrent.futures as cf, csv, datetime, glob, json, os, re, shutil, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HY = os.path.join(ROOT, 'tools', 'hybridize', 'hybridize.py')
WORK = os.path.join(ROOT, 'work', 'verify')
DECOMP = os.environ.get('PCJ_DECOMP', '/home/claude/pcj-decomp')
TARGETS = os.path.join(ROOT, 'tools', 'hybridize', 'targets.txt')
BDIR = os.path.join(ROOT, 'build-verify')
GEN = os.path.join(WORK, 'gen')
VTARGETS = os.path.join(WORK, 'targets.txt')
PLAN = os.path.join(WORK, 'plan.json')
CLEAN = os.path.join(WORK, 'clean.json')
RESULTS = os.path.join(WORK, 'results.json')
# recompiler --exclude-manual input (tools/regen_all.sh): the swap switch plus every hand-written replacement
EXCLUDE_EXTRA = ['src/det_file.c', 'src/native_gfx.c', 'src/native_dsound.c', 'src/mods_archive.c']
DET_WRAPPED = {'00284E60', '00221400', '002EE440', '00356BA9', '00356D49'}
# never candidates: a function run three times by the verifier must not own the frame, the input clock or host-side I/O
DENY_NAME = re.compile(r'MainUpdate|^(XApp|EXApp|EXBaseApp)::|^(EXGamePad|EXBaseGamePad|XGamePad|XBGamepad)::|'
                       r'^(EXFile|EXFileSys|EXFileManager|XBFileManager|EXFileSystem\w*|XBFile\w*)::|^(XBAudio|EXAudioStream|XBSoundManager)::|'
                       r'Quick[Ss]ave|^EXMemBlock::|^EXMalloc|^EXMemHeap::|'
                       # 2026-10-27: the rerun machinery itself crashes the game around front-end / HUD / window code
                       # (IntersectRect, FEObject::MakeSafeRect blamed: far too simple to be the cause). Cause unknown; see docs/verify.md.
                       r'^FE\w*::|^EXBaseWnd::|^EXBaseDisplay::|^EXWnd|^IntersectRect$|^EXSoundTag::|^EXGeoFile::|^SetFog$|^Rain_')


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), **kw)


def P(*a): return os.path.join(WORK, *a)


def jload(p, d=None):
    return json.load(open(p)) if os.path.exists(p) else d


def jsave(p, o):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    json.dump(o, open(p + '.tmp', 'w'), indent=1); os.replace(p + '.tmp', p)


# ------------------------------------------------------------------ targets.txt (unit= and mod= groups)
def read_targets(path=TARGETS):
    """-> list of (kind, unit, [VA upper]) in file order"""
    out = []
    for l in open(path):
        l = l.split('#')[0].strip()
        if not l: continue
        t = l.split()
        kind, unit = t[0].split('=', 1)
        out.append((kind, unit, [v.upper().zfill(8) for v in t[1:]]))
    return out


def target_vas(path=TARGETS):
    return {v for _, _, vs in read_targets(path) for v in vs}


def write_targets(groups, path, header):
    with open(path, 'w') as f:
        f.write(header)
        for kind, unit, vas in groups:
            if vas: f.write(f'{kind}={unit} ' + ' '.join(v.lower() for v in vas) + '\n')


def merged_targets(cands):
    """current targets.txt + candidates: a unit's candidates join its mod= line, or get a new one"""
    groups = read_targets()
    by_unit = collections.OrderedDict()
    for c in cands: by_unit.setdefault(c['unit'], []).append(c['va'])
    for unit, vas in by_unit.items():
        for i, (kind, u, vs) in enumerate(groups):
            if kind == 'mod' and u == unit:
                groups[i] = (kind, u, vs + [v for v in vas if v not in vs]); break
        else:
            groups.append(('mod', unit, list(vas)))
    return groups


# ------------------------------------------------------------------ plan
def load_scan():
    out = []
    p = P('scan.txt')
    if not os.path.exists(p): sys.exit('no work/verify/scan.txt: run  hybridize.py scan -v --diff <units>  (docs/verify.md)')
    for l in open(p):
        if not l.startswith('OK  '): continue
        t = l.split()
        st = t[-1] if t[-1] in ('MATCH', 'DIFF') else 'MATCH'
        unit = next(x for x in reversed(t) if x.startswith('@'))[1:]
        out.append(dict(va=t[1].upper().zfill(8), size=int(t[2]), flags=t[3], name=t[4], unit=unit, status=st))
    return out


def mods_file_vas():
    """VAs that have a MOD section in mods/*.cpp: those are deliberate behaviour changes, never candidates"""
    vas = set()
    for p in glob.glob(os.path.join(ROOT, 'mods', '*.cpp')):
        vas |= {m.upper().zfill(8) for m in re.findall(r'^// MOD ([0-9a-fA-F]{6,8})', open(p, errors='replace').read(), re.M)}
    return vas


def cmd_plan(a):
    os.makedirs(WORK, exist_ok=True)
    skip = target_vas() | mods_file_vas() | DET_WRAPPED
    want = ('MATCH', 'DIFF') if a.match else ('DIFF',)
    pool = [c for c in load_scan() if c['status'] in want and c['va'] not in skip and c['size'] >= a.min_size and not DENY_NAME.search(c['name'])
            and (not a.units or c['unit'] in a.units) and (not a.exclude_units or c['unit'] not in a.exclude_units)]
    pool.sort(key=lambda c: (c['unit'], -c['size']))
    if a.max and len(pool) > a.max:
        # round robin over units, largest first within a unit, so one unit cannot take the whole batch
        byu = collections.OrderedDict()
        for c in pool: byu.setdefault(c['unit'], []).append(c)
        pick = []
        while len(pick) < a.max and any(byu.values()):
            for u in list(byu):
                if byu[u] and len(pick) < a.max: pick.append(byu[u].pop(0))
        pool = pick
    jsave(PLAN, pool)
    byu = collections.Counter(c['unit'] for c in pool)
    print(f'plan: {len(pool)} candidates, {sum(c["size"] for c in pool)} bytes, {len(byu)} units -> {PLAN}')
    for u, n in byu.most_common(): print(f'  {n:4d}  {u}')


# ------------------------------------------------------------------ build
def cxx_flags():
    fm = open(os.path.join(ROOT, 'build', 'CMakeFiles', 'pcj.dir', 'flags.make')).read()
    inc = re.search(r'^CXX_INCLUDES = (.*)$', fm, re.M).group(1).split()
    dfn = re.search(r'^CXX_DEFINES = (.*)$', fm, re.M).group(1).split()
    return ['c++', '-std=gnu++17', '-fsyntax-only', '-w'] + dfn + inc


def gen_and_compile(unit, vas, tag):
    d = P('cc', f'{unit}_{tag}')
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    r = sh([sys.executable, HY, 'gen', 'mod=' + unit] + [v.lower() for v in vas] + ['-o', d], capture_output=True, text=True, cwd=ROOT)
    if r.returncode: return False, 'gen: ' + (r.stderr.strip().splitlines() or ['?'])[-1][:200]
    r = sh(cxx_flags() + glob.glob(d + '/hy_*.cpp'), capture_output=True, text=True)
    shutil.rmtree(d, ignore_errors=True)
    if r.returncode:
        e = next((l for l in r.stderr.splitlines() if 'error' in l), '?')
        return False, 'compile: ' + e.split('error', 1)[-1].strip()[:200]
    return True, ''


def bisect_unit(unit, vas, res):
    ok, why = gen_and_compile(unit, vas, 'b%d' % threading.get_ident())
    if ok:
        for v in vas: res[v] = ''
        return
    if len(vas) == 1:
        res[vas[0]] = why; return
    h = len(vas) // 2
    bisect_unit(unit, vas[:h], res); bisect_unit(unit, vas[h:], res)


def regen(targets_file, gen_dir, hy_dir, label):
    """RUN.md: hybridize gen + tools.recomp with the same --exclude-manual as tools/regen_all.sh (about 4 min)"""
    spec = [t for kind, u, vs in read_targets(targets_file) for t in [f'{kind}={u}'] + [v.lower() for v in vs]]
    os.makedirs(hy_dir, exist_ok=True)
    r = sh([sys.executable, HY, 'gen'] + spec + ['-o', hy_dir], cwd=ROOT, stdout=open(P(f'hygen-{label}.log'), 'w'), stderr=subprocess.STDOUT)
    if r.returncode: sys.exit(f'hybridize gen failed: see work/verify/hygen-{label}.log')
    with open(os.path.join(hy_dir, 'exclude_manual.c'), 'wb') as f:
        f.write(open(os.path.join(hy_dir, 'hybrid_swap.c'), 'rb').read())
        for x in EXCLUDE_EXTRA: f.write(open(os.path.join(ROOT, x), 'rb').read())
    shutil.rmtree(gen_dir, ignore_errors=True)
    cmd = [sys.executable, '-m', 'tools.recomp', '../game/default.xbe', '--all', '--split', '250', '--game-name', 'Predator: Concrete Jungle',
           '--gen-dir', gen_dir, '--disasm-dir', '../work/disasm', '--func-id-dir', '../work/func_id', '--abi-dir', '../work/abi',
           '-o', f'../work/recomp-{label}', '--manual-functions', '../game/manual_functions.json',
           '--exclude-manual', os.path.join(hy_dir, 'exclude_manual.c')]
    t0 = time.time()
    r = sh(cmd, cwd=os.path.join(ROOT, 'xboxrecomp'), stdout=open(P(f'regen-{label}.log'), 'w'), stderr=subprocess.STDOUT)
    if r.returncode: sys.exit(f'tools.recomp failed, see work/verify/regen-{label}.log')
    print(f'regen: {time.time()-t0:.0f}s, {len(glob.glob(gen_dir + "/*.c"))} files', flush=True)


def cmake_build(bdir, gen_dir, targets_file, jobs):
    t0 = time.time()
    r = sh(['cmake', '-S', ROOT, '-B', bdir, f'-DRECOMP_GEN_DIR={gen_dir}', f'-DCJ_TARGETS={targets_file}'],
           stdout=open(P('cmake.log'), 'w'), stderr=subprocess.STDOUT)
    if r.returncode: sys.exit('cmake failed: see work/verify/cmake.log')
    r = sh(['cmake', '--build', bdir, f'-j{jobs}'], stdout=open(P('build.log'), 'w'), stderr=subprocess.STDOUT)
    if r.returncode: sys.exit('build failed: see work/verify/build.log')
    print(f'build: {time.time()-t0:.0f}s -> {bdir}/pcj', flush=True)


def cmd_build(a):
    plan = jload(PLAN)
    if plan is None: sys.exit('no plan: run  verify.py plan  first')
    by_unit = collections.OrderedDict()
    for c in plan: by_unit.setdefault(c['unit'], []).append(c['va'])
    res = jload(CLEAN, {}) if not a.recheck else {}
    todo = [(u, v) for u, v in by_unit.items() if not all(x in res for x in v)]
    print(f'compile check: {len(todo)} units', flush=True)
    lock = threading.Lock()

    def work(item):
        u, v = item; r = {}
        bisect_unit(u, [x for x in v if x not in res], r)
        with lock:
            res.update(r); jsave(CLEAN, res)
        bad = [x for x in r.values() if x]
        print(f'  {u}: {len(r) - len(bad)}/{len(r)} clean' + (f'  ({bad[0][:80]})' if bad else ''), flush=True)
    with cf.ThreadPoolExecutor(a.jobs) as ex: list(ex.map(work, todo))
    clean = [c for c in plan if res.get(c['va'], 'x') == '']
    print(f'clean: {len(clean)} of {len(plan)} candidates')
    reasons = collections.Counter(re.sub(r"'[^']*'|\b\d+\b", 'X', x)[:70] for x in res.values() if x)
    for k, v in reasons.most_common(8): print(f'  {v:4d} {k}')
    if not clean: sys.exit('nothing to build')
    write_targets(merged_targets(clean), VTARGETS, '# verify.py: tools/hybridize/targets.txt + candidates as mod= groups (docs/verify.md)\n')
    jsave(P('batch.json'), clean)
    print(f'targets: {VTARGETS} ({len(clean)} candidates in {len({c["unit"] for c in clean})} units)', flush=True)
    regen(VTARGETS, GEN, os.path.join(BDIR, 'hybrid'), 'verify')
    cmake_build(BDIR, GEN, VTARGETS, a.jobs)
    # a fresh build invalidates earlier run results
    for p in (RESULTS, P('confirm.json')):
        if os.path.exists(p): os.replace(p, p + '.old')


# ------------------------------------------------------------------ replays
def fixtures(a):
    fx = a.fixtures or sorted(glob.glob(os.path.join(ROOT, 'tests', 'fixtures', 'rec-*')))
    fx = [os.path.abspath(f) for f in fx if os.path.exists(os.path.join(f, 'input.pcjr'))]
    if not fx: sys.exit('no fixtures (tests/fixtures/rec-*/input.pcjr)')
    return fx


def all_mod_vas():
    return [v for kind, _, vs in read_targets() for v in vs if kind == 'mod']


def fixture_mods(fdir):
    """the mods the recording was made with, as a VA list: env.txt CJ_MODS= ('0' = none -> ''); no env.txt means
    the build's default (every mod on)"""
    p = os.path.join(fdir, 'env.txt')
    if not os.path.exists(p): return ','.join(all_mod_vas())
    for l in open(p):
        if l.startswith('CJ_MODS='):
            v = l.strip()[8:]
            return '' if v in ('', '0') else v
    return ','.join(all_mod_vas())


_disp = collections.deque(range(120, 160)); _dlock = threading.Lock()
_gate = threading.BoundedSemaphore(int(os.environ.get('VERIFY_JOBS', '2')))   # replays running at once (2 cores)


def replay(fdir, tag, env):
    """one replay through tools/det/replay.sh on build-verify/pcj; -> (log path, seconds)"""
    with _gate:
        with _dlock: d = _disp.popleft()
        e = dict(os.environ); e.update(env)
        e.update(DISP=f':{d}', NODRAW='1', PCJ_BIN=os.path.join(BDIR, 'pcj'))
        t0 = time.time()
        os.makedirs(P('runs'), exist_ok=True)
        with open(P('runs', tag + '.txt'), 'w') as f:
            sh(['sh', os.path.join(ROOT, 'tools', 'det', 'replay.sh'), fdir, 'linux', 'verify-' + tag], stdout=f, stderr=subprocess.STDOUT, env=e, cwd=ROOT)
        with _dlock: _disp.append(d)
    return os.path.join(ROOT, 'run', 'det', 'verify-' + tag, 'log.txt'), time.time() - t0


RX_VER = re.compile(r'^\[VERIFY\] ([0-9A-F]{8}) (\S+) #(\d+) f=(\d+) (identical|UNREPRODUCIBLE|SKIPPED|DIFFERS: (\d+) RAM ranges \((\d+) inside this\+0x400\), (\d+) bytes, unjudged=\d+(?: ret)?(?: esp=([0-9A-F]{8}))?;(.*))')
RX_RANGE = re.compile(r'([0-9A-F]{8})\+(\d+)')


def parse_log(path):
    """-> dict: end ('matched'|'diverged N'|'crash'|'hang'), calls {va: n} from the last swap report,
    verify {va: {'checked': n, 'differs': [(call, frame, ranges[(addr,len)], regbad, retbad, text)]}}"""
    r = dict(end='crash', calls={}, verify=collections.defaultdict(lambda: dict(checked=0, differs=[])), crash_va=None)
    if not os.path.exists(path): return r
    last_va = None
    for l in open(path, errors='replace'):
        if l.startswith('[SWAP] ') and ' calls=' in l and ' ckpt ' not in l:
            t = l.split()
            r['calls'][t[2]] = int(t[3][6:])
        elif l.startswith('[VERIFY] begin '):
            last_va = l.split()[2]      # the function under dual execution now: blamed for a crash or hang
        elif l.startswith('[VERIFY] '):
            m = RX_VER.match(l.rstrip())
            if not m: continue
            va = m.group(1)
            v = r['verify'][va]; v['checked'] += 1
            if m.group(5) == 'UNREPRODUCIBLE': v['unrep'] = v.get('unrep', 0) + 1; continue
            if m.group(5) == 'SKIPPED': v['checked'] -= 1; v['skipped'] = v.get('skipped', 0) + 1; continue
            if m.group(5) != 'identical':
                rest = m.group(10); esp = int(m.group(9), 16) if m.group(9) else None
                v['differs'].append(dict(call=int(m.group(3)), frame=int(m.group(4)), nranges=int(m.group(6)), near=int(m.group(7)),
                                         bytes=int(m.group(8)), ranges=[(int(x, 16), int(n)) for x, n in RX_RANGE.findall(rest)],
                                         regbad='callee-saved/esp differ' in rest, retbad='return value differs' in rest, text=rest.strip()[:300], esp=esp))
        elif l.startswith('[REC] replay: input ended'):
            r['end'] = 'diverged' if 'DIVERGED' in l else 'matched'
        elif l.startswith('[REC] DIVERGE at frame'):
            r['end'] = 'diverged ' + l.split()[4]
    if r['end'] == 'crash':
        hang = os.path.join(os.path.dirname(path), 'hang.txt')
        if os.path.exists(hang):
            r['end'] = 'hang'
            m = re.search(r'cjn_([0-9A-F]{8})', open(hang, errors='replace').read())
            r['crash_va'] = m.group(1) if m else last_va
        else:
            r['crash_va'] = last_va   # the function under dual execution when the host died
    r['verify'] = dict(r['verify'])
    return r


_ARGB = None


def arg_bytes(va):
    """bytes of stack arguments of a function, from its demangled signature (symbols/functions.csv): the callee's own
    argument slots are dead after it returns, and MSVC code stores into them (SlerpConst writes the clamped t back)"""
    global _ARGB
    if _ARGB is None:
        _ARGB = {}
        for r in csv.DictReader(open(os.path.join(DECOMP, 'symbols', 'functions.csv'))):
            d = r['demangled'] or ''
            m = re.search(r'\((.*)\)', d)
            if not m: continue
            ps = [p.strip() for p in m.group(1).split(',') if p.strip() and p.strip() != 'void']
            n = 0
            for p in ps:
                if '*' in p or '&' in p: n += 4
                elif re.search(r'\b(double|__int64)\b', p): n += 8
                elif re.search(r'\b(float|int|long|short|char|bool|unsigned|signed)\b', p) or p.startswith('enum '): n += 4
                else: n = -1; break        # a struct by value: size unknown, no filtering
            _ARGB[r['va'].upper().zfill(8)] = n
    return _ARGB.get(va, -1)


def in_args(rng, esp, va):
    n = arg_bytes(va)
    if esp is None or n <= 0: return False
    a, l = rng
    return a >= esp + 4 and a + l <= esp + 4 + n


NOISE_PAD = 256   # other threads write ring buffers and counters whose exact position drifts: pad each noisy range


def in_noise(rng, noise):
    a, n = rng
    return any(a >= s - NOISE_PAD and a + n <= s + m + NOISE_PAD for s, m in noise)


def cmd_run(a):
    batch = jload(P('batch.json'))
    if not batch or not os.path.exists(os.path.join(BDIR, 'pcj')): sys.exit('no batch build: run  verify.py build  first')
    os.makedirs(P('runs'), exist_ok=True)
    cands = [c['va'] for c in batch if not DENY_NAME.search(c['name'])]
    res = jload(RESULTS, {}) if not a.fresh else {}
    res.setdefault('fixtures', {}); res.setdefault('dropped', {})
    fx = fixtures(a)
    if a.reparse:   # results from the logs on disk, no replays
        for fdir in fx:
            name = os.path.basename(fdir); out = res['fixtures'].get(name, {})
            for tag in ('self', 'verify'):
                log = os.path.join(ROOT, 'run', 'det', f'verify-{name}-{tag}', 'log.txt')
                if not os.path.exists(log): continue
                p = parse_log(log)
                if tag == 'self':
                    out['noise'] = sorted({tuple(x) for v in p['verify'].values() for d in v['differs'] for x in d['ranges']}); out['noise_end'] = p['end']
                else:
                    out['end'] = p['end']; out['calls'] = {v: p['calls'].get(v, 0) for v in cands}; out['crash_va'] = p['crash_va']; out['verify'] = {}
                    for va in cands:
                        v = p['verify'].get(va)
                        if not v: continue
                        real = [d for d in v['differs'] if d['regbad'] or d['retbad'] or not all(in_noise(r, out.get('noise', [])) or in_args(r, d.get('esp'), va) for r in d['ranges'])]
                        out['verify'][va] = dict(checked=v['checked'], differs=len(v['differs']), real=len(real), unrep=v.get('unrep', 0), skipped=v.get('skipped', 0),
                                                 first=(real or v['differs'] or [None])[0])
            res['fixtures'][name] = out
        jsave(RESULTS, res); summarise(res, cands); return
    cur = {'sel': ','.join(v for v in cands if v not in res['dropped'])}

    def one(fdir):
        name = os.path.basename(fdir); mods = fixture_mods(fdir)
        base = dict(CJ_MODS=mods or '0', CJ_SWAP_VERIFY=cur['sel'], CJ_SWAP_VERIFY_N=str(a.calls), CJ_SWAP_VERIFY_EVERY=str(a.every), CJ_SWAP_VERIFY_KEEP='orig', CJ_SWAP_LOG='0')
        out = res['fixtures'].get(name, {})
        if 'noise' not in out or a.fresh:
            log, dt = replay(fdir, name + '-self', dict(base, CJ_SWAP_VERIFY_SELF='1'))
            p = parse_log(log)
            noise = sorted({tuple(x) for v in p['verify'].values() for d in v['differs'] for x in d['ranges']})
            out['noise'] = noise; out['noise_end'] = p['end']; out['noise_s'] = round(dt)
            print(f'  {name}: noise run {p["end"]} in {dt:.0f}s, {len(noise)} noisy ranges', flush=True)
        log, dt = replay(fdir, name + '-verify', base)
        p = parse_log(log)
        out['end'] = p['end']; out['seconds'] = round(dt); out['calls'] = {v: p['calls'].get(v, 0) for v in cands}
        out['crash_va'] = p['crash_va']
        out['verify'] = {}
        for va in cands:
            v = p['verify'].get(va)
            if not v: continue
            real = [d for d in v['differs'] if d['regbad'] or d['retbad'] or not all(in_noise(r, out['noise']) or in_args(r, d.get('esp'), va) for r in d['ranges'])]
            out['verify'][va] = dict(checked=v['checked'], differs=len(v['differs']), real=len(real), unrep=v.get('unrep', 0), skipped=v.get('skipped', 0),
                                     first=(real or v['differs'] or [None])[0])
        print(f'  {name}: verify run {p["end"]} in {dt:.0f}s, {len(out["verify"])} functions checked, '
              f'{sum(1 for x in out["verify"].values() if x["real"])} differ', flush=True)
        return name, out

    # a host crash inside a candidate ends the run for every function after it: drop the culprit and run that fixture again
    res['calls_n'] = a.calls
    todo = [f for f in fx if a.fresh or res['fixtures'].get(os.path.basename(f), {}).get('end') != 'matched']
    for _ in range(40):
        if not todo: break
        with cf.ThreadPoolExecutor(a.jobs) as ex:
            for name, out in ex.map(one, todo):
                res['fixtures'][name] = out; jsave(RESULTS, res)
        a.fresh = False
        todo = []
        for f in fx:
            o = res['fixtures'].get(os.path.basename(f), {})
            if o.get('end') not in ('crash', 'hang'): continue
            va = o.get('crash_va')
            if va and va in cands and va not in res['dropped']:
                res['dropped'][va] = f'{o["end"]} in {os.path.basename(f)} while under dual execution'
                print(f'  dropped {va}: {res["dropped"][va]}', flush=True)
                todo.append(f)
            else:
                print(f'  {os.path.basename(f)}: {o["end"]} with no candidate to blame: see run/det/verify-{os.path.basename(f)}-verify/log.txt', flush=True)
        cur['sel'] = ','.join(v for v in cands if v not in res['dropped'])
    jsave(RESULTS, res)
    summarise(res, cands)



def verdicts(res, cands):
    """-> {va: (verdict, calls total, checked total, real differs total)}"""
    out = {}
    for va in cands:
        if va in res.get('dropped', {}): out[va] = ('CRASH', 0, 0, 0); continue
        calls = checked = real = unrep = skipped = 0
        for f in res['fixtures'].values():
            calls += f.get('calls', {}).get(va, 0)
            v = f.get('verify', {}).get(va)
            if v: checked += v['checked']; real += v['real']; unrep += v.get('unrep', 0); skipped += v.get('skipped', 0)
        if calls == 0: out[va] = ('UNREACHED', 0, 0, 0)
        elif real: out[va] = ('DIFFERS', calls, checked, real)
        elif checked > unrep: out[va] = ('IDENTICAL', calls, checked, 0)
        elif unrep: out[va] = ('UNREPRODUCIBLE', calls, checked, 0)
        elif skipped: out[va] = ('HOST_EFFECTS', calls, 0, 0)
        else: out[va] = ('UNCHECKED', calls, 0, 0)
    return out


def summarise(res, cands):
    v = verdicts(res, cands)
    c = collections.Counter(x[0] for x in v.values())
    print('run: ' + '  '.join(f'{k}={n}' for k, n in sorted(c.items())))


# ------------------------------------------------------------------ confirm (native kept, whole replays)
def cmd_confirm(a):
    batch = jload(P('batch.json')); res = jload(RESULTS)
    if not res: sys.exit('no results: run  verify.py run  first')
    cands = [c['va'] for c in batch if not DENY_NAME.search(c['name'])]
    good = [va for va, (vd, *_) in verdicts(res, cands).items() if vd == 'IDENTICAL']
    conf = jload(P('confirm.json'), {}) if not a.fresh else {}
    conf.setdefault('fail', {}); conf.setdefault('pass', [])
    fx = fixtures(a)
    print(f'confirm: {len(good)} identical candidates, {len(fx)} fixtures', flush=True)

    def run_set(vas, fdir, tag):
        name = os.path.basename(fdir); mods = fixture_mods(fdir)
        sel = ','.join([x for x in [mods] if x] + vas)
        log, dt = replay(fdir, f'{name}-{tag}', dict(CJ_MODS=sel, CJ_SWAP_LOG='0'))
        p = parse_log(log)
        return p['end'] == 'matched', p, dt

    def peel(vas, fdir, depth=0):
        """the subset of vas that replays exactly on this fixture; failures go to conf['fail']"""
        name = os.path.basename(fdir)
        tag = f'c{depth}-{abs(hash(tuple(vas))) % 100000}'
        ok, p, dt = run_set(vas, fdir, tag)
        print(f'  {name}: {len(vas)} native -> {p["end"]} ({dt:.0f}s)', flush=True)
        if ok: return list(vas)
        if len(vas) == 1:
            conf['fail'][vas[0]] = f'{p["end"]} in {name}'; return []
        if p['crash_va'] and p['crash_va'] in vas:
            conf['fail'][p['crash_va']] = f'{p["end"]} in {name}'
            return peel([v for v in vas if v != p['crash_va']], fdir, depth + 1)
        h = len(vas) // 2
        with cf.ThreadPoolExecutor(2) as ex:
            l, r = ex.map(lambda s: peel(s, fdir, depth + 1), [vas[:h], vas[h:]])
        # the halves may pass alone but not together (order effects): check the union once more
        if len(l) + len(r) < len(vas) and l and r:
            ok2, p2, _ = run_set(l + r, fdir, f'c{depth}-u{abs(hash(tuple(l + r))) % 100000}')
            if not ok2:
                conf['fail'].update({v: f'{p2["end"]} in {name} (only together)' for v in r}); return l
        return l + r

    live = [v for v in good if v not in conf['fail']]
    for fdir in fx:
        live = peel(live, fdir)
        jsave(P('confirm.json'), conf)
    conf['pass'] = live
    conf['fixtures'] = [os.path.basename(f) for f in fx]
    jsave(P('confirm.json'), conf)
    print(f'confirmed: {len(live)} pass every fixture natively, {len(conf["fail"])} fail')


# ------------------------------------------------------------------ report
def cmd_report(a):
    batch = jload(P('batch.json')) or []; res = jload(RESULTS) or {'fixtures': {}}
    conf = jload(P('confirm.json'), {'pass': [], 'fail': {}, 'fixtures': []})
    cands = [c['va'] for c in batch if not DENY_NAME.search(c['name'])]; meta = {c['va']: c for c in batch}
    v = verdicts(res, cands)
    rows = []
    for va in cands:
        vd, calls, checked, real = v[va]
        final = vd
        if vd == 'IDENTICAL':
            final = 'REPLAY_VERIFIED' if va in conf['pass'] else ('DIVERGES' if va in conf['fail'] else 'IDENTICAL')
        rows.append((final, va, meta[va]['unit'], meta[va]['size'], calls, checked, real, meta[va]['name']))
    order = {'REPLAY_VERIFIED': 0, 'IDENTICAL': 1, 'DIVERGES': 2, 'DIFFERS': 3, 'CRASH': 4, 'UNREPRODUCIBLE': 5, 'HOST_EFFECTS': 6, 'UNCHECKED': 7, 'UNREACHED': 8}
    rows.sort(key=lambda r: (order.get(r[0], 9), r[2], -r[3]))
    print(f'{"verdict":16} {"va":8} {"unit":22} {"size":>5} {"calls":>6} {"chk":>4} {"dif":>4}  name')
    for r in rows: print(f'{r[0]:16} {r[1]} {r[2][:22]:22} {r[3]:5d} {r[4]:6d} {r[5]:4d} {r[6]:4d}  {r[7][:60]}')
    c = collections.Counter(r[0] for r in rows)
    print('\n' + '  '.join(f'{k}={n}' for k, n in sorted(c.items(), key=lambda x: order.get(x[0], 9))))
    for va, why in conf['fail'].items(): print(f'  {va} diverges: {why}')
    for va, why in res.get('dropped', {}).items(): print(f'  {va} crash: {why}')
    with open(P('report.csv'), 'w') as f:
        w = csv.writer(f); w.writerow(['verdict', 'va', 'unit', 'size', 'calls', 'checked', 'differs', 'name'])
        for r in rows: w.writerow(r)
    if a.write_overrides:
        ov = os.path.join(DECOMP, 'status', 'verify_overrides.csv')
        have = {r['va'].lower() for r in csv.DictReader(open(ov))} if os.path.exists(ov) else set()
        n = 0; today = datetime.date.today().isoformat()
        fxs = ','.join(conf['fixtures'])
        with open(ov, 'a', newline='') as f:
            w = csv.writer(f)
            for r in rows:
                if r[0] != 'REPLAY_VERIFIED' or r[1].lower() in have: continue
                per = ' '.join(f'{k}:{res["fixtures"][k]["calls"].get(r[1], 0)}' for k in conf['fixtures'] if k in res['fixtures'])
                ev = (f'tools/verify.py (pcj-native): dual execution identical on {r[5]} calls (first {res.get('calls_n', '?')} per fixture, RAM+regs vs the '
                      f'recompiled original, other-thread noise excluded), and every fixture replays exact with it native; '
                      f'calls per fixture {per}; fixtures {fxs}')
                w.writerow([r[1].lower(), 'REPLAY_VERIFIED', ev, today]); n += 1
        print(f'wrote {n} REPLAY_VERIFIED rows to {ov}')


def cmd_status(a):
    for name, p in [('scan', P('scan.txt')), ('plan', PLAN), ('clean', CLEAN), ('batch', P('batch.json')), ('results', RESULTS), ('confirm', P('confirm.json'))]:
        if os.path.exists(p): print(f'{name:8} {datetime.datetime.fromtimestamp(os.path.getmtime(p)):%Y-%m-%d %H:%M}  {p}')
        else: print(f'{name:8} -')
    if os.path.exists(os.path.join(BDIR, 'pcj')): print(f'binary   {datetime.datetime.fromtimestamp(os.path.getmtime(os.path.join(BDIR, "pcj"))):%Y-%m-%d %H:%M}  {BDIR}/pcj')
    batch = jload(P('batch.json')); res = jload(RESULTS)
    if batch and res: summarise(res, [c['va'] for c in batch])
    conf = jload(P('confirm.json'))
    if conf: print(f'confirm: {len(conf.get("pass", []))} pass, {len(conf.get("fail", {}))} fail')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    s = sp.add_parser('plan'); s.add_argument('--units', nargs='*'); s.add_argument('--exclude-units', nargs='*')
    s.add_argument('--min-size', type=int, default=0); s.add_argument('--max', type=int, default=0)
    s.add_argument('--match', action='store_true', help='also byte-matched functions not yet promoted')
    s = sp.add_parser('build'); s.add_argument('--jobs', type=int, default=2); s.add_argument('--recheck', action='store_true')
    s = sp.add_parser('run'); s.add_argument('--fixtures', nargs='*'); s.add_argument('--calls', type=int, default=16); s.add_argument('--every', type=int, default=100)
    s.add_argument('--jobs', type=int, default=2); s.add_argument('--fresh', action='store_true'); s.add_argument('--reparse', action='store_true')
    s = sp.add_parser('confirm'); s.add_argument('--fixtures', nargs='*'); s.add_argument('--jobs', type=int, default=2); s.add_argument('--fresh', action='store_true')
    s = sp.add_parser('report'); s.add_argument('--write-overrides', action='store_true')
    s = sp.add_parser('status')
    a = ap.parse_args()
    os.makedirs(WORK, exist_ok=True)
    globals()['cmd_' + a.cmd](a)
