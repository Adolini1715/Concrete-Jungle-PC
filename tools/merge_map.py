#!/usr/bin/env python3
"""Compare tools.disasm functions.json with the linker map and write map names into it."""
import json, csv, re, sys, collections
fj, mapcsv = sys.argv[1], sys.argv[2]
F = json.load(open(fj))
rows = list(csv.DictReader(open(mapcsv)))
m = {int(r['va'], 16): r for r in rows}
d = {int(f['start'], 16): f for f in F}
missing = sorted(set(m) - set(d)); extra = sorted(set(d) - set(m))
print('map', len(m), 'disasm', len(d), 'map-not-found', len(missing), 'disasm-not-in-map', len(extra))
print(' missing by origin', collections.Counter(m[a]['origin'] for a in missing))
print(' extra by section', collections.Counter(d[a]['section'] for a in extra))
for a in missing[:15]: print('  miss %08x %s %s' % (a, m[a]['origin'], m[a]['name']))
used = collections.Counter()
def cname(r):
    n = r['name']
    if n.startswith('_') and '@' in n and not n.startswith('?'):  # _Foo@12 stdcall
        n = n[1:n.index('@')]
    elif n.startswith('_') and not n.startswith('?'):
        n = n[1:]
    n = re.sub(r'[^A-Za-z0-9_]', '_', n).strip('_')
    return n or 'fn'
named = 0
for a, f in d.items():
    if a in m:
        base = 'm_' + cname(m[a])[:80]
        f['name'] = '%s_%08X' % (base, a)
        named += 1
json.dump(F, open(fj, 'w'))
print('named', named)
