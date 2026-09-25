import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# ---- replace the whole copy_backbuffer section with a StretchRect + LockRect path
start = t.index("    // Mirrors the game's back buffer into the D3D11 texture the compositor")
end = t.index("    // Draws the game frame onto a world-locked panel for one eye.")

new = r'''    // Mirrors the game's back buffer into the D3D11 texture the compositor
    // samples: GPU StretchRect into an offscreen render target, then a small
    // LockRect readback of that.
    //
    // Rejected after being observed to crash or raise on this machine:
    //   * D3D9Ex shared surface + cross-device blit  -> fault in d3d9.dll
    //   * LockRect on the back buffer inside Present -> fault in d3d9.dll
    //   * any second D3D9 device (plain or Ex)       -> fault in d3d9.dll
    //   * IDirect3DSurface9::GetDesc                 -> fault in d3d9.dll
    //   * GetRenderTargetData at 3840x2160           -> structured exception
    // What is left is entirely same-device GPU work plus a lock, which is the
    // oldest and best-trodden D3D9 path.
    bool copy_backbuffer(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface) {
        if (!frame_tex || !lock_surface || !src_surface) return false;

        IDirect3DSurface9 *dst = nullptr;
        if (FAILED(lock_surface->GetSurfaceLevel(0, &dst)) || !dst) {
            warn_once("GetSurfaceLevel(lock surface)");
            return false;
        }

        // CopyJob/ReadbackJob run under seh::guard so a driver fault costs one
        // frame instead of the game process.
        CopyJob job = {this, d3d9_dev, src_surface, dst};
        const bool completed = seh::guard(&CopyJob::run, &job);
        dst->Release();

        if (!completed) {
            warn_once("back buffer copy raised a structured exception");
            release_shared_surface();   // rebuild cleanly on the next attempt
            return false;
        }
        return job.ok;
    }

    struct CopyJob {
        Impl *self;
        IDirect3DDevice9 *dev;
        IDirect3DSurface9 *src;
        IDirect3DSurface9 *dst;
        bool ok = false;

        static void run(void *ctx) {
            CopyJob *j = static_cast<CopyJob *>(ctx);
            j->ok = j->self->do_copy(j->dev, j->src, j->dst);
        }
    };

    bool do_copy(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface, IDirect3DSurface9 *dst) {
        // 1) GPU copy (and downscale when the source is a 4K frame) into an
        //    offscreen render target whose format matches the back buffer.
        HRESULT hr = d3d9_dev->lpVtbl->StretchRect(d3d9_dev, src_surface, nullptr, dst, nullptr,
                                                   D3DTEXF_LINEAR);
        if (FAILED(hr)) {
            warn_once_hr("StretchRect(backbuffer -> lock surface)", hr);
            return false;
        }

        // 2) lock the small surface and move the pixels into D3D11.
        D3DLOCKED_RECT locked = {};
        hr = lock_surface->lpVtbl->LockRect(lock_surface, 0, &locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr) || !locked.pBits) {
            warn_once_hr("LockRect(lock surface)", hr);
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = d11_context->Map(frame_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData) {
            warn_once_hr("Map(copy target)", hr);
            lock_surface->lpVtbl->UnlockRect(lock_surface, 0);
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
        lock_surface->lpVtbl->UnlockRect(lock_surface, 0);
        d11_context->Flush();

        ++copy_count;
        if (copy_count == 1 || (copy_count % 300) == 0) {
            VRLOG("compositor: copied %llu frames (%ux%u, pitch %d -> %u)",
                  (unsigned long long)copy_count, shared_w, shared_h, locked.Pitch, mapped.RowPitch);
        }
        return true;
    }

'''
t = t[:start] + new + t[end:]

# ---- members: swap the sysmem readback surface for an offscreen RT texture
t = t.replace('''    IDirect3DSurface9 *readback = nullptr;    // SYSTEMMEM surface on the game's device''',
              '''    IDirect3DTexture9 *lock_surface = nullptr; // offscreen RT we lock and read''')

# ---- creation
t = t.replace('''        hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, work_w, work_h, fmt, D3DPOOL_SYSTEMMEM,
                                                      reinterpret_cast<void **>(&readback), nullptr);
        if (FAILED(hr) || !readback) {
            VRLOG("openxr: CreateOffscreenPlainSurface(%ux%u sysmem) failed: 0x%08lX",
                  work_w, work_h, (unsigned long)hr);
            readback = nullptr;
            release_shared_surface();
            return false;
        }''',
'''        // Plain D3DPOOL_DEFAULT offscreen render target: StretchRect can target
        // it, and D3D9 permits locking it for reading.
        hr = dev->lpVtbl->CreateTexture(dev, work_w, work_h, 1, D3DUSAGE_RENDERTARGET, fmt,
                                        D3DPOOL_DEFAULT,
                                        reinterpret_cast<void **>(&lock_surface), nullptr);
        if (FAILED(hr) || !lock_surface) {
            VRLOG("openxr: CreateTexture(%ux%u lock surface) failed: 0x%08lX", work_w, work_h,
                  (unsigned long)hr);
            lock_surface = nullptr;
            release_shared_surface();
            return false;
        }''')

t = t.replace('''        if (readback) { readback->lpVtbl->Release(readback); readback = nullptr; }''',
              '''        if (lock_surface) { lock_surface->Release(); lock_surface = nullptr; }''')

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("copy path is now StretchRect + LockRect")
