#!/usr/bin/env python3
"""Run every offline check the proxy has, in one command: no game, no headset.

Why this exists: the checks are the only way to catch a broken path before a game run costs the
player a session, but they are driven from three different switches and two different log
directories, and one of them used to overwrite the running game's log. One command means the whole
suite is run before every deployment instead of the one check that happens to be remembered.

Checks, in order:
  1. stereo capture  (scripts\\stereo_selftest.py)  - D3D9 render target -> per-eye texture
  2. panel rendering (RE6VR_SELFTEST=1)             - both eyes sample, orientation, coverage
  3. real geometry   (RE6VR_SELFTEST_REAL=1)        - the runtime's portrait slice and 16:9 source
  4. camera probe    (scripts\\cam_selftest.py)     - must-find / must-reject camera fixtures

Usage:  python scripts\\check_all.py
Exit code 0 only when every check passes.
"""
from __future__ import annotations

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
HARNESS = os.path.join(BUILD, "harness.exe")
PROXY = os.path.join(BUILD, "d3d9.dll")
LOGDIR = os.path.join(ROOT, "_work", "harness_log")
LOG = os.path.join(LOGDIR, "re6vr.log")
POINTER = os.path.join(BUILD, "re6vr_logdir.txt")


def run(name: str, env_extra: dict, args: list) -> tuple[bool, str]:
    os.makedirs(LOGDIR, exist_ok=True)
    if os.path.exists(LOG):
        os.remove(LOG)
    env = dict(os.environ)
    for key in ("RE6VR_SELFTEST", "RE6VR_SELFTEST_REAL", "RE6VR_STEREO_SELFTEST",
                "RE6VR_CAM_SELFTEST", "RE6VR_NO_XR"):
        env.pop(key, None)
    env.update(env_extra)
    with open(POINTER, "w", encoding="utf-8") as fh:
        fh.write(LOGDIR)
    try:
        subprocess.run([HARNESS, PROXY] + args, env=env, timeout=180,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return False, "the harness did not exit"
    if not os.path.exists(LOG):
        return False, "no log written"
    with open(LOG, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    return True, text


def main() -> int:
    if not os.path.exists(HARNESS) or not os.path.exists(PROXY):
        print("[check] build\\harness.exe or build\\d3d9.dll is missing - run scripts\\build.bat")
        return 1

    results = []

    # 1. stereo capture, through its own script (it owns the re6vr_stereo.txt marker).
    rc = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "stereo_selftest.py")],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    out = rc.stdout.decode("utf-8", "replace")
    results.append(("stereo capture", rc.returncode == 0, [l for l in out.splitlines()
                                                           if "selftest-stereo:" in l]))

    # 2/3. panel rendering, with and without the runtime's real slice geometry. The two passes word
    # their verdicts differently ("selftest: PASS" vs "selftest-real: eye buffers written"), so each
    # gets the string that is actually its pass line rather than a substring search for "FAIL".
    for name, env_extra, pass_text in (
        ("panel rendering", {"RE6VR_SELFTEST": "1"}, "selftest: PASS"),
        ("panel (real geometry)", {"RE6VR_SELFTEST": "1", "RE6VR_SELFTEST_REAL": "1"},
         "selftest-real: eye buffers written"),
    ):
        ok, text = run(name, env_extra, ["5", "320", "240"])
        lines = [l.split("] ", 1)[-1] for l in text.splitlines()
                 if "selftest" in l and ("PASS" in l or "FAIL" in l or "written" in l)]
        passed = ok and any(pass_text in l for l in lines)
        results.append((name, passed, lines))

    # 4. the camera probe's fixtures.
    rc = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "cam_selftest.py")],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    out = rc.stdout.decode("utf-8", "replace")
    results.append(("camera probe", rc.returncode == 0, [l.strip() for l in out.splitlines()
                                                         if "PASS" in l or "FAIL" in l]))

    print()
    for name, passed, lines in results:
        print("[check] %-24s %s" % (name, "PASS" if passed else "FAIL"))
        for line in lines[-3:]:
            print("           %s" % line)
    all_ok = all(p for _, p, _ in results)
    print("[check] %s" % ("ALL PASS" if all_ok else "SOMETHING FAILED"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
