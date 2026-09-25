import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

new_fn = r'''    // Allocates every D3D9 surface the compositor needs. Called once per device,
    // never from inside a frame: MT Framework stops with
    // "ERR09: Unsupported function." when a texture is created mid-frame, which
    // is why nothing here may be lazy.
    bool create_device_surfaces(IDirect3DDevice9 *dev, uint32_t w, uint32_t h, D3DFORMAT fmt) {
        release_shared_surface();

        DXGI_FORMAT dxgi_fmt = DXGI_FORMAT_UNKNOWN;
        if (!d3d9_format_to_dxgi(fmt, &dxgi_fmt)) {
            VRLOG("openxr: unsupported back buffer format %lu", (unsigned long)fmt);
            return false;
        }
        if (dxgi_fmt != DXGI_FORMAT_B8G8R8A8_UNORM && dxgi_fmt != DXGI_FORMAT_B8G8R8X8_UNORM) {
            VRLOG("openxr: back buffer format %s needs conversion, unsupported",
                  dxgi_format_name(dxgi_fmt));
            return false;
        }

        // Work at 1280x720: a 4K readback is slow and is the size that makes the
        // driver raise.
        const uint32_t work_w = w > 1280 ? 1280 : w;
        const uint32_t work_h = w > 1280 ? (uint32_t)((uint64_t)h * 1280 / w) : h;

        // A8R8G8B8 cannot be read back into an identical A8R8G8B8 surface, so the
        // intermediate and the readback surface are X8R8G8B8 and the StretchRect
        // doubles as the format conversion.
        const D3DFORMAT blit_fmt = (fmt == D3DFMT_A8R8G8B8) ? D3DFMT_X8R8G8B8 : fmt;

        d3d9_owner = dev;
        HRESULT hr = dev->lpVtbl->CreateTexture(dev, work_w, work_h, 1, D3DUSAGE_RENDERTARGET, blit_fmt,
                                                D3DPOOL_DEFAULT,
                                                reinterpret_cast<void **>(&blit_tex), nullptr);
        if (FAILED(hr) || !blit_tex) {
            VRLOG("openxr: CreateTexture(%ux%u fmt %lu) failed: 0x%08lX", work_w, work_h,
                  (unsigned long)blit_fmt, (unsigned long)hr);
            blit_tex = nullptr;
            return false;
        }

        hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, work_w, work_h, blit_fmt, D3DPOOL_SYSTEMMEM,
                                                      reinterpret_cast<void **>(&readback), nullptr);
        if (FAILED(hr) || !readback) {
            VRLOG("openxr: CreateOffscreenPlainSurface(%ux%u fmt %lu) failed: 0x%08lX",
                  work_w, work_h, (unsigned long)blit_fmt, (unsigned long)hr);
            readback = nullptr;
            release_shared_surface();
            return false;
        }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = work_w;
        td.Height = work_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = d11_device->CreateTexture2D(&td, nullptr, &frame_tex);
        if (FAILED(hr) || !frame_tex) {
            VRLOG("openxr: CreateTexture2D(%ux%u) failed: 0x%08lX", work_w, work_h, (unsigned long)hr);
            release_shared_surface();
            return false;
        }
        if (FAILED(d11_device->CreateShaderResourceView(frame_tex, nullptr, &shared_srv))) {
            VRLOG("openxr: CreateShaderResourceView failed");
            release_shared_surface();
            return false;
        }

        shared_w = work_w;
        shared_h = work_h;
        shared_src_w = w;
        shared_src_h = h;
        shared_d3d9_format = fmt;
        VRLOG("openxr: surfaces %ux%u  src fmt=%lu  blit/readback fmt=%lu  (created before any frame)",
              work_w, work_h, (unsigned long)fmt, (unsigned long)blit_fmt);
        return true;
    }

'''
anchor = "    void release_shared_surface() {"
t = t.replace(anchor, new_fn + anchor, 1)

# on_end_scene: nothing to create any more, just copy
old_end = t[t.index("void OpenXrBridge::on_end_scene(IDirect3DDevice9 *d3d9_device) {"):]
old_end = old_end[:old_end.index("\n}\n") + 3]
new_end = r'''void OpenXrBridge::on_end_scene(IDirect3DDevice9 *d3d9_device) {
    if (state_ != XrState::Ready || !impl_ || !prepared_) return;
    if (device_lost_) return;   // the game is mid-reset; leave its objects alone

    // EndScene fires far more often than Present while the game is loading, so
    // this is also where session state changes get picked up.
    impl_->pump_events();
    if (!impl_->session_running) return;

    // Everything used here already exists; this path only reads and copies.
    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(d3d9_device->lpVtbl->GetRenderTarget(d3d9_device, 0, (void **)&bb)) || !bb) {
        if (FAILED(d3d9_device->lpVtbl->GetBackBuffer(d3d9_device, 0, 0, D3DBACKBUFFER_TYPE_MONO,
                                                      (void **)&bb)) ||
            !bb) {
            return;
        }
    }
    impl_->copy_backbuffer(d3d9_device, bb);
    bb->Release();
}
'''
t = t.replace(old_end, new_end, 1)

# submit: drop the removed ensure_shared_surface call
t = t.replace('''    if (g_safe_mode) {
        static bool warned = false;
        if (!warned) {
            VRLOG("submit: safe mode - session kept alive, no surface work, no composition");
            warned = true;
        }
        return false;
    }

    if (!impl_->ensure_shared_surface(d3d9_device, desc.Width, desc.Height, desc.Format)) {
        ++frames_failed_;
        return false;
    }
''', '''    if (!prepared_ || device_lost_) return false;
''')

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("restored create_device_surfaces and rewired the per-frame paths")
