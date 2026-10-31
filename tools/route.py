#!/usr/bin/env python3
"""Unattended route: boot -> main menu -> New Game -> first story level, keyed
on the [STATE] lines src/cj_pad.c logs, with presses sent through CJ_PAD_LIVE.

    python3 tools/route.py <tag> [--until menu|level] [--after N] [--dump-every N] [--gl 0|1] [--skip]

Runs ./run.sh (or $PCJ_RUN, e.g. tools/run_wine.sh) under Xvfb (:9x) with CJ_GL=1 (default), logs to run/<tag>/log.txt,
frames to run/<tag>/fr/gNNNNN.ppm. Stops N flips (default 300) after the goal
state: fe=Main for 'menu', in-level with player control for 'level'.
--skip presses A every 8 s once in-level (skips the mission intro lines); --skip-for N stops
those presses N flips after the level is in (a still camera for A/B frames).
Prints the flip at each milestone and the windowed flips/s from the [GL] log.
"""
import os, re, signal, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    a = sys.argv[1:]
    tag = a[0]
    until = 'level'
    after = 300
    every = 30
    gl = '1'
    skip = False
    skip_for = None
    press_at = []   # (flips after the goal, key or 'dump')
    i = 1
    while i < len(a):
        if a[i] == '--until': until = a[i + 1]; i += 2
        elif a[i] == '--after': after = int(a[i + 1]); i += 2
        elif a[i] == '--dump-every': every = int(a[i + 1]); i += 2
        elif a[i] == '--gl': gl = a[i + 1]; i += 2
        elif a[i] == '--skip': skip = True; i += 1
        elif a[i] == '--press-at': press_at.append((int(a[i + 1]), a[i + 2])); i += 3
        elif a[i] == '--skip-for': skip = True; skip_for = int(a[i + 1]); i += 2
        else: sys.exit('bad arg ' + a[i])
    rd = os.path.join(ROOT, 'run', tag)
    os.makedirs(os.path.join(rd, 'fr'), exist_ok=True)
    live = os.path.join(rd, 'live')
    open(live, 'w').close()
    disp = ':%d' % (90 + (os.getpid() % 9))
    xv = subprocess.Popen(['Xvfb', disp, '-screen', '0', os.environ.get('ROUTE_SCREEN', '1024x768x24')],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)
    env = dict(os.environ, DISPLAY=disp, CJ_GL=gl, CJ_PAD_REPEAT=os.environ.get('CJ_PAD_REPEAT', 'start,90,6'),
               CJ_PAD_LIVE=live, RECOMP_WATCHDOG_SECS='1800',
               RECOMP_FB_DUMP=os.path.join(rd, 'no-such-dir', 'f'), RECOMP_FB_DUMP_FLIPS='0')  # executor BMP dumps off
    if os.environ.get('ROUTE_NO_REPEAT'):
        env.pop('CJ_PAD_REPEAT', None)   # no periodic Start (it opens the pause menu in level)
    if every:
        env.update(CJ_GL_DUMP=os.path.join(rd, 'fr', 'g'), CJ_GL_DUMP_EVERY=str(every),
                   CJ_GL_DUMP_TRIGGER=os.path.join(rd, 'dump-now'))
    log = open(os.path.join(rd, 'log.txt'), 'w')
    p = subprocess.Popen([os.environ.get('PCJ_RUN', os.path.join(ROOT, 'run.sh'))], env=env, stdout=log,
                         stderr=subprocess.STDOUT, preexec_fn=os.setsid)
    t0 = time.time()

    def press(*keys):
        with open(live, 'a') as f:
            for k in keys:
                f.write(k + '\n')
        print('  [%6.0fs] press %s' % (time.time() - t0, ' '.join(keys)), flush=True)

    st = {}
    phase = 'boot'
    last_press = 0.0
    goal_flip = None
    lost_ctl = False
    flip = 0
    pos = 0
    rx = re.compile(r'\[STATE\] poll=(\d+) flip=(\d+) fe=(\S+)/(\d+) .*level_loaded=(\d) control=(\d) mission=(\d) cutscene=(\d)')
    rf = re.compile(r'\[GL\] flip (\d+):.*?([\d.]+) over the last 16 \((\d+) batches')
    try:
        while p.poll() is None:
            time.sleep(0.5)
            with open(os.path.join(rd, 'log.txt'), errors='replace') as f:
                f.seek(pos)
                chunk = f.read()
                pos = f.tell()
            for line in chunk.splitlines():
                m = rf.search(line)
                if m:
                    flip = int(m.group(1))
                    if flip % 64 == 0:
                        print('  flip %s %s flips/s %s batches/flip [%s]' % (m.group(1), m.group(2), m.group(3), phase), flush=True)
                m = rx.search(line)
                if not m:
                    continue
                fe = m.group(3)
                st = dict(flip=int(m.group(2)), fe=fe, sub=int(m.group(4)), lvl=int(m.group(5)),
                          ctl=int(m.group(6)), mis=int(m.group(7)), cut=int(m.group(8)))
                flip = st['flip']
                print('[STATE] flip=%d fe=%s/%d lvl=%d ctl=%d mission=%d cut=%d' % (
                    flip, fe, st['sub'], st['lvl'], st['ctl'], st['mis'], st['cut']), flush=True)
                if phase == 'boot' and fe == 'Main' and st['lvl'] == 1:
                    phase = 'menu'
                    print('MILESTONE main menu at flip %d (%.0fs)' % (flip, time.time() - t0), flush=True)
                    if until == 'menu':
                        goal_flip = flip
                    else:
                        time.sleep(3)
                        press('stop', 'a')
                        last_press = time.time()
                elif phase == 'menu' and fe == 'Loaded':
                    # hub: Training, Chapter Select, Ritual Challenges, Records,
                    # Hunting Gear, Costume Select, Trophies, Options, Save Game;
                    # after New Game the cursor is on Training
                    phase = 'hub'
                    time.sleep(3)
                    # a loaded save leaves the cursor where it was (often Chapter
                    # Select); a fresh one starts on Training, which the wrong-item
                    # case below steps down from
                    press('a')
                    last_press = time.time()
                elif phase == 'hub' and fe not in ('Loaded', 'Story_Level_Select', 'Done', '-', 'None'):
                    # wrong item: back out and try the next one (Ritual is just below Chapter)
                    time.sleep(3)
                    press('b'); time.sleep(3)
                    press('up' if fe == 'Ritual_Challenge_Select' else 'down'); time.sleep(2); press('a')
                    last_press = time.time()
                elif phase == 'hub' and fe == 'Story_Level_Select':
                    phase = 'select'
                    time.sleep(3)
                    press('a')
                    last_press = time.time()
                elif phase in ('select', 'hub') and fe == 'Done':
                    phase = 'loading'
                    lost_ctl = st['ctl'] == 0
                    print('MILESTONE level load at flip %d (%.0fs)' % (flip, time.time() - t0), flush=True)
                elif phase == 'loading' and st['ctl'] == 0:
                    lost_ctl = True      # the Training detour leaves level_loaded=1 control=1 behind: wait for the new level's own control
                elif phase == 'loading' and st['lvl'] == 1 and st['ctl'] == 1 and goal_flip is None and lost_ctl:
                    phase = 'level'
                    goal_flip = flip
                    print('MILESTONE in-level at flip %d (%.0fs)' % (flip, time.time() - t0), flush=True)
                elif phase == 'hub' and fe == 'Save_Game':
                    pass
            # nudges for menus that need more than one press
            now = time.time()
            if os.environ.get('ROUTE_BOOT_START') and phase == 'boot' and now - last_press > 4:
                press('start'); last_press = now   # ROUTE_NO_REPEAT=1 ROUTE_BOOT_START=1: Start only until the main menu
            if until != 'menu' and phase == 'menu' and st.get('fe') == 'Main' and now - last_press > 20:
                press('a'); last_press = now
            if until != 'menu' and phase == 'menu' and st.get('fe') == 'New_Game' and st.get('sub') == 13 and now - last_press > 6:
                # "Previous saved game will be lost. Overwrite?" defaults to No
                press('up'); time.sleep(1.5); press('a'); last_press = time.time()
            elif until != 'menu' and phase == 'menu' and st.get('fe', '').startswith('New_Game') and now - last_press > 8:
                press('a'); last_press = now
            if phase == 'hub' and st.get('fe') == 'Loaded' and now - last_press > 25:
                press('down'); time.sleep(2); press('a'); last_press = time.time()
            if phase == 'level' and skip and now - last_press > 8 and (skip_for is None or flip < goal_flip + skip_for):
                # the mission intro is an in-engine cutscene; A skips its lines
                press('a'); last_press = now
            for pa in list(press_at):
                if goal_flip is not None and flip >= goal_flip + pa[0]:
                    press_at.remove(pa)
                    if pa[1] == 'dump':
                        open(os.path.join(rd, 'dump-now'), 'w').close()
                        print('  dump trigger at flip %d' % flip, flush=True)
                    else:
                        press(pa[1])
            if goal_flip is not None and flip >= goal_flip + after:
                print('DONE at flip %d (%.0fs)' % (flip, time.time() - t0), flush=True)
                break
    finally:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except Exception:
            pass
        xv.kill()


if __name__ == '__main__':
    main()
