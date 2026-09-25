import io

p = r'C:\re6vr\src\cam_steer.cpp'
s = io.open(p, encoding='utf-8').read()

old = """    if (g_eye_force >= 0.0f) g_current_eye = (int)(g_eye_force + 0.5f);"""
new = """    if (g_eye_force >= 0.0f) g_current_eye = (int)(g_eye_force + 0.5f);
    // Stereo dual pass: the render phase runs a second time with the other eye selected. Default OFF,
    // because it makes the engine render the whole scene twice into the same target (the two eyes
    // overwrite each other until the per-eye targets exist), and that is a deliberate experiment, not
    // a shipping mode. re6vr_stereo.txt = 1.
    g_stereo = read_float_marker(L"re6vr_stereo.txt", 0.0f, 0.0f, 1.0f) >= 0.5f;
    if (g_stereo) {
        VRLOG("steer: STEREO DUAL PASS armed: the render phase will run a second time with the right "
              "eye selected (ipd %.4f m). Both passes currently draw into the same render target, so "
              "expect the frame to show one of the two - what this proves is that the engine can be "
              "made to render twice per frame with different eye poses.", (double)g_ipd);
    }"""
n = s.count(old)
s = s.replace(old, new, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('stereo marker:', n)
