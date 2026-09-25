#!/usr/bin/env python3
"""Read-only static analysis of BH6.exe: who references a given address?

No general disassembly - too error-prone for this job, and a wrong listing reads like a right
one. This only looks for the two encodings a reference can take, both of which are exact:

  * a relative CALL/JMP   (E8/E9 rel32) whose target lands on the address
  * an absolute dword     (the address stored in a table, a vtable, or pushed as data)

Usage: python xrefs.py <rva>
"""
import struct
import sys

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
IMAGE_BASE = 0x400000

data = open(EXE, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsz = struct.unpack_from("<H", data, pe + 20)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = data[o:o + 8].rstrip(b"\0").decode(errors="replace")
    vs, va, rs, raw = struct.unpack_from("<IIII", data, o + 8)
    secs.append((name, va, vs, raw, rs))


def sec_of_off(off):
    for name, va, vs, raw, rs in secs:
        if raw <= off < raw + max(vs, rs):
            return name, va + (off - raw)
    return None, None


def read_str(va):
    for name, sva, vs, raw, rs in secs:
        if sva <= va - IMAGE_BASE < sva + max(vs, rs):
            o = raw + (va - IMAGE_BASE - sva)
            t = data[o:o + 48].split(b"\0")[0]
            try:
                t = t.decode("ascii")
            except Exception:
                return ""
            return t if all(32 <= ord(c) < 127 for c in t) and len(t) >= 3 else ""
    return ""


def main():
    rva = int(sys.argv[1], 0)
    va = IMAGE_BASE + rva
    print("target rva 0x%08X  va 0x%08X" % (rva, va))
    print()

    print("--- direct rel32 CALLs (E8) ---")
    n = 0
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        # the next instruction's VA, computed from the section this byte sits in
        name, cur = sec_of_off(i)
        if cur is None:
            continue
        if cur + 5 + rel == va:
            print("  call at file 0x%08X  rva 0x%08X  (section %s)" % (i, cur, name))
            n += 1
    print("  %d hit(s)" % n)
    print()

    print("--- absolute dword references (tables / data) ---")
    pat = struct.pack("<I", va)
    start = 0
    hits = []
    while True:
        j = data.find(pat, start)
        if j < 0:
            break
        hits.append(j)
        start = j + 1
    for j in hits:
        name, cur = sec_of_off(j)
        # Show what precedes it: a table of function pointers or a vtable is a run of
        # addresses that all point into .text.
        ctx = struct.unpack_from("<8I", data, max(0, j - 16)) if j >= 16 else ()
        print("  dword at file 0x%08X  rva 0x%08X  (section %s)" % (j, cur, name))
        if ctx:
            print("    preceding 8 dwords: %s" % " ".join("0x%08X" % c for c in ctx))
    print("  %d hit(s)" % len(hits))


if __name__ == "__main__":
    main()
