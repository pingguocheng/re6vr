"""Throwaway: who uses the sBioCamera singleton (ds:[0x186E23C]) as a base for offsets >= 0xCA0?

Displacement-only scans are noisy: several classes have fields at the same offsets. This one is
anchored on the one provable fact, the global pointer `[0x186E23C]` (836 references from 387
functions; 0x4F9950 is called with ecx = [0x186E23C], and the runtime probe's `this` behaved like
a singleton), and follows that register inside the function until it is overwritten.

Whatever reads the object through that pointer at a camera-shaped offset is reading THE camera -
that is the claim this scan is meant to settle.
"""
import sys
from collections import defaultdict

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

G = 0x186E23C
LO = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0xCA0
HI = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x1620

an = analyze.load_analysis(CACHE, EXE)
byfn = defaultdict(list)
for fn, ins in analyze.iter_instructions(an):
    byfn[fn.start].append(ins)

hits = []
for start, insns in byfn.items():
    insns.sort(key=lambda i: i.va)
    reg = None
    for i in insns:
        t = i.text()
        # tracking: mov reg, ds:[0x186E23C]
        if i.mnemonic in ("mov", "movzx") and len(i.operands) == 2:
            d, s = i.operands
            if s.kind == "mem" and s.base is None and s.disp == G and d.kind == "reg":
                reg = d.text
                continue
            if d.kind == "reg" and d.text == reg:
                # the register is overwritten: if it is loaded from the object itself, keep a
                # marker for one more step (common: mov edi,[eax+0x34])
                if s.kind == "mem" and s.base == reg:
                    reg = None
                    continue
                reg = None
        if reg is None:
            continue
        for o in i.operands:
            if o.kind == "mem" and o.base == reg and LO <= o.disp < HI:
                hits.append((start, i.va, reg, o.disp, t))
for h in hits:
    print("fn 0x%08X  0x%08X  [%s+0x%X]  %s" % h)
print("%d accesses to the sBioCamera via [0x%08X] in 0x%X..0x%X (%d functions)"
      % (len(hits), G, LO, HI, len({h[0] for h in hits})))
