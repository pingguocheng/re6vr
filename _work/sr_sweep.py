#!/usr/bin/env python3
"""Stereo-render investigation: step 16 - linear sweep for d3d9 vtable calls in a range.

The cache has holes over the renderer, so this sweeps raw bytes for the ModRM forms of
`mov reg,[reg+disp32]` (8B /r with mod=10) and `call [reg+disp32]` (FF /2 with mod=10, which
MSVC does emit inside d3d9.dll-adjacent code), keeping only displacements that are exact
IDirect3DDevice9 / IDirect3D9 vtable byte offsets, and then *validating alignment*: the 1-3
bytes before the pattern must themselves decode to an instruction that ends exactly on the
pattern, or the sequence must be reachable from a preceding `mov reg,[reg]`. Every candidate is
printed with a decoded window so a false positive is visible as garbage rather than trusted.
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

DEV = {
    0x44: "Present", 0xA8: "EndScene", 0xA4: "BeginScene", 0xAC: "Clear", 0x40: "Reset",
    0x48: "GetBackBuffer", 0x60: "SetTransform", 0x6C: "SetViewport", 0x1C0: "SetRenderTarget",
    0x1C4: "GetRenderTarget", 0x00: "QueryInterface", 0x08: "Release", 0x18: "GetDirect3D",
}
D3D9 = {0x20: "GetDisplayMode", 0x40: "CreateDevice", 0x44: "CreateDeviceEx?"}


def main() -> int:
    img = pe.Image.load(IMG)
    ranges = []
    for a in sys.argv[1:]:
        lo, _, hi = a.partition("-")
        ranges.append((int(lo, 0), int(hi, 0)))
    if not ranges:
        ranges = [(0x00F00000, 0x01200000)]
    want = dict(DEV)
    want.update({k: v for k, v in D3D9.items() if k not in want})
    # A real COM call re-reads the vtable through the SAME register it then calls through:
    # `mov eax,[eax+0x44]` / `mov edx,[edx+0x44]` / `mov ecx,[ecx+0x44]`. Restricting to that
    # form removes the flood of ordinary struct field accesses (which use a different dst).
    regs = ("eax", "ecx", "edx", "ebx", "esi", "edi")
    seen = 0
    for lo, hi in ranges:
        rva = lo - img.image_base
        blob = img.read_rva(rva, hi - lo)
        print("=== range %08X..%08X (%d bytes) ===" % (lo, hi, len(blob)))
        for j in range(len(blob) - 6):
            b0 = blob[j]
            b1 = blob[j + 1]
            # mov reg,[reg+disp32] : 8B /r, mod=10, reg field == r/m field
            is_mov = (b0 == 0x8B and (b1 & 0xC0) == 0x80 and (b1 & 7) != 4
                      and ((b1 >> 3) & 7) == (b1 & 7))
            is_call = (b0 == 0xFF and (b1 & 0xC0) == 0x80 and ((b1 >> 3) & 7) == 2)
            if not (is_mov or is_call):
                continue
            disp = struct.unpack_from("<i", blob, j + 2)[0]
            if disp not in want:
                continue
            va = lo + j
            ok = False
            for back in (1, 2, 3, 4):
                if j - back < 0:
                    continue
                seq = sr_dis.disasm_range(img, va - back, 6)
                if any(x[0] == va for x in seq):
                    ok = True
                    break
            seen += 1
            print("  %s %08X  disp +0x%X (%s)  align=%s"
                  % ("MOV " if is_mov else "CALL", va, disp, want[disp], ok))
            if not ok:
                continue
            for xva, text, raw in sr_dis.disasm_range(img, va - 6, 12):
                print("        %08X  %s" % (xva, text))
            print()
    print("candidates: %d" % seen)
    return 0


if __name__ == "__main__":
    sys.exit(main())
