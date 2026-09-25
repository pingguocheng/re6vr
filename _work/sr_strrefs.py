#!/usr/bin/env python3
"""Stereo-render investigation: step 24 - readers of the strings that name the frame seam.

Every one of these strings is a literal whose VA the engine loads with `lea reg,[VA]` (the
xref-by-data scan in the project's own tooling returns nothing for them, which is what happens
when the reference is a rel32 rather than a dword). This decodes the surrounding code from the
image so the function that uses the string is identified.
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

TARGETS = {
    0x016E5F20: "present_result",
    0x016E6298: "Stereo",
    0x016E5F48: "getDevice()->CreateAdditionalSwapChain(&d3dppm,&mDisplay[i].pSwapChain)",
    0x016E5E90: "mDisplay[i].pSwapChain->GetBackBuffer(...)",
    0x016E5EE0: "getDevice()->GetBackBuffer(0,0,...)",
    0x016FB5D0: "mpSwapChain->GetBackBuffer(...)",
    0x016FB618: "IRender->getDevice()->CreateAdditionalSwapChain(...)",
    0x016E6284: "VSYNC",
    0x016E5F90: "getDevice()->Reset(&d3dppm)",
}


def main() -> int:
    img = pe.Image.load(IMG)
    for tva, name in sorted(TARGETS.items()):
        print("################ %08X  %r" % (tva, name))
        found = 0
        # The string address appears as the last 4 bytes of `lea reg,[VA]` (8D /r disp32) or
        # `push VA` (68 disp32), or `mov reg,VA` (B8+r disp32). Find the raw dword, then walk
        # back to a valid instruction start.
        for s in img.sections:
            if not s.is_executable:
                continue
            blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
            pat = tva.to_bytes(4, "little")
            pos = 0
            while True:
                j = blob.find(pat, pos)
                if j < 0:
                    break
                pos = j + 1
                hit_va = img.image_base + s.va + j
                for cand in (j - 4, j - 3, j - 2, j - 1):
                    if cand < 0:
                        continue
                    seq = sr_dis.disasm_range(img, img.image_base + s.va + cand, 3)
                    if seq and any(x[0] == hit_va + 4 for x in seq):
                        print("  --- reference at %08X (instr at %08X) ---"
                              % (hit_va, img.image_base + s.va + cand))
                        for xva, text, raw in sr_dis.disasm_range(
                                img, img.image_base + s.va + cand - 16, 16):
                            print("      %08X  %s" % (xva, text))
                        print()
                        found += 1
                        break
        if not found:
            print("  (no aligned reference found)")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
