#!/usr/bin/env python3
"""Stereo-render investigation: step 30 - who WRITES the display count / display array.

`0x00F3ECA4` and the Present loop both read `[sRender+0x462694]` as "number of displays". This
finds the instructions that store to that displacement (i.e. the code that decides how many
displays exist) and to the mDisplay array, by decoding every candidate site from the image and
keeping only real stores.
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


def main() -> int:
    img = pe.Image.load(IMG)
    s = [x for x in img.sections if x.name == ".text"][0]
    blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
    base = img.image_base + s.va
    for target, label in ((0x462694, "display count"),
                          (0x46267C, "mDisplay[] base")):
        print("=== raw refs to %s (+0x%X) ===" % (label, target))
        pat = struct.pack("<I", target)
        hits = []
        pos = 0
        while True:
            j = blob.find(pat, pos)
            if j < 0:
                break
            hits.append(base + j)
            pos = j + 1
        for h in hits:
            start = None
            for cand in range(h - 6, h + 1):
                seq = sr_dis.disasm_range(img, cand, 6)
                if any(va == h + 4 for va, _, _ in seq):
                    ins = next(x for x in seq if x[0] == cand)
                    start = cand
                    break
            if start is None:
                continue
            ins = sr_dis.disasm_range(img, start, 1)[0]
            is_store = bool(re.match(r"^(mov|cmp|add|sub|or|and|inc|dec)\s+(dword|word|byte) ptr \[",
                                     ins[1]))
            if is_store:
                print("  %08X  %s" % (start, ins[1]))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
