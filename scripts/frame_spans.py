#!/usr/bin/env python3
"""Locate the horizontal extent of the picture inside a dump.

Scans a band of rows and reports the runs of columns that carry any signal,
so "the picture is drawn three times" can be stated in pixels instead of
read off a screenshot. Works on a game frame dump and on an eye dump.

Usage: python scripts/frame_spans.py <dump.bgra> <width> <height> [row0] [row1]
"""
import sys


def main():
    src = sys.argv[1]
    width = int(sys.argv[2])
    height = int(sys.argv[3])
    row0 = int(sys.argv[4]) if len(sys.argv) > 4 else height // 3
    row1 = int(sys.argv[5]) if len(sys.argv) > 5 else height // 3 + height // 8

    data = open(src, "rb").read()
    if len(data) < width * height * 4:
        raise SystemExit(f"{src}: {len(data)} bytes, need {width * height * 4}")

    # A column is "lit" if any sampled pixel in the band is above the threshold.
    threshold = 24
    lit = bytearray(width)
    for y in range(row0, min(row1, height), 2):
        base = (y * width) * 4
        for x in range(width):
            o = base + x * 4
            r, g, b = data[o + 2], data[o + 1], data[o]
            if r > threshold or g > threshold or b > threshold:
                lit[x] = 1

    spans = []
    x = 0
    while x < width:
        if lit[x]:
            x0 = x
            gap = 0
            while x < width and gap < 12:
                if lit[x]:
                    gap = 0
                else:
                    gap += 1
                x += 1
            x1 = x - gap
            spans.append((x0, x1))
        else:
            x += 1

    print(f"{src} ({width}x{height}), rows {row0}..{row1}, threshold {threshold}")
    print(f"  {len(spans)} lit span(s) along x:")
    for x0, x1 in spans:
        w = x1 - x0 + 1
        print(f"    x {x0:5d}..{x1:5d}  width {w:5d}  "
              f"({100.0 * w / width:5.1f}% of the row)")
    if len(spans) >= 2:
        print("  gaps between spans:")
        for i in range(len(spans) - 1):
            print(f"    {spans[i][1]} .. {spans[i + 1][0]}  "
                  f"({spans[i + 1][0] - spans[i][1] - 1} px)")
        print("  centre-to-centre distances:")
        centres = [(a + b) / 2.0 for a, b in spans]
        for i in range(len(centres) - 1):
            print(f"    {centres[i]:8.1f} -> {centres[i + 1]:8.1f}  "
                  f"({centres[i + 1] - centres[i]:.1f} px)")


if __name__ == "__main__":
    main()
