#!/usr/bin/env python3
"""Build and parse ground truth for the x86 decoder, using the tools already on this machine.

The problem this solves
-----------------------
`pip` has no index access here, so Capstone and Ghidra are unavailable and the project has
been reduced to a hand-rolled decoder that has already produced two confidently-wrong
listings. But the machine *does* have Visual Studio 2022 Build Tools, which ships:

  * `ml.exe`     - a real 32-bit assembler (MASM)
  * `dumpbin.exe` - a real disassembler (`dumpbin /disasm`)

So ground truth is available offline. Assemble a corpus with ml, disassemble the object with
dumpbin, and the pair (bytes, text) per instruction is an oracle the decoder can be held to.
dumpbin prints the exact machine-code bytes it consumed for each line, so comparing our
decoder's *length* and its *rendering* against those lines validates both halves at once -
and it is not circular, because neither half was written by us.

Finding the tools: they are not on PATH (vcvars was never run in this session), so the
locations are probed directly and the result is cached in `_tools.json` next to this file.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "_tools.json")

VS_ROOT = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
MSVC_GLOBS = [
    os.path.join(VS_ROOT, "VC", "Tools", "MSVC"),
]
SDK_ROOT = r"C:\Program Files (x86)\Windows Kits\10"


class GroundTruthError(Exception):
    pass


def _find_msvc_dir() -> str | None:
    for root in MSVC_GLOBS:
        if not os.path.isdir(root):
            continue
        for name in sorted(os.listdir(root), reverse=True):
            p = os.path.join(root, name)
            if os.path.isdir(p):
                return p
    return None


def discover_tools(refresh: bool = False) -> dict:
    """Locate ml.exe / dumpbin.exe / link.exe without relying on vcvars."""
    if not refresh and os.path.exists(CACHE):
        try:
            with open(CACHE, encoding="utf-8") as fh:
                cached = json.load(fh)
            if all(os.path.exists(v) for v in cached.values()):
                return cached
        except Exception:
            pass

    msvc = _find_msvc_dir()
    if not msvc:
        raise GroundTruthError("MSVC toolset not found under %s" % MSVC_GLOBS[0])
    bin86 = os.path.join(msvc, "bin", "Hostx64", "x86")
    bins = {
        "ml": os.path.join(bin86, "ml.exe"),
        "dumpbin": os.path.join(bin86, "dumpbin.exe"),
        "link": os.path.join(bin86, "link.exe"),
    }
    for k, v in bins.items():
        if not os.path.exists(v):
            raise GroundTruthError("%s not found at %s" % (k, v))
    with open(CACHE, "w", encoding="utf-8") as fh:
        json.dump(bins, fh, indent=1)
    return bins


def _run(cmd: list[str], cwd: str) -> str:
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors="replace")
    if p.returncode != 0:
        raise GroundTruthError("%s failed (%d):\n%s\n%s" %
                               (" ".join(cmd), p.returncode, p.stdout[-4000:],
                                p.stderr[-2000:]))
    return p.stdout


def build_fixture(asm_path: str, workdir: str, refresh: bool = False) -> str:
    """Assemble `asm_path` and return the path of the resulting .obj."""
    tools = discover_tools(refresh=refresh)
    os.makedirs(workdir, exist_ok=True)
    base = os.path.splitext(os.path.basename(asm_path))[0]
    obj = os.path.join(workdir, base + ".obj")
    _run([tools["ml"], "/nologo", "/c", "/Fo" + obj, asm_path], workdir)
    return obj


_DISASM_LINE = re.compile(r"^\s*([0-9A-F]{8}):\s*((?:[0-9A-F]{2}\s)+)\s*(.*?)\s*$")
# dumpbin wraps a long instruction: the address column is replaced by an indented run of
# bytes. Those bytes belong to the *previous* instruction. Dropping them (as an earlier
# version of this parser did) makes every wrapped instruction look one byte short, which
# then reads as a decoder length bug that does not exist.
#
# The byte run is matched with `\s*` separators and no required trailing space, because the
# line is right-trimmed: a pattern demanding "XX " fails on the last byte in the line and
# silently matches nothing at all.
_CONT_LINE = re.compile(r"^\s{9,}((?:[0-9A-F]{2}\s*)+)$")


class DisasmLine:
    __slots__ = ("addr", "raw", "text", "section")

    def __init__(self, addr: int, raw: bytes, text: str, section: str):
        self.addr = addr
        self.raw = raw
        self.text = text
        self.section = section

    def __repr__(self) -> str:
        return "0x%08X %-24s %s" % (self.addr, self.raw.hex(), self.text)


def disassemble(obj_path: str, refresh: bool = False) -> dict[str, list[DisasmLine]]:
    """Return {group_name: [DisasmLine]} for everything dumpbin disassembles.

    For a COFF object dumpbin groups the listing by label (`_test_alu:`) rather than by
    segment; an executable gets `name SEGMENT`. Both are handled so the same helper works on
    either input.
    """
    tools = discover_tools(refresh=refresh)
    out = _run([tools["dumpbin"], "/nologo", "/disasm", obj_path],
               os.path.dirname(obj_path) or ".")
    sections: dict[str, list[DisasmLine]] = {}
    cur = None
    for line in out.splitlines():
        if line.startswith("Dump of file") or line.startswith("File Type:"):
            continue
        m = re.match(r"^([A-Za-z_][\w$]*)\s+SEGMENT", line)
        if m:
            cur = m.group(1)
            sections.setdefault(cur, [])
            continue
        if re.match(r"^\s*[A-Za-z_][\w$]*\s+ENDS\b", line):
            cur = None
            continue
        m = re.match(r"^([A-Za-z_][\w$@?]*):\s*$", line)
        if m:
            cur = m.group(1)
            sections.setdefault(cur, [])
            continue
        if cur is None:
            continue
        m = _CONT_LINE.match(line)
        if m and sections.get(cur):
            extra = bytes.fromhex(m.group(1).replace(" ", ""))
            sections[cur][-1].raw += extra
            continue
        m = _DISASM_LINE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        raw = bytes.fromhex(m.group(2).replace(" ", ""))
        text = m.group(3)
        sections[cur].append(DisasmLine(addr, raw, text, cur))
    return {k: v for k, v in sections.items() if v}


if __name__ == "__main__":
    try:
        t = discover_tools(refresh="--refresh" in sys.argv)
        print("tools:", json.dumps(t, indent=1))
    except GroundTruthError as e:
        print("ERROR:", e)
        sys.exit(1)
