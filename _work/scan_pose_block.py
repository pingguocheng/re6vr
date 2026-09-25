"""Throwaway: who READS the published camera pose (+0xD0 pos / +0xE0 target / +0xF0 up)?

0x5FBE00 (uCamera-family vtable slot 9, the shared "commit" step) copies the working pose
(+0x50/+0x70/+0x60) into a Vector4 block at +0xD0 / +0xE0 / +0xF0, plus fov at +0x100. That block
is the camera state other systems consume; if the renderer reads a pose rather than calling the
matrix getter, this is where it would read it.

Grouping by (function, base) and requiring at least two of the three vectors separates a real
consumer from a coincidental single-float hit.
"""
import sys
from collections import defaultdict

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

FLOAT_MN = ("movss", "fld", "mulss", "addss", "subss", "movaps", "movups", "movq", "movd",
            "minss", "maxss", "cmpless", "divss", "mov")
BLOCK = {0xD0, 0xD4, 0xD8, 0xE0, 0xE4, 0xE8, 0xF0, 0xF4, 0xF8, 0x100, 0x104}
WRITE = ("mov", "movss", "fstp", "fst", "movaps", "movups")

an = analyze.load_analysis(CACHE, EXE)
reads = defaultdict(list)
writes = defaultdict(list)
for fn, ins in analyze.iter_instructions(an):
    if not ins.operands:
        continue
    writing = ins.mnemonic in WRITE
    for k, o in enumerate(ins.operands):
        if o.kind == "mem" and o.disp in BLOCK and o.base not in (None, "esp", "ebp"):
            (writes if (k == 0 and writing) else reads)[(fn.start, o.base)].append(
                (ins.va, o.disp, ins.text()))

print("=== readers of the published pose block ===")
rows = [(len({r[1] for r in v}), k, v) for k, v in reads.items() if len({r[1] for r in v}) >= 3]
rows.sort(key=lambda r: (-r[0], r[1][0]))
for n, (start, base), v in rows[:60]:
    f = an.functions.get(start)
    print("\nfn 0x%08X [%s] %d slots (%s)" % (start, base, n,
                                              ("%d instrs" % f.instructions) if f else "?"))
    for va, disp, txt in sorted(v, key=lambda r: r[1]):
        print("     0x%08X  +0x%X  %s" % (va, disp, txt))
print("\n(%d groups read >= 3 slots)" % len(rows))

print("\n=== writers of the published pose block ===")
rows = [(len({r[1] for r in v}), k, v) for k, v in writes.items() if len({r[1] for r in v}) >= 3]
rows.sort(key=lambda r: (-r[0], r[1][0]))
for n, (start, base), v in rows[:30]:
    f = an.functions.get(start)
    print("\nfn 0x%08X [%s] %d slots (%s)" % (start, base, n,
                                              ("%d instrs" % f.instructions) if f else "?"))
    for va, disp, txt in sorted(v, key=lambda r: r[1]):
        print("     0x%08X  +0x%X  %s" % (va, disp, txt))
print("\n(%d groups write >= 3 slots)" % len(rows))
