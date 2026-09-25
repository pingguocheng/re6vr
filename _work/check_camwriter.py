"""Throwaway: characterise 0x4FF9B0 (writes all six mCameraOrg[0] fields) and 0x4F9950.

Questions that decide whether this is the head-tracking write point:
  1. what is the function, in the sense of "who calls it and how often" - a per-frame camera
     update, or a one-shot initialiser / serialiser?
  2. does it build a view matrix from those fields (the look-at maths), or does it merely copy
     them somewhere?
  3. is it reachable from the sBioCamera vtable (0x151A380) and from the frame path?
"""
import sys
from collections import defaultdict

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

an = analyze.load_analysis(CACHE, EXE)
fns = an.functions

for start in (0x4FF9B0, 0x4F9950, 0x4FA570):
    f = fns.get(start)
    if f is None:
        print("0x%08X: not an analysed function start" % start)
        continue
    callers = sorted(f.called_by)
    print("=== 0x%08X .. 0x%08X  %d instructions, %d blocks" %
          (start, f.end, f.instructions, len(f.blocks)))
    print("    origin     : %s" % f.origin)
    print("    called by  : %s%s" % (", ".join("0x%08X" % c for c in callers[:14]) or "-",
                                     " (+%d more)" % (len(callers) - 14) if len(callers) > 14 else ""))
    print("    calls      : %s" % ", ".join("0x%08X" % c for c in f.calls[:14]))
    # Which of its instructions look like matrix work: writes of 4 consecutive dwords?
    stores = defaultdict(list)
    for va in sorted(f.blocks):
        ins = an.decode_at(va)
        if ins is None or not ins.operands:
            continue
        dst = ins.operands[0]
        if dst.kind == "mem" and dst.base and dst.disp:
            stores[dst.base].append(dst.disp)
    top = sorted(stores.items(), key=lambda kv: -len(kv[1]))[:4]
    for base, disps in top:
        print("    stores via %-4s: %d, offsets %s%s"
              % (base, len(disps),
                 " ".join("+0x%X" % d for d in sorted(set(disps))[:12]),
                 " ..." if len(set(disps)) > 12 else ""))
    print()
