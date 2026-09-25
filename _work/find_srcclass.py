"""Throwaway: which registered class has a field group matching the copy helper's source?

0x4F9950 reads the source object at:
    +0x40 far, +0x44 near, +0x4C fov, +0x50/+0x54/+0x58 cameraPos,
    +0x60/+0x64/+0x68 cameraUp, +0x70/+0x74/+0x78 targetPos
and stores them into sBioCamera mCameraOrg[i] (0xE30 + 0x40*i).

If some class in the property tables has exactly those offsets with those names, the source
object's type is settled by the engine's own metadata instead of by a guessed layout.
"""
import json
import sys

FIELDS = r"C:\re6vr\_work\bh6_fields.json"

WANT = {0x40: "?", 0x44: "?", 0x4C: "?", 0x50: "?", 0x54: "?", 0x58: "?",
        0x60: "?", 0x64: "?", 0x68: "?", 0x70: "?", 0x74: "?", 0x78: "?"}

with open(FIELDS, encoding="utf-8") as fh:
    data = json.load(fh)


def near_off(fields):
    """match by offset for the whole 0x40..0x78 window"""
    got = {}
    for f in fields:
        o = f["offset"]
        if o is not None and o in WANT:
            got[o] = f["name"]
    return got


rows = []
for name, c in data.items():
    got = near_off(c["fields"])
    if len(got) >= 6:
        rows.append((len(got), name, c["size"], got))

rows.sort(key=lambda r: (-r[0], r[1]))
print("%d classes match >=6 of the 12 offsets" % len(rows))
for n, name, size, got in rows[:25]:
    print("\n  %-42s size=0x%-6X %d/12" % (name, size, n))
    for o in sorted(WANT):
        if o in got:
            print("      +0x%02X  %s" % (o, got[o]))

# Also: search by field NAME, wherever it lives.
print("\n--- classes that register a field whose name contains pos/up/target/fov/near/far "
      "with offset in 0x30..0x80 ---")
import re
pat = re.compile(r"(pos|up|target|fov|near|far|Front|Eye)", re.I)
byfield = []
for name, c in data.items():
    hits = [(f["offset"], f["name"]) for f in c["fields"]
            if f["offset"] is not None and 0x30 <= f["offset"] <= 0x80 and pat.search(f["name"])]
    if len(hits) >= 3:
        byfield.append((len(hits), name, hits))
byfield.sort(key=lambda r: (-r[0], r[1]))
for n, name, hits in byfield[:25]:
    print("  %-42s %s" % (name, " ".join("+0x%X:%s" % h for h in sorted(hits))))
