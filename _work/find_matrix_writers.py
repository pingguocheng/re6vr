"""Throwaway: find matrix-shaped writes (>= 8 consecutive float stores 4 bytes apart).

The renderer needs a 4x4 view matrix: 16 floats written back to back. A function that writes such
a run is a matrix builder; if it also reads a look-at pose (three Vector3 about 0x10 apart), it is
the camera -> matrix step that head tracking would have to hook.

Only the destination displacement is used, and the base register is printed, so a run that is
obviously some other object can be rejected by eye. Instructions are taken at decoded boundaries
only (never by byte scan), which is the rule this project learned the hard way.
"""
import sys
from collections import defaultdict

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

WRITE_MN = ("mov", "movss", "movsd", "movaps", "movups", "movdqa", "movdqu", "movq", "movd",
            "fstp", "fst")

MINRUN = int(sys.argv[1], 0) if len(sys.argv) > 1 else 8
FILTER = sys.argv[2] if len(sys.argv) > 2 else None      # only functions whose base reg matches

an = analyze.load_analysis(CACHE, EXE)
runs = []
for fn, ins in analyze.iter_instructions(an):
    if ins.mnemonic not in WRITE_MN or not ins.operands:
        continue
    d = ins.operands[0]
    if d.kind != "mem" or d.base is None:
        continue
    runs.append((fn.start, d.base, d.disp, ins.va, ins.mnemonic))

byfn = defaultdict(list)
for f, base, disp, va, mn in runs:
    byfn[(f, base)].append((disp, va, mn))

out = []
for (f, base), rows in byfn.items():
    rows.sort()
    best = []
    cur = []
    for disp, va, mn in rows:
        if cur and disp == cur[-1][0] + 4:
            cur.append((disp, va, mn))
        else:
            if len(cur) > len(best):
                best = cur
            cur = [(disp, va, mn)]
    if len(cur) > len(best):
        best = cur
    if len(best) >= MINRUN:
        out.append((len(best), f, base, best))

out.sort(key=lambda r: (-r[0], r[1]))
print("%d (function, base register) pairs write a run of >= %d consecutive floats"
      % (len(out), MINRUN))
for n, f, base, best in out:
    if FILTER and FILTER not in ("0x%08X" % f):
        continue
    fn = an.functions.get(f)
    print("  fn 0x%08X (%s)  [%s+0x%X .. +0x%X]  %d floats"
          % (f, ("%d instrs" % fn.instructions) if fn else "?", base, best[0][0],
             best[-1][0] + 4, n))
    print("        first store 0x%08X  last store 0x%08X" % (best[0][1], best[-1][1]))
