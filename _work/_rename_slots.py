import io

p = r'C:\re6vr\src\cam_steer.cpp'
s = io.open(p, encoding='utf-8').read()
pairs = [
    ('if (!s_slot_filled || now_id - s_slot_checked_ms > 2000) {',
     'if (!g_slot_filled || now_id - s_slot_checked_ms > 2000) {'),
    ('for (int i = 0; i < 8; ++i) s_slot_cams[i] = 0;',
     'for (int i = 0; i < 8; ++i) g_slot_cams[i] = 0;'),
    ('read32(rec + 4, &s_slot_cams[k]);', 'read32(rec + 4, &g_slot_cams[k]);'),
    ('if (s_slot_cams[k]) s_slot_filled = true;', 'if (g_slot_cams[k]) g_slot_filled = true;'),
    ('if (!s_reported && s_slot_cams[0]) {', 'if (!s_reported && g_slot_cams[0]) {'),
    ('s_slot_cams[0]);', 'g_slot_cams[0]);'),
    ('s_slot_cams[4] || s_slot_cams[5] || s_slot_cams[6] || s_slot_cams[7]);',
     'g_slot_cams[4] || g_slot_cams[5] || g_slot_cams[6] || g_slot_cams[7]);'),
    ('s_slot_cams[0] || s_slot_cams[1] || s_slot_cams[2] || s_slot_cams[3] ||',
     'g_slot_cams[0] || g_slot_cams[1] || g_slot_cams[2] || g_slot_cams[3] ||'),
    ('if (s_slot_cams[i] && s_slot_cams[i] == g_cam_last) {',
     'if (g_slot_cams[i] && g_slot_cams[i] == g_cam_last) {'),
]
for a, b in pairs:
    n = s.count(a)
    if n == 0:
        print('MISS: %s' % a[:60])
    s = s.replace(a, b)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('done')
