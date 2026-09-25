"""Throwaway: which code uploads the D3D9 view transform?

The renderer must hand the view matrix to D3D9 somehow. Hooking the engine's own upload point is
the shortest route to "where does the rendered view come from", and unlike a guessed offset it is
anchored on the import table (a fact of the PE, not an inference).

This lists the d3d9 imports of interest with their IAT VAs and every instruction that touches
those slots.
"""
import sys

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze, pe                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

img = pe.Image.load(EXE)
print("image base 0x%08X, %d imports" % (img.image_base, len(img.imports)))
want = ("SetTransform", "MultiplyTransform", "SetVertexShaderConstantF", "SetViewport",
        "GetTransform", "SetPixelShaderConstantF", "SetVertexDeclaration")
slots = {}
for imp in img.imports:
    if imp.name in want and "d3d9" in imp.dll.lower():
        va = img.image_base + imp.iat_rva
        slots[va] = "%s!%s" % (imp.dll, imp.name)
        print("   %-34s IAT VA 0x%08X" % (slots[va], va))

an = analyze.load_analysis(CACHE, EXE)
print()
for va, name in sorted(slots.items()):
    hits = []
    for fn, ins in analyze.iter_instructions(an):
        for o in ins.operands:
            if o.kind == "mem" and o.base is None and o.disp == va:
                hits.append((fn.start, ins.va, ins.text()))
                break
    print("%s (0x%08X): %d reference(s)" % (name, va, len(hits)))
    for f, iva, t in hits[:40]:
        print("     fn 0x%08X  0x%08X  %s" % (f, iva, t))
