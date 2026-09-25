"""Repair README.md: restore UTF-8, then repair the damage that can actually be repaired.

The lost characters are NOT recoverable - `Set-Content` converted the bytes and the originals are
gone. What is recoverable is everything I wrote in this session (the handoff section, the probe
sections) because that text exists in the conversation, and everything structural, because ASCII
survived intact.

The script:
  1. rewrites the file as UTF-8 (the actual fix),
  2. prints the damaged runes as escaped ASCII, so a GBK console can show them,
  3. reports the damage per section, so what was lost is stated rather than hidden.
"""
import re

SRC = r"C:\re6vr\README_recovered.md"
OUT = r"C:\re6vr\README.md"
R = "\ufffd"

text = open(SRC, encoding="utf-8").read()
bad = text.count(R)
print("damaged runes: %d of %d chars (%.1f%%)" % (bad, len(text), 100.0 * bad / len(text)))
print("headings total: %d, damaged: %d"
      % (len(re.findall(r"^#{1,4} ", text, re.M)),
         len([1 for m in re.finditer(r"^#{1,4} (.*)$", text, re.M) if R in m.group(1)])))

i = text.find("2026-09-25")
print("damage before the handoff section: %d ; from it onward: %d"
      % (text[:i].count(R), text[i:].count(R)))

# Show the damaged headings as escapes: the console here is GBK and cannot print U+FFFD, and the
# point of listing them is to know which ones need rewriting by hand.
print("first 15 damaged headings (escaped):")
for n, m in enumerate(re.finditer(r"^#{1,4} (.*)$", text, re.M)):
    if R in m.group(1):
        print("   " + m.group(1).encode("unicode_escape").decode("ascii")[:100])
        if n > 400:
            break
        if sum(1 for x in re.finditer(r"^#{1,4} (.*)$", text[:m.start()], re.M) if R in x.group(1)) >= 14:
            break

with open(OUT, "w", encoding="utf-8", newline="") as fh:
    fh.write(text)
print("wrote %s as UTF-8" % OUT)
