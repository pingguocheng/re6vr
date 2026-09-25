#!/usr/bin/env python3
"""Stereo-render investigation: step 1 - d3d9 / dxgi / d3d9-related imports and their IAT refs.

Lists every import from d3d9.dll (and a few others of interest) with its IAT VA, then finds
every decoded instruction that reads that IAT slot (the `call dword ptr [0x...]` form, which
for a real import *is* how an indirect call appears).
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(os.path.dirname(HERE), "scripts")
sys.path.insert(0, SCRIPTS)

from disasm_lib import pe, analyze  # noqa: E402

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
CACHE = os.path.join(os.path.dirname(HERE), "_work", "bh6_analysis.json")

img = pe.Image.load(EXE)
print("== all imported DLLs ==")
by_dll: dict[str, int] = {}
for imp in img.imports:
    by_dll[imp.dll.lower()] = by_dll.get(imp.dll.lower(), 0) + 1
for k in sorted(by_dll):
    print("  %-30s %d" % (k, by_dll[k]))

print()
print("== d3d9 / d3dx imports with IAT VA ==")
interesting = [i for i in img.imports if "d3d" in i.dll.lower() or "dxgi" in i.dll.lower()]
for i in sorted(interesting, key=lambda x: (x.dll.lower(), x.name or "")):
    print("  %08X  %-24s %s" % (img.image_base + i.iat_rva, i.dll, i.name or ("#%d" % i.ordinal)))

print()
print("== non-d3d imports containing d3d-ish names ==")
for i in img.imports:
    if "d3d" in (i.name or "").lower():
        print("  %08X  %-24s %s" % (img.image_base + i.iat_rva, i.dll, i.name))

an = analyze.load_analysis(CACHE, EXE)
if an is None:
    print("no analysis cache")
    sys.exit(1)

# Map: IAT VA -> list of instruction VAs that reference it.
want = {}
for i in img.imports:
    if "d3d" in i.dll.lower():
        want[img.image_base + i.iat_rva] = i

print()
print("== code references to d3d9 exports (decoded instructions only) ==")
refs: dict[int, list[int]] = {}
for va, ins in an.decoded.items():
    for o in ins.operands:
        if o.kind == "mem" and o.target in want:
            refs.setdefault(o.target, []).append(va)
    for t in ins.jump_targets:
        if t in want:
            refs.setdefault(t, []).append(va)

for t in sorted(refs):
    imp = want[t]
    sites = sorted(refs[t])
    print("  %-30s IAT %08X  %d site(s)" % (str(imp), t, len(sites)))
    for s in sites[:20]:
        fn = an.function_of(s) if hasattr(an, "function_of") else None
        print("        site %08X" % s)
    if len(sites) > 20:
        print("        ... %d more" % (len(sites) - 20))
