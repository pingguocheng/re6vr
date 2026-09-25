import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# 1. The mirror hypothesis was wrong: measured TL was blue / TR was green, which
#    is exactly the source layout. Remove the flip that "fixed" it.
t = t.replace('''        // The panel rectangle's right edge, as seen from the eye, maps to the
        // *left* of the NDC cube: the view basis is built with the screen's right
        // along +x, but the off-centre frustum's x shear is negated below, which
        // mirrors it. The net effect was a horizontally flipped image (the offline
        // selftest caught it: the top-left sample read the source's right half).
        // Flipping the quad's u in the constant buffer cancels it.
        const float u_flip = -1.0f;

''', '')
t = t.replace("        cb.scale_bias[0] = panel_w * 0.5f * u_flip;   // mirrors the quad in x",
              "        cb.scale_bias[0] = panel_w * 0.5f;")

# 2. The selftest sampled with the wrong formula: it ignored that the frustum is
#    derived from the panel, so nx/ny came out far too large and the probes landed
#    outside the screen (reading the surround, not a quadrant).
old = '''                    const float tanx = tanf(test_fov.angleRight);
                    const float tany = tanf(test_fov.angleUp);
                    const float nx = (panel_w * 0.5f / screen_dist) / tanx;
                    const float ny = (panel_h * 0.5f / screen_dist) / tany;'''
new = '''                    // The frustum is built from the panel itself, so half the
                    // panel maps to exactly half the NDC range. Probing at +-0.25
                    // of the panel therefore lands squarely inside each quadrant.
                    // The old version divided the panel's angular half-size by the
                    // whole eye FOV tangent, which put the probes outside the
                    // screen and made the test read the surround.
                    const float nx = 0.5f;
                    const float ny = 0.5f;'''
assert old in t
t = t.replace(old, new, 1)

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("selftest sample points corrected; bogus mirror removed")
