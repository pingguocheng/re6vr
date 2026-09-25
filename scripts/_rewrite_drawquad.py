import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    lines = f.readlines()

# Replace the whole eye-placement/convergence region inside draw_quad, from the
# `real_eye` declaration up to (not including) the world-matrix comment.
start = next(i for i, l in enumerate(lines)
             if "const XrVector3f real_eye = eye_pose.position;" in l)
end = next(i for i, l in enumerate(lines)
           if "// Screen -> world. Translation in the last row" in l)

block = '''        const XrVector3f real_eye = eye_pose.position;

        // 1. Place the screen if it is not placed yet, or re-anchor it after the
        //    player has turned well away from it.
        //
        //    This call has twice been lost to a careless scripted edit, and both
        //    times the symptom was the same: panel_center stays at its initial
        //    (0,0,0), the projection degenerates and the headset shows nothing.
        //    If the screen ever goes black again, check here first.
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

        // 2. One convergence point for both eyes, `eye_setback` metres in front of
        //    the screen plane.
        //
        //    Both eyes sharing a viewpoint is what removes the double image: a flat
        //    screen carries no stereo depth, only an offset. The setback matters -
        //    converging exactly onto the panel makes its view-space depth zero and
        //    the frustum then cannot describe it, which is a black screen.
        const float eye_setback = 1.6f;
        XrVector3f eye;
        {
            const float cx = panel_center[0] - panel_fwd[0] * eye_setback;
            const float cy = panel_center[1] - panel_fwd[1] * eye_setback;
            const float cz = panel_center[2] - panel_fwd[2] * eye_setback;
            if (ipd_scale <= 0.0f) {
                eye.x = cx; eye.y = cy; eye.z = cz;
            } else if (ipd_scale >= 1.0f) {
                eye = real_eye;
            } else {
                eye.x = cx + (real_eye.x - cx) * ipd_scale;
                eye.y = cy + (real_eye.y - cy) * ipd_scale;
                eye.z = cz + (real_eye.z - cz) * ipd_scale;
            }
        }

        static int s_eye_logs = 0;
        if (s_eye_logs < 4) {
            ++s_eye_logs;
            VRLOG("converge eye%u: player (%.2f %.2f %.2f) -> eye (%.2f %.2f %.2f), "
                  "screen (%.2f %.2f %.2f), ipd_scale=%.2f",
                  eye_index, real_eye.x, real_eye.y, real_eye.z, eye.x, eye.y, eye.z,
                  panel_center[0], panel_center[1], panel_center[2], ipd_scale);
        }

'''

lines[start:end] = [block]
with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.writelines(lines)
print("rewrote draw_quad's placement + convergence region (lines %d-%d)" % (start + 1, end))
