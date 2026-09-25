#!/usr/bin/env python3
"""Autocorrelation of a dumped game frame along x, to find periodicity.

A frame that shows the same content N times side by side has a strong
autocorrelation peak at the repeat period. This distinguishes "the game
really drew it three times" from "our capture produced a skewed/duplicated
image", because a capture artefact repeats with the capture buffer's pitch,
not with the picture's layout.

Usage: python scripts/frame_period.py <frame.bgra> [width] [height]
"""
import sys

Y_STEP = 4
X_STEP = 2
MARGIN = 64


def rows_of(data, width, height):
    rows = []
    for y in range(0, height, Y_STEP):
        base = (y * width) * 4
        row = bytearray(width)
        for x in range(0, width, X_STEP):
            o = base + x * 4
            row[x] = (data[o + 2] * 299 + data[o + 1] * 587 + data[o] * 114) // 1000
        rows.append(row)
    return rows


def main():
    src = sys.argv[1]
    width = int(sys.argv[2]) if len(sys.argv) > 2 else 1996
    height = int(sys.argv[3]) if len(sys.argv) > 3 else 1122
    data = open(src, "rb").read()
    if len(data) < width * height * 4:
        raise SystemExit(f"{src}: too small for {width}x{height}")
    rows = rows_of(data, width, height)

    # Mean absolute difference at each lag: a repeat shows up as a dip.
    results = []
    for lag in range(8, width // 2, 2):
        tot = 0
        n = 0
        for row in rows:
            for x in range(MARGIN, width - MARGIN - lag, X_STEP * 3):
                tot += abs(row[x] - row[x + lag])
                n += 1
        if n:
            results.append((tot / n, lag))

    results.sort()
    print(f"{src} ({width}x{height}): lowest mean |dL| at lag (a repeat period):")
    shown = []
    for d, lag in results:
        # keep peaks that are not multiples of an already reported one
        if any(abs(lag - s) < 8 or (s and lag % s < 8) for s in shown):
            continue
        shown.append(lag)
        print(f"    lag {lag:5d}px -> mean |dL| = {d:6.2f}   "
              f"({100.0 * lag / width:5.1f}% of the width)")
        if len(shown) >= 6:
            break

    # Reference: completely unrelated content compares at the frame's own variance.
    tot = 0
    n = 0
    row0 = rows[0]
    for x in range(MARGIN, width - MARGIN, X_STEP):
        tot += abs(row0[x] - row0[x + 1])
        n += 1
    print(f"  (neighbouring-pixel difference for scale: {tot / n:.2f})")


if __name__ == "__main__":
    main()
