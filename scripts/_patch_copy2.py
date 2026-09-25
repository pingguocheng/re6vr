import io
import sys

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    src = f.read()

# 1) back buffer geometry comes from the header-declared setter now
src = src.replace("""    // GetDesc on the back buffer faults inside d3d9.dll on this driver, so the
    // size/format are taken from the device creation parameters instead. They
    // are already known from CreateDevice, and RE6 always presents the whole
    // back buffer.
    D3DSURFACE_DESC desc = {};
    desc.Width = g_backbuffer_w;
    desc.Height = g_backbuffer_h;
    desc.Format = g_backbuffer_format;
    bb->Release();
    HRESULT hr = S_OK;
""", """    // GetDesc on the back buffer faults inside d3d9.dll on this machine, so the
    // geometry recorded at CreateDevice time is used instead.
    D3DSURFACE_DESC desc = {};
    desc.Width = backbuffer_w();
    desc.Height = backbuffer_h();
    desc.Format = backbuffer_format();
    bb->Release();
    HRESULT hr = S_OK;
""")

# 2) drop the local definitions, they live in the header now
src = src.replace("""
// Back buffer geometry as reported by IDirect3D9::CreateDevice. Querying it via
// IDirect3DSurface9::GetDesc faults inside d3d9.dll on this machine (see
// submit()), so the values captured at device creation are used instead.
uint32_t g_backbuffer_w = 1280;
uint32_t g_backbuffer_h = 720;
D3DFORMAT g_backbuffer_format = 22 /*D3DFMT_X8R8G8B8*/;

void set_backbuffer_geometry(uint32_t w, uint32_t h, D3DFORMAT fmt) {
    g_backbuffer_w = w;
    g_backbuffer_h = h;
    g_backbuffer_format = fmt;
}
""", "")

# 3) replace the CPU readback with a same-device StretchRect into a shared texture
old_start = src.index("    // Copies the game's back buffer into the staging texture, row by row.")
old_end = src.index("    // Draws the game frame onto a world-locked panel for one eye.")
new_block = r'''    // Mirrors the game's back buffer into `mirror_tex` with a same-device
    // StretchRect.
    //
    // Two earlier designs both faulted inside d3d9.dll:
    //   * sharing a D3D9Ex surface with a separate D3D9Ex device (cross-device
    //     blit from a plain D3D9 device),
    //   * LockRect on the back buffer from inside the Present hook.
    // A StretchRect between two surfaces of the *same* device is the documented,
    // driver-supported path, and it stays a GPU copy.
    bool copy_backbuffer(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface) {
        if (!mirror_tex || !src_surface) return false;

        IDirect3DSurface9 *dst = nullptr;
        if (FAILED(mirror_tex->GetSurfaceLevel(0, &dst)) || !dst) {
            warn_once("GetSurfaceLevel(mirror texture)");
            return false;
        }
        HRESULT hr = d3d9_dev->lpVtbl->StretchRect(d3d9_dev, src_surface, nullptr, dst, nullptr,
                                                  D3DTEXF_NONE);
        dst->Release();
        if (FAILED(hr)) {
            warn_once_hr("StretchRect(backbuffer -> mirror texture)", hr);
            return false;
        }

        // Make the D3D11 side see the new contents of the shared texture.
        d11_context->CopyResource(frame_tex, shared_d11_tex);
        d11_context->Flush();
        return true;
    }

'''
src = src[:old_start] + new_block + src[old_end:]

# 4) the quad draws the mirror texture's D3D11 view; frame_tex becomes the copy target
src = src.replace("""        // 1) mirror the game's back buffer into the staging texture
        static bool s_skip_copy = false;
        static bool s_skip_checked = false;
        if (!s_skip_checked) {
            wchar_t v[8] = L"";
            s_skip_copy = GetEnvironmentVariableW(L"RE6VR_SKIP_COPY", v, 8) > 0 && v[0] == L'1';
            s_skip_checked = true;
            VRLOG("draw_quad: skip_copy=%d", (int)s_skip_copy);
        }
        if (!s_skip_copy) {
            if (!copy_backbuffer(d3d9_dev, shared_w, shared_h)) return false;
        }
""", """        (void)d3d9_dev;
""")

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(src)
print("patched copy path")
