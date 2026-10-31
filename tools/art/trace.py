#!/usr/bin/env python3
"""Redraw flat-colour UI art (frames, panels) as vectors at N x.

The texture is split into flat layers by masks, each layer's outline is
traced (pixel-corner contours, holes kept), straightened with a small
Douglas-Peucker tolerance (pixel staircases become clean diagonals, straight
runs stay straight) and filled again at N x with 4x4 supersampling.

usage: trace.py in.png out.png [--scale 4] [--eps 0.15]  (outline straightening tolerance, px)
Layers: 'fill' (any coverage, the median colour of the dim pixels) under
'lines' (bright pixels).
"""
import sys
import numpy as np
import cv2
from skimage import measure
from PIL import Image

def destair(p, step):
    """p: closed rectilinear polygon (pixel-corner vertices). Runs of short
    segments (<= step px, a pixel staircase) become one diagonal; longer
    segments stay exactly horizontal/vertical."""
    # merge collinear points into axis-aligned segments
    q = [p[0]]
    for pt in p[1:]:
        if len(q) >= 2 and ((q[-2][0] == q[-1][0] == pt[0]) or (q[-2][1] == q[-1][1] == pt[1])):
            q[-1] = pt
        else:
            q.append(pt)
    q = np.array(q, np.float64)
    n = len(q)
    if n < 4: return q
    seg = [np.abs(q[(i + 1) % n] - q[i]).sum() for i in range(n)]   # segment i: q[i] -> q[i+1]
    short = [l <= step for l in seg]
    if all(short): return q
    # rotate so we start at the beginning of a long segment
    s0 = next(i for i in range(n) if not short[i])
    order = [(s0 + i) % n for i in range(n)]
    out = []
    i = 0
    while i < n:
        k = order[i]
        if not short[k]:
            out.append(q[k]); i += 1; continue
        j = i
        while j < n and short[order[j]]: j += 1
        if j - i >= 2:                       # staircase: keep its two ends only
            out.append(q[k])
        else:
            out.append(q[k])
        i = j if j - i >= 2 else i + 1
    return np.array(out)

def trace_mask_rect(mask, eps):
    # contours of the 2x nearest grid follow pixel corners exactly
    m2 = np.kron(mask.astype(np.uint8), np.ones((2, 2), np.uint8)) * 255
    m2 = cv2.copyMakeBorder(m2, 1, 1, 1, 1, cv2.BORDER_CONSTANT, value=0)
    cs, hier = cv2.findContours(m2, cv2.RETR_CCOMP, cv2.CHAIN_APPROX_SIMPLE)
    polys = []
    for c in cs:
        p = c.reshape(-1, 2).astype(np.float64) - 1
        # 2x-grid contour runs along pixel centres of the doubled image: snap to pixel corners
        p = np.round(p / 2.0 * 2) / 2.0
        p = np.round((p + 0.25) * 1.0)       # corner grid of the original (contour sits half a 2x-pixel inside)
        p = destair(p, eps)
        polys.append(p)
    return polys

def smooth_keep_corners(p, sigma=1.4, corner_deg=125):
    """Gaussian-smooth a closed outline (removes the pixel wobble on curves and
    long diagonals) but pin the points at real corners so they stay sharp."""
    if np.allclose(p[0], p[-1]): p = p[:-1]
    n = len(p)
    if n < 8: return p
    # resample to ~0.25 px spacing
    seg = np.linalg.norm(np.diff(np.vstack([p, p[:1]]), axis=0), axis=1)
    t = np.concatenate([[0], np.cumsum(seg)])
    L = t[-1]
    m = max(int(L / 0.25), 8)
    u = np.linspace(0, L, m, endpoint=False)
    q = np.vstack([np.interp(u, t, np.append(p[:, 0], p[0, 0])), np.interp(u, t, np.append(p[:, 1], p[0, 1]))]).T
    k = int(np.ceil(3 * sigma / 0.25))
    w = np.exp(-0.5 * (np.arange(-k, k + 1) * 0.25 / sigma) ** 2); w /= w.sum()
    sm = np.vstack([np.convolve(np.concatenate([q[-k:, j], q[:, j], q[:k, j]]), w, 'valid') for j in range(2)]).T
    # corner test on the original outline, 1 px either side
    d = int(round(1.0 / 0.25))
    a = np.roll(q, d, 0) - q; b = np.roll(q, -d, 0) - q
    cosang = (a * b).sum(1) / (np.linalg.norm(a, axis=1) * np.linalg.norm(b, axis=1) + 1e-9)
    corner = np.degrees(np.arccos(np.clip(cosang, -1, 1))) < corner_deg
    # blend: pinned near corners, smoothed elsewhere
    wgt = np.convolve(np.concatenate([corner[-d:], corner, corner[:d]]).astype(float), np.ones(2 * d + 1), 'valid')
    wgt = np.clip(wgt, 0, 1)[:, None]
    return q * wgt + sm * (1 - wgt)

def trace_field(field, level, eps):
    """Sub-pixel outlines of a soft coverage field (the anti-aliased edges place
    them between pixels, so curves come out round), straightened with
    Douglas-Peucker at eps px."""
    f = np.pad(field, 1)
    polys = []
    for c in measure.find_contours(f, level):
        p = c[:, ::-1] - 1 + 0.5            # (row, col) of pixel centres -> x, y on the corner grid
        if len(p) < 4: continue
        p = smooth_keep_corners(p)
        p = cv2.approxPolyDP(p.astype(np.float32).reshape(-1, 1, 2), eps, True).reshape(-1, 2).astype(np.float64)
        polys.append(p)
    return polys

def fill(polys, size, scale, ss=4):
    W, H = size
    k = scale * ss
    img = np.zeros((H * k, W * k), np.uint8)
    pts = [np.round(p * k * 16).astype(np.int32) for p in polys if len(p) >= 3]
    # even-odd via XOR of each contour (outer + holes from RETR_CCOMP)
    for q in pts:
        t = np.zeros_like(img)
        cv2.fillPoly(t, [q], 255, lineType=cv2.LINE_8, shift=4)
        img ^= t
    return cv2.resize(img, (W * scale, H * scale), interpolation=cv2.INTER_AREA)

def main():
    a = sys.argv[1:]
    scale, eps = 4, 0.15
    if '--scale' in a: i = a.index('--scale'); scale = int(a[i + 1]); del a[i:i + 2]
    if '--eps' in a: i = a.index('--eps'); eps = float(a[i + 1]); del a[i:i + 2]
    src, dst = a
    im = np.array(Image.open(src).convert('RGBA')).astype(np.int32)
    H, W = im.shape[:2]
    al = im[:, :, 3]; lum = im[:, :, :3].max(2)
    lines = (lum > 128) & (al > 128)
    anyc = al > 32
    fl = np.clip((lum - 64) / 128.0, 0, 1) * (al / 255.0)     # soft 'bright' field
    fa = al / 255.0
    dim = anyc & ~lines
    fc = np.median(im[dim], axis=0) if dim.any() else np.array([0, 0, 0, 0])
    lc = np.median(im[lines], axis=0) if lines.any() else np.array([255, 255, 255, 255])
    out = np.zeros((H * scale, W * scale, 4), np.float64)
    for mask, col, field in ((anyc, fc, fa), (lines, lc, fl)):
        if not mask.any(): continue
        level = 0.5 * float(np.median(field[mask]))
        cov = fill(trace_field(field, level, eps), (W, H), scale) / 255.0
        a_src = cov * (col[3] / 255.0)
        rgb = np.array(col[:3], np.float64)
        a_dst = out[:, :, 3]
        a_out = a_src + a_dst * (1 - a_src)
        for ch in range(3):
            out[:, :, ch] = np.where(a_out > 0, (rgb[ch] * a_src + out[:, :, ch] * a_dst * (1 - a_src)) / np.maximum(a_out, 1e-9), 0)
        out[:, :, 3] = a_out
    o = np.dstack([np.clip(out[:, :, :3], 0, 255), np.clip(out[:, :, 3] * 255, 0, 255)]).astype(np.uint8)
    Image.fromarray(o, 'RGBA').save(dst)
    print(f'{dst}: {W * scale}x{H * scale} fill {fc} lines {lc}')

if __name__ == '__main__':
    main()
