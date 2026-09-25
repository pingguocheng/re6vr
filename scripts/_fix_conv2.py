import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# Find the collapse block by its unique marker comment and replace the whole
# region from `XrVector3f eye = real_eye;` through the closing brace, plus the
# "AFTER collapse" diagnostic and the "converge" diagnostic.
s = t.index("        XrVector3f eye = real_eye;")
marker = "        // Screen -> world. Translation in the last row, basis vectors in the rows:"
e = t.index(marker)

new = '''        // One convergence point for both eyes.
        //
        // ipd_scale 0 (the default) means "both eyes render from the same point",
        // which is what removes the double image: a flat screen has no stereo
        // depth to lose, only an offset to gain. Scaling 1 keeps the real eye.
        //
        // Written as a single explicit branch rather than a scaled lerp: the lerp
        // version was silently not taking effect (the diagnostic showed the eye
        // coming out identical to the raw headset position, i.e. 63 mm apart in
        // both eyes - exactly the "two screens" that was reported).
        const XrVector3f real_eye = eye_pose.position;
        XrVector3f eye;
        if (ipd_scale <= 0.0f) {
            eye.x = panel_center[0];
            eye.y = panel_center[1];
            eye.z = panel_center[2];
        } else if (ipd_scale >= 1.0f) {
            eye = real_eye;
        } else {
            eye.x = panel_center[0] + (real_eye.x - panel_center[0]) * ipd_scale;
            eye.y = panel_center[1] + (real_eye.y - panel_center[1]) * ipd_scale;
            eye.z = panel_center[2] + (real_eye.z - panel_center[2]) * ipd_scale;
        }

        static int s_eye_logs = 0;
        if (s_eye_logs < 4) {
            ++s_eye_logs;
            VRLOG("converge eye%u: real (%.4f %.4f %.4f) -> used (%.4f %.4f %.4f), "
                  "panel (%.3f %.3f %.3f), ipd_scale=%.2f",
                  eye_index, real_eye.x, real_eye.y, real_eye.z, eye.x, eye.y, eye.z,
                  panel_center[0], panel_center[1], panel_center[2], ipd_scale);
        }

'''

t = t[:s] + new + t[e:]

# The assertion above assumed a "collapse eye" diagnostic; if it is still present
# anywhere, strip it so it cannot confuse a later reading.
while "collapse eye%u" in t:
    a = t.index("            static int s_col = 0;")
    b = t.index("            if (d > 1e-3f) {", a)
    t = t[:a] + t[b:]

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("convergence rewritten as an explicit branch")
