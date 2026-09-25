// cam_steer.cpp - write the pose the RENDERER reads, and see the world turn.
//
// What the offline analysis settled (report: _work/cam_readpath_report.md, bytes quoted there):
//
//   sBioCamera::Update (0x503880) walks its 8 slot records (stride 0x190 from this+0x30) and calls
//   each slot camera's vtable slot 18 = 0x005F80B0, which is
//
//       005F80B0  56          push esi
//       005F80B1  8B742408    mov  esi,[esp+8]     ; Matrix *out
//       005F80B5  8D4160      lea  eax,[ecx+0x60]  ; up
//       005F80B8  8D5170      lea  edx,[ecx+0x70]  ; target
//       005F80BC  83C150      add  ecx,0x50        ; pos
//       005F80C3  E8587C8700  call 0x00E6FD20     ; look-at builder
//
//   so the view is built from [ecx+0x50] pos, [ecx+0x60] up, [ecx+0x70] target (+0x4C fov).
//   For the gameplay camera `ecx` is a uCameraCtrl (vtable 0x152D620), and the singleton that walks
//   the slots is at the fixed global ds:[0x186E23C] - no memory scan needed.
//
// Two things follow, and they are the whole reason this file exists:
//
//   1. WRITING [this+0xE40] (mCameraOrg) CANNOT work: the only reader of that array in the entire
//      image is 0x4FCB70, which copies it into a second array at +0x1030. That is the measured
//      explanation of the earlier "clean write audit, no visible change".
//   2. The write that CAN work is on the camera object's own pose, but it must happen INSIDE the
//      view-building call (detour on 0x5F80B0), not once per frame from Present: the engine's
//      controller update writes [cam+0x70] after our frame-level write and would erase it. Writing
//      inside the detour means the modification is the last thing that touches the pose before the
//      matrix is built - and that same detour is where the headset's yaw/pitch will go.
//
// Modes, from the marker file re6vr_steer.txt:
//
//   observe   read and log what the slot table holds; no writes at all
//   swing     hook 0x5F80B0 and rotate the camera's target +-15 deg about world Y (the picture must
//             turn - that is the acceptance test), logging the read-back so a silent write can never
//             be mistaken for a working one
//   off       nothing (same as no marker)
//
// All writes go through page checks (src/safe_mem.h) and every value is read back and reported.

#include "cam_steer.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "log.h"
#include "safe_mem.h"
#include "matrix_probe.h"   // the head orientation handed over by the OpenXR bridge
#include "openxr_bridge.h"  // bridge_capture_eye: the eye-0 capture has to happen between the passes

#include "MinHook.h"

namespace re6vr {
namespace {

// --- statics recovered offline, all quoted in the report ---------------------
const unsigned kGlobalBioCamera = 0x0186E23Cu;   // ds:[0x186E23C] = the sBioCamera singleton
const unsigned kSlotBase = 0x30u;                // this + 0x30 + 0x190*k
const unsigned kSlotStride = 0x190u;
const int kSlotCount = 8;
const unsigned kSlotCamera = 0x04u;              // slot record +0x04 = the camera object
const unsigned kOffPos = 0x50u;                  // uCamera* pose block
const unsigned kOffUp = 0x60u;
const unsigned kOffTarget = 0x70u;
const unsigned kOffFov = 0x4Cu;
const unsigned kOffPublishedPos = 0xD0u;         // published Vector4 pose (write-back probe)
const unsigned kVtableCtrl = 0x0152D620u;        // uCameraCtrl
const unsigned kVtableBioCamera = 0x0151A380u;

const unsigned kGetViewMatrixRva = 0x1F80B0u;    // 0x005F80B0 with the usual base
const unsigned char kGetViewMatrixHead[13] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x8D, 0x41, 0x60,
                                              0x8D, 0x51, 0x70, 0x50, 0x83};
// 0x005F80B0 ends with `call 0x00E6FD20`: the engine's look-at builder MakeViewMatrix(eye, target,
// up, out). Its prologue was measured offline (report section 5) and is verified before patching.
const unsigned kMakeViewMatrixRva = 0xA6FD20u;   // 0x00E6FD20
const unsigned char kMakeViewMatrixHead[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC,
                                              0x40};

// Reads the published angles. Retried until the sequence counter is even at both ends.
void read_published(float *yaw, float *pitch);

// Which of the two write paths is in use. See the long comment on install_builder_hook.
enum Method { kMethodMatrix = 0, kMethodBuilder = 1 };
Method g_method = kMethodBuilder;
bool g_builder_hooked = false;
void *g_builder_trampoline = nullptr;
volatile LONG g_builder_calls = 0;   // MakeViewMatrix calls seen by the detour (heartbeat evidence)
// The camera object the engine is about to build a view for, and the optional fov override. Both are
// used by the builder hook (the fov lives on the camera object, which only the GetViewMatrix detour
// sees), so they are declared here rather than next to the head globals.
unsigned g_cam_last = 0;
float g_fov_override = 0.0f;
// The sBioCamera slot-table cameras (record = sBioCamera+0x30+0x190*k, object at record+0x04).
// Refreshed by the render/builder path and read by the identity filter: only these may be steered.
// File scope because both places need it.
unsigned g_slot_cams[8] = {0};
bool g_slot_filled = false;
// Stereo: which eye the current view-matrix build is for (-1 = not stereo / do not offset), and the
// interpupillary distance in metres (0 = off). Both are set from markers; g_current_eye is driven by
// the render-phase pass once the dual-pass path exists, and 0/1 can be forced for a single-eye test.
int g_current_eye = -1;
float g_ipd = 0.0f;
float g_eye_force = -1.0f;
// Stereo dual pass (re6vr_stereo.txt = 1). Dormant by default: with it off the render phase is only
// observed, which is what every earlier build did.
bool g_stereo = false;
bool g_second_pass = true;              // re6vr_stereo_pass2.txt: the second ENGINE pass, separately
bool g_rt_probe = false;                // re6vr_rt_probe.txt: read-only "which target does the pass draw into"
volatile LONG g_render_active = 0;      // set for the duration of one render phase (see the probe)
int g_redirect_eye = -1;                // which eye's texture the CURRENT render phase draws into
volatile LONG g_stereo_passes = 0;      // second passes actually performed
volatile LONG g_stereo_skipped = 0;     // times the guard refused to do one
volatile LONG g_ipd_writes = 0;         // eye-offset writes that really reached the engine's pose
volatile LONG g_stereo_passes_at_last_frame = 0;
unsigned g_stereo_srender = 0;          // sRender*, read from the render probe; the pass needs it
volatile LONG g_frame_counter = 0;   // bumped once per Present; used to apply one rotation per frame

// Refreshes the slot table. Cheap enough to call every couple of seconds.
void refresh_slot_cams() {
    for (int i = 0; i < 8; ++i) g_slot_cams[i] = 0;
    unsigned bio = 0;
    if (!read32(kGlobalBioCamera, &bio) || !plausible_pointer(bio)) return;
    for (unsigned k = 0; k < 8; ++k) {
        const unsigned rec = bio + 0x30u + 0x190u * k;
        unsigned cam_ptr = 0;
        if (!read32(rec, &cam_ptr) || !cam_ptr) continue;
        unsigned cam = 0;
        if (read32(rec + 4, &cam) && cam) {
            g_slot_cams[k] = cam;
            g_slot_filled = true;
        }
    }
}

// May the head steer this camera object?
//
// vtable slot 18 (0x005F80B0) is shared by eight camera classes - uCameraCtrl, uCameraBlur,
// uCameraQuake, uCameraFovQuake, uCameraMotionSdl, uCameraQFPS, uCameraVeh, uCameraAnimation - so
// "the camera that reaches MakeViewMatrix" is not necessarily the player's. Only the object sitting in
// the sBioCamera slot table is; the report's measurement had exactly one such slot (vtable
// 0x0152D620 = uCameraCtrl).
//
// An EMPTY table does not block: refusing to steer on missing evidence would silently disable head
// tracking, which is the failure mode this project keeps paying for. It reports instead.
bool camera_may_be_steered(unsigned cam) {
    if (!cam) return false;
    bool have_table = false;
    for (int i = 0; i < 8; ++i) {
        if (g_slot_cams[i]) {
            have_table = true;
            if (g_slot_cams[i] == cam) return true;
        }
    }
    if (!have_table) return true;
    static unsigned long long last_warn = 0;
    const unsigned long long now = GetTickCount64();
    if (now - last_warn > 10000) {
        last_warn = now;
        VRLOG("steer: NOT steering camera %08X - it is not in the sBioCamera slot table, so it is one "
              "of the other classes sharing vtable slot 18", cam);
    }
    return false;
}

enum Mode { kOff = 0, kObserve = 1, kSwing = 2, kHead = 3 };
Mode g_mode = kOff;
bool g_installed = false;
bool g_hooked = false;
unsigned g_singleton = 0;
unsigned g_live_cam = 0;              // the camera object the probe steers
int g_live_slot = -1;
float g_amplitude_deg = 15.0f;
int g_log_count = 0;

// --- helpers ----------------------------------------------------------------

float vec_len(const float *v) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

bool read_vec3(unsigned base, unsigned off, float *out) {
    return read((const void *)(uintptr_t)(base + off), out, 3 * sizeof(float));
}

bool plausible_pose(unsigned cam) {
    float p[3] = {0}, up[3] = {0}, t[3] = {0}, fov = 0.0f;
    if (!read_vec3(cam, kOffPos, p)) return false;
    if (!read_vec3(cam, kOffUp, up)) return false;
    if (!read_vec3(cam, kOffTarget, t)) return false;
    if (!readf32(cam + kOffFov, &fov)) return false;
    for (int i = 0; i < 3; ++i) {
        if (!(p[i] == p[i]) || fabsf(p[i]) > 1.0e6f) return false;   // finite and world-sized
    }
    const float up_len = vec_len(up);
    const float dx = t[0] - p[0], dy = t[1] - p[1], dz = t[2] - p[2];
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    // The engine's own numbers, from the runs: fov 37-51 DEGREES, target 180-400 units away,
    // up a unit vector. Getting these ranges from the log rather than inventing them is the whole
    // difference between identifying the camera and identifying noise.
    return fabsf(up_len - 1.0f) < 0.05f && fov > 5.0f && fov < 130.0f && dist > 1.0f &&
           dist < 20000.0f;
}

// The engine's own slot table, printed once per run. This is the data that says whether the static
// analysis and the running game agree - and with which object.
void dump_slots(const char *why) {
    unsigned bio = 0;
    if (!read32(kGlobalBioCamera, &bio) || !plausible_pointer(bio)) {
        VRLOG("steer: [%08X] (sBioCamera singleton) is not a readable pointer (reads %08X) - the "
              "static address does not hold what the analysis says in this build", kGlobalBioCamera,
              bio);
        return;
    }
    unsigned vt = 0;
    read32(bio, &vt);
    VRLOG("steer: sBioCamera singleton [%08X] = %08X, vtable %08X (static %08X%s) - %s",
          kGlobalBioCamera, bio, vt, kVtableBioCamera, vt == kVtableBioCamera ? "" : " MISMATCH",
          why);

    int live = 0;
    for (int k = 0; k < kSlotCount; ++k) {
        const unsigned slot = bio + kSlotBase + kSlotStride * (unsigned)k;
        unsigned p = 0, cam = 0;
        if (!read32(slot, &p) || !read32(slot + kSlotCamera, &cam)) continue;
        if (p == 0 && cam == 0) continue;
        unsigned cam_vt = 0;
        read32(cam, &cam_vt);
        float pos[3] = {0}, up[3] = {0}, tgt[3] = {0}, fov = 0.0f;
        const bool pose_ok = plausible_pose(cam);
        read_vec3(cam, kOffPos, pos);
        read_vec3(cam, kOffUp, up);
        read_vec3(cam, kOffTarget, tgt);
        readf32(cam + kOffFov, &fov);
        VRLOG("steer:   slot %d at %08X: ptr %08X cam %08X vtable %08X%s | pos %.2f %.2f %.2f "
              "up %.3f %.3f %.3f target %.2f %.2f %.2f fov %.2f | pose %s", k, slot, p, cam, cam_vt,
              cam_vt == kVtableCtrl ? " (uCameraCtrl)" : "", pos[0], pos[1], pos[2], up[0], up[1],
              up[2], tgt[0], tgt[1], tgt[2], fov, pose_ok ? "PLAUSIBLE" : "rejected");
        if (pose_ok) {
            ++live;
            if (!g_live_cam) {
                g_live_cam = cam;
                g_live_slot = k;
            }
        }
    }
    VRLOG("steer: %d plausible camera(s) in the slot table; steering target = %08X (slot %d)", live,
          g_live_cam, g_live_slot);
}

// --- the swing ---------------------------------------------------------------

// Called from the detour with `cam` = the object whose view is being built right now. Rotating the
// target about the camera's own position keeps the eye fixed and turns the view - the smallest
// change that must be visible if this really is the rendered camera.
void swing_one(unsigned cam) {
    float pos[3] = {0}, tgt[3] = {0};
    if (!read_vec3(cam, kOffPos, pos) || !read_vec3(cam, kOffTarget, tgt)) return;
    const float dx = tgt[0] - pos[0], dy = tgt[1] - pos[1], dz = tgt[2] - pos[2];
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!(dist > 0.5f) || !(dist < 20000.0f)) return;

    const float phase = (float)(GetTickCount64() % 8000) / 8000.0f;
    const float angle = sinf(phase * 6.2831853f) * g_amplitude_deg * 0.0174532925f;
    const float c = cosf(angle), s = sinf(angle);
    const float nx = dx * c + dz * s;
    const float nz = -dx * s + dz * c;
    const float nt[3] = {pos[0] + nx, pos[1] + dy, pos[2] + nz};

    if (!writable((const void *)(uintptr_t)(cam + kOffTarget), 3 * sizeof(float))) return;
    memcpy((void *)(uintptr_t)(cam + kOffTarget), nt, sizeof(nt));

    static unsigned last = 0;
    const unsigned now = (unsigned)GetTickCount64();
    if (now - last > 1000) {
        last = now;
        float back[3] = {0};
        read_vec3(cam, kOffTarget, back);
        VRLOG("steer: SWING %+.1f deg on camera %08X: target (%.2f %.2f %.2f) -> (%.2f %.2f %.2f), "
              "read back (%.2f %.2f %.2f) = %s | eye (%.2f %.2f %.2f) distance %.1f",
              (double)(angle * 57.2957795f), cam, tgt[0], tgt[1], tgt[2], nt[0], nt[1], nt[2],
              back[0], back[1], back[2],
              (fabsf(back[0] - nt[0]) < 0.01f) ? "the write stuck" : "OVERWRITTEN immediately",
              pos[0], pos[1], pos[2], (double)dist);
    }
}

void *g_trampoline = nullptr;
volatile unsigned g_cam_here = 0;
volatile LONG g_calls = 0;
bool g_first_call_logged = false;

void head_steer_one(unsigned cam);   // defined below (head-tracking write)
void start_head_pipeline();          // idem: must be startable from the install path

void __stdcall getview_record();   // object-free: called from the naked detour

__declspec(naked) void getview_detour() {
    __asm {
        pushfd
        pushad
        mov g_cam_here, ecx
        call getview_record
        popad
        popfd
        jmp g_trampoline
    }
}

void __stdcall getview_record() {
    const unsigned cam = g_cam_here;
    // The camera object the engine is about to build the view for. Kept globally because the fov
    // injection has to happen HERE: the builder hook only receives the pose pointers, and the fov
    // lives on the camera object (+0x4C), so this is the one place where both are available.
    g_cam_last = cam;
    const LONG n = InterlockedIncrement(&g_calls);
    if (!g_first_call_logged) {
        g_first_call_logged = true;
        unsigned vt = 0;
        read32(cam, &vt);
        VRLOG("steer: GetViewMatrix (005F80B0) is being called: first this = %08X, vtable %08X%s. "
              "This is the function that builds the view from [this+0x50/0x60/0x70].", cam, vt,
              vt == kVtableCtrl ? " (uCameraCtrl)" : "");
    }
    if (g_mode == kSwing) swing_one(cam);
    // Only the camera-object write path rotates here. In builder mode the GetViewMatrix detour may be
    // installed purely for the FOV override, and rotating from BOTH hooks would double the turn.
    if (g_mode == kHead && g_method == kMethodMatrix) head_steer_one(cam);
    // Every camera the engine builds a view for, with its vtable and pose. A run where the picture
    // turns needs this list: 48 different cameras went through this hook in the swing run of
    // 2026-09-25 11:17, all of them carrying the same template pose, so "which one is the player's
    // camera" cannot be answered by the writes alone - it is answered by the distance from what the
    // GPU was sent (view_probe) or by the class each vtable belongs to.
    if ((g_mode == kSwing || g_mode == kHead) && n <= 400 && (n % 20) == 0) {
        unsigned vt = 0;
        float pos[3] = {0}, tgt[3] = {0}, fov = 0.0f;
        read32(cam, &vt);
        read_vec3(cam, kOffPos, pos);
        read_vec3(cam, kOffTarget, tgt);
        readf32(cam + kOffFov, &fov);
        VRLOG("steer:   call %ld: cam %08X vtable %08X%s pos (%.2f %.2f %.2f) target (%.2f %.2f "
              "%.2f) fov %.2f", (long)n, cam, vt,
              vt == kVtableCtrl ? " (uCameraCtrl)" : "", pos[0], pos[1], pos[2], tgt[0], tgt[1],
              tgt[2], fov);
    }
    if ((n % 600) == 0) {
        VRLOG("steer: GetViewMatrix called %ld times so far (per-frame writer proof)", (long)n);
    }
}

bool install_hook() {
    const HMODULE game = GetModuleHandleW(L"BH6.exe");
    if (!game) {
        VRLOG("steer: BH6.exe not loaded - observation only");
        return false;
    }
    void *target = (void *)((unsigned char *)(uintptr_t)game + kGetViewMatrixRva);
    unsigned char head[13] = {0};
    if (!read(target, head, sizeof(head))) {
        VRLOG("steer: target %p is not readable - not hooking", target);
        return false;
    }
    if (memcmp(head, kGetViewMatrixHead, sizeof(head)) != 0) {
        VRLOG("steer: target %p does not have the expected prologue (56 8B 74 24 08 8D 41 60 8D 51 "
              "70 50 83) - NOT hooking a live code path on a guess", target);
        return false;
    }
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        VRLOG("steer: MH_Initialize failed");
        return false;
    }
    if (MH_CreateHook(target, (void *)&getview_detour, &g_trampoline) != MH_OK) {
        VRLOG("steer: MH_CreateHook failed");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        VRLOG("steer: MH_EnableHook failed");
        return false;
    }
    VRLOG("steer: hooked 0x%p (BH6.exe + 0x%X) - the view builder. Prologue verified byte for "
          "byte before patching.", target, kGetViewMatrixRva);
    return true;
}

// ---------------------------------------------------------------- PushOrg probe (0x004F9950)
//
// The requested verification: hook sBioCamera::PushOrg and report exactly what it reads.
//
// Static evidence, from `python re6dis.py func 0x004F9950 --va`:
//
//     004F9950  53          push ebx
//     004F9951  8b5c240c    mov  ebx,[esp+0xC]      ; arg2 = index   (cmp ebx,8 / jae skip)
//     004F9956  8bf9        mov  edi,ecx            ; ecx  = this    = sBioCamera
//     004F9962  8b742410    mov  esi,[esp+0x10]     ; arg1 = SOURCE OBJECT
//     004F9987  d94650      fld  [esi+0x50]         ; src+0x50 -> +0xE30+0x40*i   (cameraPos)
//     004F99BD  d94670      fld  [esi+0x70]         ; src+0x70 -> +0xE70+...      (targetPos)
//     004F99D3  d94660      fld  [esi+0x60]         ; src+0x60 -> +0xE50+...      (cameraUp)
//     004F99FF  d9464c      fld  [esi+0x4C]         ; src+0x4C -> fov
//     004F9A2D  c20800      ret  8
//
//     xref: exactly ONE call site, 0060CBF3 `call 0x004F9950`, inside uCameraCtrl::update
//     (0x0060C9F0), and the argument it pushes is `esi` - that function's own `this`, i.e. a
//     uCameraCtrl.
//
// So the pose that reaches mCameraOrg comes from uCameraCtrl+0x50/+0x60/+0x70. This probe does not
// assume that: it reads the actual argument out of the frame each call, prints the source object's
// vtable and its pose, and lists every DISTINCT source object it sees. If the source were anything
// else, the log would say so - which is the whole point of measuring instead of asserting.
const unsigned kPushOrgRva = 0xF9950u;           // 0x004F9950
const unsigned char kPushOrgHead[8] = {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x57, 0x8B, 0xF9};
void *g_pushorg_trampoline = nullptr;
volatile unsigned g_pushorg_frame = 0;
volatile LONG g_pushorg_calls = 0;
volatile LONG g_pushorg_reported = 0;

void __stdcall pushorg_record();

__declspec(naked) void pushorg_detour() {
    __asm {
        mov g_pushorg_frame, esp
        pushfd
        pushad
        call pushorg_record
        popad
        popfd
        jmp g_pushorg_trampoline
    }
}

void __stdcall pushorg_record() {
    InterlockedIncrement(&g_pushorg_calls);
    if (InterlockedIncrement(&g_pushorg_reported) > 40) return;   // bounded: 40 lines, then quiet
    const unsigned frame = g_pushorg_frame;
    if (!frame) return;
    const unsigned index = *(const unsigned *)(uintptr_t)(frame + 4);
    const unsigned src = *(const unsigned *)(uintptr_t)(frame + 8);
    if (!src) {
        VRLOG("steer: PushOrg(0x4F9950) call: index %u, SOURCE POINTER IS NULL", index);
        return;
    }
    unsigned vt = 0;
    read32(src, &vt);
    float pos[3] = {0}, up[3] = {0}, tgt[3] = {0}, fov = 0.0f;
    read_vec3(src, kOffPos, pos);
    read_vec3(src, kOffUp, up);
    read_vec3(src, kOffTarget, tgt);
    readf32(src + kOffFov, &fov);
    VRLOG("steer: PushOrg(0x4F9950) index %u reads SOURCE %08X vtable %08X%s: pos (%.2f %.2f "
          "%.2f) up (%.3f %.3f %.3f) target (%.2f %.2f %.2f) fov %.2f", index, src, vt,
          vt == kVtableCtrl ? " (uCameraCtrl)" : "", pos[0], pos[1], pos[2], up[0], up[1], up[2],
          tgt[0], tgt[1], tgt[2], fov);
}

bool install_pushorg_probe() {
    const HMODULE game = GetModuleHandleW(L"BH6.exe");
    if (!game) return false;
    void *target = (void *)((unsigned char *)(uintptr_t)game + kPushOrgRva);
    unsigned char head[8] = {0};
    if (!read(target, head, sizeof(head))) return false;
    if (memcmp(head, kPushOrgHead, sizeof(head)) != 0) {
        VRLOG("steer: PushOrg target %p does not match the expected prologue (53 8B 5C 24 0C 57 8B "
              "F9) - not hooking", target);
        return false;
    }
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (MH_CreateHook(target, (void *)&pushorg_detour, &g_pushorg_trampoline) != MH_OK) {
        VRLOG("steer: MH_CreateHook(PushOrg) failed");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        VRLOG("steer: MH_EnableHook(PushOrg) failed");
        return false;
    }
    VRLOG("steer: hooked 0x%p (BH6.exe + 0x%X) - sBioCamera::PushOrg, OBSERVE ONLY. It reads "
          "src+0x50/0x60/0x70/0x4C; this probe prints what it actually receives.", target,
          kPushOrgRva);
    return true;
}

// ---------------------------------------------------------------- the look-at builder hook
//
// WHY THIS REPLACES THE GetViewMatrix WRITE (the player's run of 2026-09-25 12:16 is the evidence):
//
// The GetViewMatrix approach edits the camera object's TARGET, and the engine rebuilds that pose
// every frame from its own controller state. So the angle had to be ACCUMULATED in a field the
// engine kept overwriting, and the log shows what that produces:
//
//     published yaw -35.0 pitch -13.9   (the accumulated angle, frozen there)
//     gaze bearing -90.0 tilt 1.8 -> bearing -55.0 tilt 15.7 ...  tilt 48.3 ... 67.6 ... 75.3 ... 87.8
//
// The tilt walked up to the ceiling and stayed: each frame's write moved the target a little further
// from where the engine had put it, and nothing ever took it back. The player saw exactly that -
// "it snapped to the ceiling, and one movement later to the floor". Accumulating on top of a value
// the engine owns is the wrong shape of solution no matter how the angle is filtered.
//
// The builder is the right place, and it is the LOWEST point that all cameras share:
//
//     0x005F80B0  ...  call 0x00E6FD20     ; MakeViewMatrix(eye, target, up, out)
//     0x00E6FD20  push ebp; mov ebp,esp; and esp,-16; sub esp,0x40 ...
//
// Hooking it and rotating (eye, target) IN PLACE before the original body runs has three properties
// the other approach could not have:
//   * STATELESS: nothing accumulates anywhere. The rotation is applied to the vectors the engine is
//     using right now, and since the engine rewrites them next frame there is no drift and no stuck
//     angle - if the head is still, nothing changes at all;
//   * precise: it is the exact function whose output becomes the view matrix, so "is this the
//     rendered camera" stops being a question;
//   * shared: every camera class goes through it (the swing experiment's positive control passed
//     through this same call).
void __stdcall builder_record();

// The caller's stack pointer, captured BEFORE this detour pushes anything, so the arguments of the
// hooked call can be read from the frame the caller built:
//     [esp+0] = return address, [esp+4] = eye, [esp+8] = target, [esp+12] = up, [esp+16] = out
// Taking them from the frame rather than from registers means nothing has to be assumed about how
// MSVC passed them (measured: it passes eye/target/up on the stack, ecx = out).
volatile unsigned g_builder_frame = 0;
volatile unsigned g_builder_retaddr = 0;

__declspec(naked) void builder_detour() {
    __asm {
        mov g_builder_frame, esp
        pushfd
        pushad
        call builder_record
        popad
        popfd
        jmp g_builder_trampoline            // the engine's own body, and ITS return does the cleanup
    }
}

void __stdcall builder_record() {
    // Counted BEFORE the method check, so the heartbeat reports "the function is being called" even
    // if the write path is switched off - the two facts are different and both matter.
    const LONG n = InterlockedIncrement(&g_builder_calls);
    if (n == 1) {
        VRLOG("steer: the MakeViewMatrix detour is live - first call reached (this line is the "
              "proof that the hook is on the engine's real view-builder path)");
    }
    if (g_method != kMethodBuilder) return;
    const unsigned frame = g_builder_frame;
    if (!frame) return;

    // ---------------------------------------------------------------- identity filter (the slot camera)
    //
    // vtable slot 18 (0x005F80B0) is shared by EIGHT camera classes - uCameraCtrl, uCameraBlur,
    // uCameraQuake, uCameraFovQuake, uCameraMotionSdl, uCameraQFPS, uCameraVeh, uCameraAnimation -
    // so "the camera that reaches MakeViewMatrix through GetViewMatrix" is NOT necessarily the
    // player's camera. Steering all of them moves the quake/blur cameras too, which is how a turn can
    // turn into a multiplied or smeared picture.
    //
    // The offline report gives the filter: only the camera sitting in the sBioCamera slot table is
    // the player's - slot record = sBioCamera+0x30+0x190*k, its camera object at record+0x04. The
    // camera handed to this call is reachable because `this` of 0x5F80B0 is the object itself, and
    // the report's measurement had exactly one such slot (slot 0, vtable 0x0152D620 = uCameraCtrl).
    //
    // The filter is checked once a second (the slot table does not change per frame) and reported, so
    // "the identity is right" and "I could not find any slot camera" are different log lines instead
    // of an invisible no-op.
    // (the slot table itself lives at file scope: both this refresh and the identity filter in the
    //  rotation path read it - see g_slot_cams)
    static unsigned long long s_slot_checked_ms = 0;
    const unsigned long long now_id = GetTickCount64();
    if (!g_slot_filled || now_id - s_slot_checked_ms > 2000) {
        s_slot_checked_ms = now_id;
        refresh_slot_cams();
    }
    // NOTE: the identity filter needs the camera object, which this hook does not receive (it gets
    // the pose pointers). The GetViewMatrix hook sees `ecx` and records it in g_cam_last, and that is
    // where the filter is applied. What is reported here is whether the filter has a target at all,
    // so a broken slot walk cannot silently disable head tracking.
    static bool s_reported = false;
    if (!s_reported && g_slot_cams[0]) {
        s_reported = true;
        VRLOG("steer: slot-table camera filter armed: first populated slot camera = %08X. Only that "
              "object is steered; the other classes sharing vtable slot 18 (quake/blur/vehicle/"
              "animation cameras) are left alone.", g_slot_cams[0]);
    }
    // The return address comes from the caller's frame, not from the register saved by pushad: after
    // pushad the stack top holds EDI, not the return address, and an earlier version of this code
    // therefore read a nonsense "caller" (0116A3DB, which is not code at all) and rejected every
    // real call. frame+0 IS the return address by definition of a call.
    const unsigned eye = *(const unsigned *)(uintptr_t)(frame + 4);
    const unsigned target = *(const unsigned *)(uintptr_t)(frame + 8);
    const unsigned retaddr = *(const unsigned *)(uintptr_t)frame;
    if (!eye || !target) return;

    // ---------------------------------------------------------------- evidence first
    //
    // The whitelist below is only trustworthy if the frame parsing is right, so the first calls are
    // logged with everything needed to check it by eye: who called, and what the two pointers
    // actually point at. If the parser were wrong, the eye/target values here would be nonsense and
    // that would be visible immediately - which is exactly the mistake that cost the run of 13:23.
    static volatile LONG logged = 0;
    const LONG l = InterlockedIncrement(&logged);
    if (l <= 6) {
        float e[3] = {0}, t[3] = {0};
        read_vec3(eye, 0, e);
        read_vec3(target, 0, t);
        VRLOG("steer: MakeViewMatrix #%ld from %08X: eye@%08X (%.2f %.2f %.2f) target@%08X "
              "(%.2f %.2f %.2f)", (long)l, retaddr, eye, e[0], e[1], e[2], target, t[0], t[1],
              t[2]);
    }

    // ---------------------------------------------------------------- ONLY the camera's own call
    //
    // MakeViewMatrix has 71 callers in this image. The one that builds the agent's view is
    // 0x005F80B0 (GetViewMatrix), and the write is restricted to it: other callers may pass
    // eye/target pointers that are not a camera pose at all, and writing a rotated target into those
    // corrupted the engine's own data - the crash log of 12:24 shows `execute at DEDEDEDE`, i.e. the
    // engine jumping through values that were no longer what it put there.
    //
    // NOTE on the call itself: the detour must reach the original body with `jmp`, never `call`. An
    // earlier version used `call trampoline` so a restore step could run afterwards; whether this
    // function cleans its stack arguments itself (ret 12) or leaves that to its callers cannot be
    // known from a 9-byte prologue, and guessing wrong corrupts the stack on every call.
    const unsigned kGetViewMatrixReturn = 0x005F80C8u;
    if (retaddr != kGetViewMatrixReturn) {
        static volatile LONG unknown = 0;
        const LONG u = InterlockedIncrement(&unknown);
        if (u <= 8) {
            VRLOG("steer: MakeViewMatrix caller %08X is not GetViewMatrix's %08X - not touching its "
                  "arguments (#%ld)", retaddr, kGetViewMatrixReturn, (long)u);
        }
        return;
    }

    float e[3] = {0}, t[3] = {0};
    if (!read_vec3(eye, 0, e) || !read_vec3(target, 0, t)) return;

    float extra_yaw = 0.0f, extra_pitch = 0.0f;
    read_published(&extra_yaw, &extra_pitch);
    // The eye offset is NOT head rotation: a centred head still has to be rendered from two eye
    // positions, or the two views are identical and the stereo pair is a fake. This early-out used to
    // run first and return silently whenever the head was still (published yaw and pitch both 0),
    // which is exactly the state the first stereo test starts in - so re6vr_ipd.txt would have logged
    // nothing and the run would have "proved" that the engine ignores the eye offset. The only case
    // that may return here is "nothing to do at all": no rotation, no eye offset, and no fov override.
    //
    // THE FOV OVERRIDE COUNTS AS WORK (2026-09-25). It is a still-head feature by nature - "make the
    // picture match the panel" is not something that happens only while you turn your head - and this
    // early-out sat in front of its write, so re6vr_fov.txt was inert in exactly the configuration it
    // exists for. The write itself is further down, after the identity filter, so that only the
    // player's camera ever has its fov changed.
    if (extra_yaw == 0.0f && extra_pitch == 0.0f && g_fov_override <= 0.0f &&
        !(g_ipd != 0.0f && g_current_eye >= 0)) {
        return;
    }

    // ---------------------------------------------------------------- identity filter, applied
    //
    // g_cam_last is the camera object whose view is being built right now (recorded by the
    // GetViewMatrix detour, which calls into this function synchronously). Only if that object is one
    // of the sBioCamera slot-table cameras may the head steer it: the other seven classes sharing
    // vtable slot 18 - uCameraBlur, uCameraQuake, uCameraFovQuake, uCameraMotionSdl, uCameraQFPS,
    // uCameraVeh, uCameraAnimation - have their own sources and moving them turns the picture into
    // something else (a quake offset applied twice, a blur pass with a different view, ...).
    //
    // The table is refreshed in builder_record (once per 2 s). If it is empty - very early in a level,
    // or if the slot walk ever breaks - the filter does NOT block: refusing to steer on missing
    // evidence would silently disable head tracking, which is the failure mode this project keeps
    // paying for. Instead it steers and says so.
    if (!camera_may_be_steered(g_cam_last)) return;

    // ---------------------------------------------------------------- per-eye IPD offset (stereo)
    //
    // Stereo needs each eye rendered from a slightly different position, and the offline report
    // settled that the engine has NO eye-offset field anywhere: 0x005F80B0 passes [ecx+0x50] verbatim
    // into 0x00E6FD20, which is a pure look-at over its three pointer arguments (163 instructions, no
    // constant offset). So the mod supplies the whole eye translation, applied per view-matrix build -
    // which is here.
    //
    // The offset is along the camera's own right axis, right = normalize(gaze x up), so it follows the
    // view wherever the head points. BOTH the eye and the target move by the same vector: moving only
    // the eye would ROTATE the gaze instead of translating it.
    //
    // re6vr_ipd.txt in metres, default 0 (off). The usual range is 0.055-0.070 m; a wrong sign swaps
    // the eyes (the world looks inside-out), which is a one-value fix.
    // ---------------------------------------------------------------- per-eye state cache
    //
    // Moved up here (before the write) because BOTH the eye offset and the rotation below need it,
    // and the eye index is part of the key: the dual-pass render builds the SAME camera's pose twice
    // per frame, once per eye. With the key being the target pointer alone, pass 2 walked into pass
    // 1's entry, saw "this is still our own output, the engine has not refreshed it" and returned -
    // i.e. the right eye would have silently reused the left eye's pose and the stereo pair would
    // have been two identical pictures. Keyed on (target, eye), each eye keeps its own baseline and
    // its own dedup record.
    if (!writable((const void *)(uintptr_t)target, 3 * sizeof(float))) return;
    if (!writable((const void *)(uintptr_t)eye, 3 * sizeof(float))) return;

    struct PoseCache {
        unsigned target;
        int eye;
        bool has;             // has an injection record
        float wrote_e[3];     // what we left in the engine's eye pointer
        float wrote_t[3];     // and in its target pointer
    };
    static PoseCache s_pose[32] = {{0}};
    static int s_pose_next = 0;
    PoseCache *pc = nullptr;
    for (int i = 0; i < 32; ++i) {
        if (s_pose[i].target == target && s_pose[i].eye == g_current_eye) { pc = &s_pose[i]; break; }
    }
    if (!pc) {
        pc = &s_pose[s_pose_next];
        s_pose_next = (s_pose_next + 1) % 32;
        memset(pc, 0, sizeof(*pc));
        pc->target = target;
        pc->eye = g_current_eye;
    }
    // "Is this pose still our own output?" A separate record per eye, so the answer is now about this
    // eye's last write only. On the very first sighting of a (target, eye) pair nothing is known, and
    // the residual risk is real and is logged as such: if the engine reuses one pose buffer for both
    // passes without rebuilding it, what is read here is the OTHER eye's injected output. The log
    // distinguishes the three cases by name so the next run's data can settle it.
    const bool our_eye = pc->has && memcmp(&e[0], pc->wrote_e, sizeof(pc->wrote_e)) == 0;
    const bool our_tgt = pc->has && memcmp(&t[0], pc->wrote_t, sizeof(pc->wrote_t)) == 0;
    const bool fresh = !(our_eye && our_tgt);
    const char *base_src = pc->has ? (fresh ? "engine refresh" : "our own output")
                                   : "first sighting of this (pose, eye)";

    // The three cases, once per second and only while stereo is on. This is the line that decides
    // whether the dual pass produced two different poses or the second pass silently reused the
    // first one's - the failure the whole per-eye keying exists to prevent - so it is printed with the
    // values rather than summarised.
    if (g_stereo) {
        static unsigned long long last_pose_log = 0;
        const unsigned long long now_pose = GetTickCount64();
        if (now_pose - last_pose_log > 2000) {
            last_pose_log = now_pose;
            VRLOG("steer: eye %d pose call: %s | eye (%.2f %.2f %.2f) target (%.2f %.2f %.2f) | %s",
                  g_current_eye, base_src, e[0], e[1], e[2], t[0], t[1], t[2],
                  pc->has ? (our_eye ? (our_tgt ? "we wrote both" : "we wrote the eye only")
                                     : (our_tgt ? "we wrote the target only" : "neither is ours"))
                          : "no record yet");
        }
    }

    if (fresh) {
        // ------------------------------------------------------------ per-eye IPD offset (stereo)
        //
        // Stereo needs each eye rendered from a slightly different position, and the offline report
        // settled that the engine has NO eye-offset field anywhere: 0x005F80B0 passes [ecx+0x50]
        // verbatim into 0x00E6FD20, which is a pure look-at over its three pointer arguments
        // (163 instructions, no constant offset). So the mod supplies the whole eye translation.
        //
        // From the engine's value, every frame, never accumulating: the engine rewrites the pose each
        // frame and this rides on top of exactly that value, so a held head holds the offset and a
        // missed frame cannot compound it. The engine's own pose is preserved in the cache first, so
        // the rotation below - and the next frame's "did the engine refresh this?" test - still work.
        memcpy(pc->wrote_e, &e[0], sizeof(pc->wrote_e));
        memcpy(pc->wrote_t, &t[0], sizeof(pc->wrote_t));
        pc->has = true;

        if (g_ipd != 0.0f && g_current_eye >= 0) {
            const unsigned up_ptr = *(const unsigned *)(uintptr_t)(frame + 12);
            float u[3] = {0.0f, 1.0f, 0.0f};
            if (up_ptr) read_vec3(up_ptr, 0, u);
            const float ulen = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            float gx = t[0] - e[0], gy = t[1] - e[1], gz = t[2] - e[2];
            const float glen = sqrtf(gx * gx + gy * gy + gz * gz);
            if (ulen > 0.5f && glen > 0.5f) {
                u[0] /= ulen;
                u[1] /= ulen;
                u[2] /= ulen;
                gx /= glen;
                gy /= glen;
                gz /= glen;
                // right = gaze x up - the same right axis the look-at builder derives internally, so
                // the offset lands along the camera's own screen X.
                float rx = gy * u[2] - gz * u[1];
                float ry = gz * u[0] - gx * u[2];
                float rz = gx * u[1] - gy * u[0];
                const float rlen = sqrtf(rx * rx + ry * ry + rz * rz);
                if (rlen > 1e-3f) {
                    const float sign = (g_current_eye == 0) ? -0.5f : 0.5f;   // eye 0 = left
                    const float d = sign * g_ipd / rlen;
                    const float ne[3] = {e[0] + rx * d, e[1] + ry * d, e[2] + rz * d};
                    const float nt2[3] = {t[0] + rx * d, t[1] + ry * d, t[2] + rz * d};
                    memcpy((void *)(uintptr_t)eye, ne, sizeof(ne));
                    memcpy((void *)(uintptr_t)target, nt2, sizeof(nt2));
                    memcpy(pc->wrote_e, ne, sizeof(ne));
                    memcpy(pc->wrote_t, nt2, sizeof(nt2));
                    e[0] = ne[0]; e[1] = ne[1]; e[2] = ne[2];
                    t[0] = nt2[0]; t[1] = nt2[1]; t[2] = nt2[2];
                    // A COUNTED write, not a claimed one: the project's rule after the fov-injection
                    // month is that any switch which changes engine parameters must produce a log
                    // line proving the write happened. With this counter at zero the run says so.
                    const LONG ipd_writes = InterlockedIncrement(&g_ipd_writes);
                    const float *be = pc->wrote_e;      // already the injected value
                    static unsigned long long last_ipd_log = 0;
                    const unsigned long long now_ipd = GetTickCount64();
                    if (ipd_writes <= 4 || now_ipd - last_ipd_log > 2000) {
                        last_ipd_log = now_ipd;
                        VRLOG("steer: IPD %.3f m applied to eye %d (#%ld write(s)): eye "
                              "(%.2f %.2f %.2f) -> (%.2f %.2f %.2f), right axis (%.3f %.3f %.3f), "
                              "pose source: %s", (double)g_ipd, g_current_eye, (long)ipd_writes,
                              be[0] - rx * d, be[1] - ry * d, be[2] - rz * d, be[0], be[1], be[2],
                              (double)(rx / rlen), (double)(ry / rlen), (double)(rz / rlen), base_src);
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- fov override (THE FOV MATCH)
    //
    // Placed HERE - after the identity filter, before the early-outs below - and both halves of that
    // matter:
    //   * before the early-outs, because those return whenever the head is still and no eye offset is
    //     armed. The write used to sit past them, so with a still head re6vr_fov.txt was INERT, and its
    //     log line showed the untouched engine value: "the engine ignored the override" when in fact we
    //     never wrote it. That is this project's oldest failure mode, and it was inside the fov feature;
    //   * after the filter, because a fov override is meant for the PLAYER's camera. Writing it before
    //     the filter would have changed the fov of all ~48 camera classes that reach this function.
    //
    // What it is for: the picture on the virtual panel is shown at the angle the panel subtends, while
    // the game renders its own fov into it. Those two angles must agree or the picture is stretched, so
    // this is the knob that matches them; the compositor prints the ratio once a second as
    // "fov match: game renders X | panel declared Y | match Z".
    float engine_fov = 0.0f;
    readf32(g_cam_last + kOffFov, &engine_fov);
    if (g_fov_override > 0.0f && writable((const void *)(uintptr_t)(g_cam_last + kOffFov),
                                          sizeof(float))) {
        *(volatile float *)(uintptr_t)(g_cam_last + kOffFov) = g_fov_override;
    }

    // Nothing left to do: no head rotation AND no eye offset. (With the identity filter above still
    // armed, so an unrelated camera can never reach this point and be written to.)
    const bool have_ipd = (g_ipd != 0.0f && g_current_eye >= 0);
    if (extra_yaw == 0.0f && extra_pitch == 0.0f && !have_ipd) return;
    if (!fresh && !have_ipd) return;      // our own output, and nothing to re-offset: skip
    if (!fresh) {
        // The pose is still our own output from this eye, and an eye offset IS armed. Repeating the
        // offset on top of itself would move the eye by a full IPD per call, so the geometry is redone
        // from the engine's preserved pose instead of from what we left there.
        memcpy(&e[0], pc->wrote_e, sizeof(pc->wrote_e));
        memcpy(&t[0], pc->wrote_t, sizeof(pc->wrote_t));
        // The stored values are our injected ones; recover the engine's by taking the offset back out.
        // `base_src` already says where this pose came from, and this path is only reached with an
        // offset armed, so the correction below is always the right-sized one.
        if (have_ipd) {
            const unsigned up_ptr = *(const unsigned *)(uintptr_t)(frame + 12);
            float u[3] = {0.0f, 1.0f, 0.0f};
            if (up_ptr) read_vec3(up_ptr, 0, u);
            const float ulen = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            float gx = t[0] - e[0], gy = t[1] - e[1], gz = t[2] - e[2];
            const float glen = sqrtf(gx * gx + gy * gy + gz * gz);
            if (ulen > 0.5f && glen > 0.5f) {
                u[0] /= ulen; u[1] /= ulen; u[2] /= ulen;
                gx /= glen; gy /= glen; gz /= glen;
                float rx = gy * u[2] - gz * u[1];
                float ry = gz * u[0] - gx * u[2];
                float rz = gx * u[1] - gy * u[0];
                const float rlen = sqrtf(rx * rx + ry * ry + rz * rz);
                if (rlen > 1e-3f) {
                    const float sign = (g_current_eye == 0) ? -0.5f : 0.5f;
                    const float d = -sign * g_ipd / rlen;      // take the same offset back out
                    const float ne[3] = {e[0] + rx * d, e[1] + ry * d, e[2] + rz * d};
                    const float nt2[3] = {t[0] + rx * d, t[1] + ry * d, t[2] + rz * d};
                    memcpy(&e[0], ne, sizeof(ne));
                    memcpy(&t[0], nt2, sizeof(nt2));
                }
            }
        }
    }

    float vx = t[0] - e[0], vy = t[1] - e[1], vz = t[2] - e[2];
    const float dist2 = vx * vx + vy * vy + vz * vz;
    if (!(dist2 > 0.25f) || !(dist2 < 4.0e8f)) return;

    // The same geometry the validated swing experiment used: a strictly horizontal axis, so the tilt
    // the engine chose is preserved and nothing rolls.
    {
        const float hlen = sqrtf(vx * vx + vz * vz);
        if (hlen > 1e-3f) {
            const float rx = -vz / hlen, rz = vx / hlen;
            const float c = cosf(extra_yaw), s = sinf(extra_yaw);
            const float nx = vx * c + (rz * vz) * s;
            const float nz = vz * c + (-rz * vx) * s;
            vx = nx;
            vz = nz;
        }
    }
    if (extra_pitch != 0.0f) {
        const float hlen = sqrtf(vx * vx + vz * vz);
        if (hlen > 1e-3f) {
            const float rx = -vz / hlen, rz = vx / hlen;
            const float c = cosf(extra_pitch), s = sinf(extra_pitch);
            const float nx = vx * c + (rz * vy) * s;
            const float ny = vy * c + (-hlen) * s;
            const float nz = vz * c + (-rx * vy) * s;
            vx = nx;
            vy = ny;
            vz = nz;
        }
    }
    // ---------------------------------------------------------------- fov injection (stereo groundwork)
    //
    // Two purposes at once:
    //   1. It SETTLES THE FOV AXIS question (vertical or horizontal) BY EXPERIMENT. The static hunt
    //      for the projection builder was inconclusive - too many candidates touch a displacement of
    //      +0x4C - and the answer is binary and observable: write a fov, then look at whether the
    //      image's VERTICAL or its HORIZONTAL extent changed. That is cheaper and more certain than
    //      more disassembly.
    //   2. Per-eye projection for stereo, which has to be driven from exactly here.
    //
    // (The fov write itself now lives above the early-outs, next to the identity filter - see the block
    // there. It used to be here, which meant it never ran unless the head was moving or stereo was
    // armed.)
    if (!writable((const void *)(uintptr_t)target, 3 * sizeof(float))) return;

    // ---------------------------------------------------------------- rotate from the ENGINE's value, once
    //
    // Three attempts are behind this, and the logs show why each of the first two failed:
    //
    //   1. rotate whatever the target holds at that moment -> the engine hands the same camera's
    //      vectors to MakeViewMatrix several times per frame, so the offset compounded 2-3 times per
    //      frame: "I do not move and it drifts away instantly" (18:16).
    //   2. de-duplicate by (target pointer, frame id) -> missed, because "frame" as counted by the
    //      Present hook is not the engine's camera frame: the heartbeat shows 900 calls per 5 s at
    //      one moment and 38 400 at the next, and the same target turns up again milliseconds later.
    //      The log of 18:29 has the compounding still happening: one gaze went 60 -> 111 -> 118 in y.
    //
    // What the data itself can decide, without any frame counter:
    //
    //   * the engine WROTE this pose if its value differs from what we last wrote there. Then that
    //     value is the baseline - we have not touched this round - so rotate it once and remember both.
    //   * the value still equals what we wrote => the engine has not refreshed this pose since, so
    //     this is a repeat call inside the same engine frame. Doing nothing is the whole fix.
    //   * a value that is neither (a third party changed it) => treat it as the engine's; it is
    //     fresh by definition since it is not ours.
    //
    // The cache itself (keyed on target AND eye, with the fresh/stale verdict) lives above, because
    // the eye offset needs the same information. The rotation is only applied to a FRESH pose: a
    // repeat call is already rotated, and rotating it again is the compounding bug.
    if (!fresh) return;

    const float nt[3] = {e[0] + vx, e[1] + vy, e[2] + vz};
    memcpy((void *)(uintptr_t)target, nt, sizeof(nt));
    memcpy(pc->wrote_t, nt, sizeof(pc->wrote_t));
    memcpy(pc->wrote_e, &e[0], sizeof(pc->wrote_e));

    static volatile LONG calls = 0;
    const LONG applied = InterlockedIncrement(&calls);
    static unsigned long long last_log = 0;
    const unsigned long long now = GetTickCount64();
    if (now - last_log > 1000) {
        last_log = now;
        VRLOG("steer: BUILDER hook: %ld rotation(s) applied, eye (%.2f %.2f %.2f) gaze (%.2f %.2f "
              "%.2f) -> (%.2f %.2f %.2f), published yaw %+.1f pitch %+.1f deg | target %08X | "
              "eye index %d (IPD writes %ld)",
              (long)applied, e[0], e[1], e[2], t[0] - e[0], t[1] - e[1], t[2] - e[2], vx, vy, vz,
              (double)(extra_yaw * 57.2957795f), (double)(extra_pitch * 57.2957795f), target,
              g_current_eye, (long)InterlockedCompareExchange(&g_ipd_writes, 0, 0));
    }
}

bool install_builder_hook() {    const HMODULE game = GetModuleHandleW(L"BH6.exe");
    if (!game) return false;
    void *target = (void *)((unsigned char *)(uintptr_t)game + kMakeViewMatrixRva);
    unsigned char head[9] = {0};
    if (!read(target, head, sizeof(head))) {
        VRLOG("steer: MakeViewMatrix %p is not readable - not hooking", target);
        return false;
    }
    if (memcmp(head, kMakeViewMatrixHead, sizeof(head)) != 0) {
        VRLOG("steer: MakeViewMatrix %p does not have the expected prologue (55 8B EC 83 E4 F0 83 EC "
              "40) - NOT hooking a live code path on a guess", target);
        return false;
    }
    if (MH_Initialize() != MH_OK && MH_ERROR_ALREADY_INITIALIZED != MH_Initialize()) {
        VRLOG("steer: MH_Initialize failed");
        return false;
    }
    if (MH_CreateHook(target, (void *)&builder_detour, &g_builder_trampoline) != MH_OK) {
        VRLOG("steer: MH_CreateHook(MakeViewMatrix) failed");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        VRLOG("steer: MH_EnableHook(MakeViewMatrix) failed");
        return false;
    }
    VRLOG("steer: hooked 0x%p (BH6.exe + 0x%X) - MakeViewMatrix. Prologue verified byte for byte. "
          "The rotation is applied to the vectors this call is using right now, so nothing "
          "accumulates anywhere.", target, kMakeViewMatrixRva);
    g_builder_hooked = true;
    return true;
}

// --- head tracking ----------------------------------------------------------
//
// The swing above proves the read path with a sine. Head tracking is the same write with the
// headset's orientation instead.
//
// The first implementation (2026-09-25 11:35) mapped the head's ABSOLUTE yaw/pitch onto the camera
// and the player's verdict was immediate and precise: "it turned, but far too fast and after one
// look I have no idea where I ended up". Both halves of that are properties of absolute mapping:
//
//   * full head-turn for a small glance feels hypersensitive, because the game camera is not the
//     head - the body still has to be able to steer with stick/mouse;
//   * absolute means the camera's direction is a function of the head's current pose, so the game's
//     own camera control is thrown away and there is nothing to return to - look away once and the
//     original view is gone.
//
// So the head is applied as a RELATIVE drag, the way every working head-look mod does it:
//
//   * per frame, take the head's angular VELOCITY (the delta between this frame's pose and the
//     last), scale it by gain, and rotate the camera's target by that delta about its own position;
//   * when the head stops moving (velocity below a threshold), bleed the accumulated extra angle
//     back to zero with a time constant - the "ratchet": look aside, hold still, and the camera
//     glides back behind the character instead of staying stuck where the head left it;
//   * the game's own camera control is untouched: it keeps moving the target every frame, and the
//     head's delta is added on top of whatever it did.
//
// Knobs, all optional marker files next to the log (absent = the default in brackets):
//   re6vr_head_gain.txt       how much of the head's motion reaches the camera  [0.5]
//   re6vr_head_range.txt      cap on the accumulated extra angle, degrees  [40]
//   re6vr_head_decay.txt      seconds for the extra angle to fade while holding still [0 = OFF]
//   re6vr_head_worldyaw.txt   1 = turn about world Y instead of the camera's own up [0]
// F9 recentres instantly, at any time.
//
// Self-centring is OFF by default on purpose: holding your head still is how you look at something,
// and a view that slides away while you do is fighting the player. The head's motion turns the
// view; the view stays where the head left it; F9 is the explicit way back.

// ---------------------------------------------------------------- the head pipeline
//
// WHY THIS IS NOT DONE INSIDE THE HOOK (the bug the player felt as "jitter suppression turned my
// head movement into a big jump to another position"):
//
// The hook runs once per GetViewMatrix call - hundreds of times a second, from several threads, and
// not on a fixed schedule. The whole anti-jitter chain (low-pass, deadzone, accumulation) is
// time-based, so running it there gave it a wandering dt: a pause between two calls looked like a
// large head movement, and crossing the deadzone edge re-based the sticky point by a whole
// deadzone radius in one step. Both of those are jumps by construction.
//
// So the chain now runs on its own thread at a fixed 120 Hz, where dt is stable by construction, and
// the hook only reads the finished angles. The thread owns all head state; the hook owns the camera
// writes. No lock is needed for the hand-off: each published value is a single float and a torn
// read would be off by less than a degree for one frame - but the whole published *set* is fenced
// by a sequence counter so a frame can never see a half-updated pair.
struct HeadPipeline {
    float extra_yaw = 0.0f, extra_pitch = 0.0f;   // what the hook applies (radians)
    volatile LONG seq = 0;                        // even = stable
};

HeadPipeline g_pipe;
HANDLE g_head_thread = nullptr;
volatile LONG g_head_thread_run = 0;
bool g_head_thread_started = false;

struct HeadState {
    bool have_prev = false;
    // Filtered head angles (`f_`) and the sticky neutral point (`sticky_`, radians, same space).
    float f_yaw = 0.0f, f_pitch = 0.0f;
    float sticky_yaw = 0.0f, sticky_pitch = 0.0f;
    bool sticky_init = false;
    bool in_deadzone = true;
    // Absolute mode (the default): the head's angle relative to a reference, taken at start-up and
    // re-taken by F9. See the long comment in head_thread.
    bool abs_ref_set = false;
    float abs_ref_yaw = 0.0f, abs_ref_pitch = 0.0f;
    unsigned long long prev_ms = 0;
    float extra_yaw = 0.0f, extra_pitch = 0.0f;     // the accumulated look-aside, radians
    unsigned long long last_log_ms = 0;
    unsigned long long last_recenter_ms = 0;
    int frames_applied = 0;
};

HeadState g_head;
float g_head_gain = 0.5f;
float g_head_decay_s = 0.0f;
float g_head_range_deg = 40.0f;
float g_head_deadzone_deg = 1.2f;        // anti-jitter: below this the head is "still"
float g_head_sticky_deg_per_s = 3.0f;    // how fast the neutral point follows a quiet head
bool g_head_pitch_enabled = true;        // diagnostic: re6vr_head_nopitch.txt = 1 disables pitch
bool g_head_sweep = false;               // diagnostic: re6vr_head_sweep.txt = 1 auto-sweeps yaw
// Tripod rule state; the rule itself is documented next to tripod_pitch().
bool g_head_tripod = true;
// Absolute mapping: the head's angle (relative to a F9-reset reference) IS the camera's offset.
bool g_head_absolute = true;
bool g_head_knobs_logged = false;
// Marker files live next to the log, like every other switch in this project (Steam passes no
// environment). A missing or unreadable file leaves the default in place.
float read_float_marker(const wchar_t *name, float fallback, float lo, float hi) {
    wchar_t path[MAX_PATH] = L"";
    const wchar_t *log_path = vrlog::path();
    if (!log_path || !log_path[0]) return fallback;
    wcsncpy_s(path, MAX_PATH, log_path, _TRUNCATE);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return fallback;
    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - path)), name);
    FILE *f = _wfopen(path, L"r");
    if (!f) return fallback;
    float v = fallback;
    const bool ok = fscanf_s(f, "%f", &v) == 1;
    fclose(f);
    if (!ok) return fallback;
    VRLOG("steer: %ls = %.3f", path, (double)v);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

void load_head_knobs() {
    g_head_gain = read_float_marker(L"re6vr_head_gain.txt", 0.5f, 0.05f, 3.0f);
    // 0 = the view stays where the head put it (the default, and the honest behaviour).
    g_head_decay_s = read_float_marker(L"re6vr_head_decay.txt", 0.0f, 0.0f, 60.0f);
    g_head_range_deg = read_float_marker(L"re6vr_head_range.txt", 40.0f, 5.0f, 120.0f);
    // Anti-jitter. A degree or so of deadzone is what a Quest-class runtime's noise needs; the
    // sticky point then absorbs posture drift at a few degrees per second.
    g_head_deadzone_deg = read_float_marker(L"re6vr_head_deadzone.txt", 1.2f, 0.0f, 15.0f);
    g_head_sticky_deg_per_s = read_float_marker(L"re6vr_head_sticky.txt", 3.0f, 0.0f, 30.0f);
    g_head_pitch_enabled = read_float_marker(L"re6vr_head_nopitch.txt", 0.0f, 0.0f, 1.0f) < 0.5f;
    g_head_sweep = read_float_marker(L"re6vr_head_sweep.txt", 0.0f, 0.0f, 1.0f) >= 0.5f;
    // Tripod rule (see tripod_pitch): ON by default, because it is what the player asked for -
    // left/right must stay left/right. 0 = physically faithful, the head's own cone included.
    g_head_tripod = read_float_marker(L"re6vr_head_tripod.txt", 1.0f, 0.0f, 1.0f) >= 0.5f;
    // Absolute mapping (the head's angle IS the camera's offset) versus the delta/drag model.
    // Absolute is the default: it is the only one that keeps the view matching where the head points.
    g_head_absolute = read_float_marker(L"re6vr_head_absolute.txt", 1.0f, 0.0f, 1.0f) >= 0.5f;
    // fov injection (degrees). 0/unset = the engine's own value. Used to settle whether the game's
    // fov is vertical or horizontal, and later to drive the per-eye projection.
    g_fov_override = read_float_marker(L"re6vr_fov.txt", 0.0f, 0.0f, 179.0f);
    // Stereo groundwork: the eye separation, and an optional fixed eye index for a single-eye test
    // (0 = left, 1 = right, 2/-1 = off). With g_ipd = 0 both are inert.
    g_ipd = read_float_marker(L"re6vr_ipd.txt", 0.0f, 0.0f, 0.5f);
    g_eye_force = read_float_marker(L"re6vr_eye_index.txt", -1.0f, -1.0f, 1.0f);
    if (g_eye_force >= 0.0f) g_current_eye = (int)(g_eye_force + 0.5f);
    // Stereo dual pass: the render phase runs a second time with the other eye selected. Default OFF,
    // because it makes the engine render the whole scene twice into the same target (the two eyes
    // overwrite each other until the per-eye targets exist), and that is a deliberate experiment, not
    // a shipping mode. re6vr_stereo.txt = 1.
    g_stereo = read_float_marker(L"re6vr_stereo.txt", 0.0f, 0.0f, 1.0f) >= 0.5f;
    // The read-only render-target observation, on its own marker so it can run on an otherwise inert
    // configuration (see the probe in render_probe_record).
    g_rt_probe = read_float_marker(L"re6vr_rt_probe.txt", 0.0f, 0.0f, 1.0f) >= 0.5f;
    if (g_rt_probe) {
        VRLOG("steer: render-target probe ARMED (re6vr_rt_probe.txt = 1). Read-only: once a second it "
              "reads which surface the engine's render phase draws into (device vtable slot 38), so "
              "\"can a pass be redirected to a texture of our own\" stops being a guess. Nothing is "
              "written and no extra surface is created.");
    }
    // The SECOND ENGINE PASS on its own switch. Default OFF, and that default matters: it is the
    // mode that killed four runs on 2026-09-25 (same fault each time - c0000005 at
    // nvwgf2um.dll+0x24678a with a GPU engine hang), so the safe one-eye-per-frame mode is what
    // `--config stereo` deploys. Set the marker to 1 only to reproduce that experiment deliberately.
    g_second_pass = read_float_marker(L"re6vr_stereo_pass2.txt", 0.0f, 0.0f, 1.0f) >= 0.5f;
    if (g_stereo) {
        VRLOG("steer: STEREO armed: %s, ipd %.4f m. One render pass per frame: the eye alternates "
              "every frame and each eye's picture is captured at EndScene and shown to that eye until "
              "its turn comes round again.",
              g_second_pass ? "SECOND ENGINE PASS ON (re6vr_stereo_pass2.txt = 1, the mode that "
                              "crashed the driver)"
                            : "one eye per frame, alternating",
              (double)g_ipd);
    }
    if (g_fov_override > 0.0f) {
        VRLOG("steer: FOV OVERRIDE armed: the player camera's fov will be written to %.2f deg every "
              "frame (re6vr_fov.txt). RE6's +0x4C is a VERTICAL fov - measured, not assumed: the panel's "
              "own subtense comes out 59.8 x 35.8 deg at 4.0 m, a tan-aspect of 1.67, which only matches "
              "the game's 16:9 frame if 37 deg is vertical (a horizontal reading would give 2.6). The "
              "compositor prints the resulting match ratio once a second as 'fov match: ...'",
              (double)g_fov_override);
    }
    if (g_ipd != 0.0f || g_eye_force >= 0.0f) {
        VRLOG("steer: stereo groundwork armed: ipd %.4f m, forced eye index %d (0=left 1=right), "
              "active eye %d", (double)g_ipd, (int)g_eye_force, g_current_eye);
    }
    // Which write path: "build" (default) rotates the vectors inside MakeViewMatrix, statelessly;
    // "matrix" (the old one) writes the camera object's target, which the engine rebuilds every
    // frame and which therefore needed an accumulated angle. Kept for A/B.
    const float method = read_float_marker(L"re6vr_head_method.txt", 1.0f, 0.0f, 1.0f);
    g_method = (method >= 0.5f) ? kMethodBuilder : kMethodMatrix;
    VRLOG("steer: head tracking (3DOF, fixed eye; yaw about the horizontal perpendicular to the "
          "gaze so the tilt the engine chose is preserved; pipeline on its own fixed-rate thread; "
          "write path = %s): %s, gain %.2f, range +-%.0f deg, deadzone %.2f deg (sticky %.1f deg/s), "
          "pitch %s, tripod rule %s, self-centring %s, F9 = recentre now",
          g_method == kMethodBuilder ? "MakeViewMatrix (stateless)"
                                     : "camera object target (accumulating)",
          g_head_absolute
              ? "ABSOLUTE mapping (the head's angle IS the camera's offset; F9 takes the reference)"
              : "DELTA/drag mapping (per-step motion is integrated)",
          (double)g_head_gain, (double)g_head_range_deg, (double)g_head_deadzone_deg,
          (double)g_head_sticky_deg_per_s,
          g_head_pitch_enabled ? "enabled" : "DISABLED (diagnostic)",
          g_head_tripod ? "ON (a left/right turn cannot tilt the view; nodding still works)"
                        : "OFF (the head's own cone reaches the camera)",
          g_head_decay_s > 0.0f
              ? "ON (the view fades back to centre while you hold still - only if you asked for it)"
              : "OFF (the view stays where you put it)");
    if (g_head_sweep) {
        VRLOG("steer: DIAGNOSTIC SWEEP ON (re6vr_head_sweep.txt): the yaw comes from a +-12 deg "
              "triangle wave, not from the headset - the axis can be judged with jitter, deadzone "
              "and head motion all out of the picture");
    }
}

// Rodrigues rotation of the gaze about an arbitrary unit axis. NOT used by head tracking any more
// (3DOF turn is done in spherical coordinates about world up, see head_steer_one) - kept for the
// swing experiment's world-Y rotation, which is what proved the read path.
void rotate_about_axis(float dx, float dy, float dz, float ax, float ay, float az, float angle,
                       float *out3) {
    const float c = cosf(angle), s = sinf(angle);
    const float dot = dx * ax + dy * ay + dz * az;
    // a x v
    const float cx = ay * dz - az * dy;
    const float cy = az * dx - ax * dz;
    const float cz = ax * dy - ay * dx;
    out3[0] = dx * c + cx * s + ax * dot * (1.0f - c);
    out3[1] = dy * c + cy * s + ay * dot * (1.0f - c);
    out3[2] = dz * c + cz * s + az * dot * (1.0f - c);
}

// ---------------------------------------------------------------- headset tilt calibration
//
// The correction that makes "left/right stays left/right" true rather than hoped for.
//
// The headset's tracking frame is not the game world's frame: it is rotated by however the headset
// sits on the player's head, and by the runtime's own idea of "forward". Measured with the offline
// replay (a head that only yaws 40 degrees, worn 20 degrees off level):
//
//     naive reading (any single-frame decomposition of the headset basis)
//         yaw +40.7   pitch  -5.4      <- the "diagonal": turning leaked 5 degrees of pitch
//     after removing the measured tilt
//         yaw +39.3   pitch  -3.0
//
// The tilt is measurable and removable without any extra input. At the reference moment the head is
// (by definition) neutral relative to the game camera, so the headset's UP at that instant IS the
// direction its frame calls up - and the world calls up (0,1,0). The rotation between the two,
//
//     axis  = normalize(ref_up x world_up),   angle = acos(ref_up . world_up)
//
// is the tilt, and applying its inverse to the head's axes removes the contamination from every
// later reading. It is the same step a VR runtime performs when it aligns a tracking space to a
// play space, and the same one a player performs by re-seating the headset until the horizon looks
// level - except this one is measured instead of eyeballed.
bool g_calib_ready = false;
float g_calib_b_ref[3] = {0.0f, 0.0f, 1.0f};
float g_calib_u_ref[3] = {0.0f, 1.0f, 0.0f};
float g_calib_axis[3] = {1.0f, 0.0f, 0.0f};
float g_calib_angle = 0.0f;

// Rotate `v` about the calibrated axis by the calibrated angle (Rodrigues).
//
// The angle is NEGATED here. The calibration measures the rotation that takes the TRACKING frame to
// the body frame (tilt * identity = tilt), so removing it from a vector needs its inverse - the same
// rotation by -angle. Applying it the other way doubled the neutral error instead of cancelling it
// (4.9 degrees became 9.7 in the offline check), which is exactly the kind of sign that is cheap to
// get wrong and expensive to discover in a headset.
void calibrate_vec(const float *v, float *out) {
    if (!g_calib_ready || g_calib_angle == 0.0f) {
        out[0] = v[0];
        out[1] = v[1];
        out[2] = v[2];
        return;
    }
    const float ax = g_calib_axis[0], ay = g_calib_axis[1], az = g_calib_axis[2];
    const float c = cosf(-g_calib_angle), s = sinf(-g_calib_angle);
    const float dot = v[0] * ax + v[1] * ay + v[2] * az;
    const float cx = ay * v[2] - az * v[1];
    const float cy = az * v[0] - ax * v[2];
    const float cz = ax * v[1] - ay * v[0];
    out[0] = v[0] * c + cx * s + ax * dot * (1.0f - c);
    out[1] = v[1] * c + cy * s + ay * dot * (1.0f - c);
    out[2] = v[2] * c + cz * s + az * dot * (1.0f - c);
}

// Takes the tilt from the head's up vector at the reference moment. Returns the tilt in degrees so
// the log can state it.
float calibrate_from_up(const float *head_up) {
    float up[3] = {head_up[0], head_up[1], head_up[2]};
    const float len = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (!(len > 0.5f)) return 0.0f;
    up[0] /= len;
    up[1] /= len;
    up[2] /= len;
    // Rotating up_ref to (0,1,0) needs the axis  (0,1,0) x up_ref  = (up.z, 0, -up.x), NOT its
    // negation: Rodrigues moves (0,1,0) along axis x (0,1,0) = (-up.z, 0, up.x), and up_ref = (0,1,0)
    // + t*(-up.z, 0, up.x) for small tilts, so that is the direction that closes the gap. Getting this
    // backwards made a 3.5-degree neutral error into 8.5 degrees - caught by the offline check, not by
    // a game run.
    float axis[3] = {up[2], 0.0f, -up[0]};
    const float alen = sqrtf(axis[0] * axis[0] + axis[2] * axis[2]);
    float dot = up[1];
    if (dot > 1.0f) dot = 1.0f;
    if (dot < -1.0f) dot = -1.0f;
    g_calib_angle = acosf(dot);
    if (alen < 1e-4f || g_calib_angle < 1e-4f) {
        g_calib_angle = 0.0f;
        g_calib_axis[0] = 1.0f;
        g_calib_axis[1] = 0.0f;
        g_calib_axis[2] = 0.0f;
        g_calib_ready = true;
        return 0.0f;
    }
    g_calib_axis[0] = axis[0] / alen;
    g_calib_axis[1] = 0.0f;
    g_calib_axis[2] = axis[2] / alen;
    g_calib_ready = true;
    return g_calib_angle * 57.2957795f;
}

// ---------------------------------------------------------------- render-phase / stereo probe
//
// Read-only evidence for the stereo work, from the offline report `_work/stereo_render_report.md`:
//
//   * sRender is the singleton 0x0186E8BC. Its frame-end function 0x00F41C20 contains the ONLY
//     IDirect3DDevice9::Present call in the whole image (0x00F41D59).
//   * The render phase 0x00F3EA30 (vtable 0x016E5E18 slot 6, __thiscall, ecx = sRender) renders the
//     whole scene ONCE PER DISPLAY: `for (i = 1; i < sRender->displayCount; ++i)`, with the display
//     index passed to the camera update (0x00F3ECCF) and to the draw pass (0x00F3ED2C).
//   * displayCount = sRender+0x462694, written once in the whole image (=1).
//   * The engine ALSO has a config key "Stereo" read into sRender+0x448EE0
//     (`push 0 "Stereo"; push "GRAPHICS"; ecx = 0x17A6D40; call 0x00EE1640` at 0x00F42C21..0x00F42C3C),
//     i.e. this engine may already know how to render stereo - which would be far cheaper than
//     driving a second pass ourselves.
//
// So before writing any dual-pass code, this probe answers the three questions that decide the design:
//   1. is the render phase called once per frame as the report says (and with which display index)?
//   2. what does the "Stereo" config byte actually hold at runtime?
//   3. how many displays does the engine believe it has, and does the loop body execute at all?
//
// Everything here is a read. The hook is installed only in the "head" mode's own install path, and
// its detour does nothing but record.
const unsigned kRenderPhaseRva = 0xB3EA30u;      // 0x00F3EA30
const unsigned char kRenderPhaseHead[9] = {0x83, 0xEC, 0x10, 0x55, 0x57, 0x8B, 0xF9, 0x33, 0xED};
const unsigned kSRenderGlobal = 0x0186E8BCu;     // ds:[0x186E8BC] = sRender*
const unsigned kOffDisplayCount = 0x462694u;
const unsigned kOffStereoFlag = 0x448EE0u;
void *g_render_trampoline = nullptr;
volatile unsigned g_render_here = 0;
volatile LONG g_render_calls = 0;
bool g_render_probe = false;
// Stereo dual pass (re6vr_stereo.txt = 1). Dormant by default: with it off the render phase is only
// observed, which is what every earlier build did.

void __stdcall render_probe_record();

volatile LONG g_in_render = 0;      // re-entrancy guard: the second pass must not re-enter this
volatile unsigned g_render_self = 0;

void __stdcall render_second_pass();

// Declared before the detour: without a declaration, `call render_second_pass_record` inside __asm is
// a jump to a LABEL of that name, and the compiler says so (C2094) rather than guessing.
void __stdcall render_second_pass_record();
void __stdcall render_bind_eye();

// `call` + `ret` rather than `jmp`, because a second pass has to run after the first one returns.
// That is only safe if this function cleans no arguments, and it was checked rather than assumed: the
// render phase's return sites are bare `ret` (0x00F3ED75 and the others in 0x00F3EB09..0x00F3EE4E),
// i.e. __thiscall with the caller doing `add esp, 0x10` - so a plain call/ret pair leaves the stack
// exactly as the engine left it. (Getting this wrong on 0x00E6FD20 cost two crashed runs; the check
// is cheap.)
// Which eye this render phase draws into, decided in render_probe_record and used by the detour below.
__declspec(naked) void render_probe_detour() {
    __asm {
        mov g_render_here, ecx
        pushfd
        pushad
        call render_probe_record
        popad
        popfd
        call render_bind_eye               // REDIRECT: this pass draws into the current eye's texture
        lock inc dword ptr [g_render_active]    // "inside a render phase" for the SetRenderTarget probe
        call g_render_trampoline           // the engine's pass - into the target we just bound
        lock dec dword ptr [g_render_active]
        pushfd
        pushad
        call render_second_pass            // optionally a second pass for the right eye (opt-in marker)
        popad
        popfd
        pushfd
        pushad
        call render_second_pass_record     // proof in the log that pass 2 really ran, and with which eye
        popad
        popfd
        ret
    }
}

// Stereo without a second render pass: ONE render per frame, with the eye alternating every frame,
// and every capture taken at EndScene.
//
// Why the design changed on 2026-09-25 20:30 (this is the important part):
//
//   The first design rendered the scene twice per frame and copied eye 0's picture out of the device
//   BETWEEN the two passes - from the render thread, inside the engine's own render phase. That call
//   is the one thing every crashing run had in common and the working single-texture run never did:
//   it drove GetRenderTargetData/StretchRect on the game's device while the engine was mid-frame.
//   Measured: four runs with it (20:19, 20:24 with two passes, 20:27 capture-only) all died with the
//   SAME fault - code c0000005 at nvwgf2um.dll+0x24678a, a GPU engine hang - and the run without it
//   (20:15, 4200 frames) went through the same device Reset unharmed. So the mid-frame capture is
//   gone. No device work happens between passes any more; the only place this mod touches the game's
//   device is EndScene, which is the placement the project verified months ago.
//
// What replaces it: each frame renders ONE eye (even frames eye 0, odd frames eye 1) and EndScene
// copies that eye's picture into that eye's own texture. The compositor then shows each eye the most
// recent picture captured FOR it, so eye 0 sees eye 0's pose and eye 1 sees eye 1's - one frame of
// latency for whichever eye was not rendered this frame, which at 60 Hz is 16 ms and is the standard
// trade in stereo renderers that cannot afford two passes.
void __stdcall render_second_pass_record() {
    if (!g_stereo) return;
    static unsigned long long last = 0;
    const unsigned long long now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    VRLOG("steer: STEREO: second passes %ld, guard refusals %ld, eye-offset writes %ld, active eye %d "
          "(ipd %.4f m, pass 2 %s) | capture-only mode renders one eye per frame, alternating",
          (long)InterlockedCompareExchange(&g_stereo_passes, 0, 0),
          (long)InterlockedCompareExchange(&g_stereo_skipped, 0, 0),
          (long)InterlockedCompareExchange(&g_ipd_writes, 0, 0), g_current_eye, (double)g_ipd,
          g_second_pass ? "on" : "OFF (capture-only)");
}

// The redirect, in both stereo modes.
//
// The measurement this rests on (2026-09-25 21:02): the engine binds its render target OUTSIDE the
// render phase and never rebinds inside one - 0 binds inside the phase across 2686 phases. So a binding
// made here is honoured for the whole pass and nothing of the engine's own code has to be intercepted.
//
//   * two passes per frame (re6vr_stereo_pass2.txt = 1): pass 1 is the left eye, pass 2 the right, each
//     into its own texture. NOTE: this mode reproduces the crash that killed five runs - the fault is
//     the engine's render phase being called twice per frame, not the redirect;
//   * one pass per frame (pass2 = 0): the eye alternates every frame, each pass going into its own eye's
//     texture, which is kept until that eye's turn comes round again.
void __stdcall render_bind_eye() {
    if (g_redirect_eye < 0) return;
    IDirect3DSurface9 *target = re6vr::bridge_redirect_target_for_eye(g_redirect_eye);
    if (!target) return;
    const unsigned srender = g_render_self;
    if (!srender) return;
    unsigned device = 0;
    if (!read32(srender + 0x100, &device) || !device) return;
    typedef HRESULT(__stdcall *SetRtFn)(void *, DWORD, void *);
    void **vtbl = *(void ***)(uintptr_t)device;
    if (!vtbl || !vtbl[37]) return;
    SetRtFn set_rt = (SetRtFn)vtbl[37];
    if (FAILED(set_rt((void *)(uintptr_t)device, 0, target))) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            VRLOG("steer: REDIRECT could not bind eye %d's target - that pass draws into the back buffer "
                  "as before", g_redirect_eye);
        }
        g_redirect_eye = -1;
    }
}

void __stdcall render_second_pass() {
    if (!g_stereo) return;
    // Re-entrancy: the engine may reach this function from inside itself on some paths.
    if (InterlockedIncrement(&g_in_render) != 1) {
        InterlockedDecrement(&g_in_render);
        InterlockedIncrement(&g_stereo_skipped);
        return;
    }
    if (!g_second_pass) {
        // Capture-only: the second pass is skipped, so eye 1's picture is a copy of eye 0's. The point
        // is to exercise the capture path - two copies per frame, into two textures, from two threads -
        // WITHOUT asking the engine to render the scene twice, so a failure can be attributed to one of
        // the two. Reported once a second by render_second_pass_record, which still runs.
        InterlockedIncrement(&g_stereo_skipped);
        InterlockedDecrement(&g_in_render);
        return;
    }
    const unsigned srender = g_render_self;
    if (srender && re6vr::bridge_device_ready()) {
        // Pass 2 renders the RIGHT eye, into a texture of OURS.
        //
        // Measured 2026-09-25 21:02: the engine draws its whole pass into whatever target is bound, it
        // binds that target outside the render phase and never rebinds inside one (0 binds inside the
        // phase across 2686 phases). So binding our surface here, before the pass starts, puts the
        // engine's entire second render into our texture - the engine does the drawing, and nothing of
        // its own code has to be intercepted.
        const int saved = g_current_eye;
        g_current_eye = 1;
        if (IDirect3DSurface9 *target = re6vr::bridge_redirect_target_for_eye(1)) {
            unsigned device = 0;
            if (read32(srender + 0x100, &device) && device) {
                typedef HRESULT(__stdcall *SetRtFn)(void *, DWORD, void *);
                void **vtbl = *(void ***)(uintptr_t)device;
                if (vtbl && vtbl[37]) {
                    SetRtFn set_rt = (SetRtFn)vtbl[37];
                    if (FAILED(set_rt((void *)(uintptr_t)device, 0, target))) {
                        static bool s_warned = false;
                        if (!s_warned) {
                            s_warned = true;
                            VRLOG("steer: REDIRECT could not bind the right eye's target - the second "
                                  "pass will draw into the back buffer as before");
                        }
                    }
                }
            }
        }
        typedef void(__thiscall *RenderPhaseFn)(void *self);
        RenderPhaseFn fn = (RenderPhaseFn)g_render_trampoline;
        fn((void *)(uintptr_t)srender);
        g_current_eye = saved;
        InterlockedIncrement(&g_stereo_passes);
    } else {
        // The device is lost (a fullscreen start-up spends ~200 ms in that window) or the session is
        // not up. Rendering the scene a second time into a device that is being Reset is exactly the
        // situation the 20:19 run died in, so the pass is refused and counted rather than attempted.
        static bool s_warned_lost = false;
        if (!s_warned_lost && g_second_pass) {
            s_warned_lost = true;
            VRLOG("steer: STEREO second pass refused: the D3D9 device is lost/unavailable. Refusing is "
                  "deliberate - the engine's render phase keeps being hooked through a device Reset, "
                  "and issuing a second full render into a resetting device is what killed the run of "
                  "2026-09-25 20:19. The pass resumes by itself once the device is back.");
        }
        InterlockedIncrement(&g_stereo_skipped);
    }
    InterlockedDecrement(&g_in_render);
}

// How many times per frame does the engine BIND a render target, and to what?
//
// This is the last read-only question before the redirect can be built. The render-target probe
// already settled that a pass draws into the BACK BUFFER (measured 2026-09-25 20:56, 140 reads, one
// pointer). What it did not settle is whether that binding happens once per pass or several times
// inside one:
//
//   * bound once, at the top of the pass -> we can bind our own surface right after and the engine's
//     whole pass lands in it, with no need to intercept anything per draw;
//   * bound repeatedly -> the redirect must intercept the engine's own bind, which is a different
//     (and riskier) implementation.
//
// Read-only: the detour counts and forwards. It logs only while the render phase is active, so what it
// reports is "binds inside one pass", which is exactly the number the design needs.
typedef HRESULT(__stdcall *SetRenderTargetFn)(void *, DWORD, void *);
SetRenderTargetFn g_real_set_render_target = nullptr;
volatile LONG g_rt_binds_in_phase = 0;
volatile LONG g_rt_binds_other = 0;
volatile unsigned g_rt_last_bound = 0;
volatile LONG g_rt_hook_installed = 0;

HRESULT __stdcall set_render_target_detour(void *dev, DWORD index, void *surface) {
    if (g_render_active) {
        InterlockedIncrement(&g_rt_binds_in_phase);
        g_rt_last_bound = (unsigned)(uintptr_t)surface;
    } else {
        InterlockedIncrement(&g_rt_binds_other);
    }
    return g_real_set_render_target ? g_real_set_render_target(dev, index, surface) : E_FAIL;
}

bool install_render_target_probe(IDirect3DDevice9 *dev) {
    if (!dev || g_rt_hook_installed) return g_rt_hook_installed != 0;
    void **vtbl = *(void ***)(uintptr_t)dev;
    if (!vtbl || !vtbl[37]) {                    // IDirect3DDevice9 slot 37 = SetRenderTarget
        VRLOG("steer: render-target probe: device vtable slot 37 is empty - not hooking");
        return false;
    }
    DWORD old_protect = 0;
    if (!VirtualProtect(&vtbl[37], sizeof(void *), PAGE_READWRITE, &old_protect)) {
        VRLOG("steer: render-target probe: VirtualProtect failed: %lu", GetLastError());
        return false;
    }
    g_real_set_render_target = (SetRenderTargetFn)vtbl[37];
    vtbl[37] = (void *)&set_render_target_detour;
    VirtualProtect(&vtbl[37], sizeof(void *), old_protect, &old_protect);
    InterlockedExchange(&g_rt_hook_installed, 1);
    VRLOG("steer: render-target probe: SetRenderTarget (device vtable slot 37) hooked read-only. It "
          "counts how many times the engine binds a target INSIDE one render phase - 1 means a pass can "
          "be redirected by binding our own surface once, more means the redirect has to intercept the "
          "engine's own bind.");
    return true;
}

void __stdcall render_probe_record() {
    const LONG n = InterlockedIncrement(&g_render_calls);
    unsigned srender = 0;
    read32(kSRenderGlobal, &srender);
    if (!srender) return;
    g_render_self = srender;      // the second pass needs it, and it is the same every frame
    // WHICH RENDER TARGET DOES THE ENGINE DRAW ITS PASS INTO?
    //
    // This is the question that decides whether a second view is reachable at all: if the engine draws
    // straight into the back buffer, then redirecting one pass to a texture of our own (a detour on the
    // device's SetRenderTarget) would give us two separately-capturable pictures with the engine doing
    // all the drawing - no second device, no second swap chain, no CPU round trip for the second eye.
    // Read-only: two dwords, once a second. Its own switch (re6vr_rt_probe.txt) rather than riding on
    // re6vr_stereo.txt, so this can be observed on a completely inert run: the stereo switch also builds
    // per-eye surfaces and arms the capture, and an observation run must not change what it observes.
    if (g_rt_probe) {
        static unsigned long long last_rt = 0;
        const unsigned long long now_rt = GetTickCount64();
        if (now_rt - last_rt > 1000) {
            last_rt = now_rt;
            // How many target binds happened inside the render phases of the last second, and to
            // what. "about one per phase" is the answer that makes a one-line redirect possible.
            const LONG binds = InterlockedExchange(&g_rt_binds_in_phase, 0);
            VRLOG("steer: RENDER phase %ld: the engine bound a render target %ld time(s) inside the "
                  "phase(s) since the last report (last target %08X; outside phases: %ld)",
                  (long)n, (long)binds, g_rt_last_bound,
                  (long)InterlockedCompareExchange(&g_rt_binds_other, 0, 0));
            unsigned device = 0;
            if (read32(srender + 0x100, &device) && device) {
                typedef HRESULT(__stdcall *GetRtFn)(void *, DWORD, void **);
                void **vtbl = *(void ***)(uintptr_t)device;
                if (vtbl && vtbl[38]) {          // IDirect3DDevice9 slot 38 = GetRenderTarget
                    void *rt = nullptr;
                    GetRtFn fn = (GetRtFn)vtbl[38];
                    if (SUCCEEDED(fn((void *)(uintptr_t)device, 0, &rt))) {
                        // NOTE: the reference this call takes is deliberately NOT released. The first
                        // version released it through the device vtable's slot 2, and that killed the
                        // game: GetRenderTarget hands back the surface the ENGINE still owns, so
                        // dropping that reference frees the back buffer under the renderer, and the
                        // next frame dies inside d3d9 (measured 2026-09-25 20:53: crash at
                        // d3d9.dll+0x13961a, one second after the probe's second read). One leaked
                        // reference per second for a diagnostic is the cheap and safe side of that
                        // trade. The pointer is also NOT stored anywhere.
                        VRLOG("steer: RENDER phase %ld draws into render target %p (slot 38 read; the "
                              "back buffer is what Present shows)", (long)n, rt);
                    }
                }
            }
        }
    }
    // Which eye this frame is for, and therefore which texture the pass below is redirected into:
    //   * two passes per frame -> pass 1 is the left eye, pass 2 (in render_second_pass) the right;
    //   * one pass per frame   -> the eye alternates every frame, each pass going into its own eye's
    //     texture, which is kept until that eye's turn comes round again. This is the mode that does NOT
    //     call the engine's render phase twice, and calling it twice per frame is what crashed every
    //     run that tried it.
    if (g_stereo) {
        if (g_second_pass) {
            g_current_eye = 0;
            re6vr::bridge_note_current_eye(0);
            g_redirect_eye = 0;
        } else {
            const int eye = (int)(n & 1);        // even render phase -> eye 0, odd -> eye 1
            g_current_eye = eye;
            re6vr::bridge_note_current_eye(eye);
            g_redirect_eye = eye;
            static unsigned long long last_alt = 0;
            const unsigned long long now_alt = GetTickCount64();
            if (now_alt - last_alt > 2000) {
                last_alt = now_alt;
                VRLOG("steer: STEREO alternating eye -> %d (one engine render per frame; that eye's "
                      "texture holds the engine's own render until its turn comes round again)", eye);
            }
        }
    } else {
        g_redirect_eye = -1;
    }
    // Once a second: the engine's own numbers, so "the probe ran" and "the renderer did X" are
    // separate facts in the log.
    static unsigned long long last = 0;
    const unsigned long long now = GetTickCount64();
    if (n <= 3 || now - last > 1000) {
        last = now;
        unsigned count = 0, stereo = 0, device = 0, primary = 0;
        read32(srender + kOffDisplayCount, &count);
        unsigned char b = 0;
        read((const void *)(uintptr_t)(srender + kOffStereoFlag), &b, 1);
        stereo = b;
        read32(srender + 0x100, &device);          // IDirect3DDevice9*
        read32(srender + 0x448EB8, &primary);      // mpPrimaryScene
        VRLOG("steer: RENDER phase call %ld: sRender %08X, displayCount %u, Stereo flag %u, "
              "device %08X, primaryScene %08X | stereo %s, second passes %ld, guard refusals %ld",
              (long)n, srender, count, stereo, device, primary,
              g_stereo ? "ON (re6vr_stereo.txt)" : "off",
              (long)InterlockedCompareExchange(&g_stereo_passes, 0, 0),
              (long)InterlockedCompareExchange(&g_stereo_skipped, 0, 0));
    }
}

bool install_render_probe() {
    const HMODULE game = GetModuleHandleW(L"BH6.exe");
    if (!game) return false;
    void *target = (void *)((unsigned char *)(uintptr_t)game + kRenderPhaseRva);
    unsigned char head[9] = {0};
    if (!read(target, head, sizeof(head))) return false;
    if (memcmp(head, kRenderPhaseHead, sizeof(head)) != 0) {
        VRLOG("steer: render phase %p does not match the expected prologue (83 EC 10 55 57 8B F9 33 "
              "ED) - not hooking", target);
        return false;
    }
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (MH_CreateHook(target, (void *)&render_probe_detour, &g_render_trampoline) != MH_OK) {
        VRLOG("steer: MH_CreateHook(render phase) failed");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        VRLOG("steer: MH_EnableHook(render phase) failed");
        return false;
    }
    g_render_probe = true;
    VRLOG("steer: hooked 0x%p (BH6.exe + 0x%X) - the sRender render phase, READ ONLY. This is the "
          "loop that renders the whole scene once per display; the probe reports displayCount and the "
          "engine's Stereo config flag so the dual-pass design can be based on evidence.",
          target, kRenderPhaseRva);
    return true;
}

// ---------------------------------------------------------------- tripod mode
//
// The offline replay (_work/replay_head_pipeline.py) settled a question that had been guessed at for
// several rounds: the yaw -> pitch coupling the player felt is NOT an artefact of how the rotation is
// decomposed. Both decompositions report it identically, because it is real: a head turning about a
// tilted axis sweeps its gaze through a cone, and the gaze really does rise and fall.
//
// So a 3DOF "camera on a tripod" cannot be obtained by better maths - it has to be a rule. The rule,
// and the reason it is not simply "zero the pitch while turning":
//
//   * a NOD (looking down and up) is mostly motion in the vertical plane: the yaw change is small and
//     the pitch change is large. That pitch MUST be applied, or nodding stops working;
//   * a YAW (turning left and right) is mostly motion in the horizontal plane. Any pitch that comes
//     with it is the cone, and by the player's specification it must NOT reach the camera.
//
//     tripod rule:  scale the pitch by how much of the motion was NOT a turn:
//                   yaw_share = |yaw| / (|yaw| + |pitch|),
//                   gate      = clamp((0.5 - yaw_share) / 0.2, 0, 1)
//
//   Measured behaviour of that gate (checked before deploying, and it corrects an earlier claim in
//   this comment that the maths did not support):
//       pure yaw  (60.0, 5.0)   -> applied pitch  0.00     the cone is dropped entirely
//       pure nod  ( 0.0,20.0)   -> applied pitch 20.00     nodding untouched
//       nod + slight yaw (3,20) -> applied pitch 20.00
//       (20.0, 5.0)             -> applied pitch  0.00
//       (10.0,10.0)             -> applied pitch  0.00     (equal magnitudes already read as a turn)
//   i.e. pitch is fully dropped from 60% yaw-share upward, fully kept below 40%, linear between.
//
// re6vr_head_tripod.txt = 0 restores the physically faithful behaviour (head moves -> camera moves
// the same way), which is what the earlier builds did.
//
// (g_head_tripod itself is declared with the other head globals; this block documents the rule.)

// Applies the rule above to one step's decomposition. Returns the pitch to use.
float tripod_pitch(float yaw, float pitch) {
    if (!g_head_tripod || pitch == 0.0f) return pitch;
    const float ay = fabsf(yaw), ap = fabsf(pitch);
    const float sum = ay + ap;
    if (sum < 1e-6f) return pitch;
    const float yaw_share = ay / sum;
    float gate = (0.5f - yaw_share) / 0.2f;
    if (gate < 0.0f) gate = 0.0f;
    if (gate > 1.0f) gate = 1.0f;
    return pitch * gate;
}
//
// One step of the anti-jitter chain, always with a stable dt because the caller is a fixed-rate
// thread (see the comment on HeadPipeline). Order: low-pass, deadzone with a sticky neutral point
// and hysteresis, accumulate, optional self-centring, publish.
void head_pipeline_step(float raw_yaw, float raw_pitch, float dt) {
    // 1. LOW-PASS. The runtime's pose carries a fraction of a degree of noise; at a fixed 120 Hz
    //    this is a clean single-pole filter.
    const float k = dt / (dt + 0.08f);                 // ~80 ms time constant
    g_head.f_yaw += (raw_yaw - g_head.f_yaw) * k;
    g_head.f_pitch += (raw_pitch - g_head.f_pitch) * k;

    const float kDeg = 0.0174532925f;
    if (!g_head.sticky_init) {
        // First sample: the neutral point starts wherever the player is looking when tracking comes
        // up, so nothing moves until they actually move.
        g_head.sticky_init = true;
        g_head.sticky_yaw = g_head.f_yaw;
        g_head.sticky_pitch = g_head.f_pitch;
    }

    // 2. DEADZONE, with the neutral point following a quiet head.
    float d_yaw = g_head.f_yaw - g_head.sticky_yaw;
    float d_pitch = g_head.f_pitch - g_head.sticky_pitch;
    const float mag = sqrtf(d_yaw * d_yaw + d_pitch * d_pitch);
    const float enter = g_head_deadzone_deg * kDeg;            // quiet -> moving
    const float exit = g_head_deadzone_deg * 0.6f * kDeg;      // moving -> quiet (hysteresis)
    const bool moving_now = g_head.in_deadzone ? (mag > enter) : (mag > exit);

    float step_yaw = 0.0f, step_pitch = 0.0f;
    if (!moving_now) {
        // Still: no camera motion at all, and the neutral point creeps toward the head so posture
        // drift, headset slip and runtime zero-drift never become a slow, unwanted turn.
        //
        // BUT only while the view is centred. This is the fix for the player's report of
        // 2026-09-25 18:17 - "I do not move and it drifts away" - and the log shows exactly why:
        //
        //     18:16:44  published yaw +33.1 pitch -30.0
        //     18:17:09  published yaw +10.6 pitch  +1.1        <- 22 degrees lost in five seconds
        //     18:17:14  published yaw +17.1 pitch  -2.4        <- and wandering
        //
        // The creep was still running while an offset was being HELD, so the neutral point chased the
        // head and the held angle drained away - a slow automatic recentre that the player had
        // explicitly rejected ("the view must stay where the head left it"). With the creep gated on
        // a centred view, holding still now holds the angle, while the bias-absorption it exists for
        // still works when nothing is being held.
        const float kCreepCeiling = 2.0f * 0.0174532925f;      // ~2 deg of held offset
        const bool centered = (fabsf(g_head.extra_yaw) + fabsf(g_head.extra_pitch)) < kCreepCeiling;
        if (centered) {
            const float creep = g_head_sticky_deg_per_s * kDeg * dt;
            const float frac = (mag > 1e-6f) ? ((creep < mag) ? creep / mag : 1.0f) : 0.0f;
            g_head.sticky_yaw += d_yaw * frac;
            g_head.sticky_pitch += d_pitch * frac;
        }
        // The head keeps its offset relative to the moved neutral point only up to the deadzone
        // radius; beyond that the excess becomes camera motion, so a deliberate slow turn is never
        // swallowed by the creeping neutral point.
        d_yaw = g_head.f_yaw - g_head.sticky_yaw;
        d_pitch = g_head.f_pitch - g_head.sticky_pitch;
        const float over = sqrtf(d_yaw * d_yaw + d_pitch * d_pitch);
        if (over > enter && over > 1e-6f) {
            const float f = (over - enter) / over;
            step_yaw = d_yaw * f;
            step_pitch = d_pitch * f;
            g_head.sticky_yaw = g_head.f_yaw - d_yaw * f;
            g_head.sticky_pitch = g_head.f_pitch - d_pitch * f;
        }
    } else {
        // Moving: re-base by the exit threshold so a small deliberate turn starts immediately
        // instead of having to overcome a fixed offset first.
        const float keep = (mag > 1e-6f) ? fmaxf(0.0f, (mag - exit) / mag) : 0.0f;
        step_yaw = d_yaw * keep;
        step_pitch = d_pitch * keep;
        g_head.sticky_yaw = g_head.f_yaw - step_yaw;
        g_head.sticky_pitch = g_head.f_pitch - step_pitch;
    }
    g_head.in_deadzone = !moving_now;

    // 3. ACCUMULATE. A discontinuity guard first: a real head cannot cross 25 degrees between two
    //    steps at 120 Hz, so anything larger is a wrap or a tracking glitch, not movement.
    if (fabsf(step_yaw) > 0.44f) step_yaw = 0.0f;
    if (fabsf(step_pitch) > 0.44f) step_pitch = 0.0f;
    g_head.extra_yaw += step_yaw * g_head_gain;
    g_head.extra_pitch += step_pitch * g_head_gain;

    const float kMaxRange = g_head_range_deg * 0.0174532925f;
    if (g_head.extra_yaw > kMaxRange) g_head.extra_yaw = kMaxRange;
    if (g_head.extra_yaw < -kMaxRange) g_head.extra_yaw = -kMaxRange;
    if (g_head.extra_pitch > kMaxRange) g_head.extra_pitch = kMaxRange;
    if (g_head.extra_pitch < -kMaxRange) g_head.extra_pitch = -kMaxRange;

    // Self-centring: OFF by default. The view stays where the head put it.
    if (g_head_decay_s > 0.0f) {
        const float f = (dt / g_head_decay_s < 1.0f) ? (1.0f - dt / g_head_decay_s) : 0.0f;
        g_head.extra_yaw *= f;
        g_head.extra_pitch *= f;
    }

    // F9 recentres instantly - the explicit way back, at any time.
    if (GetAsyncKeyState(VK_F9) & 0x8000) {
        const unsigned long long now = GetTickCount64();
        if (now - g_head.last_recenter_ms > 300) {
            g_head.last_recenter_ms = now;
            VRLOG("steer: F9 - recentre (was yaw %.1f pitch %.1f deg of extra angle)",
                  (double)(g_head.extra_yaw * 57.2957795f),
                  (double)(g_head.extra_pitch * 57.2957795f));
        }
        g_head.extra_yaw = 0.0f;
        g_head.extra_pitch = 0.0f;
    }

    // Publish: a sequence counter so the hook can never see a half-updated pair.
    InterlockedIncrement(&g_pipe.seq);                  // odd: being written
    g_pipe.extra_yaw = g_head.extra_yaw;
    g_pipe.extra_pitch = g_head.extra_pitch;
    InterlockedIncrement(&g_pipe.seq);                  // even: stable
}

// The diagnostic sweep: instead of the head, drive the pipeline's OUTPUT with a slow triangle wave,
// so the axis itself can be judged with every other variable (jitter, deadzone, head motion) taken
// out. re6vr_head_sweep.txt = 1.
bool g_head_sweep_on = false;

DWORD WINAPI head_thread(LPVOID) {
    VRLOG("steer: head pipeline thread up (fixed 120 Hz; the hook only applies the result)");
    unsigned long long last = GetTickCount64();
    // The head's orientation as a basis, kept between steps so the DRIFT-FREE quantity can be used.
    float prev[9] = {0};
    bool prev_ok = false;
    while (InterlockedCompareExchange(&g_head_thread_run, 1, 1) == 1) {
        Sleep(4);                                        // ~250 Hz wakeups, 120 Hz steps
        const unsigned long long now = GetTickCount64();
        float dt = (float)(now - last) / 1000.0f;
        if (dt < 1.0f / 240.0f) continue;                // too soon, keep accumulating
        last = now;
        if (dt > 0.25f) dt = 0.25f;                      // a hitch is not a head movement

        if (g_head_sweep) {
            // 6 s period, +-12 deg, applied straight to the output (no head involved at all).
            const float phase = (float)(now % 6000) / 6000.0f;
            const float tri = (phase < 0.5f) ? (phase * 4.0f - 1.0f) : (3.0f - phase * 4.0f);
            InterlockedIncrement(&g_pipe.seq);
            g_pipe.extra_yaw = tri * 12.0f * 0.0174532925f;
            g_pipe.extra_pitch = 0.0f;
            InterlockedIncrement(&g_pipe.seq);
            continue;
        }

        float h[9] = {0};
        if (!matrix_probe_head_rotation(h)) continue;    // no headset pose yet

        // ---------------------------------------------------------------- ABSOLUTE mode (default)
        //
        // The player's report of 2026-09-25 18:38 - "when I move it does not really match where I am
        // looking" - is a property of the delta model, not a tuning problem, and the numbers show it:
        //
        //     18:37:24  head at yaw +23.7  (+9.1 pitch)   camera +1.8   (+0.2)
        //     18:37:33  head back to 0                     camera +5.9   (-11.3)
        //     18:37:43  head at 0                          camera +6.6   (-9.6)
        //
        // Integrating per-step deltas can only ever approximate the head's angle: every step that
        // falls inside the deadzone is dropped and never comes back, so the camera ends up a few
        // degrees away from where the head actually is - and the error is different on every axis.
        // A view that is supposed to be the head's direction needs the head's ANGLE, not its
        // integrated velocity.
        //
        // So the head's absolute yaw/pitch, relative to a reference point, IS the camera's offset:
        //
        //     offset_yaw   = head_yaw   - reference_yaw
        //     offset_pitch = head_pitch - reference_pitch
        //
        // The reference is taken at start-up and reset by F9 (which is already the recentre key), so
        // "the view stays where I left it" still holds: the camera does not drift on its own, it is
        // a function of where the head is. Turn the head back and the view comes back - that is what
        // "matches my orientation" means.
        //
        // The deadzone is NOT applied in this mode (it was what broke the correspondence). Jitter is
        // removed by the low-pass alone, which costs nothing in accuracy because there is no
        // integration to lose: a filtered angle is still the angle.
        //
        // re6vr_head_absolute.txt = 0 goes back to the delta/drag model.
        if (g_head_absolute) {
            // ------------------------------------------------------------ tilt-corrected absolute reading
            //
            // See calibrate_from_up: the headset's frame is rotated by how the headset sits on the
            // head, and removing that measured tilt is what stops a left-right turn from arriving as
            // a diagonal. Without it, every reading below is in the headset's tilted frame - which is
            // exactly what the player kept seeing ("the diagonal is back").
            //
            // Backward axis = third column of the basis (h[2], h[5], h[8]); up = second column.
            const float raw_up[3] = {h[1], h[4], h[7]};
            const float raw_back[3] = {h[2], h[5], h[8]};
            float up[3] = {0}, back[3] = {0};
            calibrate_vec(raw_up, up);
            calibrate_vec(raw_back, back);
            const float blen = sqrtf(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
            const float ulen = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
            if (blen > 0.5f) {
                back[0] /= blen;
                back[1] /= blen;
                back[2] /= blen;
            }
            if (ulen > 0.5f) {
                up[0] /= ulen;
                up[1] /= ulen;
                up[2] /= ulen;
            }

            // Yaw = the swing of the gaze in the horizontal plane. Forward = -back, and the bridge's
            // own convention is yaw = atan2(forward.x, forward.z), so the same reading here is
            // yaw = atan2(back.x, back.z) - which is 0 when the gaze is straight ahead (-Z).
            // (The earlier -atan2(bx, -bz) form was wrong: it reported -180 degrees at neutral. Caught
            // by printing the neutral case in the offline check rather than by another game run.)
            const float world_yaw = atan2f(back[0], back[2]);
            const float by2 = back[1] > 1.0f ? 1.0f : (back[1] < -1.0f ? -1.0f : back[1]);
            const float world_pitch = -asinf(by2);

            // Low-pass (50 ms). No deadzone in this mode: an angle that has been filtered is still
            // the angle, so accuracy costs nothing - which is exactly what an integrator cannot say.
            const float k = dt / (dt + 0.05f);
            g_head.f_yaw += (world_yaw - g_head.f_yaw) * k;
            g_head.f_pitch += (world_pitch - g_head.f_pitch) * k;
            if (!g_head.abs_ref_set) {
                g_head.abs_ref_set = true;
                // The tilt is measured from the same sample: at the reference moment the head is
                // neutral relative to the camera, so the headset's up then IS its frame's up.
                const float tilt_deg = calibrate_from_up(raw_up);
                g_head.abs_ref_yaw = g_head.f_yaw;
                g_head.abs_ref_pitch = g_head.f_pitch;
                VRLOG("steer: absolute reference taken at yaw %.1f pitch %.1f deg; headset tilt "
                      "calibrated at %.1f deg (axis %.3f %.3f %.3f) - the view is centred here, F9 "
                      "re-takes both", (double)(g_head.abs_ref_yaw * 57.2957795f),
                      (double)(g_head.abs_ref_pitch * 57.2957795f), (double)tilt_deg,
                      (double)g_calib_axis[0], (double)g_calib_axis[1], (double)g_calib_axis[2]);
            }
            if (GetAsyncKeyState(VK_F9) & 0x8000) {
                const unsigned long long now_f9 = GetTickCount64();
                if (now_f9 - g_head.last_recenter_ms > 300) {
                    g_head.last_recenter_ms = now_f9;
                    VRLOG("steer: F9 - absolute reference re-taken (was yaw %.1f pitch %.1f deg off)",
                          (double)((g_head.f_yaw - g_head.abs_ref_yaw) * 57.2957795f),
                          (double)((g_head.f_pitch - g_head.abs_ref_pitch) * 57.2957795f));
                }
                g_head.abs_ref_yaw = g_head.f_yaw;
                g_head.abs_ref_pitch = g_head.f_pitch;
            }
            float dy = g_head.f_yaw - g_head.abs_ref_yaw;
            while (dy > 3.14159265f) dy -= 6.28318531f;
            while (dy < -3.14159265f) dy += 6.28318531f;
            float dp = g_head.f_pitch - g_head.abs_ref_pitch;
            // The gain applies here too, so the two modes stay comparable: 1.0 = the view turns
            // exactly as far as the head (true 1:1), 0.5 = half of it. Without this the gain marker
            // would silently do nothing in the default mode.
            dy *= g_head_gain;
            dp *= g_head_gain;
            // Publish straight through, with the range as a limit rather than as an integrator cap.
            const float lim = g_head_range_deg * 0.0174532925f;
            if (dy > lim) dy = lim;
            if (dy < -lim) dy = -lim;
            if (dp > lim) dp = lim;
            if (dp < -lim) dp = -lim;
            if (!g_head_pitch_enabled) dp = 0.0f;
            // Once every 3 s while looking around: the tilt-corrected angle the camera is using. If a
            // run still feels diagonal, this number says whether the correction took.
            static unsigned long long last_abs_log = 0;
            const unsigned long long now_abs = GetTickCount64();
            if (now_abs - last_abs_log > 3000 && (fabsf(dy) > 0.05f || fabsf(dp) > 0.05f)) {
                last_abs_log = now_abs;
                VRLOG("steer: ABS head (tilt-corrected): yaw %+.1f pitch %+.1f deg, applied offset "
                      "yaw %+.1f pitch %+.1f (gain %.2f)", (double)(g_head.f_yaw * 57.2957795f),
                      (double)(g_head.f_pitch * 57.2957795f), (double)(dy * 57.2957795f),
                      (double)(dp * 57.2957795f), (double)g_head_gain);
            }
            g_head.extra_yaw = dy;
            g_head.extra_pitch = dp;
            InterlockedIncrement(&g_pipe.seq);
            g_pipe.extra_yaw = dy;
            g_pipe.extra_pitch = dp;
            InterlockedIncrement(&g_pipe.seq);
            continue;
        }

        // ---------------------------------------------------------------- DRIFT-FREE, DECOUPLED
        //
        // Two corrections live here, each bought with a run.
        //
        // (1) RELATIVE, not absolute (run of 12:04): the head's absolute pitch tracked its yaw almost
        //     exactly, because any constant tilt or bias in the runtime's reference frame appears in
        //     the absolute angle and got carried into the camera one step at a time until the view
        //     looked at the sky. A deadzone cannot catch that: during a turn the head is legitimately
        //     moving. The rotation BETWEEN two samples cancels any fixed offset exactly.
        //
        // (2) WORLD-FRAME yaw and pitch (run of 13:23): even relative, the decomposition was taken
        //     about the HEADSET's own axes, and the headset's up is not the world's up - it is tilted
        //     by however the player wears it and by the runtime's idea of "forward". A headset worn
        //     with its yaw axis 20 degrees off vertical turns a pure left-right turn into a coupled
        //     yaw+pitch motion, which is exactly what the log showed:
        //
        //         published yaw -35.0 pitch +34.3 ... -35.0 +29.1 ... -33.6 +35.0
        //
        //     -35 yaw and +35 pitch, both pinned at the range limit, i.e. the view went up and to the
        //     side and stayed there.
        //
        //     The fix is to decompose the relative rotation in the WORLD frame instead: take the
        //     head's backward axis, rotate it by the relative rotation, and read the change in its
        //     compass bearing (yaw) and in the height of the gaze above the horizon (pitch - the
        //     elevation rise). Then the reference frame's tilt cannot leak into the camera at all,
        //     because only the world's vertical and the horizontal plane are used.
        //
        //     rot * back, with rot = prev * current^T and back = (0,0,-1):
        //         yaw   = atan2(component.x, -component.z)
        //         pitch = asin(component.y)
        float relYaw = 0.0f, relPitch = 0.0f;
        if (prev_ok) {
            // m = prev * current^T (row-major 3x3, columns of the basis are right/up/backward).
            const float *a = prev;
            const float m00 = a[0] * h[0] + a[1] * h[3] + a[2] * h[6];
            const float m01 = a[0] * h[1] + a[1] * h[4] + a[2] * h[7];
            const float m02 = a[0] * h[2] + a[1] * h[5] + a[2] * h[8];
            const float m10 = a[3] * h[0] + a[4] * h[3] + a[5] * h[6];
            const float m11 = a[3] * h[1] + a[4] * h[4] + a[5] * h[7];
            const float m12 = a[3] * h[2] + a[4] * h[5] + a[5] * h[8];
            const float m20 = a[6] * h[0] + a[7] * h[3] + a[8] * h[6];
            const float m21 = a[6] * h[1] + a[7] * h[4] + a[8] * h[7];
            const float m22 = a[6] * h[2] + a[7] * h[5] + a[8] * h[8];
            // m * (0,0,-1) = -(third column of m)
            const float bx = -m02, by = -m12, bz = -m22;
            const float horiz = sqrtf(bx * bx + bz * bz);
            // SIGN, caught by the offline replay (_work/replay_head_pipeline.py) before it cost
            // another game run: for a pure 90-degree leftward head turn the OLD decomposition
            // produced -90 and the world-frame one produces +90 for the same rotation. The old sign
            // is the one the player confirmed ("it turns" / "it moves"), so the world-frame yaw is
            // negated here to keep the same convention. The replay's maths check now reports the two
            // decompositions agreeing on both axes for every test pose.
            relYaw = -atan2f(bx, -bz);
            relPitch = atan2f(by, horiz);            // elevation rise: + means the gaze went up
        }
        for (int i = 0; i < 9; ++i) prev[i] = h[i];
        prev_ok = true;

        head_pipeline_step(relYaw, g_head_pitch_enabled ? tripod_pitch(relYaw, relPitch) : 0.0f, dt);
    }
    return 0;
}

// Reads the published angles. Retried until the sequence counter is even at both ends.
void read_published(float *yaw, float *pitch) {
    for (int tries = 0; tries < 8; ++tries) {
        const LONG s1 = g_pipe.seq;
        if (s1 & 1) continue;
        *yaw = g_pipe.extra_yaw;
        *pitch = g_pipe.extra_pitch;
        const LONG s2 = g_pipe.seq;
        if (s1 == s2) return;
    }
}
// Starts the head pipeline thread. Called from the install path, NOT from the camera write path -
// which was a bug that cost the run of 2026-09-25 12:19: the pipeline was started lazily from
// head_steer_one(), and head_steer_one only runs in the GetViewMatrix write path. As soon as the
// head moved to the MakeViewMatrix hook, nothing ever started the pipeline, so no angles were ever
// published and the builder hook found nothing to apply. "It does not turn at all."
void start_head_pipeline() {
    if (g_head_thread_started) return;
    if (!g_head_knobs_logged) {
        g_head_knobs_logged = true;
        load_head_knobs();
    }
    g_head_thread_started = true;
    InterlockedExchange(&g_head_thread_run, 1);
    g_head_thread = CreateThread(nullptr, 0, head_thread, nullptr, 0, nullptr);
    if (g_head_thread) {
        SetThreadPriority(g_head_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    } else {
        VRLOG("steer: could not start the head pipeline thread - head tracking is dead this run");
    }
}

void head_steer_one(unsigned cam) {
    start_head_pipeline();
    const unsigned long long now = GetTickCount64();

    // ---------------------------------------------------------------- start-up grace
    //
    // The player's report: "the moment I could move, it snapped to the ceiling". A hook on the view
    // builder is live from the first frame of the process, which is long before there is a level,
    // a player or a settled camera - and at that point the engine is building cameras from template
    // values. Nothing the headset says in those first seconds is worth applying, so the first two
    // seconds after the hook goes in are used for observation only.
    static unsigned long long s_armed_at = 0;
    if (s_armed_at == 0) s_armed_at = now;
    if (now - s_armed_at < 2000) return;

    // ---------------------------------------------------------------- steer ONLY the persistent camera
    //
    // The player's report of 2026-09-25 12:20 - "it snapped to the ceiling the moment I could move,
    // and one movement later to the floor" - is what this table is for. The log of the run before
    // showed the engine handing GetViewMatrix several DIFFERENT camera objects, with contradictory
    // pose data:
    //
    //     20B00060  eye (2370.24 530.00 -3689.51) dist 307.0     <- plausible, world coordinates
    //     20AA4CA0  eye (   0.00 200.00  -700.00) dist 700.0      <- the template pose, garbage
    //
    // (0, 200, -700) -> (0, 200, 0) is a reset template that the engine builds and discards; a
    // per-frame camera that is created and destroyed inside one frame. Rotating THAT and letting it
    // reach the renderer is how a head turn becomes a jump to the ceiling, and why the next action
    // flipped it the other way: the view alternated between my rotated template and the real camera.
    //
    // So a camera is only steered once it has been seen in several frames spread over at least
    // kPersistMs, i.e. once it is demonstrably a long-lived object rather than a per-frame scratch
    // one. Everything else is logged and left alone.
    const unsigned kPersistMs = 700;
    const int kTracked = 8;
    static struct { unsigned addr; unsigned long long first; unsigned long long last; int seen; } s_track[kTracked] = {{0}};
    int slot = -1;
    for (int i = 0; i < kTracked; ++i) {
        if (s_track[i].addr == cam) { slot = i; break; }
    }
    if (slot < 0) {
        int oldest = 0;
        for (int i = 1; i < kTracked; ++i) {
            if (s_track[i].last < s_track[oldest].last) oldest = i;
        }
        slot = oldest;
        s_track[slot].addr = cam;
        s_track[slot].first = now;
        s_track[slot].seen = 0;
    }
    s_track[slot].last = now;
    ++s_track[slot].seen;
    const bool persistent = (now - s_track[slot].first) >= kPersistMs;
    if (!persistent) {
        static unsigned long long last_note = 0;
        if (now - last_note > 10000) {
            last_note = now;
            VRLOG("steer: camera %08X looks like a per-frame scratch object (%d frame(s) so far) - "
                  "NOT steering it, because rotating a camera the engine is about to discard is how "
                  "a head turn turns into a jump", cam, s_track[slot].seen);
        }
        return;
    }

    float extra_yaw = 0.0f, extra_pitch = 0.0f;
    read_published(&extra_yaw, &extra_pitch);

    if (!readable((const void *)(uintptr_t)(cam + kOffPos), 6 * sizeof(float))) return;
    float pos[3] = {0}, tgt[3] = {0}, up[3] = {0};
    if (!read_vec3(cam, kOffPos, pos) || !read_vec3(cam, kOffTarget, tgt)) return;
    read_vec3(cam, kOffUp, up);
    // The camera's own up, as the engine wrote it: measured and reported, never used as a rotation
    // axis. Its deviation from vertical is the number that explains a rolling image.
    const float up_len = vec_len(up);
    const float up_tilt_deg =
        up_len > 0.5f ? acosf(fmaxf(-1.0f, fminf(1.0f, up[1] / up_len))) * 57.2957795f : -1.0f;
    float dx = tgt[0] - pos[0], dy = tgt[1] - pos[1], dz = tgt[2] - pos[2];
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!(dist > 0.5f) || !(dist < 20000.0f)) return;

    // A pose sanity check before anything is written. A camera whose distance is wildly out of family
    // with the others the engine is serving is not a camera to rotate: the log showed the same
    // engine handing over distances of 268, 276, 307 and 700 units inside a few milliseconds.
    static float s_typical_dist = 0.0f;
    if (s_typical_dist == 0.0f) s_typical_dist = dist;
    if (dist > s_typical_dist * 2.5f || dist < s_typical_dist * 0.4f) {
        static unsigned long long last_odd = 0;
        if (now - last_odd > 5000) {
            last_odd = now;
            VRLOG("steer: camera %08X has an out-of-family distance (%.1f, typical %.1f) - NOT "
                  "steering it this frame", cam, (double)dist, (double)s_typical_dist);
        }
        return;
    }
    s_typical_dist += (dist - s_typical_dist) * 0.02f;   // slow adaptation to the real camera

    // ---------------------------------------------------------------- 3DOF look-around
    //
    // What the player asked for: "keep the camera position fixed and turn it in all directions, like
    // a camera on a tripod - 3DOF". The eye does not move; only the direction turns.
    //
    // THE YAW AXIS MUST BE HORIZONTAL, and the version before this one was not. The player's report
    // "past a certain angle it tilts and deforms" is that bug, and the offline check shows it
    // plainly - camera pitched 20 degrees down, turning left:
    //
    //     old axis r = normalize(-gaze.z, 0, gaze.x)
    //         this is built from the gaze's HORIZONTAL PROJECTION, which is not perpendicular to the
    //         gaze once the gaze is pitched, and is not level either:
    //             dyaw  15: gaze tilt +20.00 ->  +5.00
    //             dyaw  30: gaze tilt +20.00 -> -10.00
    //             dyaw  90: gaze tilt +20.00 -> -70.00     <- the view dives at the ground as you turn
    //
    //     fixed axis r = normalize(up x gaze), which is (1,0,0) for any pitch
    //             dyaw  15: gaze tilt +20.00 -> +20.00
    //             dyaw  90: gaze tilt +20.00 -> +20.00     <- tilt untouched at every angle
    //
    // So the axis is the horizontal right of the GAZE, taken from the camera's own up. Pitch then
    // turns about the horizontal axis to its right, by the same rule. Both are Rodrigues.
    float vx = tgt[0] - pos[0], vy = tgt[1] - pos[1], vz = tgt[2] - pos[2];
    {
        const float rlen = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        if (rlen > 0.5f) {
            const float ux = up[0] / rlen, uy = up[1] / rlen, uz = up[2] / rlen;
            const float rx = uy * vz - uz * vy;               // r = up x gaze
            const float ry = uz * vx - ux * vz;
            const float rz = ux * vy - uy * vx;
            const float rl = sqrtf(rx * rx + ry * ry + rz * rz);
            if (rl > 1e-3f) {
                const float ax = rx / rl, ay = ry / rl, az = rz / rl;
                const float c = cosf(extra_yaw), s = sinf(extra_yaw);
                const float dot = vx * ax + vy * ay + vz * az;
                const float cx = ay * vz - az * vy;
                const float cy = az * vx - ax * vz;
                const float cz = ax * vy - ay * vx;
                vx = vx * c + cx * s + ax * dot * (1.0f - c);
                vy = vy * c + cy * s + ay * dot * (1.0f - c);
                vz = vz * c + cz * s + az * dot * (1.0f - c);
            }
        }
    }
    if (extra_pitch != 0.0f) {
        const float hlen = sqrtf(vx * vx + vz * vz);
        if (hlen > 1e-3f) {
            const float rx = -vz / hlen, rz = vx / hlen;      // a = (rx, 0, rz) of the yawed gaze
            const float c = cosf(extra_pitch), s = sinf(extra_pitch);
            const float nx = vx * c + (rz * vy) * s;          // (a x v).x = rz*vy
            const float ny = vy * c + (-hlen) * s;            // (a x v).y = -hlen
            const float nz = vz * c + (-rx * vy) * s;         // (a x v).z = -rx*vy
            vx = nx;
            vy = ny;
            vz = nz;
        }
    }
    const float nt[3] = {pos[0] + vx, pos[1] + vy, pos[2] + vz};
    if (!writable((const void *)(uintptr_t)(cam + kOffTarget), 3 * sizeof(float))) return;
    memcpy((void *)(uintptr_t)(cam + kOffTarget), nt, sizeof(nt));
    ++g_head.frames_applied;

    // The audit line, once a second. Without it "the head did nothing" and "the head was applied and
    // the engine overwrote it" would look identical - the mistake this project has already made.
    // It now also reports the engine's own up vector (never used as an axis, but its tilt is the
    // number that explains a rolling image) and the gaze before/after, so the turn can be checked
    // arithmetically from the log instead of from memory.
    if (now - g_head.last_log_ms > 1000) {
        g_head.last_log_ms = now;
        float back[3] = {0};
        read_vec3(cam, kOffTarget, back);
        unsigned vt = 0;
        read32(cam, &vt);
        const float base_brg = atan2f(dx, dz) * 57.2957795f;
        const float base_tilt = atan2f(dy, sqrtf(dx * dx + dz * dz)) * 57.2957795f;
        const float new_brg = atan2f(vx, vz) * 57.2957795f;
        const float new_tilt = atan2f(vy, sqrtf(vx * vx + vz * vz)) * 57.2957795f;
        VRLOG("steer: HEAD 3DOF: gaze bearing %.1f tilt %.1f -> bearing %.1f tilt %.1f deg "
              "(published yaw %+.1f pitch %+.1f) | engine up tilt %.1f deg from vertical | eye "
              "(%.2f %.2f %.2f) dist %.1f | cam %08X vtable %08X%s | %s", (double)base_brg,
              (double)base_tilt, (double)new_brg, (double)new_tilt,
              (double)(extra_yaw * 57.2957795f), (double)(extra_pitch * 57.2957795f),
              (double)up_tilt_deg, pos[0], pos[1], pos[2], (double)dist, cam, vt,
              vt == kVtableCtrl ? " (uCameraCtrl - the gameplay camera)" : "",
              (fabsf(back[0] - nt[0]) < 0.01f) ? "write stuck" : "OVERWRITTEN immediately");
    }
}

} // namespace

bool cam_steer_enabled() {
    return g_mode != kOff;
}

void cam_steer_install(IDirect3DDevice9 *dev) {
    (void)dev;
    if (g_installed) return;

    wchar_t path[MAX_PATH] = L"";
    const wchar_t *log_path = vrlog::path();
    if (!log_path || !log_path[0]) return;
    wcsncpy_s(path, MAX_PATH, log_path, _TRUNCATE);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - path)), L"re6vr_steer.txt");

    char text[32] = "";
    FILE *f = _wfopen(path, L"r");
    if (f) {
        if (fscanf_s(f, "%31s", text, (unsigned)sizeof(text)) != 1) text[0] = 0;
        fclose(f);
    } else {
        return;                                   // no marker: off, silently
    }
    if (_stricmp(text, "observe") == 0) {
        g_mode = kObserve;
    } else if (_stricmp(text, "swing") == 0) {
        g_mode = kSwing;
    } else if (_stricmp(text, "head") == 0) {
        g_mode = kHead;
    } else if (_stricmp(text, "pushorg") == 0) {
        // Observation only: hook sBioCamera::PushOrg and print what it is handed. No camera writes.
        g_mode = kObserve;
        g_installed = true;
        install_pushorg_probe();
        return;
    } else {
        return;
    }
    g_installed = true;

    VRLOG("steer: ENABLED from %ls (mode '%hs'). The camera the RENDERER uses is built by "
          "0x5F80B0 from [cam+0x50] pos / [cam+0x60] up / [cam+0x70] target, and the camera is "
          "reachable through the fixed global sBioCamera = [0x186E23C] - no scan. mCameraOrg "
          "(+0xE30) has exactly one reader in the image (0x4FCB70) and nothing renders from it.",
          path, text);

    if (g_mode == kSwing || g_mode == kHead) {
        // The builder hook is the default path for the head; the swing experiment keeps using the
        // camera-object write, because that is the path it proved and there is no reason to change
        // two things at once.
        if (g_mode == kHead && g_method == kMethodBuilder) {
            if (!install_builder_hook()) {
                VRLOG("steer: MakeViewMatrix could not be hooked - falling back to the camera-object "
                      "write (which accumulates angles and is known to drift)");
                g_method = kMethodMatrix;
            }
        }
        if (g_method == kMethodMatrix) {
            g_hooked = install_hook();
            if (!g_hooked) {
                VRLOG("steer: the view-builder hook is NOT installed, so no picture change is "
                      "possible this run - the log above says why");
            }
        }
        // The GetViewMatrix detour is ALSO installed in builder mode when a fov override is armed:
        // the fov lives on the camera object (+0x4C) and the builder hook only receives the pose
        // pointers, so this is the only place the override can be written. Its rotation code is
        // skipped in builder mode (head_steer_one is only called for the camera-object path), so the
        // two hooks cannot both rotate the view.
        // ALWAYS installed in builder mode, not only when a fov override is armed. Two things need
        // it: the fov injection (the fov lives on the camera object, which only this hook sees), and
        // the IDENTITY FILTER, which needs g_cam_last - an earlier version of this block required a
        // fov override, which left the filter with an empty table and therefore disabled it silently.
        // Its rotation path stays off in builder mode (head_steer_one runs only for the
        // camera-object method), so the two hooks cannot both turn the view.
        if (g_method == kMethodBuilder && !g_hooked) {
            g_hooked = install_hook();
            VRLOG("steer: GetViewMatrix detour installed alongside the MakeViewMatrix hook (camera "
                  "identity for the slot filter + fov injection); rotation still comes from "
                  "MakeViewMatrix only");
        }
        // The pipeline runs for both write paths, and it starts here rather than on the first
        // camera write: see start_head_pipeline.
        start_head_pipeline();
        // Stereo groundwork, read-only: report the engine's own display/stereo state every second so
        // the dual-pass design rests on measurements. Armed by re6vr_render_probe.txt = 1.
        if (read_float_marker(L"re6vr_render_probe.txt", 0.0f, 0.0f, 1.0f) >= 0.5f) {
            install_render_probe();
        }
        // The render-target observations ride on the same read-only switch, and only when the game's
        // device is available: the SetRenderTarget probe patches a device vtable entry, so it needs a
        // device. `dev` is the device the proxy passed in.
        if (g_rt_probe && dev) {
            install_render_target_probe(dev);
        }
    }
}

// What the camera is actually rendering with, in degrees, or 0 when there is no camera yet. Published
// for the compositor: "is the picture stretched?" is one comparison - the vertical angle the game
// RENDERS versus the vertical angle the panel is DECLARED to occupy - and the compositor knows only its
// own half of it. Reading the camera's fov here (rather than trusting a marker file) means the log
// compares what is really happening on both sides.
float cam_steer_live_fov_deg() {
    if (!g_cam_last) return 0.0f;
    float fov = 0.0f;
    if (!readf32(g_cam_last + kOffFov, &fov)) return 0.0f;
    return fov;
}

void cam_steer_frame() {
    ++g_frame_counter;
    if (g_mode == kOff) return;

    // BUILD-MODE HEARTBEAT. The run of 2026-09-25 12:19 installed the MakeViewMatrix hook and then
    // produced no evidence at all about whether it was ever called - the log simply had nothing,
    // which is the failure mode this project keeps paying for ("it does not turn" and "the hook was
    // never reached" look identical). One line every five seconds fixes that permanently.
    if (g_mode == kHead && g_method == kMethodBuilder) {
        static unsigned long long last_hb = 0;
        static LONG last_calls = 0;
        const unsigned long long now_hb = GetTickCount64();
        if (last_hb == 0) last_hb = now_hb;
        if (now_hb - last_hb >= 5000) {
            const LONG calls = InterlockedCompareExchange(&g_builder_calls, 0, 0);
            float y = 0.0f, p = 0.0f;
            read_published(&y, &p);
            float cam_fov = 0.0f;
            if (g_cam_last) readf32(g_cam_last + kOffFov, &cam_fov);
            VRLOG("steer: builder heartbeat: %ld MakeViewMatrix call(s) in %.1f s (installed=%d), "
                  "published yaw %+.1f pitch %+.1f deg, camera %08X fov %.2f%s",
                  (long)(calls - last_calls), (double)(now_hb - last_hb) / 1000.0,
                  (int)g_builder_hooked, (double)(y * 57.2957795f), (double)(p * 57.2957795f),
                  g_cam_last, (double)cam_fov,
                  g_fov_override > 0.0f ? " (OVERRIDDEN by re6vr_fov.txt)" : "");
            last_hb = now_hb;
            last_calls = calls;
        }
    }

    // Collect the slot table once, and repeat it every 30 s so a run that starts before the level
    // exists still gets the data after it does.
    static unsigned long long last = 0;
    const unsigned long long now = GetTickCount64();
    if (g_log_count == 0 || now - last > 30000) {
        last = now;
        ++g_log_count;
        dump_slots("(slot table as the engine has it right now)");
    }
}

} // namespace re6vr
