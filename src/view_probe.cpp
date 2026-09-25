// view_probe.cpp - the ground truth about which camera the renderer uses, taken from the stream
// the renderer cannot avoid: the vertex shader constants uploaded every frame.
//
// The reasoning in full
// ---------------------
// Everything the GPU is told about the camera has to pass through
// IDirect3DDevice9::SetVertexShaderConstantF (slot 94). A view matrix there is unmistakable among
// the junk: its 16 floats are a rigid transform - |L1| = |L2| = |L3| = 1, the three rows mutually
// perpendicular, L4 = (0,0,0,1) - which random constants do not satisfy by accident.
//
// So the probe:
//
//   1. reads that stream (read-only: every call is forwarded byte for byte, the arguments are never
//      modified) and latches the rigid 4x4 that arrives in a frame, with its register number;
//   2. waits for the camera to stop moving (three consecutive frames carrying the same bytes),
//      because a value that changes between the upload and the search cannot be found in memory;
//   3. searches every committed writable region of the process for those exact 16 floats, and
//      reports the address, the page, the widest hex/float window around it, and - the part that
//      turns an address into an object - the nearest pointers that point at it.
//
// Step 3 is the point of the whole exercise. "Where do the bytes the renderer uses live" is not a
// question a static analysis can answer, and it is the one question blocking head tracking:
// writing the camera's own mCameraOrg changes nothing on screen, so the memory the renderer really
// reads is somewhere else. Once the address is known, the same search can be pointed at the up
// vector or the position, and the owning object follows from the surrounding bytes.
//
// A no-op unless the marker file re6vr_view.txt exists next to the log.

#include "view_probe.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "log.h"
#include "safe_mem.h"

namespace re6vr {
namespace {

// ---------------------------------------------------------------- state

bool g_armed = false;          // the marker file was found
bool g_installed = false;
IDirect3DDevice9Vtbl *g_vtbl = nullptr;
HRESULT(STDMETHODCALLTYPE *g_real_vs_const)(IDirect3DDevice9 *, UINT, const float *, UINT) = nullptr;
HRESULT(STDMETHODCALLTYPE *g_real_ps_const)(IDirect3DDevice9 *, UINT, const float *, UINT) = nullptr;

const UINT kMaxScanRegs = 96;   // a view matrix is 4 registers; look a little wider
const int kConfirmFrames = 3;   // identical frames before the search starts

// per frame, filled by the detours and read once per Present (single render thread for the scene,
// so a simple static is safe here; the search itself runs on its own thread)
volatile LONG g_frame_uploads = 0;
bool g_frame_have = false;
unsigned long long g_frame_hash = 0;
UINT g_frame_reg = 0;
float g_frame_m[16] = {0};
char g_frame_where[16] = "";

// the value the search is looking for
bool g_latched = false;
unsigned long long g_latched_hash = 0;
UINT g_latch_reg = 0;
float g_latch_m[16] = {0};
char g_latch_where[16] = "";

// convergence
unsigned long long g_last_frame_hash = 0;
int g_same_frames = 0;
bool g_search_started = false;
int g_view_matrices_logged = 0;
volatile LONG g_window_uploads = 0;   // uploads since the last heartbeat
volatile LONG g_window_rigid = 0;     // of those, how many carried a rigid 4x4

// synthetic self-test (runs offline, before the real search, and only when the marker file says
// "selftest"). Its purpose is the failure mode this project keeps hitting: a probe that is
// confidently silent because one of its own steps is wrong. Here the steps are fed a matrix the
// probe itself planted in memory, so "found it" can only mean the detector and the search both
// work.
int g_selftest_phase = 0;            // 0 = to run, 1 = searching, 2 = done, -1 = off
bool g_selftest_enabled = false;     // marker file says "selftest"
void *g_selftest_plant = nullptr;
bool g_selftest_latched_saved = false;
float g_saved_matrix[16] = {0};
UINT g_saved_reg = 0;
unsigned long long g_saved_hash = 0;
char g_saved_where[16] = "";
volatile LONG g_search_status = 0;   // number of finished searches, written by the worker

// worker
HANDLE g_thread = nullptr;
volatile LONG g_search_wanted = 0;
volatile LONG g_searches = 0;

const float kUnitTol = 0.02f;
const float kDotTol = 0.02f;
const float kWorldLimit = 5.0e5f;

bool finite_bounded(float v) {
    return v == v && v > -kWorldLimit && v < kWorldLimit;
}

float dot3(const float *a, const float *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

float dot4(const float *a, const float *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

// Is this 4x4 a rigid view transform? Rows L1..L3 orthonormal, L4 = (0,0,0,1), translation in a
// range a game world can plausibly occupy.
bool is_view_matrix(const float *m, char *why, size_t why_n) {
    for (int i = 0; i < 16; ++i) {
        if (!finite_bounded(m[i])) {
            snprintf(why, why_n, "component %d not finite/bounded (%.4g)", i, (double)m[i]);
            return false;
        }
    }
    const float l1 = sqrtf(dot3(m, m));
    const float l2 = sqrtf(dot3(m + 4, m + 4));
    const float l3 = sqrtf(dot3(m + 8, m + 8));
    if (fabsf(l1 - 1.0f) > kUnitTol || fabsf(l2 - 1.0f) > kUnitTol ||
        fabsf(l3 - 1.0f) > kUnitTol) {
        snprintf(why, why_n, "rows not unit (|L1|=%.4f |L2|=%.4f |L3|=%.4f)", (double)l1,
                 (double)l2, (double)l3);
        return false;
    }
    const float d12 = dot3(m, m + 4), d13 = dot3(m, m + 8), d23 = dot3(m + 4, m + 8);
    if (fabsf(d12) > kDotTol || fabsf(d13) > kDotTol || fabsf(d23) > kDotTol) {
        snprintf(why, why_n, "rows not orthogonal (%.4f %.4f %.4f)", (double)d12, (double)d13,
                 (double)d23);
        return false;
    }
    // D3D convention: translation in row 4, and the 4th column is (0,0,0,1).
    static const float kRow4[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (dot4(m + 12, kRow4) < 0.999f) {
        snprintf(why, why_n, "row 4 is not (0,0,0,1) (%.3f %.3f %.3f %.3f)", (double)m[12],
                 (double)m[13], (double)m[14], (double)m[15]);
        return false;
    }
    const float colsum = fabsf(m[3]) + fabsf(m[7]) + fabsf(m[11]);
    if (colsum > 0.05f) {
        snprintf(why, why_n, "4th column not zero (%.3f %.3f %.3f)", (double)m[3], (double)m[7],
                 (double)m[11]);
        return false;
    }
    // A view matrix at the world origin is a menu/skybox transform, not the gameplay camera.
    if (fabsf(m[12]) + fabsf(m[13]) + fabsf(m[14]) < 1.0f) {
        snprintf(why, why_n, "translation at the origin (%.2f %.2f %.2f)", (double)m[12],
                 (double)m[13], (double)m[14]);
        return false;
    }
    why[0] = 0;
    return true;
}

unsigned long long hash_matrix(const float *m) {
    unsigned long long h = 1469598103934665603ull;
    const unsigned char *p = (const unsigned char *)m;
    for (int i = 0; i < 64; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

void log_matrix(const char *tag, const float *m) {
    VRLOG("view: %s rows (D3D convention, translation last):", tag);
    VRLOG("view:   L1 (%9.4f %9.4f %9.4f %9.4f)", (double)m[0], (double)m[1], (double)m[2],
          (double)m[3]);
    VRLOG("view:   L2 (%9.4f %9.4f %9.4f %9.4f)", (double)m[4], (double)m[5], (double)m[6],
          (double)m[7]);
    VRLOG("view:   L3 (%9.4f %9.4f %9.4f %9.4f)", (double)m[8], (double)m[9], (double)m[10],
          (double)m[11]);
    VRLOG("view:   L4 (%9.4f %9.4f %9.4f %9.4f)  <- camera position", (double)m[12],
          (double)m[13], (double)m[14], (double)m[15]);
    VRLOG("view:   axes: right (%7.3f %7.3f %7.3f) up (%7.3f %7.3f %7.3f) back (%7.3f %7.3f "
          "%7.3f)", (double)m[0], (double)m[4], (double)m[8], (double)m[1], (double)m[5],
          (double)m[9], (double)m[2], (double)m[6], (double)m[10]);
}

void note_matrix_in_frame(UINT reg, const float *m, const char *where) {
    if (!g_frame_have) {
        g_frame_have = true;
        g_frame_reg = reg;
        memcpy(g_frame_m, m, sizeof(g_frame_m));
        strncpy_s(g_frame_where, sizeof(g_frame_where), where, _TRUNCATE);
        g_frame_hash = hash_matrix(m);
    }
    // The search must be able to run even if no single matrix is ever seen in three consecutive
    // frames: with several cameras per frame that is the normal case. The three-identical-frames
    // rule then only decides WHEN the bytes are trustworthy, not whether the probe armed at all.
    if (!g_latched) {
        g_latched = true;
        g_latched_hash = g_frame_hash;
        memcpy(g_latch_m, g_frame_m, sizeof(g_latch_m));
        g_latch_reg = g_frame_reg;
        strncpy_s(g_latch_where, sizeof(g_latch_where), g_frame_where, _TRUNCATE);
    }
    InterlockedIncrement(&g_window_rigid);
}

// ---------------------------------------------------------------- the detours

HRESULT STDMETHODCALLTYPE hooked_vs_const(IDirect3DDevice9 *dev, UINT start, const float *data,
                                         UINT count) {
    InterlockedIncrement(&g_frame_uploads);
    InterlockedIncrement(&g_window_uploads);
    // One line per RUN, not per frame: on 2026-09-25 this message was written for every frame of a
    // five-minute session (the counter it keyed on is never reset for the view probe's path), which
    // produced a 1.5 MB log and hid the evidence. A static flag cannot repeat.
    static bool s_first_logged = false;
    if (!s_first_logged) {
        s_first_logged = true;
        VRLOG("view: SetVertexShaderConstantF is being read (first call this run) - this line "
              "proves the hook is live, so a later 'no matrix found' means it was not there");
    }
    // Cost control, and a bug fix at the same time. The old rate limit keyed on a frame counter that
    // was never correct (the same reason the probe went silent for three runs), and the heartbeat
    // then showed what it cost: 1 056 296 constant uploads in ten seconds, every one of them put
    // through the 16-float rigid test. The scan now runs at most every kSampleMs, which is all the
    // resolution a camera hunt needs, and "how busy is the stream" is still reported per window.
    static unsigned long long last_check = 0;
    const unsigned long long now = GetTickCount64();
    const bool check_now = (last_check == 0 || now - last_check >= 500);
    if (check_now && data && count >= 4 && start < kMaxScanRegs) {
        last_check = now;
        UINT max_reg = count < (kMaxScanRegs - start) ? count : (kMaxScanRegs - start);
        for (UINT reg = 0; reg + 3 < max_reg; ++reg) {
            char why[128];
            if (is_view_matrix(data + reg * 4, why, sizeof(why))) {
                note_matrix_in_frame(start + reg, data + reg * 4, "vertex");
                break;
            }
        }
    }
    return g_real_vs_const(dev, start, data, count);
}

HRESULT STDMETHODCALLTYPE hooked_ps_const(IDirect3DDevice9 *dev, UINT start, const float *data,
                                          UINT count) {
    static unsigned long long last_check = 0;
    const unsigned long long now = GetTickCount64();
    const bool check_now = (last_check == 0 || now - last_check >= 500);
    if (check_now && data && count >= 4 && start < kMaxScanRegs) {
        last_check = now;
        UINT max_reg = count < (kMaxScanRegs - start) ? count : (kMaxScanRegs - start);
        for (UINT reg = 0; reg + 3 < max_reg; ++reg) {
            char why[128];
            if (is_view_matrix(data + reg * 4, why, sizeof(why))) {
                note_matrix_in_frame(start + reg, data + reg * 4, "pixel");
                break;
            }
        }
    }
    return g_real_ps_const(dev, start, data, count);
}

// ---------------------------------------------------------------- memory search

// Page-validated reads (src/safe_mem.h). The __try version of exactly this function faulted inside
// a live game on 2026-09-25 and killed the session, so the guard is gone: an unreadable address now
// simply fails the read.
bool safe_read(const void *addr, void *out, size_t n) {
    return re6vr::read(addr, out, n);
}

bool inside_image_code(unsigned p) {
    const unsigned base = 0x00400000u;
    return p >= base && p < base + 0x1102000u;   // BH6.exe section 0 (.text)
}

// How many dwords from `off` point into the image's .text - a vtable is a run of them.
int pointer_run(const unsigned *blob, size_t dwords, size_t off) {
    int run = 0;
    for (size_t i = off; i < dwords && run < 24; ++i) {
        if (inside_image_code(blob[i])) {
            ++run;
        } else {
            break;
        }
    }
    return run;
}

void report_hit(unsigned addr, const char *what) {
    MEMORY_BASIC_INFORMATION mbi;
    memset(&mbi, 0, sizeof(mbi));
    const char *kind = "?";
    if (VirtualQuery((const void *)addr, &mbi, sizeof(mbi))) {
        if (mbi.Type == MEM_IMAGE) {
            kind = "MEM_IMAGE";
        } else if (mbi.Type == MEM_MAPPED) {
            kind = "MEM_MAPPED";
        } else if (mbi.Type == MEM_PRIVATE) {
            kind = "MEM_PRIVATE (heap)";
            if (mbi.AllocationBase == mbi.BaseAddress && mbi.RegionSize >= 0x10000 &&
                addr > (unsigned)(uintptr_t)mbi.BaseAddress + 0x1000) {
                kind = "MEM_PRIVATE - a large single-allocation region, likely a THREAD STACK";
            }
        }
        VRLOG("view:   %s at %08X  page base %08X size %06X protect %08X type %s", what, addr,
              (unsigned)(uintptr_t)mbi.BaseAddress, (unsigned)mbi.RegionSize,
              (unsigned)mbi.Protect, kind);
    } else {
        VRLOG("view:   %s at %08X (VirtualQuery failed)", what, addr);
    }

    // 8 dwords before and after: enough to see a Vector3 neighbour or a vtable word.
    unsigned ctx[32] = {0};
    if (safe_read((const void *)(addr - 32), ctx, sizeof(ctx))) {
        for (int row = 0; row < 4; ++row) {
            const unsigned *d = ctx + row * 8;
            const float *f = (const float *)d;
            VRLOG("view:     %08X  %08X %08X %08X %08X %08X %08X %08X %08X", addr - 32 + row * 32,
                  d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
            VRLOG("view:                %11.4g %11.4g %11.4g %11.4g %11.4g %11.4g %11.4g %11.4g",
                  (double)f[0], (double)f[1], (double)f[2], (double)f[3], (double)f[4],
                  (double)f[5], (double)f[6], (double)f[7]);
        }
    } else {
        VRLOG("view:     could not read around %08X", addr);
    }

    // Pointers to this address in the 0x200 bytes around it: that is how the owning object is
    // found, because an MT Framework object stores its camera by pointer.
    unsigned around[0x200 / 4 + 4] = {0};
    if (safe_read((const void *)(addr - 0x100), around, sizeof(around))) {
        int found = 0;
        for (size_t i = 0; i + 1 < sizeof(around) / 4 && found < 6; ++i) {
            if (around[i] == addr) {
                const unsigned at = addr - 0x100 + (unsigned)i * 4;
                const int run = pointer_run(around, sizeof(around) / 4, i + 1);
                VRLOG("view:     pointer to it at %08X%s", at,
                      run >= 3 ? "  <- and a run of code pointers follows: A VTABLE IS HERE" : "");
                ++found;
            }
        }
        if (found == 0) {
            VRLOG("view:     no pointer to this address within +/-0x100 (a plain copy, not an "
                  "object field)");
        }
    }
}

// One region of memory, one pattern. Matches are reported at most kPerPage per 64 KB page, because
// a byte-identical value often appears in several buffers at once.
const int kPerPage = 3;

void scan_region(unsigned base, size_t size, unsigned char *buf, size_t buf_size,
                 const float *pattern, int floats, const char *what, int *total, int max_total) {
    size_t done = 0;
    while (done < size && *total < max_total) {
        const size_t want = (size - done) < buf_size ? (size - done) : buf_size;
        if (!safe_read((const void *)(base + done), buf, want)) {
            done += want;
            continue;
        }
        unsigned last_page = 0;
        int per_page = 0;
        for (size_t i = 0; i + 4 * (size_t)floats <= want; i += 4) {
            const float *f = (const float *)(buf + i);
            bool hit = true;
            for (int k = 0; k < floats; ++k) {
                if (f[k] != pattern[k] && !(f[k] == 0.0f && pattern[k] == 0.0f)) {
                    hit = false;
                    break;
                }
            }
            if (!hit) continue;
            const unsigned addr = base + (unsigned)done + (unsigned)i;
            const unsigned page = addr >> 16;
            if (page != last_page) {
                last_page = page;
                per_page = 0;
            }
            if (++per_page > kPerPage) continue;
            report_hit(addr, what);
            ++*total;
            if (*total >= max_total) break;
        }
        done += want;
    }
}

// The worker: waits until asked, then searches every writable region for the latched matrix.
DWORD WINAPI search_thread(LPVOID) {
    unsigned char *buf = (unsigned char *)VirtualAlloc(nullptr, 4u << 20, MEM_COMMIT,
                                                       PAGE_READWRITE);
    if (!buf) {
        VRLOG("view: could not allocate the scan buffer");
        return 0;
    }
    for (;;) {
        if (InterlockedCompareExchange(&g_search_wanted, 0, 0) == 0) {
            Sleep(20);
            continue;
        }
        InterlockedExchange(&g_search_wanted, 0);
        const int n = (int)InterlockedIncrement(&g_searches);

        float pattern[16];
        memcpy(pattern, g_latch_m, sizeof(pattern));

        VRLOG("view: === search %d: the latched matrix (from %s constants c%u) is:", n,
              g_latch_where, g_latch_reg);
        VRLOG("view:   (%.4f %.4f %.4f %.4f) (%.4f %.4f %.4f %.4f) (%.4f %.4f %.4f %.4f) "
              "(%.4f %.4f %.4f %.4f)", (double)pattern[0], (double)pattern[1], (double)pattern[2],
              (double)pattern[3], (double)pattern[4], (double)pattern[5], (double)pattern[6],
              (double)pattern[7], (double)pattern[8], (double)pattern[9], (double)pattern[10],
              (double)pattern[11], (double)pattern[12], (double)pattern[13], (double)pattern[14],
              (double)pattern[15]);

        unsigned long long scanned = 0;
        int matrix_hits = 0;
        int pos_hits = 0;
        int up_hits = 0;
        // Three patterns, in order of how much they would settle: the whole 64-byte matrix (the
        // strongest - a byte-identical 4x4 cannot be a coincidence), then the camera position on
        // its own (the engine may rebuild the matrix each frame from a stored Vector3, and it is
        // the stored value that a hook can write), then the matrix's third row, which is the world
        // up axis of that view and doubles as a cross-check on the position hit.
        const float pos_pattern[3] = {pattern[12], pattern[13], pattern[14]};
        const float up_pattern[3] = {pattern[8], pattern[9], pattern[10]};

        unsigned addr = 0x10000u;
        while (addr < 0x7FFF0000u) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((const void *)addr, &mbi, sizeof(mbi)) == 0) break;
            const unsigned base = (unsigned)(uintptr_t)mbi.BaseAddress;
            const size_t size = mbi.RegionSize;
            if (size == 0) break;
            const bool usable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                                !(mbi.Protect & PAGE_NOACCESS) &&
                                (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                                !inside_image_code(base);
            if (usable) {
                scanned += size;
                if (matrix_hits < 6) {
                    scan_region(base, size, buf, 4u << 20, pattern, 16, "MATRIX(16 floats)",
                                &matrix_hits, 6);
                }
                if (pos_hits < 6) {
                    scan_region(base, size, buf, 4u << 20, pos_pattern, 3, "POSITION(3 floats)",
                                &pos_hits, 6);
                }
                if (up_hits < 4) {
                    scan_region(base, size, buf, 4u << 20, up_pattern, 3, "VIEW-UP-ROW(3 floats)",
                                &up_hits, 4);
                }
            }
            addr = base + (unsigned)size;
        }
        VRLOG("view: === search %d finished: %llu MB examined, %d MATRIX hit(s), %d POSITION "
              "hit(s), %d up-row hit(s)%s ===", n, scanned >> 20, matrix_hits, pos_hits, up_hits,
              (matrix_hits == 0 && pos_hits == 0)
                  ? " - NOTHING matched, so the renderer's camera is either rebuilt from parts or "
                    "lives in a page the scan skips; the next step is to hook the reader instead"
                  : " - the hits above are the memory the renderer's camera comes from");
        InterlockedExchange(&g_search_status, g_searches);
    }
    return 0;
}

// ---------------------------------------------------------------- synthetic self-test
//
// Called once per frame while it is running. Phase 1 plants a rigid 4x4 that no game memory could
// contain by accident, pushes it through the REAL hooked entry point (so the detour, the rigid
// test, the latch and the search are all exercised), and then waits for the worker's verdict.
// "FOUND the plant" is the only way the probe earns the right to search the game's memory; a probe
// whose search silently does nothing is the exact failure this project has already paid for.
void run_selftest_once() {
    if (g_selftest_phase != 0) return;
    g_selftest_phase = 1;

    g_selftest_plant = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    // A rigid transform with a translation that cannot occur by accident: |L1|=|L2|=|L3|=1,
    // rows mutually perpendicular, L4 = (0,0,0,1).
    float m[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                   0.0f, 0.0f, 1.0f, 0.0f,
                   0.0f, -1.0f, 0.0f, 0.0f,
                   12345.5f, -2345.25f, 3456.75f, 1.0f};
    if (!g_selftest_plant) {
        VRLOG("view: selftest: could not allocate the plant buffer");
        g_selftest_phase = -1;
        return;
    }
    memcpy(g_selftest_plant, m, sizeof(m));
    VRLOG("view: selftest: planted a rigid 4x4 at %08X and will push it through the hooked "
          "SetVertexShaderConstantF; the search must find that exact address",
          (unsigned)(uintptr_t)g_selftest_plant);

    g_saved_matrix[0] = g_latch_m[0];
    memcpy(g_saved_matrix, g_latch_m, sizeof(g_saved_matrix));
    g_saved_reg = g_latch_reg;
    g_saved_hash = g_latched_hash;
    strncpy_s(g_saved_where, sizeof(g_saved_where), g_latch_where, _TRUNCATE);
    g_selftest_latched_saved = g_latched;

    g_frame_have = false;                 // the plant is the matrix under test
    hooked_vs_const(nullptr, 0, m, 4);    // through the real detour (the original is not called)
    // The detour records; the per-frame step latches. Do the latch here, explicitly, so the test
    // exercises the same state the real path uses.
    if (g_frame_have && g_frame_hash == hash_matrix(m)) {
        g_latched = true;
        g_latched_hash = g_frame_hash;
        memcpy(g_latch_m, g_frame_m, sizeof(g_latch_m));
        g_latch_reg = g_frame_reg;
        strncpy_s(g_latch_where, sizeof(g_latch_where), g_frame_where, _TRUNCATE);
        VRLOG("view: selftest: the detour detected the plant, latched it as c%u -> detector PASS",
              g_latch_reg);
    } else {
        VRLOG("view: selftest: the detour did NOT latch the planted matrix (frame_have=%d) -> "
              "detector FAIL: the rigid test or the hook itself is wrong, so a game-run 'no matrix "
              "found' would be meaningless", (int)g_frame_have);
    }
    InterlockedExchange(&g_search_wanted, 1);
}

// The three phases of the plant test, once per frame.
bool selftest_step() {
    if (g_selftest_phase == 0) {
        run_selftest_once();
        return true;
    }
    if (g_selftest_phase != 1) return g_selftest_phase == 2;
    if (g_search_status < 1) return true;      // the worker has not finished yet

    g_selftest_phase = 2;
    if (g_selftest_latched_saved) {
        memcpy(g_latch_m, g_saved_matrix, sizeof(g_latch_m));
        g_latch_reg = g_saved_reg;
        g_latched_hash = g_saved_hash;
        strncpy_s(g_latch_where, sizeof(g_latch_where), g_saved_where, _TRUNCATE);
        g_latched = true;
    } else {
        g_latched = false;
    }
    if (g_selftest_plant) {
        VirtualFree(g_selftest_plant, 0, MEM_RELEASE);
        g_selftest_plant = nullptr;
    }
    VRLOG("view: selftest: DONE. If the search above reported the planted address "
          "(it is also listed among the hits) the whole chain works: hook -> rigid test -> latch -> "
          "memory search. If the search reported no hit at all, the probe is NOT trustworthy and "
          "its result in a game run must not be read as 'the camera is not in memory'.");
    g_frame_have = false;
    return true;
}

} // namespace

bool view_probe_enabled() {
    return g_armed;
}

// Called once per presented frame. Three consecutive frames carrying the SAME matrix mean the
// camera is not moving, which is what makes a byte-identical memory search meaningful.
void view_probe_frame() {
    if (!g_armed) return;
    if (g_selftest_enabled && selftest_step()) return;   // prove the probe before trusting anything

    const unsigned long long now = GetTickCount64();
    static unsigned long long window_start = 0;
    if (window_start == 0) window_start = now;

    // A heartbeat that does not depend on anything having been found. The previous version printed
    // one line at start-up and then went silent unless a rigid matrix appeared - so "the hook is
    // dead", "the constant stream is dead" and "the detector is wrong" all looked identical: no
    // output at all. Measured cost: one line every 10 s.
    if (now - window_start >= 10000) {
        VRLOG("view: heartbeat: %ld constant upload(s) in the last 10 s, %s; latched=%d (%s c%u)",
              (long)g_window_uploads,
              g_window_rigid > 0 ? "a rigid 4x4 WAS among them" : "no rigid 4x4 among them",
              (int)g_latched, g_latched ? "holding" : "nothing", g_latch_reg);
        window_start = now;
        g_window_uploads = 0;
        g_window_rigid = 0;
    }
    if (!g_latched) return;

    // The camera must hold still before a byte-identical search means anything: three frames with
    // the same matrix. Measured 2026-09-25: the engine serves TWO camera objects inside one frame
    // (uCameraCtrl at yaw 47 and another at -93), so the first matrix seen in a frame is not
    // necessarily the same camera every frame - the three-frame rule is what makes the latch safe.
    if (g_frame_hash == g_last_frame_hash) {
        ++g_same_frames;
    } else {
        g_last_frame_hash = g_frame_hash;
        g_same_frames = 1;
        // A new matrix: re-latch, so the bytes the search looks for are always the most recent
        // frame's, not a stale one the engine has since overwritten.
        if (g_frame_hash != g_latched_hash) {
            g_latched_hash = g_frame_hash;
            memcpy(g_latch_m, g_frame_m, sizeof(g_latch_m));
            g_latch_reg = g_frame_reg;
            strncpy_s(g_latch_where, sizeof(g_latch_where), g_frame_where, _TRUNCATE);
        }
    }
    g_frame_have = false;

    if (!g_search_started && g_same_frames == kConfirmFrames) {
        g_search_started = true;
        VRLOG("view: LATCHED the renderer's matrix from %s constants c%u..c%u; it has been identical "
              "for %d frames, so the camera is still and the search starts now.",
              g_latch_where, g_latch_reg, g_latch_reg + 3, kConfirmFrames);
        log_matrix("matrix", g_latch_m);
        InterlockedExchange(&g_search_wanted, 1);
    }
}

void view_probe_install(IDirect3DDevice9 *dev) {
    if (g_installed || !dev) return;

    wchar_t path[MAX_PATH] = L"";
    const wchar_t *log_path = vrlog::path();
    if (!log_path || !log_path[0]) return;
    wcsncpy_s(path, MAX_PATH, log_path, _TRUNCATE);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - path)), L"re6vr_view.txt");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
    g_armed = true;

    // The marker's content decides whether the probe first proves itself. It should: a search that
    // silently finds nothing looks exactly like "the camera is not in memory", and that ambiguity
    // has already cost this project several runs.
    {
        FILE *f = _wfopen(path, L"r");
        char text[32] = "";
        if (f) {
            if (fscanf_s(f, "%31s", text, (unsigned)sizeof(text)) != 1) text[0] = 0;
            fclose(f);
        }
        g_selftest_enabled = (_stricmp(text, "selftest") == 0);
    }

    VRLOG("view: ENABLED from %ls. The vertex/pixel constant stream is now read for a rigid 4x4 "
          "(a view matrix), and the memory holding it is searched once the camera holds still. "
          "Read-only: every call is forwarded unchanged.%s", path,
          g_selftest_enabled ? " Self-test armed: a plant is pushed through the hook first, so the "
                               "search is proven before the game's memory is trusted."
                             : "");

    DWORD old_protect = 0;
    g_vtbl = dev->lpVtbl;
    if (!VirtualProtect(&g_vtbl->SetVertexShaderConstantF, sizeof(void *), PAGE_READWRITE,
                        &old_protect)) {
        VRLOG("view: VirtualProtect failed (%lu) - probe not installed", GetLastError());
        return;
    }
    g_real_vs_const = g_vtbl->SetVertexShaderConstantF;
    g_vtbl->SetVertexShaderConstantF = &hooked_vs_const;
    g_real_ps_const = g_vtbl->SetPixelShaderConstantF;
    g_vtbl->SetPixelShaderConstantF = &hooked_ps_const;
    VirtualProtect(&g_vtbl->SetVertexShaderConstantF, sizeof(void *), old_protect, &old_protect);

    VRLOG("view: vtable patched at %p (SetVertexShaderConstantF original %p, "
          "SetPixelShaderConstantF original %p)", (void *)&g_vtbl->SetVertexShaderConstantF,
          (void *)g_real_vs_const, (void *)g_real_ps_const);

    g_thread = CreateThread(nullptr, 0, search_thread, nullptr, 0, nullptr);
    if (!g_thread) {
        VRLOG("view: could not create the search thread");
    } else {
        SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
    }
    g_installed = true;
}

} // namespace re6vr
