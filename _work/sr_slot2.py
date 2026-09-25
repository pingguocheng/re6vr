#!/usr/bin/env python3
"""Stereo-render investigation: step 11 - every writer of the d3d9 device vtable slots.

The engine reaches Present/EndScene/BeginScene/Clear through the device's vtable. Because the
recursive descent does not cover the whole renderer, a *raw* 4-byte-displacement search for
`8B xx 44 00 00 00` (mov reg,[reg+0x44]) would produce false hits inside immediates, so this
walks decoded instruction boundaries only, but over EVERY function in the cache, and prints
each hit with its surrounding window. It also prints the offsets seen, so an unexpected
distribution is visible rather than filtered away.
"""
from __future__ import annotations

import re
import sys
from collections import Counter

import sr_lib

RE_MOV = re.compile(r"^mov\s+([a-z0-9]+),\s*(?:\w+ ptr )?\[([a-z0-9]+)(?:\+(-?0x[0-9a-f]+))?\]$")
RE_MOV_ANY = re.compile(r"\[\s*([a-z0-9]+)\s*\+\s*(0x[0-9a-f]+)\s*\]")


def main() -> int:
    want = set(int(a, 0) for a in sys.argv[1:]) or {0x44, 0xA8}
    funcs = sr_lib.build_all()
    print("scanning %d functions / %d instructions for slots %s"
          % (len(funcs), sum(len(r) for _, _, r in funcs),
             ", ".join("0x%X" % w for w in sorted(want))))
    cnt = Counter()
    for start, end, rows in funcs:
        for i, (va, text, is_call, is_jmp, ln) in enumerate(rows):
            m = RE_MOV.match(text)
            if not m:
                continue
            dst, base, disp = m.group(1), m.group(2), m.group(3)
            if dst == base or disp is None:
                continue
            d = int(disp, 16)
            if d not in want:
                continue
            cnt[d] += 1
            print("=== %08X in fn %08X : %s" % (va, start, text))
            for k in range(max(0, i - 3), min(len(rows), i + 4)):
                r = rows[k]
                print("    %s %08X  %s" % (">>" if k == i else "  ", r[0], r[1]))
            print()
    print("counts: %s" % dict(cnt))
    return 0


if __name__ == "__main__":
    sys.exit(main())
