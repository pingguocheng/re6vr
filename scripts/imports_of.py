#!/usr/bin/env python3
"""List the functions BH6.exe imports from one DLL (read-only).

Usage: python imports_of.py dinput8
"""
import struct
import sys

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
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


def off(rva):
    for nm, va, vs, raw, rs in secs:
        if va <= rva < va + max(vs, rs):
            return raw + (rva - va)
    return None


def cstr(p):
    if p is None:
        return ""
    return data[p:p + 64].split(b"\0")[0].decode(errors="replace")


want = (sys.argv[1] if len(sys.argv) > 1 else "").lower()
imp_rva, imp_sz = struct.unpack_from("<II", data, pe + 24 + 104)
o = off(imp_rva)
k = 0
while True:
    ent = data[o + 20 * k: o + 20 * k + 20]
    if len(ent) < 20 or ent == b"\0" * 20:
        break
    oft, tds, fwd, name_rva, first = struct.unpack_from("<IIIII", ent, 0)
    dll = cstr(off(name_rva))
    if want and want not in dll.lower():
        k += 1
        continue
    print("=== %s" % dll)
    tbl = oft if oft else first
    p = off(tbl)
    if p is None:
        print("   (no thunk table)")
        k += 1
        continue
    j = 0
    while True:
        f = struct.unpack_from("<I", data, p + 4 * j)[0]
        if f == 0:
            break
        if f & 0x80000000:
            print("   ordinal %d" % struct.unpack_from("<H", data, p + 4 * j + 2)[0])
        else:
            print("   %s" % cstr(off(f + 2)))
        j += 1
    k += 1
