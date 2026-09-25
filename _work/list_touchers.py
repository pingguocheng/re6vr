"""Throwaway: print every instruction that touches the resolved camera array (+0x1030 group)
or mCameraOrg (+0xE30), grouped by function - the full list, not a top-N.

Uses the corrected read/write classification from scan_readwrite.py (a one-operand memory operand
is a SOURCE, not a store). Output is grouped so a function that reads a whole entry as a pose is
distinguishable from one that merely touches one float.
"""
import sys
from collections import defaultdict

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

WRITE_MN = ("mov", "movss", "movsd", "movaps", "movups", "movdqa", "movdqu", "movq", "movd",
            "fstp", "fst", "add", "sub", "mul", "div", "xor", "and", "or", "inc", "dec")
ENTRY = (0x00, 0x04, 0x08, 0x10, 0x14, 0x18, 0x20, 0x24, 0x28, 0x30, 0x34, 0x38, 0x3C)

WHICH = sys.argv[1] if len(sys.argv) > 1 else "GRP"        # GRP or ORG
BASE = 0x1030 if WHICH == "GRP" else 0xE30
SET = {BASE + 0x40 * i + e: i for i in range(8) for e in ENTRY}

an = analyze.load_analysis(CACHE, EXE)
byfn = defaultdict(list)
for fn, ins in analyze.iter_instructions(an):
    if not ins.operands:
        continue
    writing = ins.mnemonic in WRITE_MN and not ins.mnemonic.startswith(("cmp", "test"))
    for k, o in enumerate(ins.operands):
        if o.kind == "mem" and o.disp in SET:
            role = "W" if (k == 0 and writing) else "r"
            byfn[fn.start].append((ins.va, role, ins.mnemonic, o.base, o.disp, SET[o.disp],
                                   ins.text()))

print("%s: %d functions touch the base 0x%X" % (WHICH, len(byfn), BASE))
for start in sorted(byfn):
    rows = sorted(byfn[start])
    f = an.functions.get(start)
    ents = sorted({r[5] for r in rows})
    roles = "".join(sorted({r[1] for r in rows}))
    print("\nfn 0x%08X  %s  entries %s  roles %s  (%d instructions)"
          % (start, ("%d instrs" % f.instructions) if f else "?", ents, roles, len(rows)))
    for va, role, mn, base, disp, idx, txt in rows:
        print("     0x%08X %s  %s" % (va, role, txt))
