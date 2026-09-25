import io

p = r'C:\re6vr\src\cam_steer.cpp'
s = io.open(p, encoding='utf-8').read()

# g_stereo / g_stereo_passes / g_stereo_skipped must exist before load_head_knobs reads them.
old_block = """bool g_stereo = false;
volatile LONG g_stereo_passes = 0;      // second passes actually performed
volatile LONG g_stereo_skipped = 0;     // times the guard refused to do one
"""
print('later block found:', s.count(old_block))
s = s.replace(old_block, "", 1)

anchor = """int g_current_eye = -1;
float g_ipd = 0.0f;
float g_eye_force = -1.0f;"""
new_anchor = anchor + """
// Stereo dual pass (re6vr_stereo.txt = 1). Dormant by default: with it off the render phase is only
// observed, which is what every earlier build did.
bool g_stereo = false;
volatile LONG g_stereo_passes = 0;      // second passes actually performed
volatile LONG g_stereo_skipped = 0;     // times the guard refused to do one"""
print('anchor found:', s.count(anchor))
s = s.replace(anchor, new_anchor, 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('written')
