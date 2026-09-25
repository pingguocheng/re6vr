#!/usr/bin/env python3
"""Measure panel coverage and letterbox bands in ONE eye slice.

Reads a single-eye BGRA buffer (sc_width x sc_height, e.g. 998x2148) and reports
where the picture starts and ends on each axis, which is what decides whether the
source frame is shown whole (letterboxed) or cropped to the viewport's shape.

Usage: python scripts/eye_cover.py <src.bgra> <w> <h> [offset_rows]
"""
import sys

LUMA_MIN = 8


def main(src, width, height, row_offset=0):
    data = open(src, "rb").read()
    need = (row_offset + height) * width * 4
    if len(data) < need:
        raise SystemExit(f"{src}: too small ({len(data)}) for {need}")

    col_hits = [0] * width
    row_hits = [0] * height
    for y in range(height):
        base = ((row_offset + y) * width) * 4
        hits_row = 0
        for x in range(width):
            o = base + x * 4
            b, g, r = data[o], data[o + 1], data[o + 2]
            if (r * 299 + g * 587 + b * 114) // 1000 >= LUMA_MIN:
                col_hits[x] += 1
                hits_row += 1
        row_hits[y] = hits_row

    def span(hits, total, frac=0.5):
        th = max(1, int(total * frac))
        idx = [i for i, h in enumerate(hits) if h >= th]
        return (idx[0], idx[-1]) if idx else (None, None)

    print(f"{src} [rows {row_offset}..{row_offset+height-1}]: {width}x{height}")
    x0, x1 = span(col_hits, height)
    y0, y1 = span(row_hits, width)
    if x0 is None:
        print("  BLACK")
        return
    cw, ch = x1 - x0 + 1, y1 - y0 + 1
    print(f"  columns {x0}..{x1}  -> width {cw} ({100.0*cw/width:.1f}% of viewport),"
          f" left bar {x0}px ({100.0*x0/width:.1f}%), right bar {width-1-x1}px "
          f"({100.0*(width-1-x1)/width:.1f}%)")
    print(f"  rows    {y0}..{y1}  -> height {ch} ({100.0*ch/height:.1f}% of viewport),"
          f" top bar {y0}px ({100.0*y0/height:.1f}%), bottom bar {height-1-y1}px "
          f"({100.0*(height-1-y1)/height:.1f}%)")
    print(f"  picture aspect {cw/ch:.3f}  (source is 16:9 = 1.778, "
          f"viewport is {width/height:.3f})")
    if abs(cw / ch - 16.0 / 9.0) < 0.06:
        print("  -> picture keeps the 16:9 shape: the whole frame fits (bars are "
              "letterbox/pillarbox)")
    elif cw / ch < 16.0 / 9.0 - 0.06:
        print("  -> picture is TALLER than 16:9: the frame is cropped horizontally "
              "and/or stretched vertically")
    else:
        print("  -> picture is WIDER than 16:9: unexpected")

    # Vertical edges of the picture, to see if the panel is centred.
    print(f"  vertical centring: top bar {y0}, bottom bar {height-1-y1} "
          f"(difference {abs(y0-(height-1-y1))}px)")


if __name__ == "__main__":
    a = sys.argv[1:]
    main(a[0], int(a[1]), int(a[2]), int(a[3]) if len(a) > 3 else 0)
