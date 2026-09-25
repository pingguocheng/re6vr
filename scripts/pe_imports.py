#!/usr/bin/env python3
"""Dump a PE's import table with per-DLL function names (dumpbin substitute)."""
import struct
import sys


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def sections(d):
    pe = u32(d, 0x3C)
    nsec = u16(d, pe + 6)
    opt_size = u16(d, pe + 20)
    opt = pe + 24
    magic = u16(d, opt)
    dd = opt + (112 if magic == 0x20B else 96)
    out = []
    sec = opt + opt_size
    for i in range(nsec):
        name = d[sec + i * 40:sec + i * 40 + 8].rstrip(b"\0").decode(errors="replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, sec + i * 40 + 8)
        out.append((name, vaddr, max(vsize, rawsize), rawptr))
    return out, dd


def rva2off(secs, rva):
    for _, vaddr, size, rawptr in secs:
        if vaddr <= rva < vaddr + size:
            return rawptr + (rva - vaddr)
    return None


def cstr(d, off):
    end = d.index(b"\0", off)
    return d[off:end].decode(errors="replace")


def main(path):
    d = open(path, "rb").read()
    secs, dd = sections(d)
    imp_rva, imp_size = u32(d, dd + 8), u32(d, dd + 12)
    delay_rva = u32(d, dd + 13 * 8)
    print(f"{path}\n  import directory rva=0x{imp_rva:X} size={imp_size}")
    if imp_rva:
        o = rva2off(secs, imp_rva)
        while True:
            oft, tds, fwd, name_rva, first_thunk = struct.unpack_from("<IIIII", d, o)
            if not any((oft, tds, fwd, name_rva, first_thunk)):
                break
            dll = cstr(d, rva2off(secs, name_rva))
            funcs = []
            thunk_rva = oft or first_thunk
            to = rva2off(secs, thunk_rva)
            i = 0
            while to is not None:
                val = u32(d, to + i * 4)
                if val == 0:
                    break
                if val & 0x80000000:
                    funcs.append(f"#{val & 0xFFFF}")
                else:
                    no = rva2off(secs, val)
                    funcs.append(cstr(d, no + 2))
                i += 1
                if i > 400:
                    break
            print(f"  {dll}: {len(funcs)} imports")
            for f in funcs:
                print(f"      {f}")
            o += 20
    if delay_rva:
        print(f"  (delay-load directory present at rva 0x{delay_rva:X})")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        main(p)
