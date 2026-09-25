#!/usr/bin/env python3
"""Stereo-render investigation: step 26 - who touches the mViewportCamera region.

mViewportCamera sits at sBioCamera+0x12A0, 8 entries of 0x60, matrix at entry+0x10. Any code
that reads entry k's matrix must use a displacement in 0x12B0..0x161F (for k=0..7) or compute
address = base + 0x60*k. This walks decoded instruction boundaries in the cache over the whole
image looking for BOTH: a displacement in the region, and the constants 0x12A0 / 0x60 in the
same function. Everything is reported with the function and the instruction text.
"""
from __future__ import annotations

import re
import sys
from collections import defaultdict

import sr_lib

RE_DISP = re.compile(r"\[[a-z0-9]+\+(0x[0-9a-f]+)\]")


def main() -> int:
    funcs = sr_lib.build_all()
    lo, hi = 0x12A0, 0x1620
    per_fn = defaultdict(list)
    for start, end, rows in funcs:
        for va, text, is_call, is_jmp, ln in rows:
            for m in RE_DISP.finditer(text):
                d = int(m.group(1), 16)
                if lo <= d < hi:
                    per_fn[start].append((va, text, d))
                    break
    print("== functions with a displacement in 0x%X..0x%X ==" % (lo, hi))
    for start in sorted(per_fn):
        rows = per_fn[start]
        # only interesting if the function also mentions 0x12A0 or the 0x60 stride, OR the
        # displacement is not a plausible field of some other class (we cannot tell, so print
        # everything but flag the ones with 0x12A0/0x60)
        alltxt = " ".join(t for _, t, _ in rows)
        interesting = ("0x12A0" in alltxt) or any(
            "0x60" in t for _, t, _ in rows)
        print("  fn %08X  %d hit(s)%s" % (start, len(rows), "   <== also uses 0x12A0/0x60" if interesting else ""))
        for va, text, d in rows[:14]:
            print("      %08X  [..+0x%X]  %s" % (va, d, text))
        if len(rows) > 14:
            print("      ... %d more" % (len(rows) - 14))
    return 0


if __name__ == "__main__":
    sys.exit(main())
