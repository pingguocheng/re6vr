#!/usr/bin/env python3
"""Stereo-render investigation: step 14 - disassemble an arbitrary VA range from the image.

The analysis cache has holes exactly where the renderer lives (the Direct3DCreate9 caller at
0x00F42D6A is not inside any cached function), so scans over cached blocks cannot answer
"who calls Present". This decodes directly from the PE image with the same validated decoder,
controlled by the caller so it never desynchronises silently: it starts at a VA the caller
knows is an instruction boundary and stops at the first undecodable byte, reporting it.
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                "scripts"))

from disasm_lib import pe, x86  # noqa: E402

IMG = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"


def disasm_range(img, start_va, count):
    rva = start_va - img.image_base
    blob = img.read_rva(rva, 16 * count + 64)
    dec = x86.Decoder(blob, start_va)
    out = []
    i = 0
    for _ in range(count):
        try:
            ins = dec.decode(i)
        except Exception as e:                       # the decoder has a few unfilled tables
            out.append((start_va + i, "!DECODER-ERROR: %s" % e, ""))
            break
        if not ins.ok:
            out.append((start_va + i, "!UNDECODABLE", ins.error))
            break
        out.append((ins.va, ins.text(), ins.raw.hex()))
        i += ins.length
    return out


def main() -> int:
    img = pe.Image.load(IMG)
    if len(sys.argv) < 3:
        print("usage: sr_dis.py <startVA> <count> [--grep REGEX]")
        return 2
    start = int(sys.argv[1], 0)
    count = int(sys.argv[2], 0)
    grep = None
    if len(sys.argv) > 4 and sys.argv[3] == "--grep":
        import re
        grep = re.compile(sys.argv[4], re.I)
    for va, text, raw in disasm_range(img, start, count):
        if grep is None or grep.search(text):
            print("%08X  %-46s %s" % (va, text, raw[:24] if raw != "!UNDECODABLE" else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
