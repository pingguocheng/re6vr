import io
import re

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# --- rewrite the surface creation block with formats decided up front ---------
start = t.index("        // Render targets cannot be locked, so the GPU blit goes into a small")
end = t.index("        D3D11_TEXTURE2D_DESC td = {};")

new = r'''        // Only two formats ever reach this point: the back buffer's A8R8G8B8 and
        // the lockable X8R8G8B8. GetRenderTargetData refuses an A8R8G8B8 ->
        // A8R8G8B8 pair ("the formats must be identical"), so when the game
        // presents A8R8G8B8 the intermediate and the readback surface are both
        // X8R8G8B8 and the StretchRect doubles as the format conversion.
        const D3DFORMAT src_fmt = fmt;
        const D3DFORMAT blit_fmt = (fmt == D3DFMT_A8R8G8B8) ? D3DFMT_X8R8G8B8 : fmt;

        HRESULT hr = E_FAIL;
        if (work_w != w) {
            hr = dev->lpVtbl->CreateTexture(dev, work_w, work_h, 1, D3DUSAGE_RENDERTARGET, src_fmt,
                                            D3DPOOL_DEFAULT,
                                            reinterpret_cast<void **>(&scaler), nullptr);
            if (FAILED(hr) || !scaler) {
                VRLOG("openxr: CreateTexture(%ux%u scaler, fmt %lu) failed: 0x%08lX",
                      work_w, work_h, (unsigned long)src_fmt, (unsigned long)hr);
                scaler = nullptr;
                return false;
            }
        }

        hr = dev->lpVtbl->CreateTexture(dev, work_w, work_h, 1, D3DUSAGE_RENDERTARGET, blit_fmt,
                                        D3DPOOL_DEFAULT,
                                        reinterpret_cast<void **>(&lock_surface), nullptr);
        if (FAILED(hr) || !lock_surface) {
            VRLOG("openxr: CreateTexture(%ux%u blit target, fmt %lu) failed: 0x%08lX",
                  work_w, work_h, (unsigned long)blit_fmt, (unsigned long)hr);
            lock_surface = nullptr;
            release_shared_surface();
            return false;
        }

        hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, work_w, work_h, blit_fmt,
                                                      D3DPOOL_SYSTEMMEM,
                                                      reinterpret_cast<void **>(&readback), nullptr);
        if (FAILED(hr) || !readback) {
            VRLOG("openxr: CreateOffscreenPlainSurface(%ux%u fmt %lu) failed: 0x%08lX",
                  work_w, work_h, (unsigned long)blit_fmt, (unsigned long)hr);
            readback = nullptr;
            release_shared_surface();
            return false;
        }

        VRLOG("openxr: surfaces ready %ux%u  src fmt=%lu  blit/readback fmt=%lu  scaler=%s",
              work_w, work_h, (unsigned long)src_fmt, (unsigned long)blit_fmt,
              scaler ? "yes" : "no");

'''
t = t[:start] + new + t[end:]

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("surface creation rewritten with formats decided up front")
