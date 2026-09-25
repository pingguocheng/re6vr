#!/usr/bin/env python3
"""Crop a region out of a raw BGRA dump and write it as a PNG, 1:1 or magnified.

Usage: python scripts/eye_crop.py <dump.bgra> <out.png> <w> <h> <x> <y> <cw> <ch> [scale]
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
    src, out = sys.argv[1], sys.argv[2]
    w, h = int(sys.argv[3]), int(sys.argv[4])
    x0, y0 = int(sys.argv[5]), int(sys.argv[6])
    cw, ch = int(sys.argv[7]), int(sys.argv[8])
    scale = int(sys.argv[9]) if len(sys.argv) > 9 else 1

    data = open(src, "rb").read()
    ow, oh = cw * scale, ch * scale
    rows = []
    for oy in range(oh):
        sy = y0 + oy // scale
        row = bytearray(ow * 3)
        for ox in range(ow):
            sx = x0 + ox // scale
            o = ((sy * w) + sx) * 4
            row[ox * 3] = data[o + 2]
            row[ox * 3 + 1] = data[o + 1]
            row[ox * 3 + 2] = data[o]
        rows.append(bytes(row))
    write_png(out, ow, oh, rows)
    print(f"{out}: {ow}x{oh} = ({x0},{y0}) {cw}x{ch} at {scale}x")


if __name__ == "__main__":
    main()
