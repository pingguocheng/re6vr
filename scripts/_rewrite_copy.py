import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# Replace everything from the probe through do_copy with one coherent block.
start = t.index("    // Copies a 16x16 corner from the blit target into a tiny SYSTEMMEM surface.")
end = t.index("    // Draws the game frame onto a world-locked panel for one eye.")

block = r'''    // Work handed to seh::guard. SEH needs the guarded body in its own function,
    // so each operation gets a static entry point here.
    struct SehJob {
        Impl *self;
        IDirect3DDevice9 *dev;
        IDirect3DSurface9 *src;
        IDirect3DSurface9 *dst;
        bool is_readback;   // true: GetRenderTargetData(src -> dst)
        bool ok = false;
        HRESULT hr = E_FAIL;

        static void run(void *ctx) {
            SehJob *j = static_cast<SehJob *>(ctx);
            if (j->is_readback) {
                j->hr = j->self->d3d9_owner->lpVtbl->GetRenderTargetData(j->self->d3d9_owner,
                                                                        j->src, j->dst);
                j->ok = SUCCEEDED(j->hr);
            } else {
                j->ok = j->self->do_copy(j->dev, j->src, j->dst);
            }
        }
    };

    // One-off capability probe, run once right after the surfaces exist and
    // before any frame is rendered. Dimensions and formats match, so a failure
    // here means the driver refuses the operation itself.
    void probe_readback(IDirect3DDevice9 *dev) {
        const D3DFORMAT blit_fmt = (shared_d3d9_format == D3DFMT_A8R8G8B8) ? D3DFMT_X8R8G8B8
                                                                          : shared_d3d9_format;
        IDirect3DTexture9 *tiny_rt = nullptr;
        IDirect3DSurface9 *tiny_src = nullptr;
        IDirect3DSurface9 *tiny_dst = nullptr;

        HRESULT hr = dev->lpVtbl->CreateTexture(dev, 16, 16, 1, D3DUSAGE_RENDERTARGET, blit_fmt,
                                                D3DPOOL_DEFAULT,
                                                reinterpret_cast<void **>(&tiny_rt), nullptr);
        if (SUCCEEDED(hr) && tiny_rt) tiny_rt->GetSurfaceLevel(0, &tiny_src);
        hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, 16, 16, blit_fmt, D3DPOOL_SYSTEMMEM,
                                                      reinterpret_cast<void **>(&tiny_dst), nullptr);

        if (tiny_src && tiny_dst) {
            SehJob job = {this, dev, tiny_src, tiny_dst, true, false, E_FAIL};
            if (!seh::guard(&SehJob::run, &job)) {
                VRLOG("probe: GetRenderTargetData RAISED - readback unusable on this machine");
            } else {
                VRLOG("probe: GetRenderTargetData returned 0x%08lX - readback %s",
                      (unsigned long)job.hr, SUCCEEDED(job.hr) ? "WORKS" : "REFUSED");
            }
        } else {
            VRLOG("probe: probe surfaces unavailable (rt=%p src=%p dst=%p)", (void *)tiny_rt,
                  (void *)tiny_src, (void *)tiny_dst);
        }

        if (tiny_src) tiny_src->lpVtbl->Release(tiny_src);
        if (tiny_dst) tiny_dst->lpVtbl->Release(tiny_dst);
        if (tiny_rt) tiny_rt->Release();
    }

    // Per-frame: mirror the game's back buffer into the texture the compositor
    // samples. Only pre-existing surfaces are used, never created here.
    bool copy_backbuffer(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface) {
        if (!frame_tex || !blit_tex || !readback || !src_surface) return false;

        IDirect3DSurface9 *dst = nullptr;
        if (FAILED(blit_tex->GetSurfaceLevel(0, &dst)) || !dst) {
            warn_once("GetSurfaceLevel(blit target)");
            return false;
        }

        SehJob job = {this, d3d9_dev, src_surface, dst, false, false, E_FAIL};
        const bool completed = seh::guard(&SehJob::run, &job);
        dst->lpVtbl->Release(dst);

        if (!completed) {
            warn_once("frame copy raised a structured exception");
            return false;
        }
        return job.ok;
    }

    bool do_copy(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface, IDirect3DSurface9 *dst) {
        // 1) GPU copy + downscale + A8->X8 conversion, into the blit target.
        HRESULT hr = d3d9_dev->lpVtbl->StretchRect(d3d9_dev, src_surface, nullptr, dst, nullptr,
                                                   D3DTEXF_NONE);
        if (FAILED(hr)) {
            warn_once_hr("StretchRect(backbuffer -> blit target)", hr);
            return false;
        }

        // 2) into the matching SYSTEMMEM surface (same size, same format).
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

        const uint8_t *s = (const uint8_t *)locked.pBits;
        uint8_t *d = (uint8_t *)mapped.pData;
        const size_t want = (size_t)shared_w * 4;
        const size_t row_copy = want < (size_t)mapped.RowPitch ? want : (size_t)mapped.RowPitch;
        for (uint32_t y = 0; y < shared_h; ++y) {
            memcpy(d + (size_t)y * mapped.RowPitch, s + (size_t)y * locked.Pitch, row_copy);
        }

        d11_context->Unmap(frame_tex, 0);
        readback->lpVtbl->UnlockRect(readback);
        d11_context->Flush();

        ++copy_count;
        if (copy_count == 1 || (copy_count % 300) == 0) {
            VRLOG("compositor: copied %llu frames", (unsigned long long)copy_count);
        }
        return true;
    }

'''
t = t[:start] + block + t[end:]

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("rewrote the copy/probe section coherently")
