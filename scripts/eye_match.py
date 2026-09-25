#!/usr/bin/env python3
"""Normalised cross-correlation of the eye viewport against the game frame.

Answers "is the panel showing the whole frame?" by scaling the eye viewport back
to the frame's aspect and sliding it over the frame. A peak at scale 1.0 offset
(0,0) means the whole picture is on the panel; a peak at some other scale means
the viewport is a crop (zoomed in).

Usage: python scripts/eye_match.py <frame.bgra> <eye.bgra>
       (frame 1280x720, eye 1996x2148 side-by-side or 998x2148 single slice)
"""
import sys


def load(path, w, h):
    d = open(path, "rb").read()
    if len(d) < w * h * 4:
        raise SystemExit(f"{path}: too small ({len(d)}) for {w}x{h}")
    return d


def luma_at(d, w, x, y):
    o = (y * w + x) * 4
    return (d[o + 2] * 299 + d[o + 1] * 587 + d[o] * 114) // 1000


def profile_x(d, w, x0, x1, y0, y1, step=2):
    """Mean luma per column over a row band."""
    prof = []
    rows = list(range(y0, y1, step))
    for x in range(x0, x1):
        s = 0
        for y in rows:
            s += luma_at(d, w, x, y)
        prof.append(s / len(rows))
    return prof


def zncc(a, b):
    n = min(len(a), len(b))
    if n == 0:
        return -2.0
    ma = sum(a[:n]) / n
    mb = sum(b[:n]) / n
    va = sum((v - ma) ** 2 for v in a[:n])
    vb = sum((v - mb) ** 2 for v in b[:n])
    if va <= 0 or vb <= 0:
        return -2.0
    cov = sum((a[i] - ma) * (b[i] - mb) for i in range(n))
    return cov / ((va ** 0.5) * (vb ** 0.5))


def main(frame_path, eye_path):
    FW, FH = 1280, 720
    frame = load(frame_path, FW, FH)

    # Eye buffer: accept either the full side-by-side or a single slice.
    raw = open(eye_path, "rb").read()
    if len(raw) >= 1996 * 2148 * 4:
        EW, EH = 1996, 2148
        eye = raw
    elif len(raw) >= 998 * 2148 * 4:
        EW, EH = 998, 2148
        eye = raw
    else:
        raise SystemExit(f"{eye_path}: unexpected size {len(raw)}")

    # Source band: the title's logo strip (12%..45% down) is the most
    # feature-rich part of this frame, so it correlates well.
    fy0, fy1 = int(FH * 0.12), int(FH * 0.45)
    fprof = profile_x(frame, FW, 0, FW, fy0, fy1)
    fmean = sum(fprof) / len(fprof)

    # Eye band: same relative rows, one viewport wide.
    ew = EW // 2 if EW == 1996 else EW
    ey0, ey1 = int(EH * 0.12), int(EH * 0.45)
    eprof = profile_x(eye, EW, 0, ew, ey0, ey1)
    emean = sum(eprof) / len(eprof)

    print(f"frame {FW}x{FH}: profile mean {fmean:.1f}, min {min(fprof):.0f}, "
          f"max {max(fprof):.0f}")
    print(f"eye   {ew}x{EH} viewport: profile mean {emean:.1f}, "
          f"min {min(eprof):.0f}, max {max(eprof):.0f}")

    # Try each horizontal scale, resampling the frame profile to the viewport
    # width, then best-shift by ZNCC.
    print("  scale  best-x0  best-x1  zncc")
    best = None
    for pct in range(30, 105, 2):
        s = pct / 100.0          # fraction of the frame width mapped to the viewport
        span = int(FW * s)
        # Resample frame columns [x0, x0+span) to len(eprof) samples.
        for x0 in range(0, FW - span + 1, 8):
            res = [fprof[x0 + (i * span) // len(eprof)] for i in range(len(eprof))]
            z = zncc(res, eprof)
            if best is None or z > best[0]:
                best = (z, s, x0, x0 + span)
    z, s, x0, x1 = best
    print(f"  PEAK: viewport shows source columns {x0}..{x1} "
          f"({s*100:.0f}% of width {FW}), zncc={z:.3f}")

    # Overlap test between the two viewports of a side-by-side buffer.
    if EW == 1996:
        left = profile_x(eye, EW, 0, ew, ey0, ey1)
        right = profile_x(eye, EW, ew, EW, ey0, ey1)
        z0 = zncc(left, right)
        print(f"  left vs right viewport, zero shift: zncc={z0:.3f}")
        best = None
        for sh in range(-(ew - 16), ew - 16, 4):
            a = left[max(0, sh):] if sh >= 0 else left[:sh]
            b = right[max(0, -sh):] if sh < 0 else right[:len(a)]
            z = zncc(a, b)
            if best is None or z > best[0]:
                best = (z, sh)
        print(f"  left vs right viewport, best shift {best[1]:+d}px: zncc={best[0]:.3f}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
