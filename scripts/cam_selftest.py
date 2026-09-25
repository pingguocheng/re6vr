#!/usr/bin/env python3
"""Run the camera probe's synthetic self-test, without the game and without a headset.

The probe (`src/mem_cam.cpp`) walks process memory for objects whose class pointer is
sBioCamera and checks each one's camera fields geometrically. Its failure mode is a clean,
confident "NO live sBioCamera object found" - and that reads the same whether the search is
right and there was no stage loaded, or the search itself is broken.

So the search is tested against objects it MUST find and MUST reject:

  * a planted object with the right class pointer and a valid look-at pose,
  * a planted object with the right class pointer and a pose that cannot be one.

Both are allocated and freed by the probe itself. This script only drives the offline harness
(`build\\harness.exe`), sets the two environment variables that arm the self-test, and reads
the verdict out of the log.

Usage:  python scripts\\cam_selftest.py
Exit code 0 when the self-test passes, 1 otherwise.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
HARNESS = os.path.join(BUILD, "harness.exe")
PROXY = os.path.join(BUILD, "d3d9.dll")
# A log directory of its own. The harness writes the same `re6vr.log` name the game does, so
# running the self-test used to DESTROY the previous game session's log - which is how the
# evidence from the 12-scan run (2026-09-24 23:49) was lost. Logs are this project's only
# evidence, so the offline run gets its own directory.
LOGDIR = os.path.join(ROOT, "_work", "harness_log")
LOG = os.path.join(LOGDIR, "re6vr.log")


def main() -> int:
    for path in (HARNESS, PROXY):
        if not os.path.exists(path):
            print("[cam-selftest] missing %s" % path)
            print("               build it with:  scripts\\build.bat  +  scripts\\build_harness.bat")
            return 2

    # Keep the harness away from the game's log (see LOGDIR) - and put the pointer back
    # afterwards, because it is the same file the proxy reads to find the marker directory. A
    # self-test that leaves it pointing at `harness_log` would make the next real game run read
    # no markers at all, i.e. the switch that arms this probe would silently not exist.
    logdir_file = os.path.join(BUILD, "re6vr_logdir.txt")
    original = None
    if os.path.exists(logdir_file):
        with open(logdir_file, encoding="utf-8") as fh:
            original = fh.read()
    os.makedirs(LOGDIR, exist_ok=True)
    with open(logdir_file, "w", encoding="utf-8") as fh:
        fh.write(LOGDIR)
    if os.path.exists(LOG):
        os.remove(LOG)

    try:
        return _run(env_extra={"RE6VR_CAM_SELFTEST": "1", "RE6VR_SELFTEST": "1",
                              "RE6VR_SHOT_SELFTEST": "1"})
    finally:
        if original is not None:
            with open(logdir_file, "w", encoding="utf-8") as fh:
                fh.write(original)
        print("[cam-selftest] log directory restored to %r" % (original or "").strip())


def _run(env_extra: dict) -> int:
    env = dict(os.environ)
    env.update(env_extra)

    print("[cam-selftest] running %s (this starts a window for a few seconds)" % HARNESS)
    proc = subprocess.run([HARNESS, PROXY, "3", "320", "240"], cwd=BUILD, env=env,
                          capture_output=True, text=True, timeout=180)
    out = (proc.stdout or "") + (proc.stderr or "")
    for line in out.strip().splitlines():
        print("   " + line)

    if not os.path.exists(LOG):
        print("[cam-selftest] no log at %s - the proxy did not run" % LOG)
        return 2
    with open(LOG, encoding="utf-8", errors="replace") as fh:
        log = fh.read()

    verdict = None
    for line in log.splitlines():
        if "cam: selftest:" in line and ("PASS" in line or "FAIL" in line):
            verdict = line
    for line in log.splitlines():
        if ("cam: selftest" in line and "must-find" not in line) or "shot:" in line:
            print("   " + line.split("] ", 1)[-1])
    if verdict is None:
        print("[cam-selftest] the self-test did not report - the probe never ran")
        print("               (check that %s was rebuilt: the strings 'cam: selftest' must be "
              "in it)" % PROXY)
        return 2

    print()
    print("[cam-selftest] %s" % verdict.split("] ", 1)[-1])
    return 0 if "PASS" in verdict else 1


if __name__ == "__main__":
    sys.exit(main())
