"""Dump the bytes around the "vtable" addresses build_vtables.py exported.

Why: the live sBioCamera object found at runtime (this = 1F47A060) has 0x0151A394 in its first
dword, while `bh6_vtables.txt` says the class's vtable is 0x0151A380 - exactly 0x14 bytes earlier.
A 0x14 difference is suspicious in one specific way: the static fixture the runtime scanner keeps
matching (78B38B50) reads

    +0x00 0151A380   +0x04 017C3164   +0x08 00001620   +0x0C 78B38C60   +0x10 0152C3C0   +0x14 018702E4

i.e. the *next* class descriptor's vtable/record pair begins at +0x14. If 0x0151A380 were the
vtable, its slots would be code pointers; if it is a descriptor, its dwords are the values above.
So: read the raw image and print both neighbourhoods, then say which one is a table of code.

Usage: python _work\\dump_classdesc.py [VA ...]
"""
from __future__ import annotations

import os
import struct
import sys

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
IMAGE_BASE = 0x400000

DEFAULT_VAS = [0x0151A380, 0x0151A394, 0x0152C3C0, 0x0152C3D4, 0x017C3164, 0x018702E4]


def sections(data: bytes):
    """(name, va, vsize, raw_off, raw_size) for each PE section."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0", "not a PE"
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


def va_to_off(secs, va: int):
    rva = va - IMAGE_BASE
    for name, sva, vsize, raw_off, raw_size in secs:
        if sva <= rva < sva + max(vsize, raw_size):
            delta = rva - sva
            if delta >= raw_size:
                return None, name
            return raw_off + delta, name
    return None, "?"


def main() -> int:
    vas = [int(a, 16) for a in sys.argv[1:]] or DEFAULT_VAS
    with open(EXE, "rb") as fh:
        data = fh.read()
    secs = sections(data)
    print("image %s, %d bytes, %d sections: %s"
          % (os.path.basename(EXE), len(data), len(secs),
             ", ".join("%s@%X" % (s[0], s[1]) for s in secs)))
    for va in vas:
        off, sec = va_to_off(secs, va)
        if off is None:
            print("\n%08X: not in any raw section (mapped as %s)" % (va, sec))
            continue
        print("\n%08X (section %s, file 0x%X):" % (va, sec, off))
        chunk = data[off - 16:off + 64]
        for i in range(0, len(chunk), 16):
            row_va = va - 16 + i
            dwords = struct.unpack_from("<4I", chunk, i)
            floats = struct.unpack_from("<4f", chunk, i)
            marker = "  <== here" if row_va <= va < row_va + 16 else ""
            print("  %08X  %s  | %s%s"
                  % (row_va,
                     " ".join("%08X" % d for d in dwords),
                     " ".join("%11.4g" % f for f in floats),
                     marker))
    return 0


if __name__ == "__main__":
    sys.exit(main())
