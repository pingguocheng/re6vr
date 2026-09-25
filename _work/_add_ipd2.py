import io

p = r'C:\re6vr\src\cam_steer.cpp'
s = io.open(p, encoding='utf-8').read()

# 1. globals next to the other builder-hook state
old_g = """unsigned g_slot_cams[8] = {0};
bool g_slot_filled = false;"""
new_g = """unsigned g_slot_cams[8] = {0};
bool g_slot_filled = false;
// Stereo: which eye the current view-matrix build is for (-1 = not stereo / do not offset), and the
// interpupillary distance in metres (0 = off). Both are set from markers; g_current_eye is driven by
// the render-phase pass once the dual-pass path exists, and 0/1 can be forced for a single-eye test.
int g_current_eye = -1;
float g_ipd = 0.0f;
float g_eye_force = -1.0f;"""
n1 = s.count(old_g)
s = s.replace(old_g, new_g, 1)

# 2. marker parsing in load_head_knobs
old_m = """    g_fov_override = read_float_marker(L"re6vr_fov.txt", 0.0f, 0.0f, 179.0f);"""
new_m = """    g_fov_override = read_float_marker(L"re6vr_fov.txt", 0.0f, 0.0f, 179.0f);
    // Stereo groundwork: the eye separation, and an optional fixed eye index for a single-eye test
    // (0 = left, 1 = right, 2/-1 = off). With g_ipd = 0 both are inert.
    g_ipd = read_float_marker(L"re6vr_ipd.txt", 0.0f, 0.0f, 0.5f);
    g_eye_force = read_float_marker(L"re6vr_eye_index.txt", -1.0f, -1.0f, 1.0f);
    if (g_eye_force >= 0.0f) g_current_eye = (int)(g_eye_force + 0.5f);
    if (g_ipd != 0.0f || g_eye_force >= 0.0f) {
        VRLOG("steer: stereo groundwork armed: ipd %.4f m, forced eye index %d (0=left 1=right), "
              "active eye %d", (double)g_ipd, (int)g_eye_force, g_current_eye);
    }"""
n2 = s.count(old_m)
s = s.replace(old_m, new_m, 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('globals:', n1, 'markers:', n2)
