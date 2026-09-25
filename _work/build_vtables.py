"""Export each attributed class's vtable address, so the runtime probe can key on it.

Why a second key is needed: the probe has been searching for objects whose class record sits at
+0x04, and across two real sessions (one of them 8400 frames in a level) it found zero
sBioCamera objects and zero of any other camera class. Either the engine never built them, or
`+0x04 == class record` is not how this build identifies instances.

A vtable is the other static fact available, and it is independent of the first: `propmap.py`
already attributes a property table to a class *via its vtable* (the run in `.rdata` that
contains the class's MtDti getter). This script writes that mapping out as
{class name, DTI address, vtable address} so the probe can search for objects whose first dword
is a known vtable - one full pass over memory answering "which of the 404 attributed classes
does this engine actually have instances of".

Output: _work/bh6_vtables.txt, one line per class:

    <vtable VA> <dti VA> <size> <class name>
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))

from disasm_lib import analyze, dti, propmap                          # noqa: E402

ROOT = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(ROOT, "bh6_analysis.json")
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
OUT = os.path.join(ROOT, "bh6_vtables.txt")


def main() -> int:
    an = analyze.load_analysis(CACHE, EXE)
    if an is None:
        print("no cache at %s" % CACHE)
        return 2
    img = an.img
    dtis = dti.enumerate_dtis(img)
    tables = propmap.scan_fields_by_function(an, img)
    constructors = propmap.scan_constructors(an, img, {d.address: d for d in dtis})

    # The same attribution the library uses, so the exported vtables are the ones that were
    # actually justified - not an independent guess.
    att = propmap.class_tables_by_vtable(an, img, dtis, tables, constructors)
    by_addr = {d.address: d for d in dtis}

    # class_tables_by_vtable knows the function that registers the fields; the vtable address is
    # the run that contained the MtDti getter. Re-derive it here so both facts come out together.
    getters = {}
    for dti_addr, sites in constructors.items():
        for site in sites:
            fn = propmap._function_containing(an, site)
            if fn is not None:
                getters.setdefault(fn, dti_addr)
                break
    rows = []
    for table_va, slots in propmap.scan_vtables(img):
        names = [getters[s] for s in slots if s in getters]
        if not names:
            continue
        d = by_addr.get(names[0])
        if d is None or d.name not in att:
            continue
        rows.append((table_va, d.address, d.size, d.name))

    seen = set()
    with open(OUT, "w", encoding="utf-8") as fh:
        for table_va, dti_addr, size, name in rows:
            if name in seen:
                continue
            seen.add(name)
            fh.write("%08X %08X %X %s\n" % (table_va, dti_addr, size, name))
    print("wrote %s: %d classes with a vtable + DTI" % (OUT, len(seen)))

    with open(OUT, encoding="utf-8") as fh:
        for line in fh:
            if any(k in line for k in ("Camera", "camera")):
                print("   " + line.rstrip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
