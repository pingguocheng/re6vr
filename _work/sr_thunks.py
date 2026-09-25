#!/usr/bin/env python3
"""Stereo-render investigation: step 3 - who calls the import thunks.

RE6 reaches d3d9 through `ff 25 <IAT>` thunks in a stub block at 0x13E9D60-ish, not through
`call dword ptr [IAT]`. So: locate every thunk address, then find every `call rel32` (E8) and
`jmp rel32` (E9) in the executable sections that lands on one, and report the containing
analysed function.
"""
from __future__ import annotations

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(os.path.dirname(HERE), "scripts")
sys.path.insert(0, SCRIPTS)

from disasm_lib import pe, analyze  # noqa: E402

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
CACHE = os.path.join(os.path.dirname(HERE), "_work", "bh6_analysis.json")

img = pe.Image.load(EXE)

# 1. find every `ff 25 <imm32>` thunk in the code sections
thunks: dict[int, str] = {}
for s in img.sections:
    if not s.is_executable:
        continue
    blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
    pos = 0
    while True:
        j = blob.find(b"\xff\x25", pos)
        if j < 0:
            break
        if j + 6 <= len(blob):
            tgt = struct.unpack_from("<I", blob, j + 2)[0]
            imp = img.iat.get(tgt - img.image_base)
            if imp is not None:
                thunks[img.image_base + s.va + j] = str(imp)
        pos = j + 1
print("== import thunks found: %d ==" % len(thunks))
for t in sorted(thunks):
    print("  %08X  -> %s" % (t, thunks[t]))

# 2. every E8/E9 in the code sections that lands on a thunk
print()
print("== direct call/jmp sites that land on a d3d9/dx thunk ==")
want = set(thunks)
by_target: dict[int, list[tuple[int, str]]] = {}
for s in img.sections:
    if not s.is_executable:
        continue
    blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
    base = img.image_base + s.va
    for j in range(len(blob) - 5):
        op = blob[j]
        if op != 0xE8 and op != 0xE9:
            continue
        rel = struct.unpack_from("<i", blob, j + 1)[0]
        tgt = base + j + 5 + rel
        if tgt in want:
            by_target.setdefault(tgt, []).append((base + j, "call" if op == 0xE8 else "jmp"))

an = analyze.load_analysis(CACHE, EXE)
starts = an.function_starts if an is not None else None


def fn_of(va):
    if not starts:
        return None
    import bisect
    i = bisect.bisect_right(starts, va) - 1
    if i < 0:
        return None
    st = starts[i]
    f = an.functions.get(st)
    if f is not None and va < f.end:
        return st
    return None


for t in sorted(by_target):
    sites = sorted(set(by_target[t]))
    print("  %s (thunk %08X): %d site(s)" % (thunks[t], t, len(sites)))
    for va, kind in sites[:40]:
        f = fn_of(va)
        print("      %08X %s   in fn %s" % (va, kind, ("%08X" % f) if f else "(unanalysed)"))
    if len(sites) > 40:
        print("      ... %d more" % (len(sites) - 40))
