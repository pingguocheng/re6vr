import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# Remove the obsolete lazy-creation function: prepare()/create_device_surfaces()
# replaced it, and leaving it behind keeps stale scaler/lock_surface references.
start = t.index("    // Creates (or recreates) the D3D11 texture the game frame is mirrored into.")
end = t.index("    void release_shared_surface() {")
removed = end - start
t = t[:start] + t[end:]

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("removed %d chars of the obsolete lazy-creation function" % removed)
