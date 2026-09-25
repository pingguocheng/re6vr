import io
import re

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# 1) drop the delayed-crash experiment
t = t.replace('''                            session_running = XR_SUCCEEDED(r);
                            VRLOG("openxr: session_running=%d, sleeping 5s to see if the crash is async",
                                  (int)session_running);
                            Sleep(5000);
                            VRLOG("openxr: survived the 5s sleep, leaving pump_events");
''', '''                            session_running = XR_SUCCEEDED(r);
''')

# 2) drop the per-submit tracing
t = re.sub(r'    static LONG s_submit_enter = 0;\n'
           r'    const LONG tick = InterlockedIncrement\(&s_submit_enter\);\n'
           r'    if \(tick <= 3\) VRLOG\("submit #%ld: enter \(state=%d, session_running=%d\)", \(long\)tick,\n'
           r'                         \(int\)state_, \(int\)impl_->session_running\);\n\n', '', t)
for pat in [
    r'    if \(tick <= 3\) VRLOG\("submit #%ld: GetBackBuffer \(vtbl=%p, slot18=%p\)", \(long\)tick,\n'
    r'                         \(void \*\)d3d9_device->lpVtbl, \(void \*\)d3d9_device->lpVtbl->GetBackBuffer\);\n',
    r'    if \(tick <= 3\) VRLOG\("submit #%ld: bb=%p bb_vtbl=%p GetDesc=%p Release=%p AddRef=%p", \(long\)tick,\n'
    r'                         \(void \*\)bb, \(void \*\)bb->lpVtbl, \(void \*\)bb->lpVtbl->GetDesc,\n'
    r'                         \(void \*\)bb->lpVtbl->Release, \(void \*\)bb->lpVtbl->AddRef\);\n',
    r'    if \(tick <= 3\) VRLOG\("submit #%ld: backbuffer %ux%u fmt=%lu", \(long\)tick, desc\.Width, desc\.Height,\n'
    r'                         \(unsigned long\)desc\.Format\);\n',
    r'    if \(tick <= 3\) VRLOG\("submit #%ld: backbuffer %ux%u fmt=%lu, ensuring shared surface",\n'
    r'                         \(long\)tick, desc\.Width, desc\.Height, \(unsigned long\)desc\.Format\);\n',
    r'    if \(tick <= 3\) VRLOG\("submit #%ld: ensure_shared_surface done", \(long\)tick\);\n',
    r'    if \(tick <= 3\) VRLOG\("submit #%ld: xrWaitFrame", \(long\)tick\);\n',
    r'    if \(tick <= 3\) VRLOG\("submit #%ld: xrBeginFrame", \(long\)tick\);\n',
    r'    if \(tick <= 3\) VRLOG\("submit #%ld: locate_views done, acquiring image", \(long\)tick\);\n',
    r'    if \(tick <= 3\) VRLOG\("submit #%ld: draw_quad \(image %u\)", \(long\)tick, image_index\);\n',
    r'    if \(tick <= 3\) VRLOG\("submit #%ld: xrEndFrame", \(long\)tick\);\n',
]:
    t = re.sub(pat, '', t)

# 3) device-lost awareness + guard the readback
t = t.replace('''void OpenXrBridge::on_end_scene(IDirect3DDevice9 *d3d9_device) {
    if (state_ != XrState::Ready || !impl_ || !impl_->session_running) return;''',
'''void OpenXrBridge::on_end_scene(IDirect3DDevice9 *d3d9_device) {
    if (state_ != XrState::Ready || !impl_ || !impl_->session_running) return;
    if (device_lost_) return;   // the game is mid-recovery; leave its objects alone''')

t = t.replace('''bool OpenXrBridge::submit(IDirect3DDevice9 *d3d9_device) {
    if (state_ != XrState::Ready || !impl_) return false;
''',
'''bool OpenXrBridge::submit(IDirect3DDevice9 *d3d9_device) {
    if (state_ != XrState::Ready || !impl_) return false;
    if (device_lost_) return false;
''')

# track D3DERR_DEVICELOST from the real Present
t = t.replace('''    ++frames_submitted_;''',
'''    ++frames_submitted_;''')

# 4) react to the device being lost: drop device-dependent objects, stop composing
t = t.replace('''void OpenXrBridge::on_device_lost() {
    if (!impl_) return;
    impl_->destroy_frame_srv();
    impl_->release_shared_surface();
    VRLOG("openxr: device lost - released shared surface");
}

void OpenXrBridge::on_device_reset() {
    VRLOG("openxr: device reset - shared surface will be rebuilt on demand");
}''',
'''void OpenXrBridge::on_device_lost() {
    if (!impl_) return;
    if (!device_lost_) {
        VRLOG("openxr: D3D9 device lost - releasing device objects and pausing composition");
    }
    device_lost_ = true;
    impl_->destroy_frame_srv();
    impl_->release_shared_surface();
}

void OpenXrBridge::on_device_reset() {
    if (!device_lost_) return;
    VRLOG("openxr: D3D9 device restored - composition resumes");
    device_lost_ = false;
}''')

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("cleaned up and added device-lost handling")
