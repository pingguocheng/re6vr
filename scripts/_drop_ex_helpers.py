import io
import sys

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    lines = f.readlines()

# Drop the now-unused D3D9Ex helpers: from the "Lazily obtains an IDirect3D9Ex
# factory" comment up to (but not including) the ensure_shared_surface comment.
start = None
for i, ln in enumerate(lines):
    if "Lazily obtains an IDirect3D9Ex factory" in ln:
        start = i
        break
if start is None:
    sys.exit("start not found")
while start > 0 and lines[start - 1].strip().startswith("//"):
    start -= 1

end = None
for i, ln in enumerate(lines):
    if "Creates (or recreates) the D3D11 texture" in ln and i > start:
        end = i
        break
if end is None:
    sys.exit("end not found")
while end > start and lines[end - 1].strip().startswith("//"):
    end -= 1
end -= 1

out = lines[:start] + ["    // NOTE: the D3D9Ex shared-surface path was removed on purpose.\n",
                       "    // RE6 uses a plain D3D9 device and cross-device blits into a D3D9Ex\n",
                       "    // surface fault inside d3d9.dll; see copy_backbuffer below.\n",
                       "\n"] + lines[end + 1:]
with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.writelines(out)
print("removed lines %d..%d" % (start + 1, end + 1))
