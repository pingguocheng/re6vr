#!/usr/bin/env python3
"""Find the code that turns uCamera's fov (field `mFov`, +0x4C) into a projection matrix.

Why: the VR work needs a per-eye projection, and the first thing that must be settled is whether the
game's fov is a VERTICAL or a HORIZONTAL angle - getting that wrong stretches the image. The field
table gives the name and offset; only the projection builder shows the semantics.

The search is deliberately narrow and evidence-producing: keep the functions that BOTH
  * touch a float displacement of +0x4C (uCamera::mFov) or +0x220 (uCameraVeh::mFovy), and
  * contain a trig/divide sequence (x87 fld/fdiv/fmul/fstp/fsin or SSE movss/mulss/divss),
then print the matching functions' instructions so the maths can be read directly.

Usage: python _work/find_projection_builder.py [displacement-hex ...]
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))

from disasm_lib import analyze  # noqa: E402

CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bh6_analysis.json")
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

DISPS = [int(a, 16) for a in sys.argv[1:]] or [0x4C, 0x220, 0xE60]
TRIG = ("fsin", "fcos", "fptan", "fpatan", "fdiv", "fmul", "fld")
SSE = ("movss", "mulss", "divss")


def main() -> int:
    an = analyze.load_analysis(CACHE, EXE)
    if an is None:
        print("no analysis cache at %s" % CACHE)
        return 2

    want = set(DISPS)
    hits = {}      # fn.start -> set of displacements touched
    trig = {}      # fn.start -> count of trig/sse ops
    insn_count = {}
    for fn, ins in analyze.iter_instructions(an):
        insn_count[fn.start] = insn_count.get(fn.start, 0) + 1
        if ins.mnemonic in TRIG or ins.mnemonic in SSE:
            trig[fn.start] = trig.get(fn.start, 0) + 1
        for op in ins.operands:
            if op.kind == "mem" and op.base and (op.disp in want):
                hits.setdefault(fn.start, set()).add(op.disp)

    print("decoded %d function(s)" % len(insn_count))
    for d in DISPS:
        print("\n=== displacement +0x%X ===" % d)
        cands = [s for s, ds in hits.items() if d in ds]
        cands.sort(key=lambda s: -trig.get(s, 0))
        print("  %d function(s) touch it; top candidates by trig/SSE density:" % len(cands))
        for start in cands[:8]:
            print("  %08X  %4d insns, trig/sse=%d" % (start, insn_count.get(start, 0),
                                                      trig.get(start, 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
