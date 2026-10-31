#!/usr/bin/env python3
"""Rebuild the game's bitmap-font atlases at N x resolution from a vector font.

Each glyph is found in the dumped atlas (rows of ink, then column runs; '"'
is two runs) and the vector glyph is rasterised straight into the same box
at N x, so the engine's UV rectangles and metrics (unchanged) land on it.
No resampling of the original pixels: the original only supplies positions.

usage: fonts.py atlas.png out.png font.ttf ROWS... [--scale 4] [--stroke 0.02]
ROWS are the characters of each atlas row in order (shell-quote them).
"""
import sys
import numpy as np
from PIL import Image, ImageFont, ImageDraw

def runs(mask):
    out, i, n = [], 0, len(mask)
    while i < n:
        if mask[i]:
            j = i
            while j < n and mask[j]: j += 1
            out.append((i, j)); i = j
        else: i += 1
    return out

def main():
    args = sys.argv[1:]
    scale = 4
    stroke = 0.0
    if '--stroke' in args:   # extra weight, as a fraction of the em (the originals run heavier than Arial)
        k = args.index('--stroke'); stroke = float(args[k + 1]); del args[k:k + 2]
    if '--scale' in args:
        k = args.index('--scale'); scale = int(args[k + 1]); del args[k:k + 2]
    src, dst, ttf, rows = args[0], args[1], args[2], args[3:]
    a = np.array(Image.open(src).convert('RGBA'))[:, :, 3].astype(np.int32)
    H, W = a.shape
    bands = runs((a > 8).any(1))
    if len(bands) != len(rows):
        sys.exit(f'{src}: {len(bands)} ink rows, {len(rows)} given')
    big = 512
    font = ImageFont.truetype(ttf, big)
    out = Image.new('L', (W * scale, H * scale), 0)
    for (y0, y1), chars in zip(bands, rows):
        blobs = runs((a[y0:y1] > 8).any(0))
        chars = chars.replace(" ", "")
        need = sum(2 if c == "\"" else 1 for c in chars)
        if need != len(blobs):
            sys.exit(f'row {y0}-{y1}: {len(blobs)} glyph runs, {need} expected for {chars!r}')
        bi = 0
        for c in chars:
            n = 2 if c == '"' else 1
            bx0, bx1 = blobs[bi][0], blobs[bi + n - 1][1]
            bi += n
            cell = a[y0:y1, bx0:bx1]
            # ink box at half coverage (the anti-aliased fringe is not part of the shape)
            ys, xs = np.nonzero(cell >= 128)
            if len(xs) == 0: ys, xs = np.nonzero(cell > 8)
            gx0, gx1 = bx0 + xs.min(), bx0 + xs.max() + 1
            gy0, gy1 = y0 + ys.min(), y0 + ys.max() + 1
            # vector glyph, rendered large, its own half-coverage ink box
            g = Image.new('L', (big * 2, big * 2), 0)
            sw = int(round(stroke * big))
            ImageDraw.Draw(g).text((big // 2, big // 4), c, font=font, fill=255, stroke_width=sw, stroke_fill=255)
            ga = np.array(g)
            ys2, xs2 = np.nonzero(ga >= 128)
            box = (xs2.min(), ys2.min(), xs2.max() + 1, ys2.max() + 1)
            tw, th = (gx1 - gx0) * scale, (gy1 - gy0) * scale
            # pad by the fringe so the soft edge survives the downscale
            pad_x = (box[2] - box[0]) / tw * 2
            pad_y = (box[3] - box[1]) / th * 2
            crop = g.crop((int(box[0] - pad_x), int(box[1] - pad_y), int(box[2] + pad_x + 1), int(box[3] + pad_y + 1)))
            ox = int(round(gx0 * scale - 2)); oy = int(round(gy0 * scale - 2))
            glyph = crop.resize((tw + 4, th + 4), Image.LANCZOS)
            region = out.crop((ox, oy, ox + glyph.width, oy + glyph.height))
            out.paste(Image.fromarray(np.maximum(np.array(region), np.array(glyph))), (ox, oy))
    rgba = Image.merge('RGBA', [Image.new('L', out.size, 255)] * 3 + [out])
    rgba.save(dst)
    print(f'{dst}: {out.size[0]}x{out.size[1]}')

if __name__ == '__main__':
    main()
