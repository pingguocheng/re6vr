import io

p = r'C:\re6vr\src\cam_steer.cpp'
s = io.open(p, encoding='utf-8').read()

old = """        if (g_method == kMethodBuilder && g_fov_override > 0.0f && !g_hooked) {
            g_hooked = install_hook();
            VRLOG("steer: GetViewMatrix detour installed for the FOV override (rotation still comes "
                  "from the MakeViewMatrix hook, so the view is turned only once)");
        }"""
new = """        // ALWAYS installed in builder mode, not only when a fov override is armed. Two things need
        // it: the fov injection (the fov lives on the camera object, which only this hook sees), and
        // the IDENTITY FILTER, which needs g_cam_last - an earlier version of this block required a
        // fov override, which left the filter with an empty table and therefore disabled it silently.
        // Its rotation path stays off in builder mode (head_steer_one runs only for the
        // camera-object method), so the two hooks cannot both turn the view.
        if (g_method == kMethodBuilder && !g_hooked) {
            g_hooked = install_hook();
            VRLOG("steer: GetViewMatrix detour installed alongside the MakeViewMatrix hook (camera "
                  "identity for the slot filter + fov injection); rotation still comes from "
                  "MakeViewMatrix only");
        }"""

print('install block found:', s.count(old))
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('written')
