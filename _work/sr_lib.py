#!/usr/bin/env python3
"""Shared helpers for the stereo-render static investigation.

WHY THIS EXISTS
---------------
Two traps cost time on this task and are avoided here for good:

1. `analyze.load_analysis()` leaves `Analysis.decoded` EMPTY on purpose. A scan that iterates
   `an.decoded` sees zero instructions and silently reports "no hits". Everything must iterate
   `f.blocks` through `an.decode_at()` / `analyze.iter_instructions()`.

2. `Operand.text` is a *placeholder* for register operands until the opcode's register class
   relabels it (`_modrm` returns `R32[rm]` and the caller rewrites it). Reading `o.text`
   therefore yields the right register name for memory operands but the *wrong* or relabelled
   one for registers. Always use `ins.text()` for display, and identify registers by comparing
   rendered operand strings, not `o.text`.
"""
from __future__ import annotations

import os
import pickle
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(os.path.dirname(HERE), "scripts")
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

CACHE = os.path.join(os.path.dirname(HERE), "_work", "bh6_analysis.json")
ALLINS = os.path.join(HERE, "_sr_all_ins.pkl")

MOV_MEM_RE = re.compile(r"^mov\s+([a-z0-9]+),\s*(?:\w+ )?\[([a-z0-9]+)(?:\+([0-9a-fx]+))?\]$")


def build_all(force: bool = False):
    """[(fn_start, fn_end, [(va, text, is_call, is_jmp, length)])] for the whole image.

    Cached as a pickle next to this file because re-decoding 3.1M instructions takes ~3 min.
    """
    if not force and os.path.exists(ALLINS):
        with open(ALLINS, "rb") as fh:
            obj = pickle.load(fh)
        # Guard against a stale pickle written by an older revision of this file: the row
        # tuple gained a field, and unpickling the old shape silently unpacked wrong.
        if isinstance(obj, dict) and obj.get("v") == 2:
            return obj["rows"]
    force = True
    from disasm_lib import analyze
    an = analyze.load_analysis(CACHE, None)
    out = []
    for f in an.functions.values():
        rows = []
        for va in sorted(f.blocks):
            ins = an.decode_at(va)
            if ins is None or not ins.ok:
                continue
            rows.append((va, ins.text(), ins.is_call, ins.is_jmp, ins.length))
        out.append((f.start, f.end, rows))
    with open(ALLINS, "wb") as fh:
        pickle.dump({"v": 2, "rows": out}, fh)
    return out


def iter_all(force: bool = False):
    """Yield (fn_start, fn_end, rows_by_va, ordered_rows) per function."""
    for start, end, rows in build_all(force):
        yield start, end, {r[0]: r for r in rows}, rows


def find_fn(funcs, va):
    for start, end, rows in funcs:
        if start <= va < end:
            return start, end, rows
    return None


def contexts(funcs, va, before=6, after=20):
    """Print a window of instructions around `va` with the `>>` marker."""
    got = find_fn(funcs, va)
    if got is None:
        print("(0x%08X is not inside an analysed function)" % va)
        return
    start, end, rows = got
    idx = next((i for i, r in enumerate(rows) if r[0] == va), None)
    if idx is None:
        idx = next((i for i, r in enumerate(rows) if r[0] > va), len(rows))
    print("--- fn %08X .. %08X   window around %08X ---" % (start, end, va))
    for k in range(max(0, idx - before), min(len(rows), idx + after)):
        r = rows[k]
        print("%s %08X  %s" % (">>" if r[0] == va else "  ", r[0], r[1]))
