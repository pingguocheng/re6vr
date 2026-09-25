#!/usr/bin/env python3
"""Look for a horizontally duplicated image inside one game frame.

The headset showed the Capcom logo as "capcapcom", i.e. every feature present twice
with a small horizontal gap. If the game draws its scene twice per frame into the
same target, that duplication is in the source frame and can be measured directly:
correlating a frame with itself shifted horizontally peaks at the duplication offset.

Usage: python scripts/frame_dup.py <frame.bgra> [width] [height]
"""
import sys

LUMA = lambda b, g, r: (r * 299 + g * 587 + b * 114) // 1000


def main(src, w=1280, h=720):
    data = open(src, "rb").read()
    if len(data) < w * h * 4:
        raise SystemExit(f"{src}: too small for {w}x{h}")

    # One row band from the middle, where a title logo would sit.
    y0, y1 = h // 4, h // 2
    rows = range(y0, y1, 4)
    prof = []
    for x in range(w):
        s = 0
        for y in rows:
            o = (y * w + x) * 4
            s += LUMA(data[o], data[o + 1], data[o + 2])
        prof.append(s / len(list(rows)))

    mean = sum(prof) / len(prof)
    var = sum((v - mean) ** 2 for v in prof)
    print(f"{src}: {w}x{h}, band rows {y0}..{y1}, column luma mean {mean:.2f}, "
          f"std {(var/len(prof))**0.5:.2f}")
    if var <= 0:
        print("  flat band - nothing to correlate")
        return

    # Normalised autocorrelation over a range of shifts.
    best = []
    for shift in range(2, w // 2):
        n = w - shift
        cov = sum((prof[i] - mean) * (prof[i + shift] - mean) for i in range(n))
        best.append((cov / var, shift))
    best.sort(reverse=True)
    print("  strongest self-similarity at shift:")
    for z, s in best[:6]:
        print(f"    shift {s:>4}px  normalised correlation {z:+.3f}")

    # A duplicated image shows up as a secondary peak, well below the trivial
    # self-correlation at shift 0 but clearly above the noise floor.
    peak = best[0]
    floor = sum(z for z, _ in best[20:120]) / 100 if len(best) > 120 else 0.0
    print(f"  noise floor (average over shifts 20..120 of the ranking): {floor:+.3f}")
    if peak[0] > 0.30 and peak[1] < w // 3:
        print(f"  -> POSSIBLE duplication at ~{peak[1]}px")
    else:
        print("  -> no strong horizontal duplication found in this frame")


if __name__ == "__main__":
    a = sys.argv[1:]
    w = int(a[1]) if len(a) > 1 else 1280
    h = int(a[2]) if len(a) > 2 else 720
    main(a[0], w, h)
