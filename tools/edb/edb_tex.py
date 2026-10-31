#!/usr/bin/env python3
"""Export the textures of an EngineX .edb (Predator: Concrete Jungle, version 250) as PNGs named
the way the port's texture packs expect (src/texpack.c): <key>_<w>x<h>_<fmt>.png, so an edited or
upscaled copy dropped in textures/replace/ replaces that texture in game.

    python3 -I tools/edb/edb_tex.py <file.edb | BuildData dir + name> <out dir>
    python3 -I tools/edb/edb_tex.py --archive <BuildData dir> pred.edb <out dir>

Header texture list at +0xb8 (EXGeoTextureHeader, 0x1c each: hash, flags, file offset of the
EXGeoTexture, ...). EXGeoTexture (include/EXGeoTexture.h): u16 w, h; u8 frames +0xc, images +0xd,
mips +0x12, format +0x13 (index into EXTexFmt: 0 R5G6B5, 1 X1R5G5B5, 2/3 DXT1, 4 DXT2,
5 A4R4G4B4, 6 A8R8G8B8, 7 P8, 8 A1R5G5B5, 9 linear A8R8G8B8); +0x1c 0xffffffff when the data is in
this file; frame relptrs from +0x2c (relative to the field). Swizzled formats are Morton order.
The key is texpack_key over level 0 as the GPU sees it, with the NV097 colour format code."""
import os, re, struct, sys
import numpy as np
from PIL import Image

NV = {0: 0x05, 1: 0x03, 2: 0x0C, 3: 0x0C, 4: 0x0E, 5: 0x04, 6: 0x06, 7: 0x0B, 8: 0x02, 9: 0x12}
NAMES = {0: 'R5G6B5', 1: 'X1R5G5B5', 2: 'DXT1', 3: 'DXT1', 4: 'DXT2', 5: 'A4R4G4B4', 6: 'A8R8G8B8',
         7: 'P8', 8: 'A1R5G5B5', 9: 'LIN_A8R8G8B8'}
M64 = (1 << 64) - 1


def u16(d, o): return struct.unpack_from('<H', d, o)[0]
def u32(d, o): return struct.unpack_from('<I', d, o)[0]


def texpack_key(data, fmt, w, h):
    k = 0xCBF29CE484222325
    P = 0x100000001B3
    for v in (fmt, w, h):
        k = ((k ^ v) * P) & M64
    n = len(data)
    i = 0
    words = np.frombuffer(data[:n - n % 8], dtype='<u8')
    for v in words.tolist():
        k = ((k ^ v) * P) & M64
        k ^= k >> 29
    i = n - n % 8
    for b in data[i:]:
        k = ((k ^ b) * P) & M64
    return k or 1


def morton(w, h):
    """Index into swizzled data for each (y, x): Xbox swizzle interleaves x and y bits while both
    have bits left, then the longer axis continues alone."""
    xs = np.zeros(w, dtype=np.int64)
    ys = np.zeros(h, dtype=np.int64)
    bit = 0
    bx = by = 1
    while bx < w or by < h:
        if bx < w:
            xs |= ((np.arange(w) & bx) != 0).astype(np.int64) << bit
            bit += 1
            bx <<= 1
        if by < h:
            ys |= ((np.arange(h) & by) != 0).astype(np.int64) << bit
            bit += 1
            by <<= 1
    return ys[:, None] | xs[None, :]


def rgb565(v):
    r = ((v >> 11) & 31) * 255 // 31
    g = ((v >> 5) & 63) * 255 // 63
    b = (v & 31) * 255 // 31
    return r, g, b


def dxt(data, w, h, kind):
    out = np.zeros((h, w, 4), dtype=np.uint8)
    bs = 8 if kind == 1 else 16
    p = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = data[p:p + bs]
            p += bs
            if kind == 1:
                cb = blk
                alpha = None
            else:
                alpha = struct.unpack_from('<Q', blk, 0)[0]
                cb = blk[8:]
            c0, c1, idx = struct.unpack_from('<HHI', cb, 0)
            a = rgb565(c0)
            b = rgb565(c1)
            if kind == 1 and c0 <= c1:
                pal = [a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0)]
            else:
                pal = [a + (255,), b + (255,), tuple((2 * x + y) // 3 for x, y in zip(a, b)) + (255,),
                       tuple((x + 2 * y) // 3 for x, y in zip(a, b)) + (255,)]
            for j in range(16):
                y, x = by + j // 4, bx + j % 4
                if y >= h or x >= w:
                    continue
                c = pal[(idx >> (2 * j)) & 3]
                if alpha is not None:
                    c = c[:3] + ((((alpha >> (4 * j)) & 15) * 17),)
                out[y, x] = c
    return out


def decode(data, w, h, f):
    if f in (2, 3):
        return dxt(data, w, h, 1)
    if f == 4:
        return dxt(data, w, h, 3)
    if f in (6, 9):
        px = np.frombuffer(data[:w * h * 4], dtype=np.uint8).reshape(-1, 4)
        if f == 6:
            px = px[morton(w, h).ravel()]
        px = px.reshape(h, w, 4)
        return px[:, :, [2, 1, 0, 3]].copy()
    if f in (0, 1, 5, 8):
        v = np.frombuffer(data[:w * h * 2], dtype='<u2').astype(np.uint32)
        v = v[morton(w, h).ravel()].reshape(h, w)
        if f == 0:
            r, g, b = (v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31
            a = np.full_like(v, 255)
        elif f == 5:
            a, r, g, b = (v >> 12 & 15) * 17, (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17
        else:
            a = np.where(v & 0x8000, 255, 0) if f == 8 else np.full_like(v, 255)
            r, g, b = (v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31
        return np.stack([r, g, b, a], -1).astype(np.uint8)
    return None


def level0_size(w, h, f):
    if f in (2, 3):
        return max(1, w // 4) * max(1, h // 4) * 8
    if f == 4:
        return max(1, w // 4) * max(1, h // 4) * 16
    if f in (6, 9):
        return w * h * 4
    if f == 7:
        return w * h
    return w * h * 2


def read_archive(build, name):
    for l in open(os.path.join(build, 'Filelist.txt'), errors='replace'):
        if l.lower().split(':')[0].strip().endswith('\\' + name.lower()) or ('\\' + name.lower() + ' ') in l.lower():
            m = re.search(r'Len\s+(\d+).*Loc\s+([0-9a-f]+):(\d+)', l, re.I)
            ln, off, part = int(m.group(1)), int(m.group(2), 16), int(m.group(3))
            with open(os.path.join(build, 'Filelist.%03d' % part), 'rb') as f:
                f.seek(off)
                return f.read(ln)
    sys.exit('not in Filelist.txt: ' + name)


def main():
    a = sys.argv[1:]
    if a and a[0] == '--archive':
        d = read_archive(a[1], a[2])
        out = a[3]
        tag = os.path.splitext(a[2])[0]
    else:
        d = open(a[0], 'rb').read()
        out = a[1]
        tag = os.path.splitext(os.path.basename(a[0]))[0]
    assert d[:4] == b'MOEG', 'not an .edb'
    os.makedirs(out, exist_ok=True)
    n = u16(d, 0xb8)
    rel = u32(d, 0xbc)
    base = 0xbc + rel
    done = skipped = 0
    for i in range(n):
        e = base + i * 0x1c
        h_, off = u32(d, e), u32(d, e + 8)
        if not off or off + 0x30 > len(d):
            skipped += 1
            continue
        w, h = u16(d, off), u16(d, off + 2)
        fmt = d[off + 0x13]
        if u32(d, off + 0x1c) != 0xffffffff or fmt not in NV or not w or not h:
            skipped += 1
            continue
        r = u32(d, off + 0x2c)
        p = (off + 0x2c + r) & 0xffffffff
        sz = level0_size(w, h, fmt)
        if not r or p + sz > len(d):
            skipped += 1
            continue
        raw = d[p:p + sz]
        img = decode(raw, w, h, fmt)
        if img is None:
            skipped += 1
            continue
        key = texpack_key(raw, NV[fmt], w, h)
        name = '%016x_%dx%d_%02X_%s_%08x.png' % (key, w, h, NV[fmt], tag, h_)
        Image.fromarray(img, 'RGBA').save(os.path.join(out, name))
        done += 1
    print('%s: %d textures written, %d skipped (external, palettised or not loaded from this file)' % (tag, done, skipped))


if __name__ == '__main__':
    main()
