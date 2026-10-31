#!/usr/bin/env python3
"""Drive a running route (tools/route.py TAG --until menu --after 30000 --dump-every 100000,
Start repeat left on) to Options > PC Settings, then run live.py items.
Usage: to_pcsettings.py TAG [item ...]   (items as in tools/live.py)"""
import re, sys, time, subprocess
tag = sys.argv[1]; extra = sys.argv[2:]
log = 'run/%s/log.txt' % tag; live = 'run/%s/live' % tag
def fe():
    last = None
    try:
        for l in open(log, errors='ignore'):
            m = re.search(r'\[STATE\] poll=\d+ flip=(\d+) fe=(\S+)/(\d+)', l)
            if m: last = (m.group(2), int(m.group(3)), int(m.group(1)))
    except FileNotFoundError: pass
    return last
def press(k): open(live, 'a').write(k + '\n'); time.sleep(2.5)
def wait(pred, t=900):
    t0 = time.time()
    while time.time() - t0 < t:
        s = fe()
        if s and pred(s): return s
        time.sleep(1)
    sys.exit('timeout waiting, state %s' % (fe(),))
# the real main menu comes after the attract states (the route's Start repeat skips them)
wait(lambda s: s[0] == 'Main' and s[2] > 600); time.sleep(4)
# Under this harness a single d-pad move on the Options list also opens the
# next page (the same with the original code, CJ_MODS=0), so walk the pages:
# Controller, Camera, Audio, Game, then PC Settings (the Audio state again).
seen_game = False
for _ in range(25):
    s = fe()
    if s[0] == 'Options_Audio' and seen_game: break
    if s[0] == 'Main': press('up'); press('up'); press('a'); time.sleep(4)
    elif s[0] == 'Options': press('down'); time.sleep(4)
    elif s[0].startswith('Options_'):
        if s[0] == 'Options_Game': seen_game = True
        press('b'); time.sleep(4)
        if fe()[0] == 'Options': press('down'); time.sleep(4)
    else: time.sleep(3)
else: sys.exit('could not reach PC Settings: %s' % (fe(),))
time.sleep(3)
print('page', fe(), flush=True)
subprocess.run([sys.executable, 'tools/live.py', tag] + extra)
