#!/usr/bin/env python3
"""Compare the moving candidates: which one behaves like a camera in a level?

Written after cam_find.py kept failing to separate them. Its smoothness ratio is computed on
consecutive samples, and because a frame contains the same register several times the
"previous" sample is often the same matrix listed twice, giving a step of 0 for everything -
so every moving register scored a median step of 0.000 and they all "qualified".

What actually separates them is visible in the values themselves, so this prints them:

  * a world camera's position is a PLACE that drifts smoothly as the player walks, and it is
    large in every component;
  * a viewport or screen-space matrix carries a screen size: one component sits at 1 or 17
    and never moves, while the others jump by hundreds when a render pass re-sets it up.

Usage:
    python scripts\\reg_compare.py <re6vr.log>
"""
import math
import re
import sys
from collections import OrderedDict

TRACE_RE = re.compile(
    r"trace f=(\d+) slot=(\d+) reg=(\d+) kind=(\S+) "
    r"t=\(([-\d.]+) ([-\d.]+) ([-\d.]+)\) tmag=([\d.]+)")


def parse(path):
    per_reg = OrderedDict()
    for line in open(path, "r", encoding="utf-8", errors="replace"):
        m = TRACE_RE.search(line)
        if not m:
            continue
        reg = int(m.group(3))
        rec = per_reg.setdefault(reg, {"kinds": set(), "seq": []})
        rec["kinds"].add(m.group(4))
        rec["seq"].append((int(m.group(1)),
                           (float(m.group(5)), float(m.group(6)), float(m.group(7)))))
    return per_reg


def report(reg, rec, stride=400):
    seq = rec["seq"]
    kinds = ",".join(sorted(rec["kinds"]))
    print(f"=== reg {reg}  ({kinds}, {len(seq)} samples) ===")

    # Sample every `stride`-th entry: consecutive entries are the same matrix listed more
    # than once per frame, so a dense listing would show no change even where there is one.
    pts = [(f, t) for i, (f, t) in enumerate(seq) if i % stride == 0]
    if len(pts) < 3:
        print("  too few samples after decimation\n")
        return None

    # How much each component actually varies over the whole run.
    per_axis = []
    for ax in range(3):
        vals = [t[ax] for _f, t in pts]
        per_axis.append((min(vals), max(vals), max(vals) - min(vals)))
    print("  component ranges over the run (min, max, spread):")
    for ax, name in enumerate("xyz"):
        lo, hi, spread = per_axis[ax]
        print(f"    {name}: {lo:>11.1f} .. {hi:>11.1f}   spread {spread:>10.1f}")

    # The discriminator: a component that never moves is a screen-space constant.
    frozen = [name for ax, name in enumerate("xyz") if per_axis[ax][2] < 1.0]
    if frozen:
        print(f"  -> component(s) {frozen} NEVER MOVE: this is screen-space, not a place")
    else:
        print("  -> every component moves: consistent with a position in a level")

    print("  trajectory (every %dth sample):" % stride)
    prev = None
    steps = []
    for f, t in pts[:10]:
        d = "" if prev is None else "  step %.1f" % math.dist(t, prev)
        print(f"    f={f:<7} ({t[0]:>10.1f} {t[1]:>10.1f} {t[2]:>10.1f}){d}")
        if prev is not None:
            steps.append(math.dist(t, prev))
        prev = t
    if steps:
        steps_sorted = sorted(steps)
        print(f"  step between samples: median {steps_sorted[len(steps_sorted)//2]:.1f}, "
              f"max {max(steps):.1f}")
    print()
    return {"frozen": frozen, "spread": [p[2] for p in per_axis], "kinds": kinds}


def main(path):
    per_reg = parse(path)
    if not per_reg:
        print(f"{path}: no 'trace f=' lines")
        return 1

    # Only the registers whose position travels at all are candidates.
    interesting = []
    for reg, rec in sorted(per_reg.items()):
        pts = [t for i, (_f, t) in enumerate(rec["seq"]) if i % 400 == 0]
        if len(pts) < 3:
            continue
        travel = sum(math.dist(pts[i], pts[i - 1]) for i in range(1, len(pts)))
        if travel > 10.0:
            interesting.append((reg, rec, travel))

    print(f"{path}: {len(per_reg)} register(s), {len(interesting)} of them moving\n")
    results = {}
    for reg, rec, travel in sorted(interesting, key=lambda x: -x[2]):
        results[reg] = report(reg, rec)

    print("=== verdict ===")
    world = [r for r, v in results.items() if v and not v["frozen"]]
    screen = [r for r, v in results.items() if v and v["frozen"]]
    print(f"  registers with every component moving (world-like): {world}")
    print(f"  registers with a frozen component (screen-space):    {screen}")
    if world:
        best = world[0]
        print(f"  -> reg {best} is the camera: it is a place in the level, not a screen "
              f"coordinate")
        print(f"     set re6vr_head_view.txt to '1 {best}' to rotate ONLY this matrix")
    else:
        print("  -> no register behaves like a world position; the camera's matrix is "
              "probably not in the vertex-constant stream")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
