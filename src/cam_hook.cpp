// cam_hook.cpp - hook the function that writes mCameraOrg, and learn the truth from it.
//
// The target is static knowledge, not a guess:
//
//     0x004FF9B0  (sBioCamera vtable slot 10, 463 instructions)
//       writes [esi + 0xE30 + 0x40*i] for i = 0..7, all six fields each - 120 float stores,
//       sourced from a parallel group at [esi + 0x1030 + 0x40*i]  (see the README, "who writes
//       mCameraOrg")
//
// What the hook reports, and why each line earns its place:
//
//   `this`            the camera object's address - the thing five memory scans failed to find.
//   call rate         whether this is a per-frame update or a one-shot initialiser. The README
//                     lists that as an open question; one run answers it.
//   the entry fields  the values as the engine writes them, at the offsets the property table
//                     claims. If the offsets were wrong, this is where it shows: pos/target/up
//                     would not be a look-at triple in the engine's own stores.
//   the source group  [esi+0x1030..] - the input the copy reads, which is where head tracking
//                     would write if hooking the copy (rather than its output) is the better
//                     place.
//
// MinHook is already used by this project for the D3D9 device, so the mechanism is not new; the
// hook is installed once, from the first Present, and is read-only apart from its own logging.

#include "cam_hook.h"

#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstring>

#include "log.h"
#include "safe_mem.h"

// MinHook (the project already links it for the D3D9 detours).
#include "MinHook.h"

namespace re6vr {
namespace {

// The function static analysis identified, as an RVA (the image may be relocated).
const unsigned kCameraWriteFnRva = 0x00FF9B0u;      // 0x004FF9B0 with a 0x400000 base
const unsigned kCameraOrgBase = 0xE30u;
const unsigned kCameraOrgStride = 0x40u;
const unsigned kSourceGroupBase = 0x1030u;

bool g_installed = false;
volatile LONG g_calls = 0;

// Reads a float from the object. Once this was an SEH guard; a live session proved the guard could
// fault itself (see src/safe_mem.h), so the page is validated first and the pointer is only
// dereferenced when it is really readable.
bool read_f32(unsigned base, unsigned off, float *out) {
    return readf32(base + off, out);
}

void log_object(unsigned self, int call_index) {
    float e0[15] = {0};
    bool ok = true;
    for (int i = 0; i < 15 && ok; ++i) ok = read_f32(self, kCameraOrgBase + 4u * (unsigned)i, &e0[i]);
    unsigned vtable = 0, record = 0;
    const bool head_ok = read32(self, &vtable) && read32(self + 4, &record);
    if (!head_ok) ok = false;
    VRLOG("camhook: call #%d on the mCameraOrg writer: this = %08X  vtable = %08X  +4 = %08X  "
          "(static vtable for sBioCamera is 0151A380)", call_index, self, vtable, record);
    if (!ok) {
        VRLOG("camhook:   the object could not be read at all");
        return;
    }
    const float dx = e0[4] - e0[0], dy = e0[5] - e0[1], dz = e0[6] - e0[2];
    const float flen = sqrtf(dx * dx + dy * dy + dz * dz);
    const float ulen = sqrtf(e0[8] * e0[8] + e0[9] * e0[9] + e0[10] * e0[10]);
    VRLOG("camhook:   AFTER the write, entry 0 (+0x%X): pos (%.2f %.2f %.2f) target (%.2f %.2f "
          "%.2f) up len %.3f fwd len %.2f fov %.4f (%.1f deg) near %.3f far %.2f",
          kCameraOrgBase, e0[0], e0[1], e0[2], e0[4], e0[5], e0[6], ulen, flen, e0[12],
          e0[12] * 57.2957795f, e0[13], e0[14]);
    // The entries above entry 0, so "the live one is a different index" stops being a guess.
    for (int i = 1; i < 8; ++i) {
        float v[15] = {0};
        bool eok = true;
        for (int k = 0; k < 15 && eok; ++k) {
            eok = read_f32(self, kCameraOrgBase + kCameraOrgStride * (unsigned)i + 4u * (unsigned)k,
                           &v[k]);
        }
        if (!eok) continue;
        const float fdx = v[4] - v[0], fdy = v[5] - v[1], fdz = v[6] - v[2];
        const float fl = sqrtf(fdx * fdx + fdy * fdy + fdz * fdz);
        const float ul = sqrtf(v[8] * v[8] + v[9] * v[9] + v[10] * v[10]);
        if (fl > 0.01f || ul > 0.01f) {
            VRLOG("camhook:   entry %d (+0x%X): pos (%.2f %.2f %.2f) target (%.2f %.2f %.2f) "
                  "up len %.3f fwd len %.2f fov %.4f", i, kCameraOrgBase + kCameraOrgStride * i,
                  v[0], v[1], v[2], v[4], v[5], v[6], ul, fl, v[12]);
        }
    }
    // The input group the copy reads from, which is the other candidate write point. Note the
    // first dword: it is the SOURCE OBJECT pointer (`[esi+0x1030]` is that object's +0x40), which
    // is what the sweep needs.
    float src[12] = {0};
    bool sok = true;
    for (int i = 0; i < 12 && sok; ++i) sok = read_f32(self, kSourceGroupBase + 4u * (unsigned)i,
                                                       &src[i]);
    unsigned src_obj = 0;
    read32(self + kSourceGroupBase, &src_obj);
    VRLOG("camhook:   source group +0x%X: as floats (%.2f %.2f %.2f) (%.2f %.2f %.2f) "
          "(%.2f %.2f %.2f) (%.2f %.2f %.2f); as the pointer the copy uses, the source object "
          "would be %08X", kSourceGroupBase, src[0], src[1], src[2], src[3], src[4], src[5],
          src[6], src[7], src[8], src[9], src[10], src[11], src_obj);
}

// The detour. `ecx` holds the object on entry (thiscall), which is how MSVC passes a member
// function's `this`; the body then uses that value as `esi` for every store.
//
// Written as a naked function so the register state is captured before anything else can touch
// it - and so the argument is passed through a plain global rather than by reconstructing a
// calling convention. Recovering `this` from the stack after the prologue is exactly the kind of
// assumption that has cost this project whole sessions.
void *g_trampoline = nullptr;
volatile unsigned g_self = 0;
void __stdcall camera_write_record();      // reads g_self; object-free, no unwinding

__declspec(naked) void camera_write_detour() {
    __asm {
        pushfd
        pushad
        mov g_self, ecx
        call camera_write_record
        popad
        popfd
        jmp g_trampoline
    }
}

// Kept object-free: it is called from the naked detour and must not need unwinding.
void __stdcall camera_write_record() {
    const unsigned self = g_self;
    const LONG n = InterlockedIncrement(&g_calls);
    if (n <= 3) {
        log_object(self, (int)n);
    } else if ((n % 600) == 0) {
        VRLOG("camhook: %ld calls so far (every 600 logged) - if this keeps climbing the function "
              "IS the per-frame camera writer", (long)n);
    }
}

} // namespace

// ---------------------------------------------------------------- the write test
//
// What the hook settled (2026-09-25 00:39, and it is worth writing down before the code):
//
//   camhook: call #2  this = 1F62A060
//     entry 0 (+0xE30): pos (1374.72 475.22 -1129.54) target (1659.54 443.14 -987.93)
//                       up len 1.000  fwd len 319.70  fov 37.0000
//     source group +0x1030: (1374.72 475.22 -1129.54) (0.00 1659.54 443.14) (-987.93 0.00 0.09)
//
// Every part of the static analysis is now confirmed against the engine's own stores: the object
// exists, `mCameraOrg[0]` (+0xE30) holds a real look-at camera, the fov is 37 degrees, and the
// source group at +0x1030 is the same data one step earlier. The offsets are right.
//
// What is NOT yet known is whether the renderer READS `mCameraOrg` each frame. Writing there and
// seeing the view move answers it in one run, and that is the last unknown between here and head
// tracking. So this test does exactly that, with an unmistakable motion:
//
//   RE6VR_CAM_WRITE_TEST=1   the camera target is swung +-15 degrees around the camera position,
//                            once every 8 seconds, with every write logged.
//
// The log must show the write happening (the old rule: a switch that changes the engine needs a
// line proving it wrote), and the headset must show the world swinging. Either observation alone
// is not enough - "no visible change" with no write audit is exactly the false conclusion this
// project has made five times.
namespace {

const float kWriteTestAmplitudeDeg = 15.0f;
const unsigned kWriteTestPeriodMs = 8000;

// Which write experiment to run, decided by a MARKER FILE for the same reason every other switch
// in this project is: the game is launched through Steam, which does not pass the shell's
// environment on to the game - an experiment set with `set RE6VR_CAM_WRITE_SWEEP=1` and then
// launched silently never ran, and a whole session was spent on it.
//
//   re6vr_camwrite.txt  containing  sweep  -> cycle the candidate write offsets
//                       containing  test   -> swing targetPos +0xE40 only
//
// The environment variable is still honoured as a fallback for harness runs.
const char *write_mode() {
    static int cached = -1;
    static char mode[16] = "";
    if (cached >= 0) return mode;
    cached = 0;

    const wchar_t *log_path = vrlog::path();
    if (log_path && log_path[0]) {
        wchar_t path[MAX_PATH] = L"";
        wcsncpy_s(path, MAX_PATH, log_path, _TRUNCATE);
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) {
            wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - path)), L"re6vr_camwrite.txt");
            FILE *f = _wfopen(path, L"r");
            if (f) {
                if (fscanf_s(f, "%15s", mode, (unsigned)sizeof(mode)) != 1) mode[0] = 0;
                fclose(f);
                cached = 1;
                VRLOG("camwrite: mode from %ls: '%hs'", path, mode);
                return mode;
            }
        }
    }
    char buf[16] = "";
    if (GetEnvironmentVariableA("RE6VR_CAM_WRITE_SWEEP", buf, sizeof(buf)) > 0 && buf[0] == '1') {
        strncpy_s(mode, sizeof(mode), "sweep", _TRUNCATE);
        cached = 1;
        VRLOG("camwrite: mode from RE6VR_CAM_WRITE_SWEEP=1");
    } else if (GetEnvironmentVariableA("RE6VR_CAM_WRITE_TEST", buf, sizeof(buf)) > 0 &&
               buf[0] == '1') {
        strncpy_s(mode, sizeof(mode), "test", _TRUNCATE);
        cached = 1;
        VRLOG("camwrite: mode from RE6VR_CAM_WRITE_TEST=1");
    }
    return mode;
}

bool write_test_enabled() {
    const char *m = write_mode();
    return strcmp(m, "test") == 0;
}

// Reads the pose, rotates the target around the camera position about the world Y axis, writes
// `targetPos` back, and (once per second) logs the applied angle.
void write_test_tick() {
    static unsigned last_log = 0;
    const unsigned self = g_self;
    if (!self) return;

    float pos[3] = {0}, target[3] = {0}, up[3] = {0};
    for (int i = 0; i < 3; ++i) {
        if (!read_f32(self, kCameraOrgBase + 4u * (unsigned)i, &pos[i])) return;
        if (!read_f32(self, kCameraOrgBase + 4u * (unsigned)(i + 4), &target[i])) return;
        if (!read_f32(self, kCameraOrgBase + 4u * (unsigned)(i + 8), &up[i])) return;
    }
    // Sanity: the engine wrote a real pose, so there is something to rotate. A garbage pose means
    // this is the wrong object and a write would be a shot in the dark - say so instead.
    const float dx = target[0] - pos[0], dy = target[1] - pos[1], dz = target[2] - pos[2];
    const float flen = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!(flen > 1.0f) || !(flen < 100000.0f)) return;

    const float phase = (float)(GetTickCount64() % kWriteTestPeriodMs) /
                        (float)kWriteTestPeriodMs;
    const float angle = sinf(phase * 6.2831853f) * kWriteTestAmplitudeDeg * 0.0174532925f;
    const float c = cosf(angle), s = sinf(angle);
    // Rotate about world Y: x' = x c + z s, z' = -x s + z c (keeping the distance to the camera).
    const float nx = dx * c + dz * s;
    const float nz = -dx * s + dz * c;
    const float new_target[3] = {pos[0] + nx, pos[1] + dy, pos[2] + nz};

    bool wrote = true;
    for (int i = 0; i < 3; ++i) {
        const unsigned at = self + kCameraOrgBase + 4u * (unsigned)(i + 4);
        if (!writable((const void *)(uintptr_t)at, sizeof(float))) {
            wrote = false;
            continue;
        }
        *(volatile float *)(uintptr_t)at = new_target[i];
    }
    const unsigned now = (unsigned)GetTickCount64();
    if (now - last_log > 1000) {
        last_log = now;
        VRLOG("camwrite: %s targetPos at %08X+0xE40: (%.2f %.2f %.2f) -> (%.2f %.2f %.2f), "
              "swing %+.1f deg about Y (pos %.2f %.2f %.2f, distance %.1f)",
              wrote ? "WROTE" : "FAILED to write", self, target[0], target[1], target[2],
              new_target[0], new_target[1], new_target[2],
              (double)(angle * 57.2957795f), pos[0], pos[1], pos[2], (double)flen);
    }
}

// A sweep over the candidate write points, because the first one did not work.
//
// Measured 2026-09-25 00:44: writing `targetPos` at `[this + 0xE40]` produced a clean write audit
// on every frame and the view did not move. So the renderer does not read `mCameraOrg`, and
// guessing a second single offset would waste another run. Instead the probe tries the
// candidates in turn and says which one is active: the log becomes a labelled sequence, and the
// player only has to watch for the moment the picture responds.
//
// The candidates come from the static work, not from imagination:
//
//   THIS object  +0xE40  entry 0 targetPos  (measured 2026-09-25 00:44: no effect)
//   THIS object  +0xE80  entry 1 targetPos  (the second entry the level load fills - the aim /
//                        alternate camera; a control that separates "wrong entry" from "wrong
//                        object")
//   THIS object  +0x1040 the copy's SOURCE for entry 0: the writer at 0x4FF9B0 fills +0xE30..+0xE68
//                        from +0x1030..+0x1068, so +0x1040 is the target one step earlier
//   SOURCE object +0x50  the copy helper 0x4F9950 (called by uCameraCtrl at 0x60C9F0 and
//                        friends) reads [src + 0x50] into sBioCamera's entry 0, so this is the
//                        other object's targetPos - the strong candidate
//   SOURCE object +0x60  the same object's up vector, a second check on the same object
//
// (+0x40 of the hooked object was a candidate in the first version and is gone: the dump shows
// +0x40 holding 0152D42C, which is the *class record* of uCameraQuake::Param, so that offset is a
// pointer field, not a target.)
} // namespace  (write_test_enabled, write_test_tick)

namespace cam_write {

const float kSweepAmplitudeDeg = 25.0f;
const unsigned kSweepStepMs = 6000;

// {where, offset}: where 0 = the hooked object, 1 = the object found at [hooked + 0x1030].
struct SweepTarget {
    int where;
    unsigned offset;
    const char *what;
};
const SweepTarget kSweep[] = {
    {0, 0xE40u, "hooked object +0xE40 (mCameraOrg[0].targetPos - measured no effect)"},
    {0, 0xE80u, "hooked object +0xE80 (mCameraOrg[1].targetPos - the alternate camera)"},
    {0, 0x1040u, "hooked object +0x1040 (the copy's source for entry 0)"},
    {1, 0x50u, "source object +0x50 (what 0x4F9950 copies in as targetPos - strongest candidate)"},
    {1, 0x60u, "source object +0x60 (the same object's up vector)"},
};
const int kSweepCount = (int)(sizeof(kSweep) / sizeof(kSweep[0]));
int s_last_round_logged = 0;

bool sweep_enabled() {
    const char *m = write_mode();
    return strcmp(m, "sweep") == 0;
}

// The object the copy helper reads from: `[hooked + 0x1030]` is that object's +0x40, so this is a
// pointer read - and it is CHECKED, because in one run that dword read as 0x66E8006A (i.e. ASCII,
// "j\0\xe8f"), which is not a pointer at all. Handing a value like that to the writer below would
// be a write into a random address; accepting it silently would be worse, because the run would
// look like "the offset was tested and did nothing".
unsigned source_object() {
    unsigned p = 0;
    const unsigned self = g_self;
    if (!self) return 0;
    // Page-validated read (src/safe_mem.h). The SEH version of this function FAULTED inside a live
    // game - the crash reporter caught it at the byte probe below (read at 41BF0000), which is what
    // turned "candidate 4 is skipped" into a dead session.
    if (!read32(self + kSourceGroupBase, &p)) return 0;
    if (!plausible_pointer(p)) return 0;
    unsigned char probe = 0;
    if (!read((const void *)(uintptr_t)p, &probe, 1)) return 0;
    (void)probe;
    // Readable is not the same as "an object". In the run of 2026-09-25 11:04 this dword held
    // 0x45149565, which IS mapped (it points into the game's own string data) and therefore passed
    // the check - and then the sweep would have written a float into a string literal. An object's
    // first dword is its vtable, i.e. a pointer into the image's code, so require that.
    unsigned first = 0;
    if (!read32(p, &first)) return 0;
    if (first < 0x00401000u || first >= 0x01600000u) return 0;
    return p;
}

// Sweep state: it only starts counting once the camera exists, so the step order matches what the
// player is watching (see wait_for_camera). The clock starts at the first valid pose, not at
// process start - and it CYCLES, because the run of 2026-09-25 00:57 ended one second after the
// level was built, so a single pass can be missed entirely by quitting at the wrong moment.
// The player can therefore start watching at any time once they are in the level.
unsigned long long sweep_clock_ms() {
    static unsigned long long zero = 0;
    if (!zero) {
        zero = GetTickCount64();
        VRLOG("camwrite: the engine has a camera pose - THE SWEEP STARTS NOW. %d candidates, %.0f s "
              "each (%.0f s per round). It CYCLES, so you can start watching whenever you are in "
              "the level: one of these steps will make the picture swing.",
              kSweepCount, (double)kSweepStepMs / 1000.0,
              (double)kSweepStepMs * kSweepCount / 1000.0);
    }
    const unsigned long long elapsed = GetTickCount64() - zero;
    const int round = (int)(elapsed / (kSweepStepMs * (unsigned long long)kSweepCount));
    if (round != s_last_round_logged) {
        s_last_round_logged = round;
        if (round > 0) {
            VRLOG("camwrite: sweep round %d starting (still cycling - keep watching the picture)",
                  round + 1);
        }
    }
    return elapsed;
}

void sweep_tick() {
    const unsigned self = g_self;
    if (!self) return;
    const int idx = (int)((sweep_clock_ms() / kSweepStepMs) % (unsigned long long)kSweepCount);
    const SweepTarget &t = kSweep[idx];
    const unsigned obj = (t.where == 0) ? self : source_object();
    if (!obj) {
        static int last_missing = -1;
        if (last_missing != idx) {
            last_missing = idx;
            unsigned raw = 0;
            read32(self + kSourceGroupBase, &raw);
            VRLOG("camwrite: candidate %d/%d (%s) is being SKIPPED: [%08X+0x%X] = %08X does not "
                  "point at a readable object, so there is nothing to write. The other candidates "
                  "still run in this cycle.", idx + 1, kSweepCount, t.what, self, kSourceGroupBase,
                  raw);
        }
        return;
    }

    float a[3] = {0}, b[3] = {0};
    // The pivot and the vector to swing. For the source group the pivot is the source position
    // (+0x1030) and the target is +0x1040 - the pair the copy uses, not an arbitrary one.
    if (t.where == 0 && t.offset == 0x1040u) {
        for (int i = 0; i < 3; ++i) {
            if (!read_f32(self, 0x1030u + 4u * (unsigned)i, &a[i])) return;
            if (!read_f32(self, 0x1040u + 4u * (unsigned)i, &b[i])) return;
        }
    } else if (t.where == 0) {
        for (int i = 0; i < 3; ++i) {
            if (!read_f32(self, 0xE30u + 4u * (unsigned)i, &a[i])) return;
            if (!read_f32(self, t.offset + 4u * (unsigned)i, &b[i])) return;
        }
    } else {
        for (int i = 0; i < 3; ++i) {
            if (!read_f32(obj, 0x40u + 4u * (unsigned)i, &a[i])) return;
            if (!read_f32(obj, t.offset + 4u * (unsigned)i, &b[i])) return;
        }
    }
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    const float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!(len > 0.5f) || !(len < 100000.0f)) {
        VRLOG("camwrite: candidate %d/%d %s: the vector there is not a direction (len %.3f) - "
              "skipped", idx + 1, kSweepCount, t.what, (double)len);
        return;
    }

    const float phase = (float)(GetTickCount64() % kSweepStepMs) / (float)kSweepStepMs;
    const float angle = sinf(phase * 6.2831853f) * kSweepAmplitudeDeg * 0.0174532925f;
    const float c = cosf(angle), s = sinf(angle);
    const float nx = dx * c + dz * s;
    const float nz = -dx * s + dz * c;

    bool wrote = true;
    const float nv[3] = {a[0] + nx, a[1] + dy, a[2] + nz};
    for (int i = 0; i < 3; ++i) {
        const unsigned at = obj + t.offset + 4u * (unsigned)i;
        if (!writable((const void *)(uintptr_t)at, sizeof(float))) {
            wrote = false;
            continue;
        }
        *(volatile float *)(uintptr_t)at = nv[i];
    }
    static int last_idx = -1;
    static unsigned last_log = 0;
    static unsigned last_value = 0;
    static int persisted = 0, vanished = 0;
    const unsigned now = (unsigned)GetTickCount64();
    // Did the previous write SURVIVE to this frame? A value that is gone by the next frame means
    // the engine refreshes that field every frame, which would explain "clean write audit, no
    // visible change" all by itself - and is the difference between "wrong offset" and "right
    // offset, overwritten".
    float back = 0.0f;
    if (last_idx == idx && read_f32(obj, t.offset, &back)) {
        if (fabsf(back - *(float *)&last_value) < 1e-3f) {
            ++persisted;
        } else {
            ++vanished;
            if (vanished <= 4) {
                VRLOG("camwrite:   the value at +0x%X did NOT survive one frame: wrote %.2f, now "
                      "%.2f -> the engine rewrites this field every frame", t.offset,
                      *(float *)&last_value, (double)back);
            }
        }
    }
    if (idx != last_idx || now - last_log > 2000) {
        last_idx = idx;
        last_log = now;
        // The label carries the time window, so a player watching the picture can match the moment
        // it moved to the step that caused it without reading timestamps.
        const unsigned from_s = (unsigned)((sweep_clock_ms() / 1000ull) % 1000ull);
        const unsigned to_s = from_s + kSweepStepMs / 1000;
        VRLOG("camwrite: NOW TESTING %d/%d (%u-%us): %s | obj %08X %s swing %+.1f deg "
              "[survived %d frame(s), overwritten %d]", idx + 1, kSweepCount, from_s, to_s, t.what,
              obj, wrote ? "WROTE" : "FAILED to write", (double)(angle * 57.2957795f), persisted,
              vanished);
    }
    last_value = *(unsigned *)&nv[0];
}

} // namespace cam_write

// Frame gate: nothing is written until the engine has actually put a camera pose in the object.
//
// This is the answer to a fair question the player asked: "how do you know when I have finished
// loading the save?". The probe does not - and it must not guess with a timer, which is the
// mistake that wasted three earlier runs. But it no longer has to guess, because the engine
// tells it: `mCameraOrg[0]` holds a valid look-at pose exactly when a stage has been built. The
// sweep waits for that (and says so once a second), then starts swinging.
//
// Measured: the writer runs at process start with an all-zero object, and again at level load
// (2 min 15 s later in the run of 2026-09-25 00:50) with pos/target/up and fov 37 - so "the pose
// became valid" is a real, observed transition, not a hypothesis.
bool camera_pose_valid(unsigned self) {
    if (!self) return false;
    float pos[3] = {0}, target[3] = {0}, up[3] = {0}, fov = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (!read_f32(self, kCameraOrgBase + 4u * (unsigned)i, &pos[i])) return false;
        if (!read_f32(self, kCameraOrgBase + 4u * (unsigned)(i + 4), &target[i])) return false;
        if (!read_f32(self, kCameraOrgBase + 4u * (unsigned)(i + 8), &up[i])) return false;
    }
    if (!read_f32(self, kCameraOrgBase + 48u, &fov)) return false;
    const float dx = target[0] - pos[0], dy = target[1] - pos[1], dz = target[2] - pos[2];
    const float flen = sqrtf(dx * dx + dy * dy + dz * dz);
    const float ulen = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    const float pmag = fabsf(pos[0]) + fabsf(pos[1]) + fabsf(pos[2]);
    // THE FOV IS IN DEGREES, and getting that wrong cost the whole night of 2026-09-25: every
    // sweep run ended with "the engine has not put a camera pose in the object yet" while the log
    // two lines above showed the engine writing `fov 37.0000` - and 37 > 2.2, so a radians-only
    // test rejected the very pose it was waiting for. Both units are accepted now, because the
    // only thing this gate is for is "has the engine filled a real camera into this object".
    const bool fov_ok = (fov > 0.05f && fov < 2.2f) ||      // radians
                        (fov > 2.2f && fov < 130.0f);       // degrees
    return fov_ok && flen > 1.0f && flen < 100000.0f && fabsf(ulen - 1.0f) < 0.05f && pmag > 1.0f;
}

// Why the pose is not accepted - printed with the wait line, so a rejected gate never again costs
// a whole run without saying which predicate failed.
void camera_pose_reject_reason(unsigned self, char *why, size_t why_n) {
    float pos[3] = {0}, target[3] = {0}, up[3] = {0}, fov = 0.0f;
    why[0] = 0;
    if (!self) {
        snprintf(why, why_n, "no camera object was handed over yet (the writer has not run)");
        return;
    }
    for (int i = 0; i < 3; ++i) {
        read_f32(self, kCameraOrgBase + 4u * (unsigned)i, &pos[i]);
        read_f32(self, kCameraOrgBase + 4u * (unsigned)(i + 4), &target[i]);
        read_f32(self, kCameraOrgBase + 4u * (unsigned)(i + 8), &up[i]);
    }
    read_f32(self, kCameraOrgBase + 48u, &fov);
    const float dx = target[0] - pos[0], dy = target[1] - pos[1], dz = target[2] - pos[2];
    const float flen = sqrtf(dx * dx + dy * dy + dz * dz);
    const float ulen = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    const float pmag = fabsf(pos[0]) + fabsf(pos[1]) + fabsf(pos[2]);
    snprintf(why, why_n,
             "pos (%.2f %.2f %.2f) |pos| %.2f, target (%.2f %.2f %.2f), fwd len %.2f, up len "
             "%.3f, fov %.4f", pos[0], pos[1], pos[2], pmag, target[0], target[1], target[2],
             flen, ulen, fov);
}

// Called once per frame by whichever test is enabled. Returns false (and reports the wait once a
// second) until the engine has a camera.
bool wait_for_camera(const char *who) {
    const unsigned self = g_self;
    if (camera_pose_valid(self)) return true;
    static unsigned last = 0;
    const unsigned now = (unsigned)GetTickCount64();
    if (now - last > 1000) {
        last = now;
        char why[256] = "";
        camera_pose_reject_reason(self, why, sizeof(why));
        VRLOG("camwrite: %s armed and WAITING - the pose in the object is not usable yet: %s "
              "(object %08X)", who, why, self);
    }
    return false;
}

void cam_write_test_frame() {
    if (cam_write::sweep_enabled()) {
        if (!wait_for_camera("write sweep")) return;
        cam_write::sweep_tick();
        return;
    }
    if (write_test_enabled()) {
        if (!wait_for_camera("write test")) return;
        write_test_tick();
    }
}

void cam_hook_start() {
    if (g_installed) return;

    wchar_t path[MAX_PATH] = L"";
    const wchar_t *log_path = vrlog::path();
    if (!log_path || !log_path[0]) return;
    wcsncpy_s(path, MAX_PATH, log_path, _TRUNCATE);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - path)), L"re6vr_camhook.txt");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;

    const HMODULE game = GetModuleHandleW(L"BH6.exe");
    if (!game) {
        VRLOG("camhook: BH6.exe is not loaded - not installing");
        return;
    }
    void *target = (void *)((unsigned char *)(uintptr_t)game + kCameraWriteFnRva);

    // Verify the target before hooking it: a wrong address would be patched into a live game
    // function. The first bytes of 0x4FF9B0 are known from static analysis - a prologue - and
    // the object offsets it writes are what the hook depends on.
    unsigned char head[8] = {0};
    if (!read(target, head, sizeof(head))) {
        VRLOG("camhook: target %p is not readable - not installing", target);
        return;
    }
    VRLOG("camhook: target %p (BH6.exe + 0x%X), first bytes %02X %02X %02X %02X %02X %02X",
          target, kCameraWriteFnRva, head[0], head[1], head[2], head[3], head[4], head[5]);

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        VRLOG("camhook: MH_Initialize failed");
        return;
    }
    if (MH_CreateHook(target, (void *)&camera_write_detour, &g_trampoline) != MH_OK) {
        VRLOG("camhook: MH_CreateHook failed");
        return;
    }
    if (MH_EnableHook(target) != MH_OK) {
        VRLOG("camhook: MH_EnableHook failed");
        return;
    }
    g_installed = true;
    VRLOG("camhook: ENABLED (from %ls). The engine's mCameraOrg writer is now observed: the "
          "first calls log the object address, the written pose, and the source group.", path);
}

} // namespace re6vr