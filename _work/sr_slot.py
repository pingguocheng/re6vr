#!/usr/bin/env python3
"""Stereo-render investigation: step 9 - hunt the d3d9 device call sites by slot constant.

Uses the *rendered* instruction text (register names must come from `ins.text()`, not
`Operand.text`) to find any `mov reg,[reg+SLOT]` with SLOT in the IDirect3DDevice9 range, and
prints the following few instructions, so the calling shape can be read rather than assumed.
"""
from __future__ import annotations

import re
import sys

import sr_lib

RE_MOV = re.compile(r"^mov\s+([a-z0-9]+),\s*(?:\w+ ptr )?\[([a-z0-9]+)(?:\+(-?0x[0-9a-f]+))?\]$")

DEV = {
    0x44: "Present", 0xA8: "EndScene", 0xA4: "BeginScene", 0xAC: "Clear",
    0x40: "Reset", 0x48: "GetBackBuffer", 0x6C: "SetViewport", 0x1C0: "SetRenderTarget",
    0x1C4: "GetRenderTarget", 0x70: "GetViewport", 0xBC: "GetTexture",
    0xB4: "SetTexture", 0x60: "SetTransform", 0x68: "MultiplyTransform",
    0x18: "GetDirect3D", 0xB8: "GetTextureStageState", 0x00: "QueryInterface",
}


def main() -> int:
    want = set(int(a, 0) for a in sys.argv[1:]) or {0x44, 0xA8, 0xA4, 0xAC, 0x40}
    funcs = sr_lib.build_all()
    print("functions %d  instructions %d" % (len(funcs), sum(len(r) for _, _, r in funcs)))
    total = 0
    for start, end, rows in funcs:
        for i, (va, text, is_call, is_jmp, ln) in enumerate(rows):
            m = RE_MOV.match(text)
            if not m:
                continue
            dst, base, disp = m.group(1), m.group(2), m.group(3)
            if dst == base:
                continue
            d = int(disp, 16) if disp else 0
            if d not in want:
                continue
            total += 1
            print("=== %08X in fn %08X : %s   (slot %d = %s)"
                  % (va, start, text, d // 4, DEV.get(d, "?")))
            for k in range(i, min(i + 6, len(rows))):
                r = rows[k]
                print("    %s %08X  %s" % (">>" if k == i else "  ", r[0], r[1]))
            print()
    print("total: %d" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
