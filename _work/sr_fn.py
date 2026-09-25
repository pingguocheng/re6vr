#!/usr/bin/env python3
"""Stereo-render investigation: step 25 - map the function containing the Present call.

Finds the enclosing function boundaries around a VA by scanning outward for prologue/align
patterns, then lists every `call rel32` whose target is inside that function - i.e. its callers.
"""
from __future__ import annotations

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "scripts"))

import sr_dis  # noqa: E402
from disasm_lib import pe  # noqa: E402

IMG = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"


def main() -> int:
    img = pe.Image.load(IMG)
    target = int(sys.argv[1], 0)
    s = [x for x in img.sections if x.name == ".text"][0]
    blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
    base = img.image_base + s.va

    # walk backwards for int3 padding (function end of the previous one) and forwards for the
    # ret/int3 pair that ends this one
    j = target - base
    start = None
    while j > 0:
        if blob[j - 1] == 0xCC and (j < 2 or blob[j - 2] == 0xCC):
            start = base + j
            break
        j -= 1
    print("function start (after int3 padding): 0x%08X" % (start if start else -1))
    if start:
        for xva, text, raw in sr_dis.disasm_range(img, start, 14):
            print("   %08X  %s" % (xva, text))

    # find the end: next run of >=2 int3 after target
    j = target - base
    end = None
    while j < len(blob) - 4:
        if blob[j] == 0xCC and blob[j + 1] == 0xCC:
            end = base + j
            break
        j += 1
    print("next int3 padding at 0x%08X" % (end if end else -1))
    if end:
        for xva, text, raw in sr_dis.disasm_range(img, end - 32, 10):
            print("   %08X  %s" % (xva, text))

    # callers: any E8 whose target lies in [start,end)
    lo = start if start else target - 0x2000
    hi = end if end else target + 0x2000
    print()
    print("== call sites landing inside 0x%08X..0x%08X ==" % (lo, hi))
    seen = []
    for k in range(len(blob) - 5):
        if blob[k] != 0xE8:
            continue
        rel = struct.unpack_from("<i", blob, k + 1)[0]
        tgt = base + k + 5 + rel
        if lo <= tgt < hi:
            seen.append((base + k, tgt))
    for c, t in seen[:40]:
        print("   %08X -> %08X" % (c, t))
    print("   %d site(s)" % len(seen))
    return 0


if __name__ == "__main__":
    sys.exit(main())
