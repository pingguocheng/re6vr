import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# ReadbackJob is defined inside do_copy's vicinity; find it wherever it lives and
# lift it, together with CopyJob, above create_device_surfaces.
def lift(marker_start, marker_end):
    global t
    s = t.index(marker_start)
    e = t.index(marker_end, s)
    block = t[s:e]
    t = t[:s] + t[e:]
    return block

# ReadbackJob block
rb_start = t.index("    // SEH requires the guarded work in its own function with no C++ objects that")
rb_end = t.index("    ", t.index("};", t.index("struct ReadbackJob", rb_start))) + 4
rb_block = t[rb_start:rb_end]
t = t[:rb_start] + t[rb_end:]

# CopyJob block
cj_start = t.index("    struct CopyJob {")
cj_end = t.index("    bool do_copy(", cj_start)
cj_block = t[cj_start:cj_end]
t = t[:cj_start] + t[cj_end:]

anchor = "    bool create_device_surfaces(IDirect3DDevice9 *dev, uint32_t w, uint32_t h, D3DFORMAT fmt) {"
t = t.replace(anchor, rb_block + cj_block + anchor, 1)

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("lifted CopyJob + ReadbackJob above create_device_surfaces")
