#!/usr/bin/env python3
"""Drive a running route (tools/route.py TAG --until menu --after 3000) to
Options > Controller, then press `right` N times (control style).
Usage: to_controller.py TAG [N]"""
import re, sys, time, os
tag = sys.argv[1]; n = int(sys.argv[2]) if len(sys.argv) > 2 else 0
log = 'run/%s/log.txt' % tag; live = 'run/%s/live' % tag
def fe():
    last = None
    try:
        for l in open(log, errors='ignore'):
            m = re.search(r'\[STATE\] .*fe=(\S+)/(\d+)', l)
            if m: last = (m.group(1), int(m.group(2)))
    except FileNotFoundError: pass
    return last
def press(k): open(live, 'a').write(k + '\n'); time.sleep(2.5)
def wait(pred, t=300):
    t0 = time.time()
    while time.time() - t0 < t:
        s = fe()
        if s and pred(s): return s
        time.sleep(1)
    sys.exit('timeout waiting, state %s' % (fe(),))
wait(lambda s: s == ('Main', 1)); time.sleep(4)
press('a')
for _ in range(12):
    time.sleep(3)
    s = fe()
    if s[0] == 'Options': break
    if s[0] in ('New_Game', 'Load_Game', 'Loaded'):
        press('b'); wait(lambda s: s[0] == 'Main'); time.sleep(3)
        press('down' if s[0] == 'Load_Game' else 'up'); press('a')
    elif s[0] == 'Main':
        press('down'); press('a')
time.sleep(2); press('a')
wait(lambda s: s[0] == 'Options_Controller')
time.sleep(2)
for _ in range(n): press('right')
time.sleep(3)
print('at', fe())
