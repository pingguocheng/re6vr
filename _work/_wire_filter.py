import io

p = r'C:\re6vr\src\cam_steer.cpp'
s = io.open(p, encoding='utf-8').read()

# 1. simplify the table refresh in builder_record to use the shared helper
old = """    if (!g_slot_filled || now_id - s_slot_checked_ms > 2000) {
        s_slot_checked_ms = now_id;
        for (int i = 0; i < 8; ++i) g_slot_cams[i] = 0;
        unsigned bio = 0;
        if (read32(kGlobalBioCamera, &bio) && plausible_pointer(bio)) {
            for (unsigned k = 0; k < 8; ++k) {
                const unsigned rec = bio + 0x30u + 0x190u * k;
                unsigned cam_ptr = 0;
                if (read32(rec, &cam_ptr) && cam_ptr) {
                    read32(rec + 4, &g_slot_cams[k]);
                    if (g_slot_cams[k]) g_slot_filled = true;
                }
            }
        }
    }"""
new = """    if (!g_slot_filled || now_id - s_slot_checked_ms > 2000) {
        s_slot_checked_ms = now_id;
        refresh_slot_cams();
    }"""
print('refresh block found:', s.count(old))
s = s.replace(old, new)

# 2. replace the inline identity filter in the rotation path with the helper call
start_marker = "        const bool have_table = (g_slot_cams[0]"
i = s.find(start_marker)
if i < 0:
    print('FILTER BLOCK NOT FOUND')
else:
    # walk back to the opening brace line
    j = s.rfind('    {', 0, i)
    # find the end: the matching close of that block, i.e. the next "\n    }\n"
    k = s.find('\n    }\n', i)
    if j < 0 or k < 0:
        print('could not delimit the filter block')
    else:
        block = s[j:k + len('\n    }\n')]
        print('filter block %d bytes' % len(block))
        s = s[:j] + "    if (!camera_may_be_steered(g_cam_last)) return;\n" + s[k + len('\n    }\n'):]

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('written')
