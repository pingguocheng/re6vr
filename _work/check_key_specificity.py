"""Is the identity key specific enough to be worth a run? Check it against the image itself.

The probe looks for a dword equal to a class record's address (sBioCamera's is 0x017C3164) and
treats "an address whose +4 holds that value" as "an object of that class". That is only a good
key if the value is RARE in memory: if the image is full of copies of it, a match means nothing.

This measures how many copies exist *statically*, where what is found can be inspected - in the
executable's data sections and in the decoded instruction stream. The answer decides whether the
key is worth more runs.

Usage:  python _work\\check_key_specificity.py
"""
from __future__ import annotations

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))

from disasm_lib import analyze, pe                                      # noqa: E402

ROOT = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(ROOT, "bh6_analysis.json")
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

# sBioCamera's record, plus the other camera classes the probe keys on.
KEYS = {
    0x017C3164: "sBioCamera",
    0x0186E260: "sCamera",
    0x017D26F0: "uCameraCtrl",
    0x017D1DD0: "uCameraBase",
    0x017D2A00: "uCameraQFPS",
    0x017D1D44: "uCameraAnimation",
}


def main() -> int:
    img = pe.Image.load(EXE)
    base = img.image_base
    print("image %s (base %08X)" % (os.path.basename(EXE), base))

    for key, name in sorted(KEYS.items()):
        pat = struct.pack("<I", key)
        total = 0
        where = []
        for s in img.sections:
            blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
            pos = 0
            n = 0
            first = None
            while True:
                j = blob.find(pat, pos)
                if j < 0:
                    break
                n += 1
                total += 1
                if first is None:
                    first = base + s.va + j
                pos = j + 1
            if n:
                where.append("%s x%d (first %08X)" % (s.name, n, first))
        print("  %-16s (%08X): %d dword(s) in the image   %s"
              % (name, key, total, "; ".join(where) if where else "-"))

    # How often does the value appear as an IMMEDIATE in decoded code? Those are the places the
    # engine deliberately names the class, and they bound how much runtime data can hold it.
    an = analyze.load_analysis(CACHE, EXE)
    if an is None:
        print("no analysis cache; skipping the instruction-side count")
        return 0
    print()
    for key, name in sorted(KEYS.items()):
        hits = []
        for fn, ins in analyze.iter_instructions(an):
            for o in ins.operands:
                if o.kind == "imm" and o.text.lower() == hex(key):
                    hits.append((ins.va, fn.start, ins.text()))
        print("  %-16s: %d instruction(s) name it as an immediate" % (name, len(hits)))
        for va, fstart, text in hits[:4]:
            print("        %08X (fn %08X)  %s" % (va, fstart, text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
