"""Draw the modern controller diagram for Pause > Controller (and the texture it replaces).

The pause page (mods/FEPause.cpp) shows this picture across the page body (0x1d0 x 0xda FE
units) with a 9-row, 3-column label grid over it: column 0 (0 .. 28%) holds right-aligned
labels, column 2 (72% .. 100%) left-aligned ones, row i centred at (i + 0.5) * H / 9. Every
leader line here ends at its label's row and column edge, so labels never crowd.
The button order per side follows mods/FEPause.cpp (k_ctrl in src/native/hybrid/cjmod.c).
Drawn from primitives (no traced artwork).

    python3 tools/art/controller_pause.py out.png [controller.png]

controller.png (optional): a ready-made controller picture with a transparent or black
background, placed in the middle instead of the drawn one (the button positions below must
then match it: tune PAD below).
"""
import sys
from PIL import Image, ImageDraw, ImageFilter

UW, UH = 464, 218          # FE units of the page body
S = 4                      # pixels per unit
W, H = UW * S, UH * S
COL0, COL2 = 0.34 * UW, 0.66 * UW
ROWS = 9


def row_y(i):
    return (i + 0.5) * UH / ROWS


# Controller geometry (units), centred on x = 232.
CX, CY = 232, 112
PAD = {
    'lt': (196, 44), 'rt': (268, 44), 'lb': (194, 56), 'rb': (270, 56),
    'ls': (199, 90), 'dpad': (214, 126), 'rs': (250, 126),
    'view': (221, 92), 'menu': (243, 92), 'guide': (232, 74),
    'y': (271, 80), 'x': (260, 91), 'b': (282, 91), 'a': (271, 102),
}
# label rows, top to bottom: (side, row, button, where on the button the line ends)
LEFT = ['lt', 'lb', 'view', 'ls', 'ls_click', 'dpad_up', 'dpad_left', 'dpad_down', 'dpad_right']
RIGHT = ['rt', 'rb', 'menu', 'y', 'x', 'b', 'a', 'rs', 'rs_click']


def target(name):
    if name.startswith('dpad_'):
        x, y = PAD['dpad']
        r = globals().get('DPAD_R', 7)
        dx, dy = {'up': (0, -r), 'down': (0, r), 'left': (-r, 0), 'right': (r, 0)}[name[5:]]
        return x + dx, y + dy
    if name.endswith('_click'):
        x, y = PAD[name[:2]]
        return x, y + 8
    return PAD[name]


def P(*xy):
    return [v * S for v in xy]


img = Image.new('RGBA', (W, H), (0, 0, 0, 0))


def body_shape(d, fill, g=0):
    # grips
    d.ellipse(P(170 - g, 78 - g, 222 + g, 188 + g), fill=fill)
    d.ellipse(P(242 - g, 78 - g, 294 + g, 188 + g), fill=fill)
    # main body
    d.rounded_rectangle(P(170 - g, 60 - g, 294 + g, 140 + g), radius=26 * S, fill=fill)
    d.ellipse(P(196 - g, 118 - g, 268 + g, 150 + g), fill=fill)


if len(sys.argv) > 2:
    # A controller picture on black: the black becomes transparent, the pad is scaled to BODY_W
    # units wide and centred; PIX gives its buttons in picture pixels.
    import numpy as np
    pic = Image.open(sys.argv[2]).convert('RGB')
    a = np.asarray(pic).astype(np.float32)
    lum = a.max(-1)
    ys, xs = np.where(lum > 12)
    x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()
    from scipy import ndimage
    solid = ndimage.binary_fill_holes(ndimage.binary_closing(lum > 12, iterations=3))   # the pad's own blacks stay opaque
    edge = np.clip((lum - 3.0) / 10.0, 0, 1)
    alpha = np.where(ndimage.binary_erosion(solid, iterations=2), 1.0, np.where(solid, np.maximum(edge, 0.6), edge)) * 255
    rgba = np.dstack([a, alpha]).astype(np.uint8)
    pic = Image.fromarray(rgba, 'RGBA').crop((x0, y0, x1 + 1, y1 + 1))
    BODY_W = 140
    k = BODY_W / pic.width                      # units per picture pixel
    pcx, pcy = (x0 + x1) / 2, (y0 + y1) / 2
    PIX = {'lt': (480, 112), 'rt': (1190, 112), 'lb': (560, 150), 'rb': (1110, 150),
           'ls': (517, 341), 'dpad': (665, 528), 'rs': (995, 505), 'view': (745, 353), 'menu': (925, 353),
           'guide': (832, 243), 'y': (1153, 268), 'x': (1070, 347), 'b': (1233, 347), 'a': (1153, 425)}
    for n_, (px, py) in PIX.items():
        PAD[n_] = (CX + (px - pcx) * k, CY + (py - pcy) * k)
    glow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    big = pic.resize((int(pic.width * k * S), int(pic.height * k * S)), Image.LANCZOS)
    pos = (int(CX * S - big.width / 2), int(CY * S - big.height / 2))
    sil = Image.new('RGBA', big.size, (190, 20, 20, 0))
    sil.putalpha(big.split()[3])
    glow.alpha_composite(sil, pos)
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(6 * S)))
    img.alpha_composite(big, pos)
    DPAD_R = 9
else:
    glow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    body_shape(ImageDraw.Draw(glow), (190, 20, 20, 255), 3)
    glow = glow.filter(ImageFilter.GaussianBlur(6 * S))
    img.alpha_composite(glow)
    d = ImageDraw.Draw(img)
    # triggers and bumpers behind the body
    for x0 in (184, 256):
        d.rounded_rectangle(P(x0, 38, x0 + 24, 56), radius=6 * S, fill=(58, 58, 62, 255))
    for x0 in (178, 254):
        d.rounded_rectangle(P(x0, 52, x0 + 32, 62), radius=5 * S, fill=(84, 84, 90, 255))
    body_shape(d, (24, 24, 27, 255))
    sheen = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(sheen).rounded_rectangle(P(182, 64, 282, 96), radius=18 * S, fill=(255, 255, 255, 26))
    img.alpha_composite(sheen.filter(ImageFilter.GaussianBlur(5 * S)))
    d = ImageDraw.Draw(img)

    def stick(cx, cy):
        d.ellipse(P(cx - 10, cy - 10, cx + 10, cy + 10), fill=(8, 8, 8, 255))
        d.ellipse(P(cx - 8, cy - 8, cx + 8, cy + 8), fill=(66, 66, 70, 255))
        d.ellipse(P(cx - 5.5, cy - 5.5, cx + 5.5, cy + 5.5), fill=(44, 44, 48, 255))

    stick(*PAD['ls'])
    stick(*PAD['rs'])
    x, y = PAD['dpad']
    d.ellipse(P(x - 10, y - 10, x + 10, y + 10), fill=(10, 10, 10, 255))
    d.rounded_rectangle(P(x - 2.6, y - 8, x + 2.6, y + 8), radius=1.2 * S, fill=(70, 70, 74, 255))
    d.rounded_rectangle(P(x - 8, y - 2.6, x + 8, y + 2.6), radius=1.2 * S, fill=(70, 70, 74, 255))
    for k, col in (('y', (230, 196, 30)), ('x', (40, 110, 230)), ('b', (220, 40, 40)), ('a', (60, 180, 60))):
        x, y = PAD[k]
        d.ellipse(P(x - 5, y - 5, x + 5, y + 5), fill=(12, 12, 12, 255))
        d.ellipse(P(x - 4, y - 4, x + 4, y + 4), fill=col + (255,))
    for k in ('view', 'menu'):
        x, y = PAD[k]
        d.ellipse(P(x - 3, y - 3, x + 3, y + 3), fill=(70, 70, 74, 255))
    x, y = PAD['guide']
    d.ellipse(P(x - 5.5, y - 5.5, x + 5.5, y + 5.5), fill=(180, 180, 186, 255))
    d.ellipse(P(x - 4, y - 4, x + 4, y + 4), fill=(30, 30, 32, 255))

# leader lines: from the column edge along the row, then straight to the button
lines = Image.new('RGBA', (W, H), (0, 0, 0, 0))
ld = ImageDraw.Draw(lines)


def leader(side, i, name):
    tx, ty = target(name)
    y = row_y(i)
    if side == 0:
        x0, xe = COL0 + 3, min(tx - 6, COL0 + 8 + i * 1.5)
    else:
        x0, xe = COL2 - 3, max(tx + 6, COL2 - 8 - i * 1.5)
    if name in ('lt', 'lb', 'rt', 'rb'):   # shoulders: run along the row to above the button, then down
        xe = tx - 3 if side == 0 else tx + 3
    r = 6.5 if name in ('y', 'x', 'b', 'a') else 0   # stop at a face button's rim (its letter stays clear)
    if r:
        import math
        dx, dy = xe - tx, y - ty
        L = math.hypot(dx, dy) or 1
        tx, ty = tx + dx / L * r, ty + dy / L * r
    pts = [(x0, y), (xe, y), (tx, ty)]
    ld.line([(px * S, py * S) for px, py in pts], fill=(255, 60, 60, 255), width=int(1.2 * S), joint='curve')
    ld.ellipse(P(tx - 1.6, ty - 1.6, tx + 1.6, ty + 1.6), fill=(255, 90, 90, 255))


for i, n in enumerate(LEFT):
    leader(0, i, n)
for i, n in enumerate(RIGHT):
    leader(1, i, n)
glow = lines.filter(ImageFilter.GaussianBlur(2.5 * S))
img.alpha_composite(glow)
img.alpha_composite(lines)
img.save(sys.argv[1])
print(sys.argv[1], img.size)
