#!/usr/bin/env python3
"""Show the two eyes side by side, the way the headset pairs them.

Two dump layouts exist in this project and both need to work here:

  * gameplay eye dump (`re6vr_eye_left.bgra`) - one file holding both slices
    STACKED vertically (see eye_raw.py), e.g. 998 x 4296;
  * offline self-test (`re6vr_real_eye0.bgra` / `eye1.bgra`) - one file per eye.

The stacked layout is the opposite of how anyone wants to look at it: a doubling or
a mismatch between the eyes is a *horizontal* judgement ("does the left half line up
with the right half"), so the two views have to sit left and right to be judged.

Usage:
  python scripts/eye_sbs.py <stacked.bgra> <out.png> [w] [h] [scale]
  python scripts/eye_sbs.py <eye0.bgra> <eye1.bgra> <out.png> [w] [h] [scale]
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


def row_of(data, row_start, pw, scale, w):
    out = bytearray(w * 3)
    for x in range(w):
        o = row_start + (x * scale) * 4
        b, g, r = data[o], data[o + 1], data[o + 2]
        out[x * 3] = r
        out[x * 3 + 1] = g
        out[x * 3 + 2] = b
    return bytes(out)


def compose(left_bytes, left_off, right_bytes, right_off, pw, ph, scale, dst):
    out_w, out_h = pw // scale, ph // scale
    stripe = 3   # separator, so the two views cannot read as one wide image
    rows = []
    for oy in range(out_h):
        y = oy * scale
        lrow = row_of(left_bytes, (left_off + y * pw) * 4, pw, scale, out_w)
        rrow = row_of(right_bytes, (right_off + y * pw) * 4, pw, scale, out_w)
        rows.append(lrow + b"\x30\x30\x30" * stripe + rrow)
    write_png(dst, out_w * 2 + stripe, out_h, rows)
    print(f"{dst}: {out_w*2+stripe}x{out_h} (left | right, {scale}x downscale)")


def main(argv):
    paths = []
    nums = []
    for a in argv:
        try:
            nums.append(int(a))
        except ValueError:
            paths.append(a)
    # Trailing integers are w/h/scale; default them if absent.
    if len(paths) == 2:          # stacked single file + out.png
        src, dst = paths
        pw, ph, scale = (nums + [998, 2148, 4])[:3]
        data = open(src, "rb").read()
        if len(data) < pw * ph * 4 * 2:
            raise SystemExit(f"{src}: needs two stacked slices of {pw}x{ph}, "
                             f"got {len(data)} bytes")
        compose(data, 0, data, ph, pw, ph, scale, dst)
    elif len(paths) == 3:        # eye0, eye1, out.png
        s0, s1, dst = paths
        pw, ph, scale = (nums + [998, 2148, 4])[:3]
        d0 = open(s0, "rb").read()
        d1 = open(s1, "rb").read()
        need = pw * ph * 4
        if len(d0) < need or len(d1) < need:
            raise SystemExit(f"{s0}/{s1}: smaller than one {pw}x{ph} slice")
        compose(d0, 0, d1, 0, pw, ph, scale, dst)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
