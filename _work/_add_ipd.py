import io

p = r'C:\re6vr\src\cam_steer.cpp'
s = io.open(p, encoding='utf-8').read()

anchor = "    if (!camera_may_be_steered(g_cam_last)) return;\n"
block = anchor + """
    // ---------------------------------------------------------------- per-eye IPD offset (stereo)
    //
    // Stereo needs each eye rendered from a slightly different position, and the offline report
    // settled that the engine has NO eye-offset field anywhere: 0x005F80B0 passes [ecx+0x50] verbatim
    // into 0x00E6FD20, which is a pure look-at over its three pointer arguments (163 instructions, no
    // constant offset). So the mod supplies the whole eye translation, applied per view-matrix build -
    // which is here.
    //
    // The offset is along the camera's own right axis, right = normalize(gaze x up), so it follows the
    // view wherever the head points. BOTH the eye and the target move by the same vector: moving only
    // the eye would ROTATE the gaze instead of translating it.
    //
    // re6vr_ipd.txt in metres, default 0 (off). The usual range is 0.055-0.070 m; a wrong sign swaps
    // the eyes (the world looks inside-out), which is a one-value fix.
    if (g_ipd != 0.0f && g_current_eye >= 0) {
        const unsigned up_ptr = *(const unsigned *)(uintptr_t)(frame + 12);
        float u[3] = {0.0f, 1.0f, 0.0f};
        if (up_ptr) read_vec3(up_ptr, 0, u);
        const float ulen = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        float gx = t[0] - e[0], gy = t[1] - e[1], gz = t[2] - e[2];
        const float glen = sqrtf(gx * gx + gy * gy + gz * gz);
        if (ulen > 0.5f && glen > 0.5f) {
            u[0] /= ulen;
            u[1] /= ulen;
            u[2] /= ulen;
            gx /= glen;
            gy /= glen;
            gz /= glen;
            // right = gaze x up - the same right axis the look-at builder derives internally, so the
            // offset lands along the camera's own screen X.
            float rx = gy * u[2] - gz * u[1];
            float ry = gz * u[0] - gx * u[2];
            float rz = gx * u[1] - gy * u[0];
            const float rlen = sqrtf(rx * rx + ry * ry + rz * rz);
            if (rlen > 1e-3f) {
                const float sign = (g_current_eye == 0) ? -0.5f : 0.5f;   // eye 0 = left
                const float d = sign * g_ipd / rlen;
                if (writable((const void *)(uintptr_t)eye, 3 * sizeof(float)) &&
                    writable((const void *)(uintptr_t)target, 3 * sizeof(float))) {
                    const float ne[3] = {e[0] + rx * d, e[1] + ry * d, e[2] + rz * d};
                    const float nt2[3] = {t[0] + rx * d, t[1] + ry * d, t[2] + rz * d};
                    memcpy((void *)(uintptr_t)eye, ne, sizeof(ne));
                    memcpy((void *)(uintptr_t)target, nt2, sizeof(nt2));
                    e[0] = ne[0]; e[1] = ne[1]; e[2] = ne[2];
                    t[0] = nt2[0]; t[1] = nt2[1]; t[2] = nt2[2];
                    static unsigned long long last_ipd_log = 0;
                    const unsigned long long now_ipd = GetTickCount64();
                    if (now_ipd - last_ipd_log > 2000) {
                        last_ipd_log = now_ipd;
                        VRLOG("steer: IPD %.3f m applied to eye %d: eye moved (%.2f %.2f %.2f) -> "
                              "(%.2f %.2f %.2f), right axis (%.3f %.3f %.3f)", (double)g_ipd,
                              g_current_eye, e[0] - rx * d, e[1] - ry * d, e[2] - rz * d, e[0], e[1],
                              e[2], (double)(rx / rlen), (double)(ry / rlen), (double)(rz / rlen));
                    }
                }
            }
        }
    }
"""

print('anchor count:', s.count(anchor))
s = s.replace(anchor, block, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('written')
