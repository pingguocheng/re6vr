#!/usr/bin/env python3
"""Read a stereo run's log and say what actually happened - the run's evidence in one screen.

Why: the player's report ("it looks 3D" / "it looks the same" / "one eye is black") has to be matched
against numbers, not against a hunch, and the log is where they are. This turns the handful of lines
that matter into a verdict, with each claim kept separate, because in this project the expensive
mistakes were always one claim being assumed from another:
    "the engine rendered twice"  !=  "the two renders used different poses"
    "two frames were captured"   !=  "each eye sampled its own frame"

Usage:  python scripts\\stereo_report.py [_work\\re6vr.log]
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_lines(path: str) -> list[str]:
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def last(pattern: str, lines: list[str]) -> str | None:
    rx = re.compile(pattern)
    hit = None
    for line in lines:
        if rx.search(line):
            hit = line
    return hit


def all_matches(pattern: str, lines: list[str], limit: int = 6) -> list[str]:
    rx = re.compile(pattern)
    out = [l for l in lines if rx.search(l)]
    return out[:limit] + (["   ... %d more" % (len(out) - limit)] if len(out) > limit else [])


def tail(line: str | None) -> str:
    return line.split("] ", 1)[-1] if line else "(not present in the log)"


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "_work", "re6vr.log")
    if not os.path.exists(path):
        print("[stereo-report] no log at %s" % path)
        return 1
    lines = read_lines(path)
    print("[stereo-report] %s (%d lines)" % (path, len(lines)))
    print()

    # --- 0. did the switches even get read? -------------------------------------------------------
    print("SWITCHES")
    for pat in (r"stereo: ENABLED", r"stereo groundwork armed", r"IPD .* armed"):
        print("   " + tail(last(pat, lines)))

    # --- 1. the engine rendered twice -------------------------------------------------------------
    print()
    print("1. DID THE ENGINE RENDER TWICE PER FRAME?")
    first = last(r"RENDER phase call \d+", lines)
    print("   first: " + tail(first))
    print("   last : " + tail(last(r"RENDER phase call \d+", lines)))
    print("   " + tail(last(r"ster: STEREO: second passes", lines)))
    m = re.search(r"second passes (\d+)", tail(last(r"ster: STEREO: second passes", lines)) or "")
    if m:
        n = int(m.group(1))
        print("   -> %s (%d second pass(es))" % ("YES" if n > 0 else "NO", n))
    else:
        print("   -> no stereo heartbeat line at all: re6vr_stereo.txt was not 1 at start-up")

    # --- 2. the two renders used different eye poses ----------------------------------------------
    print()
    print("2. DID THE TWO RENDERS USE DIFFERENT EYE POSES? (the eye offset)")
    for line in all_matches(r"IPD .* applied to eye", lines, 4):
        print("   " + tail(line))
    m = re.search(r"eye-offset writes (\d+)", tail(last(r"ster: STEREO: second passes", lines)) or "")
    if m:
        n = int(m.group(1))
        print("   -> %s (%d write(s) reached the engine's pose)"
              % ("YES" if n > 0 else "NO - the offset never landed", n))
    print("   builder heartbeat: " + tail(last(r"builder heartbeat", lines)))

    # --- 3. two pictures were captured ------------------------------------------------------------
    print()
    print("3. WERE TWO PICTURES CAPTURED?")
    print("   " + tail(last(r"STEREO capture armed", lines)))
    e0 = all_matches(r"STEREO eye 0 frame (\d+) copied .*fingerprint (\d+)", lines, 2)
    e1 = all_matches(r"STEREO eye 1 frame (\d+) copied .*fingerprint (\d+)", lines, 2)
    for line in e0 + e1:
        print("   " + tail(line))
    fp = {}
    for eye, group in ((0, e0), (1, e1)):
        hits = re.findall(r"fingerprint (\d+)", " ".join(group))
        if hits:
            fp[eye] = hits[-1]
    if len(fp) == 2:
        print("   -> %s" % ("both eyes captured, and the fingerprints %s"
                            % ("DIFFER (different pictures)" if fp[0] != fp[1]
                               else "are IDENTICAL - the two renders produced the same picture"))
              )
    else:
        print("   -> only %d eye(s) captured" % len(fp))

    # --- 4. each eye sampled its own frame --------------------------------------------------------
    print()
    print("4. DID EACH EYE SAMPLE ITS OWN FRAME?")
    print("   " + tail(last(r"submit: .*frames submitted", lines)))
    for line in all_matches(r"submit: eye[01]", lines, 4):
        print("   " + tail(line))

    # --- 5. anything that refused to work ---------------------------------------------------------
    print()
    print("PROBLEMS REPORTED")
    problems = [l for l in lines if "WARN" in l or "FAIL" in l or "STUCK" in l or "refus" in l]
    if problems:
        for line in problems[-10:]:
            print("   " + tail(line))
    else:
        print("   (none)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
