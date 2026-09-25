#!/usr/bin/env python3
"""Decode the proxy's eye dump with its REAL on-disk layout.

dump_eye_image() copies the whole swapchain texture into a staging texture and
maps one array slice, then writes rows using the *texture* width (sc_width, the
per-eye width) while iterating d.Height rows per slice. The file is therefore

    width  = sc_width          (per-eye width, e.g. 998)
    height = sc_height * N     (slices stacked vertically, e.g. 2148 * 2)

Both slices come from the same staging copy, so for RE6VR_IPD_SCALE=0 the two
halves are expected to be byte-identical; they are still split here so a real
per-eye difference would be visible.

Usage: python scripts/eye_raw.py <eye.bgra> <out_prefix> [per_eye_w] [per_eye_h] [scale]
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


def crop_to_png(data, src_w, sx, sy, cw, ch, out, scale):
    ow, oh = cw // scale, ch // scale
    rows = []
    nonblack = tot = lsum = 0
    for oy in range(oh):
        base = ((sy + oy * scale) * src_w + sx) * 4
        row = bytearray(ow * 3)
        for ox in range(ow):
            o = base + (ox * scale) * 4
            b, g, r = data[o], data[o + 1], data[o + 2]
            row[ox * 3] = r
            row[ox * 3 + 1] = g
            row[ox * 3 + 2] = b
            tot += 1
            if b or g or r:
                nonblack += 1
            lsum += (r * 299 + g * 587 + b * 114) // 1000
        rows.append(bytes(row))
    write_png(out, ow, oh, rows)
    print(f"{out}: {ow}x{oh} from ({sx},{sy}) {cw}x{ch}  "
          f"non-black {100.0*nonblack/tot:.1f}%  luma {lsum/tot:.1f}")


def main(src, prefix, pw=998, ph=2148, scale=4):
    data = open(src, "rb").read()
    total_rows = len(data) // (pw * 4)
    slices = total_rows // ph
    print(f"{src}: {len(data)} bytes -> {pw}x{total_rows} "
          f"= {slices} slice(s) of {pw}x{ph}")
    if slices < 1:
        raise SystemExit("file too small for one slice")
    names = ["L", "R"] if slices >= 2 else ["L"]
    for i, nm in enumerate(names):
        crop_to_png(data, pw, 0, i * ph, pw, ph, f"{prefix}_{nm}.png", scale)
    if slices >= 2:
        a = data[:pw * ph * 4]
        b = data[pw * ph * 4:pw * ph * 8]
        print(f"  slice L == slice R byte-for-byte: {a == b}")


if __name__ == "__main__":
    a = sys.argv[1:]
    args = [int(x) for x in a[2:5]] if len(a) > 2 else []
    main(a[0], a[1], *args)
