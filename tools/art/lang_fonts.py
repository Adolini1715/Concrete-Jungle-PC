#!/usr/bin/env python3
"""Accented letters for the game's fonts (translations).

The game's four bitmap fonts (menu bold, title italic, small, in-game HUD) hold ASCII only. For each
one this writes an atlas twice as tall: the top half is the Sharp UI 4x atlas (packs/sharp-ui, the
same glyphs where they always were), the bottom half holds accented letters drawn from the same
Liberation font, each one placed exactly as its base letter is (the base letter's ink box fixes the
scale and position, the accent goes where the vector font puts it). src/langfont.c points the
font's glyph table at the new cells and halves every old glyph's v, so the game lays the new letters
out like any other.

  lang_fonts.py <BuildData dir> <sharp-ui replace dir> <out dir>

Writes <out dir>/<atlas key>.png (4x, 2x the original height) and <out dir>/fonts.txt:
  font <atlas key> <original 'a' u0> <atlas w> <atlas h> <'a' row>   (original size, pixels)
  <char code> <base char code> <u0> <v0> <u1> <v1> <width> <height>
UVs are for the tall atlas, in the corrected layout src/recomp_manual.c gives the old glyphs
(pixel / size, one blank column of padding on the left, width + 1).
"""
import re, struct, sys, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'edb'))
LIB = '/usr/share/fonts/truetype/liberation/'
SCALE = 4
# (file, font hash, atlas key, ttf, stroke): as tools/art/build_sharp_ui.sh draws them
FONTS = [('fonts.edb', 0x07000004, '8662cbf2181cf340', 'LiberationSans-Bold.ttf', 0.0),
         ('fonts.edb', 0x0700000C, '531ef27bd120d318', 'LiberationSans-BoldItalic.ttf', 0.025),
         ('fonts.edb', 0x0700000A, '9594b0b78805ac05', 'LiberationSans-Bold.ttf', 0.012),
         ('loading.edb', 0x07000004, '93f9cab9e029acd6', 'LiberationSans-Bold.ttf', 0.0)]
# accented letter -> the letter whose cell, scale and ink box it follows
CHARS = {}
for base, acc in (('a', 'àáâãäª'), ('e', 'èéêë'), ('i', 'ìíîï'), ('o', 'òóôõöº'), ('u', 'ùúûü'),
                  ('c', 'ç'), ('n', 'ñ'),
                  ('A', 'ÀÁÂÃÄ'), ('E', 'ÈÉÊË'), ('I', 'ÌÍÎÏ'), ('O', 'ÒÓÔÕÖ'), ('U', 'ÙÚÛÜ'),
                  ('C', 'Ç'), ('N', 'Ñ')):
    for a in acc:
        CHARS[a] = base


def edb_index(bd):
    idx = {}
    for l in open(os.path.join(bd, 'Filelist.txt'), errors='replace'):
        m = re.search(r'\\([^\\]+?\.edb)\s*: Len\s+(\d+).*Loc\s+([0-9a-f]+):(\d+)', l)
        if m:
            idx.setdefault(m.group(1).strip().lower(), (int(m.group(2)), int(m.group(3), 16), int(m.group(4))))
    return idx


def read_edb(bd, idx, name):
    ln, off, p = idx[name]
    with open(os.path.join(bd, 'Filelist.%03d' % p), 'rb') as f:
        f.seek(off)
        return f.read(ln)


def font_glyphs(d, fhash):
    """char -> (x, y, w, h) in atlas pixels (u * (size - 1), the game's layout), and the texture index"""
    i32 = lambda o: struct.unpack_from('<i', d, o)[0]
    u32 = lambda o: struct.unpack_from('<I', d, o)[0]
    cnt = struct.unpack_from('<H', d, 0xa0)[0]
    fb = 0xa4 + i32(0xa4)
    for k in range(cnt):
        e = fb + k * 0x10
        if u32(e) != fhash:
            continue
        o = u32(e + 8)
        page = o + 4 + i32(o + 4)
        g = {}
        for c in range(256):
            r = i32(page + c * 4)
            if r:
                q = page + c * 4 + r
                g[c] = struct.unpack_from('<4f', d, q) + struct.unpack_from('<2H', d, q + 16)
        return g, u32(o)
    raise SystemExit('font %08x not found' % fhash)


def atlas(d, ti):
    import edb_tex as E
    n = E.u16(d, 0xb8)
    base = 0xbc + E.u32(d, 0xbc)
    o = E.u32(d, base + ti * 0x1c + 8)
    w, h, fmt = E.u16(d, o), E.u16(d, o + 2), d[o + 0x13]
    r = E.u32(d, o + 0x2c)
    q = (o + 0x2c + r) & 0xffffffff
    raw = d[q:q + E.level0_size(w, h, fmt)]
    return '%016x' % E.texpack_key(raw, E.NV[fmt], w, h), E.decode(raw, w, h, fmt)


def ink_box(a, thr=128):
    ys, xs = np.nonzero(a >= thr)
    if len(xs) == 0:
        ys, xs = np.nonzero(a > 8)
    return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1


def render(font, c, big, stroke):
    g = Image.new('L', (big * 3, big * 3), 0)
    sw = int(round(stroke * big))
    ImageDraw.Draw(g).text((big, big), c, font=font, fill=255, stroke_width=sw, stroke_fill=255)
    return np.array(g)


def main():
    bd, sharp, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    idx = edb_index(bd)
    lines = []
    for fname, fhash, key, ttf, stroke in FONTS:
        d = read_edb(bd, idx, fname)
        glyphs, ti = font_glyphs(d, fhash)
        k2, orig = atlas(d, ti)
        assert k2 == key, (fname, k2, key)
        H, W = orig.shape[:2]
        alpha = orig[:, :, 3].astype(np.int32)
        hi = np.array(Image.open(os.path.join(sharp, [f for f in os.listdir(sharp) if f.startswith(key)][0])).convert('RGBA'))
        assert hi.shape[1] == W * SCALE and hi.shape[0] == H * SCALE
        tall = np.zeros((H * 2 * SCALE, W * SCALE), np.uint8)
        tall[:H * SCALE] = hi[:, :, 3]
        big = 512
        font = ImageFont.truetype(LIB + ttf, big)
        a_u0 = glyphs[ord('a')][0]
        lines.append('font %s %.6f %d %d %d' % (key, a_u0, W, H, int(round(glyphs[ord('a')][1] * (H - 1)))))
        cx, cy, rowh = 2, H + 1, 0
        for ch, b in CHARS.items():
            u0, v0, u1, v1, w, h = glyphs[ord(b)]
            x0, y0 = int(round(u0 * (W - 1))), int(round(v0 * (H - 1)))
            # the base letter: its ink box in the atlas, and the vector glyph's
            bx0, by0, bx1, by1 = ink_box(alpha[y0:y0 + h, x0:x0 + w])
            vb = render(font, b, big, stroke)
            vx0, vy0, vx1, vy1 = ink_box(vb)
            sx = (bx1 - bx0) * SCALE / (vx1 - vx0)
            sy = (by1 - by0) * SCALE / (vy1 - vy0)
            va = render(font, ch, big, stroke)
            # map the accented glyph with the base letter's transform into a cell of the base's size
            img = Image.fromarray(va)
            # cell-pixel = (vector-pixel - v0) * s + ink offset; take the vector region the cell covers
            L = vx0 - bx0 * SCALE / sx
            T = vy0 - by0 * SCALE / sy
            R = L + w * SCALE / sx
            B = T + h * SCALE / sy
            # an accent that would run off the top of the cell (capitals in the small font): squash
            # the letter onto its baseline until the accent's top sits half a pixel into the cell
            ay0 = ink_box(va, 40)[1]
            cellH = h * SCALE
            if (ay0 - T) * cellH / (B - T) < SCALE * 0.5:
                vby1 = ink_box(vb)[3]
                k = (by1 * SCALE - SCALE * 0.5) / (vby1 - ay0)
                T = vby1 - by1 * SCALE / k
                B = T + cellH / k
            # an accent wider than a narrow letter (i): widen the cell to fit it, the letter centred
            ax0, _, ax1, _ = ink_box(va, 40)
            kx = sx   # cell pixels per vector pixel
            lpx, rpx = (ax0 - L) * kx, (ax1 - L) * kx
            if lpx < SCALE * 0.5 or rpx > w * SCALE - SCALE * 0.5:
                need = int(np.ceil((rpx - lpx + SCALE) / SCALE))
                w = max(w, need)
                L = L + (lpx - (w * SCALE - (rpx - lpx)) / 2) / kx
            R = L + w * SCALE / kx
            cell = img.transform((w * SCALE * 4, cellH * 4), Image.EXTENT, (L, T, R, B), Image.BILINEAR)
            cell = np.array(cell.resize((w * SCALE, cellH), Image.BOX)).astype(np.int32)
            if cx + w + 2 > W:
                cx, cy = 2, cy + rowh + 1
            tall[cy * SCALE:(cy + h) * SCALE, cx * SCALE:(cx + w) * SCALE] = np.clip(cell, 0, 255)
            # the corrected layout: pixel / size, one blank column in front, width + 1
            lines.append('%d %d %.6f %.6f %.6f %.6f %d %d' % (
                ord(ch.encode('latin-1')), ord(b), (cx - 1) / W, cy / (2 * H), (cx + w) / W, (cy + h) / (2 * H), w + 1, h))
            cx += w + 2
            rowh = max(rowh, h)
        assert cy + rowh <= 2 * H, 'out of room'
        rgba = np.dstack([np.full(tall.shape, 255, np.uint8)] * 3 + [tall])
        Image.fromarray(rgba, 'RGBA').save(os.path.join(out, key + '.png'), optimize=True)
        print(key, tall.shape, file=sys.stderr)
    open(os.path.join(out, 'fonts.txt'), 'w').write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
