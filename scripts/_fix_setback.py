import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

s = t.index("        const XrVector3f real_eye = eye_pose.position;")
marker = "        // Screen -> world. Translation in the last row, basis vectors in the rows:"
e = t.index(marker)

new = '''        const XrVector3f real_eye = eye_pose.position;

        // One convergence point for both eyes, sitting `eye_setback` metres in
        // FRONT of the screen plane (on the viewer's side).
        //
        // Putting it exactly on the panel makes the panel's view-space depth zero,
        // and the projection then has to describe a 1.45 m half-width at distance
        // zero: a ~88 degree half-angle that the shader's frustum cannot express,
        // so the quad landed entirely off-screen and the headset showed nothing.
        // `eye_setback` keeps the panel at a sane distance while still letting both
        // eyes share one viewpoint, which is what removes the double image.
        //
        // ipd_scale 1 keeps the real eye separation (correct stereo parallax for a
        // screen in space, at the cost of the two images not coinciding).
        const float eye_setback = 1.6f;
        XrVector3f eye;
        if (ipd_scale <= 0.0f) {
            eye.x = panel_center[0] - panel_fwd[0] * eye_setback;
            eye.y = panel_center[1] - panel_fwd[1] * eye_setback;
            eye.z = panel_center[2] - panel_fwd[2] * eye_setback;
        } else if (ipd_scale >= 1.0f) {
            eye = real_eye;
        } else {
            const float cx = panel_center[0] - panel_fwd[0] * eye_setback;
            const float cy = panel_center[1] - panel_fwd[1] * eye_setback;
            const float cz = panel_center[2] - panel_fwd[2] * eye_setback;
            eye.x = cx + (real_eye.x - cx) * ipd_scale;
            eye.y = cy + (real_eye.y - cy) * ipd_scale;
            eye.z = cz + (real_eye.z - cz) * ipd_scale;
        }

        static int s_eye_logs = 0;
        if (s_eye_logs < 4) {
            ++s_eye_logs;
            VRLOG("converge eye%u: real (%.4f %.4f %.4f) -> used (%.4f %.4f %.4f), "
                  "panel (%.3f %.3f %.3f) setback %.2f, ipd_scale=%.2f",
                  eye_index, real_eye.x, real_eye.y, real_eye.z, eye.x, eye.y, eye.z,
                  panel_center[0], panel_center[1], panel_center[2], eye_setback, ipd_scale);
        }

'''
t = t[:s] + new + t[e:]

# The clamp that hid this must not stay silent any more.
t = t.replace("        const float panel_z = view_panel[2] > 0.05f ? view_panel[2] : 0.05f;",
              '''        // If this ever clamps, the panel is at or behind the eye and the frustum
        // is meaningless - say so rather than silently drawing nothing.
        if (view_panel[2] < 0.25f) {
            warn_once("panel too close to the eye; frustum clamped (screen will be wrong)");
        }
        const float panel_z = view_panel[2] > 0.25f ? view_panel[2] : 0.25f;''')

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("convergence point now sits in front of the screen plane")
