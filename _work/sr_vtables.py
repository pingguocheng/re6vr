#!/usr/bin/env python3
"""Stereo-render investigation: step 22 - vtable slot table for the key singletons.

Dumps the slot -> function table for the vtables the project has already identified, so a
"call [reg+slot]" site can be named by the class whose vtable it must be. Then lists every
`mov reg,ds:[GLOBAL]` + `call [reg+slot]` site for the renderer globals, which is how the frame
code drives the renderer.
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

VTABLES = {
    0x0151A380: "sBioCamera",
    0x016E5E18: "sRender",
    0x0152D620: "uCameraCtrl",
    0x0152C3C0: "uCamera",
    0x0186E260: "sCamera DTI (not a vtable)",
}


def main() -> int:
    img = pe.Image.load(IMG)
    for vt, name in sorted(VTABLES.items()):
        print("=== vtable 0x%08X  %s ===" % (vt, name))
        for slot in range(0, 48):
            rva = vt + slot * 4 - img.image_base
            v = img.read_u32_rva(rva)
            if v is None:
                break
            if not (0x401000 <= v < 0x1500000):
                print("   slot %2d (+0x%02X)  0x%08X  <-- not code, table ends?" % (slot, slot * 4, v))
                break
            print("   slot %2d (+0x%02X)  0x%08X" % (slot, slot * 4, v))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
