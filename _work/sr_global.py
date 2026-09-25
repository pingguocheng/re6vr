#!/usr/bin/env python3
"""Stereo-render investigation: step 15 - raw dword refs to the render singleton, with context.

`mov ecx, 0x186E8BC` / `mov eax, ds:[0x186E8BC]` are absolute 32-bit displacements, so the
4-byte little-endian value appears literally in .text. This finds every such site and prints a
window around it decoded from the image (not from the cache, whose holes cover the renderer).
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
    targets = [int(a, 0) for a in sys.argv[1:]] or [0x186E8BC]
    before = 12
    after = 14
    for tva in targets:
        pat = struct.pack("<I", tva)
        hits = []
        for s in img.sections:
            if not s.is_executable:
                continue
            blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
            pos = 0
            while True:
                j = blob.find(pat, pos)
                if j < 0:
                    break
                hits.append(img.image_base + s.va + j)
                pos = j + 1
        print("== %d reference(s) to %08X ==" % (len(hits), tva))
        for h in hits:
            # the dword sits inside a `mov`/`cmp`/`push` displacement: the instruction starts
            # 1-3 bytes before it. Try the few plausible starts and keep the one whose decode
            # lands exactly on h+4.
            start = None
            for cand in range(h - 4, h + 1):
                got = sr_dis.disasm_range(img, cand, 1)
                if got and got[0][0] == cand:
                    # decode forward to see if an instruction boundary lands on h+4
                    seq = sr_dis.disasm_range(img, cand, 4)
                    if any(va == h + 4 for va, _, _ in seq):
                        start = cand
                        break
            if start is None:
                print("  %08X  (could not align an instruction start)" % h)
                continue
            print("  --- %08X ---" % h)
            for va, text, raw in sr_dis.disasm_range(img, start - 0, before + after):
                print("      %08X  %s" % (va, text))
                if va >= h + 8:
                    pass
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
