#!/usr/bin/env python3
"""Stereo-render investigation: step 18 - virtual calls on the render singleton.

`sRender`'s instance is at the fixed address 0x186E8BC (MEASURED: `mov esi,[0x186E8BC]`
followed by `mov [esi+0x120], eax` where +0x120 is `sRender::mScreenSize`). So `mov ecx,
0x186E8BC` / `mov esi, ds:[0x186E8BC]` followed by `call [reg+slot]` is how the engine drives
the renderer per frame. This finds those sites and prints the slot, which identifies the sRender
methods that the frame code calls.
"""
from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "scripts"))

import sr_dis  # noqa: E402
from disasm_lib import pe  # noqa: E402

IMG = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
GLOBALS = [0x186E8BC, 0x186E870, 0x186E8B8, 0x186E8C0, 0x186E1B4]


def main() -> int:
    img = pe.Image.load(IMG)
    for g in GLOBALS:
        print("################ global %08X" % g)
        # find every site that mentions the global, decode a window, and look for a call
        # through a slot within the next few instructions
        rva_refs = []
        for s in img.sections:
            if not s.is_executable:
                continue
            blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
            pat = (g).to_bytes(4, "little")
            pos = 0
            while True:
                j = blob.find(pat, pos)
                if j < 0:
                    break
                rva_refs.append(img.image_base + s.va + j)
                pos = j + 1
        print("  %d raw references" % len(rva_refs))
        for h in rva_refs:
            # find an instruction start in [h-4, h] that reaches h+4
            start = None
            for cand in range(h - 4, h + 1):
                seq = sr_dis.disasm_range(img, cand, 6)
                if any(va == h + 4 for va, _, _ in seq):
                    start = cand
                    break
            if start is None:
                continue
            seq = sr_dis.disasm_range(img, start, 16)
            for k, (va, text, raw) in enumerate(seq):
                m = re.search(r"call (\w+)", text)
                if not m:
                    continue
                reg = m.group(1)
                # find the last `mov reg,[?+0xNN]` before it
                slot = None
                base = None
                for back in range(k - 1, max(-1, k - 6), -1):
                    m2 = re.match(r"^mov\s+%s,\s*(?:\w+ ptr )?\[(\w+)\+?(0x[0-9a-f]+)?\]$"
                                  % reg, seq[back][1])
                    if m2:
                        slot = int(m2.group(2), 16) if m2.group(2) else 0
                        base = m2.group(1)
                        break
                if slot is None:
                    continue
                print("  site %08X (global mentioned at %08X): slot +0x%X  base %s"
                      % (va, h, slot, base))
                for xva, xt, xr in seq:
                    print("        %08X  %s" % (xva, xt))
                print()
                break
    return 0


if __name__ == "__main__":
    sys.exit(main())
