"""Throwaway: which camera classes share the view-matrix getter at slot 18 (vtable+0x48)?

0x5F80B0 is `lea eax,[ecx+0x60]; lea edx,[ecx+0x70]; add ecx,0x50; call 0xE6FD20` - it builds a
look-at view matrix from the camera's own pose. If a class overrides slot 18 with its own getter,
hooking only 0x5F80B0 would miss that class. This prints slot 18 (and 11/9) for every camera-class
vtable in _work/bh6_vtables.txt.
"""
import struct
import sys

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
an = analyze.load_analysis(CACHE, EXE)
img = an.img

rows = []
with open(r"C:\re6vr\_work\bh6_vtables.txt", encoding="utf-8") as fh:
    for line in fh:
        p = line.split()
        if len(p) >= 4 and ("amera" in p[3]):
            rows.append((int(p[0], 16), p[3], int(p[2], 16)))

print("%-38s %-10s %-12s %-12s %-12s" % ("class", "vtable", "slot 9(+0x24)", "slot 11(+0x2C)",
                                         "slot 18(+0x48)"))
for vt, name, size in sorted(rows, key=lambda r: r[1]):
    off = img.rva_to_off(vt - img.image_base)
    slots = {}
    for i in (9, 11, 18):
        v = struct.unpack_from("<I", img.data, off + 4 * i)[0]
        slots[i] = v
    print("%-38s 0x%08X 0x%08X   0x%08X   0x%08X%s"
          % (name, vt, slots[9], slots[11], slots[18],
             "   <-- shared look-at getter" if slots[18] == 0x5F80B0 else ""))
