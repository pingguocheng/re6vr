// mem_cam_tables.h - GENERATED. Do not edit by hand.
//
// The camera classes whose vtable address and MtDti record are known statically, exported by
// _work/build_vtables.py from what propmap.py could justify (a property table attributed to the
// class through the vtable run that contains its MtDti getter).
//
// Why the probe needs a SECOND identifying key
// --------------------------------------------
// It has been searching for objects whose class record sits at +0x04 - the MT Framework layout -
// and two real sessions (one of them 8400 frames inside a level) found zero sBioCamera objects
// and zero of ANY camera class, including uCameraAnimation, which the engine cannot do without.
// A search that finds nothing anywhere is not evidence about the camera; it is evidence about
// the search. The vtable is the other static fact available, it lives at +0x00, and it comes
// from the same verified analysis - so an object found through it either confirms the +0x04
// layout or shows what the layout really is.
//
// Fields: vtable VA, class record (MtDti) VA, instance size, class name.
#pragma once

namespace re6vr {

struct KnownClass {
    unsigned vtable;
    unsigned dti;
    unsigned size;
    const char *name;
};

// Only camera-family classes: a full census of 404 classes is a bigger change, and the camera is
// what this probe is for. Regenerate with:  python _work\build_vtables.py
static const KnownClass kKnownClasses[] = {
    {0x0151A380u, 0x017C3164u, 0x1620u, "sBioCamera"},
    {0x0152C3C0u, 0x018702E4u, 0x0080u, "uCamera"},
    {0x0152CA98u, 0x017D2720u, 0x0210u, "uCameraBlur"},
    {0x0152CBF8u, 0x017D26D0u, 0x0B50u, "uCameraQuake"},
    {0x0152D42Cu, 0x017D2090u, 0x0090u, "uCameraQuake::Param"},
    {0x0152D4F0u, 0x017D2760u, 0x0230u, "uCameraFovQuake"},
    {0x0152D7C8u, 0x017D27C0u, 0x0260u, "uCameraMotionSdl"},
    {0x0152D9E8u, 0x017D2A00u, 0x1B80u, "uCameraQFPS"},
    {0x0152DEA0u, 0x017D29C0u, 0x03F0u, "uCameraQFPS::cOffset"},
    {0x0152DF70u, 0x017D29E0u, 0x00F0u, "uCameraQFPS::cLookCamera"},
    {0x0152E178u, 0x017D2AA0u, 0x0280u, "uCameraVeh"},
    {0x016AAED4u, 0x018075D4u, 0x0200u, "cFSMOrder::FuncParamCamera"},
    {0x016AB020u, 0x01807434u, 0x00A0u, "cFSMOrder::FuncParamCameraQuake"},
    {0x016EE018u, 0x01870324u, 0x00B0u, "uFreeCamera"},
    {0x016F3CE0u, 0x01871170u, 0x0090u, "uOrthoCamera"},
    {0x0163F11Cu, 0x018131B0u, 0x00C0u, "cSmPlJeepCameraSiteParam"},
    {0x01644B08u, 0x017F81B0u, 0x00F0u, "uSmRide::CameraInfo"},
};

static const int kKnownClassCount = (int)(sizeof(kKnownClasses) / sizeof(kKnownClasses[0]));

// The two classes the analysis knows without a property table (no fields registered), taken from
// the static notes: sCamera is the renderer camera and the parent of sBioCamera; uCameraCtrl is
// the camera manager. Their vtables are listed in the DTI/class tables but carry no field table,
// so build_vtables.py cannot attribute them - they are added by hand, with the source noted.
//
//   sCamera      DTI 0x0186E260  size 0x0CE0   (disasm_lib/camera.py)
//   uCameraCtrl  DTI 0x017D26F0  size 0x4B70   (disasm_lib/camera.py)
//
// Their vtable addresses are NOT known statically, so the probe cannot key on them; they are
// reported only through the globals it reads.

} // namespace re6vr
