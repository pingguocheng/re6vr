import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

start = t.index("    // One-off capability probe, run once right after the surfaces exist and")
end = t.index("    // Draws the game frame onto a world-locked panel for one eye.")

block = r'''    // One-off capability probe. Kept because it settled a long argument: this
    // machine *can* read a render target back, so the earlier crashes were never
    // about readback itself.
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
        }

        if (tiny_src) tiny_src->lpVtbl->Release(tiny_src);
        if (tiny_dst) tiny_dst->lpVtbl->Release(tiny_dst);
        if (tiny_rt) tiny_rt->Release();
    }

    // Per-frame: copy the game's back buffer straight into the D3D11 texture the
    // compositor samples.
    //
    // Two things are deliberately absent. There is no StretchRect: both scaling
    // (3840x2160 -> 1280x720) and format conversion through it raised on this
    // driver, and the probe above proved readback itself is fine, so the blit was
    // the only broken part. There is also no allocation: every surface exists
    // before the first frame, because creating one from inside a frame makes MT
    // Framework stop with "ERR09: Unsupported function.".
    bool copy_backbuffer(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface) {
        if (!frame_tex || !src_surface) return false;

        SehJob job = {this, d3d9_dev, src_surface, nullptr, false, false, E_FAIL};
        const bool completed = seh::guard(&SehJob::run, &job);
        if (!completed) {
            warn_once("frame copy raised a structured exception");
            return false;
        }
        return job.ok;
    }

    // Runs inside seh::guard. Locks the back buffer (read-only) and samples it
    // down to the compositor's working size on the CPU: point sampling is
    // acceptable for a virtual screen and needs no GPU-side scaling at all.
    bool do_copy(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface, IDirect3DSurface9 *) {
        D3DLOCKED_RECT locked = {};
        HRESULT hr = src_surface->lpVtbl->LockRect(src_surface, &locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr) || !locked.pBits) {
            warn_once_hr("LockRect(back buffer)", hr);
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = d11_context->Map(frame_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData) {
            warn_once_hr("Map(copy target)", hr);
            src_surface->lpVtbl->UnlockRect(src_surface);
            return false;
        }

        const uint8_t *base = (const uint8_t *)locked.pBits;
        uint8_t *dst = (uint8_t *)mapped.pData;

        // Nearest-neighbour from src_w x src_h down to shared_w x shared_h. The
        // source pitch can be larger than the visible row, so it is honoured.
        const uint32_t src_w = shared_src_w ? shared_src_w : shared_w;
        const uint32_t src_h = shared_src_h ? shared_src_h : shared_h;
        const uint32_t step_x = src_w > shared_w ? src_w / shared_w : 1;
        const uint32_t step_y = src_h > shared_h ? src_h / shared_h : 1;

        for (uint32_t y = 0; y < shared_h; ++y) {
            const uint8_t *srow = base + (size_t)(y * step_y) * locked.Pitch;
            uint32_t *drow = reinterpret_cast<uint32_t *>(dst + (size_t)y * mapped.RowPitch);
            for (uint32_t x = 0; x < shared_w; ++x) {
                const uint32_t *px = reinterpret_cast<const uint32_t *>(srow + (size_t)(x * step_x) * 4);
                drow[x] = *px;
            }
        }

        d11_context->Unmap(frame_tex, 0);
        src_surface->lpVtbl->UnlockRect(src_surface);
        d11_context->Flush();

        ++copy_count;
        if (copy_count == 1 || (copy_count % 300) == 0) {
            VRLOG("compositor: copied %llu frames (%ux%u <- %ux%u, pitch %d)",
                  (unsigned long long)copy_count, shared_w, shared_h, src_w, src_h, locked.Pitch);
        }
        return true;
    }

'''
t = t[:start] + block + t[end:]

# The blit target and the SYSTEMMEM readback surface are no longer needed; keep
# only the D3D11 side plus a compact memory note.
start2 = t.index("        // Work at 1280x720: a 4K readback is slow and is the size that makes the")
end2 = t.index("        D3D11_TEXTURE2D_DESC td = {};")
t = t[:start2] + r'''        // 1280x720 is the compositor's working size: a 4K panel buys nothing on a
        // headset and makes every copy four times more expensive.
        const uint32_t work_w = w > 1280 ? 1280 : w;
        const uint32_t work_h = w > 1280 ? (uint32_t)((uint64_t)h * 1280 / w) : h;

        d3d9_owner = dev;

''' + t[end2:]

t = t.replace('''        const D3DFORMAT blit_fmt = (fmt == D3DFMT_A8R8G8B8) ? D3DFMT_X8R8G8B8 : fmt;

''', '')
t = t.replace('''        VRLOG("openxr: surfaces %ux%u  src fmt=%lu  blit/readback fmt=%lu  (created before any frame)",
              work_w, work_h, (unsigned long)fmt, (unsigned long)blit_fmt);''',
'''        VRLOG("openxr: compositor target %ux%u for a %ux%u fmt=%lu back buffer (created up front)",
              work_w, work_h, w, h, (unsigned long)fmt);''')

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("copy path is now LockRect + CPU downscale, no StretchRect")
