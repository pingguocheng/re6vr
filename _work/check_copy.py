"""Throwaway: map the whole copy inside 0x4FF9B0 (source offsets -> mCameraOrg[0] fields).

The instruction stream shows a mirror: `[esi+0x1030..]` is copied into `[esi+0xE30..]`, i.e.
the field group that the property table calls `mCameraOrg[0]` is filled from a parallel group
0x1200 bytes higher. If that holds for all six fields *and* for all eight entries, then the
source group is the camera's input pose - which is where head tracking would write.
"""
import re
import sys
from collections import defaultdict

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

an = analyze.load_analysis(CACHE, EXE)
f = an.functions[0x4FF9B0]

# Collect store targets with a *constant* displacement, so slot arithmetic is visible.
rows = []
for va in sorted(f.blocks):
    ins = an.decode_at(va)
    if ins is None or not ins.operands:
        continue
    dst = ins.operands[0]
    if dst.kind != "mem" or dst.base != "esi" or not dst.disp:
        continue
    if ins.mnemonic not in ("mov", "movss", "movups", "movq", "fstp", "movd"):
        continue
    rows.append((dst.disp, ins.mnemonic, va))

dst_disps = sorted({d for d, _, _ in rows})
print("distinct store displacements via esi: %d" % len(dst_disps))
print("range: +0x%X .. +0x%X" % (min(dst_disps), max(dst_disps)))

# mCameraOrg[i] base is 0xE30 + 0x40*i (verified from the property table).
print()
print("mCameraOrg[i] targets that the function writes (property-table layout):")
LAYOUT = [(0x00, "cameraPos"), (0x10, "targetPos"), (0x20, "cameraUp"),
          (0x30, "fov"), (0x34, "nearPlane"), (0x38, "farPlane")]
for i in range(8):
    base = 0xE30 + 0x40 * i
    got = [d for d in dst_disps if base <= d < base + 0x40]
    if not got:
        continue
    names = []
    for off, nm in LAYOUT:
        names.append("%s%s" % (nm, "*" if base + off in got else "-"))
    print("   [%d] +0x%X: %s   (written: %s)"
          % (i, base, " ".join(names), " ".join("+0x%X" % d for d in got)))

print()
print("the parallel group +0x1030.. (source of the copy):")
for d in dst_disps:
    if 0x1000 <= d <= 0x1120:
        print("   +0x%X" % d, end="")
print()

# Pair source->destination by looking at each destination store: which displacement was read
# just before it?
print()
print("read->write pairs (source read immediately before the destination store):")
pairs = []
for disp, mn, va in rows:
    if not (0xE30 <= disp < 0xE30 + 0x40 * 8):
        continue
    prev = None
    for back in (2, 3, 4, 5, 6, 7, 8, 9, 10):
        p = analyze.instruction_at(an, va - back)
        if p is not None and p.va + p.length == va:
            prev = p
            break
    src = None
    if prev is not None and prev.operands:
        o = prev.operands[-1]
        if o.kind == "mem" and o.base == "esi":
            src = o.disp
    pairs.append((disp, src, mn))
for disp, src, mn in sorted(pairs):
    i = (disp - 0xE30) // 0x40
    off = (disp - 0xE30) % 0x40
    nm = dict(LAYOUT).get(off, "?")
    print("   mCameraOrg[%d].%-10s +0x%X  <-  %s"
          % (i, nm, disp, ("+0x%X" % src) if src is not None else "(not a plain [esi+x] read)"))
