#!/usr/bin/env python3
"""Query tool for BH6.exe: disassemble a range, follow a function, look up references.

This replaces the hand-rolled `x86_disasm.py` / `xrefs.py` pair, which could decode only a
handful of instruction forms and had no notion of a function. Everything here runs off
`disasm_lib`, whose decoder is validated against `dumpbin` by `tests/run_tests.py`.

Examples
--------
    python re6dis.py disasm 0x104DA40 40            disassemble 40 instructions at an RVA
    python re6dis.py disasm 0x401000 40 --va        ...or give a virtual address
    python re6dis.py func 0x104DA40                 the whole function containing an address
    python re6dis.py xref 0x1337BF8                 who references this address
    python re6dis.py str "uCamera"                  find a string and its references
    python re6dis.py dti uCamera                    the MtDti class map (name -> class record)
    python re6dis.py manager uCamera                classes reached through a DTI factory call
    python re6dis.py props sBioCamera               a class's field offsets (name/type/offset)
    python re6dis.py field targetPos                which class has this field, at what offset
    python re6dis.py stats                          analysis summary, if a cache exists

The cache is written by `python -m disasm_lib.analyze -o <file>`; commands that need it say
so rather than silently returning nothing.

Addresses: `disasm`/`func`/`xref` take an **RVA** by default and `--va` marks the argument as a
virtual address; printed addresses are always VAs. The cache-based commands (`props`, `field`)
use VAs throughout, because that is what the cache stores.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from disasm_lib import pe, x86                                   # noqa: E402

DEFAULT_EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
DEFAULT_CACHE = os.path.join(os.path.dirname(HERE), "_work", "bh6_analysis.json")


def load_image(path: str) -> pe.Image:
    img = pe.Image.load(path)
    return img


def load_cache(path: str, required: bool = True) -> dict | None:
    if os.path.exists(path):
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    if required:
        print("no analysis cache at %s" % path, file=sys.stderr)
        print("build it with:  python -m disasm_lib.analyze -o \"%s\"" % path,
              file=sys.stderr)
    return None


def annotate(img: pe.Image, ins: x86.Instruction) -> str:
    """Extra text for an instruction: what it calls, what string it touches."""
    notes = []
    for t in ins.jump_targets:
        if ins.is_call:
            imp = img.iat.get(t - img.image_base)
            if imp:
                notes.append(str(imp))
    for o in ins.operands:
        if o.kind == "mem" and o.target:
            imp = img.iat.get(o.target - img.image_base)
            if imp:
                notes.append(str(imp))
            else:
                s = img.is_printable_string_at(o.target - img.image_base, minlen=4)
                if s:
                    notes.append('"%s"' % s[:60])
        if o.kind == "imm" and o.text.startswith("0x"):
            try:
                v = int(o.text, 16)
            except ValueError:
                continue
            imp = img.iat.get(v - img.image_base)
            if imp:
                notes.append(str(imp))
            else:
                s = img.is_printable_string_at(v - img.image_base, minlen=4)
                if s:
                    notes.append('"%s"' % s[:60])
    return ("   ; " + ", ".join(notes)) if notes else ""


def cmd_disasm(args) -> int:
    img = load_image(args.exe)
    va = args.addr + (0 if args.va else img.image_base)
    section = img.section_of_rva(va - img.image_base)
    if section is None:
        print("0x%08X is not inside any section" % va)
        return 2
    off = va - img.image_base
    blob = img.read_rva(off, 16 * args.count + 64)
    dec = x86.Decoder(blob, va)
    i = 0
    for _ in range(args.count):
        ins = dec.decode(i)
        mark = " " if ins.ok else "!"
        print("%s%08X  %-30s %-42s%s"
              % (mark, ins.va, ins.raw.hex()[:28], ins.text(),
                 annotate(img, ins) if ins.ok else ""))
        if not ins.ok:
            print("            ^ decoder stopped: %s" % ins.error)
            break
        i += ins.length
    return 0


def cmd_func(args) -> int:
    img = load_image(args.exe)
    cache = load_cache(args.cache)
    if cache is None:
        return 2
    va = args.addr + (0 if args.va else img.image_base)
    funcs = cache["functions"]
    start = None
    for k, f in funcs.items():
        if int(k, 16) <= va < f["end"]:
            start = k
            break
    if start is None:
        print("no analysed function contains 0x%08X" % va)
        print("(the analysis only covers what was reachable from a code root)")
        return 2
    f = funcs[start]
    print("function 0x%s .. 0x%08X  (%d instructions, %d blocks, origin: %s)"
          % (start, f["end"], f["instructions"], len(f["blocks"]), f["origin"]))
    if f["called_by"]:
        print("called by: %s" % ", ".join("0x%08X" % c for c in f["called_by"][:12]))
    else:
        print("called by: (none found - a root, or addressed indirectly)")
    if f["calls"]:
        print("calls:     %s" % ", ".join("0x%08X" % c for c in f["calls"][:12]))
    if f["data_refs"]:
        for r in f["data_refs"][:10]:
            s = cache.get("strings", {}).get("%08X" % r)
            print("  data 0x%08X%s" % (r, ("  \"%s\"" % s) if s else ""))
    print()
    section = img.section_of_rva(int(start, 16) - img.image_base)
    if section is None:
        return 2
    blob = img.read_rva(int(start, 16) - img.image_base, f["end"] - int(start, 16) + 16)
    dec = x86.Decoder(blob, int(start, 16))
    i = 0
    while i < len(blob) - 16:
        ins = dec.decode(i)
        print("%s%08X  %-30s %-42s%s"
              % (" " if ins.ok else "!", ins.va, ins.raw.hex()[:28], ins.text(),
                 annotate(img, ins) if ins.ok else ""))
        if not ins.ok:
            break
        i += ins.length
        if ins.va + ins.length >= f["end"]:
            break
    return 0


def cmd_xref(args) -> int:
    img = load_image(args.exe)
    va = args.addr + (0 if args.va else img.image_base)
    print("references to 0x%08X" % va)
    found = 0

    # Absolute dword references first - they need no decoding and catch tables and vtables.
    pat = va.to_bytes(4, "little")
    for s in img.sections:
        blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
        pos = 0
        while True:
            j = blob.find(pat, pos)
            if j < 0:
                break
            print("  dword at %08X (%s)" % (img.image_base + s.va + j, s.name))
            found += 1
            pos = j + 1

    # Then a recursive descent over the code, which is both faster and far more trustworthy
    # than a linear sweep: a sweep has to resynchronise after every byte it cannot decode,
    # and each resynchronisation is a chance to report a reference that is not one.
    from disasm_lib import analyze
    an = analyze.Analysis(img)
    an.run(extra_roots=[], max_functions=args.max_functions or 0)
    for ins_va, ins in sorted(an.decoded.items()):
        hit = va in ins.jump_targets or any(
            (o.kind == "mem" and o.target == va) or
            (o.kind == "imm" and o.text == "0x%X" % va)
            for o in ins.operands)
        if hit:
            print("  code at %08X  %-26s %s"
                  % (ins.va, ins.raw.hex()[:24], ins.text()))
            found += 1
            if found >= args.limit:
                print("  ... stopping at %d" % args.limit)
                break
    print("%d reference(s); %d instructions decoded to look for them"
          % (found, len(an.decoded)))
    return 0


def cmd_str(args) -> int:
    img = load_image(args.exe)
    needle = args.text.encode("ascii", "replace")
    hits = 0
    for s in img.sections:
        if s.is_executable:
            continue
        blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
        pos = 0
        while True:
            j = blob.find(needle, pos)
            if j < 0:
                break
            rva = s.va + j
            print("%08X (%s)  %r" % (img.image_base + rva, s.name,
                                     img.read_string(rva, 90)))
            hits += 1
            pos = j + 1
            if hits >= args.limit:
                return 0
    if not hits:
        print("no string containing %r" % args.text)
    return 0


def cmd_stats(args) -> int:
    cache = load_cache(args.cache)
    if cache is None:
        return 2
    funcs = cache["functions"]
    real = {k: f for k, f in funcs.items() if not f.get("alias_of")}
    ins = sum(f["instructions"] for f in real.values())
    unreached = [k for k, f in real.items() if not f["called_by"]]
    print("image           %s" % cache["image"])
    print("image base      0x%08X" % cache["image_base"])
    print("functions       %d  (%d are re-entries into already-analysed code)"
          % (len(funcs), len(funcs) - len(real)))
    print("instructions    %d (excluding re-entries)" % ins)
    print("no caller       %d (roots or indirectly called)" % len(unreached))
    print("undecodable     %d address(es)" % len(cache.get("invalid", {})))
    inv = sorted(cache.get("invalid", {}).items())
    for k, v in inv[:20]:
        print("   0x%s  %s" % (k, v))
    if len(inv) > 20:
        print("   ... %d more" % (len(inv) - 20))
    print("strings found   %d" % len(cache.get("strings", {})))
    print("largest functions:")
    for k, f in sorted(real.items(), key=lambda kv: -kv[1]["instructions"])[:10]:
        print("   0x%s  %6d instructions" % (k, f["instructions"]))
        if f["calls"]:
            print("            calls: %s"
                  % ", ".join("0x%08X" % c for c in f["calls"][:6]))
    return 0


def cmd_dti(args) -> int:
    """Query the MtDti class map: every class name -> its static record."""
    from disasm_lib import dti as dti_mod
    img = load_image(args.exe)
    if args.tree:
        dti_mod.hierarchy(img, args.tree)
        return 0
    dtis = dti_mod.enumerate_dtis(img)
    by_addr = {d.address: d for d in dtis}
    needle = (args.pattern or "").lower()
    shown = 0
    for d in sorted(dtis, key=lambda x: x.address):
        if needle and needle not in d.name.lower():
            continue
        parent = by_addr.get(d.descriptor)
        print("%-40s dti=0x%08X size=0x%-6X parent=%-26s reg@0x%08X"
              % (d.name, d.address, d.size, parent.name if parent else "-", d.site))
        shown += 1
        if shown >= args.limit:
            print("... stopping at %d (raise --limit to see more)" % args.limit)
            break
    if not shown:
        print("no class matches %r (%d classes in the image)" % (args.pattern, len(dtis)))
    return 0


def cmd_manager(args) -> int:
    """Query the DTI factory map: class -> the context global its factory call reads."""
    from disasm_lib import managers as mgr_mod
    img = load_image(args.exe)
    mgrs = mgr_mod.find_managers(img)
    needle = (args.pattern or "").lower()
    shown = 0
    for m in mgrs:
        if needle and needle not in m.name.lower():
            continue
        print("%-36s dti=0x%08X  context=0x%08X  size=0x%X"
              % (m.name, m.dti, m.context, m.size))
        shown += 1
        if shown >= args.limit:
            print("... stopping at %d" % args.limit)
            break
    print("(%d classes reached through a DTI factory call. `context` is the global the call "
          "reads - NOT proven to be the instance pointer; see disasm_lib/managers.py)"
          % len(mgrs))
    return 0


def load_analysis_cache(args):
    """The recursive-descent cache, rebuilt into an `Analysis` (see analyze.load_analysis).

    Rebuilding rather than sweeping bytes matters for anything that needs instruction
    boundaries: `Function.blocks` holds addresses the descent actually decoded from, so a
    query over them cannot land in the middle of an instruction.
    """
    from disasm_lib import analyze as analyze_mod
    an = analyze_mod.load_analysis(args.cache, args.exe)
    if an is None:
        print("no analysis cache at %s" % args.cache, file=sys.stderr)
        print("build it with:  python -m disasm_lib.analyze -o \"%s\"" % args.cache,
              file=sys.stderr)
    return an


def _class_tables(args):
    """`{class name: ClassFields}` for the whole image - shared by the props/field commands.

    Deliberately the same entry point the library uses (`propmap.class_fields`), so the CLI
    cannot drift from the API: an earlier version re-derived the attribution here and
    disagreed with the library about `sBioCamera`.
    """
    from disasm_lib import dti as dti_mod
    from disasm_lib import propmap
    an = load_analysis_cache(args)
    if an is None:
        return None
    img = an.img
    dtis = dti_mod.enumerate_dtis(img)
    return propmap.class_fields(an, img, dtis), an


def cmd_props(args) -> int:
    """Print a class's field offsets, recovered from its property-registration code."""
    got = _class_tables(args)
    if got is None:
        return 2
    tables, _an = got
    needle = (args.pattern or "").lower()
    shown = 0
    for name in sorted(tables):
        cf = tables[name]
        if needle and needle not in name.lower():
            continue
        print("%s  dti=0x%08X size=0x%X  ctor=0x%08X  (%d fields)"
              % (name, cf.dti, cf.size, cf.constructor or 0, len(cf.fields)))
        print("    evidence: %s" % cf.evidence)
        for f in cf.fields:
            print("    %s" % f)
        print()
        shown += 1
        if shown >= args.limit:
            print("... stopping at %d (raise --limit to see more)" % shown)
            break
    if not shown:
        print("no attributed property table matches %r" % args.pattern)
    return 0


def cmd_field(args) -> int:
    """Find which class owns a registered field name, and at what offset."""
    got = _class_tables(args)
    if got is None:
        return 2
    tables, _an = got
    needle = args.text.lower()
    hits = 0
    for name in sorted(tables):
        for f in tables[name].fields:
            if needle in f.name.lower():
                print("%-26s %-42s %-10s %s"
                      % (name, f.name, f.type_name,
                         ("+0x%X" % f.offset) if f.offset is not None else "+?"))
                hits += 1
                if hits >= args.limit:
                    return 0
    if not hits:
        print("no registered field name contains %r" % args.text)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--cache", default=DEFAULT_CACHE)
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("disasm", help="disassemble from an address")
    d.add_argument("addr", type=lambda s: int(s, 0))
    d.add_argument("count", nargs="?", type=int, default=40)
    d.add_argument("--va", action="store_true", help="the address is already a VA")
    d.set_defaults(fn=cmd_disasm)

    f = sub.add_parser("func", help="show one analysed function")
    f.add_argument("addr", type=lambda s: int(s, 0))
    f.add_argument("--va", action="store_true")
    f.set_defaults(fn=cmd_func)

    x = sub.add_parser("xref", help="find references to an address")
    x.add_argument("addr", type=lambda s: int(s, 0))
    x.add_argument("--va", action="store_true")
    x.add_argument("--limit", type=int, default=40)
    # `cmd_xref` runs the recursive-descent analysis itself; it reads this from the namespace.
    # Without the argument every `xref` invocation died with AttributeError before printing
    # anything, which made the "who reads this address" question look unanswered rather than
    # unasked.
    x.add_argument("--max-functions", type=int, default=0,
                   help="stop the descent after N functions (0 = no limit)")
    x.set_defaults(fn=cmd_xref)

    s = sub.add_parser("str", help="find a string in the image")
    s.add_argument("text")
    s.add_argument("--limit", type=int, default=20)
    s.set_defaults(fn=cmd_str)

    dt = sub.add_parser("dti", help="MtDti class map (name -> static class record)")
    dt.add_argument("pattern", nargs="?", default="", help="substring of the class name")
    dt.add_argument("--limit", type=int, default=60)
    dt.add_argument("--tree", metavar="ROOT", default=None,
                    help="print the inheritance tree under this class")
    dt.set_defaults(fn=cmd_dti)

    mg = sub.add_parser("manager", help="engine singletons (class -> live instance address)")
    mg.add_argument("pattern", nargs="?", default="", help="substring of the class name")
    mg.add_argument("--limit", type=int, default=60)
    mg.set_defaults(fn=cmd_manager)

    pr = sub.add_parser("props", help="per-class field table (name/type/offset), from the "
                                      "class's property-registration code")
    pr.add_argument("pattern", nargs="?", default="", help="substring of the class name")
    pr.add_argument("--limit", type=int, default=20)
    pr.set_defaults(fn=cmd_props)

    fd = sub.add_parser("field", help="find a registered field name and its offset")
    fd.add_argument("text")
    fd.add_argument("--limit", type=int, default=60)
    fd.set_defaults(fn=cmd_field)

    st = sub.add_parser("stats", help="summarise the analysis cache")
    st.set_defaults(fn=cmd_stats)

    args = ap.parse_args()
    try:
        return args.fn(args)
    except pe.PeError as e:
        print("PE error: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
