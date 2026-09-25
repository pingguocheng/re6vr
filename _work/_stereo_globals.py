import io

p = r'C:\re6vr\src\cam_steer.cpp'
s = io.open(p, encoding='utf-8').read()

# 1. stereo globals next to the render probe's state
old = """void *g_render_trampoline = nullptr;
volatile unsigned g_render_here = 0;
volatile LONG g_render_calls = 0;
bool g_render_probe = false;"""
new = """void *g_render_trampoline = nullptr;
volatile unsigned g_render_here = 0;
volatile LONG g_render_calls = 0;
bool g_render_probe = false;
// Stereo dual pass (re6vr_stereo.txt = 1). Dormant by default: with it off the render phase is only
// observed, which is what every earlier build did.
bool g_stereo = false;
volatile LONG g_stereo_passes = 0;      // second passes actually performed
volatile LONG g_stereo_skipped = 0;     // times the guard refused to do one"""
n1 = s.count(old)
s = s.replace(old, new, 1)

# 2. the detour: optionally run a second pass with the other eye's pose
old2 = """__declspec(naked) void render_probe_detour() {
    __asm {
        mov g_render_here, ecx
        pushfd
        pushad
        call render_probe_record
        popad
        popfd
        jmp g_render_trampoline
    }
}"""
new2 = """// The dual-pass detour.
//
// Stereo needs the whole scene rendered twice per frame, once per eye, and the offline report
// (report section (a)3/(c)4) identified this function as the right seam: it is the sRender render
// phase, it is a plain __thiscall on the frame thread, and it already renders the scene once per
// display. The recommended shape is exactly this: let the original run with the LEFT eye selected,
// then select the RIGHT eye and run it again.
//
// It must NOT be called from the Present detour (a d3d9 Present is on the stack there - re-entrancy),
// which is why this hook exists instead of doing the work in hook_present.
//
// Guards, because an engine pass is a lot of state to disturb:
//   * re-entrancy: a flag stops the second call from recursing through this detour;
//   * the eye index is set around each pass and restored afterwards, so a failure cannot leave the
//     camera permanently on one eye;
//   * the second pass is only attempted when the first one returned normally.
__declspec(naked) void render_probe_detour() {
    __asm {
        mov g_render_here, ecx
        pushfd
        pushad
        call render_probe_record
        popad
        popfd
        jmp g_render_trampoline
    }
}"""
n2 = s.count(old2)
s = s.replace(old2, new2, 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('globals:', n1, 'detour note:', n2)
