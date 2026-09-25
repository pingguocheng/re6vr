#!/usr/bin/env python3
"""Find which register carries the world camera, from a re6vr_trace.txt run.

This answers the question that rotating things and looking through the headset could
not: a view matrix cannot be told from a HUD placement by its shape, because both are
rigid transforms with a translation. What separates them is behaviour, and the trace
records that -- one line per view candidate per frame, with how much each candidate's
rotation and translation moved since the previous frame.

Judged on TRANSLATION movement, deliberately:

  * the world camera moves as the player walks (its translation is a place, in the
    thousands of world units);
  * HUD, sprite and UI placements keep a fixed translation ((0,0,1) and the like);
  * rotation movement is NOT used to decide, because the plugin itself writes rotations
    when head look is on. Translation is only ever written by the engine, so it stays a
    clean signal either way.

Reading the output:
  camera slot   the candidate with the largest translation travel
  static        candidates whose translation never moved - these are the ones that made
                "rotate every view matrix" stretch the picture

Usage:
    python scripts\\trace_check.py <re6vr.log>
"""
import re
import sys
from collections import OrderedDict

# trace f=21 slot=2 reg=0 kind=VIEW t=(1200.00 300.00 -800.00) drot=2299.64566 dtr=0.000 r0=(...) r1=(...) r2=(...)
TRACE_RE = re.compile(
    r"trace f=(\d+) slot=(\d+) reg=(\d+) kind=(\S+) "
    r"t=\(([-\d.]+) ([-\d.]+) ([-\d.]+)\) drot=([\d.]+) dtr=([\d.]+)")
AUTO_RE = re.compile(r"head: auto - the camera is the matrix at reg (\d+)")


def parse(path):
    per_slot = OrderedDict()
    auto_slot = None
    for line in open(path, "r", encoding="utf-8", errors="replace"):
        m = AUTO_RE.search(line)
        if m:
            auto_slot = int(m.group(1))
            continue
        m = TRACE_RE.search(line)
        if not m:
            continue
        slot = int(m.group(2))
        rec = per_slot.setdefault(slot, {
            "reg": set(), "kinds": set(), "travel": 0.0, "rot_travel": 0.0,
            "frames": set(), "t_min": None, "t_max": None, "last": None})
        rec["reg"].add(int(m.group(3)))
        rec["kinds"].add(m.group(4))
        rec["frames"].add(int(m.group(1)))
        rec["rot_travel"] += float(m.group(8))
        t = (float(m.group(5)), float(m.group(6)), float(m.group(7)))
        if rec["last"] is not None:
            d = sum(abs(t[i] - rec["last"][i]) for i in range(3))
            # Only count real movement: a jitter of a fraction of a unit is noise.
            if d > 1.0:
                rec["travel"] += d
        rec["last"] = t
        rec["t_min"] = t if rec["t_min"] is None else tuple(min(a, b) for a, b in zip(rec["t_min"], t))
        rec["t_max"] = t if rec["t_max"] is None else tuple(max(a, b) for a, b in zip(rec["t_max"], t))
    return per_slot, auto_slot


def main(path):
    per_slot, auto_slot = parse(path)
    if not per_slot:
        print(f"{path}: no 'trace f=' lines found.")
        print("  Is re6vr_trace.txt in the marker directory the game reads?")
        return 1

    frames = max(len(r["frames"]) for r in per_slot.values())
    print(f"{path}")
    print(f"  traced frames: {frames}, candidates: {len(per_slot)}")
    print()
    print("  slot  reg        kind                      translation travel   rot travel")
    ranked = sorted(per_slot.items(), key=lambda kv: -kv[1]["travel"])
    for slot, r in ranked:
        regs = ",".join(str(x) for x in sorted(r["reg"]))
        kinds = ",".join(sorted(r["kinds"]))
        print(f"  {slot:>4}  {regs:<10} {kinds:<25} {r['travel']:>18.1f} {r['rot_travel']:>13.1f}")

    print()
    # A world camera that walked moves hundreds of units; a HUD placement moves
    # by nothing. The gap is enormous, so the cut does not need to be delicate.
    moving = [(s, r) for s, r in ranked if r["travel"] > 10.0]
    static = [(s, r) for s, r in ranked if r["travel"] <= 10.0]

    if not moving:
        print("  RESULT: no candidate's translation moved at all.")
        print("          Either the player never moved during the trace, or the world camera")
        print("          is not going through SetVertexShaderConstantF as a rigid matrix.")
        return 1

    cam_slot, cam = moving[0]
    cam_regs = ",".join(str(x) for x in sorted(cam["reg"]))
    print(f"  RESULT: slot #{cam_slot} (reg {cam_regs}) is the world camera - its translation")
    print(f"          travelled {cam['travel']:.1f} world units, the most of any candidate.")
    if static:
        print(f"          Static candidates (do NOT rotate these): "
              f"{[s for s, _ in static]}")
        for s, r in static:
            regs = ",".join(str(x) for x in sorted(r["reg"]))
            print(f"            slot #{s} reg {regs}: translation stuck at {r['last']}")
    if auto_slot is not None:
        if auto_slot == cam_slot:
            print(f"          auto picked reg {auto_slot} - AGREES with this analysis.")
        else:
            print(f"          auto picked slot #{auto_slot} - DISAGREES "
                  f"(expected #{cam_slot}).")
            return 1
    else:
        print(f"          (no 'head: auto' line in this log; the trace was read-only)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
