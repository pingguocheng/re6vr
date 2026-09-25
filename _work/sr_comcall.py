#!/usr/bin/env python3
"""Stereo-render investigation: step 17 - global sweep for direct COM calls on ANY object.

`call [reg+disp32]` (FF /2, mod=10) is what MSVC emits when the receiver is already in a
register. Its operand is not a string pointer and not a common struct offset, so a sweep for
exact d3d9 vtable offsets over the whole .text finds real COM call sites with few false hits.
Also reports `mov reg,[reg+disp32]` where disp is 0x44/0xA8 for the vtable-reload idiom.
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

NAMES = {
    0x44: "Present", 0xA8: "EndScene", 0xA4: "BeginScene", 0xAC: "Clear", 0x40: "Reset",
    0x48: "GetBackBuffer", 0x60: "SetTransform", 0x6C: "SetViewport", 0x1C0: "SetRenderTarget",
    0x1C4: "GetRenderTarget", 0x08: "Release", 0x18: "GetDirect3D", 0x20: "GetDisplayMode",
}


def main() -> int:
    img = pe.Image.load(IMG)
    want = set(int(a, 0) for a in sys.argv[1:]) or {0x44, 0xA8, 0xA4, 0xAC, 0x48}
    s = [x for x in img.sections if x.name == ".text"][0]
    blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
    base = img.image_base + s.va
    print("sweeping .text %08X..%08X for call [reg+disp] with disp in %s"
          % (base, base + len(blob), ", ".join("0x%X" % w for w in sorted(want))))
    hits = []
    for j in range(len(blob) - 6):
        if blob[j] != 0xFF:
            continue
        b1 = blob[j + 1]
        if (b1 & 0xC0) != 0x80 or ((b1 >> 3) & 7) != 2 or (b1 & 7) == 4:
            continue
        disp = struct.unpack_from("<i", blob, j + 2)[0]
        if disp not in want:
            continue
        hits.append((base + j, disp))
    print("call [reg+disp] candidates: %d" % len(hits))
    for va, disp in hits:
        print("  %08X  call [reg+0x%X]  (%s)" % (va, disp, NAMES.get(disp, "?")))

    print()
    print("== mov reg,[reg+0x44] / [reg+0xA8] (vtable reload idiom) ==")
    hits2 = []
    for j in range(len(blob) - 6):
        if blob[j] != 0x8B:
            continue
        b1 = blob[j + 1]
        if (b1 & 0xC0) != 0x80 or (b1 & 7) == 4 or ((b1 >> 3) & 7) != (b1 & 7):
            continue
        disp = struct.unpack_from("<i", blob, j + 2)[0]
        if disp in (0x44, 0xA8, 0xA4):
            hits2.append((base + j, disp))
    print("candidates: %d" % len(hits2))
    for va, disp in hits2[:120]:
        print("  %08X  mov reg,[reg+0x%X]  (%s)" % (va, disp, NAMES.get(disp, "?")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
