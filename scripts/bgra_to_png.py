#!/usr/bin/env python3
"""Convert a raw BGRA dump (as written by the proxy) into a viewable PNG.

Downscales by an integer factor so the result is small enough to look at
directly. The dump is bottom-up-agnostic raw rows, so row order is preserved.
"""
import struct
import sys
import zlib


def write_png(path, width, height, rgb_rows):
    raw = b"".join(b"\x00" + row for row in rgb_rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def convert(src, dst, scale=4):
    data = open(src, "rb").read()
    # The proxy writes fixed 1996x2148 frames.
    width, height = 1996, 2148
    if len(data) < width * height * 4:
        raise SystemExit(f"{src}: too small ({len(data)} bytes) for {width}x{height}")
    ow, oh = width // scale, height // scale
    rows = []
    nonblack = 0
    total = 0
    luma_sum = 0
    for oy in range(oh):
        y = oy * scale
        base = y * width * 4
        row = bytearray(ow * 3)
        for ox in range(ow):
            o = base + (ox * scale) * 4
            b, g, r = data[o], data[o + 1], data[o + 2]
            row[ox * 3] = r
            row[ox * 3 + 1] = g
            row[ox * 3 + 2] = b
            total += 1
            if b or g or r:
                nonblack += 1
            luma_sum += (r * 299 + g * 587 + b * 114) // 1000
        rows.append(bytes(row))
    write_png(dst, ow, oh, rows)
    print(f"{dst}: {ow}x{oh}, non-black {nonblack}/{total} ({100.0 * nonblack / total:.1f}%), "
          f"mean luma {luma_sum / total:.1f}")


if __name__ == "__main__":
    for a in sys.argv[1:]:
        out = a.rsplit(".", 1)[0] + ".png"
        convert(a, out)
