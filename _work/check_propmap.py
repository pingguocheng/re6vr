"""Throwaway: check the property-table pairing on the class that owns mCameraOrg.

Ground truth to match, from the README's manual parse of the DTI block:

    mCameraOrg[i], stride 0x40, i = 0..7, each entry
        cameraPos / targetPos / cameraUp / fov / nearPlane / farPlane
    i=0 base +0xE30, so cameraPos +0xE30, targetPos +0xE40, cameraUp +0xE50, fov +0xE60 ...
"""
import sys

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze, dti, pe, propmap                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

an = analyze.load_analysis(CACHE, EXE)
print("loaded: %d functions" % len(an.functions))
img = an.img

# Which analysed function actually contains the property-registration stream?
for probe in (0x4FA560, 0x4FA87E, 0x60AA60):
    hit = None
    for k, f in an.functions.items():
        if k <= probe < f.end:
            if hit is None or f.instructions > an.functions[hit].instructions:
                hit = k
    if hit is None:
        print("0x%08X: no analysed function contains it" % probe)
        continue
    f = an.functions[hit]
    print("0x%08X -> function 0x%08X .. 0x%08X  %d instructions, origin %s"
          % (probe, hit, f.end, f.instructions, f.origin))

hit = None
for k, f in an.functions.items():
    if k <= 0x4FA87E < f.end and (hit is None or f.instructions > an.functions[hit].instructions):
        hit = k

fields = propmap.scan_fields(an, img, functions={hit} if hit else None)
print("%d fields recovered in function 0x%08X" % (len(fields), hit or 0))
for f in fields[:20]:
    print("   %s   site=0x%08X" % (f, f.site))
print("   ...")
for f in fields[-5:]:
    print("   %s   site=0x%08X" % (f, f.site))

cam = [f for f in fields if f.name.startswith("mCameraOrg[0]")]
print()
print("mCameraOrg[0].* :")
for f in cam:
    print("   %-40s %-8s +0x%X" % (f.name, f.type_name, f.offset or 0))
