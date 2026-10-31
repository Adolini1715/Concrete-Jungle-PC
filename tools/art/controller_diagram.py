"""Draw a modern (dual-offset-stick) controller diagram for Options > Controller.

Output: a 512x512 RGBA PNG that replaces the game's Xbox-controller diagram
texture (texture pack key ba1de2225579e28c). The leader lines end where the
game draws its labels (measured from the original texture); they start at the
buttons of this layout. The art is drawn from primitives, no traced artwork.
Run: python3 controller_diagram.py out.png
"""
import sys
from PIL import Image, ImageDraw, ImageFilter

S = 4                      # supersampling
W = 512 * S

def P(*xy):
    return [v * S for v in xy]

body = Image.new('RGBA', (W, W), (0, 0, 0, 0))
d = ImageDraw.Draw(body)

def shape(draw, fill, grow=0):
    g = grow
    # grips and body: two ellipses for the grips, a rounded slab across the top
    draw.ellipse(P(152 - g, 190 - g, 238 + g, 350 + g), fill=fill)
    draw.ellipse(P(287 - g, 190 - g, 373 + g, 350 + g), fill=fill)
    draw.rounded_rectangle(P(155 - g, 148 - g, 370 + g, 292 + g), radius=40 * S, fill=fill)
    draw.ellipse(P(200 - g, 250 - g, 325 + g, 305 + g), fill=fill)
    # bumpers and triggers
    draw.rounded_rectangle(P(170 - g, 138 - g, 232 + g, 156 + g), radius=9 * S, fill=fill)
    draw.rounded_rectangle(P(293 - g, 138 - g, 355 + g, 156 + g), radius=9 * S, fill=fill)

# red glow behind the body (the game's style)
glow = Image.new('RGBA', (W, W), (0, 0, 0, 0))
shape(ImageDraw.Draw(glow), (200, 20, 20, 255), grow=4)
glow = glow.filter(ImageFilter.GaussianBlur(7 * S))
body.alpha_composite(glow)

# triggers behind
d.rounded_rectangle(P(180, 120, 222, 142), radius=8 * S, fill=(70, 70, 74, 255))
d.rounded_rectangle(P(303, 120, 345, 142), radius=8 * S, fill=(70, 70, 74, 255))
shape(d, (26, 26, 28, 255))
# bumpers lighter
d.rounded_rectangle(P(172, 140, 230, 154), radius=7 * S, fill=(88, 88, 92, 255))
d.rounded_rectangle(P(295, 140, 353, 154), radius=7 * S, fill=(88, 88, 92, 255))
# top sheen
sheen = Image.new('RGBA', (W, W), (0, 0, 0, 0))
ImageDraw.Draw(sheen).rounded_rectangle(P(165, 152, 360, 205), radius=30 * S, fill=(255, 255, 255, 34))
sheen = sheen.filter(ImageFilter.GaussianBlur(6 * S))
body.alpha_composite(sheen)

def stick(cx, cy):
    d.ellipse(P(cx - 21, cy - 21, cx + 21, cy + 21), fill=(12, 12, 12, 255))
    d.ellipse(P(cx - 16, cy - 16, cx + 16, cy + 16), fill=(70, 70, 74, 255))
    d.ellipse(P(cx - 11, cy - 11, cx + 11, cy + 11), fill=(48, 48, 52, 255))

stick(205, 195)        # left stick
stick(300, 258)        # right stick
# d-pad
cx, cy = 232, 258
d.ellipse(P(cx - 21, cy - 21, cx + 21, cy + 21), fill=(12, 12, 12, 255))
d.rectangle(P(cx - 6, cy - 17, cx + 6, cy + 17), fill=(80, 80, 84, 255))
d.rectangle(P(cx - 17, cy - 6, cx + 17, cy + 6), fill=(80, 80, 84, 255))
# view / menu / home
d.ellipse(P(238, 189, 250, 201), fill=(70, 70, 74, 255))
d.ellipse(P(276, 189, 288, 201), fill=(70, 70, 74, 255))
d.ellipse(P(254, 162, 272, 180), fill=(60, 60, 64, 255))
d.ellipse(P(258, 166, 268, 176), fill=(30, 30, 32, 255))
# face buttons
for (bx, by), col, ch in (((330, 174), (230, 200, 40), 'Y'), ((310, 195), (40, 90, 220), 'X'),
                           ((350, 195), (210, 40, 40), 'B'), ((330, 216), (60, 170, 60), 'A')):
    d.ellipse(P(bx - 10, by - 10, bx + 10, by + 10), fill=(10, 10, 10, 255))
    d.ellipse(P(bx - 8, by - 8, bx + 8, by + 8), fill=col + (255,))
    d.ellipse(P(bx - 6, by - 7, bx + 4, by - 2), fill=(255, 255, 255, 60))

# leader lines: game label endpoints -> buttons of this layout
LINES = [
    [(112, 96), (200, 96), (200, 121)],                  # LT
    [(176, 141), (192, 141), (203, 175)],                # L3: camera distance
    [(112, 208), (182, 208), (188, 203)],                # left stick
    [(112, 235), (190, 235), (241, 199)],                # Back
    [(112, 256), (200, 256), (279, 199)],                # Start
    [(112, 299), (196, 299), (213, 262)],                # D-pad left
    [(112, 341), (215, 341), (232, 279)],                # D-pad down
    [(112, 384), (228, 384), (251, 262)],                # D-pad right
    [(248, 105), (248, 165), (233, 238)],                # D-pad up
    [(415, 96), (325, 96), (325, 121)],                  # RT
    [(415, 128), (348, 128), (313, 186)],                # X
    [(415, 155), (356, 155), (337, 167)],                # Y
    [(415, 208), (370, 208), (359, 199)],                # B
    [(415, 229), (366, 229), (338, 220)],                # A
    [(415, 267), (374, 267), (343, 153)],                # Black -> RB
    [(415, 299), (395, 299), (268, 226), (212, 152)],    # White -> LB
    [(415, 331), (336, 331), (316, 271)],                # right stick
    [(333, 386), (333, 300), (306, 277)],                # R3
]
lines = Image.new('RGBA', (W, W), (0, 0, 0, 0))
ld = ImageDraw.Draw(lines)
for pts in LINES:
    q = [(x * S, y * S) for x, y in pts]
    ld.line(q, fill=(200, 30, 30, 255), width=5 * S, joint='curve')
lglow = lines.filter(ImageFilter.GaussianBlur(2 * S))
for pts in LINES:
    q = [(x * S, y * S) for x, y in pts]
    ld.line(q, fill=(255, 225, 225, 255), width=1 * S + 2, joint='curve')
out = Image.new('RGBA', (W, W), (0, 0, 0, 0))
out.alpha_composite(lglow)
out.alpha_composite(body)
out.alpha_composite(lines)
out.resize((512, 512), Image.LANCZOS).save(sys.argv[1] if len(sys.argv) > 1 else 'controller.png')
