#!/usr/bin/env python3
"""Footer / prompt button icons drawn from shapes at high resolution
(A, B, X face buttons and the d-pad), matching the originals' layout:
a coloured gem with a pale letter, and a dark d-pad in a steel ring.

usage: buttons.py outdir"""
import sys, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

FONT = '/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf'
SS = 4

def radial(size, c_in, c_out, cx, cy, r):
    y, x = np.mgrid[0:size, 0:size].astype(np.float64)
    t = np.clip(np.hypot(x - cx, y - cy) / r, 0, 1)[..., None]
    return (np.array(c_in) * (1 - t) + np.array(c_out) * t)

def gem(letter, base, light, dark, letter_col, n=128, inset=0.06):
    N = n * SS
    r = N * (0.5 - inset)
    c = N / 2
    body = radial(N, light, dark, c - r * 0.35, c - r * 0.4, r * 1.6)
    img = np.zeros((N, N, 4))
    img[..., :3] = body
    m = Image.new('L', (N, N), 0)
    ImageDraw.Draw(m).ellipse((c - r, c - r, c + r, c + r), fill=255)
    rim = Image.new('L', (N, N), 0)
    ImageDraw.Draw(rim).ellipse((c - r, c - r, c + r, c + r), outline=255, width=int(N * 0.035))
    a = np.array(m) / 255.0
    rm = np.array(rim)[..., None] / 255.0
    img[..., :3] = img[..., :3] * (1 - rm * 0.55)           # darker rim
    img[..., 3] = a * 255
    out = Image.fromarray(img.astype(np.uint8), 'RGBA')
    # soft gloss on the upper half
    gl = Image.new('L', (N, N), 0)
    ImageDraw.Draw(gl).ellipse((c - r * 0.72, c - r * 0.88, c + r * 0.72, c + r * 0.05), fill=70)
    gl = gl.filter(ImageFilter.GaussianBlur(N * 0.03))
    white = Image.new('RGBA', (N, N), (255, 255, 255, 0)); white.putalpha(gl)
    out = Image.alpha_composite(out, Image.composite(white, Image.new('RGBA', (N, N), (0, 0, 0, 0)), m))
    # letter
    f = ImageFont.truetype(FONT, int(N * 0.62))
    d = ImageDraw.Draw(out)
    bb = d.textbbox((0, 0), letter, font=f)
    x = c - (bb[0] + bb[2]) / 2; y = c - (bb[1] + bb[3]) / 2
    d.text((x + N * 0.012, y + N * 0.02), letter, font=f, fill=dark + (200,))
    d.text((x, y), letter, font=f, fill=letter_col + (255,))
    return out.resize((n, n), Image.LANCZOS)

def dpad(hi_updown, n=256):
    N = n * SS
    c = N / 2
    out = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(out)
    r = N * 0.47
    d.ellipse((c - r, c - r, c + r, c + r), fill=(12, 14, 18, 255), outline=(120, 140, 160, 255), width=int(N * 0.03))
    w = N * 0.30; L = N * 0.40
    arm = (90, 104, 120, 255)
    # cross outline then fill
    for (x0, y0, x1, y1) in ((c - w / 2, c - L, c + w / 2, c + L), (c - L, c - w / 2, c + L, c + w / 2)):
        d.rounded_rectangle((x0, y0, x1, y1), radius=N * 0.03, fill=arm)
    for (x0, y0, x1, y1) in ((c - w / 2 + N * 0.02, c - L + N * 0.02, c + w / 2 - N * 0.02, c + L - N * 0.02),
                             (c - L + N * 0.02, c - w / 2 + N * 0.02, c + L - N * 0.02, c + w / 2 - N * 0.02)):
        d.rounded_rectangle((x0, y0, x1, y1), radius=N * 0.02, fill=(8, 9, 12, 255))
    if hi_updown:   # the up and down arms lit (Scroll)
        for (y0, y1) in ((c - L + N * 0.02, c - w / 2 - N * 0.01), (c + w / 2 + N * 0.01, c + L - N * 0.02)):
            d.rounded_rectangle((c - w / 2 + N * 0.02, y0, c + w / 2 - N * 0.02, y1), radius=N * 0.02, fill=(205, 210, 215, 255))
    return out.resize((n, n), Image.LANCZOS)

def main():
    od = sys.argv[1]; os.makedirs(od, exist_ok=True)
    gem('X', None, (90, 150, 255), (20, 50, 170), (215, 235, 255)).save(os.path.join(od, '1e8915c2572c0f4e_btn_x.png'))
    gem('A', None, (170, 230, 90), (60, 130, 20), (225, 250, 200)).save(os.path.join(od, '248359337bd916d5_btn_a.png'))
    gem('B', None, (255, 110, 100), (180, 15, 15), (255, 225, 220)).save(os.path.join(od, 'd56d95b022fd4e65_btn_b.png'))
    dpad(False).save(os.path.join(od, '2aea95cb700e0049_dpad.png'))
    dpad(True).save(os.path.join(od, '54b06f707c177670_dpad_scroll.png'))
    print('buttons ->', od)

if __name__ == '__main__':
    main()
