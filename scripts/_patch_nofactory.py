import io
import re

path = r"C:\re6vr\src\d3d9_proxy.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# ---------------------------------------------------------------- new exports
old_create9 = t[t.index('extern "C" __declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9'):]
old_create9 = old_create9[:old_create9.index('\n}\n') + 3]

new_create9 = r'''extern "C" __declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk_version) {
    if (!g_real_create9) {
        VRLOG("Direct3DCreate9: real entry point missing");
        return nullptr;
    }
    IDirect3D9 *real = g_real_create9(sdk_version);
    VRLOG("Direct3DCreate9(%u) -> 0x%p (vtbl 0x%p, CreateDevice slot %p)", sdk_version,
          (void *)real, real ? (void *)real->lpVtbl : nullptr,
          real ? (void *)real->lpVtbl->CreateDevice : nullptr);
    if (!real) return nullptr;

    // Return the *real* object and patch its own vtable copy in place.
    //
    // Wrapping the factory was tried twice and both attempts let the game jump
    // into uninitialised memory (fault offset 0xCCCCCCE0) on its first
    // CreateDevice call, even though the wrapper's bytes and vtable were
    // verified correct from the harness. D3D9 hands every object a private
    // vtable copy, so patching the object we actually return avoids the whole
    // class of problems: the game sees a genuine IDirect3D9 with a genuine
    // vtable, and only slot 16 is ours.
    g_real_d3d9 = real;
    if (!hook_create_device(real)) {
        VRLOG("Direct3DCreate9: could not patch CreateDevice, passing the object through unhooked");
    }
    return real;
}
'''
t = t.replace(old_create9, new_create9)

# ---------------------------------------------------------------- new hook
hook_block = r'''// ------------------------------------------- IDirect3D9::CreateDevice detour
typedef HRESULT(STDMETHODCALLTYPE *CreateDeviceFn)(IDirect3D9 *, UINT, DWORD, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
CreateDeviceFn g_original_create_device = nullptr;

HRESULT STDMETHODCALLTYPE hooked_CreateDevice(IDirect3D9 *self, UINT adapter, DWORD dev_type, HWND hwnd,
                                              DWORD flags, D3DPRESENT_PARAMETERS *pp,
                                              IDirect3DDevice9 **out) {
    VRLOG("CreateDevice #%ld: adapter=%u type=%lu hwnd=0x%p flags=0x%08lX windowed=%d %ux%u fmt=%lu msaa=%lu/%lu",
          (long)InterlockedIncrement(&g_create_device_calls), adapter, (unsigned long)dev_type, hwnd,
          (unsigned long)flags, pp ? (int)pp->Windowed : -1,
          pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
          pp ? (unsigned long)pp->BackBufferFormat : 0,
          pp ? (unsigned long)pp->MultiSampleType : 0, pp ? (unsigned long)pp->MultiSampleQuality : 0);

    if (!g_original_create_device) {
        VRLOG("CreateDevice: FATAL no original entry point");
        if (out) *out = nullptr;
        return E_FAIL;
    }

    D3DPRESENT_PARAMETERS local = {};
    if (pp) local = *pp;

    HRESULT hr = g_original_create_device(self, adapter, dev_type, hwnd, flags, &local, out);
    VRLOG("CreateDevice: original returned 0x%08lX, device 0x%p", (unsigned long)hr,
          out ? (void *)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        if (local.BackBufferWidth && local.BackBufferHeight) {
            re6vr::set_backbuffer_geometry(local.BackBufferWidth, local.BackBufferHeight,
                                           local.BackBufferFormat ? local.BackBufferFormat
                                                                  : D3DFMT_X8R8G8B8);
            VRLOG("CreateDevice: back buffer %ux%u fmt=%lu recorded for the compositor",
                  local.BackBufferWidth, local.BackBufferHeight, (unsigned long)local.BackBufferFormat);
        }
        hook_present(*out);
        if (pp) *pp = local;  // the runtime may have corrected the parameters
    }
    return hr;
}

// Patches slot 16 of the (private) vtable belonging to `factory`.
bool hook_create_device(IDirect3D9 *factory) {
    if (g_create_device_hooked) return true;
    if (!factory || !factory->lpVtbl) return false;

    g_original_create_device = factory->lpVtbl->CreateDevice;
    if (!g_original_create_device) {
        VRLOG("CreateDevice: factory 0x%p has a null CreateDevice slot", (void *)factory);
        return false;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(&factory->lpVtbl->CreateDevice, sizeof(void *), PAGE_READWRITE, &old_protect)) {
        VRLOG("CreateDevice: VirtualProtect failed: %lu", GetLastError());
        return false;
    }
    factory->lpVtbl->CreateDevice = &hooked_CreateDevice;
    VirtualProtect(&factory->lpVtbl->CreateDevice, sizeof(void *), old_protect, &old_protect);

    g_create_device_hooked = true;
    VRLOG("CreateDevice: vtable patched at %p (original %p, detour %p)",
          (void *)&factory->lpVtbl->CreateDevice, (void *)g_original_create_device,
          (void *)&hooked_CreateDevice);
    return true;
}

'''

# insert the hook right before the EndScene detour block
anchor = "// ------------------------------------------------- EndScene detour"
t = t.replace(anchor, hook_block + anchor)

# globals
t = t.replace("""bool                  g_present_hooked = false;""",
              """bool                  g_present_hooked = false;
bool                  g_create_device_hooked = false;""")

# forward declaration for the helper used by Direct3DCreate9
t = t.replace("""RealExports g_fwd;""",
              """RealExports g_fwd;

bool hook_create_device(IDirect3D9 *factory);""")

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("Direct3DCreate9 now returns the real object with a patched vtable")
