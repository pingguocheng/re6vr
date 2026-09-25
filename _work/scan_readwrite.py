"""Throwaway: who READS and who WRITES the two 8-entry look-at arrays inside sBioCamera?

sBioCamera has two parallel arrays of 8 camera entries, stride 0x40, layout
{pos +0x00, target +0x10, up +0x20, fov +0x30, near +0x34, far +0x38}:

    +0xE30   mCameraOrg[8]   the array the property table names  (0xE30..0x1028)
    +0x1030  unnamed[8]      the array 0x4F9A30 copies OUT of   (0x1030..0x1228)

0x4F9950 (called from uCameraCtrl 0x60C9F0) writes mCameraOrg[i] from a source object's
pose; 0x4F9A30 reads the +0x1030 array back into that object. So "which function reads
+0xE30 / writes +0x1030" answers where the renderer's camera comes from.

Classification is by operand role:
  store  = the displacement appears as the destination of a writing mnemonic
  read   = the displacement appears as a source operand
Nothing here proves a base register holds an sBioCamera; the grouping by function and the
number of distinct offsets touched is what makes a candidate stand out.
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


def offs_of(base):
    s = {}
    for i in range(8):
        for e in ENTRY:
            s[base + 0x40 * i + e] = i
    return s


ORG = offs_of(0xE30)
GRP = offs_of(0x1030)

an = analyze.load_analysis(CACHE, EXE)

stores = defaultdict(list)     # fn -> [(va, mnemonic, reg, disp, which)]
reads = defaultdict(list)

for fn, ins in analyze.iter_instructions(an):
    if not ins.operands:
        continue
    dst = ins.operands[0]
    # A memory operand is a STORE only when it is operand 0 of a writing mnemonic. One-operand
    # instructions (`fld [mem]`, `push [mem]`) have the memory operand in slot 0 as a SOURCE -
    # getting this wrong is how the first version of this scan missed 0x4F9A30's reads.
    writing = ins.mnemonic in WRITE_MN and not ins.mnemonic.startswith(("cmp", "test"))
    for k, o in enumerate(ins.operands):
        if o.kind != "mem" or (o.disp not in ORG and o.disp not in GRP):
            continue
        which = "ORG" if o.disp in ORG else "GRP"
        idx = ORG.get(o.disp, GRP.get(o.disp))
        if k == 0 and writing:
            stores[fn.start].append((ins.va, ins.mnemonic, o.base, o.disp, which, idx))
        else:
            reads[fn.start].append((ins.va, ins.mnemonic, o.base, o.disp, which, idx))


def report(title, table):
    print("=" * 100)
    print(title)
    rows = []
    for start, hits in table.items():
        orgs = {h[3] for h in hits if h[4] == "ORG"}
        grps = {h[3] for h in hits if h[4] == "GRP"}
        rows.append((len(orgs) + len(grps), start, orgs, grps, hits))
    rows.sort(key=lambda r: -r[0])
    for n, start, orgs, grps, hits in rows:
        f = an.functions.get(start)
        idxs = sorted({h[5] for h in hits if h[5] is not None})
        print("  fn 0x%08X  %s  ORG=%d offsets GRP=%d offsets  entries %s"
              % (start, ("%d instrs" % f.instructions) if f else "?",
                 len(orgs), len(grps), idxs))
        for va, mn, reg, disp, which, idx in hits[:8]:
            print("        0x%08X  %-8s [%s+0x%X]  %s[%s]" % (va, mn, reg, disp, which, idx))
        if len(hits) > 8:
            print("        ... %d more" % (len(hits) - 8))
    print("  (%d functions total)" % len(table))


report("STORES into the two arrays", stores)
print()
report("READS from the two arrays", reads)
