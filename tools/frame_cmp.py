#!/usr/bin/env python3
"""GL back end vs software NV2A, frame by frame (coarse regression check).

    python3 tools/frame_cmp.py <gl_dir> <sw_dir> [--flips 30,45,60] [--tol 24]

<gl_dir>: CJ_GL_DUMP frames (gNNNNN.ppm, NNNNN = flip number).
<sw_dir>: RECOMP_FB_DUMP_FLIPS=1 frames (fNNNNN.bmp, NNNNN = dump sequence; the
flip dump of flip N is fN-1 when no after-draw dumps came first, so the script
picks, for each flip, the sw frame among N-3..N+1 closest to the GL frame:
the two renderers' timing differs by a flip or two at most in the 2D phase).

Prints mean absolute difference per channel (0-255) and PSNR at 640x480;
exit status 1 if any compared frame is over --tol.

    python3 tools/frame_cmp.py <dir> --ab [--flips ...] [--tol 24]

A/B mode (CJ_GL_VSH_AB=1 runs): the same flip rendered with GPU vertex
programs (gNNNNN.ppm) and with the CPU interpreter (gcNNNNN.ppm) in one run,
so content is identical; also prints the share of pixels off by more than 8
and the largest difference.
"""
import os, sys
from PIL import Image, ImageChops, ImageStat
import math


def load(p):
    return Image.open(p).convert('RGB').resize((640, 480))


def diff(a, b):
    d = ImageChops.difference(a, b)
    st = ImageStat.Stat(d)
    mad = sum(st.mean) / 3
    mse = sum(v * v for v in st.rms) / 3
    psnr = 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)
    return mad, psnr


def ab(a):
    d = a[0]
    flips, tol = None, 24.0
    i = 2
    while i < len(a):
        if a[i] == '--flips': flips = [int(x) for x in a[i + 1].split(',')]; i += 2
        elif a[i] == '--tol': tol = float(a[i + 1]); i += 2
        else: sys.exit('bad arg ' + a[i])
    if flips is None:
        flips = sorted(int(f[2:7]) for f in os.listdir(d) if f.startswith('gc') and f.endswith('.ppm'))
    bad = 0
    for n in flips:
        gp, cp = os.path.join(d, 'g%05d.ppm' % n), os.path.join(d, 'gc%05d.ppm' % n)
        if not (os.path.exists(gp) and os.path.exists(cp)):
            continue
        g, c = load(gp), load(cp)
        mad, psnr = diff(g, c)
        dd = ImageChops.difference(g, c).convert('L')
        h = dd.histogram()
        over = sum(h[9:]) / float(640 * 480) * 100
        mx = max(k for k in range(256) if h[k])
        flag = 'OK' if mad <= tol else 'DIFF'
        bad += flag != 'OK'
        print('flip %5d GPU-VS vs CPU-VS: mean abs diff %6.3f  PSNR %5.1f dB  >8: %5.2f%%  max %3d  %s'
              % (n, mad, psnr, over, mx, flag))
    sys.exit(1 if bad else 0)


def main():
    a = sys.argv[1:]
    if len(a) > 1 and a[1] == '--ab':
        return ab(a)
    gl, sw = a[0], a[1]
    flips, tol = None, 24.0
    i = 2
    while i < len(a):
        if a[i] == '--flips': flips = [int(x) for x in a[i + 1].split(',')]; i += 2
        elif a[i] == '--tol': tol = float(a[i + 1]); i += 2
        else: sys.exit('bad arg ' + a[i])
    if flips is None:
        flips = sorted(int(f[1:6]) for f in os.listdir(gl) if f.startswith('g') and f[1].isdigit() and f.endswith('.ppm'))
    bad = 0
    for n in flips:
        gp = os.path.join(gl, 'g%05d.ppm' % n)
        if not os.path.exists(gp):
            continue
        g = load(gp)
        best = None
        for k in range(n - 3, n + 2):
            sp = os.path.join(sw, 'f%05d.bmp' % k)
            if os.path.exists(sp):
                r = diff(g, load(sp))
                if best is None or r[0] < best[0][0]:
                    best = (r, k)
        if best is None:
            print('flip %5d: no software frame' % n)
            continue
        (mad, psnr), k = best
        flag = 'OK' if mad <= tol else 'DIFF'
        bad += flag != 'OK'
        print('flip %5d vs sw f%05d: mean abs diff %5.1f  PSNR %5.1f dB  %s' % (n, k, mad, psnr, flag))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
