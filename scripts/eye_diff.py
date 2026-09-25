#!/usr/bin/env python3
"""Compare the two viewports of a side-by-side eye buffer.

With RE6VR_IPD_SCALE=0 both eyes render through the same eye position, so the
two viewports must contain the *same* picture. Any large per-pixel difference
means the eyes were fed different content, which is what a doubled/diagonal
image looks like in the headset.

Usage: python scripts/eye_diff.py <eye.bgra> [width] [height]
"""
import sys


def main(src, width=1996, height=2148):
    data = open(src, "rb").read()
    if len(data) < width * height * 4:
        raise SystemExit(f"{src}: too small for {width}x{height}")

    half = width // 2
    diff_sum = 0
    diff_max = 0
    n = 0
    big = 0
    # Column-wise mean luma of each half, to see the shapes line up.
    for y in range(0, height, 4):
        base = y * width * 4
        for x in range(0, half, 4):
            o0 = base + x * 4
            o1 = base + (half + x) * 4
            l0 = (data[o0 + 2] * 299 + data[o0 + 1] * 587 + data[o0] * 114) // 1000
            l1 = (data[o1 + 2] * 299 + data[o1 + 1] * 587 + data[o1] * 114) // 1000
            d = abs(l0 - l1)
            diff_sum += d
            if d > diff_max:
                diff_max = d
            if d > 32:
                big += 1
            n += 1

    print(f"{src}: viewport width {half}")
    print(f"  mean |lumaL - lumaR| = {diff_sum/n:.2f}")
    print(f"  max  |lumaL - lumaR| = {diff_max}")
    print(f"  pixels differing by >32 luma: {big}/{n} ({100.0*big/n:.1f}%)")

    # Horizontal offset search: does one half match the other shifted?
    print("  best alignment of left half against right half (coarse search):")
    best = []
    for shift in range(-64, 65, 4):
        s = 0
        c = 0
        for y in range(height // 4, 3 * height // 4, 32):
            base = y * width * 4
            for x in range(64, half - 64, 16):
                xs = x
                xo = half + x + shift
                if xo < half or xo >= width:
                    continue
                o0 = base + xs * 4
                o1 = base + xo * 4
                s += abs(((data[o0 + 2] * 299 + data[o0 + 1] * 587 + data[o0] * 114) // 1000)
                         - ((data[o1 + 2] * 299 + data[o1 + 1] * 587 + data[o1] * 114) // 1000))
                c += 1
        if c:
            best.append((s / c, shift))
    best.sort()
    for v, sh in best[:4]:
        print(f"    shift {sh:+4d}px -> mean diff {v:.2f}")


if __name__ == "__main__":
    a = sys.argv[1:]
    w = int(a[1]) if len(a) > 1 else 1996
    h = int(a[2]) if len(a) > 2 else 2148
    main(a[0], w, h)
