#!/usr/bin/env python3
"""Find every instruction that touches a given sRender field, by exact byte pattern.

Why this exists: the offline report of 2026-09-25 said the engine's own "Stereo" config flag is "a
driver-level interleaved stereo mode that does not give the mod two capturable pictures" - an
INFERENCE, marked as such, not a measurement. The mod has since paid for five crashed runs trying to
build its own two-view path. Before dismissing a switch the ENGINE ITSELF already reads, it is worth
30 seconds to find out what reads it. `re6dis.py xref` cannot answer this: the field is reached as
`[reg+disp32]` with the singleton in a register, so there is no address to xref - the displacement
bytes are the needle.

Usage:  python scripts\\field_refs.py 0x448EE0            # any [reg+disp] read/write
        python scripts\\field_refs.py 0x462694 --bytes    # also print the 4 bytes after each hit
"""
from __future__ import annotations

import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
IMAGE_BASE = 0x400000
# .text is the only section worth scanning here; the report's cheat sheet gives the mapping used
# below (file offset = RVA - 0x1000 + 0x400).
TEXT_RVA = 0x1000
TEXT_FILE = 0x400


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    field = int(sys.argv[1], 0)
    show_bytes = "--bytes" in sys.argv[1:]

    with open(EXE, "rb") as fh:
        data = fh.read()
    disp = struct.pack("<I", field)
    needle = bytes([0x86]) + disp          # [esi+disp32] - the sRender singleton is kept in esi
    needle2 = bytes([0x87]) + disp         # [edi+disp32]

    print("BH6.exe %d bytes; searching for [esi+0x%X] and [edi+0x%X]" % (len(data), field, field))
    total = 0
    for tag, nd in (("esi", needle), ("edi", needle2)):
        start = 0
        while True:
            i = data.find(nd, start)
            if i < 0:
                break
            start = i + 1
            va = i - TEXT_FILE + TEXT_RVA + IMAGE_BASE
            opcode = data[i - 3:i + 6]
            hexs = " ".join("%02X" % b for b in opcode)
            extra = ""
            if show_bytes:
                after = data[i + 5:i + 9]
                extra = "  following dword: %s" % " ".join("%02X" % b for b in after)
            print("  VA 0x%08X  ...%s  ([%s+0x%X])%s" % (va, hexs, tag, field, extra))
            total += 1
    print("%d reference(s)" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
