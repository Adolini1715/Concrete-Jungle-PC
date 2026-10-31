#!/usr/bin/env python3
"""Draws the PC button-prompt art (src/glyphs.c) and writes src/glyph_art.h.

Keyboard keycaps (light cap, dark legend, like a real key) and the modern
controller bumpers. Each image is H pixels tall; its width sets how wide the
glyph is in the game's text (m_Scale = w / h).

    python3 tools/make_glyphs.py [--png DIR]   (--png also writes each image, for a look)
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

H = 64          # output height
SS = 4          # supersampling
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
FONT_C = '/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf'

CAP_TOP = (236, 232, 226, 255)
CAP_SIDE = (150, 142, 136, 255)
CAP_EDGE = (30, 24, 22, 255)
INK = (26, 20, 19, 255)


def keycap(w_units, legend=None, draw_fn=None, small=False):
    """A key cap w_units * H wide. legend: text; draw_fn(draw, box): custom legend."""
    W = int(round(w_units * H))
    im = Image.new('RGBA', (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = 3 * SS                       # margin around the cap
    r = 10 * SS
    # outline, side (the cap's lower lip) and top
    d.rounded_rectangle([m, m, W * SS - m, H * SS - m], r, fill=CAP_EDGE)
    d.rounded_rectangle([m + 2 * SS, m + 2 * SS, W * SS - m - 2 * SS, H * SS - m - 2 * SS], r - 2 * SS, fill=CAP_SIDE)
    top = [m + 4 * SS, m + 3 * SS, W * SS - m - 4 * SS, H * SS - m - 9 * SS]
    d.rounded_rectangle(top, r - 4 * SS, fill=CAP_TOP)
    if legend:
        size = (24 if small or len(legend) > 1 else 34) * SS
        font = ImageFont.truetype(FONT_C if len(legend) > 3 else FONT, size)
        bb = d.textbbox((0, 0), legend, font=font)
        tw, th = bb[2] - bb[0], bb[3] - bb[1]
        cx, cy = (top[0] + top[2]) / 2, (top[1] + top[3]) / 2
        d.text((cx - tw / 2 - bb[0], cy - th / 2 - bb[1]), legend, font=font, fill=INK)
    if draw_fn:
        draw_fn(d, top)
    return im.resize((W, H), Image.LANCZOS)


def arrow(d, cx, cy, s, direction):
    """Filled triangle pointing up/down/left/right, size s (supersampled px)."""
    pts = {
        'up': [(cx, cy - s), (cx - s, cy + s * 0.7), (cx + s, cy + s * 0.7)],
        'down': [(cx, cy + s), (cx - s, cy - s * 0.7), (cx + s, cy - s * 0.7)],
        'left': [(cx - s, cy), (cx + s * 0.7, cy - s), (cx + s * 0.7, cy + s)],
        'right': [(cx + s, cy), (cx - s * 0.7, cy - s), (cx - s * 0.7, cy + s)],
    }[direction]
    d.polygon(pts, fill=INK)


def arrows_fn(dirs):
    def f(d, box):
        cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
        if len(dirs) == 1:
            arrow(d, cx, cy, 11 * SS, dirs[0])
            return
        if dirs == ['up', 'down']:
            arrow(d, cx, cy - 10 * SS, 7 * SS, 'up')
            arrow(d, cx, cy + 10 * SS, 7 * SS, 'down')
            return
        if dirs == ['left', 'right']:
            arrow(d, cx - 13 * SS, cy, 7 * SS, 'left')
            arrow(d, cx + 13 * SS, cy, 7 * SS, 'right')
            return
        s = 6 * SS                       # all four
        arrow(d, cx, cy - 11 * SS, s, 'up')
        arrow(d, cx, cy + 11 * SS, s, 'down')
        arrow(d, cx - 14 * SS, cy, s, 'left')
        arrow(d, cx + 14 * SS, cy, s, 'right')
    return f


def arrow_cluster():
    """The arrow keys as they sit on a keyboard: Up on top, Left Down Right below. Square, like the
    d-pad icon it stands in for: the menus size their prompt boxes with the pad icons."""
    k = 22                           # one small key, px
    W = H
    im = Image.new('RGBA', (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x0, y0 = (W - 3 * k) // 2 + 1, (H - 2 * k) // 2 + 2
    def key(x, y, direction):
        xa, ya, xb, yb = x * SS, y * SS, (x + k) * SS, (y + k) * SS
        r = 4 * SS
        d.rounded_rectangle([xa + SS, ya + SS, xb - SS, yb - SS], r, fill=CAP_EDGE)
        d.rounded_rectangle([xa + 2 * SS, ya + 2 * SS, xb - 2 * SS, yb - 2 * SS], r - SS, fill=CAP_SIDE)
        top = [xa + 3 * SS, ya + 2 * SS, xb - 3 * SS, yb - 4 * SS]
        d.rounded_rectangle(top, r - 2 * SS, fill=CAP_TOP)
        arrow(d, (top[0] + top[2]) / 2, (top[1] + top[3]) / 2, 3.6 * SS, direction)
    key(x0 + k, y0 - 1, 'up')
    key(x0, y0 + k - 2, 'left')
    key(x0 + k, y0 + k - 2, 'down')
    key(x0 + 2 * k, y0 + k - 2, 'right')
    return im.resize((W, H), Image.LANCZOS)


def bumper(label):
    """Modern controller shoulder button: dark rounded pill with a white legend."""
    W = int(1.5 * H)
    im = Image.new('RGBA', (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = 4 * SS
    d.rounded_rectangle([m, 10 * SS, W * SS - m, H * SS - 10 * SS], 16 * SS, fill=(235, 235, 235, 255))
    d.rounded_rectangle([m + 3 * SS, 13 * SS, W * SS - m - 3 * SS, H * SS - 13 * SS], 13 * SS, fill=(40, 40, 44, 255))
    font = ImageFont.truetype(FONT, 26 * SS)
    bb = d.textbbox((0, 0), label, font=font)
    d.text((W * SS / 2 - (bb[2] - bb[0]) / 2 - bb[0], H * SS / 2 - (bb[3] - bb[1]) / 2 - bb[1]), label, font=font,
           fill=(245, 245, 245, 255))
    return im.resize((W, H), Image.LANCZOS)


def face(letter, col):
    """Modern controller face button: coloured letter on a dark disc."""
    im = Image.new('RGBA', (H * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = 4 * SS
    d.ellipse([m, m, H * SS - m, H * SS - m], fill=(22, 22, 24, 255), outline=(200, 200, 200, 255), width=2 * SS)
    d.ellipse([m + 5 * SS, m + 5 * SS, H * SS - m - 5 * SS, H * SS - m - 5 * SS], fill=col)
    font = ImageFont.truetype(FONT, 30 * SS)
    bb = d.textbbox((0, 0), letter, font=font)
    d.text((H * SS / 2 - (bb[2] - bb[0]) / 2 - bb[0], H * SS / 2 - (bb[3] - bb[1]) / 2 - bb[1]), letter, font=font,
           fill=(255, 255, 255, 255), stroke_width=SS, stroke_fill=(0, 0, 0, 160))
    return im.resize((H, H), Image.LANCZOS)


def trigger(label):
    """Trigger: a rounded tab, flat at the bottom."""
    W = int(1.25 * H)
    im = Image.new('RGBA', (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = 4 * SS
    d.rounded_rectangle([m, m, W * SS - m, H * SS - m], 18 * SS, fill=(235, 235, 235, 255))
    d.rounded_rectangle([m + 3 * SS, m + 3 * SS, W * SS - m - 3 * SS, H * SS - m - 3 * SS], 15 * SS, fill=(40, 40, 44, 255))
    font = ImageFont.truetype(FONT, 26 * SS)
    bb = d.textbbox((0, 0), label, font=font)
    d.text((W * SS / 2 - (bb[2] - bb[0]) / 2 - bb[0], H * SS / 2 - (bb[3] - bb[1]) / 2 - bb[1]), label, font=font,
           fill=(245, 245, 245, 255))
    return im.resize((W, H), Image.LANCZOS)


def disc(draw_fn):
    im = Image.new('RGBA', (H * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = 4 * SS
    d.ellipse([m, m, H * SS - m, H * SS - m], fill=(235, 235, 235, 255))
    d.ellipse([m + 3 * SS, m + 3 * SS, H * SS - m - 3 * SS, H * SS - m - 3 * SS], fill=(40, 40, 44, 255))
    draw_fn(d, H * SS / 2, H * SS / 2)
    return im.resize((H, H), Image.LANCZOS)


def stick(label):
    def f(d, cx, cy):
        r = 20 * SS
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(150, 150, 155, 255), width=2 * SS)
        font = ImageFont.truetype(FONT, 17 * SS)
        bb = d.textbbox((0, 0), label, font=font)
        d.text((cx - (bb[2] - bb[0]) / 2 - bb[0], cy - (bb[3] - bb[1]) / 2 - bb[1]), label, font=font, fill=(245, 245, 245, 255))
    return disc(f)


def menu_btn(d, cx, cy):
    for k in (-8, 0, 8):
        d.rounded_rectangle([cx - 12 * SS, cy + k * SS - 2 * SS, cx + 12 * SS, cy + k * SS + 2 * SS], SS, fill=(245, 245, 245, 255))


def view_btn(d, cx, cy):
    d.rounded_rectangle([cx - 12 * SS, cy - 10 * SS, cx + 4 * SS, cy + 4 * SS], 2 * SS, outline=(245, 245, 245, 255), width=2 * SS)
    d.rounded_rectangle([cx - 4 * SS, cy - 4 * SS, cx + 12 * SS, cy + 10 * SS], 2 * SS, fill=(245, 245, 245, 255))


def dpad(lit):
    """D-pad cross; the arms in `lit` are white, the others grey."""
    def f(d, cx, cy):
        a, w = 17 * SS, 6 * SS
        arms = {'up': [cx - w, cy - a, cx + w, cy - w], 'down': [cx - w, cy + w, cx + w, cy + a],
                'left': [cx - a, cy - w, cx - w, cy + w], 'right': [cx + w, cy - w, cx + a, cy + w]}
        d.rectangle([cx - w, cy - w, cx + w, cy + w], fill=(110, 110, 115, 255))
        for k, box in arms.items():
            d.rounded_rectangle(box, 2 * SS, fill=(250, 250, 250, 255) if k in lit else (110, 110, 115, 255))
    return disc(f)


def mouse():
    """A mouse, for 'move the camera' prompts."""
    W = H
    im = Image.new('RGBA', (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x0, x1, y0, y1 = 16 * SS, W * SS - 16 * SS, 5 * SS, H * SS - 5 * SS
    d.rounded_rectangle([x0, y0, x1, y1], 16 * SS, fill=CAP_EDGE)
    d.rounded_rectangle([x0 + 3 * SS, y0 + 3 * SS, x1 - 3 * SS, y1 - 3 * SS], 13 * SS, fill=CAP_TOP)
    cx = W * SS / 2
    d.line([cx, y0 + 3 * SS, cx, y0 + 24 * SS], fill=CAP_EDGE, width=2 * SS)
    d.line([x0 + 3 * SS, y0 + 24 * SS, x1 - 3 * SS, y0 + 24 * SS], fill=CAP_EDGE, width=2 * SS)
    return im.resize((W, H), Image.LANCZOS)


ART = [
    ('kb_space', lambda: keycap(2.3, 'Space')),
    ('kb_enter', lambda: keycap(2.1, 'Enter')),
    ('kb_esc', lambda: keycap(1.5, 'Esc')),
    ('kb_shift', lambda: keycap(2.1, 'Shift')),
    ('kb_q', lambda: keycap(1.0, 'Q')),
    ('kb_e', lambda: keycap(1.0, 'E')),
    ('kb_r', lambda: keycap(1.0, 'R')),
    ('kb_f', lambda: keycap(1.0, 'F')),
    ('kb_c', lambda: keycap(1.0, 'C')),
    ('kb_z', lambda: keycap(1.0, 'Z')),
    ('kb_1', lambda: keycap(1.0, '1')),
    ('kb_2', lambda: keycap(1.0, '2')),
    ('kb_up', lambda: keycap(1.0, draw_fn=arrows_fn(['up']))),
    ('kb_down', lambda: keycap(1.0, draw_fn=arrows_fn(['down']))),
    ('kb_left', lambda: keycap(1.0, draw_fn=arrows_fn(['left']))),
    ('kb_right', lambda: keycap(1.0, draw_fn=arrows_fn(['right']))),
    ('kb_updown', arrow_cluster),
    ('kb_leftright', lambda: keycap(1.2, draw_fn=arrows_fn(['left', 'right']))),
    ('kb_arrows', arrow_cluster),
    ('kb_wasd', lambda: keycap(1.9, 'WASD')),
    ('kb_mouse', mouse),
    ('pad_lb', lambda: bumper('LB')),
    ('pad_rb', lambda: bumper('RB')),
    ('pad_lt', lambda: trigger('LT')),
    ('pad_rt', lambda: trigger('RT')),
    ('pad_a', lambda: face('A', (16, 124, 16, 255))),
    ('pad_b', lambda: face('B', (200, 32, 32, 255))),
    ('pad_x', lambda: face('X', (24, 96, 200, 255))),
    ('pad_y', lambda: face('Y', (214, 160, 0, 255))),
    ('pad_ls', lambda: stick('LS')),
    ('pad_rs', lambda: stick('RS')),
    ('pad_menu', lambda: disc(menu_btn)),
    ('pad_view', lambda: disc(view_btn)),
    ('pad_dpad', lambda: dpad(['up', 'down', 'left', 'right'])),
    ('pad_dpad_ud', lambda: dpad(['up', 'down'])),
    ('pad_dpad_lr', lambda: dpad(['left', 'right'])),
    ('pad_up', lambda: dpad(['up'])),
    ('pad_down', lambda: dpad(['down'])),
    ('pad_left', lambda: dpad(['left'])),
    ('pad_right', lambda: dpad(['right'])),
]


# Rebindable keys (src/keymap.c): SDL scancode, art name, cap legend, cap width. The keys the
# prompts already had keep their art above; these fill in the rest of a keyboard.
KEYS = []
for i, ch in enumerate('ABCDEFGHIJKLMNOPQRSTUVWXYZ'):
    KEYS.append((4 + i, 'kb_' + ch.lower(), ch, 1.0))
for i, ch in enumerate('1234567890'):
    KEYS.append((30 + i, 'kb_' + ch, ch, 1.0))
for i in range(12):
    KEYS.append((58 + i, 'kb_f%d' % (i + 1), 'F%d' % (i + 1), 1.3))
KEYS += [
    (40, 'kb_enter', 'Enter', 2.1), (41, 'kb_esc', 'Esc', 1.5), (42, 'kb_backspace', 'Bksp', 1.9),
    (43, 'kb_tab', 'Tab', 1.6), (44, 'kb_space', 'Space', 2.3), (45, 'kb_minus', '-', 1.0),
    (46, 'kb_equals', '=', 1.0), (47, 'kb_lbracket', '[', 1.0), (48, 'kb_rbracket', ']', 1.0),
    (49, 'kb_backslash', '\\', 1.0), (51, 'kb_semicolon', ';', 1.0), (52, 'kb_quote', "'", 1.0),
    (53, 'kb_grave', '`', 1.0), (54, 'kb_comma', ',', 1.0), (55, 'kb_period', '.', 1.0),
    (56, 'kb_slash', '/', 1.0), (57, 'kb_caps', 'Caps', 1.8), (73, 'kb_insert', 'Ins', 1.5),
    (74, 'kb_home', 'Home', 1.8), (75, 'kb_pgup', 'PgUp', 1.9), (76, 'kb_delete', 'Del', 1.5),
    (77, 'kb_end', 'End', 1.5), (78, 'kb_pgdn', 'PgDn', 1.9), (79, 'kb_right', None, 1.0),
    (80, 'kb_left', None, 1.0), (81, 'kb_down', None, 1.0), (82, 'kb_up', None, 1.0),
    (84, 'kb_kp_div', 'Num /', 2.0), (85, 'kb_kp_mul', 'Num *', 2.0), (86, 'kb_kp_minus', 'Num -', 2.0),
    (87, 'kb_kp_plus', 'Num +', 2.0), (88, 'kb_kp_enter', 'Num Ent', 2.5),
]
for i, ch in enumerate('123456789'):
    KEYS.append((89 + i, 'kb_kp_' + ch, 'Num ' + ch, 2.0))
KEYS += [(98, 'kb_kp_0', 'Num 0', 2.0), (99, 'kb_kp_period', 'Num .', 2.0),
         (224, 'kb_lctrl', 'Ctrl', 1.7), (225, 'kb_shift', 'Shift', 2.1), (226, 'kb_lalt', 'Alt', 1.5),
         (228, 'kb_rctrl', 'R Ctrl', 2.2), (229, 'kb_rshift', 'R Shift', 2.5), (230, 'kb_ralt', 'R Alt', 2.0)]
# Mouse buttons (codes 1001.. in src/keymap.c), drawn from tools/art/mouse/*.png
MOUSE = [(1001, 'kb_mouse_left', 'mouse_left'), (1002, 'kb_mouse_middle', 'mouse_middle'),
         (1003, 'kb_mouse_right', 'mouse_right'), (1004, 'kb_wheel_up', 'mouse_wheel_up'),
         (1005, 'kb_wheel_down', 'mouse_wheel_down')]


def mouse_icon(png):
    im = Image.open(os.path.join(ROOT, 'tools', 'art', 'mouse', png + '.png')).convert('RGBA')
    return im.resize((H, H), Image.LANCZOS)


def add_keys():
    have = {n for n, _ in ART}
    for code, name, legend, w in KEYS:
        if name not in have:
            ART.append((name, (lambda l=legend, w=w: keycap(w, l))))
            have.add(name)
    for code, name, png in MOUSE:
        ART.append((name, (lambda p=png: mouse_icon(p))))


def main():
    add_keys()
    png_dir = sys.argv[sys.argv.index('--png') + 1] if '--png' in sys.argv else None
    out = ['/* Generated by tools/make_glyphs.py: PC button-prompt art (RGBA, top row first). Do not edit. */',
           'typedef struct { const char *name; int w, h; const unsigned char *rgba; } GlyphArt;']
    names = []
    for name, fn in ART:
        im = fn().convert('RGBA')
        if png_dir:
            os.makedirs(png_dir, exist_ok=True)
            im.save(os.path.join(png_dir, name + '.png'))
        data = im.tobytes()
        rows = [', '.join(str(b) for b in data[i:i + 32]) for i in range(0, len(data), 32)]
        out.append('static const unsigned char glyph_px_%s[%d] = {\n%s};' % (name, len(data), ',\n'.join(rows)))
        names.append((name, im.width, im.height))
    out.append('static const GlyphArt glyph_art[] = {')
    out += ['    {"%s", %d, %d, glyph_px_%s},' % (n, w, h, n) for n, w, h in names]
    out.append('};')
    out.append('#define GLYPH_ART_N %d' % len(names))
    out.append('/* Rebindable keys: SDL scancode (mouse buttons 1001..1005) -> art name (src/keymap.c). */')
    out.append('typedef struct { int code; const char *art, *label; } GlyphKey;')
    out.append('static const GlyphKey glyph_keys[] = {')
    for code, name, legend, w in KEYS:
        lab = legend if legend else {'kb_up': 'Up', 'kb_down': 'Down', 'kb_left': 'Left', 'kb_right': 'Right'}[name]
        out.append('    {%d, "%s", "%s"},' % (code, name, lab.replace('\\', '\\\\')))
    for code, name, png in MOUSE:
        out.append('    {%d, "%s", "%s"},' % (code, name, {'kb_mouse_left': 'Mouse L', 'kb_mouse_middle': 'Mouse M',
                    'kb_mouse_right': 'Mouse R', 'kb_wheel_up': 'Wheel Up', 'kb_wheel_down': 'Wheel Down'}[name]))
    out.append('};')
    out.append('#define GLYPH_KEYS_N %d' % (len(KEYS) + len(MOUSE)))
    with open(os.path.join(ROOT, 'src', 'glyph_art.h'), 'w') as f:
        f.write('\n'.join(out) + '\n')
    print('%d images -> src/glyph_art.h' % len(names))


if __name__ == '__main__':
    main()
