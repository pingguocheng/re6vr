#!/usr/bin/env python3
"""Parse RE6VR_CAPTURE_VS output into a map of where the matrices live.

The probe writes raw register dumps; this reconstructs, for every
SetVertexShaderConstantF batch, which register range holds a view matrix and which
holds a projection matrix. Doing it here rather than by eye matters because the
engine uploads the view matrix at different offsets for different shaders, and the
question the stereo work needs answered is "which offsets, and do they move".

Usage: python scripts/matrix_map.py <re6vr.log> [--frames a b]
"""
import re
import sys

# cap:   reg  27 = (1.44 0 0 0)  hex (0x3FB87889 ...)
REG_RE = re.compile(
    r"cap:\s+reg\s+(\d+)\s+=\s+\(([^)]*)\)\s+hex\s+\(([^)]*)\)")
HDR_RE = re.compile(
    r"cap:\s+----\s+frame\s+(\d+),\s+SetVertexShaderConstantF\(reg\s+(\d+),\s+(\d+)\s+vec4\)"
    r"\s+:\s+(\d+)\s+candidate")


def parse(path):
    """-> list of batches: {frame, start, count, regs: {index: (f0..f3)}}"""
    batches = []
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = HDR_RE.search(line)
            if m:
                cur = {"frame": int(m.group(1)), "start": int(m.group(2)),
                       "count": int(m.group(3)), "regs": {}}
                batches.append(cur)
                continue
            m = REG_RE.search(line)
            if m and cur is not None:
                idx = int(m.group(1))
                vals = []
                for tok in m.group(2).split():
                    try:
                        vals.append(float(tok))
                    except ValueError:
                        vals.append(float("nan"))
                if len(vals) == 4:
                    cur["regs"][idx] = vals
    return batches


def rows_of(mat):
    """A candidate 4x4 as four rows."""
    return mat


def classify(regs, start):
    """Look at regs[start..start+3]; return (kind, matrix) or None.

    A VIEW matrix read as rows has an orthonormal 3x3 and a non-trivial fourth row.
    A PROJECTION matrix as rows has a zero fourth column apart from the perspective
    term and a fourth row of (0, 0, k, 0).
    """
    r = [regs.get(start + i) for i in range(4)]
    if any(v is None for v in r):
        return None
    if any(any(x != x for x in v) for v in r):
        return None

    def orthonormal(m):
        worst = 0.0
        for i in range(3):
            n = sum(m[i][j] ** 2 for j in range(3)) ** 0.5
            worst = max(worst, abs(n - 1.0))
        for i in range(3):
            for k in range(i + 1, 3):
                d = sum(m[i][j] * m[k][j] for j in range(3))
                worst = max(worst, abs(d))
        return worst

    if orthonormal(r) < 1e-3 and sum(abs(r[3][j]) for j in range(3)) > 0.01:
        # Rigid - but a projection matrix for a 1920-wide target is rigid too, with
        # its "translation" equal to the target width in screen units. Only a
        # world-scale translation is a camera.
        tnorm = sum(r[3][j] ** 2 for j in range(3)) ** 0.5
        if 1.0 <= tnorm <= 1e6:
            return "VIEW", r
        return None
    # projection in column-major memory: read it transposed for the shape test
    t = [[r[c][row] for c in range(4)] for row in range(4)]
    for cand, tag in ((r, "PROJ"), (t, "PROJ^T")):
        col3_sparse = abs(cand[0][3]) < 1e-3 and abs(cand[1][3]) < 1e-3 and abs(cand[2][3]) > 1e-6
        row3_persp = abs(cand[3][0]) < 1e-3 and abs(cand[3][1]) < 1e-3 and abs(cand[3][3]) < 1e-3
        if col3_sparse and row3_persp:
            return tag, cand
    return None


def main(path, lo=None, hi=None):
    batches = parse(path)
    print(f"{path}: {len(batches)} register batch(es) parsed")
    if not batches:
        print("  no 'cap:' batches found - is the marker file in place?")
        return

    # For each batch, scan every register offset for a view matrix.
    print("\n  where the matrices are:")
    print("    frame  upload            VIEW at        PROJ at")
    seen_views = {}
    seen_projs = {}
    for b in batches:
        if lo is not None and b["frame"] < lo:
            continue
        if hi is not None and b["frame"] > hi:
            continue
        views, projs = [], []
        for off in range(b["start"], b["start"] + b["count"]):
            got = classify(b["regs"], off)
            if not got:
                continue
            kind, _ = got
            if kind == "VIEW":
                views.append(off)
            elif kind.startswith("PROJ"):
                projs.append(off)
        # Collapse overlapping windows (the same matrix reported at several offsets).
        def collapse(offs):
            out = []
            for o in offs:
                if not out or o > out[-1] + 1:
                    out.append(o)
            return out
        v, p = collapse(views), collapse(projs)
        if v or p:
            print(f"    {b['frame']:>5}  reg {b['start']:>3},{b['count']:>3} vec4     "
                  f"{str(v):<14} {str(p)}")
        for o in v:
            seen_views[o] = seen_views.get(o, 0) + 1
        for o in p:
            seen_projs[o] = seen_projs.get(o, 0) + 1

    print("\n  summary (register offset -> how many batches):")
    print(f"    VIEW offsets seen: {dict(sorted(seen_views.items()))}")
    print(f"    PROJ offsets seen: {dict(sorted(seen_projs.items()))}")

    # Camera motion: list the view translations in frame order.
    print("\n  view-matrix translation over time (frame -> x y z):")
    prev = None
    for b in batches:
        for off in range(b["start"], b["start"] + b["count"]):
            got = classify(b["regs"], off)
            if got and got[0] == "VIEW":
                t = got[1][3][:3]
                if prev is None or any(abs(t[i] - prev[i]) > 1.0 for i in range(3)):
                    print(f"    frame {b['frame']:>5}: {t[0]:>12.3f} {t[1]:>12.3f} {t[2]:>12.3f}")
                    prev = t
                break


if __name__ == "__main__":
    args = sys.argv[1:]
    lo = hi = None
    if "--frames" in args:
        i = args.index("--frames")
        lo, hi = int(args[i + 1]), int(args[i + 2])
        args = args[:i]
    main(args[0], lo, hi)
