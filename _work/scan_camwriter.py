"""Throwaway: who writes the camera's look-at fields?

If some function stores to `[reg+0xE30]` / `[reg+0xE40]` / `[reg+0xE50]` (the verified
mCameraOrg[0] cameraPos/targetPos/cameraUp offsets), that function is the write point head
tracking needs - and it can be found statically, with no game run.

This is a *scan for candidates*, not a proof: a displacement alone does not establish that the
base register holds an sBioCamera. The output is therefore grouped by function with the
neighbouring displacements shown, so a function that writes several of the table's offsets in
a row stands out from one that happens to touch +0xE40 of something else.
"""
import sys
from collections import defaultdict

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

# The verified sBioCamera layout (from propmap / the property tables).
WATCH = {
    0xE30: "mCameraOrg[0].cameraPos",
    0xE40: "mCameraOrg[0].targetPos",
    0xE50: "mCameraOrg[0].cameraUp",
    0xE60: "mCameraOrg[0].fov",
    0xE64: "mCameraOrg[0].nearPlane",
    0xE68: "mCameraOrg[0].farPlane",
}

an = analyze.load_analysis(CACHE, EXE)
hits: dict[int, list[tuple[int, str, str, int]]] = defaultdict(list)
for fn, ins in analyze.iter_instructions(an):
    if len(ins.operands) < 1:
        continue
    dst = ins.operands[0]
    if dst.kind != "mem" or dst.disp not in WATCH or dst.base is None:
        continue
    # Only a store: the destination must be the first operand of a writing mnemonic.
    if ins.mnemonic not in ("mov", "movss", "movsd", "movaps", "movups", "movdqa",
                            "movq", "movd", "fstp", "fst", "add", "sub", "mul", "xor",
                            "and", "or", "inc", "dec", "lea"):
        continue
    hits[fn.start].append((ins.va, ins.mnemonic, dst.base, dst.disp))

print("%d functions touch an mCameraOrg[0] offset:" % len(hits))
ranked = sorted(hits.items(), key=lambda kv: (-len({h[3] for h in kv[1]}), -len(kv[1])))
for start, rows in ranked[:18]:
    f = an.functions[start]
    named = sorted({r[3] for r in rows})
    print("  fn 0x%08X .. 0x%08X  %4d instrs  %2d distinct offsets %s"
          % (start, f.end, f.instructions, len(named),
             " ".join("+0x%X" % d for d in named)))
    for va, mn, base, disp in rows[:6]:
        print("        0x%08X  %-8s [%s+0x%X]   %s" % (va, mn, base, disp, WATCH[disp]))
