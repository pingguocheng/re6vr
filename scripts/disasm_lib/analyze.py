"""Recursive-descent analysis of a PE32 image: functions, call graph, cross references.

Why recursive descent rather than a linear sweep
-----------------------------------------------
A linear sweep decodes every byte from the start of .text and hopes each boundary lines up.
In a real MSVC binary it does not: the gap between functions is `int3` padding for a while
and then data tables, jump tables and string literals, and a sweep that walks into those
produces a listing that is confident and wrong.

Recursive descent starts from addresses that are provably code - the entry point, TLS
callbacks, export thunks, every pointer in a vtable-shaped run in .rdata - decodes forward
from each, and follows calls and branches. Anything never reached is reported as *not
reached*, which is a fact about the analysis; a linear sweep would instead have invented an
instruction for it.

The decoder (`disasm_lib.x86`) refuses unknown opcodes rather than guessing, so a function
that hits one is ended and the address recorded. Those addresses are the interesting output:
they mean the decoder's tables have a hole, and the tool says so instead of papering over it.
"""
from __future__ import annotations

import json
import os
import struct
import sys
from bisect import bisect_right
from collections import deque
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from disasm_lib import pe, x86                                   # noqa: E402

# A jump table in MSVC code is usually `jmp dword ptr [eax*4 + TABLE]`, and TABLE lives in
# .rdata as a run of code addresses. Recognising those runs is what makes switch statements
# reachable at all.
MIN_TABLE_ENTRIES = 4


@dataclass
class Function:
    start: int                    # VA
    end: int = 0                  # VA, exclusive
    name: str = ""
    calls: list[int] = field(default_factory=list)
    tail_calls: list[int] = field(default_factory=list)
    called_by: list[int] = field(default_factory=list)
    data_refs: list[int] = field(default_factory=list)
    jump_targets: list[int] = field(default_factory=list)
    table_refs: list[int] = field(default_factory=list)
    blocks: list[int] = field(default_factory=list)
    instructions: int = 0
    ends_with_invalid: bool = False
    origin: str = ""              # why it was considered code
    alias_of: int = 0             # non-zero when this root re-entered an analysed body
    # `blocks` is a list (JSON-friendly); this is the membership test that callers actually
    # want, built on first use.
    block_set: set[int] = field(default_factory=set, repr=False, compare=False)


class Analysis:
    def __init__(self, img: pe.Image, verbose: bool = False):
        self.img = img
        self.verbose = verbose
        self.code: dict[int, bytes] = {}          # section RVA -> bytes
        self.decoders: dict[int, x86.Decoder] = {}
        self.functions: dict[int, Function] = {}
        self.decoded: dict[int, x86.Instruction] = {}
        self.invalid: dict[int, str] = {}
        self.xrefs_to: dict[int, list[int]] = {}   # target VA -> [instruction VAs]
        self.strings: dict[int, str] = {}
        # Sorted function starts, built lazily: `load_analysis` fills `functions` wholesale,
        # and both `instruction_at` and the CLI need to map an address to its function.
        self.function_starts: list[int] | None = None
        self.skipped_roots = 0
        self._prepare_sections()

    # ---------------------------------------------------------------- setup
    def _prepare_sections(self) -> None:
        for s in self.img.code_sections():
            blob = self.img.data[s.raw:s.raw + min(s.rawsize, len(self.img.data) - s.raw)]
            rva = s.va
            self.code[rva] = blob
            self.decoders[rva] = x86.Decoder(blob, self.img.image_base + s.va)
        if not self.code:
            raise pe.PeError("no executable sections in %s" % self.img.path)

    def section_for(self, va: int) -> tuple[int, int] | None:
        """(section rva, offset within it) for a VA, or None if not in a code section."""
        rva = va - self.img.image_base
        for srva, blob in self.code.items():
            if srva <= rva < srva + len(blob):
                return srva, rva - srva
        return None

    def decode_at(self, va: int) -> x86.Instruction | None:
        if va in self.decoded:
            return self.decoded[va]
        loc = self.section_for(va)
        if loc is None:
            return None
        srva, off = loc
        ins = self.decoders[srva].decode(off)
        if not ins.ok:
            self.invalid[va] = ins.error
            return None
        self.decoded[va] = ins
        for t in ins.jump_targets:
            self.xrefs_to.setdefault(t, []).append(va)
        for o in ins.operands:
            if o.kind == "mem" and o.target:
                self.xrefs_to.setdefault(o.target, []).append(va)
        return ins

    def is_code_va(self, va: int) -> bool:
        """True when `va` (a runtime address) lands inside one of the code sections."""
        return self.section_for(va) is not None

    def _is_code_va(self, va: int) -> bool:
        """Same question as `is_code_va`, restricted to executable sections and aligned.

        Executability is the part that matters: `.rdata` and `.data` are full of addresses of
        strings, tables and other data, and accepting *any mapped* address here made every
        data pointer look like a function - 87 000 bogus roots, and an analysis that decoded
        nothing because it spent its time at addresses like 0x1.

        This deliberately does NOT compare against `image_base` either: `section_for` already
        does that arithmetic, and doing it twice rejected every real address in the image.

        Alignment matters because a mixture of small values and text addresses in a data
        section is common; a run of *aligned* text addresses is not.
        """
        s = self.img.section_of_rva(va - self.img.image_base)
        return s is not None and s.is_executable and (va & 3) == 0

    # ---------------------------------------------------------------- roots
    def entry_points(self) -> dict[int, str]:
        """Addresses that are provably code, with the reason.

        Every entry here is a *VA*. `_scan_pointer_tables` yields table VAs for the same
        reason: mixing RVAs into this dict produced 87 000 roots whose addresses were off by
        the image base, and because those addresses still landed inside `.text`'s raw extent
        the mistake looked like a plausible (if enormous) function list.
        """
        img = self.img
        roots: dict[int, str] = {}
        roots[img.entry_va] = "PE entry point"
        for cb in img.tls_callbacks:
            roots[img.image_base + cb] = "TLS callback"
        for e in img.exports:
            if not e.forwarder and e.rva:
                roots[img.image_base + e.rva] = "export %s" % (e.name or e.ordinal)
        for table_va, count in self._scan_pointer_tables():
            for i in range(count):
                va = table_va + i * 4
                tgt = img.read_u32_rva(va - img.image_base)
                if tgt is not None and self._is_code_va(tgt):
                    roots.setdefault(tgt, "pointer table at 0x%X" % table_va)
        return roots

    def _scan_pointer_tables(self):
        """Yield (table_va, entry_count) for runs of pointers into executable memory.

        A switch jump table and a C++ vtable look the same at this level: consecutive dwords
        that all address executable code. Both are code roots, so one scan finds both.

        Two filters keep the false-positive rate down, because a run of dwords that lands in
        the executable range by accident is not rare in a 2 MB .rdata:
          * at least MIN_TABLE_ENTRIES entries, and
          * the target must start with a byte that a real function prologue starts with.
            A table of function pointers whose first byte is not a prologue byte is a table
            of something else.
        """
        img = self.img
        for s in img.sections:
            if s.is_executable or s.is_code:
                continue
            blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
            n = len(blob) // 4
            if n == 0:
                continue
            vals = struct.unpack_from("<%dI" % n, blob, 0)
            i = 0
            while i < n:
                if not self._is_code_va(vals[i]):
                    i += 1
                    continue
                j = i
                while j < n and self._is_code_va(vals[j]):
                    j += 1
                if j - i >= MIN_TABLE_ENTRIES and self._looks_like_prologue(vals[i]):
                    yield (img.image_base + s.va + i * 4, j - i)
                i = j

    # Bytes that begin a function in MSVC-compiled code: push ebp / mov edi,edi / push of a
    # callee-saved register / sub esp / a jump. A pointer table's first entry is the start of
    # a function, so one of these should be there.
    _PROLOGUE_BYTES = frozenset((0x55, 0x53, 0x56, 0x57, 0x83, 0x81, 0xE9, 0xE8, 0x8B,
                                 0x6A, 0x68, 0xB8, 0x33, 0x8D, 0x50, 0x51, 0x52))

    def _looks_like_prologue(self, va: int) -> bool:
        b = self.img.read_rva(va - self.img.image_base, 1)
        return bool(b) and b[0] in self._PROLOGUE_BYTES

    def _is_code_va(self, va: int) -> bool:
        """Same question as `is_code_va`, plus alignment.

        Alignment matters because a mixture of small values and text addresses that happens
        to sit in a data section is common; a run of *aligned* code addresses is not.
        """
        return self.section_for(va) is not None and (va & 3) == 0

    # ---------------------------------------------------------------- descent
    def run(self, extra_roots: list[int] | None = None, max_functions: int = 0) -> None:
        roots = self.entry_points()
        for va in (extra_roots or []):
            roots.setdefault(va, "command line")
        queue = deque(sorted(roots))
        origins = roots

        # A first pass: walk each root forward, following intra-function control flow only.
        while queue:
            va = queue.popleft()
            if va in self.functions:
                continue
            if max_functions and len(self.functions) >= max_functions:
                break
            before = len(self.decoded)
            fn = self._walk_function(va, origins.get(va, ""))
            fresh = len(self.decoded) - before

            # A pointer table full of interior addresses (or a wrong one) makes many
            # "functions" that are really the same body. Storing them all inflates every
            # count: BH6 reported 3.1 M instructions from 1.87 M decoded, with six
            # consecutive entries each claiming 5500 instructions of the same code.
            # A function that contributes almost no new instructions is a re-entry into
            # something already analysed, so it is recorded as an alias instead.
            if fn.instructions and fresh < max(4, fn.instructions // 4):
                if va != fn.start:
                    fn.alias_of = fn.start
                else:
                    fn.alias_of = 0
                self.skipped_roots += 1
            self.functions[va] = fn

            for t in fn.calls + fn.tail_calls + fn.table_refs:
                if t not in self.functions:
                    queue.append(t)
            if self.verbose and len(self.functions) % 200 == 0:
                print("  ... %d functions, %d instructions"
                      % (len(self.functions), len(self.decoded)), file=sys.stderr)

        self._resolve_callers()
        self._collect_strings()

    def covered_bytes(self) -> int:
        """Bytes of the executable sections that the analysis actually decoded.

        Summing function sizes double-counts every overlapping body; this counts each byte
        once, which is the only figure that means anything as coverage.
        """
        covered: set[int] = set()
        for va, ins in self.decoded.items():
            loc = self.section_for(va)
            if loc is None:
                continue
            srva, off = loc
            covered.update(range(off, off + ins.length))
        return len(covered)

    def _walk_function(self, start: int, origin: str) -> Function:
        fn = Function(start=start, origin=origin)
        todo = deque([start])
        seen: set[int] = set()
        lo = hi = start
        while todo:
            addr = todo.popleft()
            while True:
                if addr in seen or addr in self.functions and addr != start:
                    break
                seen.add(addr)
                ins = self.decode_at(addr)
                if ins is None:
                    fn.ends_with_invalid = True
                    break
                fn.instructions += 1
                lo = min(lo, addr)
                hi = max(hi, addr + ins.length)
                if addr != start and self._looks_like_function_start(ins):
                    break
                if ins.is_call:
                    tgt = ins.jump_targets[0] if ins.jump_targets else None
                    if tgt is not None:
                        fn.calls.append(tgt)
                    elif ins.indirect_branch:
                        self._resolve_indirect(ins, fn)
                    # A call returns, so keep walking.
                elif ins.is_jmp:
                    for t in ins.jump_targets:
                        if not self._in_function(t, start, hi):
                            fn.tail_calls.append(t)
                        else:
                            todo.append(t)
                    if ins.indirect_branch:
                        self._resolve_indirect(ins, fn)
                    break
                elif ins.is_jcc:
                    for t in ins.jump_targets:
                        todo.append(t)
                    if not ins.fall_through:
                        break
                elif ins.is_terminal or not ins.fall_through:
                    break
                addr += ins.length
                if addr - start > 0x40000:      # 256 KB of one "function" is a mistake
                    break
        fn.start, fn.end = lo, hi
        fn.blocks = sorted(seen)
        return fn

    def _looks_like_function_start(self, ins: x86.Instruction) -> bool:
        """A prologue that means "this is a separate function, stop walking into it"."""
        return (ins.mnemonic == "int3") or (ins.mnemonic == "push" and
                                            ins.operands and ins.operands[0].text == "ebp")

    def _in_function(self, target: int, start: int, hi: int) -> bool:
        return start <= target < hi

    def _resolve_indirect(self, ins: x86.Instruction, fn: Function) -> None:
        """`call [0xADDR]` / `jmp [0xADDR]` where ADDR is a pointer table in .rdata."""
        for o in ins.operands:
            if o.kind != "mem" or not o.target:
                continue
            tgt = self.img.read_u32_rva(o.target - self.img.image_base)
            if tgt is None:
                continue
            if self.is_code_va(tgt):
                (fn.calls if ins.is_call else fn.tail_calls).append(tgt)
            elif self._is_code_va(tgt):
                fn.calls.append(tgt)

    def _resolve_callers(self) -> None:
        for va, fn in self.functions.items():
            for t in set(fn.calls + fn.tail_calls):
                callee = self.functions.get(t)
                if callee is not None:
                    callee.called_by.append(va)
        for fn in self.functions.values():
            fn.called_by = sorted(set(fn.called_by))

    def _collect_strings(self) -> None:
        """Annotate data references that land on a printable string."""
        for fn in self.functions.values():
            for o in self.instructions_data_refs(fn):
                s = self.img.is_printable_string_at(o - self.img.image_base, minlen=4)
                if s:
                    self.strings[o] = s[:120]
                    fn.data_refs.append(o)

    def instructions_data_refs(self, fn: Function):
        refs = set()
        for va in fn.blocks:
            ins = self.decoded.get(va)
            if not ins:
                continue
            for o in ins.operands:
                if o.kind == "mem" and o.target and self.img.is_mapped_rva(
                        o.target - self.img.image_base):
                    refs.add(o.target)
                elif o.kind == "imm" and o.text.startswith("0x"):
                    try:
                        v = int(o.text, 16)
                    except ValueError:
                        continue
                    if self.img.is_mapped_rva(v - self.img.image_base):
                        refs.add(v)
        return sorted(refs)

    # ---------------------------------------------------------------- output
    def summary(self) -> str:
        code_bytes = sum(len(b) for b in self.code.values())
        covered = self.covered_bytes()
        return ("functions: %d (%d were re-entries into already-analysed code)\n"
                "decoded instructions: %d   undecodable addresses: %d\n"
                "code sections: %d   executable bytes: %d   decoded: %d (%.1f%%)"
                % (len(self.functions), self.skipped_roots, len(self.decoded),
                   len(self.invalid), len(self.code), code_bytes, covered,
                   100.0 * covered / max(1, code_bytes)))

    def to_json(self) -> dict:
        return {
            "image": self.img.path,
            "image_base": self.img.image_base,
            "functions": {
                "%08X" % va: {
                    "end": fn.end, "name": fn.name, "origin": fn.origin,
                    "instructions": fn.instructions,
                    "calls": sorted(set(fn.calls)),
                    "tail_calls": sorted(set(fn.tail_calls)),
                    "called_by": fn.called_by,
                    "data_refs": sorted(set(fn.data_refs)),
                    "blocks": fn.blocks,
                    "invalid": fn.ends_with_invalid,
                    "alias_of": fn.alias_of,
                } for va, fn in sorted(self.functions.items())
            },
            "invalid": {"%08X" % k: v for k, v in sorted(self.invalid.items())},
            "strings": {"%08X" % k: v for k, v in sorted(self.strings.items())},
        }


def load_analysis(cache_path: str, exe: str | None = None) -> Analysis | None:
    """Rebuild an `Analysis` from a cache written by `to_json`.

    The point of reloading rather than keeping a live object alive is that decoding is the
    expensive half of this tool: a query pass ("which functions touch offset 0xE30?", "which
    function registers the property named X?") wants the *instruction stream*, and rebuilding
    it from the image costs ~90 s and 1.5 GB every time. The cache stores, per function, the
    addresses of every decoded instruction; re-decoding those few hundred thousand addresses
    is seconds.

    Two things are deliberately NOT restored, because the cache does not carry them:

      * `Analysis.decoded` — populated lazily by `decode_at`, so only the addresses a caller
        actually touches get decoded again;
      * `Analysis.xrefs_to` — same, filled as a side effect of `decode_at`.

    Callers that need an instruction at address `va` should use `instruction_at()`, which
    answers only for addresses that are *known* to start an instruction (the `blocks` lists),
    rather than decoding whatever a byte-level pattern match happened to straddle. That
    distinction matters: scanning raw bytes for `c7 44 24 10 <imm32>` finds 9374 hits in
    BH6.exe but most of them are operand bytes of some other instruction.
    """
    if not os.path.exists(cache_path):
        return None
    with open(cache_path, encoding="utf-8") as fh:
        cache = json.load(fh)
    img = pe.Image.load(exe or cache["image"])
    an = Analysis(img)
    an.functions = {}
    for key, f in cache["functions"].items():
        va = int(key, 16)
        an.functions[va] = Function(
            start=va, end=f["end"], name=f.get("name", ""), origin=f.get("origin", ""),
            calls=list(f.get("calls", [])), tail_calls=list(f.get("tail_calls", [])),
            called_by=list(f.get("called_by", [])), data_refs=list(f.get("data_refs", [])),
            blocks=[int(b) for b in f.get("blocks", [])],
            instructions=f.get("instructions", 0),
            ends_with_invalid=bool(f.get("invalid")),
            alias_of=f.get("alias_of", 0),
        )
    an.invalid = {int(k, 16): v for k, v in cache.get("invalid", {}).items()}
    an.strings = {int(k, 16): v for k, v in cache.get("strings", {}).items()}
    an.function_starts = sorted(an.functions)
    return an


def instruction_at(an: Analysis, va: int) -> x86.Instruction | None:
    """The instruction starting at `va`, or None when `va` is not an instruction boundary."""
    if va in an.decoded:
        return an.decoded[va]
    if an.function_starts is None:
        an.function_starts = sorted(an.functions)
    i = bisect_right(an.function_starts, va) - 1
    if i < 0:
        return None
    fn = an.functions[an.function_starts[i]]
    if not fn.block_set:
        fn.block_set = set(fn.blocks)
    if va not in fn.block_set:
        return None
    return an.decode_at(va)


def iter_instructions(an: Analysis, fn: Function | None = None):
    """Yield `(function, instruction)` for every decoded instruction, in address order.

    Iterating `blocks` rather than sweeping bytes is what keeps the caller honest: a block
    entry is an address the recursive descent *decoded from*, so every yield is a real
    instruction boundary. A byte sweep over `.text` finds patterns inside operands, which
    looks like data and is not.
    """
    fns = [fn] if fn is not None else an.functions.values()
    for f in fns:
        for va in sorted(f.blocks):
            ins = an.decode_at(va)
            if ins is not None:
                yield f, ins


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("exe", nargs="?", default=r"C:\Program Files (x86)\Steam\steamapps\common"
                                             r"\Resident Evil 6\BH6.exe")
    ap.add_argument("-o", "--out", default=None, help="cache file to write (JSON)")
    ap.add_argument("--max-functions", type=int, default=0)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--root", action="append", default=[], type=lambda s: int(s, 0),
                    help="extra code root (VA or 0xRVA)")
    args = ap.parse_args()

    img = pe.Image.load(args.exe)
    print(img.summary())
    an = Analysis(img, verbose=args.verbose)
    roots = an.entry_points()
    print("code roots: %d (entry, TLS, exports, %d pointer-table slots)"
          % (len(roots), sum(1 for r in roots.values() if "vtable" in r)))
    an.run(extra_roots=args.root, max_functions=args.max_functions)
    print(an.summary())
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(an.to_json(), fh)
        print("wrote %s (%.1f MB)" % (args.out, os.path.getsize(args.out) / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())
