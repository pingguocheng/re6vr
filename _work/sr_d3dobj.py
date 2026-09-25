#!/usr/bin/env python3
"""Stereo-render investigation: step 21 - `mov reg,[reg+0x100..0x10C]` then indirect call.

sRender (instance 0x186E8BC, vtable 0x016E5E18) keeps its D3D objects at +0x100/+0x104/+0x108
(MEASURED: `mov eax,[ebx+0x108]` = IDirect3D9* at 0x00F42781, and `mov eax,[ecx+0x100]` /
`mov ecx,[eax]; mov edx,[ecx+0x18]` = a COM call on the object at +0x100 at 0x00F3D50D).
A call on one of those slots is a d3d9 call; the vtable displacement then names the method.
"""
from __future__ import annotations

import os
import re
import struct
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "scripts"))

import sr_dis  # noqa: E402
from disasm_lib import pe  # noqa: E402

IMG = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"


def main() -> int:
    img = pe.Image.load(IMG)
    s = [x for x in img.sections if x.name == ".text"][0]
    blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
    base = img.image_base + s.va
    offsets = [0x100, 0x104, 0x108, 0x10C, 0x110]
    print("== sites loading a d3d object from [reg+0x10?] ==")
    sites = []
    for j in range(len(blob) - 6):
        if blob[j] != 0x8B:
            continue
        b1 = blob[j + 1]
        if (b1 & 0xC0) != 0x80 or (b1 & 7) == 4:
            continue
        disp = struct.unpack_from("<i", blob, j + 2)[0]
        if disp in offsets:
            sites.append((base + j, disp))
    print("  %d site(s)" % len(sites))
    cnt = Counter(d for _, d in sites)
    print("  by offset: %s" % dict(cnt))
    for va, disp in sites:
        st = None
        for back in range(0, 5):
            seq = sr_dis.disasm_range(img, va - back, 4)
            if any(x[0] == va for x in seq):
                st = va - back
                break
        if st is None:
            continue
        print("  --- %08X  (from [reg+0x%X]) ---" % (va, disp))
        for xva, text, raw in sr_dis.disasm_range(img, st - 2, 12):
            print("      %08X  %s" % (xva, text))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
