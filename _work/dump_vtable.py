"""Throwaway: dump a vtable with the function behind each slot, and name the class if the table
contains an MtDti getter (mov eax,<DTI>; ret) for one of the DTI addresses the engine registers.

    python dump_vtable.py 0x152D620
"""
import struct
import sys

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze, dti, pe                                  # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

an = analyze.load_analysis(CACHE, EXE)
img = an.img
dtis = {d.address: d.name for d in dti.enumerate_dtis(img)}

for arg in sys.argv[1:]:
    vt = int(arg, 0)
    off = img.rva_to_off(vt - img.image_base)
    print("=== vtable 0x%08X" % vt)
    slots = []
    for i in range(60):
        v = struct.unpack_from("<I", img.data, off + 4 * i)[0]
        if not (0x401000 <= v < 0x1511000):
            break
        slots.append(v)
    for i, v in enumerate(slots):
        f = an.functions.get(v)
        note = ""
        # a getter is `mov eax,<imm>; ret` (or a jmp to the real body)
        for back in (0,):
            ins = an.decode_at(v)
            if ins is not None and ins.mnemonic == "mov" and ins.operands and \
               ins.operands[0].kind == "reg" and ins.operands[0].text == "eax" and \
               ins.operands[1].kind == "imm":
                addr = int(ins.operands[1].text, 0)
                if addr in dtis:
                    note = "   <-- MtDti getter for %s (0x%08X)" % (dtis[addr], addr)
        print("   slot %2d (+0x%02X)  0x%08X  %s%s"
              % (i, 4 * i, v, ("%d instrs" % f.instructions) if f else "<not analysed>", note))
