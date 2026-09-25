"""Throwaway: vtable slots of the camera functions, and where 0x4FF9B0 sits in the frame path.

A vtable slot index is the cheapest available statement of a function's *role*: slot 0 is the
destructor, slot 1 the MtDti getter, and the rest are virtual methods in declaration order. If
0x4FF9B0 is a virtual method of sBioCamera, its slot says whether it is an update, a draw, a
serialiser, or something a caller drives explicitly.
"""
import struct
import sys

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze, pe                                     # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

an = analyze.load_analysis(CACHE, EXE)
img = an.img
base = img.image_base

VT = 0x151A380
off = img.rva_to_off(VT - base)
slots = []
for i in range(40):
    v = struct.unpack_from("<I", img.data, off + 4 * i)[0]
    if not (0x401000 <= v <= 0x1511000):
        break
    slots.append(v)

DTI = 0x17C3164
getter = None
for i, v in enumerate(slots):
    f = an.functions.get(v)
    if f is None:
        continue
    for blk in f.blocks:
        ins = an.decode_at(blk)
        if ins and any(o.kind == "imm" and o.text.lower() == hex(DTI) for o in ins.operands):
            getter = i
print("vtable 0x%08X: %d slots; MtDti getter (0x%08X) at slot %s"
      % (VT, len(slots), DTI, getter))
for i, v in enumerate(slots):
    f = an.functions.get(v)
    n = f.instructions if f else 0
    print("   slot %2d  0x%08X  %s" % (i, v, ("%d instructions" % n) if f else "<not analysed>"))

print()
target = 0x4FF9B0
f = an.functions[target]
print("0x%08X: %d instructions. Its calls and what those are:" % (target, f.instructions))
for c in f.calls:
    cf = an.functions.get(c)
    print("   -> 0x%08X %s" % (c, ("%d instructions, called_by %d"
                                   % (cf.instructions, len(cf.called_by))) if cf else "?"))
print()
print("callers of 0x4F9950 (the Renderer-copy helper):")
for c in sorted(an.functions[0x4F9950].called_by):
    cf = an.functions.get(c)
    print("   0x%08X  %s  calls %s"
          % (c, ("%d instrs" % cf.instructions) if cf else "?",
             ", ".join("0x%08X" % x for x in (cf.calls[:6] if cf else []))))
