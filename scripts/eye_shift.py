#!/usr/bin/env python3
"""Measure the horizontal shift between the two eyes of an eye dump.

The dump is ONE texture of sc_width x sc_height written with two vertical
slices stacked (see README conclusion 7), so for a 1996x2148 swapchain the
file is 1996 wide and 4296 tall: rows [0,2148) are eye 0, rows [2148,4296)
are eye 1. Reading it as side-by-side halves mixes the two slices and gives
a plausible-looking but wrong answer.

It measures the shift that best aligns eye 1 onto eye 0, by minimising the
mean absolute luma difference on a sampled grid. Only sampled rows/columns
are touched, so no numpy is needed and a 17 MB dump still runs in seconds.

Usage:
    python scripts/eye_shift.py <stacked.bgra> --stacked [width] [height]
    python scripts/eye_shift.py <eye0.bgra> <eye1.bgra>  [width] [height]
"""
import sys

Y_STEP = 8      # rows sampled
X_STEP = 2      # columns sampled
MARGIN = 96     # ignore this many columns at each edge
RANGE = 192     # search +- this many pixels


def plane(data, width, height, y0, y_step=Y_STEP):
    """Sampled grayscale rows as bytearrays (only every y_step-th row)."""
    rows = {}
    for y in range(0, height, y_step):
        base = ((y0 + y) * width) * 4
        row = bytearray(width)
        for x in range(0, width, X_STEP):
            o = base + x * 4
            row[x] = (data[o + 2] * 299 + data[o + 1] * 587 + data[o] * 114) // 1000
        rows[y] = row
    return rows


def mad(rows_a, rows_b, shift, width):
    total = 0
    n = 0
    lo = MARGIN
    hi = width - MARGIN
    for y, ra in rows_a.items():
        rb = rows_b[y]
        for x in range(lo, hi, X_STEP):
            xs = x + shift
            if 0 <= xs < width:
                total += abs(ra[x] - rb[xs])
                n += 1
    return (total / n) if n else 1e9


def mean_luma(rows):
    tot = 0
    n = 0
    for row in rows.values():
        for x in range(MARGIN, len(row) - MARGIN, X_STEP):
            tot += row[x]
            n += 1
    return tot / n if n else 0.0


def main():
    a = sys.argv[1:]
    stacked = "--stacked" in a
    a = [v for v in a if v != "--stacked"]
    if not a:
        raise SystemExit(__doc__)
    width = int(a[2]) if len(a) > 2 else 1996
    height = int(a[3]) if len(a) > 3 else 2148

    if stacked:
        data = open(a[0], "rb").read()
        need = width * height * 2 * 4
        if len(data) < need:
            raise SystemExit(f"{a[0]}: {len(data)} bytes, need {need} for {width}x{height} x2")
        rows0 = plane(data, width, height, 0)
        rows1 = plane(data, width, height, height)
        print(f"{a[0]}: stacked {width}x{height} x2")
    else:
        d0 = open(a[0], "rb").read()
        d1 = open(a[1], "rb").read()
        if len(d0) < width * height * 4 or len(d1) < width * height * 4:
            raise SystemExit("file(s) too small for the given size")
        rows0 = plane(d0, width, height, 0)
        rows1 = plane(d1, width, height, 0)
        print(f"{a[0]} vs {a[1]}: {width}x{height}")

    print(f"  mean luma: eye0 {mean_luma(rows0):.1f}, eye1 {mean_luma(rows1):.1f}")

    coarse = sorted((mad(rows0, rows1, s, width), s) for s in range(-RANGE, RANGE + 1, 4))
    best_s = coarse[0][1]
    fine = sorted((mad(rows0, rows1, s, width), s) for s in range(best_s - 5, best_s + 6))
    best_d, best_s = fine[0]
    zero_d = mad(rows0, rows1, 0, width)

    print("  best alignments (eye1 sampled at x+s, lower = better):")
    for d, s in fine[:4]:
        print(f"    shift {s:+5d}px -> mean |dL| = {d:6.2f}")
    print(f"    shift     0px -> mean |dL| = {zero_d:6.2f}   (the no-shift case)")

    print(f"  RESULT: best shift {best_s:+d}px "
          f"({100.0 * best_s / width:+.2f}% of the {width}px picture), residual {best_d:.2f}")


if __name__ == "__main__":
    main()
