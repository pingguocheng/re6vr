#!/usr/bin/env python3
"""Compare two raw eye dumps byte for byte, and locate where they differ.

Two 25 MB files cannot be compared convincingly by eye, and "they look the
same" is exactly the kind of judgement that has been wrong before in this
project. This reports the exact number of differing bytes, where the first
difference is, and - for the rows that do differ - the horizontal shift that
best aligns them.

Usage: python scripts/eye_compare.py <a.bgra> <b.bgra> [width] [height]
"""
import sys


def main():
    a_path, b_path = sys.argv[1], sys.argv[2]
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 0

    a = open(a_path, "rb").read()
    b = open(b_path, "rb").read()
    print(f"{a_path}: {len(a)} bytes")
    print(f"{b_path}: {len(b)} bytes")
    if len(a) != len(b):
        print("  sizes differ")
        return
    if w and h and w * h * 4 != len(a):
        print(f"  note: {w}x{h}x4 = {w*h*4} does not match the file size")

    diff = 0
    first = None
    step = 1
    for i in range(0, len(a), step):
        if a[i] != b[i]:
            diff += 1
            if first is None:
                first = i
    print(f"  differing bytes: {diff} of {len(a)} ({100.0*diff/len(a):.2f}%)")
    if first is None:
        print("  VERDICT: the two eye images are byte-for-byte identical")
        return
    row_bytes = w * 4 if w else None
    if row_bytes:
        print(f"  first difference at byte {first} -> x={first % row_bytes // 4}, "
              f"y={first // row_bytes}")
    print("  VERDICT: the two eye images differ")
    if not (w and h):
        return

    rows = [y for y in range(0, h, max(1, h // 8))]
    print("  per-row mean |dL| and best horizontal shift for a few rows:")
    for y in rows:
        base = (y * w) * 4
        best = None
        for shift in range(-w // 4, w // 4, 4):
            tot = 0
            n = 0
            for x in range(w // 8, 7 * w // 8, 8):
                xs = x + shift
                if not (0 <= xs < w):
                    continue
                o1 = base + x * 4
                o2 = base + xs * 4
                l1 = (a[o1 + 2] * 299 + a[o1 + 1] * 587 + a[o1] * 114) // 1000
                l2 = (b[o2 + 2] * 299 + b[o2 + 1] * 587 + b[o2] * 114) // 1000
                tot += abs(l1 - l2)
                n += 1
            if n:
                d = tot / n
                if best is None or d < best[0]:
                    best = (d, shift)
        z = 0
        zn = 0
        for x in range(w // 8, 7 * w // 8, 8):
            o = base + x * 4
            l1 = (a[o + 2] * 299 + a[o + 1] * 587 + a[o] * 114) // 1000
            l2 = (b[o + 2] * 299 + b[o + 1] * 587 + b[o] * 114) // 1000
            z += abs(l1 - l2)
            zn += 1
        print(f"    row {y:5d}: zero-shift mean {z/zn:7.2f} -> best shift {best[1]:+5d}px "
              f"(mean {best[0]:7.2f})")


if __name__ == "__main__":
    main()
