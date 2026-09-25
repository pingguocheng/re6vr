import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# ---- 1. eye: converge to a point in FRONT of the panel ----------------------
s = t.index("        // Collapse the eye towards the panel centre by (1 - ipd_scale). At 0 both")
e = t.index("        // Screen -> world. Translation in the last row, basis vectors in the rows:")
t = t[:s] + '''        // The convergence point both eyes share, placed *in front of* the panel.
        //
        // Eye separation going to zero is what removes the double image, but the
        // converging eye still has to sit at a sane distance with the panel ahead
        // of it. An earlier attempt collapsed the eye onto the panel centre
        // itself, which put the "eye" on the panel plane facing away from it: the
        // whole quad then landed behind the near plane and nothing was drawn at
        // all (the exported eye images were pure black).
        const XrVector3f real_eye = eye_pose.position;
        XrVector3f eye = real_eye;
        {
            const float to_panel[3] = {panel_center[0] - real_eye.x,
                                       panel_center[1] - real_eye.y,
                                       panel_center[2] - real_eye.z};
            const float d = sqrtf(to_panel[0] * to_panel[0] + to_panel[1] * to_panel[1] +
                                  to_panel[2] * to_panel[2]);
            if (d > 1e-3f) {
                const float back = d * (1.0f - ipd_scale);
                eye.x = panel_center[0] - to_panel[0] / d * back;
                eye.y = panel_center[1] - to_panel[1] / d * back;
                eye.z = panel_center[2] - to_panel[2] / d * back;
            }
        }

''' + t[e:]

# ---- 2. projection derived from the panel rectangle ------------------------
s2 = t.index("        const float tan_l = tanf(eye_fov.angleLeft);")
e2 = t.index("        struct {", s2)
t = t[:s2] + '''        // Frustum derived from the panel rectangle and the convergence point,
        // not from the runtime's per-eye FOV. Both eyes now share one convergence
        // point, so the panel's angular size is identical for both and the two
        // images land on exactly the same pixels.
        float view_panel[3];
        view_panel[0] = panel_center[0] * view[0] + panel_center[1] * view[4] +
                        panel_center[2] * view[8] + view[12];
        view_panel[1] = panel_center[0] * view[1] + panel_center[1] * view[5] +
                        panel_center[2] * view[9] + view[13];
        view_panel[2] = panel_center[0] * view[2] + panel_center[1] * view[6] +
                        panel_center[2] * view[10] + view[14];

        const float panel_z = view_panel[2] > 0.05f ? view_panel[2] : 0.05f;
        const float tan_l = (view_panel[0] - panel_w * 0.5f) / panel_z;
        const float tan_r = (view_panel[0] + panel_w * 0.5f) / panel_z;
        const float tan_d = (view_panel[1] - panel_h * 0.5f) / panel_z;
        const float tan_u = (view_panel[1] + panel_h * 0.5f) / panel_z;

        const float near_z = 0.05f, far_z = 50.0f;
        float proj[16] = {0};
        proj[0] = 2.0f / (tan_r - tan_l);
        proj[5] = 2.0f / (tan_u - tan_d);
        proj[8] = -(tan_r + tan_l) / (tan_r - tan_l);
        proj[9] = -(tan_u + tan_d) / (tan_u - tan_d);
        proj[10] = far_z / (far_z - near_z);
        proj[11] = 1.0f;
        proj[14] = -(far_z * near_z) / (far_z - near_z);

''' + t[e2:]

# ---- 3. panel anchored in front of the player's LOCAL position --------------
s3 = t.index("        // Anchored to the reference space origin, not to a sampled eye pose.")
e3 = t.index("        panel_placed = true;", s3)
t = t[:s3] + '''        // Anchored in front of the player's own position in the LOCAL reference
        // space, which is what the runtime keeps at the player and recentres.
        //
        // Taking the first frame's eye pose put the screen wherever the player
        // happened to be looking while a fullscreen game was starting. Anchoring
        // to the raw reference origin was worse: the origin is not the player, so
        // the screen could end up anywhere at all.
        panel_fwd[0] = 0.0f; panel_fwd[1] = 0.0f; panel_fwd[2] = -1.0f;
        panel_right[0] = 1.0f; panel_right[1] = 0.0f; panel_right[2] = 0.0f;
        panel_up[0] = 0.0f; panel_up[1] = 1.0f; panel_up[2] = 0.0f;
        panel_center[0] = eye.x;
        panel_center[1] = eye.y;
        panel_center[2] = eye.z - screen_dist;

''' + t[e3:]

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("convergence point in front of the panel; frustum from the panel rect")
