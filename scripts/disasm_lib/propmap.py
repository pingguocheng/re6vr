"""Per-class field tables, recovered from the property-registration code in `.text`.

WHY THIS EXISTS
---------------
The camera work needs to know *where a field lives inside the object*. Until now the offsets
came from two places, neither of them BH6's own code: a DMC4 reference and a manual parse of
a DTI block. That is exactly the kind of borrowed fact this project has been burned by - and
the README says so in as many words ("the field offsets inside a camera instance ... has NOT
been confirmed against BH6. Do not write to those offsets before checking them against a
live object").

BH6 does carry its own answer. Every engine class registers each field with the editor/property
system, and the registration compiles to a fixed shape:

    lea   edx, [ebx+0xE30]              ; 8D /r  mod=10 - the field's address
    mov   ecx, edi                      ; the property list
    mov   dword [esp+0x10], <name VA>   ; C7 44 24 10 <imm32>
    mov   dword [esp+0x14], <type>      ; C7 44 24 14 <imm32>
    ...
    call  0xE6E210                      ; append (name, type, addr) to the list

So for every field of every class the binary states its name, its type and its byte offset.
The offsets are therefore readable *statically and exactly* - the only thing the static data
cannot say is which runtime object a given table belongs to, and that is what the constructor
stamp sites (`mov reg, <MtDti address>`, `analyze`-aligned) are for.

TWO LAYERS OF EVIDENCE, KEPT SEPARATE
-------------------------------------
`Field` records the raw observation. `class_fields()` adds the attribution and reports how
confident it is, because a property table sitting in a function that also stamps a class DTI
is strong evidence, while a table in a function with no stamp is only "somewhere in .text".
Nothing in this module guesses an offset: every one of them is the displacement the engine's
own registration instruction names.

WHY IT ITERATES BLOCKS AND NOT BYTES
------------------------------------
The naive version of this scan - `text.find(b"\\xc7\\x44\\x24\\x10")` over `.text` - returns
9374 hits, and nearly all of them are *operand bytes of some other instruction*: the four
bytes appear inside immediates and displacements constantly. Decoding only from addresses the
recursive descent already reached (`Function.blocks`) removes that class of false positive
entirely, which is why this module needs `analyze.load_analysis`.
"""
from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from disasm_lib import analyze, pe                                      # noqa: E402

# The engine's "append a property" call. One per registered field.
PROPERTY_APPEND = 0x00E6E210

# Type codes seen in the registration stream. The names come from the editor's type enum;
# only the ones this project actually needs are spelled out, and an unknown code is reported
# as its number rather than silently dropped.
TYPE_NAMES = {
    0x01: "int8",
    0x02: "ptr",
    0x03: "bool",
    0x04: "uint8",
    0x06: "int32",
    0x07: "uint32",
    0x0A: "uint16",
    0x0B: "int16",
    0x0C: "float",
    0x13: "Coord",
    0x14: "Vector3",
    0x15: "Vector4",
    0x16: "Matrix4",
}


@dataclass
class Field:
    name: str
    type_code: int
    offset: int | None          # byte offset in the object, from the `lea`
    site: int                   # VA of the name load
    function: int               # VA of the containing function

    @property
    def type_name(self) -> str:
        t = TYPE_NAMES.get(self.type_code)
        return t if t else "type0x%X" % self.type_code

    def __str__(self) -> str:
        off = ("+0x%X" % self.offset) if self.offset is not None else "+?"
        return "%-44s %-8s %s" % (self.name, self.type_name, off)


@dataclass
class ClassFields:
    name: str
    dti: int
    size: int
    constructor: int | None
    fields: list[Field] = field(default_factory=list)
    evidence: str = ""

    def by_name(self, needle: str) -> list[Field]:
        return [f for f in self.fields if needle.lower() in f.name.lower()]


def _stack_store(ins) -> tuple[int | None, int | None]:
    """`mov dword [esp+disp], imm32` -> (disp, imm), else (None, None)."""
    if ins.mnemonic != "mov" or len(ins.operands) != 2:
        return None, None
    dst, src = ins.operands
    if dst.kind != "mem" or dst.base != "esp" or dst.index is not None:
        return None, None
    if src.kind != "imm":
        return None, None
    try:
        imm = int(src.text, 16)
    except ValueError:
        return None, None
    return dst.disp, imm


def scan_fields_by_function(an: analyze.Analysis, img: pe.Image) -> dict[int, list[Field]]:
    """{function start VA: [Field, ...]} for the whole image, in one pass.

    One pass rather than one scan per class: the pairing rule is only valid *within* a
    registration loop, and walking the image once keeps that grouping explicit instead of
    re-deriving it 3372 times.
    """
    events: dict[int, list[tuple[int, str, object]]] = {}
    for fn, ins in analyze.iter_instructions(an):
        if ins.mnemonic == "lea" and len(ins.operands) == 2:
            o = ins.operands[1]
            if (ins.operands[0].kind == "reg" and o.kind == "mem"
                    and o.base is not None and o.index is None):
                events.setdefault(fn.start, []).append((ins.va, "lea", o.disp))
                continue
        disp, imm = _stack_store(ins)
        if disp != 0x10:
            continue
        rva = imm - img.image_base
        if not img.is_mapped_rva(rva):
            continue
        name = img.is_printable_string_at(rva, minlen=2)
        if name:
            events.setdefault(fn.start, []).append((ins.va, "name", name))

    out: dict[int, list[Field]] = {}
    for fn_start, evs in events.items():
        evs.sort(key=lambda e: e[0])
        fields: list[Field] = []
        pending: int | None = None
        for va, kind, payload in evs:
            if kind == "lea":
                # The `lea` for field i is emitted before the name store of field i-1 (the
                # template's two halves are separated by `mov ecx, edi`), so the pending value
                # is only consumed by the *next* name store.
                pending = payload                    # type: ignore[assignment]
                continue
            type_code = _type_code_after(an, va)
            if type_code is None:
                pending = None
                continue
            fields.append(Field(name=str(payload), type_code=type_code, offset=pending,
                                site=va, function=fn_start))
            pending = None
        if fields:
            out[fn_start] = fields
    return out


def _table_function(an: analyze.Analysis, tables: dict[int, list[Field]], fn: int,
                    depth: int = 3) -> tuple[int | None, str]:
    """The function that holds the property table for a class stamped in `fn`.

    Breadth-first over tail calls first (that is the common thunk shape) and then over direct
    calls, stopping at `depth` so a generic helper cannot drag the search across the image.
    """
    if tables.get(fn):
        return fn, "in the same function that stores the DTI"
    seen = {fn}
    frontier = [fn]
    for level in range(depth):
        nxt: list[int] = []
        for f in frontier:
            fobj = an.functions.get(f)
            if fobj is None:
                continue
            for target in list(fobj.tail_calls) + list(fobj.calls):
                if target in seen:
                    continue
                seen.add(target)
                if tables.get(target):
                    return target, ("reached by %s from 0x%08X"
                                    % ("tail call" if target in fobj.tail_calls else "call", f))
                nxt.append(target)
        frontier = nxt
        if not frontier:
            break
    return None, ""


def resolve_getter(an: analyze.Analysis, fn: int, depth: int = 4) -> int:
    """Follow `mov eax, <DTI>; jmp <real body>` thunks to the function that does the work.

    BH6's MtDti accessors are sometimes compiled as a one-instruction thunk plus a jump
    (`0060AA60 mov eax, 0x17D26F0 / 0060AA66 jmp 0x009768F0`). Attributing a property table to
    the thunk finds nothing, because the table belongs to the body it jumps to.
    """
    seen = {fn}
    cur = fn
    for _ in range(depth):
        fobj = an.functions.get(cur)
        if fobj is None or not fobj.tail_calls:
            return cur
        nxt = fobj.tail_calls[0]
        if nxt in seen or nxt not in an.functions:
            return cur
        seen.add(nxt)
        cur = nxt
    return cur


def scan_fields(an: analyze.Analysis, img: pe.Image, functions: set[int] | None = None
                ) -> list[Field]:
    """Fields for `functions` (or the whole image), flattened and in address order.

    See `scan_fields_by_function` for the template and why the `lea` pairs with the *next*
    name store rather than the nearest preceding one.
    """
    by_fn = scan_fields_by_function(an, img)
    out: list[Field] = []
    for start, fields in by_fn.items():
        if functions is not None and start not in functions:
            continue
        out.extend(fields)
    return sorted(out, key=lambda f: f.site)


def _type_code_after(an: analyze.Analysis, va: int) -> int | None:
    """The type from the `mov [esp+0x14], <type>` that follows a name store."""
    ins = analyze.instruction_at(an, va)
    if ins is None:
        return None
    va += ins.length
    for _ in range(6):
        nxt = analyze.instruction_at(an, va)
        if nxt is None or nxt.is_call or nxt.is_jmp or nxt.is_terminal:
            return None
        disp, imm = _stack_store(nxt)
        if disp == 0x14:
            return imm
        va += nxt.length
    return None


def scan_constructors(an: analyze.Analysis, img: pe.Image,
                      dtis: dict[int, object]) -> dict[int, list[int]]:
    """{class DTI address: [VAs where `mov reg, <that DTI>` appears]}.

    An instance in MT Framework carries its class record; a constructor stores it with a
    `mov reg, imm32`. Finding that store is what ties a class name to the code that builds it,
    and - through the containing function - to the property table that describes it.
    """
    out: dict[int, list[int]] = {}
    for fn, ins in analyze.iter_instructions(an):
        for o in ins.operands:
            if o.kind != "imm" or not o.text.startswith("0x"):
                continue
            try:
                v = int(o.text, 16)
            except ValueError:
                continue
            if v in dtis:
                out.setdefault(v, []).append(ins.va)
    return out


def _function_containing(an: analyze.Analysis, va: int) -> int | None:
    if getattr(an, "_fn_starts", None) is None:
        an._fn_starts = sorted(an.functions)
    from bisect import bisect_right
    i = bisect_right(an._fn_starts, va) - 1
    if i < 0:
        return None
    start = an._fn_starts[i]
    fn = an.functions[start]
    return start if va < fn.end else None


def scan_vtables(img: pe.Image) -> list[tuple[int, list[int]]]:
    """Runs of consecutive code pointers outside the executable sections: the vtables.

    A run is what a vtable looks like from the outside - aligned dwords that all address
    executable memory, sitting in `.rdata`. Run length is not fixed (these run from a handful
    of slots to a few hundred), so a caller must search a run rather than index it.
    """
    out: list[tuple[int, list[int]]] = []
    exec_lo, exec_hi = _exec_range(img)
    for s in img.sections:
        if s.is_executable:
            continue
        blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
        run: list[int] = []
        run_start = 0
        for off in range(0, len(blob) - 3, 4):
            v = int.from_bytes(blob[off:off + 4], "little")
            if exec_lo <= v < exec_hi and (v & 3) == 0:
                if not run:
                    run_start = off
                run.append(v)
                continue
            if len(run) >= 4:
                out.append((img.image_base + s.va + run_start, list(run)))
            run = []
        if len(run) >= 4:
            out.append((img.image_base + s.va + run_start, list(run)))
    return out


def _exec_range(img: pe.Image) -> tuple[int, int]:
    lo, hi = 1 << 62, 0
    for s in img.sections:
        if s.is_executable:
            lo = min(lo, img.image_base + s.va)
            hi = max(hi, img.image_base + s.va + s.rawsize)
    return lo, hi


def class_tables_by_vtable(an: analyze.Analysis, img: pe.Image, dtis: dict,
                           tables: dict[int, list[Field]],
                           constructors: dict[int, list[int]]) -> dict[str, tuple[int, str]]:
    """{class name: (the table function that registers the most fields, evidence)} via a vtable.

    A class record is reachable from its vtable, because the slot that returns the MtDti
    (`mov eax, <DTI>; ret`) is a vtable entry. `sBioCamera` is why this route exists at all:
    its constructor stores nothing a direct search can find, yet the run at 0x151A380 lists
    `004FA560` (the MtDti getter), `004FA570` (the constructor registering all 59 fields) and
    `004FC610` (the destructor) together.

    Selecting among the run's candidates by *field count* is a deliberate, reportable choice:
    more than one slot can sit in a registration function (the constructor, and helpers it
    calls), and the class's own table is the one with the fields. The count is printed in the
    evidence so a wrong pick is visible rather than silent.
    """
    getters: dict[int, int] = {}
    for dti_addr, sites in constructors.items():
        for site in sites:
            fn = _function_containing(an, site)
            if fn is not None:
                getters.setdefault(fn, dti_addr)
                break
    by_addr = {d.address: d for d in dtis}
    out: dict[str, tuple[int, str]] = {}
    for table_va, slots in scan_vtables(img):
        names = [getters[s] for s in slots if s in getters]
        if not names:
            continue
        d = by_addr.get(names[0])
        if d is None or d.name in out:
            continue
        candidates = [(len(tables[slot]), slot) for slot in slots if tables.get(slot)]
        if not candidates:
            continue
        count, slot = max(candidates)
        if len(candidates) > 1:
            why = ("class vtable 0x%08X has %d table candidates; picked 0x%08X with %d "
                   "fields (%s)" % (table_va, len(candidates), slot, count,
                                    ", ".join("0x%08X=%d" % (s, c)
                                              for c, s in sorted(candidates, reverse=True))))
        else:
            why = ("class vtable 0x%08X lists 0x%08X, which registers %d fields"
                   % (table_va, slot, count))
        out[d.name] = (slot, why)
    return out


def class_fields(an: analyze.Analysis, img: pe.Image, dtis: list,
                 constructors: dict[int, list[int]] | None = None,
                 tables: dict[int, list[Field]] | None = None) -> dict[str, ClassFields]:
    """Attribute every property table to a class.

    Two routes, and the `evidence` string says which one answered:

      1. the class's vtable, which names the class outright (it contains the MtDti getter) -
         this is the route that answers for the camera and for 400 other classes;
      2. the function that stores the class's MtDti, following tail calls out of a thunk, for
         classes whose record is not reachable through a vtable run.

    Neither route decides ownership from a field *name*: names are reported, never used to
    attribute, because prefix matching would look like evidence while being a guess.
    """
    by_addr = {d.address: d for d in dtis}
    if constructors is None:
        constructors = scan_constructors(an, img, by_addr)
    if tables is None:
        tables = scan_fields_by_function(an, img)

    found: dict[str, ClassFields] = {}
    for name, (target, why) in class_tables_by_vtable(an, img, dtis, tables,
                                                      constructors).items():
        d = by_addr.get(next((x.address for x in dtis if x.name == name), 0))
        if d is None:
            continue
        found[name] = ClassFields(name=name, dti=d.address, size=d.size, constructor=target,
                                  fields=sorted(tables[target], key=lambda f: f.site),
                                  evidence=why)

    for dti_addr, sites in constructors.items():
        d = by_addr[dti_addr]
        if d.name in found:
            continue
        for site in sites:
            raw = _function_containing(an, site)
            if raw is None:
                continue
            fn = resolve_getter(an, raw)
            target, how = _table_function(an, tables, fn)
            if target is None:
                continue
            found[d.name] = ClassFields(
                name=d.name, dti=d.address, size=d.size, constructor=target,
                fields=sorted(tables[target], key=lambda f: f.site),
                evidence="MtDti stored at 0x%08X in function 0x%08X%s, table %s"
                         % (site, raw, ("" if fn == raw else " (thunk -> 0x%08X)" % fn), how))
            break
    return found


def camera_field_report(an: analyze.Analysis, img: pe.Image, dtis: list,
                        names: list[str]) -> None:
    tables = class_fields(an, img, dtis)
    for name in names:
        cf = tables.get(name)
        if cf is None:
            print("%s: no property table attributed" % name)
            continue
        print("%s  dti=0x%08X size=0x%X  ctor=0x%08X  (%d fields)"
              % (name, cf.dti, cf.size, cf.constructor or 0, len(cf.fields)))
        for f in cf.fields:
            print("    %s" % f)
        print()


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pattern", nargs="?", default="",
                    help="substring of a class name; empty lists every attributed table")
    ap.add_argument("--cache", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "_work",
        "bh6_analysis.json"))
    ap.add_argument("--exe", default=getattr(pe, "DEFAULT_EXE", None)
                    or r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe")
    ap.add_argument("--limit", type=int, default=20)
    args = ap.parse_args()

    an = analyze.load_analysis(args.cache, args.exe)
    if an is None:
        print("no analysis cache at %s" % args.cache, file=sys.stderr)
        print("build it with:  python -m disasm_lib.analyze -o \"%s\"" % args.cache,
              file=sys.stderr)
        return 2
    img = an.img
    from disasm_lib import dti as dti_mod
    tables = scan_fields_by_function(an, img)
    dtis = dti_mod.enumerate_dtis(img)
    by_addr = {d.address: d for d in dtis}
    owner: dict[int, str] = {}
    for dti_addr, sites in scan_constructors(an, img, by_addr).items():
        for site in sites:
            fn = _function_containing(an, site)
            if fn is not None and tables.get(fn):
                owner[fn] = by_addr[dti_addr].name
                break

    print("%d property tables, %d fields, %d attributed to a class"
          % (len(tables), sum(len(v) for v in tables.values()), len(owner)))
    needle = args.pattern.lower()
    shown = 0
    for fn in sorted(tables, key=lambda k: owner.get(k, "\uffff")):
        name = owner.get(fn, "<function 0x%08X>" % fn)
        if needle and needle not in name.lower():
            continue
        print("%s  ctor=0x%08X  (%d fields)" % (name, fn, len(tables[fn])))
        for f in sorted(tables[fn], key=lambda x: x.site):
            print("    %s" % f)
        print()
        shown += 1
        if shown >= args.limit:
            print("... stopping at %d (raise --limit to see more)" % shown)
            break
    if not shown:
        print("nothing matches %r" % args.pattern)
    return 0


if __name__ == "__main__":
    import sys as _sys
    _sys.exit(main())
