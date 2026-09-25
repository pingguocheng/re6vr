import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# ---------------------------------------------------------------- new API impl
anchor = "void OpenXrBridge::on_end_scene(IDirect3DDevice9 *d3d9_device) {"
new_fn = r'''// Creates every D3D9 surface the compositor needs, called once right after the
// game's device exists.
//
// This is the whole point of the design: MT Framework stops with
// "ERR09: Unsupported function." if a texture is created from inside a frame, so
// nothing the compositor needs may be allocated lazily. Creating it here (before
// the first BeginScene) is safe, and the per-frame path only ever reuses what
// already exists.
bool OpenXrBridge::prepare(IDirect3DDevice9 *d3d9_device, uint32_t w, uint32_t h, D3DFORMAT fmt) {
    if (!impl_ || prepared_) return prepared_;
    if (!impl_->frame_tex) return false;   // called again after a device reset

    set_backbuffer_geometry(w, h, fmt);
    if (!impl_->create_device_surfaces(d3d9_device, w, h, fmt)) {
        VRLOG("prepare: could not create the compositor surfaces, VR stays off");
        return false;
    }
    prepared_ = true;
    VRLOG("prepare: compositor surfaces ready for a %ux%u fmt=%lu back buffer", w, h,
          (unsigned long)fmt);
    return true;
}

void OpenXrBridge::release_surfaces() {
    prepared_ = false;
    if (impl_) impl_->release_shared_surface();
}

'''
t = t.replace(anchor, new_fn + anchor, 1)

# --------------------------------------------------- member: create_device_surfaces
mem_anchor = "    bool ensure_shared_surface(IDirect3DDevice9 *dev, uint32_t w, uint32_t h, D3DFORMAT fmt) {"
create_fn = r'''    // Allocates the blit target and the matching SYSTEMMEM readback surface.
    // Called once per device, never from inside a frame.
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

        // Work at 1280x720: a 4K readback is both slow and the size that makes
        // the driver raise.
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
t = t.replace(mem_anchor, create_fn + mem_anchor, 1)

# --------------------------------------------------- per-frame copy uses LockRect
old_copy = t[t.index("    bool do_copy(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface, IDirect3DSurface9 *dst) {"):]
old_copy = old_copy[:old_copy.index("\n    }\n") + len("\n    }\n")]
new_copy = r'''    bool do_copy(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface, IDirect3DSurface9 *dst) {
        // 1) GPU copy (plus the 4K -> 720p downscale and the A8 -> X8 format
        //    conversion) into the surface that was created before the frame.
        HRESULT hr = d3d9_dev->lpVtbl->StretchRect(d3d9_dev, src_surface, nullptr, dst, nullptr,
                                                   D3DTEXF_NONE);
        if (FAILED(hr)) {
            warn_once_hr("StretchRect(backbuffer -> blit target)", hr);
            return false;
        }

        // 2) into the SYSTEMMEM surface, then lock that (a plain surface, so it
        //    locks; the render target above would not).
        hr = d3d9_dev->lpVtbl->GetRenderTargetData(d3d9_dev, dst, readback);
        if (FAILED(hr)) {
            warn_once_hr("GetRenderTargetData(blit target -> sysmem)", hr);
            return false;
        }

        D3DLOCKED_RECT locked = {};
        hr = readback->lpVtbl->LockRect(readback, &locked, nullptr, 0);
        if (FAILED(hr) || !locked.pBits) {
            warn_once_hr("LockRect(readback surface)", hr);
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = d11_context->Map(frame_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData) {
            warn_once_hr("Map(copy target)", hr);
            readback->lpVtbl->UnlockRect(readback);
            return false;
        }

        const uint8_t *src = (const uint8_t *)locked.pBits;
        uint8_t *d = (uint8_t *)mapped.pData;
        const size_t want = (size_t)shared_w * 4;
        const size_t row_copy = want < (size_t)mapped.RowPitch ? want : (size_t)mapped.RowPitch;
        for (uint32_t y = 0; y < shared_h; ++y) {
            memcpy(d + (size_t)y * mapped.RowPitch, src + (size_t)y * locked.Pitch, row_copy);
        }

        d11_context->Unmap(frame_tex, 0);
        readback->lpVtbl->UnlockRect(readback);
        d11_context->Flush();

        ++copy_count;
        if (copy_count == 1 || (copy_count % 300) == 0) {
            VRLOG("compositor: copied %llu frames (%ux%u)", (unsigned long long)copy_count,
                  shared_w, shared_h);
        }
        return true;
    }
'''
t = t.replace(old_copy, new_copy, 1)

# members: rename lock_surface -> blit_tex, drop scaler
t = t.replace("    IDirect3DTexture9 *lock_surface = nullptr; // small offscreen RT for the GPU blit",
              "    IDirect3DTexture9 *blit_tex = nullptr;    // small offscreen RT, created up front")
t = t.replace("    IDirect3DTexture9 *scaler   = nullptr;    // GPU downscale target (RT texture)\n", "")
t = t.replace("if (!frame_tex || !lock_surface || !src_surface) return false;",
              "if (!frame_tex || !blit_tex || !readback || !src_surface) return false;")
t = t.replace("if (FAILED(lock_surface->GetSurfaceLevel(0, &dst)) || !dst) {",
              "if (FAILED(blit_tex->GetSurfaceLevel(0, &dst)) || !dst) {")
t = t.replace("        if (lock_surface) { lock_surface->Release(); lock_surface = nullptr; }",
              "        if (blit_tex) { blit_tex->Release(); blit_tex = nullptr; }")
t = t.replace("if (scaler) { scaler->Release(); scaler = nullptr; }", "")

# safe mode no longer applies: the whole point is that nothing is created per frame
t = t.replace("bool g_safe_mode = true;", "bool g_safe_mode = false;")

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("surfaces are now created before the first frame")
