#!/usr/bin/env python3
"""Stereo-render investigation: step 19 - who consumes mViewportCamera / how many views.

Two independent signals for "the renderer walks the view table":

  A. `add reg,0x60` (83 C0+r 60 / 81 C0+r 60 00 00 00) - the stride the view entry is advanced
     by in sBioCamera::Update. A second walker of the same table uses the same stride.
  B. immediates 0x60 used in an imul/lea with a viewport-shaped base, plus the displacement
     range 0x12A0..0x1620 (the mViewportCamera region inside sBioCamera).

Everything is decoded from the image (not the cache) because the cache has holes over the
renderer, and every candidate is printed with a window.
"""
from __future__ import annotations

import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "scripts"))

import sr_dis  # noqa: E402
from disasm_lib import pe  # noqa: E402

IMG = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"


def aligned_start(img, va, maxback=6):
    for back in range(0, maxback):
        seq = sr_dis.disasm_range(img, va - back, 5)
        if any(x[0] == va for x in seq):
            return va - back
    return None


def main() -> int:
    img = pe.Image.load(IMG)
    s = [x for x in img.sections if x.name == ".text"][0]
    blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
    base = img.image_base + s.va

    print("== A: `add reg,0x60` sites (view-table stride) ==")
    n_a = 0
    for j in range(len(blob) - 4):
        b0, b1, b2 = blob[j], blob[j + 1], blob[j + 2]
        ok = False
        if b0 == 0x83 and 0xC0 <= b1 <= 0xC7 and b2 == 0x60:
            ok = True
        elif b0 == 0x81 and 0xC0 <= b1 <= 0xC7:
            if struct.unpack_from("<I", blob, j + 2)[0] == 0x60:
                ok = True
        if not ok:
            continue
        va = base + j
        st = aligned_start(img, va)
        if st is None:
            continue
        n_a += 1
        print("  --- %08X ---" % va)
        for xva, text, raw in sr_dis.disasm_range(img, st - 12, 22):
            print("      %08X  %s" % (xva, text))
        print()
    print("  total %d aligned sites" % n_a)

    print()
    print("== B: explicit displacements into the mViewportCamera region ==")
    lo, hi = 0x12A0, 0x1620
    n_b = 0
    for j in range(len(blob) - 6):
        if blob[j] not in (0x8B, 0x89, 0x8D, 0xF3, 0x0F, 0xD9, 0xDD, 0xF2):
            continue
        # look for a disp32 in range anywhere in the next 5 bytes
        for k in range(j + 1, min(j + 6, len(blob) - 4)):
            disp = struct.unpack_from("<i", blob, k)[0]
            if lo <= disp < hi:
                va = base + j
                st = aligned_start(img, va)
                if st is None:
                    continue
                n_b += 1
                print("  --- %08X disp 0x%X ---" % (va, disp))
                for xva, text, raw in sr_dis.disasm_range(img, st - 8, 14):
                    print("      %08X  %s" % (xva, text))
                print()
                break
    print("  total %d (unvalidated) sites" % n_b)
    return 0


if __name__ == "__main__":
    sys.exit(main())
