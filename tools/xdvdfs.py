# Minimal XDVDFS (Xbox disc filesystem) lister/extractor.
import struct, sys, os
MAGIC = b'MICROSOFT*XBOX*MEDIA'
def find_base(f):
    for base in (0, 0x18300000, 0x30600000, 0x2080000, 0xFD90000):
        f.seek(base + 32*2048); 
        if f.read(20) == MAGIC: return base
    raise SystemExit('no XDVDFS header found')
def walk(f, base, sec, size, path, out):
    f.seek(base + sec*2048); data = f.read(size)
    stack = [0]
    seen=set()
    while stack:
        off = stack.pop()
        if off*4 >= len(data) or off in seen: continue
        seen.add(off)
        e = data[off*4:]
        if len(e) < 14 or e[:4] == b'\xff\xff\xff\xff': continue
        l, r, s, sz, attr, nl = struct.unpack_from('<HHIIBB', e)
        name = e[14:14+nl].decode('latin-1')
        p = path + '/' + name
        if attr & 0x10:
            if sz: walk(f, base, s, sz, p, out)
        else:
            out.append((p, s, sz))
        if l: stack.append(l)
        if r: stack.append(r)
def listing(img):
    f = open(img, 'rb'); base = find_base(f)
    f.seek(base + 32*2048 + 20); rs, rz = struct.unpack('<II', f.read(8))
    out = []; walk(f, base, rs, rz, '', out); return f, base, out
if __name__ == '__main__':
    img, cmd = sys.argv[1], sys.argv[2]
    f, base, files = listing(img)
    if cmd == 'ls':
        for p, s, sz in sorted(files): print(f'{sz:12d} {p}')
    elif cmd == 'x':
        dest = sys.argv[3]
        for p, s, sz in files:
            if len(sys.argv) > 4 and p.lower() not in [a.lower() for a in sys.argv[4:]]: continue
            o = os.path.join(dest, p.lstrip('/')); os.makedirs(os.path.dirname(o), exist_ok=True)
            f.seek(base + s*2048); left = sz
            with open(o, 'wb') as w:
                while left:
                    b = f.read(min(left, 1<<24))
                    if not b: raise SystemExit(f'{img} ends inside {p}: the disc image is incomplete')
                    w.write(b); left -= len(b)
