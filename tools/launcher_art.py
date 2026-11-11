#!/usr/bin/env python3
"""launcher_art.py GAME_DIR OUT_DIR: take the launcher's art from the player's own disc.

The launcher (launcher/launcher.c) looks like the game because it uses the game's own logo and
loading-screen art, read here from the disc the player built the port from; none of it ships
with the port. Textures are found by the same key the texture packs use (src/texpack.c
texpack_key: format, size, level-0 bytes), so a different file layout cannot pick a wrong image.
Writes OUT_DIR/logo.png and OUT_DIR/bg.png. Plain Python (struct, zlib): the installer's
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
                png(os.path.join(out, name + ".png"), w, h, dxt(data, w, h, efmt == 4))
                got += 1
                break
    print(f"launcher art: {got} of {len(WANT)} images from the disc")
    return 0 if got == len(WANT) else 1


if __name__ == "__main__":
    sys.exit(main())
