#!/usr/bin/env python3
"""Locate the content bounding box inside a compositor frame dump (BGRA).

Used to tell "the panel itself has black regions" apart from "the compositor
cropped the panel", by measuring the *source* frame rather than the eye buffer.

Usage: python scripts/frame_probe.py <frame.bgra> [width] [height]
"""
import sys

LUMA_MIN = 8


def main(src, width=1280, height=720):
    data = open(src, "rb").read()
    if len(data) < width * height * 4:
        raise SystemExit(f"{src}: too small ({len(data)}) for {width}x{height}")

    col_hits = [0] * width
    row_hits = [0] * height
    for y in range(height):
        base = y * width * 4
        for x in range(width):
            o = base + x * 4
            b, g, r = data[o], data[o + 1], data[o + 2]
            if (r * 299 + g * 587 + b * 114) // 1000 >= LUMA_MIN:
                col_hits[x] += 1
                row_hits[y] += 1

    def span(hits, total):
        th = max(1, total // 5)
        idx = [i for i, h in enumerate(hits) if h >= th]
        return (idx[0], idx[-1]) if idx else (None, None)

    x0, x1 = span(col_hits, height)
    y0, y1 = span(row_hits, width)
    print(f"{src}: {width}x{height}")
    if x0 is None:
        print("  frame is entirely black")
        return
    print(f"  content columns {x0}..{x1} of {width} "
          f"(right margin {width-1-x1}px = {100.0*(width-1-x1)/width:.1f}%)")
    print(f"  content rows    {y0}..{y1} of {height} "
          f"(bottom margin {height-1-y1}px = {100.0*(height-1-y1)/height:.1f}%)")
    print(f"  extent {x1-x0+1}x{y1-y0+1}, aspect {(x1-x0+1)/(y1-y0+1):.3f}")

    # Fraction of the right tenth that is dark, to judge the right-edge band.
    tenth = width // 10
    dark = sum(1 for x in range(width - tenth, width)
               for _ in (0,) if col_hits[x] < height // 5)
    print(f"  right tenth ({tenth}px): {dark}/{tenth} columns mostly dark")


if __name__ == "__main__":
    a = sys.argv[1:]
    w = int(a[1]) if len(a) > 1 else 1280
    h = int(a[2]) if len(a) > 2 else 720
    main(a[0], w, h)
