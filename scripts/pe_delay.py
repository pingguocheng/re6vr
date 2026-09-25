#!/usr/bin/env python3
"""Report whether a PE imports a DLL eagerly or through the delay-load table."""
import struct
import sys


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def dirs(d):
    pe = u32(d, 0x3C)
    opt = pe + 24
    magic = u16(d, opt)
    dd = opt + (112 if magic == 0x20B else 96)
    n = u32(d, dd - 4) if magic == 0x20B else u32(d, dd - 4)
    out = {}
    names = {0: "export", 1: "import", 2: "resource", 12: "iat", 13: "delay_import",
             14: "com_descriptor"}
    for i in range(min(n, 16)):
        rva, size = u32(d, dd + i * 8), u32(d, dd + i * 8 + 4)
        out[names.get(i, f"dir{i}")] = (rva, size)
    return out


def sections(d):
    pe = u32(d, 0x3C)
    nsec = u16(d, pe + 6)
    opt_size = u16(d, pe + 20)
    sec = pe + 24 + opt_size
    out = []
    for i in range(nsec):
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, sec + i * 40 + 8)
        out.append((vaddr, max(vsize, rawsize), rawptr))
    return out


def rva2off(secs, rva):
    for vaddr, size, rawptr in secs:
        if vaddr <= rva < vaddr + size:
            return rawptr + (rva - vaddr)
    return None


for path in sys.argv[1:]:
    d = open(path, "rb").read()
    secs = sections(d)
    dd = dirs(d)
    print(path)
    for k in ("import", "delay_import", "iat"):
        rva, size = dd.get(k, (0, 0))
        print(f"  {k:13s} rva=0x{rva:06X} size={size}")
    rva, size = dd.get("delay_import", (0, 0))
    if rva:
        o = rva2off(secs, rva)
        count = 0
        while o is not None:
            attrs, name_rva = u32(d, o), u32(d, o + 4)
            if attrs == 0 and name_rva == 0:
                break
            no = rva2off(secs, name_rva)
            end = d.index(b"\0", no)
            print(f"      delay-load: {d[no:end].decode(errors='replace')}  attrs=0x{attrs:X}")
            count += 1
            o += 32
        if count == 0:
            print("      (delay table present but empty)")
