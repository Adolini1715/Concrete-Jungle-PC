#!/usr/bin/env python3
"""launcher_art.py GAME_DIR OUT_DIR: take the launcher's art from the player's own disc.

The launcher (launcher/launcher.c) looks like the game because it uses the game's own logo and
loading-screen art, read here from the disc the player built the port from; none of it ships
with the port. Textures are found by the same key the texture packs use (src/texpack.c
texpack_key: format, size, level-0 bytes), so a different file layout cannot pick a wrong image.
Writes OUT_DIR/logo.png, OUT_DIR/bg.png and the icon (OUT_DIR/icon.png, OUT_DIR/pcj.ico: the
menu's red clan glyph, cleaned up and drawn on a dark rounded tile). Plain Python (struct, zlib): the installer's
private Python runs it.

EDB texture table (EXGeoHeader +0xb8, entries of 0x1c): +8 offset of the EXGeoTexture
(pcj-decomp include/EXGeoTexture.h): +0 width, +2 height, +0x13 engine format (2 DXT1, 4 DXT3),
+0x2c relative pointer to frame 0 (level 0 first).
"""
import os
import re
import struct
import sys
import zlib

M64 = 0xFFFFFFFFFFFFFFFF
# name: (key, file, width, height, NV2A format of the key, engine format)
WANT = {
    "bg": (0x9d18f6dd59a274bc, "loading.edb", 512, 512, 0x0C, 2),
    "logo": (0x96ee9703a985c1ba, "logo.edb", 512, 256, 0x0E, 4),
    "glyph": (0x2219a1d5be5e0c12, "front_en.edb", 64, 64, 0x0E, 4),   # the red clan glyph: the icon
}


def texkey(d, fmt, w, h):
    k = 0xCBF29CE484222325
    for v in (fmt, w, h):
        k = ((k ^ v) * 0x100000001B3) & M64
    n = len(d) - len(d) % 8
    for v in struct.unpack_from("<%dQ" % (n // 8), d):
        k = ((k ^ v) * 0x100000001B3) & M64
        k ^= k >> 29
    for b in d[n:]:
        k = ((k ^ b) * 0x100000001B3) & M64
    return k or 1


def c565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def dxt(d, w, h, three):
    """DXT1 / DXT3 (linear block order) -> RGBA bytes"""
    out = bytearray(w * h * 4)
    p = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            alpha = None
            if three:
                alpha = struct.unpack_from("<Q", d, p)[0]
                p += 8
            c0, c1, bits = struct.unpack_from("<HHI", d, p)
            p += 8
            a, b = c565(c0), c565(c1)
            if c0 > c1 or three:
                pal = [a + (255,), b + (255,), tuple((2 * x + y) // 3 for x, y in zip(a, b)) + (255,),
                       tuple((x + 2 * y) // 3 for x, y in zip(a, b)) + (255,)]
            else:
                pal = [a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0)]
            for py in range(4):
                for px in range(4):
                    i = py * 4 + px
                    r, g, bb, al = pal[(bits >> (2 * i)) & 3]
                    if alpha is not None:
                        al = ((alpha >> (4 * i)) & 15) * 17
                    o = ((by + py) * w + bx + px) * 4
                    out[o:o + 4] = bytes((r, g, bb, al))
    return bytes(out)


def png(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def make_icon(rgba, w, h, out):
    """The clan glyph (64x64 RGBA) -> a crisp red glyph on a dark rounded tile: icon.png (256) and
    pcj.ico (16-256). Plain Python: mask from alpha and redness, blurred a little, scaled up with
    bilinear filtering and sharpened with a smooth threshold so the edges stay clean."""
    m = [[0.0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b, a = rgba[(y * w + x) * 4:(y * w + x) * 4 + 4]
            m[y][x] = a / 255.0 * min(1.0, max(0.0, (r - g) / 180.0))
    pts = [(x, y) for y in range(h) for x in range(w) if m[y][x] > 0.16]
    if not pts:
        return False
    x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts) + 1
    y0, y1 = min(p[1] for p in pts), max(p[1] for p in pts) + 1
    bl = [[0.0] * w for _ in range(h)]   # 3x3 box blur: smoother contours after the upscale
    for y in range(h):
        for x in range(w):
            acc = n = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < h and 0 <= xx < w:
                        acc += m[yy][xx]; n += 1
            bl[y][x] = acc / n
    S = 512
    gw, gh = x1 - x0, y1 - y0
    sc = S * 0.72 / max(gw, gh)
    ox, oy = (S - gw * sc) / 2, (S - gh * sc) / 2
    rad = S // 6
    px = bytearray(S * S * 4)
    for y in range(S):
        t = y / (S - 1)
        cr, cg, cb = int(235 - 50 * t), int(18 * (1 - t)), int(14 * (1 - t))
        for x in range(S):
            # rounded tile (antialiased by distance to the corner circle)
            cx = rad - x if x < rad else (x - (S - 1 - rad) if x > S - 1 - rad else 0)
            cy = rad - y if y < rad else (y - (S - 1 - rad) if y > S - 1 - rad else 0)
            ta = min(1.0, max(0.0, rad - (cx * cx + cy * cy) ** 0.5 + 0.5)) if (cx or cy) else 1.0
            # glyph coverage
            gx, gy = (x - ox) / sc + x0 - 0.5, (y - oy) / sc + y0 - 0.5
            ix, iy = int(gx // 1), int(gy // 1)
            v = 0.0
            if -1 <= ix < w and -1 <= iy < h:
                fx, fy = gx - ix, gy - iy
                def at(a, b):
                    return bl[b][a] if 0 <= a < w and 0 <= b < h else 0.0
                v = (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) + (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy
            k = min(1.0, max(0.0, (v - 0.30) / 0.08))
            o = (y * S + x) * 4
            px[o] = int(18 + (cr - 18) * k); px[o + 1] = int(15 + (cg - 15) * k); px[o + 2] = int(15 + (cb - 15) * k)
            px[o + 3] = int(255 * ta)

    def down(src, s, d):   # area average s -> d (d divides s or not: box over the covered span)
        f = s / d
        res = bytearray(d * d * 4)
        for y in range(d):
            ya, yb = int(y * f), max(int(y * f) + 1, int((y + 1) * f))
            for x in range(d):
                xa, xb = int(x * f), max(int(x * f) + 1, int((x + 1) * f))
                acc = [0, 0, 0, 0]; n = 0
                for yy in range(ya, yb):
                    row = yy * s
                    for xx in range(xa, xb):
                        o = (row + xx) * 4; al = src[o + 3]
                        acc[0] += src[o] * al; acc[1] += src[o + 1] * al; acc[2] += src[o + 2] * al; acc[3] += al; n += 1
                o = (y * d + x) * 4
                if acc[3]:
                    res[o:o + 3] = bytes(min(255, c // acc[3]) for c in acc[:3])
                res[o + 3] = acc[3] // n
        return res
    import io
    pngs = []
    for d in (256, 128, 64, 48, 32, 24, 16):
        img = down(px, S, d)
        path = os.path.join(out, "icon.png" if d == 256 else "icon-%d.png" % d)
        png(path, d, d, bytes(img))
        pngs.append((d, open(path, "rb").read()))
        if d != 256:
            os.remove(path)
    with open(os.path.join(out, "pcj.ico"), "wb") as f:   # ICO with PNG entries (Vista and later)
        f.write(struct.pack("<HHH", 0, 1, len(pngs)))
        off = 6 + 16 * len(pngs)
        for d, data in pngs:
            f.write(struct.pack("<BBBBHHII", d % 256, d % 256, 0, 0, 1, 32, len(data), off))
            off += len(data)
        for d, data in pngs:
            f.write(data)
    return True


def find_builddata(game):
    for root, dirs, files in os.walk(game):
        if "Filelist.txt" in files:
            return root
    return None


def main():
    game, out = sys.argv[1], sys.argv[2]
    bd = find_builddata(game)
    if not bd:
        print("launcher art: no BuildData/Filelist.txt under", game)
        return 1
    where = {}
    for line in open(os.path.join(bd, "Filelist.txt"), errors="replace"):
        m = re.match(r"\S*\\(\S+\.edb)\s*: Len\s+(\d+).*Loc\s+([0-9a-f]+):(\d+)", line.strip())
        if m:
            where[m.group(1).lower()] = (int(m.group(2)), int(m.group(3), 16), m.group(4))
    os.makedirs(out, exist_ok=True)
    got = 0
    for name, (key, edb, w, h, fmt, efmt) in WANT.items():
        if edb not in where:
            continue
        ln, off, part = where[edb]
        with open(os.path.join(bd, "Filelist." + part), "rb") as f:
            f.seek(off)
            D = f.read(ln)
        n = struct.unpack_from("<h", D, 0xb8)[0]
        o = 0xb8 + 4 + struct.unpack_from("<i", D, 0xb8 + 4)[0]
        for i in range(n):
            e = o + i * 0x1c
            to = struct.unpack_from("<I", D, e + 8)[0]
            if to + 0x30 > len(D) or struct.unpack_from("<HH", D, to) != (w, h) or D[to + 0x13] != efmt:
                continue
            base = to + 0x2c + struct.unpack_from("<i", D, to + 0x2c)[0]
            size = w * h // 2 if efmt == 2 else w * h
            data = D[base:base + size]
            if len(data) == size and texkey(data, fmt, w, h) == key:
                if name == "glyph":
                    got += make_icon(dxt(data, w, h, efmt == 4), w, h, out)
                else:
                    png(os.path.join(out, name + ".png"), w, h, dxt(data, w, h, efmt == 4))
                    got += 1
                break
    print(f"launcher art: {got} of {len(WANT)} images from the disc")
    return 0 if got == len(WANT) else 1


if __name__ == "__main__":
    sys.exit(main())
