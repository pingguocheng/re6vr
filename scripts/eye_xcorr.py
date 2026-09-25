#!/usr/bin/env python3
"""Cross-correlate the two viewports of a side-by-side eye dump.

If both eyes render the same panel through the same frustum they must be
identical. If instead they sample different windows of the source, some
horizontal shift will align them. The best shift and its residual decide which.

Usage: python scripts/eye_xcorr.py <eye.bgra> [width] [height]
"""
import sys


def col_profile(data, width, height, x0, x1):
    """Per-column mean luma over the upper 45% of rows (the logo band)."""
    prof = []
    y0, y1 = int(height * 0.12), int(height * 0.45)
    for x in range(x0, x1):
        s = 0
        for y in range(y0, y1, 4):
            o = (y * width + x) * 4
            s += (data[o + 2] * 299 + data[o + 1] * 587 + data[o] * 114) // 1000
        prof.append(s / len(range(y0, y1, 4)))
    return prof


def main(src, width=1996, height=2148):
    data = open(src, "rb").read()
    half = width // 2
    if len(data) < width * height * 4:
        raise SystemExit(f"{src}: too small for {width}x{height}")

    a = col_profile(data, width, height, 0, half)
    b = col_profile(data, width, height, half, width)

    # Self-similarity baseline: how well does each profile match itself?
    scale = sum(abs(v) for v in a) / len(a) or 1.0

    print(f"{src}: viewport {half}px, profile mean magnitudes "
          f"L={sum(a)/len(a):.1f} R={sum(b)/len(b):.1f}")
    print("  cross-correlation of L against R (R shifted by s samples):")
    best = None
    results = []
    for s in range(-half + 8, half - 8, 2):
        tot = 0
        n = 0
        for i in range(32, half - 32, 3):
            j = i + s
            if 0 <= j < half:
                tot += abs(a[i] - b[j])
                n += 1
        if n < 100:
            continue
        d = tot / n
        results.append((d, s))
        if best is None or d < best[0]:
            best = (d, s)

    results.sort()
    for d, s in results[:5]:
        print(f"    shift {s:+5d}px -> mean |dL| = {d:6.2f}  "
              f"({100.0*d/scale:5.1f}% of profile magnitude)")

    # Zero-shift sanity: are they just *different*?
    zero = [d for d, s in results if s == 0]
    if zero:
        print(f"  zero shift  -> mean |dL| = {zero[0]:6.2f} "
              f"({100.0*zero[0]/scale:5.1f}% of profile magnitude)")

    d, s = best
    if s == 0 and d < 0.25 * scale:
        print("  VERDICT: viewports are (near) identical - layout is consistent")
    elif d < 0.35 * scale:
        print(f"  VERDICT: right viewport is the LEFT one shifted {s:+d}px - "
              f"same window, offset placement")
    else:
        print("  VERDICT: no shift aligns them; the two viewports show "
              "genuinely different content")


if __name__ == "__main__":
    a = sys.argv[1:]
    w = int(a[1]) if len(a) > 1 else 1996
    h = int(a[2]) if len(a) > 2 else 2148
    main(a[0], w, h)
