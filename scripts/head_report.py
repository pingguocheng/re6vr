#!/usr/bin/env python3
"""Read a NON-stereo (head-tracking) run's log and check the regression list in one screen.

The list is the one the handoff has carried since the head tracking was accepted - "still and it does
not drift", "a left/right turn is purely horizontal", "nodding is 1:1", "a big angle does not tilt the
view", "no crash in a cutscene", "stereo off still shows the game". Four of those are the player's
judgement; what a log can settle is the machinery behind them, and it must be checked in this order,
because the expensive failures in this project were all "the feature never ran and the log looked
fine":

  1. the hooks are installed and being called at all (builder heartbeat, GetViewMatrix counter)
  2. nothing the filter rejects is being steered (no "NOT steering camera" lines)
  3. the stereo path is completely inert (second passes 0)
  4. the picture reached the headset (frames submitted climbing)
  5. anything that refused to work

Usage:  python scripts\\head_report.py [_work\\re6vr.log]
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "_work", "re6vr.log")
    if not os.path.exists(path):
        print("[head-report] no log at %s" % path)
        return 1
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    print("[head-report] %s (%d lines)" % (path, len(lines)))

    def last(pattern, default="(not present in the log)"):
        rx = re.compile(pattern)
        hit = None
        for line in lines:
            if rx.search(line):
                hit = line
        return hit.split("] ", 1)[-1] if hit else default

    print()
    print("SWITCHES READ")
    print("   " + last(r"steer: ENABLED"))

    print()
    print("1. ARE THE HOOKS LIVE?")
    print("   " + last(r"MakeViewMatrix detour is live"))
    print("   " + last(r"GetViewMatrix detour installed"))
    print("   " + last(r"builder heartbeat"))
    print("   " + last(r"GetViewMatrix called \d+ times"))

    print()
    print("2. IS ANYTHING STEERED THAT SHOULD NOT BE?")
    bad = [l for l in lines if "NOT steering camera" in l or "not GetViewMatrix" in l]
    print("   (%d line(s) rejecting an unrelated camera or caller)" % len(bad))
    for line in bad[:3]:
        print("   " + line.split("] ", 1)[-1])

    print()
    print("3. IS THE STEREO PATH INERT?  (this run is the control)")
    print("   " + last(r"RENDER phase call \d+"))
    print("   " + last(r"STERE0|STEREO"))
    print("   " + last(r"stereo surfaces NOT created", "(no stereo-surface line: created only when "
                                                     "re6vr_stereo.txt is 1)"))

    print()
    print("4. DID THE PICTURE REACH THE HEADSET?")
    for pattern in (r"openxr: eye \d runtime FOV", r"submit: .*frames submitted",
                    r"compositor: copied \d+ frames", r"compositor: frame \d+ .*non-black"):
        print("   " + last(pattern))

    print()
    print("5. PROBLEMS REPORTED")
    problems = [l for l in lines if "WARN" in l or "FAIL" in l or "STUCK" in l or "crash" in l.lower()]
    if problems:
        for line in problems[-10:]:
            print("   " + line.split("] ", 1)[-1])
    else:
        print("   (none)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
