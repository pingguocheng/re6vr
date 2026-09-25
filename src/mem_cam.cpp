// mem_cam.cpp - the runtime half of "where is the camera, and is it live?".
//
// The static half (scripts/disasm_lib/propmap.py) recovered the field offsets from BH6's own
// property-registration code. This half answers the question static analysis cannot: does a
// live sBioCamera exist, where is it, and does its mCameraOrg[0] actually hold a camera pose?
//
// Everything here is read-only. The probe never writes to the game's memory: the point is to
// establish the object and the offsets before anything is written, because this project has
// already paid five times for treating a plausible-looking candidate as an answer.
//
// See mem_cam.h for the identification argument (class pointer, not shape).

#include "mem_cam.h"
#include "mem_cam_tables.h"
#include "screenshot.h"

#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

#include "log.h"
#include "safe_mem.h"

namespace re6vr {
namespace {

// ---------------------------------------------------------------- static facts (verified)
//
// All of these come from BH6.exe itself, not from DMC4 and not from a guess:
//   * class records and sizes: scripts/disasm_lib/dti.py (MtDti registration template)
//   * instance layout:        scripts/disasm_lib/propmap.py (property-registration template)
// `re6dis.py props sBioCamera` reprints the whole table.
const unsigned kVtSlots = 8;                  // enough to cover vtable + class ptr + 2 fields

const unsigned kDtiBioCamera = 0x017C3164u;   // sBioCamera
const unsigned kDtiCameraCtrl = 0x017D26F0u;  // uCameraCtrl (the camera manager, 0x4B70 bytes)

const unsigned kCameraOrgBase = 0xE30u;       // mCameraOrg[0].cameraPos
const unsigned kCameraOrgStride = 0x40u;      // i = 0..7

const unsigned kOffClassPtr = 0x04u;          // instance + 4 = its MtDti record

// How far a planted fixture object has to reach to cover the camera fields (+0xE30 + 15 floats).
const size_t kPlantSize = kCameraOrgBase + 16 * 4;

// The three candidate globals the static notes left open, to be settled by one read each.
const unsigned kGlobalCameraCtrlSlot = 0x017D270Cu;   // [this] -> uCameraCtrl instance?
const unsigned kGlobalStageObject = 0x017CF454u;      // stage object, used all over stage code
const unsigned kStageCameraOffset = 0x640u;           // [stage + 0x640] -> uCameraCtrl

// The marker file, in the same directory as the log (see vrlog::path).
const wchar_t *kMarker = L"re6vr_cam.txt";

// The probe scans on a schedule; nothing gates it. Three sessions were spent learning that a
// gate which never opens is indistinguishable from a probe that does not work.
const unsigned kFirstScanMs = 30000;      // let the game get past the first cutscene
const unsigned kScanRetryMs = 30000;      // between whole-process scans
const int kScanAttempts = 20;

// How many verified instances to describe in full. The first few are the ones worth reading;
// a full list would bury the log.
const int kMaxDetailed = 6;

// ---------------------------------------------------------------- state
bool g_enabled = false;
bool g_running = false;
HANDLE g_thread = nullptr;
unsigned g_module_base = 0;

struct Instance {
    void *addr;
    bool geometry_ok;
    float pos[3], target[3], up[3], fov, near_plane, far_plane;
};

// ---------------------------------------------------------------- safe memory access
//
// These used to be `__try/__except` wrappers. On 2026-09-25 the crash reporter showed the wrappers
// THEMSELVES faulting inside a live game (0xC0000005 at the `mov eax,[eax]` of safe_read_u32 and at
// the byte probe of source_object), which killed the session - a guard that can kill the process
// is not a guard. They now validate the page with VirtualQuery first (src/safe_mem.h) and only
// then touch memory, so an untrusted pointer simply fails the read.
//
// The old rule this file was written around no longer applies either: with no __try there is no
// reason to keep every function free of C++ objects.

bool safe_read(const void *addr, void *out, size_t n) {
    return read(addr, out, n);
}

bool safe_read_u32(unsigned addr, unsigned *out) {
    return read32(addr, out);
}

bool safe_read_f32(unsigned addr, float *out) {
    return readf32(addr, out);
}

// Read a whole region into the caller's buffer. Returns how many bytes were readable and
// contiguous from the start (0 when the very first byte is not readable), so a region with an
// unreadable hole inside it costs one skipped chunk instead of the rest of the region.
size_t safe_read_region(const void *addr, void *out, size_t n) {
    const size_t got = readable_prefix(addr, n);
    if (got == 0) return 0;
    memcpy(out, addr, got);
    return got;
}

bool region_readable(const void *addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    if (mbi.Protect & PAGE_NOACCESS) return false;
    return (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
}

// ---------------------------------------------------------------- the game's sections
//
// The first real run exposed a false-positive class the static notes could not have predicted:
// searching for `+0x04 == sBioCamera's class record` also matches the *metadata tables* that
// hold class-record pointers. Two hits from that run show it plainly - one at 0x01814840 whose
// head reads `017C1ABC 017C3164 017D2780 017C1674` (three DTI records 0x20 apart, i.e. an
// sBioCamera/sCamera/uCameraCtrl run in a table), and one at 0x0186E268, which is *inside* the
// sCamera class record itself (0x0186E260).
//
// So "the class pointer matches" is not enough. A real instance also has a **vtable pointing
// into executable memory**, and it lives on the **heap**, not inside the image. Both facts are
// free to check once the image's own sections are known, and both are exactly what separates
// "an object the engine built" from "a table that mentions the class".
struct Section {
    unsigned va, vsize, rawsize;
    bool executable;
    bool mapped;          // rawsize > 0: has bytes on disk (not BSS)
};

Section g_sections[16] = {};
int g_section_count = 0;

void read_pe_sections() {
    g_section_count = 0;
    if (!g_module_base) return;
    const unsigned char *base = (const unsigned char *)(uintptr_t)g_module_base;
    unsigned char head[0x1000];
    if (!safe_read(base, head, sizeof(head))) return;
    if (head[0] != 'M' || head[1] != 'Z') return;
    unsigned pe_off = 0;
    memcpy(&pe_off, head + 0x3C, 4);
    if (pe_off + 0x2C > sizeof(head)) return;
    if (memcmp(head + pe_off, "PE\0\0", 4) != 0) return;
    const unsigned char *coff = head + pe_off + 4;
    unsigned nsec = 0, opt_size = 0;
    memcpy(&nsec, coff + 2, 2);
    memcpy(&opt_size, coff + 16, 2);
    const unsigned char *opt = coff + 20;
    unsigned char *sec = (unsigned char *)(opt + opt_size);
    if ((unsigned char *)sec + nsec * 40 > head + sizeof(head)) return;
    for (unsigned i = 0; i < nsec && i < 16; ++i, sec += 40) {
        unsigned va = 0, vsize = 0, rawsize = 0, chars = 0;
        memcpy(&vsize, sec + 8, 4);
        memcpy(&va, sec + 12, 4);
        memcpy(&rawsize, sec + 16, 4);
        memcpy(&chars, sec + 36, 4);
        g_sections[i].va = va;
        g_sections[i].vsize = vsize;
        g_sections[i].rawsize = rawsize;
        g_sections[i].executable = (chars & 0x20000000u) != 0;   // IMAGE_SCN_MEM_EXECUTE
        g_sections[i].mapped = rawsize > 0;
        ++g_section_count;
    }
    VRLOG("cam: image sections parsed: %d", g_section_count);
}

// Is this VA inside the image, in a section that has bytes on disk?
bool inside_image_data(unsigned p) {
    if (!g_module_base || p < g_module_base) return false;
    const unsigned rva = p - g_module_base;
    for (int i = 0; i < g_section_count; ++i) {
        const Section &s = g_sections[i];
        if (rva >= s.va && rva < s.va + (s.vsize ? s.vsize : s.rawsize)) return s.mapped;
    }
    return false;
}

// Does this VA point into an EXECUTABLE section of the image? That is what a vtable is.
bool inside_image_code(unsigned p) {
    if (!g_module_base || p < g_module_base) return false;
    const unsigned rva = p - g_module_base;
    for (int i = 0; i < g_section_count; ++i) {
        const Section &s = g_sections[i];
        if (rva >= s.va && rva < s.va + (s.vsize ? s.vsize : s.rawsize)) return s.executable;
    }
    return false;
}

// Any address inside an executable section of THIS module, for fixtures that need a plausible
// vtable. The camera self-test used to borrow sBioCamera's real vtable address plus the base
// delta - correct inside the game, nonsense inside the offline harness, which is a different
// image entirely. The planted object was then rejected for a reason that had nothing to do with
// the search, and the self-test failed (it caught its own fixture's assumption, which is the
// point of having one).
unsigned first_executable_address() {
    if (!g_module_base || !g_section_count) return 0;
    for (int i = 0; i < g_section_count; ++i) {
        if (g_sections[i].executable) return g_module_base + g_sections[i].va;
    }
    return 0;
}

// Is this VA inside the image's own data (its class records, string literals, tables)?
bool inside_image(unsigned p) {
    if (!g_module_base || p < g_module_base) return false;
    const unsigned rva = p - g_module_base;
    for (int i = 0; i < g_section_count; ++i) {
        const Section &s = g_sections[i];
        if (rva >= s.va && rva < s.va + (s.vsize ? s.vsize : s.rawsize)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- a second identifying key
//
// Everything above keys on `+0x04 == class record`. Two real sessions - the second one 8400
// frames inside a level - found zero sBioCamera objects that way, and zero objects of any other
// camera class, including uCameraAnimation, which the engine cannot run without. A search that
// finds nothing anywhere says something about the SEARCH, not about the camera.
//
// mem_cam_tables.h carries the other static fact: each camera class's vtable VA, harvested by
// the same analysis that produced the field offsets. An object's vtable is its first dword, so
// searching for a known vtable finds instances directly - and each hit then reports what its
// +0x04 actually says, which settles the layout question instead of assuming it.

// A census over the known vtables: for every camera class, how many objects claim it.
//
// One pass over memory, and the answer is a list of names with counts - the difference between
// "the engine built nothing" (impossible) and "the engine built these, and here is what their
// class pointer looks like".
void vtable_census() {
    unsigned char *chunk = (unsigned char *)VirtualAlloc(nullptr, 4u << 20, MEM_COMMIT,
                                                        PAGE_READWRITE);
    if (!chunk) return;

    std::vector<int> counts(kKnownClassCount, 0);
    std::vector<unsigned> first(kKnownClassCount, 0);
    std::vector<int> dti_agrees(kKnownClassCount, 0);
    int scanned_mb = 0;

    unsigned addr = 0x10000u;
    while (g_running && addr < 0x7FFF0000u) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((const void *)addr, &mbi, sizeof(mbi)) == 0) break;
        const unsigned base = (unsigned)mbi.BaseAddress;
        const size_t size = (size_t)mbi.RegionSize;
        if (size == 0) break;
        const bool usable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                            !(mbi.Protect & PAGE_NOACCESS) &&
                            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                            !(g_section_count && inside_image_code(base));
        if (usable) {
            size_t done = 0;
            while (done < size && g_running) {
                const size_t want = (size - done) < (4u << 20) ? (size - done) : (4u << 20);
                const size_t got = safe_read_region((const void *)(base + done), chunk, want);
                if (got == 0) { done += want; continue; }
                scanned_mb += (int)(got >> 20);
                for (size_t i = 0; i + 8 <= got; i += 4) {
                    unsigned vt = 0, cls = 0;
                    memcpy(&vt, chunk + i, 4);
                    memcpy(&cls, chunk + i + kOffClassPtr, 4);
                    for (int c = 0; c < kKnownClassCount; ++c) {
                        if (vt != kKnownClasses[c].vtable) continue;
                        // The vtable value also appears in the image's own vtable tables; those
                        // are skipped above. A hit out here is an object whose first dword is a
                        // known vtable, which is what an instance looks like.
                        if (counts[c] < 1000000) ++counts[c];
                        if (!first[c]) first[c] = base + (unsigned)done + (unsigned)i;
                        if (cls == kKnownClasses[c].dti) ++dti_agrees[c];
                    }
                }
                done += got;
            }
        }
        addr = base + (unsigned)size;
    }
    VirtualFree(chunk, 0, MEM_RELEASE);

    VRLOG("cam: vtable census (%d MB examined, %d camera classes known statically):",
          scanned_mb, kKnownClassCount);
    int total = 0;
    for (int c = 0; c < kKnownClassCount; ++c) {
        total += counts[c];
        if (counts[c] == 0) continue;
        VRLOG("cam:   %-28s vtable %08X  count %d  (of those, %d also carry the expected class "
              "record at +4)  first at %08X",
              kKnownClasses[c].name, kKnownClasses[c].vtable, counts[c], dti_agrees[c],
              first[c]);
    }
    if (total == 0) {
        VRLOG("cam: the vtable census found NO object of any known camera class. That is not a "
              "statement about the camera: it means the game had not built one yet at this point "
              "(menus/loading), or none of these classes is what this build instantiates.");
    }
}

struct Instance;

// Declared here because the vtable-keyed search is defined above the geometry check, and a
// forward declaration is cheaper to keep correct than a reordering that invites drift.
bool check_camera_geometry(unsigned addr, Instance *out, char *why, size_t why_n);
void dump_object(void *addr, const char *why);
void probe_guard_selftest() {
    unsigned v = 0xDEADBEEFu;
    float f = 0.0f;
    const bool u32_refused = !safe_read_u32(0x1u, &v);
    const bool f32_refused = !safe_read_f32(0x1u, &f);
    const bool obvious = (v == 0xDEADBEEFu);
    // And a positive control: an address that certainly IS readable must still be read, or "the
    // guard refuses everything" would look like success.
    const unsigned here = (unsigned)(uintptr_t)&v;
    unsigned back = 0;
    const bool real_read_works = safe_read_u32(here, &back) && back == 0xDEADBEEFu;
    VRLOG("cam: memory-guard selftest: read32(0x1) refused=%d, readf32(0x1) refused=%d, value "
          "untouched=%d, real read works=%d -> %s", (int)u32_refused, (int)f32_refused,
          (int)obvious, (int)real_read_works,
          (u32_refused && f32_refused && obvious && real_read_works) ? "PASS" : "FAIL");
    (void)f;
}
void dump_camera_entries(void *addr, const char *why);

// Find instances of the known camera classes by their VTABLE, not by `+4`.
//
// This is the search that should have been first. `+4 == class record` is the MT Framework
// layout as documented for a sibling title, and it has now produced zero hits in three real
// sessions - so either the layout differs in this build or the object is not built when the
// probe looks. The vtable, by contrast, is a value this project verified statically
// (mem_cam_tables.h) and it sits at +0x00 of every instance by definition of a vtable.
//
// A hit is accepted only with the full geometry pass, so a data copy of a vtable pointer cannot
// become a conclusion. For every class the function reports `+4` as an OBSERVATION: whatever it
// says is the answer to "what does this build actually put there", which is the question the
// class-record key kept failing to answer.
int find_by_vtable(std::vector<Instance> *out, int *vtable_hits, int *geom_rejected,
                   unsigned *per_class_first, int *per_class_count,
                   const unsigned *extra_vtables = nullptr, int extra_count = 0,
                   unsigned *per_class_valid_first = nullptr,
                   int *per_class_valid_count = nullptr) {
    unsigned char *chunk = (unsigned char *)VirtualAlloc(nullptr, 4u << 20, MEM_COMMIT,
                                                        PAGE_READWRITE);
    if (!chunk) return 0;
    for (int c = 0; c < kKnownClassCount; ++c) {
        per_class_first[c] = 0;
        per_class_count[c] = 0;
        if (per_class_valid_first) per_class_valid_first[c] = 0;
        if (per_class_valid_count) per_class_valid_count[c] = 0;
    }
    const unsigned delta = g_module_base >= 0x400000u ? g_module_base - 0x400000u : 0u;
    int verified = 0;

    unsigned addr = 0x10000u;
    while (g_running && addr < 0x7FFF0000u) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((const void *)addr, &mbi, sizeof(mbi)) == 0) break;
        const unsigned base = (unsigned)mbi.BaseAddress;
        const size_t size = (size_t)mbi.RegionSize;
        if (size == 0) break;
        const bool usable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                            !(mbi.Protect & PAGE_NOACCESS) &&
                            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                            !(g_section_count && inside_image_code(base));
        if (usable) {
            size_t done = 0;
            while (done < size && g_running) {
                const size_t want = (size - done) < (4u << 20) ? (size - done) : (4u << 20);
                const size_t got = safe_read_region((const void *)(base + done), chunk, want);
                if (got == 0) { done += want; continue; }
                for (size_t i = 0; i + 8 <= got; i += 4) {
                    unsigned vt = 0;
                    memcpy(&vt, chunk + i, 4);
                    int cls = -1;
                    for (int c = 0; c < kKnownClassCount; ++c) {
                        if (vt == kKnownClasses[c].vtable + delta) { cls = c; break; }
                    }
                    if (cls < 0 && extra_vtables) {
                        for (int e = 0; e < extra_count; ++e) {
                            if (vt == extra_vtables[e]) { cls = 0; break; }   // fixture: sBioCamera
                        }
                    }
                    if (cls < 0) continue;
                    ++*vtable_hits;
                    if (!per_class_first[cls]) per_class_first[cls] = base + (unsigned)done +
                                                                      (unsigned)i;
                    if (per_class_count[cls] < 1000000) ++per_class_count[cls];

                    const unsigned obj = base + (unsigned)done + (unsigned)i;
                    Instance ins;
                    memset(&ins, 0, sizeof(ins));
                    ins.addr = (void *)obj;
                    char why[192] = "";
                    if (!check_camera_geometry(obj, &ins, why, sizeof(why))) {
                        if (*geom_rejected < 16) {
                            ++*geom_rejected;
                            unsigned cls_ptr = 0;
                            safe_read_u32(obj + kOffClassPtr, &cls_ptr);
                            const bool agrees = cls_ptr == kKnownClasses[cls].dti + delta;
                            VRLOG("cam:   %s at %08X (+4 = %08X%s) failed geometry: %s",
                                  kKnownClasses[cls].name, obj, cls_ptr,
                                  agrees ? ", which IS the matching class record" : "", why);
                        }
                        // The camera itself gets a full entry dump, because identity is settled
                        // for it (its vtable is the static one) and the only open question is
                        // where its fields are. For every other class one line is enough.
                        if (cls == 0) {
                            dump_camera_entries((void *)obj, "sBioCamera: where is the live pose?");
                        }
                        continue;
                    }
                    out->push_back(ins);
                    ++verified;
                    unsigned cls_ptr = 0;
                    safe_read_u32(obj + kOffClassPtr, &cls_ptr);
                    VRLOG("cam: *** %s INSTANCE at %08X - vtable matches the static table, "
                          "geometry OK. +4 = %08X (the image's class record is %08X). ***",
                          kKnownClasses[cls].name, obj, cls_ptr,
                          kKnownClasses[cls].dti + delta);
                    if (!per_class_valid_first[cls]) per_class_valid_first[cls] = obj;
                    if (per_class_valid_count[cls] < 1000000) ++per_class_valid_count[cls];
                }
                done += got;
            }
        }
        addr = base + (unsigned)size;
    }
    VirtualFree(chunk, 0, MEM_RELEASE);
    return verified;
}

// Dump an object's bytes when its identity is certain but its layout is not.
//
// The run of 2026-09-25 00:16 produced exactly that case: vtable `0151A380` (sBioCamera's, from
// the static table) AND `+4 == 017C3164` (sBioCamera's class record) at the same address - two
// independent identifiers agreeing - while `+0xE30` held no camera pose. When identity is that
// solid and the fields are not where the property table says, the useful next fact is the bytes:
// header, and the neighbourhood of the offset the property table named.
void dump_object(void *addr, const char *why) {
    const unsigned a = (unsigned)(uintptr_t)addr;
    VRLOG("cam: object dump at %08X (%s)", a, why);
    unsigned head[8] = {0};
    if (safe_read((const void *)a, head, sizeof(head))) {
        VRLOG("cam:   +0x00 vtable  %08X", head[0]);
        VRLOG("cam:   +0x04 record  %08X", head[1]);
        VRLOG("cam:   +0x08 %08X   +0x0C %08X   +0x10 %08X   +0x14 %08X", head[2], head[3],
              head[4], head[5]);
        VRLOG("cam:   +0x18 %08X   +0x1C %08X", head[6], head[7]);
    }
    float f = 0.0f;
    const unsigned probes[] = {0x00, 0x04, 0x30, 0x40, 0x50, 0xE30, 0xE40, 0xE50, 0xE60, 0xE64,
                               0xE68, 0x1030, 0x1040, 0x1050, 0x12A0};
    for (int i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); ++i) {
        const unsigned off = probes[i];
        if (!safe_read_f32(a + off, &f)) {
            VRLOG("cam:   +0x%-5X unreadable", off);
            continue;
        }
        unsigned raw = 0;
        safe_read_u32(a + off, &raw);
        VRLOG("cam:   +0x%-5X %08X  (%g)", off, raw, (double)f);
    }
    // A window around the camera fields: 32 dwords either side of +0xE30 shows whether the table
    // is displaced, or simply not filled in yet.
    for (unsigned base = 0xE00; base <= 0xE60; base += 0x10) {
        unsigned row[4] = {0};
        if (!safe_read((const void *)(a + base), row, sizeof(row))) break;
        VRLOG("cam:   [%08X+%X] %08X %08X %08X %08X", a, base, row[0], row[1], row[2], row[3]);
    }
}

// A dump of the eight mCameraOrg entries, and the same geometry test applied to each.
//
// This is what the run of 2026-09-25 00:25 needs: a live sBioCamera WAS found (vtable
// 0x0151A380, the static one) and the game WAS rendering a level (a screenshot at that moment
// shows the dining hall), yet `mCameraOrg[0]` held nothing. Two explanations remain, and one
// measurement separates them:
//
//   * the renderer uses a different entry - then one of entries 1..7 is the valid pose;
//   * the property-table offsets are wrong for this build - then none of them is, and the
//     header dump is the evidence to re-derive the layout from.
//
// It also reports what the class record pointer actually holds, because that turned out to be a
// surprise worth measuring rather than assuming: the live objects point at 0x1F47A060, a runtime
// COPY of the class record, while the image's own 0x017C3164 sits in a table elsewhere.
void dump_camera_entries(void *addr, const char *why) {
    const unsigned a = (unsigned)(uintptr_t)addr;
    VRLOG("cam: entry dump at %08X (%s)", a, why);
    unsigned head[6] = {0};
    if (safe_read((const void *)a, head, sizeof(head))) {
        VRLOG("cam:   +0x00 vtable %08X   +0x04 record %08X   +0x08 %08X   +0x0C %08X   "
              "+0x10 %08X   +0x14 %08X", head[0], head[1], head[2], head[3], head[4], head[5]);
    }
    for (int i = 0; i < 8; ++i) {
        const unsigned base = kCameraOrgBase + kCameraOrgStride * (unsigned)i;
        float v[15] = {0};
        if (!safe_read((const void *)(a + base), v, sizeof(v))) {
            VRLOG("cam:   entry %d (+0x%X): unreadable", i, base);
            continue;
        }
        const float dx = v[4] - v[0], dy = v[5] - v[1], dz = v[6] - v[2];
        const float flen = sqrtf(dx * dx + dy * dy + dz * dz);
        const float ulen = sqrtf(v[8] * v[8] + v[9] * v[9] + v[10] * v[10]);
        VRLOG("cam:   entry %d (+0x%X): pos (%.2f %.2f %.2f) target (%.2f %.2f %.2f) "
              "up len %.3f fwd len %.2f fov %.4f near %.3f far %.2f", i, base, v[0], v[1], v[2],
              v[4], v[5], v[6], ulen, flen, v[12], v[13], v[14]);
    }
    // The other offsets the property table names, in case the whole table is displaced.
    const unsigned probes[] = {0x30, 0x40, 0x50, 0xD80, 0x1030, 0x1040, 0x1050, 0x12A0};
    for (int i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); ++i) {
        float f = 0.0f;
        const unsigned off = probes[i];
        if (!safe_read_f32(a + off, &f)) {
            VRLOG("cam:   +0x%-5X unreadable", off);
        } else {
            unsigned raw = 0;
            safe_read_u32(a + off, &raw);
            VRLOG("cam:   +0x%-5X %08X  (%g)", off, raw, (double)f);
        }
    }
}

// ---------------------------------------------------------------- helpers

// Is this float a real number in a range a direction/coordinate could live in? Rejects NaN and
// infinity as a side effect, which matters: an uninitialised region full of infinities matched
// every tolerance test this project wrote until close_enough() grew the same guard.
bool sane_float(float v, float limit) {
    return (v == v) && (v > -limit) && (v < limit);
}

bool unit3(const float *v, float tol) {
    const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return fabsf(len - 1.0f) < tol && sane_float(len, 100.0f);
}

// Is `p` plausibly a vtable: inside the module and inside a non-executable (data) section?
//
// This is deliberately loose about WHICH data section: the check exists to separate "a pointer
// into the image's data" from "a heap pointer / garbage", not to pin the exact vtable address.
// The exact vtable address is reported separately, because that comparison (against 0x151A380
// plus the relocation delta) is the one that can be checked by hand.
bool looks_like_vtable(unsigned p) {
    // The strong form when the image layout is known: a vtable points at CODE inside the image.
    // "readable and somewhere in the module" is not enough - that definition accepted a pointer
    // into `.data`, which is where the class-record tables live, and those tables are exactly
    // what the search kept hitting on the first real run.
    if (g_section_count) return inside_image_code(p);
    if (!g_module_base) return p != 0 && region_readable((const void *)p);
    if (p < g_module_base || p > g_module_base + 0x2000000u) return false;
    return region_readable((const void *)p);
}

// Every camera-family class record the static work recovered, for the class-keyed census.
const unsigned kCameraClasses[] = {
    0x017C3164u,   // sBioCamera
    0x0186E260u,   // sCamera (renderer camera)
    0x017D26F0u,   // uCameraCtrl (manager)
    0x017D1DD0u,   // uCameraBase
    0x017D2A00u,   // uCameraQFPS
    0x017D1D44u,   // uCameraAnimation
};
const int kCameraClassCount = (int)(sizeof(kCameraClasses) / sizeof(kCameraClasses[0]));

const char *class_name_for(unsigned dti) {
    switch (dti) {
        case kDtiBioCamera:  return "sBioCamera";
        case kDtiCameraCtrl: return "uCameraCtrl";
        default:             return nullptr;
    }
}

// A census of what the search is missing: how many addresses claim each camera class. This is
// what turns a bare "found nothing" into a named answer - a game that built a uCameraQFPS and no
// sBioCamera is a different problem from a game that built nothing - and it is cheap, because it
// never touches the geometry.
//
// It shares `find_instances` with `count_only = true` rather than walking memory a second time:
// two walkers would be two things to keep in agreement.

// Read the object head and report the class it claims to be. Returns the class record, or 0.
unsigned object_class(unsigned addr, unsigned *vtable_out) {
    unsigned vt = 0, dti = 0;
    if (!safe_read_u32(addr, &vt)) return 0;
    if (!safe_read_u32(addr + kOffClassPtr, &dti)) return 0;
    if (vtable_out) *vtable_out = vt;
    return dti;
}

// The geometry check: does [addr + 0xE30] hold a real look-at camera pose?
//
// Every condition is a NECESSARY property of a look-at parameterisation, not a heuristic
// threshold: up is a unit vector, forward is a unit vector perpendicular to up, fov is a
// plausible lens angle, and the near/far planes are ordered. A wrong object cannot satisfy all
// of them; a right one cannot fail them.
//
// `why` receives the names of the conditions that failed, so a miss says WHICH test disagreed
// instead of only that something did - the difference between "the offsets are wrong" and "this
// candidate is not a camera at all".
bool check_camera_geometry(unsigned addr, Instance *out, char *why, size_t why_n) {
    float v[15] = {0};
    if (!safe_read((const void *)(addr + kCameraOrgBase), v, sizeof(v))) {
        _snprintf_s(why, why_n, _TRUNCATE, "unreadable at +0x%X", kCameraOrgBase);
        return false;
    }
    const float *p = v + 0, *t = v + 4, *u = v + 8;
    const float fov = v[12], near_plane = v[13], far_plane = v[14];

    for (int i = 0; i < 15; ++i) {
        if (!sane_float(v[i], 1.0e6f)) {
            _snprintf_s(why, why_n, _TRUNCATE, "field %d not finite/bounded (%g)", i,
                        (double)v[i]);
            return false;
        }
    }
    const float dx = t[0] - p[0], dy = t[1] - p[1], dz = t[2] - p[2];
    const float flen = sqrtf(dx * dx + dy * dy + dz * dz);
    const bool fwd_ok = flen > 0.1f && flen < 100000.0f;
    const bool up_ok = unit3(u, 1e-3f);
    const bool fov_ok = fov > 0.05f && fov < 2.2f;
    const bool planes_ok = near_plane > 0.0f && far_plane > near_plane &&
                           far_plane < 1.0e6f;
    bool perp = false;
    float dot = 0.0f;
    if (fwd_ok && up_ok) {
        dot = (dx / flen) * u[0] + (dy / flen) * u[1] + (dz / flen) * u[2];
        perp = fabsf(dot) < 0.15f;
    }

    if (out) {
        out->addr = (void *)addr;
        out->geometry_ok = fwd_ok && up_ok && fov_ok && planes_ok && perp;
        memcpy(out->pos, p, sizeof(out->pos));
        memcpy(out->target, t, sizeof(out->target));
        memcpy(out->up, u, sizeof(out->up));
        out->fov = fov;
        out->near_plane = near_plane;
        out->far_plane = far_plane;
    }
    if (!(fwd_ok && up_ok && fov_ok && planes_ok && perp)) {
        _snprintf_s(why, why_n, _TRUNCATE,
                    "%s%s%s%s%s (forward %.3f, up len %.3f, fov %.4f, near %.3f far %.3f)",
                    fwd_ok ? "" : "forward!unit ", up_ok ? "" : "up!unit ",
                    fov_ok ? "" : "fov!range ", planes_ok ? "" : "planes!order ",
                    perp ? "" : "forward.up!=0", flen,
                    sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]), fov, near_plane, far_plane);
        return false;
    }
    if (why && why_n) why[0] = 0;
    return true;
}

// `%f` is locale-dependent and this project's logs are read by scripts: print floats by hand.
void fmt3(char *dst, size_t n, const float *v) {
    _snprintf_s(dst, n, _TRUNCATE, "(%.2f %.2f %.2f)", v[0], v[1], v[2]);
}

// ---------------------------------------------------------------- description

void describe_instance(unsigned addr, const Instance &ins, unsigned vtable) {
    char pos[64], tgt[64], up[64];
    fmt3(pos, sizeof(pos), ins.pos);
    fmt3(tgt, sizeof(tgt), ins.target);
    fmt3(up, sizeof(up), ins.up);
    VRLOG("cam: obj %p  vtable=%08X  class=sBioCamera  mCameraOrg[0]:", (void *)addr, vtable);
    VRLOG("cam:     cameraPos %s  targetPos %s  cameraUp %s", pos, tgt, up);
    VRLOG("cam:     fov %.4f rad (%.1f deg)  near %.3f  far %.2f   forward len %.2f",
          ins.fov, ins.fov * 57.2957795f, ins.near_plane, ins.far_plane,
          sqrtf((ins.target[0] - ins.pos[0]) * (ins.target[0] - ins.pos[0]) +
                (ins.target[1] - ins.pos[1]) * (ins.target[1] - ins.pos[1]) +
                (ins.target[2] - ins.pos[2]) * (ins.target[2] - ins.pos[2])));
    VRLOG("cam:     geometry %s - %s", ins.geometry_ok ? "OK" : "FAILED",
          ins.geometry_ok
              ? "so this object is a LIVE look-at camera and +0xE40 (targetPos) / +0xE50 "
                "(cameraUp) are the fields to write for head look"
              : "so it is an sBioCamera-typed object whose camera fields are not a valid pose "
                "(uninitialised, or a different entry is the live one)");
}

// ---------------------------------------------------------------- candidate globals
//
// Each of these settles one of the open questions in the README, and each is reported as three
// separate facts (the raw value, whether it points at a readable object, and what class that
// object claims) so a null in one column cannot be read as a verdict on the others.

void probe_global(const char *what, unsigned global_va, unsigned expect_class) {
    unsigned value = 0;
    if (!safe_read_u32(global_va, &value)) {
        VRLOG("cam: %-28s [%08X] unreadable (not mapped)", what, global_va);
        return;
    }
    if (value == 0) {
        VRLOG("cam: %-28s [%08X] = 0  -> NOT a live instance slot", what, global_va);
        return;
    }
    unsigned vt = 0;
    const unsigned dti = object_class(value, &vt);
    const char *cls = class_name_for(dti);
    VRLOG("cam: %-28s [%08X] = %08X -> vtable %08X  class %s", what, global_va, value, vt,
          cls ? cls : "(not a camera class record)");
    if (expect_class && dti == expect_class) {
        VRLOG("cam: *** %s IS a live %s instance: the instance-pointer hypothesis holds ***",
              what, cls);
    } else if (expect_class) {
        VRLOG("cam: %s does not point at a %s (class record reads %08X, expected %08X) - so it "
              "is a context/owner global, exactly as disasm_lib/managers.py warned",
              what, class_name_for(expect_class), dti, expect_class);
    }
}

// ---------------------------------------------------------------- the scan

// Walk the process's committed memory looking for objects of camera class `needle`.
//
// The class pointer is the identifying fact, not a shape: an orthonormal triple matches many
// things, while `*(u32*)(addr+4) == <a class record>` matches only objects the engine built as
// that class. Byte alignment is not required (an object can start anywhere), so the walk
// compares at every byte offset - it is a memcmp-shaped search, not a dword scan.
//
// `count_only` skips the geometry pass and just counts, which is what the census at the end of a
// fruitless run uses: "the engine has N objects of this class but none of them has a valid pose"
// and "the engine has none at all" are different problems.
int find_instances(std::vector<Instance> *out, int *raw_hits, int *rejected,
                   unsigned needle, bool count_only) {
    unsigned char *chunk = (unsigned char *)VirtualAlloc(nullptr, 4u << 20, MEM_COMMIT,
                                                        PAGE_READWRITE);
    if (!chunk) {
        VRLOG("cam: could not allocate a scan buffer");
        return 0;
    }

    unsigned addr = 0x10000u;               // below this nothing is ever allocated
    const unsigned limit = 0x7FFF0000u;     // 32-bit user space
    unsigned long long scanned = 0;
    unsigned long long last_report = 0;
    int verified = 0;

    while (g_running && addr < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((const void *)addr, &mbi, sizeof(mbi)) == 0) break;
        const unsigned base = (unsigned)mbi.BaseAddress;
        const size_t size = (size_t)mbi.RegionSize;
        if (size == 0) break;

        const bool usable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                            !(mbi.Protect & PAGE_NOACCESS) &&
                            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                            // Skip only the image's EXECUTABLE sections (`.text`): they hold no
                            // objects, and walking 17 MB of code costs time for nothing. The
                            // image's data must stay in the scan: skip-by-section was tried and
                            // is wrong, because the game's heap is allocated as an image region
                            // too, so skipping everything inside the image would skip exactly
                            // where the objects are. The metadata tables are excluded by the
                            // object checks instead (a real instance has a vtable pointing at
                            // code), which is what those checks are for.
                            !(g_section_count && inside_image_code(base));
        if (usable) {
            size_t done = 0;
            while (done < size && g_running) {
                const size_t want = (size - done) < (4u << 20) ? (size - done) : (4u << 20);
                const size_t got = safe_read_region((const void *)(base + done), chunk, want);
                if (got == 0) { done += want; continue; }   // hole: step over it
                scanned += got;
                for (size_t i = 0; i + 8 <= got; ++i) {
                    unsigned dti = 0;
                    memcpy(&dti, chunk + i + kOffClassPtr, 4);
                    if (dti != needle) continue;
                    ++*raw_hits;
                    const unsigned obj = base + (unsigned)done + (unsigned)i;
                    // Guard the class-pointer read again: a match can sit inside a string or
                    // in a page that turned unreadable since the chunk was copied.
                    if (object_class(obj, nullptr) != needle) continue;
                    // The match can also be a *copy* of the class pointer inside some other
                    // object's data (an object list, a factory cache), or one of the image's own
                    // class-record tables: the first real run's hits at 0x01814840 and 0x0186E268
                    // were exactly that (runs of DTI records 0x20 apart). A real instance also has
                    // a vtable, and a vtable points at CODE - so requiring that separates "an
                    // object the engine built" from "a table that mentions the class".
                    unsigned vt = 0;
                    if (!safe_read_u32(obj, &vt) || !looks_like_vtable(vt)) continue;
                    if (count_only) {
                        // The census only wants "does the engine have objects of this class":
                        // one line per object, no geometry, and the address so the caller can
                        // walk it by hand later.
                        if (out->size() < 24) {
                            Instance brief;
                            memset(&brief, 0, sizeof(brief));
                            brief.addr = (void *)obj;
                            out->push_back(brief);
                            VRLOG("cam:   census: %08X has class %08X at +4 and a code vtable "
                                  "%08X", obj, needle, vt);
                        }
                        ++verified;
                        continue;
                    }
                    Instance ins;
                    memset(&ins, 0, sizeof(ins));
                    ins.addr = (void *)obj;
                    char why[192] = "";
                    if (!check_camera_geometry(obj, &ins, why, sizeof(why))) {
                        // A class-pointer match with the wrong geometry is worth one line: it
                        // separates "the offsets are wrong" from "this is not the live object".
                        if (*rejected < 12) {
                            ++*rejected;
                            unsigned head[4] = {0};
                            unsigned raw4[4] = {0};
                            float f4[4] = {0};
                            safe_read((const void *)obj, head, sizeof(head));
                            safe_read((const void *)(obj + kCameraOrgBase), raw4, sizeof(raw4));
                            memcpy(f4, raw4, sizeof(f4));
                            VRLOG("cam:   class-pointer match at %08X (head %08X %08X %08X %08X) "
                                  "but geometry failed: %s", obj, head[0], head[1], head[2],
                                  head[3], why);
                            VRLOG("cam:     [obj+0xE30] = %08X %08X %08X %08X (as float %.2f "
                                  "%.2f %.2f %.2f); region base %08X, chunk off %X, byte off %X",
                                  raw4[0], raw4[1], raw4[2], raw4[3], f4[0], f4[1], f4[2], f4[3],
                                  base, (unsigned)done, (unsigned)i);
                        }
                        continue;
                    }
                    out->push_back(ins);
                    ++verified;
                }
                done += got;
            }
            if (scanned - last_report > (256u << 20)) {
                last_report = scanned;
                VRLOG("cam: scan progress: %llu MB examined, %d candidate object(s)",
                      scanned >> 20, verified);
            }
        }
        addr = base + (unsigned)size;
    }
    VirtualFree(chunk, 0, MEM_RELEASE);
    VRLOG("cam: scan finished: %llu MB examined, %d raw class-pointer hit(s)", scanned >> 20,
          *raw_hits);
    return verified;
}

// ---------------------------------------------------------------- thread

// The stage gate, and why it must NOT be the only path to a scan.
//
// The idea was sound: wait for the engine's own "a stage exists" signal (`[0x17CF454] + 0x640`,
// the pointer the stage loader stores the uCameraCtrl at) instead of a timer, so a scan cannot
// land on a menu. The run of 2026-09-24 23:42 then showed the flaw: the player WAS in a level -
// 4800 frames submitted, head tracking live - and that pointer stayed 0 for the entire session,
// so the gate never opened and the probe never scanned at all. A gate that can silently swallow
// a whole run is worse than no gate.
//
// So the gate is now only an *accelerator*: it shortens the wait when it opens, and the probe
// scans on its own schedule either way. Its value is still reported, because "did [stage+0x640]
// ever become non-zero" is itself one of the questions the static notes left open.
// A cheap, engine-provided "something changed" signal for the scan schedule.
//
// The report from the player is what this is for: loading a save shows a cutscene first and the
// game only becomes controllable afterwards, and even walking through a door stops for dialogue.
// A probe that scans on a fixed schedule therefore spends its budget on states where a camera
// object legitimately does not exist yet (or no longer does) and then reports "found nothing" -
// which is what the 23:49 run did, twelve times.
//
// The stage pointer is the signal: it changes when the stage changes, and a change is exactly
// when a camera object is created. It is NOT a level-loaded gate (measured: `[stage+0x640]` is 0
// even in game - that route is dead). The pointer itself moving is all this needs.
unsigned stage_marker() {
    unsigned stage = 0;
    if (!safe_read_u32(kGlobalStageObject, &stage)) return 0;
    unsigned cam = 0;
    if (stage) safe_read_u32(stage + kStageCameraOffset, &cam);
    return stage ^ (cam * 2654435761u);      // a change detector, not a meaning
}

bool stage_ready() {
    unsigned stage = 0;
    if (!safe_read_u32(kGlobalStageObject, &stage) || !stage) return false;
    unsigned cam = 0;
    return safe_read_u32(stage + kStageCameraOffset, &cam) && cam != 0;
}

DWORD WINAPI cam_thread(LPVOID) {
    // The game's module base, taken BEFORE anything uses it. `find_instances` needs it to
    // relocate the class-pointer needle, and an image that is not loaded at its preferred base
    // would otherwise be searched for with the wrong constant and report a clean zero.
    const HMODULE game = GetModuleHandleW(L"BH6.exe");
    g_module_base = game ? (unsigned)(uintptr_t)game : 0;
    read_pe_sections();

    VRLOG("cam: probe armed. It scans every %.0f s (up to %d times) for objects whose class "
          "pointer is sBioCamera (%08X%s) and checks each one geometrically. The stage pointer "
          "[%08X] + 0x%X only shortens the first wait - the scan no longer depends on it, because "
          "a gate that stays shut swallows a whole run. Read-only; nothing is written.",
          (double)kScanRetryMs / 1000.0, kScanAttempts, kDtiBioCamera,
          g_module_base ? ", relocated with the module" : "",
          kGlobalStageObject, kStageCameraOffset);
    VRLOG("cam: BH6.exe module base = %08X (the static notes assume 00400000, so the delta is "
          "%+d)", g_module_base, (int)(g_module_base - 0x400000u));
    if (g_section_count) {
        for (int i = 0; i < g_section_count; ++i) {
            VRLOG("cam:   section %d: va 0x%X vsize 0x%X rawsize 0x%X %s", i,
                  g_sections[i].va, g_sections[i].vsize, g_sections[i].rawsize,
                  g_sections[i].executable ? "exec" : "data");
        }
    }

    // First wait: short, so a run that never loads a level still produces a scan and a log block
    // to read - unless the engine says a stage is ready sooner, in which case scan immediately.
    for (int i = 0; i < 60 && g_running; ++i) {
        if (stage_ready()) {
            VRLOG("cam: a stage is loaded ([stage+0x%X] is non-zero) - scanning now",
                  kStageCameraOffset);
            break;
        }
        Sleep(500);
    }
    if (!g_running) return 0;

    // The three globals the static notes left open. Reading them costs nothing and needs no
    // scan, so they are reported even if the scan below finds nothing.
    probe_global("uCameraCtrl slot", kGlobalCameraCtrlSlot, kDtiCameraCtrl);
    probe_global("stage object", kGlobalStageObject, 0);
    {
        unsigned stage = 0;
        if (safe_read_u32(kGlobalStageObject, &stage) && stage) {
            unsigned cam = 0;
            if (safe_read_u32(stage + kStageCameraOffset, &cam)) {
                if (cam == 0) {
                    VRLOG("cam: [stage + 0x%X] = 0  -> no camera controller there (yet?)",
                          kStageCameraOffset);
                } else {
                    unsigned vt = 0;
                    const unsigned dti = object_class(cam, &vt);
                    const char *cls = class_name_for(dti);
                    VRLOG("cam: [stage + 0x%X] = %08X -> vtable %08X  class %s",
                          kStageCameraOffset, cam, vt, cls ? cls : "(not a camera class)");
                }
            }
        }
    }

    // The schedule, in one place, so each attempt can be described and the run can be interpreted
    // afterwards. Three rules, each from a measured failure:
    //
    //   1. a level is NOT required for a scan (the 23:42 run never scanned at all behind that
    //      gate - a gate that silences a whole run is worse than no gate);
    //   2. an attempt only counts toward the retry budget - and the whole probe - once the stage
    //      has changed at least once, so cutscenes, menus and dialogue stops do not eat the
    //      budget (the player's report: loading a save shows a cutscene first, and doors stop for
    //      dialogue);
    //   3. the last attempt is allowed on a shutdown request, so quitting the game still leaves a
    //      finished answer in the log rather than silence.
    std::vector<Instance> found;
    int raw = 0, rejected = 0;
    int n = 0;
    int dump_budget = 4;      // full entry dumps per scan, so the log stays readable

    VRLOG("cam: the globals above are reported; scanning starts in %.0f s and then repeats every "
          "%.0f s. There is deliberately NO gate any more: the 23:42 run never scanned behind one "
          "gate and the 00:03 run never scanned behind its replacement, and a gate that never "
          "opens is indistinguishable from a probe that does not work.",
          (double)kFirstScanMs / 1000.0, (double)kScanRetryMs / 1000.0);
    {
        const unsigned long long until = GetTickCount64() + kFirstScanMs;
        while (g_running && GetTickCount64() < until) Sleep(250);
    }

    for (int attempt = 0; attempt < kScanAttempts; ++attempt) {
        const bool stopping = !g_running;
        if (stopping) {
            VRLOG("cam: the game is shutting down - taking one last scan so the log ends with an "
                  "answer instead of silence");
        }

        // Ask for a picture of the frame this attempt is about to look at, and judge it. The
        // image and the verdict land in the log together with the scan result, so "found nothing"
        // can be read against "the game was showing a menu" instead of being guessed at - which is
        // exactly what three gated runs could not tell apart.
        {
            char label[48];
            _snprintf_s(label, sizeof(label), _TRUNCATE, "scan%02d", attempt + 1);
            screenshot_request(label);
            // The bridge captures on the render thread, so give it a few frames to happen.
            const unsigned long long until = GetTickCount64() + 400;
            while (g_running && GetTickCount64() < until) Sleep(50);
        }

        // --- key 1: the vtable, a statically verified value at +0x00 of every instance ---
        // This is the key that should work: it does not depend on the `+4` layout question, and
        // every hit reports what `+4` actually holds.
        found.clear();
        int vtable_hits = 0, geom_rejected = 0;
        unsigned first[kKnownClassCount];
        int count[kKnownClassCount];
        unsigned valid_first[kKnownClassCount];
        int valid_count[kKnownClassCount];
        const int nv = find_by_vtable(&found, &vtable_hits, &geom_rejected, first, count,
                                      nullptr, 0, valid_first, valid_count);

        // --- key 2: the class record (the documented layout; reported as an observation) ---
        std::vector<Instance> by_class;
        int craw = 0, crej = 0;
        const unsigned bio_needle = kDtiBioCamera + (g_module_base >= 0x400000u
                                                        ? g_module_base - 0x400000u : 0u);
        const int nc = find_instances(&by_class, &craw, &crej, bio_needle, false);

        unsigned stage = 0, cam = 0;
        const bool have_stage = safe_read_u32(kGlobalStageObject, &stage);
        const bool have_cam = have_stage && stage &&
                              safe_read_u32(stage + kStageCameraOffset, &cam);
        VRLOG("cam: attempt %d/%d  vtable key: %d hit(s)/%d instance(s); class-record key: "
              "%d hit(s)/%d instance(s); stage [%08X] = %s, [stage+0x%X] = %s", attempt + 1,
              kScanAttempts, vtable_hits, nv, craw, nc, kGlobalStageObject,
              have_stage ? "set" : "unreadable", kStageCameraOffset,
              (have_cam && cam) ? "non-zero" : "zero/unreadable");
        for (int c = 0; c < kKnownClassCount; ++c) {
            if (!count[c]) continue;
            // `valid` is the interesting column: an object of a camera class whose pose checks
            // out. Any class with valid>0 is a live camera the renderer could be using, and its
            // address is directly usable.
            VRLOG("cam:   %-28s %2d instance(s), first at %08X   | pose valid: %d, first at %s",
                  kKnownClasses[c].name, count[c], first[c], valid_count[c],
                  valid_first[c] ? "see above" : "-");
            if (valid_first[c] && valid_count[c] > 0 && c != 0) {
                VRLOG("cam:     ^ %s at %08X HAS A VALID POSE at +0x%X", kKnownClasses[c].name,
                      valid_first[c], kCameraOrgBase);
                if (dump_budget > 0) {
                    --dump_budget;
                    dump_camera_entries((void *)valid_first[c], "a camera class with a valid pose");
                }
            }
        }

        if (nv > 0) { n = nv; break; }
        if (nc > 0) {
            found.swap(by_class);
            n = nc;
            break;
        }
        if (stopping) break;
        if (attempt + 1 < kScanAttempts) {
            const unsigned long long until = GetTickCount64() + kScanRetryMs;
            while (g_running && GetTickCount64() < until) Sleep(250);
        }
    }

    if (n == 0) {
        VRLOG("cam: neither key found a camera instance. Both statically verified identifiers "
              "have now been tried in memory, so what remains is not a search problem:");
        vtable_census();
        g_running = false;
        return 0;
    }

    VRLOG("cam: *** %d live sBioCamera object(s) with a valid camera pose ***", n);
    const int show = n < kMaxDetailed ? n : kMaxDetailed;
    for (int i = 0; i < show; ++i) {
        unsigned vt = 0;
        object_class((unsigned)(uintptr_t)found[i].addr, &vt);
        describe_instance((unsigned)(uintptr_t)found[i].addr, found[i], vt);
    }
    if (n > show) {
        VRLOG("cam: ... and %d more (same class); the first one is the one to write.", n - show);
    }
    VRLOG("cam: the static notes predicted vtable %08X (0x151A380 + delta); if a described "
          "object's vtable equals that, the class map, the vtable and the live object all agree.",
          0x151A380u + (g_module_base ? g_module_base - 0x400000u : 0u));

    g_running = false;
    return 0;
}

} // namespace

// A synthetic self-test for the scan, so its logic can be checked without the game.
//
// Why this is worth the code: the probe's failure mode is a clean, confident ZERO ("no live
// sBioCamera found"), and a zero is indistinguishable from "the class pointer is relocated",
// "the needle is wrong", "the walk skips the heap" or "the geometry test is too strict". This
// builds an object the scanner MUST find (correct class pointer, valid pose) next to one it
// MUST reject (correct class pointer, garbage pose), runs the real `find_instances`, and says
// whether the result set is exactly right.
//
// Set RE6VR_CAM_SELFTEST=1 to run it at install time. It allocates and frees its own memory
// and never touches the game.
bool mem_cam_selftest() {
    // The guard test first: everything below reads untrusted memory, so "do the guards actually
    // refuse a bad address" is a precondition of trusting any of this output.
    probe_guard_selftest();
    // The search loop is gated on `g_running` (so a probe can be cancelled), and `mem_cam_start`
    // is what normally sets it. Calling the search without it does not fail loudly - it walks
    // zero bytes and reports "found nothing", which is exactly the failure this self-test exists
    // to catch. It caught it here first, which is the point.
    g_running = true;
    // The needle is a relocated constant, so the base has to be known before the search runs -
    // including here, where the self-test may be the first thing in the process to ask.
    if (!g_module_base) {
        const HMODULE game = GetModuleHandleW(L"BH6.exe");
        const HMODULE self = GetModuleHandleW(nullptr);
        g_module_base = game ? (unsigned)(uintptr_t)game
                             : (self ? (unsigned)(uintptr_t)self : 0x400000u);
    }
    const unsigned base = g_module_base;
    const unsigned needle = kDtiBioCamera + (base >= 0x400000u ? base - 0x400000u : 0u);

    // The plant is written as bytes at fixed offsets with the offsets spelled out, so the
    // fixture's shape is readable from the source and does not depend on struct layout.
    //
    // A stack-local struct with 3.6 KB of padding was the previous form, and the probe faulted
    // (0xC0000005) before printing anything once it was introduced. A test fixture is not worth
    // that kind of cleverness: the object lives on the heap, and kPlantSize says how much of it
    // the camera fields need.
    const size_t span = kPlantSize + 0x1000;
    unsigned char *mem = (unsigned char *)VirtualAlloc(nullptr, span, MEM_COMMIT,
                                                      PAGE_READWRITE);
    if (!mem) {
        VRLOG("cam: selftest: VirtualAlloc failed");
        return false;
    }
    memset(mem, 0xCD, span);
    const unsigned good = (unsigned)(uintptr_t)mem + 0x200;
    const unsigned bad = good + (unsigned)kPlantSize;

    for (int which = 0; which < 2; ++which) {
        const unsigned obj = which ? bad : good;
        unsigned char *p = (unsigned char *)(uintptr_t)obj;
        // Written as bytes at fixed offsets with the offsets spelled out, so the fixture's shape
        // is readable from the source and does not depend on struct layout. (A stack-local
        // struct with 3.6 KB of padding was the previous form and faulted inside the probe; a
        // fixture is not worth that kind of cleverness.)
        memset(p, 0xCD, kPlantSize);
        // The vtable must be a plausible one for THIS module - see first_executable_address.
        // (The census checks the known camera vtables separately; this is the class-record key's
        // own precondition.)
        unsigned vt = first_executable_address();
        if (!vt) vt = base + 0x1000;
        const unsigned head[2] = {vt, needle};              // +0x00 vtable, +0x04 class record
        memcpy(p, head, sizeof(head));
        float f[15];
        for (int i = 0; i < 15; ++i) f[i] = 0.0f;
        if (which == 0) {
            f[0] = 100.0f; f[1] = 200.0f; f[2] = 300.0f;    // cameraPos
            f[4] = 100.0f; f[5] = 200.0f; f[6] = 299.0f;    // targetPos: 1 unit ahead
            f[8] = 0.0f;   f[9] = 1.0f;   f[10] = 0.0f;     // cameraUp
            f[12] = 0.9f;  f[13] = 0.1f;  f[14] = 500.0f;   // fov, near, far
        } else {
            // Same class pointer, but these fields are not a pose: forward has no length, up is
            // not a unit vector, fov is outside any lens, planes are unordered. The search must
            // reject this one - a fixture that only proves "it can find things" would accept a
            // search that returns every needle hit.
            f[0] = 0.0f; f[1] = 0.0f; f[2] = 0.0f;
            f[4] = 0.0f; f[5] = 0.0f; f[6] = 0.0f;
            f[8] = 5.0f; f[9] = 5.0f; f[10] = 5.0f;
            f[12] = 0.0f; f[13] = 0.0f; f[14] = 0.0f;
        }
        memcpy(p + kCameraOrgBase, f, sizeof(f));
    }

    std::vector<Instance> found;
    int raw = 0, rejected = 0;

    // The addresses the fixture planted at, printed unconditionally: when a self-test fails, the
    // first question is whether the fixture and the search are even talking about the same bytes.
    unsigned was[4] = {0};
    VRLOG("cam: selftest: planted at good=%08X bad=%08X (buffer %08X, module base %08X, needle "
          "%08X)", good, bad, (unsigned)(uintptr_t)mem, base, needle);
    {
        unsigned b4[4] = {0}, gf[4] = {0}, bf[4] = {0};
        safe_read((const void *)good, was, sizeof(was));
        safe_read((const void *)bad, b4, sizeof(b4));
        safe_read((const void *)(good + kCameraOrgBase), gf, sizeof(gf));
        safe_read((const void *)(bad + kCameraOrgBase), bf, sizeof(bf));
        VRLOG("cam: selftest: just after planting, [good] = %08X %08X %08X %08X ; "
              "[good+0xE30] = %08X %08X %08X %08X", was[0], was[1], was[2], was[3],
              gf[0], gf[1], gf[2], gf[3]);
        VRLOG("cam: selftest: just after planting, [bad]  = %08X %08X %08X %08X ; "
              "[bad+0xE30]  = %08X %08X %08X %08X", b4[0], b4[1], b4[2], b4[3],
              bf[0], bf[1], bf[2], bf[3]);
    }

    // Before searching, read the plant back through exactly the accessors the search uses. If
    // this disagrees with what was written, the fault is in the test fixture and every later
    // line would be a misleading verdict about the search itself.
    {
        unsigned back_vt = 0, back_dti = 0;
        float back_f[15] = {0};
        const bool head_ok = safe_read_u32(good, &back_vt) && safe_read_u32(good + 4, &back_dti);
        const bool body_ok = safe_read((const void *)(good + kCameraOrgBase), back_f,
                                       sizeof(back_f));
        VRLOG("cam: selftest: readback of the valid plant at %08X: head %s (vtable=%08X dti=%08X, "
              "expected dti=%08X), body %s (pos %.1f %.1f %.1f target %.1f %.1f %.1f up %.1f %.1f "
              "%.1f fov %.3f)", good, head_ok ? "ok" : "FAILED", back_vt, back_dti, needle,
              body_ok ? "ok" : "FAILED", back_f[0], back_f[1], back_f[2], back_f[4], back_f[5],
              back_f[6], back_f[8], back_f[9], back_f[10], back_f[12]);
    }

    const int n = find_instances(&found, &raw, &rejected, needle, false);
    {
        unsigned now4[4] = {0};
        safe_read((const void *)good, now4, sizeof(now4));
        VRLOG("cam: selftest: after the scan, [good] = %08X %08X %08X %08X (before it: %08X %08X "
              "%08X %08X)", now4[0], now4[1], now4[2], now4[3], was[0], was[1], was[2], was[3]);
    }
    bool found_good = false, found_bad = false;
    for (size_t i = 0; i < found.size(); ++i) {
        if ((unsigned)(uintptr_t)found[i].addr == good) found_good = true;
        if ((unsigned)(uintptr_t)found[i].addr == bad) found_bad = true;
    }
    VRLOG("cam: selftest: planted a valid sBioCamera at %08X and an invalid one at %08X, "
          "scanned the process, and found %d object(s) with valid geometry (raw class-pointer "
          "hits: %d, rejected by geometry: %d)", good, bad, n, raw, rejected);
    VRLOG("cam: selftest: must-find object %s, must-reject object %s -> %s",
          found_good ? "FOUND" : "MISSED", found_bad ? "WRONGLY ACCEPTED" : "correctly rejected",
          (found_good && !found_bad) ? "PASS" : "FAIL");
    VRLOG("cam: selftest: running the vtable-keyed search over the planted fixtures (both carry a "
          "real sBioCamera vtable; only the valid one has a camera pose)");
    {
        std::vector<Instance> by_vt;
        int vt_hits = 0, vt_rejected = 0;
        unsigned f[kKnownClassCount];
        int c[kKnownClassCount];
        const unsigned fixture_vt = first_executable_address();
        const unsigned extra[1] = {fixture_vt ? fixture_vt : (base + 0x1000)};
        const int nvt = find_by_vtable(&by_vt, &vt_hits, &vt_rejected, f, c, extra, 1);
        bool vt_good = false, vt_bad = false;
        for (size_t i = 0; i < by_vt.size(); ++i) {
            if ((unsigned)(uintptr_t)by_vt[i].addr == good) vt_good = true;
            if ((unsigned)(uintptr_t)by_vt[i].addr == bad) vt_bad = true;
        }
        VRLOG("cam: selftest: vtable key found %d instance(s) (%d hit(s), %d rejected): "
              "must-find %s, must-reject %s -> %s", nvt, vt_hits, vt_rejected,
              vt_good ? "FOUND" : "MISSED", vt_bad ? "WRONGLY ACCEPTED" : "correctly rejected",
              (vt_good && !vt_bad) ? "PASS" : "FAIL");
    }
    VirtualFree(mem, 0, MEM_RELEASE);
    return found_good && !found_bad;
}

void mem_cam_start() {
    if (g_enabled) return;
    wchar_t path[MAX_PATH] = L"";
    const wchar_t *log_path = vrlog::path();
    if (!log_path || !log_path[0]) return;
    wcsncpy_s(path, MAX_PATH, log_path, _TRUNCATE);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return;
    const size_t tail = (size_t)(MAX_PATH - (slash + 1 - path));
    wcscpy_s(slash + 1, tail, kMarker);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;

    g_enabled = true;
    g_running = true;
    VRLOG("cam: ENABLED from %ls", path);
    probe_guard_selftest();
    g_thread = CreateThread(nullptr, 0, cam_thread, nullptr, 0, nullptr);
    if (!g_thread) {
        g_enabled = false;
        VRLOG("cam: could not start the probe thread");
    }
}

} // namespace re6vr
