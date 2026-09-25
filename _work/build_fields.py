"""Build the per-class field map (background-friendly, prints progress)."""
import sys
import time

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze, dti, propmap                             # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

t0 = time.time()
an = analyze.load_analysis(CACHE, EXE)
print("load: %.1fs, %d functions" % (time.time() - t0, len(an.functions)), flush=True)

t0 = time.time()
tables = propmap.scan_fields_by_function(an, an.img)
nfields = sum(len(v) for v in tables.values())
print("scan: %.1fs, %d functions with a table, %d fields"
      % (time.time() - t0, len(tables), nfields), flush=True)

t0 = time.time()
dtis = dti.enumerate_dtis(an.img)
print("dtis: %d (%.1fs)" % (len(dtis), time.time() - t0), flush=True)

t0 = time.time()
cf = propmap.class_fields(an, an.img, dtis)
print("attribute: %.1fs, %d of %d classes got a table"
      % (time.time() - t0, len(cf), len(dtis)), flush=True)

for n in ("sBioCamera", "sCamera::Viewport", "uCameraCtrl", "uCameraBase", "uCameraQFPS",
          "uCamera", "uFreeCamera", "cCameraParam"):
    c = cf.get(n)
    if c:
        print("  %-20s dti=0x%08X size=0x%-6X ctor=0x%08X fields=%d"
              % (n, c.dti, c.size, c.constructor or 0, len(c.fields)), flush=True)
    else:
        print("  %-20s NO TABLE" % n, flush=True)

# Save for follow-up work.
import json
out = {name: {"dti": c.dti, "size": c.size, "ctor": c.constructor,
              "fields": [{"name": f.name, "type": f.type_code, "offset": f.offset,
                          "site": f.site} for f in c.fields]}
       for name, c in cf.items()}
with open(r"C:\re6vr\_work\bh6_fields.json", "w", encoding="utf-8") as fh:
    json.dump(out, fh)
print("wrote _work\\bh6_fields.json (%d classes)" % len(out), flush=True)
