#!/usr/bin/env python3
"""Find the projection-matrix builder and, with it, whether the game's fov is vertical or horizontal.

Why this matters for the VR work: the per-eye projection has to be built from the headset's per-eye
FOV, and the game's own fov (uCamera::mFov at +0x4C, measured in DEGREES - it reads 37.0 in the logs)
has to be interpreted the same way. Applying a vertical angle as if it were horizontal stretches the
image; the numbers would look "almost right", which is the worst kind of wrong.

The scale (degrees vs radians) is already known: uCameraCtrl::update publishes the pose with
`fld [esi+0x4C] -> fstp [esi+0x100]` (0x005FBE89), and the logged value is 37.0, i.e. degrees.
What is NOT known is the axis. That is decided by where the resulting cotangent is stored:

    D3D9 projection:  m[1][1] = 1/tan(fov/2)   (vertical fov)   -> row 1, column 1 = byte 20
                      m[0][0] = 1/tan(fov/2)   (horizontal fov) -> row 0, column 0 = byte  0

So this script hunts for the sequence "read the fov, halve it, take the tangent, invert it, store it
into a 4x4" and reports WHICH element it lands in.

Usage: python _work/find_proj_from_fov.py
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))

from disasm_lib import analyze  # noqa: E402

CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bh6_analysis.json")
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

TAN_OPS = ("fptan", "fsin", "fcos", "fdiv", "fdivr", "fld1")


def main() -> int:
    an = analyze.load_analysis(CACHE, EXE)
    if an is None:
        print("no analysis cache")
        return 2

    # Functions that contain the trig/reciprocal pattern AND touch a 4x4-ish store.
    interesting = {}
    for fn, ins in analyze.iter_instructions(an):
        m = ins.mnemonic
        if m in TAN_OPS:
            interesting.setdefault(fn.start, []).append(ins.va)
    print("functions containing a tan/reciprocal step: %d" % len(interesting))
    print("(a projection builder must also have a constant like 0.5 or pi/180 nearby)\n")

    shown = 0
    for start in sorted(interesting):
        insns = [i for f, i in analyze.iter_instructions(an) if f.start == start]
        text = "\n".join("%08X %s" % (i.va, i.mnemonic + " " + " ".join(o.text for o in i.operands))
                         for i in insns)
        if "fptan" not in text and "fdiv" not in text:
            continue
        # A projection builder is small and dense in x87; skip the huge inlined / runtime-general ones.
        if len(insns) > 400:
            continue
        shown += 1
        if shown > 8:
            break
        print("=== candidate %08X (%d insns) ===" % (start, len(insns)))
        for line in text.splitlines()[:40]:
            print("   " + line)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
