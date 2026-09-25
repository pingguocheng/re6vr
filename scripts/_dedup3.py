import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    lines = f.readlines()

# Keep only the first s_eye_logs declaration block.
idx = [i for i, l in enumerate(lines) if "static int s_eye_logs = 0;" in l]
print("s_eye_logs at lines:", [i + 1 for i in idx])
for i in reversed(idx[1:]):
    # remove the declaration and its following if-block
    start = i
    end = start
    while "}" not in lines[end]:
        end += 1
    del lines[start:end + 1]

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.writelines(lines)
print("kept one s_eye_logs block")
