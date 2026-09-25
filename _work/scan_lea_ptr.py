"""Throwaway: who takes a POINTER to a mViewportCamera entry (or a mCameraOrg entry)?

A packed-SSE read of the matrix region does not exist in the image, so if anything consumes
mViewportCamera as a matrix it must first form the address (`lea reg,[base+disp]`) and then use
[reg+k]. This scan lists every `lea` whose displacement falls in the arrays, which is how the
two-instruction getter 0x5CA720 (`lea eax,[ecx+0x1320]; ret`) was found.

    python scan_lea_ptr.py 0x12A0 0x300      # mViewportCamera (8 * 0x60 at +0x12A0)
    python scan_lea_ptr.py 0xE30 0x200       # mCameraOrg
    python scan_lea_ptr.py 0x1030 0x200      # the copy target of 0x4FCB70
"""
import sys

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

BASE = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x12A0
SPAN = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x300
SET = set(range(BASE, BASE + SPAN, 4))

an = analyze.load_analysis(CACHE, EXE)
rows = []
for fn, ins in analyze.iter_instructions(an):
    if ins.mnemonic != "lea" or len(ins.operands) < 2:
        continue
    d = ins.operands[0]
    s = ins.operands[-1]
    if s.kind == "mem" and s.disp in SET:
        rows.append((fn.start, ins.va, s.base, s.disp, ins.text()))

print("%d lea instructions point into 0x%X..0x%X" % (len(rows), BASE, BASE + SPAN))
for r in sorted(rows, key=lambda r: (r[3], r[0])):
    print("   0x%08X  fn 0x%08X  [%s+0x%X]  %s" % (r[1], r[0], r[2], r[3], r[4]))
