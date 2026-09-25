// screenshot.h - capture the game's back buffer and judge whether this frame is worth scanning.
//
// Why this exists
// ---------------
// The camera probe has to scan process memory to find the live camera object, and a scan is only
// meaningful when the game is actually in a stage: a menu, a loading screen or a cutscene has no
// playable camera. Every attempt to decide that from a *signal* has failed - a 20 s timer landed
// on the title screen, `[stage object + 0x640]` stayed 0 through a whole session, and a
// stage-change marker never moved. Three runs were spent on gates that silently swallowed the
// scan.
//
// A screenshot is not a guess. It is the frame the game is actually drawing, so the question
// "was this a good moment to scan?" gets an answer that can be checked afterwards by looking at
// the image - and, cheaply, in the log, by the numbers this file computes.
//
// The capture happens on the render thread (the bridge calls `service()` while it already has the
// back buffer locked), because touching the back buffer from another thread is not safe. The
// probe only raises the request flag.
#pragma once

#include <cstdint>
#include <windows.h>

#include "d3d9_min.h"

namespace re6vr {

// Ask for a screenshot at the next frame. `label` is logged with the image, so a scan attempt and
// its picture can be matched up (use e.g. "scan 3/20").
void screenshot_request(const char *label);

// Called by the bridge with the back buffer locked. Returns true if it captured this frame.
// Cheap when nothing was requested (one atomic read).
bool screenshot_service(const uint8_t *pixels, uint32_t pitch, uint32_t w, uint32_t h);

// Serves a pending request from the EndScene hook, grabbing the back buffer itself.
//
// This is the path that works WITHOUT a headset: the bridge's own call sits inside the
// compositor's readback loop, which only runs while an OpenXR session exists. A screenshot is
// most useful exactly when XR is unavailable - that is when nobody can see the screen.
//
// The interface is `re6vr::IDirect3DDevice9` (the proxy's own minimal vtable declaration), the
// same type the bridge takes - there is no global IDirect3DDevice9 in this build.
void screenshot_service_from_device(IDirect3DDevice9 *device);

// How many screenshots have been written this session.
int screenshot_count();

// Synthetic check of the capture path and the verdict: two frames whose classification is known
// by construction (a gradient = gameplay, a black field with a pale panel = menu) go through the
// real sampler, and the file writer runs too. Run with RE6VR_SHOT_SELFTEST=1.
bool screenshot_selftest();

} // namespace re6vr
