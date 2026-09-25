#!/usr/bin/env python3
"""Minimal PE inspection: architecture, exports, imports.

Used to verify the built proxy without dumpbin (not on PATH in this shell).
"""
import struct
import sys


def read_pe(path):
    with open(path, "rb") as f:
        data = f.read()

    if data[:2] != b"MZ":
        raise SystemExit(f"{path}: not a PE file")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        raise SystemExit(f"{path}: bad PE signature")
    machine, nsec, _, _, _, opt_size, chars = struct.unpack_from("<HHIIIHH", data, pe_off + 4)
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    is64 = magic == 0x20B
    dd_off = opt_off + (112 if is64 else 96)
    export_rva, export_size = struct.unpack_from("<II", data, dd_off)
    import_rva, import_size = struct.unpack_from("<II", data, dd_off + 8)

    sections = []
    sec_off = opt_off + opt_size
    for i in range(nsec):
        name, vsize, vaddr, rawsize, rawptr = struct.unpack_from("<8sIIII", data, sec_off + i * 40)
        sections.append((name.rstrip(b"\0").decode(), vaddr, vsize, rawptr, rawsize))

    def rva_to_off(rva):
        for _, vaddr, vsize, rawptr, rawsize in sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rawptr + (rva - vaddr)
        return None

    # --- exports ---
    exports = []
    if export_rva:
        eo = rva_to_off(export_rva)
        nnames = struct.unpack_from("<I", data, eo + 24)[0]
        names_rva = struct.unpack_from("<I", data, eo + 32)[0]
        no = rva_to_off(names_rva)
        for i in range(nnames):
            nrva = struct.unpack_from("<I", data, no + i * 4)[0]
            off = rva_to_off(nrva)
            end = data.index(b"\0", off)
            exports.append(data[off:end].decode())

    # --- imports ---
    imports = []
    if import_rva:
        io = rva_to_off(import_rva)
        while True:
            oft, tds, _, name_rva, iat = struct.unpack_from("<IIIII", data, io)
            if not any((oft, tds, name_rva, iat)):
                break
            no = rva_to_off(name_rva)
            end = data.index(b"\0", no)
            imports.append(data[no:end].decode())
            io += 20

    machine_name = {0x14C: "x86", 0x8664: "x64", 0xAA64: "arm64"}.get(machine, hex(machine))
    print(f"{path}")
    print(f"  machine   : {machine_name}")
    print(f"  subsystem : {'DLL' if chars & 0x2000 else 'EXE'}")
    print(f"  exports   : {len(exports)}")
    for e in sorted(exports):
        print(f"      {e}")
    print(f"  imports   : {', '.join(sorted(set(imports)))}")
    return exports


if __name__ == "__main__":
    for p in sys.argv[1:]:
        read_pe(p)
