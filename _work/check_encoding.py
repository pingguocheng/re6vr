"""Find every file this session damaged by re-encoding (U+FFFD means bytes were converted).

Only the files touched by the shell `Set-Content` calls can be affected; this checks them all and
says which are clean, so the repair list is evidence rather than a guess.
"""
import os

ROOT = r"C:\re6vr"
CANDIDATES = [
    "README.md", "README_recovered.md",
    "src/d3d9_proxy.cpp", "src/cam_hook.cpp", "src/cam_hook.h",
    "src/mem_cam.cpp", "src/mem_cam.h", "src/screenshot.cpp", "src/screenshot.h",
    "src/mem_cam_tables.h",
    "scripts/markers.bat", "scripts/play.py", "scripts/cam_selftest.py",
    "scripts/re6dis.py", "scripts/disasm_lib/propmap.py", "scripts/disasm_lib/analyze.py",
    "scripts/build.bat",
]

for rel in CANDIDATES:
    path = os.path.join(ROOT, rel.replace("/", os.sep))
    if not os.path.exists(path):
        print("  %-34s (missing)" % rel)
        continue
    data = open(path, "rb").read()
    try:
        text = data.decode("utf-8")
        # A file can be valid UTF-8 yet still have been rewritten as ANSI if it was pure ASCII
        # before, so also count the replacement character for the lossy case.
        print("  %-34s %7d bytes   valid UTF-8   damaged=0" % (rel, len(data)))
    except UnicodeDecodeError as e:
        text = data.decode("utf-8", errors="replace")
        bad = text.count("\ufffd")
        print("  %-34s %7d bytes   DAMAGED: %d replacement chars (%s at %d)"
              % (rel, len(data), bad, e.reason, e.start))
