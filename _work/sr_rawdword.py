#!/usr/bin/env python3
"""Stereo-render investigation: step 10 - raw dword search for string pointers.

`re6dis.py xref` reports nothing for the D3D9 assert strings, which is either a real absence or
a decoder-coverage hole (the renderer code around 0x11xxxxx is not in the recursive-descent
roots). A raw little-endian dword search over the whole image settles it, and also finds the
pointer tables that hold these strings.
"""
from __future__ import annotations

import struct
import sys

import sr_lib
from disasm_lib import pe  # noqa: E402
import os

IMG = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"


def main() -> int:
    img = pe.Image.load(IMG)
    targets = [int(a, 0) for a in sys.argv[1:]]
    if not targets:
        print("usage: sr_rawdword.py <va> [va...]")
        return 2
    for va in targets:
        pat = struct.pack("<I", va)
        print("== references to %08X ==" % va)
        s0 = img.is_printable_string_at(va - img.image_base, 4)
        if s0:
            print("   string: %r" % s0)
        for s in img.sections:
            blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
            pos = 0
            n = 0
            while True:
                j = blob.find(pat, pos)
                if j < 0:
                    break
                print("   dword at %08X (%s)" % (img.image_base + s.va + j, s.name))
                n += 1
                pos = j + 1
                if n > 40:
                    print("   ...")
                    break
    return 0


if __name__ == "__main__":
    sys.exit(main())
