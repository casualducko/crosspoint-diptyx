#!/usr/bin/env python3
"""Reference implementation of the Diptyx 'Comics' halftone (lib/Epub/Epub/converters/ComicsHalftone.cpp).

An independent integer implementation of the same algorithm; test_comics_halftone.py requires the firmware code to match
it bit for bit. Input: the page already downscaled to the panel as 8-bit gray. Output: 0 (black) or 3 (white) per pixel.

  1. Sharpen: S = clamp(2*G - B), B = [1 4 6 4 1] x [1 4 6 4 1] / 256 blur of G (edges replicated, rounded).
  2. Edge mask: pixels within 1 of a 5x5 neighbourhood of G whose max - min > 110 (lettering, line art).
  3. Near edges: black if S < min(0.9 * mean13(S), 150) (a local threshold, so strokes survive an uneven paper tone).
     Elsewhere: Floyd-Steinberg error diffusion (serpentine) of the tone-mapped S, which keeps shading as a halftone.
Tone mapping is the firmware's: tone = LUT[S], then 0 below 40, 255 above 224, (tone-40)*355>>8 between.
"""
import sys

import numpy as np


def _hsum(a, r):  # sum over x-r..x+r, edges replicated; a: int array (h, w)
    p = np.pad(a, ((0, 0), (r, r)), mode="edge").astype(np.int64)
    c = np.cumsum(np.pad(p, ((0, 0), (1, 0))), axis=1)
    w = a.shape[1]
    return c[:, 2 * r + 1:2 * r + 1 + w] - c[:, :w]


def _vsum(a, r):
    return _hsum(a.T, r).T


def _hfilter(a, r, fn):  # max/min over x-r..x+r, edges replicated
    p = np.pad(a, ((0, 0), (r, r)), mode="edge")
    w = a.shape[1]
    return fn(np.stack([p[:, k:k + w] for k in range(2 * r + 1)]), axis=0)


def stretch(t):
    t = t.astype(np.int64)
    return np.where(t <= 40, 0, np.where(t >= 224, 255, ((t - 40) * 355) >> 8))


def halftone(G, tone_lut):
    G = G.astype(np.int64)
    h, w = G.shape
    # 1. sharpen
    K = np.array([1, 4, 6, 4, 1])
    pg = np.pad(G, ((0, 0), (2, 2)), mode="edge")
    H = sum(K[k] * pg[:, k:k + w] for k in range(5))
    ph = np.pad(H, ((2, 2), (0, 0)), mode="edge")
    B = (sum(K[k] * ph[k:k + h, :] for k in range(5)) + 128) >> 8
    S = np.clip(2 * G - B, 0, 255)
    # 2. edge mask
    rmax, rmin = _hfilter(G, 2, np.max), _hfilter(G, 2, np.min)
    mx = _hfilter(rmax.T, 2, np.max).T
    mn = _hfilter(rmin.T, 2, np.min).T
    c0 = ((mx - mn) > 110).astype(np.int64)
    c = _hfilter(c0, 1, np.max)
    c = _hfilter(c.T, 1, np.max).T
    C = c > 0
    # 3a. local threshold near edges
    total = _vsum(_hsum(S, 6), 6)
    mean = (total + 84) // 169
    thr = np.minimum((mean * 9) // 10, 150)
    th = np.where(S >= thr, 3, 0)
    # 3b. Floyd-Steinberg on the tone-mapped sharpened image
    lut = np.asarray(tone_lut, dtype=np.int64)
    a = stretch(lut[S])
    fs = np.zeros((h, w), np.int64)
    e0 = [0] * (w + 2)
    e1 = [0] * (w + 2)
    for y in range(h):
        d = -1 if (y & 1) else 1
        xs = range(w - 1, -1, -1) if d < 0 else range(w)
        row = a[y]
        for x in xs:
            v = int(row[x]) + e0[x + 1]
            new = 255 if v >= 128 else 0
            fs[y, x] = 3 if new else 0
            err = v - new
            q7 = (err * 7) >> 4
            q3 = (err * 3) >> 4
            q5 = (err * 5) >> 4
            e0[x + 1 + d] += q7
            e1[x + 1 - d] += q3
            e1[x + 1] += q5
            e1[x + 1 + d] += err - q7 - q3 - q5
        e0, e1 = e1, [0] * (w + 2)
    return np.where(C, th, fs).astype(np.uint8)


def main():
    """stdin: 'w h' line, 256 tone-LUT values (one line), then w*h bytes; stdout: w*h output bytes."""
    data = sys.stdin.buffer.read()
    first, rest = data.split(b"\n", 1)
    w, h = (int(v) for v in first.split())
    lut_line, raw = rest.split(b"\n", 1)
    lut = [int(v) for v in lut_line.split()]
    G = np.frombuffer(raw, dtype=np.uint8, count=w * h).reshape(h, w)
    sys.stdout.buffer.write(halftone(G, lut).tobytes())


if __name__ == "__main__":
    main()
