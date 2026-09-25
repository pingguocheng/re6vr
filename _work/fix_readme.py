"""Recover README.md after a PowerShell Set-Content re-encode damaged it.

What happened: the file is UTF-8, but `Set-Content` without -Encoding wrote it in the machine's
ANSI code page (GBK here), converting every non-ASCII character. The original UTF-8 bytes are
gone; what is in the file now is a mix (parts that are still valid UTF-8 survive, the converted
parts show up as replacement characters).

This script measures the damage and writes a recovered copy, so the decision about what to keep
is made on evidence rather than on a guess.
"""
import sys

PATH = r"C:\re6vr\README.md"
OUT = r"C:\re6vr\README_recovered.md"

data = open(PATH, "rb").read()
print("file size: %d bytes" % len(data))

text = data.decode("utf-8", errors="replace")
bad = text.count("\ufffd")
print("decoded with replacement: %d chars, %d damaged" % (len(text), bad))
print("headings: %d   code fences: %d   lines: %d"
      % (text.count("## "), text.count("```"), text.count("\n")))

for probe in ("收工状态", "THE SWEEP STARTS NOW", "play.py --sweep", "mem_cam.cpp",
              "camhook:", "mCameraOrg", "sBioCamera"):
    print("  contains %-22s %s" % (probe, probe in text))

# How much of the file is damaged? A percentage tells whether a repair is worth attempting.
print("damaged fraction: %.1f%%" % (100.0 * bad / max(1, len(text))))

with open(OUT, "w", encoding="utf-8", newline="") as fh:
    fh.write(text)
print("wrote %s (UTF-8, damage marked with U+FFFD)" % OUT)
