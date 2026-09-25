"""Throwaway: which vtable slot is each camera function in?

A vtable slot index is the cheapest statement of a function's role (slot 0 dtor, slot 1 MtDti
getter, then declaration order). It also tells whether a function is reached only virtually -
which is why the xref tool reports "no caller" for the per-frame camera code.
"""
import sys

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze, pe, propmap                             # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

TARGETS = [int(a, 0) for a in sys.argv[1:]] or [
    0x4FF9B0, 0x4F9950, 0x4F9A30, 0x4FCB70, 0x503880, 0x4FA570, 0x60C9F0, 0x4F95A0, 0x4F9DF0]

an = analyze.load_analysis(CACHE, EXE)
img = an.img

# class name per vtable (from the earlier build_vtables run)
NAMES = {}
try:
    with open(r"C:\re6vr\_work\bh6_vtables.txt", encoding="utf-8") as fh:
        for line in fh:
            p = line.split()
            if len(p) >= 4:
                NAMES[int(p[0], 16)] = (p[3], int(p[1], 16), int(p[2], 16))
except OSError:
    pass

tables = list(propmap.scan_vtables(img))
print("scanned %d pointer tables" % len(tables))
for t, slots in tables:
    for target in TARGETS:
        if target in slots:
            i = slots.index(target)
            cls = NAMES.get(t)
            print("  0x%08X  slot %2d (vtable+0x%X)  in table 0x%08X  %s"
                  % (target, i, 4 * i, t, ("class %s (size 0x%X)" % (cls[0], cls[2])) if cls else ""))
print()
for target in TARGETS:
    hits = [(t, slots.index(target)) for t, slots in tables if target in slots]
    if not hits:
        print("  0x%08X  not in any pointer table" % target)
