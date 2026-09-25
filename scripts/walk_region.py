#!/usr/bin/env python3
"""Dump a region of BH6.exe with annotations: what each dword points at.

Written because two guesses about the engine's reflection code were both wrong, and each wrong
guess cost a run:
  * xrefs.py searched for an address in a table and found none, which was read as "unreachable";
  * a rigid 11-byte pattern for the property-registration idiom matched nothing.
The fix for both is to stop guessing the shape and look at the bytes.

Usage: python walk_region.py <rva> <bytes>
"""
import struct
import sys

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
IMAGE_BASE = 0x400000
data = open(EXE, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsz = struct.unpack_from("<H", data, pe + 20)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    nm = data[o:o + 8].rstrip(b"\0").decode(errors="replace")
    vs, va, rs, raw = struct.unpack_from("<IIII", data, o + 8)
    secs.append((nm, va, vs, raw, rs))
sec_by_rva = {}
for nm, va, vs, raw, rs in secs:
    sec_by_rva[nm] = (va, vs, raw, rs)


def which(rva):
    for nm, va, vs, raw, rs in secs:
        if va <= rva < va + max(vs, rs):
            return nm
    return "?"


def to_off(rva):
    for nm, va, vs, raw, rs in secs:
        if va <= rva < va + max(vs, rs):
            return raw + (rva - va)
    return None


def cstr_va(va):
    o = to_off(va - IMAGE_BASE)
    if o is None:
        return None
    s = data[o:o + 64].split(b"\0")[0]
    try:
        s = s.decode("ascii")
    except Exception:
        return None
    return s if all(32 <= ord(c) < 127 for c in s) and len(s) >= 3 else None


def describe(val):
    """What could this dword be, if read as a pointer into the module?"""
    s = cstr_va(val)
    if s:
        return "STRING %r" % s
    rva = val - IMAGE_BASE
    if 0 <= rva < 0x2000000:
        sec = which(rva)
        if sec != "?":
            b = data[to_off(rva):to_off(rva) + 6]
            if b[:1] == b"\x55":                      # push ebp - a function prologue
                return "-> .%s 0x%08X  (looks like a FUNCTION: %s)" % (sec, val, b.hex())
            return "-> .%s 0x%08X  (%s)" % (sec, val, b.hex())
    return ""


def main():
    rva = int(sys.argv[1], 0)
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 256
    off = to_off(rva)
    sec = which(rva)
    print("region rva 0x%08X in .%s, %d bytes" % (rva, sec, n))
    for i in range(0, n, 4):
        va = IMAGE_BASE + rva + i
        b = data[off + i:off + i + 4]
        val = struct.unpack("<I", b)[0]
        mark = ""
        if val == 0:
            mark = "(zero)"
        else:
            mark = describe(val)
        print("  +%03X  0x%08X  %-10s  %s" % (i, va, b.hex(), mark))


if __name__ == "__main__":
    main()
