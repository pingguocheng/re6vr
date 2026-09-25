import math
#!/usr/bin/env python3
"""Find which register carries the world camera, from a trace of every candidate.

The question this answers is the one that has cost the most runs: several registers carry
rigid transforms (orthonormal 3x3 plus a translation), and shape alone cannot tell a camera
from a HUD placement, a bone matrix or a viewport. Picking the wrong one is silent - the
picture distorts and the camera never turns.

The discriminator used here is SMOOTHNESS, and it is physical rather than statistical. A
camera is an object in the world: walking moves it by a continuous amount, so between two
frames its translation changes by a little relative to its own magnitude. The alternatives
behave differently and recognisably:

  * a viewport matrix carries a screen size instead of a place, and jumps by hundreds of
    units whenever the viewport is set up (measured: 1920,609,17 then 1920,1044,17 - x and
    z identical, y jumping);
  * a HUD or sprite placement does not move at all;
  * a bone or object matrix moves with an animation, which is smooth but lives near the
    origin in local space.

So the test is: world-scale translation, and a per-frame change that stays a small fraction
of the magnitude. The ratio printed below is that fraction - a camera sits far below 1, a
viewport matrix spikes above it.

Usage:
    python scripts\\cam_find.py <re6vr.log>
"""
import math
import re
import sys
from collections import OrderedDict

TRACE_RE = re.compile(
    r"trace f=(\d+) slot=(\d+) reg=(\d+) kind=(\S+) "
    r"t=\(([-\d.]+) ([-\d.]+) ([-\d.]+)\) tmag=([\d.]+) drot=([\d.]+) dtr=([\d.]+)")

# Sample every Nth trace entry before computing step statistics. Consecutive entries are
# the same matrix listed again within one frame, so a dense listing hides the movement;
# decimating is what makes the trajectory visible.
kDecimate = 400


def parse(path):
    """-> per register: the sequence of (frame, translation, rotation rows, tmag)."""
    per_reg = OrderedDict()
    for line in open(path, "r", encoding="utf-8", errors="replace"):
        m = TRACE_RE.search(line)
        if not m:
            continue
        reg = int(m.group(3))
        rec = per_reg.setdefault(reg, {
            "kinds": set(), "samples": 0, "seq": [],
            "jumps": 0, "sum_dt": 0.0, "max_ratio": 0.0, "ratios": []})
        rec["kinds"].add(m.group(4))
        rec["samples"] += 1
        t = (float(m.group(5)), float(m.group(6)), float(m.group(7)))
        tmag = float(m.group(8))
        rec["seq"].append((int(m.group(1)), t, tmag))
    return per_reg


def analyse(rec):
    """Per-frame statistics for one register."""
    prev = None
    moved = 0.0
    for _frame, t, tmag in rec["seq"]:
        if prev is not None:
            dt = sum(abs(t[i] - prev[i]) for i in range(3))
            rec["sum_dt"] += dt
            if dt > 1.0:
                moved += dt
            # The ratio is what separates the two behaviours: a camera's per-frame step is a
            # small fraction of how far away it is, while a viewport matrix's is comparable
            # to the value itself.
            if tmag > 1.0:
                ratio = dt / tmag
                rec["ratios"].append(ratio)
                rec["max_ratio"] = max(rec["max_ratio"], ratio)
                if ratio > 0.5:
                    rec["jumps"] += 1
        prev = t
    rec["travel"] = moved
    rec["jump_rate"] = rec["jumps"] / max(1, len(rec["ratios"]))
    if rec["ratios"]:
        s = sorted(rec["ratios"])
        rec["median_ratio"] = s[len(s) // 2]
    else:
        rec["median_ratio"] = 0.0

    # STEP STATISTICS OVER THE WHOLE RUN, on decimated samples.
    #
    # This is what finally separated the candidates, after four other criteria failed. The
    # per-frame ratio above cannot do it: a frame lists the same register several times, so
    # "the previous sample" is usually the same matrix again, the step is 0, and every moving
    # register scores a median of 0.000 and looks equally camera-like.
    #
    # Decimating first and then looking at the distribution does work. Measured on one
    # session, the three moving candidates were:
    #
    #   reg 9  : steps 3.9, 31.9, 38.6, 98.7, 174.0, 290.0 ...   median 98.7   <- the camera
    #   reg 1  : steps 171, 1787, 4926, 1550, 6966 ...            median 1550
    #   reg 27 : steps 188, 3796, 3098, 4951, 8114 ...            median 4951
    #
    # A camera walks: its position changes by small amounts relative to its distance, most
    # of the time. The others teleport, because a viewport or a secondary pass is re-set-up
    # rather than moved.
    pts = [(f, t) for i, (f, t, _tm) in enumerate(rec["seq"]) if i % kDecimate == 0]
    steps = [math.dist(pts[i][1], pts[i - 1][1]) for i in range(1, len(pts))]
    rec["steps"] = steps
    if steps:
        ss = sorted(steps)
        rec["median_step"] = ss[len(ss) // 2]
        rec["p90_step"] = ss[int(len(ss) * 0.9)]
    else:
        rec["median_step"] = rec["p90_step"] = 0.0

    # A component that never moves over the whole run is a screen-space constant (a viewport
    # matrix keeps one axis pinned at 1 or 17), which a place in a level cannot do.
    frozen = []
    for ax in range(3):
        vals = [t[ax] for _f, t in pts]
        if vals and (max(vals) - min(vals)) < 1.0:
            frozen.append("xyz"[ax])
    rec["frozen"] = frozen

    rec["max_tmag"] = max((tm for _f, _t, tm in rec["seq"]), default=0.0)
    return rec


def main(path):
    per_reg = parse(path)
    if not per_reg:
        print(f"{path}: no 'trace f=' lines.")
        print("  Is re6vr_trace.txt in the marker directory the game reads?")
        return 1

    for rec in per_reg.values():
        analyse(rec)

    print(f"{path}")
    print(f"  {len(per_reg)} register(s) carried a candidate\n")
    hdr = ("  reg  kind                 samples    travel   median-step  p90-step  frozen  "
           "verdict")
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))

    ranked = sorted(per_reg.items(), key=lambda kv: -kv[1]["travel"])
    for reg, r in ranked:
        kinds = ",".join(sorted(r["kinds"]))
        frozen = "".join(r["frozen"]) if r["frozen"] else "-"
        # The verdict applies the physical argument, smallest threshold first.
        if r["travel"] < 10.0:
            verdict = "static - not the camera"
        elif r["frozen"]:
            verdict = f"component {frozen} never moves - screen-space, not a place"
        elif r["p90_step"] > 1000.0:
            verdict = "teleports - a re-set-up pass, not a walked camera"
        else:
            verdict = "*** WALKS: CAMERA ***"
        print(f"  {reg:>4} {kinds:<20} {r['samples']:>7} {r['travel']:>9.0f} "
              f"{r['median_step']:>12.1f} {r['p90_step']:>9.1f}  {frozen:^6}  {verdict}")

    print()
    cameras = [reg for reg, r in ranked
               if r["travel"] >= 10.0 and not r["frozen"] and r["p90_step"] <= 1000.0]
    if not cameras:
        print("  RESULT: no register behaves like a walked camera.")
        print("          If everything either stands still or teleports, the camera's own")
        print("          matrix is probably not in the vertex-constant stream at all - which")
        print("          matches the finding that MT Framework has no engine-wide view-matrix")
        print("          slot.")
        return 1

    print(f"  RESULT: reg {cameras[0]} is the world camera.")
    for reg in cameras[1:]:
        print(f"          (reg {reg} also qualifies - a second camera, e.g. a reflection or "
              f"shadow pass, is expected)")
    reg = cameras[0]
    r = per_reg[reg]
    print(f"          travel {r['travel']:.0f} units, median per-frame step "
          f"{r['median_ratio']:.3f} of its distance, jump-rate {r['jump_rate']:.3f}")
    print(f"          -> set re6vr_head_view.txt to '1 {reg}' to rotate ONLY this matrix")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
