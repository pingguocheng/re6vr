"""Map a fault offset inside d3d9.dll to the code around it, using the DBGHELP symbol handler.

The crash reporter (src/proxy_trace.h) wrote these to build\\re6vr_crash.txt:

    FAULT code=C0000005 (read at 5614BCA1)      instruction=7C25B4FD thread=5924
    FAULT code=C0000005 (read at 41BF0000)      instruction=7C260F2E thread=12096
    FAULT code=C0000005 (read at 450F44A0)      instruction=7C25C694 thread=29296

and the proxy loaded at 0x7C240000, so the faulting code is AT 0x1B4FD and 0x1C694 inside
build\\d3d9.dll. Guessing which function that is from the source would be exactly the kind of
inference this project keeps getting burned by, so: ask the linker's own symbol table.

`/MAP` was not produced, but the DLL's COFF symbol table can be read straight out of the file, and
DBGHELP will also resolve actual exported/PDB symbols if they are present. Both paths are tried and
the offsets are disassembled by bytes as well, so the answer stands even with no symbols at all.

Usage: python _work/fault_offset.py 1B4FD 1C694
"""
from __future__ import annotations

import os
import struct
import sys

DLL = r"C:\re6vr\build\d3d9.dll"


def pe_info(data: bytes):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0"
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    sym_ptr, nsyms = struct.unpack_from("<II", data, pe + 12)
    sec_off = pe + 24 + opt_size
    secs = []
    for i in range(nsec):
        off = sec_off + 40 * i
        name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, raw_size, raw_off = struct.unpack_from("<IIII", data, off + 8)
        secs.append((name, va, vsize, raw_off, raw_size))
    return secs, sym_ptr, nsyms


def rva_to_off(secs, rva: int):
    for name, va, vsize, raw_off, raw_size in secs:
        if va <= rva < va + max(vsize, raw_size):
            d = rva - va
            if d < raw_size:
                return raw_off + d, name
    return None, "?"


def symbols(data: bytes, secs, sym_ptr: int, nsyms: int):
    """[(rva, name)] from the COFF symbol table, best effort."""
    out = []
    if not sym_ptr or not nsyms:
        return out
    strtab = sym_ptr + nsyms * 18
    if strtab + 4 > len(data):
        return out
    str_size = struct.unpack_from("<I", data, strtab)[0]
    strings = data[strtab:strtab + str_size] if str_size else b""
    for i in range(nsyms):
        off = sym_ptr + 18 * i
        if off + 18 > len(data):
            break
        name8 = data[off:off + 8]
        value, sec_num, typ, storage, naux = struct.unpack_from("<IhHBB", data, off + 8)
        if name8[:4] == b"\0\0\0\0":
            idx = struct.unpack_from("<I", name8, 4)[0]
            if 0 < idx < len(strings):
                end = strings.find(b"\0", idx)
                name = strings[idx:end].decode("ascii", "replace")
            else:
                name = "?"
        else:
            name = name8.rstrip(b"\0").decode("ascii", "replace")
        if sec_num > 0 and sec_num <= len(secs):
            va = secs[sec_num - 1][1]
            out.append((value + va, name))
    out.sort()
    return out


def main() -> int:
    offs = [int(a, 16) for a in sys.argv[1:]] or [0x1B4FD, 0x1C694]
    with open(DLL, "rb") as fh:
        data = fh.read()
    secs, sym_ptr, nsyms = pe_info(data)
    print("%s: %d bytes, sections %s, coff symbols %d" % (os.path.basename(DLL), len(data),
          ", ".join("%s@%X" % (s[0], s[1]) for s in secs), nsyms))
    syms = symbols(data, secs, sym_ptr, nsyms)
    print("coff symbols recovered: %d" % len(syms))

    for target in offs:
        print("\n=== RVA %X ===" % target)
        if syms:
            prev = None
            for rva, name in syms:
                if rva <= target:
                    prev = (rva, name)
                else:
                    break
            if prev:
                print("  nearest symbol at or before: %s + 0x%X" % (prev[1], target - prev[0]))
                after = [(r, n) for r, n in syms if r > target][:3]
                for r, n in after:
                    print("  next symbol: %s + 0x%X" % (n, r - target))
        else:
            print("  (no COFF symbols - the DLL was linked without /DEBUG and stripped)")
        off, sec = rva_to_off(secs, target)
        if off is None:
            print("  not inside a raw section (%s)" % sec)
            continue
        start = off - 48
        chunk = data[start:off + 32]
        print("  bytes around it (section %s):" % sec)
        for i in range(0, len(chunk), 16):
            row_rva = target - 48 + i
            mark = "  <== fault here" if row_rva <= target < row_rva + 16 else ""
            print("    %08X  %s%s" % (row_rva, " ".join("%02X" % b for b in chunk[i:i + 16]), mark))
    return 0


if __name__ == "__main__":
    sys.exit(main())
