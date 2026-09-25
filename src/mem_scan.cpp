// mem_scan.cpp - find where the engine keeps the camera, by looking for its own data.
//
// Why this exists
// ---------------
// Rotating matrices in the shader constant stream does not work on this engine, and the
// research says why: MT Framework has no engine-wide view-matrix constant slot (shaders
// are per-material), and the values in that stream that satisfy "orthonormal 3x3 with a
// translation" are as likely to be HUD placements or a combined view*projection. Measured
// on the real game: the only candidates that ever appeared were 4-register batches whose
// translations were (0,0,1)-scale - UI matrices - while the picture stretched, because a
// pure view matrix cannot stretch when rotated but those can.
//
// The way that does work, and what REFramework does, is to write the CAMERA: MT Framework's
// camera is a look-at camera (position / target / up + FOV), it holds no matrix, and the
// view matrix is rebuilt from those every frame by a virtual function. So the first
// question is not "which register" but "where is the camera".
//
// This scanner answers that without an offset table. It takes the view matrix the engine
// itself uploaded, twice, in two DIFFERENT camera poses, and searches process memory for
// the address that holds both. One pose alone would match thousands of addresses by
// coincidence - every copy of 1.0f and 0.0f matches it - so it is the second pose that
// turns a match into an answer.

#include "mem_scan.h"

#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

#include "log.h"
#include "safe_mem.h"

namespace re6vr {
namespace {

struct Capture {
    float m[16];
    unsigned long long tick;
};
Capture g_cap[2];
int g_cap_count = 0;

// How far the camera has to have travelled between the two captures before the scan is
// worth running. A parked camera's position matches hundreds of addresses by coincidence; a
// camera that has walked is a distinctive triple. 60 units is comfortably more than the
// jitter of a stationary camera (measured: 0.6 units over 3 s in a menu) while being a small
// fraction of a second's walk in game, which matters because this number is also what GATES
// pose 2 - see mem_scan_note_matrix.
const float kMinCameraTravel = 60.0f;
// Run the scan anyway after this long, so an unmet gate cannot swallow a whole run.
const unsigned long long kScanDeadlineMs = 90000;
// How long pose 1 waits for the camera to start MOVING before it gives up and re-arms.
//
// Why the capture is movement-triggered rather than timed: three runs in a row captured pose
// 2 exactly 3 s after pose 1 and landed in a menu or a loading screen both times, where the
// camera is parked - so the pair identified nothing and the run was wasted. Waiting for
// motion instead means the scan happens when the player is actually walking, without the
// player having to time anything.
const unsigned long long kArmTimeoutMs = 60000;
// How far the camera has to travel from pose 1 before pose 2 is taken.
const float kArmTravel = kMinCameraTravel;

// How far the head has turned AWAY FROM ITS STARTING POSE, which is the meaningful number.
//
// Measuring yaw against an absolute -Z reference assumes the headset starts level and
// facing forward. This one does not: it reported a neutral quaternion of (0, 0, 0, 1) and
// then immediately (0.097, -0.201, 0.015, 0.975), a pose tilted about 23 degrees before the
// player did anything. Against an absolute reference that baseline eats into every
// measurement - the player turns their head a long way and the number barely moves - so the
// reference is taken from the first frame instead, and what is reported is the change.
float g_head_fwd0[3] = {0.0f, 0.0f, -1.0f};
bool g_head_fwd0_valid = false;
float g_head_basis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
bool g_head_basis_valid = false;
float g_head_turn_deg = 0.0f;
const float kHeadTurnForBasisSearch = 20.0f;     // degrees of yaw before the search arms

bool g_enabled = false;
bool g_running = false;
bool g_done = false;
HANDLE g_thread = nullptr;

// Scale-aware tolerance. An absolute epsilon is wrong here: 1200.0 differs in its low bits
// while 0.0 differs by nothing, so a fixed 1e-6 either rejects real matches or accepts
// noise. This is roughly 100 ULP, tight enough that a coincidence across two poses is very
// unlikely, loose enough to survive the engine's own float arithmetic.
//
// The non-finite guard is not defensive padding - it fixes a real false-positive source.
// close_enough(1920.0, INFINITY) computed diff = inf and scale = inf, so the test
// "diff <= 1e-5 + 1e-5 * scale" became "inf <= inf", which is TRUE in IEEE arithmetic.
// Every uninitialised region holding infinities therefore matched any value, and the
// candidate dumps filled up with them.
bool close_enough(float a, float b) {
    if (!(a == a) || !(b == b)) return false;                    // NaN
    const float mag = fmaxf(fabsf(a), fabsf(b));
    if (mag > 1.0e7f) return false;      // never a matrix element, and breaks the form above
    const float diff = fabsf(a - b);
    return diff <= 1e-5f + 1e-5f * mag;
}

bool matches(const float *cand, const float *target) {
    for (int i = 0; i < 16; ++i) {
        if (!close_enough(cand[i], target[i])) return false;
    }
    return true;
}

bool readable(const void *addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    return (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
}

// A view matrix V = [ R | t ] maps world to view, so for a camera at world position p the
// translation row is t = -R*p, and therefore p = -R^T * t. Recovering p matters because p is
// a real stored FIELD of the camera, while t is a derived value that only ever exists inside
// the matrix - searching for p finds the parameter; searching for t finds the matrix.
//
// R is the rows of the matrix (row-major, the layout the engine uploads), so R^T * t means
// dotting t with each row.
void world_pos_from_view(const float *view, float *out3) {
    const float tx = view[12], ty = view[13], tz = view[14];
    out3[0] = -(view[0] * tx + view[1] * ty + view[2] * tz);
    out3[1] = -(view[4] * tx + view[5] * ty + view[6] * tz);
    out3[2] = -(view[8] * tx + view[9] * ty + view[10] * tz);
}

// Safe read of 64 bytes: validate the page first (src/safe_mem.h) instead of relying on
// __try/__except, which this project measured FAULTING rather than catching (see safe_mem.h).
bool matches_safe(const float *cand, const float *target) {
    if (!re6vr::readable(cand, 16 * sizeof(float))) return false;
    for (int i = 0; i < 16; ++i) {
        if (!close_enough(cand[i], target[i])) return false;
    }
    return true;
}

// Fallback for when the full matrix is not stored anywhere - the normal case for MT
// Framework, which keeps a look-at camera (position / up / target + FOV) and rebuilds the
// view matrix every frame from those, so the matrix itself has no lasting home. The
// camera's POSITION, on the other hand, is a real field with a real home.
//
// Two spaces are searched because either one may be what the engine stores: the camera's
// WORLD position (what a look-at camera keeps), and the view-space translation row (what a
// matrix-based camera would keep). Both are derived from the captured poses rather than
// guessed, and the caller reports which one matched.
struct PositionSearch {
    float in[3];
    float out[3];
    const char *space;
};

// Defined below; declared here so the search can use them without reordering the file.
bool triple_matches_safe(const float *f, float a, float b, float c);
bool position_pair_matches(const float *f, const float *pos_in, const float *pos_out);
// Reads a candidate address as an `sBioCamera::ViewportCamera` entry and reports whether it
// really looks like one. Declared here for the same reason as the two above.
void check_camera_org(void *addr, const float *cols, const float *pos);

void collect_position(const PositionSearch &ps, std::vector<void *> *out) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    unsigned char *p = (unsigned char *)si.lpMinimumApplicationAddress;
    unsigned char *end = (unsigned char *)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned long long examined = 0;
    unsigned long long regions = 0;

    while (p < end) {
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) break;
        const bool usable = mbi.State == MEM_COMMIT &&
                            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                            PAGE_WRITECOPY)) &&
                            !(mbi.Protect & PAGE_GUARD);
        if (usable) {
            ++regions;
            unsigned char *base = (unsigned char *)mbi.BaseAddress;
            const size_t size = mbi.RegionSize;
            examined += size;
            // Step by 4 bytes: a position is not guaranteed to be 16-byte aligned, and a
            // missed alignment is a missed camera.
            for (size_t off = 0; off + 32 <= size; off += 4) {
                const float *f = (const float *)(base + off);
                // Filter on the first capture cheaply, then confirm against the second.
                if (!triple_matches_safe(f, ps.in[0], ps.in[1], ps.in[2])) continue;
                if (!position_pair_matches(f, ps.in, ps.out)) continue;
                if (out->size() < 200000) out->push_back((void *)f);
            }
        }
        p = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    VRLOG("scan: %s search examined %llu MB in %llu region(s), collected %u address(es)",
          ps.space, examined / (1024ull * 1024ull), regions, (unsigned)out->size());
}

// Does this address hold the given three floats, in either of the layouts a vector is
// written in? Kept object-free because __try cannot coexist with stack unwinding (error
// C2712), so the loop that walks the candidates stays outside.
bool triple_matches_safe(const float *f, float a, float b, float c) {
    bool ok = false;
    __try {
        ok = (close_enough(f[0], a) && close_enough(f[1], b) && close_enough(f[2], c)) ||
             (close_enough(f[0], a) && close_enough(f[2], b) && close_enough(f[4], c));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// pos_in and pos_out are two camera positions in the SAME space (world or view). A field
// that holds the camera's position holds both - one per capture - and that is what makes
// the hit trustworthy: a single triple matches any coincidental equal values.
bool position_pair_matches(const float *f, const float *pos_in, const float *pos_out) {
    bool ok = false;
    __try {
        // Layout A: packed. Layout B: each component padded to 16 bytes (a Vector4 field).
        ok = (close_enough(f[0], pos_in[0]) && close_enough(f[1], pos_in[1]) &&
              close_enough(f[2], pos_in[2]) &&
              close_enough(f[0], pos_out[0]) && close_enough(f[1], pos_out[1]) &&
              close_enough(f[2], pos_out[2])) ||
             (close_enough(f[0], pos_in[0]) && close_enough(f[2], pos_in[1]) &&
              close_enough(f[4], pos_in[2]) &&
              close_enough(f[0], pos_out[0]) && close_enough(f[2], pos_out[1]) &&
              close_enough(f[4], pos_out[2]));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// Safety valve for the basis search: one entry per 4-byte offset in a page whose first
// float already matched, capped so a pathological page cannot exhaust memory.
struct BasisHit {
    void *addr;
    int layout;      // 0 = packed 3x3, 1 = rows padded to 16 bytes
};

// Search for the head's basis as a contiguous 3x3. The camera's own basis is the head
// basis composed with the game camera's rotation, so this finds the camera's orientation
// storage only when the player has turned their head enough for the head to dominate -
// which is exactly why note_head only arms the search past a large angle.
void collect_head_basis(const float *b, std::vector<BasisHit> *out) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    unsigned char *p = (unsigned char *)si.lpMinimumApplicationAddress;
    unsigned char *end = (unsigned char *)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned long long examined = 0;
    int hits = 0;

    while (p < end) {
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) break;
        const bool usable = mbi.State == MEM_COMMIT &&
                            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                            PAGE_WRITECOPY)) &&
                            !(mbi.Protect & PAGE_GUARD);
        if (usable) {
            unsigned char *base = (unsigned char *)mbi.BaseAddress;
            const size_t size = mbi.RegionSize;
            examined += size;
            for (size_t off = 0; off + 48 <= size; off += 4) {
                const float *f = (const float *)(base + off);
                // Cheap rejection on the first element before touching the whole block.
                if (!close_enough(f[0], b[0])) continue;
                bool ok = false;
                int layout = 0;
                __try {
                    ok = close_enough(f[1], b[1]) && close_enough(f[2], b[2]) &&
                         close_enough(f[4], b[3]) && close_enough(f[5], b[4]) &&
                         close_enough(f[6], b[5]) && close_enough(f[8], b[6]) &&
                         close_enough(f[9], b[7]) && close_enough(f[10], b[8]);
                    layout = 0;
                    if (!ok) {
                        // Rows padded to 16 bytes, i.e. a Vector4 per row: the layout a
                        // MtMatrix-like or Vector4-based basis uses.
                        ok = close_enough(f[4], b[1]) && close_enough(f[8], b[2]) &&
                             close_enough(f[16], b[3]) && close_enough(f[20], b[4]) &&
                             close_enough(f[24], b[5]) && close_enough(f[28], b[6]) &&
                             close_enough(f[32], b[7]) && close_enough(f[36], b[8]);
                        layout = 1;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    ok = false;
                }
                if (ok) {
                    if (out->size() < 5000) out->push_back({(void *)f, layout});
                    ++hits;
                }
            }
        }
        p = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    VRLOG("scan: head-basis search examined %llu MB, %d match(es)",
          examined / (1024ull * 1024ull), hits);
}

// Pass 1: collect every address that holds this pose. Collecting rather than reporting is
// what makes the second pass meaningful - the answer is the INTERSECTION of the two poses,
// and printing pass 1 alone would list thousands of coincidences.
void collect_matches(const float *target, std::vector<void *> *out) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    unsigned char *p = (unsigned char *)si.lpMinimumApplicationAddress;
    unsigned char *end = (unsigned char *)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned long long examined = 0;

    while (p < end) {
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) break;
        const bool usable = mbi.State == MEM_COMMIT &&
                            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                            PAGE_WRITECOPY)) &&
                            !(mbi.Protect & PAGE_GUARD);
        if (usable && out->size() < 200000) {
            unsigned char *base = (unsigned char *)mbi.BaseAddress;
            const size_t size = mbi.RegionSize;
            examined += size;
            // 16-byte aligned: a matrix is never at an odd offset, and this quarters the
            // number of comparisons.
            for (size_t off = 0; off + 64 <= size; off += 16) {
                const float *cand = (const float *)(base + off);
                if (matches_safe(cand, target)) out->push_back((void *)cand);
            }
        }
        p = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    VRLOG("scan: pass 1 examined %llu MB and collected %u address(es) holding pose 1",
          examined / (1024ull * 1024ull), (unsigned)out->size());
}

// A camera's parameters live near its transform, and they are recognisable: a field of view
// in radians, an aspect ratio, near and far clip planes in metres. Printing the plausible
// ones turns "here is an address" into "here is the camera's struct", which is what the
// next step (editing position/target/up) needs.
//
// The window is deliberately wide and printed as labelled rows, because the useful answer
// is "which neighbouring field is the target, and which is up" - and that is far easier to
// read off a dump than to extract one guessed scalar at a time. Any 3x3 that behaves like a
// rotation is called out, since the camera's basis is stored somewhere near its position.
void describe_context(void *addr, const float *pose) {
    const float *base = (const float *)addr;
    float world_pos[3] = {0.0f, 0.0f, 0.0f};
    if (pose) world_pos_from_view(pose, world_pos);

    // Rows of four, from 80 floats before to 80 after, so the layout is visible.
    for (int row = -20; row < 20; ++row) {
        const float *f = base + row * 4;
        if (!readable(f) || !readable(f + 3)) continue;
        float v0 = 0, v1 = 0, v2 = 0, v3 = 0;
        __try {
            v0 = f[0]; v1 = f[1]; v2 = f[2]; v3 = f[3];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        // Does this row start a rotation? Three rows of unit length that are mutually
        // perpendicular is what a camera basis looks like, and it is worth flagging because
        // if the basis is here then the position and target are within a few rows of it.
        const bool row_is_unit = fabsf(sqrtf(v0 * v0 + v1 * v1 + v2 * v2) - 1.0f) < 1e-3f;
        char tag[96] = "";
        if (row_is_unit && readable(f + 11)) {
            bool ortho = false;
            __try {
                const float *r1 = f + 4, *r2 = f + 8;
                const float d01 = v0 * r1[0] + v1 * r1[1] + v2 * r1[2];
                const float d02 = v0 * r2[0] + v1 * r2[1] + v2 * r2[2];
                const float d12 = r1[0] * r2[0] + r1[1] * r2[1] + r1[2] * r2[2];
                const float n1 = sqrtf(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
                const float n2 = sqrtf(r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2]);
                ortho = fabsf(d01) < 1e-3f && fabsf(d02) < 1e-3f && fabsf(d12) < 1e-3f &&
                        fabsf(n1 - 1.0f) < 1e-3f && fabsf(n2 - 1.0f) < 1e-3f;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ortho = false;
            }
            if (ortho) _snprintf_s(tag, sizeof(tag), _TRUNCATE, "   <-- ORTHONORMAL 3x3 STARTS HERE");
        }
        // Is this the camera's world position? Computed from the pose rather than guessed.
        char wtag[96] = "";
        if (close_enough(v0, world_pos[0]) && close_enough(v1, world_pos[1]) &&
            close_enough(v2, world_pos[2])) {
            _snprintf_s(wtag, sizeof(wtag), _TRUNCATE, "   <-- camera WORLD POSITION");
        }
        VRLOG("scan:   [%+3d] %.4f %.4f %.4f %.4f%s%s", row * 4, v0, v1, v2, v3, tag, wtag);
    }
}

// Does this address look like the `mCameraOrg[i]` entry of `sBioCamera::ViewportCamera`?
//
// This is the check that turns "an address holding the camera's position" into "the camera
// object", and it exists because the offset table is available after all. `BH6.exe` ships
// MT Framework's DTI property tables as ordinary code: each field is registered with
// `C7 44 24 10 <name>` / `C7 44 24 14 <type>` preceded by `lea reg,[ebx+disp32]`, and the
// class name is pushed once per class at its registration site. Reading them out gives, for
// `sBioCamera::ViewportCamera`:
//
//     mCameraOrg[i], stride 0x40, i = 0..7, base + 0xE30
//       +0x00 cameraPos   +0x10 targetPos   +0x20 cameraUp
//       +0x30 fov         +0x34 nearPlane   +0x38 farPlane
//
// The verification is deliberately independent of that table: a look-at camera's three
// vectors are geometrically constrained (up is unit, forward = target - position is unit and
// perpendicular to up), and the position must equal the one derived from the view matrix the
// engine itself uploaded. A coincidence cannot satisfy all of that, so a hit here is an
// answer rather than another candidate.
//
// Kept object-free because __try cannot coexist with stack unwinding (C2712).
void check_camera_org(void *addr, const float *cols, const float *pos) {
    float v[12] = {0};
    __try {
        const float *f = (const float *)addr;
        for (int i = 0; i < 12; ++i) v[i] = f[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    const float *p = v + 0, *t = v + 4, *u = v + 8;
    const float fov = ((const float *)addr)[12];
    const float flen = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);

    VRLOG("scan:     as mCameraOrg[i]: pos (%.2f %.2f %.2f)  target (%.2f %.2f %.2f)  "
          "up (%.3f %.3f %.3f)  fov %.4f rad (%.1f deg)",
          p[0], p[1], p[2], t[0], t[1], t[2], u[0], u[1], u[2], fov, fov * 57.2957795f);

    const bool up_unit = fabsf(sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) - 1.0f) < 1e-3f;
    const bool tgt_gap = flen > 1.0f && flen < 100000.0f;
    const bool fov_ok = fov > 0.05f && fov < 2.2f;
    const bool pos_ok = close_enough(p[0], pos[0]) && close_enough(p[1], pos[1]) &&
                        close_enough(p[2], pos[2]);
    bool perp = false, fwd_unit = false;
    if (tgt_gap && fov_ok) {
        const float fx = t[0] / flen, fy = t[1] / flen, fz = t[2] / flen;
        const float dot = fx * u[0] + fy * u[1] + fz * u[2];
        perp = fabsf(dot) < 0.15f;
        // The camera's right = up x forward, and the view matrix's first ROW (its columns
        // are the right/up/backward axes, so column 0 is the first component of each row).
        const float rx = u[1] * fz - u[2] * fy;
        const float ry = u[2] * fx - u[0] * fz;
        const float rz = u[0] * fy - u[1] * fx;
        fwd_unit = fabsf(fabsf(rx - cols[0]) + fabsf(ry - cols[4]) + fabsf(rz - cols[8])) < 0.15f;
    }

    const bool layout_ok = up_unit && tgt_gap && fov_ok && perp && pos_ok && fwd_unit;
    if (layout_ok) {
        VRLOG("scan: *** CAMERA ORG ENTRY CONFIRMED at %p *** pos matches the view matrix "
              "(-R^T*t), up is unit, up . forward = %.3f, right matches the view row, and "
              "fov = %.1f deg. This IS the camera object; targetPos/cameraUp are the fields "
              "to write for head look, and the next entry is +0x40 away.",
              addr, fwd_unit ? 1.0f : 0.0f, fov * 57.2957795f);
        return;
    }

    // A near miss is still worth one line: it says which check failed, which is what decides
    // whether the offsets are wrong or the candidate is simply something else.
    char why[160] = "";
    if (!up_unit) _snprintf_s(why + strlen(why), sizeof(why) - strlen(why), _TRUNCATE, " up!unit");
    if (!tgt_gap) _snprintf_s(why + strlen(why), sizeof(why) - strlen(why), _TRUNCATE, " target-bad");
    if (!fov_ok)  _snprintf_s(why + strlen(why), sizeof(why) - strlen(why), _TRUNCATE, " fov=%.3f", fov);
    if (!perp)    _snprintf_s(why + strlen(why), sizeof(why) - strlen(why), _TRUNCATE, " up.forward!=0");
    if (!fwd_unit) _snprintf_s(why + strlen(why), sizeof(why) - strlen(why), _TRUNCATE, " right!=view-row");
    if (!pos_ok)  _snprintf_s(why + strlen(why), sizeof(why) - strlen(why), _TRUNCATE, " pos!=view");
    VRLOG("scan:     camera-org check at %p: not a match (%s)", addr, why);
}

// Does this 4x4 look like a projection matrix (rather than a view matrix or garbage)?
//
// This is the discriminator for the viewport signature below: put a VIEW matrix at +0xB0 and a
// PROJECTION matrix at +0xF0 and the constraint "two well-formed matrices exactly 0x40 apart"
// becomes very hard to satisfy by accident, without knowing a single address. (The offset
// layout is not guessed - it is what dmc4_hook documents for the same engine family:
// `sCamera_ViewPort` holds `mViewMat` +0xB0 and `mProjMat` +0xF0, stride 0x590.)
//
// A projection: bottom row has no translation, and the position transform carries the
// perspective divide (m[11] non-zero with a zero m[15]); a view matrix has the opposite shape,
// an affine basis plus a translation row. Kept object-free for the __try rule.
bool looks_projection(const float *m) {
    if (m[12] != 0.0f || m[13] != 0.0f || m[14] != 0.0f) return false;
    if (fabsf(m[15]) > 1e-4f) return false;
    if (fabsf(m[11]) < 1e-6f) return false;
    float maxv = 0.0f;
    for (int i = 0; i < 16; ++i) {
        const float a = fabsf(m[i]);
        if (!(a < 1e6f)) return false;              // covers NaN as well
        if (a > maxv) maxv = a;
    }
    return maxv > 1e-3f;
}

// Is the 4x4 at `m` a view matrix, judged only by "it is affine with a real basis"? The
// translation may legitimately be zero at the origin, so no magnitude test is applied to it.
bool looks_view_affine(const float *m) {
    for (int i = 0; i < 16; ++i) {
        if (!(fabsf(m[i]) < 1e6f)) return false;
    }
    if (fabsf(m[15] - 1.0f) > 1e-3f) return false;
    const float len0 = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const float len1 = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    const float len2 = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    return len0 > 1e-4f && len1 > 1e-4f && len2 > 1e-4f;
}

// Scan for the renderer's camera viewport.
//
// The layout comes from dmc4_hook, a 32-bit DX9 MT Framework project of the same engine family:
// `sCamera_ViewPort` holds `mViewMat` at +0xB0 and `mProjMat` at +0xF0, 0x40 apart, with a
// back-pointer `mpCamera` at +0x04 and a struct stride of 0x590.
//
// It is NOT assumed to hold in RE6. This project has already lost five runs to one wrong
// assumption, and RE6's own reflection strings are missing `mProjMat`, `mPrevViewMat` and
// `mFrustum`, which the sibling title has - so the shape differs at least a little. Therefore
// every plausible variation is tried in the SAME pass instead of one per run:
//
//   * the projection may be before or after the view matrix (D3D row-vector convention can flip
//     the struct order), so both +0x40 and -0x40 are accepted;
//   * the pair may sit at a different offset inside the viewport, so a hit is reported with the
//     offset it was found at;
//   * a candidate with a PROJECTION but no room for the camera pointer is still reported,
//     because the offset it lands on is information.
//
// The constraint that does the work is "a view-shaped 4x4 with a projection-shaped 4x4 exactly
// 0x40 away": it needs no address, no captured value, and cannot be satisfied by accident often.
struct ViewportHit {
    void *view;            // address of the view-shaped matrix
    void *proj;            // address of the projection-shaped matrix
    int   view_off;        // offset of the view matrix from the candidate base
    int   proj_off;        // offset of the projection matrix from the candidate base
    void *camera;          // *(void**)(base + 0x04), may be null
    float m11;
};

// The offsets that were tried for the view matrix (0xB0 is the documented one).
//
// They are no longer needed: the signature is the 0x40 GAP between the two matrices, which is
// invariant to where inside the viewport they sit, so the search pivots on the projection and
// probes 0x40 either side of it - one shape test per surviving address instead of twenty. Kept
// as a comment because the offsets are what the log prints once a hit is found, and comparing
// that to +0xB0 is how RE6's layout gets confirmed or refuted.
//   {0xB0, 0xA0, 0xC0, 0x90, 0xD0, 0x80, 0xE0, 0x100, 0x70, 0x110}

bool candidate_at(unsigned char *base, size_t off, int view_off, int proj_off, ViewportHit *out) {
    const float *view = (const float *)(base + off + view_off);
    const float *proj = (const float *)(base + off + proj_off);
    bool ok = false;
    __try {
        ok = looks_view_affine(view) && looks_projection(proj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) return false;
    out->view = (void *)view;
    out->proj = (void *)proj;
    out->view_off = view_off;
    out->proj_off = proj_off;
    out->camera = nullptr;
    out->m11 = 0.0f;
    __try {
        out->m11 = proj[11];
        out->camera = *(void **)(base + off + 0x04);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->camera = nullptr;
    }
    return true;
}

void collect_viewports(std::vector<ViewportHit> *out) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    unsigned char *p = (unsigned char *)si.lpMinimumApplicationAddress;
    unsigned char *end = (unsigned char *)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned long long examined = 0;
    const int kDelta = 0x40;

    while (p < end) {
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) break;
        const bool usable = mbi.State == MEM_COMMIT && mbi.Type != MEM_IMAGE &&
                            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) &&
                            !(mbi.Protect & PAGE_GUARD);
        if (usable) {
            unsigned char *base = (unsigned char *)mbi.BaseAddress;
            const size_t size = mbi.RegionSize;
            examined += size;

            // The loop is pivoted on the PROJECTION matrix, not on the candidate base.
            //
            // The first version probed all ten view offsets (x2 orders) at every 4-byte address,
            // i.e. twenty candidate checks per address, and the run of 2026-09-24 00:14 never
            // finished the walk before the player quit. A projection matrix has a very rare
            // property - m[11] (the perspective term) is non-zero while m[12..15] are zero - so
            // screening on that one float first throws away practically every address for the
            // price of one load. Only when it hits are the view offsets around it probed.
            const size_t first = 0x10;            // leave room for the -0x40 order
            for (size_t off = first; off + 0x200 <= size; off += 4) {
                const float *pp = (const float *)(base + off);
                float m11 = 0.0f, m12 = 1.0f, m13 = 1.0f, m14 = 1.0f, m15 = 1.0f;
                __try {
                    m11 = pp[11]; m12 = pp[12]; m13 = pp[13]; m14 = pp[14]; m15 = pp[15];
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    continue;
                }
                if (!(fabsf(m11) > 1e-6f)) continue;
                if (!(m12 == 0.0f && m13 == 0.0f && m14 == 0.0f && fabsf(m15) <= 1e-4f)) continue;

                // A perspective term, with an affine bottom row. The view-shaped matrix of the
                // pair is 0x40 either side of it; try both orders. This is the whole signature -
                // "these two shapes exactly 0x40 apart" - and it needs no address, no stride and
                // no captured value.
                ViewportHit h;
                if (off >= (size_t)kDelta && candidate_at(base, off - kDelta, 0, kDelta, &h)) {
                    if (out->size() < 128) out->push_back(h);
                    continue;
                }
                if (candidate_at(base, off, -kDelta, 0, &h)) {
                    if (out->size() < 128) out->push_back(h);
                }
            }
        }
        p = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    VRLOG("scan: viewport signature search examined %llu MB, found %u candidate(s) "
          "(a view-shaped 4x4 exactly 0x40 from a projection-shaped one, both orders, pivoted on "
          "the projection's m[11] so the walk costs one load per address)",
          examined / (1024ull * 1024ull), (unsigned)out->size());
}

// Does the view matrix at this candidate carry the captured translation? Kept object-free: the
// caller holds std::vector<ViewportHit>, and __try cannot coexist with anything that needs stack
// unwinding (error C2712, which this file has already been taught once).
bool view_translation_matches(volatile const float *m, float a, float b, float c) {
    bool ok = false;
    __try {
        ok = close_enough(m[12], a) && close_enough(m[13], b) && close_enough(m[14], c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

DWORD WINAPI scan_thread(LPVOID) {
    VRLOG("scan: thread started. The scan needs the player to be MOVING, so it waits for the "
          "camera to actually travel and for the head to turn - a scan taken while the camera "
          "is parked identifies nothing, because a stationary position matches a thousand "
          "addresses by coincidence and the head basis is not distinguishable from the game "
          "camera's own rotation.");

    // Wait until the run is worth measuring. The first version scanned as soon as two
    // matrices had been seen, which happens seconds after launch: both poses were the
    // parked camera (2.4 units apart), and it produced 17 "candidates" that were nothing
    // but coincidences.
    //
    // There is a deadline as well as a condition. If the gate never opens - the camera
    // register was mis-identified, or the player stood still - the scan must still run and
    // say so, because a run that silently produces nothing is the failure mode this whole
    // session has been fighting.
    const unsigned long long wait_start = GetTickCount64();
    bool poses_ok = false, head_ok = false;
    while (g_running) {
        if (g_cap_count >= 2) {
            const float dx = g_cap[1].m[12] - g_cap[0].m[12];
            const float dy = g_cap[1].m[13] - g_cap[0].m[13];
            const float dz = g_cap[1].m[14] - g_cap[0].m[14];
            const float moved = sqrtf(dx * dx + dy * dy + dz * dz);
            poses_ok = moved >= kMinCameraTravel;
            head_ok = g_head_turn_deg >= kHeadTurnForBasisSearch;
            if (poses_ok && head_ok) break;          // everything the scan can use
        }
        if (GetTickCount64() - wait_start > kScanDeadlineMs) break;
        Sleep(200);
    }
    if (!g_running) return 0;

    const float dx = g_cap[1].m[12] - g_cap[0].m[12];
    const float dy = g_cap[1].m[13] - g_cap[0].m[13];
    const float dz = g_cap[1].m[14] - g_cap[0].m[14];
    const float moved = sqrtf(dx * dx + dy * dy + dz * dz);

    // Two requirements, two different consumers, so they are gated SEPARATELY.
    //
    // This used to be one gate that refused everything unless both were met, and the run of
    // 2026-09-24 00:09 paid for it: the camera travelled 187 units (fine) while the head turned
    // 2 degrees, so the scan refused - and the viewport signature search, which needs no head
    // turn at all and was the entire point of that run, never ran. A gate must not be stricter
    // than the thing behind it.
    //
    //   * the two poses   -> needed by the matrix/position/basis search;
    //   * the head turn   -> needed ONLY by the head-basis search.
    if (!poses_ok) {
        // Running the value searches on two copies of one parked pose cannot produce an answer,
        // so they are skipped - but ONLY they. The viewport signature search does not compare
        // poses, so it still runs below.
        VRLOG("scan: skipping the pose-dependent searches (matrix / world position / view "
              "translation) - they need two DIFFERENT poses and the camera only moved %.1f units "
              "of the %.0f required in %llu s. The viewport signature search does not compare "
              "poses and runs anyway.", moved, kMinCameraTravel, kScanDeadlineMs / 1000ull);
        const bool paused = moved < kMinCameraTravel * 0.01f;
        if (paused) {
            VRLOG("scan: the camera barely moved at all, which is what a PAUSED or unfocused "
                  "game looks like. Load a save and start WALKING.");
        } else {
            VRLOG("scan: the camera did move, but not far enough. Keep walking for longer.");
        }
    }
    if (!head_ok) {
        VRLOG("scan: the head only turned %.0f deg of the %.0f required, so the head-BASIS "
              "search is skipped (it is the only consumer of the head pose). Turn the head as "
              "well next time to enable it.", g_head_turn_deg, kHeadTurnForBasisSearch);
    }

    VRLOG("scan: pose 1 (%.1f %.1f %.1f), pose 2 (%.1f %.1f %.1f), camera moved %.1f units, "
          "head turned %.0f deg%s",
          g_cap[0].m[12], g_cap[0].m[13], g_cap[0].m[14],
          g_cap[1].m[12], g_cap[1].m[13], g_cap[1].m[14], moved, g_head_turn_deg,
          (poses_ok && head_ok) ? " - both gates met, running everything"
                                : " - running what these poses and this head pose permit");

    // FIRST: the renderer's camera viewport, by shape rather than by value.
    //
    // This runs before anything that searches for a captured matrix value, because the research
    // on this engine family says there is no camera matrix to find in the camera object (it is a
    // look-at triple) and the authoritative view matrix sits behind a pointer chain in the
    // renderer's viewport struct. A signature that needs no address and no captured value cannot
    // suffer the false negative that five earlier runs ended on.
    {
        std::vector<ViewportHit> hits;
        collect_viewports(&hits);
        if (!hits.empty()) {
            const int show = (int)hits.size() < 8 ? (int)hits.size() : 8;
            VRLOG("scan: --- %u viewport candidate(s); the first %d follow. `view` is the "
                  "view-shaped 4x4, `proj` the projection-shaped one 0x40 away, and `camera` is "
                  "*(base+0x04) (mpCamera):",
                  (unsigned)hits.size(), show);
            for (int i = 0; i < show; ++i) {
                const float *v = (const float *)hits[i].view;
                const bool matched = view_translation_matches(v, g_cap[0].m[12], g_cap[0].m[13],
                                                              g_cap[0].m[14]);
                VRLOG("scan: --- viewport %d: view@%p (off +0x%X) proj@%p (off +0x%X) "
                      "mProjMat m11=%.4f camera=%p | view translation (%.2f %.2f %.2f)%s",
                      i + 1, hits[i].view, hits[i].view_off, hits[i].proj, hits[i].proj_off,
                      hits[i].m11, hits[i].camera, v[12], v[13], v[14],
                      matched ? "   <-- THE CAPTURED CAMERA (translation matches)" : "");
            }
            VRLOG("scan: the offsets are as important as the find: if a candidate's view off is "
                  "not +0xB0, RE6 puts mViewMat somewhere else than the sibling title does, and "
                  "mpCamera is then NOT at base+0x04 - read the struct around the reported "
                  "address before trusting it. For the winner, walk its camera pointer and read "
                  "the look-at triple (mCameraPos +0x30, mCameraUp +0x40, mTargetPos +0x50 for "
                  "uCamera on this engine family): head tracking writes those three vectors, not "
                  "a matrix.");
        } else {
            VRLOG("scan: no viewport-shaped struct found. The layout may differ in RE6 from the "
                  "sibling title this signature came from; the position search below still runs.");
        }
    }

    // DECISIVE CHECK: does the engine keep the view matrix we captured AT ALL?
    //
    // Everything so far has searched for a derived value (the world position p = -R^T*t) or for a
    // shape (two matrices 0x40 apart). Both can miss. This searches for the exact three floats
    // the engine itself uploaded as the view matrix's translation row, in BOTH captured poses -
    // values the plugin observed rather than inferred, and specific enough that a hit means
    // something (they are large, non-round world coordinates).
    //
    // The outcome is read the same either way:
    //   * addresses found  -> the stored view matrix is reachable in memory, and its address is
    //     the pointer to walk to the camera;
    //   * none found       -> the view matrix exists only transiently (rebuilt per frame and
    //     discarded), and NO amount of memory scanning will find the camera. That closes the
    //     value-scan family for good and makes the remaining options explicit: a data breakpoint
    //     in a debugger, or a real disassembler on the camera update path.
    {
        PositionSearch tr;
        tr.space = "view translation row (exact, both poses)";
        tr.in[0] = g_cap[0].m[12];
        tr.in[1] = g_cap[0].m[13];
        tr.in[2] = g_cap[0].m[14];
        tr.out[0] = g_cap[1].m[12];
        tr.out[1] = g_cap[1].m[13];
        tr.out[2] = g_cap[1].m[14];
        std::vector<void *> hits;
        collect_position(tr, &hits);
        if (hits.empty()) {
            VRLOG("scan: NOTHING holds the captured view matrix's translation row (%.1f %.1f %.1f) "
                  "-> (%.1f %.1f %.1f). The matrix is rebuilt and discarded every frame, so no "
                  "memory scan can reach the camera by value. Stop scanning; the remaining routes "
                  "are a data breakpoint on a suspected camera field, or disassembling the camera "
                  "update path.",
                  tr.in[0], tr.in[1], tr.in[2], tr.out[0], tr.out[1], tr.out[2]);
        } else {
            const int show = (int)hits.size() < 6 ? (int)hits.size() : 6;
            VRLOG("scan: %u address(es) hold the captured view translation in BOTH poses - the "
                  "stored view matrix is real and reachable:", (unsigned)hits.size());
            for (int i = 0; i < show; ++i) {
                VRLOG("scan: --- view-matrix translation %d of %u at %p ---", i + 1,
                      (unsigned)hits.size(), hits[i]);
                describe_context(hits[i], g_cap[0].m);
            }
        }
    }

    std::vector<void *> first;
    collect_matches(g_cap[0].m, &first);
    bool use_position = first.empty();

    // FIRST, and ahead of the matrix work: search for the head basis. The head pose is a
    // value the plugin knows rather than infers, so the camera's orientation is the one
    // thing here that can be searched for exactly instead of by heuristic. It is also the
    // field that matters, because turning the camera means changing its orientation.
    if (g_head_basis_valid && g_head_turn_deg >= kHeadTurnForBasisSearch) {
        VRLOG("scan: the head is turned %.0f deg, so searching memory for its basis - the "
              "camera's own basis is the head basis composed with the game camera's "
              "rotation, and at this angle the head dominates it", g_head_turn_deg);
        std::vector<BasisHit> hits;
        collect_head_basis(g_head_basis, &hits);
        if (!hits.empty()) {
            const int show = (int)hits.size() < 6 ? (int)hits.size() : 6;
            VRLOG("scan: %u address(es) hold the head basis. Layout 0 = packed 3x3, "
                  "layout 1 = rows padded to 16 bytes:", (unsigned)hits.size());
            for (int i = 0; i < show; ++i) {
                VRLOG("scan: --- basis candidate %d of %u at %p (layout %d) ---", i + 1,
                      (unsigned)hits.size(), hits[i].addr, hits[i].layout);
                describe_context(hits[i].addr, g_cap[0].m);
            }
        } else {
            VRLOG("scan: no address holds the head basis as a 3x3. Either the camera stores "
                  "its orientation composed with the game camera's rotation (so the two never "
                  "coincide), or it is stored as a quaternion, or the engine composes it "
                  "fresh each frame without keeping it.");
        }
    } else if (g_head_basis_valid) {
        VRLOG("scan: head turned only %.0f deg - turn further (over %.0f) for the basis "
              "search, because at a small angle the camera's basis is dominated by the game "
              "camera's rotation rather than the head's", g_head_turn_deg,
              kHeadTurnForBasisSearch);
    }

    if (!use_position) {
        // Intersection: of everything that matched pose 1, keep only what also matches
        // pose 2. An address that held pose 1 but not pose 2 is not the camera's storage -
        // it is somewhere the matrix passes through or gets overwritten, and reporting it
        // would send the next step looking at the wrong struct.
        int survivors = 0;
        for (size_t i = 0; i < first.size(); ++i) {
            const float *cand = (const float *)first[i];
            if (!matches_safe(cand, g_cap[1].m)) continue;
            ++survivors;
            if (survivors <= 20) describe_context(first[i], g_cap[0].m);
        }
        VRLOG("scan: %u address(es) held pose 1, %d of them also held pose 2",
              (unsigned)first.size(), survivors);
        if (survivors == 0) {
            // The matrix IS stored - something held pose 1 - but nothing holds both, which
            // means the storage is rewritten in place as the camera moves. That is still a
            // perfectly good place to write a modified matrix, but it is worth also having
            // the camera's parameters, so fall through to the position search as well.
            VRLOG("scan: the matrix is stored but rewritten in place (nothing held both "
                  "poses). Also locating the camera's position, which is the field to edit:");
            use_position = true;
        }
    } else {
        VRLOG("scan: no address holds the whole matrix, so it is rebuilt each frame rather "
              "than stored - which is what a look-at camera does. Looking for the camera's "
              "POSITION instead:");
    }
    if (use_position) {
        // Try the camera's WORLD position first: that is what a look-at camera stores, and
        // it is derived from the pose rather than guessed. Its components are large and
        // distinctive (hundreds to thousands of units), so a hit is meaningful.
        float w0[3], w1[3];
        world_pos_from_view(g_cap[0].m, w0);
        world_pos_from_view(g_cap[1].m, w1);
        VRLOG("scan: the camera's world position was (%.2f %.2f %.2f), then "
              "(%.2f %.2f %.2f)", w0[0], w0[1], w0[2], w1[0], w1[1], w1[2]);

        PositionSearch searches[2];
        searches[0].space = "world position";
        memcpy(searches[0].in, w0, sizeof(w0));
        memcpy(searches[0].out, w1, sizeof(w1));
        // The view-space translation row is the other possibility: a camera that stores its
        // matrix rather than its parameters would keep that instead.
        searches[1].space = "view translation";
        searches[1].in[0] = g_cap[0].m[12];
        searches[1].in[1] = g_cap[0].m[13];
        searches[1].in[2] = g_cap[0].m[14];
        searches[1].out[0] = g_cap[1].m[12];
        searches[1].out[1] = g_cap[1].m[13];
        searches[1].out[2] = g_cap[1].m[14];

        for (int s = 0; s < 2; ++s) {
            first.clear();
            collect_position(searches[s], &first);
            if (first.empty()) {
                VRLOG("scan: nothing holds the camera's %s - the two captures agree on no "
                      "address in that space", searches[s].space);
                continue;
            }
            VRLOG("scan: %u address(es) hold the camera's %s in BOTH captures. That is the "
                  "field to edit, and the dump below shows what sits around it:",
                  (unsigned)first.size(), searches[s].space);
            const int show = (int)first.size() < 3 ? (int)first.size() : 3;
            for (int i = 0; i < show; ++i) {
                VRLOG("scan: --- candidate %d of %u at %p ---", i + 1,
                      (unsigned)first.size(), first[i]);
                describe_context(first[i], g_cap[0].m);
                // The layout check, against the DTI offsets of sBioCamera::ViewportCamera.
                // Only meaningful for the world-position space: there the address is the
                // camera's own position field, which is what mCameraOrg[i].cameraPos is.
                // In the view-translation space the address holds a matrix row instead, so
                // the check would be answering a different question.
                if (s == 0) check_camera_org(first[i], g_cap[0].m, w0);
            }
            g_done = true;
            return 0;
        }
        VRLOG("scan: the camera's position was not found in either space. The next thing to "
              "try is scanning for the look-at TARGET and the up vector, which are also "
              "derived from the pose.");
        g_done = true;
        return 0;
    }

    // Both paths above return; nothing is left to do here.
    g_done = true;
    return 0;
}

} // namespace

void mem_scan_note_head(const float *basis9) {
    if (!g_enabled || !basis9) return;
    memcpy(g_head_basis, basis9, sizeof(g_head_basis));
    g_head_basis_valid = true;

    // Row 2 of the basis is the head's backward vector, so its negation is forward.
    const float fx = -basis9[6], fy = -basis9[7], fz = -basis9[8];
    // The reference is the FIRST pose seen, not an absolute -Z. The headset starts tilted
    // (see the note on g_head_fwd0), and measuring against absolute forward makes that
    // baseline permanent: the reported turn is then the difference between two large
    // angles instead of how far the player actually moved.
    if (!g_head_fwd0_valid) {
        g_head_fwd0[0] = fx; g_head_fwd0[1] = fy; g_head_fwd0[2] = fz;
        g_head_fwd0_valid = true;
        g_head_turn_deg = 0.0f;
        return;
    }
    // Angle between the starting forward and the current one, on the horizontal plane.
    // Only the horizontal part counts: this drives "has the head turned enough for the head
    // to dominate the camera's basis", and looking up or down does not change that.
    const float a0 = atan2f(g_head_fwd0[0], -g_head_fwd0[2]);
    const float a1 = atan2f(fx, -fz);
    float d = (a1 - a0) * 57.2957795f;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    g_head_turn_deg = fabsf(d);
}

// Is this 3x3 a rotation? A camera's view matrix has an orthonormal basis; a view*projection,
// a shadow matrix, a scaled or sheared composite does not.
//
// This test exists because its absence wasted a whole run. The probe offered reg 27 on the
// grounds that "every translation component is above 50", and that matrix turned out not to
// be rigid at all: between the two captured poses its translation row moved 4512.6 units
// while the world position p = -R^T*t derived from the same two matrices moved 3751.9 - and
// for one rigid transform those distances MUST be equal. Nothing in memory can hold the
// position of a matrix that is not a pose, so the search had no chance from the start.
bool is_rotation_3x3(const float *m) {
    const float r[3][3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
    auto dot = [](const float *a, const float *b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    for (int i = 0; i < 3; ++i) {
        if (fabsf(dot(r[i], r[i]) - 1.0f) > 2e-2f) return false;      // rows unit length
        for (int j = i + 1; j < 3; ++j) {
            if (fabsf(dot(r[i], r[j])) > 2e-2f) return false;         // rows perpendicular
        }
    }
    // Columns too: a matrix can have orthonormal rows and still be a projection-like
    // composite if its columns are not (the transpose test).
    for (int c = 0; c < 3; ++c) {
        const float col[3] = {r[0][c], r[1][c], r[2][c]};
        if (fabsf(dot(col, col) - 1.0f) > 2e-2f) return false;
    }
    return true;
}

// Is this translation a PLACE IN THE WORLD, or a screen-space offset dressed up as one?
//
// A viewport / HUD placement carries a pixel offset: measured on this game, reg 1 held
// (1920.0, 1005.0, 17.0) and then (1920.0, 609.0, 17.0) - 1920 is the render width and 17 is
// a constant, which is a screen layout, not a location. That matrix is orthonormal and it
// does move, so the rigidity test above accepts it and it simply wins the race for pose 1 by
// being offered first. It then costs a whole run, because the search looks for a world
// position that the value can never be.
//
// So screen-space is excluded by construction: a camera's world position in this game is a
// place with components in the hundreds-to-thousands and is never exactly a round number,
// while every screen-space translation this project has measured has had an EXACT component
// (1920.0, 1005.0, 1080.0, 17.0, 1.0) and a Z pinned near zero.
bool looks_world_space(const float *m) {
    const float t[3] = {m[12], m[13], m[14]};
    // TWO OR MORE exact integers. One can happen by coincidence in a real position; two at
    // once is a layout, and a screen-space matrix always has them (1920 and 17, or 1005 and
    // 17 - measured on this game).
    int exact = 0;
    for (int i = 0; i < 3; ++i) {
        if (fabsf(t[i] - floorf(t[i] + 0.5f)) < 1e-3f) ++exact;
    }
    if (exact >= 2) return false;
    // The constant 17 in reg 1 is the giveaway that matters most: screen-space translations
    // keep a component pinned at a small constant.
    for (int i = 0; i < 3; ++i) {
        if (t[i] != 0.0f && fabsf(t[i]) < 25.0f) return false;
    }
    const float mag = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    return mag >= 200.0f;
}

void mem_scan_note_matrix(const float *m) {
    if (!g_enabled || g_cap_count >= 2) return;

    // Refuse anything that is not a rigid pose, and say so once per register so the log
    // shows what was offered and why it was turned down.
    if (!is_rotation_3x3(m)) {
        static int s_rejected = 0;
        if (s_rejected < 5) {
            ++s_rejected;
            VRLOG("scan: refused a matrix that is not a rigid pose (basis not orthonormal) - "
                  "translation (%.1f %.1f %.1f). A non-rigid matrix has no camera position to "
                  "find in memory, so feeding it to the search can only produce a false "
                  "'nothing holds it'.",
                  m[12], m[13], m[14]);
        }
        return;
    }
    // ... and anything whose translation is a screen layout rather than a place. See
    // looks_world_space: this is what stopped the search wasting a run on the viewport
    // matrix (1920.0, 1005.0, 17.0).
    if (!looks_world_space(m)) {
        static int s_screen = 0;
        if (s_screen < 5) {
            ++s_screen;
            VRLOG("scan: refused a screen-space translation (%.1f %.1f %.1f) - exact integers "
                  "and/or a Z at the screen plane mean this is a viewport/HUD layout, not a "
                  "place in the world, so no memory address can hold it as the camera's "
                  "position.",
                  m[12], m[13], m[14]);
        }
        return;
    }

    const unsigned long long now = GetTickCount64();

    if (g_cap_count == 0) {
        memcpy(g_cap[0].m, m, sizeof(g_cap[0].m));
        g_cap[0].tick = now;
        ++g_cap_count;
        VRLOG("scan: captured camera pose 1/2 - translation (%.1f %.1f %.1f). Pose 2 is NOT "
              "taken on a timer: it is taken as soon as the camera has MOVED %.0f units from "
              "here, so walk until it does (up to %.0f s, then this pose is dropped and it "
              "re-arms).",
              m[12], m[13], m[14], kArmTravel, (double)kArmTimeoutMs / 1000.0);
        return;
    }

    // Pose 1 is held. Take pose 2 only once the camera has actually moved - a menu or a
    // loading screen keeps the camera parked, and two captures of one parked pose identify
    // nothing at all.
    const float dx = m[12] - g_cap[0].m[12];
    const float dy = m[13] - g_cap[0].m[13];
    const float dz = m[14] - g_cap[0].m[14];
    const float moved = sqrtf(dx * dx + dy * dy + dz * dz);
    if (moved < kArmTravel) {
        if (now - g_cap[0].tick > kArmTimeoutMs) {
            VRLOG("scan: pose 1 was dropped after %.0f s - this register only moved %.1f units "
                  "of the %.0f required, so the game is still in a menu or a cutscene, or this "
                  "is not the camera. Re-arming: the next capture becomes pose 1 again.",
                  (double)kArmTimeoutMs / 1000.0, moved, kArmTravel);
            g_cap_count = 0;
        }
        return;
    }
    memcpy(g_cap[1].m, m, sizeof(g_cap[1].m));
    g_cap[1].tick = now;
    ++g_cap_count;
    VRLOG("scan: captured camera pose 2/2 - translation (%.1f %.1f %.1f), %.1f units from "
          "pose 1 (needed %.0f) - conditions met, the scan can run on a real pair",
          m[12], m[13], m[14], moved, kArmTravel);
}

bool mem_scan_enabled() { return g_enabled; }

void mem_scan_start() {
    wchar_t path[MAX_PATH] = L"";
    const wchar_t *log_path = vrlog::path();
    if (!log_path || !log_path[0]) return;
    wcsncpy_s(path, MAX_PATH, log_path, _TRUNCATE);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - path)), L"re6vr_scan.txt");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;

    g_enabled = true;
    g_running = true;
    VRLOG("scan: ENABLED from %ls - will capture two camera poses and search memory for the "
          "address that holds both", path);
    g_thread = CreateThread(nullptr, 0, scan_thread, nullptr, 0, nullptr);
    if (!g_thread) {
        g_enabled = false;
        VRLOG("scan: could not start the scan thread");
    }
}

bool mem_scan_done() { return g_done; }

} // namespace re6vr
