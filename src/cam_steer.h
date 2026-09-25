// cam_steer.h - write the pose the RENDERER reads, and see the world turn.
//
// Why this exists (and why the earlier probes could never work)
// ------------------------------------------------------------
// Offline static analysis on 2026-09-25 found the read path (report:
// _work/cam_readpath_report.md, quoted bytes included):
//
//   sBioCamera::Update 0x503880 walks its 8 slot records (stride 0x190 from this+0x30), takes each
//   slot's camera object and calls its vtable slot 18 (offset +0x48 = 0x005F80B0). That function is
//   `uCamera*::GetViewMatrix(Matrix *out)` and it builds the view from
//
//       [ecx+0x50] cameraPos    [ecx+0x60] cameraUp    [ecx+0x70] targetPos
//       [ecx+0x4C] fov          [ecx+0x44] near        [ecx+0x40] far
//
//   by calling the engine's look-at builder 0xE6FD20. For the gameplay camera `ecx` is a
//   uCameraCtrl, which the module's own metadata names (vtable 0x152D620, slot 4 returns the
//   uCameraCtrl MtDti 0x017D26F0).
//
// Two things follow, and both were measured against the binary rather than assumed:
//
//   * mCameraOrg (+0xE30) has exactly ONE reader in the whole image (0x4FCB70, which copies it into
//     the second pose array at +0x1030). Nothing renders from it. That is the complete explanation
//     of "I write [this+0xE40] every frame and nothing moves" - it was never the live pose.
//   * The camera singleton is reachable WITHOUT any scan: sBioCamera := ds:[0x186E23C].
//
// So this probe does not guess an offset. It reads the singleton, walks the engine's own slot
// records, prints what it finds, and then swings the TARGET of the live camera around its own
// position by +-15 degrees about world Y. If the picture turns, the read path is confirmed and the
// same code becomes head tracking (swap the sine for the headset's yaw).
//
// Enabled by the marker file re6vr_steer.txt whose content selects the mode:
//   observe   only read and log what the slot table contains (no writes at all)
//   pushorg   hook sBioCamera::PushOrg (0x004F9950) and print the SOURCE OBJECT it is handed, its
//             vtable and its pos/up/target/fov - observation only, nothing is written
//   swing     rotate the live camera's target by +-15 deg about world Y, logging every change
//   head      rotate it by the HEADSET's yaw/pitch instead - this is head tracking
//   off       do nothing (same as no marker)
//
// In "head" mode the sensitivity comes from re6vr_head_gain.txt (a float, default 1.0).
#pragma once

#include "d3d9_min.h"

namespace re6vr {

// Called once from the first Present. Reads the marker and arms the probe; a no-op otherwise.
void cam_steer_install(IDirect3DDevice9 *dev);

// Called once per presented frame (data collection every 2 s, writes every frame while swinging).
void cam_steer_frame();

// True when a mode other than "off" is armed.
bool cam_steer_enabled();

// The fov (degrees) the camera the RENDERER uses is currently set to, or 0 before one exists.
//
// Published for the compositor's fov report: "is the game's picture stretched?" is one comparison -
// the vertical angle the game renders versus the vertical angle the panel is declared to occupy - and
// the compositor only knows its own half. Reading it from the live camera means the log compares what
// is actually happening on both sides instead of comparing a marker file with reality.
float cam_steer_live_fov_deg();

} // namespace re6vr
