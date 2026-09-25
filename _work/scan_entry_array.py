"""Throwaway: who touches the per-entry camera-state array at sBioCamera+0x12B0 (stride 0x60)?

sBioCamera::Update (0x503880, vtable slot 11) walks 8 camera slots and, for each, writes 16
consecutive floats at `this + 0x12B0 + 0x60*k` (esi = this+0x12C8, store at [esi-0x18]) plus a few
floats at +0x48..+0x58 of the same entry. That array - 8 * 0x60 = 0x300 bytes ending exactly at
mWipe (+0x15B0, a registered field) - is the resolved per-entry camera state.

Whoever READS it is reading the per-frame camera the renderer was given, so this prints every
access, split into read and write, with the base register.
"""
import sys
from collections import defaultdict

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

WRITE_MN = ("mov", "movss", "movsd", "movaps", "movups", "movdqa", "movdqu", "movq", "movd",
            "fstp", "fst", "add", "sub", "mul", "div", "xor", "and", "or", "inc", "dec")

BASE = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x12B0
STRIDE = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x60
COUNT = int(sys.argv[3], 0) if len(sys.argv) > 3 else 8
SPAN = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x60
SET = {BASE + STRIDE * i + e: i for i in range(COUNT) for e in range(0, SPAN, 4)}

an = analyze.load_analysis(CACHE, EXE)
byfn = defaultdict(list)
for fn, ins in analyze.iter_instructions(an):
    if not ins.operands:
        continue
    writing = ins.mnemonic in WRITE_MN and not ins.mnemonic.startswith(("cmp", "test"))
    for k, o in enumerate(ins.operands):
        if o.kind == "mem" and o.disp in SET:
            role = "W" if (k == 0 and writing) else "r"
            byfn[fn.start].append((ins.va, role, o.base, o.disp, SET[o.disp], ins.text()))

print("base 0x%X stride 0x%X count %d: %d functions" % (BASE, STRIDE, COUNT, len(byfn)))
for start in sorted(byfn):
    rows = sorted(byfn[start])
    f = an.functions.get(start)
    reads = [r for r in rows if r[1] == "r"]
    writes = [r for r in rows if r[1] == "W"]
    print("\nfn 0x%08X  %s  r=%d w=%d  entries %s"
          % (start, ("%d instrs" % f.instructions) if f else "?", len(reads), len(writes),
             sorted({r[4] for r in rows})))
    for va, role, base, disp, idx, txt in rows:
        print("     0x%08X %s  entry %d +0x%02X  %s" % (va, role, idx, disp - BASE - STRIDE * idx,
                                                         txt))
