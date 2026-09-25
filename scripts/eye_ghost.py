#!/usr/bin/env python3
"""Find a horizontal duplicate (ghost) inside one eye image.

A "capcapcom" style ghost is the row repeating itself at a small horizontal
lag. For each candidate lag this computes the correlation of the row with
itself shifted, and reports the lag whose correlation stands out above its
neighbours (a local maximum). A picture that is merely smooth has a broad
rise with no single lag standing out, so the peak's prominence is what counts.

Usage: python scripts/eye_ghost.py <dump.bgra> <width> <height> [row0] [row1]
"""
import sys


def main():
    src = sys.argv[1]
    w = int(sys.argv[2])
    h = int(sys.argv[3])
    row0 = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    row1 = int(sys.argv[5]) if len(sys.argv) > 5 else h

    data = open(src, "rb").read()
    if len(data) < w * h * 4:
        raise SystemExit(f"{src}: {len(data)} bytes < {w*h*4}")

    y_step = 2
    x_step = 2
    lo, hi = 100, w - 100
    lags = list(range(4, 400, 2))
    scores = {}

    for lag in lags:
        tot = 0
        n = 0
        for y in range(row0, row1, y_step):
            base = (y * w) * 4
            for x in range(lo, hi - lag, x_step):
                o1 = base + x * 4
                o2 = base + (x + lag) * 4
                l1 = (data[o1 + 2] * 299 + data[o1 + 1] * 587 + data[o1] * 114) // 1000
                l2 = (data[o2 + 2] * 299 + data[o2 + 1] * 587 + data[o2] * 114) // 1000
                tot += abs(l1 - l2)
                n += 1
        scores[lag] = tot / n if n else 1e9

    best = min(scores, key=lambda k: scores[k])
    # Prominence of the best lag against its neighbours 20 px either side.
    nb = [scores[k] for k in scores if 20 <= abs(k - best) <= 60]
    prom = (sum(nb) / len(nb) - scores[best]) if nb else 0.0

    print(f"{src} ({w}x{h}), rows {row0}..{row1}")
    print(f"  lowest mean |dL| at lag {best}px -> {scores[best]:.3f}")
    print(f"  neighbourhood mean {sum(nb)/len(nb):.3f}, "
          f"prominence {prom:+.3f} "
          f"({100.0*prom/(scores[best] or 1e-9):+.1f}% of the minimum)")
    ranked = sorted(scores.items(), key=lambda kv: kv[1])[:6]
    print("  best lags:")
    for lag, d in ranked:
        print(f"    lag {lag:4d}px -> {d:7.3f}")

    # Row-to-row band with the most detail, so the report is not dominated by black.
    band = []
    for y in range(0, h, 16):
        base = (y * w) * 4
        s = 0
        for x in range(lo, hi, 8):
            o = base + x * 4
            s += (data[o + 2] * 299 + data[o + 1] * 587 + data[o] * 114) // 1000
        band.append((s, y))
    band.sort(reverse=True)
    print(f"  brightest sampled rows: {[y for _, y in band[:6]]}")


if __name__ == "__main__":
    main()
