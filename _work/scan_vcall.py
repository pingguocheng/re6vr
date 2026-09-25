"""Throwaway: find virtual call sites of the form `mov reg,[obj+SLOT]; ...; call reg`.

MSVC does not emit `call dword ptr [eax+0x48]` in this build: it loads the slot into a register
first (`mov edx,[edx+0x48]; call edx`, exactly as sBioCamera::Update does at 0x503B40). Scanning
for the memory-operand form alone therefore returns zero and would have hidden every caller of the
camera's matrix getter.

    python scan_vcall.py 0x48      # view-matrix getter (0x5F80B0 in the uCamera family)
    python scan_vcall.py 0x2C      # sBioCamera::Update (0x503880)
"""
import sys
from collections import defaultdict

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

SLOT = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x48
an = analyze.load_analysis(CACHE, EXE)
byfn = defaultdict(list)
for fn, ins in analyze.iter_instructions(an):
    byfn[fn.start].append(ins)

hits = []
for start, insns in byfn.items():
    insns.sort(key=lambda i: i.va)
    for k, i in enumerate(insns):
        if i.mnemonic != "mov" or len(i.operands) != 2:
            continue
        d, s = i.operands
        if d.kind != "reg" or s.kind != "mem" or s.disp != SLOT:
            continue
        for j in insns[k + 1:k + 5]:
            if j.mnemonic == "call" and j.operands and j.operands[0].kind == "reg" \
               and j.operands[0].text == d.text:
                hits.append((start, i.va, s.base, SLOT, j.va, i.text(), j.text()))
                break
print("%d `mov reg,[obj+0x%X]; call reg` sites" % (len(hits), SLOT))
for h in hits:
    print("   fn 0x%08X  0x%08X  [%s+0x%X]  call@0x%08X   %s ; %s" % h)
