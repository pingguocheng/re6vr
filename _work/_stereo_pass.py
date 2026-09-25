import io

p = r'C:\re6vr\src\cam_steer.cpp'
s = io.open(p, encoding='utf-8').read()

old = """__declspec(naked) void render_probe_detour() {
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
new = """volatile LONG g_in_render = 0;      // re-entrancy guard: the second pass must not re-enter this
volatile unsigned g_render_self = 0;

void __stdcall render_second_pass();

// `call` + `ret` rather than `jmp`, because a second pass has to run after the first one returns.
// That is only safe if this function cleans no arguments, and it was checked rather than assumed: the
// render phase's return sites are bare `ret` (0x00F3ED75 and the others in 0x00F3EB09..0x00F3EE4E),
// i.e. __thiscall with the caller doing `add esp, 0x10` - so a plain call/ret pair leaves the stack
// exactly as the engine left it. (Getting this wrong on 0x00E6FD20 cost two crashed runs; the check
// is cheap.)
__declspec(naked) void render_probe_detour() {
    __asm {
        mov g_render_here, ecx
        pushfd
        pushad
        call render_probe_record
        popad
        popfd
        call g_render_trampoline           // pass 1: whatever eye is selected now
        pushfd
        pushad
        call render_second_pass            // optionally pass 2, with the other eye
        popad
        popfd
        ret
    }
}

void __stdcall render_second_pass() {
    if (!g_stereo) return;
    // Re-entrancy: the engine may reach this function from inside itself on some paths.
    if (InterlockedIncrement(&g_in_render) != 1) {
        InterlockedDecrement(&g_in_render);
        return;
    }
    const unsigned srender = g_render_self;
    if (srender) {
        // Pass 2 renders the RIGHT eye: the camera hook reads g_current_eye and offsets the eye
        // position by half the IPD (see the per-eye IPD block in builder_record).
        const int saved = g_current_eye;
        g_current_eye = 1;
        typedef void(__thiscall *RenderPhaseFn)(void *self);
        RenderPhaseFn fn = (RenderPhaseFn)g_render_trampoline;
        fn((void *)(uintptr_t)srender);
        g_current_eye = saved;
        InterlockedIncrement(&g_stereo_passes);
    }
    InterlockedDecrement(&g_in_render);
}"""
n = s.count(old)
s = s.replace(old, new, 1)

# record the sRender pointer for the second pass, and report the stereo counters
old2 = """        VRLOG("steer: RENDER phase call %ld: sRender %08X, displayCount %u, Stereo flag %u, "
              "device %08X, primaryScene %08X", (long)n, srender, count, stereo, device, primary);"""
new2 = """        VRLOG("steer: RENDER phase call %ld: sRender %08X, displayCount %u, Stereo flag %u, "
              "device %08X, primaryScene %08X | stereo %s, second passes %ld, guard refusals %ld",
              (long)n, srender, count, stereo, device, primary,
              g_stereo ? "ON (re6vr_stereo.txt)" : "off",
              (long)InterlockedCompareExchange(&g_stereo_passes, 0, 0),
              (long)InterlockedCompareExchange(&g_stereo_skipped, 0, 0));"""
n2 = s.count(old2)
s = s.replace(old2, new2, 1)

# remember sRender for the second pass
old3 = """    const LONG n = InterlockedIncrement(&g_render_calls);
    unsigned srender = 0;
    read32(kSRenderGlobal, &srender);
    if (!srender) return;"""
new3 = """    const LONG n = InterlockedIncrement(&g_render_calls);
    unsigned srender = 0;
    read32(kSRenderGlobal, &srender);
    if (!srender) return;
    g_render_self = srender;      // the second pass needs it, and it is the same every frame"""
n3 = s.count(old3)
s = s.replace(old3, new3, 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('detour:', n, 'heartbeat:', n2, 'self:', n3)
