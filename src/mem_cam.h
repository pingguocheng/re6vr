// mem_cam.h - find and VERIFY the live sBioCamera instance in the running game.
//
// Why this exists
// ---------------
// The static work answered "which byte offset is the camera's look-at target" from BH6's own
// property-registration code (see scripts/disasm_lib/propmap.py):
//
//     sBioCamera  MtDti 0x017C3164  size 0x1620
//       mCameraOrg[i]         stride 0x40, i = 0..7, base +0xE30
//       +0x00 cameraPos   +0x10 targetPos   +0x20 cameraUp
//       +0x30 fov         +0x34 nearPlane   +0x38 farPlane
//
// What static analysis cannot answer is whether any of that is LIVE: every offset is worthless
// until an actual object is found and shown to hold a real camera pose. The README lists that
// as the decisive next step ("one memory read settles it"), and this module is that read.
//
// How it identifies the object without guessing an address
// -------------------------------------------------------
// An MT Framework instance carries its class record at +0x04, and the class record's address is
// a link-time constant (0x017C3164 for sBioCamera, relocated with the module if the image is
// relocated). So the identifying signature is not a shape and not a heuristic:
//
//     *(void **)(addr + 0x04) == 0x017C3164
//
// A shape like "orthonormal triple" matches plenty of things; the class pointer does not.
// Every candidate is then checked GEOMETRICALLY before anything is believed:
//
//   * cameraUp is a unit vector;
//   * forward = targetPos - cameraPos is unit length and perpendicular to cameraUp;
//   * fov is in (0.05, 2.2) rad;
//   * nearPlane < farPlane, both positive.
//
// That is the same evidence standard the rest of this project uses: a candidate becomes a
// conclusion only when a coincidence cannot produce it.
//
// Enabled by the marker file re6vr_cam.txt in the log/marker directory (next to re6vr.log).
#pragma once

namespace re6vr {

// Reads the marker file and, if present, starts the probe thread. Safe to call more than once.
// Does nothing unless re6vr_cam.txt exists.
void mem_cam_start();

// Synthetic check of the scan itself: plants one object the scan must find and one it must
// reject, runs the real search, and reports PASS/FAIL. Run with RE6VR_CAM_SELFTEST=1.
// It exists because the probe's failure mode is a confident zero, which looks the same whether
// the search is right or the needle is wrong. Returns true when the result set is exactly right.
bool mem_cam_selftest();

} // namespace re6vr
