#!/usr/bin/env python3
"""Stereo-render investigation: step 23 - callers of the sRender vtable methods.

sRender's vtable is 0x016E5E18 (MEASURED: slot 3 = the ctor 0x00F3F0B0 that registers its 42
fields, slot 0 = 0x00F41F20). Listing every real `call` to each of its non-shared slots shows
which engine function drives the renderer - that function is the frame.
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
VT = 0x016E5E18


def main() -> int:
    img = pe.Image.load(IMG)
    methods = []
    for slot in range(0, 20):
        v = img.read_u32_rva(VT + slot * 4 - img.image_base)
        if v is None or not (0x401000 <= v < 0x1500000):
            break
        methods.append((slot, v))
    print("sRender vtable methods: %s" % ", ".join("%d:0x%08X" % m for m in methods))

    s = [x for x in img.sections if x.name == ".text"][0]
    blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
    base = img.image_base + s.va

    want = {v: slot for slot, v in methods}
    # every E8 rel32 whose target is one of the methods
    sites = {}
    for j in range(len(blob) - 5):
        if blob[j] != 0xE8:
            continue
        rel = struct.unpack_from("<i", blob, j + 1)[0]
        tgt = base + j + 5 + rel
        if tgt in want:
            sites.setdefault(tgt, []).append(base + j)
    for slot, v in methods:
        if v not in sites:
            continue
        print()
        print("== slot %d -> 0x%08X : %d direct caller(s) ==" % (slot, v, len(sites[v])))
        for c in sites[v][:20]:
            print("   call at %08X" % c)
            for xva, text, raw in sr_dis.disasm_range(img, c - 14, 10):
                print("        %08X  %s" % (xva, text))
    # unique caller sites per method is what matters; also report methods with none
    missing = [("%d" % slot, "0x%08X" % v) for slot, v in methods if v not in sites]
    print()
    print("methods with no direct caller: %s" % missing)
    return 0


if __name__ == "__main__":
    sys.exit(main())
