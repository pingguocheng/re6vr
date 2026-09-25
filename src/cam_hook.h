// cam_hook.h - catch the engine's own camera update, instead of searching memory for the object.
//
// Why this replaces the search
// ---------------------------
// Five sessions tried to identify the live camera by scanning memory for an identifying value.
// Every key failed for a different reason (the class record at +4 is a runtime copy; the vtable
// key finds objects whose mCameraOrg fields are not a pose; the metadata tables look like
// objects), and each reason was only discoverable after a full game run.
//
// There is a way to avoid guessing altogether. Static analysis identified the function at
// 0x004FF9B0 (sBioCamera vtable slot 10, 463 instructions) that writes all 120 floats of
// `mCameraOrg[0..7]` - every entry, every field. If that function runs in the game, then hooking
// it hands over:
//
//   * the object address (`ecx` / the register holding `this`), with no search and no key;
//   * proof that the function is live, and how often it runs (per frame? per stage?);
//   * the values AS THE ENGINE WRITES THEM, which is the ground truth the offsets can be
//     checked against - a far better test than reading a field later and hoping.
//
// A hook cannot be fooled by a look-alike in a data table. That is the whole point.
//
// The hook is read-only: the original function still runs, and nothing is modified except that
// its arguments and its write targets get logged (at most a few times).
#pragma once

namespace re6vr {

// Installs the hook when `re6vr_camhook.txt` exists next to the log. Safe to call more than once.
void cam_hook_start();

// Called every frame (from the Present hook). With RE6VR_CAM_WRITE_TEST=1 it swings the camera's
// targetPos around its own position - the experiment that answers whether the renderer reads
// `mCameraOrg` per frame, which is the last unknown before head tracking.
void cam_write_test_frame();

} // namespace re6vr
