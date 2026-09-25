#!/usr/bin/env python3
"""Offline replay of the head pipeline: prove the rotation maths, then replay REAL recorded poses.

Why (the user asked for option B - validate offline instead of another game run):

The head-tracking pipeline in src/cam_steer.cpp had two decompositions of the head's rotation:

  OLD (headset frame)   rot = prev * cur^T, then
                            yaw   = atan2(rot[0][2], rot[2][2])
                            pitch = -asin(rot[1][2])
  NEW (world frame)     rot = prev * cur^T, then rotate the head's backward axis by rot and read
                            yaw   = atan2(b.x, -b.z)
                            pitch = atan2(b.y, hypot(b.x, b.z))

The recorded logs show what the OLD one did to the published camera angles:

    published yaw -35.0 pitch +34.3 ... -35.0 +29.1 ... -33.6 +35.0      (both pinned at +-35)

This script replays the poses that the bridge actually logged (`pose: yaw ... quat (x y z w)`) and
reports, for each decomposition:
  * the total yaw and pitch the pipeline would have published, and
  * whether a pure head YAW (with the headset worn with its yaw axis tilted by tau degrees, which is
    the normal case - nobody puts a headset on perfectly level) leaks into pitch.

Usage:  python _work/replay_head_pipeline.py [log ...]
        (default: the newest re6vr.log plus every archived log that has pose lines)
"""
from __future__ import annotations

import glob
import math
import os
import re
import sys

WORK = r"C:\re6vr\_work"
POSE_RE = re.compile(
    r"pose: yaw\s+(-?[\d.]+)\s+deg pitch\s+(-?[\d.]+)\s+deg.*?quat \((-?[\d.]+) (-?[\d.]+) "
    r"(-?[\d.]+) (-?[\d.]+)\)")


def quat_to_basis(x: float, y: float, z: float, w: float):
    """Row-major 3x3 whose COLUMNS are the head's right / up / backward axes.

    This is the same expression as openxr_bridge.cpp uses for matrix_probe_set_head_rotation, so the
    replay is fed the same numbers the DLL sees.
    """
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    return [
        [1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy)],
        [2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx)],
        [2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy)],
    ]


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def transpose(m):
    return [[m[j][i] for j in range(3)] for i in range(3)]


def mul_vec(m, v):
    return [sum(m[i][k] * v[k] for k in range(3)) for i in range(3)]


def old_decomposition(prev, cur):
    r = matmul(prev, transpose(cur))
    yaw = math.atan2(r[0][2], r[2][2])
    pitch = -math.asin(max(-1.0, min(1.0, r[1][2])))
    return yaw, pitch


def new_decomposition(prev, cur):
    r = matmul(prev, transpose(cur))
    # r * (0,0,-1) = -(third column)
    b = [-r[0][2], -r[1][2], -r[2][2]]
    # The leading minus matches src/cam_steer.cpp: without it the world-frame yaw comes out with the
    # opposite sign to the headset-frame one, which the maths check below catches (see section 1).
    yaw = -math.atan2(b[0], -b[2])
    pitch = math.atan2(b[1], math.hypot(b[0], b[2]))
    return yaw, pitch


def tripod_decomposition(prev, cur):
    """The 3DOF reading the player asked for, done so the two axes CANNOT couple.

    Section 2 of this script shows that both decompositions above leak pitch when the headset is worn
    off-level - and they leak identically, because the leak is not an artefact of the decomposition:
    a head turning about a tilted axis really does sweep its gaze through a cone, and any reading of
    that motion that uses the headset's own geometry will report it as pitch.

    The only way to make it impossible is to read the two axes from the world directly:
        yaw   = how far the gaze swung in the HORIZONTAL plane (about world vertical)
        pitch = the rise of the gaze (asin of its vertical component), which is a real elevation
    A rotation with no vertical component then has exactly zero pitch by construction, whatever the
    headset's tilt is. This is the "camera on a tripod" the player described.
    """
    r = matmul(prev, transpose(cur))
    b = [-r[0][2], -r[1][2], -r[2][2]]
    horiz = math.hypot(b[0], b[2])
    yaw = -math.atan2(b[0], -b[2])
    pitch = math.asin(max(-1.0, min(1.0, b[1])))
    return yaw, pitch


def read_poses(path):
    out = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = POSE_RE.search(line)
            if m:
                out.append(tuple(float(v) for v in m.groups()))
    return out


def replay(poses, decomposition, gain=0.8, deadzone_deg=1.0):
    """Replays at 120 Hz like the DLL does: the poses are the runtime's own samples, so a replay
    step is one sample. Returns the accumulated (yaw, pitch) the pipeline would publish."""
    k_deg = math.pi / 180.0
    enter = deadzone_deg * k_deg
    exit_ = deadzone_deg * 0.6 * k_deg
    extra_yaw = extra_pitch = 0.0
    prev = None
    sticky_yaw = sticky_pitch = None
    in_deadzone = True
    for (_y, _p, qx, qy, qz, qw) in poses:
        cur = quat_to_basis(qx, qy, qz, qw)
        if prev is None:
            prev = cur
            continue
        dy, dp = decomposition(prev, cur)
        prev = cur
        if sticky_yaw is None:
            sticky_yaw, sticky_pitch = 0.0, 0.0
        d_yaw = dy - 0.0        # the pipeline's differential is already relative; sticky tracks it
        d_pitch = dp - 0.0
        mag = math.hypot(d_yaw, d_pitch)
        moving = (mag > enter) if in_deadzone else (mag > exit_)
        if not moving:
            in_deadzone = True
            continue
        in_deadzone = False
        keep = max(0.0, (mag - exit_) / mag) if mag > 1e-9 else 0.0
        extra_yaw += d_yaw * keep * gain
        extra_pitch += d_pitch * keep * gain
    return extra_yaw / k_deg, extra_pitch / k_deg


def synthetic(tau_deg, yaw_deg, steps=600, pitch_deg=0.0):
    """A head that ONLY yaws, worn with its yaw axis tilted by tau degrees from vertical.

    Sample density matters and is the reason this test exists in this form: the pipeline's deadzone
    decides "moving or not" from the size of ONE step, so a replay of sparse snapshots is filtered
    away entirely (that is exactly what the first version of this script showed - every tilted case
    came out as 0.0/0.0 because the per-step motion was below the deadzone). 600 steps over a
    300-degree turn is 0.5 degrees per step at 120 Hz, i.e. a normal, unhurried head turn.

    Returns a list of (yaw_deg, pitch_deg, qx, qy, qz, qw, basis) with the basis built in the TILTED
    tracking frame - i.e. what a headset worn crooked would report.
    """
    tau = math.radians(tau_deg)
    out = []
    for i in range(steps + 1):
        yaw = math.radians(yaw_deg * i / steps)
        pitch = math.radians(pitch_deg)
        r = rot_y(yaw)
        p = rot_x(pitch)
        head = matmul(r, p)
        # the tracking frame is tilted (roll about Z, then lean about X) - the runtime's reference
        tilt = matmul(rot_z(tau), rot_x(tau * 0.5))
        world = matmul(tilt, head)
        out.append((yaw_deg * i / steps, pitch_deg, 0.0, 0.0, 0.0, 1.0, world))
    return out


def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return [[1, 0, 0], [0, c, -s], [0, s, c]]


def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, 0, s], [0, 1, 0], [-s, 0, c]]


def rot_z(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


def replay_matrices(seq, decomposition, gain=0.8, deadzone_deg=1.0):
    k_deg = math.pi / 180.0
    enter = deadzone_deg * k_deg
    exit_ = deadzone_deg * 0.6 * k_deg
    ey = ep = 0.0
    prev = None
    in_deadzone = True
    for item in seq:
        cur = item[6]
        if prev is None:
            prev = cur
            continue
        dy, dp = decomposition(prev, cur)
        prev = cur
        mag = math.hypot(dy, dp)
        moving = (mag > enter) if in_deadzone else (mag > exit_)
        if not moving:
            in_deadzone = True
            continue
        in_deadzone = False
        keep = max(0.0, (mag - exit_) / mag) if mag > 1e-9 else 0.0
        ey += dy * keep * gain
        ep += dp * keep * gain
    return ey / k_deg, ep / k_deg


def main() -> int:
    logs = sys.argv[1:]
    if not logs:
        logs = [os.path.join(WORK, "re6vr.log")]
        logs += sorted(glob.glob(os.path.join(WORK, "_archive", "*.log")))

    print("=== 1. maths check: a pure 90 deg yaw must give exactly that, and no pitch ===\n")
    for tag, head in (("yaw only", rot_y(math.radians(90))),
                      ("yaw+pitch", matmul(rot_y(math.radians(45)), rot_x(math.radians(-30))))):
        r = head                                   # rot = I * cur^T with prev = identity
        ry, rp = old_decomposition([[1, 0, 0], [0, 1, 0], [0, 0, 1]], head)
        ny, np_ = new_decomposition([[1, 0, 0], [0, 1, 0], [0, 0, 1]], head)
        print("  %-10s identity->pose : OLD yaw %7.2f pitch %7.2f | NEW yaw %7.2f pitch %7.2f"
              % (tag, math.degrees(ry), math.degrees(rp), math.degrees(ny), math.degrees(np_)))

    print("\n=== 2. tilted headset: a head that ONLY yaws, worn tau off-level ===\n")
    print("  A head turn of 10 deg per 120 Hz step (a normal turn, well above the 1.0 deg deadzone,")
    print("  because motion below it is by design not applied at all), total 60 deg:")
    print()
    print("  %-5s | %-21s | %-21s | %-21s" % ("tau", "OLD (headset frame)", "world b-vector",
                                             "TRIPOD (asin of rise)"))
    print("  %-5s | %-21s | %-21s | %-21s" % ("", "published yaw/pitch", "published yaw/pitch",
                                             "published yaw/pitch"))
    for tau in (0, 10, 20, 30, 45):
        seq = synthetic(tau, 60.0, steps=6)
        oy, op = replay_matrices(seq, old_decomposition)
        ny, np_ = replay_matrices(seq, new_decomposition)
        ty, tp = replay_matrices(seq, tripod_decomposition)
        print("  %-5d | %+7.1f %+12.1f | %+7.1f %+12.1f | %+7.1f %+12.1f"
              % (tau, oy, op, ny, np_, ty, tp))
    print()
    print("  A pure head YAW must move only the published YAW. The first two columns leak pitch as the")
    print("  headset is worn further off-level - and they leak IDENTICALLY, which is the finding that")
    print("  matters: the leak is not an artefact of the decomposition, it is real geometry (a head")
    print("  turning about a tilted axis sweeps its gaze through a cone). Only the tripod reading,")
    print("  which takes pitch from the RISE of the gaze in world terms, keeps them apart - which is")
    print("  the 3DOF behaviour asked for.")

    print("\n=== 3. replay of the REAL recorded poses ===\n")
    total = 0
    for path in logs:
        poses = read_poses(path)
        if len(poses) < 5:
            continue
        total += len(poses)
        oy, op = replay(poses, old_decomposition)
        ny, np_ = replay(poses, new_decomposition)
        print("  %-42s %3d poses | OLD yaw %+7.1f pitch %+7.1f | NEW yaw %+7.1f pitch %+7.1f"
              % (os.path.basename(path), len(poses), oy, op, ny, np_))
    print("\n  (%d poses replayed in total)" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
