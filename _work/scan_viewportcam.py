"""Throwaway: who reads mViewportCamera (sBioCamera+0x12A0, 8 entries of 0x60) as a MATRIX?

The 0x300 bytes between mViewportCamera (+0x12A0, registered with type 0x200001) and mDispCtrlFlag
(+0x15A0) are exactly 8 * 0x60, and sBioCamera::Update writes, for each of 8 slots, a block of
floats at entry+0x10 plus three floats at entry+0x50..0x58. That is a per-entry camera state.

A displacement-only match is noisy (other classes have fields at the same offsets and read them as
integers), so this scan keeps only VECTOR/FLOAT reads of entry 0's block: `movss`/`movaps`/`movups`
/`movq`/`movd`/`mulps`... A reader that loads those as floats is consuming the matrix, not a
counter.
"""
import sys
from collections import defaultdict

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

FLOAT_MN = ("movss", "movaps", "movups", "movq", "movd", "movdqa", "movdqu", "mulps", "addps",
            "subps", "mulss", "addss", "subss", "cmpps", "cmpless", "maxss", "minss",
            "unpcklps", "shufps", "andps", "orps", "xorps", "fld")
BASE = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x12A0
SPAN = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x300
SET = set(range(BASE, BASE + SPAN, 4))

an = analyze.load_analysis(CACHE, EXE)
byfn = defaultdict(list)
for fn, ins in analyze.iter_instructions(an):
    if ins.mnemonic not in FLOAT_MN or not ins.operands:
        continue
    for o in ins.operands:
        if o.kind == "mem" and o.disp in SET:
            byfn[fn.start].append((ins.va, o.base, o.disp, ins.text()))

print("base 0x%X span 0x%X: %d functions read it as a float/vector" % (BASE, SPAN, len(byfn)))
for start in sorted(byfn):
    rows = byfn[start]
    f = an.functions.get(start)
    print("\nfn 0x%08X  %s  (%d hits, offsets %s)"
          % (start, ("%d instrs" % f.instructions) if f else "?", len(rows),
             sorted({"0x%X" % r[2] for r in rows})))
    for va, base, disp, txt in rows[:24]:
        print("     0x%08X  %s" % (va, txt))
