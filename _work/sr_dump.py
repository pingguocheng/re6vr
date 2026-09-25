#!/usr/bin/env python3
"""Stereo-render investigation: step 27 - structure of the main frame function.

Dumps the full call/flow skeleton of a function (address, mnemonic with operands, and calls
resolved to their target VA) so the frame's phases can be read off without a 1000-line listing.
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "scripts"))

import sr_dis  # noqa: E402
from disasm_lib import pe  # noqa: E402

IMG = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"


def main() -> int:
    img = pe.Image.load(IMG)
    start = int(sys.argv[1], 0)
    end = int(sys.argv[2], 0)
    count = (end - start) // 2 + 8
    keep = sys.argv[3] if len(sys.argv) > 3 else None
    for va, text, raw in sr_dis.disasm_range(img, start, count):
        if va >= end:
            break
        if keep and keep not in text:
            continue
        print("%08X  %s" % (va, text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
