#!/usr/bin/env python3
"""Stereo-render investigation: step 12 - every import call, with nearby string arguments.

RE6 calls small numbers of imported functions through `call dword ptr ds:[IAT]` (the direct
form). Listing every such site with the callee name, plus any `push <string>` in the preceding
few instructions, maps the engine's start-up / frame code by its assert strings even where the
recursive descent did not decode a function.
"""
from __future__ import annotations

import os
import re
import sys

import sr_lib
from disasm_lib import pe  # noqa: E402
from disasm_lib import analyze  # noqa: E402

IMG = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bh6_analysis.json")

RE_IAT = re.compile(r"call dword ptr ds:\[(0x[0-9a-fA-F]+)\]")
RE_PUSH_STR = re.compile(r"push (0x[0-9a-fA-F]+)")


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "--xdata":
        # xref-by-data: search the whole image for dword referrers of a given string VA is done
        # elsewhere; here we just dump which IAT slots are called and from where.
        pass
    img = pe.Image.load(IMG)
    iat = {img.image_base + rva: imp for rva, imp in
           ((i.iat_rva, i) for i in img.imports)}
    an = analyze.load_analysis(CACHE, IMG)
    print("cache functions %d" % len(an.functions))

    hits = []
    for fn in an.functions.values():
        rows = []
        for va in sorted(fn.blocks):
            ins = an.decode_at(va)
            if ins is None or not ins.ok:
                continue
            rows.append((va, ins.text()))
        for i, (va, text) in enumerate(rows):
            m = RE_IAT.match(text)
            if not m:
                continue
            slot = int(m.group(1), 16)
            # gather preceding pushes and any string they name
            strs = []
            for k in range(max(0, i - 8), i):
                pm = RE_PUSH_STR.search(rows[k][1])
                if pm:
                    s = img.is_printable_string_at(int(pm.group(1), 16) - img.image_base, 4)
                    if s:
                        strs.append(s[:70])
            hits.append((va, fn.start, iat.get(slot, "?"), strs))
    print("import call sites found: %d" % len(hits))
    for va, fs, imp, strs in sorted(hits):
        print("  %08X fn %08X  %-34s %s" % (va, fs, str(imp), ("| " + " | ".join(strs)) if strs else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
