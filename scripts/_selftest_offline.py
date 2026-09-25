import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# The selftest must run with no headset at all, so it cannot depend on the
# session having brought the D3D11 device up.
old = "bool OpenXrBridge::run_selftest() {\n    if (!impl_) return false;\n    return impl_->compose_selftest();\n}"
new = '''bool OpenXrBridge::run_selftest() {
    if (!impl_) return false;

    // Offline check: it renders through the real pipeline into offscreen targets,
    // so it needs the D3D11 side but not a session. When no headset is connected
    // the session never comes up, and without this the test could not run at all.
    if (!impl_->d11_device && !impl_->create_d11_device_offline()) {
        VRLOG("selftest: no D3D11 device available, cannot run");
        return false;
    }

    // The D3D9 surfaces may still be queued (prepare() runs at CreateDevice,
    // before any frame); the test needs them to build the panel.
    try_create_pending();

    return impl_->compose_selftest();
}'''
assert old in t
t = t.replace(old, new, 1)

# offline device creation, mirroring the session path but without a LUID
anchor = "    bool create_d11_device() {"
offline = '''    // Creates a D3D11 device without consulting the runtime. Used by the offline
    // selftest when no headset is attached.
    bool create_d11_device_offline() {
        if (d11_device) return true;
        UINT flags = 0;
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
        IDXGIAdapter1 *chosen = nullptr;
        IDXGIFactory1 *factory = nullptr;

        // Prefer a hardware adapter from the runtime's LUID if there is one, else
        // just the first non-software adapter.
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) && factory) {
            LUID wanted = {};
            bool have_luid = false;
            PFN_xrGetD3D11GraphicsRequirementsKHR get_reqs = nullptr;
            if (instance && system_id != XR_NULL_SYSTEM_ID &&
                resolve(&get_reqs, "xrGetD3D11GraphicsRequirementsKHR") && get_reqs) {
                XrGraphicsRequirementsD3D11KHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
                if (XR_SUCCEEDED(get_reqs(instance, system_id, &reqs))) {
                    wanted = reqs.adapterLuid;
                    have_luid = true;
                }
            }
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1 *a = nullptr;
                if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
                if (!a) break;
                DXGI_ADAPTER_DESC1 d = {};
                a->GetDesc1(&d);
                const bool match = have_luid && d.AdapterLuid.HighPart == wanted.HighPart &&
                                   d.AdapterLuid.LowPart == wanted.LowPart;
                const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                if (match || (!chosen && !software)) {
                    chosen = a;
                    if (match) break;
                } else {
                    a->Release();
                }
            }
        }

        HRESULT hr = D3D11CreateDevice(chosen,
                                       chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                       &d11_device, &got, &d11_context);
        if (factory) factory->Release();
        if (chosen) chosen->Release();
        if (FAILED(hr) || !d11_device) {
            VRLOG("selftest: D3D11CreateDevice failed: 0x%08lX", (unsigned long)hr);
            return false;
        }
        VRLOG("selftest: D3D11 device up on its own (feature level 0x%04X)", (unsigned)got);
        return true;
    }

    bool create_d11_device() {'''
assert anchor in t
t = t.replace(anchor, offline, 1)

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("selftest can now run without a headset")
