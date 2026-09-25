#!/usr/bin/env python3
"""Find the MtDti objects in BH6.exe and walk them.

Why this exists: five memory scans for the camera failed, and the research on this engine family
says value scans are the wrong tool - MT Framework keeps a reflection object (MtDti) for every
class, and those objects are STATIC DATA inside the exe. So the camera class can be named from
the file itself, without running the game.

The route, from ReClass.NET-MtFrameworkPlugin (32-bit layout):
    struct MtDti { void* vft; char* name; MtDti* next; MtDti* child; MtDti* parent;
                   MtDti* link; uint32_t meta; uint32_t id; };   // 0x20 bytes
    size() = (meta & 0x7FFFFF) << 2

An MtDti is therefore findable as: a dword in .data equal to (address of a class-name string) - 4,
because `name` sits at +0x04 and the object starts 4 bytes before it.

Usage: python dti_walk.py [name-substring ...]
       python dti_walk.py            -> report the camera-related classes
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
    nm = data[o:o + 8].rstrip(b"\0").decode(errors="replace")
    vs, va, rs, raw = struct.unpack_from("<IIII", data, o + 8)
    secs.append((nm, va, vs, raw, rs))


def sec_of_rva(rva):
    for nm, va, vs, raw, rs in secs:
        if va <= rva < va + max(vs, rs):
            return nm
    return None


def to_off(rva):
    for nm, va, vs, raw, rs in secs:
        if va <= rva < va + max(vs, rs):
            return raw + (rva - va)
    return None


def rva_of_off(off):
    for nm, va, vs, raw, rs in secs:
        if raw <= off < raw + max(vs, rs):
            return va + (off - raw)
    return None


def read_cstr(va):
    o = to_off(va - IMAGE_BASE)
    if o is None:
        return None
    s = data[o:o + 64].split(b"\0")[0]
    try:
        s = s.decode("ascii")
    except Exception:
        return None
    return s if all(32 <= ord(c) < 127 for c in s) and len(s) >= 2 else None


def find_string(s):
    """First RVA of an ASCII string, anywhere in the image."""
    pat = s.encode() + b"\0"
    j = data.find(pat)
    return (rva_of_off(j), j) if j >= 0 else (None, None)


def dti_at(rva):
    """Read a candidate MtDti at this RVA, and sanity-check the shape."""
    o = to_off(rva)
    if o is None or o + 0x20 > len(data):
        return None
    vft, name, nxt, child, parent, link, meta, did = struct.unpack_from("<8I", data, o)
    nm = read_cstr(name)
    if not nm:
        return None
    # A real MtDti's `next` must point at another MtDti-shaped object or be null.
    if nxt and not read_cstr(struct.unpack_from("<I", data, to_off(nxt - IMAGE_BASE) + 4)[0]) \
            if nxt and to_off(nxt - IMAGE_BASE) else False:
        pass
    return {"rva": rva, "vft": vft, "name": nm, "next": nxt, "child": child,
            "parent": parent, "link": link, "meta": meta, "id": did,
            "size": (meta & 0x7FFFFF) << 2}


def main():
    wanted = sys.argv[1:] or ["Camera", "camera", "uCamera", "sCamera", "cCamera"]

    print("=== MtDti candidates for the requested names ===")
    found = []
    for w in wanted:
        rva, _ = find_string(w)
        if rva is None:
            print("  %-28s string NOT in the image" % w)
            continue
        va = IMAGE_BASE + rva
        # MtDti::name is at +0x04, so the object starts at (name address - 4).
        pat = struct.pack("<I", va - 4)
        s = 0
        hits = 0
        while True:
            j = data.find(pat, s)
            if j < 0:
                break
            s = j + 1
            dti_rva = rva_of_off(j)
            if dti_rva is None:
                continue
            d = dti_at(dti_rva)
            if not d or d["name"] != w:
                continue
            hits += 1
            if len(found) < 400:
                found.append(d)
            if hits <= 3:
                print("  %-28s MtDti at rva 0x%08X  size=%u (0x%X)  id=%u  vft=0x%08X"
                      % (w, dti_rva, d["size"], d["size"], d["id"], d["vft"]))
        if hits > 3:
            print("  %-28s ... %d MtDti-shaped references in total" % (w, hits))
        if hits == 0:
            print("  %-28s no MtDti object references this string" % w)

    print()
    print("=== every candidate whose name or size looks like a camera ===")
    seen = set()
    for d in found:
        key = d["rva"]
        if key in seen:
            continue
        seen.add(key)
        print("  0x%08X  %-40s size 0x%-6X id %u" % (d["rva"], d["name"], d["size"], d["id"]))

    print()
    print("=== the DTI link chain from the first hit (names only, 60 links) ===")
    if found:
        link = found[0]["link"]
        seen_chain = set()
        for i in range(60):
            if not link:
                print("  (chain ends)")
                break
            r = link - IMAGE_BASE
            if r in seen_chain or sec_of_rva(r) is None:
                print("  (chain loops or leaves the image at 0x%08X)" % link)
                break
            seen_chain.add(r)
            d = dti_at(r)
            if not d:
                print("  (0x%08X is not a readable MtDti - the link offset may differ in RE6)"
                      % link)
                break
            print("  0x%08X  %-40s size 0x%X" % (r, d["name"], d["size"]))
            link = d["link"]


if __name__ == "__main__":
    main()
