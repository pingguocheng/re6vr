#!/usr/bin/env python3
"""Archive the previous run's log, then launch Resident Evil 6 through Steam.

Why the archive: a run's log is this project's only evidence, and the proxy TRUNCATES
re6vr.log at start-up - so every run destroys the previous one's data. That already cost a whole
session's results (the 12-scan run of 2026-09-24 23:49), and a batch version of this script
produced empty timestamps on this machine's locale, so the archiving lives in Python where the
timestamp is unambiguous.

Usage:  python scripts\\play.py            archive, then start the game via Steam
        python scripts\\play.py --no-launch  archive only
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from datetime import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6"
APP_ID = "221040"          # Resident Evil 6


def log_dir() -> str:
    """Where the proxy writes (and reads markers from): build\\re6vr_logdir.txt, else _work."""
    pointer = os.path.join(ROOT, "build", "re6vr_logdir.txt")
    if os.path.exists(pointer):
        with open(pointer, encoding="utf-8", errors="replace") as fh:
            value = fh.read().strip()
        if value:
            return value
    return os.path.join(ROOT, "_work")


def main() -> int:
    work = log_dir()
    log = os.path.join(work, "re6vr.log")
    if os.path.exists(log):
        archive = os.path.join(work, "_archive")
        os.makedirs(archive, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        dst = os.path.join(archive, "re6vr_%s.log" % stamp)
        shutil.copy2(log, dst)
        print("[play] archived the previous log -> %s (%d bytes)" % (dst, os.path.getsize(dst)))
    else:
        print("[play] no previous log at %s" % log)

    # What the probe needs armed, printed so a forgotten marker cannot silently waste a run.
    marker = os.path.join(work, "re6vr_cam.txt")
    print("[play] camera probe marker: %s"
          % (("ARMED (%s)" % open(marker).read().strip()) if os.path.exists(marker)
             else "NOT SET - run: scripts\\markers.bat cam 1"))

    # The write experiment is switched by a MARKER FILE, not by an environment variable: Steam does
    # not pass the shell's environment to the game, so an experiment set with `set ...` and then
    # launched silently never ran (one whole session was spent on that). The marker directory is the
    # channel this project can rely on.
    if "--sweep" in sys.argv or "--write-test" in sys.argv:
        mode = "sweep" if "--sweep" in sys.argv else "test"
        path = os.path.join(work, "re6vr_camwrite.txt")
        with open(path, "w", encoding="ascii") as fh:
            fh.write(mode + "\n")
        print("[play] camera write experiment armed: %s -> %s" % (mode, path))
        print("[play]   it stays idle until the engine puts a camera pose in the object (i.e. after")
        print("[play]   you load a save and the level is built), then it starts stepping.")
    else:
        stale = os.path.join(work, "re6vr_camwrite.txt")
        if os.path.exists(stale):
            os.remove(stale)
            print("[play] removed %s (no write experiment requested this run)" % stale)

    if "--no-launch" in sys.argv:
        print("[play] --no-launch: not starting the game")
        return 0

    print("[play] starting Resident Evil 6 through Steam...")
    try:
        os.startfile("steam://rungameid/%s" % APP_ID)      # noqa: S606 - the documented way
    except OSError as e:
        print("[play] could not start Steam: %s" % e)
        print("[play] start the game manually, then read %s" % log)
        return 1
    print("[play] after the run, read %s (look for the 'cam:' lines)" % log)
    return 0


if __name__ == "__main__":
    sys.exit(main())
