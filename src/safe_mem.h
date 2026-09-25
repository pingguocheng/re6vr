// safe_mem.h - memory reads that cannot take the process down.
//
// Why this exists, and why it is NOT __try/__except
// ------------------------------------------------
// This project wrapped every risky read in `__try { ... } __except (EXCEPTION_EXECUTE_HANDLER)`.
// On 2026-09-25 that turned out to be fatal in the worst possible way: the game died with
// 0xC0000005 and the crash reporter pointed at two of those very wrappers -
//
//     FAULT code=C0000005 (read at 5614BCA1) instruction=+0x1B4FD   (safe_read_u32)
//     FAULT code=C0000005 (read at 41BF0000) instruction=+0x1C694   (safe_read)
//     FAULT code=C0000005 (read at 450F44A0) instruction=+0x1C694
//
// - so the guards faulted instead of catching, and a bad pointer anywhere became a dead game. The
// same thing killed the offline harness (read at 0). A guard that can itself kill the process is
// not a guard, and the whole point of these functions is that an untrusted pointer from the game
// must never be able to end the session.
//
// The replacement asks Windows instead of the exception dispatcher: VirtualQuery says whether the
// page is committed, readable and not a guard page, and only then is memory touched. A pointer into
// unmapped memory is simply not read, and nothing needs to unwind. It costs one VirtualQuery per
// read (a few hundred ns, these are not hot paths) and it cannot fail fatally.
//
// Usage:
//   unsigned v;
//   if (re6vr::read32(addr, &v)) { ... }        // false = not readable, addr was never touched
//   float f;
//   if (re6vr::readf32(addr, &f)) { ... }
//   unsigned char buf[64];
//   if (re6vr::read(addr, buf, sizeof(buf))) { ... }   // all-or-nothing over the whole range
#pragma once

#include <windows.h>

#include <cstring>

namespace re6vr {

// Does the whole [addr, addr+n) range live in committed, readable, non-guard pages?
inline bool readable(const void *addr, size_t n) {
    if (!addr || n == 0) return false;
    const unsigned char *p = (const unsigned char *)addr;
    unsigned char *end = (unsigned char *)addr + n;          // may wrap; checked below
    if (end < p) return false;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                             PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
            return false;
        }
        const unsigned char *region_end =
            (const unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= p) return false;                     // no progress: give up
        p = region_end < end ? region_end : end;
    }
    return true;
}

// The whole range or nothing. Returns false without touching memory when it is not all readable.
//
// Two layers on purpose. The page check is what makes this safe even where SEH is not working (the
// measured failure), and the __try catches the narrow race where the owner frees the page between
// the check and the copy - a region can be unmapped by another thread at any moment, and a scan
// that walks a live process will eventually meet that race.
inline bool read(const void *addr, void *out, size_t n) {
    if (!readable(addr, n)) return false;
    __try {
        memcpy(out, addr, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool read32(unsigned addr, unsigned *out) {
    return read((const void *)(uintptr_t)addr, out, sizeof(*out));
}

// Can this range be written? Used before poking a value into game memory: the writes in this
// project come from offsets derived from static analysis, and an offset that is wrong by enough to
// leave the object must fail the check instead of killing the session.
inline bool writable(const void *addr, size_t n) {
    if (!addr || n == 0) return false;
    const unsigned char *p = (const unsigned char *)addr;
    unsigned char *end = (unsigned char *)addr + n;
    if (end < p) return false;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE))) {
            return false;
        }
        const unsigned char *region_end = (const unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= p) return false;
        p = region_end < end ? region_end : end;
    }
    return true;
}

inline bool readf32(unsigned addr, float *out) {
    return read((const void *)(uintptr_t)addr, out, sizeof(*out));
}

// How many bytes from `addr` (up to `n`) are readable and contiguous. Used by the whole-process
// scanners: a region with one unreadable page inside it then costs that page, not the region.
inline size_t readable_prefix(const void *addr, size_t n) {
    if (!addr || n == 0) return 0;
    size_t done = 0;
    while (done < n) {
        const unsigned char *p = (const unsigned char *)addr + done;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return done;
        if (mbi.State != MEM_COMMIT) return done;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return done;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                             PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
            return done;
        }
        const unsigned char *region_end = (const unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= p) return done;
        size_t step = (size_t)(region_end - p);
        if (step > n - done) step = n - done;
        done += step;
    }
    return done;
}

// An address that is at least inside this process's user space and 4-byte aligned. Cheap first
// filter for pointers that come out of game memory (classes, objects, DTI records).
inline bool plausible_pointer(unsigned p) {
    return p >= 0x10000u && p < 0x7FFF0000u && (p & 3u) == 0;
}

} // namespace re6vr
