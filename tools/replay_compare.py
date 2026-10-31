#!/usr/bin/env python3
"""Compare a CJ_PAD_REPLAY_SYNC run with the xemu recording it replays.

    tools/replay_compare.py run/TAG/log.txt <session>/frames.csv [--every 10]

Prints, at recording time t, the port's player position (the [RP] lines),
xemu's (frames.csv, interpolated) and the distance between them, then the
time the two first drift more than 1, 3 and 10 units apart.
"""
import argparse, bisect, csv, math, re

ap = argparse.ArgumentParser()
ap.add_argument("log"); ap.add_argument("frames")
ap.add_argument("--every", type=float, default=10.0)
a = ap.parse_args()

xs = []
for r in csv.DictReader(open(a.frames)):
    if r["player"] in ("0", "") or r["level_loaded"] != "1":
        continue
    xs.append((float(r["t"]), float(r["x"]), float(r["y"]), float(r["z"]), float(r["health"])))
ts = [x[0] for x in xs]

def ref(t):
    i = bisect.bisect_left(ts, t)
    if i <= 0 or i >= len(xs):
        return None
    a0, a1 = xs[i - 1], xs[i]
    if a1[0] - a0[0] > 1.0:
        return None
    f = (t - a0[0]) / (a1[0] - a0[0])
    return tuple(a0[k] + (a1[k] - a0[k]) * f for k in (1, 2, 3)) + (a1[4],)

rx = re.compile(r"\[RP\] t=([\d.]+) x=(\S+) y=(\S+) z=(\S+) hp=(\S+)")
first = {}
last_print = -1e9
for line in open(a.log, errors="replace"):
    m = rx.search(line)
    if not m:
        continue
    t, x, y, z, hp = map(float, m.groups())
    r = ref(t)
    if not r:
        continue
    d = math.dist((x, y, z), r[:3])
    for lim in (1, 3, 10):
        if d > lim and lim not in first:
            first[lim] = t
    if t - last_print >= a.every:
        last_print = t
        print(f"t={t:7.2f}  port {x:8.2f} {y:7.2f} {z:8.2f} hp {hp:5.1f} | xemu {r[0]:8.2f} {r[1]:7.2f} {r[2]:8.2f} hp {r[3]:5.1f} | d={d:6.2f}")
for lim in (1, 3, 10):
    print(f"first drift > {lim}: " + (f"t={first[lim]:.2f}" if lim in first else "never"))
