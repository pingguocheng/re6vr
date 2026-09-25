#!/usr/bin/env python3
"""Find red-dominant columns in a rendered eye image.

The stripe ruler burns ONE red stripe into the panel at a known texel. Finding
where (and whether) red actually appears in the rendered eye texture says
whether the test pattern reached the screen and where the picture is being
sampled - which a greyscale look at the image cannot answer.

Usage: python scripts/eye_red.py <eye.bgra> <width> <height> [row]
"""
import sys


def main():
    src = sys.argv[1]
    w = int(sys.argv[2])
    h = int(sys.argv[3])
    data = open(src, "rb").read()
    if len(data) < w * h * 4:
        raise SystemExit(f"{src}: {len(data)} bytes < {w*h*4}")

    rows = [int(sys.argv[4])] if len(sys.argv) > 4 else list(range(0, h, max(1, h // 12)))
    print(f"{src} ({w}x{h}): scanning {len(rows)} row(s)")
    for y in rows:
        base = (y * w) * 4
        red_cols = []
        white_cols = []
        for x in range(w):
            o = base + x * 4
            b, g, r = data[o], data[o + 1], data[o + 2]
            if r > 90 and r > g * 2 and r > b * 2:
                red_cols.append(x)
            elif r > 150 and g > 150 and b > 150:
                white_cols.append(x)
        def spans(cols):
            out = []
            for c in cols:
                if out and c - out[-1][1] <= 2:
                    out[-1][1] = c
                else:
                    out.append([c, c])
            return out
        rs = spans(red_cols)
        ws = spans(white_cols)
        print(f"  row {y:5d}: red runs {rs if rs else 'NONE'}   "
              f"white runs {len(ws)} {ws[:6]}")


if __name__ == "__main__":
    main()
