#!/usr/bin/env python3
"""Run the offline harness from Python with a chosen switch set, and report exactly how it died.

Why: `scripts\\_run_to2.bat` reported exit -1073741819 (0xC0000005) for the camera self-test while
`scripts\\cam_selftest.py` reported PASS minutes earlier with what looked like the same settings.
The difference has to be one of: log directory, seconds, switch set, or how the child is started.
This script varies exactly one thing per case, so the answer is a table rather than a guess.

Usage: python scripts\\harness_case.py <name> <seconds> <logdir> [switches...]
       switches: cam, shot, panel, none
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
HARNESS = os.path.join(BUILD, "harness.exe")
PROXY = os.path.join(BUILD, "d3d9.dll")
LOGDIR_POINTER = os.path.join(BUILD, "re6vr_logdir.txt")
OUTDIR = os.path.join(ROOT, "_work", "harness_log")


def main() -> int:
    name = sys.argv[1] if len(sys.argv) > 1 else "case"
    seconds = sys.argv[2] if len(sys.argv) > 2 else "1"
    logdir = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, "_work", "harness_log")
    switches = sys.argv[4:] or ["none"]

    os.makedirs(logdir, exist_ok=True)
    os.makedirs(OUTDIR, exist_ok=True)
    with open(LOGDIR_POINTER, "w", encoding="utf-8") as fh:
        fh.write(logdir)
    log = os.path.join(logdir, "re6vr.log")
    if os.path.exists(log):
        os.remove(log)

    env = dict(os.environ)
    for var in ("RE6VR_CAM_SELFTEST", "RE6VR_SHOT_SELFTEST", "RE6VR_SELFTEST"):
        env.pop(var, None)
    if "cam" in switches:
        env["RE6VR_CAM_SELFTEST"] = "1"
    if "shot" in switches:
        env["RE6VR_SHOT_SELFTEST"] = "1"
    if "panel" in switches:
        env["RE6VR_SELFTEST"] = "1"

    out_path = os.path.join(OUTDIR, "case_%s.txt" % name)
    try:
        proc = subprocess.run([HARNESS, PROXY, seconds, "320", "240"], cwd=BUILD, env=env,
                              capture_output=True, text=True, timeout=120)
        rc = proc.returncode
        out = (proc.stdout or "") + (proc.stderr or "")
    except subprocess.TimeoutExpired as e:
        rc = "timeout"
        out = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (
            e.stdout or "")
    with open(out_path, "w", encoding="utf-8") as fh:
        fh.write("rc=%s\n%s" % (rc, out))

    lines = []
    verdict = ""
    if os.path.exists(log):
        with open(log, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        lines = text.splitlines()
        for line in lines:
            if "selftest" in line and ("PASS" in line or "FAIL" in line or "returned" in line):
                verdict = line
    print("[case %s] switches=%s seconds=%s logdir=%s" % (name, ",".join(switches), seconds,
                                                           logdir))
    print("[case %s] rc=%s  proxy log lines=%d  output=%s" % (name, rc, len(lines), out_path))
    print("[case %s] last proxy line: %s" % (name, lines[-1] if lines else "(no log)"))
    if verdict:
        print("[case %s] verdict: %s" % (name, verdict))
    return 0


if __name__ == "__main__":
    sys.exit(main())
