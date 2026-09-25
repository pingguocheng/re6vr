import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# real_eye must exist before the placement call, which uses it.
old = "        if (!panel_placed) {\n            place_panel(real_eye, fwd);"
new = "        const XrVector3f real_eye = eye_pose.position;\n\n        if (!panel_placed) {\n            place_panel(real_eye, fwd);"
assert old in t
t = t.replace(old, new, 1)

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("declared real_eye before the placement call")
