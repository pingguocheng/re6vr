#!/usr/bin/env python3
"""Stereo-render investigation: step 13 - code coverage of the analysis cache by section.

`re6dis.py xref` and every block-based scan can only see what the recursive descent reached.
Before concluding "no reader exists", measure how much of .text was reached and where the gaps
are, so an absence can be reported with its confidence rather than as a fact.
"""
from __future__ import annotations

import os
import sys

import sr_lib
from disasm_lib import pe  # noqa: E402

IMG = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"


def main() -> int:
    img = pe.Image.load(IMG)
    print(img.summary())
    funcs = sr_lib.build_all()
    print("cached functions %d" % len(funcs))
    spans = [(s.va, s.va + s.vsize, s.name) for s in img.sections]
    for va0, va1, name in sorted(spans):
        covered = 0
        total = 0
        for start, end, rows in funcs:
            lo = max(start, va0)
            hi = min(end, va1)
            if hi > lo:
                covered += hi - lo
        print("  %-8s %08X..%08X  covered %d bytes" % (name, va0, va1, covered))
    # histogram of coverage per 1 MB of .text
    tex = [s for s in img.sections if s.name == ".text"]
    if tex:
        s = tex[0]
        base, size = s.va, s.vsize
        print()
        print(".text coverage per 1 MB window (base 0x%08X, size 0x%X):" % (base, size))
        for off in range(0, size, 0x100000):
            lo, hi = base + off, base + min(off + 0x100000, size)
            cov = 0
            for start, end, rows in funcs:
                a = max(start, lo)
                b = min(end, hi)
                if b > a:
                    cov += b - a
            print("   %08X  %6.1f%%  (%d/%d)" % (lo, 100.0 * cov / (hi - lo), cov, hi - lo))
    return 0


if __name__ == "__main__":
    sys.exit(main())
