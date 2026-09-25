#!/usr/bin/env python3
"""Print a crop of a raw BGRA dump as coarse colour characters.

Each output character is the average of a block of pixels, so the result is a
faithful (if blocky) picture of the data itself - no PNG encoding, no preview
downscaling and no JPEG re-encoding in between. That matters for judging
whether a doubling is really in the pixels.

Characters: ' ' dark, '.' very dim, '-' dim, '=' mid, '#' bright,
            'R'/'G'/'B'/'Y'/'C'/'M'/'W' when one channel dominates.

Usage: python scripts/eye_ascii.py <dump.bgra> <w> <h> <x> <y> <cw> <ch> [blockw] [blockh]
"""
import sys


def main():
    src = sys.argv[1]
    w, h = int(sys.argv[2]), int(sys.argv[3])
    x0, y0 = int(sys.argv[4]), int(sys.argv[5])
    cw, ch = int(sys.argv[6]), int(sys.argv[7])
    bw = int(sys.argv[8]) if len(sys.argv) > 8 else 8
    bh = int(sys.argv[9]) if len(sys.argv) > 9 else 12

    data = open(src, "rb").read()
    if len(data) < w * h * 4:
        raise SystemExit(f"{src}: {len(data)} bytes < {w*h*4}")
    print(f"{src} crop ({x0},{y0}) {cw}x{ch}, block {bw}x{bh}")

    for by in range(y0, y0 + ch, bh):
        line = []
        for bx in range(x0, x0 + cw, bw):
            r = g = b = 0
            n = 0
            for y in range(by, min(by + bh, y0 + ch), 2):
                if y >= h:
                    continue
                base = (y * w) * 4
                for x in range(bx, min(bx + bw, x0 + cw)):
                    o = base + x * 4
                    b += data[o]
                    g += data[o + 1]
                    r += data[o + 2]
                    n += 1
            if not n:
                line.append(' ')
                continue
            r, g, b = r / n, g / n, b / n
            mx = max(r, g, b)
            if mx < 8:
                line.append(' ')
            elif mx < 18:
                line.append('.')
            elif mx < 40:
                line.append('-')
            elif mx < 90:
                line.append('=')
            elif max(r, g, b) - min(r, g, b) < 30:
                line.append('W' if mx > 150 else '#')
            else:
                # dominant channel(s)
                if r > g * 1.5 and r > b * 1.5:
                    line.append('R')
                elif g > r * 1.5 and g > b * 1.5:
                    line.append('G')
                elif b > r * 1.5 and b > g * 1.5:
                    line.append('B')
                elif r > b * 1.3 and g > b * 1.3:
                    line.append('Y')
                elif g > r * 1.3 and b > r * 1.3:
                    line.append('C')
                elif r > g * 1.3 and b > g * 1.3:
                    line.append('M')
                else:
                    line.append('+')
        print("  " + "".join(line))


if __name__ == "__main__":
    main()
