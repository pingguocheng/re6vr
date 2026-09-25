#!/usr/bin/env python3
"""Three-line status of the current run - the numbers that must move, or not move.

Written for the isolation runs, where the question is not "is the picture right" but "which of two
mechanisms is running, and how far did it get". A run that died can look identical to a run that was
stopped early, so the timestamps and the counters are printed together.

Usage:  python scripts\\run_status.py [_work\\re6vr.log]
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "_work", "re6vr.log")
    if not os.path.exists(path):
        print("[status] no log at %s" % path)
        return 1
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    # The very first line starts with a BOM, which would hide the timestamp of the start line.
    lines = [l.lstrip("\ufeff") for l in lines]
    if not lines:
        print("[status] the log is empty")
        return 1

    def stamp(line: str) -> str:
        m = re.match(r"\[(\d\d:\d\d:\d\d)", line)
        return m.group(1) if m else "?"

    def tail_of(line: str) -> str:
        return line.split("] ", 1)[-1] if line else ""

    def last_line(pattern: str):
        rx = re.compile(pattern)
        hit = None
        for line in lines:
            if rx.search(line):
                hit = line
        return hit

    first = lines[0]
    final = lines[-1]
    print("[status] %s" % path)
    print("   start %s   last entry %s   (%d lines)" % (stamp(first), stamp(final), len(lines)))

    # The switches that decide which mechanism is live this run.
    for pattern, label in ((r"ster: STEREO DUAL PASS armed", "stereo"),
                           (r"stereo groundwork armed", "eye offset"),
                           (r"STEREO capture armed", "per-eye capture")):
        print("   %-16s %s" % (label, tail_of(last_line(pattern))))

    # The counters: "second passes" must stay 0 in capture-only mode, "guard refusals" then moves.
    for pattern, label in ((r"ster: STEREO: second passes", "second passes"),
                           (r"STEREO eye 0 frame \d+ copied", "eye 0 capture"),
                           (r"STEREO eye 1 frame \d+ copied", "eye 1 capture"),
                           (r"compositor: copied \d+ frames", "single-texture copies"),
                           (r"openxr: \d+ frames submitted", "frames submitted")):
        hit = last_line(pattern)
        print("   %-16s %s" % (label, ("[%s] %s" % (stamp(hit), tail_of(hit))) if hit else "(none)"))

    # Anything that complained, and whether the process ended cleanly.
    bad = [l for l in lines if "WARN" in l or "DISARMED" in l or "standing down" in l or
           "STUCK" in l or "FAIL" in l]
    print("   %-16s %d line(s)%s" % ("problems", len(bad),
                                     (": " + tail_of(bad[-1])) if bad else ""))
    detached = any("process detach" in l for l in lines)
    print("   %-16s %s" % ("exit", "clean (proxy detached)" if detached
                           else "NO detach line - the process died instead of exiting"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
