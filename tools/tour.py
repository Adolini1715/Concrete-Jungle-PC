#!/usr/bin/env python3
"""Automated smoke tour: boots the port in a fast test mode (CJ_GL_SKIP=1, drawing only around
screenshots), drives it through the menus and a level with the pad, the keyboard and the mouse,
screenshots every step and scans the log for crashes, freezes and wild calls.

    python3 tools/tour.py [--wine] [--only menus|level|monkey] [--monkey-min N] [TAG]

--wine runs the Windows build (build-win/pcj.exe via tools/run_wine.sh). Results go to
run/tour-TAG/: report.md (pass/fail per step, problems with their log lines) and NN_step.png.
Exit status 0 when every step passed.

Parts:
  menus   Options pages, PC Settings (Frame Rate 30/60 x10), the hub's Hunting Gear and Trophies
  level   first level: weapon select (pad Back, keyboard Tab, mouse clicks), pause (Start, Esc,
          every tab, mouse hover/clicks), quit to the main menu through the pause menu, and back in
  monkey  N minutes of random pad, key and mouse input in menus and the level (default 10)
"""
import glob, os, random, re, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROBLEMS = [
    (re.compile(r'\[CRASH\]'), 'crash'),
    (re.compile(r'\[QS\] stall: no frame'), 'freeze (no frame for 15 s)'),
    (re.compile(r'is not code -- skipped 1 time'), 'null or wild function call'),
    (re.compile(r'\[ICALL\] Failed to resolve'), 'unresolved indirect call'),
    (re.compile(r'\[WATCH\] \['), 'watched memory written'),
    (re.compile(r'\[FONT\] the font code changed'), 'font code wrote a vtable'),
    (re.compile(r'CONTIG\] arena exhausted'), 'graphics memory exhausted'),
]
NOTES = [
    (re.compile(r'\[TEXT\] box \d+ wide'), 'text box widened (wrap guard)'),
]


class Tour:
    def __init__(self, tag, wine):
        self.tag, self.wine = tag, wine
        self.out = os.path.join(ROOT, 'run', 'tour-' + tag)
        shutil.rmtree(self.out, ignore_errors=True)
        os.makedirs(self.out)
        self.results = []   # (part, step, ok, detail, png)
        self.n = 0
        self.proc = None

    # ---- one game run ------------------------------------------------------------------------
    def start(self, part, until, extra_env=None, args=()):
        self.stop()
        self.part = part
        self.rtag = 'tour-%s-%s' % (self.tag, part)
        self.rd = os.path.join(ROOT, 'run', self.rtag)
        shutil.rmtree(self.rd, ignore_errors=True)
        env = dict(os.environ, CJ_GL_SKIP='1', CJ_MENU_MOUSE_LOG='1', ROUTE_NO_REPEAT='1', ROUTE_BOOT_START='1',
                   CJ_PAD_HOST='1')   # the host keyboard and mouse reach the game as in play
        if self.wine:
            env['PCJ_RUN'] = os.path.join(ROOT, 'tools', 'run_wine.sh')
            stage = os.path.join(ROOT, 'dist', 'stage', 'pcj.exe')
            shutil.copy(os.path.join(ROOT, 'build-win', 'pcj.exe'), stage)
        env.update(extra_env or {})
        cmd = [sys.executable, os.path.join(ROOT, 'tools', 'route.py'), self.rtag, '--until', until,
               '--after', '100000000', '--dump-every', '100000000'] + list(args)
        self.route_log = open(os.path.join(ROOT, 'run', self.rtag + '.route.txt'), 'w')
        self.proc = subprocess.Popen(cmd, env=env, stdout=self.route_log, stderr=subprocess.STDOUT,
                                     cwd=ROOT, preexec_fn=os.setsid)
        self.logpos = 0
        self.log = os.path.join(self.rd, 'log.txt')
        goal = 'MILESTONE main menu' if until == 'menu' else 'MILESTONE in-level'
        t0 = time.time()
        while time.time() - t0 < (1500 if self.wine else 900):
            time.sleep(2)
            txt = open(self.route_log.name, errors='replace').read()
            if goal in txt:
                break
            if self.proc.poll() is not None:
                return self.record('start', False, 'the run ended before "%s"' % goal)
            bad = self.scan(note=False)
            if bad:
                return self.record('start', False, bad)
        else:
            return self.record('start', False, 'timed out waiting for "%s"' % goal)
        time.sleep(8 if until == 'menu' else 25)
        self.display = self.find_display()
        if until == 'menu':
            # route.py's Start repeat (to get past the title) can run on into the main menu at
            # full speed and open New Game: back out to the main menu.
            for _ in range(8):
                f = (self.fe() or ('',))[0]
                if f == 'Main':
                    break
                self.pad('b', wait=2.5)
        return self.record('start', True, 'reached %s in %.0f s' % (until, time.time() - t0))

    def stop(self):
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, 15)
            except ProcessLookupError:
                pass
            self.proc.wait(timeout=30)
        for pat in ('build/pcj ', 'dist/stage/pcj.exe'):
            subprocess.run(['pkill', '-f', pat], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.proc = None

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def find_display(self):
        return ':%d' % (90 + (self.proc.pid % 9))   # route.py's choice (its own pid)

    # ---- input -------------------------------------------------------------------------------
    def pad(self, *keys, wait=1.2):
        for i, k in enumerate(keys):   # one press at a time: presses written together can merge
            with open(os.path.join(self.rd, 'live'), 'a') as f:
                f.write(k + '\n')
            time.sleep(wait if i == len(keys) - 1 else 0.8)

    def xdo(self, *a):
        return subprocess.run(['xdotool'] + [str(x) for x in a], env=dict(os.environ, DISPLAY=self.display),
                              capture_output=True, text=True).stdout

    def window(self):
        best = None
        for w in self.xdo('search', '--onlyvisible', '--name', '.').split():
            g = self.xdo('getwindowgeometry', w)
            m = re.search(r'Position: (-?\d+),(-?\d+).*Geometry: (\d+)x(\d+)', g, re.S)
            if m:
                x, y, ww, hh = map(int, m.groups())
                if not best or ww * hh > best[3] * best[4]:
                    best = (w, x, y, ww, hh)
        return best

    def key(self, name, wait=1.2):
        w = self.window()
        if w:
            self.xdo('windowfocus', w[0])
        self.xdo('keydown', name)
        time.sleep(0.25)
        self.xdo('keyup', name)
        time.sleep(wait)

    def fe_to_screen(self, fx, fy):
        w = self.window()
        if not w:
            return None
        _, x, y, ww, hh = w
        s = min(ww / 640.0, hh / 480.0)
        ox, oy = x + (ww - 640 * s) / 2, y + (hh - 480 * s) / 2
        return int(ox + fx * s), int(oy + fy * s)

    def mouse(self, fx, fy, click=False, wait=0.8):
        p = self.fe_to_screen(fx, fy)
        if not p:
            return
        self.xdo('mousemove', p[0] - 3, p[1] - 3)
        time.sleep(0.15)
        self.xdo('mousemove', p[0], p[1])
        time.sleep(0.3)
        if click:
            self.xdo('click', 1)
        time.sleep(wait)

    # ---- checks ------------------------------------------------------------------------------
    def scan(self, note=True):
        """New problem lines in the log since the last scan (text), or ''."""
        try:
            with open(self.log, errors='replace') as f:
                f.seek(self.logpos)
                new = f.read()
                self.logpos = f.tell()
        except FileNotFoundError:
            return ''
        bad = []
        for line in new.splitlines():
            for rx, what in PROBLEMS:
                if rx.search(line):
                    bad.append('%s: %s' % (what, line.strip()[:160]))
            if note:
                for rx, what in NOTES:
                    if rx.search(line) and len(self.notes) < 20:
                        self.notes.add('%s: %s' % (what, line.strip()[:120]))
        return '; '.join(bad[:4])

    notes = set()

    def paused(self):
        """The pause menu (FEPause, vtable 0x00389A18) is taking input: its list logs within ~3 s."""
        pos = os.path.getsize(self.log)
        time.sleep(3.5)
        with open(self.log, errors='replace') as f:
            f.seek(pos)
            return 'vt 00389a18' in f.read()

    def fe(self):
        last = None
        try:
            for l in open(self.log, errors='replace'):
                m = re.search(r'\[STATE\] .*flip=(\d+) fe=(\S+)/(\d+)', l)
                if m:
                    last = (m.group(2), int(m.group(3)), int(m.group(1)))
        except FileNotFoundError:
            pass
        return last

    def count(self, pattern):
        try:
            return len(re.findall(pattern, open(self.log, errors='replace').read()))
        except FileNotFoundError:
            return 0

    def shot(self, name):
        rd = self.rd
        before = set(glob.glob(rd + '/fr/*.ppm'))
        open(rd + '/dump-now', 'w').close()
        new = []
        for _ in range(60):
            time.sleep(0.5)
            new = sorted(set(glob.glob(rd + '/fr/*.ppm')) - before)
            if len(new) >= 4:
                break
        if not new:
            return None
        time.sleep(1)
        from PIL import Image
        self.n += 1
        out = os.path.join(self.out, '%02d_%s_%s.png' % (self.n, self.part, re.sub(r'\W+', '_', name)))
        for f in reversed(new):
            try:
                Image.open(f).save(out)
                return out
            except OSError:
                pass
        return None

    def record(self, step, ok, detail='', png=None):
        self.results.append((self.part, step, ok, detail, png))
        print('%s %-8s %-40s %s' % ('PASS' if ok else 'FAIL', self.part, step, detail), flush=True)
        return ok

    def step(self, name, fn, check=None, shot=True):
        """Run fn(), then: the game still running, no problem lines, the optional check."""
        if not self.alive():
            return self.record(name, False, 'skipped: the game is not running')
        try:
            detail = fn() or ''
        except Exception as e:   # a broken step must not stop the tour
            import traceback
            traceback.print_exc()
            detail = 'step error: %r' % (e,)
        time.sleep(1)
        bad = self.scan()
        png = self.shot(name) if shot and self.alive() else None
        ok = not bad and self.alive()
        if ok and check:
            r = check()
            if r is not True:
                ok, detail = False, (detail + '; ' if detail else '') + (r or 'check failed')
        if not self.alive():
            detail = (detail + '; ' if detail else '') + 'the game exited'
        return self.record(name, ok, (detail + ('; ' if detail and bad else '') + bad).strip('; '), png)

    # ---- the parts ---------------------------------------------------------------------------
    def part_menus(self):
        if not self.start('menus', 'menu'):
            return
        st = self
        st.step('main menu', lambda: None, lambda: True if (st.fe() or ('',))[0] == 'Main' else 'fe=%s' % (st.fe(),))
        # Options: Up twice from New Game (Quit Game is last), then A
        def open_options():
            # Main: New Game, Load Game, Options, Quit Game. One press at a time, and check where A landed.
            for attempt in range(5):
                f = (st.fe() or ('',))[0]
                if f.startswith('Options'):
                    return 'attempt %d' % attempt
                if f != 'Main':
                    st.pad('b', wait=2.5)
                    continue
                for k in ('up', 'up', 'up', 'up', 'down', 'down'):   # to the top (it stops there), then two down
                    st.pad(k, wait=0.9)
                st.pad('a', wait=3)
            return 'fe=%s' % (st.fe(),)
        st.step('open Options', open_options,
                lambda: True if (st.fe() or ('',))[0].startswith('Options') else 'fe=%s' % (st.fe(),))
        # Options: Controller, Camera, Audio, Game, PC Settings (the cursor stays where B left it)
        seen = []
        names = ['Controller', 'Camera', 'Audio', 'Game', 'PC Settings']
        for i, name in enumerate(names):
            if i:
                st.pad('down', wait=1.2)
            if name == 'PC Settings':
                break
            st.step('page ' + name, lambda: st.pad('a', wait=3),
                    lambda: True if (st.fe() or ('',))[0].startswith('Options_') else 'fe=%s' % (st.fe(),))
            seen.append((st.fe() or ('',))[0])
            st.pad('b', wait=2.5)
        st.record('Options pages visited', len(seen) == 4, ', '.join(seen))
        def fps_toggle():
            st.pad('a', wait=3)                           # PC Settings: the cursor is on Widescreen
            st.pad('down', 'down', 'down', 'down', wait=1.5)   # Frame Rate
            n0 = st.count(r'\[FPS\] frame-rate cap')
            for _ in range(10):
                st.pad('right', wait=1.6)
                st.pad('left', wait=1.6)
            return 'frame-rate changes logged: %d' % (st.count(r'\[FPS\] frame-rate cap') - n0)
        st.step('PC Settings: Frame Rate 30/60 x10', fps_toggle,
                lambda: True if st.count(r'\[FPS\] frame-rate cap') >= 8 else 'the Frame Rate row did not change')
        time.sleep(18)   # a freeze reports after 15 s without a frame
        st.step('after the toggles: still drawing', lambda: None)
        # mouse: hover and click the footer's Back prompt area, then a PC Settings row
        st.step('mouse: hover rows', lambda: [st.mouse(200, 190 + 26 * i, wait=0.4) for i in range(6)] and None)
        st.step('keyboard: Esc back to Options', lambda: st.key('Escape', wait=3),
                lambda: True if (st.fe() or ('',))[0] == 'Options' else 'fe=%s' % (st.fe(),))
        st.step('keyboard: Esc back to the main menu', lambda: st.key('Escape', wait=3),
                lambda: True if (st.fe() or ('',))[0] in ('Main', 'Options') else 'fe=%s' % (st.fe(),))
        st.stop()

    def part_level(self):
        if not self.start('level', 'level', {'ROUTE_NO_REPEAT': '1'}, ['--skip-for', '240']):
            return
        st = self
        time.sleep(20)
        n_sel = lambda: st.count(r'selector 1\b')
        st.step('in play', lambda: None)
        st.step('weapon select: pad Back opens', lambda: st.pad('back', wait=2.5))
        st.step('weapon select: pad Back closes', lambda: st.pad('back', wait=2))
        for i in range(3):
            st.step('weapon select: keyboard Tab #%d' % (i + 1), lambda: st.key('Tab', wait=2.5), shot=(i == 0))
            st.step('weapon select: Tab closes #%d' % (i + 1), lambda: st.key('Tab', wait=2), shot=False)
        def wsel_mouse():
            st.key('Tab', wait=2)
            for y in (195, 225, 255, 285):          # the rows, then each row's right arrow
                st.mouse(320, y, click=True, wait=1.2)
                st.mouse(440, y, click=True, wait=1.2)
        st.step('weapon select: mouse clicks rows and arrows', wsel_mouse)
        st.step('weapon select: close', lambda: st.key('Tab', wait=2))
        st.step('pause: pad Start', lambda: st.pad('start', wait=3))
        for i in range(5):   # the tabs: Quit, Objectives, Hints, Options, Controller, Weapons; the triggers move
            st.step('pause: next tab #%d' % (i + 1), lambda: st.pad('rt', wait=2))
        tabs = ((120, 83), (190, 83), (266, 83), (328, 83), (410, 83), (499, 83))
        st.step('pause: mouse hover tabs and items', lambda: [st.mouse(x, y, wait=0.3) for (x, y) in
                tabs + ((320, 140), (320, 200), (320, 260))] and None)
        st.step('pause: mouse clicks the Options tab', lambda: st.mouse(328, 83, click=True, wait=2))
        st.step('pause: pad Start closes', lambda: st.pad('start', wait=3))
        st.step('pause: pad Start opens', lambda: st.pad('start', wait=3))
        st.step('pause: pad B closes', lambda: st.pad('b', wait=3))
        st.step('pause: Esc opens', lambda: st.key('Escape', wait=3))
        st.step('pause: Esc closes', lambda: st.key('Escape', wait=3))
        # quit to the main menu: open pause, go to the Options tab's last item, A, confirm
        def quit_to_menu():
            # the pause menu's Quit tab (leftmost): Level Stats, Save Game, Load Game, Restart Level,
            # Quit Game, Quit to Main Menu; click the last, then Yes on the question
            if not st.paused():
                st.pad('start', wait=3)
            st.mouse(120, 83, click=True, wait=2)
            st.mouse(320, 360, click=True, wait=3)
            for _ in range(3):
                if (st.fe() or ('-',))[0] not in ('-',):
                    break
                st.pad('up', wait=1)
                st.pad('a', wait=8)
            return 'front end: %s' % (st.fe(),)
        st.step('pause: Quit to Main Menu', quit_to_menu,
                lambda: True if (st.fe() or ('-',))[0] not in ('-',) else 'did not reach the front end')
        time.sleep(15)
        st.step('back at the menus: still running', lambda: None)
        st.step('menus after the level: mouse moves and clicks', lambda: [st.mouse(320, 150 + 40 * i, wait=0.5) for i in range(5)] and None)
        st.stop()

    def part_monkey(self, minutes):
        rnd = random.Random(1234)
        for where, until in (('monkey-menus', 'menu'), ('monkey-level', 'level')):
            if not self.start(where, until, {'ROUTE_NO_REPEAT': '1'} if until == 'level' else None,
                              ['--skip-for', '240'] if until == 'level' else []):
                continue
            t_end = time.time() + minutes * 30
            n = 0
            pads = ['up', 'down', 'left', 'right', 'a', 'b', 'x', 'y', 'start', 'back']
            keys = ['Up', 'Down', 'Left', 'Right', 'Return', 'Escape', 'Tab', 'BackSpace', 'space', 'e', 'q', 'f']
            while time.time() < t_end and self.alive():
                r = rnd.random()
                if r < 0.45:
                    self.pad(rnd.choice(pads), wait=rnd.uniform(0.2, 1.0))
                elif r < 0.75:
                    self.key(rnd.choice(keys), wait=rnd.uniform(0.2, 1.0))
                else:
                    self.mouse(rnd.randint(20, 620), rnd.randint(20, 460), click=rnd.random() < 0.5, wait=rnd.uniform(0.2, 0.8))
                n += 1
                bad = self.scan()
                if bad:
                    self.record('%d random inputs' % n, False, bad, self.shot('problem'))
                    break
                if 'Quit Game chosen' in open(self.log, errors='replace').read()[-200000:]:
                    self.record('%d random inputs' % n, True, 'the monkey chose Quit Game (a normal exit)')
                    break
            else:
                if self.alive():
                    self.record('%d random inputs' % n, True, '', self.shot('end'))
                else:
                    self.record('%d random inputs' % n, False, 'the game exited: ' + self.scan())
            self.stop()

    def report(self):
        npass = sum(1 for r in self.results if r[2])
        lines = ['# Tour %s (%s build)' % (self.tag, 'Windows/Wine' if self.wine else 'Linux'), '',
                 '%d of %d steps passed.' % (npass, len(self.results)), '',
                 '| Part | Step | Result | Detail | Screenshot |', '|---|---|---|---|---|']
        for part, step, ok, detail, png in self.results:
            lines.append('| %s | %s | %s | %s | %s |' % (part, step, 'pass' if ok else '**FAIL**',
                         detail.replace('|', '/'), os.path.basename(png) if png else ''))
        if self.notes:
            lines += ['', 'Notes:', ''] + ['- ' + n for n in sorted(self.notes)]
        with open(os.path.join(self.out, 'report.md'), 'w') as f:
            f.write('\n'.join(lines) + '\n')
        print('\n'.join(lines[:3]))
        return npass == len(self.results)


def main():
    a = sys.argv[1:]
    wine = '--wine' in a
    only = a[a.index('--only') + 1] if '--only' in a else None
    minutes = int(a[a.index('--monkey-min') + 1]) if '--monkey-min' in a else 10
    rest = [x for i, x in enumerate(a) if not x.startswith('--') and (i == 0 or a[i - 1] not in ('--only', '--monkey-min'))]
    tag = rest[0] if rest else time.strftime('%H%M')
    t = Tour(tag, wine)
    try:
        if only in (None, 'menus'):
            t.part_menus()
        if only in (None, 'level'):
            t.part_level()
        if only == 'monkey' or (only is None and '--monkey' in a):
            t.part_monkey(minutes)
    finally:
        t.stop()
    sys.exit(0 if t.report() else 1)


if __name__ == '__main__':
    main()
