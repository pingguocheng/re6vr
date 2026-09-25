"""The MtDti class map for BH6.exe: every class name -> its static object address.

This is the anchor the earlier sessions concluded RE6 did not have. Two things were wrong
before:

  1. `dti_walk.py` looked for a dword equal to `name_address - 4`, i.e. it expected `name` at
     object+0. In MT Framework `name` is at object+4, and the field at object+0 is the
     vtable - so the scan was searching for the wrong value at the wrong place.
  2. The objects live in `.data` BSS (virtual size > raw size), so they have no bytes on
     disk. The old scripts read them anyway - `rva_to_off` did not check rawsize at the time,
     so it returned bytes belonging to another part of the file. "No dword points at the
     class name" was therefore an artefact of reading the wrong bytes.

The classes *are* statically described: every registration compiles to a fixed instruction
sequence naming the class and giving its static address, which is what this module extracts.

Layout of one MtDti object (32 bytes, which is why the registrations are 0x20 apart):

    +0x00  vtable           (written by the caller after the registrar returns)
    +0x04  name             const char*
    +0x08  ...
    +0x14  0
    +0x18  packed: size | hash bits   (read by the resolver at 0xE67C50)
    +0x1C  parent MtDti*    (0 for a root class)
"""
from __future__ import annotations

import os
import struct
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from disasm_lib import pe                                    # noqa: E402

REGISTRAR = 0xE67920
# The two instructions that bracket a registration. Every one of the 3372 in BH6.exe uses
# this exact sequence, emitted by the same template in the engine's class macros.
CALL_OPCODE = 0xE8
MOV_ECX_OPCODE = 0xB9


@dataclass
class Dti:
    address: int          # the static MtDti object (a .data BSS address)
    name: str
    descriptor: int       # the pushed pointer: the PARENT class's MtDti (0 for a root class)
    size: int             # instance size in bytes
    site: int             # where the registration code lives

    def __str__(self) -> str:
        return "0x%08X size=0x%-6X %s" % (self.address, self.size, self.name)


def _string_at(img: pe.Image, va: int) -> str:
    if not va or not img.is_mapped_rva(va - img.image_base):
        return ""
    return img.is_printable_string_at(va - img.image_base, minlen=2)


def enumerate_dtis(img: pe.Image) -> list[Dti]:
    """Find every MtDti registration by its call pattern."""
    out: list[Dti] = []
    for s in img.code_sections():
        blob = img.read_rva(s.va, min(s.vsize, len(img.data)))
        base = img.image_base + s.va
        for i in range(len(blob) - 12):
            if blob[i] != MOV_ECX_OPCODE or blob[i + 5] != CALL_OPCODE:
                continue
            rel = struct.unpack_from("<i", blob, i + 6)[0]
            if base + i + 10 + rel != REGISTRAR:
                continue
            cursor = i
            pushes = []
            for _ in range(3):
                cursor -= 5
                if cursor < 0 or blob[cursor] != 0x68:
                    break
                pushes.append(struct.unpack_from("<I", blob, cursor + 1)[0])
            if len(pushes) != 3:
                continue
            name_va, desc_va, size = pushes
            name = _string_at(img, name_va)
            if not name:
                continue
            out.append(Dti(struct.unpack_from("<I", blob, i + 1)[0], name, desc_va, size,
                           base + i))
    return out


def class_map(img: pe.Image | None = None) -> dict[int, Dti]:
    """{static MtDti address: Dti} for the whole image. Scans once, ~2 seconds."""
    if img is None:
        img = pe.Image.load(DEFAULT_EXE)
    return {d.address: d for d in enumerate_dtis(img)}


def hierarchy(img: pe.Image, root_name: str, max_depth: int = 4) -> None:
    """Print the class tree under `root_name`, using the parent DTI links."""
    by_addr = class_map(img)
    by_name = {d.name: d for d in by_addr.values()}
    root = by_name.get(root_name)
    if root is None:
        print("no class named %r" % root_name)
        return
    children: dict[int, list[Dti]] = {}
    for d in by_addr.values():
        children.setdefault(d.descriptor, []).append(d)

    def walk(d: Dti, depth: int) -> None:
        print("  " * depth + "%-34s 0x%08X  size=0x%X" % (d.name, d.address, d.size))
        if depth >= max_depth:
            return
        for c in sorted(children.get(d.address, []), key=lambda x: x.name):
            walk(c, depth + 1)

    walk(root, 0)


DEFAULT_EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

if __name__ == "__main__":
    image = pe.Image.load(DEFAULT_EXE)
    if len(sys.argv) > 1 and sys.argv[1] == "--tree":
        hierarchy(image, sys.argv[2] if len(sys.argv) > 2 else "uCameraBase")
    else:
        dtis = enumerate_dtis(image)
        print("%d MtDti classes" % len(dtis))
        needle = sys.argv[1] if len(sys.argv) > 1 else ""
        by_addr = {d.address: d for d in dtis}
        for d in sorted(dtis, key=lambda x: x.address):
            if not needle or needle.lower() in d.name.lower():
                parent = by_addr.get(d.descriptor)
                print("  %s  parent=%s  reg@0x%08X"
                      % (d, parent.name if parent else "-", d.site))
