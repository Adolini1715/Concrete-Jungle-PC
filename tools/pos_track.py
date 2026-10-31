#!/usr/bin/env python3
"""pos_track.py FRAMES_CSV T0-T1[,T2-T3...] [--kill T:R ...] [--every N] > track.txt

Turn the player positions of a gamelog frames.csv into a CJ_POS_TRACK file
(src/quicksave.c pos_track): keys every N game frames over the given time
ranges (seconds of that session), concatenated, so a cloud run walks the
player along a path a person played. --kill T:R adds "kill actors within R"
at session time T (inside a range). A heal key is added every 300 frames.
"""
import csv, sys

def main():
    a = sys.argv[1:]
    path, ranges = a[0], [tuple(map(float, r.split('-'))) for r in a[1].split(',')]
    kills, every = [], 6
    i = 2
    while i < len(a):
        if a[i] == '--kill': t, r = a[i + 1].split(':'); kills.append((float(t), float(r))); i += 2
        elif a[i] == '--every': every = int(a[i + 1]); i += 2
        else: sys.exit('bad arg ' + a[i])
    rows = list(csv.DictReader(open(path)))
    out, base = [], 0
    for t0, t1 in ranges:
        seg = [r for r in rows if t0 <= float(r['t']) <= t1]
        if not seg: continue
        f0 = int(seg[0]['frame'])
        last = None
        for r in seg:
            n = base + int(r['frame']) - f0
            if last is None or n - last >= every or r is seg[-1]:
                out.append((n, '%d %s %s %s' % (n, r['x'], r['y'], r['z'])))
                last = n
        for t, rad in kills:
            if t0 <= t <= t1:
                fr = min(seg, key=lambda r: abs(float(r['t']) - t))
                n = base + int(fr['frame']) - f0
                out.append((n, '%d K %g' % (n, rad)))
        base += int(seg[-1]['frame']) - f0 + 1
    for n in range(0, base, 300):
        out.append((n, '%d H' % n))
    out.sort(key=lambda x: x[0])
    sys.stdout.write(''.join(l + '\n' for _, l in out))
    sys.stderr.write('%d keys, %d frames\n' % (len(out), base))

if __name__ == '__main__':
    main()
