// view_probe.h - find the view matrix the GAME actually renders with, and then find where that
// matrix lives in memory.
//
// Why this and not another offset guess
// -------------------------------------
// The camera object's mCameraOrg[0] (+0xE30) is written by the engine at level load with a real
// look-at pose, and writing targetPos there every frame changes nothing on screen. So the renderer
// does not read that copy. Five memory scans and one hook have all ended at the same wall: a
// camera value that is definitely the engine's, but definitely not the one being rendered.
//
// The ground truth that cannot lie is what the GPU is handed: every transform the engine uses has
// to arrive through IDirect3DDevice9::SetVertexShaderConstantF. So this probe reads that stream
// (read-only - every call is forwarded byte for byte), picks out the rigid 4x4 that is a view
// matrix, and then searches committed writable memory for those exact 64 bytes.
//
// Where the bytes are found is the answer the project needs, because that location is the thing to
// write for head tracking: it is by construction the memory the renderer is using.
//
// Enabled by the marker file re6vr_view.txt (any content). Safe to call more than once.
#pragma once

#include "d3d9_min.h"

namespace re6vr {

// Patches slots 94 and 109 of the device's own vtable copy (the mechanism the proxy already uses
// for Present/EndScene). A no-op unless the marker file exists.
void view_probe_install(IDirect3DDevice9 *dev);

// Called once per presented frame: advances the frame counter and starts the memory search once a
// view matrix has been seen and the camera has had time to settle.
void view_probe_frame();

// True when the probe is armed, so other code can avoid duplicate logging.
bool view_probe_enabled();

} // namespace re6vr
