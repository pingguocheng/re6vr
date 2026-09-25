import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

start = t.index("    // Copies a 16x16 corner from the blit target into a tiny SYSTEMMEM surface.")
end = t.index("    void release_shared_surface() {")
block = t[start:end]
t = t[:start] + t[end:]

# ReadbackJob is declared after create_device_surfaces, so the probe has to move
# past it. Insert right before copy_backbuffer.
anchor = "    // Mirrors the game's back buffer into the D3D11 texture the compositor"
t = t.replace(anchor, block + anchor, 1)

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("moved probe_readback after ReadbackJob")
