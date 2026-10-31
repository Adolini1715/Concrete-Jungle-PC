#!/usr/bin/env python3
"""Compare two frame-count based runs (docs/determinism.md): det_cmp.py <tagA> <tagB>
Per-frame [DET] data/heap hashes (first divergence), [SWAP] final lines, and the
flipped frames both runs dumped (run/ab/<tag>/frames)."""
import hashlib, os, re, sys
AB = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'run', 'ab')
def load(tag):
    det, swap, calls = {}, {}, []
    for l in open(os.path.join(AB, tag + '.log'), errors='replace'):
        m = re.match(r'\[DET\] f=(\d+) data=(\w+) heap=(\w+)', l)
        if m: det[int(m.group(1))] = (m.group(2), m.group(3))
        m = re.match(r'\[SWAP\] (final|f\d+) (\w+) (.*)', l)
        if m: swap[(m.group(1), m.group(2))] = m.group(3)
        m = re.match(r'\[SWAP\] (\w{8}) #(\d+) f=(\d+) .*', l)
        if m: calls.append(l.split(' ', 1)[1].strip())
    fr = {}
    d = os.path.join(AB, tag, 'frames')
    if os.path.isdir(d):
        for f in sorted(os.listdir(d)):
            fr[f] = hashlib.md5(open(os.path.join(d, f), 'rb').read()).hexdigest()
    return det, swap, calls, fr
a, b = load(sys.argv[1]), load(sys.argv[2])
common = sorted(set(a[0]) & set(b[0]))
for i, name in ((0, 'data'), (1, 'heap')):
    diff = [f for f in common if a[0][f][i] != b[0][f][i]]
    print(f'{name}: {len(common)} common frames, {len(diff)} differ' + (f', first f={diff[0]}' if diff else ''))
keys = sorted(set(a[1]) | set(b[1]))
sd = [k for k in keys if a[1].get(k) != b[1].get(k)]
print(f'swap reports: {len(keys)} lines, {len(sd)} differ')
for k in sd[:12]: print('  ', k, '\n     A', a[1].get(k), '\n     B', b[1].get(k))
for x, y in zip(a[2], b[2]):
    if x != y: print('first differing logged call:\n   A', x, '\n   B', y); break
cf = sorted(set(a[3]) & set(b[3]))
fd = [f for f in cf if a[3][f] != b[3][f]]
print(f'frames: {len(cf)} common (A {len(a[3])}, B {len(b[3])}), {len(fd)} differ' + (f': {fd[0]}..{fd[-1]}' if fd else ''))
