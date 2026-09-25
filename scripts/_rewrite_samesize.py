import io

path = r"\re6vr\src\openxr_bridge.cpp".replace("\\d", "C:\\d", 1)
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# ---- surfaces: full-size render target + matching readback, created up front
s = t.index("        // 1280x720 is the compositor's working size: a 4K panel buys nothing on a")
e = t.index("        D3D11_TEXTURE2D_DESC td = {};")
t = t[:s] + r'''        // The GPU copy target is deliberately the *same size and format* as the
        // back buffer. GetRenderTargetData demands an exact match, and the GPU
        // StretchRect that used to do the downscaling and the format conversion
        // raised on this driver - so the copy is a plain same-format blit and the
        // shrink to the panel's 1280x720 happens on the CPU afterwards.
        d3d9_owner = dev;
        HRESULT hr = S_OK;

        hr = dev->lpVtbl->CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT,
                                        reinterpret_cast<void **>(&blit_tex), nullptr);
        if (FAILED(hr) || !blit_tex) {
            VRLOG("openxr: CreateTexture(%ux%u fmt %lu) failed: 0x%08lX", w, h, (unsigned long)fmt,
                  (unsigned long)hr);
            blit_tex = nullptr;
            return false;
        }

        hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, w, h, fmt, D3DPOOL_SYSTEMMEM,
                                                      reinterpret_cast<void **>(&readback), nullptr);
        if (FAILED(hr) || !readback) {
            VRLOG("openxr: CreateOffscreenPlainSurface(%ux%u fmt %lu) failed: 0x%08lX", w, h,
                  (unsigned long)fmt, (unsigned long)hr);
            readback = nullptr;
            release_shared_surface();
            return false;
        }

        const uint32_t work_w = w > 1280 ? 1280 : w;
        const uint32_t work_h = w > 1280 ? (uint32_t)((uint64_t)h * 1280 / w) : h;

''' + t[e:]

# ---- copy: blit into the same-size RT, read it back, shrink on the CPU
s = t.index("    // Runs inside seh::guard. Locks the back buffer (read-only) and samples it")
e = t.index("    // Draws the game frame onto a world-locked panel for one eye.")
t = t[:s] + r'''    // Runs inside seh::guard.
    bool do_copy(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface, IDirect3DSurface9 *dst) {
        // 1) same-size, same-format GPU copy. Dimensions and format match exactly,
        //    which is all this driver reliably accepts.
        HRESULT hr = d3d9_dev->lpVtbl->StretchRect(d3d9_dev, src_surface, nullptr, dst, nullptr,
                                                   D3DTEXF_NONE);
        if (FAILED(hr)) {
            warn_once_hr("StretchRect(back buffer -> copy target)", hr);
            return false;
        }

        // 2) the documented readback, proven to work by the probe.
        hr = d3d9_dev->lpVtbl->GetRenderTargetData(d3d9_dev, dst, readback);
        if (FAILED(hr)) {
            warn_once_hr("GetRenderTargetData(copy target -> sysmem)", hr);
            return false;
        }

        D3DLOCKED_RECT locked = {};
        hr = readback->lpVtbl->LockRect(readback, &locked, nullptr, D3DLOCK_READONLY);
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

        // 3) nearest-neighbour shrink to the panel size.
        const uint8_t *base = (const uint8_t *)locked.pBits;
        uint8_t *out = (uint8_t *)mapped.pData;
        const uint32_t src_w = shared_src_w ? shared_src_w : shared_w;
        const uint32_t src_h = shared_src_h ? shared_src_h : shared_h;
        const uint32_t step_x = src_w > shared_w ? src_w / shared_w : 1;
        const uint32_t step_y = src_h > shared_h ? src_h / shared_h : 1;

        for (uint32_t y = 0; y < shared_h; ++y) {
            const uint8_t *srow = base + (size_t)(y * step_y) * locked.Pitch;
            uint32_t *drow = reinterpret_cast<uint32_t *>(out + (size_t)y * mapped.RowPitch);
            for (uint32_t x = 0; x < shared_w; ++x) {
                drow[x] = *reinterpret_cast<const uint32_t *>(srow + (size_t)(x * step_x) * 4);
            }
        }

        d11_context->Unmap(frame_tex, 0);
        readback->lpVtbl->UnlockRect(readback);
        d11_context->Flush();

        ++copy_count;
        if (copy_count == 1 || (copy_count % 300) == 0) {
            VRLOG("compositor: copied %llu frames (%ux%u <- %ux%u)", (unsigned long long)copy_count,
                  shared_w, shared_h, src_w, src_h);
        }
        return true;
    }

''' + t[e:]

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("copy path: same-size StretchRect + proven readback + CPU shrink")
