// openxr_bridge.cpp - OpenXR session, D3D9<->D3D11 interop and quad compositor.
//
// Design notes for stage 1
// ------------------------
// * The game renders with D3D9Ex (we ask for it in the proxy; plain D3D9 has no
//   shareable resources). We create the shared render target with
//   D3D9Ex::CreateTexture(..., D3DUSAGE_RENDERTARGET, ..., hSharedHandle) and
//   open that same surface in D3D11 via OpenSharedResource.
// * Both eyes are submitted through a single XrSwapchain at the runtime's
//   recommended per-eye size.
// * The game frame is drawn onto a world-locked quad. The per-eye projection
//   matrix is derived analytically from the quad rectangle and the eye
//   position, so the panel is geometrically correct (no keystone warp) from any
//   head position, while the eye poses still come from xrLocateViews so the
//   runtime can apply its own IPD / orientation prediction.
// * Stage 2 replaces the quad with real stereo: render the scene twice with the
//   eye view matrices extracted from MT Framework.
#include "openxr_bridge.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "d3d9_min.h"
#include "cam_steer.h"      // cam_steer_live_fov_deg: the fov the camera is really rendering with
#include "log.h"
#include "matrix_probe.h"
#include "mem_scan.h"
#include "screenshot.h"
#include "seh_guard.h"

#ifndef XR_EXT_debug_utils
#define XR_EXT_debug_utils 1
#endif

// xrSetEnvironmentBlendMode's PFN typedef is missing from some published header
// revisions, so declare the shape we need locally.
typedef XrResult(XRAPI_PTR *PFN_xrSetEnvironmentBlendMode_)(XrSession session,
                                                            XrEnvironmentBlendMode mode);
typedef XrResult(XRAPI_PTR *PFN_xrGetD3D11GraphicsRequirementsKHR)(
    XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsD3D11KHR *graphicsRequirements);

namespace re6vr {

// The vertical fov (degrees) submitted to the headset on the last frame. Written by submit(), read by
// the camera code; a plain atomic float is enough because a torn read would only mis-scale one frame's
// fov override.
#include <atomic>

static std::atomic<float> g_submitted_fov_v_deg{0.0f};
static std::atomic<unsigned> g_submitted_frames{0};

float bridge_submitted_fov_v_deg() { return g_submitted_fov_v_deg.load(); }

// "Is a headset being fed frames right now?" - so the camera code can distinguish "no headset this
// run" from "headset up but nothing submitted yet".
bool bridge_submitting() { return g_submitted_frames.load() > 0; }

// When true, the bridge never creates surfaces or touches the game's device
// beyond reading the back buffer pointer. Set from DllMain.
bool g_safe_mode = false;

namespace {
uint32_t  g_backbuffer_w = 1280;
uint32_t  g_backbuffer_h = 720;
D3DFORMAT g_backbuffer_format = 22; // D3DFMT_X8R8G8B8
} // namespace

void set_backbuffer_geometry(uint32_t w, uint32_t h, D3DFORMAT fmt) {
    g_backbuffer_w = w;
    g_backbuffer_h = h;
    g_backbuffer_format = fmt;
}
uint32_t  backbuffer_w() { return g_backbuffer_w; }
uint32_t  backbuffer_h() { return g_backbuffer_h; }
D3DFORMAT backbuffer_format() { return g_backbuffer_format; }

namespace {

// ------------------------------------------------------------------ helpers
const char *xr_result_str(XrResult r) {
    switch (r) {
        case XR_SUCCESS: return "XR_SUCCESS";
        case XR_TIMEOUT_EXPIRED: return "XR_TIMEOUT_EXPIRED";
        case XR_SESSION_LOSS_PENDING: return "XR_SESSION_LOSS_PENDING";
        case XR_EVENT_UNAVAILABLE: return "XR_EVENT_UNAVAILABLE";
        case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
        case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_OUT_OF_MEMORY: return "XR_ERROR_OUT_OF_MEMORY";
        case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
        case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
        case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
        case XR_ERROR_HANDLE_INVALID: return "XR_ERROR_HANDLE_INVALID";
        case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
        case XR_ERROR_SESSION_RUNNING: return "XR_ERROR_SESSION_RUNNING";
        case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
        case XR_ERROR_SESSION_LOST: return "XR_ERROR_SESSION_LOST";
        case XR_ERROR_SYSTEM_INVALID: return "XR_ERROR_SYSTEM_INVALID";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case XR_ERROR_CALL_ORDER_INVALID: return "XR_ERROR_CALL_ORDER_INVALID";
        case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
        case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_PATH_UNSUPPORTED: return "XR_ERROR_PATH_UNSUPPORTED";
        case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
        case XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED: return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
        case XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED: return "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED";
        case XR_ERROR_LAYER_INVALID: return "XR_ERROR_LAYER_INVALID";
        case XR_ERROR_SIZE_INSUFFICIENT: return "XR_ERROR_SIZE_INSUFFICIENT";
        default: return "XR_ERROR_<other>";
    }
}

#define XRCHECK(expr, what)                                                          \
    do {                                                                             \
        XrResult _r = (expr);                                                        \
        if (XR_FAILED(_r)) {                                                         \
            VRLOG("openxr: %s failed: %s (%d)", (what), xr_result_str(_r), (int)_r);  \
            return false;                                                            \
        }                                                                            \
        VRLOG("openxr: %s ok", (what));                                              \
    } while (0)

const char *dxgi_format_name(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "DXGI_FORMAT_B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8X8_UNORM: return "DXGI_FORMAT_B8G8R8X8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "DXGI_FORMAT_R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "DXGI_FORMAT_R16G16B16A16_FLOAT";
        default: return "DXGI_FORMAT_<other>";
    }
}

bool d3d9_format_to_dxgi(D3DFORMAT fmt, DXGI_FORMAT *out) {
    switch (fmt) {
        case D3DFMT_A8R8G8B8: *out = DXGI_FORMAT_B8G8R8A8_UNORM; return true;
        case D3DFMT_X8R8G8B8: *out = DXGI_FORMAT_B8G8R8X8_UNORM; return true;
        case D3DFMT_A16B16G16R16F: *out = DXGI_FORMAT_R16G16B16A16_FLOAT; return true;
        default: return false;
    }
}

} // namespace

// ---------------------------------------------------------------------- Impl
struct OpenXrBridge::Impl {
    // loader
    HMODULE loader_dll = nullptr;

    PFN_xrGetInstanceProcAddr pfn_get_proc = nullptr;
    PFN_xrCreateInstance pfn_create_instance = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties pfn_enum_instance_ext = nullptr;
    PFN_xrGetSystem pfn_get_system = nullptr;
    PFN_xrGetSystemProperties pfn_get_system_props = nullptr;
    PFN_xrEnumerateViewConfigurations pfn_enum_view_configs = nullptr;
    PFN_xrGetViewConfigurationProperties pfn_view_config_props = nullptr;
    PFN_xrEnumerateViewConfigurationViews pfn_enum_view_config_views = nullptr;
    PFN_xrEnumerateEnvironmentBlendModes pfn_enum_blend_modes = nullptr;
    PFN_xrCreateSession pfn_create_session = nullptr;
    PFN_xrDestroySession pfn_destroy_session = nullptr;
    PFN_xrCreateReferenceSpace pfn_create_reference_space = nullptr;
    PFN_xrDestroySpace pfn_destroy_space = nullptr;
    PFN_xrCreateSwapchain pfn_create_swapchain = nullptr;
    PFN_xrDestroySwapchain pfn_destroy_swapchain = nullptr;
    PFN_xrEnumerateSwapchainImages pfn_enum_swapchain_images = nullptr;
    PFN_xrAcquireSwapchainImage pfn_acquire_swapchain_image = nullptr;
    PFN_xrWaitSwapchainImage pfn_wait_swapchain_image = nullptr;
    PFN_xrReleaseSwapchainImage pfn_release_swapchain_image = nullptr;
    PFN_xrLocateViews pfn_locate_views = nullptr;
    PFN_xrWaitFrame pfn_wait_frame = nullptr;
    PFN_xrBeginFrame pfn_begin_frame = nullptr;
    PFN_xrEndFrame pfn_end_frame = nullptr;
    PFN_xrPollEvent pfn_poll_event = nullptr;
    PFN_xrBeginSession pfn_begin_session = nullptr;
    PFN_xrEndSession pfn_end_session = nullptr;
    PFN_xrRequestExitSession pfn_request_exit_session = nullptr;
    PFN_xrSetEnvironmentBlendMode_ pfn_set_blend_mode = nullptr;
    PFN_xrResultToString pfn_result_to_string = nullptr;

    // openxr objects
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local_space = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t sc_width = 0;
    uint32_t sc_height = 0;
    int64_t sc_format = 0;
    uint32_t sc_image_count = 0;

    bool session_running = false;
    bool exit_requested = false;
    XrTime last_display_time = 0;

    XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t view_count = 0;

    // D3D11 bridge
    ID3D11Device *d11_device = nullptr;
    ID3D11DeviceContext *d11_context = nullptr;
    IDXGIAdapter *dxgi_adapter = nullptr;

    // game-side copy objects (rebuilt when the backbuffer changes)
    IDirect3DTexture9 *blit_tex = nullptr;    // small offscreen RT, created up front
    IDirect3DSurface9 *readback = nullptr;     // SYSTEMMEM surface with the same format
    IDirect3DDevice9  *d3d9_owner = nullptr;  // the game's device (not owned)
    uint64_t copy_count = 0;
    // Non-zero = draw the panel in this ARGB colour instead of the game frame
    // (re6vr_test_solid.txt). A solid colour has no internal detail to double, so two
    // copies of the panel in the headset can only mean the compositing path draws it
    // twice - which is a different fault from a duplicated picture.
    uint32_t test_solid_argb = 0;
    // Frame number at which to dump the composite as the game produced it
    // (re6vr_dump_at.txt), 0 = off. A member rather than a local because the dump
    // happens here in copy_backbuffer while the marker is parsed on the submit side.
    uint64_t extra_dump_at = 0;
    // Additional frames, so one run can capture several screens: the title screen is
    // only up briefly and finding it by guessing single frame numbers costs a run
    // each time.
    static const int kExtraDumps = 8;
    uint64_t extra_dump_list[kExtraDumps] = {0};
    int extra_dump_count = 0;
    HANDLE shared_handle = nullptr;
    ID3D11Texture2D *frame_tex = nullptr;     // D3D11 copy of the mirror texture
    IDirect3D9Ex       *ex_d3d9        = nullptr;  // owned Ex factory
    IDirect3DDevice9Ex *ex_owner       = nullptr;  // device owning the mirror texture
    bool                ex_owner_owned = false;
    uint32_t shared_w = 0;        // working size handed to the compositor
    uint32_t shared_h = 0;
    uint32_t shared_src_w = 0;    // size of the back buffer it came from
    uint32_t shared_src_h = 0;
    D3DFORMAT shared_d3d9_format = D3DFMT_UNKNOWN;

    ID3D11Texture2D *shared_d11_tex = nullptr;
    ID3D11RenderTargetView *shared_rtv = nullptr;
    ID3D11ShaderResourceView *shared_srv = nullptr;   // view of the game frame

    // ---------------------------------------------------------------- stereo (one picture per eye)
    //
    // The scene is rendered TWICE per frame (cam_steer's dual-pass hook, re6vr_stereo.txt) and the two
    // pictures have to be kept apart: pass 1's eye-0 frame exists only until pass 2 paints over it.
    // So each eye gets its own copy of the whole capture chain (staging render target -> sysmem
    // readback -> D3D11 texture -> SRV) and the compositor samples the eye's own texture.
    //
    // Everything is created up front, with the single-eye surfaces, because creating a texture from
    // inside a frame makes MT Framework stop with "ERR09: Unsupported function." - the lazy version of
    // this would fail on the very first frame it was needed, which is the worst possible time.
    //
    // When this pair is missing (a device that would not give us a D3D9Ex device, an out of memory),
    // arms_stereo stays false and the compositor keeps the verified single-texture path, so a failure
    // here costs stereo and not the picture.
    IDirect3DTexture9 *eye_blit_tex[2] = {nullptr, nullptr};
    IDirect3DSurface9 *eye_readback[2] = {nullptr, nullptr};
    ID3D11Texture2D *eye_frame_tex[2] = {nullptr, nullptr};
    ID3D11ShaderResourceView *eye_srv[2] = {nullptr, nullptr};
    bool arms_stereo = false;          // both eyes' chains exist -> the capture path is usable
    bool stereo_wanted = false;        // re6vr_stereo.txt was set when the surfaces were created
    uint64_t eye_copy_count[2] = {0, 0};
    unsigned eye_src_w = 0, eye_src_h = 0;   // what the last capture per eye came from
    IDirect3DSurface9 *eye_src_surface[2] = {nullptr, nullptr};
    // One capture at a time, whoever asks. The stereo path is the first thing in this project whose
    // pixel work can arrive from two threads in the same frame (the render thread between the passes,
    // the copy thread at EndScene), and two concurrent StretchRect/GetRenderTargetData paths on one
    // D3D9Ex device is what the 20:19 run died on.
    std::recursive_mutex stereo_lock;
    // Which eye the camera code is rendering this frame (0/1), or -1 when stereo is off. The capture
    // at EndScene copies the frame into THIS eye's texture - the camera code alternates it, so one
    // render pass per frame still fills both eyes over two frames.
    int current_eye = -1;

    // ---------------------------------------------------------------- shared-texture stereo path
    //
    // The readback chain above (StretchRect -> GetRenderTargetData -> LockRect -> D3D11 Map/Unmap ->
    // memcpy) is this mod's most fragile machinery and it is where every crash of 2026-09-25 landed.
    // The established answer, used by every working DX9 VR path, is to keep the frame on the GPU:
    // a D3D9Ex texture created WITH a shared handle, opened in D3D11 by OpenSharedResource, sampled
    // directly by the compositor. No staging surface, no readback, no Map, no memcpy.
    //
    // This path is built ALONGSIDE the readback one and only when it can be built: if any step fails
    // (sharing unsupported, format refused, OpenSharedResource refused) it logs exactly which step and
    // leaves the readback path in charge, so a failure here costs stereo and never the picture.
    IDirect3DTexture9 *eye_shared_tex[2] = {nullptr, nullptr};   // on the private D3D9Ex device
    HANDLE eye_shared_handle[2] = {nullptr, nullptr};            // what D3D11 opens
    IDirect3DTexture9 *eye_shared_local[2] = {nullptr, nullptr}; // the same surface on the GAME device
    ID3D11Texture2D *eye_shared_d11[2] = {nullptr, nullptr};
    ID3D11ShaderResourceView *eye_shared_srv[2] = {nullptr, nullptr};
    bool arms_shared = false;                                    // the whole chain is usable
    uint64_t eye_shared_blit_count[2] = {0, 0};

    // ---------------------------------------------------------------- redirect path (engine-drawn eyes)
    //
    // The measurements of 2026-09-25 21:02 made this possible: the engine draws its whole pass into
    // whatever render target is bound, it binds that target OUTSIDE the render phase, and it never
    // rebinds inside one (0 binds inside the phase across 2686 phases). So binding a texture of our own
    // once, when a pass starts, puts the engine's ENTIRE render into it - the engine does the drawing
    // and we never touch its own binding code.
    //
    // Both passes are redirected (one texture each); the back buffer keeps whatever the last pass left,
    // which is what the monitor shows. Per frame we then copy exactly ONE eye into that eye's compositor
    // texture, alternating frames - so the GPU readback count per frame stays at the value the verified
    // single-texture build has always run at, which is the whole point.
    IDirect3DTexture9 *eye_rt[2] = {nullptr, nullptr};      // the engine renders into these
    IDirect3DSurface9 *eye_rt_surface[2] = {nullptr, nullptr};
    ID3D11Texture2D *eye_stage_tex[2] = {nullptr, nullptr};  // D3D11 side, one per eye
    ID3D11ShaderResourceView *eye_stage_srv[2] = {nullptr, nullptr};
    bool arms_redirect = false;
    uint64_t eye_redirect_copy_count[2] = {0, 0};
    unsigned redirect_copy_toggle = 0;      // which eye's picture is copied this frame
    // virtual screen quad
    ID3D11VertexShader *quad_vs = nullptr;
    ID3D11PixelShader *quad_ps = nullptr;
    ID3D11InputLayout *quad_layout = nullptr;
    ID3D11Buffer *quad_vb = nullptr;
    ID3D11Buffer *quad_cb = nullptr;
    ID3D11SamplerState *quad_sampler = nullptr;
    ID3D11RasterizerState *quad_raster = nullptr;

    std::vector<XrSwapchainImageD3D11KHR> sc_images[2];
    // Two legal submission layouts: one array swapchain holding both eyes as slices
    // (sc_images_shared), or one swapchain per eye. Two separated swapchains are the
    // only way to rule out a runtime mishandling slice selection.
    XrSwapchain swapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool sc_images_shared = true;
    std::vector<ID3D11RenderTargetView *> sc_rtvs;

    // --------------------------------------------------------------- loader
    bool load_loader(HMODULE proxy_module) {
        wchar_t dir[MAX_PATH] = L"";
        GetModuleFileNameW(proxy_module, dir, MAX_PATH);
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) *(slash + 1) = L'\0';

        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%sopenxr_loader.dll", dir);

        // Confine the search to the proxy's own directory: the game folder has
        // no openxr_loader.dll, so this cannot pick up a system copy by accident.
        loader_dll = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!loader_dll) {
            VRLOG("openxr: LoadLibraryExW(%ls) failed: %lu", path, GetLastError());
            return false;
        }
        VRLOG("openxr: loader loaded from %ls", path);

        pfn_get_proc = (PFN_xrGetInstanceProcAddr)GetProcAddress(loader_dll, "xrGetInstanceProcAddr");
        pfn_create_instance = (PFN_xrCreateInstance)GetProcAddress(loader_dll, "xrCreateInstance");
        pfn_enum_instance_ext = (PFN_xrEnumerateInstanceExtensionProperties)GetProcAddress(
            loader_dll, "xrEnumerateInstanceExtensionProperties");
        if (!pfn_get_proc || !pfn_create_instance || !pfn_enum_instance_ext) {
            VRLOG("openxr: loader is missing required entry points (gp=%p ci=%p ee=%p)",
                  (void *)pfn_get_proc, (void *)pfn_create_instance, (void *)pfn_enum_instance_ext);
            return false;
        }
        return true;
    }

    template <typename T>
    bool resolve(T *fn, const char *name) {
        XrResult r = pfn_get_proc(instance, name, reinterpret_cast<PFN_xrVoidFunction *>(fn));
        if (XR_FAILED(r) || !*fn) {
            VRLOG("openxr: xrGetInstanceProcAddr(%s) failed: %s", name, xr_result_str(r));
            return false;
        }
        return true;
    }

    bool resolve_all() {
        bool ok = true;
        ok &= resolve(&pfn_get_system, "xrGetSystem");
        ok &= resolve(&pfn_get_system_props, "xrGetSystemProperties");
        ok &= resolve(&pfn_enum_view_configs, "xrEnumerateViewConfigurations");
        ok &= resolve(&pfn_view_config_props, "xrGetViewConfigurationProperties");
        ok &= resolve(&pfn_enum_view_config_views, "xrEnumerateViewConfigurationViews");
        ok &= resolve(&pfn_enum_blend_modes, "xrEnumerateEnvironmentBlendModes");
        ok &= resolve(&pfn_create_session, "xrCreateSession");
        ok &= resolve(&pfn_destroy_session, "xrDestroySession");
        ok &= resolve(&pfn_create_reference_space, "xrCreateReferenceSpace");
        ok &= resolve(&pfn_destroy_space, "xrDestroySpace");
        ok &= resolve(&pfn_create_swapchain, "xrCreateSwapchain");
        ok &= resolve(&pfn_destroy_swapchain, "xrDestroySwapchain");
        ok &= resolve(&pfn_enum_swapchain_images, "xrEnumerateSwapchainImages");
        ok &= resolve(&pfn_acquire_swapchain_image, "xrAcquireSwapchainImage");
        ok &= resolve(&pfn_wait_swapchain_image, "xrWaitSwapchainImage");
        ok &= resolve(&pfn_release_swapchain_image, "xrReleaseSwapchainImage");
        ok &= resolve(&pfn_locate_views, "xrLocateViews");
        ok &= resolve(&pfn_wait_frame, "xrWaitFrame");
        ok &= resolve(&pfn_begin_frame, "xrBeginFrame");
        ok &= resolve(&pfn_end_frame, "xrEndFrame");
        ok &= resolve(&pfn_poll_event, "xrPollEvent");
        ok &= resolve(&pfn_begin_session, "xrBeginSession");
        ok &= resolve(&pfn_end_session, "xrEndSession");
        ok &= resolve(&pfn_request_exit_session, "xrRequestExitSession");
        resolve(&pfn_set_blend_mode, "xrSetEnvironmentBlendMode");
        resolve(&pfn_result_to_string, "xrResultToString");
        return ok;
    }

    // ---------------------------------------------------------------- D3D11
    // Creates the D3D11 device on the exact adapter the runtime asks for.
    // On a multi-GPU machine D3D11CreateDevice(nullptr, HARDWARE) can pick a
    // different adapter than the runtime wants, and xrCreateSession then fails
    // with XR_ERROR_GRAPHICS_DEVICE_INVALID (-50). Asking OpenXR for the LUID
    // first removes the guesswork.
    // Creates a D3D11 device without consulting the runtime. Used by the offline
    // selftest when no headset is attached.
    bool create_d11_device_offline() {
        if (d11_device) return true;
        UINT flags = 0;
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
        IDXGIAdapter1 *chosen = nullptr;
        IDXGIFactory1 *factory = nullptr;

        // Prefer a hardware adapter from the runtime's LUID if there is one, else
        // just the first non-software adapter.
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) && factory) {
            LUID wanted = {};
            bool have_luid = false;
            PFN_xrGetD3D11GraphicsRequirementsKHR get_reqs = nullptr;
            if (instance && system_id != XR_NULL_SYSTEM_ID &&
                resolve(&get_reqs, "xrGetD3D11GraphicsRequirementsKHR") && get_reqs) {
                XrGraphicsRequirementsD3D11KHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
                if (XR_SUCCEEDED(get_reqs(instance, system_id, &reqs))) {
                    wanted = reqs.adapterLuid;
                    have_luid = true;
                }
            }
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1 *a = nullptr;
                if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
                if (!a) break;
                DXGI_ADAPTER_DESC1 d = {};
                a->GetDesc1(&d);
                const bool match = have_luid && d.AdapterLuid.HighPart == wanted.HighPart &&
                                   d.AdapterLuid.LowPart == wanted.LowPart;
                const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                if (match || (!chosen && !software)) {
                    chosen = a;
                    if (match) break;
                } else {
                    a->Release();
                }
            }
        }

        HRESULT hr = D3D11CreateDevice(chosen,
                                       chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                       &d11_device, &got, &d11_context);
        if (factory) factory->Release();
        if (chosen) chosen->Release();
        if (FAILED(hr) || !d11_device) {
            VRLOG("selftest: D3D11CreateDevice failed: 0x%08lX", (unsigned long)hr);
            return false;
        }
        VRLOG("selftest: D3D11 device up on its own (feature level 0x%04X)", (unsigned)got);
        return true;
    }

    bool create_d11_device() {
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;

        IDXGIAdapter1 *chosen = nullptr;
        LUID wanted = {};
        bool have_luid = false;

        PFN_xrGetD3D11GraphicsRequirementsKHR get_reqs = nullptr;
        if (resolve(&get_reqs, "xrGetD3D11GraphicsRequirementsKHR") && get_reqs) {
            XrGraphicsRequirementsD3D11KHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
            XrResult rr = get_reqs(instance, system_id, &reqs);
            if (XR_SUCCEEDED(rr)) {
                wanted = reqs.adapterLuid;
                have_luid = true;
                VRLOG("openxr: runtime wants adapter luid %08lX:%08lX, min feature level 0x%04X",
                      (unsigned long)wanted.HighPart, (unsigned long)wanted.LowPart,
                      (unsigned)reqs.minFeatureLevel);
                if ((UINT)reqs.minFeatureLevel > (UINT)got) got = reqs.minFeatureLevel;
            } else {
                VRLOG("openxr: xrGetD3D11GraphicsRequirementsKHR failed: %s", xr_result_str(rr));
            }
        }

        IDXGIFactory1 *factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) && factory) {
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1 *adapter = nullptr;
                if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
                if (!adapter) break;

                DXGI_ADAPTER_DESC1 desc = {};
                adapter->GetDesc1(&desc);
                char name[256] = "";
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);

                const bool luid_match = have_luid && desc.AdapterLuid.HighPart == wanted.HighPart &&
                                        desc.AdapterLuid.LowPart == wanted.LowPart;
                const bool is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                VRLOG("openxr: adapter %u '%s' luid %08lX:%08lX software=%d%s",
                      i, name, (unsigned long)desc.AdapterLuid.HighPart,
                      (unsigned long)desc.AdapterLuid.LowPart, (int)is_software,
                      luid_match ? "  <-- runtime's choice" : "");

                if (luid_match) {
                    chosen = adapter;  // keep the reference
                    break;
                }
                if (!chosen && !is_software) {
                    chosen = adapter;
                    break;
                }
                adapter->Release();
            }
            // Drop any non-chosen references held by the factory.
            if (chosen && have_luid &&
                !( [&] {
                    DXGI_ADAPTER_DESC1 d = {};
                    chosen->GetDesc1(&d);
                    return d.AdapterLuid.HighPart == wanted.HighPart &&
                           d.AdapterLuid.LowPart == wanted.LowPart;
                }())) {
                // chosen fell back to a non-matching adapter: still usable, but
                // the session will likely fail, so say so plainly.
                VRLOG("openxr: no adapter matches the runtime LUID, using the fallback adapter");
            }
        }

        HRESULT hr = E_FAIL;
        for (int attempt = 0; attempt < 2; ++attempt) {
            hr = D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &d11_device, &got, &d11_context);
            if (hr == E_INVALIDARG && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
                VRLOG("openxr: D3D11 debug device rejected, retrying without the debug layer");
                flags &= ~D3D11_CREATE_DEVICE_DEBUG;
                continue;
            }
            break;
        }
        if (factory) factory->Release();
        if (chosen) chosen->Release();

        if (FAILED(hr) || !d11_device) {
            VRLOG("openxr: D3D11CreateDevice failed: 0x%08lX", (unsigned long)hr);
            return false;
        }

        IDXGIDevice *dxgi_dev = nullptr;
        if (SUCCEEDED(d11_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_dev)) && dxgi_dev) {
            dxgi_dev->GetAdapter(&dxgi_adapter);
            dxgi_dev->Release();
            if (dxgi_adapter) {
                DXGI_ADAPTER_DESC desc = {};
                dxgi_adapter->GetDesc(&desc);
                char name[256] = "";
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                VRLOG("openxr: D3D11 device on '%s' (feature level 0x%04X, luid %08lX:%08lX)",
                      name, (unsigned)got, (unsigned long)desc.AdapterLuid.HighPart,
                      (unsigned long)desc.AdapterLuid.LowPart);
            }
        }
        return true;
    }

    bool create_instance(HMODULE proxy_module) {
        uint32_t ext_count = 0;
        if (XR_FAILED(pfn_enum_instance_ext(nullptr, 0, &ext_count, nullptr))) {
            VRLOG("openxr: xrEnumerateInstanceExtensionProperties failed");
            return false;
        }
        std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
        if (XR_FAILED(pfn_enum_instance_ext(nullptr, ext_count, &ext_count, exts.data()))) {
            VRLOG("openxr: extension enumeration failed");
            return false;
        }
        bool has_d3d11 = false;
        bool has_depth_layer = false;
        for (auto &e : exts) {
            if (strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) has_d3d11 = true;
            if (strcmp(e.extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME) == 0) {
                has_depth_layer = true;
            }
        }
        VRLOG("openxr: %u instance extensions, XR_KHR_d3d11_enable=%s, "
              "XR_KHR_composition_layer_depth=%s", ext_count, has_d3d11 ? "yes" : "NO",
              has_depth_layer ? "yes" : "NO");
        // The list is printed in full because it decides which stereo techniques exist at all. The
        // depth-layer extension matters most: with it the RUNTIME does the per-eye reprojection from ONE
        // picture plus its depth buffer, which needs neither a second render pass nor a second eye
        // texture - the two things that crashed eight runs on 2026-09-25.
        for (uint32_t i = 0; i < ext_count; ++i) {
            VRLOG("openxr:   extension[%u] %s", i, exts[i].extensionName);
        }
        if (!has_d3d11) {
            VRLOG("openxr: runtime does not expose D3D11 binding, cannot compose");
            return false;
        }

        const char *enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        XrApplicationInfo app = {};
        strcpy_s(app.applicationName, "RE6 VR Mod");
        app.applicationVersion = 1;
        strcpy_s(app.engineName, "MT Framework (injected)");
        app.engineVersion = 1;
        app.apiVersion = XR_MAKE_VERSION(1, 0, 0);

        XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
        ci.next = nullptr;
        ci.createFlags = 0;
        ci.applicationInfo = app;
        ci.enabledApiLayerCount = 0;
        ci.enabledApiLayerNames = nullptr;
        ci.enabledExtensionCount = 1;
        ci.enabledExtensionNames = enabled;

        XRCHECK(pfn_create_instance(&ci, &instance), "xrCreateInstance");
        if (instance == XR_NULL_HANDLE) return false;
        resolve(&pfn_result_to_string, "xrResultToString");
        return resolve_all();
    }

    bool create_session_and_swapchain() {
        XrSystemGetInfo sys_info = {XR_TYPE_SYSTEM_GET_INFO};
        sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrResult r = pfn_get_system(instance, &sys_info, &system_id);
        if (XR_FAILED(r)) {
            VRLOG("openxr: xrGetSystem failed: %s (is a headset connected and the runtime running?)",
                  xr_result_str(r));
            return false;
        }
        VRLOG("openxr: system id %llu", (unsigned long long)system_id);

        XrSystemProperties props = {XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(pfn_get_system_props(instance, system_id, &props))) {
            VRLOG("openxr: system '%s' vendor=%u maxSwapchain=%ux%u maxLayers=%u",
                  props.systemName, props.vendorId,
                  props.graphicsProperties.maxSwapchainImageWidth,
                  props.graphicsProperties.maxSwapchainImageHeight,
                  props.graphicsProperties.maxLayerCount);
        }

        // View configuration must be stereo projection.
        uint32_t cfg_count = 0;
        if (XR_FAILED(pfn_enum_view_configs(instance, system_id, 0, &cfg_count, nullptr)) || cfg_count == 0) {
            VRLOG("openxr: no view configurations");
            return false;
        }
        std::vector<XrViewConfigurationType> cfgs(cfg_count);
        pfn_enum_view_configs(instance, system_id, cfg_count, &cfg_count, cfgs.data());
        bool has_stereo = false;
        for (auto c : cfgs) {
            if (c == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) has_stereo = true;
        }
        if (!has_stereo) {
            VRLOG("openxr: runtime has no PRIMARY_STEREO view configuration");
            return false;
        }

        XrViewConfigurationProperties vc_props = {XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
        if (XR_SUCCEEDED(pfn_view_config_props(instance, system_id,
                                               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, &vc_props))) {
            VRLOG("openxr: view config fov mutable=%d", (int)vc_props.fovMutable);
        }

        view_count = 2;
        for (uint32_t i = 0; i < 2; ++i) {
            views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
            views[i].next = nullptr;
        }
        if (XR_FAILED(pfn_enum_view_config_views(instance, system_id,
                                                 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                 2, &view_count, views))) {
            VRLOG("openxr: xrEnumerateViewConfigurationViews failed");
            return false;
        }
        VRLOG("openxr: %u views, recommended %ux%u per eye, max %ux%u",
              view_count,
              views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight,
              views[0].maxImageRectWidth, views[0].maxImageRectHeight);

        if (!create_d11_device()) return false;

        XrGraphicsBindingD3D11KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = d11_device;

        XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
        sci.next = &binding;
        sci.createFlags = 0;
        sci.systemId = system_id;
        XRCHECK(pfn_create_session(instance, &sci, &session), "xrCreateSession");

        // LOCAL space: eye poses are relative to wherever the player recentred.
        XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rsci.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
        XRCHECK(pfn_create_reference_space(instance, &rsci, &local_space), "xrCreateReferenceSpace(LOCAL)");

        // Optional: pick an environment blend mode the runtime actually accepts.
        uint32_t blend_count = 0;
        if (XR_SUCCEEDED(pfn_enum_blend_modes(instance, system_id,
                                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blend_count, nullptr)) &&
            blend_count > 0) {
            std::vector<XrEnvironmentBlendMode> modes(blend_count);
            if (XR_SUCCEEDED(pfn_enum_blend_modes(instance, system_id,
                                                  XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                  blend_count, &blend_count, modes.data()))) {
                if (pfn_set_blend_mode) {
                    XrResult br = pfn_set_blend_mode(session, modes[0]);
                    VRLOG("openxr: blend modes=%u, set first -> %s", blend_count, xr_result_str(br));
                }
            }
        }

        // One swapchain, one image per eye - or two independent swapchains.
        //
        // The array layout is what the spec recommends ("array textures: both eyes live
        // in one XrSwapchain as separate array layers") and every modern runtime is
        // expected to handle it. But it is the one part of the submission path that
        // could silently give both eyes the same slice, and that failure looks exactly
        // like what the headset has been showing: each eye's image is individually
        // correct, but the two are offset from each other, and nothing about the
        // render parameters changes it.
        //
        // re6vr_two_swapchains.txt switches to the other legal layout: arraySize = 1,
        // one swapchain per eye, imageArrayIndex always 0. If the doubling disappears
        // here, the array indexing was the fault.
        bool two_swapchains = false;
        {
            const wchar_t *log_path = vrlog::path();
            if (log_path && log_path[0]) {
                wchar_t marker[MAX_PATH] = L"";
                wcsncpy_s(marker, MAX_PATH, log_path, _TRUNCATE);
                wchar_t *slash = wcsrchr(marker, L'\\');
                if (slash) {
                    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - marker)),
                             L"re6vr_two_swapchains.txt");
                    two_swapchains = GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
                }
            }
        }
        sc_format = (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM;
        sc_width = views[0].recommendedImageRectWidth;
        sc_height = views[0].recommendedImageRectHeight;

        XrSwapchainCreateInfo swci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swci.createFlags = 0;
        swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        swci.format = sc_format;
        swci.sampleCount = 1;
        swci.width = sc_width;
        swci.height = sc_height;
        swci.faceCount = 1;
        swci.arraySize = two_swapchains ? 1 : view_count;
        swci.mipCount = 1;

        // Optional swapchain size override: RE6VR_SWAPCHAIN_W / RE6VR_SWAPCHAIN_H
        // (0 or unset keeps the runtime's recommendation for that axis).
        //
        // The recommended 1996x2148 per eye covers the whole eye (~110 x 96 deg),
        // but only the panel is ever drawn, and it subtends about 60 x 36 deg. The
        // slice therefore carries pixels for angles that are never submitted. Sizing
        // it to the submitted angles at the runtime's OWN pixel density keeps the
        // sharpness identical and removes the waste:
        //
        //     998 * 59.8/110 = 545       2148 * 35.8/96 = 800
        //
        // i.e. about 0.44 M pixels instead of 2.14 M for the same pixels per degree.
        // Note this is not "shrink until it looks soft": at 561 rows the vertical
        // density would drop to match the horizontal one (~16 px/deg), which is below
        // the runtime's own recommendation for this panel size.
        {
            wchar_t wbuf[32] = L"", hbuf[32] = L"";
            bool set_w = GetEnvironmentVariableW(L"RE6VR_SWAPCHAIN_W", wbuf, 32) > 0;
            bool set_h = GetEnvironmentVariableW(L"RE6VR_SWAPCHAIN_H", hbuf, 32) > 0;
            // Marker file, because Steam does not pass the shell's environment on and
            // this is a switch that has to be testable in a real game run. Contents:
            // "<width> <height>", with height 0 meaning "match the 16:9 picture".
            if (!set_w && !set_h) {
                const wchar_t *log_path = vrlog::path();
                if (log_path && log_path[0]) {
                    wchar_t marker[MAX_PATH] = L"";
                    wcsncpy_s(marker, MAX_PATH, log_path, _TRUNCATE);
                    wchar_t *slash = wcsrchr(marker, L'\\');
                    if (slash) {
                        wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - marker)),
                                 L"re6vr_swapchain.txt");
                        if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) {
                            FILE *f = _wfopen(marker, L"r");
                            long mw = 0, mh = 0;
                            if (f) {
                                if (fscanf_s(f, "%ld %ld", &mw, &mh) < 1) { mw = 0; mh = 0; }
                                fclose(f);
                            }
                            if (mw > 0) {
                                _snwprintf_s(wbuf, _countof(wbuf), _TRUNCATE, L"%ld", mw);
                                _snwprintf_s(hbuf, _countof(hbuf), _TRUNCATE, L"%ld", mh);
                                set_w = true;
                                set_h = true;
                                VRLOG("openxr: swapchain override from %ls: %ld x %ld "
                                      "(height 0 = match 16:9)", marker, mw, mh);
                            }
                        }
                    }
                }
            }
            if (set_w || set_h) {
                uint32_t new_w = set_w ? (uint32_t)_wtoi(wbuf) : sc_width;
                uint32_t new_h = set_h ? (uint32_t)_wtoi(hbuf) : sc_height;
                // "Match the picture" (height 0) must be resolved BEFORE the minimum
                // clamp below. It was placed after it and the height was clamped to 64
                // first, so a request for 1996x0 asking for a 16:9 swapchain produced
                // 1996x64 - a swapchain 33x shorter than intended, which made the whole
                // aspect-ratio experiment meaningless while the log still looked
                // plausible. Clamps and derived values have to be ordered deliberately.
                if (set_h && new_h == 0) {
                    new_h = (uint32_t)((uint64_t)new_w * 9 / 16);
                    VRLOG("openxr: swapchain height 0 -> matching the 16:9 picture: "
                          "using %ux%u (aspect %.3f)", new_w, new_h,
                          new_h ? (float)new_w / (float)new_h : 0.0f);
                }
                if (new_w < 64) new_w = 64;
                if (new_h < 64) new_h = 64;
                if (new_w > views[0].maxImageRectWidth) new_w = views[0].maxImageRectWidth;
                if (new_h > views[0].maxImageRectHeight) new_h = views[0].maxImageRectHeight;
                VRLOG("openxr: swapchain size override -> %ux%u (recommended %ux%u, max %ux%u)",
                      new_w, new_h, views[0].recommendedImageRectWidth,
                      views[0].recommendedImageRectHeight, views[0].maxImageRectWidth,
                      views[0].maxImageRectHeight);
                sc_width = new_w;
                sc_height = new_h;
                swci.width = sc_width;
                swci.height = sc_height;
            }
        }

        // Create the swapchain(s). With arraySize = view_count there is one swapchain
        // holding both eyes as array slices (the layout the spec recommends). With
        // two_swapchains there is one arraySize=1 swapchain per eye instead, which is
        // the other legal layout and the one that cannot be affected by a runtime
        // mishandling slice selection.
        uint32_t img_count = 0;
        sc_rtvs.assign(64, nullptr);
        if (two_swapchains) {
            for (uint32_t eye = 0; eye < view_count && eye < 2; ++eye) {
                XRCHECK(pfn_create_swapchain(session, &swci, &swapchains[eye]),
                        "xrCreateSwapchain(per eye)");
            }
            swapchain = swapchains[0];
            XRCHECK(pfn_enum_swapchain_images(swapchains[0], 0, &img_count, nullptr),
                    "xrEnumerateSwapchainImages(count)");
            for (uint32_t eye = 0; eye < view_count && eye < 2; ++eye) {
                sc_images[eye].assign(img_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                for (auto &img : sc_images[eye]) {
                    img.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
                    img.next = nullptr;
                }
                XRCHECK(pfn_enum_swapchain_images(
                            swapchains[eye], img_count, &img_count,
                            reinterpret_cast<XrSwapchainImageBaseHeader *>(sc_images[eye].data())),
                        "xrEnumerateSwapchainImages(per eye)");
                for (uint32_t i = 0; i < img_count; ++i) {
                    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
                    rtv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    HRESULT rtv_hr = d11_device->CreateRenderTargetView(
                        sc_images[eye][i].texture, &rtv_desc, &sc_rtvs[(size_t)eye * img_count + i]);
                    if (FAILED(rtv_hr)) {
                        VRLOG("openxr: RTV(image %u, eye %u) failed: 0x%08lX", i, eye,
                              (unsigned long)rtv_hr);
                        sc_rtvs[(size_t)eye * img_count + i] = nullptr;
                    }
                }
            }
            sc_images_shared = false;
            VRLOG("openxr: TWO swapchains, %ux%u x%u images each (one per eye), format=%s",
                  sc_width, sc_height, img_count, dxgi_format_name((DXGI_FORMAT)sc_format));
        } else {
            XRCHECK(pfn_create_swapchain(session, &swci, &swapchain), "xrCreateSwapchain");
            swapchains[0] = swapchain;
            XRCHECK(pfn_enum_swapchain_images(swapchain, 0, &img_count, nullptr),
                    "xrEnumerateSwapchainImages(count)");
            sc_images[0].assign(img_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            for (auto &img : sc_images[0]) {
                img.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
                img.next = nullptr;
            }
            XRCHECK(pfn_enum_swapchain_images(
                        swapchain, img_count, &img_count,
                        reinterpret_cast<XrSwapchainImageBaseHeader *>(sc_images[0].data())),
                    "xrEnumerateSwapchainImages");

            // One render target view per swapchain image *per eye*. The swapchain is
            // an array of view_count slices and xrEndFrame submits both of them with
            // imageArrayIndex = 0/1, so a view that only ever targets slice 0 leaves
            // the other eye on whatever the runtime handed over - a black right eye,
            // which is exactly what a single-view render target view produces.
            for (uint32_t i = 0; i < img_count; ++i) {
                for (uint32_t eye = 0; eye < view_count; ++eye) {
                    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
                    rtv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    rtv_desc.Texture2DArray.MipSlice = 0;
                    rtv_desc.Texture2DArray.FirstArraySlice = eye;
                    rtv_desc.Texture2DArray.ArraySize = 1;
                    HRESULT rtv_hr = d11_device->CreateRenderTargetView(
                        sc_images[0][i].texture, &rtv_desc, &sc_rtvs[(size_t)i * view_count + eye]);
                    if (FAILED(rtv_hr)) {
                        VRLOG("openxr: CreateRenderTargetView(image %u, eye %u) failed: 0x%08lX", i,
                              eye, (unsigned long)rtv_hr);
                        sc_rtvs[(size_t)i * view_count + eye] = nullptr;
                    }
                }
            }
            sc_images_shared = true;
            VRLOG("openxr: swapchain %ux%u x%u images, format=%s", sc_width, sc_height, img_count,
                  dxgi_format_name((DXGI_FORMAT)sc_format));
        }
        sc_image_count = img_count;
        return true;
    }

    bool create_quad_pipeline() {
        // Full-screen-style textured quad. Vertices are in *quad local* space
        // with the panel facing +Z; UVs follow D3D convention (v=0 at the top).
        // A four-vertex strip must alternate along the quad, not go round it:
        // (TL,TR,BL,BR) gives the triangles (TL,TR,BL) and (TR,BL,BR), which cover
        // the whole panel. Going round the quad - (TL,TR,BR,BL) - gives
        // (TL,TR,BR) and (TR,BR,BL), which share the TR-BR edge and leave the
        // triangle {TL,BL,centre} unpainted. That gap was rendered as a black notch
        // in the left half of the screen.
        struct V { float x, y, z, u, v; };
        const V verts[4] = {
            {-1.0f, +1.0f, 0.0f, 0.0f, 0.0f}, // top-left
            {+1.0f, +1.0f, 0.0f, 1.0f, 0.0f}, // top-right
            {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f}, // bottom-left
            {+1.0f, -1.0f, 0.0f, 1.0f, 1.0f}, // bottom-right
        };

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(verts);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = verts;
        if (FAILED(d11_device->CreateBuffer(&bd, &sd, &quad_vb))) {
            VRLOG("openxr: quad vertex buffer creation failed");
            return false;
        }

        static const char *kVs = R"(
cbuffer QuadCB : register(b0) {
    float4x4 gViewProj;
    float4   gScaleBias;   // xy = scale, zw = bias, applied to the local quad
    float4   gUvShift;     // xy = texture-space shift of the sampled picture
    float4   gRotZoom;     // x = roll about the panel centre (radians), y = zoom
};
struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut main(VSIn i) {
    VSOut o;
    // The quad's local space already has its origin at the panel centre (the
    // vertex buffer is -1..1 scaled by the half extents), so a roll about the
    // centre is just a rotation here - no offset needed, and none of the sideways
    // displacement that rotating about an eye's own axis would introduce.
    float3 local = float3(i.pos.xy * gScaleBias.xy, i.pos.z);
    float sn, cs;
    sincos(gRotZoom.x, sn, cs);
    local.xy *= gRotZoom.y;
    local.xy = float2(local.x * cs - local.y * sn, local.x * sn + local.y * cs);
    o.pos = mul(float4(local, 1.0), gViewProj);
    o.uv  = i.uv + gUvShift.xy;
    return o;
}
)";
        static const char *kPs = R"(
Texture2D    gGame   : register(t0);
SamplerState gSampler : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return float4(gGame.Sample(gSampler, uv).rgb, 1.0);
}
)";

        ID3DBlob *vs_blob = nullptr;
        ID3DBlob *ps_blob = nullptr;
        ID3DBlob *err = nullptr;
        HRESULT hr = D3DCompile(kVs, strlen(kVs), "quad_vs", nullptr, nullptr, "main", "vs_4_0", 0, 0,
                                &vs_blob, &err);
        if (FAILED(hr)) {
            VRLOG("openxr: quad VS compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
            if (err) err->Release();
            return false;
        }
        hr = D3DCompile(kPs, strlen(kPs), "quad_ps", nullptr, nullptr, "main", "ps_4_0", 0, 0,
                        &ps_blob, &err);
        if (FAILED(hr)) {
            VRLOG("openxr: quad PS compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
            vs_blob->Release();
            if (err) err->Release();
            return false;
        }

        d11_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &quad_vs);
        d11_device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &quad_ps);

        D3D11_INPUT_ELEMENT_DESC il[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        d11_device->CreateInputLayout(il, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &quad_layout);
        vs_blob->Release();
        ps_blob->Release();

        D3D11_BUFFER_DESC cbd = {};
        cbd.ByteWidth = 112; // 64 matrix + 16 scale/bias + 16 uv shift + 16 rot/zoom
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        d11_device->CreateBuffer(&cbd, nullptr, &quad_cb);

        D3D11_SAMPLER_DESC sdesc = {};
        sdesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sdesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sdesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sdesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sdesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sdesc.MaxLOD = D3D11_FLOAT32_MAX;
        d11_device->CreateSamplerState(&sdesc, &quad_sampler);

        D3D11_RASTERIZER_DESC rdesc = {};
        rdesc.FillMode = D3D11_FILL_SOLID;
        rdesc.CullMode = D3D11_CULL_NONE;
        rdesc.DepthClipEnable = TRUE;
        d11_device->CreateRasterizerState(&rdesc, &quad_raster);

        return quad_vs && quad_ps && quad_layout && quad_cb && quad_sampler && quad_raster;
    }

    // ------------------------------------------------------------ D3D9Ex
    // D3D9Ex is only needed to create a *shareable* texture. The texture is
    // created on an Ex device that we own; the game's own device stays a plain
    // D3D9 device, and the copy is done inside the game's device (same-device
    // StretchRect is the only blit path that does not fault on this driver).
    bool ensure_ex_d3d9() {
        VRLOG("openxr: ensure_ex_d3d9 on impl 0x%p (ex_d3d9=%p)", (void *)this, (void *)ex_d3d9);
        if (ex_d3d9) return true;
        HMODULE real = LoadLibraryExW(L"d3d9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!real) {
            VRLOG("openxr: cannot load the system d3d9.dll");
            return false;
        }
        auto create_ex = (PFN_Direct3DCreate9Ex)GetProcAddress(real, "Direct3DCreate9Ex");
        if (!create_ex) {
            VRLOG("openxr: system d3d9.dll has no Direct3DCreate9Ex");
            return false;
        }
        HRESULT hr = create_ex(D3D_SDK_VERSION, &ex_d3d9);
        VRLOG("openxr: Direct3DCreate9Ex -> 0x%08lX", (unsigned long)hr);
        return ex_d3d9 != nullptr;
    }

    IDirect3DDevice9Ex *acquire_ex_device(IDirect3DDevice9 *game_dev) {
        VRLOG("openxr: acquire_ex_device on impl 0x%p (ex_owner=%p)", (void *)this, (void *)ex_owner);
        if (ex_owner) return ex_owner;
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        game_dev->lpVtbl->GetCreationParameters(game_dev, &cp);

        D3DPRESENT_PARAMETERS pp = {};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        // A hidden window rather than the game's own: attaching a second device
        // to the game's focus window confuses the runtime's swap chain handling.
        static HWND s_helper_window = nullptr;
        if (!s_helper_window) {
            s_helper_window = CreateWindowExW(0, L"STATIC", L"re6vr helper", WS_POPUP,
                                              0, 0, 16, 16, nullptr, nullptr,
                                              GetModuleHandleW(nullptr), nullptr);
            const DWORD win_err = s_helper_window ? 0 : GetLastError();
            if (!s_helper_window) s_helper_window = GetDesktopWindow();
            VRLOG("openxr: helper window for the private device: 0x%p (CreateWindowExW %s, err %lu)",
                  (void *)s_helper_window, win_err ? "FAILED" : "ok", (unsigned long)win_err);
        }
        pp.hDeviceWindow = s_helper_window;
        pp.BackBufferWidth = 16;
        pp.BackBufferHeight = 16;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferCount = 1;
        pp.EnableAutoDepthStencil = FALSE;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        // CreateDeviceEx is the only reliable way to obtain an Ex device here:
        // querying IDirect3DDevice9Ex out of a plain device returns E_NOINTERFACE
        // on this runtime, while an Ex-created device accepts the shared-handle
        // texture creation below.
        //
        // NOTE (measured 2026-09-25 20:38): this call CRASHES inside the real d3d9.dll
        // (null write at `call [eax+4]`) when the process already has a device created through this
        // proxy's wrapped factory - reproducible in the offline harness, not yet observed in the game.
        // A structured-exception guard turns that from "the process dies" into "the Ex device is
        // unavailable", because everything that needs it is optional: the readback path stays in
        // charge and the picture is unaffected.
        IDirect3DDevice9 *created = nullptr;
        VRLOG("openxr: calling CreateDeviceEx on 0x%p (adapter %u, hwnd 0x%p, flags 0x%08lX)",
              (void *)ex_d3d9, cp.AdapterOrdinal, pp.hDeviceWindow, (unsigned long)cp.BehaviorFlags);
        HRESULT hr = E_FAIL;
        bool raised = false;
        __try {
            hr = ex_d3d9->lpVtbl->CreateDeviceEx(ex_d3d9, cp.AdapterOrdinal, D3DDEVTYPE_HAL,
                                                 pp.hDeviceWindow,
                                                 cp.BehaviorFlags | D3DCREATE_FPU_PRESERVE,
                                                 &pp, nullptr, &created);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            raised = true;
        }
        if (raised) {
            VRLOG("openxr: CreateDeviceEx RAISED inside d3d9 - the private D3D9Ex device is not "
                  "available in this process. Shareable (GPU-only) textures need it, so the compositor "
                  "stays on the readback path; nothing else is affected.");
            created = nullptr;
            return nullptr;
        }
        if (FAILED(hr) || !created) {
            VRLOG("openxr: private CreateDeviceEx failed: 0x%08lX", (unsigned long)hr);
            return nullptr;
        }
        ex_owner = reinterpret_cast<IDirect3DDevice9Ex *>(created);
        ex_owner_owned = true;
        VRLOG("openxr: private D3D9Ex device created via CreateDeviceEx: 0x%p", (void *)ex_owner);
        VRLOG("openxr: created a private D3D9Ex device for the shareable mirror texture");
        return ex_owner;
    }

    // NOTE: cross-device blits were the earlier crash; see copy_backbuffer.

    // ------------------------------------------------------------- per frame
    void pump_events() {
        XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
        while (pfn_poll_event && pfn_poll_event(instance, &ev) == XR_SUCCESS) {
            switch (ev.type) {
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    auto *sc = reinterpret_cast<XrEventDataSessionStateChanged *>(&ev);
                    VRLOG("openxr: session state -> %d", (int)sc->state);
                    switch (sc->state) {
                        case XR_SESSION_STATE_READY: {
                            if (session_running) {
                                // READY can arrive again after a STOPPING cycle; only
                                // one begin_session may be outstanding at a time.
                                VRLOG("openxr: READY while already running, ignoring");
                                break;
                            }
                            XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                            bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                            XrResult r = pfn_begin_session(session, &bi);
                            session_running = XR_SUCCEEDED(r);
                            VRLOG("openxr: xrBeginSession -> %s (running=%d)", xr_result_str(r),
                                  (int)session_running);
                            break;
                        }
                        case XR_SESSION_STATE_STOPPING:
                            if (session_running) {
                                VRLOG("openxr: xrEndSession");
                                pfn_end_session(session);
                                session_running = false;
                            }
                            break;
                        case XR_SESSION_STATE_FOCUSED:
                            // Taking the headset off and putting it back on should
                            // bring the screen back in front rather than leaving it
                            // wherever it was last placed.
                            recenter_panel();
                            break;
                        case XR_SESSION_STATE_EXITING:
                        case XR_SESSION_STATE_LOSS_PENDING:
                            exit_requested = true;
                            session_running = false;
                            break;
                        default:
                            break;
                    }
                    break;
                }
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    VRLOG("openxr: instance loss pending");
                    exit_requested = true;
                    break;
                default:
                    break;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    // The game calls EndScene and Present from different threads (the log shows
    // the copy on one thread and the submit on another), and a D3D11 immediate
    // context is not thread safe: the copy's Map/Unmap can otherwise overlap the
    // compositor's Draw, and a device Reset can free the frame texture from under a
    // draw that is already using it. Recursive because the surface helpers call
    // each other.
    std::recursive_mutex d11_lock;

    // Which surface the panel reads, decided once and explained once. GetDesc only
    // became usable after the surface vtable slot was fixed, and it is what settles
    // the question the copy loop cannot answer on its own: is the source really the
    // 3840x2160 back buffer, or a smaller render target that happens to be blank?
    bool source_logged = false;
    bool use_back_buffer = false;

    void note_source_surfaces(IDirect3DSurface9 *rt, IDirect3DSurface9 *back) {
        if (source_logged) return;
        source_logged = true;

        D3DSURFACE_DESC rd = {}, bd = {};
        const bool rt_desc = rt && SUCCEEDED(rt->lpVtbl->GetDesc(rt, &rd));
        const bool back_desc = back && SUCCEEDED(back->lpVtbl->GetDesc(back, &bd));
        VRLOG("compositor: render target %p%s %ux%u, back buffer %p%s %ux%u (device says %ux%u)",
              (void *)rt, rt_desc ? "" : " (no desc),", rt_desc ? rd.Width : 0,
              rt_desc ? rd.Height : 0, (void *)back, back_desc ? "" : " (no desc),",
              back_desc ? bd.Width : 0, back_desc ? bd.Height : 0, backbuffer_w(), backbuffer_h());

        // THE DEPTH BUFFER. Whether one exists decides a stereo technique that needs no second render at
        // all: hand the runtime ONE picture plus its depth buffer (XR_KHR_composition_layer_depth) and it
        // performs the per-eye reprojection itself. That is the VorpX-class approach and it is the only
        // route left that does not render the scene twice - the thing that crashed every run of
        // 2026-09-25. So this is measured rather than assumed: ask the device for its depth-stencil
        // surface and report its format, size and multisampling.
        {
            IDirect3DSurface9 *depth = nullptr;
            if (d3d9_owner && SUCCEEDED(d3d9_owner->lpVtbl->GetDepthStencilSurface(
                                  d3d9_owner, reinterpret_cast<void **>(&depth))) &&
                depth) {
                D3DSURFACE_DESC dd = {};
                if (SUCCEEDED(depth->lpVtbl->GetDesc(depth, &dd))) {
                    VRLOG("compositor: DEPTH BUFFER present: %ux%u fmt=%lu (D24S8=75, D16=80, D32F=82) "
                          "usage=0x%08lX pool=%lu multisample=%lu/%lu -> %s",
                          dd.Width, dd.Height, (unsigned long)dd.Format, (unsigned long)dd.Usage,
                          (unsigned long)dd.Pool, (unsigned long)dd.MultiSampleType,
                          (unsigned long)dd.MultiSampleQuality,
                          (dd.Usage & 0x00000002u) ? "NOT readable by us (D3DUSAGE_DEPTHSTENCIL only)"
                                                   : "readable");
                } else {
                    VRLOG("compositor: DEPTH BUFFER present but GetDesc failed");
                }
                depth->lpVtbl->Release(depth);
            } else {
                VRLOG("compositor: DEPTH BUFFER absent - GetDepthStencilSurface returned nothing, so no "
                      "depth-based stereo technique is available in this frame");
            }
        }

        // Only deviate from the current render target when the evidence says it is
        // not the frame: a different size from the back buffer, or unusable.
        use_back_buffer = (rt == nullptr) || (rt_desc && (rd.Width != backbuffer_w() ||
                                                         rd.Height != backbuffer_h()));
        if (use_back_buffer) {
            VRLOG("compositor: the current render target is not the back buffer size - "
                  "copying the back buffer instead");
        }
    }

    IDirect3DSurface9 *pick_source(IDirect3DSurface9 *rt, IDirect3DSurface9 *back) {
        if (use_back_buffer) return back ? back : rt;
        return rt ? rt : back;
    }

    // Allocates every D3D9 surface the compositor needs. Called once per device,
    // never from inside a frame: MT Framework stops with
    // "ERR09: Unsupported function." when a texture is created mid-frame, which
    // is why nothing here may be lazy.
    bool create_device_surfaces(IDirect3DDevice9 *dev, uint32_t w, uint32_t h, D3DFORMAT fmt) {
        std::lock_guard<std::recursive_mutex> lock(d11_lock);
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

        // The staging target is what the game frame is downscaled to on the GPU, and
        // its size is the single biggest sharpness decision in the whole pipeline:
        // the eye image is built from it, so anything thrown away here can never come
        // back.
        //
        // It used to be hard-coded to 1280x720, which meant a 3840x2160 frame lost
        // ~89% of its pixels and the compositor then *upscaled* 720p into a ~1996-wide
        // eye texture. That is what "the picture is blurry" was: not the FOV, not the
        // projection - the frame was being thrown away before it ever reached the
        // headset.
        //
        // The default now follows the eye image (so the readback carries about as many
        // pixels as the eye texture can show), never exceeding the source resolution.
        // Measured cost of the readback at this size is acceptable: 4.89 ms/frame at
        // 3.7 MB/frame, and the copy ran at ~117 fps.
        d3d9_owner = dev;
        HRESULT hr = S_OK;

        uint32_t want_w = 0;
        {
            wchar_t buf[32] = L"";
            if (GetEnvironmentVariableW(L"RE6VR_CAPTURE_W", buf, 32) > 0) {
                want_w = (uint32_t)_wtoi(buf);
            } else {
                const wchar_t *log_path = vrlog::path();
                if (log_path && log_path[0]) {
                    wchar_t marker[MAX_PATH] = L"";
                    wcsncpy_s(marker, MAX_PATH, log_path, _TRUNCATE);
                    wchar_t *slash = wcsrchr(marker, L'\\');
                    if (slash) {
                        wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - marker)),
                                 L"re6vr_capture_w.txt");
                        if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) {
                            FILE *f = _wfopen(marker, L"r");
                            long v = 0;
                            if (f) {
                                if (fscanf_s(f, "%ld", &v) == 1) want_w = (uint32_t)v;
                                fclose(f);
                            }
                        }
                    }
                }
            }
            // 0 = automatic: match the eye image width.
            if (want_w == 0) want_w = sc_width ? sc_width : 1280;
            if (want_w < 320) want_w = 320;
            if (want_w > w) want_w = w;          // never upscale the game's own frame
        }
        const uint32_t work_w = want_w;
        const uint32_t work_h = (w && work_w != w) ? (uint32_t)((uint64_t)h * work_w / w) : h;
        VRLOG("openxr: capture staging %ux%u from a %ux%u back buffer (%.0f%% of the "
              "source pixels)", work_w, work_h, w, h,
              100.0 * ((double)work_w * work_h) / ((double)w * h ? (double)w * h : 1.0));

        hr = dev->lpVtbl->CreateTexture(dev, work_w, work_h, 1, D3DUSAGE_RENDERTARGET, fmt,
                                        D3DPOOL_DEFAULT,
                                        reinterpret_cast<void **>(&blit_tex), nullptr);
        if (FAILED(hr) || !blit_tex) {
            VRLOG("openxr: CreateTexture(%ux%u fmt %lu) failed: 0x%08lX", work_w, work_h,
                  (unsigned long)fmt, (unsigned long)hr);
            blit_tex = nullptr;
            return false;
        }

        hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, work_w, work_h, fmt, D3DPOOL_SYSTEMMEM,
                                                      reinterpret_cast<void **>(&readback), nullptr);
        if (FAILED(hr) || !readback) {
            VRLOG("openxr: CreateOffscreenPlainSurface(%ux%u fmt %lu) failed: 0x%08lX", work_w, work_h,
                  (unsigned long)fmt, (unsigned long)hr);
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

        // Until the first frame is copied in the panel would be pure black, which
        // in the headset is indistinguishable from "composition never ran". A dark
        // grey fill instead reads as "the compositor is up and waiting for pixels",
        // which is the difference between two very different investigations.
        {
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(d11_context->Map(frame_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) &&
                m.pData) {
                for (uint32_t y = 0; y < work_h; ++y) {
                    uint32_t *row = reinterpret_cast<uint32_t *>((uint8_t *)m.pData +
                                                                 (size_t)y * m.RowPitch);
                    for (uint32_t x = 0; x < work_w; ++x) row[x] = 0xFF202020u;  // BGRA dark grey
                }
                d11_context->Unmap(frame_tex, 0);
            }
        }

        shared_w = work_w;
        shared_h = work_h;
        shared_src_w = w;
        shared_src_h = h;
        shared_d3d9_format = fmt;
        VRLOG("openxr: compositor target %ux%u for a %ux%u fmt=%lu back buffer (created up front)",
              work_w, work_h, w, h, (unsigned long)fmt);

        // One-off probe: can this machine read a render target back at all?
        // Everything else in the compositor depends on the answer, and finding
        // out here (once, before any frame) is far cheaper than discovering it
        // per frame.
        probe_readback(dev);
        create_stereo_surfaces(dev, work_w, work_h, fmt);
        // The GPU-only path, built after the readback one so a failure here is reported as what it is
        // (an interop that refused) rather than hiding behind the readback chain's success.
        create_shared_stereo_surfaces(dev, work_w, work_h, fmt);
        // The redirect path: the engine's two passes are sent into our own textures (see
        // create_redirect_surfaces). Independent of the shared path, which is unavailable on this
        // machine - the compositor prefers whichever is armed.
        create_redirect_surfaces(dev, work_w, work_h, fmt);
        // The device is up and every surface that belongs to it exists: captures may run again. The
        // flag can only be cleared HERE, because clearing it earlier would let a capture touch surfaces
        // that do not exist yet - which is the failure this device-loss window caused.
        note_standdown_clear();
        return true;
    }

    // Builds the second copy of the capture chain, for the right eye, so that both of the frame's two
    // renders survive to the compositor. Called from create_device_surfaces, i.e. before any frame has
    // been drawn: MT Framework refuses operations that create resources mid-frame, so this is the only
    // safe moment. Failure is reported and non-fatal - the compositor then keeps the single-texture
    // path it has been running all along.
    void create_stereo_surfaces(IDirect3DDevice9 *dev, uint32_t w, uint32_t h, D3DFORMAT fmt) {
        VRLOG("compositor: stereo surface check: d11=%p device=%p requested=%ux%u fmt=%lu",
              (void *)d11_device, (void *)dev, w, h, (unsigned long)fmt);
        if (!d11_device || !d11_context) {
            VRLOG("compositor: stereo surfaces NOT created - the D3D11 device is not up (with no "
                  "headset this is expected; in a game run it means the session never came up)");
            return;
        }
        // re6vr_stereo.txt decides whether the effort is worth making. Read here rather than lazily:
        // the whole point is that nothing may be created later.
        stereo_wanted = false;
        if (const wchar_t *log_path = vrlog::path()) {
            if (log_path[0]) {
                wchar_t marker[MAX_PATH] = L"";
                wcsncpy_s(marker, MAX_PATH, log_path, _TRUNCATE);
                if (wchar_t *slash = wcsrchr(marker, L'\\')) {
                    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - marker)),
                             L"re6vr_stereo.txt");
                    FILE *f = _wfopen(marker, L"r");
                    if (f) {
                        long v = 0;
                        if (fscanf_s(f, "%ld", &v) == 1 && v != 0) stereo_wanted = true;
                        fclose(f);
                    }
                }
            }
        }
        if (!stereo_wanted) {
            VRLOG("compositor: stereo surfaces NOT created (re6vr_stereo.txt is not 1) - the single "
                  "game-frame texture stays in use, which is the verified path");
            return;
        }

        // Two chains, not one: index 0 is pass 1's frame and index 1 is pass 2's. They cannot share,
        // because pass 2 writes its own picture over the back buffer that pass 1's was copied from.
        for (int i = 0; i < 2; ++i) {
            HRESULT hr = dev->lpVtbl->CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, fmt,
                                                    D3DPOOL_DEFAULT,
                                                    reinterpret_cast<void **>(&eye_blit_tex[i]),
                                                    nullptr);
            if (FAILED(hr) || !eye_blit_tex[i]) {
                VRLOG("compositor: stereo eye %d CreateTexture(%ux%u) failed: 0x%08lX - stereo off",
                      i, w, h, (unsigned long)hr);
                release_stereo_surfaces();
                return;
            }
            hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, w, h, fmt, D3DPOOL_SYSTEMMEM,
                                                          reinterpret_cast<void **>(&eye_readback[i]),
                                                          nullptr);
            if (FAILED(hr) || !eye_readback[i]) {
                VRLOG("compositor: stereo eye %d CreateOffscreenPlainSurface failed: 0x%08lX - "
                      "stereo off", i, (unsigned long)hr);
                release_stereo_surfaces();
                return;
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
            if (FAILED(d11_device->CreateTexture2D(&td, nullptr, &eye_frame_tex[i])) ||
                !eye_frame_tex[i] ||
                FAILED(d11_device->CreateShaderResourceView(eye_frame_tex[i], nullptr, &eye_srv[i])) ||
                !eye_srv[i]) {
                VRLOG("compositor: stereo eye %d D3D11 texture/SRV failed - stereo off", i);
                release_stereo_surfaces();
                return;
            }
            // Same dark grey as the single-texture path: black in the headset is indistinguishable
            // from "composition never ran", grey reads as "waiting for pixels".
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(d11_context->Map(eye_frame_tex[i], 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) &&
                m.pData) {
                for (uint32_t y = 0; y < h; ++y) {
                    uint32_t *row = reinterpret_cast<uint32_t *>((uint8_t *)m.pData +
                                                                 (size_t)y * m.RowPitch);
                    for (uint32_t x = 0; x < w; ++x) row[x] = 0xFF202020u;
                }
                d11_context->Unmap(eye_frame_tex[i], 0);
            }
        }
        arms_stereo = true;
        capture_standdown = false;
        VRLOG("compositor: STEREO capture armed - one %ux%u frame texture per eye, created up front. "
              "Eye 0's picture is taken between the two render passes (it exists only until pass 2 "
              "paints over it), eye 1's at EndScene. Submitting: each eye samples its own texture.",
              w, h);
    }

    void release_stereo_surfaces() {
        for (int i = 0; i < 2; ++i) {
            if (eye_srv[i]) { eye_srv[i]->Release(); eye_srv[i] = nullptr; }
            if (eye_frame_tex[i]) { eye_frame_tex[i]->Release(); eye_frame_tex[i] = nullptr; }
            if (eye_blit_tex[i]) { eye_blit_tex[i]->Release(); eye_blit_tex[i] = nullptr; }
            if (eye_readback[i]) { eye_readback[i]->lpVtbl->Release(eye_readback[i]); eye_readback[i] = nullptr; }
            // These are borrowed references (GetRenderTarget/GetBackBuffer results).
            if (eye_src_surface[i]) { eye_src_surface[i]->lpVtbl->Release(eye_src_surface[i]); eye_src_surface[i] = nullptr; }
            eye_copy_count[i] = 0;
        }
        arms_stereo = false;
    }

    // Builds the GPU-only stereo path: two D3D9Ex textures WITH shared handles, opened both in D3D11
    // and on the game's device. Called from create_device_surfaces, because nothing may be created
    // mid-frame.
    //
    // Every step is logged with its HRESULT: the whole point of this path is that it is SIMPLER than
    // the readback chain, so if it cannot be built the log must name the single step that refused
    // rather than saying "stereo did not work".
    void create_shared_stereo_surfaces(IDirect3DDevice9 *game_dev, uint32_t w, uint32_t h,
                                       D3DFORMAT fmt) {
        if (!stereo_wanted) {
            VRLOG("compositor: SHARED stereo path not built (re6vr_stereo.txt is not 1)");
            return;
        }
        if (!d11_device) {
            VRLOG("compositor: SHARED stereo path not built (no D3D11 device)");
            return;
        }
        if (!game_dev) {
            VRLOG("compositor: SHARED stereo path not built (no game device this run)");
            return;
        }
        // Which device can create a texture with a shared handle? Only a D3D9Ex one - and this process
        // has none, so the shared path cannot be built at all.
        //
        // 2026-09-25, measured the hard way: the game's device is created with the plain
        // Direct3D9::CreateDevice (the proxy deliberately does not attempt the Ex call any more - see
        // hooked_CreateDevice), and manufacturing a SECOND D3D9Ex device in this process crashes inside
        // the real d3d9.dll. Both routes were tried and both faulted, so the private-device fallback
        // that used to be here is REMOVED rather than left in as a guarded attempt: every launch that
        // reached it raised a structured exception inside the graphics driver.
        //
        // What remains is the honest check: if the game's device ever is an Ex device (a wrapper change,
        // a different runtime), the path builds itself; otherwise it says so and the readback path -
        // which is the one that has always been in use - stays in charge.
        IDirect3DDevice9 *ex_dev = nullptr;
        if (game_dev->lpVtbl->QueryInterface(game_dev, D3D9_IID(IDirect3DDevice9Ex),
                                             (void **)&ex_dev) == S_OK && ex_dev) {
            ex_dev->lpVtbl->Release(ex_dev);        // the pointer is only used to prove the interface
            ex_dev = game_dev;
            VRLOG("compositor: the game's device IS a D3D9Ex device - shared textures are created on it");
        } else {
            VRLOG("compositor: SHARED stereo path not built - the game's device is plain D3D9 and this "
                  "process has no way to create a D3D9Ex one (measured: both routes fault inside d3d9). "
                  "The readback path stays in charge, which is the path every working run has used.");
            return;
        }
        const D3DFORMAT shared_fmt = fmt;

        for (int eye = 0; eye < 2; ++eye) {
            HRESULT hr = ex_dev->lpVtbl->CreateTexture(ex_dev, w, h, 1, D3DUSAGE_RENDERTARGET,
                                                       shared_fmt, D3DPOOL_DEFAULT,
                                                       reinterpret_cast<void **>(
                                                           &eye_shared_tex[eye]),
                                                       &eye_shared_handle[eye]);
            if (FAILED(hr) || !eye_shared_tex[eye] || !eye_shared_handle[eye]) {
                VRLOG("compositor: SHARED stereo path unavailable - CreateTexture(shared) for eye "
                      "%d failed: 0x%08lX. Staying on the readback path.", eye, (unsigned long)hr);
                release_shared_stereo_surfaces();
                return;
            }
            // The blit is issued on the game's device, so that device needs the same surface. When it
            // IS the Ex device that created it, the texture pointer itself is enough (the device sees
            // its own texture); otherwise the game has to open the shared handle.
            if (ex_dev == game_dev) {
                eye_shared_tex[eye]->lpVtbl->AddRef(eye_shared_tex[eye]);
                eye_shared_local[eye] = eye_shared_tex[eye];
            } else {
                hr = game_dev->lpVtbl->CreateTexture(game_dev, w, h, 1, D3DUSAGE_RENDERTARGET,
                                                     shared_fmt, D3DPOOL_DEFAULT,
                                                     reinterpret_cast<void **>(
                                                         &eye_shared_local[eye]),
                                                     &eye_shared_handle[eye]);
                if (FAILED(hr) || !eye_shared_local[eye]) {
                    VRLOG("compositor: SHARED stereo path unavailable - the game's device could not open "
                          "the shared surface for eye %d: 0x%08lX. Staying on the readback path.",
                          eye, (unsigned long)hr);
                    release_shared_stereo_surfaces();
                    return;
                }
            }
            hr = d11_device->OpenSharedResource(eye_shared_handle[eye], __uuidof(ID3D11Texture2D),
                                                reinterpret_cast<void **>(&eye_shared_d11[eye]));
            if (FAILED(hr) || !eye_shared_d11[eye]) {
                VRLOG("compositor: SHARED stereo path unavailable - D3D11 OpenSharedResource for eye %d "
                      "failed: 0x%08lX. Staying on the readback path.", eye, (unsigned long)hr);
                release_shared_stereo_surfaces();
                return;
            }
            hr = d11_device->CreateShaderResourceView(eye_shared_d11[eye], nullptr,
                                                      &eye_shared_srv[eye]);
            if (FAILED(hr) || !eye_shared_srv[eye]) {
                VRLOG("compositor: SHARED stereo path unavailable - CreateShaderResourceView for eye %d "
                      "failed: 0x%08lX. Staying on the readback path.", eye, (unsigned long)hr);
                release_shared_stereo_surfaces();
                return;
            }
        }

        arms_shared = true;
        VRLOG("compositor: SHARED stereo path ARMED - two %ux%u fmt=%lu D3D9Ex textures with shared "
              "handles (0x%p, 0x%p), opened on the game's device and in D3D11. Each eye's finished "
              "frame is blitted into its own texture and sampled directly: no readback, no lock, no "
              "CPU copy - the frame never leaves the GPU.", w, h, (unsigned long)shared_fmt,
              (void *)eye_shared_handle[0], (void *)eye_shared_handle[1]);
    }

    void release_shared_stereo_surfaces() {
        for (int i = 0; i < 2; ++i) {
            if (eye_shared_srv[i]) { eye_shared_srv[i]->Release(); eye_shared_srv[i] = nullptr; }
            if (eye_shared_d11[i]) { eye_shared_d11[i]->Release(); eye_shared_d11[i] = nullptr; }
            if (eye_shared_local[i]) { eye_shared_local[i]->Release(); eye_shared_local[i] = nullptr; }
            if (eye_shared_tex[i]) { eye_shared_tex[i]->Release(); eye_shared_tex[i] = nullptr; }
            eye_shared_handle[i] = nullptr;
            eye_shared_blit_count[i] = 0;
        }
        arms_shared = false;
    }

    // ---------------------------------------------------------------- redirect path
    //
    // Builds the two textures the engine's passes are redirected into, plus the D3D11 side the
    // compositor samples. Called from create_device_surfaces, because nothing may be created mid-frame.
    // Failure is reported and non-fatal: the compositor then falls back to the plain single-texture
    // path, which is the one every working run has used.
    void create_redirect_surfaces(IDirect3DDevice9 *game_dev, uint32_t w, uint32_t h, D3DFORMAT fmt) {
        if (!stereo_wanted) {
            VRLOG("compositor: redirect stereo path not built (re6vr_stereo.txt is not 1)");
            return;
        }
        if (!game_dev || !d11_device) {
            VRLOG("compositor: redirect stereo path not built (game device %p, d11 %p)",
                  (void *)game_dev, (void *)d11_device);
            return;
        }
        for (int eye = 0; eye < 2; ++eye) {
            HRESULT hr = game_dev->lpVtbl->CreateTexture(game_dev, w, h, 1, D3DUSAGE_RENDERTARGET, fmt,
                                                         D3DPOOL_DEFAULT,
                                                         reinterpret_cast<void **>(&eye_rt[eye]),
                                                         nullptr);
            if (FAILED(hr) || !eye_rt[eye]) {
                VRLOG("compositor: redirect path unavailable - CreateTexture for eye %d failed: "
                      "0x%08lX (the compositor stays on the single-texture path)", eye,
                      (unsigned long)hr);
                release_redirect_surfaces();
                return;
            }
            hr = eye_rt[eye]->lpVtbl->GetSurfaceLevel(eye_rt[eye], 0, &eye_rt_surface[eye]);
            if (FAILED(hr) || !eye_rt_surface[eye]) {
                VRLOG("compositor: redirect path unavailable - GetSurfaceLevel for eye %d failed: "
                      "0x%08lX", eye, (unsigned long)hr);
                release_redirect_surfaces();
                return;
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
            if (FAILED(d11_device->CreateTexture2D(&td, nullptr, &eye_stage_tex[eye])) ||
                !eye_stage_tex[eye] ||
                FAILED(d11_device->CreateShaderResourceView(eye_stage_tex[eye], nullptr,
                                                            &eye_stage_srv[eye])) ||
                !eye_stage_srv[eye]) {
                VRLOG("compositor: redirect path unavailable - D3D11 texture/SRV for eye %d failed", eye);
                release_redirect_surfaces();
                return;
            }
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(d11_context->Map(eye_stage_tex[eye], 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) &&
                m.pData) {
                for (uint32_t y = 0; y < h; ++y) {
                    uint32_t *row = reinterpret_cast<uint32_t *>((uint8_t *)m.pData +
                                                                 (size_t)y * m.RowPitch);
                    for (uint32_t x = 0; x < w; ++x) row[x] = 0xFF202020u;
                }
                d11_context->Unmap(eye_stage_tex[eye], 0);
            }
        }
        arms_redirect = true;
        VRLOG("compositor: REDIRECT stereo path ARMED - two %ux%u render targets (%p, %p). The engine's "
              "two passes are redirected into them by binding, so the engine draws both eyes and the "
              "compositor copies ONE eye per frame (alternating), keeping the per-frame readback at the "
              "single-texture level.", w, h, (void *)eye_rt_surface[0], (void *)eye_rt_surface[1]);
    }

    void release_redirect_surfaces() {
        for (int i = 0; i < 2; ++i) {
            if (eye_stage_srv[i]) { eye_stage_srv[i]->Release(); eye_stage_srv[i] = nullptr; }
            if (eye_stage_tex[i]) { eye_stage_tex[i]->Release(); eye_stage_tex[i] = nullptr; }
            if (eye_rt_surface[i]) {
                eye_rt_surface[i]->lpVtbl->Release(eye_rt_surface[i]);
                eye_rt_surface[i] = nullptr;
            }
            if (eye_rt[i]) { eye_rt[i]->Release(); eye_rt[i] = nullptr; }
            eye_redirect_copy_count[i] = 0;
        }
        arms_redirect = false;
    }

    // The surface the engine's pass for this eye should be redirected into (or null when the path is
    // not armed). The camera code's SetRenderTarget detour calls this - it must not touch anything but
    // the pointer.
    IDirect3DSurface9 *redirect_target(int eye) {
        if (!arms_redirect || eye < 0 || eye > 1) return nullptr;
        return eye_rt_surface[eye];
    }

    // One eye's engine-drawn picture -> the compositor texture for that eye. Same three hops as the
    // verified single-texture path, from a source WE own instead of the device's current target, which
    // is what makes it safe to run at EndScene only.
    bool copy_eye_redirect(IDirect3DDevice9 *dev, int eye) {
        if (!arms_redirect || eye < 0 || eye > 1 || !dev) return false;
        if (capture_standdown) return false;
        if (!eye_rt_surface[eye] || !eye_stage_tex[eye] || !blit_tex || !readback) return false;
        IDirect3DSurface9 *staging = nullptr;
        if (FAILED(blit_tex->lpVtbl->GetSurfaceLevel(blit_tex, 0, &staging)) || !staging) {
            warn_once("GetSurfaceLevel(redirect staging)");
            return false;
        }
        bool ok = false;
        {
            std::lock_guard<std::recursive_mutex> guard(stereo_lock);
            HRESULT hr = dev->lpVtbl->StretchRect(dev, eye_rt_surface[eye], nullptr, staging, nullptr,
                                                  D3DTEXF_NONE);
            if (FAILED(hr)) {
                hr = dev->lpVtbl->StretchRect(dev, eye_rt_surface[eye], nullptr, staging, nullptr,
                                              D3DTEXF_LINEAR);
                static bool s_warned = false;
                if (!s_warned) {
                    s_warned = true;
                    VRLOG("compositor: REDIRECT StretchRect(eye target -> staging) -> 0x%08lX %s",
                          (unsigned long)hr, SUCCEEDED(hr) ? "(LINEAR worked)" : "- blit failed");
                }
            }
            if (SUCCEEDED(hr)) {
                hr = dev->lpVtbl->GetRenderTargetData(dev, staging, readback);
                if (FAILED(hr)) {
                    warn_once_hr("GetRenderTargetData(redirect staging -> sysmem)", hr);
                } else {
                    D3DLOCKED_RECT locked = {};
                    if (SUCCEEDED(readback->lpVtbl->LockRect(readback, &locked, nullptr,
                                                             D3DLOCK_READONLY)) &&
                        locked.pBits) {
                        std::lock_guard<std::recursive_mutex> d11(d11_lock);
                        D3D11_MAPPED_SUBRESOURCE mapped = {};
                        if (SUCCEEDED(d11_context->Map(eye_stage_tex[eye], 0,
                                                       D3D11_MAP_WRITE_DISCARD, 0, &mapped)) &&
                            mapped.pData) {
                            for (uint32_t y = 0; y < shared_h; ++y) {
                                memcpy((uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch,
                                       (const uint8_t *)locked.pBits + (size_t)y * locked.Pitch,
                                       (size_t)shared_w * 4);
                            }
                            d11_context->Unmap(eye_stage_tex[eye], 0);
                            ok = true;
                        } else {
                            warn_once_hr("Map(redirect eye texture)", hr);
                        }
                        readback->lpVtbl->UnlockRect(readback);
                    } else {
                        warn_once_hr("LockRect(redirect readback)", hr);
                    }
                }
            }
            staging->lpVtbl->Release(staging);
        }
        if (ok) {
            const uint64_t n = ++eye_redirect_copy_count[eye];
            if (n <= 2 || (n % 300) == 0) {
                VRLOG("compositor: REDIRECT eye %d picture %llu copied (%ux%u from the engine's own "
                      "render)", eye, (unsigned long long)n, shared_w, shared_h);
            }
        }
        return ok;
    }

    // The per-frame operation of the shared path: blit the frame the device currently holds into the
    // given eye's texture. Same placement and the same call shape the verified single-texture path has
    // used from the beginning - only the destination differs, and it is a texture the compositor can
    // sample, so nothing is read back to the CPU.
    bool blit_eye_shared(IDirect3DDevice9 *dev, int eye) {
        if (!arms_shared || eye < 0 || eye > 1 || !dev || !eye_shared_local[eye]) return false;
        if (capture_standdown) return false;
        IDirect3DSurface9 *rt = nullptr;
        IDirect3DSurface9 *back = nullptr;
        dev->lpVtbl->GetRenderTarget(dev, 0, (void **)&rt);
        dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, (void **)&back);
        IDirect3DSurface9 *src = pick_source(rt, back);
        bool ok = false;
        if (src) {
            IDirect3DSurface9 *dst = nullptr;
            if (SUCCEEDED(eye_shared_local[eye]->lpVtbl->GetSurfaceLevel(eye_shared_local[eye], 0,
                                                                        &dst)) && dst) {
                std::lock_guard<std::recursive_mutex> guard(stereo_lock);
                HRESULT hr = dev->lpVtbl->StretchRect(dev, src, nullptr, dst, nullptr, D3DTEXF_NONE);
                if (FAILED(hr)) {
                    const HRESULT plain = hr;
                    hr = dev->lpVtbl->StretchRect(dev, src, nullptr, dst, nullptr, D3DTEXF_LINEAR);
                    static bool s_warned = false;
                    if (!s_warned) {
                        s_warned = true;
                        VRLOG("compositor: SHARED stereo StretchRect NONE 0x%08lX, LINEAR 0x%08lX %s",
                              (unsigned long)plain, (unsigned long)hr,
                              SUCCEEDED(hr) ? "(LINEAR worked)"
                                            : "- the eye frame cannot be blitted");
                    }
                }
                ok = SUCCEEDED(hr);
                if (ok) {
                    const uint64_t n = ++eye_shared_blit_count[eye];
                    if (n <= 2 || (n % 600) == 0) {
                        VRLOG("compositor: SHARED stereo eye %d blit %llu (%ux%u, GPU to GPU)",
                              eye, (unsigned long long)n, shared_w, shared_h);
                    }
                }
                dst->lpVtbl->Release(dst);
            } else {
                warn_once("GetSurfaceLevel(shared eye texture)");
            }
        }
        if (rt) rt->lpVtbl->Release(rt);
        if (back) back->lpVtbl->Release(back);
        return ok;
    }

    // Takes one eye's finished picture, wherever the device currently has it, into that eye's texture.
    // Called twice per frame from two different places, which is what makes the two pictures different
    // rather than the same picture twice:
    //   eye 0 - from cam_steer's render-phase detour, between pass 1 and pass 2;
    //   eye 1 - from on_end_scene, after the last pass has finished.
    // Never allocates: every surface exists from device creation.
    bool capture_eye(IDirect3DDevice9 *dev, int eye) {
        if (!arms_stereo || eye < 0 || eye > 1 || !dev) return false;
        if (!eye_blit_tex[eye] || !eye_readback[eye] || !eye_frame_tex[eye]) return false;
        if (capture_standdown) return false;         // the device is lost/reset; see note_standdown()
        IDirect3DSurface9 *rt = nullptr;
        IDirect3DSurface9 *back = nullptr;
        dev->lpVtbl->GetRenderTarget(dev, 0, (void **)&rt);
        dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, (void **)&back);
        IDirect3DSurface9 *src = pick_source(rt, back);
        bool ok = false;
        if (src) {
            // Serialised against the copy thread, which runs do_copy_eye for the same device from
            // EndScene. Measured on the stereo run of 20:19: the compositor's pixel work came from TWO
            // threads at once, and the game died 150 ms later with a video-engine hang
            // (LiveKernelEvent 141) and a null read inside nvwg2um.dll. Two concurrent
            // GetRenderTargetData/StretchRect paths on one D3D9Ex device is the one thing this design
            // did that the verified single-texture path never did.
            std::lock_guard<std::recursive_mutex> guard(stereo_lock);
            if (eye_src_surface[eye]) { eye_src_surface[eye]->lpVtbl->Release(eye_src_surface[eye]); }
            src->lpVtbl->AddRef(src);
            eye_src_surface[eye] = src;
            IDirect3DSurface9 *staging = nullptr;
            if (SUCCEEDED(eye_blit_tex[eye]->GetSurfaceLevel(0, &staging)) && staging) {
                SehCopyJob job = {this, dev, src, staging, eye_readback[eye], eye, false, E_FAIL};
                const bool completed = seh::guard(&SehCopyJob::run, &job);
                if (!completed) warn_once("stereo frame copy raised a structured exception");
                ok = completed && job.ok;
                staging->lpVtbl->Release(staging);
            } else {
                warn_once("GetSurfaceLevel(stereo staging)");
            }
            if (ok) ++eye_copy_count[eye];
        }
        if (rt) rt->lpVtbl->Release(rt);
        if (back) back->lpVtbl->Release(back);
        // A capture that keeps failing is not worth retrying forever: after a few dozen refusals the
        // stereo path disarms itself and the compositor goes back to the single-texture path it has
        // been running all along. Better a session without stereo than a session that keeps poking a
        // device that says no.
        static volatile LONG s_failures = 0;
        if (!ok && InterlockedIncrement(&s_failures) == 30) {
            arms_stereo = false;
            VRLOG("compositor: STEREO capture DISARMED after 30 failed captures - the picture falls "
                  "back to the single game frame in both eyes for the rest of this run");
        }
        return ok;
    }

    // Called from the device-loss/reset path (OpenXrBridge::on_device_lost and release_surfaces).
    // While it is set, no capture touches the device at all: this project's own rule is that a lost
    // device must be left alone, and the stereo path was the one place that rule was not applied - the
    // render-phase hook keeps firing through a Reset, and a fullscreen start-up does exactly one Reset
    // in the middle of it.
    void note_standdown(const char *why) {
        if (!arms_stereo || capture_standdown) return;
        capture_standdown = true;
        VRLOG("compositor: STEREO capture standing down (%s) - both eyes fall back to the single game "
              "frame until the device is back", why);
    }
    void note_standdown_clear() {
        if (!capture_standdown) return;
        capture_standdown = false;
        VRLOG("compositor: STEREO capture resumed (device restored, surfaces rebuilt)");
    }
    bool capture_standdown = false;

    void release_shared_surface() {
        std::lock_guard<std::recursive_mutex> lock(d11_lock);
        // The stereo pair belongs to the same device generation as everything else here: a resolution
        // change or a device loss invalidates both, so they are rebuilt together (and the stereo
        // switch is re-read at that point, so switching it needs no code change).
        release_stereo_surfaces();
        release_shared_stereo_surfaces();
        release_redirect_surfaces();
        if (shared_srv) { shared_srv->Release(); shared_srv = nullptr; }
        if (frame_tex) { frame_tex->Release(); frame_tex = nullptr; }
        if (shared_d11_tex) { shared_d11_tex->Release(); shared_d11_tex = nullptr; }
        if (shared_rtv) { shared_rtv->Release(); shared_rtv = nullptr; }
        if (blit_tex) { blit_tex->Release(); blit_tex = nullptr; }
        if (readback) { readback->lpVtbl->Release(readback); readback = nullptr; }
        
        d3d9_owner = nullptr;
        copy_count = 0;
        if (ex_owner && ex_owner_owned) { ex_owner->lpVtbl->Release(reinterpret_cast<IDirect3DDevice9 *>(ex_owner)); }
        ex_owner = nullptr;
        shared_handle = nullptr;
        shared_w = shared_h = 0;
        shared_src_w = shared_src_h = 0;
        shared_d3d9_format = D3DFMT_UNKNOWN;
    }

    // Work handed to seh::guard. SEH needs the guarded body in its own function,
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

    // The stereo counterpart: one eye's frame through the same three hops, guarded the same way. The
    // staging surface is resolved outside the guard (a NULL there must not be reported as a fault),
    // so it is handed in already valid.
    struct SehCopyJob {
        Impl *self;
        IDirect3DDevice9 *dev;
        IDirect3DSurface9 *src;
        IDirect3DSurface9 *staging;
        IDirect3DSurface9 *sysmem;
        int eye;
        bool ok = false;
        HRESULT hr = E_FAIL;

        static void run(void *ctx) {
            SehCopyJob *j = static_cast<SehCopyJob *>(ctx);
            j->ok = j->self->do_copy_eye(j->dev, j->src, j->staging, j->sysmem, j->eye);
        }
    };

    // One-off capability probe. Kept because it settled a long argument: this
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
                // A device loss during a fullscreen start-up is not a verdict on
                // readback, and calling it "REFUSED" sent an earlier round down the
                // wrong path for a while.
                const bool lost = (job.hr == (HRESULT)0x88760868L);
                VRLOG("probe: GetRenderTargetData returned 0x%08lX - readback %s",
                      (unsigned long)job.hr,
                      SUCCEEDED(job.hr) ? "WORKS"
                                        : (lost ? "INCONCLUSIVE (device was lost)" : "REFUSED"));
            }
        }

        if (tiny_src) tiny_src->lpVtbl->Release(tiny_src);
        if (tiny_dst) tiny_dst->lpVtbl->Release(tiny_dst);
        if (tiny_rt) tiny_rt->Release();
    }

    // Per-frame: mirror the game's back buffer into the D3D11 texture the
    // compositor samples.    //
    // Nothing is allocated here: every surface exists before the first frame,
    // because creating one from inside a frame makes MT Framework stop with
    // "ERR09: Unsupported function.". The staging render target is what makes the
    // rest work, and it has to be handed to StretchRect as the *destination* - an
    // earlier build passed NULL, which returns D3DERR_INVALIDCALL on every frame
    // and left the compositor with nothing to read.
    bool copy_backbuffer(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface) {
        if (!frame_tex || !blit_tex || !readback || !src_surface) return false;

        IDirect3DSurface9 *staging = nullptr;
        if (FAILED(blit_tex->GetSurfaceLevel(0, &staging)) || !staging) {
            warn_once("GetSurfaceLevel(staging target)");
            return false;
        }

        SehJob job = {this, d3d9_dev, src_surface, staging, false, false, E_FAIL};
        const bool completed = seh::guard(&SehJob::run, &job);
        staging->lpVtbl->Release(staging);

        if (!completed) {
            warn_once("frame copy raised a structured exception");
            return false;
        }
        return job.ok;
    }

    // Writes one frame as raw BGRA next to the log. The headset can only report
    // "black" or "something", and this is what turns that into an answer: the file
    // converts to a PNG, so "the panel shows the game" can be checked against the
    // pixels themselves.
    void dump_frame(const uint8_t *pixels, uint32_t pitch, uint32_t w, uint32_t h,
                    const wchar_t *file_name) {
        wchar_t path[MAX_PATH] = L"re6vr_frame0.bgra";
        const wchar_t *log_path = vrlog::path();
        if (log_path && log_path[0]) {
            wcsncpy_s(path, MAX_PATH, log_path, _TRUNCATE);
            wchar_t *slash = wcsrchr(path, L'\\');
            if (slash) {
                wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - path)), file_name);
            }
        }
        HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            VRLOG("compositor: cannot write %ls (err %lu)", path, GetLastError());
            return;
        }
        DWORD written = 0;
        for (uint32_t y = 0; y < h; ++y) {
            WriteFile(f, pixels + (size_t)y * pitch, w * 4, &written, nullptr);
        }
        CloseHandle(f);
        VRLOG("compositor: first frame dumped to %ls (%ux%u BGRA)", path, w, h);
    }

    // Runs inside seh::guard.
    bool do_copy(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface, IDirect3DSurface9 *staging) {
        LARGE_INTEGER s_t0 = {};
        QueryPerformanceCounter(&s_t0);   // start of the readback measured at the end
        // Serialised with the stereo captures, because this is the OTHER thread that can be doing a
        // GetRenderTargetData on the same device at the same moment (the single-texture path runs on the
        // copy thread at EndScene, the stereo eye-0 capture on the render thread between the passes).
        std::lock_guard<std::recursive_mutex> serialize(stereo_lock);
        // 1) GPU blit into the staging target, which is the panel size: StretchRect
        //    does the downscale here (measured S_OK at 3840x2160 -> 1280x720), so
        //    only panel-sized pixels ever reach the readback below. Formats must
        //    match - StretchRect cannot convert between them - and the staging
        //    target is created with the back buffer's format for that reason.
        HRESULT hr = d3d9_dev->lpVtbl->StretchRect(d3d9_dev, src_surface, nullptr, staging, nullptr,
                                                   D3DTEXF_NONE);
        if (FAILED(hr)) {
            // MT Framework also binds render targets of its own while a frame is
            // being built, and those may differ in size; a filtered stretch
            // covers that case.
            const HRESULT plain = hr;
            hr = d3d9_dev->lpVtbl->StretchRect(d3d9_dev, src_surface, nullptr, staging, nullptr,
                                               D3DTEXF_LINEAR);
            static bool s_blit_reported = false;
            if (!s_blit_reported) {
                s_blit_reported = true;
                VRLOG("compositor: StretchRect(source -> staging) NONE 0x%08lX, LINEAR 0x%08lX -> %s",
                      (unsigned long)plain, (unsigned long)hr,
                      SUCCEEDED(hr) ? "copying" : "STUCK - the frame cannot be read");
            }
            if (FAILED(hr)) return false;
        } else {
            static bool s_blit_ok = false;
            if (!s_blit_ok) {
                s_blit_ok = true;
                VRLOG("compositor: StretchRect(source -> staging) ok (same size, same format)");
            }
        }

        // 2) the documented readback, proven to work by the probe.
        hr = d3d9_dev->lpVtbl->GetRenderTargetData(d3d9_dev, staging, readback);
        if (FAILED(hr)) {
            warn_once_hr("GetRenderTargetData(staging -> sysmem)", hr);
            return false;
        }

        D3DLOCKED_RECT locked = {};
        hr = readback->lpVtbl->LockRect(readback, &locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr) || !locked.pBits) {
            warn_once_hr("LockRect(readback surface)", hr);
            return false;
        }

        std::lock_guard<std::recursive_mutex> guard(d11_lock);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = d11_context->Map(frame_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData) {
            warn_once_hr("Map(copy target)", hr);
            readback->lpVtbl->UnlockRect(readback);
            return false;
        }

        // 3) Straight row copy: the staging surface is already the panel size
        //    because the GPU blit above did the scaling. The luma reading of the
        //    very first frame is the one number that separates "the headset shows
        //    the game" from "the headset shows black".
        const uint8_t *base = (const uint8_t *)locked.pBits;
        uint8_t *out = (uint8_t *)mapped.pData;
        const uint32_t src_w = shared_src_w ? shared_src_w : shared_w;
        const uint32_t src_h = shared_src_h ? shared_src_h : shared_h;
        uint64_t luma_sum = 0;
        uint32_t lit = 0, sampled = 0;
        const bool first = (copy_count == 0);
        // The first frame is very often a black loading frame, so the content is
        // re-measured later as well: a copy loop that runs happily while reading a
        // blank surface looks identical in the log otherwise.
        const bool sample_now = first || ((copy_count % 300) == 299);

        for (uint32_t y = 0; y < shared_h; ++y) {
            const uint8_t *srow = base + (size_t)y * locked.Pitch;
            uint8_t *drow = out + (size_t)y * mapped.RowPitch;
            memcpy(drow, srow, (size_t)shared_w * 4);
            if (sample_now) {
                const uint32_t *px = reinterpret_cast<const uint32_t *>(srow);
                for (uint32_t x = 0; x < shared_w; x += 4) {
                    luma_sum += (px[x] & 0xFFu) + ((px[x] >> 8) & 0xFFu) + ((px[x] >> 16) & 0xFFu);
                    if ((px[x] & 0x00FFFFFFu) != 0) ++lit;
                    ++sampled;
                }
            }
        }

        // A screenshot on request, taken here because the surface is still locked and this is
        // the render thread. Used by the camera probe to record - and judge - the frame at the
        // moment it scans, so "found nothing" can be told apart from "there was nothing to
        // find": see screenshot.cpp.
        screenshot_service(base, (uint32_t)locked.Pitch, shared_w, shared_h);

        d11_context->Unmap(frame_tex, 0);
        readback->lpVtbl->UnlockRect(readback);
        d11_context->Flush();
        // Cost of the readback hop itself, and the rate the copy thread achieves.
        //
        // This is the part of the pipeline that cannot be made free: a 3840x2160
        // frame leaves the GPU through StretchRect -> GetRenderTargetData -> LockRect
        // -> memcpy every frame. The eye-buffer work on the D3D11 side is one clear
        // plus one quad, which is nothing next to this, so when frame rate becomes
        // the problem this is the number to look at.
        {
            static LARGE_INTEGER s_freq = {};
            static LARGE_INTEGER s_last = {};
            static double s_sum_ms = 0.0;
            static uint64_t s_since = 0;
            if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            const double ms = s_freq.QuadPart
                                  ? 1000.0 * (double)(now.QuadPart - s_t0.QuadPart) /
                                        (double)s_freq.QuadPart
                                  : 0.0;
            s_sum_ms += ms;
            ++s_since;
            // Report on a fixed stride, and only once there is a window worth
            // averaging. An earlier version also reported when the counter had just
            // been reset, which combined with the reset below to log on every single
            // frame: 8182 lines of log in one 90 second session, drowning everything
            // else. Reset and report must stay tied to the same condition.
            if (s_since >= 300) {
                double fps = 0.0;
                if (s_last.QuadPart && now.QuadPart != s_last.QuadPart) {
                    fps = (double)s_since * (double)s_freq.QuadPart /
                          (double)(now.QuadPart - s_last.QuadPart);
                }
                VRLOG("compositor: readback %.2f ms/frame avg over %llu frames",
                      s_sum_ms / (double)s_since, (unsigned long long)s_since);
                if (fps > 0.0) {
                    VRLOG("compositor: copy thread ~%.1f frames/s (%.1f ms/frame wall)",
                          fps, 1000.0 / fps);
                }
                s_last = now;
                s_sum_ms = 0.0;
                s_since = 0;
            }
        }

        if (sample_now) {
            VRLOG("compositor: frame %llu %ux%u <- %ux%u, %u/%u pixels non-black, mean luma %llu",
                  (unsigned long long)copy_count, shared_w, shared_h, src_w, src_h, lit, sampled,
                  (unsigned long long)(sampled ? luma_sum / ((uint64_t)sampled * 3) : 0));
        }
        if (first) {
            // Dumped while the surface is still locked, straight from the readback.
            dump_frame(base, (uint32_t)locked.Pitch, shared_w, shared_h, L"re6vr_frame0.bgra");
        } else if (copy_count == 299) {
            // A frame from after start-up: this is the one that shows game content.
            dump_frame(base, (uint32_t)locked.Pitch, shared_w, shared_h, L"re6vr_frame300.bgra");
        } else if (extra_dump_count > 0 || extra_dump_at) {
            // An arbitrary frame, so a specific screen (the title, where a duplicated
            // logo is unmistakable) can be captured without guessing which frame the
            // fixed 300 lands on. re6vr_dump_at.txt may list several frames, comma or
            // whitespace separated, so one run can catch several screens.
            uint64_t which = 0;
            bool hit = false;
            if (extra_dump_at && copy_count == extra_dump_at) {
                which = extra_dump_at;
                hit = true;
            }
            for (int i = 0; i < extra_dump_count && !hit; ++i) {
                if (copy_count == extra_dump_list[i]) {
                    which = extra_dump_list[i];
                    hit = true;
                }
            }
            if (hit) {
                wchar_t name[64] = L"re6vr_frame_at.bgra";
                _snwprintf_s(name, _countof(name), _TRUNCATE, L"re6vr_frame_at_%llu.bgra",
                             (unsigned long long)which);
                dump_frame(base, (uint32_t)locked.Pitch, shared_w, shared_h, name);
            }
        }

        ++copy_count;
        if (copy_count == 1 || (copy_count % 300) == 0) {
            VRLOG("compositor: copied %llu frames (%ux%u <- %ux%u)", (unsigned long long)copy_count,
                  shared_w, shared_h, src_w, src_h);
        }
        return true;
    }

    // The same three hops as do_copy, into the given EYE's own texture. Kept as its own function
    // rather than a parameter on do_copy so that the verified single-texture path stays byte for byte
    // what it was: with stereo off, nothing in this function is ever called.
    //
    // Runs inside seh::guard, like do_copy: a driver that faults here should cost one frame, not the
    // session.
    bool do_copy_eye(IDirect3DDevice9 *d3d9_dev, IDirect3DSurface9 *src_surface,
                     IDirect3DSurface9 *staging, IDirect3DSurface9 *sysmem, int eye) {
        ID3D11Texture2D *tex = eye_frame_tex[eye];
        // Held for the whole hop: D3D9 blit, readback, lock and D3D11 map. A capture arriving from the
        // other thread waits here instead of interleaving with this one.
        std::lock_guard<std::recursive_mutex> serialize(stereo_lock);
        HRESULT hr = d3d9_dev->lpVtbl->StretchRect(d3d9_dev, src_surface, nullptr, staging, nullptr,
                                                   D3DTEXF_NONE);
        if (FAILED(hr)) {
            hr = d3d9_dev->lpVtbl->StretchRect(d3d9_dev, src_surface, nullptr, staging, nullptr,
                                               D3DTEXF_LINEAR);
            static bool s_eye_blit_reported = false;
            if (!s_eye_blit_reported) {
                s_eye_blit_reported = true;
                VRLOG("compositor: STEREO StretchRect(source -> eye staging) -> 0x%08lX %s",
                      (unsigned long)hr,
                      SUCCEEDED(hr) ? "(LINEAR worked)" : "STUCK - the eye frame cannot be read");
            }
            if (FAILED(hr)) return false;
        }
        hr = d3d9_dev->lpVtbl->GetRenderTargetData(d3d9_dev, staging, sysmem);
        if (FAILED(hr)) {
            warn_once_hr("GetRenderTargetData(stereo staging -> sysmem)", hr);
            return false;
        }
        D3DLOCKED_RECT locked = {};
        hr = sysmem->lpVtbl->LockRect(sysmem, &locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr) || !locked.pBits) {
            warn_once_hr("LockRect(stereo readback)", hr);
            return false;
        }
        std::lock_guard<std::recursive_mutex> guard(d11_lock);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = d11_context->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData) {
            warn_once_hr("Map(stereo copy target)", hr);
            sysmem->lpVtbl->UnlockRect(sysmem);
            return false;
        }
        // A cheap fingerprint per eye, sampled the same way for both, so "the two eyes are actually
        // different pictures" stops being an assumption: as long as the eye index reaches the camera,
        // the two sums differ. Identical sums across a whole run mean the second render used the first
        // render's pose - the exact failure the dual pass exists to rule out.
        uint64_t luma_sum = 0;
        const uint8_t *base = (const uint8_t *)locked.pBits;
        uint8_t *out = (uint8_t *)mapped.pData;
        for (uint32_t y = 0; y < shared_h; ++y) {
            const uint8_t *srow = base + (size_t)y * locked.Pitch;
            memcpy(out + (size_t)y * mapped.RowPitch, srow, (size_t)shared_w * 4);
            if ((y & 31u) == 0) {
                const uint32_t *px = reinterpret_cast<const uint32_t *>(srow);
                for (uint32_t x = 0; x < shared_w; x += 16) {
                    luma_sum += (px[x] & 0xFFu) + ((px[x] >> 8) & 0xFFu) + ((px[x] >> 16) & 0xFFu);
                }
            }
        }
        d11_context->Unmap(tex, 0);
        sysmem->lpVtbl->UnlockRect(sysmem);
        d11_context->Flush();
        const uint64_t n = ++eye_copy_count[eye];
        if (n <= 3 || (n % 300) == 0) {
            VRLOG("compositor: STEREO eye %d frame %llu copied (%ux%u), luma fingerprint %llu",
                  eye, (unsigned long long)n, shared_w, shared_h,
                  (unsigned long long)luma_sum);
        }
        return true;
    }

    // ------------------------------------------------------------------ offline redirect test
    //
    // The redirect path's one mechanism, checked without the game: bind a texture of OURS as the
    // device's render target, draw something into it through the D3D9 API, and read the result back out
    // of OUR texture. If the colour arrives, "whatever is drawn while our surface is bound lands in our
    // surface" is proven on this machine, and the only thing left for a game run to show is that the
    // engine's pass honours the same binding (which the 21:02 probe already measured).
    //
    // Driven by RE6VR_REDIRECT_SELFTEST=1 (the proxy's own self-test switch must be on as well).
    bool redirect_selftest(IDirect3DDevice9 *game_device) {
        wchar_t buf[8] = L"";
        if (GetEnvironmentVariableW(L"RE6VR_REDIRECT_SELFTEST", buf, 8) == 0 || buf[0] != L'1') {
            return true;
        }
        VRLOG("selftest-redirect: requested");
        if (!arms_redirect || !game_device) {
            VRLOG("selftest-redirect: the redirect path is not armed (stereo marker %s, device %p) - "
                  "nothing to test", stereo_wanted ? "set" : "not set", (void *)game_device);
            return arms_redirect;      // "not armed" is not a failure of the mechanism
        }
        IDirect3DDevice9 *dev = game_device;
        IDirect3DSurface9 *saved_rt = nullptr;
        dev->lpVtbl->GetRenderTarget(dev, 0, (void **)&saved_rt);

        const DWORD colours[2] = {0x00102040u, 0x00A0C0E0u};
        bool ok = true;
        for (int eye = 0; ok && eye < 2; ++eye) {
            if (!eye_rt_surface[eye]) { ok = false; break; }
            HRESULT hr = dev->lpVtbl->SetRenderTarget(dev, 0, eye_rt_surface[eye]);
            if (SUCCEEDED(hr)) hr = dev->lpVtbl->Clear(dev, 0, nullptr, D3DCLEAR_TARGET, colours[eye],
                                                       1.0f, 0);
            if (FAILED(hr)) {
                VRLOG("selftest-redirect: could not draw into eye %d's texture: 0x%08lX", eye,
                      (unsigned long)hr);
                ok = false;
                break;
            }
        }
        if (saved_rt) {
            dev->lpVtbl->SetRenderTarget(dev, 0, saved_rt);
            saved_rt->lpVtbl->Release(saved_rt);
        }

        // Read each texture back through the compositor's own path, so the check covers the copy the
        // game will use and not just the drawing.
        for (int eye = 0; ok && eye < 2; ++eye) {
            if (!copy_eye_redirect(dev, eye)) {
                VRLOG("selftest-redirect: eye %d copy FAILED", eye);
                ok = false;
                break;
            }
            D3DLOCKED_RECT locked = {};
            if (SUCCEEDED(readback->lpVtbl->LockRect(readback, &locked, nullptr, D3DLOCK_READONLY)) &&
                locked.pBits) {
                const uint8_t *row = (const uint8_t *)locked.pBits +
                                     (size_t)(shared_h / 2) * locked.Pitch;
                const uint8_t *px = row + (size_t)(shared_w / 2) * 4;
                const uint32_t got = ((uint32_t)px[0]) | ((uint32_t)px[1] << 8) |
                                     ((uint32_t)px[2] << 16);
                const bool right = (got & 0x00FFFFFFu) == (colours[eye] & 0x00FFFFFFu);
                VRLOG("selftest-redirect: eye %d read %08X from our own render target (drew %08X) -> %s",
                      eye, (unsigned)got, (unsigned)colours[eye], right ? "ok" : "WRONG");
                ok = ok && right;
                readback->lpVtbl->UnlockRect(readback);
            } else {
                VRLOG("selftest-redirect: eye %d could not be read back - INCONCLUSIVE", eye);
                ok = false;
            }
        }
        VRLOG("selftest-redirect: %s", ok ? "PASS - a texture of ours takes the drawing and reaches the "
                                            "compositor"
                                          : "FAIL");
        return ok;
    }
    //
    // Does the GPU-only path actually work on this machine? That is a question with a yes/no answer and
    // it can be asked WITHOUT the game: create a shared D3D9Ex texture, clear it through D3D9, open the
    // handle in D3D11 and read the pixel back in D3D11. If the colour arrives, the whole approach is
    // sound and any later failure is in the plumbing; if it does not, no amount of per-frame debugging
    // would ever have found it.
    //
    // Driven by RE6VR_SHARED_SELFTEST=1 (the proxy's own self-test switch must be on as well).
    bool shared_interop_selftest(IDirect3DDevice9 *game_device) {
        wchar_t buf[8] = L"";
        if (GetEnvironmentVariableW(L"RE6VR_SHARED_SELFTEST", buf, 8) == 0 || buf[0] != L'1') {
            return true;                 // not requested: nothing logged, nothing created
        }
        VRLOG("selftest-shared: requested");
        if (!d11_device && !create_d11_device_offline()) {
            VRLOG("selftest-shared: no D3D11 device - cannot run");
            return false;
        }
        // Prefer the device the caller handed over: with the proxy now asking for a D3D9Ex device at
        // CreateDevice time, the game's own device can create shared textures, which is the whole point
        // (no second device in the process).
        IDirect3DDevice9 *ex = nullptr;
        if (game_device) {
            IDirect3DDevice9 *probe = nullptr;
            if (game_device->lpVtbl->QueryInterface(game_device, D3D9_IID(IDirect3DDevice9Ex),
                                                    (void **)&probe) == S_OK && probe) {
                probe->lpVtbl->Release(probe);
                ex = game_device;
                VRLOG("selftest-shared: the device handed over IS D3D9Ex - testing on it");
            } else {
                VRLOG("selftest-shared: the device handed over is plain D3D9 (no shared textures "
                      "possible on it)");
            }
        }
        if (!ex) {
            // No fallback to a private D3D9Ex device: creating one in this process faults inside d3d9
            // (measured twice, 2026-09-25), and a diagnostic that can crash the game is not worth
            // running. The check reports what it found instead.
            VRLOG("selftest-shared: no D3D9Ex device in this process (the caller's device is plain D3D9 "
                  "and the private-Ex route is removed because it faults inside d3d9) - INCONCLUSIVE");
            return true;
        }
        const uint32_t w = 64, h = 64;
        const D3DFORMAT fmt = D3DFMT_X8R8G8B8;
        const uint32_t put = 0x00132639u;    // BGRA: blue 0x13, green 0x26, red 0x39

        IDirect3DTexture9 *tex = nullptr;
        HANDLE handle = nullptr;
        HRESULT hr = ex->lpVtbl->CreateTexture(ex, w, h, 1, D3DUSAGE_RENDERTARGET, fmt,
                                              D3DPOOL_DEFAULT,
                                              reinterpret_cast<void **>(&tex), &handle);
        if (FAILED(hr) || !tex || !handle) {
            VRLOG("selftest-shared: D3D9Ex CreateTexture(shared) failed: 0x%08lX - FAIL", (unsigned long)hr);
            if (tex) tex->Release();
            return false;
        }
        VRLOG("selftest-shared: D3D9Ex shared texture %ux%u created, handle %p", w, h, (void *)handle);

        IDirect3DSurface9 *surf = nullptr;
        bool ok = false;
        if (SUCCEEDED(tex->lpVtbl->GetSurfaceLevel(tex, 0, &surf)) && surf) {
            hr = ex->lpVtbl->SetRenderTarget(ex, 0, surf);
            if (SUCCEEDED(hr)) {
                hr = ex->lpVtbl->Clear(ex, 0, nullptr, D3DCLEAR_TARGET, (DWORD)put, 1.0f, 0);
            }
            if (FAILED(hr)) {
                VRLOG("selftest-shared: could not fill the D3D9 side: 0x%08lX", (unsigned long)hr);
            } else {
                ID3D11Texture2D *shared = nullptr;
                hr = d11_device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D),
                                                    reinterpret_cast<void **>(&shared));
                if (FAILED(hr) || !shared) {
                    VRLOG("selftest-shared: D3D11 OpenSharedResource failed: 0x%08lX - FAIL",
                          (unsigned long)hr);
                } else {
                    D3D11_TEXTURE2D_DESC d = {};
                    shared->GetDesc(&d);
                    D3D11_TEXTURE2D_DESC sd = d;
                    sd.Usage = D3D11_USAGE_STAGING;
                    sd.BindFlags = 0;
                    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    sd.MiscFlags = 0;
                    ID3D11Texture2D *stage = nullptr;
                    if (SUCCEEDED(d11_device->CreateTexture2D(&sd, nullptr, &stage)) && stage) {
                        d11_context->CopyResource(stage, shared);
                        D3D11_MAPPED_SUBRESOURCE m = {};
                        if (SUCCEEDED(d11_context->Map(stage, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
                            const uint8_t *px = (const uint8_t *)m.pData +
                                                (size_t)(h / 2) * m.RowPitch + (size_t)(w / 2) * 4;
                            const uint32_t got = ((uint32_t)px[0]) | ((uint32_t)px[1] << 8) |
                                                 ((uint32_t)px[2] << 16);
                            ok = ((got & 0x00FFFFFFu) == (put & 0x00FFFFFFu));
                            VRLOG("selftest-shared: D3D11 read %08X from the shared surface (D3D9 wrote "
                                  "%08X) -> %s", (unsigned)got, (unsigned)put, ok ? "PASS" : "FAIL");
                            d11_context->Unmap(stage, 0);
                        } else {
                            VRLOG("selftest-shared: Map(staging) failed - INCONCLUSIVE");
                        }
                        stage->Release();
                    } else {
                        VRLOG("selftest-shared: could not create the readback staging texture "
                              "- INCONCLUSIVE");
                    }
                    shared->Release();
                }
            }
            surf->lpVtbl->Release(surf);
        }
        tex->Release();
        return ok;
    }
    //
    // The stereo capture path is the one piece of this work that could not be checked with the
    // existing offline tests: the panel self-test starts from a texture, and this path starts from a
    // D3D9 RENDER TARGET and goes through StretchRect -> GetRenderTargetData -> LockRect -> D3D11 map.
    // A fault anywhere in it would show up in the headset as "one eye is black", which is expensive to
    // diagnose from the outside - so it is checked here, on a real D3D9 device, before any game run.
    //
    // What it proves, with the device itself as the witness:
    //   1. the four surfaces per eye can be created for this back-buffer format and size (they are
    //      created at device creation in the real path for exactly this reason - MT Framework refuses
    //      resource creation inside a frame);
    //   2. the copy really reads the render target - two known colours must come back with the
    //      fingerprints they went in with, through the capture chain, not through a shortcut;
    //   3. an eye that has never been captured falls back to the shared texture, so a stereo run that
    //      fails to capture shows the game frame instead of black.
    //
    // Driven by RE6VR_STEREO_SELFTEST=1 (the proxy's own self-test switch must be on as well).
    // `game_device` is the device the harness/game is drawing with; it is used only if the ordinary
    // path (create_device_surfaces) has not recorded one, which is the case whenever there is no
    // headset - i.e. exactly when this test is worth running.
    bool stereo_capture_selftest(IDirect3DDevice9 *game_device) {
        wchar_t buf[8] = L"";
        if (GetEnvironmentVariableW(L"RE6VR_STEREO_SELFTEST", buf, 8) == 0 || buf[0] != L'1') {
            return true;                 // not requested: nothing is logged, nothing is created
        }
        VRLOG("selftest-stereo: requested");
        for (int i = 0; i < 2; ++i) {
            if (!eye_blit_tex[i]) {
                VRLOG("selftest-stereo: eye %d surfaces missing - nothing to test. The stereo pair is "
                      "built at device creation and only when re6vr_stereo.txt was 1 at that moment "
                      "(this run's marker: %s)", i, stereo_wanted ? "set" : "not set");
                return false;
            }
        }

        // The D3D11 side is needed to write into the eye textures; the D3D9 side comes from the test.
        if (!d11_device && !create_d11_device_offline()) {
            VRLOG("selftest-stereo: no D3D11 device - cannot run");
            return false;
        }
        IDirect3DDevice9 *dev = d3d9_owner ? d3d9_owner : game_device;
        if (!dev) {
            VRLOG("selftest-stereo: no D3D9 device available - cannot run");
            return false;
        }
        IDirect3DDevice9 *saved_owner = d3d9_owner;
        d3d9_owner = dev;                // only the copy path reads it; restored before returning
        const uint32_t w = shared_w ? shared_w : 64;
        const uint32_t h = shared_h ? shared_h : 64;
        const D3DFORMAT fmt = shared_d3d9_format ? shared_d3d9_format : D3DFMT_X8R8G8B8;

        IDirect3DTexture9 *rt = nullptr;
        IDirect3DSurface9 *rt_surf = nullptr;
        IDirect3DSurface9 *sysmem = nullptr;
        HRESULT hr = dev->lpVtbl->CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, fmt,
                                                D3DPOOL_DEFAULT,
                                                reinterpret_cast<void **>(&rt), nullptr);
        if (SUCCEEDED(hr) && rt) rt->lpVtbl->GetSurfaceLevel(rt, 0, &rt_surf);
        hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, w, h, fmt, D3DPOOL_SYSTEMMEM,
                                                      reinterpret_cast<void **>(&sysmem), nullptr);
        bool ok = (rt_surf && sysmem);
        if (!ok) {
            VRLOG("selftest-stereo: could not prepare a %ux%u render target (0x%08lX) - INCONCLUSIVE",
                  w, h, (unsigned long)hr);
        }

        const uint32_t colours[2] = {0x00102040u, 0x00A0C0E0u};   // dark for eye 0, bright for eye 1
        uint64_t fingerprint[2] = {0, 0};
        for (int eye = 0; ok && eye < 2; ++eye) {
            IDirect3DSurface9 *prev = nullptr;
            dev->lpVtbl->GetRenderTarget(dev, 0, (void **)&prev);
            if (FAILED(dev->lpVtbl->SetRenderTarget(dev, 0, rt_surf)) ||
                FAILED(dev->lpVtbl->Clear(dev, 0, nullptr, D3DCLEAR_TARGET, (DWORD)colours[eye],
                                          1.0f, 0))) {
                VRLOG("selftest-stereo: could not make eye %d's source frame - INCONCLUSIVE", eye);
                if (prev) prev->lpVtbl->Release(prev);
                ok = false;
                break;
            }
            const bool want_back = use_back_buffer;
            use_back_buffer = false;   // take the render target, exactly as the real path does
            ok = capture_eye(dev, eye);
            use_back_buffer = want_back;
            dev->lpVtbl->SetRenderTarget(dev, 0, prev);
            if (prev) prev->lpVtbl->Release(prev);
            if (!ok) {
                VRLOG("selftest-stereo: capture_eye(%d) FAILED", eye);
                break;
            }
        }

        // Read each eye's captured picture back out of its own texture and check the colour.
        //
        // A fresh SYSTEMMEM destination and GetRenderTargetData, not the eye's own readback surface:
        // that one holds the source frame the copy consumed, so reading it back would compare the
        // input with itself and pass whatever the capture did. (That is exactly what the first version
        // of this test did - it reported black for both eyes and was measuring nothing.)
        for (int eye = 0; ok && eye < 2; ++eye) {
            IDirect3DSurface9 *tex_surf = nullptr;
            IDirect3DSurface9 *check = nullptr;
            bool got_pixel = false;
            uint32_t got = 0;
            if (eye_blit_tex[eye] &&
                SUCCEEDED(eye_blit_tex[eye]->lpVtbl->GetSurfaceLevel(eye_blit_tex[eye], 0,
                                                                     &tex_surf)) &&
                tex_surf &&
                SUCCEEDED(dev->lpVtbl->CreateOffscreenPlainSurface(dev, w, h, fmt, D3DPOOL_SYSTEMMEM,
                                                                   reinterpret_cast<void **>(&check),
                                                                   nullptr)) &&
                check) {
                D3DLOCKED_RECT locked = {};
                if (SUCCEEDED(dev->lpVtbl->GetRenderTargetData(dev, tex_surf, check)) &&
                    SUCCEEDED(check->lpVtbl->LockRect(check, &locked, nullptr, D3DLOCK_READONLY)) &&
                    locked.pBits) {
                    const uint8_t *row = (const uint8_t *)locked.pBits +
                                         (size_t)(h / 2) * locked.Pitch;
                    const uint8_t *px = row + (size_t)(w / 2) * 4;      // BGRA
                    got = ((uint32_t)px[0]) | ((uint32_t)px[1] << 8) |
                          ((uint32_t)px[2] << 16) | 0xFF000000u;
                    got_pixel = true;
                    check->lpVtbl->UnlockRect(check);
                } else {
                    VRLOG("selftest-stereo: eye %d: could not read its texture back (GetRenderTargetData "
                          "or LockRect failed) - INCONCLUSIVE", eye);
                }
            } else {
                VRLOG("selftest-stereo: eye %d: could not build a readback surface - INCONCLUSIVE", eye);
            }
            if (check) check->lpVtbl->Release(check);
            if (tex_surf) tex_surf->lpVtbl->Release(tex_surf);
            if (!got_pixel) {
                ok = false;
                break;
            }
            fingerprint[eye] = (uint64_t)(got & 0x00FFFFFFu);
            VRLOG("selftest-stereo: eye %d copied frame read back at the centre = %08X (put %08X), "
                  "%llu capture(s)", eye, (unsigned)got, (unsigned)colours[eye],
                  (unsigned long long)eye_copy_count[eye]);
        }

        // The selection rule, which is what the compositor uses to decide what each eye samples.
        // Both eyes switch together: with one eye captured and the other not, both must fall back to
        // the shared game frame rather than showing eye 0's picture to eye 1.
        const bool srv0 = (frame_srv_for_eye(0) != nullptr);
        const uint64_t real0 = eye_copy_count[0];
        const uint64_t real1 = eye_copy_count[1];
        eye_copy_count[1] = 0;                       // pretend eye 1 was never captured
        const bool fallback = (frame_srv_for_eye(0) == shared_srv);
        eye_copy_count[0] = real0;
        eye_copy_count[1] = real1;

        if (rt_surf) rt_surf->lpVtbl->Release(rt_surf);
        if (rt) rt->lpVtbl->Release(rt);
        if (sysmem) sysmem->lpVtbl->Release(sysmem);
        d3d9_owner = saved_owner;

        if (ok) {
            const bool differ = (fingerprint[0] != fingerprint[1]);
            const bool right = (fingerprint[0] == (uint64_t)(colours[0] & 0x00FFFFFFu)) &&
                               (fingerprint[1] == (uint64_t)(colours[1] & 0x00FFFFFFu));
            ok = differ && right && srv0 && fallback;
            VRLOG("selftest-stereo: eye0 %llu eye1 %llu, differ=%d correct=%d, eye srv=%d, "
                  "uncaptured eye falls back=%d -> %s",
                  (unsigned long long)fingerprint[0], (unsigned long long)fingerprint[1],
                  (int)differ, (int)right, (int)srv0, (int)fallback, ok ? "PASS" : "FAIL");
        }
        return ok;
    }

    // The virtual screen lives in the world, not in each eye's field of view.
    //
    // Anchoring it to the FOV (what this used to do) had two visible consequences,
    // both reported from the headset: the panel was stretched to the FOV's nearly
    // square shape, and - because each eye's FOV is centred differently - the two
    // eyes' panels sat in two different places, which reads as two screens instead
    // of one. A fixed 16:9 screen in space fixes both at once, and the per-eye
    // parallax that comes with it is what makes it look like a real screen.
    bool  panel_placed = false;    float panel_center[3] = {0.0f, 0.0f, 0.0f};
    float panel_right[3] = {1.0f, 0.0f, 0.0f};
    float panel_up[3] = {0.0f, 1.0f, 0.0f};
    float panel_fwd[3] = {0.0f, 0.0f, 1.0f};
    // Defaults are the "cinema" feel chosen from the headset: a big screen a few
    // metres away, subtending ~60 x 34 degrees. screen_dist is the true
    // player-to-screen distance, so these two numbers are the whole story of how
    // large the panel is *in the world*; how large it looks on the display is
    // screen_dist together with screen_scale_user, applied where the frame is
    // submitted (see on_frame).
    float screen_dist = 4.0f;                    // metres, RE6VR_SCREEN_DIST
    float panel_w = 4.6f;                        // metres, RE6VR_SCREEN_WIDTH
    float panel_h = 4.6f * 9.0f / 16.0f;         // 16:9, the shape of the game image

    // How big the screen ends up on the headset's display, as a plain multiplier.
    //
    // This is the number that was missing, and its absence is why nobody could
    // make the screen change size: the panel was drawn to fill the *submitted*
    // frustum exactly, and the frustum submitted to the runtime was the panel's
    // own rectangle. So whatever the panel's size or distance, the compositor was
    // handed "this picture covers exactly this much of your view" and mapped the
    // picture onto the same part of the display every time. The projection and
    // the picture cancel, so RE6VR_SCREEN_DIST and RE6VR_SCREEN_WIDTH could be
    // changed all day without the screen looking any different - the only thing
    // that ever changed was the render resolution.
    //
    // The submitted FOV is therefore separated from the panel's own rectangle
    // below (on_frame): the picture keeps covering its whole slice, but the
    // rectangle that slice is declared to occupy is divided by this factor, so
    // the screen really does get smaller or bigger on the display.
    //
    // screen_scale itself carries the distance relationship as well - see the
    // comment at the submission site - so <ref>/screen_dist keeps the panel
    // looking like a panel of that size at that distance, and this file is the
    // extra "make it bigger/smaller than that" trim.
    float screen_scale_user = 1.0f;              // RE6VR_SCREEN_SCALE

    // How the eye frustum relates to the swapchain slice's aspect ratio.
    //
    // The per-eye slice is 998x2148 - a portrait strip, aspect 0.465 - while the
    // submitted projection region is the 16:9 panel, aspect 1.778. Those two only
    // agree if the frustum is derived from the panel alone (mode 0).
    //
    //   0 = panel  : frustum is exactly the panel rectangle. The submitted region
    //                is therefore 16:9, so the runtime maps the strip onto a 16:9
    //                area and the picture is correct on the headset. The cost is
    //                pixel density: tangent-per-pixel is 3.8x finer vertically than
    //                horizontally, so the strip is vertically oversampled and the
    //                horizontal axis is the limiting one.
    //   1 = viewport: frustum is widened to the slice's own aspect (0.465). The
    //                buffer pixels are then isotropic, the panel keeps its 16:9
    //                shape on screen, but it covers only part of the slice and the
    //                rest is cleared black inside the submitted region.
    float frustum_aspect_mode = 0.0f;            // RE6VR_FRUSTUM_ASPECT_MODE

    // Eye separation used when building the per-eye frusta, as a fraction of the
    // runtime's IPD.
    //
    // A world-locked screen with the full IPD is geometrically correct but reads
    // as a double image to anyone who is not used to VR: the two eyes converge on
    // the same panel from 63 mm apart, so every edge shows a small disparity.
    // Lowering it trades the (meaningless, for a flat screen) stereo parallax of
    // the panel itself for a crisp single image. 0 = both eyes see exactly the
    // same thing, 1 = the runtime's real IPD.
    float ipd_scale = 0.0f;                      // RE6VR_IPD_SCALE

    // Per-eye horizontal shift of the sampled picture, in units of the panel's
    // half angle (tan). Positive moves the picture to the right in the eye.
    //
    // Why this exists: the frustum below is derived from the panel rectangle
    // *per eye*, which maps the panel onto the full NDC range in both eyes. The
    // two eye images are therefore geometrically identical, while the runtime
    // still reprojects each layer by its own eye pose. The result is a doubled
    // image exactly one IPD of parallax wide, which is what the headset has
    // shown from the first working build. This knob cancels it from our side:
    // shift the left eye one way, the right eye the other, and the two land on
    // top of each other again.
    //
    // Set from re6vr_uv_shift.txt (Steam does not pass the shell's environment
    // on) or RE6VR_UV_SHIFT. Contents: "<left> <right>" in percent of the
    // panel's half angle, e.g. "-1.5 1.5".
    float uv_shift_l[2] = {0.0f, 0.0f};
    float uv_shift_r[2] = {0.0f, 0.0f};
    // Counter-roll per eye, radians. The fourth degree of freedom a stereo pair
    // needs: two images that are rotated relative to each other cannot be brought
    // into alignment by any translation.
    float roll_l = 0.0f, roll_r = 0.0f;
    // Per-eye scale trim, as a multiplier on the panel size. See gRotZoom.
    float zoom_l = 1.0f, zoom_r = 1.0f;
    bool uv_shift_set = false;
    // Non-zero = derive the shift from the eye separation instead of reading it:
    // +1 or -1 picks the sign. See read_screen_env and place_panel.
    float auto_uv_shift = 0.0f;
    // "sweep": walk the trim through a range while the game runs, so the value
    // that cancels the runtime's offset can be found by eye in one session
    // instead of one game launch per candidate value. The current value is
    // printed on every step, so the log says what the viewer was looking at.
    bool uv_shift_sweep = false;
    // "keys": the viewer drives the trim with the arrow keys while wearing the
    // headset. A number that has to be found by eye is far easier to find by
    // nudging it interactively than by reading a sequence of timed steps, and the
    // count of presses is the measurement.
    bool uv_shift_keys = false;
    int key_latch = 0;
    long long key_repeat_from = 0;
    bool key_repeat_armed = false;
    uint64_t sweep_frames = 0;
    int sweep_step = 0;
    LARGE_INTEGER sweep_last = {};
    // Set by draw_quad when ipd_scale >= 1.5 (the converged diagnostic): on_frame
    // then submits both views with the same pose.
    bool converge_views = false;
    // Median-plane separation of the two real eyes, captured by draw_quad.
    //
    // This is the number the automatic trim needs, and it is NOT the same as the
    // camera position: with ipd_scale = 0 the camera is deliberately collapsed
    // onto the midpoint while the pose handed to the runtime stays the real
    // per-eye pose. Deriving the trim from the collapsed camera made it exactly
    // zero - visible in the log as "eye offset of +0.0 mm" - so the compensation
    // did nothing at all in the very configuration it was written for.
    float converge_offset[2] = {0.0f, 0.0f};
    // Filled by draw_quad, cleared and re-filled by on_frame: uv_shift_l/r are
    // derived from the last panel geometry, which only draw_quad knows.
    // [eye][0 = horizontal, 1 = vertical]
    float eye_uv_shift[2][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};
    // Push a second, identical projection layer on top (UEVR's Virtual Desktop
    // trick). Off unless re6vr_vd_dummy.txt exists.
    bool push_dummy_layer = false;
    // Submit ONE symmetric field of view for both eyes instead of the runtime's
    // own per-eye asymmetric one. DEFAULT ON - this is the fix for the double
    // image that this project spent a long time chasing.
    //
    // The runtime's per-eye FOV is asymmetric (on this headset, with Virtual
    // Desktop's FOV set to 100: left eye -54/+40 deg, right eye -40/+54) because
    // each lens sits off-centre on its panel. A compositor reprojects a submitted
    // image by assuming the app rendered it with the FOV the app declared - and
    // this app's picture is NOT rendered with that frustum, it is rendered with
    // the panel's own rectangle. So the reprojection was applied to content it
    // does not match, and because the two eyes' FOVs are mirror images the two
    // mismatches differ, displacing the two eye images by about an IPD.
    //
    // Handing both eyes the same symmetric frustum removes the asymmetry and with
    // it the doubling: with it on, both crosses overlap and no amount of head
    // tilting separates them, where sixteen rounds of horizontal/vertical/scale/
    // roll trimming never could - because a translation cannot cancel a mismatch
    // that is applied per eye by the compositor.
    //
    // Put re6vr_asym_fov.txt (any content) next to the log to go back to the
    // runtime's per-eye FOV.
    bool shared_fov = true;
    // Midpoint of the two eyes, recomputed from the real per-eye poses by
    // note_eye_positions so the screen is anchored to the player and not to
    // whichever eye happened to draw first.
    XrVector3f real_eye_mid = {0.0f, 0.0f, 0.0f};

    // Re-anchor the screen when the head turns away from it. On by default: a
    // game screen you can lose by turning your head is worse than useless.
    // RE6VR_SCREEN_FOLLOW=0 makes it world-locked instead.
    bool yaw_follow = true;

    // Called by on_frame once both eye poses are known, and by the offscreen
    // tests. Deriving the trim here rather than from a value cached during
    // drawing means the offscreen self-tests exercise the same code the game
    // does - the first version cached it inside the frame path, so in the
    // self-test it silently stayed at zero and could not be calibrated at all.
    void note_eye_positions(const XrVector3f &eye0, const XrVector3f &eye1) {
        real_eye_mid.x = 0.5f * (eye0.x + eye1.x);
        real_eye_mid.y = 0.5f * (eye0.y + eye1.y);
        real_eye_mid.z = 0.5f * (eye0.z + eye1.z);
        converge_offset[0] = eye0.x - real_eye_mid.x;
        converge_offset[1] = eye1.x - real_eye_mid.x;
        apply_auto_uv_shift(converge_offset[0], converge_offset[1]);
    }

    // Auto convergence trim.
    //
    // `head` is the midpoint of the two real eyes. `off_l` / `off_r` are how far
    // each real eye sits from that midpoint, which is what the runtime uses to
    // place the layer: it is handed the real per-eye pose, so the picture
    // separates by exactly this offset. Shifting the sampled texture region by
    // offset / panel_w - in the picture's own units - cancels that separation and
    // puts the two eye images back on top of each other.
    //
    // It has to come from the REAL eye positions, never from the drawing camera:
    // with ipd_scale = 0 the camera is deliberately collapsed onto the midpoint
    // while the submitted pose stays per-eye, so deriving the trim from the
    // camera yields exactly zero - which is precisely the bug that made the first
    // version of this compensation do nothing in the configuration it was
    // written for.
    //
    // Being a pure translation in view space, the required shift does not depend
    // on the viewing distance - only on the eye separation and the panel width.
    // Steps the "sweep" mode: every 3 s (or 360 frames, whichever is longer) the
    // trim moves to the next candidate value and the value is logged, so a single
    // run lets the viewer find the setting that removes the offset and the log
    // says which one it was.
    //
    // The range is deliberately narrow and fine. The quantity being cancelled is
    // the stereo disparity of a panel `screen_dist` away, which for a 63 mm IPD at
    // 4 m is 0.9 deg - about 1.5% of a 60 deg panel, roughly 38 px on a 2492-wide
    // slice. A first version stepped 0 / +-4 / +-8 / +-12 / +-16 %, so it JUMPED
    // OVER the answer: every step was either 0 or at least 2.6x too large. It also
    // ran with the panel showing a flat colour, which cannot show a translation at
    // all - so "no visible change" was guaranteed and meant nothing.
    void advance_uv_sweep() {
        if (!uv_shift_sweep || panel_w <= 1e-4f) return;
        ++sweep_frames;
        LARGE_INTEGER f = {}, now = {};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&now);
        const double elapsed = (sweep_last.QuadPart && f.QuadPart)
                                   ? (double)(now.QuadPart - sweep_last.QuadPart) /
                                         (double)f.QuadPart
                                   : 0.0;
        if (sweep_last.QuadPart && elapsed < 6.0 && sweep_frames < 720) return;
        sweep_last = now;
        sweep_frames = 0;

        // ONE DIRECTION ONLY, and slow. An earlier version alternated sign and
        // returned to 0 between steps, which from inside the headset is
        // indistinguishable from "the picture just slides left and right": each
        // comparison was against a moving baseline, and 3 s per step is not long
        // enough to judge the sharpness of animated text anyway. A single
        // monotonic ramp means the answer is simply "how many steps until the
        // doubling closes up".
        //
        // The range starts at a step of 0.25% of the panel half angle (about 3 px
        // per eye on a 2492-wide slice) and ends at 2.5% (about 31 px per eye),
        // which brackets the geometric answer of +-0.75%.
        static const float steps_pct[] = {0.0f,  -0.25f, -0.5f,  -0.75f, -1.0f, -1.25f,
                                          -1.5f, -1.75f, -2.0f,  -2.25f, -2.5f};
        const int n_steps = (int)(sizeof(steps_pct) / sizeof(steps_pct[0]));
        const int step = sweep_step;
        ++sweep_step;
        if (step >= n_steps) {
            uv_shift_l[0] = uv_shift_r[0] = 0.0f;
            static bool done = false;
            if (!done) {
                done = true;
                VRLOG("converge: sweep finished after %d steps - trim left at 0", step);
            }
            return;
        }
        const float p = steps_pct[step];
        // left eye one way, right eye the other: this changes their separation
        uv_shift_l[0] = p * 0.01f;
        uv_shift_r[0] = -p * 0.01f;
        VRLOG("converge: sweep step %d/%d -> uv shift left %+.5f right %+.5f (+-%.2f%% of "
              "the panel half angle, %.1f px apart at %u wide)", step, n_steps, uv_shift_l[0],
              uv_shift_r[0], p, (uv_shift_r[0] - uv_shift_l[0]) * (float)sc_width, sc_width);
    }

    // Arrow-key control of the trim, for the "keys" mode. Called once per frame
    // from the frame path.
    //
    // The keys are polled with GetAsyncKeyState rather than caught from the
    // window procedure: the game owns the window and its own input handling, and
    // a mod that swallows the player's keys is worse than one that reads them.
    // A latch makes each physical press count once.
    void poll_uv_shift_keys() {
        if (!uv_shift_keys || panel_w <= 1e-4f) return;
        int down = 0;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) down |= 1;   // pictures apart
        if (GetAsyncKeyState('D') & 0x8000) down |= 1;
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) down |= 2;    // pictures together
        if (GetAsyncKeyState('A') & 0x8000) down |= 2;
        if (GetAsyncKeyState('Z') & 0x8000) down |= 4;        // reset
        if (GetAsyncKeyState(VK_UP) & 0x8000) down |= 8;      // right eye up
        if (GetAsyncKeyState('W') & 0x8000) down |= 8;
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) down |= 16;   // right eye down
        if (GetAsyncKeyState('S') & 0x8000) down |= 16;
        if (GetAsyncKeyState('Q') & 0x8000) down |= 32;       // counter-roll one way
        if (GetAsyncKeyState('E') & 0x8000) down |= 64;       // counter-roll the other
        if (GetAsyncKeyState('R') & 0x8000) down |= 128;      // reset roll, scale, position
        if (GetAsyncKeyState(VK_OEM_4) & 0x8000) down |= 256; // '[' one eye smaller
        if (GetAsyncKeyState(VK_OEM_6) & 0x8000) down |= 512; // ']' the other way

        const int pressed = down & ~key_latch;
        key_latch = down;

        // A latch fires once per press, which makes 0.2% a laborious way to cross
        // a degree. Holding a key now repeats after a short delay, like a keyboard
        // does: coarse enough to cover the range quickly, fine enough to stop on
        // the setting that fuses.
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        bool repeat = false;
        if (pressed) {
            key_repeat_from = now.QuadPart;
            key_repeat_armed = true;
        } else if (down && key_repeat_armed && key_repeat_from) {
            const double held = (double)(now.QuadPart - key_repeat_from) / 10000000.0;
            if (held > 0.4) repeat = true;
        } else {
            key_repeat_armed = false;
        }
        const int effective = pressed ? pressed : (repeat ? down : 0);
        if (!effective) return;

        // 0.2% of the panel half angle per press, four times the first setting:
        // about 2.5 px per eye on a 2492-wide slice. The vertical step is half
        // that, because the same percentage of the half angle is a smaller number
        // of pixels vertically where the panel is 16:9.
        const float step_h = 0.002f;
        const float step_v = 0.001f;
        if (effective & 4) {
            uv_shift_l[0] = uv_shift_r[0] = uv_shift_l[1] = uv_shift_r[1] = 0.0f;
            VRLOG("converge: keys -> trim reset to 0");
        }
        if (effective & 1) { uv_shift_l[0] -= step_h; uv_shift_r[0] += step_h; }
        if (effective & 2) { uv_shift_l[0] += step_h; uv_shift_r[0] -= step_h; }
        if (effective & 8) { uv_shift_l[1] += step_v; uv_shift_r[1] -= step_v; }
        if (effective & 16) { uv_shift_l[1] -= step_v; uv_shift_r[1] += step_v; }
        // Roll: opposite directions, so the two pictures rotate towards each other.
        // 0.1 deg per press.
        const float step_roll = 0.001745f;
        if (effective & 128) {
            roll_l = roll_r = 0.0f;
            uv_shift_l[0] = uv_shift_r[0] = uv_shift_l[1] = uv_shift_r[1] = 0.0f;
            VRLOG("converge: keys -> roll and position reset to 0");
        }
        if (effective & 32) { roll_l += step_roll; roll_r -= step_roll; }
        if (effective & 64) { roll_l -= step_roll; roll_r += step_roll; }
        if (effective & (32 | 64 | 128)) {
            VRLOG("converge: keys -> roll L%+.4f R%+.4f rad (relative %.2f deg)",
                  roll_l, roll_r, (roll_l - roll_r) * 57.2957795f);
        }
        // Scale trim: one eye grows while the other shrinks. 0.05% per press is
        // 1.2 px across a 2492-wide slice, which is the scale at which a mismatch
        // becomes visible at the edges.
        const float step_zoom = 0.0005f;
        if (effective & 256) { zoom_l += step_zoom; zoom_r -= step_zoom; }
        if (effective & 512) { zoom_l -= step_zoom; zoom_r += step_zoom; }
        if (effective & 1024) {
            zoom_l = zoom_r = 1.0f;
            VRLOG("converge: keys -> scale reset to 1");
        }
        if (effective & (256 | 512 | 1024)) {
            VRLOG("converge: keys -> scale L%.5f R%.5f (relative %.4f%%)", zoom_l, zoom_r,
                  (zoom_l - zoom_r) * 100.0f);
        }
        if (effective & 31) {
            VRLOG("converge: keys -> trim horizontal L%+.4f R%+.4f (separation %.1f px), "
                  "vertical L%+.4f R%+.4f (separation %.1f px)",
                  uv_shift_l[0], uv_shift_r[0],
                  (uv_shift_r[0] - uv_shift_l[0]) * (float)sc_width,
                  uv_shift_l[1], uv_shift_r[1],
                  (uv_shift_r[1] - uv_shift_l[1]) * (float)sc_height);
        }
    }

    void apply_auto_uv_shift(float off_l, float off_r) {
        if (panel_w <= 1e-4f) return;
        // The sweep and the keyboard both own the trim while they run: they set it
        // before the panel is placed, so the geometry-derived value must not
        // overwrite it here.
        if (uv_shift_sweep || uv_shift_keys) return;
        if (auto_uv_shift == 0.0f) return;
        uv_shift_l[0] = auto_uv_shift * off_l / panel_w;
        uv_shift_r[0] = auto_uv_shift * off_r / panel_w;
        static int s_auto_logs = 0;
        if (s_auto_logs < 4) {
            ++s_auto_logs;
            VRLOG("converge: auto uv shift left %+.4f right %+.4f from eye offsets %+.1f / %+.1f mm "
                  "(panel %.2f m wide) -> %.1f px at %u wide",
                  uv_shift_l[0], uv_shift_r[0], off_l * 1000.0f, off_r * 1000.0f, panel_w,
                  uv_shift_l[0] * (float)sc_width, sc_width);
        }
    }

    // Reads re6vr_test_solid.txt: the panel-diagnostic switch. Split out of
    // on_frame because the offscreen self-tests have to reach the same code -
    // they do not call on_frame, so a marker read only there silently never
    // applied during an offline run.
    void read_test_solid_marker() {
        const wchar_t *log_path = vrlog::path();
        if (!log_path || !log_path[0]) return;
        wchar_t marker[MAX_PATH] = L"";
        wcsncpy_s(marker, MAX_PATH, log_path, _TRUNCATE);
        wchar_t *slash = wcsrchr(marker, L'\\');
        if (!slash) return;
        wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - marker)), L"re6vr_test_solid.txt");
        if (GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES) return;
        char txt[64] = "";
        FILE *f = _wfopen(marker, L"r");
        if (f) {
            if (fscanf_s(f, "%63s", txt, (unsigned)sizeof(txt)) != 1) txt[0] = 0;
            fclose(f);
        }
        uint32_t c = 0;
        if (_stricmp(txt, "pereye") == 0) c = 0xFEEDFACEu;          // left red, right green
        else if (_stricmp(txt, "half") == 0) c = 0x00FACADEu;       // red|green + white line
        else if (_stricmp(txt, "stripes") == 0) c = 0x00ADD1CCu;    // ruler: 24 stripes + 1 red
        else if (_stricmp(txt, "grid") == 0) c = 0x00671D0Cu;       // ruler in both directions
        else if (_stricmp(txt, "cross") == 0) c = 0x00C0FFEEu;      // one horizontal + one vertical bar
        else if (_stricmp(txt, "off") == 0) c = 0;
        else if (_stricmp(txt, "green") == 0) c = 0x0000FF00u;
        else if (_stricmp(txt, "blue") == 0) c = 0x000000FFu;
        else if (_stricmp(txt, "white") == 0) c = 0x00FFFFFFu;
        else if (_stricmp(txt, "magenta") == 0) c = 0x00FF00FFu;
        else if (_stricmp(txt, "cyan") == 0) c = 0x0000FFFFu;
        else if (_stricmp(txt, "yellow") == 0) c = 0x00FFFF00u;
        else if (txt[0]) c = (uint32_t)strtoul(txt, nullptr, 0);
        if (c != 0) {
            test_solid_argb = c;
            VRLOG("test: re6vr_test_solid.txt -> panel will be drawn as 0x%08X%s%s", c,
                  c == 0xFEEDFACEu ? "  [per-eye: left red, right green]" : "",
                  c == 0x00ADD1CCu ? "  [stripe ruler]" : "");
        }
    }

    void place_panel(const XrVector3f &eye, const float fwd[3]) {
        (void)eye;
        (void)fwd;
        read_screen_env();

        // Anchored in front of the player's own position in the LOCAL reference
        // space, which is what the runtime keeps at the player and recentres.
        //
        // Taking the first frame's eye pose put the screen wherever the player
        // happened to be looking while a fullscreen game was starting. Anchoring
        // to the raw reference origin was worse: the origin is not the player, so
        // the screen could end up anywhere at all.
        panel_fwd[0] = 0.0f; panel_fwd[1] = 0.0f; panel_fwd[2] = -1.0f;
        panel_right[0] = 1.0f; panel_right[1] = 0.0f; panel_right[2] = 0.0f;
        panel_up[0] = 0.0f; panel_up[1] = 1.0f; panel_up[2] = 0.0f;
        // The screen sits exactly `screen_dist` in front of the player, and the
        // converging virtual eye is placed the same `screen_dist` back from the
        // panel (see draw_quad), so the player's own position *is* the camera.
        //
        // This used to add a hard-coded 1.6 m here while draw_quad subtracted its
        // own hard-coded 1.6 m, which made the two consistent with each other but
        // left `screen_dist` with no effect on how big the screen looked: it only
        // slid the panel through the world. Measured in the game log as
        // "screen 2.5 m ahead ... panel at 1.60 m". Both now use screen_dist, so
        // RE6VR_SCREEN_DIST sets the real viewing distance and the apparent size.
        panel_center[0] = eye.x;
        panel_center[1] = eye.y;
        panel_center[2] = eye.z - screen_dist;

        panel_placed = true;
        apply_auto_uv_shift(converge_offset[0], converge_offset[1]);
        VRLOG("place_panel: player (%.2f %.2f %.2f) -> screen centre (%.2f %.2f %.2f), "
              "screen %.1f m ahead, %.2f x %.2f m, ipd_scale=%.2f, follow=%d",
              eye.x, eye.y, eye.z, panel_center[0], panel_center[1], panel_center[2],
              screen_dist, panel_w, panel_h, ipd_scale, (int)yaw_follow);
    }

    // Reads a float from a marker file next to the log. Used for the screen geometry, which Steam
    // makes unreachable through the environment: the README documents RE6VR_SCREEN_DIST and
    // RE6VR_SCREEN_WIDTH as the knobs that set the screen's distance and size, but Steam does not pass
    // the shell's environment to the game, so in a real run those two names do nothing. Measured
    // 2026-09-25 by checking this path: the env var was never set, so the screen has only ever been at
    // its compiled default. A knob nobody can turn is worse than no knob, because it reads as "it made
    // no difference".
    bool read_marker_float(const wchar_t *name, float lo, float hi, float *out) {
        const wchar_t *log_path = vrlog::path();
        if (!log_path || !log_path[0] || !out) return false;
        wchar_t marker[MAX_PATH] = L"";
        wcsncpy_s(marker, MAX_PATH, log_path, _TRUNCATE);
        wchar_t *slash = wcsrchr(marker, L'\\');
        if (!slash) return false;
        wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - marker)), name);
        if (GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES) return false;
        FILE *f = _wfopen(marker, L"r");
        if (!f) return false;
        float v = 0.0f;
        const bool ok = fscanf_s(f, "%f", &v) == 1;
        fclose(f);
        if (!ok || v < lo || v > hi) return false;
        *out = v;
        return true;
    }

    void read_screen_env() {
        wchar_t buf[32] = L"";
        if (GetEnvironmentVariableW(L"RE6VR_SCREEN_DIST", buf, 32) > 0) {
            const float v = (float)_wtof(buf);
            if (v > 0.3f && v < 20.0f) screen_dist = v;
        }
        if (GetEnvironmentVariableW(L"RE6VR_SCREEN_WIDTH", buf, 32) > 0) {
            const float v = (float)_wtof(buf);
            if (v > 0.3f && v < 30.0f) {
                panel_w = v;
                panel_h = v * 9.0f / 16.0f;
            }
        }
        if (GetEnvironmentVariableW(L"RE6VR_IPD_SCALE", buf, 32) > 0) {
            const float v = (float)_wtof(buf);
            if (v >= 0.0f && v <= 2.0f) ipd_scale = v;
        }
        if (GetEnvironmentVariableW(L"RE6VR_SCREEN_SCALE", buf, 32) > 0) {
            const float v = (float)_wtof(buf);
            if (v >= 0.05f && v <= 8.0f) screen_scale_user = v;
        }
        // ...and the marker files, which are what a real run can actually use (Steam passes no
        // environment). Announced in the log, because a knob whose effect cannot be seen is the failure
        // this project keeps paying for.
        {
            float v = 0.0f;
            if (read_marker_float(L"re6vr_screen_dist.txt", 0.5f, 30.0f, &v)) {
                screen_dist = v;
                VRLOG("screen: distance %.2f m from re6vr_screen_dist.txt", (double)screen_dist);
            }
            if (read_marker_float(L"re6vr_screen_width.txt", 0.5f, 40.0f, &v)) {
                panel_w = v;
                panel_h = v * 9.0f / 16.0f;
                VRLOG("screen: width %.2f m (height %.2f m) from re6vr_screen_width.txt",
                      (double)panel_w, (double)panel_h);
            }
            if (read_marker_float(L"re6vr_screen_scale.txt", 0.05f, 8.0f, &v)) {
                screen_scale_user = v;
                VRLOG("screen: scale %.2f from re6vr_screen_scale.txt", (double)screen_scale_user);
            }
            VRLOG("screen: effective geometry dist %.2f m, %.2f x %.2f m, scale %.2f -> the picture's "
                  "angular size is set by this, not by how far the eye is",
                  (double)screen_dist, (double)panel_w, (double)panel_h, (double)screen_scale_user);
        }
        // Marker file too: Steam does not pass the shell's environment on, and stereo
        // separation is the one number that has to be testable in a real run - it is
        // the difference between "one screen" and "two images".
        {
            const wchar_t *log_path = vrlog::path();
            if (log_path && log_path[0]) {
                wchar_t marker[MAX_PATH] = L"";
                wcsncpy_s(marker, MAX_PATH, log_path, _TRUNCATE);
                wchar_t *slash = wcsrchr(marker, L'\\');
                if (slash) {
                    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - marker)),
                             L"re6vr_ipd_scale.txt");
                    if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) {
                        FILE *f = _wfopen(marker, L"r");
                        float v = -1.0f;
                        if (f) {
                            if (fscanf_s(f, "%f", &v) != 1) v = -1.0f;
                            fclose(f);
                        }
                        if (v >= 0.0f && v <= 2.0f) {
                            ipd_scale = v;
                            VRLOG("compositor: stereo separation from %ls: ipd_scale=%.2f "
                                  "(0 = both eyes share one viewpoint)", marker, v);
                        }
                    }
                }
            }
        }
        if (GetEnvironmentVariableW(L"RE6VR_SCREEN_FOLLOW", buf, 32) > 0) {
            yaw_follow = _wtoi(buf) != 0;
        }
        // Marker files for the screen's size and distance, which until now were environment
        // variables ONLY - and Steam does not pass the shell's environment on, so those two
        // switches could not be used at all in a real run. `markers.bat dist/width` wrote
        // the files and the compositor silently ignored them, which is why the panel stayed
        // at 4.00 m however the files were set (measured in the log: "panel 4.60 x 2.59 m at
        // 4.00 m" while re6vr_screen_dist.txt held 5.5).
        //
        // Read once per process, like the other markers: this runs from place_panel, which
        // is called more than once per frame.
        static bool s_screen_marker_read = false;
        if (!s_screen_marker_read) {
            s_screen_marker_read = true;
            const wchar_t *log_path = vrlog::path();
            if (log_path && log_path[0]) {
                wchar_t marker[MAX_PATH] = L"";
                wcsncpy_s(marker, MAX_PATH, log_path, _TRUNCATE);
                wchar_t *slash = wcsrchr(marker, L'\\');
                if (slash) {
                    const size_t tail = (size_t)(MAX_PATH - (slash + 1 - marker));
                    wcscpy_s(slash + 1, tail, L"re6vr_screen_dist.txt");
                    if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) {
                        FILE *f = _wfopen(marker, L"r");
                        float v = -1.0f;
                        if (f) {
                            if (fscanf_s(f, "%f", &v) != 1) v = -1.0f;
                            fclose(f);
                        }
                        if (v > 0.3f && v < 20.0f) {
                            screen_dist = v;
                            VRLOG("compositor: screen distance from %ls: %.2f m", marker, v);
                        }
                    }
                    wcscpy_s(slash + 1, tail, L"re6vr_screen_width.txt");
                    if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) {
                        FILE *f = _wfopen(marker, L"r");
                        float v = -1.0f;
                        if (f) {
                            if (fscanf_s(f, "%f", &v) != 1) v = -1.0f;
                            fclose(f);
                        }
                        if (v > 0.3f && v < 30.0f) {
                            panel_w = v;
                            panel_h = v * 9.0f / 16.0f;
                            VRLOG("compositor: screen width from %ls: %.2f m", marker, v);
                        }
                    }
                    // Apparent size on the display, as a multiplier. Marker file as
                    // well as the environment: this is a number that has to be dialled
                    // in from inside the headset, and Steam does not pass the shell's
                    // environment on.
                    wcscpy_s(slash + 1, tail, L"re6vr_screen_scale.txt");
                    if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) {
                        FILE *f = _wfopen(marker, L"r");
                        float v = -1.0f;
                        if (f) {
                            if (fscanf_s(f, "%f", &v) != 1) v = -1.0f;
                            fclose(f);
                        }
                        if (v >= 0.05f && v <= 8.0f) {
                            screen_scale_user = v;
                            VRLOG("compositor: screen scale from %ls: %.2f x", marker, v);
                        }
                    }
                }
            }
        }
        // Marker file too, for the same reason as ipd_scale above: Steam does not pass
        // the shell's environment on, and this is the switch that has to be turned OFF to
        // test head look cleanly. When the head turns past about 45 degrees this re-anchors
        // the screen to straight ahead, which moves the whole panel and masks the scene
        // rotation the head-look experiment is trying to show.
        {
            const wchar_t *log_path = vrlog::path();
            if (log_path && log_path[0]) {
                wchar_t marker[MAX_PATH] = L"";
                wcsncpy_s(marker, MAX_PATH, log_path, _TRUNCATE);
                wchar_t *slash = wcsrchr(marker, L'\\');
                if (slash) {
                    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - marker)),
                             L"re6vr_screen_follow.txt");
                    if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) {
                        FILE *f = _wfopen(marker, L"r");
                        int v = -1;
                        if (f) {
                            if (fscanf_s(f, "%d", &v) != 1) v = -1;
                            fclose(f);
                        }
                        if (v == 0 || v == 1) {
                            yaw_follow = (v != 0);
                            VRLOG("compositor: screen follow from %ls: yaw_follow=%d",
                                  marker, (int)yaw_follow);
                        }
                    }
                }
            }
        }
        // Per-eye horizontal convergence trim. Marker file only: it is a number
        // that has to be dialled in while wearing the headset, and Steam does not
        // pass the shell's environment on. Contents, one line:
        //
        //     auto            cancel the geometry-derived eye separation (default sign)
        //     auto-           same, opposite sign
        //     sweep           step through a range of values while the game runs
        //     keys            drive the trim with the arrow keys, live
        //     <l> <r>         explicit, in percent of the panel's half angle
        //     0 0             off
        //
        // Read once per process. This is called from place_panel, which runs more
        // than once per frame, and re-reading each time threw away whatever the
        // keyboard had just set - "the keys do nothing" is exactly what that looks
        // like from the headset.
        static bool s_uv_marker_read = false;
        if (!s_uv_marker_read) {
            const wchar_t *log_path = vrlog::path();
            if (log_path && log_path[0]) {
                wchar_t marker[MAX_PATH] = L"";
                wcsncpy_s(marker, MAX_PATH, log_path, _TRUNCATE);
                wchar_t *slash = wcsrchr(marker, L'\\');
                if (slash) {
                    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - marker)),
                             L"re6vr_uv_shift.txt");
                    if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) {
                        char txt[128] = "";
                        FILE *f = _wfopen(marker, L"r");
                        if (f) {
                            // The WHOLE line, not one whitespace-delimited token: the
                            // explicit form is "<left> <right>", and reading a single
                            // token threw the second number away and then defaulted it
                            // to the first, so "0.75 -0.75" silently became
                            // "0.75 0.75" - two shifts in the same direction, which
                            // is a no-op for the separation it was meant to change.
                            if (!fgets(txt, (int)sizeof(txt), f)) txt[0] = 0;
                            fclose(f);
                            for (char *p = txt; *p; ++p) {
                                if (*p == '\n' || *p == '\r') { *p = 0; break; }
                            }
                        }
                        if (_strnicmp(txt, "auto", 4) == 0) {
                            auto_uv_shift = (txt[4] == '-') ? -1.0f : 1.0f;
                            uv_shift_set = true;
                            VRLOG("compositor: per-eye uv shift from %ls: auto (sign %+.0f), "
                                  "recomputed from the eye separation", marker, auto_uv_shift);
                        } else if (_strnicmp(txt, "sweep", 5) == 0) {
                            uv_shift_sweep = true;
                            uv_shift_set = true;
                            VRLOG("compositor: per-eye uv shift from %ls: SWEEP, stepping the "
                                  "trim every 6 s in ONE direction so the step where the "
                                  "doubling closes up can be read off a running game", marker);
                        } else if (_strnicmp(txt, "keys", 4) == 0) {
                            uv_shift_keys = true;
                            uv_shift_set = true;
                            VRLOG("compositor: per-eye uv shift from %ls: KEYBOARD - press the "
                                  "LEFT or RIGHT arrow (or A / D) to bring the two pictures "
                                  "towards each other, and Z to zero it. The panel shows the "
                                  "current trim as a bar, and every change is logged.", marker);
                        } else if (txt[0]) {
                            // "<left_h> <right_h>" or, for the other two axes,
                            // "<left_h> <right_h> <left_v> <right_v> <roll_deg>".
                            // Two numbers keep meaning "horizontal only", so every
                            // earlier note about this file still holds.
                            float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f, e = 0.0f;
                            const int got = sscanf_s(txt, "%f %f %f %f %f", &a, &b, &c, &d, &e);
                            if (got < 2) b = a;
                            uv_shift_l[0] = a * 0.01f;
                            uv_shift_r[0] = b * 0.01f;
                            uv_shift_l[1] = c * 0.01f;
                            uv_shift_r[1] = d * 0.01f;
                            if (got >= 5) {
                                const float rad = e * 0.0174532925f;
                                roll_l = rad;
                                roll_r = -rad;
                            }
                            auto_uv_shift = 0.0f;
                            if (!uv_shift_set) {
                                uv_shift_set = true;
                                VRLOG("compositor: per-eye trim from %ls: horizontal %.3f%% / "
                                      "%.3f%%, vertical %.3f%% / %.3f%%, roll %.3f deg (relative "
                                      "%.3f deg)", marker, a, b, c, d, e);
                            }
                        }
                    }
                }
            }
            s_uv_marker_read = true;
        }
        if (GetEnvironmentVariableW(L"RE6VR_FRUSTUM_ASPECT_MODE", buf, 32) > 0) {
            const float v = (float)_wtof(buf);
            if (v >= 0.0f && v <= 1.0f) frustum_aspect_mode = v;
        }
    }

    // Puts the screen in front of `eye` along `fwd` (yaw only). Used only by the
    // follow mode, after the head has turned well away from the screen.
    void place_panel_at(const XrVector3f &eye, const float fwd[3]) {
        float fx = fwd[0], fz = fwd[2];
        const float len = sqrtf(fx * fx + fz * fz);
        if (len < 1e-4f) {
            fx = 0.0f;
            fz = -1.0f;
        } else {
            fx /= len;
            fz /= len;
        }
        panel_fwd[0] = fx;    panel_fwd[1] = 0.0f; panel_fwd[2] = fz;
        panel_right[0] = -fz; panel_right[1] = 0.0f; panel_right[2] = fx;
        panel_up[0] = 0.0f;   panel_up[1] = 1.0f;  panel_up[2] = 0.0f;
        panel_center[0] = eye.x + fx * screen_dist;
        panel_center[1] = eye.y;
        panel_center[2] = eye.z + fz * screen_dist;
        apply_auto_uv_shift(converge_offset[0], converge_offset[1]);
        VRLOG("compositor: screen re-anchored in front of the player");
    }

    // Called when the session regains focus, so taking the headset off and putting
    // it back on re-centres the screen instead of leaving it behind the player.
    void recenter_panel() { panel_placed = false; }

    // Draws the game frame onto that screen for one eye.
    bool draw_quad(IDirect3DDevice9 *d3d9_dev, uint32_t image_index, uint32_t eye_index,
                   const XrPosef &eye_pose, const XrFovf &eye_fov) {
        (void)d3d9_dev;

        // 2) draw the panel into this eye's slice of this swapchain image.
        //
        // The slot layout depends on which of the two legal swapchain layouts is
        // in use, because the render target views are created in that order:
        //
        //   shared:  one array swapchain -> rtv[image * view_count + eye]
        //   two:     one swapchain per eye -> rtv[eye * image_count + image]
        //
        // Using the shared formula for both silently sent eye 1's draw to a
        // render target view belonging to the wrong swapchain image, which is
        // exactly the shape of the "capcapcom" fault: both eyes individually
        // plausible, the pair offset from each other.
        size_t slot = 0;
        if (sc_images_shared) {
            slot = (size_t)image_index * (view_count ? view_count : 1) + eye_index;
        } else {
            slot = (size_t)eye_index * (sc_image_count ? sc_image_count : 1) + image_index;
        }
        if (slot >= sc_rtvs.size() || !sc_rtvs[slot]) {
            warn_once("swapchain render target view");
            return false;
        }
        ID3D11RenderTargetView *rtv = sc_rtvs[slot];
        // Same audit as the submit log. Only pointers already validated above are
        // touched here: an earlier version also dereferenced sc_images[] by index
        // to print the texture, and the offscreen self-test leaves those vectors
        // in a different state, which turned the log line into an access
        // violation. Diagnostics must not be able to crash the path they observe.
        {
            static int s_rtv_logs = 0;
            if (s_rtv_logs < 6) {
                ++s_rtv_logs;
                VRLOG("draw: layout=%s eye%u image=%u -> rtv slot %u of %u (%p)",
                      sc_images_shared ? "shared-array" : "two-swapchains", eye_index,
                      image_index, (unsigned)slot, (unsigned)sc_rtvs.size(), (void *)rtv);
            }
        }
        std::lock_guard<std::recursive_mutex> guard(d11_lock);

        const float q[4] = {eye_pose.orientation.x, eye_pose.orientation.y,
                            eye_pose.orientation.z, eye_pose.orientation.w};
        const float right[3] = {1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]),
                                2.0f * (q[0] * q[1] + q[3] * q[2]),
                                2.0f * (q[0] * q[2] - q[3] * q[1])};
        const float up[3] = {2.0f * (q[0] * q[1] - q[3] * q[2]),
                             1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]),
                             2.0f * (q[1] * q[2] + q[3] * q[0])};
        const float fwd[3] = {-2.0f * (q[0] * q[2] + q[3] * q[1]),
                              -2.0f * (q[1] * q[2] - q[3] * q[0]),
                              -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]))};
        // The convergence point both eyes share, placed *in front of* the panel.
        //
        // Eye separation going to zero is what removes the double image, but the
        // converging eye still has to sit at a sane distance with the panel ahead
        // of it. An earlier attempt collapsed the eye onto the panel centre
        // itself, which put the "eye" on the panel plane facing away from it: the
        // whole quad then landed behind the near plane and nothing was drawn at
        // all (the exported eye images were pure black).

        // Place the screen before anything is derived from it. This call had been
        // dropped by an earlier edit, which left panel_center at its initial
        // (0,0,0) while the eye sat near the origin too: the projection collapsed
        // to w = 0 and the composited image was pure black in both eyes.
        const XrVector3f real_eye = eye_pose.position;

        // The player's own position, as the midpoint of the two eyes.
        //
        // The screen is anchored to this rather than to whichever eye drew
        // first. Anchoring to one eye put the screen half an IPD off centre,
        // which with ipd_scale = 0 (camera at the midpoint) shifted the picture
        // sideways in *both* eyes and made the re-anchored distance differ from
        // the placed one - visible in the log as the panel jumping between
        // 4.00 m and 3.11 m.
        //
        // The real pair is recorded in draw_quad's caller (note_eye_positions);
        // all this needs is the midpoint, and a single eye is the best estimate
        // available when only one has been seen.
        real_eye_mid = real_eye;

        // 1. Place the screen if it is not placed yet, or re-anchor it after the
        //    player has turned well away from it.
        //
        //    This call has twice been lost to a careless scripted edit, and both
        //    times the symptom was the same: panel_center stays at its initial
        //    (0,0,0), the projection degenerates and the headset shows nothing.
        //    If the screen ever goes black again, check here first.
        if (!panel_placed) {
            place_panel(real_eye_mid, fwd);
        } else if (yaw_follow) {
            const float len2 = fwd[0] * fwd[0] + fwd[2] * fwd[2];
            if (len2 > 1e-4f) {
                const float fl = sqrtf(len2);
                const float dot = (panel_fwd[0] * fwd[0] + panel_fwd[2] * fwd[2]) / fl;
                if (dot < 0.7f) place_panel_at(real_eye_mid, fwd);   // ~45 degrees
            }
        }

        // 2. One convergence point for both eyes, `screen_dist` metres in front of
        //    the screen plane - which is where the player is, so the virtual camera
        //    and the head coincide and the screen is seen from screen_dist away.
        //
        //    Both eyes sharing a viewpoint is what removes the double image: a flat
        //    screen carries no stereo depth, only an offset. The setback matters -
        //    converging exactly onto the panel makes its view-space depth zero and
        //    the frustum then cannot describe it, which is a black screen. Deriving
        //    it from screen_dist instead of a constant is what makes
        //    RE6VR_SCREEN_DIST control the screen's apparent size.
        const float eye_setback = screen_dist;
        XrVector3f eye;
        {
            const float cx = panel_center[0] - panel_fwd[0] * eye_setback;
            const float cy = panel_center[1] - panel_fwd[1] * eye_setback;
            const float cz = panel_center[2] - panel_fwd[2] * eye_setback;
            if (ipd_scale <= 0.0f) {
                eye.x = cx; eye.y = cy; eye.z = cz;
            } else if (ipd_scale >= 1.0f && ipd_scale < 1.5f) {
                eye = real_eye;
            } else if (ipd_scale >= 1.5f) {
                // >= 1.5 is the "converged" diagnostic: the camera goes to the
                // shared panel axis even when the two real eyes are apart, and
                // on_frame then reports both views at that same pose. If the
                // double image survives this, the runtime is not deriving the
                // separation from the pose we submit.
                eye.x = cx; eye.y = cy; eye.z = cz;
                converge_views = true;
            } else {
                eye.x = cx + (real_eye.x - cx) * ipd_scale;
                eye.y = cy + (real_eye.y - cy) * ipd_scale;
                eye.z = cz + (real_eye.z - cz) * ipd_scale;
            }
        }

        static int s_eye_logs = 0;
        if (s_eye_logs < 4) {
            ++s_eye_logs;
            VRLOG("converge eye%u: player (%.2f %.2f %.2f) -> eye (%.2f %.2f %.2f), "
                  "screen (%.2f %.2f %.2f), ipd_scale=%.2f",
                  eye_index, real_eye.x, real_eye.y, real_eye.z, eye.x, eye.y, eye.z,
                  panel_center[0], panel_center[1], panel_center[2], ipd_scale);
        }

        // Screen -> world. Translation in the last row, basis vectors in the rows:
        // p_world = p_local.x * row0 + p_local.y * row1 + p_local.z * row2 + row3.
        float world[16] = {0};
        world[0] = panel_right[0]; world[1] = panel_right[1]; world[2] = panel_right[2];
        world[4] = panel_up[0];    world[5] = panel_up[1];    world[6] = panel_up[2];
        world[8] = panel_fwd[0];   world[9] = panel_fwd[1];   world[10] = panel_fwd[2];
        world[12] = panel_center[0]; world[13] = panel_center[1]; world[14] = panel_center[2];
        world[15] = 1.0f;

        // World -> eye. +z is forward (left-handed), so the third column is +fwd.
        float view[16] = {0};
        view[0] = right[0]; view[4] = right[1]; view[8] = right[2];
        view[1] = up[0];    view[5] = up[1];    view[9] = up[2];
        view[2] = fwd[0];   view[6] = fwd[1];   view[10] = fwd[2];
        view[12] = -(right[0] * eye.x + right[1] * eye.y + right[2] * eye.z);
        view[13] = -(up[0] * eye.x + up[1] * eye.y + up[2] * eye.z);
        view[14] = -(fwd[0] * eye.x + fwd[1] * eye.y + fwd[2] * eye.z);
        view[15] = 1.0f;

        // The eye's own asymmetric frustum, as a proper off-centre projection: the
        // shear sign matters now. It used to be flipped to cancel the mirrored
        // panel placement; with the screen out in the world it has to be the plain
        // off-centre frustum, or the screen sits off to one side.
        // Frustum derived from the panel rectangle and the convergence point,
        // not from the runtime's per-eye FOV. Both eyes now share one convergence
        // point, so the panel's angular size is identical for both and the two
        // images land on exactly the same pixels.

        float view_panel[3];
        view_panel[0] = panel_center[0] * view[0] + panel_center[1] * view[4] +
                        panel_center[2] * view[8] + view[12];
        view_panel[1] = panel_center[0] * view[1] + panel_center[1] * view[5] +
                        panel_center[2] * view[9] + view[13];
        view_panel[2] = panel_center[0] * view[2] + panel_center[1] * view[6] +
                        panel_center[2] * view[10] + view[14];

        // If this ever clamps, the panel is at or behind the eye and the frustum
        // is meaningless - say so rather than silently drawing nothing.
        if (view_panel[2] < 0.25f) {
            warn_once("panel too close to the eye; frustum clamped (screen will be wrong)");
        }
        const float panel_z = view_panel[2] > 0.25f ? view_panel[2] : 0.25f;

        // Per-eye convergence trim, expressed in the picture's own units. The
        // caller sets uv_shift_* in percent of the panel's half angle; a shift of
        // s * half_tan in tangent units is s * half_tan * panel_z / panel_w of the
        // picture, which is what the sampler needs. The vertical half angle comes
        // from the panel's height, so "1%" means the same angle either way even
        // though the panel is 16:9. Doing the conversion here keeps the number in
        // the marker file independent of the screen size and the viewing distance.
        {
            const int e = eye_index < 2 ? (int)eye_index : 0;
            const float pct_h = (e == 0) ? uv_shift_l[0] : uv_shift_r[0];
            const float pct_v = (e == 0) ? uv_shift_l[1] : uv_shift_r[1];
            // Simplifies to pct * 0.5: a trim of 1% of the panel half angle moves
            // the sampled window by 0.5% of the picture, whatever the panel size
            // and the viewing distance are.
            eye_uv_shift[e][0] = pct_h * 0.5f;
            eye_uv_shift[e][1] = pct_v * 0.5f;
            if (pct_h != 0.0f || pct_v != 0.0f) {
                static int s_uv_logs = 0;
                if (s_uv_logs < 6) {
                    ++s_uv_logs;
                    VRLOG("converge: eye%u uv shift (%.3f, %.3f) = (%.2f%%, %.2f%%) of the "
                          "panel half angle -> (%.1f, %.1f) px in a %ux%u slice", e,
                          eye_uv_shift[e][0], eye_uv_shift[e][1], pct_h * 100.0f,
                          pct_v * 100.0f, eye_uv_shift[e][0] * (float)sc_width,
                          eye_uv_shift[e][1] * (float)sc_height, sc_width, sc_height);
                }
            }
        }
        float tan_l = (view_panel[0] - panel_w * 0.5f) / panel_z;
        float tan_r = (view_panel[0] + panel_w * 0.5f) / panel_z;
        float tan_d = (view_panel[1] - panel_h * 0.5f) / panel_z;
        float tan_u = (view_panel[1] + panel_h * 0.5f) / panel_z;

        // Mode 1: widen the frustum to the swapchain slice's own aspect ratio, so
        // the slice's pixels are isotropic and the panel keeps its 16:9 shape while
        // covering only the middle of the slice. Mode 0 leaves the frustum equal to
        // the panel rectangle, which is what makes the submitted region 16:9.
        if (frustum_aspect_mode >= 0.5f && sc_height > 0) {
            const float vp_aspect = (float)sc_width / (float)sc_height;
            const float off_x = (tan_l + tan_r) * 0.5f;
            const float off_y = (tan_d + tan_u) * 0.5f;
            float w = tan_r - tan_l, h = tan_u - tan_d;
            if (w / h >= vp_aspect) {
                h = w / vp_aspect;          // panel is the wider one: grow vertically
            } else {
                w = h * vp_aspect;          // panel is the taller one: grow horizontally
            }
            tan_l = off_x - w * 0.5f;
            tan_r = off_x + w * 0.5f;
            tan_d = off_y - h * 0.5f;
            tan_u = off_y + h * 0.5f;
        }

        // Report the angular size being submitted. This is the number that decides
        // how much of the panel the headset actually gets to show: the runtime maps
        // this region onto the display, so a region far larger than the panel leaves
        // the game screen sitting in a mostly black view.
        static int s_fov_logs = 0;
        if (s_fov_logs < 8) {
            ++s_fov_logs;
            const float deg = 57.2957795f;
            const float h_deg = (atanf(tan_r) - atanf(tan_l)) * deg;
            const float v_deg = (atanf(tan_u) - atanf(tan_d)) * deg;
            VRLOG("frustum eye%u: submitting %.1f x %.1f deg (mode %.0f), panel %.2f x %.2f m "
                  "at %.2f m, slice %ux%u aspect %.3f, submitted tan-aspect %.3f",
                  eye_index, h_deg, v_deg, frustum_aspect_mode, panel_w, panel_h, panel_z,
                  sc_width, sc_height, sc_height ? (float)sc_width / (float)sc_height : 0.0f,
                  (tan_r - tan_l) / (tan_u - tan_d));
        }

        const float near_z = 0.05f, far_z = 50.0f;
        float proj[16] = {0};
        proj[0] = 2.0f / (tan_r - tan_l);
        proj[5] = 2.0f / (tan_u - tan_d);
        proj[8] = -(tan_r + tan_l) / (tan_r - tan_l);
        proj[9] = -(tan_u + tan_d) / (tan_u - tan_d);
        proj[10] = far_z / (far_z - near_z);
        proj[11] = 1.0f;
        proj[14] = -(far_z * near_z) / (far_z - near_z);

        struct {
            float view_proj[16];
            float scale_bias[4];
            float uv_shift[4];
            float rot_zoom[4];
        } cb;
        float tmp[16];
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float acc = 0.0f;
                for (int k = 0; k < 4; ++k) acc += world[r * 4 + k] * view[k * 4 + c];
                tmp[r * 4 + c] = acc;
            }
        }
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float acc = 0.0f;
                for (int k = 0; k < 4; ++k) acc += tmp[r * 4 + k] * proj[k * 4 + c];
                cb.view_proj[r * 4 + c] = acc;
            }
        }
        cb.scale_bias[0] = panel_w * 0.5f;
        cb.scale_bias[1] = panel_h * 0.5f;
        cb.scale_bias[2] = 0.0f;
        cb.scale_bias[3] = 0.0f;
        cb.uv_shift[0] = eye_uv_shift[eye_index < 2 ? eye_index : 0][0];
        cb.uv_shift[1] = eye_uv_shift[eye_index < 2 ? eye_index : 0][1];
        cb.uv_shift[2] = 0.0f;
        cb.uv_shift[3] = 0.0f;
        {
            const int e = eye_index < 2 ? (int)eye_index : 0;
            const float roll = (e == 0) ? roll_l : roll_r;
            cb.rot_zoom[0] = roll;
            // The zoom has two jobs. One is to keep a rotated panel covering its
            // own frustum (a rotated rectangle does not fill it): |sin| + |cos|
            // times the half extents is what it needs. The other is the per-eye
            // scale trim, which is the fifth degree of freedom a stereo pair can
            // need and the one that looks most like "impossible to adjust": if the
            // two pictures differ in magnification they can be aligned at the
            // centre or at an edge, never both, and no amount of translation
            // helps.
            const float a = fabsf(roll);
            cb.rot_zoom[1] = (1.0f + a * 0.6f) * ((e == 0) ? zoom_l : zoom_r);
            cb.rot_zoom[2] = 0.0f;
            cb.rot_zoom[3] = 0.0f;
        }

        // Self-check: project the panel's corners through the matrix we are about
        // to hand the shader. If the panel is rendered black or not at all, this
        // says whether it landed off-screen, behind the camera, or was never
        // transformed - all of which look identical in the headset.
        static int s_vp_logs = 0;
        if (s_vp_logs < 3) {
            ++s_vp_logs;
            const float hw = panel_w * 0.5f, hh = panel_h * 0.5f;
            const float corners[4][3] = {
                {-hw, +hh, 0.0f}, {+hw, +hh, 0.0f}, {+hw, -hh, 0.0f}, {-hw, -hh, 0.0f}};
            for (int ci = 0; ci < 4; ++ci) {
                // world = local * world-basis, then through view*proj
                const float lx = corners[ci][0], ly = corners[ci][1], lz = corners[ci][2];
                const float wx = lx * world[0] + ly * world[4] + lz * world[8] + world[12];
                const float wy = lx * world[1] + ly * world[5] + lz * world[9] + world[13];
                const float wz = lx * world[2] + ly * world[6] + lz * world[10] + world[14];
                const float vx = wx * cb.view_proj[0] + wy * cb.view_proj[4] + wz * cb.view_proj[8] +
                                 cb.view_proj[12];
                const float vy = wx * cb.view_proj[1] + wy * cb.view_proj[5] + wz * cb.view_proj[9] +
                                 cb.view_proj[13];
                const float vw = wx * cb.view_proj[3] + wy * cb.view_proj[7] + wz * cb.view_proj[11] +
                                 cb.view_proj[15];
                VRLOG("quad check eye%u corner%d ndc=(%.2f %.2f) w=%.2f", eye_index, ci,
                      vw != 0.0f ? vx / vw : 0.0f, vw != 0.0f ? vy / vw : 0.0f, vw);
            }
            VRLOG("quad check: panel centre (%.2f %.2f %.2f) eye (%.2f %.2f %.2f)",
                  panel_center[0], panel_center[1], panel_center[2], eye.x, eye.y, eye.z);
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(d11_context->Map(quad_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, &cb, sizeof(cb));
            d11_context->Unmap(quad_cb, 0);
        }

        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        d11_context->ClearRenderTargetView(rtv, clear);        d11_context->OMSetRenderTargets(1, &rtv, nullptr);

        // A solid-colour test: when set, the panel shows this colour instead of the
        // game frame.
        //
        // The value 0xFEEDFACE is special: the LEFT eye is drawn solid red and the
        // RIGHT eye solid green. One colour for both eyes cannot answer the question
        // this test exists for - "are both eyes handed the same picture?" - because a
        // single colour looks identical whichever eye it came from. Two colours make
        // it readable at a glance:
        //
        //   merged yellow  = each eye gets its own colour (left red + right green)
        //   red + green in the SAME eye, or two separate bands = one eye is being
        //                    given the other eye's image as well
        //   all one colour = both eyes are getting the same slice
        ID3D11ShaderResourceView *srv_to_use = frame_srv_for_eye(eye_index);
        ID3D11Texture2D *solid_tex = nullptr;
        uint32_t solid_rgb = test_solid_argb;   // 0x00RRGGBB, channels by name
        if (solid_rgb == 0xFEEDFACEu) {
            solid_rgb = (eye_index == 0) ? 0x00FF0000u : 0x0000FF00u;   // left red, right green
        }
        // A pattern instead of a flat colour: "half" builds a 64x2 texture of
        // left-half red / right-half green with a white line down the middle. A flat
        // colour cannot show whether the picture is being drawn more than once -
        // copies of a flat colour look exactly like one copy. This pattern can:
        //
        //   two bands (red | green)      = the eye image is drawn exactly once
        //   four bands (red green red green) = drawn twice, side by side
        //   any repeat of the white line  = the picture is being tiled horizontally
        const bool want_pattern = (solid_rgb == 0x00FACADEu);
        if (want_pattern) solid_rgb = 0;    // handled by the pattern texture below
        // "stripes": a ruler burned into the panel. 24 black/white stripes across
        // the picture, one of them bright magenta, with a thin magenta line one
        // stripe before it so the middle is identifiable without counting. Unlike
        // a flat colour or a two-tone split, a ruler makes a sub-degree horizontal
        // offset visible: the eye's own span is 59.8 deg, so the stripes are
        // 2.49 deg apart and a shift shows up as a red/green fringe along every
        // boundary.
        const bool want_stripes = (solid_rgb == 0x00ADD1CCu);
        if (want_stripes) solid_rgb = 0;
        // "grid": the same ruler, but two-dimensional. A pattern with vertical
        // edges only can say that something is doubled, never which way: the
        // answer "the whole picture just slides left and right" is equally
        // consistent with a horizontal offset, a vertical one and a scale
        // difference. A grid separates them at a glance:
        //
        //   vertical edges doubled, horizontal single  -> horizontal offset
        //   horizontal edges doubled, vertical single  -> vertical offset
        //   edges doubled away from the centre, single at the centre -> scale
        //   everything doubled along one diagonal      -> a rotation or a roll
        const bool want_grid = (solid_rgb == 0x00671D0Cu);
        if (want_grid) solid_rgb = 0;
        // "cross": one white horizontal bar and one white vertical bar on black.
        // The question it answers needs no vocabulary: when the two bars are
        // doubled, is the copy of the HORIZONTAL bar displaced sideways? Is the
        // copy of the VERTICAL bar displaced up or down? A sideways-only
        // displacement is what a horizontal shift can cancel; a vertical one is
        // not, which is why 24 horizontal shift steps changed nothing.
        const bool want_cross = (solid_rgb == 0x00C0FFEEu);
        if (want_cross) solid_rgb = 0;
        if (solid_rgb != 0 || want_pattern || want_stripes || want_grid || want_cross) {
            D3D11_TEXTURE2D_DESC sdt = {};
            sdt.Width = (want_pattern || want_stripes || want_cross) ? 64u : (want_grid ? 64u : 2u);
            // The grid needs real vertical resolution. A 64x2 texture spanning the
            // panel gets magnified about 1000x along its 2 rows, and the sampler
            // cannot resolve a rule in that direction - the first attempt rendered
            // one smeared white band instead of three rules.
            sdt.Height = (want_grid || want_cross) ? 64u : 2u;
            sdt.MipLevels = 1;
            sdt.ArraySize = 1;
            sdt.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sdt.SampleDesc.Count = 1;
            sdt.Usage = D3D11_USAGE_DYNAMIC;
            sdt.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            sdt.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (SUCCEEDED(d11_device->CreateTexture2D(&sdt, nullptr, &solid_tex)) && solid_tex) {
                D3D11_MAPPED_SUBRESOURCE sm = {};
                if (SUCCEEDED(d11_context->Map(solid_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &sm))) {
                    // Written into a B8G8R8A8_UNORM texel, which in memory is
                    // bb,gg,rr,aa - so the uint32 is 0xAARRGGBB when read as a number.
                    // Channels are taken by name from the 0x00RRGGBB value; an earlier
                    // version shifted between two unnamed conventions and produced
                    // 0x000000FF for "red", which the headset showed as blue.
                    auto texel = [](uint32_t rgb) {
                        const uint32_t r = (rgb >> 16) & 0xFFu;
                        const uint32_t g = (rgb >> 8) & 0xFFu;
                        const uint32_t b = rgb & 0xFFu;
                        return 0xFF000000u | (r << 16) | (g << 8) | b;
                    };
                    for (int y = 0; y < (int)sdt.Height; ++y) {
                        uint32_t *row = reinterpret_cast<uint32_t *>((uint8_t *)sm.pData +
                                                                     (size_t)y * sm.RowPitch);
                        if (want_stripes) {
                            // A ruler across the picture. 24 stripes, so the stripe
                            // period is 2.49 deg at the eye's true 59.8 deg span;
                            // that is fine enough that a sub-degree horizontal
                            // offset between the eyes shows up as a red/green
                            // fringe along every boundary, and coarse enough to
                            // still be countable.
                            //
                            // One stripe is bright magenta instead of white, with a
                            // thin magenta line one stripe before it, so the middle
                            // of the ruler is identifiable without counting from
                            // the edge.
                            //
                            // Crossing positions are derived from the texel centre
                            // ((x + 0.5) / 64) so the boundary lands on a texel
                            // boundary instead of half a texel off it.
                            for (UINT x = 0; x < 64; ++x) {
                                const float u = ((float)x + 0.5f) / 64.0f;
                                const float p = u * 24.0f;
                                const bool stripe = (((int)p) % 2) != 0;
                                row[x] = texel(stripe ? 0x00FFFFFFu : 0x00000000u);
                            }
                            for (UINT x = 15; x <= 16 && x < 64; ++x) row[x] = texel(0x00FF00FFu);
                            for (UINT x = 19; x <= 20 && x < 64; ++x) row[x] = texel(0x00FF00FFu);
                        } else if (want_grid) {
                            // White rules on black, in both directions, in a 64x64
                            // texture: 24 divisions across the width, 8 across the
                            // height (every 8 texels), giving rules roughly 2.5 deg
                            // and 4.9 deg apart on the panel.
                            {
                                const float v = ((float)y + 0.5f) / 64.0f;
                                const bool h_rule = (((int)(v * 8.0f)) % 2) == 0;
                                for (UINT x = 0; x < 64; ++x) row[x] = texel(h_rule ? 0x00FFFFFFu : 0x00000000u);
                                for (UINT x = 0; x < 64; ++x) {
                                    const float u = ((float)x + 0.5f) / 64.0f;
                                    if ((((int)(u * 24.0f)) % 2) == 0) row[x] = texel(0x00FFFFFFu);
                                }
                            }
                        } else if (want_cross) {
                            // One horizontal bar and one vertical bar, both through the
                            // middle, on black. Thin enough (8 of 64 texels) that a
                            // displacement of a fraction of a degree shows as a second
                            // bar beside the first.
                            //
                            // The bottom rows carry a readout of the current trim: a
                            // white bar whose length grows with the trim, and a marker
                            // on the exact centre so the count can be verified against
                            // the log. Being ON the panel matters - the viewer is
                            // wearing the headset, so anything shown on the monitor is
                            // invisible to them.
                            const bool h_bar = (y >= 28 && y <= 35);
                            // Tinted per eye: red for one, green for the other. Two
                            // identical white crosses cannot say WHICH eye each one
                            // belongs to, and with an offset still present that is
                            // exactly what the viewer needs to know - a red bar and a
                            // green bar side by side say "these are the two eyes, and
                            // this is how far apart they are", and the moment they
                            // overlap the bar goes yellow.
                            const uint32_t tint = (eye_index == 0) ? 0x00FF4040u : 0x0040FF40u;
                            for (UINT x = 0; x < 64; ++x) {
                                const bool v_bar = (x >= 28 && x <= 35);
                                row[x] = texel((h_bar || v_bar) ? tint : 0x00000000u);
                            }
                            if (y >= 58) {
                                // Two readouts: the top band of the strip is the
                                // horizontal trim, the lower one the vertical trim.
                                // A red pair of texels marks zero, so a glance says
                                // which side of zero each one is on.
                                const bool horizontal_band = (y < 61);
                                const float trim_pct =
                                    horizontal_band
                                        ? (uv_shift_l[0] - uv_shift_r[0]) * 50.0f
                                        : (uv_shift_l[1] - uv_shift_r[1]) * 50.0f;
                                const float frac = fabsf(trim_pct) / 5.0f;
                                const UINT fill = (UINT)(frac * 64.0f + 0.5f);
                                for (UINT x = 0; x < 64; ++x) {
                                    row[x] = texel((x < fill && x < 62) ? 0x00FFFFFFu
                                                                        : 0x00202020u);
                                }
                                row[31] = row[32] = texel(0x00FF0000u);
                            }
                        } else if (!want_pattern) {
                            row[0] = texel(solid_rgb);
                            row[1] = texel(solid_rgb);
                        } else {
                            for (UINT x = 0; x < 64; ++x) {
                                const bool middle = (x == 31 || x == 32);
                                row[x] = texel(middle ? 0x00FFFFFFu
                                                      : (x < 32 ? 0x00FF0000u : 0x0000FF00u));
                            }
                        }
                    }
                    d11_context->Unmap(solid_tex, 0);
                    ID3D11ShaderResourceView *tmp_srv = nullptr;
                    if (SUCCEEDED(d11_device->CreateShaderResourceView(solid_tex, nullptr, &tmp_srv))) {
                        srv_to_use = tmp_srv;
                    }
                }
            }
        }
        if (want_cross) {
            static bool logged5 = false;
            if (!logged5) {
                logged5 = true;
                VRLOG("test: panel forced to the CROSS: one horizontal bar, one vertical bar. "
                      "With both eyes open, look whether the copy of each bar is displaced "
                      "sideways or up/down.");
            }
        }
        if (want_grid) {
            static bool logged4 = false;
            if (!logged4) {
                logged4 = true;
                VRLOG("test: panel forced to the GRID: white rules both ways. Read it as "
                      "which EDGES are doubled - the vertical ones only (horizontal "
                      "offset), the horizontal ones only (vertical offset), away from the "
                      "centre but not at it (a scale difference), or along one diagonal "
                      "(a roll).");
            }
        }
        if (test_solid_argb != 0) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                VRLOG("test: panel forced to solid colour (game frame ignored), eye %u -> "
                      "0x%06X%s", eye_index, solid_rgb,
                      test_solid_argb == 0xFEEDFACEu ? "  [per-eye: left red, right green]" : "");
            }
        }
        if (want_pattern) {
            static bool logged2 = false;
            if (!logged2) {
                logged2 = true;
                VRLOG("test: panel forced to the half/pattern texture: left red, right green, "
                      "white line down the middle. One clean split means the eye image is "
                      "drawn once; four bands means twice.");
            }
        }
        if (want_stripes) {
            static bool logged3 = false;
            if (!logged3) {
                logged3 = true;
                VRLOG("test: panel forced to the STRIPE ruler: 24 stripes across the picture "
                      "(2.5 deg each) with one red stripe a quarter in from the left. Look "
                      "with one eye, find the red stripe, then compare its position between "
                      "the eyes; a fringe of red/green along the stripe edges means the two "
                      "eyes are offset by that fraction of a stripe.");
            }
        }

        D3D11_VIEWPORT vpd = {};
        vpd.Width = (float)sc_width;
        vpd.Height = (float)sc_height;
        vpd.MinDepth = 0.0f;
        vpd.MaxDepth = 1.0f;
        d11_context->RSSetViewports(1, &vpd);

        UINT stride = sizeof(float) * 5, offset = 0;
        d11_context->IASetInputLayout(quad_layout);
        d11_context->IASetVertexBuffers(0, 1, &quad_vb, &stride, &offset);
        d11_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        d11_context->VSSetShader(quad_vs, nullptr, 0);
        d11_context->VSSetConstantBuffers(0, 1, &quad_cb);
        d11_context->PSSetShader(quad_ps, nullptr, 0);
        d11_context->PSSetShaderResources(0, 1, &srv_to_use);
        d11_context->PSSetSamplers(0, 1, &quad_sampler);
        d11_context->RSSetState(quad_raster);
        d11_context->Draw(4, 0);

        ID3D11RenderTargetView *null_rtv[1] = {nullptr};
        d11_context->OMSetRenderTargets(1, null_rtv, nullptr);
        ID3D11ShaderResourceView *null_srv[1] = {nullptr};
        d11_context->PSSetShaderResources(0, 1, null_srv);
        if (srv_to_use && srv_to_use != shared_srv) srv_to_use->Release();
        if (solid_tex) solid_tex->Release();
        return true;
    }

    // ------------------------------------------------------------ self test
    // Renders the panel through the real pipeline - same shaders, same matrices,
    // same per-eye slice indexing - into offscreen targets, then reports the mean
    // colour of each quadrant of each eye. It exists because every question the
    // headset can answer ("black?", "upside down?", "only one eye?") otherwise
    // costs a whole game launch, and all of them can be answered here instead.
    bool compose_selftest() {
        const bool saved_in_selftest = in_selftest;
        in_selftest = true;      // this test owns shared_srv; see frame_srv_for_eye
        const uint32_t eye_w = 512, eye_h = 512;
        const uint32_t fw = 1280, fh = 720;
        // One synthetic eye pose and frustum for both eyes: symmetric, so the
        // expected screen rectangle can be stated in closed form below.
        const XrPosef test_pose = {{0, 0, 0, 1}, {0, 0, 0}};
        const XrFovf test_fov = {-0.8f, 0.8f, 0.8f, -0.8f};   // ~46 degrees half-angle

        ID3D11Device *test_dev = nullptr;
        ID3D11DeviceContext *test_ctx = nullptr;
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                     &test_dev, &level, &test_ctx))) {
            VRLOG("selftest: D3D11CreateDevice failed");
            return false;
        }
        VRLOG("selftest: D3D11 device up (feature level 0x%04X)", (unsigned)level);

        // The pipeline slots are borrowed for the duration of the test. Resources
        // belong to a device, so everything built here is torn down again and the
        // real session builds its own on the runtime's adapter.
        ID3D11Device *saved_dev = d11_device;
        ID3D11DeviceContext *saved_ctx = d11_context;
        std::vector<ID3D11RenderTargetView *> saved_rtvs;
        saved_rtvs.swap(sc_rtvs);
        const uint32_t saved_sc_w = sc_width, saved_sc_h = sc_height;
        const uint32_t saved_view_count = view_count;
        // sc_image_count is part of the slot arithmetic draw_quad uses in the
        // two-swapchain layout, so the offscreen tests have to save it as well as
        // the render target views they swap out.
        const uint32_t saved_sc_image_count = sc_image_count;
        ID3D11ShaderResourceView *saved_srv = shared_srv;
        d11_device = test_dev;
        d11_context = test_ctx;
        shared_srv = nullptr;

        ID3D11Texture2D *src_tex = nullptr, *eye_tex = nullptr, *stage_tex = nullptr;
        ID3D11ShaderResourceView *src_srv = nullptr;
        ID3D11RenderTargetView *eye_rtvs[2] = {nullptr, nullptr};
        bool ok = create_quad_pipeline();

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = fw;
        td.Height = fh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (ok && SUCCEEDED(test_dev->CreateTexture2D(&td, nullptr, &src_tex))) {
            // Four quadrants, so a vertical flip or a horizontal mirror cannot be
            // mistaken for "the game is dark": blue top-left, green top-right,
            // red bottom-left, white bottom-right (BGRA, so 0xFF0000FF is blue).
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(test_ctx->Map(src_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                for (uint32_t y = 0; y < fh; ++y) {
                    uint32_t *row = reinterpret_cast<uint32_t *>((uint8_t *)m.pData +
                                                                 (size_t)y * m.RowPitch);
                    for (uint32_t x = 0; x < fw; ++x) {
                        const bool right = x >= fw / 2;
                        const bool bottom = y >= fh / 2;
                        // BGRA: 0xFF0000FF is blue and 0xFFFF0000 is red.
                        row[x] = bottom ? (right ? 0xFFFFFFFFu : 0xFFFF0000u)   // white / red
                                        : (right ? 0xFF00FF00u : 0xFF0000FFu); // green / blue
                    }
                }
                test_ctx->Unmap(src_tex, 0);
                ok = SUCCEEDED(test_dev->CreateShaderResourceView(src_tex, nullptr, &src_srv));
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }

        // Two slices in one array, the same shape the runtime's swapchain has.
        D3D11_TEXTURE2D_DESC ed = td;
        ed.Width = eye_w;
        ed.Height = eye_h;
        ed.ArraySize = 2;
        ed.Usage = D3D11_USAGE_DEFAULT;
        ed.BindFlags = D3D11_BIND_RENDER_TARGET;
        ed.CPUAccessFlags = 0;
        if (ok && SUCCEEDED(test_dev->CreateTexture2D(&ed, nullptr, &eye_tex))) {
            for (uint32_t eye = 0; eye < 2; ++eye) {
                D3D11_RENDER_TARGET_VIEW_DESC rd = {};
                rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rd.Texture2DArray.MipSlice = 0;
                rd.Texture2DArray.FirstArraySlice = eye;
                rd.Texture2DArray.ArraySize = 1;
                if (FAILED(test_dev->CreateRenderTargetView(eye_tex, &rd, &eye_rtvs[eye]))) ok = false;
            }
        } else {
            ok = false;
        }

        if (ok) {
            shared_srv = src_srv;
            sc_width = eye_w;
            sc_height = eye_h;
            view_count = 2;
            sc_rtvs.assign(2, nullptr);
            sc_rtvs[0] = eye_rtvs[0];
            sc_rtvs[1] = eye_rtvs[1];
            sc_image_count = 1;     // one offscreen image backs both eyes here
            panel_placed = false;   // measure a fresh placement against this FOV
            for (uint32_t eye = 0; eye < 2; ++eye) {
                if (!draw_quad(nullptr, 0, eye, test_pose, test_fov)) ok = false;
            }
        }

        D3D11_TEXTURE2D_DESC sd = ed;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (ok && SUCCEEDED(test_dev->CreateTexture2D(&sd, nullptr, &stage_tex))) {
            test_ctx->CopyResource(stage_tex, eye_tex);
            for (uint32_t eye = 0; eye < 2; ++eye) {
                D3D11_MAPPED_SUBRESOURCE m = {};
                if (SUCCEEDED(test_ctx->Map(stage_tex, D3D11CalcSubresource(0, eye, 1),
                                            D3D11_MAP_READ, 0, &m))) {
                    // The screen is drawn inside a frustum derived from the panel
                    // rectangle, so the panel always maps to the full NDC range and
                    // the probes can be placed from the panel's own geometry instead
                    // of from assumed NDC fractions.
                    //
                    // An earlier version probed at fixed NDC 0.95 to check the black
                    // surround, which silently became wrong as soon as the viewing
                    // distance turned into a setting: with the panel mapped to NDC
                    // +-1 by construction, "0.95 of the view" is the picture's corner,
                    // not the surround. There is no surround to probe here - the
                    // default 4 m / 4.6 m screen subtends 60 x 36 degrees and the
                    // frustum covers exactly that - so this now checks what is really
                    // at stake: which way round the image is, and whether both eyes
                    // agree.
                    const float nx = 0.5f;   // tangential half-extents of the panel
                    const float ny = 0.5f;
                    // Source-space points at the centre of each quadrant, mapped to
                    // the pixels that should show them (u = x/eye_w, v = y/eye_h).
                    const float quad_u[4] = {0.25f, 0.75f, 0.25f, 0.75f};
                    const float quad_v[4] = {0.25f, 0.25f, 0.75f, 0.75f};
                    const unsigned long want[4] = {0xFF0000FFu, 0xFF00FF00u,
                                                   0xFFFF0000u, 0xFFFFFFFFu};
                    unsigned long got[4] = {0, 0, 0, 0};
                    for (int i = 0; i < 4; ++i) {
                        uint32_t px_x = (uint32_t)(quad_u[i] * (float)eye_w);
                        uint32_t px_y = (uint32_t)(quad_v[i] * (float)eye_h);
                        if (px_x >= eye_w) px_x = eye_w - 1;
                        if (px_y >= eye_h) px_y = eye_h - 1;
                        const uint32_t *row = reinterpret_cast<const uint32_t *>(
                            (const uint8_t *)m.pData + (size_t)px_y * m.RowPitch);
                        got[i] = row[px_x];
                    }
                    // Measure where the screen actually landed instead of assuming:
                    // first and last non-black pixel along the centre row and column,
                    // reported in NDC so it can be compared with the expected extent.
                    const uint32_t cy = eye_h / 2, cxp = eye_w / 2;
                    uint32_t x0 = eye_w, x1 = 0, y0 = eye_h, y1 = 0;
                    for (uint32_t x = 0; x < eye_w; ++x) {
                        const uint32_t px = reinterpret_cast<const uint32_t *>(
                            (const uint8_t *)m.pData + (size_t)cy * m.RowPitch)[x];
                        if ((px & 0x00FFFFFFu) != 0) {
                            if (x < x0) x0 = x;
                            x1 = x;
                        }
                    }
                    for (uint32_t y = 0; y < eye_h; ++y) {
                        const uint32_t px = reinterpret_cast<const uint32_t *>(
                            (const uint8_t *)m.pData + (size_t)y * m.RowPitch)[cxp];
                        if ((px & 0x00FFFFFFu) != 0) {
                            if (y < y0) y0 = y;
                            y1 = y;
                        }
                    }
                    VRLOG("selftest: eye %u screen measured x_ndc %.3f..%.3f, y_ndc %.3f..%.3f",
                          eye, (x0 / (float)eye_w) * 2.0f - 1.0f, (x1 / (float)eye_w) * 2.0f - 1.0f,
                          1.0f - (y0 / (float)eye_h) * 2.0f, 1.0f - (y1 / (float)eye_h) * 2.0f);
                    // A vertical probe through the left half of the screen: at 28% of
                    // the way from the centre to its left edge, top to bottom must run
                    // red red blue blue if the left half is drawn, and the four values
                    // say exactly where (and whether) it stops.
                    {
                        const int32_t probe_x = (int32_t)((0.5f + (-0.28f) * 0.5f) * (float)eye_w);
                        char line[256] = "";
                        for (int k = 0; k < 4; ++k) {
                            const float ny_k = 0.24f - 0.16f * (float)k;
                            const int32_t probe_y = (int32_t)((0.5f - ny_k * 0.5f) * (float)eye_h);
                            const uint32_t px = reinterpret_cast<const uint32_t *>(
                                (const uint8_t *)m.pData + (size_t)probe_y * m.RowPitch)[probe_x];
                            char one[16];
                            _snprintf_s(one, sizeof(one), _TRUNCATE, "%08lX ", (unsigned long)px);
                            strcat_s(line, one);
                        }
                        VRLOG("selftest: eye %u left-half probe (y +0.24..-0.24): %s", eye, line);
                    }
                    // Keep the rendered eye image: when the numbers and the picture
                    // disagree, the picture wins.
                    if (eye == 0) {
                        HANDLE f = CreateFileA("re6vr_selftest_eye0.bgra", GENERIC_WRITE, FILE_SHARE_READ,
                                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (f != INVALID_HANDLE_VALUE) {
                            DWORD written = 0;
                            for (uint32_t y = 0; y < eye_h; ++y) {
                                WriteFile(f, (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
                                          eye_w * 4, &written, nullptr);
                            }
                            CloseHandle(f);
                            VRLOG("selftest: wrote re6vr_selftest_eye0.bgra (%ux%u)", eye_w, eye_h);
                        }
                    }
                    VRLOG("selftest: eye %u panel fills the frustum; source quadrant centres read "
                          "TL=%08lX TR=%08lX BL=%08lX BR=%08lX",
                          eye, got[0], got[1], got[2], got[3]);
                    bool match = true;
                    for (int i = 0; i < 4; ++i) {
                        if (got[i] != want[i]) match = false;
                    }
                    if (!match) {
                        VRLOG("selftest: eye %u expected BGRA TL=FF0000FF(blue) TR=FF00FF00(green) "
                              "BL=FFFF0000(red) BR=FFFFFFFF(white)",
                              eye);
                        ok = false;
                    }
                    test_ctx->Unmap(stage_tex, D3D11CalcSubresource(0, eye, 1));
                } else {
                    VRLOG("selftest: could not map eye %u back", eye);
                    ok = false;
                }
            }
        } else if (ok) {
            VRLOG("selftest: staging texture creation failed");
            ok = false;
        }

        // --- teardown: everything that belongs to the test device goes away ---
        if (stage_tex) stage_tex->Release();
        if (eye_rtvs[0]) eye_rtvs[0]->Release();
        if (eye_rtvs[1]) eye_rtvs[1]->Release();
        if (eye_tex) eye_tex->Release();
        if (src_srv) src_srv->Release();
        if (src_tex) src_tex->Release();
        if (quad_vs) { quad_vs->Release(); quad_vs = nullptr; }
        if (quad_ps) { quad_ps->Release(); quad_ps = nullptr; }
        if (quad_layout) { quad_layout->Release(); quad_layout = nullptr; }
        if (quad_vb) { quad_vb->Release(); quad_vb = nullptr; }
        if (quad_cb) { quad_cb->Release(); quad_cb = nullptr; }
        if (quad_sampler) { quad_sampler->Release(); quad_sampler = nullptr; }
        if (quad_raster) { quad_raster->Release(); quad_raster = nullptr; }
        sc_rtvs.clear();
        sc_rtvs.swap(saved_rtvs);
        shared_srv = saved_srv;
        sc_width = saved_sc_w;
        sc_height = saved_sc_h;
        view_count = saved_view_count;
        sc_image_count = saved_sc_image_count;
        d11_device = saved_dev;
        d11_context = saved_ctx;
        test_ctx->Release();
        test_dev->Release();

        VRLOG("selftest: %s", ok ? "PASS - both eyes drew the panel, orientation matches the source"
                                 : "FAIL - see the quadrant colours above");
        in_selftest = saved_in_selftest;
        return ok;
    }

    // Offline pass through the REAL eye geometry: a portrait per-eye slice like the
    // runtime's 998x2148, a 16:9 source frame, the current screen settings, and both
    // eye buffers written to disk.
    //
    // compose_selftest() uses a square 512x512 target and a synthetic pose, so it
    // cannot show what the slice's aspect ratio does to the picture - and that is
    // exactly the property that decides how much of the headset the screen fills.
    // Writing the buffers out is what makes "how big is the screen, and is it the
    // right shape" answerable without launching the game and wearing the headset.
    bool compose_selftest_real_geometry() {
        const bool saved_in_selftest = in_selftest;
        in_selftest = true;      // this test owns shared_srv; see frame_srv_for_eye
        const uint32_t eye_w = 998, eye_h = 2148;      // the Quest 3 slice, 1:1
        const uint32_t fw = 1280, fh = 720;            // the game frame, 16:9
        // The panel test switch has to be read here too: this path never goes
        // through on_frame, so a marker read only there did not apply offline and
        // a diagnostic pattern could look like "the pattern does nothing".
        read_test_solid_marker();

        ID3D11Device *test_dev = nullptr;
        ID3D11DeviceContext *test_ctx = nullptr;
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                     &test_dev, &level, &test_ctx))) {
            VRLOG("selftest-real: D3D11CreateDevice failed");
            return false;
        }

        ID3D11Device *saved_dev = d11_device;
        ID3D11DeviceContext *saved_ctx = d11_context;
        std::vector<ID3D11RenderTargetView *> saved_rtvs;
        saved_rtvs.swap(sc_rtvs);
        const uint32_t saved_sc_w = sc_width, saved_sc_h = sc_height;
        const uint32_t saved_view_count = view_count;
        // sc_image_count is part of the slot arithmetic draw_quad uses in the
        // two-swapchain layout, so the offscreen tests have to save it as well as
        // the render target views they swap out.
        const uint32_t saved_sc_image_count = sc_image_count;
        ID3D11ShaderResourceView *saved_srv = shared_srv;
        d11_device = test_dev;
        d11_context = test_ctx;
        shared_srv = nullptr;

        ID3D11Texture2D *src_tex = nullptr, *eye_tex = nullptr, *stage_tex = nullptr;
        ID3D11ShaderResourceView *src_srv = nullptr;
        ID3D11RenderTargetView *eye_rtvs[2] = {nullptr, nullptr};
        bool ok = create_quad_pipeline();

        // A grid on grey: the checker cells make a vertical stretch obvious, and the
        // bright border makes it obvious whether the whole frame is on the panel.
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = fw;
        td.Height = fh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (ok && SUCCEEDED(test_dev->CreateTexture2D(&td, nullptr, &src_tex))) {
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(test_ctx->Map(src_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                for (uint32_t y = 0; y < fh; ++y) {
                    uint32_t *row = reinterpret_cast<uint32_t *>((uint8_t *)m.pData +
                                                                 (size_t)y * m.RowPitch);
                    for (uint32_t x = 0; x < fw; ++x) {
                        const bool border = x < 4 || y < 4 || x >= fw - 4 || y >= fh - 4;
                        const bool cell = (((x / 80) + (y / 80)) & 1) != 0;
                        const bool centre = (x > fw / 2 - 40 && x < fw / 2 + 40 &&
                                             y > fh / 2 - 40 && y < fh / 2 + 40);
                        uint32_t c = cell ? 0xFF404040u : 0xFFA0A0A0u;   // BGRA greys
                        if (border) c = 0xFF20C020u;                     // green frame
                        if (centre) c = 0xFF0000FFu;                     // red centre
                        row[x] = c;
                    }
                }
                test_ctx->Unmap(src_tex, 0);
                ok = SUCCEEDED(test_dev->CreateShaderResourceView(src_tex, nullptr, &src_srv));
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }

        D3D11_TEXTURE2D_DESC ed = td;
        ed.Width = eye_w;
        ed.Height = eye_h;
        ed.ArraySize = 2;
        ed.Usage = D3D11_USAGE_DEFAULT;
        ed.BindFlags = D3D11_BIND_RENDER_TARGET;
        ed.CPUAccessFlags = 0;
        if (ok && SUCCEEDED(test_dev->CreateTexture2D(&ed, nullptr, &eye_tex))) {
            for (uint32_t eye = 0; eye < 2; ++eye) {
                D3D11_RENDER_TARGET_VIEW_DESC rd = {};
                rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rd.Texture2DArray.MipSlice = 0;
                rd.Texture2DArray.FirstArraySlice = eye;
                rd.Texture2DArray.ArraySize = 1;
                if (FAILED(test_dev->CreateRenderTargetView(eye_tex, &rd, &eye_rtvs[eye]))) ok = false;
            }
        } else {
            ok = false;
        }

        if (ok) {
            shared_srv = src_srv;
            sc_width = eye_w;
            sc_height = eye_h;
            view_count = 2;
            sc_rtvs.assign(2, nullptr);
            sc_rtvs[0] = eye_rtvs[0];
            sc_rtvs[1] = eye_rtvs[1];
            sc_image_count = 1;     // one offscreen image backs both eyes here

            // A pose looking straight down the panel's own axis, so the reported
            // frustum is the one the game produces with ipd_scale = 0.
            XrPosef test_pose = {{0, 0, 0, 1}, {0, 0, 0}};
            const XrFovf test_fov = {-0.8f, 0.8f, 0.8f, -0.8f};
            panel_placed = false;   // measure a fresh placement with these settings
            // RE6VR_SELFTEST_STEREO=1 renders the two eyes from *different*
            // positions, which is the only way to measure the stereo disparity
            // our own path produces: with one shared pose the panels land in
            // identical places by construction. The default separation is a
            // deliberately large 126 mm so that the measured shift is far above
            // the noise; set RE6VR_SELFTEST_STEREO_IPD to use a real IPD.
            wchar_t stbuf[32] = L"";
            const bool stereo = GetEnvironmentVariableW(L"RE6VR_SELFTEST_STEREO", stbuf, 32) > 0 &&
                                _wtoi(stbuf) != 0;
            float ipd = 0.126f;
            if (GetEnvironmentVariableW(L"RE6VR_SELFTEST_STEREO_IPD", stbuf, 32) > 0) {
                const float v = (float)_wtof(stbuf);
                if (v > 0.0f && v < 0.5f) ipd = v;
            }
            if (stereo) {
                VRLOG("selftest-real: stereo pair, eye separation %.1f mm "
                      "(difference between the two dumps is the disparity we produce)",
                      ipd * 1000.0f);
            }
            XrPosef eye_poses[2];
            for (uint32_t eye = 0; eye < 2; ++eye) {
                eye_poses[eye] = test_pose;
                if (stereo) eye_poses[eye].position.x += (eye == 0 ? -0.5f : 0.5f) * ipd;
            }
            // Same call the frame path makes, so the automatic trim is computed
            // identically here and in the game - otherwise the self-test would be
            // measuring a configuration the game never runs.
            note_eye_positions(eye_poses[0].position, eye_poses[1].position);
            for (uint32_t eye = 0; eye < 2; ++eye) {
                if (!draw_quad(nullptr, 0, eye, eye_poses[eye], test_fov)) ok = false;
            }
            VRLOG("selftest-real: panel %.2f x %.2f m at %.2f m, slice %ux%u (aspect %.3f), "
                  "source %ux%u (aspect %.3f), frustum mode %.0f",
                  panel_w, panel_h, screen_dist, eye_w, eye_h, (float)eye_w / (float)eye_h,
                  fw, fh, (float)fw / (float)fh, frustum_aspect_mode);
        }

        D3D11_TEXTURE2D_DESC sd = ed;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (ok && SUCCEEDED(test_dev->CreateTexture2D(&sd, nullptr, &stage_tex))) {
            test_ctx->CopyResource(stage_tex, eye_tex);
            for (uint32_t eye = 0; eye < 2; ++eye) {
                D3D11_MAPPED_SUBRESOURCE m = {};
                if (SUCCEEDED(test_ctx->Map(stage_tex, D3D11CalcSubresource(0, eye, 1),
                                            D3D11_MAP_READ, 0, &m))) {
                    const char *name = (eye == 0) ? "re6vr_real_eye0.bgra" : "re6vr_real_eye1.bgra";
                    HANDLE f = CreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (f != INVALID_HANDLE_VALUE) {
                        DWORD written = 0;
                        for (uint32_t y = 0; y < eye_h; ++y) {
                            WriteFile(f, (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
                                      eye_w * 4, &written, nullptr);
                        }
                        CloseHandle(f);
                        VRLOG("selftest-real: eye %u written to %s (%ux%u)", eye, name, eye_w, eye_h);
                    } else {
                        VRLOG("selftest-real: cannot write %s", name);
                        ok = false;
                    }
                    test_ctx->Unmap(stage_tex, D3D11CalcSubresource(0, eye, 1));
                } else {
                    VRLOG("selftest-real: could not map eye %u back", eye);
                    ok = false;
                }
            }
        } else if (ok) {
            VRLOG("selftest-real: staging texture creation failed");
            ok = false;
        }

        if (stage_tex) stage_tex->Release();
        if (eye_rtvs[0]) eye_rtvs[0]->Release();
        if (eye_rtvs[1]) eye_rtvs[1]->Release();
        if (eye_tex) eye_tex->Release();
        if (src_srv) src_srv->Release();
        if (src_tex) src_tex->Release();
        if (quad_vs) { quad_vs->Release(); quad_vs = nullptr; }
        if (quad_ps) { quad_ps->Release(); quad_ps = nullptr; }
        if (quad_layout) { quad_layout->Release(); quad_layout = nullptr; }
        if (quad_vb) { quad_vb->Release(); quad_vb = nullptr; }
        if (quad_cb) { quad_cb->Release(); quad_cb = nullptr; }
        if (quad_sampler) { quad_sampler->Release(); quad_sampler = nullptr; }
        if (quad_raster) { quad_raster->Release(); quad_raster = nullptr; }
        sc_rtvs.clear();
        sc_rtvs.swap(saved_rtvs);
        sc_width = saved_sc_w;
        sc_height = saved_sc_h;
        view_count = saved_view_count;
        sc_image_count = saved_sc_image_count;
        shared_srv = saved_srv;
        in_selftest = saved_in_selftest;
        d11_device = saved_dev;
        d11_context = saved_ctx;
        test_ctx->Release();
        test_dev->Release();

        VRLOG("selftest-real: %s", ok ? "eye buffers written"
                                      : "FAILED - see the messages above");
        return ok;
    }

    // Writes the eye image we just handed to the runtime, raw BGRA.
    //
    // This is the last link that can be checked from inside the process: it shows
    // the panel as it sits in the eye's view. "The headset is black" then splits
    // cleanly into "our eye image is empty" (our draw) or "our eye image has the
    // panel but nothing is displayed" (runtime/headset side) - two problems with
    // nothing in common, and no way to tell them apart without this file.
    bool dump_eye_image(uint32_t image_index, uint32_t eye) {
        // Two layouts: one shared array swapchain (eye picks the array slice), or one
        // swapchain per eye (eye picks the swapchain, and the slice is always 0).
        const uint32_t eye_idx = sc_images_shared ? 0 : (eye < 2 ? eye : 0);
        const uint32_t slice = sc_images_shared ? (eye < 2 ? eye : 0) : 0;
        if (image_index >= sc_images[eye_idx].size()) return false;
        if (!sc_images[eye_idx][image_index].texture) return false;
        ID3D11Texture2D *src = sc_images[eye_idx][image_index].texture;

        D3D11_TEXTURE2D_DESC d = {};
        src->GetDesc(&d);
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;

        wchar_t path[MAX_PATH] = L"re6vr_eye_left.bgra";
        const wchar_t *log_path = vrlog::path();
        if (log_path && log_path[0]) {
            wcsncpy_s(path, MAX_PATH, log_path, _TRUNCATE);
            wchar_t *slash = wcsrchr(path, L'\\');
            if (slash) {
                const wchar_t *name = (eye == 0) ? L"re6vr_eye_left.bgra" : L"re6vr_eye_right.bgra";
                wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - path)), name);
            }
        }

        // Every context call stays under the lock: the copy thread and the submit
        // thread share this immediate context, and this runs on the submit thread.
        bool ok = false;
        {
            std::lock_guard<std::recursive_mutex> guard(d11_lock);
            ID3D11Texture2D *stage = nullptr;
            if (FAILED(d11_device->CreateTexture2D(&sd, nullptr, &stage)) || !stage) {
                VRLOG("openxr: eye dump: staging texture %ux%u fmt=%d refused", d.Width, d.Height,
                      (int)d.Format);
                return false;
            }
            // CopyResource is void and silently does nothing if the descriptions do
            // not match, which is the safe failure: no dump, no crash.
            d11_context->CopyResource(stage, src);

            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(d11_context->Map(stage, D3D11CalcSubresource(0, slice, 1), D3D11_MAP_READ, 0,
                                           &m))) {
                HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    for (uint32_t y = 0; y < d.Height; ++y) {
                        WriteFile(f, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, d.Width * 4,
                                  &written, nullptr);
                    }
                    CloseHandle(f);
                    ok = true;
                    VRLOG("openxr: eye dump written to %ls (%ux%u, eye %u)", path, d.Width, d.Height,
                          eye);
                } else {
                    VRLOG("openxr: eye dump: cannot write %ls (err %lu)", path, GetLastError());
                }
                d11_context->Unmap(stage, D3D11CalcSubresource(0, slice, 1));
            } else {
                VRLOG("openxr: eye dump: Map failed");
            }
            stage->Release();
        }
        return ok;
    }

    bool create_frame_srv() {
        if (shared_srv) return true;
        if (!shared_d11_tex) return false;
        if (FAILED(d11_device->CreateShaderResourceView(shared_d11_tex, nullptr, &shared_srv))) {
            warn_once("CreateShaderResourceView(shared)");
            return false;
        }
        return true;
    }

    // Which picture this eye is shown. Stereo: the eye's own captured frame. Otherwise the single
    // game-frame texture, exactly as before - that path has been verified in the headset and is not
    // touched by the stereo switch.
    //
    // Falls back to the shared texture whenever an eye's own texture is not ready, so a stereo run
    // that could not capture one eye shows the game frame in that eye rather than nothing at all.
    ID3D11ShaderResourceView *frame_srv_for_eye(uint32_t eye_index) {
        // The offline self-tests put their own synthetic picture in shared_srv and are testing the
        // panel pipeline, not the capture: handing them a captured eye texture instead turned their
        // verdict into "every quadrant is black", which is exactly how a working capture looked like a
        // broken panel for one build.
        if (in_selftest) return shared_srv;
        // The redirect path first: these are pictures the ENGINE drew for that eye, copied once per
        // frame each, so when they exist nothing else should be in the way.
        if (arms_redirect && eye_index < 2 && eye_stage_srv[eye_index] &&
            eye_redirect_copy_count[0] > 0 && eye_redirect_copy_count[1] > 0) {
            return eye_stage_srv[eye_index];
        }
        // The GPU-only path first: it is the one that does not read the frame back, so when it exists
        // nothing else should be in the way.
        if (arms_shared && eye_index < 2 && eye_shared_srv[eye_index] &&
            eye_shared_blit_count[0] > 0 && eye_shared_blit_count[1] > 0) {
            return eye_shared_srv[eye_index];
        }
        // Both eyes switch together, or neither does. Falling back per eye would show eye 1 the
        // picture captured for eye 0 (eye 1 is copied after pass 2, so anything that breaks it leaves
        // eye 0's texture in place) - two different pictures, one of them labelled as the other eye.
        // With both falling back the player sees the plain game frame in both eyes: no stereo, but
        // never a mismatched pair.
        if (arms_stereo && eye_index < 2 && eye_srv[eye_index] && eye_copy_count[0] > 0 &&
            eye_copy_count[1] > 0) {
            return eye_srv[eye_index];
        }
        return shared_srv;
    }
    bool in_selftest = false;      // an offline test owns shared_srv right now

    void destroy_frame_srv() {
        if (shared_srv) { shared_srv->Release(); shared_srv = nullptr; }
    }

    // Rate-limited diagnostics: some of these can fire once per frame.
    void warn_once(const char *what) {
        for (auto &s : warned) {
            if (s == what) return;
        }
        warned.push_back(what);
        VRLOG("openxr: WARN %s failed (further occurrences suppressed)", what);
    }
    void warn_once_hr(const char *what, HRESULT hr) {
        for (auto &s : warned) {
            if (s == what) return;
        }
        warned.push_back(what);
        VRLOG("openxr: WARN %s failed: 0x%08lX (further occurrences suppressed)", what, (unsigned long)hr);
    }
    std::vector<const char *> warned;
};

// ------------------------------------------------------------ public methods
OpenXrBridge::OpenXrBridge()
    : impl_(new Impl()), state_(XrState::NotTried), prepared_(false), device_lost_(false),
      frames_submitted_(0), frames_failed_(0) {}

// One bridge for the process. d3d9_proxy.cpp owns the instance (it is the one that creates it at
// CreateDevice time) and registers it here, so the camera code can reach the capture path without
// holding a second pointer that could go stale.
static OpenXrBridge *g_active_bridge = nullptr;
OpenXrBridge *active_bridge() { return g_active_bridge; }
void set_active_bridge(OpenXrBridge *bridge) { g_active_bridge = bridge; }

// Asked once per frame by the camera code before it issues the second render pass. "No bridge yet" is
// deliberately NOT ready: the very first frames of a session run before the OpenXR side exists, and
// rendering the scene twice there buys nothing.
bool bridge_device_ready() {
    OpenXrBridge *bridge = active_bridge();
    if (!bridge) return false;
    return bridge->state() == XrState::Ready && !bridge->device_lost();
}

OpenXrBridge::~OpenXrBridge() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool OpenXrBridge::init(HMODULE proxy_module, IDirect3DDevice9 *d3d9_device) {
    if (state_ != XrState::NotTried) return state_ == XrState::Ready;

    if (!impl_->load_loader(proxy_module)) {
        state_ = XrState::Unavailable;
        return false;
    }
    if (!impl_->create_instance(proxy_module)) {
        state_ = XrState::Failed;
        return false;
    }
    if (!impl_->create_session_and_swapchain()) {
        state_ = XrState::Failed;
        return false;
    }
    if (!impl_->create_quad_pipeline()) {
        state_ = XrState::Failed;
        return false;
    }

    // The device must be D3D9Ex for any of this to work.
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    d3d9_device->lpVtbl->GetCreationParameters(d3d9_device, &cp);
    VRLOG("openxr: game device: adapter=%u type=%lu focus=0x%p behaviour=0x%08lX",
          cp.AdapterOrdinal, (unsigned long)cp.DeviceType, cp.hFocusWindow, (unsigned long)cp.BehaviorFlags);

    // Which D3D11 adapter the swapchain textures come from, next to which D3D9
    // adapter the game renders on. A mismatch (e.g. a laptop iGPU) would mean the
    // compositor can never see our writes, whatever the pose and FOV say, so it is
    // worth having in the log.
    if (impl_->dxgi_adapter) {
        DXGI_ADAPTER_DESC ad = {};
        if (SUCCEEDED(impl_->dxgi_adapter->GetDesc(&ad))) {
            VRLOG("openxr: swapchain D3D11 device %p on '%ls' vendor=0x%04X device=0x%04X",
                  (void *)impl_->d11_device, ad.Description, ad.VendorId, ad.DeviceId);
        }
    }

    state_ = XrState::Ready;
    VRLOG("openxr: initialised, waiting for the session to reach READY");
    return true;
}

// Creates every D3D9 surface the compositor needs, called once right after the
// game's device exists.
//
// This is the whole point of the design: MT Framework stops with
// "ERR09: Unsupported function." if a texture is created from inside a frame, so
// nothing the compositor needs may be allocated lazily. Creating it here (before
// the first BeginScene) is safe, and the per-frame path only ever reuses what
// already exists.
bool OpenXrBridge::prepare(IDirect3DDevice9 *d3d9_device, uint32_t w, uint32_t h, D3DFORMAT fmt) {
    if (!impl_) return false;
    set_backbuffer_geometry(w, h, fmt);

    // Called from CreateDevice, where OpenXR is not up yet. Remember the device
    // and geometry; the surfaces are created at the first opportunity, which is
    // still before the game presents anything of its own.
    pending_device_ = d3d9_device;
    pending_w_ = w;
    pending_h_ = h;
    pending_fmt_ = fmt;
    pending_ = true;
    VRLOG("prepare: queued %ux%u fmt=%lu (surfaces are created before the first frame)",
          w, h, (unsigned long)fmt);
    return try_create_pending();
}

// Creates the queued surfaces. Safe to call repeatedly: it does nothing until
// both the OpenXR side and the game device are available, and it may be retried
// after a device reset.
bool OpenXrBridge::try_create_pending() {
    if (!pending_ || prepared_ || !impl_ || !pending_device_) return prepared_;
    if (state_ != XrState::Ready) return false;   // wait for the runtime

    if (!impl_->create_device_surfaces(pending_device_, pending_w_, pending_h_, pending_fmt_)) {
        // A handful of retries, not one: surface creation can fail transiently
        // while a fullscreen game is still changing display mode, and giving up on
        // the first failure is what leaves composition switched off for the rest
        // of the session.
        if (++create_attempts_ >= 3) {
            pending_ = false;
            VRLOG("prepare: surface creation failed %u times, composition stays off", create_attempts_);
        } else {
            VRLOG("prepare: surface creation failed, retrying (%u/3)", create_attempts_);
        }
        return false;
    }
    create_attempts_ = 0;
    prepared_ = true;
    VRLOG("prepare: compositor surfaces ready (%ux%u back buffer)", pending_w_, pending_h_);
    return true;
}

bool OpenXrBridge::run_selftest(IDirect3DDevice9 *d3d9_device) {
    if (!impl_) return false;

    // The D3D9 surfaces may still be queued (prepare() runs at CreateDevice,
    // before any frame); the test needs them to build the panel.
    try_create_pending();
    // ...and when there is no headset they stay queued forever, because surface creation waits for a
    // running session. The offline test needs them anyway: it is the only place the D3D9-side
    // resources (the single game-frame chain and the stereo pair) can be built and checked without a
    // game. This runs the same create_device_surfaces the session path runs, with the same device and
    // geometry, so what it validates is what the game will use.
    //
    // The caller must be somewhere a resource may be created - EndScene, never Present (see the call
    // site in d3d9_proxy.cpp).
    if (pending_device_ && !prepared_) {
        VRLOG("selftest: no session to wait for - building the compositor surfaces for the test");
        impl_->create_device_surfaces(pending_device_, pending_w_, pending_h_, pending_fmt_);
    }

    // Offline check of the stereo capture path (RE6VR_STEREO_SELFTEST=1). It needs a real D3D9 device
    // and a stereo marker, neither of which the headset path provides, so it builds what it needs
    // itself and is skipped silently when the switch is not set. It brings up its own D3D11 device,
    // which is why the two checks below are not in front of it.
    impl_->stereo_capture_selftest(d3d9_device);

    // The GPU-only path's interop (RE6VR_SHARED_SELFTEST=1): D3D9Ex shared texture -> D3D11. This is
    // the check that decides whether the no-readback design is viable on this machine at all.
    impl_->shared_interop_selftest(d3d9_device);

    // The redirect mechanism (RE6VR_REDIRECT_SELFTEST=1): draw while a texture of OURS is bound and read
    // the result back out of that texture. This is the one link a game run then only has to confirm.
    impl_->redirect_selftest(d3d9_device);

    // Offline check of the panel rendering: it renders through the real pipeline into offscreen
    // targets, so it needs the D3D11 side but not a session. When no headset is connected the session
    // never comes up, and without this the test could not run at all.
    if (!impl_->d11_device && !impl_->create_d11_device_offline()) {
        VRLOG("selftest: no D3D11 device available, cannot run");
        return false;
    }

    // RE6VR_SELFTEST_REAL=1 runs the second, geometry-accurate pass instead: same
    // pipeline, but a portrait 998x2148 slice and 16:9 source, with both eye
    // buffers written out. It is the pass that can answer "how much of the view is
    // the screen" without a headset.
    wchar_t buf[32] = L"";
    if (GetEnvironmentVariableW(L"RE6VR_SELFTEST_REAL", buf, 32) > 0 && _wtoi(buf) != 0) {
        return impl_->compose_selftest_real_geometry();
    }

    return impl_->compose_selftest();
}

void OpenXrBridge::release_surfaces() {
    prepared_ = false;
    // The queued request (device + geometry) is kept on purpose. A lost device is
    // the same device object once the game calls Reset, and clearing the request
    // here is what used to leave composition switched off for the rest of the
    // session: a fullscreen D3D9 game loses its device during start-up.
    pending_ = (pending_device_ != nullptr);
    if (impl_) impl_->release_shared_surface();
}

void OpenXrBridge::on_end_scene(IDirect3DDevice9 *d3d9_device) {
    if (state_ != XrState::Ready || !impl_) return;
    if (!prepared_ && !try_create_pending()) return;
    if (device_lost_) return;   // the game is mid-reset; leave its objects alone

    // EndScene fires far more often than Present while the game is loading, so
    // this is also where session state changes get picked up.
    impl_->pump_events();
    if (!impl_->session_running) return;

    // Everything used here already exists; this path only reads and copies.
    //
    // Both candidates are fetched, because which one holds the finished frame is
    // not a given: the back buffer is what gets presented, while the current render
    // target is normally the same surface but can also be one of MT Framework's
    // own, at a different size - and copying that one would put a partial or blank
    // frame on the panel. note_source_surfaces() logs both and decides once.
    IDirect3DSurface9 *rt = nullptr;
    IDirect3DSurface9 *back = nullptr;
    d3d9_device->lpVtbl->GetRenderTarget(d3d9_device, 0, (void **)&rt);
    d3d9_device->lpVtbl->GetBackBuffer(d3d9_device, 0, 0, D3DBACKBUFFER_TYPE_MONO, (void **)&back);
    impl_->note_source_surfaces(rt, back);

    IDirect3DSurface9 *src = impl_->pick_source(rt, back);
    // Stereo: this EndScene ends the frame's render pass, so what the device holds now is the picture
    // of whichever eye this frame rendered (the camera code alternates it). It goes into THAT eye's own
    // texture and stays there until this eye's turn comes round again, which is what lets one render
    // pass per frame still give each eye its own pose.
    //
    // This is the ONLY place this mod touches the game's device, and there are three ways to do it:
    //   * the REDIRECT path (preferred): both engine passes were sent into textures of ours, so there is
    //     nothing to read out of the device's target - one eye is copied per frame, alternating, into
    //     the compositor texture for that eye. Readback stays at the single-texture level, and the
    //     pictures are drawn by the ENGINE for each eye;
    //   * the GPU-only path (unavailable on this machine): one StretchRect into a shared texture;
    //   * the readback path (fallback): staging -> sysmem -> D3D11 Map.
    // Which one is live is in the log ("REDIRECT stereo path ARMED" / "SHARED ... ARMED" / "staying on
    // the readback path").
    if (impl_->arms_redirect) {
        // Copy the eye this frame rendered into: the camera code decided it and the pass was redirected
        // into exactly that eye's texture. One copy per frame, alternating, so the readback stays at the
        // level the verified single-texture build has always run at.
        const int eye = (impl_->current_eye == 0 || impl_->current_eye == 1)
                            ? impl_->current_eye
                            : (int)(impl_->redirect_copy_toggle ^= 1u);
        capture_redirect_eye(d3d9_device, eye);
    } else if (impl_->arms_stereo || impl_->arms_shared) {
        if (src) {
            if (impl_->arms_shared) {
                impl_->blit_eye_shared(d3d9_device, impl_->current_eye);
            } else {
                impl_->capture_eye(d3d9_device, impl_->current_eye);
            }
        }
    }
    if (src) impl_->copy_backbuffer(d3d9_device, src);
    if (rt) rt->lpVtbl->Release(rt);
    if (back) back->lpVtbl->Release(back);
}

// Called by the camera code (cam_steer's render-phase detour) BETWEEN the two render passes, with
// the back buffer holding pass 1's finished eye-0 picture. This is the only moment that picture
// exists: pass 2 renders the same scene from the other eye straight over it.
//
// Everything here is a blit on surfaces that already exist - no resource is created inside a frame,
// which is what MT Framework refuses with "ERR09: Unsupported function.". The function is a no-op
// unless stereo surfaces were built at device creation, so a non-stereo deployment cannot be affected
// by it.
void OpenXrBridge::capture_eye_now(IDirect3DDevice9 *d3d9_device, int eye) {
    if (state_ != XrState::Ready || !impl_) return;
    if (device_lost_) return;
    if (!impl_->arms_stereo) return;
    impl_->capture_eye(d3d9_device, eye);
}

void OpenXrBridge::note_current_eye(int eye) {
    if (!impl_) return;
    impl_->current_eye = (eye == 0 || eye == 1) ? eye : -1;
}

// The surface the engine's pass for this eye should be redirected into, or null when the redirect path
// is not armed. The camera code's SetRenderTarget detour calls this every frame, so it only reads
// pointers: no allocation, no logging, nothing that can block.
IDirect3DSurface9 *OpenXrBridge::redirect_target_for_eye(int eye) {
    if (!impl_) return nullptr;
    return impl_->redirect_target(eye);
}

// Hands one engine-drawn eye picture to the compositor. Called at EndScene on the frame where that eye
// was the one being copied (the camera code alternates), which is what keeps the readback count per
// frame at the single-texture level.
bool OpenXrBridge::capture_redirect_eye(IDirect3DDevice9 *d3d9_device, int eye) {
    if (state_ != XrState::Ready || !impl_ || device_lost_) return false;
    return impl_->copy_eye_redirect(d3d9_device, eye);
}

// Brings up the D3D11 device the offline checks measure through, without a session. Separate from
// run_selftest so the caller can do it from a point in the frame where creating things is safe and
// before anything else asks for a D3D11 device - two of them in one process was the alternative.
bool OpenXrBridge::prepare_selftest_d3d11() {
    if (!impl_) return false;
    if (impl_->d11_device) return true;
    return impl_->create_d11_device_offline();
}

// A free function as well, because the caller (cam_steer.cpp) reaches this module through a single
// declaration and has no OpenXrBridge instance of its own: d3d9_proxy.cpp creates the one bridge and
// hands it to `set_active_bridge`. Resolving it here keeps the pointer's ownership exactly where it
// is instead of duplicating it in two translation units.
void bridge_capture_eye(IDirect3DDevice9 *d3d9_device, int eye) {
    if (OpenXrBridge *bridge = active_bridge()) bridge->capture_eye_now(d3d9_device, eye);
}

// The camera module's "this frame is eye N" announcement, kept here so the EndScene capture fills the
// right texture. Cheap enough to call every frame; it only stores the value.
void bridge_note_current_eye(int eye) {
    if (OpenXrBridge *bridge = active_bridge()) bridge->note_current_eye(eye);
}

IDirect3DSurface9 *bridge_redirect_target_for_eye(int eye) {
    if (OpenXrBridge *bridge = active_bridge()) return bridge->redirect_target_for_eye(eye);
    return nullptr;
}

bool bridge_capture_redirect_eye(IDirect3DDevice9 *d3d9_device, int eye) {
    if (OpenXrBridge *bridge = active_bridge()) return bridge->capture_redirect_eye(d3d9_device, eye);
    return false;
}

bool OpenXrBridge::submit(IDirect3DDevice9 *d3d9_device) {
    if (state_ != XrState::Ready || !impl_) return false;
    if (device_lost_) return false;

    impl_->pump_events();
    if (impl_->exit_requested) {
        // Headset asked to quit: stop composing but leave the game running.
        VRLOG("openxr: exit requested by the runtime, disabling composition");
        state_ = XrState::Failed;
        return false;
    }
    if (!impl_->session_running) {
        // The runtime can stop and restart the session (STOPPING -> READY); until
        // it is running again there is nothing to submit.
        static bool warned = false;
        if (!warned) {
            VRLOG("submit: session is not running yet, frames are not being submitted");
            warned = true;
        }
        return false;
    }

    // Back buffer description drives the shared surface.
    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(d3d9_device->lpVtbl->GetBackBuffer(d3d9_device, 0, 0, 0, (void **)&bb)) || !bb) return false;
    // GetDesc on the back buffer faults inside d3d9.dll on this machine, so the
    // geometry recorded at CreateDevice time is used instead.
    D3DSURFACE_DESC desc = {};
    desc.Width = backbuffer_w();
    desc.Height = backbuffer_h();
    desc.Format = backbuffer_format();
    bb->Release();
    HRESULT hr = S_OK;
    if (FAILED(hr) || desc.Width == 0 || desc.Height == 0) return false;

    // Safe mode: never create a texture from inside Present. This is the path
    // that was creating surfaces on every frame and making MT Framework stop
    // with "ERR09: Unsupported function.".
    if (device_lost_) return false;
    if (!prepared_ && !try_create_pending()) return false;
    if (!impl_->create_frame_srv()) {
        VRLOG("openxr: CreateShaderResourceView(shared) failed");
        ++frames_failed_;
        return false;
    }

    // --- frame loop ---
    XrFrameState frame_state = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo frame_wait = {XR_TYPE_FRAME_WAIT_INFO};
    if (XR_FAILED(impl_->pfn_wait_frame(impl_->session, &frame_wait, &frame_state))) return false;
    impl_->last_display_time = frame_state.predictedDisplayTime;

    XrFrameBeginInfo begin_info = {XR_TYPE_FRAME_BEGIN_INFO};
    XrResult r = impl_->pfn_begin_frame(impl_->session, &begin_info);
    if (XR_FAILED(r)) {
        VRLOG("openxr: xrBeginFrame failed: %s", xr_result_str(r));
        ++frames_failed_;
        return false;
    }

    // Eye poses from the runtime (orientation + IPD prediction), used for the
    // quad projection; positions are also used for the per-eye matrices.
    XrViewState view_state = {XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t view_count = impl_->view_count ? impl_->view_count : 2;
    XrViewLocateInfo locate_info = {XR_TYPE_VIEW_LOCATE_INFO};
    locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate_info.displayTime = frame_state.predictedDisplayTime;
    locate_info.space = impl_->local_space;
    uint32_t located = 0;
    if (XR_FAILED(impl_->pfn_locate_views(impl_->session, &locate_info, &view_state,
                                          view_count, &located, views))) {
        views[0].pose = {{0, 0, 0, 1}, {0, 0, 0}};
        views[1].pose = {{0, 0, 0, 1}, {0, 0, 0}};
        view_count = 2;
    } else {
        // The runtime's per-eye FOV is what the display can actually show, and it is
        // the yardstick for how big the virtual screen can usefully be: the panel is
        // drawn inside a frustum derived from the panel rectangle, so a panel much
        // narrower than this leaves the game screen sitting in a mostly black view.
        static int s_fov_logs = 0;
        if (s_fov_logs < 2) {
            ++s_fov_logs;
            const float deg = 57.2957795f;
            for (uint32_t i = 0; i < view_count && i < 2; ++i) {
                const float l = atanf(-views[i].fov.angleLeft) * deg;
                const float rr = atanf(views[i].fov.angleRight) * deg;
                const float u = atanf(views[i].fov.angleUp) * deg;
                const float d = atanf(-views[i].fov.angleDown) * deg;
                VRLOG("openxr: eye %u runtime FOV %.1f deg wide (%.1f left %.1f right), "
                      "%.1f deg tall (%.1f up %.1f down)", i, l + rr, l, rr, u + d, u, d);
            }
        }
    }

    // Acquire the image(s). With one shared array swapchain there is a single
    // acquire for both eyes; with one swapchain per eye each eye's chain is acquired
    // and waited on separately, which is what makes this layout a real test of the
    // slice handling rather than a cosmetic difference.
    uint32_t image_index = 0;
    uint32_t image_index_by_eye[2] = {0, 0};
    const int chains = impl_->sc_images_shared ? 1 : 2;
    XrSwapchainImageAcquireInfo acquire = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait_info = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    // Never block the game's render thread indefinitely: if the compositor is
    // still holding the image we would rather drop a frame than stall the game.
    wait_info.timeout = 100 * 1000 * 1000; // 100 ms
    for (int c = 0; c < chains; ++c) {
        XrSwapchain sc = impl_->swapchains[c] ? impl_->swapchains[c] : impl_->swapchain;
        uint32_t idx = 0;
        r = impl_->pfn_acquire_swapchain_image(sc, &acquire, &idx);
        if (XR_FAILED(r)) {
            static bool warned = false;
            if (!warned) { VRLOG("openxr: xrAcquireSwapchainImage failed: %s", xr_result_str(r)); warned = true; }
            impl_->pfn_end_frame(impl_->session, nullptr);
            ++frames_failed_;
            return false;
        }
        r = impl_->pfn_wait_swapchain_image(sc, &wait_info);
        if (XR_FAILED(r)) {
            VRLOG("openxr: xrWaitSwapchainImage failed: %s", xr_result_str(r));
            impl_->pfn_end_frame(impl_->session, nullptr);
            ++frames_failed_;
            return false;
        }
        if (c == 0) {
            image_index = idx;
            image_index_by_eye[0] = idx;
            image_index_by_eye[1] = idx;
        } else {
            image_index_by_eye[1] = idx;
        }
    }

    // Stage 1: the same game frame is drawn for both eyes - but into each eye's
    // own swapchain image and from that eye's own pose and FOV, so the panel sits
    // correctly in front of each eye and is seen with the right parallax. Stage 2
    // replaces the source image with a real per-eye render of the scene.
    bool eyes_ok = true;
    impl_->converge_views = false;   // draw_quad raises this for the diagnostic
    // Both eye poses are known here, so the inter-eye trim is derived from the
    // real geometry before anything is drawn.
    if (view_count >= 2) {
        impl_->note_eye_positions(views[0].pose.position, views[1].pose.position);
    }
    impl_->advance_uv_sweep();
    impl_->poll_uv_shift_keys();

    // Which way is the head actually pointing? This is logged unconditionally, because
    // everything else in this area is downstream of it and a silent failure here looks
    // exactly like "head tracking is not implemented".
    //
    // Two things can go wrong and neither was being checked. The runtime reports whether
    // the orientation it returned is meaningful at all (XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    // when that bit is clear the pose comes back as zeroes, so the head rotation is the
    // IDENTITY and any code driven by it does nothing while looking perfectly healthy. And
    // the yaw itself is worth seeing, since "I turned my head and the number did not move"
    // is a different bug from "the number moved and the scene did not".
    // Logged when the yaw CHANGES by a visible amount, not for the first N frames.
    //
    // The first version logged the first 40 frames and then stopped, and every one of those
    // was captured during start-up - so the log showed a narrow band of head angles and
    // gave the impression the headset barely moved, while the player was turning their head
    // freely in a level that had not been logged at all. A cap on the number of lines is the
    // wrong shape for a signal that has to be watched over a whole session.
    static int s_pose_logs = 0;
    static float s_last_logged_yaw = 1000.0f;
    {
        const bool orient_valid =
            (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        const bool pos_valid = (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        const XrQuaternionf q = views[0].pose.orientation;
        // Yaw taken from the FORWARD VECTOR, not from the quaternion components directly.
        //
        // The first version used yaw = atan2(2(wy + xz), 1 - 2(yy + xx)), which is the
        // textbook yaw only while the head is upright and level. This headset sits at about
        // 11 degrees of pitch even at rest, and the component formula measures the wrong
        // axis as soon as that happens. Rotating the local forward (0, 0, -1) by the
        // quaternion gives the true gaze direction, and its angle away from -Z is the yaw
        // in any pose. (The two forms also disagree in sign, so the old one reported turns
        // backwards - which mattered, because the yaw sign is what decides whether the game
        // camera follows the head the right way round.)
        const float fx = -2.0f * (q.x * q.z + q.w * q.y);
        const float fy = -2.0f * (q.y * q.z - q.w * q.x);
        const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        const float yaw = atan2f(fx, -fz) * 57.2957795f;
        const float pitch = asinf(fmaxf(-1.0f, fminf(1.0f, fy))) * 57.2957795f;

        // Publish the head yaw for the DirectInput8 proxy.
        //
        // Why a shared mapping rather than a call: the two proxies are separate DLLs in one
        // process, loaded by different parts of the game, and neither can see the other's
        // symbols. The mapping is the whole interface - the proxy adds this many degrees to
        // the right stick, and the game's own camera controller does the turning.
        //
        // What is published is the turn AWAY FROM THE POSE THE PLAYER STARTED IN, because
        // that is what a stick deflection means. The absolute yaw of this headset at rest is
        // not zero (it sits tilted), and feeding the absolute value would hold the camera
        // permanently turned by that offset.
        {
            static HANDLE s_map = nullptr;
            static struct HeadShared {
                volatile LONG seq;
                float yaw_deg;
                float pitch_deg;
                float pad[4];
            } *s_view = nullptr;
            static bool s_ref_ok = false;
            static float s_ref_yaw = 0.0f;
            if (!s_map) {
                s_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                           sizeof(HeadShared), L"re6vr_head_pose");
                if (s_map) {
                    s_view = (HeadShared *)MapViewOfFile(s_map, FILE_MAP_ALL_ACCESS, 0, 0,
                                                         sizeof(HeadShared));
                    VRLOG("head: publishing yaw for the dinput8 proxy through '%ls' (%s)",
                          L"re6vr_head_pose", s_view ? "mapped" : "map FAILED");
                } else {
                    VRLOG("head: CreateFileMapping('%ls') failed: %lu - the game camera "
                          "cannot be steered by the head", L"re6vr_head_pose", GetLastError());
                }
            }
            if (s_view) {
                if (!s_ref_ok) { s_ref_ok = true; s_ref_yaw = yaw; }
                float rel = yaw - s_ref_yaw;
                while (rel > 180.0f) rel -= 360.0f;
                while (rel < -180.0f) rel += 360.0f;
                ++s_view->seq;                       // odd = being written
                s_view->yaw_deg = rel;
                s_view->pitch_deg = pitch;
                ++s_view->seq;                       // even = complete
            }
        }
        if (s_pose_logs < 400 && fabsf(yaw - s_last_logged_yaw) >= 5.0f) {
            ++s_pose_logs;
            s_last_logged_yaw = yaw;
            VRLOG("pose: yaw %.1f deg pitch %.1f deg (orientation_valid=%d position_valid=%d) "
                  "forward (%.3f %.3f %.3f) quat (%.4f %.4f %.4f %.4f)",
                  yaw, pitch, (int)orient_valid, (int)pos_valid, fx, fy, fz,
                  q.x, q.y, q.z, q.w);
        }
        if (!orient_valid && s_pose_logs < 400) {
            VRLOG("pose: THE RUNTIME REPORTS NO VALID HEAD ORIENTATION. Head rotation is "
                  "the identity, so anything driven by it will do nothing at all while "
                  "looking like it works. Nothing downstream of this can be trusted.");
        }
    }

    // Feed the camera scanner the head basis whether or not head look is enabled: finding
    // the camera is a measurement, and the head pose is the one known value that makes the
    // camera's orientation searchable in memory.
    if (view_count >= 1) {
        const XrQuaternionf qh = views[0].pose.orientation;
        const float hx = qh.x * qh.x, hy = qh.y * qh.y, hz = qh.z * qh.z;
        const float hxy = qh.x * qh.y, hxz = qh.x * qh.z, hyz = qh.y * qh.z;
        const float hwx = qh.w * qh.x, hwy = qh.w * qh.y, hwz = qh.w * qh.z;
        const float hb[9] = {
            1.0f - 2.0f * (hy + hz), 2.0f * (hxy + hwz),        2.0f * (hxz - hwy),
            2.0f * (hxy - hwz),      1.0f - 2.0f * (hx + hz),   2.0f * (hyz + hwx),
            2.0f * (hxz + hwy),      2.0f * (hyz - hwx),        1.0f - 2.0f * (hx + hy)};
        mem_scan_note_head(hb);
    }

    // Head look: the game's camera is rotated by however far the head has turned from straight
    // ahead. Taken from eye 0's pose, which is the head pose in the same LOCAL space the screen is
    // anchored in; at the neutral pose the rotation is the identity, so "facing forward" means
    // "camera unrotated" with no separate recentring step.
    //
    // Handed over UNCONDITIONALLY. It used to be gated on the old head-look path's marker
    // (matrix_probe_head_enabled), which meant the newer consumer - the camera-steering probe that
    // writes the camera the renderer actually reads - would have received nothing unless an
    // unrelated feature was switched on. Storing the orientation costs a memcpy; deciding whether
    // to steer with it belongs to the consumer.
    if (view_count >= 1) {
        const XrQuaternionf q = views[0].pose.orientation;
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        // Row-major, columns = the head's right / up / backward axes (the same
        // convention the panel's own basis uses).
        const float r[9] = {
            1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz),        2.0f * (xz - wy),
            2.0f * (xy - wz),        1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx),
            2.0f * (xz + wy),        2.0f * (yz - wx),        1.0f - 2.0f * (xx + yy)};
        matrix_probe_set_head_rotation(r);
    }
    for (uint32_t eye = 0; eye < view_count && eye < 2; ++eye) {
        eyes_ok = impl_->draw_quad(d3d9_device, image_index_by_eye[eye], eye, views[eye].pose,
                                   views[eye].fov) && eyes_ok;
    }
    // The converged diagnostic reports one pose for both views. The runtime is
    // what turns a pair of view poses into the two sampled positions, so handing
    // it a zero separation is the only way to ask "is the separation yours?"
    if (impl_->converge_views && view_count == 2) {
        static bool s_conv_logged = false;
        if (!s_conv_logged) {
            s_conv_logged = true;
            VRLOG("converge: ipd_scale >= 1.5 -> both views submitted at the SAME pose "
                  "(eye0 %.3f %.3f %.3f). If the double image survives this, the runtime "
                  "is not deriving it from the pose we submit.",
                  views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z);
        }
        XrPosef shared = views[0].pose;
        views[0].pose = shared;
        views[1].pose = shared;
    }

    // Once, a few seconds in, keep a copy of what the runtime is being handed.
    // It has to happen before the image is released, while it is still ours.
    // Dump what the runtime is being handed so the composition can be inspected
    // off-line instead of guessed at. RE6VR_DUMP_FRAME picks the frame (0 = off).
    static uint64_t s_dump_frame = 0;
    static bool s_dump_parsed = false;
    if (!s_dump_parsed) {
        s_dump_parsed = true;
        wchar_t buf[32] = L"";
        if (GetEnvironmentVariableW(L"RE6VR_DUMP_FRAME", buf, 32) > 0) {
            s_dump_frame = (uint64_t)_wtoi64(buf);
        } else {
            s_dump_frame = 800;   // late enough that the game is showing something
        }
        // The eye dump and the frame dump have to land on the SAME frame to be
        // comparable: the eye image at frame 800 says nothing about what was on screen
        // at frame 1400. re6vr_dump_at.txt's first entry is therefore reused as the eye
        // dump frame whenever it is set, so one run produces a matched pair.
        if (impl_->extra_dump_count > 0) {
            s_dump_frame = impl_->extra_dump_list[0];
            VRLOG("compositor: eye dump aligned to frame %llu (same frame as the frame "
                  "dump, so the two can be compared)",
                  (unsigned long long)s_dump_frame);
        }
        // Solid-colour / stripe-ruler test switch. Parsed by a shared helper so
        // the offscreen self-tests reach the same code: they never call on_frame,
        // and a marker read only in on_frame silently never applied offline.
        impl_->read_test_solid_marker();

        // UEVR's Virtual Desktop trick: a second, identical projection layer on
        // top. Existence of the file is the switch.
        {
            const wchar_t *log_path = vrlog::path();
            if (log_path && log_path[0]) {
                wchar_t marker[MAX_PATH] = L"";
                wcsncpy_s(marker, MAX_PATH, log_path, _TRUNCATE);
                wchar_t *slash = wcsrchr(marker, L'\\');
                if (slash) {
                    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - marker)),
                             L"re6vr_vd_dummy.txt");
                    impl_->push_dummy_layer =
                        GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
                    // Same directory, so the same trick for the FOV switch.
                    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - marker)),
                             L"re6vr_asym_fov.txt");
                    if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) {
                        impl_->shared_fov = false;
                        static bool logged_asym = false;
                        if (!logged_asym) {
                            logged_asym = true;
                            VRLOG("submit: asymmetric per-eye FOV re-enabled by "
                                  "re6vr_asym_fov.txt (this is the setting that produced "
                                  "the double image)");
                        }
                    }
                }
            }
        }
        // Marker file for the extra frame dump, same reason as the other switches:
        // Steam does not pass the shell's environment on. Contents = the frame number.
        {
            const wchar_t *log_path = vrlog::path();
            if (log_path && log_path[0]) {
                wchar_t marker[MAX_PATH] = L"";
                wcsncpy_s(marker, MAX_PATH, log_path, _TRUNCATE);
                wchar_t *slash = wcsrchr(marker, L'\\');
                if (slash) {
                    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - marker)),
                             L"re6vr_dump_at.txt");
                    if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) {
                        FILE *f = _wfopen(marker, L"r");
                        if (f) {
                            // A list, so one run can catch several screens: the title
                            // screen is only up briefly, and finding it by guessing one
                            // frame number costs a whole run per guess.
                            long v = 0;
                            while (impl_->extra_dump_count < 8 &&
                                   fscanf_s(f, "%ld", &v) == 1) {
                                if (v > 0) {
                                    impl_->extra_dump_list[impl_->extra_dump_count++] =
                                        (uint64_t)v;
                                }
                            }
                            fclose(f);
                        }
                        if (impl_->extra_dump_count > 0) {
                            VRLOG("compositor: will dump the composite at %d frame(s), "
                                  "first %llu", impl_->extra_dump_count,
                                  (unsigned long long)impl_->extra_dump_list[0]);
                        }
                    }
                }
            }
        }
    }
    if (s_dump_frame && frames_submitted_ == s_dump_frame && !eye_dumped_) {
        eye_dumped_ = true;
        impl_->dump_eye_image(image_index, 0);
        impl_->dump_eye_image(image_index, 1);
    }
    XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    for (int c = 0; c < chains; ++c) {
        XrSwapchain sc = impl_->swapchains[c] ? impl_->swapchains[c] : impl_->swapchain;
        impl_->pfn_release_swapchain_image(sc, &release);
    }

    XrCompositionLayerProjectionView proj_views[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (uint32_t i = 0; i < view_count && i < 2; ++i) {
        proj_views[i].pose = views[i].pose;
        proj_views[i].fov = views[i].fov;
        proj_views[i].subImage.swapchain =
            impl_->sc_images_shared ? impl_->swapchain : impl_->swapchains[i];
        proj_views[i].subImage.imageRect.offset = {0, 0};
        proj_views[i].subImage.imageRect.extent = {(int32_t)impl_->sc_width, (int32_t)impl_->sc_height};
        proj_views[i].subImage.imageArrayIndex = impl_->sc_images_shared ? i : 0;
    }
    // The raw union, kept for the report below so all three stages (raw union -> 16:9
    // reshape -> submitted) can be printed side by side.
    float raw_l = 0.0f, raw_r = 0.0f, raw_u = 0.0f, raw_d = 0.0f;

    if (impl_->shared_fov && view_count == 2) {
        // One symmetric frustum covering both eyes' bounds, given to both.
        XrFovf f = {};        f.angleLeft = views[0].fov.angleLeft < views[1].fov.angleLeft
                          ? views[0].fov.angleLeft
                          : views[1].fov.angleLeft;
        f.angleRight = views[0].fov.angleRight > views[1].fov.angleRight
                           ? views[0].fov.angleRight
                           : views[1].fov.angleRight;
        f.angleUp = views[0].fov.angleUp > views[1].fov.angleUp ? views[0].fov.angleUp
                                                                : views[1].fov.angleUp;
        f.angleDown = views[0].fov.angleDown < views[1].fov.angleDown
                          ? views[0].fov.angleDown
                          : views[1].fov.angleDown;

        // The raw material this is all derived from, printed alongside the result. Two
        // runs in a row produced numbers that do not reconcile (a 108 deg union in one
        // field and 78.2 deg in another), and an unexplainable number in the one line that
        // exists to be the evidence is exactly the failure this project keeps hitting. So
        // the inputs, the reshaped frustum and the submitted one are all reported here.
        raw_l = f.angleLeft; raw_r = f.angleRight; raw_u = f.angleUp; raw_d = f.angleDown;
        {
            static int s_union_logs = 0;
            ++s_union_logs;
            if (s_union_logs <= 3 || (s_union_logs % 600) == 0) {
                const float dg = 57.2957795f;
                VRLOG("union: eye0 raw L%.4f R%.4f U%.4f D%.4f | eye1 raw L%.4f R%.4f U%.4f "
                      "D%.4f -> union L%.4f R%.4f U%.4f D%.4f (%.1f x %.1f deg, "
                      "tan-aspect %.3f) [tangent units]",
                      views[0].fov.angleLeft, views[0].fov.angleRight,
                      views[0].fov.angleUp, views[0].fov.angleDown,
                      views[1].fov.angleLeft, views[1].fov.angleRight,
                      views[1].fov.angleUp, views[1].fov.angleDown,
                      f.angleLeft, f.angleRight, f.angleUp, f.angleDown,
                      (atanf(f.angleRight) - atanf(f.angleLeft)) * dg,
                      (atanf(f.angleUp) - atanf(f.angleDown)) * dg,
                      (f.angleRight - f.angleLeft) / (f.angleUp - f.angleDown));
            }
        }

        // The union's VERTICAL span is narrowed to the picture's own 16:9, centred on the
        // eye's vertical centre.
        //
        // Taking the union on all four angles left the submitted region's shape up to the
        // headset: this one reports angleUp 44 / angleDown -55, so the union was 108 x 99
        // deg (tan-aspect 1.09) while the picture inside it is 16:9 (1.778). A compositor
        // maps the declared region onto the display as declared, so a 16:9 picture in a
        // 1.09 region is STRETCHED: 99 deg of display height carrying what 108/1.778 = 60.8
        // deg should, i.e. about 1.6x too tall, and every circle in the game reads as an
        // ellipse. The horizontal span is the one that has to stay wide (it is what removes
        // the double image - see `shared_fov` above), so the vertical is the one to fix.
        //
        // Measured, not assumed: the eye dumps hold the picture edge to edge (the quad fills
        // its slice in NDC by construction), so the aspect the runtime is told is the only
        // place this can go wrong, and the log's own "submitted tan-aspect 1.778" line next
        // to a 108 x 99 union is what exposed it.
        {
            // OpenXR's XrFovf fields are TANGENTS despite the name: the spec's own
            // conversion is angle = atan(tangent), which is why every reader in this file
            // wraps them in atanf. Running tanf() over them first - as this block did -
            // therefore applied a second tangent to an already-angular value and squashed
            // the horizontal span without anything saying so: the union's +-0.9425 tan
            // became tan(0.9425) = 1.376, so the block computed the 16:9 split of a 108 deg
            // window while the log reported the 86.6 deg one. Measured exactly, in 32-bit
            // float, with _work\stage_check.cpp: the old form reproduces the log's
            // "display's 108.0 x 73.7" and the new one gives "86.6 x 48.7" with a
            // tan-aspect of 1.7785 - 16:9, which is the whole point of the reshape.
            const float tan_l = f.angleLeft, tan_r = f.angleRight;
            const float tan_u = f.angleUp, tan_d = f.angleDown;
            const float cy = 0.5f * (tan_u + tan_d);          // keep the eye's vertical centre
            const float half_v = 0.5f * (tan_r - tan_l) / (16.0f / 9.0f);
            f.angleUp = atanf(cy + half_v);
            f.angleDown = atanf(cy - half_v);
        }

        proj_views[0].fov = f;        proj_views[1].fov = f;
        // Published for the camera code (see bridge_submitted_fov_v_deg): the vertical angle the
        // player's eyes are actually covering this frame, in degrees.
        g_submitted_fov_v_deg.store((f.angleUp - f.angleDown) * 57.2957795f);
        g_submitted_frames.fetch_add(1);
    }

    // ---- how big the screen is on the display --------------------------------
    //
    // Everything above this line decided *what* is drawn on the panel. This
    // decides how much of the headset's display that panel is declared to cover,
    // and until now nothing did: the frustum handed to the runtime was the
    // panel's own rectangle, and draw_quad renders the picture so that it exactly
    // fills that rectangle in NDC. Both ends match, so the fraction of the
    // display the screen occupies came out the same for every panel size and
    // every viewing distance - RE6VR_SCREEN_DIST and RE6VR_SCREEN_WIDTH moved the
    // panel through the world and changed the render resolution, but never made
    // the screen look bigger or smaller. That is the reported symptom, and it is
    // not a tuning problem: there was no path from those numbers to the submitted
    // FOV at all.
    //
    // The submitted FOV is the compositor's only source for "the pixels in this
    // rectangle represent these angles", so the screen's apparent size *is* this
    // number. Dividing the panel's rectangle fraction by a factor makes the
    // picture cover 1/factor of the display:
    //
    //   apparent = panel_half_tan / submitted_half_tan  (of the display's)
    //
    // The factor carries the panel's angular size, so the screen behaves like a
    // real object: k = (ref_dist / screen_dist) * (panel_w / ref_w), i.e. twice as
    // far away is half as wide, and twice as wide fills the display again from
    // twice as far. Only `dist` and `width` together decide how big the screen
    // looks - which is what those two numbers say on the tin.
    // RE6VR_SCREEN_SCALE (marker: re6vr_screen_scale.txt) multiplies on top of
    // that for "same distance, bigger screen". At the reference geometry
    // (4 m, 4.6 m wide) and scale 1 the factor is 1, which leaves the screen
    // exactly as it has been: filling the display.
    //
    // Only the submission is scaled, never the drawing: draw_quad keeps mapping
    // the picture onto its whole slice, and the slice is what gets declared
    // smaller. Scaling the projection instead would have changed nothing at all -
    // the panel is positioned to fill its frustum in NDC, so any uniform scale of
    // view*proj cancels out of the picture.
    //
    // Cost, since it is a real one: a smaller submitted region means fewer display
    // pixels carrying the same slice, so shrinking the screen softens it (at k = 2,
    // about half the linear resolution). Moving the screen further away is the
    // sharp direction - the slice is then downscaled instead of magnified - and
    // RE6VR_SWAPCHAIN_W/H should be lowered to match, e.g. 2492/k.
    {
        const float ref_dist = 4.0f;             // geometry at which the screen fills the display
        const float ref_w = 4.6f;
        const float d = impl_->screen_dist > 0.1f ? impl_->screen_dist : 0.1f;
        const float w = impl_->panel_w > 0.1f ? impl_->panel_w : 0.1f;
        float k = (ref_dist / d) * (w / ref_w);
        k *= impl_->screen_scale_user;
        if (!(k > 0.01f)) k = 0.01f;             // never invert or collapse

        // The display's own angular size, for the report below (and so "how much
        // of the display is this" can be read straight off the log).
        const float disp_l = proj_views[0].fov.angleLeft, disp_r = proj_views[0].fov.angleRight;
        const float disp_d = proj_views[0].fov.angleDown, disp_u = proj_views[0].fov.angleUp;

        for (uint32_t i = 0; i < view_count && i < 2; ++i) {
            proj_views[i].fov.angleLeft /= k;
            proj_views[i].fov.angleRight /= k;
            proj_views[i].fov.angleUp /= k;
            proj_views[i].fov.angleDown /= k;
        }

        // Report the angular size the compositor will actually build. All three stages are
        // in one line, in the same units, because a mixed-unit report is what made the
        // previous two runs unreadable: the old line divided one field by k, wrapped two
        // others in atanf(tanf()) - a second tangent on values that are already tangents -
        // and printed a "display" figure that was neither the union nor the submitted
        // frustum. Verified in 32-bit float in _work\stage_check.cpp.
        static int s_apparent_logs = 0;
        ++s_apparent_logs;
        if (s_apparent_logs <= 6 || (s_apparent_logs % 600) == 0) {
            const float deg = 57.2957795f;
            const XrFovf &sf = proj_views[0].fov;
            // THE FOV MATCH, stated in the units where it can actually be judged.
            //
            // "Is the picture stretched?" is exactly one ratio: the vertical angle the game RENDERS
            // versus the vertical angle the panel is DECLARED to occupy. Smaller declared angle = the
            // content is squeezed; larger = spread out. Read from the live camera, not from a marker, so
            // this line cannot agree with itself while disagreeing with the game.
            const float game_fov = re6vr::cam_steer_live_fov_deg();
            const float sub_v = (atanf(sf.angleUp) - atanf(sf.angleDown)) * deg;
            const float sub_h = (atanf(sf.angleRight) - atanf(sf.angleLeft)) * deg;
            const float panel_v = 2.0f * atanf(tanf(0.5f * impl_->panel_h / impl_->screen_dist)) * deg;
            VRLOG("apparent: dist %.2f m width %.2f m scale %.2f -> k=%.3f | raw union "
                  "%.1f x %.1f deg (tan-aspect %.3f) -> reshaped to 16:9 %.1f x %.1f deg "
                  "(tan-aspect %.3f) -> submitting %.1f x %.1f deg = %.0f%% of the display's "
                  "width",
                  impl_->screen_dist, impl_->panel_w, impl_->screen_scale_user, k,
                  (atanf(raw_r) - atanf(raw_l)) * deg,
                  (atanf(raw_u) - atanf(raw_d)) * deg,
                  (raw_r - raw_l) / (raw_u - raw_d),
                  (atanf(disp_r) - atanf(disp_l)) * deg,
                  (atanf(disp_u) - atanf(disp_d)) * deg,
                  (disp_r - disp_l) / (disp_u - disp_d),
                  (atanf(sf.angleRight) - atanf(sf.angleLeft)) * deg,
                  (atanf(sf.angleUp) - atanf(sf.angleDown)) * deg,
                  100.0f / k);
            // The one line that answers "is the game's picture stretched, and how much of the headset
            // does it cover".
            //
            // Both halves are needed and both must be compared in the SAME space. The submitted angles
            // are the ones the compositor uses; the game's fov is the vertical angle its frame was
            // rendered at. A 16:9 frame rendered at vertical angle V comes out at horizontal
            // atan(tan(V/2)*16/9), so the two are directly comparable - and if they differ, the picture
            // is stretched by exactly that ratio. Measured 2026-09-25: the game rendered 37 deg vertical
            // (61.5 deg horizontal) into a panel declared at 51.5 x 86.6 deg, i.e. squeezed horizontally
            // by 29%, which is what "the picture looks flat and far away" was.
            const float game_fov_h = 2.0f * atanf(tanf(0.5f * game_fov * 0.0174532925f) * 16.0f / 9.0f) / 0.0174532925f;
            const float match_h = (sub_h > 0.01f && game_fov > 0.0f) ? (game_fov_h / sub_h) : 0.0f;
            const float match_v = (sub_v > 0.01f && game_fov > 0.0f) ? (game_fov / sub_v) : 0.0f;
            const float eye_w = (atanf(views[0].fov.angleRight) - atanf(views[0].fov.angleLeft)) / 0.0174532925f;
            const float eye_h = (atanf(views[0].fov.angleUp) - atanf(views[0].fov.angleDown)) / 0.0174532925f;
            VRLOG("fov match: game renders %.1f v / %.1f h deg | panel declared %.1f v / %.1f h deg | "
                  "match %.2f v, %.2f h %s | covers %.0f%% x %.0f%% of the headset's %.1f x %.1f deg",
                  (double)game_fov, (double)game_fov_h, (double)sub_v, (double)sub_h,
                  (double)match_v, (double)match_h,
                  game_fov <= 0.0f ? "(no camera yet)"
                                   : (fabsf(match_v - 1.0f) < 0.06f && fabsf(match_h - 1.0f) < 0.06f
                                          ? "= NO STRETCH"
                                          : (match_h < 1.0f ? "= picture squeezed horizontally"
                                                            : "= picture stretched horizontally")),
                  (double)(100.0f * tanf(0.5f * atanf(tanf(0.5f * (sub_h * 0.0174532925f)))) /
                           tanf(0.5f * (eye_w * 0.0174532925f))),
                  (double)(100.0f * tanf(0.5f * (sub_v * 0.0174532925f)) /
                           tanf(0.5f * (eye_h * 0.0174532925f))),
                  (double)eye_w, (double)eye_h);
        }
    }

    XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    proj.space = impl_->local_space;
    proj.viewCount = 2;
    proj.views = proj_views;

    // Submission audit. Everything the runtime is handed for this frame, per eye,
    // printed once so the three claims that matter can be read off the log instead
    // of inferred:
    //
    //   1. each eye carries its OWN swapchain handle (two-swapchain mode),
    //   2. imageArrayIndex is 0 for both in that mode,
    //   3. the render target view draw_quad wrote is the one belonging to that
    //      eye's swapchain image - which is what the slot fix above is about.
    //
    // In the shared-array mode the two eyes must instead name the SAME handle
    // with different imageArrayIndex values, so the log distinguishes the
    // layouts at a glance.
    {
        static int s_submit_logs = 0;
        if (s_submit_logs < 6) {
            ++s_submit_logs;
            const char *mode = impl_->sc_images_shared ? "shared-array" : "two-swapchains";
            VRLOG("submit: layout=%s, view_count=%u, image_count=%u, acquire[eye0]=%u acquire[eye1]=%u",
                  mode, (unsigned)view_count, (unsigned)impl_->sc_image_count,
                  (unsigned)image_index_by_eye[0], (unsigned)image_index_by_eye[1]);
            for (uint32_t i = 0; i < view_count && i < 2; ++i) {
                const XrPosef &p = proj_views[i].pose;
                const XrFovf &f = proj_views[i].fov;
                const float deg = 57.2957795f;
                VRLOG("submit: eye%u swapchain=%p imageArrayIndex=%u imageRect=(%d,%d %dx%d)",
                      i, (void *)proj_views[i].subImage.swapchain,
                      (unsigned)proj_views[i].subImage.imageArrayIndex,
                      (int)proj_views[i].subImage.imageRect.offset.x,
                      (int)proj_views[i].subImage.imageRect.offset.y,
                      (int)proj_views[i].subImage.imageRect.extent.width,
                      (int)proj_views[i].subImage.imageRect.extent.height);
                VRLOG("submit: eye%u pos=(%.4f %.4f %.4f) quat=(%.4f %.4f %.4f %.4f)",
                      i, p.position.x, p.position.y, p.position.z,
                      p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
                VRLOG("submit: eye%u fov angleLeft=%.2f angleRight=%.2f angleUp=%.2f angleDown=%.2f deg",
                      i, f.angleLeft * deg, f.angleRight * deg, f.angleUp * deg, f.angleDown * deg);
            }
        }
    }

    // Optional second layer, on top, requested by re6vr_vd_dummy.txt.
    //
    // UEVR carries a switch for exactly this, and its comment cites the Virtual
    // Desktop author: "VD composites all layers using the top layer's pose". If
    // that is what VD does, then something about how a single layer is placed is
    // not what the app asked for, and adding a second layer changes which pose
    // the compositor works from.
    //
    // The dummy is given the same swapchain image and the same pose/fov as the
    // real layer's eye 0 for both eyes, so it is a second copy of content that is
    // already there: if the compositor ignores it the picture is unchanged
    // (identical pixels drawn twice), and if it honours it the placement changes.
    // Either way the app's own output cannot end up wrong - only possibly
    // duplicated, which is the point of the experiment.
    XrCompositionLayerProjection dummy = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView dummy_views[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    const bool push_dummy = impl_->push_dummy_layer && view_count == 2;
    if (push_dummy) {
        for (uint32_t i = 0; i < 2; ++i) {
            dummy_views[i].pose = views[0].pose;
            dummy_views[i].fov = views[0].fov;
            dummy_views[i].subImage.swapchain =
                impl_->sc_images_shared ? impl_->swapchain : impl_->swapchains[0];
            dummy_views[i].subImage.imageRect.offset = {0, 0};
            dummy_views[i].subImage.imageRect.extent = {(int32_t)impl_->sc_width,
                                                        (int32_t)impl_->sc_height};
            dummy_views[i].subImage.imageArrayIndex = 0;
        }
        dummy.space = impl_->local_space;
        dummy.viewCount = 2;
        dummy.views = dummy_views;
        static bool logged = false;
        if (!logged) {
            logged = true;
            VRLOG("submit: VD dummy layer pushed on top (same image and pose as eye 0, "
                  "so the picture can only change if the compositor uses its pose)");
        }
    }

    const XrCompositionLayerBaseHeader *layers[2] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader *>(&proj), nullptr};
    if (push_dummy) {
        layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&dummy);
    }
    XrFrameEndInfo end_info = {XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount = eyes_ok ? (push_dummy ? 2u : 1u) : 0u;
    end_info.layers = eyes_ok ? layers : nullptr;
    r = impl_->pfn_end_frame(impl_->session, &end_info);
    if (XR_FAILED(r)) {
        static bool warned = false;
        if (!warned) { VRLOG("openxr: xrEndFrame failed: %s", xr_result_str(r)); warned = true; }
        ++frames_failed_;
        return false;
    }

    ++frames_submitted_;
    if (frames_submitted_ == 1 || (frames_submitted_ % 600) == 0) {
        VRLOG("openxr: %llu frames submitted (%llu failed), display time %lld",
              (unsigned long long)frames_submitted_, (unsigned long long)frames_failed_,
              (long long)frame_state.predictedDisplayTime);
    }
    return true;
}

void OpenXrBridge::on_device_lost() {
    if (!impl_) return;
    if (!device_lost_) {
        VRLOG("openxr: D3D9 device lost - releasing device objects and pausing composition");
    }
    device_lost_ = true;
    // Before anything is released: the stereo path must stop touching the device NOW. The render-phase
    // hook keeps firing while the device is lost, and its capture would otherwise run against objects
    // that the next lines are about to free.
    impl_->note_standdown("device lost");
    impl_->destroy_frame_srv();
    impl_->release_shared_surface();
}

void OpenXrBridge::on_device_reset() {
    if (!device_lost_) return;
    VRLOG("openxr: D3D9 device restored - rebuilding the compositor surfaces");
    device_lost_ = false;
    // Rebuild on the next frame. Without this the surfaces released above are
    // never recreated and the headset stays black for the whole session, which is
    // exactly what the first fullscreen run looked like.
    if (pending_device_) {
        prepared_ = false;
        pending_ = true;
    }
}

void OpenXrBridge::shutdown() {
    if (!impl_) return;
    if (impl_->swapchain) impl_->pfn_destroy_swapchain(impl_->swapchain);
    impl_->swapchain = XR_NULL_HANDLE;
    if (impl_->local_space) impl_->pfn_destroy_space(impl_->local_space);
    impl_->local_space = XR_NULL_HANDLE;
    if (impl_->session) impl_->pfn_destroy_session(impl_->session);
    impl_->session = XR_NULL_HANDLE;
    impl_->destroy_frame_srv();
    impl_->release_shared_surface();
    if (impl_->instance) {
        // xrDestroyInstance lives on the loader; resolve it late.
        auto destroy = (PFN_xrDestroyInstance)GetProcAddress(impl_->loader_dll, "xrDestroyInstance");
        if (destroy) destroy(impl_->instance);
        impl_->instance = XR_NULL_HANDLE;
    }
    if (impl_->quad_vb) { impl_->quad_vb->Release(); impl_->quad_vb = nullptr; }
}

} // namespace re6vr
