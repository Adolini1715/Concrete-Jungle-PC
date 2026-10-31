#!/usr/bin/env python3
"""Per-phase GPU census: difference of RECOMP_GPU_CENSUS snapshots.

usage: census_phases.py <snapshot dir> <flip,flip,...> [top]
Each snapshot is a copy of the census file (taken every 10 s by the run
helper); a phase a..b is snapshot(<=b) minus snapshot(<=a), keyed by the
'X flips' counter in each file. Format: header of nv2a_gpu_census.c.
"""
import sys, glob, collections


def load(f):
    d = collections.OrderedDict()
    for l in open(f):
        p = l.split()
        if not p:
            continue
        k = p[0]
        if k == 'X':
            d[('X', p[1])] = int(p[2])
        elif k == 'P':
            d[('P', p[1])] = (int(p[3]), 'len=%s' % p[2])
        elif k == 'R':
            d[('R', p[1])] = (int(p[3]), 'stages=%s ctl=%s stageprog=%s' % (p[2], p[5], p[6]))
        elif k == 'T':
            d[('T', 'fmt=%s dim=%s cube=%s lv=%s %sx%s pitch=%s' % tuple(p[1:8]) + ' rt=' + p[10])] = (int(p[8]), '')
        elif k == 'S':
            d[('S', 'fmt=%s pitch=%s %sx%s aa=%s off=%s' % (p[1], p[2], p[3], p[4], p[5], p[8]))] = (int(p[6]), '')
        elif k == 'D':
            d[('D', 'prim=%s mode=%s' % (p[1], p[3]))] = (int(p[2]), '')
        elif k == 'V':
            d[('V', 'attr=%s type=%s size=%s stride=%s' % tuple(p[1:5]))] = (int(p[5]), '')
    return d


def main():
    snaps = {}
    for f in glob.glob(sys.argv[1] + '/*.txt'):
        for l in open(f):
            if l.startswith('X flips'):
                snaps[int(l.split()[2])] = f
    keys = sorted(snaps)
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 10

    def at(n):
        ks = [k for k in keys if k <= n]
        return (load(snaps[max(ks)]), max(ks)) if ks and n > 0 else ({}, 0)

    b = [int(x) for x in sys.argv[2].split(',')]
    for a, c in zip(b, b[1:]):
        (A, sa), (C, sc) = at(a), at(c)
        print('=== flips %d..%d (snapshots %d..%d)' % (a, c, sa, sc))
        for t in 'XPRTSDV':
            rows = []
            for k, v in C.items():
                if k[0] != t:
                    continue
                if t == 'X':
                    rows.append((v - A.get(k, 0), k[1], ''))
                    continue
                dv = v[0] - (A[k][0] if k in A else 0)
                if dv > 0:
                    rows.append((dv, k[1], v[1] + ('' if k in A else ' NEW')))
            rows.sort(reverse=True)
            if t == 'X':
                print('  X', ' '.join('%s=%d' % (r[1], r[0]) for r in rows))
            else:
                print('  %s distinct=%d' % (t, len(rows)))
                for r in rows[:top]:
                    print('    %8d %s %s' % r)


if __name__ == '__main__':
    main()
