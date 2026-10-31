"""Walk an EngineX .edb's animation graph (Predator: Concrete Jungle, version 250):
anim modes -> transitions (keyed by the previous mode's index) -> commands -> anim sets -> items.

On disk a header list (EXGeoCommonArray) is: s16 count... actually u16 count, s16 hash count,
then a relative offset from the second dword's own address to the elements. Elements:
  mode/set header (0x14): u32 hash, u32 flags (low 15 bits: section), u32 file offset, u32 runtime ptr, u32 count
  anim header (0x20):     u32 hash, u32 flags, u32 offset, u32 ptr, motion offset, ...
Offsets are absolute in the file for section 0, else relative to the section's seek position.

Mode data (ChangeToAnimMode 0x1CC530): count transitions of {u32 from-mode index, relptr cmdlist};
cmdlist: u32 n, then n relptrs to commands {u32 type, u32 set hash, u16 frames, u16 flags, u32 param}
  types 0xc000002/3 blend to set, 0xc000004 change to set, 0xc000005 snapshot, 0xc000006 handler callback.
Set data (BlendToAnimSet): count entries {u16 stack, s16 n, f32 blend, n x {u32 hash, u16 flags, u16 frames, u32 spline}}.

Usage: python3 -I edb_anim.py <edb> [names.h] [--json out.json]"""
import struct, sys, json, re

def u16(d, o): return struct.unpack_from('<H', d, o)[0]
def s16(d, o): return struct.unpack_from('<h', d, o)[0]
def u32(d, o): return struct.unpack_from('<I', d, o)[0]
def f32(d, o): return struct.unpack_from('<f', d, o)[0]

class Edb:
    def __init__(self, d):
        self.d = d
        assert d[:4] == b'MOEG'
        self.sections = self.lst(0x40, 0x10, raw=True)
    def lst(self, off, stride, raw=False):
        d = self.d
        n = u16(d, off)
        rel = u32(d, off + 4)
        base = off + 4 + rel if rel else 0
        out = []
        for i in range(n):
            e = base + i * stride
            if raw:
                out.append((u32(d, e), u32(d, e + 4), u32(d, e + 8)))
            else:
                out.append(dict(hash=u32(d, e), flags=u32(d, e + 4), off=u32(d, e + 8), count=u32(d, e + 16) if stride >= 0x14 else None, at=e))
        return out
    def addr(self, el):
        # absolute file offset; the section number (flags & 0x7fff) only says which streamed
        # chunk loads the object
        return el['off'] if el['off'] < len(self.d) else None

def relp(d, o):
    r = u32(d, o)
    return (o + r) & 0xffffffff if r else None

def main():
    path = sys.argv[1]
    names = {}
    for a in sys.argv[2:]:
        if a.endswith('.h'):
            for l in open(a, errors='replace'):
                m = re.search(r'#define\s+(HT_\w+)\s+(0x[0-9a-fA-F]+)', l)
                if m: names[int(m.group(2), 16)] = m.group(1)
    nm = lambda h: names.get(h, '%08x' % h)
    d = open(path, 'rb').read()
    E = Edb(d)
    anims = E.lst(0x58, 0x20)
    modes = E.lst(0x78, 0x14)
    sets = E.lst(0x80, 0x14)
    scripts = E.lst(0x68, 0x10)
    print('%s: %d anims, %d modes, %d sets, %d scripts, %d sections' % (path, len(anims), len(modes), len(sets), len(scripts), len(E.sections)))
    def idx(h, lst):
        if (h & 0xff000000) == 0 or (h >> 31): return h & 0xffffff
        for i, e in enumerate(lst):
            if e['hash'] == h: return i
        return None
    setname = lambda h: (nm(sets[idx(h, sets)]['hash']) if idx(h, sets) is not None and idx(h, sets) < len(sets) else '?%08x' % h)
    scrname = lambda h: (nm(scripts[idx(h, scripts)]['hash']) if idx(h, scripts) is not None and idx(h, scripts) < len(scripts) else '?%08x' % h)
    out = dict(modes={}, sets={}, anims=[nm(a['hash']) for a in anims])
    # sets
    for s in sets:
        a = E.addr(s)
        entries = []
        if a is not None:
            p = a
            for i in range(s['count']):
                stack, n, blend = u16(d, p), s16(d, p + 2), f32(d, p + 4)
                items = []
                for j in range(max(n, 0)):
                    q = p + 8 + j * 12
                    items.append(dict(hash=scrname(u32(d, q)), flags=u16(d, q + 4), frames=u16(d, q + 6), spline=u32(d, q + 8)))
                entries.append(dict(stack=stack, blend=round(blend, 3), items=items))
                p += 8 + max(n, 0) * 12
        out['sets'][nm(s['hash'])] = entries
    # modes
    for m in modes:
        a = E.addr(m)
        trans = []
        if a is not None:
            for i in range(m['count']):
                t = a + i * 8
                frm = u32(d, t)
                cl = relp(d, t + 4)
                cmds = []
                if cl is not None:
                    n = u32(d, cl)
                    for k in range(n):
                        c = relp(d, cl + 4 + 4 * k)
                        if c is None: continue
                        typ = u32(d, c)
                        cmds.append(dict(type={0xc000002: 'blend', 0xc000003: 'blend+', 0xc000004: 'change', 0xc000005: 'snapshot', 0xc000006: 'callback'}.get(typ, '%08x' % typ),
                                         set=setname(u32(d, c + 4)), frames=u16(d, c + 8), flags=u16(d, c + 10), param=u32(d, c + 12)))
                trans.append(dict(frm=(nm(modes[frm]['hash']) if frm < len(modes) else frm), cmds=cmds))
        out['modes'][nm(m['hash'])] = trans
    if '--json' in sys.argv:
        json.dump(out, open(sys.argv[sys.argv.index('--json') + 1], 'w'), indent=1)
    return out

if __name__ == '__main__':
    o = main()
    k = list(o['modes'])[:3]
    for m in k: print(m, json.dumps(o['modes'][m])[:400])
    s = list(o['sets'])[:3]
    for m in s: print(m, json.dumps(o['sets'][m])[:300])

def scripts_refs(path):
    """script hash -> list of (cmd type, file hash, anim ref, thread): every (file, anim) pair in a
    command payload (blend spaces list several). file 0xffffffff = this file; anim refs are
    0x03xxxxxx hashes or 0x83 + index."""
    d = open(path, 'rb').read()
    E = Edb(d)
    out = {}
    for s in E.lst(0x68, 0x10):
        a = E.addr(s)
        refs = []
        if a is not None and a + 0x44 < len(d):
            i = a + 0x44
            for _ in range(4000):
                if i + 4 > len(d): break
                t, sz = d[i], d[i + 1]
                if t == 0x12 or sz == 0: break
                for k in range(4, sz - 4, 1):
                    f, o = u32(d, i + k), u32(d, i + k + 4)
                    if (f == 0xffffffff or (f >> 24) == 0x01) and (o >> 24) & 0x7f == 0x03:
                        refs.append((t, f, o, u16(d, i + 8)))
                i += sz
        out[s['hash']] = refs
    return out
