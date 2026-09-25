"""Engine classes that are *looked up by identity* at a static address.

MT Framework's "get the object of class X" idiom compiles to a fixed instruction sequence:

    push <MtDti of class X>     ; 68 <imm32>
    call 0xE67C50               ; DTI hash lookup -> the class's static record
    mov  ecx, ds:[<global>]     ; context passed to the factory
    mov  edx, [eax]             ; the class record's vtable
    mov  edx, [edx+0x1C]        ; slot 4: the factory
    push ecx / push 0x10 / push <instance size>
    call edx

Scanning for `68 <address of an MtDti>` therefore enumerates every place the program asks for
a class by identity, and the `mov ecx, ds:[...]` that follows names the global it passes in.

WHAT THE GLOBAL IS, AND WHAT IT IS NOT
--------------------------------------
It is the *context/owner* the factory receives. In every case found it is `<MtDti> + 0x1C`,
which is a field inside the class's own static record.

It is NOT established that this global holds the live instance pointer. The field is written
by the registrar (0xE67920) as a hash of the class name, and nothing in the image writes an
instance pointer to it by absolute address - the 8 sites that mention 0x17D270C all *read* it.
So: these are the addresses the engine uses to reach a class's context, and they are the right
thing to watch under a debugger, but "singleton slot" would be an overclaim. The empirical
check is one memory read: if `[0x17D270C]` points at a 0x4B70-byte object whose vtable is
uCameraCtrl's, the claim is confirmed; until then it is a hypothesis with the address attached.
"""
from __future__ import annotations

import os
import struct
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from disasm_lib import pe                                     # noqa: E402
from disasm_lib.dti import enumerate_dtis                     # noqa: E402

REGISTRY_CALL = 0xE67C50
# The context global always sits here inside the class's static record. That is a fact about
# the registration template, not about what the field means at runtime.
CONTEXT_FIELD_OFFSET = 0x1C


@dataclass
class ClassLookup:
    name: str
    dti: int                # the class's static MtDti record
    context: int            # the global the factory call reads as its context argument
    size: int               # instance size in bytes, from the registration
    site: int               # a call site that creates it

    def __str__(self) -> str:
        return "%-34s dti=0x%08X context=0x%08X size=0x%X" % (
            self.name, self.dti, self.context, self.size)


def find_managers(img: pe.Image) -> list[ClassLookup]:
    dtis = {d.address: d for d in enumerate_dtis(img)}
    out: dict[int, ClassLookup] = {}
    for s in img.code_sections():
        text = img.read_rva(s.va, min(s.vsize, len(img.data)))
        base = img.image_base + s.va
        for i in range(len(text) - 16):
            if text[i] != 0x68:                     # push imm32
                continue
            val = struct.unpack_from("<I", text, i + 1)[0]
            d = dtis.get(val)
            if d is None:
                continue
            if text[i + 5] != 0xE8:                 # call rel32
                continue
            rel = struct.unpack_from("<i", text, i + 6)[0]
            if base + i + 10 + rel != REGISTRY_CALL:
                continue
            if d.address in out:
                continue
            out[d.address] = ClassLookup(d.name, d.address,
                                         d.address + CONTEXT_FIELD_OFFSET, d.size,
                                         base + i)
    return sorted(out.values(), key=lambda m: m.name)


def manager_map(img: pe.Image | None = None) -> dict[str, ClassLookup]:
    if img is None:
        img = pe.Image.load(DEFAULT_EXE)
    return {m.name: m for m in find_managers(img)}


DEFAULT_EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

if __name__ == "__main__":
    image = pe.Image.load(DEFAULT_EXE)
    mgrs = find_managers(image)
    needle = sys.argv[1] if len(sys.argv) > 1 else ""
    print("%d classes reached through a DTI factory call" % len(mgrs))
    for m in mgrs:
        if not needle or needle.lower() in m.name.lower():
            print("  " + str(m))
