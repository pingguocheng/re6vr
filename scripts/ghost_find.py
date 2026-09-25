#!/usr/bin/env python3
"""Find a duplicated (ghosted) copy of an image inside itself.

A doubled image - "capcapcom" instead of "capcom" - is one picture plus a copy of
itself at a fixed horizontal offset. Autocorrelation finds it as a *local maximum*
away from zero: the trivial peak at shift 0 and the smooth falloff next to it are
not evidence, so what matters is whether any distant shift stands clearly above its
own neighbourhood.

An earlier version of this just took the global maximum and reported "possible
duplication at 2px" for every smooth image, which is useless. Reporting the profile
and looking for a genuine isolated bump is the whole point.

Usage: python scripts/ghost_find.py <image.bgra> [width] [height] [band_lo] [band_hi]
"""
import sys


def column_profile(data, w, h, y0, y1):
    rows = list(range(y0, y1, 2))
    prof = []
    for x in range(w):
        s = 0
        for y in rows:
            o = (y * w + x) * 4
            s += (data[o + 2] * 299 + data[o + 1] * 587 + data[o] * 114) // 1000
        prof.append(s / len(rows))
    return prof


def main(src, w=1280, h=720, y0=None, y1=None):
    data = open(src, "rb").read()
    if len(data) < w * h * 4:
        raise SystemExit(f"{src}: too small ({len(data)}) for {w}x{h}")
    if y0 is None:
        y0, y1 = h // 4, h * 3 // 4

    prof = column_profile(data, w, h, y0, y1)
    n = len(prof)
    mean = sum(prof) / n
    var = sum((v - mean) ** 2 for v in prof)
    if var <= 0:
        print(f"{src}: flat image, nothing to analyse")
        return
    print(f"{src}: {w}x{h}, rows {y0}..{y1}, luma mean {mean:.2f}, "
          f"std {(var/n)**0.5:.2f}")

    # Normalised autocorrelation. The denominator uses the full variance so the
    # curve is comparable across shifts (fewer overlapping samples at large shifts
    # would otherwise inflate it).
    corr = []
    for s in range(1, n // 2):
        cov = sum((prof[i] - mean) * (prof[i + s] - mean) for i in range(n - s))
        corr.append(cov / var)

    def at(s):
        return corr[s - 1] if 1 <= s < n // 2 else 0.0

    # Print a coarse profile so the shape is visible, not just a verdict.
    step = max(1, (n // 2) // 12)
    print("  correlation profile:")
    for s in range(step, n // 2, step):
        bar = "#" * max(0, int(at(s) * 40))
        print(f"    shift {s:>4}px  {at(s):+.3f}  {bar}")

    # A real ghost is a local maximum that stands above its own surroundings on both
    # sides. The monotone shoulder next to shift 0 is not a ghost.
    W = 8       # neighbourhood half-width for the local-maximum test
    best = None
    for s in range(3 * W, n // 3):
        v = at(s)
        left = max(at(s - k) for k in range(1, W + 1))
        right = max(at(s + k) for k in range(1, W + 1))
        if v > left and v > right:
            prominence = v - max(left, right)
            if best is None or prominence > best[0]:
                best = (prominence, s, v)

    if best is None:
        print("  no local maximum found away from zero")
        print("  -> NO duplication detected")
        return

    prominence, s, v = best
    print(f"  strongest local maximum: shift {s}px, correlation {v:+.3f}, "
          f"prominence {prominence:+.3f}")
    if prominence > 0.05 and v > 0.15:
        print(f"  -> DUPLICATION at ~{s}px (value {v:+.3f}, clearly above its "
              f"neighbourhood)")
    else:
        print("  -> no duplication: the largest bump is within the noise of a "
              "smooth image")


if __name__ == "__main__":
    a = sys.argv[1:]
    args = [int(x) for x in a[1:5]]
    main(a[0], *args)
