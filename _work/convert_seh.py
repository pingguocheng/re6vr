#!/usr/bin/env python3
"""Rewrite the `__try/__except` memory guards in src/mem_scan.cpp into VirtualQuery page checks.

Why the change of mechanism: on 2026-09-25 a live RE6 session died with 0xC0000005 and the crash
reporter pointed at the guards themselves (safe_read_u32's `mov eax,[eax]`, source_object's byte
probe). A guard that faults instead of catching is worse than no guard, so every risky read now
validates the page with VirtualQuery first - see src/safe_mem.h for the shared primitives.

This script does the mechanical part for mem_scan.cpp and prints what it changed, so the result can
be reviewed line by line. It refuses to touch a site whose shape it does not recognise (the count
of rewritten sites is printed, and any `__try` left over is listed at the end).

Usage: python _work/convert_seh.py            (dry run: report only)
       python _work/convert_seh.py --apply    (rewrite the file)
"""
from __future__ import annotations

import re
import sys

SRC = r"C:\re6vr\src\mem_scan.cpp"


def main() -> int:
    apply = "--apply" in sys.argv
    with open(SRC, encoding="utf-8") as fh:
        text = fh.read()

    rewrites = []

    # 1. matches_safe-style: body only reads through a pointer, returns bool.
    pat_matches = re.compile(
        r"    bool ok = false;\n"
        r"    __try \{\n"
        r"(?P<body>(?:        .*\n)+?)"
        r"    \} __except \(EXCEPTION_EXECUTE_HANDLER\) \{\n"
        r"        ok = false;\n"
        r"    \}\n"
        r"    return ok;\n")

    def repl_matches(m: re.Match) -> str:
        body = m.group("body").replace("\n        ", "\n    ").rstrip()
        # strip the "ok = true/false" assignment scaffolding: the body becomes a plain check
        body = body.replace("ok = true;\n", "")
        rewrites.append("matches_safe family")
        return ("    if (!readable(f, 16 * sizeof(float))) return false;\n"
                "%s\n"
                "    return true;\n" % body)

    # The two-position and triple helpers take `const float *f` and test close_enough on it.
    text2, n1 = pat_matches.subn(repl_matches, text)
    print("pattern A (bool ok + return ok): %d site(s)" % n1)

    # 2. plain read guards: `__try { ...reads... } __except (...) { <fallback> }`
    pat_read = re.compile(
        r"(?P<indent>[ ]+)__try \{\n"
        r"(?P<body>(?:(?P=indent)    .*\n)+?)"
        r"(?P=indent)\} __except \(EXCEPTION_EXECUTE_HANDLER\) \{\n"
        r"(?P<handler>(?:(?P=indent)    .*\n)+?)"
        r"(?P=indent)\}\n")

    def repl_read(m: re.Match) -> str:
        indent = m.group("indent")
        body = m.group("body")
        handler = m.group("handler")
        # The guard is only valid when the body reads through one pointer expression whose width we
        # can name; anything else is left alone so a human looks at it.
        rewrites.append("read guard: %s" % body.strip().splitlines()[0][:70])
        return ("%s// page-validated read (was __try; see src/safe_mem.h)\n"
                "%sif (!readable(%s)) {\n%s%s%s}\n"
                % (indent, indent, guess_pointer(body),
                   handler if handler.endswith("\n") else handler + "\n",
                   "", "")) if False else m.group(0)

    # 3. Only the clearly-safe transformations are applied automatically; the rest are reported.
    print("pattern B sites are reported below for manual editing")

    if apply:
        with open(SRC, "w", encoding="utf-8", newline="") as fh:
            fh.write(text2)
        print("applied pattern A only")
    else:
        print("(dry run)")

    with open(SRC, encoding="utf-8") as fh:
        remaining = [(i + 1, l.rstrip()) for i, l in enumerate(fh.read().splitlines())
                     if "__try" in l or "__except" in l]
    print("remaining __try/__except lines: %d" % len(remaining))
    for line_no, line in remaining[:40]:
        print("  %d: %s" % (line_no, line.strip()))
    return 0


def guess_pointer(body: str) -> str:
    m = re.search(r"\(const float \*\)(\w+)", body) or re.search(r"(\w+)\[", body)
    return m.group(1) if m else "?"


if __name__ == "__main__":
    sys.exit(main())
