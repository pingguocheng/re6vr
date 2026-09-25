"""Throwaway: who WRITES a uCameraCtrl-style pose block (+0x50 pos, +0x60 up, +0x70 target)?

0x5F80B0 (uCameraCtrl vtable slot 0x48) builds the view matrix from exactly those three vectors
(`lea eax,[ecx+0x60]; lea edx,[ecx+0x70]; add ecx,0x50; call 0xE6FD20`), and 0xE6FD20 is the
engine's look-at builder. So the fields at +0x50/+0x60/+0x70 ARE the per-frame camera, and
whoever stores to them each frame is the camera update head tracking has to precede.

The scan groups stores by (function, base register) and keeps groups that hit at least 4 of the 9
float slots, which is the shape of "write a whole pose" rather than "poke one float".
"""
import sys
from collections import defaultdict

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

WRITE_MN = ("mov", "movss", "movsd", "movaps", "movups", "movq", "fstp", "fst")
POSE = (0x50, 0x54, 0x58, 0x60, 0x64, 0x68, 0x70, 0x74, 0x78)
LO = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x400000
HI = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x1520000
MIN = int(sys.argv[3], 0) if len(sys.argv) > 3 else 4

an = analyze.load_analysis(CACHE, EXE)
groups = defaultdict(list)
for fn, ins in analyze.iter_instructions(an):
    if ins.mnemonic not in WRITE_MN or not ins.operands:
        continue
    d = ins.operands[0]
    if d.kind == "mem" and d.base is not None and d.disp in POSE:
        groups[(fn.start, d.base)].append((d.disp, ins.va, ins.text()))

rows = [(len({r[0] for r in v}), k, v) for k, v in groups.items() if len({r[0] for r in v}) >= MIN]
rows.sort(key=lambda r: (-r[0], r[1][0]))
print("%d (function, base) groups write >= %d pose slots" % (len(rows), MIN))
for n, (start, base), v in rows:
    if not (LO <= start < HI):
        continue
    f = an.functions.get(start)
    print("\nfn 0x%08X  [%s]  %d slots  (%s)"
          % (start, base, n, ("%d instrs" % f.instructions) if f else "?"))
    for disp, va, txt in sorted(v):
        print("     0x%08X  %s" % (va, txt))
