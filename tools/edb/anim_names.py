#!/usr/bin/env python3
"""Put names on the animation-mode hashes in a log, summary.txt or events.jsonl.

    python3 -I tools/edb/anim_names.py <file> [names.h] [--graph docs/pred-anim-graph.json]

names.h: any header of '#define HT_Name 0xHASH' lines (not in the repo). Without it, modes get
their index (0x09xxxxxx low bits) and, from the graph, the clips they reach. Lines are printed
with each 8-digit 09xxxxxx hash followed by [name]."""
import json, os, re, sys


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    path = a[0]
    names = {}
    graph = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'docs', 'pred-anim-graph.json')
    i = 1
    while i < len(a):
        if a[i] == '--graph':
            graph = a[i + 1]; i += 2; continue
        for l in open(a[i], errors='replace'):
            m = re.search(r'#define\s+(HT_\w+)\s+(0x[0-9a-fA-F]+)', l)
            if m:
                names[int(m.group(2), 16)] = m.group(1).replace('HT_AnimMode_', '')
        i += 1
    clips = {}
    if os.path.exists(graph):
        g = json.load(open(graph))
        for r in g.get('rows', []):
            for m in r['modes']:
                clips.setdefault(m, []).append(r['anim'].replace('HT_Animation_', ''))

    def label(h):
        v = int(h, 16)
        if v in names:
            return names[v]
        c = clips.get(h) or clips.get('%08x' % v)
        return 'mode %d%s' % (v & 0xffffff, (': ' + ', '.join(c[:3])) if c else '')

    for l in open(path, errors='replace'):
        print(re.sub(r'\b(09[0-9a-fA-F]{6})\b', lambda m: '%s [%s]' % (m.group(1), label(m.group(1).lower())), l), end='')


if __name__ == '__main__':
    main()
