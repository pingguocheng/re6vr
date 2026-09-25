"""Throwaway: dump the registered field tables of the camera classes, sorted by offset."""
import json
import sys

FIELDS = r"C:\re6vr\_work\bh6_fields.json"
names = sys.argv[1:] or ["uCameraBase", "uCamera", "uCameraCtrl", "cCameraParam", "sCamera",
                         "sBioCamera"]

with open(FIELDS, encoding="utf-8") as fh:
    data = json.load(fh)

TYPES = {0x2: "ptr", 0x3: "bool", 0x6: "int32", 0xC: "float", 0x13: "Coord", 0x14: "Vector3",
         0x4: "int8", 0x1: "?", 0x10: "?"}

for n in names:
    # exact then substring
    keys = [k for k in data if k == n] or [k for k in data if n in k]
    for k in keys[:4]:
        c = data[k]
        print("=== %-34s dti=0x%08X size=0x%-6X ctor=0x%08X fields=%d"
              % (k, c["dti"], c["size"], c["ctor"] or 0, len(c["fields"])))
        for f in sorted(c["fields"], key=lambda f: (f["offset"] is None, f["offset"])):
            o = f["offset"]
            t = TYPES.get(f["type"], "0x%X" % f["type"])
            print("    %-8s %-12s %s" % (("+0x%X" % o) if o is not None else "  --  ",
                                         t, f["name"]))
        print()
