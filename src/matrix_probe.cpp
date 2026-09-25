// matrix_probe.cpp - classifies the matrices MT Framework feeds the vertex shader.
//
// What it is looking for, and why those tests:
//
//   * A VIEW matrix is a rigid transform: its 3x3 part is orthonormal (rows of unit
//     length that are mutually perpendicular) and the translation lives in the
//     fourth row. So: three unit-length rows, pairwise dot products near zero.
//
//   * A PROJECTION matrix is not orthonormal at all. In the usual D3D convention it
//     has a zeroed fourth column apart from the perspective term, and its fourth
//     ROW is (0, 0, 1, 0) - which is exactly what makes clip.w equal to view.z.
//
//   * The product view*projection satisfies neither test, which is how the combined
//     matrix (many engines upload only this) is told apart from its two factors.
//
// Everything here is inspection only: the constants are forwarded byte for byte.
#include "matrix_probe.h"
#include "log.h"
#include "mem_scan.h"

#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace re6vr {
namespace {

typedef HRESULT(STDMETHODCALLTYPE *SetVsConstFn)(IDirect3DDevice9 *, UINT, const float *, UINT);
SetVsConstFn g_original_set_vs_const = nullptr;
bool g_installed = false;
bool g_enabled = false;
bool g_parsed = false;

// Logging budget. The game uploads constants thousands of times per frame, so the
// probe stays quiet unless it has something new to say.
const int kTopCandidates = 6;
int g_frames_seen = 0;            // presented frames; advanced once per Present, always
// Frames counted while the probe was actually measuring. Kept separate from the counter
// above so that nothing has to depend on a marker file being present in order to progress.
int g_frames_measured = 0;
unsigned long long g_calls = 0;
int g_reports = 0;
const int kMaxReports = 10;

// ---- per-frame state, PER THREAD ------------------------------------------
//
// The game presents from more than one thread (measured: two of them, uploading
// different batches inside the same frame - see the log line pairs at t=31312 and
// t=20856). Everything below that means "the frame being uploaded right now" is
// therefore thread-local, not global. It used to be global, and the two threads
// trampled each other's state: one reset the other's view counter mid-frame so the
// slot indices shifted, and one cleared the other's anchor so a matrix that had
// already been rotated got re-anchored at the rotated value and rotated again -
// which is the COMPOUNDING the log reported, four times, and it is exactly the kind
// of accumulating distortion that shows up as a stretched image rather than as a
// camera that turns.
//
// The frame is counted from the upload stream itself, not from Present and not from
// g_frames_seen (which advances once per Present, from whichever thread got there
// first) - see note_batch_and_detect_frame_start above.
// ---- thread-local frame boundary, derived from the upload stream ------------
//
// "This thread's current frame" cannot come from Present: the game presents on one
// thread but uploads constants from at least two (measured t=31312 and t=20856, both
// uploading inside the same frame), so a Present-driven counter never advances for
// the other uploader and its anchors would never be cleared.
//
// Instead the boundary is taken from the stream itself. The engine rebuilds the same
// set of constant buffers every frame, so the sequence of (start, count) pairs is
// cyclic, and a repeat of a pair already seen in this cycle means a new frame has
// begun. That needs no frame counter at all and is correct per thread by
// construction.
struct BatchKey {
    UINT start;
    UINT count;
};
const int kMaxBatchKeys = 128;
thread_local BatchKey tl_seen_batches[kMaxBatchKeys];
thread_local int tl_seen_batch_count = 0;
thread_local int tl_frame = 0;
thread_local bool tl_frame_just_started = false;
thread_local int tl_shift_frame = -1;
thread_local int tl_shift_seen = 0;
thread_local int tl_head_frame = -1;

// Returns true when this batch begins a new frame for the calling thread.
bool note_batch_and_detect_frame_start(UINT start, UINT count) {
    for (int i = 0; i < tl_seen_batch_count; ++i) {
        if (tl_seen_batches[i].start == start && tl_seen_batches[i].count == count) {
            // The cycle has come round: this is the first batch of a new frame.
            tl_seen_batch_count = 0;
            ++tl_frame;
            // Fall through and record this batch as the new frame's first.
            tl_seen_batches[tl_seen_batch_count].start = start;
            tl_seen_batches[tl_seen_batch_count].count = count;
            ++tl_seen_batch_count;
            return true;
        }
    }
    if (tl_seen_batch_count < kMaxBatchKeys) {
        tl_seen_batches[tl_seen_batch_count].start = start;
        tl_seen_batches[tl_seen_batch_count].count = count;
        ++tl_seen_batch_count;
    }
    return false;
}

// Camera-motion detector.
//
// The view matrix only tells us anything new when the camera moves, and the first
// capture proved it: the game sat on the title screen and all four snapshots carried
// the identical translation (42, -30, -100). A fixed report budget would be spent
// entirely on identical frames. So a translation that has moved re-arms the budget
// and the next uploads get logged, which is what makes walking around in a level
// produce a series of distinct cameras instead of one repeated four times.
bool g_have_last_view = false;
float g_last_view_translation[3] = {0.0f, 0.0f, 0.0f};
// A world-scale translation is what separates the level's camera from a HUD placement:
// a HUD matrix carries a screen-space or unit offset, the camera carries a place in the
// level. 50 units is far above anything a UI matrix uses (measured: 0.0003 and 1.0) and
// far below the camera's own coordinates (measured: hundreds to thousands).
const float kWorldTranslationMin = 50.0f;
// The register first seen carrying a world-scale translation, to be confirmed on a later
// upload of the same register before the measurement starts.
const float kViewMoveEpsilon = 0.05f;   // metres

// The safety experiment (see hooked_SetVertexShaderConstantF).
bool g_shift_enabled = false;
float g_shift_metres = 0.0f;
unsigned long long g_shift_hits = 0;
int g_shift_pick = -1;            // which one to move; -1 = all, -2 = last
// Per view index: the engine's own translation and what this hook last wrote there.
const int kMaxShiftSlots = 8;
float g_shift_original[kMaxShiftSlots] = {0};
float g_shift_written[kMaxShiftSlots] = {0};
bool g_shift_seen_summary = false;
// Pure reconnaissance mode: report every view matrix of every frame (index, source
// register, translation, first rotation row) without moving anything. This is what
// distinguishes "two nearly identical cameras" from "one camera plus a secondary
// pass", and it is what a `pick` value should be chosen from rather than guessed.
bool g_shift_log_all = false;
int g_cap_frame_count = 0;
bool g_raw_logged = false;

// ---- head look ------------------------------------------------------------
//
// The view matrix carries the camera's world->view rotation in its 3x3 part and
// its position in the translation row. Rotating the camera about its own position
// is therefore a 3x3 change only - and because it is a *rotation of the basis*,
// not a nudge to one basis vector, the result stays a valid rigid transform no
// matter how far the head turns. That matters: the earlier "shift the
// translation" experiment could only ever slide the camera, never turn it.
//
// The rotation is anchored per view slot exactly like the translation shift is,
// and for the same reason: the engine re-uploads the same buffer several times
// per frame, so a plain in-place multiply would compound and the scene would spin
// away instead of following the head.
bool g_head_enabled = false;
float g_head_gain = 1.0f;
unsigned long long g_head_hits = 0;
int g_head_logged = 0;
float g_head_rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
bool  g_head_valid = false;
// Per view index: the engine's own 3x3, row-major, and what this hook last wrote.
float g_head_anchor[kMaxShiftSlots][9] = {{0}};
bool g_head_anchor_valid[kMaxShiftSlots] = {false};
// What this hook last wrote into that slot, so an upload can be told apart as either the
// engine's own matrix (re-anchor) or our own write coming back (keep the anchor).
float g_head_written[kMaxShiftSlots][9] = {{0}};
// "The last view matrix of the frame" cannot be named while the frame is still being
// uploaded, so pick = -2 records the slot here and rotates it from the next frame on.
int g_head_last_index = -1;
int g_head_apply_index = -1;
bool g_head_apply_valid = false;
int g_head_apply_logged = 0;
// The view slots of the frame being assembled, in upload order. The last entry once
// the frame ends is its final view matrix; `_prev` keeps the completed frame so the
// new one can start empty while the previous ordering is still readable.
int g_head_order[kMaxShiftSlots] = {0};
int g_head_order_len = 0;
int g_head_order_prev[kMaxShiftSlots] = {0};
int g_head_order_len_prev = 0;
// Last rotated basis per slot, so a write that accumulates on top of itself is caught
// in the act rather than argued about from a headset. The comparison itself must be
// thread-local: another thread writing a different rotation into the same slot is not
// compounding, and reporting it as such is what a shared copy did here.
thread_local float tl_head_result[kMaxShiftSlots][9] = {{0}};
thread_local bool tl_head_result_valid[kMaxShiftSlots] = {false};
bool g_head_logged_slot[kMaxShiftSlots] = {false};
bool g_head_late_logged[kMaxShiftSlots] = {false};
int g_head_compound = 0;
// ---- which slot is the actual camera? -------------------------------------
//
// This is the question the on-machine runs answered wrong. Two separate mistakes have
// been made here, and both are recorded because both cost a run:
//
//  1. "rotate every rigid matrix" is not "rotate the camera". The slots carrying
//     translations like (0, 0, 1) are HUD and sprite placements, and rotating those is
//     what stretched the picture.
//  2. The statistics were keyed by SLOT NUMBER, which is frame-local and drifts (the
//     same matrix was logged as slot #2 in one place and slot #0 in another inside one
//     frame). Comparing "this slot's rotation last time" against "this slot's rotation
//     now" therefore compared two different matrices, and every score came out 0.00000.
//
// So the key here is the REGISTER OFFSET, which is what actually identifies an upload.
// A view matrix cannot be told from a UI matrix by its shape - both are rigid - so the
// camera is found by behaviour: its rotation changes when the player looks around and a
// HUD placement's does not.
const int kMaxTrackedRegs = 64;
float g_head_track_rot[kMaxTrackedRegs][9] = {{0}};
bool g_head_track_have[kMaxTrackedRegs] = {false};
float g_head_track_score[kMaxTrackedRegs] = {0.0f};
float g_head_track_trans[kMaxTrackedRegs][3] = {{0}};
int g_head_track_samples[kMaxTrackedRegs] = {0};
// How many times head look has actually WRITTEN into each register. This exists because a
// test that appears to show "rotating the camera does nothing" is worthless if nothing was
// written - and that has now happened twice. The candidate log is capped (g_head_hits < 3)
// so it only ever shows the first few calls, which are not where the interesting registers
// appear; a register can be selected and never logged, or never reached at all, and the log
// looks identical either way. Counting writes per register, unconditionally, is what makes
// "the write happened and had no visible effect" a claim that can be checked.
int g_head_writes[kMaxTrackedRegs] = {0};
int g_head_write_regs_seen = 0;
int g_head_write_report_frame = 0;
// Largest "smallest component of the translation" seen for this register. A place in the
// world has every component large; a screen-space position pins one of them to 1.
float g_head_track_mincomp[kMaxTrackedRegs] = {0.0f};
const float kWorldComponentMin = 50.0f;
// The register the memory scanner has settled on as the camera, once its position has been
// observed to travel. -1 until then.
int scan_cam_reg = -1;
int g_head_auto_slot = -1;         // -1 until the camera has been identified
int g_head_auto_frames = 0;
// The camera is remembered as a REGISTER SIGNATURE, not as a slot number - see above.
UINT g_head_cam_reg = 0;
bool g_head_cam_valid = false;
// The camera's position travels with the player; a HUD placement stays put, so many
// world units of accumulated travel means "this is the one that moves".
const float kAutoMinTravel = 200.0f;
// Continuous trace of every view candidate, one line per candidate per frame. This is
// the measurement that answers "which register is the world camera", which cannot be
// answered by rotating things and looking at the headset: it needs a log in which each
// candidate's rotation and translation can be followed while the player actually walks
// and looks around. Off by default because it is verbose (tens of lines per frame).
//
// It starts when the first view matrix MOVES - which means a level is loaded and the
// player is in it - and then runs to the end of the process. The first version had a
// total budget instead and spent the whole of it during the title screen, where no camera
// exists yet; a frame number is no better, because the title screen and gameplay present
// at wildly different rates. The camera's own movement is the only reliable "we are in a
// level now" signal.
bool g_trace_enabled = false;
int g_trace_frames = 0;
bool trace_active = false;   // set when a view matrix first moves: a level is loaded
thread_local double tl_trace_stat[kMaxShiftSlots][6];   // |rot| change, |trans| change
thread_local int tl_trace_slot = 0;                    // frame-local slot for the trace

struct Candidate {
    int offset;
    const char *kind;
    float score;
    float diag[4];
    float m[16];
    bool axis_aligned = false;
};

float vec_len3(float x, float y, float z) {
    return sqrtf(x * x + y * y + z * z);
}

// How well the 3x3 part behaves like a rotation: 1.0 is perfect.
float orthonormality(const float *m, int row_stride) {
    float rows[3][3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) rows[r][c] = m[r * row_stride + c];
    }
    float worst = 1.0f;
    for (int r = 0; r < 3; ++r) {
        const float len = vec_len3(rows[r][0], rows[r][1], rows[r][2]);
        const float dev = fabsf(len - 1.0f);
        if (dev < worst) worst = dev;
    }
    // worst currently holds the smallest deviation; convert to a 0..1 score of the
    // largest one instead, which is the strict measure.
    float largest = 0.0f;
    for (int r = 0; r < 3; ++r) {
        const float len = vec_len3(rows[r][0], rows[r][1], rows[r][2]);
        const float dev = fabsf(len - 1.0f);
        if (dev > largest) largest = dev;
        for (int s = r + 1; s < 3; ++s) {
            const float d = rows[r][0] * rows[s][0] + rows[r][1] * rows[s][1] + rows[r][2] * rows[s][2];
            if (fabsf(d) > largest) largest = fabsf(d);
        }
    }
    (void)worst;
    return largest;
}

void log_matrix(const char *tag, const float *m) {
    VRLOG("%s [ %.4f %.4f %.4f %.4f ]", tag, m[0], m[1], m[2], m[3]);
    VRLOG("%s [ %.4f %.4f %.4f %.4f ]", tag, m[4], m[5], m[6], m[7]);
    VRLOG("%s [ %.4f %.4f %.4f %.4f ]", tag, m[8], m[9], m[10], m[11]);
    VRLOG("%s [ %.4f %.4f %.4f %.4f ]", tag, m[12], m[13], m[14], m[15]);
}

// True for the rigid transforms that can be a camera: a rotation with a translation.
// This deliberately includes "UI/world-axis", which is a rigid transform whose rotation
// happens to be the identity - a camera that has not turned yet looks exactly like that,
// and leaving it out meant such a camera could never be selected. Selecting a world
// placement by mistake is harmless by comparison: rotating a basis that is already the
// world basis still draws the object where it belongs.
bool is_view_candidate(const Candidate &c) {
    return strcmp(c.kind, "VIEW") == 0 || strcmp(c.kind, "UI/world-axis") == 0;
}

// Classifies one 16-float window. Returns false when it is not a plausible matrix.
bool classify(const float *m, Candidate *out) {
    for (int i = 0; i < 16; ++i) {
        if (!(m[i] == m[i])) return false;                       // NaN
        if (fabsf(m[i]) > 1.0e7f) return false;                  // not a matrix element
    }

    const float on = orthonormality(m, 4);                       // row-major reading
    // The same window read column-major is what a transposed upload looks like.
    float t[16];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) t[r * 4 + c] = m[c * 4 + r];
    }
    const float on_t = orthonormality(t, 4);

    const float translation = vec_len3(m[12], m[13], m[14]);

    out->offset = 0;
    out->kind = "?";
    out->score = 0.0f;
    memcpy(out->m, m, sizeof(out->m));

    if (on < 1.0e-3f && translation > 0.01f) {
        // Rigid with translation. The old test for telling a view matrix from a
        // projection matrix was "the translation is world-sized (>= 50 units)",
        // because a projection matrix's fourth row holds a screen size rather than
        // a place. That heuristic fails whenever the camera is near the world
        // origin: the harness's own synthetic camera sits 7.8 units out and was
        // therefore labelled PROJ(rigid), which silently disabled anything keyed
        // on the label.
        //
        // The robust test is the one already computed above: a rigid transform is
        // orthonormal read BOTH ways, because its transpose is its inverse. A
        // projection matrix is orthonormal in neither reading - its rows are a
        // screen scale and a zero row. So "both readings orthonormal" identifies a
        // rigid transform regardless of where the camera happens to be.
        if (on_t < 1.0e-3f) {
            const bool non_trivial_rotation = fabsf(m[1]) > 1.0e-3f || fabsf(m[2]) > 1.0e-3f ||
                                              fabsf(m[4]) > 1.0e-3f || fabsf(m[6]) > 1.0e-3f ||
                                              fabsf(m[8]) > 1.0e-3f || fabsf(m[9]) > 1.0e-3f;
            if (non_trivial_rotation) {
                out->kind = "VIEW";
                out->score = 1.0f / (1.0f + on);
                out->axis_aligned = false;
                return true;
            }
            // Exactly the identity rotation with a translation: a world-axis-aligned
            // placement (UI, or a camera that has not turned yet). Still rigid, and
            // it still must not be mistaken for a projection.
            out->kind = "UI/world-axis";
            out->score = 1.0f;
            return true;
        }
        // Rigid in one reading only. With a world-sized translation this is the
        // original "camera somewhere far away" case; otherwise it is a projection
        // whose scaling rows happen to be unit length.
        const float translation_len = translation;
        const bool world_scale = translation_len >= 50.0f && translation_len <= 1.0e6f;
        if (world_scale) {
            out->kind = "VIEW";
            out->score = 1.0f / (1.0f + on);
            out->axis_aligned = false;
            return true;
        }
        out->kind = "PROJ(rigid)";
        out->score = 1.0f;
        return true;
    }
    if (on_t < 1.0e-3f) {
        out->kind = "VIEW^T";
        out->score = 0.9f / (1.0f + on_t);
        return true;
    }
    // Projection, accepting either memory layout. Both are seen in the wild and the
    // game actually uses the column-major one: clip.w must end up as view z, which
    // the classic test below expresses for a row-major upload.
    {
        // Layout A (row-major): fourth column zero except the perspective term,
        // fourth row (0, 0, 1, 0).
        const bool fourth_col_sparse = fabsf(m[3]) < 1.0e-3f && fabsf(m[7]) < 1.0e-3f &&
                                       fabsf(m[11]) > 1.0e-6f;
        const bool fourth_row_persp = fabsf(m[12]) < 1.0e-3f && fabsf(m[13]) < 1.0e-3f &&
                                      fabsf(m[15]) < 1.0e-3f;
        // Layout B (column-major, what this engine uploads): the same two tests on
        // the transposed reading.
        const bool t_fourth_col_sparse = fabsf(t[3]) < 1.0e-3f && fabsf(t[7]) < 1.0e-3f &&
                                         fabsf(t[11]) > 1.0e-6f;
        const bool t_fourth_row_persp = fabsf(t[12]) < 1.0e-3f && fabsf(t[13]) < 1.0e-3f &&
                                        fabsf(t[15]) < 1.0e-3f;
        if ((fourth_col_sparse && fourth_row_persp) || (t_fourth_col_sparse && t_fourth_row_persp)) {
            out->kind = "PROJ";
            out->score = 1.0f;
            return true;
        }
    }
    if (fabsf(m[15]) < 1.0e-3f && fabsf(m[11]) > 1.0e-6f && translation > 0.01f && on > 1.0e-2f) {
        // Neither factor passes alone but the odd 3x3 does: view*proj combined.
        out->kind = "VIEW*PROJ";
        out->score = 0.5f;
        return true;
    }
    return false;
}

// Raw dump of every register in an upload.
//
// The classifier alone is not enough to work from: it reports 16-float windows, so
// one matrix inside a large upload shows up several times at several offsets, and
// the register boundaries - the thing that says where the matrix actually starts -
// are invisible. The first game capture made that concrete: the view matrix turned up
// at reg 9 in one shader and reg 27 in another, so its register index is not a
// constant and a fixed injection point would be wrong.
//
// Printing every register (capped, since one upload can be enormous) is what lets the
// whole batch be reconstructed off-line. Values are printed as (x y z w) with each
// component also in hex, because a hex pattern like 3F800000 is unambiguous about
// being 1.0f where a rounded decimal is not.
void dump_registers(UINT start, UINT count, const float *data) {
    const UINT shown = count < 40 ? count : 40;
    VRLOG("cap:   raw dump, reg %u..%u of %u (%s)", start, start + count - 1, count,
          count > shown ? "truncated to 40" : "all");
    for (UINT i = 0; i < shown; ++i) {
        const float *v = data + (size_t)i * 4;
        unsigned int h[4];
        memcpy(h, v, sizeof(h));
        VRLOG("cap:   reg %3u = (%.6g %.6g %.6g %.6g)  hex (0x%08X 0x%08X 0x%08X 0x%08X)",
              start + i, v[0], v[1], v[2], v[3], h[0], h[1], h[2], h[3]);
    }
    if (count > shown) {
        for (UINT i = count - 4; i < count; ++i) {
            const float *v = data + (size_t)i * 4;
            unsigned int h[4];
            memcpy(h, v, sizeof(h));
            VRLOG("cap:   reg %3u = (%.6g %.6g %.6g %.6g)  hex (0x%08X 0x%08X 0x%08X 0x%08X)",
                  start + i, v[0], v[1], v[2], v[3], h[0], h[1], h[2], h[3]);
        }
    }
}

void report(UINT start, UINT count, const float *data) {
    Candidate cands[kTopCandidates];
    int found = 0;
    const UINT vecs = count;
    for (UINT v = 0; v + 3 < vecs; ++v) {
        Candidate c;
        if (!classify(data + (size_t)v * 4, &c)) continue;
        c.offset = (int)(start + v);
        if (found < kTopCandidates) {
            cands[found++] = c;
        }
    }
    if (!found) return;

    ++g_reports;
    VRLOG("cap: ---- frame %d, SetVertexShaderConstantF(reg %u, %u vec4) : %d candidate(s) ----",
          g_frames_seen, start, count, found);
    dump_registers(start, count, data);
    for (int i = 0; i < found; ++i) {
        VRLOG("cap: reg %d looks like %s (score %.2f)", cands[i].offset, cands[i].kind,
              cands[i].score);
        char tag[48];
        _snprintf_s(tag, sizeof(tag), _TRUNCATE, "cap:   reg %d", cands[i].offset);
        log_matrix(tag, cands[i].m);
    }
}

// One trace line for a view candidate. Slot numbers drift between frames, so the analysis
// is done per REGISTER; what the line has to carry is everything needed to tell a camera
// from the other rigid matrices in the stream, without another run to ask for more.
//
// The discriminator is smoothness. A camera is a physical object: walking moves it by
// continuous amounts, so its translation changes by a little each frame relative to its own
// magnitude. A viewport matrix carries a screen size instead of a place and its "position"
// jumps by hundreds of units whenever the viewport is re-setup; a HUD placement does not
// move at all. Printing the per-frame deltas alongside the absolute values is what lets
// that be measured rather than assumed.
void trace_candidate(int slot, UINT reg, const char *kind, const float *q) {
    if (slot < 0 || slot >= kMaxShiftSlots) return;

    // Reject junk before it can be mistaken for a camera. The first version of this
    // recorded whatever classified as a rigid transform, and inf/nan/garbage rows then
    // matched "world-scale translation" tests and produced candidate dumps of infinities.
    const float rot_now[9] = {q[0], q[1], q[2], q[4], q[5], q[6], q[8], q[9], q[10]};
    for (int k = 0; k < 16; ++k) {
        if (!(q[k] == q[k])) return;                     // NaN
        if (fabsf(q[k]) > 1.0e6f) return;                // not a matrix element
    }

    double drot = 0.0;
    for (int k = 0; k < 9; ++k) {
        drot += fabs((double)rot_now[k] - tl_trace_stat[slot][k]);
    }
    const double dtr = fabs((double)q[12] - tl_trace_stat[slot][3]) +
                       fabs((double)q[13] - tl_trace_stat[slot][4]) +
                       fabs((double)q[14] - tl_trace_stat[slot][5]);
    for (int k = 0; k < 9; ++k) tl_trace_stat[slot][k] = rot_now[k];
    tl_trace_stat[slot][3] = q[12];
    tl_trace_stat[slot][4] = q[13];
    tl_trace_stat[slot][5] = q[14];
    VRLOG("trace f=%d slot=%d reg=%u kind=%s t=(%.3f %.3f %.3f) tmag=%.2f drot=%.5f "
          "dtr=%.3f r0=(%.4f %.4f %.4f) r1=(%.4f %.4f %.4f) r2=(%.4f %.4f %.4f)",
          g_trace_frames, slot, reg, kind, q[12], q[13], q[14],
          sqrtf(q[12] * q[12] + q[13] * q[13] + q[14] * q[14]), drot, dtr,
          rot_now[0], rot_now[1], rot_now[2], rot_now[3], rot_now[4], rot_now[5],
          rot_now[6], rot_now[7], rot_now[8]);
}

HRESULT STDMETHODCALLTYPE hooked_SetVertexShaderConstantF(IDirect3DDevice9 *dev, UINT start,
                                                          const float *data, UINT count) {
    ++g_calls;
    // Frames are delimited by the upload stream, which is the only frame boundary that
    // exists per thread - see note_batch_and_detect_frame_start.
    tl_frame_just_started = note_batch_and_detect_frame_start(start, count);
    if (tl_frame_just_started) {
        tl_trace_slot = 0;
        if (g_trace_enabled && trace_active) ++g_trace_frames;
    }
    if (g_calls < 4) {
        VRLOG("cap: hook call %llu (reg %u, %u vec4) enabled=%d shift=%d head=%d",
              g_calls, start, count, (int)g_enabled, (int)g_shift_enabled, (int)g_head_enabled);
    }
    // g_head_enabled belongs in this list: the head-look block below lives inside
    // this guard, and leaving it out meant "re6vr_head_view.txt alone" took the
    // whole path out - head tracking would have looked like "the feature does
    // nothing" rather than like a bug. (Found the hard way, twice: first the
    // install guard, then this one.)
    // The same list as the install guard, and for the same reason: a switch that reads
    // matrices has to appear in BOTH, or the hook installs and then does nothing. Missing
    // g_trace_enabled here meant "trace = 1" printed its enabled line and then produced not
    // a single trace line for an entire gameplay run.
    if ((g_enabled || g_shift_enabled || g_head_enabled || g_trace_enabled) &&
        data && count > 0) {
        // A view matrix that has moved means the camera is doing something new, so
        // the report budget is re-armed rather than exhausted by identical frames.
        // The trace is deliberately NOT gated on the head switch: identifying which
        // register is the world camera is a measurement, and it should be possible to
        // take it without rotating anything at all.
        //
        // START OF THE MEASUREMENT. One condition, and it is a magnitude rather than a
        // change: a candidate whose translation is world-scale. Frame numbers are useless
        // here (the title screen and gameplay present at wildly different rates), and "a
        // view matrix moved" was worse than useless - the first sample of anything always
        // counts as "moved" because there is nothing to compare against yet, and at the
        // title screen that latched onto a UI matrix and started rotating it.
        //
        // World-scale magnitude does not have that failure mode: it is a property of the
        // value, not of a comparison, so it cannot fire on the first sample of something
        // that has no world position. A HUD placement's translation is (0,0,1) from the
        // first frame to the last.
        if (!trace_active) {
            for (UINT v = 0; v + 3 < count; ++v) {
                Candidate c;
                if (!classify(data + (size_t)v * 4, &c)) continue;
                if (!is_view_candidate(c)) continue;
                const float *m = c.m;
                const float tmag = sqrtf(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]);
                if (tmag < kWorldTranslationMin) continue;
                trace_active = true;
                g_trace_frames = 0;
                VRLOG("trace: reg %u carries a world-scale translation (%.1f %.1f %.1f), so "
                      "a level is loaded - the trace starts here", start + v,
                      m[12], m[13], m[14]);
                break;
            }
        }
        if (g_trace_enabled && trace_active) {
            for (UINT v = 0; v + 3 < count; ++v) {
                float *q = const_cast<float *>(data) + (size_t)v * 4;
                Candidate tc;
                if (!classify(q, &tc)) continue;
                if (!is_view_candidate(tc)) continue;
                const int slot = tl_trace_slot++;
                if (slot >= kMaxShiftSlots) continue;
                trace_candidate(slot, start + v, tc.kind, q);
            }
        }
        // Sample rather than log everything: the first frames carry the scene setup,
        // and after that the same matrices repeat every frame.
        if (g_enabled && g_reports < kMaxReports && (g_calls < 4000 || (g_calls % 20000) == 0)) {
            report(start, count, data);
        }

        // Feed the camera-object scanner. This sits OUTSIDE the head-look block on purpose:
        // finding the camera is a measurement, and it has to work with head look switched
        // off. Placed inside that block it silently never ran - the hook was installed, the
        // scanner waited, and the log said nothing about why.
        //
        // Which matrix gets fed matters more than anything else here, and the previous
        // versions of this selection were wrong twice over:
        //
        //   * "world-scale translation" alone does NOT mean "camera" - a viewport matrix
        //     carries (1920, 1005, 17) and sails past any magnitude test;
        //   * "every component above 50" does not either. It picked reg 27 on 2026-09-23,
        //     and that matrix is not even rigid: between two poses its translation moved
        //     4512.6 units while p = -R^T*t moved 3751.9, which one rigid transform cannot
        //     do. Memory holds no position for a matrix that is not a pose, so the search
        //     that followed had no chance and reported a false "nothing holds it".
        //
        // So the choice is no longer made here by a heuristic at all. Every candidate view
        // register is offered to the scanner, and the scanner takes the first RIGID one whose
        // position travels - it can test rigidity, this loop cannot. The selection therefore
        // costs nothing and cannot lock onto a composite matrix.
        if (mem_scan_enabled() && !mem_scan_done()) {
            for (UINT v = 0; v + 3 < count; ++v) {
                Candidate sc;
                if (!classify(data + (size_t)v * 4, &sc)) continue;
                if (!is_view_candidate(sc)) continue;
                const int rk = (int)(start + v);
                if (rk < 0 || rk >= kMaxTrackedRegs) continue;
                const float *sm = sc.m;
                const float tm = sqrtf(sm[12] * sm[12] + sm[13] * sm[13] + sm[14] * sm[14]);
                if (tm < kWorldTranslationMin) continue;
                if (g_head_track_have[rk]) {
                    const float travel = fabsf(sm[12] - g_head_track_trans[rk][0]) +
                                         fabsf(sm[13] - g_head_track_trans[rk][1]) +
                                         fabsf(sm[14] - g_head_track_trans[rk][2]);
                    g_head_track_score[rk] += travel;
                    const float min_comp = fminf(fabsf(sm[12]), fminf(fabsf(sm[13]), fabsf(sm[14])));
                    if (min_comp > g_head_track_mincomp[rk]) g_head_track_mincomp[rk] = min_comp;
                    // Offer it, whatever register it is: mem_scan_note_matrix refuses
                    // anything that is not a rigid pose and says so in the log.
                    static int s_fed = 0;
                    if (s_fed < 4) {
                        ++s_fed;
                        VRLOG("scan: offering reg %d (translation %.1f %.1f %.1f, travelled "
                              "%.0f) - the scanner decides whether it is a rigid pose",
                              rk, sm[12], sm[13], sm[14], g_head_track_score[rk]);
                    }
                    mem_scan_note_matrix(sm);
                }
                g_head_track_trans[rk][0] = sm[12];
                g_head_track_trans[rk][1] = sm[13];
                g_head_track_trans[rk][2] = sm[14];
                g_head_track_have[rk] = true;
                break;
            }
        }

        // ---- the safety experiment -------------------------------------------
        // Shift the view matrix translation, in place. Two traps are already known:
        //
        //  1. **the engine reuses the uploaded buffer, and more than once per frame.**
        //     The first run edited the same buffer twice and asked for 0.03 m but
        //     applied 0.06 m, which is why the title screen showed every letter twice.
        //     A plain in-place += therefore cannot be used at all: this writes
        //     `anchor + shift` and re-anchors whenever the engine supplies a new
        //     original, which cannot compound no matter how often it is called.
        //
        //  2. **the frame contains more than one view matrix.** Shifting all of them
        //     draws the same geometry from several cameras at once, which reads as a
        //     duplicated image. `pick` selects which one to move.
        //
        // The offsets are discovered per batch rather than hard-coded: the game
        // uploads the view matrix at reg 1, 4, 6, 9 and 27 depending on the shader.
        if ((g_shift_enabled || g_shift_log_all) && count >= 4) {
            if (tl_frame_just_started || tl_shift_frame != tl_frame) {
                tl_shift_frame = tl_frame;
                tl_shift_seen = 0;
            }
            const int pick = g_shift_pick;   // -1 = every, -2 = last, >=0 = that index
            // Where the LAST view matrix of this batch sits, as a register offset. -2
            // selects it by location rather than by a counter, because the two available
            // counters count different things: `index` counts view candidates across the
            // whole frame (the numbering `pick >= 0` and the candidate log use), while a
            // position within the batch is what "the last one here" actually means. An
            // earlier version compared a frame-local counter against a batch-local one and
            // so never selected anything at all, with no error in the log to show for it.
            UINT last_reg = 0;
            bool has_last = false;
            {
                for (UINT v = 0; v + 3 < count; ++v) {
                    Candidate c;
                    if (classify(data + (size_t)v * 4, &c) && is_view_candidate(c)) {
                        last_reg = v;
                        has_last = true;
                    }
                }
            }
            // The engine uploads a transposed copy of the view matrix next to the
            // original (reg 8..11 beside reg 5..8 in the title-screen batch), and a
            // sliding window reads that copy as its own candidate. Those must not
            // consume an index, or "view #0" would mean different matrices depending
            // on the batch layout.
            for (UINT v = 0; v + 3 < count; ++v) {
                float *m = const_cast<float *>(data) + (size_t)v * 4;
                Candidate c;
                if (!classify(m, &c)) continue;
                // VIEW^T is the same matrix read the other way round and appears as an
                // overlapping window of the same upload; counting it would make the
                // index depend on how the engine happened to lay the batch out. An
                // identity-rotation camera is a view matrix too - see is_view_candidate.
                if (!is_view_candidate(c)) continue;

                const int index = tl_shift_seen++;
                const bool chosen = (pick == -1) || (index == pick) ||
                                    (pick == -2 && has_last && v == last_reg);

                // Anchor per view index, not globally: a frame contains several view
                // matrices and a single shared anchor would be overwritten by each of
                // them in turn, leaving the offset applied to whichever came last. The
                // engine rebuilds these values every frame, so re-anchoring whenever the
                // value differs from what we wrote is both correct and stable.
                float *anchor = nullptr;
                float *written = nullptr;
                if (index < kMaxShiftSlots) {
                    anchor = &g_shift_original[index];
                    written = &g_shift_written[index];
                }
                if (!anchor || m[12] != *written) {
                    if (anchor) {
                        *anchor = m[12];
                        *written = m[12];
                    }
                }

                // Structural log: every view matrix of the frame, in the order the
                // engine uploaded it, with where it came from. This is what tells the
                // "two nearly identical cameras" case (a doubled image) apart from
                // "one camera plus a shadow/reflection camera" - their translations
                // differ by the camera-to-camera offset in one case and by nothing at
                // all in the other.
                if (g_shift_log_all && g_cap_frame_count < 60) {
                    ++g_cap_frame_count;
                    // Only claim a write when one actually happened: with pick = -1
                    // every candidate is "chosen", so labelling them all SHIFTED made
                    // a baseline run (no shift marker at all) look like it was
                    // modifying the engine. The log has to be trustworthy enough to
                    // tell a baseline apart from an experiment.
                    const bool wrote = chosen && g_shift_enabled;
                    VRLOG("cap: frame %d view#%d (batch reg %u) translation %.2f %.2f %.2f "
                          "rot0 %.4f %.4f %.4f%s", g_frames_seen, index, start,
                          m[12], m[13], m[14], m[0], m[1], m[2],
                          wrote ? "  <-- WRITTEN" : "");
                }
                // Raw registers for the first view matrix of each frame, so the batch
                // that holds a camera can be read directly instead of inferred. Only
                // the first few frames: this is the setup, and it is the layout that
                // is being established.
                if (g_shift_log_all && g_frames_seen <= 3 && index == 0 && !g_raw_logged) {
                    g_raw_logged = true;
                    for (UINT r = 0; r < 12 && r < count; ++r) {
                        const float *rv = data + (size_t)r * 4;
                        VRLOG("cap:   batch reg %u +%u = (%.6g %.6g %.6g %.6g)",
                              start, r, rv[0], rv[1], rv[2], rv[3]);
                    }
                }

                if (!chosen || !anchor) continue;

                const float before = m[12];
                m[12] = *anchor + g_shift_metres;
                *written = m[12];
                ++g_shift_hits;
                if (g_shift_hits <= 10) {
                    VRLOG("shift: frame %d view#%d reg %u translation %.4f -> %.4f "
                          "(anchor %.4f + %.3f m)", g_frames_seen, index, start + v, before,
                          m[12], *anchor, g_shift_metres);
                }
                if (!g_shift_seen_summary) {
                    g_shift_seen_summary = true;
                    VRLOG("shift: applied %.3f m to view#%d (pick mode %d, %d view matrix(es) "
                          "in this frame)", m[12] - *anchor, index, pick, tl_shift_seen);
                }
            }
        }

        // ---- head look -------------------------------------------------------
        // Same selection logic as the shift above, but rotating the basis instead
        // of moving the position. Anchored identically, so re-uploads of the same
        // buffer cannot compound.
        if (g_head_enabled && count >= 4) {
            if (g_head_hits < 3 && g_calls < 5000) {
                VRLOG("head: block entered (call %llu, count %u, reg %u, rot %.2f %.2f %.2f)",
                      g_calls, count, start, g_head_rot[0], g_head_rot[1], g_head_rot[2]);
            }
            // View indices run across the whole FRAME, not within one batch: the game
            // uploads the camera in a 128-register block and other matrices in small
            // batches, so a batch-local numbering and a frame-local numbering disagree
            // (measured: view#2 and view#3 in the reg 0 batch, then view#5 in the reg 24
            // batch of the same frame). Everything here therefore uses the frame-local
            // index, which is the one `pick` and the candidate log already refer to.
            const int pick = g_shift_pick;

            // Which view slots this batch carried, so the frame's ordering can be
            // assembled across batches.
            bool batch_has[kMaxShiftSlots] = {false};
            const bool new_frame = (tl_frame_just_started || tl_head_frame != tl_frame);
            if (new_frame) {
                tl_head_frame = tl_frame;
                // Snapshot the frame that just ended before starting the new one.
                memcpy(g_head_order_prev, g_head_order, sizeof(g_head_order));
                g_head_order_len_prev = g_head_order_len;
                g_head_order_len = 0;
                // The engine rebuilds the constant buffer every frame, so this frame's
                // matrices are fresh data: drop last frame's anchors. Without this the
                // anchor would stay latched onto the first frame's value, and a head that
                // turned would have its rotation re-applied on top of the previous one.
                memset(g_head_anchor_valid, 0, sizeof(g_head_anchor_valid));
                // pick = -2 is "find the camera, then rotate only it". The camera is the
                // candidate whose rotation actually moves as the player looks around; a
                // HUD or sprite placement is rigid too and looks identical in shape, so
                // only its behaviour tells them apart.
                if (pick == -2 && !g_head_cam_valid) {
                    ++g_head_auto_frames;
                    int best = -1;
                    float best_score = 0.0f;
                    for (int i = 0; i < kMaxTrackedRegs; ++i) {
                        if (g_head_track_samples[i] < 20) continue;
                        if (g_head_track_score[i] > best_score) {
                            best_score = g_head_track_score[i];
                            best = i;
                        }
                    }
                    // Report the table regularly: this is the line that says which registers
                    // carry a world-scale translation and how far each of them travelled.
                    if (g_head_auto_frames % 120 == 0) {
                        VRLOG("head: auto - the camera is the register whose position "
                              "travels furthest (frame %d, best so far reg %d travelled "
                              "%.0f units, needs %.0f):", g_head_auto_frames, best,
                              best_score, kAutoMinTravel);
                        for (int i = 0; i < kMaxTrackedRegs; ++i) {
                            if (g_head_track_samples[i] == 0) continue;
                            VRLOG("head: auto -   reg %d: travelled %.0f over %d samples, "
                                  "now at (%.1f %.1f %.1f)", i, g_head_track_score[i],
                                  g_head_track_samples[i], g_head_track_trans[i][0],
                                  g_head_track_trans[i][1], g_head_track_trans[i][2]);
                        }
                    }
                    if (best >= 0 && best_score >= kAutoMinTravel) {
                        g_head_cam_reg = (UINT)best;
                        g_head_cam_valid = true;
                        VRLOG("head: auto - the camera is the matrix at reg %u (it travelled "
                              "%.0f world units, further than any other candidate); rotating "
                              "ONLY that matrix from now on", g_head_cam_reg, best_score);
                    }
                }
            }
            for (UINT v = 0; v + 3 < count; ++v) {
                float *m = const_cast<float *>(data) + (size_t)v * 4;
                Candidate c;
                const bool ok = classify(m, &c);
                if (g_head_hits < 3 && v < 4) {
                    VRLOG("head: window %u classify=%d kind=%s count=%u reg %u",
                          v, (int)ok, ok ? c.kind : "-", count, start + v);
                }
                if (!ok) continue;
                if (!is_view_candidate(c)) continue;

                const int index = tl_shift_seen++;
                if (index >= kMaxShiftSlots) continue;

                // Learn this register's behaviour, keyed by REGISTER so that every
                // comparison is between the same matrix.
                //
                // The world camera and a HUD placement are both rigid transforms, so shape
                // cannot tell them apart - but their TRANSLATIONS live in different worlds.
                // A HUD or sprite matrix carries a screen-space or unit offset like
                // (0, 0, 1); the world camera carries a place in the level, hundreds to
                // thousands of units out. That difference is what identifies the camera,
                // and it is measured rather than assumed.
                //
                // Rotation was tried first and it was the wrong signal twice over: at the
                // title screen the only candidate is a UI matrix, so "the rotation moved"
                // identified THAT matrix as the camera and rotating it is what stretched the
                // picture from the title screen onwards. Translation cannot make that
                // mistake, because the title screen's matrix has no world-scale translation
                // to begin with.
                {
                    const int rk = (int)(start + v);
                    if (rk >= 0 && rk < kMaxTrackedRegs) {
                        const float *r = m;
                        const float tmag = sqrtf(r[12] * r[12] + r[13] * r[13] + r[14] * r[14]);
                        const bool world_scale = tmag >= kWorldTranslationMin;
                        if (world_scale && pick == -2 && !g_head_cam_valid) {
                            if (g_head_track_have[rk]) {
                                // How far this register's position travelled, summed over the
                                // run. The camera is the one that actually goes somewhere.
                                g_head_track_score[rk] +=
                                    fabsf(r[12] - g_head_track_trans[rk][0]) +
                                    fabsf(r[13] - g_head_track_trans[rk][1]) +
                                    fabsf(r[14] - g_head_track_trans[rk][2]);
                            }
                            g_head_track_trans[rk][0] = r[12];
                            g_head_track_trans[rk][1] = r[13];
                            g_head_track_trans[rk][2] = r[14];
                            g_head_track_samples[rk] += 1;
                            g_head_track_have[rk] = true;
                        }
                    }
                }

                // pick = -2 means "the last view matrix of the frame", and the frame's
                // last matrix is only known once every upload of that frame has gone by.
                // Since the hook necessarily runs *during* the frame, the decision is
                // taken here and acted on from the next frame on - by which time the
                // engine has re-uploaded the same matrices, so the same slot carries the
                // same camera. Acting on it inside the current frame is impossible: this
                // call is that frame's last view upload.
                // Selection, by register in every case.
                //
                // `pick = N` used to mean "the Nth view candidate of the frame", and that was
                // a trap: candidate numbers are frame-local and drift (the same matrix is
                // logged as one number here and another there), and the numbering only ever
                // reaches however many candidates a frame happens to hold. So `pick = 9` -
                // which reads like "register 9" and was documented as such - silently
                // selected NOTHING, because no frame ever had nine candidates. The log said
                // head look was enabled and not one matrix was ever written.
                //
                // A register offset is stable and is what the log, the trace and the analysis
                // scripts all print, so `pick = N` now means register N.
                bool chosen = false;
                if (pick == -1) chosen = true;
                else if (pick == -2) {
                    // Before the camera is identified, rotate nothing: this phase is the
                    // measurement, and rotating would corrupt the very signal being read.
                    chosen = g_head_cam_valid && (start + v) == g_head_cam_reg;
                } else chosen = ((int)(start + v) == pick);
                const bool settled = true;

                if (g_head_hits < 3) {
                    VRLOG("head: candidate kind=%s index=%d chosen=%d settled=%d count=%u reg %u",
                          c.kind, index, (int)chosen, (int)settled, count, start + v);
                }
                if (index >= kMaxShiftSlots) continue;

                // Every call for the chosen slot gets rotated, not just the first: the
                // engine may upload the same camera more than once per frame, and leaving
                // a later copy unrotated would draw part of the scene from the original
                // orientation. Re-applying is safe as long as the anchor is the engine's
                // own matrix rather than the value written last time - the anchor is
                // cleared at each frame boundary above, so a re-upload of the same slot
                // within one frame re-anchors to whatever the engine just supplied and
                // the write is idempotent instead of cumulative.
                if (settled && chosen) {
                    float *anchor = &g_head_anchor[index][0];
                    // Idempotence, without depending on knowing where frames begin.
                    //
                    // A frame boundary is not reliably detectable here: the upload stream
                    // is not cleanly cyclic (the first attempt at detecting a boundary this
                    // way counted 716,400 "frames" in a run of a few thousand), and the two
                    // upload threads do not share one anyway. So instead of resetting the
                    // anchor on a guessed boundary, the anchor is validated against the data:
                    //
                    //   * m == our previous write  -> this is our own value being uploaded
                    //     again; the anchor is still the engine's, keep it.
                    //   * m != our previous write  -> the engine rebuilt this constant, so
                    //     it is the engine's own matrix and becomes the new anchor.
                    //
                    // The write is then always "anchor rotated by the CURRENT head pose",
                    // which is a pure function of two inputs and therefore cannot accumulate:
                    // applying it twice gives the same answer. That is what makes the old
                    // per-frame reset unnecessary rather than merely fixed.
                    bool re_anchor = !g_head_anchor_valid[index];
                    if (g_head_anchor_valid[index]) {
                        for (int k = 0; k < 9; ++k) {
                            if (fabsf(m[(k / 3) * 4 + (k % 3)] - g_head_written[index][k]) > 1.0e-6f) {
                                re_anchor = true;
                                break;
                            }
                        }
                    }
                    if (re_anchor) {
                        for (int r = 0; r < 3; ++r) {
                            for (int k = 0; k < 3; ++k) anchor[r * 3 + k] = m[r * 4 + k];
                        }
                        g_head_anchor_valid[index] = true;
                    }
                    // new_row_r[c] = sum_k anchor[r][k] * R[k][c]
                    for (int r = 0; r < 3; ++r) {
                        for (int col = 0; col < 3; ++col) {
                            const float a0 = anchor[r * 3 + 0];
                            const float a1 = anchor[r * 3 + 1];
                            const float a2 = anchor[r * 3 + 2];
                            m[r * 4 + col] = a0 * g_head_rot[0 * 3 + col] +
                                             a1 * g_head_rot[1 * 3 + col] +
                                             a2 * g_head_rot[2 * 3 + col];
                        }
                    }
                    for (int k = 0; k < 9; ++k) {
                        g_head_written[index][k] = m[(k / 3) * 4 + (k % 3)];
                    }
                    ++g_head_hits;
                    // Unconditional per-register write accounting, and a one-line report the
                    // first time each register is written. Without this, "the write never
                    // happened" and "the write happened and changed nothing" are
                    // indistinguishable in the log - and that ambiguity has already cost two
                    // runs.
                    {
                        const int wr = (int)(start + v);
                        if (wr >= 0 && wr < kMaxTrackedRegs) {
                            if (g_head_writes[wr] == 0) {
                                ++g_head_write_regs_seen;
                                VRLOG("head: FIRST WRITE to reg %d (kind=%s, count=%u) - rows "
                                      "(%.4f %.4f %.4f) (%.4f %.4f %.4f) (%.4f %.4f %.4f)",
                                      wr, c.kind, count,
                                      m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
                            }
                            ++g_head_writes[wr];
                        }
                    }
                    // Log the first rotation of each slot, and then once more much later:
                    // two samples of the same slot at different head poses are what show a
                    // human reader that the basis follows the head instead of drifting.
                    const bool first_for_slot = !g_head_logged_slot[index];
                    const bool late_sample = (g_head_hits >= 2000) && !g_head_late_logged[index];
                    if (first_for_slot || late_sample) {
                        g_head_logged_slot[index] = true;
                        if (late_sample) g_head_late_logged[index] = true;
                        VRLOG("head: frame %d view#%d reg %u %s (gain %.2f): rows "
                              "(%.4f %.4f %.4f) (%.4f %.4f %.4f) (%.4f %.4f %.4f), "
                              "translation (%.3f %.3f %.3f) unchanged",
                              g_frames_seen, index, start + v,
                              re_anchor ? "anchored+rotated" : "rotated", g_head_gain,
                              m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10],
                              m[12], m[13], m[14]);
                    }
                }


                // Remember which view slots this batch carried, to be folded into the
                // frame's ordering after the batch has been walked.
                if (index < kMaxShiftSlots) batch_has[index] = true;
            }
            // Fold this batch's slots into the frame ordering. A batch's matrices are
            // uploaded in ascending register order, so within a batch the higher slot is
            // the later upload; and every batch of this frame has been walked in call
            // order, so appending keeps the frame's true sequence. The final entry is
            // therefore the frame's last view matrix - which is the whole point, because
            // it cannot be known from inside the batch that carries it.
            for (int i = 0; i < kMaxShiftSlots; ++i) {
                if (!batch_has[i]) continue;
                if (g_head_order_len < kMaxShiftSlots) g_head_order[g_head_order_len++] = i;
                g_head_last_index = i;
            }
        }
    }
    return g_original_set_vs_const(dev, start, data, count);
}

} // namespace

// Resolves the marker files exactly once. They are files rather than more
// environment variables because the game is launched through Steam and Steam does
// not pass the shell's environment on.
//
// Two independent switches:
//   re6vr_capture_vs.txt   - classify and log the uploaded matrices (read-only)
//   re6vr_shift_view.txt   - the safety experiment; the file holds the offset in
//                            metres, e.g. "0.03"
void matrix_probe_resolve() {
    if (g_parsed) return;
    g_parsed = true;

    wchar_t buf[32] = L"";
    g_enabled = GetEnvironmentVariableW(L"RE6VR_CAPTURE_VS", buf, 32) > 0 && _wtoi(buf) != 0;

    const wchar_t *log_path = vrlog::path();
    if (!log_path || !log_path[0]) return;

    wchar_t dir[MAX_PATH] = L"";
    wcsncpy_s(dir, MAX_PATH, log_path, _TRUNCATE);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *(slash + 1) = L'\0';

    if (!g_enabled) {
        wchar_t marker[MAX_PATH] = L"";
        wcscpy_s(marker, MAX_PATH, dir);
        wcscat_s(marker, MAX_PATH, L"re6vr_capture_vs.txt");
        if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) {
            g_enabled = true;
            VRLOG("cap: found %ls", marker);
        }
    }
    if (g_enabled) {
        VRLOG("cap: capturing - classifying vertex shader constants");
        // The same switch drives the per-frame structural view of every view matrix,
        // which is the data a `pick` value has to be chosen from.
        g_shift_log_all = true;
    }

    wchar_t shift_marker[MAX_PATH] = L"";
    wcscpy_s(shift_marker, MAX_PATH, dir);
    wcscat_s(shift_marker, MAX_PATH, L"re6vr_shift_view.txt");
    if (GetFileAttributesW(shift_marker) != INVALID_FILE_ATTRIBUTES) {
        // Line 1: offset in metres. Optional line 2: which view matrix of the frame
        // to move (the frame holds several - main camera plus shadow/reflection
        // passes - and moving all of them draws the scene from several cameras at
        // once, which reads as a duplicated image). -1 = all, 0 = first, 1 = second,
        // and -2 = last. Absent means all, matching the first experiment.
        float v = 0.03f;
        int pick = -1;
        FILE *f = _wfopen(shift_marker, L"r");
        if (f) {
            if (fscanf_s(f, "%f", &v) != 1) v = 0.03f;
            if (fscanf_s(f, "%d", &pick) != 1) pick = -1;
            fclose(f);
        }
        if (v > 0.001f && v < 5.0f) {
            g_shift_metres = v;
            g_shift_pick = pick;
            g_shift_enabled = true;
            VRLOG("shift: ENABLED from %ls - view translation will be set to "
                  "original + %.3f m, target %s", shift_marker, v,
                  pick == -1 ? "every view matrix"
                             : (pick == -2 ? "the LAST view matrix of the frame"
                                           : "view matrix #"));
        } else {
            VRLOG("shift: %ls holds %.3f, outside the sane range - ignored", shift_marker, v);
        }
    }

    // re6vr_head_view.txt - head look. Contents: "<gain> [<target>]", where target is
    //   auto        find the camera by watching which slot's rotation moves (default)
    //   last        the frame's last view matrix
    //   all         every view matrix - measured to be wrong: it also turns HUD and
    //               sprite placements, which stretches the image instead of turning
    //   <n>         that view index
    // Gain scales the head rotation, which is also the quickest way to answer "is it
    // moving at all" without taking the headset off. A negative gain reverses the turn.
    wchar_t head_marker[MAX_PATH] = L"";
    wcscpy_s(head_marker, MAX_PATH, dir);
    wcscat_s(head_marker, MAX_PATH, L"re6vr_head_view.txt");
    if (GetFileAttributesW(head_marker) != INVALID_FILE_ATTRIBUTES) {
        float gain = 1.0f;
        int pick = -2;                  // -2 = the `auto` search
        char target[32] = "";
        FILE *f = _wfopen(head_marker, L"r");
        if (f) {
            if (fscanf_s(f, "%f", &gain) != 1) gain = 1.0f;
            if (fscanf_s(f, "%31s", target, (unsigned)_countof(target)) == 1) {
                if (_stricmp(target, "auto") == 0) pick = -2;
                else if (_stricmp(target, "all") == 0) pick = -1;
                else pick = atoi(target);
            }
            fclose(f);
        }
        if (gain > 0.01f && gain < 10.0f) {
            g_head_gain = gain;
            g_shift_pick = pick;
            g_head_enabled = true;
            VRLOG("head: ENABLED from %ls - the view matrix basis will be rotated by the "
                  "head orientation (gain %.2f, target %s)", head_marker, gain,
                  pick == -1 ? "every view matrix"
                             : (pick == -2 ? "auto - the slot whose rotation moves"
                                           : "view matrix #"));
        } else {
            VRLOG("head: %ls holds %.3f, outside the sane range - ignored", head_marker, gain);
        }
    }

    // re6vr_trace.txt - one line per view candidate per frame. This is the measurement
    // that identifies which register holds the world camera: it cannot be read off the
    // matrix's shape (a HUD placement is rigid too), so it has to be found by watching
    // which candidate moves while the player walks and looks around. Also switched on by
    // re6vr_capture_vs.txt, since capturing already implies "tell me where things are".
    {
        wchar_t trace_marker[MAX_PATH] = L"";
        wcscpy_s(trace_marker, MAX_PATH, dir);
        wcscat_s(trace_marker, MAX_PATH, L"re6vr_trace.txt");
        if (GetFileAttributesW(trace_marker) != INVALID_FILE_ATTRIBUTES) {
            g_trace_enabled = true;
            g_trace_frames = 0;
            VRLOG("trace: enabled from %ls - logging every view candidate per frame",
                  trace_marker);
        }
    }

    // re6vr_scan.txt - the camera-object hunt. It needs this probe's hook to see the view
    // matrices the engine uploads, so the marker has to enable the probe as well; without
    // that the scanner installed itself and then waited forever for a matrix that never
    // arrived, because the hook was never installed in the first place.
    {
        wchar_t scan_marker[MAX_PATH] = L"";
        wcscpy_s(scan_marker, MAX_PATH, dir);
        wcscat_s(scan_marker, MAX_PATH, L"re6vr_scan.txt");
        if (GetFileAttributesW(scan_marker) != INVALID_FILE_ATTRIBUTES) {
            g_enabled = true;
            VRLOG("scan: %ls present, so the vertex-constant hook is installed to feed it",
                  scan_marker);
        }
    }

    // Last, so a test rotation wins over whatever the headset would supply.
    matrix_probe_load_test_rotation();
}

// Called once per frame from the frame path with the head's orientation as a
// row-major 3x3 (columns = the head's right / up / forward axes).
//
// Stored unconditionally: this used to return early unless the OLD head-look path was enabled, but
// the consumer changed - the camera-steering probe (src/cam_steer.cpp) now needs the head
// orientation to drive the camera the renderer really reads, and whether that consumer wants to
// steer is its own decision. A bridge that only hands over data when a different consumer is armed
// is the kind of silent coupling this project has already paid for.
void matrix_probe_set_head_rotation(const float *r) {
    if (!r) return;
    for (int i = 0; i < 9; ++i) g_head_rot[i] = r[i];
    g_head_valid = true;
}

bool matrix_probe_head_rotation(float *out9) {
    if (!out9 || !g_head_valid) return false;
    for (int i = 0; i < 9; ++i) out9[i] = g_head_rot[i];
    return true;
}

bool matrix_probe_head_enabled() { return g_head_enabled; }

// Test hook: feed the head rotation from re6vr_head_test.txt instead of from the
// headset. Offline this is the only way to check the rotation maths - the whole
// point being that "the scene turns the right way" is otherwise only observable
// in the headset, and a wrong basis convention would look like "head tracking
// does nothing" rather than like a bug.
void matrix_probe_test_rotation(float *out9) {
    if (!out9) return;
    for (int i = 0; i < 9; ++i) out9[i] = g_head_rot[i];
}

void matrix_probe_load_test_rotation() {
    const wchar_t *log_path = vrlog::path();
    if (!log_path || !log_path[0]) return;
    wchar_t marker[MAX_PATH] = L"";
    wcsncpy_s(marker, MAX_PATH, log_path, _TRUNCATE);
    wchar_t *slash = wcsrchr(marker, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - marker)), L"re6vr_head_test.txt");
    if (GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES) return;
    FILE *f = _wfopen(marker, L"r");
    if (!f) return;
    float m[9];
    int got = 0;
    for (int i = 0; i < 9; ++i) {
        if (fscanf_s(f, "%f", &m[i]) != 1) break;
        ++got;
    }
    fclose(f);
    if (got == 9) {
        for (int i = 0; i < 9; ++i) g_head_rot[i] = m[i];
        VRLOG("head: test rotation loaded from %ls", marker);
    } else {
        VRLOG("head: %ls needs 9 numbers (row-major 3x3), got %d", marker, got);
    }
}

bool matrix_probe_enabled() {
    matrix_probe_resolve();
    return g_enabled;
}

void matrix_probe_frame() {
    // Only the global counter is advanced here; it numbers log lines and budgets the
    // reports. The per-thread frame boundary lives in the upload hook, because this
    // runs on the presenting thread while uploads come from more than one thread.
    //
    // This counter is deliberately advanced UNCONDITIONALLY. It used to move only when
    // re6vr_capture_vs.txt was set, and the write audit below is hung off it - so in the
    // "head look only" configuration, which is exactly how a head-tracking test is run, the
    // counter stayed at 0 and the audit never printed. A periodic report that depends on a
    // switch being set is a report that goes missing precisely when it is wanted.
    ++g_frames_seen;
    if (g_enabled || g_shift_log_all) g_frames_measured = g_frames_seen;

    // Periodic write audit: which registers head look has actually written, and how often.
    // This is the line that settles "did the experiment even touch anything".
    if (g_head_enabled && g_frames_seen >= g_head_write_report_frame + 600) {
        g_head_write_report_frame = g_frames_seen;
        char line[512] = "";
        int n = 0;
        for (int i = 0; i < kMaxTrackedRegs && n < (int)sizeof(line) - 24; ++i) {
            if (g_head_writes[i] == 0) continue;
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "reg %d x%d  ", i,
                             g_head_writes[i]);
        }
        VRLOG("head: write audit at frame %d - %d register(s) written: %s", g_frames_seen,
              g_head_write_regs_seen, line[0] ? line : "(NONE - head look never wrote "
                                                "anything)");
    }
}

void matrix_probe_install(IDirect3DDevice9 *dev) {
    matrix_probe_resolve();
    // EVERY switch that depends on this hook has to be in this guard. It has now been
    // wrong three times, each time with the same silent symptom: the switch reads its
    // marker file, logs that it is enabled, and then nothing at all happens, because the
    // hook was never installed and no code path ever asks why.
    //   * re6vr_head_view.txt was missing first (the head block was skipped);
    //   * re6vr_scan.txt was missing second (the scanner waited forever);
    //   * re6vr_trace.txt was missing third - a whole gameplay run produced zero trace
    //     lines, which is what prompted writing this list down.
    // The rule to apply when adding a switch: if it reads matrices, it belongs here.
    const bool wanted = g_enabled || g_shift_enabled || g_head_enabled || g_trace_enabled;
    if (!wanted || g_installed || !dev || !dev->lpVtbl) return;
    if (!dev->lpVtbl->SetVertexShaderConstantF) {
        VRLOG("cap: device has a null SetVertexShaderConstantF slot");
        return;
    }
    g_installed = true;
    g_original_set_vs_const = dev->lpVtbl->SetVertexShaderConstantF;

    DWORD old_protect = 0;
    if (VirtualProtect(&dev->lpVtbl->SetVertexShaderConstantF, sizeof(void *), PAGE_READWRITE,
                       &old_protect)) {
        dev->lpVtbl->SetVertexShaderConstantF = &hooked_SetVertexShaderConstantF;
        VirtualProtect(&dev->lpVtbl->SetVertexShaderConstantF, sizeof(void *), old_protect,
                       &old_protect);
        VRLOG("cap: SetVertexShaderConstantF patched (original %p)",
              (void *)g_original_set_vs_const);
    } else {
        g_installed = false;
        VRLOG("cap: VirtualProtect failed on the vtable slot");
    }
}

} // namespace re6vr
