#!/usr/bin/env python3
"""scene.py: start test runs at a saved moment instead of replaying from boot.

  scene.py list
  scene.py add NAME FILE.pcjs CHAPTER "description"     store a quick save as a scene
  scene.py run NAME TAG [--after N] [--track FILE] [--wine] [K=V ...]

A scene is a quick-save file (src/quicksave.c disk format) plus scenes/scenes.json. `run` boots the
game through tools/route.py to the scene's chapter (the route harness only reaches chapter 1 so
far), and CJ_QS_AUTOLOAD=1 loads the scene the first time the player has control. --track starts a
CJ_POS_TRACK path after the load (e.g. walk into a trigger); K=V pairs are passed as environment
(CJ_AI_TRACE=..., CJ_WIDESCREEN=1, ...). Output lands in run/TAG like any route run.

Scenes only load into the same build family (quick-save build key = host region list) and need the
worker-thread stacks where they were; `run` reports a refused load. Re-record scenes with
tools/scene_record.sh after a build changes the region list."""
import json, os, shutil, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDIR = os.path.join(ROOT, 'scenes')
META = os.path.join(SDIR, 'scenes.json')


def load():
    return json.load(open(META)) if os.path.exists(META) else {}


def save(m):
    os.makedirs(SDIR, exist_ok=True)
    json.dump(m, open(META, 'w'), indent=1, sort_keys=True)


def main():
    a = sys.argv[1:]
    if not a: sys.exit(__doc__)
    m = load()
    if a[0] == 'list':
        for k, v in sorted(m.items()):
            print(f"{k:16s} ch{v['chapter']}  {v['desc']}  ({v['file']})")
        return
    if a[0] == 'add':
        name, src, ch, desc = a[1], a[2], int(a[3]), a[4]
        os.makedirs(SDIR, exist_ok=True)
        dst = os.path.join(SDIR, name + '.pcjs')
        shutil.copyfile(src, dst)
        m[name] = {'file': name + '.pcjs', 'chapter': ch, 'desc': desc}
        save(m); print('added', name); return
    if a[0] == 'run':
        name, tag = a[1], a[2]
        if name not in m: sys.exit('no scene ' + name)
        after, track, wine, env = 600, None, False, dict(os.environ)
        i = 3
        while i < len(a):
            if a[i] == '--after': after = int(a[i + 1]); i += 2
            elif a[i] == '--track': track = os.path.abspath(a[i + 1]); i += 2
            elif a[i] == '--wine': wine = True; i += 1
            elif '=' in a[i]: k, v = a[i].split('=', 1); env[k] = v; i += 1
            else: sys.exit('bad arg ' + a[i])
        qd = os.path.join('/tmp/claude-0', 'scene_' + tag)
        shutil.rmtree(qd, ignore_errors=True); os.makedirs(qd)
        shutil.copyfile(os.path.join(SDIR, m[name]["file"]), os.path.join(qd, "quicksave.pcjs"))
        ini = os.path.join(qd, 'pcj.ini')
        open(ini, 'w').write('CJ_QS_SLOT=1\n')
        def winpath(p): return 'Z:' + p.replace('/', '\\')
        env.update({'CJ_QS_AUTOLOAD': '1', 'CJ_QS_ANYMAP': '1', 'CJ_QS_DIR': winpath(qd) if wine else qd,
                    'CJ_PC_INI': winpath(ini) if wine else ini})
        if track: env['CJ_POS_TRACK'] = winpath(track) if wine else track
        if wine: env['PCJ_RUN'] = 'tools/run_wine.sh'
        subprocess.call(['git', 'checkout', '-q', 'run/save/Partition5.img'], cwd=ROOT)
        if wine: shutil.copyfile(os.path.join(ROOT, 'run/save/Partition5.img'), os.path.join(ROOT, 'run/save-win/Partition5.img'))
        shutil.rmtree(os.path.join(ROOT, 'run', tag), ignore_errors=True)
        out = os.path.join('/tmp/claude-0', tag + '.out')
        with open(out, 'w') as f:
            rc = subprocess.call(['python3', 'tools/route.py', tag, '--until', 'level', '--after', str(after),
                                  '--skip-for', '150', '--dump-every', '30'], cwd=ROOT, env=env, stdout=f, stderr=subprocess.STDOUT)
        subprocess.call(['git', 'checkout', '-q', 'run/save/Partition5.img'], cwd=ROOT)
        log = os.path.join(ROOT, 'run', tag, 'log.txt')
        lines = [l.rstrip() for l in open(log, errors='replace') if '[QS]' in l and ('load' in l or 'verify' in l or 'refuse' in l.lower() or "can't" in l.lower())]
        print('\n'.join(lines[:8]) or 'no quick-save lines (see ' + log + ')')
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
