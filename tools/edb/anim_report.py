"""Full Predator animation graph report: code -> mode -> set -> script -> animation."""
import sys, json, struct, re, collections
sys.path.insert(0, '/home/claude/pcj-tools'); sys.path.insert(0, '/home/claude/pcj-decomp/tools')
import edb_anim
from xbe import XBE
names = {}
for l in open('/home/claude/pcj/proto_aug04/Predator/Albert/hashcodes.h', errors='replace'):
    m = re.search(r'#define\s+(HT_\w+)\s+(0x[0-9a-fA-F]+)', l)
    if m: names[int(m.group(2), 16)] = m.group(1)
nm = lambda h: names.get(h, '%08x' % h)
d = open('pred.edb', 'rb').read(); E = edb_anim.Edb(d)
anims = E.lst(0x58, 0x20); modes = E.lst(0x78, 0x14); sets = E.lst(0x80, 0x14); scripts = E.lst(0x68, 0x10)
def resolve(h, lst):
    if (h & 0xff000000) == 0 or (h >> 31): i = h & 0xffffff
    else: i = next((k for k, e in enumerate(lst) if e['hash'] == h), None)
    return lst[i]['hash'] if i is not None and i < len(lst) else None
# code references: 32-bit immediates in .text
x = XBE('/home/claude/pcj-decomp/orig/DEFAULT.XBE'); t = x.sec('.text')
code = x.d[t['raw']:t['raw'] + t['rsize']]
imm = set(struct.unpack_from('<I', code, i)[0] for i in range(0, len(code) - 4))
# mode -> set (Default transitions keyed by target mode index)
dflt = [m for m in modes if m['hash'] == 0x09000004][0]
a = E.addr(dflt); mode_set = {}
for i in range(dflt['count']):
    tgt = edb_anim.u32(d, a + i * 8); cl = edb_anim.relp(d, a + i * 8 + 4)
    if cl is None or tgt >= len(modes): continue
    for k in range(edb_anim.u32(d, cl)):
        c = edb_anim.relp(d, cl + 4 + 4 * k)
        sh = resolve(edb_anim.u32(d, c + 4), sets)
        mode_set.setdefault(modes[tgt]['hash'], []).append(sh)
# set -> scripts
set_scripts = {}
for s in sets:
    p = E.addr(s); lst = []
    for i in range(s['count']):
        n = edb_anim.s16(d, p + 2)
        for j in range(max(n, 0)):
            lst.append(resolve(edb_anim.u32(d, p + 8 + j * 12), scripts))
        p += 8 + max(n, 0) * 12
    set_scripts[s['hash']] = lst
sref = edb_anim.scripts_refs('pred.edb')
script_anims = {h: [resolve(o, anims) if f == 0xffffffff else ('ext', f, o) for _, f, o, _ in v] for h, v in sref.items()}
# invert
anim_users = collections.defaultdict(list)
for mh, shs in mode_set.items():
    for sh in shs:
        for sc in set_scripts.get(sh, []):
            for an in script_anims.get(sc, []):
                if isinstance(an, int): anim_users[an].append((mh, sh, sc))
rows = []
for an in anims:
    h = an['hash']; us = anim_users.get(h, [])
    incode = any(m in imm for m, _, _ in us)
    rows.append(dict(anim=nm(h), modes=sorted(set(nm(m) for m, _, _ in us)), mode_in_code=incode))
json.dump(dict(rows=rows, mode_set={nm(k): [nm(x) for x in v if x] for k, v in mode_set.items()},
               set_scripts={nm(k): [nm(x) for x in v if x] for k, v in set_scripts.items()},
               script_anims={nm(k): [nm(x) if isinstance(x, int) else 'ext' for x in v] for k, v in script_anims.items()},
               modes_in_code=sorted(nm(m['hash']) for m in modes if m['hash'] in imm)), open('pred_graph.json', 'w'), indent=1)
c = collections.Counter(('reached' if r['modes'] else 'no mode') + (' / code' if r['mode_in_code'] else '') for r in rows)
print(len(anims), 'anims:', dict(c)); print(len([m for m in modes if m['hash'] in imm]), 'of', len(modes), 'modes named in code')
kw = re.compile('roll|flip|sprint|land_to|land_roll|wallcling|wall_climb|slide|super|jump_forward|dodge|powerland|wallslide|strafe', re.I)
for r in rows:
    if kw.search(r['anim']): print(f"{r['anim'][8:]:40} modes: {', '.join(m[12:] for m in r['modes']) or '-'}  {'(code)' if r['mode_in_code'] else ''}")
