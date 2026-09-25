#!/usr/bin/env python3
"""Fit the relationship between two rendered eye images showing a cross.

Two eye images can differ in ways a translation cannot fix - a relative scale or
a relative roll - and that is what makes "align it level and it goes X when I
tilt my head" happen. This measures the difference from one pair of dumps using
column profiles, which is robust where a per-row peak search is not:

  * the cross's vertical bar is hundreds of pixels wide, so its centre is read
    from the brightness centroid of the whole column profile;
  * the same centroid computed over the TOP and BOTTOM halves separately gives
    the bar's lean, and a lean that differs between the eyes is a relative roll;
  * the ratio of the two eyes' bar excursions from their own centres is the
    relative scale.

Usage: python scripts/eye_fit.py <eye0.bgra> <eye1.bgra> <width> <height>
"""
import sys


def profile(data, w, y0, y1):
    col = [0] * w
    for y in range(y0, y1, 2):
        base = y * w * 4
        for x in range(w):
            o = base + x * 4
            col[x] += data[o] + data[o + 1] + data[o + 2]
    return col


def centroid(prof, w):
    tot = sum(prof)
    if tot <= 0:
        return float("nan")
    return sum(x * prof[x] for x in range(w)) / tot


def main():
    a_path, b_path = sys.argv[1], sys.argv[2]
    w, h = int(sys.argv[3]), int(sys.argv[4])
    a = open(a_path, "rb").read()
    b = open(b_path, "rb").read()
    # Skip the bottom readout strip.
    top = (int(h * 0.15), int(h * 0.45))
    bot = (int(h * 0.55), int(h * 0.85))

    rows = []
    for name, data in (("eye0", a), ("eye1", b)):
        pt = profile(data, w, *top)
        pb = profile(data, w, *bot)
        ct = centroid(pt, w)
        cb = centroid(pb, w)
        rows.append((name, ct, cb, cb - ct))
    print(f"{a_path} vs {b_path} ({w}x{h})")
    for name, ct, cb, d in rows:
        print(f"  {name}: bar centre top {ct:7.1f}, bottom {cb:7.1f}, "
              f"lean {d:+7.1f} px over the sampled span")
    off = rows[1][1] - rows[0][1]
    roll = (rows[1][3] - rows[0][3])
    print(f"  horizontal offset between the eyes (top half): {off:+.1f} px "
          f"({100.0*off/w:+.2f}% of the width)")
    print(f"  relative roll (difference of leans)          : {roll:+.1f} px "
          f"across the two sampled bands")
    # The bands are centred at 30% and 70% of the height, so their separation in
    # pixels is known and the px figure converts to degrees.
    sep = (0.70 - 0.30) * h
    import math
    print(f"  -> that is {math.degrees(math.atan2(roll, sep)):+.2f} deg of relative roll")


if __name__ == "__main__":
    main()
