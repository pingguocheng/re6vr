import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# ---- 1. place_panel: anchor to the reference space origin -------------------
s = t.index("    void place_panel(const XrVector3f &eye, const float fwd[3]) {")
e = t.index("    // Called when the session regains focus")

new = '''    void place_panel(const XrVector3f &eye, const float fwd[3]) {
        (void)eye;
        (void)fwd;
        read_screen_env();

        // Anchored to the reference space origin, not to a sampled eye pose.
        //
        // The LOCAL space is the runtime's own idea of where the player is and
        // which way they face (set at session start, and by the runtime's own
        // recentre). -z in that space is straight ahead, so a screen at
        // (0, 0, -dist) is reliably in front of the player. Deriving the position
        // from an eye pose captured on the first frame instead put the screen
        // wherever the player happened to be looking at that instant - which is
        // usually off to one side while a fullscreen game is still starting up.
        panel_fwd[0] = 0.0f; panel_fwd[1] = 0.0f; panel_fwd[2] = -1.0f;
        panel_right[0] = 1.0f; panel_right[1] = 0.0f; panel_right[2] = 0.0f;
        panel_up[0] = 0.0f; panel_up[1] = 1.0f; panel_up[2] = 0.0f;
        panel_center[0] = 0.0f;
        panel_center[1] = 0.0f;
        panel_center[2] = -screen_dist;

        panel_placed = true;
        VRLOG("compositor: screen %.1f m ahead of the reference origin, %.2f x %.2f m, "
              "ipd_scale=%.2f, follow=%d",
              screen_dist, panel_w, panel_h, ipd_scale, (int)yaw_follow);
    }

    void read_screen_env() {
        wchar_t buf[32] = L"";
        if (GetEnvironmentVariableW(L"RE6VR_SCREEN_DIST", buf, 32) > 0) {
            const float v = (float)_wtof(buf);
            if (v > 0.3f && v < 20.0f) screen_dist = v;
        }
        if (GetEnvironmentVariableW(L"RE6VR_SCREEN_WIDTH", buf, 32) > 0) {
            const float v = (float)_wtof(buf);
            if (v > 0.3f && v < 30.0f) {
                panel_w = v;
                panel_h = v * 9.0f / 16.0f;
            }
        }
        if (GetEnvironmentVariableW(L"RE6VR_IPD_SCALE", buf, 32) > 0) {
            const float v = (float)_wtof(buf);
            if (v >= 0.0f && v <= 2.0f) ipd_scale = v;
        }
        if (GetEnvironmentVariableW(L"RE6VR_SCREEN_FOLLOW", buf, 32) > 0) {
            yaw_follow = _wtoi(buf) != 0;
        }
    }

    // Puts the screen in front of `eye` along `fwd` (yaw only). Used only by the
    // follow mode, after the head has turned well away from the screen.
    void place_panel_at(const XrVector3f &eye, const float fwd[3]) {
        float fx = fwd[0], fz = fwd[2];
        const float len = sqrtf(fx * fx + fz * fz);
        if (len < 1e-4f) {
            fx = 0.0f;
            fz = -1.0f;
        } else {
            fx /= len;
            fz /= len;
        }
        panel_fwd[0] = fx;    panel_fwd[1] = 0.0f; panel_fwd[2] = fz;
        panel_right[0] = -fz; panel_right[1] = 0.0f; panel_right[2] = fx;
        panel_up[0] = 0.0f;   panel_up[1] = 1.0f;  panel_up[2] = 0.0f;
        panel_center[0] = eye.x + fx * screen_dist;
        panel_center[1] = eye.y;
        panel_center[2] = eye.z + fz * screen_dist;
        VRLOG("compositor: screen re-anchored in front of the player");
    }

'''
t = t[:s] + new + t[e:]

# ---- 2. follow mode uses place_panel_at and a wider threshold ---------------
old_follow_s = t.index("        } else if (yaw_follow) {")
old_follow_e = t.index("        if (ipd_scale != 1.0f) {", old_follow_s)
t = t[:old_follow_s] + '''        } else if (yaw_follow) {
            // Keep a game screen reachable: re-anchor only once the head has
            // turned well away from it (~45 degrees), then leave it alone.
            const float len2 = fwd[0] * fwd[0] + fwd[2] * fwd[2];
            if (len2 > 1e-4f) {
                const float fl = sqrtf(len2);
                const float dot = (panel_fwd[0] * fwd[0] + panel_fwd[2] * fwd[2]) / fl;
                if (dot < 0.7f) place_panel_at(eye, fwd);
            }
        }
''' + t[old_follow_e:]

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("panel anchoring rewritten")
