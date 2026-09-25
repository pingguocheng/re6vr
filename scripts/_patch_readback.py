import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

old_start = t.index("    // Mirrors the game's back buffer into `mirror_tex` with a same-device")
old_end = t.index("    // Draws the game frame onto a world-locked panel for one eye.")

new = r'''    // Mirrors the game's back buffer into the D3D11 texture the compositor
    // samples, using GetRenderTargetData into a system-memory surface.
    //
    // Everything else was tried and faults inside d3d9.dll on this machine:
    //   * a D3D9Ex shared surface on a second D3D9Ex device (cross-device blit),
    //   * LockRect on the back buffer from inside the Present hook,
    //   * creating any second D3D9 device at all, plain or Ex, headless or on
    //     the game's own window.
    // GetRenderTargetData to a SYSTEMMEM surface is the one documented readback
    // path that needs no second device.
    bool copy_backbuffer(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface) {
        if (!frame_tex || !readback || !src_surface) return false;

        HRESULT hr = d3d9_dev->lpVtbl->GetRenderTargetData(d3d9_dev, src_surface, readback);
        if (FAILED(hr)) {
            warn_once_hr("GetRenderTargetData(backbuffer -> sysmem)", hr);
            return false;
        }

        D3DLOCKED_RECT locked = {};
        hr = readback->lpVtbl->LockRect(readback, &locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr) || !locked.pBits) {
            warn_once_hr("LockRect(sysmem readback)", hr);
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
        uint8_t *dst = (uint8_t *)mapped.pData;
        const size_t row_bytes = (size_t)shared_w * 4;
        if ((size_t)locked.Pitch == (size_t)mapped.RowPitch && (size_t)locked.Pitch == row_bytes) {
            memcpy(dst, src, row_bytes * shared_h);
        } else {
            const size_t copy_bytes = row_bytes < (size_t)mapped.RowPitch ? row_bytes
                                                                         : (size_t)mapped.RowPitch;
            for (uint32_t y = 0; y < shared_h; ++y) {
                memcpy(dst + (size_t)y * mapped.RowPitch, src + (size_t)y * locked.Pitch, copy_bytes);
            }
        }

        d11_context->Unmap(frame_tex, 0);
        readback->lpVtbl->UnlockRect(readback);
        d11_context->Flush();

        ++copy_count;
        if (copy_count == 1 || (copy_count % 300) == 0) {
            VRLOG("compositor: copied %llu frames (%ux%u, pitch %d -> %u)",
                  (unsigned long long)copy_count, shared_w, shared_h, locked.Pitch, mapped.RowPitch);
        }
        return true;
    }

'''
t = t[:old_start] + new + t[old_end:]

t = t.replace("""        // The mirror texture lives on the game's own D3D9Ex device and is shared
        // with D3D11, so the compositor can sample it without a CPU round trip.
        if (!ensure_ex_d3d9()) return false;
        IDirect3DDevice9Ex *owner = acquire_ex_device(dev);
        if (!owner) return false;

        HRESULT hr = owner->lpVtbl->CreateTexture(reinterpret_cast<IDirect3DDevice9 *>(owner),
                                                  w, h, 1, D3DUSAGE_RENDERTARGET, fmt,
                                                  D3DPOOL_DEFAULT,
                                                  reinterpret_cast<void **>(&mirror_tex), &shared_handle);
        if (FAILED(hr) || !mirror_tex) {
            VRLOG("openxr: D3D9Ex CreateTexture(%ux%u mirror) failed: 0x%08lX", w, h, (unsigned long)hr);
            mirror_tex = nullptr;
            return false;
        }

        hr = d11_device->OpenSharedResource(shared_handle, __uuidof(ID3D11Texture2D),
                                           (void **)&shared_d11_tex);
        if (FAILED(hr) || !shared_d11_tex) {
            VRLOG("openxr: OpenSharedResource failed: 0x%08lX", (unsigned long)hr);
            release_shared_surface();
            return false;
        }

        D3D11_TEXTURE2D_DESC td = {};""",
"""        // A system-memory surface on the game's own device is the readback
        // source: no second device, no sharing, no Ex required.
        HRESULT hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, w, h, fmt, D3DPOOL_SYSTEMMEM,
                                                              reinterpret_cast<void **>(&readback),
                                                              nullptr);
        if (FAILED(hr) || !readback) {
            VRLOG("openxr: CreateOffscreenPlainSurface(%ux%u sysmem) failed: 0x%08lX",
                  w, h, (unsigned long)hr);
            readback = nullptr;
            return false;
        }

        D3D11_TEXTURE2D_DESC td = {};""")

t = t.replace("""        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = d11_device->CreateTexture2D(&td, nullptr, &frame_tex);""",
"""        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = d11_device->CreateTexture2D(&td, nullptr, &frame_tex);""")

t = t.replace("""        VRLOG("openxr: mirror texture %ux%u ready (shared with D3D11, d3d9 fmt=%lu)", w, h,
              (unsigned long)fmt);""",
"""        VRLOG("openxr: readback surface %ux%u ready (sysmem, d3d9 fmt=%lu)", w, h,
              (unsigned long)fmt);""")

t = t.replace("""        if (mirror_tex) { mirror_tex->Release(); mirror_tex = nullptr; }""",
"""        if (readback) { readback->lpVtbl->Release(readback); readback = nullptr; }
        copy_count = 0;""")

t = t.replace("""    IDirect3DTexture9 *mirror_tex = nullptr;  // on the game's own device""",
"""    IDirect3DSurface9 *readback = nullptr;    // SYSTEMMEM surface on the game's device
    uint64_t copy_count = 0;""")

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("rewrote the copy path to GetRenderTargetData")
