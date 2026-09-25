// d3d9_proxy.cpp - a d3d9.dll proxy that injects the OpenXR compositor into
// Resident Evil 6 (BH6.exe, 32-bit DX9 / MT Framework).
//
// Why a proxy DLL: the game has no d3d9.dll of its own, so dropping ours next
// to BH6.exe makes the loader resolve Direct3DCreate9 to us. We hand back a
// wrapper whose only patched entry point is IDirect3D9::CreateDevice; the real
// device is left completely untouched apart from a MinHook detour on
// IDirect3DDevice9::Present, which is where composition happens.
//
// This keeps the engine's own state intact (no vtable surgery on the device),
// which matters a lot for a title this old and fragile.
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "d3d9_min.h"
#include "log.h"
#include "matrix_probe.h"
#include "view_probe.h"
#include "cam_steer.h"
#include "cam_hook.h"
#include "mem_cam.h"
#include "mem_scan.h"
#include "proxy_trace.h"
#include "screenshot.h"
#include "openxr_bridge.h"

namespace {

// --------------------------------------------------------------- globals
HMODULE               g_self = nullptr;
PFN_Direct3DCreate9   g_real_create9 = nullptr;
PFN_Direct3DCreate9Ex g_real_create9ex = nullptr;
re6vr::OpenXrBridge  *g_bridge = nullptr;
bool                  g_present_hooked = false;
bool                  g_create_device_hooked = false;
bool                  g_no_xr = false;   // RE6VR_NO_XR=1 -> passthrough only
bool                  g_selftest = false; // RE6VR_SELFTEST=1 -> offline panel check
bool                  g_selftest_done = false;
bool                  g_selftest_d11_ready = false;   // the checks' own D3D11 device is up
LONG                  g_present_calls = 0;
DWORD                 g_init_not_before = 0;   // OpenXR init waits for this tick
LONG                  g_create_device_calls = 0;
LONG                  g_create_deviceex_calls = 0;

// The games we care about create exactly one IDirect3D9, so the real factory is
// cached here as well. Recovering it from a module-level variable instead of
// from the wrapper object removes any chance of reading a corrupted or
// mis-laid-out wrapper (see the 0xCCCCCCE0 note in README.md).
IDirect3D9   *g_real_d3d9 = nullptr;
IDirect3D9Ex *g_real_d3d9ex = nullptr;

// Forwarders recover the real object from the wrapper. The wrapper counts its
// own references and holds exactly one reference on the real object, so the
// game releasing the wrapper (instead of the object it actually obtained) can
// never free the real object from under us.
template <typename T>
T *real_of(void *self) {
    return static_cast<T *>(*reinterpret_cast<void **>(self));
}

ULONG wrapper_addref(void *self) { return InterlockedIncrement((volatile LONG *)self + 2); }

ULONG wrapper_release(void *self) {
    LONG n = InterlockedDecrement((volatile LONG *)self + 2);
    if (n <= 0) {
        void **fields = reinterpret_cast<void **>(self);
        auto *real = static_cast<IDirect3D9 *>(fields[1]);
        VRLOG("wrapper: refcount hit zero, releasing the real object 0x%p", (void *)real);
        if (real) real->lpVtbl->Release(real);
        fields[1] = nullptr;
        delete[] reinterpret_cast<char *>(self);
    }
    return (ULONG)(n > 0 ? n : 0);
}

// ------------------------------------------------- IDirect3D9 wrapper
IDirect3D9Vtbl g_wrapped9_vtbl;
IDirect3D9ExVtbl g_wrapped9ex_vtbl;

struct WrappedD3D9 {
    IDirect3D9Vtbl *lpVtbl;
    IDirect3D9     *real;
    volatile LONG   refs;
    LONG            pad;
};

HRESULT STDMETHODCALLTYPE w9_QueryInterface(IDirect3D9 *self, const IID &riid, void **out) {
    // Hand out the real object: the game always uses it as an IDirect3D9, and
    // an identity IID would otherwise give it a pointer it cannot Release.
    return real_of<IDirect3D9>(self)->lpVtbl->QueryInterface(real_of<IDirect3D9>(self), riid, out);
}
ULONG STDMETHODCALLTYPE w9_AddRef(IDirect3D9 *self) { return wrapper_addref(self); }
ULONG STDMETHODCALLTYPE w9_Release(IDirect3D9 *self) { return wrapper_release(self); }
HRESULT STDMETHODCALLTYPE w9_RegisterSoftwareDevice(IDirect3D9 *self, void *a) {
    return real_of<IDirect3D9>(self)->lpVtbl->RegisterSoftwareDevice(real_of<IDirect3D9>(self), a);
}
UINT STDMETHODCALLTYPE w9_GetAdapterCount(IDirect3D9 *self) {
    return real_of<IDirect3D9>(self)->lpVtbl->GetAdapterCount(real_of<IDirect3D9>(self));
}
HRESULT STDMETHODCALLTYPE w9_GetAdapterIdentifier(IDirect3D9 *self, UINT a, DWORD b, void *c) {
    return real_of<IDirect3D9>(self)->lpVtbl->GetAdapterIdentifier(real_of<IDirect3D9>(self), a, b, c);
}
UINT STDMETHODCALLTYPE w9_GetAdapterModeCount(IDirect3D9 *self, UINT a, D3DFORMAT b) {
    return real_of<IDirect3D9>(self)->lpVtbl->GetAdapterModeCount(real_of<IDirect3D9>(self), a, b);
}
HRESULT STDMETHODCALLTYPE w9_EnumAdapterModes(IDirect3D9 *self, UINT a, D3DFORMAT b, UINT c, D3DDISPLAYMODE *d) {
    return real_of<IDirect3D9>(self)->lpVtbl->EnumAdapterModes(real_of<IDirect3D9>(self), a, b, c, d);
}
HRESULT STDMETHODCALLTYPE w9_GetAdapterDisplayMode(IDirect3D9 *self, UINT a, D3DDISPLAYMODE *b) {
    return real_of<IDirect3D9>(self)->lpVtbl->GetAdapterDisplayMode(real_of<IDirect3D9>(self), a, b);
}
HRESULT STDMETHODCALLTYPE w9_CheckDeviceType(IDirect3D9 *self, UINT a, DWORD b, D3DFORMAT c, D3DFORMAT d, BOOL e) {
    return real_of<IDirect3D9>(self)->lpVtbl->CheckDeviceType(real_of<IDirect3D9>(self), a, b, c, d, e);
}
HRESULT STDMETHODCALLTYPE w9_CheckDeviceFormat(IDirect3D9 *self, UINT a, DWORD b, D3DFORMAT c, DWORD d, DWORD e, D3DFORMAT f) {
    return real_of<IDirect3D9>(self)->lpVtbl->CheckDeviceFormat(real_of<IDirect3D9>(self), a, b, c, d, e, f);
}
HRESULT STDMETHODCALLTYPE w9_CheckDeviceMultiSampleType(IDirect3D9 *self, UINT a, DWORD b, D3DFORMAT c, BOOL d, DWORD e, DWORD *f) {
    return real_of<IDirect3D9>(self)->lpVtbl->CheckDeviceMultiSampleType(real_of<IDirect3D9>(self), a, b, c, d, e, f);
}
HRESULT STDMETHODCALLTYPE w9_CheckDepthStencilMatch(IDirect3D9 *self, UINT a, DWORD b, D3DFORMAT c, D3DFORMAT d, D3DFORMAT e) {
    return real_of<IDirect3D9>(self)->lpVtbl->CheckDepthStencilMatch(real_of<IDirect3D9>(self), a, b, c, d, e);
}
HRESULT STDMETHODCALLTYPE w9_CheckDeviceFormatConversion(IDirect3D9 *self, UINT a, DWORD b, D3DFORMAT c, D3DFORMAT d) {
    return real_of<IDirect3D9>(self)->lpVtbl->CheckDeviceFormatConversion(real_of<IDirect3D9>(self), a, b, c, d);
}
HRESULT STDMETHODCALLTYPE w9_GetDeviceCaps(IDirect3D9 *self, UINT a, DWORD b, void *c) {
    return real_of<IDirect3D9>(self)->lpVtbl->GetDeviceCaps(real_of<IDirect3D9>(self), a, b, c);
}
HMONITOR STDMETHODCALLTYPE w9_GetAdapterMonitor(IDirect3D9 *self, UINT a) {
    return real_of<IDirect3D9>(self)->lpVtbl->GetAdapterMonitor(real_of<IDirect3D9>(self), a);
}
HRESULT STDMETHODCALLTYPE w9_CreateDevice(IDirect3D9 *self, UINT adapter, DWORD dev_type, HWND hwnd,
                                          DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out);

// The IDirect3D9Ex wrapper: same 17 forwarding slots plus CreateDeviceEx. It is
// a distinct type because the Ex vtable has no plain CreateDevice at all.
struct WrappedD3D9Ex {
    IDirect3D9ExVtbl *lpVtbl;
    IDirect3D9Ex     *real;
    volatile LONG    refs;
    LONG             pad;
};

HRESULT STDMETHODCALLTYPE w9ex_QueryInterface(IDirect3D9Ex *self, const IID &riid, void **out) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->QueryInterface(real_of<IDirect3D9Ex>(self), riid, out);
}
ULONG STDMETHODCALLTYPE w9ex_AddRef(IDirect3D9Ex *self) { return wrapper_addref(self); }
ULONG STDMETHODCALLTYPE w9ex_Release(IDirect3D9Ex *self) { return wrapper_release(self); }
HRESULT STDMETHODCALLTYPE w9ex_RegisterSoftwareDevice(IDirect3D9Ex *self, void *a) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->RegisterSoftwareDevice(real_of<IDirect3D9Ex>(self), a);
}
UINT STDMETHODCALLTYPE w9ex_GetAdapterCount(IDirect3D9Ex *self) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->GetAdapterCount(real_of<IDirect3D9Ex>(self));
}
HRESULT STDMETHODCALLTYPE w9ex_GetAdapterIdentifier(IDirect3D9Ex *self, UINT a, DWORD b, void *c) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->GetAdapterIdentifier(real_of<IDirect3D9Ex>(self), a, b, c);
}
UINT STDMETHODCALLTYPE w9ex_GetAdapterModeCount(IDirect3D9Ex *self, UINT a, D3DFORMAT b) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->GetAdapterModeCount(real_of<IDirect3D9Ex>(self), a, b);
}
HRESULT STDMETHODCALLTYPE w9ex_EnumAdapterModes(IDirect3D9Ex *self, UINT a, D3DFORMAT b, UINT c, D3DDISPLAYMODE *d) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->EnumAdapterModes(real_of<IDirect3D9Ex>(self), a, b, c, d);
}
HRESULT STDMETHODCALLTYPE w9ex_GetAdapterDisplayMode(IDirect3D9Ex *self, UINT a, D3DDISPLAYMODE *b) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->GetAdapterDisplayMode(real_of<IDirect3D9Ex>(self), a, b);
}
HRESULT STDMETHODCALLTYPE w9ex_CheckDeviceType(IDirect3D9Ex *self, UINT a, DWORD b, D3DFORMAT c, D3DFORMAT d, BOOL e) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->CheckDeviceType(real_of<IDirect3D9Ex>(self), a, b, c, d, e);
}
HRESULT STDMETHODCALLTYPE w9ex_CheckDeviceFormat(IDirect3D9Ex *self, UINT a, DWORD b, D3DFORMAT c, DWORD d, DWORD e, D3DFORMAT f) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->CheckDeviceFormat(real_of<IDirect3D9Ex>(self), a, b, c, d, e, f);
}
HRESULT STDMETHODCALLTYPE w9ex_CheckDeviceMultiSampleType(IDirect3D9Ex *self, UINT a, DWORD b, D3DFORMAT c, BOOL d, DWORD e, DWORD *f) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->CheckDeviceMultiSampleType(real_of<IDirect3D9Ex>(self), a, b, c, d, e, f);
}
HRESULT STDMETHODCALLTYPE w9ex_CheckDepthStencilMatch(IDirect3D9Ex *self, UINT a, DWORD b, D3DFORMAT c, D3DFORMAT d, D3DFORMAT e) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->CheckDepthStencilMatch(real_of<IDirect3D9Ex>(self), a, b, c, d, e);
}
HRESULT STDMETHODCALLTYPE w9ex_CheckDeviceFormatConversion(IDirect3D9Ex *self, UINT a, DWORD b, D3DFORMAT c, D3DFORMAT d) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->CheckDeviceFormatConversion(real_of<IDirect3D9Ex>(self), a, b, c, d);
}
HRESULT STDMETHODCALLTYPE w9ex_GetDeviceCaps(IDirect3D9Ex *self, UINT a, DWORD b, void *c) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->GetDeviceCaps(real_of<IDirect3D9Ex>(self), a, b, c);
}
HMONITOR STDMETHODCALLTYPE w9ex_GetAdapterMonitor(IDirect3D9Ex *self, UINT a) {
    return real_of<IDirect3D9Ex>(self)->lpVtbl->GetAdapterMonitor(real_of<IDirect3D9Ex>(self), a);
}
HRESULT STDMETHODCALLTYPE w9ex_CreateDeviceEx(IDirect3D9Ex *self, UINT adapter, DWORD dev_type, HWND hwnd,
                                              DWORD flags, D3DPRESENT_PARAMETERS *pp, void *fullscreen_mode,
                                              IDirect3DDevice9 **out);

// ------------------------------------------- IDirect3D9::CreateDevice detour
bool hook_create_device(IDirect3D9 *factory);
bool hook_present(IDirect3DDevice9 *dev);
typedef HRESULT(STDMETHODCALLTYPE *CreateDeviceFn)(IDirect3D9 *, UINT, DWORD, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
CreateDeviceFn g_original_create_device = nullptr;

HRESULT STDMETHODCALLTYPE hooked_CreateDevice(IDirect3D9 *self, UINT adapter, DWORD dev_type, HWND hwnd,
                                              DWORD flags, D3DPRESENT_PARAMETERS *pp,
                                              IDirect3DDevice9 **out) {
    VRLOG("CreateDevice #%ld: adapter=%u type=%lu hwnd=0x%p flags=0x%08lX windowed=%d %ux%u fmt=%lu msaa=%lu/%lu",
          (long)InterlockedIncrement(&g_create_device_calls), adapter, (unsigned long)dev_type, hwnd,
          (unsigned long)flags, pp ? (int)pp->Windowed : -1,
          pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
          pp ? (unsigned long)pp->BackBufferFormat : 0,
          pp ? (unsigned long)pp->MultiSampleType : 0, pp ? (unsigned long)pp->MultiSampleQuality : 0);

    if (!g_original_create_device) {
        VRLOG("CreateDevice: FATAL no original entry point");
        if (out) *out = nullptr;
        return E_FAIL;
    }

    D3DPRESENT_PARAMETERS local = {};
    if (pp) local = *pp;

    // The game's device is created with the plain call, and this is deliberate.
    //
    // 2026-09-25: an attempt was made to create it as a D3D9Ex device instead (an Ex device is the only
    // thing that can create a texture with a shared handle, which the no-readback stereo path needs).
    // Every attempt RAISED inside the real d3d9.dll (`execute at 0x0000E084` - the HWND used as a jump
    // target) whichever swap effect was asked for, and while the raise was caught, runs after it began
    // dying in d3d9.dll itself. The attempt is therefore REMOVED rather than left in as a guarded
    // fallback: a caught structured exception inside the graphics driver is not something to keep doing
    // on every launch for a path that never succeeded once.
    //
    // Consequence, measured and recorded: no D3D9Ex device exists in this process, so shareable
    // (GPU-only) textures are not available and the compositor stays on the readback path.
    HRESULT hr = g_original_create_device(self, adapter, dev_type, hwnd, flags, &local, out);
    VRLOG("CreateDevice: original returned 0x%08lX, device 0x%p", (unsigned long)hr,
          out ? (void *)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        if (local.BackBufferWidth && local.BackBufferHeight) {
            const D3DFORMAT fmt = local.BackBufferFormat ? local.BackBufferFormat : D3DFMT_X8R8G8B8;
            re6vr::set_backbuffer_geometry(local.BackBufferWidth, local.BackBufferHeight, fmt);
            VRLOG("CreateDevice: back buffer %ux%u fmt=%lu recorded for the compositor",
                  local.BackBufferWidth, local.BackBufferHeight, (unsigned long)local.BackBufferFormat);

            // Create the compositor's textures HERE, before the game renders a
            // single frame. Doing this lazily from inside a frame makes MT
            // Framework stop with "ERR09: Unsupported function.".
            g_bridge->prepare(*out, local.BackBufferWidth, local.BackBufferHeight, fmt);
        }
        hook_present(*out);
        if (pp) *pp = local;  // the runtime may have corrected the parameters
    }
    return hr;
}

// Patches slot 16 of the (private) vtable belonging to `factory`.
bool hook_create_device(IDirect3D9 *factory) {
    if (g_create_device_hooked) return true;
    if (!factory || !factory->lpVtbl) return false;

    g_original_create_device = factory->lpVtbl->CreateDevice;
    if (!g_original_create_device) {
        VRLOG("CreateDevice: factory 0x%p has a null CreateDevice slot", (void *)factory);
        return false;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(&factory->lpVtbl->CreateDevice, sizeof(void *), PAGE_READWRITE, &old_protect)) {
        VRLOG("CreateDevice: VirtualProtect failed: %lu", GetLastError());
        return false;
    }
    factory->lpVtbl->CreateDevice = &hooked_CreateDevice;
    VirtualProtect(&factory->lpVtbl->CreateDevice, sizeof(void *), old_protect, &old_protect);

    g_create_device_hooked = true;
    VRLOG("CreateDevice: vtable patched at %p (original %p, detour %p)",
          (void *)&factory->lpVtbl->CreateDevice, (void *)g_original_create_device,
          (void *)&hooked_CreateDevice);
    return true;
}

// Device-lost codes we react to (from d3d9.h, absent in this build environment).
#define D3DERR_DEVICELOST      ((HRESULT)0x88760868L)
#define D3DERR_DEVICENOTRESET  ((HRESULT)0x88760869L)

// ------------------------------------------------- Reset detour
// The reliable recovery point. A fullscreen game changes display mode at start-up
// and comes back through Reset, which also invalidates every D3DPOOL_DEFAULT
// object we own. Watching Present's HRESULT alone is not enough: D3D9 only reports
// D3DERR_DEVICENOTRESET from TestCooperativeLevel, so the compositor used to stay
// paused for the rest of the session and the headset never showed a frame.
typedef HRESULT(STDMETHODCALLTYPE *ResetFn)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
ResetFn g_original_reset = nullptr;

HRESULT STDMETHODCALLTYPE hooked_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
    VRLOG("reset: requested on device 0x%p (%ux%u windowed=%d fmt=%lu)", (void *)dev,
          pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
          pp ? (int)pp->Windowed : -1, pp ? (unsigned long)pp->BackBufferFormat : 0);
    g_bridge->release_surfaces();
    g_bridge->on_device_lost();
    HRESULT hr = g_original_reset(dev, pp);
    VRLOG("reset: original returned 0x%08lX", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        g_bridge->on_device_reset();
        if (pp && pp->BackBufferWidth && pp->BackBufferHeight) {
            g_bridge->prepare(dev, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat);
        }
    }
    return hr;
}

// ------------------------------------------------- EndScene detour
// The back buffer may only be touched between EndScene and Present; doing it
// inside Present faults inside d3d9.dll.
typedef HRESULT(STDMETHODCALLTYPE *EndSceneFn)(IDirect3DDevice9 *);
EndSceneFn g_original_endscene = nullptr;

HRESULT STDMETHODCALLTYPE hooked_EndScene(IDirect3DDevice9 *dev) {
    HRESULT hr = g_original_endscene(dev);

    // The offline checks run HERE rather than in the Present hook, and that placement is the whole
    // point: creating a D3D9 resource from inside Present makes MT Framework stop with
    // "ERR09: Unsupported function.", and the stereo check has to create D3D9 render targets. EndScene
    // is the frame boundary this project already treats as safe for touching surfaces.
    //
    // Order matters as well: the D3D11 device is brought up first here, because the checks below
    // measure D3D9 -> D3D11 copies and would otherwise create a second D3D11 device behind the
    // bridge's back.
    if (!g_no_xr && g_selftest && g_bridge) {
        if (!g_selftest_d11_ready) {
            g_selftest_d11_ready = true;
            g_bridge->prepare_selftest_d3d11();
        }
        if (!g_selftest_done) {
            g_selftest_done = true;
            // The device goes along because the stereo capture check needs a real D3D9 device, and the
            // headset path has not recorded one yet when there is no headset.
            const bool st_ok = g_bridge->run_selftest(dev);
            VRLOG("present: RE6VR_SELFTEST -> %s", st_ok ? "PASS" : "FAIL");
        }
    }

    g_bridge->on_end_scene(dev);
    // A screenshot request is served even when the compositor is not running.
    //
    // It used to be served from inside the compositor's readback loop, which only runs while an
    // OpenXR session is up - so the run of 2026-09-25 00:16, launched without a headset
    // (`xrGetSystem -> FORM_FACTOR_UNAVAILABLE`), produced no pictures at all and the scan's
    // verdict could not be checked. The picture is most needed precisely when the headset is NOT
    // in play, because that is when nobody can see what the game is showing.
    re6vr::screenshot_service_from_device(dev);
    return hr;
}

// ------------------------------------------------- Present detour
typedef HRESULT(STDMETHODCALLTYPE *PresentFn)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
PresentFn g_real_present = nullptr;      // MinHook trampoline
PresentFn g_original_present = nullptr;  // vtable value before hooking
bool      g_no_trampoline = false;       // RE6VR_NO_TRAMPOLINE=1

HRESULT STDMETHODCALLTYPE hooked_Present(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst,
                                         HWND window, const RGNDATA *dirty) {
    const LONG n = InterlockedIncrement(&g_present_calls);
    re6vr::matrix_probe_frame();
    re6vr::view_probe_frame();
    re6vr::cam_steer_frame();
    re6vr::cam_write_test_frame();
    if (n == 1) {
        VRLOG("present: first call on device 0x%p (no_xr=%d, no_trampoline=%d)",
              (void *)dev, (int)g_no_xr, (int)g_no_trampoline);
        VRLOG("present: original=%p vtable_now=%p", (void *)g_original_present,
              (void *)dev->lpVtbl->Present);
        if (g_no_xr) {
            VRLOG("present: RE6VR_NO_XR is set, forwarding without composition");
        } else {
            // Deliberately not on this frame. Bringing OpenXR up blocks for about
            // a second, and a fullscreen D3D9 game is still changing display mode
            // right after its first Present: initialising inside that window
            // stalled the game straight into a device loss. Waiting until the
            // display has settled costs one second of flat rendering instead.
            g_init_not_before = GetTickCount() + 1200;
            VRLOG("present: OpenXR init deferred by 1200 ms (fullscreen mode changes)");
        }
    }

    if (!g_no_xr && g_bridge->state() == re6vr::XrState::NotTried && g_init_not_before != 0 &&
        (LONG)(GetTickCount() - g_init_not_before) >= 0) {
        // The game's device is a plain D3D9 device unless the Ex path was taken;
        // OpenXrBridge queries for IDirect3DDevice9Ex and builds its own Ex device
        // when needed, so this call is safe either way.
        const bool ok = g_bridge->init(g_self, dev);
        VRLOG("present: OpenXR init -> %s (state=%d)", ok ? "ok" : "unavailable",
              (int)g_bridge->state());
    }

    if (!g_no_xr) {
        g_bridge->submit(dev);
    }

    // The saved original runs the real present; no trampoline involved.
    HRESULT hr = g_original_present(dev, src, dst, window, dirty);

    // A fullscreen D3D9 game loses its device during start-up (resolution and
    // window-style changes) and recovers through Reset. While that is happening
    // we must not touch its surfaces: reading a lost device's back buffer faults
    // inside d3d9.dll.
    if (hr == D3DERR_DEVICELOST) {
        g_bridge->release_surfaces();
        g_bridge->on_device_lost();
    } else if (SUCCEEDED(hr) && g_bridge->device_lost()) {
        // D3D9 reports D3DERR_DEVICENOTRESET from TestCooperativeLevel only, never
        // from Present, so the trigger for recovery is the first Present that
        // works again. Waiting for DEVICENOTRESET here left the compositor paused
        // for the rest of the session after the start-up device loss.
        g_bridge->on_device_reset();
    }
    return hr;
}

bool hook_present(IDirect3DDevice9 *dev) {
    if (g_present_hooked) {
        VRLOG("present: device 0x%p already hooked, skipping", (void *)dev);
        return true;
    }

    // Patch the device's *own copy* of the vtable instead of patching code in
    // d3d9.dll. D3D9 gives every device a private vtable copy, so this needs no
    // trampoline and no executable-memory surgery: the original entry is saved
    // and called to perform the real present.
    g_original_present = dev->lpVtbl->Present;
    if (!g_original_present) {
        VRLOG("present: device 0x%p has a null Present slot", (void *)dev);
        return false;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(&dev->lpVtbl->Present, sizeof(void *), PAGE_READWRITE, &old_protect)) {
        VRLOG("present: VirtualProtect failed: %lu", GetLastError());
        return false;
    }
    dev->lpVtbl->Present = &hooked_Present;
    VirtualProtect(&dev->lpVtbl->Present, sizeof(void *), old_protect, &old_protect);

    g_original_endscene = dev->lpVtbl->EndScene;
    if (g_original_endscene &&
        VirtualProtect(&dev->lpVtbl->EndScene, sizeof(void *), PAGE_READWRITE, &old_protect)) {
        dev->lpVtbl->EndScene = &hooked_EndScene;
        VirtualProtect(&dev->lpVtbl->EndScene, sizeof(void *), old_protect, &old_protect);
        VRLOG("endscene: vtable patched (original %p)", (void *)g_original_endscene);
    }

    g_original_reset = dev->lpVtbl->Reset;
    if (g_original_reset &&
        VirtualProtect(&dev->lpVtbl->Reset, sizeof(void *), PAGE_READWRITE, &old_protect)) {
        dev->lpVtbl->Reset = &hooked_Reset;
        VirtualProtect(&dev->lpVtbl->Reset, sizeof(void *), old_protect, &old_protect);
        VRLOG("reset: vtable patched (original %p)", (void *)g_original_reset);
    }

    g_real_present = g_original_present;  // no trampoline needed any more
    VRLOG("present: vtable patched at %p (original %p, detour %p)",
          (void *)&dev->lpVtbl->Present, (void *)g_original_present, (void *)&hooked_Present);

    // Stage 2 reconnaissance: classify the matrices the engine uploads. Read-only,
    // and a no-op unless RE6VR_CAPTURE_VS is set.
    re6vr::matrix_probe_install(dev);

    // The camera the RENDERER uses, taken from the only stream it cannot avoid: the vertex shader
    // constants. It latches the first rigid 4x4 uploaded and then hunts those exact bytes in
    // memory, which is the address to write for head tracking. A no-op unless re6vr_view.txt
    // exists.
    re6vr::view_probe_install(dev);

    // The camera read path found by static analysis: the view is built by 0x5F80B0 from
    // [cam+0x50/0x60/0x70], the camera comes from the slot table of the sBioCamera singleton at
    // [0x186E23C], and mCameraOrg is a dead end (one reader, nothing renders from it). Mode comes
    // from re6vr_steer.txt: observe = log only, swing = rotate the live target and prove the read
    // path by eye.
    re6vr::cam_steer_install(dev);

    // Finding the camera object: captures two camera poses and intersects the addresses
    // that hold both. A no-op unless re6vr_scan.txt exists.
    re6vr::mem_scan_start();

    // Finding the camera object the OTHER way: the class pointer and the verified field
    // offsets, with a geometric check. Answers "is [0x017D270C] the instance" and "is the
    // live sBioCamera's mCameraOrg[0] a real pose". A no-op unless re6vr_cam.txt exists.
    re6vr::mem_cam_start();

    // The no-guessing route: hook the function that WRITES mCameraOrg (identified statically at
    // 0x004FF9B0) and let the engine hand over the object address, the pose it writes and the
    // source group it copies from. A no-op unless re6vr_camhook.txt exists.
    re6vr::cam_hook_start();
    g_present_hooked = true;
    return true;
}

// -------------------------------------------------- CreateDevice hooks
HRESULT STDMETHODCALLTYPE w9_CreateDevice(IDirect3D9 *self, UINT adapter, DWORD dev_type, HWND hwnd,
                                          DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) {
    // Use the module-level factory pointer rather than reading it back out of
    // the wrapper; see the note next to g_real_d3d9.
    IDirect3D9 *real = g_real_d3d9 ? g_real_d3d9 : real_of<IDirect3D9>(self);
    VRLOG("CreateDevice #%ld: adapter=%u type=%lu hwnd=0x%p flags=0x%08lX windowed=%d %ux%u fmt=%lu msaa=%lu/%lu",
          (long)InterlockedIncrement(&g_create_device_calls), adapter, (unsigned long)dev_type, hwnd,
          (unsigned long)flags, pp ? (int)pp->Windowed : -1,
          pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
          pp ? (unsigned long)pp->BackBufferFormat : 0,
          pp ? (unsigned long)pp->MultiSampleType : 0, pp ? (unsigned long)pp->MultiSampleQuality : 0);

    if (!real) {
        VRLOG("CreateDevice: FATAL no real IDirect3D9 available");
        if (out) *out = nullptr;
        return E_FAIL;
    }

    D3DPRESENT_PARAMETERS local = {};
    if (pp) local = *pp;

    VRLOG("CreateDevice: real=%p real_vtbl=%p real_slot16=%p (wrapper self=%p)",
          (void *)real, (void *)real->lpVtbl, (void *)real->lpVtbl->CreateDevice, (void *)self);

    HRESULT hr = real->lpVtbl->CreateDevice(real, adapter, dev_type, hwnd, flags, &local, out);
    VRLOG("CreateDevice: real returned 0x%08lX, device 0x%p", (unsigned long)hr,
          out ? (void *)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        if (local.BackBufferWidth && local.BackBufferHeight) {
            re6vr::set_backbuffer_geometry(local.BackBufferWidth, local.BackBufferHeight,
                                           local.BackBufferFormat ? local.BackBufferFormat
                                                                  : D3DFMT_X8R8G8B8);
            VRLOG("CreateDevice: back buffer %ux%u fmt=%lu recorded for the compositor",
                  local.BackBufferWidth, local.BackBufferHeight, (unsigned long)local.BackBufferFormat);
        }
        hook_present(*out);
        if (pp) *pp = local; // the runtime may have corrected the parameters
        // Ask for D3D9Ex so the back buffer can be shared with D3D11. Adding
        // D3DCREATE_FPU_PRESERVE keeps the engine's FPU state untouched, which
        // MT Framework relies on.
        VRLOG("CreateDevice: suggest EX_VP + FPU_PRESERVE (0x%08lX) if a reset path allows it",
              (unsigned long)((flags | D3DCREATE_FPU_PRESERVE)));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE w9ex_CreateDeviceEx(IDirect3D9Ex *self, UINT adapter, DWORD dev_type, HWND hwnd,
                                              DWORD flags, D3DPRESENT_PARAMETERS *pp, void *fullscreen_mode,
                                              IDirect3DDevice9 **out) {
    IDirect3D9Ex *real = real_of<IDirect3D9Ex>(self);
    VRLOG("CreateDeviceEx #%ld: adapter=%u type=%lu windowed=%d %ux%u fmt=%lu",
          (long)InterlockedIncrement(&g_create_deviceex_calls), adapter, (unsigned long)dev_type,
          pp ? (int)pp->Windowed : -1, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
          pp ? (unsigned long)pp->BackBufferFormat : 0);

    D3DPRESENT_PARAMETERS local = {};
    if (pp) local = *pp;
    HRESULT hr = real->lpVtbl->CreateDeviceEx(real, adapter, dev_type, hwnd, flags, &local,
                                              fullscreen_mode, out);
    VRLOG("CreateDeviceEx -> 0x%08lX, device 0x%p", (unsigned long)hr, out ? (void *)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        if (local.BackBufferWidth && local.BackBufferHeight) {
            re6vr::set_backbuffer_geometry(local.BackBufferWidth, local.BackBufferHeight,
                                           local.BackBufferFormat ? local.BackBufferFormat
                                                                  : D3DFMT_X8R8G8B8);
        }
        hook_present(*out);
        if (pp) *pp = local;
    }
    return hr;
}

void init_wrapped_vtables() {
    g_wrapped9_vtbl.QueryInterface = &w9_QueryInterface;
    g_wrapped9_vtbl.AddRef = &w9_AddRef;
    g_wrapped9_vtbl.Release = &w9_Release;
    g_wrapped9_vtbl.RegisterSoftwareDevice = &w9_RegisterSoftwareDevice;
    g_wrapped9_vtbl.GetAdapterCount = &w9_GetAdapterCount;
    g_wrapped9_vtbl.GetAdapterIdentifier = &w9_GetAdapterIdentifier;
    g_wrapped9_vtbl.GetAdapterModeCount = &w9_GetAdapterModeCount;
    g_wrapped9_vtbl.EnumAdapterModes = &w9_EnumAdapterModes;
    g_wrapped9_vtbl.GetAdapterDisplayMode = &w9_GetAdapterDisplayMode;
    g_wrapped9_vtbl.CheckDeviceType = &w9_CheckDeviceType;
    g_wrapped9_vtbl.CheckDeviceFormat = &w9_CheckDeviceFormat;
    g_wrapped9_vtbl.CheckDeviceMultiSampleType = &w9_CheckDeviceMultiSampleType;
    g_wrapped9_vtbl.CheckDepthStencilMatch = &w9_CheckDepthStencilMatch;
    g_wrapped9_vtbl.CheckDeviceFormatConversion = &w9_CheckDeviceFormatConversion;
    g_wrapped9_vtbl.GetDeviceCaps = &w9_GetDeviceCaps;
    g_wrapped9_vtbl.GetAdapterMonitor = &w9_GetAdapterMonitor;
    g_wrapped9_vtbl.CreateDevice = &w9_CreateDevice;

    // g_wrapped9ex_vtbl is intentionally left uninitialised: see Direct3DCreate9Ex.
    if (false) g_wrapped9ex_vtbl.QueryInterface = &w9ex_QueryInterface;
    g_wrapped9ex_vtbl.AddRef = &w9ex_AddRef;
    g_wrapped9ex_vtbl.Release = &w9ex_Release;
    g_wrapped9ex_vtbl.RegisterSoftwareDevice = &w9ex_RegisterSoftwareDevice;
    g_wrapped9ex_vtbl.GetAdapterCount = &w9ex_GetAdapterCount;
    g_wrapped9ex_vtbl.GetAdapterIdentifier = &w9ex_GetAdapterIdentifier;
    g_wrapped9ex_vtbl.GetAdapterModeCount = &w9ex_GetAdapterModeCount;
    g_wrapped9ex_vtbl.EnumAdapterModes = &w9ex_EnumAdapterModes;
    g_wrapped9ex_vtbl.GetAdapterDisplayMode = &w9ex_GetAdapterDisplayMode;
    g_wrapped9ex_vtbl.CheckDeviceType = &w9ex_CheckDeviceType;
    g_wrapped9ex_vtbl.CheckDeviceFormat = &w9ex_CheckDeviceFormat;
    g_wrapped9ex_vtbl.CheckDeviceMultiSampleType = &w9ex_CheckDeviceMultiSampleType;
    g_wrapped9ex_vtbl.CheckDepthStencilMatch = &w9ex_CheckDepthStencilMatch;
    g_wrapped9ex_vtbl.CheckDeviceFormatConversion = &w9ex_CheckDeviceFormatConversion;
    g_wrapped9ex_vtbl.GetDeviceCaps = &w9ex_GetDeviceCaps;
    g_wrapped9ex_vtbl.GetAdapterMonitor = &w9ex_GetAdapterMonitor;
    g_wrapped9ex_vtbl.CreateDeviceEx = &w9ex_CreateDeviceEx;
}

} // namespace

// ------------------------------------------------------------- exports
extern "C" __declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk_version) {
    if (!g_real_create9) {
        VRLOG("Direct3DCreate9: real entry point missing");
        return nullptr;
    }

    // Synthetic check of the camera probe's search, before anything else touches the scene.
    // A probe whose failure mode is "found nothing" cannot be trusted on the strength of one
    // real run, so its search is validated against an object it must find and one it must
    // reject. Off unless RE6VR_CAM_SELFTEST=1.
    {
        static bool s_selftest_done = false;
        if (!s_selftest_done) {
            s_selftest_done = true;
            char buf[8] = "";
            if (GetEnvironmentVariableA("RE6VR_CAM_SELFTEST", buf, sizeof(buf)) > 0 &&
                buf[0] == '1') {
                VRLOG("cam: selftest requested (RE6VR_CAM_SELFTEST=1)");
                const bool ok = re6vr::mem_cam_selftest();
                VRLOG("cam: selftest returned %s", ok ? "PASS" : "FAIL");
            }
            char shot[8] = "";
            if (GetEnvironmentVariableA("RE6VR_SHOT_SELFTEST", shot, sizeof(shot)) > 0 &&
                shot[0] == '1') {
                const bool ok = re6vr::screenshot_selftest();
                VRLOG("shot: selftest returned %s", ok ? "PASS" : "FAIL");
            }
        }
    }

    IDirect3D9 *real = g_real_create9(sdk_version);
    VRLOG("Direct3DCreate9(%u) -> 0x%p (vtbl 0x%p, CreateDevice slot %p)", sdk_version,
          (void *)real, real ? (void *)real->lpVtbl : nullptr,
          real ? (void *)real->lpVtbl->CreateDevice : nullptr);
    if (!real) return nullptr;

    // Return the *real* object and patch its own vtable copy in place.
    //
    // Wrapping the factory was tried twice and both attempts let the game jump
    // into uninitialised memory (fault offset 0xCCCCCCE0) on its first
    // CreateDevice call, even though the wrapper's bytes and vtable were
    // verified correct from the harness. D3D9 hands every object a private
    // vtable copy, so patching the object we actually return avoids the whole
    // class of problems: the game sees a genuine IDirect3D9 with a genuine
    // vtable, and only slot 16 is ours.
    g_real_d3d9 = real;
    if (!hook_create_device(real)) {
        VRLOG("Direct3DCreate9: could not patch CreateDevice, passing the object through unhooked");
    }
    return real;
}

extern "C" __declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT sdk_version, IDirect3D9Ex **out) {
    // We deliberately do NOT hand out a wrapped IDirect3D9Ex. IDirect3D9Ex has
    // four extra slots beyond IDirect3D9, and a wrapper that only forwards the
    // 17 IDirect3D9 slots makes any Ex-specific call (or a caller that assumes
    // the full 21-slot layout) jump into uninitialised memory. The mod creates
    // its own D3D9Ex factory internally (see OpenXrBridge::ensure_ex_d3d9), and
    // RE6 itself only ever uses Direct3DCreate9, so the safe answer here is to
    // pass the real object straight through.
    if (!g_real_create9ex) {
        VRLOG("Direct3DCreate9Ex: real entry point missing");
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }
    IDirect3D9Ex *real = nullptr;
    HRESULT hr = g_real_create9ex(sdk_version, &real);
    VRLOG("Direct3DCreate9Ex(%u) -> 0x%08lX, real 0x%p (passed through unwrapped)",
          sdk_version, (unsigned long)hr, (void *)real);
    if (out) *out = real;
    return hr;
}

// ------------------------------------------------------------- lifecycle
// ------------------------------------------------- forwarded entry points
// The D3D9 export table is small and frozen. Rather than relying on linker
// forwarders (which are fragile for a 32-bit process loading "System32"), every
// export below is resolved from the real module at load time.
struct RealExports {
    HMODULE module = nullptr;
    FARPROC perf_begin_event = nullptr;
    FARPROC perf_end_event = nullptr;
    FARPROC perf_get_status = nullptr;
    FARPROC perf_query_repeat_frame = nullptr;
    FARPROC perf_set_marker = nullptr;
    FARPROC perf_set_options = nullptr;
    FARPROC perf_set_region = nullptr;
    FARPROC debug_set_level = nullptr;
    FARPROC debug_set_mute = nullptr;
    FARPROC maximized_shim = nullptr;
    FARPROC shader_validator_create9 = nullptr;
    FARPROC psgp_error = nullptr;
    FARPROC psgp_sample_texture = nullptr;
};

RealExports g_fwd;



extern "C" __declspec(dllexport) int WINAPI D3DPERF_BeginEvent(DWORD color, LPCWSTR name) {
    typedef int(WINAPI * Fn)(DWORD, LPCWSTR);
    return g_fwd.perf_begin_event ? ((Fn)g_fwd.perf_begin_event)(color, name) : 0;
}
extern "C" __declspec(dllexport) int WINAPI D3DPERF_EndEvent(void) {
    typedef int(WINAPI * Fn)(void);
    return g_fwd.perf_end_event ? ((Fn)g_fwd.perf_end_event)() : 0;
}
extern "C" __declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus(void) {
    typedef DWORD(WINAPI * Fn)(void);
    return g_fwd.perf_get_status ? ((Fn)g_fwd.perf_get_status)() : 0;
}
extern "C" __declspec(dllexport) BOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
    typedef BOOL(WINAPI * Fn)(void);
    return g_fwd.perf_query_repeat_frame ? ((Fn)g_fwd.perf_query_repeat_frame)() : FALSE;
}
extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetMarker(DWORD color, LPCWSTR name) {
    typedef void(WINAPI * Fn)(DWORD, LPCWSTR);
    if (g_fwd.perf_set_marker) ((Fn)g_fwd.perf_set_marker)(color, name);
}
extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD options) {
    typedef void(WINAPI * Fn)(DWORD);
    if (g_fwd.perf_set_options) ((Fn)g_fwd.perf_set_options)(options);
}
extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetRegion(DWORD color, LPCWSTR name) {
    typedef void(WINAPI * Fn)(DWORD, LPCWSTR);
    if (g_fwd.perf_set_region) ((Fn)g_fwd.perf_set_region)(color, name);
}
extern "C" __declspec(dllexport) void WINAPI DebugSetLevel(DWORD level) {
    typedef void(WINAPI * Fn)(DWORD);
    if (g_fwd.debug_set_level) ((Fn)g_fwd.debug_set_level)(level);
}
extern "C" __declspec(dllexport) void WINAPI DebugSetMute(void) {
    typedef void(WINAPI * Fn)(void);
    if (g_fwd.debug_set_mute) ((Fn)g_fwd.debug_set_mute)();
}
extern "C" __declspec(dllexport) void WINAPI Direct3D9EnableMaximizedWindowedModeShim(BOOL enable) {
    typedef void(WINAPI * Fn)(BOOL);
    if (g_fwd.maximized_shim) ((Fn)g_fwd.maximized_shim)(enable);
}
extern "C" __declspec(dllexport) void *WINAPI Direct3DShaderValidatorCreate9(void) {
    typedef void *(WINAPI * Fn)(void);
    return g_fwd.shader_validator_create9 ? ((Fn)g_fwd.shader_validator_create9)() : nullptr;
}
extern "C" __declspec(dllexport) void WINAPI PSGPError(void) {
    typedef void(WINAPI * Fn)(void);
    if (g_fwd.psgp_error) ((Fn)g_fwd.psgp_error)();
}
extern "C" __declspec(dllexport) void WINAPI PSGPSampleTexture(void) {
    typedef void(WINAPI * Fn)(void);
    if (g_fwd.psgp_sample_texture) ((Fn)g_fwd.psgp_sample_texture)();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    // Raw Win32 tracing from the very first instruction: this must not depend on
    // the CRT being initialised, otherwise a crash before CRT init is invisible.
    {
        HANDLE f = CreateFileW(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Resident Evil 6\\re6vr_early.txt",
                               FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            const char *m = "EARLY: DllMain entered\\r\\n";
            WriteFile(f, m, 26, &w, nullptr);
            FlushFileBuffers(f);
            CloseHandle(f);
        }
    }
#if PROXY_SKIP_ATTACH
    if (reason == DLL_PROCESS_ATTACH) {
        PT_STEP("attach: SKIP variant, nothing else runs");
        return TRUE;
    }
#endif
    if (reason == DLL_PROCESS_ATTACH) {
        PT_STEP("attach: entered DllMain");
        PT_INSTALL_CRASH_REPORTER();
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        PT_STEP("attach: DisableThreadLibraryCalls done");
        vrlog::init(instance);
        PT_STEP("attach: logger ready");
        VRLOG("d3d9 proxy: process attach, module 0x%p, log %ls", (void *)instance, vrlog::path());

        wchar_t system_dir[MAX_PATH] = L"";
        GetSystemDirectoryW(system_dir, MAX_PATH);
        wchar_t real_path[MAX_PATH];
        _snwprintf_s(real_path, MAX_PATH, _TRUNCATE, L"%s\\d3d9.dll", system_dir);

        HMODULE real = LoadLibraryExW(real_path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!real) {
            VRLOG("d3d9 proxy: FATAL cannot load %ls (err %lu)", real_path, GetLastError());
            PT_STEP("attach: FATAL cannot load system d3d9.dll");
            return TRUE; // let the game fail loudly on its own terms
        }
        PT_STEP("attach: system d3d9.dll loaded");
        g_real_create9 = (PFN_Direct3DCreate9)GetProcAddress(real, "Direct3DCreate9");
        g_real_create9ex = (PFN_Direct3DCreate9Ex)GetProcAddress(real, "Direct3DCreate9Ex");
        g_fwd.module = real;
        g_fwd.perf_begin_event = GetProcAddress(real, "D3DPERF_BeginEvent");
        g_fwd.perf_end_event = GetProcAddress(real, "D3DPERF_EndEvent");
        g_fwd.perf_get_status = GetProcAddress(real, "D3DPERF_GetStatus");
        g_fwd.perf_query_repeat_frame = GetProcAddress(real, "D3DPERF_QueryRepeatFrame");
        g_fwd.perf_set_marker = GetProcAddress(real, "D3DPERF_SetMarker");
        g_fwd.perf_set_options = GetProcAddress(real, "D3DPERF_SetOptions");
        g_fwd.perf_set_region = GetProcAddress(real, "D3DPERF_SetRegion");
        g_fwd.debug_set_level = GetProcAddress(real, "DebugSetLevel");
        g_fwd.debug_set_mute = GetProcAddress(real, "DebugSetMute");
        g_fwd.maximized_shim = GetProcAddress(real, "Direct3D9EnableMaximizedWindowedModeShim");
        g_fwd.shader_validator_create9 = GetProcAddress(real, "Direct3DShaderValidatorCreate9");
        g_fwd.psgp_error = GetProcAddress(real, "PSGPError");
        g_fwd.psgp_sample_texture = GetProcAddress(real, "PSGPSampleTexture");
        PT_STEP("attach: exports resolved");
        VRLOG("d3d9 proxy: system d3d9.dll 0x%p, Direct3DCreate9=%p, Direct3DCreate9Ex=%p",
              (void *)real, (void *)g_real_create9, (void *)g_real_create9ex);
        if (!g_real_create9) {
            VRLOG("d3d9 proxy: FATAL system d3d9.dll has no Direct3DCreate9");
            PT_STEP("attach: FATAL no Direct3DCreate9");
            return TRUE;
        }

        {
            wchar_t flag[8] = L"";
            g_no_xr = GetEnvironmentVariableW(L"RE6VR_NO_XR", flag, 8) > 0 && flag[0] == L'1';
            g_no_trampoline = GetEnvironmentVariableW(L"RE6VR_NO_TRAMPOLINE", flag, 8) > 0 &&
                              flag[0] == L'1';
            g_selftest = GetEnvironmentVariableW(L"RE6VR_SELFTEST", flag, 8) > 0 && flag[0] == L'1';
            PT_STEP(g_no_xr ? "attach: RE6VR_NO_XR set, VR disabled"
                            : "attach: VR enabled");
        }


        init_wrapped_vtables();
        PT_STEP("attach: vtables initialised");
        g_bridge = new re6vr::OpenXrBridge();
        // Registered so the camera code can reach the stereo capture path (bridge_capture_eye) without
        // a second pointer of its own: the eye-0 picture has to be taken between the two render passes,
        // which is a moment only the camera hook sees.
        re6vr::set_active_bridge(g_bridge);
        PT_STEP("attach: bridge allocated, ready");
        VRLOG("d3d9 proxy: ready");
    } else if (reason == DLL_PROCESS_DETACH) {
        // This runs inside loader lock: do not call into the runtime here.
        PT_STEP("detach: entered");
        VRLOG("d3d9 proxy: process detach");
        vrlog::shutdown();
    }
    return TRUE;
}
