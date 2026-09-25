#!/usr/bin/env python3
"""Stereo-render investigation: step 29 - read a vtable as a run of pointers.

`sr_vtables.py` starts at a guessed base; this starts at the ACTUAL vptr value a live object
carries (so it cannot be off by 0x14 the way an eyeballed base can) and prints the slots with
their first bytes, so a slot that is really a string ("CameraParam...") is visible as such.
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "scripts"))

from disasm_lib import pe  # noqa: E402

IMG = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"


def main() -> int:
    img = pe.Image.load(IMG)
    for vt in [int(a, 0) for a in sys.argv[1:]] or [0x151A394]:
        print("=== vptr 0x%08X ===" % vt)
        for slot in range(0, 22):
            va = vt + slot * 4
            v = img.read_u32_rva(va - img.image_base)
            kind = ""
            if v is None:
                break
            if 0x401000 <= v < 0x1500000:
                kind = "code"
            else:
                s = img.is_printable_string_at(v - img.image_base, 3)
                kind = "STRING %r" % s if s else "data"
            print("  +0x%02X  slot %2d  %08X  %s" % (slot * 4, slot, v, kind))
    return 0


if __name__ == "__main__":
    sys.exit(main())
