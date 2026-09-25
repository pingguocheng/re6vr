#!/usr/bin/env python3
"""Decide the on-disk layout of an eye dump by looking for the picture's edges.

The dump is one buffer of sc_width-wide rows holding as many vertical slices
as the swapchain has. Both candidate widths can be made to fit the file size,
so the layout has to be decided from the content:

  * If a per-eye slice is narrower than the file width, the slice boundary is
    a hard seam - the picture stops and black starts, twice in the row.
  * If the file width IS the per-eye width, the picture is a single 16:9
    rectangle with black bars above and below it, and no vertical seam.

So: scan the middle row band for lit column runs at a low threshold. One run
= single picture per row (per-eye width = file width). Two runs with black
between them = the file is two slices side by side.

Usage: python scripts/eye_layout.py <dump.bgra> <file_width> <total_rows> <slice_h> [band]
"""
import sys


def main():
    src = sys.argv[1]
    fw = int(sys.argv[2])          # file width in pixels
    rows = int(sys.argv[3])        # total rows in the file
    sh = int(sys.argv[4])          # candidate per-eye height
    band = int(sys.argv[5]) if len(sys.argv) > 5 else 40

    data = open(src, "rb").read()
    expect = fw * rows * 4
    print(f"{src}: {len(data)} bytes; as {fw} wide that is {len(data)//(fw*4)} rows "
          f"(assumed {rows}, {'OK' if len(data) == expect else 'MISMATCH'})")
    slices = rows // sh if sh else 0
    print(f"  {rows} rows / {sh} per slice = {slices} vertical slice(s)")

    for si in range(max(slices, 1)):
        y0 = si * sh + sh // 2
        for label, yc in (("upper third", si * sh + sh // 3),
                          ("middle", y0)):
            lit = bytearray(fw)
            for y in range(yc - band, yc + band, 4):
                if y < 0 or y >= rows:
                    continue
                base = (y * fw) * 4
                for x in range(fw):
                    o = base + x * 4
                    if data[o] > 20 or data[o + 1] > 20 or data[o + 2] > 20:
                        lit[x] = 1
            spans = []
            x = 0
            while x < fw:
                if lit[x]:
                    x0 = x
                    gap = 0
                    while x < fw and gap < 10:
                        gap = gap + 1 if not lit[x] else 0
                        x += 1
                    spans.append((x0, x - gap))
                else:
                    x += 1
            print(f"  slice {si} {label} (y~{yc}): {len(spans)} lit span(s)")
            for a, b in spans:
                if b - a > 8:
                    print(f"      x {a:5d}..{b:5d}  width {b - a + 1:5d}  "
                          f"({100.0*(b-a+1)/fw:5.1f}% of the row)")


if __name__ == "__main__":
    main()
