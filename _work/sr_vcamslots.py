#!/usr/bin/env python3
"""Stereo-render investigation: step 20 - callers of the camera vtable slots, from the cache.

`mov reg,[obj+SLOT]; call reg` is the form MSVC emits for virtual calls here (the
memory-operand form returns zero hits). Slot 18 (+0x48) is GetViewMatrix for the whole uCamera
family, slot 9 (+0x24) the per-frame update, slot 11 (+0x2C) sBioCamera::Update. Listing every
site for these slots shows which code drives cameras, and the enclosing function is the
candidate frame path.
"""
from __future__ import annotations

import re
import sys
from collections import defaultdict

import sr_lib

SLOTS = {
    0x24: "slot 9  (uCameraCtrl::update / uCamera::update)",
    0x2C: "slot 11 (sBioCamera::Update)",
    0x48: "slot 18 (uCamera GetViewMatrix)",
    0x54: "slot 21 (uCameraCtrl 0x60D380)",
}


def main() -> int:
    funcs = sr_lib.build_all()
    print("functions %d" % len(funcs))
    sites = defaultdict(list)
    for start, end, rows in funcs:
        for i, (va, text, is_call, is_jmp, ln) in enumerate(rows):
            if not is_call:
                continue
            m = re.match(r"^call\s+([a-z0-9]+)$", text)
            if not m:
                continue
            reg = m.group(1)
            for back in range(i - 1, max(-1, i - 4), -1):
                m2 = re.match(r"^mov\s+%s,\s*\[([a-z0-9]+)\+(0x[0-9a-f]+)\]$" % reg, rows[back][1])
                if m2:
                    slot = int(m2.group(2), 16)
                    if slot in SLOTS:
                        sites[slot].append((va, start, rows[back][1], m2.group(1)))
                    break
    for slot in sorted(sites):
        print()
        print("== slot +0x%X  %s : %d site(s) ==" % (slot, SLOTS[slot], len(sites[slot])))
        for va, fn, movtxt, base in sorted(sites[slot]):
            print("   %08X in fn %08X   %s ; call reg" % (va, fn, movtxt))
    return 0


if __name__ == "__main__":
    sys.exit(main())
