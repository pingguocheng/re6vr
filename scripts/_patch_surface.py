import io
import sys

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    lines = f.readlines()


def find(pred, frm=0):
    for i in range(frm, len(lines)):
        if pred(lines[i]):
            return i
    return None


start = find(lambda l: "bool ensure_shared_surface(IDirect3DDevice9 *dev" in l)
if start is None:
    sys.exit("start not found")
# walk back over the comment block
while start > 0 and lines[start - 1].strip().startswith("//"):
    start -= 1

# the block ends right before the next member function (draw_quad) or its comment
end = find(lambda l: "bool draw_quad(" in l, start)
if end is None:
    sys.exit("draw_quad not found")
while end > start and (lines[end - 1].strip().startswith("//") or lines[end - 1].strip() == ""):
    end -= 1
end -= 1  # last line of release_shared_surface

new_block = r'''    // Creates (or recreates) the D3D11 texture the game frame is mirrored into.
    //
    // We deliberately avoid D3D9Ex shared surfaces: RE6 creates a *plain* D3D9
    // device, and blitting from that device into a surface owned by a separate
    // D3D9Ex device crashes inside d3d9.dll. Copying through a staging texture
    // needs no D3D9Ex, no cross-device blit and no format negotiation.
    bool ensure_shared_surface(IDirect3DDevice9 *dev, uint32_t w, uint32_t h, D3DFORMAT fmt) {
        if (frame_tex && shared_w == w && shared_h == h) return true;

        release_shared_surface();

        DXGI_FORMAT dxgi_fmt = DXGI_FORMAT_UNKNOWN;
        if (!d3d9_format_to_dxgi(fmt, &dxgi_fmt)) {
            VRLOG("openxr: unsupported D3D9 backbuffer format %lu", (unsigned long)fmt);
            return false;
        }
        if (dxgi_fmt != DXGI_FORMAT_B8G8R8A8_UNORM && dxgi_fmt != DXGI_FORMAT_B8G8R8X8_UNORM) {
            VRLOG("openxr: backbuffer format %s needs conversion, not supported yet",
                  dxgi_format_name(dxgi_fmt));
            return false;
        }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = d11_device->CreateTexture2D(&td, nullptr, &frame_tex);
        if (FAILED(hr) || !frame_tex) {
            VRLOG("openxr: CreateTexture2D(%ux%u staging) failed: 0x%08lX", w, h, (unsigned long)hr);
            return false;
        }
        if (FAILED(d11_device->CreateShaderResourceView(frame_tex, nullptr, &shared_srv))) {
            VRLOG("openxr: CreateShaderResourceView(staging) failed");
            release_shared_surface();
            return false;
        }
        shared_w = w;
        shared_h = h;
        shared_d3d9_format = fmt;

        VRLOG("openxr: staging texture %ux%u ready (d3d9 fmt=%lu, %u KiB per frame)",
              w, h, (unsigned long)fmt, (unsigned)((size_t)w * h * 4 / 1024));
        return true;
    }

    void release_shared_surface() {
        if (shared_srv) { shared_srv->Release(); shared_srv = nullptr; }
        if (frame_tex) { frame_tex->Release(); frame_tex = nullptr; }
        if (shared_d11_tex) { shared_d11_tex->Release(); shared_d11_tex = nullptr; }
        if (shared_rtv) { shared_rtv->Release(); shared_rtv = nullptr; }
        shared_handle = nullptr;
        shared_w = shared_h = 0;
        shared_d3d9_format = D3DFMT_UNKNOWN;
    }

    // Copies the game's back buffer into the staging texture, row by row.
    bool copy_backbuffer(IDirect3DDevice9 *d3d9_dev, uint32_t w, uint32_t h) {
        IDirect3DSurface9 *bb = nullptr;
        if (FAILED(d3d9_dev->lpVtbl->GetBackBuffer(d3d9_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO,
                                                   (void **)&bb)) ||
            !bb) {
            warn_once("GetBackBuffer");
            return false;
        }

        D3DLOCKED_RECT locked = {};
        const DWORD lock_flags = D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK;
        HRESULT hr = bb->lpVtbl->LockRect(bb, &locked, nullptr, lock_flags);
        if (FAILED(hr) || !locked.pBits) {
            warn_once_hr("LockRect(backbuffer)", hr);
            bb->Release();
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = d11_context->Map(frame_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData) {
            warn_once_hr("Map(staging texture)", hr);
            bb->lpVtbl->UnlockRect(bb);
            bb->Release();
            return false;
        }

        const uint8_t *src = (const uint8_t *)locked.pBits;
        uint8_t *dst = (uint8_t *)mapped.pData;
        const size_t row_bytes = (size_t)w * 4;
        if (locked.Pitch == (INT)mapped.RowPitch && (size_t)locked.Pitch == row_bytes) {
            memcpy(dst, src, row_bytes * h);
        } else {
            for (uint32_t y = 0; y < h; ++y) {
                memcpy(dst + (size_t)y * mapped.RowPitch, src + (size_t)y * locked.Pitch, row_bytes);
            }
        }

        d11_context->Unmap(frame_tex, 0);
        bb->lpVtbl->UnlockRect(bb);
        bb->Release();
        return true;
    }

'''

out = lines[:start] + [new_block] + lines[end + 1:]
with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.writelines(out)
print("replaced lines %d..%d" % (start + 1, end + 1))
