#!/usr/bin/env python3
"""Send presses to a running route and grab frames.
Usage: live.py TAG item...   item = key name | shot:NAME | wait:SECONDS | state
Run the route with --dump-every 100000 (keeps CJ_GL_DUMP_TRIGGER on)."""
import glob, os, re, sys, time
tag = sys.argv[1]; rd = 'run/%s' % tag
def fe():
    last = None
    for l in open(rd + '/log.txt', errors='ignore'):
        m = re.search(r'\[STATE\] .*flip=(\d+) fe=(\S+)/(\d+)', l)
        if m: last = (m.group(2), int(m.group(3)), int(m.group(1)))
    return last
for it in sys.argv[2:]:
    if it.startswith('wait:'): time.sleep(float(it[5:]))
    elif it == 'state': print('state', fe(), flush=True)
    elif it.startswith('shot:'):
        before = set(glob.glob(rd + '/fr/*.ppm'))
        open(rd + '/dump-now', 'w').close()
        for _ in range(40):
            time.sleep(0.5)
            new = sorted(set(glob.glob(rd + '/fr/*.ppm')) - before)
            if len(new) >= 3: break
        if new:
            from PIL import Image
            out = rd + '/shot_%s.png' % it[5:]
            time.sleep(1)
            for f in reversed(new):
                try: Image.open(f).save(out); print('shot', out, flush=True); break
                except OSError: pass
        else: print('no frame', flush=True)
    else:
        open(rd + '/live', 'a').write(it + '\n'); time.sleep(1.5)
