#!/usr/bin/env python3
"""Offline check of the stereo capture path - no game, no headset.

What it is for: the per-eye capture is the one part of the stereo work that the existing offline
tests could not reach. They all start from a texture; this path starts from a D3D9 RENDER TARGET and
goes StretchRect -> GetRenderTargetData -> LockRect -> D3D11 map, once per eye. A fault in it would
reach the player as "one eye is black", which is the most expensive possible way to find out.

So the proxy builds its real D3D9 + D3D11 surfaces (the same code the game run uses), renders two
known colours into a render target, runs the real `capture_eye` for each eye, and reads the colours
back out of each eye's own texture. It also checks the selection rule: an eye that has not been
captured must fall back to the shared game-frame texture rather than showing nothing.

The switch is RE6VR_STEREO_SELFTEST=1 plus a `re6vr_stereo.txt` marker, because the surfaces are
created at device creation and only when that marker was set at that moment (in the game that is
exactly the same condition - see create_stereo_surfaces).

Usage:  python scripts\\stereo_selftest.py
Exit code 0 when the self-test passes, 1 otherwise.
"""
from __future__ import annotations

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
HARNESS = os.path.join(BUILD, "harness.exe")
PROXY = os.path.join(BUILD, "d3d9.dll")
# Its own log directory, so the offline run can never overwrite a game session's log (the game's log
# is this project's only evidence, and the proxy truncates re6vr.log at start-up).
LOGDIR = os.path.join(ROOT, "_work", "harness_log")
LOG = os.path.join(LOGDIR, "re6vr.log")
POINTER = os.path.join(BUILD, "re6vr_logdir.txt")


def main() -> int:
    if not os.path.exists(HARNESS) or not os.path.exists(PROXY):
        print("[stereo-selftest] build\\harness.exe or build\\d3d9.dll is missing")
        return 1

    os.makedirs(LOGDIR, exist_ok=True)
    with open(os.path.join(LOGDIR, "re6vr_stereo.txt"), "w", encoding="ascii") as fh:
        fh.write("1\n")
    # The harness reads every marker from this directory, and a stale one changes the run it is
    # supposed to be a clean check of. `re6vr_view.txt = selftest` in particular was left behind by an
    # earlier experiment and killed the harness before it presented a single frame, which looked
    # exactly like "the stereo test did not report a verdict".
    with open(os.path.join(LOGDIR, "re6vr_view.txt"), "w", encoding="ascii") as fh:
        fh.write("off\n")
    # The read-only render-target probe belongs to a game run; leaving the marker set here would only
    # add noise to a harness log.
    with open(os.path.join(LOGDIR, "re6vr_rt_probe.txt"), "w", encoding="ascii") as fh:
        fh.write("0\n")
    # An absolute path: the proxy resolves a relative one against its own folder, which has bitten
    # this project before.
    with open(POINTER, "w", encoding="utf-8") as fh:
        fh.write(LOGDIR)

    if os.path.exists(LOG):
        os.remove(LOG)

    env = dict(os.environ)
    env["RE6VR_SELFTEST"] = "1"
    env["RE6VR_STEREO_SELFTEST"] = "1"
    env.pop("RE6VR_NO_XR", None)
    env.pop("RE6VR_CAM_SELFTEST", None)
    env.pop("RE6VR_SHOT_SELFTEST", None)

    print("[stereo-selftest] running %s (a window opens for a few seconds)" % HARNESS)
    try:
        subprocess.run([HARNESS, PROXY, "5", "320", "240"], env=env, timeout=120,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired:
        print("[stereo-selftest] the harness did not exit - treating as FAIL")
        return 1

    if not os.path.exists(LOG):
        print("[stereo-selftest] no log at %s - the proxy did not start" % LOG)
        return 1
    with open(LOG, encoding="utf-8", errors="replace") as fh:
        text = fh.read()

    for line in text.splitlines():
        if "selftest-stereo" in line or "STEREO capture armed" in line:
            print("   " + line.split("] ", 1)[-1])

    verdict = None
    for line in text.splitlines():
        if "selftest-stereo:" in line and ("-> PASS" in line or "-> FAIL" in line):
            verdict = "PASS" if "-> PASS" in line else "FAIL"
    if verdict is None:
        print("[stereo-selftest] the test did not report a verdict - see %s" % LOG)
        return 1
    print("[stereo-selftest] %s" % verdict)
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
