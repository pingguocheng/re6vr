#!/usr/bin/env python3
"""Stereo-render investigation: step 28 - call graph over a range.

Every `call rel32` inside [lo,hi) with its target, in address order - the phase skeleton of a
function without decoding every instruction. Also resolves calls to the sBioCamera-derived
vtable methods (vtable 0x151A394) and to the sRender vtable (0x016E5E18).
"""
from __future__ import annotations

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "scripts"))

from disasm_lib import pe  # noqa: E402

IMG = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
KNOWN = {}


def build_known(img):
    for vt, name in ((0x0151A394, "sBioCamera-derived"), (0x016E5E18, "sRender"),
                     (0x0151A380, "sBioCamera"), (0x0152D620, "uCameraCtrl"),
                     (0x0152C3C0, "uCamera")):
        for slot in range(0, 40):
            v = img.read_u32_rva(vt + slot * 4 - img.image_base)
            if v is None or not (0x401000 <= v < 0x1500000):
                break
            KNOWN.setdefault(v, "%s slot %d" % (name, slot))


def main() -> int:
    img = pe.Image.load(IMG)
    build_known(img)
    lo = int(sys.argv[1], 0)
    hi = int(sys.argv[2], 0)
    s = [x for x in img.sections if x.name == ".text"][0]
    blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
    base = img.image_base + s.va
    for va in range(lo, hi):
        k = va - base
        if blob[k] in (0xE8, 0xE9):
            rel = struct.unpack_from("<i", blob, k + 1)[0]
            tgt = va + 5 + rel
            note = KNOWN.get(tgt, "")
            print("%08X  %s 0x%08X  %s" % (va, "call" if blob[k] == 0xE8 else "jmp ", tgt, note))
    return 0


if __name__ == "__main__":
    sys.exit(main())
