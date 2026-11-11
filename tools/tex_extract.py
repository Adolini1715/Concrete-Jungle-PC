#!/usr/bin/env python3
"""tex_extract.py OUT_DIR EDB [EDB ...] [--min 64] [--bd BUILDDATA]

Extract the textures of the named .edb files (e.g. d_mafia) straight from the disc as PNG,
named NN_<pack key>_<w>x<h>_<fmt>_<edb>.png. The 16-hex key is the texture-pack key
(src/texpack.c), so a redrawn PNG keeping that name drops straight into textures/replace.
Skips textures that reference another file, ones smaller than --min pixels on their longer
side, and duplicates (same key). Engine formats: 2 and 3 are DXT1 (NV2A 0x0C), 4 is DXT3 (0x0E).
Also writes _sheet.png (all of them, labelled) for an overview.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import edb_scan as E          # noqa: E402
import launcher_art as L      # noqa: E402

FMT = {2: (0x0C, False), 3: (0x0C, False), 4: (0x0E, True)}


def main():
    a = sys.argv[1:]
    mn = 64
    bd = E.BD
    if "--min" in a:
        i = a.index("--min"); mn = int(a[i + 1]); del a[i:i + 2]
    if "--bd" in a:
        i = a.index("--bd"); bd = a[i + 1]; del a[i:i + 2]
    out, names = a[0], [n.lower().removesuffix(".edb") for n in a[1:]]
    os.makedirs(out, exist_ok=True)
    seen, written, num = set(), [], 0
    for name, ln, off, part in E.files(bd):
        base = name.lower()[:-4]
        if base not in names:
            continue
        D = E.read(bd, off, ln, part)
        n, o = E.arr(D, 0xb8)
        for i in range(n):
            to = struct.unpack_from("<I", D, o + i * 0x1c + 8)[0]
            if to + 0x30 > len(D):
                continue
            w, h = struct.unpack_from("<HH", D, to)
            ef = D[to + 0x13]
            if ef not in FMT or struct.unpack_from("<I", D, to + 0x1c)[0] != 0xFFFFFFFF or max(w, h) < mn:
                continue
            fmt, three = FMT[ef]
            b = to + 0x2c + struct.unpack_from("<i", D, to + 0x2c)[0]
            size = w * h if three else w * h // 2
            data = D[b:b + size]
            if len(data) != size:
                continue
            key = L.texkey(data, fmt, w, h)
            if key in seen:
                continue
            seen.add(key)
            num += 1
            fn = "%02d_%016x_%dx%d_%02X_%s.png" % (num, key, w, h, fmt, base)
            L.png(os.path.join(out, fn), w, h, L.dxt(data, w, h, three))
            written.append(fn)
    print(f"{len(written)} textures -> {out}")
    try:   # overview sheet (needs Pillow; skipped without it)
        from PIL import Image, ImageDraw
        T, cols = 160, 8
        rows = (len(written) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * T, rows * (T + 14)), (40, 40, 40))
        d = ImageDraw.Draw(sheet)
        for k, fn in enumerate(written):
            im = Image.open(os.path.join(out, fn)).convert("RGBA")
            im.thumbnail((T, T))
            bg = Image.new("RGBA", im.size, (90, 90, 90, 255))
            bg.alpha_composite(im)
            x, y = (k % cols) * T, (k // cols) * (T + 14)
            sheet.paste(bg.convert("RGB"), (x, y))
            d.text((x + 2, y + T), fn.split("_")[0] + " " + fn.split("_")[2], fill=(255, 255, 0))
        sheet.save(os.path.join(out, "_sheet.png"))
    except ImportError:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
