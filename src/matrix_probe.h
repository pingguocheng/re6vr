// matrix_probe.h - read-only reconnaissance of the shader constants MT Framework
// uploads each frame.
//
// Stage 2 (real per-eye stereo) needs the scene's view and projection matrices, so
// that the second eye can be rendered from a camera offset by the IPD. MT Framework
// is closed source and its matrices never appear in a public header, but every
// transform it uses has to reach the GPU through SetVertexShaderConstantF. This
// probe hooks that entry point and classifies what it sees, turning "reverse
// engineer the engine" into "read two lines of log".
//
// Enabled with RE6VR_CAPTURE_VS=1. It never modifies the data it inspects: it only
// looks at the registers, classifies them, and forwards the call untouched.
#pragma once

#include "d3d9_min.h"

namespace re6vr {

// Patches the device's own vtable copy (same mechanism as Present/EndScene; MinHook
// crashes on this game, see README). Safe to call more than once. Does nothing
// unless RE6VR_CAPTURE_VS is set.
void matrix_probe_install(IDirect3DDevice9 *dev);

// True when capturing is enabled, so other code can avoid duplicate logging.
bool matrix_probe_enabled();

// Called once per presented frame, so the report can say which frame a matrix
// belongs to.
void matrix_probe_frame();

// Head look: hands the probe the head's orientation as a row-major 3x3 whose
// columns are the head's right / up / forward axes. The probe rotates the view
// matrix basis by it, anchored per view slot so re-uploads cannot compound.
// A no-op unless re6vr_head_view.txt exists.
void matrix_probe_set_head_rotation(const float *r);

bool matrix_probe_head_enabled();

// Reads re6vr_head_test.txt (nine numbers, row-major) as the head rotation, so the
// rotation maths can be checked offline. Called by matrix_probe_resolve.
void matrix_probe_load_test_rotation();

// The head orientation the OpenXR bridge most recently handed over: row-major 3x3 whose columns are
// the head's right / up / backward axes. Returns false until a valid pose has arrived, so a
// consumer (the camera-steering probe in src/cam_steer.cpp) can refuse to steer on stale data.
bool matrix_probe_head_rotation(float *out9);

} // namespace re6vr
