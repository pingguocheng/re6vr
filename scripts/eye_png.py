#!/usr/bin/env python3
"""Convert the proxy's eye/frame BGRA dumps into PNGs *inside the workspace*.

The game directory is outside the this workspace, so the tool may read from it
but not write there. Output goes next to this project instead.

Usage:  python scripts/eye_png.py <src.bgra> <out.png> [scale] [width] [height]
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


def convert(src, dst, scale=4, width=1996, height=2148):
    data = open(src, "rb").read()
    need = width * height * 4
    if len(data) < need:
        raise SystemExit(f"{src}: too small ({len(data)} bytes) for {width}x{height}")
    ow, oh = width // scale, height // scale
    rows = []
    nonblack = 0
    total = 0
    luma_sum = 0
    for oy in range(oh):
        base = (oy * scale) * width * 4
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
    print(f"{dst}: {ow}x{oh}, non-black {nonblack}/{total} "
          f"({100.0 * nonblack / total:.1f}%), mean luma {luma_sum / total:.1f}")


if __name__ == "__main__":
    src = sys.argv[1]
    dst = sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    w = int(sys.argv[4]) if len(sys.argv) > 4 else 1996
    h = int(sys.argv[5]) if len(sys.argv) > 5 else 2148
    convert(src, dst, scale, w, h)
