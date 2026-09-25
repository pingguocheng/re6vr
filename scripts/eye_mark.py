#!/usr/bin/env python3
"""Draw guide lines at the suspected tile seams of an eye dump.

Used to settle whether the picture inside one eye's texture is repeated: the
seams guessed from the screenshot are marked, so the output either lines up
with them (the picture really does repeat) or does not.

Usage: python scripts/eye_mark.py <eye.bgra> <out.png> <width> <height> [x ...]
"""
import struct
import sys
import zlib


def write_png(path, width, height, rgb_rows):
    raw = b"".join(b"\x00" + r for r in rgb_rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    src = sys.argv[1]
    out = sys.argv[2]
    w = int(sys.argv[3])
    h = int(sys.argv[4])
    marks = [int(v) for v in sys.argv[5:]]

    data = open(src, "rb").read()
    if len(data) < w * h * 4:
        raise SystemExit(f"{src}: {len(data)} bytes < {w*h*4}")

    scale = 2
    ow, oh = w // scale, h // scale
    rows = []
    for oy in range(oh):
        base = (oy * scale * w) * 4
        row = bytearray(ow * 3)
        for ox in range(ow):
            o = base + ox * scale * 4
            r, g, b = data[o + 2], data[o + 1], data[o]
            # red line where a seam is suspected, green every 100 px otherwise
            col = ox * scale
            if any(abs(col - m) <= scale for m in marks):
                r, g, b = 255, 0, 0
            row[ox * 3] = r
            row[ox * 3 + 1] = g
            row[ox * 3 + 2] = b
        rows.append(bytes(row))
    write_png(out, ow, oh, rows)
    print(f"{out}: {ow}x{oh}, red lines at x = {marks}")


if __name__ == "__main__":
    main()
