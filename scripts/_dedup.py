import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    lines = f.readlines()

def find_all(needle):
    return [i for i, l in enumerate(lines) if needle in l]

# Drop the first (now duplicated) real_eye declaration at line 1438 (1-based).
dup = find_all("const XrVector3f real_eye = eye_pose.position;")
assert len(dup) >= 2, dup
del lines[dup[0]]

# Drop the first s_eye_logs declaration (the one merged from the old block).
dup2 = find_all("static int s_eye_logs = 0;")
assert len(dup2) >= 2, dup2
# remove the whole old logging block that follows it (up to the closing brace)
start = dup2[0]
end = start
while "}" not in lines[end]:
    end += 1
del lines[start:end + 1]

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.writelines(lines)
print("removed duplicate declarations")
