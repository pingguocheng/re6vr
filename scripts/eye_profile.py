#!/usr/bin/env python3
"""Scan an eye dump's horizontal profile for seams and black bands.

Prints the mean luma of each 1/64th column band, then reports the largest
adjacent-band jumps. A layout mistake (two eyes sampling the same slice, or a
half-blitted buffer) shows up as a discontinuity near the midpoint or as a
band that is flat black while its neighbour is not.

Usage: python scripts/eye_profile.py <eye.bgra> [width] [height]
"""
import sys


def main(src, width=1996, height=2148):
    data = open(src, "rb").read()
    if len(data) < width * height * 4:
        raise SystemExit(f"{src}: too small for {width}x{height}")

    bands = 64
    bw = width // bands
    means = []
    for b in range(bands):
        x0 = b * bw
        s = 0
        n = 0
        # Sample a horizontal stripe out of the middle third, avoiding the very
        # top and bottom where letterboxing would dominate.
        for y in range(height // 3, 2 * height // 3, 7):
            base = y * width * 4
            for x in range(x0, x0 + bw, 3):
                o = base + x * 4
                s += (data[o + 2] * 299 + data[o + 1] * 587 + data[o] * 114) // 1000
                n += 1
        means.append(s / max(1, n))

    print(f"{src}: {width}x{height}, column profile in {bands} bands "
          f"({bw}px each, mid-third stripe)")
    print("  band  x-range      mean-luma   bar")
    for b, m in enumerate(means):
        bar = "#" * int(m / 2)
        print(f"  {b:>4}  {b*bw:>5}-{b*bw+bw-1:<6}  {m:7.1f}   {bar}")

    jumps = sorted(((abs(means[i + 1] - means[i]), i) for i in range(bands - 1)),
                   reverse=True)[:6]
    print("  largest adjacent jumps:")
    for d, i in jumps:
        print(f"    between band {i} and {i+1} at x={i*bw+bw}: delta {d:.1f} "
              f"({means[i]:.1f} -> {means[i+1]:.1f})")

    mid = bands // 2
    print(f"  midpoint band {mid} starts at x={mid*bw} (50% = {width//2}): "
          f"luma {means[mid]:.1f}, neighbour {means[mid-1]:.1f}")


if __name__ == "__main__":
    a = sys.argv[1:]
    w = int(a[1]) if len(a) > 1 else 1996
    h = int(a[2]) if len(a) > 2 else 2148
    main(a[0], w, h)
