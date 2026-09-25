#!/usr/bin/env python3
"""Compare candidate repeat regions of a frame: same shape, or different content?

Used to tell "the frame contains three copies of one picture" (a rendering
fault) from "the frame contains three different versions of an animation"
(what an intro does). For each horizontal window it reports the non-black
coverage, the peak luma and the mean luma, so brightness differences and
shape differences are both visible.

Usage: python scripts/frame_regions.py <frame.bgra> <w> <h> <y0> <y1> <x0:x1> [...]
"""
import sys


def stats(data, w, y0, y1, x0, x1):
    tot = 0
    n = 0
    peak = 0
    nonblack = 0
    lsum = 0
    for y in range(y0, y1):
        base = (y * w) * 4
        for x in range(x0, x1):
            o = base + x * 4
            b, g, r = data[o], data[o + 1], data[o + 2]
            l = (r * 299 + g * 587 + b * 114) // 1000
            lsum += l
            if l > peak:
                peak = l
            if l > 12:
                nonblack += 1
            tot += l
            n += 1
    return nonblack / n, peak, lsum / n


def main():
    src = sys.argv[1]
    w, h = int(sys.argv[2]), int(sys.argv[3])
    y0, y1 = int(sys.argv[4]), int(sys.argv[5])
    windows = []
    for a in sys.argv[6:]:
        lo, hi = a.split(":")
        windows.append((int(lo), int(hi)))

    data = open(src, "rb").read()
    print(f"{src} rows {y0}..{y1}")
    for x0, x1 in windows:
        cov, peak, mean = stats(data, w, y0, y1, x0, x1)
        print(f"  x {x0:5d}..{x1:5d} (w {x1-x0:4d}): lit {100*cov:5.1f}%  "
              f"peak luma {peak:3d}  mean luma {mean:6.2f}")


if __name__ == "__main__":
    main()
