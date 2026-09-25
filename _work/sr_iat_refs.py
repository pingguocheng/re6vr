#!/usr/bin/env python3
"""Stereo-render investigation: step 2 - raw byte search for IAT slot references.

The first scan (decoded-instruction operands) found nothing, which is either true or a
coverage hole. A raw search for the 4-byte little-endian VA of each IAT slot decides which:
every `mov reg,[0x...]` / `call [0x...]` / `push dword [0x...]` reaches the slot through an
absolute 32-bit displacement, so the bytes must be present somewhere in .text if it is used.
"""
from __future__ import annotations

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(os.path.dirname(HERE), "scripts")
sys.path.insert(0, SCRIPTS)

from disasm_lib import pe  # noqa: E402

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
img = pe.Image.load(EXE)

targets = {}
for i in img.imports:
    targets[img.image_base + i.iat_rva] = str(i)

# also the whole IAT block, to see what a raw dump around it looks like
print("== raw byte occurrences of each d3d9-related IAT slot VA ==")
for va in sorted(targets):
    name = targets[va]
    if "d3d" not in name.lower() and "dinput" not in name.lower():
        continue
    pat = struct.pack("<I", va)
    hits = []
    for s in img.sections:
        if not s.is_executable:
            continue
        blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
        pos = 0
        while True:
            j = blob.find(pat, pos)
            if j < 0:
                break
            hits.append(img.image_base + s.va + j)
            pos = j + 1
    print("  %08X %-24s -> %d hit(s)" % (va, name, len(hits)))
    for h in hits[:20]:
        print("        %08X" % h)

# Direct3DCreate9 in particular: what surrounds each hit
print()
print("== context around each Direct3DCreate9 IAT hit ==")
dc9 = [img.image_base + i.iat_rva for i in img.imports if i.name == "Direct3DCreate9"]
for va in dc9:
    pat = struct.pack("<I", va)
    for s in img.sections:
        if not s.is_executable:
            continue
        blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
        pos = 0
        while True:
            j = blob.find(pat, pos)
            if j < 0:
                break
            hva = img.image_base + s.va + j
            print("  hit at %08X, bytes:" % hva)
            print("   ", blob[max(0, j - 8):j + 12].hex(" "))
            pos = j + 1
