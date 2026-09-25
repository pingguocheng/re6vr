// openxr_bridge.h - OpenXR session + D3D9/D3D11 interop for the RE6 VR proxy.
//
// Stage 1 scope: present the game's back buffer as a world-locked virtual
// screen inside the headset, with correct per-eye projection so the panel looks
// undistorted from any head position. No stereo camera yet (that is stage 2).
#pragma once

#include <windows.h>

#include <cstdint>

#include "d3d9_min.h"

struct IDirect3DSurface9;

namespace re6vr {

// Back buffer geometry as reported by IDirect3D9::CreateDevice. Querying it
// through IDirect3DSurface9::GetDesc faults inside d3d9.dll on this machine, so
// the values captured at device creation are used instead.
extern bool g_safe_mode;

void      set_backbuffer_geometry(uint32_t w, uint32_t h, D3DFORMAT fmt);
uint32_t  backbuffer_w();
uint32_t  backbuffer_h();
D3DFORMAT backbuffer_format();

enum class XrState {
    NotTried,   // nothing attempted yet
    Unavailable,// runtime or headset missing -> stay flat, never retry
    Failed,     // tried and failed -> stay flat, never retry
    Ready,      // session running, frames submitted
};

class OpenXrBridge {
public:
    OpenXrBridge();
    ~OpenXrBridge();

    OpenXrBridge(const OpenXrBridge &) = delete;
    OpenXrBridge &operator=(const OpenXrBridge &) = delete;

    // Loads the loader, creates the instance/system/session/swapchain and sets
    // up the D3D9<->D3D11 bridge. `d3d9_device` must be a D3D9Ex device (the
    // caller falls back to plain D3D9 if Ex is unavailable).
    bool init(HMODULE proxy_module, IDirect3DDevice9 *d3d9_device);

    // Creates every D3D9 surface the compositor needs. Must be called right
    // after the game's device exists, before it renders anything: creating a
    // texture from inside a frame makes MT Framework stop with
    // "ERR09: Unsupported function.".
    bool prepare(IDirect3DDevice9 *d3d9_device, uint32_t w, uint32_t h, D3DFORMAT fmt);

    // Creates the queued surfaces once the runtime is up. Idempotent.
    bool try_create_pending();

    // Drops those surfaces (device reset, resolution change).
    void release_surfaces();

    // Mirrors the game's back buffer into the texture the compositor samples.
    // Called from the EndScene hook, which is the safe place to touch the back
    // buffer (doing it inside Present faults inside d3d9.dll).
    void on_end_scene(IDirect3DDevice9 *d3d9_device);

    // Copies the mirrored frame into the swapchain and submits it to the
    // runtime. Returns false if the caller should just present normally.
    bool submit(IDirect3DDevice9 *d3d9_device);

    // Called from IDirect3DDevice9::Reset hooks so we drop device-dependent
    // objects before the reset and rebuild them lazily afterwards.
    void on_device_lost();
    void on_device_reset();

    // True while the game's device is lost. A fullscreen D3D9 game loses it
    // during start-up, and D3D9 reports that as Present returning DEVICELOST -
    // DEVICENOTRESET only ever comes from TestCooperativeLevel, so the caller
    // watches for the first Present that succeeds again instead.
    bool device_lost() const { return device_lost_; }

    void shutdown();

    // Offline check of the panel rendering: draws with the real pipeline, shaders,
    // matrices and per-eye slices into offscreen targets and reports the colour of
    // each quadrant. Answers orientation, coverage and per-eye slicing without a
    // headset. Triggered by RE6VR_SELFTEST=1.
    //
    // `d3d9_device` is the device the caller is drawing with; it is only used by the stereo capture
    // check (RE6VR_STEREO_SELFTEST=1), which needs a real D3D9 device and gets none from the headset
    // path when no headset is attached. Pass nullptr if there is none.
    bool run_selftest(IDirect3DDevice9 *d3d9_device = nullptr);

    // Brings up the D3D11 device the offline checks use when there is no session. Call it from a point
    // in the frame where creating things is safe (EndScene), before run_selftest.
    bool prepare_selftest_d3d11();

    // Takes one eye's finished picture into that eye's own texture. Needed as a member because the
    // caller (bridge_capture_eye, called from the camera code between the two render passes) has no
    // bridge of its own; see the free function below.
    void capture_eye_now(IDirect3DDevice9 *d3d9_device, int eye);

    // Which eye the camera code is rendering this frame (0/1, -1 off); see bridge_note_current_eye.
    void note_current_eye(int eye);

    // The render target this eye's engine pass should be redirected into, or null when the redirect
    // path is not armed. Called from the SetRenderTarget detour.
    IDirect3DSurface9 *redirect_target_for_eye(int eye);

    // Copies one engine-drawn eye picture into the compositor texture for that eye.
    bool capture_redirect_eye(IDirect3DDevice9 *d3d9_device, int eye);

    XrState state() const { return state_; }

    // --- stats for logging ------------------------------------------------
    uint64_t frames_submitted() const { return frames_submitted_; }
    uint64_t frames_failed() const { return frames_failed_; }


private:
    struct Impl;
    Impl *impl_;
    XrState state_;
    bool prepared_;
    bool pending_ = false;
    IDirect3DDevice9 *pending_device_ = nullptr;
    uint32_t pending_w_ = 0;
    uint32_t pending_h_ = 0;
    D3DFORMAT pending_fmt_ = 0;
    uint32_t create_attempts_ = 0;   // transient failures during a mode change
    bool device_lost_;       // game device is mid-reset; do not touch its objects
    uint64_t frames_submitted_;
    uint64_t frames_failed_;
    bool eye_dumped_ = false;   // the eye image has been kept once for diagnosis
};

// fov matching, reported by the bridge: the VERTICAL field of view (DEGREES) actually submitted to the
// headset for the last frame, i.e. what the player's eyes are covering.
//
// Why the camera code needs it: the game renders its own 37-degree vertical fov into a texture that is
// then placed on a virtual screen, so the player sees a small rectangle in the middle of a much wider
// headset view. To make the game's image COVER that view - the minimum for anything that can be called
// VR - the game's fov has to be driven to match this number, and the only correct way to compute it is
// from what the runtime was actually handed. Returns 0 until a frame has been submitted.
float bridge_submitted_fov_v_deg();

// True while the bridge has a live session (state Ready), so the camera code can tell "no headset this
// run" apart from "headset up but no frames yet".
bool bridge_submitting();

// True while the game's D3D9 device may be used at all. False during the lost/reset window a
// fullscreen start-up spends ~200 ms in, and false before the session exists.
//
// Why the camera code needs this: the second render pass is issued from a hook on the engine's render
// phase, and that hook keeps firing straight through a device Reset. Asking the engine to render the
// whole scene twice into a device that is being reset is undefined behaviour, and the run of
// 2026-09-25 20:19 died exactly there - Reset at 29.971, surfaces rebuilt at 30.191, first stereo
// capture at 30.310, video-engine hang (LiveKernelEvent 141) and a null read inside the D3D11 driver
// by 30.35.
bool bridge_device_ready();

// The bridge that d3d9_proxy.cpp created, so code in other translation units can reach it without a
// second copy of the pointer. Null before the proxy has made one.
OpenXrBridge *active_bridge();
void set_active_bridge(OpenXrBridge *bridge);

// Copies one eye's just-rendered picture out of the game's device into that eye's own frame texture.
// Called between the two render passes (eye 0 - the picture exists only until the second pass paints
// over it) and from EndScene (eye 1). A no-op unless the bridge built its stereo surfaces, which
// itself only happens when re6vr_stereo.txt was set as the device was created.
void bridge_capture_eye(IDirect3DDevice9 *d3d9_device, int eye);

// Tells the bridge which eye the camera code is rendering right now, so the EndScene capture knows
// which eye's texture to fill. Called once per frame by the render-phase probe in the one-eye-per-frame
// mode; the bridge keeps its own copy rather than reaching into the camera module.
void bridge_note_current_eye(int eye);

// The render target the engine's pass for this eye should be redirected into, or null. Used by the
// SetRenderTarget detour in the camera module, which is how an engine pass ends up drawing into a
// texture of ours instead of the back buffer.
IDirect3DSurface9 *bridge_redirect_target_for_eye(int eye);

// Hands one engine-drawn eye picture to the compositor (EndScene, on the frame this eye is copied).
bool bridge_capture_redirect_eye(IDirect3DDevice9 *d3d9_device, int eye);

} // namespace re6vr
