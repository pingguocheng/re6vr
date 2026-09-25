#!/usr/bin/env python3
"""Minimal A/B for a crash that should not be happening: same command, two ways.

Observed: `scripts\\cam_selftest.py` (which calls the harness with RE6VR_CAM_SELFTEST=1 etc.) PASSES,
while `scripts\\harness_case.py` with the same switches returns 0xC0000005 and the proxy writes no
log at all. The two differ only in how the child is started, so this script isolates that single
variable: the same subprocess call, run twice, once exactly as cam_selftest.py does it and once
exactly as harness_case.py does it.
"""
from __future__ import annotations

import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
HARNESS = os.path.join(BUILD, "harness.exe")
PROXY = os.path.join(BUILD, "d3d9.dll")
LOGDIR = os.path.join(ROOT, "_work", "harness_log")


def run(tag: str, *, cwd, extra_env: dict, seconds: str) -> None:
    env = dict(os.environ)
    env.update(extra_env)
    try:
        proc = subprocess.run([HARNESS, PROXY, seconds, "320", "240"], cwd=cwd, env=env,
                              capture_output=True, text=True, timeout=120)
        rc = proc.returncode
        tail = (proc.stdout or "").strip().splitlines()
    except subprocess.TimeoutExpired:
        rc, tail = "timeout", []
    print("[%s] rc=%s cwd=%s seconds=%s first_stdout=%s"
          % (tag, rc, cwd, seconds, tail[0] if tail else "(none)"))


def main() -> int:
    os.makedirs(LOGDIR, exist_ok=True)
    with open(os.path.join(BUILD, "re6vr_logdir.txt"), "w", encoding="utf-8") as fh:
        fh.write(LOGDIR)

    cam_only = {"RE6VR_CAM_SELFTEST": "1", "RE6VR_SELFTEST": "1", "RE6VR_SHOT_SELFTEST": "1"}
    # 1: exactly like cam_selftest.py: three switches, seconds=3
    run("as-cam_selftest", cwd=BUILD, extra_env=cam_only, seconds="3")
    # 2: exactly like harness_case.py: the same three switches (it sets all three too)
    run("as-harness_case", cwd=BUILD, extra_env=cam_only, seconds="3")
    # 3: one switch only
    run("cam-only-switch", cwd=BUILD, extra_env={"RE6VR_CAM_SELFTEST": "1"}, seconds="3")
    # 4: no switches at all
    run("no-switches", cwd=BUILD, extra_env={}, seconds="3")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
