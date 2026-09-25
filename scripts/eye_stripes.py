#!/usr/bin/env python3
"""Locate the stripe ruler in a rendered eye image.

The ruler is 24 black/white stripes across the picture with bright magenta
markers. Reporting where the stripe boundaries and the magenta bands land says
exactly where the picture sits in that eye - which is the only way to tell
"both eyes show the same picture" from "both eyes show it in the same place".

Usage: python scripts/eye_stripes.py <eye.bgra> <width> <height> [row]
"""
import sys


def main():
    src = sys.argv[1]
    w = int(sys.argv[2])
    h = int(sys.argv[3])
    row = int(sys.argv[4]) if len(sys.argv) > 4 else h // 2

    data = open(src, "rb").read()
    if len(data) < w * h * 4:
        raise SystemExit(f"{src}: {len(data)} bytes < {w*h*4}")
    base = (row * w) * 4

    lum = []
    for x in range(w):
        o = base + x * 4
        b, g, r = data[o], data[o + 1], data[o + 2]
        lum.append((r * 299 + g * 587 + b * 114) // 1000)

    bright = [l > 120 for l in lum]
    # Mid-crossings of the bright bands: where the picture's stripe boundaries are.
    mid = []
    x = 0
    while x < w:
        if bright[x]:
            x0 = x
            while x < w and bright[x]:
                x += 1
            mid.append((x0 + x - 1) / 2.0)
        else:
            x += 1

    magenta = []
    x = 0
    while x < w:
        o = base + x * 4
        b, g, r = data[o], data[o + 1], data[o + 2]
        if r > 90 and b > 90 and g < 0.6 * min(r, b):
            x0 = x
            while x < w:
                o = base + x * 4
                b, g, r = data[o], data[o + 1], data[o + 2]
                if not (r > 90 and b > 90 and g < 0.6 * min(r, b)):
                    break
                x += 1
            magenta.append((x0, x - 1))
        else:
            x += 1

    print(f"{src} row {row}: {len(mid)} bright bands, {len(magenta)} magenta band(s)")
    if len(mid) >= 3:
        gaps = [mid[i + 1] - mid[i] for i in range(len(mid) - 1)]
        print(f"  first bright centre {mid[0]:.1f}, last {mid[-1]:.1f}")
        print(f"  stripe pitch (mean of {len(gaps)}): {sum(gaps)/len(gaps):.2f} px, "
              f"min {min(gaps):.2f}, max {max(gaps):.2f}")
        print(f"  centres: {[round(m, 1) for m in mid[:10]]}")
    print(f"  magenta bands: {magenta}")


if __name__ == "__main__":
    main()
