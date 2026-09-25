#!/usr/bin/env python3
"""Measure how well the composed panel covers an eye buffer.

Reads a BGRA eye dump and reports, per edge, where real content starts/ends,
plus a coarse 8x8 occupancy map so banding is obvious without eyeballing.

Usage: python scripts/eye_probe.py <eye.bgra> [width] [height]
"""
import sys

W_DEF, H_DEF = 1996, 2148
# A pixel counts as "content" if it clears this luma; the panel background is
# dark but not pure black, so a low bar separates content from cleared buffer.
LUMA_MIN = 8


def main(src, width=W_DEF, height=H_DEF):
    data = open(src, "rb").read()
    if len(data) < width * height * 4:
        raise SystemExit(f"{src}: too small for {width}x{height}")

    step = 2  # sample every other pixel; plenty for coverage statistics
    col_hits = [0] * width
    row_hits = [0] * height
    grid = [[0] * 8 for _ in range(8)]
    grid_tot = [[0] * 8 for _ in range(8)]

    for y in range(0, height, step):
        base = y * width * 4
        gy = y * 8 // height
        for x in range(0, width, step):
            o = base + x * 4
            b, g, r = data[o], data[o + 1], data[o + 2]
            luma = (r * 299 + g * 587 + b * 114) // 1000
            if luma >= LUMA_MIN:
                col_hits[x] += 1
                row_hits[y] += 1
            gx = x * 8 // width
            grid_tot[gy][gx] += 1
            if luma >= LUMA_MIN:
                grid[gy][gx] += 1

    def first_last(hits, total_rows):
        """First and last index whose hit-rate clears 20%."""
        thresh = max(1, total_rows // 5)
        idx = [i for i, h in enumerate(hits) if h >= thresh]
        return (idx[0], idx[-1]) if idx else (None, None)

    cols_sampled = len(range(0, height, step))
    rows_sampled = len(range(0, width, step))
    x0, x1 = first_last(col_hits, cols_sampled)
    y0, y1 = first_last(row_hits, rows_sampled)

    print(f"{src}: {width}x{height}")
    if x0 is None:
        print("  NO CONTENT AT ALL (buffer is black)")
        return
    print(f"  content columns {x0}..{x1} of {width}  "
          f"(left black {x0}px {100.0*x0/width:.1f}%, "
          f"right black {width-1-x1}px {100.0*(width-1-x1)/width:.1f}%)")
    print(f"  content rows    {y0}..{y1} of {height}  "
          f"(top black {y0}px {100.0*y0/height:.1f}%, "
          f"bottom black {height-1-y1}px {100.0*(height-1-y1)/height:.1f}%)")
    print(f"  content extent  {x1-x0+1}x{y1-y0+1} px, aspect {(x1-x0+1)/(y1-y0+1):.3f}"
          f"  (16:9 = 1.778)")

    print("  occupancy map (percent of samples clearing luma>=%d):" % LUMA_MIN)
    for gy in range(8):
        cells = []
        for gx in range(8):
            t = grid_tot[gy][gx] or 1
            cells.append(f"{100*grid[gy][gx]//t:3d}")
        print("    " + " ".join(cells))


if __name__ == "__main__":
    a = sys.argv[1:]
    w = int(a[1]) if len(a) > 1 else W_DEF
    h = int(a[2]) if len(a) > 2 else H_DEF
    main(a[0], w, h)
