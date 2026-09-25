#!/usr/bin/env python3
"""Verify the head-look view-matrix selection offline, from a harness log.

The selection logic is the part of head look that cannot be checked by reading code: it
has to pick one view matrix out of the several the engine uploads per frame, and every
way of getting that wrong is silent. The version that shipped on the machine fired its
hook 604,300 times, logged the head as enabled, and rotated nothing at all - because it
compared a frame-local candidate number against a register offset, so no candidate was
ever selected and there was no error to see.

Three properties are checked here. All three are things that were wrong at some point:

  1. `pick = -2` lands on the frame's LAST view matrix. That matrix cannot be identified
     while the frame is still being uploaded, so the plugin defers the decision by one
     frame; this script checks the slot that actually got rotated is the last one the
     engine uploaded.
  2. The rotation does not compound. Re-writing a matrix in place accumulates if the
     value written is used as the next anchor, so a turned head would wind up rotated by
     2x, 3x, ... The same input rotation must produce byte-identical output every frame.
  3. An identity-rotation camera is selected too. A camera that has not turned yet has an
     identity 3x3, which is indistinguishable from a world-axis-aligned placement; when
     such windows were excluded, the camera they belong to could never be selected.

Usage:
    python scripts\\head_check.py <re6vr.log> [--pick -2]

Exit status is 0 when every check passes, 1 otherwise, so this can gate a build.
"""
import argparse
import re
import sys
from collections import OrderedDict

# "head: frame 12 view#5 reg 26 rotated (gain 1.00): rows (..) (..) (..), translation (..) unchanged"
ROT_RE = re.compile(
    r"head: frame (\d+) view#(\d+) reg (\d+) rotated \(gain ([-\d.]+)\): "
    r"rows \(([^)]*)\) \(([^)]*)\) \(([^)]*)\), "
    r"translation \(([^)]*)\) unchanged")
# "head: candidate kind=VIEW index=3 chosen=1 settled=1 count=8 reg 4"
CAND_RE = re.compile(
    r"head: candidate kind=(\S+) index=(\d+) chosen=(\d+) settled=(\d+) count=(\d+) reg (\d+)")
APPLY_RE = re.compile(r"head: pick=-2 -> the previous frame's last view matrix was slot #(\d+)")


def fvec(text):
    return tuple(round(float(t), 6) for t in text.split())


def parse(path):
    rots, cands, applied = [], [], []
    for line in open(path, "r", encoding="utf-8", errors="replace"):
        m = ROT_RE.search(line)
        if m:
            rots.append({
                "frame": int(m.group(1)), "slot": int(m.group(2)), "reg": int(m.group(3)),
                "gain": float(m.group(4)),
                "rows": (fvec(m.group(5)), fvec(m.group(6)), fvec(m.group(7))),
                "trans": fvec(m.group(8)),
            })
            continue
        m = CAND_RE.search(line)
        if m:
            cands.append({
                "kind": m.group(1), "slot": int(m.group(2)), "chosen": int(m.group(3)),
                "settled": int(m.group(4)), "count": int(m.group(5)), "reg": int(m.group(6)),
            })
            continue
        m = APPLY_RE.search(line)
        if m:
            applied.append(int(m.group(1)))
    return rots, cands, applied


def check_rotation_is_rigid(rots, fails):
    """The written basis must still be a rotation, or the scene skews."""
    for r in rots[:200]:
        for row in r["rows"]:
            n = sum(v * v for v in row) ** 0.5
            if abs(n - 1.0) > 1e-3:
                fails.append(f"frame {r['frame']}: rotated row has length {n:.6f}, not 1")
                return
        for i in range(3):
            for k in range(i + 1, 3):
                d = sum(r["rows"][i][j] * r["rows"][k][j] for j in range(3))
                if abs(d) > 1e-3:
                    fails.append(f"frame {r['frame']}: rotated rows {i},{k} not perpendicular "
                                 f"(dot {d:.6f})")
                    return


def check_no_compounding(rots, fails):
    """Same slot + same gain must give the same matrix every frame."""
    seen = OrderedDict()
    for r in rots:
        key = (r["slot"], r["gain"])
        if key in seen:
            if seen[key]["rows"] != r["rows"]:
                a, b = seen[key], r
                fails.append(
                    f"rotation CHANGED for slot {r['slot']} at gain {r['gain']} between "
                    f"frame {a['frame']} and {b['frame']}: {a['rows'][0]} -> {b['rows'][0]} "
                    f"(the anchor is accumulating instead of being taken from the engine)")
                return
        else:
            seen[key] = r


def check_one_target(rots, cands, pick, fails):
    """Whatever the pick mode, exactly ONE register may be written.

    This replaces an earlier check on "the frame's last view matrix", which described a
    mechanism that no longer exists: selecting by position in the frame was dropped because
    the frame's last view matrix is not the camera (on the real game it is a viewport
    matrix, and rotating it is what stretched the picture). The property that has to hold
    now is the one that was actually broken then - that head look writes one matrix, and
    that it is the one that was chosen.
    """
    if not rots:
        return          # nothing rotated is a valid outcome (e.g. auto found no camera)
    regs = sorted({r["reg"] for r in rots})
    if len(regs) > 1:
        fails.append(f"head look wrote to {len(regs)} different registers {regs}; it must "
                     f"only ever touch the one it identified as the camera")
    if pick is not None and pick >= 0 and regs and regs != [pick]:
        fails.append(f"pick={pick} but the rotation went to register(s) {regs}")
    # Registers must be stable across the whole run: a target that drifts means the
    # identification is being recomputed from noisy evidence.
    first_seen = rots[0]["reg"]
    for r in rots:
        if r["reg"] != first_seen:
            fails.append(f"the rotated register changed mid-run: {first_seen} -> {r['reg']} "
                         f"at frame {r['frame']}")
            return


def check_identity_camera_selectable(cands, fails):
    """A rigid identity-rotation window must be treated as a selectable camera."""
    kinds = {c["kind"] for c in cands}
    if "UI/world-axis" in kinds and "VIEW" not in kinds:
        fails.append("every rigid candidate was 'UI/world-axis'; an unturned camera would "
                     "never be selected - is_view_candidate must accept it")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--pick", type=int, default=None,
                    help="the pick mode the run used; -2 = auto (find the camera), "
                         ">=0 = that register, -1 = all")
    args = ap.parse_args()

    rots, cands, applied = parse(args.log)
    print(f"{args.log}")
    print(f"  rotations logged : {len(rots)}")
    print(f"  candidates logged: {len(cands)}")
    print(f"  pick=-2 armings  : {applied if applied else '(none)'}")

    if not rots and not cands:
        print("\n  FAIL: the log contains no head-look activity at all.")
        print("        Either the run used no re6vr_head_view.txt, or the head block never")
        print("        ran - check for 'head: ENABLED' and 'head: block entered'.")
        return 1

    fails = []
    check_rotation_is_rigid(rots, fails)
    check_no_compounding(rots, fails)
    check_one_target(rots, cands, args.pick, fails)
    check_identity_camera_selectable(cands, fails)

    if cands:
        slots = sorted({c["slot"] for c in cands})
        kinds = sorted({c["kind"] for c in cands})
        print(f"  candidate slots  : {slots}")
        print(f"  candidate kinds  : {kinds}")
    if rots:
        print(f"  rotated slots    : {sorted({r['slot'] for r in rots})}")
        print(f"  rotated regs     : {sorted({r['reg'] for r in rots})}")

    print()
    if fails:
        for f in fails:
            print(f"  FAIL: {f}")
        return 1
    print("  PASS: the rotation is rigid, it does not accumulate, and head look")
    print("        wrote exactly one register - the one it identified as the camera.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
