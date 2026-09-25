import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    lines = f.readlines()

idx = [i for i, l in enumerate(lines) if "const XrVector3f real_eye = eye_pose.position;" in l]
print("declarations at lines:", [i + 1 for i in idx])
if len(idx) > 1:
    # keep the first, drop the rest
    for i in reversed(idx[1:]):
        del lines[i]
with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.writelines(lines)
print("kept 1 declaration")
