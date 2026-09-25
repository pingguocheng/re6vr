#!/usr/bin/env python3
"""Split a side-by-side eye dump into per-eye PNGs and a stacked preview.

The OpenXR swapchain is created with arraySize = viewCount and the runtime hands
back one image holding both eyes side by side, so the left slice is the left
half of the buffer and the right slice is the right half.

Usage: python scripts/eye_split.py <eye.bgra> <out_prefix> [scale] [width] [height]
"""
import sys
import zlib
import struct


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


def blit(data, src_w, sx, sy, dst_w, dst_h, scale, expect_black):
    """Sample a rect of the source into an RGB row list."""
    rows = []
    stats = [0, 0, 0]  # nonblack, total, luma_sum
    for oy in range(dst_h):
        base = ((sy + oy * scale) * src_w + sx) * 4
        row = bytearray(dst_w * 3)
        for ox in range(dst_w):
            o = base + (ox * scale) * 4
            b, g, r = data[o], data[o + 1], data[o + 2]
            row[ox * 3] = r
            row[ox * 3 + 1] = g
            row[ox * 3 + 2] = b
            stats[1] += 1
            if b or g or r:
                stats[0] += 1
            stats[2] += (r * 299 + g * 587 + b * 114) // 1000
        rows.append(bytes(row))
    return rows, stats


def main(src, prefix, scale=4, width=1996, height=2148):
    data = open(src, "rb").read()
    if len(data) < width * height * 4:
        raise SystemExit(f"{src}: too small for {width}x{height}")

    half = width // 2
    ow, oh = half // scale, height // scale

    rows_l, st_l = blit(data, width, 0, 0, ow, oh, scale, False)
    write_png(f"{prefix}_L.png", ow, oh, rows_l)
    print(f"{prefix}_L.png: {ow}x{oh}, non-black {100.0*st_l[0]/st_l[1]:.1f}%, "
          f"luma {st_l[2]/st_l[1]:.1f}")

    rows_r, st_r = blit(data, width, half, 0, ow, oh, scale, False)
    write_png(f"{prefix}_R.png", ow, oh, rows_r)
    print(f"{prefix}_R.png: {ow}x{oh}, non-black {100.0*st_r[0]/st_r[1]:.1f}%, "
          f"luma {st_r[2]/st_r[1]:.1f}")

    # Stacked preview: left eye on top, right eye below, so a mismatch between
    # the two eyes is visible in a single glance.
    write_png(f"{prefix}_stacked.png", ow, oh * 2, rows_l + rows_r)
    print(f"{prefix}_stacked.png: {ow}x{oh*2}")


if __name__ == "__main__":
    a = sys.argv[1:]
    sc = int(a[2]) if len(a) > 2 else 4
    w = int(a[3]) if len(a) > 3 else 1996
    h = int(a[4]) if len(a) > 4 else 2148
    main(a[0], a[1], sc, w, h)
