import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

old = "        const XrVector3f real_eye = eye_pose.position;\n        XrVector3f eye = real_eye;\n"
new = '''        const XrVector3f real_eye = eye_pose.position;

        // Place the screen before anything is derived from it. This call had been
        // dropped by an earlier edit, which left panel_center at its initial
        // (0,0,0) while the eye sat near the origin too: the projection collapsed
        // to w = 0 and the composited image was pure black in both eyes.
        if (!panel_placed) {
            place_panel(real_eye, fwd);
        } else if (yaw_follow) {
            const float len2 = fwd[0] * fwd[0] + fwd[2] * fwd[2];
            if (len2 > 1e-4f) {
                const float fl = sqrtf(len2);
                const float dot = (panel_fwd[0] * fwd[0] + panel_fwd[2] * fwd[2]) / fl;
                if (dot < 0.7f) place_panel_at(real_eye, fwd);   // ~45 degrees
            }
        }

        XrVector3f eye = real_eye;
'''
assert old in t
t = t.replace(old, new, 1)

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("restored the place_panel call")
