"""Throwaway dump helper for the camera read-path investigation.

Wraps the truth-tested disasm_lib analysis cache so a function can be printed in full
(with bytes) without re-writing the same boilerplate in every scratch script.

Usage:
    python dv.py func  0x4FF9B0            function header (size, callers, calls)
    python dv.py asm   0x4FF9B0 [n]        full listing (n instructions, default all)
    python dv.py range 0x4FF9B0 40         listing of n instructions from a VA
    python dv.py callers 0x4FF9B0          who calls it, and who calls those
    python dv.py callers2 0x4FF9B0         two levels up
    python dv.py find  "pattern"           grep the listing of the whole image (mnemonic+operands)
"""
import re
import sys

sys.path.insert(0, r"C:\re6vr\scripts")
from disasm_lib import analyze                                        # noqa: E402

CACHE = r"C:\re6vr\_work\bh6_analysis.json"
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

_AN = None


def an():
    global _AN
    if _AN is None:
        _AN = analyze.load_analysis(CACHE, EXE)
    return _AN


def a2i(s):
    return int(s, 0)


def fmt(ins):
    return "  %08X  %-26s %s" % (ins.va, ins.raw.hex(), ins.text())


def asm_range(va, count):
    a = an()
    out = []
    cur = va
    for _ in range(count):
        ins = a.decode_at(cur)
        if ins is None:
            out.append("  %08X  <not an instruction boundary / undecoded>" % cur)
            break
        out.append(fmt(ins))
        cur = ins.va + ins.length
    return out


def asm_func(va, limit=None, skip=0):
    a = an()
    f = a.functions.get(va)
    if f is None:
        for s, fn in a.functions.items():
            if s <= va < fn.end:
                f = fn
                break
    if f is None:
        return ["no analysed function contains 0x%08X" % va]
    out = ["function 0x%08X .. 0x%08X  (%d instructions, %d blocks, origin: %s)"
           % (f.start, f.end, f.instructions, len(f.blocks), getattr(f, "origin", "?"))]
    out.append("called by: %s" % (", ".join("0x%08X" % x for x in sorted(f.called_by)) or "(none)"))
    out.append("calls:     %s" % (", ".join("0x%08X" % x for x in f.calls)))
    out.append("")
    cur = f.start
    n = 0
    while cur < f.end:
        ins = a.decode_at(cur)
        if ins is None:
            out.append("  %08X  <undecoded byte>" % cur)
            cur += 1
            continue
        if ins.va + ins.length > f.end:
            break
        if n >= skip:
            out.append(fmt(ins))
        n += 1
        if limit is not None and n >= skip + limit:
            break
        cur = ins.va + ins.length
    return out


def func_header(va):
    a = an()
    f = a.functions.get(va)
    if f is None:
        for s, fn in a.functions.items():
            if s <= va < fn.end:
                f = fn
                break
    if f is None:
        return ["no analysed function contains 0x%08X" % va]
    out = ["function 0x%08X .. 0x%08X  (%d instructions, %d blocks)"
           % (f.start, f.end, f.instructions, len(f.blocks))]
    out.append("called by (%d):" % len(f.called_by))
    for c in sorted(f.called_by):
        cf = a.functions.get(c)
        out.append("   0x%08X  %s" % (c, ("%d instrs" % cf.instructions) if cf else "?"))
    out.append("calls (%d):" % len(f.calls))
    for c in f.calls:
        cf = a.functions.get(c)
        n = len(cf.called_by) if cf else -1
        out.append("   0x%08X  %s" % (c, ("%d instrs, %d callers" % (cf.instructions, n))
                                      if cf else "?"))
    return out


def callers(va, depth=1, seen=None):
    a = an()
    f = a.functions.get(va)
    if f is None:
        return ["no analysed function at 0x%08X" % va]
    seen = seen or set()
    out = []
    for c in sorted(f.called_by):
        cf = a.functions.get(c)
        tag = "%-34s" % (("0x%08X  %d instrs" % (c, cf.instructions)) if cf else "0x%08X  ?" % c)
        out.append("  " * 0 + "  " + tag + "  calls " +
                   ", ".join("0x%08X" % x for x in (cf.calls[:8] if cf else [])))
        if depth > 1 and c not in seen:
            seen.add(c)
            out.extend("      " + s for s in callers(c, depth - 1, seen))
    return out


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__)
        return 1
    cmd = argv[0]
    if cmd == "func":
        print("\n".join(func_header(a2i(argv[1]))))
    elif cmd == "asm":
        lim = int(argv[2], 0) if len(argv) > 2 else None
        skip = int(argv[3], 0) if len(argv) > 3 else 0
        print("\n".join(asm_func(a2i(argv[1]), lim, skip)))
    elif cmd == "range":
        print("\n".join(asm_range(a2i(argv[1]), int(argv[2], 0))))
    elif cmd == "callers":
        print("\n".join(callers(a2i(argv[1]), 1)))
    elif cmd == "callers2":
        print("\n".join(callers(a2i(argv[1]), 2)))
    elif cmd == "find":
        a = an()
        pat = re.compile(argv[1])
        n = 0
        for fn, ins in analyze.iter_instructions(a):
            t = "0x%08X  %-10s %s" % (ins.va, ins.mnemonic, ins.text())
            if pat.search(t):
                print("  fn 0x%08X | %s" % (fn.start, t))
                n += 1
                if n > int(argv[2], 0) if len(argv) > 2 else n > 200:
                    print("... (capped)")
                    break
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
