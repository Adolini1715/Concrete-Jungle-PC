#!/usr/bin/env python3
"""Batch promotion of byte-matched functions to native (docs/promote.md).

  tools/promote.py scan                 hybridize scan -v over every pcj-decomp unit -> work/promote/scan.txt
  tools/promote.py compilecheck         per unit: gen + c++ -fsyntax-only of every translatable function; bisects
                                        failing units -> work/promote/clean.txt
  tools/promote.py reach                CJ_DET boot under gdb tbreaks on every candidate -> work/promote/reached.txt
  tools/promote.py batch -n N           pick N (reached AND clean) candidates, write work/promote/targets-batch.txt,
                                        run hybridize + tools.recomp ONCE (work/gen-batch), configure + build build-batch
  tools/promote.py test                 baseline x2, then bisection over groups (2 runs at a time) ->
                                        tools/hybridize/promote-results.json / .csv
  tools/promote.py promote              passing functions -> targets.txt, regen src/recomp/gen, rebuild build,
                                        300-frame CJ_DET all-on vs all-off on the promoted set, bisect + drop offenders
  tools/promote.py status               print the results table summary

Everything is resumable: each step reads/writes files under work/promote and tools/hybridize.
No git commits are made by this tool.
"""
import argparse, collections, concurrent.futures as cf, glob, hashlib, json, os, re, shutil, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HY = os.path.join(ROOT, 'tools', 'hybridize', 'hybridize.py')
WORK = os.path.join(ROOT, 'work', 'promote')
DECOMP = os.environ.get('PCJ_DECOMP', '/home/claude/pcj-decomp')
DISC = os.environ.get('DISC', '/home/claude/pcj/retail/disc_full')
TARGETS = os.path.join(ROOT, 'tools', 'hybridize', 'targets.txt')
RESULTS = os.path.join(ROOT, 'tools', 'hybridize', 'promote-results.json')
# CJ_DET wrappers that tools.recomp must also keep (src/det_file.c); never promotion candidates.
DET_WRAPPED = {'00284E60', '00221400', '002EE440', '00356BA9', '00356D49'}
STALL = int(os.environ.get('PROMOTE_STALL', '100'))
FRAMES = int(os.environ.get('PROMOTE_FRAMES', '300'))


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), **kw)


def read_targets(path=TARGETS):
    """-> OrderedDict unit -> [VA upper]"""
    d = collections.OrderedDict()
    for l in open(path):
        l = l.split('#')[0].strip()
        if not l: continue
        t = l.split()
        assert t[0].startswith('unit='), l
        d.setdefault(t[0][5:], []).extend(v.upper().zfill(8) for v in t[1:])
    return d


def write_targets(d, path, header=''):
    with open(path, 'w') as f:
        f.write(header)
        for u, vas in d.items():
            if vas: f.write(f'unit={u} ' + ' '.join(v.lower() for v in vas) + '\n')


def core_vas():
    return [v for vs in read_targets().values() for v in vs]


def load_scan():
    """-> list of dict(va, size, flags, name, unit) for OK lines"""
    out = []
    for l in open(os.path.join(WORK, 'scan.txt')):
        if not l.startswith('OK  '): continue
        t = l.split()
        out.append(dict(va=t[1].upper().zfill(8), size=int(t[2]), flags=t[3], name=t[4], unit=t[-1][1:]))
    return out


# ------------------------------------------------------------------ scan
def cmd_scan(a):
    os.makedirs(WORK, exist_ok=True)
    units = sorted(os.path.basename(p)[:-4] for p in glob.glob(DECOMP + '/src/*/*.cpp'))
    with open(os.path.join(WORK, 'scan.txt'), 'w') as f:
        sh([sys.executable, HY, 'scan', '-v'] + units, stdout=f, cwd=ROOT)


# ------------------------------------------------------------------ compile check
def cxx_flags():
    """the real compile flags of the hybrid sources, from an existing build dir"""
    fm = open(os.path.join(ROOT, 'build', 'CMakeFiles', 'pcj.dir', 'flags.make')).read()
    inc = re.search(r'^CXX_INCLUDES = (.*)$', fm, re.M).group(1).split()
    dfn = re.search(r'^CXX_DEFINES = (.*)$', fm, re.M).group(1).split()
    return ['c++', '-std=gnu++17', '-fsyntax-only', '-w'] + dfn + inc


def gen_and_compile(unit, vas, tag):
    d = os.path.join(WORK, 'cc', f'{unit}_{tag}')
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    r = sh([sys.executable, HY, 'gen', 'unit=' + unit] + [v.lower() for v in vas] + ['-o', d], capture_output=True, text=True, cwd=ROOT)
    if r.returncode: return False, 'gen: ' + (r.stderr.strip().splitlines() or ['?'])[-1][:200]
    r = sh(cxx_flags() + glob.glob(d + '/hy_*.cpp'), capture_output=True, text=True)
    shutil.rmtree(d, ignore_errors=True)
    if r.returncode:
        e = next((l for l in r.stderr.splitlines() if 'error' in l), '?')
        return False, 'compile: ' + e.split('error', 1)[-1].strip()[:200]
    return True, ''


def bisect_unit(unit, vas, res, depth=0):
    ok, why = gen_and_compile(unit, vas, 'b')
    if ok:
        for v in vas: res[v] = ''
        return
    if len(vas) == 1:
        res[vas[0]] = why; return
    h = len(vas) // 2
    bisect_unit(unit, vas[:h], res, depth + 1); bisect_unit(unit, vas[h:], res, depth + 1)


def cmd_compilecheck(a):
    cands = load_scan()
    skip = set(core_vas()) | DET_WRAPPED
    only = set(open(a.only).read().split()) if a.only else None
    by_unit = collections.OrderedDict()
    for c in cands:
        if c['va'] in skip or (only and c['va'] not in only): continue
        by_unit.setdefault(c['unit'], []).append(c['va'])
    out = os.path.join(WORK, 'clean.json')
    res = json.load(open(out)) if os.path.exists(out) else {}
    todo = [(u, v) for u, v in by_unit.items() if not all(x in res for x in v)]
    print(f'{len(todo)} units to check', flush=True)
    lock = threading.Lock()

    def work(item):
        u, v = item; r = {}
        bisect_unit(u, v, r)
        with lock:
            res.update(r); json.dump(res, open(out, 'w'), indent=0)
        print(f'{u}: {sum(1 for x in r.values() if not x)}/{len(r)} clean', flush=True)
    with cf.ThreadPoolExecutor(a.jobs) as ex: list(ex.map(work, todo))
    n = sum(1 for x in res.values() if not x)
    print(f'clean: {n} / {len(res)}')
    reasons = collections.Counter(re.sub(r"'[^']*'|\b\d+\b", 'X', x)[:70] for x in res.values() if x)
    for k, v in reasons.most_common(12): print(f'  {v:4d} {k}')


# ------------------------------------------------------------------ reachability (gdb tbreaks)
def run_env(extra=None):
    e = dict(os.environ)
    e.update(RECOMP_AC97_READY='1', RECOMP_TITLE_KEVENTS='1', RECOMP_VBLANK='1', RECOMP_PB_EXEC='1',
             CJ_DET='1', CJ_LOCKSTEP='2', CJ_STOP_FRAME=str(FRAMES), CJ_DET_HASH='1', RECOMP_WATCHDOG_SECS='800',
             CJ_DET_HASH_CONT=os.environ.get('PROMOTE_CONT', '0'))   # contiguous memory differs run to run (host GPU writes)
    if extra: e.update(extra)
    return e


def nm_symbols(binary):
    out = sh(['nm', binary], capture_output=True, text=True).stdout
    m = {}
    for l in out.splitlines():
        t = l.split()
        if len(t) == 3 and t[1] in 'Tt':
            x = re.search(r'_([0-9A-F]{8})$', t[2])
            if x and (t[2].startswith('m_') or t[2].startswith('sub_')) and not t[2].endswith(('_gen', '_enter')):
                m.setdefault(x.group(1), t[2])
    return m


def cmd_reach(a):
    binary = os.path.join(ROOT, a.bin)
    syms = nm_symbols(binary)
    skip = set(core_vas()) | DET_WRAPPED
    vas = [c['va'] for c in load_scan() if c['va'] not in skip]
    if a.only: vas = [v for v in vas if v in set(open(a.only).read().split())]
    miss = [v for v in vas if v not in syms]
    vas = [v for v in vas if v in syms]
    print(f'{len(vas)} candidates with a host symbol ({len(miss)} without)')
    sc = os.path.join(WORK, 'reach.gdb')
    with open(sc, 'w') as f:
        f.write('set pagination off\nset confirm off\nset disable-randomization off\nset breakpoint pending off\n'
                'handle SIGSEGV SIGBUS SIGUSR1 SIGUSR2 SIGALRM SIGPIPE nostop noprint pass\n')
        for v in vas:
            f.write(f'tbreak {syms[v]}\ncommands\nsilent\nprintf "HIT {v}\\n"\ncontinue\nend\n')
        f.write('run\nquit\n')
    d = os.path.join(WORK, 'reach'); shutil.rmtree(d, ignore_errors=True); os.makedirs(d + '/save')
    log = os.path.join(WORK, 'reach.log')
    env = run_env(dict(RECOMP_FB_DUMP=d + '/f', RECOMP_FB_DUMP_FLIPS='1', CJ_NATIVE_SWAP='0'))
    t0 = time.time()
    with open(log, 'w') as f:
        sh(['gdb', '-q', '-batch', '-x', sc, '--args', binary, DISC, d + '/save'], stdout=f, stderr=subprocess.STDOUT, env=env, cwd=ROOT)
    shutil.rmtree(d, ignore_errors=True)
    hit = sorted({l.split()[1] for l in open(log, errors='replace') if l.startswith('HIT ')})
    open(os.path.join(WORK, 'reached.txt'), 'w').write('\n'.join(hit) + '\n')
    print(f'reached {len(hit)} of {len(vas)} in {time.time()-t0:.0f}s')


# ------------------------------------------------------------------ batch (one regeneration)
def merge_by_unit(core, extra):
    d = collections.OrderedDict((u, list(v)) for u, v in core.items())
    for u, v in extra.items():
        d.setdefault(u, [])
        d[u] += [x for x in v if x not in d[u]]
    return d


def regen(targets_file, gen_dir, hy_dir, label):
    """RUN.md: hybridize gen + tools.recomp (--exclude-manual = hybrid_swap.c + det_file.c). ~3 min."""
    d = read_targets(targets_file)
    spec = [t for u, vs in d.items() for t in ['unit=' + u] + [v.lower() for v in vs]]
    os.makedirs(hy_dir, exist_ok=True)
    r = sh([sys.executable, HY, 'gen'] + spec + ['-o', hy_dir], cwd=ROOT)
    if r.returncode: sys.exit('hybridize gen failed')
    with open(os.path.join(hy_dir, 'exclude_manual.c'), 'wb') as f:
        f.write(open(os.path.join(hy_dir, 'hybrid_swap.c'), 'rb').read() + open(os.path.join(ROOT, 'src', 'det_file.c'), 'rb').read())
    shutil.rmtree(gen_dir, ignore_errors=True)
    tk = os.path.join(ROOT, 'xboxrecomp')
    cmd = [sys.executable, '-m', 'tools.recomp', '../game/default.xbe', '--all', '--split', '250', '--game-name', 'Predator: Concrete Jungle',
           '--gen-dir', gen_dir, '--disasm-dir', '../work/disasm', '--func-id-dir', '../work/func_id', '--abi-dir', '../work/abi',
           '-o', f'../work/recomp-{label}', '--manual-functions', '../game/manual_functions.json',
           '--exclude-manual', os.path.join(hy_dir, 'exclude_manual.c')]
    t0 = time.time()
    r = sh(cmd, cwd=tk, stdout=open(os.path.join(WORK, f'regen-{label}.log'), 'w'), stderr=subprocess.STDOUT)
    if r.returncode: sys.exit('tools.recomp failed, see work/promote/regen-%s.log' % label)
    print(f'regen {label}: {time.time()-t0:.0f}s, {len(glob.glob(gen_dir + "/*.c"))} files')


def cmake_build(bdir, gen_dir, targets_file, jobs=2):
    sh(['cmake', '-S', ROOT, '-B', bdir, '-DRECOMP_ABI_CHECK=ON', f'-DRECOMP_GEN_DIR={gen_dir}', f'-DCJ_TARGETS={targets_file}'],
       check=True, stdout=open(os.path.join(WORK, 'cmake-' + os.path.basename(bdir) + '.log'), 'w'), stderr=subprocess.STDOUT)
    r = sh(['cmake', '--build', bdir, f'-j{jobs}'], stdout=open(os.path.join(WORK, 'build-' + os.path.basename(bdir) + '.log'), 'w'), stderr=subprocess.STDOUT)
    if r.returncode: sys.exit('build failed: see work/promote/build-%s.log' % os.path.basename(bdir))


def cmd_batch(a):
    clean = json.load(open(os.path.join(WORK, 'clean.json')))
    reached = set(open(os.path.join(WORK, 'reached.txt')).read().split())
    skip = set(core_vas()) | DET_WRAPPED
    meta = {c['va']: c for c in load_scan()}
    pool = [meta[v] for v in sorted(reached) if v in meta and v not in skip and clean.get(v, 'x') == '' and meta[v]['size'] >= a.min_size]
    print(f'pool: {len(pool)} reached and compile-clean')
    # round robin over units (largest first inside a unit) so that one broken unit cannot sink the batch
    byu = collections.OrderedDict()
    for c in sorted(pool, key=lambda c: -c['size']): byu.setdefault(c['unit'], []).append(c)
    pick = []
    while len(pick) < a.n and any(byu.values()):
        for u in list(byu):
            if byu[u] and len(pick) < a.n: pick.append(byu[u].pop(0))
    extra = collections.OrderedDict()
    for c in pick: extra.setdefault(c['unit'], []).append(c['va'])
    tb = os.path.join(WORK, 'targets-batch.txt')
    write_targets(merge_by_unit(read_targets(), extra), tb, '# promote.py batch: promoted targets + candidates (unit groups)\n')
    json.dump([dict(va=c['va'], unit=c['unit'], name=c['name'], size=c['size']) for c in pick], open(os.path.join(WORK, 'batch.json'), 'w'), indent=0)
    print(f'batch: {len(pick)} candidates, {sum(c["size"] for c in pick)} bytes in {len(extra)} units')
    gen = os.path.join(ROOT, 'work', 'gen-batch'); bdir = os.path.join(ROOT, 'build-batch')
    regen(tb, gen, os.path.join(bdir, 'hybrid'), 'batch')
    cmake_build(bdir, gen, tb)


# ------------------------------------------------------------------ runs and comparison
def parse_log(path):
    det, rep, calls = {}, {}, {}
    bad = collections.Counter()
    div = None
    for l in open(path, errors='replace'):
        m = re.match(r'\[DET\] f=(\d+) data=(\w+) heap=(\w+)(?: cont=(\w+))?', l)
        if m: det[int(m.group(1))] = (m.group(2), m.group(3), m.group(4)); continue
        m = re.match(r'\[SWAP\] (final|f\d+|diverge) (\w{8}) calls=(\d+) enter\(gen body\)=(\d+) esp_bad=(\d+) csr_bad=(\d+) run_hash=(\w+)', l)
        if m:
            rep[(m.group(1), m.group(2))] = (int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)), m.group(7)); continue
        m = re.match(r'\[DET\] DIVERGE f=(\d+) region=(\w+)', l)
        if m: div = (int(m.group(1)), m.group(2)); continue
        if 'FATAL' in l or '[HYB]' in l: bad['fatal_hyb'] += 1
        elif l.startswith('[ABI]'): bad['abi:' + l.strip()[:100]] += 1
    stopped = any(l.startswith('[DET] stop at frame') for l in open(path, errors='replace'))
    return dict(det=det, rep=rep, bad=bad, div=div, stopped=stopped)


def swap_calls(path, keep_order=False):
    """logged wrapper calls: (VA, n) -> (pre, post, ret)"""
    out = [] if keep_order else {}
    for l in open(path, errors='replace'):
        m = re.match(r'\[SWAP\] (\w{8}) #(\d+) f=\d+ this=\w+ ret=(\w+) esp\S+ pre=(\w+) post=(\w+)', l)
        if m:
            kv = ((m.group(1), int(m.group(2))), (m.group(4), m.group(5), m.group(3)))
            if keep_order: out.append(kv)
            else: out[kv[0]] = kv[1]
    return out


def frame_hashes(d):
    out = {}
    for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        h = hashlib.md5(); h.update(open(os.path.join(d, f), 'rb').read()); out[f] = h.hexdigest()
    return out


class Runner:
    def __init__(self, bdir, ref_tag=None):
        self.bin = os.path.join(bdir, 'pcj'); self.ref_tag = ref_tag
        self.ab = os.path.join(ROOT, 'run', 'promote'); os.makedirs(self.ab, exist_ok=True)
        self.core = core_vas()

    def run(self, tag, on, early=True):
        """on = candidate VAs that run native (core is always on). Returns the run dir."""
        d = os.path.join(self.ab, tag); shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d + '/save'); os.makedirs(d + '/frames')
        if os.path.isdir(os.path.join(ROOT, 'run', 'save')): sh(['cp', '-r', os.path.join(ROOT, 'run', 'save') + '/.', d + '/save/'])
        sel = ','.join(self.core + list(on))
        extra = dict(CJ_NATIVE_SWAP=sel, RECOMP_FB_DUMP=d + '/frames/f', RECOMP_FB_DUMP_FLIPS='1')
        if early and self.ref_tag: extra['CJ_DET_REF'] = os.path.join(self.ab, self.ref_tag, 'log')
        logp = os.path.join(d, 'log')
        with open(logp, 'w') as f:
            pr = subprocess.Popen([self.bin, DISC, d + '/save'], stdout=f, stderr=subprocess.STDOUT, env=run_env(extra), cwd=ROOT)
            last, t_last = -1, time.time()
            while pr.poll() is None:
                time.sleep(2)
                # stall = no new "[DET] f=" line for STALL seconds (a hung boot or an endless loop in a native body)
                n = sum(1 for l in open(logp, errors='replace') if l.startswith('[DET] f='))
                if n != last: last, t_last = n, time.time()
                elif time.time() - t_last > STALL:
                    f.flush()
                    # every thread writes its active native chain (guest_rt nat_usr1, "[HYB] STALL native=cjn_<VA> chain=..."): hang blame
                    try:
                        import ctypes; libc = ctypes.CDLL(None, use_errno=True)
                        for tid in os.listdir(f'/proc/{pr.pid}/task'): libc.syscall(234, pr.pid, int(tid), 10)   # tgkill(pid, tid, SIGUSR1)
                        time.sleep(1)
                    except Exception: pass
                    bt = sh(['gdb', '-q', '-batch', '-p', str(pr.pid), '-ex', 'thread apply all bt 30'], capture_output=True, text=True).stdout
                    pr.kill(); pr.wait(); f.write('\n[PROMOTE] stalled, killed\n' + bt); break
        return d, pr.returncode

    def judge(self, d, rc, base_tag):
        """-> (verdict dict). Pass criteria: docs/promote.md"""
        b = parse_log(os.path.join(self.ab, base_tag, 'log')); r = parse_log(os.path.join(d, 'log'))
        v = dict(ok=True, why='', frame=None, region=None)
        if r['div']:
            v.update(ok=False, why='DIVERGE', frame=r['div'][0], region=r['div'][1]); return v, r
        if not r['stopped'] or rc != 0:
            v.update(ok=False, why=f'no clean stop (rc={rc})', frame=max(r['det'], default=0), region='crash/hang'); return v, r
        for f in sorted(set(b['det']) & set(r['det'])):
            if b['det'][f][0] != r['det'][f][0]: v.update(ok=False, why='data hash', frame=f, region='data'); return v, r
            if f != 1 and b['det'][f][1] != r['det'][f][1]: v.update(ok=False, why='heap hash', frame=f, region='heap'); return v, r
            if b['det'][f][2] and r['det'][f][2] and b['det'][f][2] != r['det'][f][2]:
                v.update(ok=False, why='contiguous memory hash', frame=f, region='cont'); return v, r
        if set(b['det']) != set(r['det']): v.update(ok=False, why='frame count', frame=None, region='frames'); return v, r
        fb, fr = frame_hashes(os.path.join(self.ab, base_tag, 'frames')), frame_hashes(os.path.join(d, 'frames'))
        diff = [f for f in sorted(set(fb) & set(fr)) if fb[f] != fr[f]]
        if diff or len(fb) != len(fr):
            v.update(ok=False, why=f'frames differ ({len(diff)}), counts {len(fb)}/{len(fr)}', frame=int(re.sub(r'\D', '', diff[0]) or 0) if diff else None, region='image'); return v, r
        for k, bv in b['rep'].items():
            rv = r['rep'].get(k)
            if rv is None or rv[0] != bv[0]: v.update(ok=False, why=f'call count {k[1]}@{k[0]}: {bv[0]} vs {rv and rv[0]}', frame=None, region='calls'); return v, r
        for k, rv in r['rep'].items():
            if rv[2] or rv[3]: v.update(ok=False, why=f'esp/csr failure {k[1]}', frame=None, region='abi'); return v, r
        if r['bad'].get('fatal_hyb'): v.update(ok=False, why='FATAL/[HYB]', region='log'); return v, r
        if set(r['bad']) - set(b['bad']): v.update(ok=False, why='new [ABI] report', region='log'); return v, r
        return v, r


def cleanup(d, keep_log=True):
    shutil.rmtree(os.path.join(d, 'frames'), ignore_errors=True); shutil.rmtree(os.path.join(d, 'save'), ignore_errors=True)


# ------------------------------------------------------------------ test (bisection)
def save_results(res, meta):
    rows = []
    for va, m in meta.items():
        r = dict(va=va, **m); r.update(res.get(va, {}))
        rows.append(r)
    json.dump(rows, open(RESULTS, 'w'), indent=1)
    with open(RESULTS[:-5] + '.csv', 'w') as f:
        f.write('va,name,unit,size,status,calls,first_divergent_frame,region,detail,group_runs\n')
        for r in rows:
            f.write(','.join(str(x).replace(',', ';') for x in (r['va'], r['name'], r['unit'], r['size'], r.get('status', ''), r.get('calls', ''),
                    r.get('frame', ''), r.get('region', ''), r.get('detail', ''), r.get('runs', ''))) + '\n')


def cmd_regen_batch(a):
    """rebuild build-batch from the existing targets-batch.txt (after a hybridize.py / runtime change)"""
    tb = os.path.join(WORK, 'targets-batch.txt')
    gen = os.path.join(ROOT, 'work', 'gen-batch'); bdir = os.path.join(ROOT, 'build-batch')
    regen(tb, gen, os.path.join(bdir, 'hybrid'), 'batch')
    cmake_build(bdir, gen, tb)


def cmd_test(a):
    global RESULTS
    if a.out: RESULTS_IN = RESULTS; shutil.copy(RESULTS, a.out); RESULTS = a.out
    bdir = os.path.join(ROOT, a.build)
    batch = json.load(open(os.path.join(WORK, 'batch.json')))
    meta = collections.OrderedDict((c['va'], dict(name=c['name'], unit=c['unit'], size=c['size'])) for c in batch)
    res = {}
    R = Runner(bdir)
    t0 = time.time()
    # baseline x2 (all candidates off, core on): identical or the A/B is meaningless
    da = os.path.join(R.ab, 'base_a')
    if a.reuse_base and os.path.exists(da + '/log') and os.path.isdir(da + '/frames'):
        A = parse_log(da + '/log'); print('reusing the baselines of an earlier invocation', flush=True)
    else:
        with cf.ThreadPoolExecutor(2) as ex:
            fa = ex.submit(R.run, 'base_a', [], False); fb = ex.submit(R.run, 'base_b', [], False)
            (da, ra), (db, rb) = fa.result(), fb.result()
        A, B = parse_log(os.path.join(da, 'log')), parse_log(os.path.join(db, 'log'))
        same = all(A['det'].get(f, (0, 0, 0))[0] == B['det'].get(f, (1, 1, 1))[0] and (f == 1 or A['det'][f][1] == B['det'][f][1])
                   and A['det'][f][2] == B['det'][f][2] for f in A['det']) and A['det'].keys() == B['det'].keys()
        fh = frame_hashes(da + '/frames') == frame_hashes(db + '/frames')
        print(f'baselines: rc {ra},{rb}; frames {len(A["det"])}/{len(B["det"])}; hashes identical={same}; images identical={fh}; {time.time()-t0:.0f}s', flush=True)
        if not (same and fh): sys.exit('baseline is not deterministic; aborting')
        cleanup(db)
    R.ref_tag = 'base_a'
    calls = {va: A['rep'].get(('final', va), (0,))[0] for va in meta}
    for va in meta:
        if calls[va] == 0: res[va] = dict(status='NOT_REACHED', calls=0)
        else: res[va] = dict(status='PENDING', calls=calls[va])
    live = [va for va in meta if calls[va] > 0]
    if a.prefail:   # keep the FAIL verdicts of an earlier invocation, except constructors (re-tested after the ctor fix)
        for x in json.load(open(a.prefail)):
            parts = x['name'].split('::')
            ctor = len(parts) >= 2 and parts[-1] == parts[-2]
            if x.get('status') == 'FAIL' and not ctor and x['va'] in live:
                res[x['va']].update(status='FAIL', frame=x.get('frame'), region=x.get('region'), detail='(earlier run) ' + str(x.get('detail', '')), runs=x.get('runs'))
                live.remove(x['va'])
        print(f'{len(live)} candidates left to test after the earlier verdicts', flush=True)
    if a.skip_pass:   # earlier PASS verdicts stand (those functions are in targets.txt, i.e. on in every run now)
        for x in json.load(open(RESULTS)):
            if x.get('status') == 'PASS' and x['va'] in live:
                res[x['va']].update(status='PASS', runs=x.get('runs')); live.remove(x['va'])
    for x in a.exclude:   # VA:reason -> FAIL without testing (e.g. a function that hangs the boot)
        va, _, why = x.partition(':'); va = va.upper().zfill(8)
        if va in live: live.remove(va)
        if va in res: res[va].update(status='FAIL', frame=0, region='hang', detail=why, runs='excluded')
    if a.only:
        only = {v.upper().zfill(8) for v in a.only}
        for va in list(live):
            if va not in only: live.remove(va); res[va]['status'] = 'UNTESTED'
    print(f'{len(live)} reached in the baseline run, {len(meta) - len(live)} NOT_REACHED or kept', flush=True)
    save_results(res, meta)
    cnt = [0]

    lock = threading.Lock()
    base_swap = swap_calls(os.path.join(R.ab, 'base_a', 'log'))

    def culprit(d, g, rc):
        """(1) a [HYB] / FATAL report names the native function that made the offending call
        (`native=cjn_<VA> chain=<outer>,...,<VA>` from guest_rt): that VA, or when it is not a candidate of this
        group (a core / promoted function) the innermost candidate on its chain; (2) the first cjn_<VA> frame of a
        crash backtrace or of the gdb dump of a hung run"""
        for l in open(os.path.join(d, 'log'), errors='replace'):
            if '[HYB]' not in l: continue
            m = re.search(r'native=cjn_([0-9A-F]{8}) chain=(\S+)', l)
            if not m: continue
            if m.group(1) in g: return m.group(1), '[HYB] blamed native ' + m.group(1)
            for x in reversed(m.group(2).split(',')):
                if x in g: return x, f'[HYB] innermost candidate on native chain (innermost {m.group(1)})'
        for l in open(os.path.join(d, 'log'), errors='replace'):
            m = re.search(r'cjn_([0-9A-F]{8})', l)
            if m and m.group(1) in g and '[HYB]' not in l: return m.group(1), 'native body ' + ('crashed' if rc == 139 else 'hung')
        return None, ''

    def suspect(d, g):
        """first logged wrapper call whose pre/post/ret differ from the baseline's: the candidate that ran it, or the
        last candidate call before it when it is a core/other function"""
        runc = swap_calls(os.path.join(d, 'log'), keep_order=True)
        last_cand = None
        for key, val in runc:
            if key[0] in g: last_cand = key[0]
            bv = base_swap.get(key)
            if bv is not None and bv != val:
                if key[0] in g and bv[0] == val[0]: return key[0], f'{key[0]} call #{key[1]}: same pre-state, different post/ret'
                return (last_cand or (key[0] if key[0] in g else None)), f'first differing logged call {key[0]} #{key[1]}'
        return None, ''

    def solve(g, name):
        g = list(g)
        while g:
            d, rc = R.run(name, g)
            v, r = R.judge(d, rc, 'base_a')
            cnt[0] += 1
            if not v['ok'] and v['region'] == 'image' and v['frame'] is not None and v['frame'] <= 3:
                # an f1..f3 image difference with identical hashes is the loading screen (texture streaming progress follows
                # real-time loader threads, docs/promote.md round 5): decide by one repeat run, a flake passes the second time
                print(f'           image difference at f={v["frame"]} in group {name} ({len(g)}): repeating once', flush=True)
                d, rc = R.run(name, g)
                v, r = R.judge(d, rc, 'base_a')
                cnt[0] += 1
            tag = 'PASS' if v['ok'] else 'FAIL ' + v['why'] + ' f=' + str(v['frame']) + ' ' + str(v['region'])
            print(f'[{time.time()-t0:5.0f}s] group {name} ({len(g)}): {tag}', flush=True)
            if v['ok']:
                with lock:
                    for va in g: res[va].update(status='PASS', runs=name)
                    save_results(res, meta)
                cleanup(d); return
            fr = v['frame'] if v['frame'] is not None else max(r['det'], default=0)
            c, why = (None, '')
            c, why = culprit(d, g, rc)
            if not c: c, why = suspect(d, g)
            if c:
                with lock:
                    res[c].update(status='FAIL', frame=fr, region=v['region'], detail=(why + '; ' + v['why'])[:160], runs=name)
                    save_results(res, meta)
                g.remove(c); print(f'           -> {c} FAIL ({why}), {len(g)} left', flush=True); continue
            if len(g) == 1:
                with lock:
                    res[g[0]].update(status='FAIL', frame=fr, region=v['region'], detail=v['why'], runs=name); save_results(res, meta)
                return
            h = len(g) // 2
            solve(g[:h], name + 'a'); solve(g[h:], name + 'b'); return

    half = len(live) // 2
    with cf.ThreadPoolExecutor(2) as ex:
        fs = [ex.submit(solve, live[:half], 'A'), ex.submit(solve, live[half:], 'B')]
        for f in fs: f.result()
    save_results(res, meta)
    c = collections.Counter(r['status'] for r in res.values())
    print('results:', dict(c), f'{cnt[0]} group runs, {time.time()-t0:.0f}s')


# ------------------------------------------------------------------ promote
def cmd_promote(a):
    rows = json.load(open(RESULTS))
    passing = [r for r in rows if r.get('status') == 'PASS' and r['va'] not in set(a.drop)]
    core = read_targets()
    extra = collections.OrderedDict()
    for r in passing: extra.setdefault(r['unit'], []).append(r['va'])
    new = merge_by_unit(core, extra)
    hdr = ''.join(l for l in open(TARGETS) if l.startswith('#'))
    shutil.copy(TARGETS, os.path.join(WORK, 'targets.before-promote.txt'))
    write_targets(new, TARGETS, hdr)
    print(f'targets.txt: {sum(len(v) for v in new.values())} functions ({len(passing)} promoted)')
    batch_set = {c['va'] for c in json.load(open(os.path.join(WORK, 'batch.json')))}
    if a.regen:
        regen(TARGETS, os.path.join(ROOT, 'src', 'recomp', 'gen'), os.path.join(ROOT, 'build', 'hybrid'), 'final')
        cmake_build(os.path.join(ROOT, 'build'), os.path.join(ROOT, 'src', 'recomp', 'gen'), TARGETS)


def cmd_final(a):
    """all-on (core + every promoted target) vs all-off (CJ_NATIVE_SWAP=0), 300 frames, two runs at the same time"""
    R = Runner(os.path.join(ROOT, 'build'))
    if a.off2: return cmd_off2(R)
    allv = [v for vs in read_targets().values() for v in vs]
    R.core = []
    t0 = time.time()
    with cf.ThreadPoolExecutor(2) as ex:
        # mode 0: everything generated; the run() selector string is a VA list, so use a VA that selects nothing
        fo = ex.submit(R.run, 'final_off', ['00000000'], False)
        fn = ex.submit(R.run, 'final_on', allv, False)
        (do, ro), (dn, rn) = fo.result(), fn.result()
    # judge needs the 'off' as baseline; all-off means CJ_NATIVE_SWAP=0 semantic = no native: 00000000 never matches a VA
    v, r = R.judge(dn, rn, 'final_off')
    print(f'final {time.time()-t0:.0f}s:', v)
    on = parse_log(os.path.join(dn, 'log'))
    print('esp_bad/csr_bad total:', sum(x[2] + x[3] for x in on['rep'].values()))
    json.dump(dict(ok=v['ok'], detail=v), open(os.path.join(WORK, 'final.json'), 'w'))
    if v['ok']: cleanup(do); cleanup(dn)


def cmd_off2(R):
    """swap-off pair: everything generated, twice at the same time, hygiene on; must agree on everything"""
    R.core = []; t0 = time.time()
    with cf.ThreadPoolExecutor(2) as ex:
        fa = ex.submit(R.run, 'off2_a', ['00000000'], False); fb = ex.submit(R.run, 'off2_b', ['00000000'], False)
        (da, ra), (db, rb) = fa.result(), fb.result()
    v, r = R.judge(db, rb, 'off2_a')
    print(f'swap-off pair {time.time()-t0:.0f}s rc {ra},{rb}:', v, 'frames', len(r['det']))


def cmd_status(a):
    rows = json.load(open(RESULTS))
    c = collections.Counter(r.get('status') for r in rows)
    print(dict(c))
    for r in rows:
        if r.get('status') == 'FAIL': print(r['va'], r['name'], r.get('frame'), r.get('region'), r.get('detail'))


def main():
    ap = argparse.ArgumentParser(); sp = ap.add_subparsers(dest='cmd', required=True)
    sp.add_parser('scan')
    c = sp.add_parser('compilecheck'); c.add_argument('--jobs', type=int, default=1); c.add_argument('--only')
    r = sp.add_parser('reach'); r.add_argument('--bin', default='build/pcj'); r.add_argument('--only')
    b = sp.add_parser('batch'); b.add_argument('-n', type=int, default=120); b.add_argument('--min-size', type=int, default=0)
    t = sp.add_parser('test'); t.add_argument('--build', default='build-batch'); t.add_argument('--reuse-base', action='store_true'); t.add_argument('--prefail'); t.add_argument('--skip-pass', action='store_true'); t.add_argument('--exclude', nargs='*', default=[]); t.add_argument('--only', nargs='*'); t.add_argument('--out')
    p = sp.add_parser('promote'); p.add_argument('--drop', nargs='*', default=[]); p.add_argument('--regen', action='store_true')
    f = sp.add_parser('final'); f.add_argument('--off2', action='store_true', help='swap-off determinism pair instead: two all-generated runs')
    sp.add_parser('regen-batch'); sp.add_parser('status')
    a = ap.parse_args()
    {'scan': cmd_scan, 'compilecheck': cmd_compilecheck, 'reach': cmd_reach, 'batch': cmd_batch, 'test': cmd_test,
     'promote': cmd_promote, 'regen-batch': cmd_regen_batch, 'final': cmd_final, 'status': cmd_status}[a.cmd](a)


if __name__ == '__main__':
    main()
