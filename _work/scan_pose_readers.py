"""Throwaway: readers of a uCameraCtrl-style working pose (+0x50 pos / +0x60 up / +0x70 target).

0x5F80B0 turns exactly those three vectors into the view matrix via 0xE6FD20. Any OTHER function
that reads all three (base register not esp/ebp) is either another matrix builder for the same
camera or a system that consumes the camera pose directly - which is what "the renderer reads the
pose" would look like in code.

    python scan_pose_readers.py [lo] [hi]
"""
import sys
from collections import defaultdict

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

FLOAT_MN = ("movss", "fld", "mulss", "addss", "subss", "movaps", "movups", "movq", "movd",
            "minss", "maxss", "divss", "comiss", "ucomiss", "mov")
WRITE = ("mov", "movss", "fstp", "fst", "movaps", "movups", "movq", "movd")
VEC = {0x50: "pos", 0x54: "pos", 0x58: "pos", 0x60: "up", 0x64: "up", 0x68: "up",
       0x70: "tgt", 0x74: "tgt", 0x78: "tgt"}

an = analyze.load_analysis(CACHE, EXE)
reads = defaultdict(list)
for fn, ins in analyze.iter_instructions(an):
    if ins.mnemonic not in FLOAT_MN or not ins.operands:
        continue
    writing = ins.mnemonic in WRITE
    for k, o in enumerate(ins.operands):
        if o.kind == "mem" and o.disp in VEC and o.base not in (None, "esp", "ebp"):
            if k == 0 and writing:
                continue
            reads[(fn.start, o.base)].append((ins.va, o.disp, ins.text()))

rows = []
for (start, base), v in reads.items():
    groups = {VEC[r[1]] for r in v}
    if len(groups) >= 3:
        rows.append((start, base, v))
rows.sort()
print("%d (function, base) groups read all three pose vectors" % len(rows))
for start, base, v in rows:
    f = an.functions.get(start)
    print("\nfn 0x%08X [%s] (%s)" % (start, base, ("%d instrs" % f.instructions) if f else "?"))
    for va, disp, txt in sorted(v, key=lambda r: r[1]):
        print("     0x%08X  +0x%X  %s" % (va, disp, txt))
