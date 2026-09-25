#!/usr/bin/env python3
"""Measure the vertical shift between two rendered eye images.

The horizontal twin of this measures the convergence trim; this one exists to
prove the vertical trim reaches the picture at all. Two images that differ only
by a vertical offset compare best at that row offset.

Usage: python scripts/eye_shift_v.py <a.bgra> <b.bgra> <width> <height>
"""
import sys

Y_STEP = 4
X_STEP = 16
MARGIN = 100


def luma(data, w, x, y):
    o = (y * w + x) * 4
    return (data[o + 2] * 299 + data[o + 1] * 587 + data[o] * 114) // 1000


def main():
    a_path, b_path = sys.argv[1], sys.argv[2]
    w, h = int(sys.argv[3]), int(sys.argv[4])
    a = open(a_path, "rb").read()
    b = open(b_path, "rb").read()
    if len(a) < w * h * 4 or len(b) < w * h * 4:
        raise SystemExit("file(s) too small")

    def mad(shift):
        tot = 0
        n = 0
        for y in range(MARGIN, h - MARGIN, Y_STEP):
            ys = y + shift
            if not (MARGIN <= ys < h - MARGIN):
                continue
            for x in range(MARGIN, w - MARGIN, X_STEP):
                tot += abs(luma(a, w, x, y) - luma(b, w, x, ys))
                n += 1
        return tot / n if n else 1e9

    coarse = sorted((mad(s), s) for s in range(-40, 41, 2))
    fine = sorted((mad(s), s) for s in range(coarse[0][1] - 2, coarse[0][1] + 3))
    print(f"{a_path} vs {b_path} ({w}x{h})")
    for d, s in fine[:4]:
        print(f"    vertical shift {s:+4d}px -> mean |dL| = {d:7.2f}")
    print(f"    vertical shift    0px -> mean |dL| = {mad(0):7.2f}   (the no-shift case)")
    print(f"  RESULT: best vertical shift {fine[0][1]:+d}px, residual {fine[0][0]:.2f}")


if __name__ == "__main__":
    main()
