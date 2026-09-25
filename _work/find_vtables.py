"""Locate the real vtables of the camera classes by scanning .rdata for the known slot values.

Why this exists: `bh6_vtables.txt` (from build_vtables.py) lists 0x0151A380 as sBioCamera's
vtable, but a live camera object found at runtime holds 0x0151A394 in its vptr -- 0x14 bytes
later. 0x0151A394 is also, as it happens, the return address pushed by `call 0x004FF990` when the
instruction after the call is 0x004FF980+0x14... so the value in the object header needs a real
explanation, not a guess.

What is actually known and can be checked against the raw image:
  * the hooked writer is at 0x004FF9B0 and it IS a slot of sBioCamera (README: "sBioCamera vtable
    slot 10");
  * 0x004FF9B0 appears in the image at 0x0151A3A8 only (see dump_classdesc.py output);
  * if 0x0151A3A8 is slot 10 then the table's first slot is 0x0151A3A8 - 10*4 = 0x0151A380, and
    the vptr a live object carries should be 0x0151A380, not 0x0151A394.

So either the table at 0x0151A380 is a vtable (slots 0..10 = 0x004FA430, 0x00E17210, ...,
0x004FF9B0 at 0x0151A3A8) and the live object's first dword is NOT its vptr, or the table's slots
are shifted by 5 and the live vptr is right. This script settles it the only way that is
independent of both: it searches the whole .rdata section for each camera class's most distinctive
function address (the DTI-registration function from the README table, which is by construction
reachable from that class's own code) and reports every table-like run it appears in, with the
surrounding slots printed so a vtable can be recognised (a vtable is a run of code pointers into
.text, and the entry points after its slot 10 usually include the same destructor address).

Usage: python _work/find_vtables.py
"""
from __future__ import annotations

import struct

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
IMAGE_BASE = 0x400000

# class -> a VA that must appear in (or be reachable from) its vtable. The registration RVAs come
# from the README table (RVA + 0x400000); 0x004FF9B0 is the hooked mCameraOrg writer, known to be
# sBioCamera vtable slot 10.
PROBES = {
    "sBioCamera": [0x004FF9B0],                       # writer, README: slot 10
    "sBioCamera(ViewportCamera)": [0x004FFA7D - 0x7D + 0x7D],   # placeholder, replaced below
}
PROBES = {
    "sBioCamera": [0x004FF9B0],
    "uCameraCtrl": [],                                 # filled from registration points below
}

# Registration points (VA) from the README's table: the `push imm32 <class name>` site for each
# class. The function containing the site is the class's DTI registration function, which is
# normally one of the vtable slots of that class.
REGISTRATION = {
    "sBioCamera": 0x400000 + 0x104DA40,
    "sBioCamera::ViewportCamera": 0x400000 + 0x104DA7D,
    "uCameraCtrl": 0x400000 + 0x1053E30,
    "uCameraBase": 0x400000 + 0x1053860,
    "cCameraParam": 0x400000 + 0x104DABD,
    "uCamera": 0x400000 + 0x10D61B0,
    "uFreeCamera": 0x400000 + 0x10D6230,
    "sCamera::Viewport": 0x400000 + 0x10D2AD0,
}

TEXT_LO, TEXT_HI = 0x00401000, 0x01513000


def sections(data: bytes):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    sec_off = pe + 24 + opt_size
    out = []
    for i in range(nsec):
        off = sec_off + 40 * i
        name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, raw_size, raw_off = struct.unpack_from("<IIII", data, off + 8)
        out.append((name, va, vsize, raw_off, raw_size))
    return out


def main() -> int:
    with open(EXE, "rb") as fh:
        data = fh.read()
    secs = sections(data)
    rdata = next(s for s in secs if s[0] == ".rdata")
    _name, rva, vsize, raw_off, raw_size = rdata
    base_va = IMAGE_BASE + rva
    blob = data[raw_off:raw_off + raw_size]
    print(".rdata: VA %08X..%08X (%d bytes of %d virtual)"
          % (base_va, base_va + raw_size, len(blob), vsize))

    for cls, reg_va in REGISTRATION.items():
        needle = struct.pack("<I", reg_va)
        hits = []
        start = 0
        while True:
            i = blob.find(needle, start)
            if i < 0:
                break
            hits.append(i)
            start = i + 1
        print("\n%s: registration VA %08X appears %d time(s) in .rdata" % (cls, reg_va, len(hits)))
        for i in hits[:6]:
            va = base_va + i
            # print the 13 dwords before and 4 after, to see whether this is a slot in a run of
            # code pointers (a vtable) and where the run starts.
            lo = max(0, i - 13 * 4)
            hi = min(len(blob), i + 5 * 4)
            dwords = struct.unpack_from("<%dI" % ((hi - lo) // 4), blob, lo)
            line = " ".join(("%08X" % d) for d in dwords)
            print("   at %08X  run[%08X..]: %s" % (va, base_va + lo, line))
        # direct probe for the writer, the one slot whose owning class is known for sure
    print("\n--- where 0x004FF9B0 (the hooked writer) appears in .rdata ---")
    needle = struct.pack("<I", 0x004FF9B0)
    start = 0
    while True:
        i = blob.find(needle, start)
        if i < 0:
            break
        va = base_va + i
        # find the start of the table: walk back while the dwords are code pointers
        j = i
        while j - 4 >= 0:
            d = struct.unpack_from("<I", blob, j - 4)[0]
            if TEXT_LO <= d < TEXT_HI:
                j -= 4
            else:
                break
        run = (i - j) // 4
        print("  slot %08X -> table starts %08X, slot index %d" % (va, base_va + j, run))
        start = i + 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
