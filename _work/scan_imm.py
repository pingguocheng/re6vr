"""Throwaway: find the code that indexes the per-slot camera array (stride 0x190).

sBioCamera::Update walks `[this+0x34 + 0x190*k]` (k = camera/viewport slot) and calls
`camera->vtable[0x48]` - uCameraCtrl's GetViewMatrix (0x5F80B0) - to obtain the view matrix it
stores in mViewportCamera[k]. The stride 0x190 is unusual enough to be found by its immediate,
which identifies both the array and the code that REGISTERS a camera into a slot.

    python scan_imm.py 0x190
"""
import sys

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

IMM = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x190
an = analyze.load_analysis(CACHE, EXE)
n = 0
for fn, ins in analyze.iter_instructions(an):
    for o in ins.operands:
        if o.kind == "imm":
            try:
                v = int(o.text, 0)
            except ValueError:
                continue
            if v == IMM:
                print("fn 0x%08X  0x%08X  %s" % (fn.start, ins.va, ins.text()))
                n += 1
                break
print("%d instructions with immediate 0x%X" % (n, IMM))
