"""Drop the mangled handoff section that a PowerShell here-string wrote to the top of README.md.

That section is the only damaged part of the file (3835 replacement runes, all within it, 0
before it - measured by repair_readme.py). It gets removed here and rewritten properly with the
edit tool, which writes UTF-8.

Everything below the marker is untouched: the script only deletes a prefix and verifies what
remains.
"""
MARK = "# RE6 VR Mod"
p = r"C:\re6vr\README.md"

text = open(p, encoding="utf-8").read()
i = text.find(MARK)
if i <= 0:
    raise SystemExit("marker not found (i=%d) - refusing to touch the file" % i)

head, keep = text[:i], text[i:]
bad_head = head.count("\ufffd")
bad_keep = keep.count("\ufffd")
print("removing %d chars of prefix, %d damaged runes in it" % (len(head), bad_head))
print("keeping %d chars, %d damaged runes in the rest (must be 0)" % (len(keep), bad_keep))
if bad_keep:
    raise SystemExit("the part that must survive is damaged too - stopping")

with open(p, "w", encoding="utf-8", newline="") as fh:
    fh.write(keep)
print("wrote README.md (%d bytes, UTF-8)" % len(keep.encode("utf-8")))
