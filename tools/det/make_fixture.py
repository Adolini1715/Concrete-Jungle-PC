#!/usr/bin/env python3
"""make_fixture.py: record a scripted play session as a replay fixture (tests/fixtures/rec-<name>).

  tools/det/make_fixture.py NAME [--mods 0|default|VA,VA,..] [--scene SCENE] [--after N] [--no-skip]
                                 [--press-at FLIP KEY[:HOLD]]... [--check]

Boots through tools/route.py (menu -> New Game -> chapter 1, mission intro lines skipped) with
CJ_REC=1, then sends the --press-at presses (flips after the player has control, same syntax as
route.py: lup:60 holds the left stick up for 60 polls, lup+a:3, x, y, b, rt, ...). --after N stops
N flips after the last press (default 300). --scene loads a quick save from scenes/scenes.json at
the first control (CJ_QS_AUTOLOAD, docs/quicksave.md): a fight, the courtyard, ... The recording
(logs/rec-*) is moved to tests/fixtures/rec-NAME with env.txt = the mods it was made with:
  --mods 0        no gameplay mods: the original game (best for verifying decomp functions); the
                  front-end menu mods stay on, the route needs the menus as the build shows them
  --mods default  every mod in tools/hybridize/targets.txt except the ones that change behaviour
                  on purpose (Camera, FEMainMenu): what the other fixtures use
  --mods VA,..    an explicit list
--check replays the new fixture once (tools/det/replay.sh) and reports PASS/DIVERGE; a fixture that
does not replay exact is deleted.

Examples
  tools/det/make_fixture.py ch1-run-600 --mods 0 --press-at 60 lup:200 --press-at 300 lup+a:3 --after 200 --check
  tools/det/make_fixture.py ch1-fight --mods 0 --scene ch1-fight --press-at 120 x --press-at 160 x --press-at 200 y --check
"""
import glob, json, os, re, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIX = os.path.join(ROOT, 'tests', 'fixtures')
BEHAVIOUR_MODS = ('Camera',)   # mod= groups that change what the game does on purpose in play


def fe_mod_vas():
    vas = []
    for l in open(os.path.join(ROOT, 'tools', 'hybridize', 'targets.txt')):
        l = l.split('#')[0].strip()
        if l.startswith('mod=FE'): vas += [v.upper().zfill(8) for v in l.split()[1:]]
    return vas


def default_mods():
    vas = []
    for l in open(os.path.join(ROOT, 'tools', 'hybridize', 'targets.txt')):
        l = l.split('#')[0].strip()
        if not l.startswith('mod='): continue
        t = l.split()
        if t[0][4:] in BEHAVIOUR_MODS: continue
        vas += [v.upper().zfill(8) for v in t[1:]]
    return ','.join(vas)


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'): sys.exit(__doc__)
    name = a[0]; mods = '0'; scene = None; after = 300; skip = True; presses = []; check = False
    i = 1
    while i < len(a):
        if a[i] == '--mods': mods = a[i + 1]; i += 2
        elif a[i] == '--scene': scene = a[i + 1]; i += 2
        elif a[i] == '--after': after = int(a[i + 1]); i += 2
        elif a[i] == '--no-skip': skip = False; i += 1
        elif a[i] == '--press-at': presses.append((int(a[i + 1]), a[i + 2])); i += 3
        elif a[i] == '--check': check = True; i += 1
        else: sys.exit('bad arg ' + a[i])
    dest = os.path.join(FIX, 'rec-' + name)
    if os.path.exists(dest): sys.exit(f'{dest} exists')
    if mods == 'default': mods = default_mods()
    # the front-end mods stay on whatever --mods says: route.py navigates the menus as the build shows them, and
    # they do not run in play. The periodic Start pulses (route.py default) are what get through the Training
    # intro; route.py sends 'stop' once in the level.
    fe = [v for v in default_mods().split(',') if v]
    fe_only = ','.join(v for v in fe if v in fe_mod_vas())
    if mods == '0': mods = fe_only or '0'
    env = dict(os.environ, CJ_REC='1', CJ_MODS=mods)
    if scene:
        meta = json.load(open(os.path.join(ROOT, 'scenes', 'scenes.json')))
        if scene not in meta: sys.exit(f'no scene {scene}: ' + ', '.join(meta))
        # as tools/scene.py run: the scene file as slot 1 of a private quick-save folder, loaded at first control
        qd = os.path.join('/tmp/claude-0', 'fixture_' + name)
        shutil.rmtree(qd, ignore_errors=True); os.makedirs(qd)
        shutil.copyfile(os.path.join(ROOT, 'scenes', meta[scene]['file']), os.path.join(qd, 'quicksave.pcjs'))
        open(os.path.join(qd, 'pcj.ini'), 'w').write('CJ_QS_SLOT=1\n')
        env.update(CJ_QS_AUTOLOAD='1', CJ_QS_ANYMAP='1', CJ_QS_DIR=qd, CJ_PC_INI=os.path.join(qd, 'pcj.ini'))
    last = max([f for f, _ in presses] + [0])
    cmd = [sys.executable, os.path.join(ROOT, 'tools', 'route.py'), 'fixture-' + name, '--until', 'level', '--after', str(last + after), '--dump-every', '0']
    if skip: cmd += ['--skip-for', str(min(f for f, _ in presses) if presses else 60)]
    for f, k in presses: cmd += ['--press-at', str(f), k]
    before = set(glob.glob(os.path.join(ROOT, 'logs', 'rec-*')))
    print('record:', ' '.join(cmd), flush=True)
    t0 = time.time()
    r = subprocess.run(cmd, env=env, cwd=ROOT)
    new = sorted(set(glob.glob(os.path.join(ROOT, 'logs', 'rec-*'))) - before, key=os.path.getmtime)
    if r.returncode or not new: sys.exit(f'route failed (rc {r.returncode}) or no recording in logs/: see run/fixture-{name}/log.txt')
    rec = new[-1]
    subprocess.call(['git', 'checkout', '-q', 'run/save/Partition5.img'], cwd=ROOT)   # the route's boot wrote the save disk
    if not os.path.exists(os.path.join(rec, 'input.pcjr')): sys.exit(f'{rec} has no input.pcjr')
    with open(os.path.join(rec, 'env.txt'), 'w') as f: f.write(f'CJ_MODS={mods}\n')
    shutil.move(rec, dest)
    rows = (os.path.getsize(os.path.join(dest, 'input.pcjr')) - 72) // 18   # RecHeader is 72 bytes
    print(f'fixture {dest}: {rows} polls, {time.time()-t0:.0f}s to record, mods={mods}', flush=True)
    if check:
        r = subprocess.run(['bash', os.path.join(ROOT, 'tools', 'det', 'soak.sh'), 'linux', dest], cwd=ROOT)
        if r.returncode:
            shutil.rmtree(dest); sys.exit('the new fixture does not replay exact: deleted (see run/det/soak.txt)')
        print('replays exact')


if __name__ == '__main__':
    main()
