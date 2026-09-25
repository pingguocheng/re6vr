#!/usr/bin/env python3
"""Check the view-matrix pick logic against a recorded capture log.

The pick logic lives in C++ and needs a game launch to exercise, which makes every
mistake cost a run. The capture log already contains the raw registers, so the same
decision can be replayed here in a second: which candidate would be selected, and is
it a camera or a projection matrix in disguise?

The giveaway that cost two runs: a projection matrix is rigid too, and its "translation"
is one over the screen size (1/3840 = 0.000260417), which sails past any world-scale
filter because it is a perfectly ordinary small number.

Usage: python scripts/pick_check.py <re6vr.log>
"""
import re
import sys

REG_RE = re.compile(r"cap:\s+reg\s+(\d+)\s+=\s+\(([^)]*)\)")
HDR_RE = re.compile(r"cap:\s+----\s+frame\s+(\d+),\s+SetVertexShaderConstantF"
                    r"\(reg\s+(\d+),\s+(\d+)\s+vec4\)")


def parse(path):
    batches, cur = [], None
    for line in open(path, "r", encoding="utf-8", errors="replace"):
        m = HDR_RE.search(line)
        if m:
            cur = {"frame": int(m.group(1)), "start": int(m.group(2)),
                   "count": int(m.group(3)), "regs": {}}
            batches.append(cur)
            continue
        m = REG_RE.search(line)
        if m and cur is not None:
            vals = []
            for tok in m.group(2).split():
                try:
                    vals.append(float(tok))
                except ValueError:
                    vals.append(float("nan"))
            if len(vals) == 4:
                cur["regs"][int(m.group(1))] = vals
    return batches


def rigid(m):
    """Largest deviation from orthonormality of the 3x3 part, or None if not rigid."""
    worst = 0.0
    for i in range(3):
        n = sum(m[i][j] ** 2 for j in range(3)) ** 0.5
        worst = max(worst, abs(n - 1.0))
    for i in range(3):
        for k in range(i + 1, 3):
            worst = max(worst, abs(sum(m[i][j] * m[k][j] for j in range(3))))
    return worst


def non_trivial_rotation(m):
    """A camera rotates the world; an identity 3x3 is a projection or a UI transform.

    This is the test the pick logic was missing. The game's projection matrix is rigid
    with a (1920, 1080)-style "translation" (a screen size, not a place), so magnitude
    filters alone select it - and shifting a projection matrix changes nothing, which
    is why the first pick experiment appeared to work but had no effect.
    """
    off = [m[0][1], m[0][2], m[1][0], m[1][2], m[2][0], m[2][1]]
    return any(abs(v) > 1e-3 for v in off)


def world_scale(m):
    t = sum(m[3][j] ** 2 for j in range(3)) ** 0.5
    return 50.0 <= t <= 1e6


def is_projection(m):
    """Projection fingerprint: rigid-ish 3x3 with a zeroed last column and a
    fourth row of (0, 0, k, 0), or a translation equal to a screen reciprocal."""
    col3 = abs(m[0][3]) < 1e-3 and abs(m[1][3]) < 1e-3
    row3 = abs(m[3][0]) < 1e-3 and abs(m[3][1]) < 1e-3 and abs(m[3][3]) < 1e-3
    return col3 and row3


def main(path):
    batches = parse(path)
    print(f"{path}: {len(batches)} batch(es)")
    frames = {}
    for b in batches:
        for off in range(b["start"], b["start"] + b["count"]):
            r = [b["regs"].get(off + i) for i in range(4)]
            if any(v is None for v in r):
                continue
            dev = rigid(r)
            if dev > 1e-3:
                continue
            tnorm = sum(r[3][j] ** 2 for j in range(3)) ** 0.5
            if tnorm < 1e-4:
                continue
            if is_projection(r):
                kind = "PROJ"
            elif not world_scale(r):
                kind = "PROJ/UI"        # screen-scale translation: not the camera
            elif not non_trivial_rotation(r):
                kind = "UI(identity)"   # rigid but axis-aligned: billboard / UI
            else:
                kind = "VIEW"
            frames.setdefault(b["frame"], []).append((off, kind, tnorm, r[3][:3]))

    print("\n  per frame: every rigid candidate, in register order")
    shown = 0
    for fr in sorted(frames):
        cands = frames[fr]
        # Collapse overlapping windows: keep the first register of each run.
        keep = []
        for off, kind, tn, t in cands:
            if keep and off <= keep[-1][0] + 1:
                continue
            keep.append((off, kind, tn, t))
        line = f"    frame {fr:>5}: " + "  ".join(
            f"reg{o}({k},{tn:.4g})" for o, k, tn, _ in keep)
        print(line)
        shown += 1
        if shown >= 14:
            print("    ...")
            break

    print("\n  verdict:")
    real = [(fr, [c for c in cs if c[1] == "VIEW"]) for fr, cs in sorted(frames.items())]
    counts = {}
    for fr, cs in real:
        counts[len(cs)] = counts.get(len(cs), 0) + 1
    print(f"    VIEW candidates per frame: {dict(sorted(counts.items()))}")
    if counts:
        if list(counts) == [1]:
            print("    -> exactly one per frame: pick 0 is unambiguous")
        else:
            print("    -> more than one in some frames; pick must be chosen per layout")
        print("    -> a pick index counts only VIEW entries, so PROJ/UI must be excluded")
        for fr, cs in real[:6]:
            for off, kind, tn, t in cs:
                print(f"       frame {fr:>5} reg {off:>3}: translation "
                      f"({t[0]:.4g}, {t[1]:.4g}, {t[2]:.4g})")
    else:
        print("    -> NO camera found in this log; the criteria are still wrong")


if __name__ == "__main__":
    main(sys.argv[1])
