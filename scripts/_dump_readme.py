import io

p = r"C:\re6vr\README.md"
raw = open(p, "rb").read()
text = raw.decode("utf-8", errors="replace")

out = []
out.append("bytes=%d chars=%d lines=%d" % (len(raw), len(text), text.count("\n") + 1))
out.append("--- first 60 lines ---")
for i, line in enumerate(text.splitlines()[:60], 1):
    out.append("%3d| %s" % (i, line))

io.open(r"C:\re6vr\_readme_dump.txt", "w", encoding="utf-8").write("\n".join(out))
print("dumped")
