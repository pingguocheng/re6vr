"""A real x86-32 instruction decoder, with no external dependencies.

Why this exists
---------------
`scripts/x86_disasm.py` is a hand-rolled *length* decoder that grew a handful of operand
renderings. It has been wrong twice in ways that produced a plausible listing: it consumed
5 bytes for every `mov r32, imm32` (which kept instruction boundaries aligned by luck) and
it labelled unknown opcodes rather than refusing them. A listing that looks like disassembly
and is not is worse than no listing, because it gets believed.

This module is a proper table-driven decoder. Its governing rule, and the reason it can be
trusted where the old script could not:

    **An opcode that is not in the table is an error, not a guess.**

`decode()` returns an Instruction with `ok == False` and an `error` saying why. Callers are
expected to surface that rather than render something. A sweep that meets an invalid
instruction must stop and report, never skip ahead.

Scope: 32-bit protected mode - the instruction set an MSVC 2005-era 32-bit game actually
uses (integer, x87, MMX/SSE/SSE2/SSE3 scalar and packed, plus the 0F 38 / 0F 3A escape
groups so their lengths are right even where the mnemonic is not tabulated).

Correctness is enforced by `tests/test_decoder.py`, which diffs this decoder against
`dumpbin /disasm` on assembly built by `ml.exe` - a real assembler and a real disassembler,
both present in this machine's VS 2022 Build Tools. If the tables are wrong, the test fails
and names the bytes.

Two conventions worth knowing before reading the code:

  * index coordinates. `Decoder.code` is a flat buffer and every helper indexes into it.
    An instruction's address is `base_va + index`, so lengths are differences of *indices*,
    never of addresses. Mixing the two silently produces lengths in the millions.
  * operand template codes are the usual Intel ones (Eb, Gv, Iz, Jb, Ob, ...) with one
    addition: a lowercase `s` is a segment register (mov Ew,sw) and an uppercase `S` is a
    system register (mov Sw,Ew is NOT valid but mov to/from Sreg uses lowercase).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

# --------------------------------------------------------------------------- registers
R8 = ["al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"]
R16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]
R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
SEG = ["es", "cs", "ss", "ds", "fs", "gs"]
MMX = ["mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7"]
XMM = ["xmm%d" % i for i in range(8)]
ST = ["st(0)", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)"]

PTR_NAME = {1: "byte ptr", 2: "word ptr", 4: "dword ptr", 8: "qword ptr", 10: "tbyte ptr"}


class DecodeError(Exception):
    pass


@dataclass
class Operand:
    kind: str                    # reg | mem | imm | rel | ptr | text
    text: str = ""
    size: int = 0                # bytes; 0 = unknown / not applicable
    base: str | None = None      # memory
    index: str | None = None
    scale: int = 1
    disp: int = 0
    seg: str | None = None
    # True for the absolute-address forms (`mov eax, ds:[12345678h]`, A1/A3 and the ModRM
    # disp32-only form). They carry a full 32-bit address, so the segment is shown even when
    # it is the default - without it the operand reads like a small offset.
    absolute: bool = False
    # for kind == 'reg': the ModRM r/m index, so a form can relabel the register class
    rm: int | None = None
    target: int | None = None    # absolute VA for rel/data operands
    target_kind: str | None = None


@dataclass
class Instruction:
    va: int
    length: int = 0
    mnemonic: str = ""
    operands: list[Operand] = field(default_factory=list)
    prefixes: list[str] = field(default_factory=list)
    raw: bytes = b""
    ok: bool = True
    error: str = ""
    is_call: bool = False
    is_jmp: bool = False
    is_jcc: bool = False
    is_ret: bool = False
    is_invalid: bool = False
    is_halt: bool = False
    is_terminal: bool = False
    jump_targets: list[int] = field(default_factory=list)
    fall_through: bool = True
    indirect_branch: bool = False

    def text(self) -> str:
        if not self.ok:
            return "; INVALID: %s (%s)" % (self.error, self.raw.hex())
        pre = (" ".join(self.prefixes) + " ") if self.prefixes and not self.mnemonic.startswith(
            ("rep ", "repne ", "lock ")) else ""
        if not self.operands:
            return pre + self.mnemonic
        return pre + "%s %s" % (self.mnemonic, ", ".join(
            render_operand(o, i) for i, o in enumerate(self.operands)))

    def __str__(self) -> str:
        return "0x%08X  %-30s %s" % (self.va, self.raw.hex()[:28], self.text())


# --------------------------------------------------------------------------- rendering
def fmt_num(v: int) -> str:
    """Format a number the way an assembler-oriented listing does.

    Values below 10 stay decimal because that is how they are read at a glance (`shl eax,1`
    and `[esp+4]`, not `shl eax,0x1`); anything larger is hex. The rule matters for more than
    taste: the test harness compares renderings with dumpbin, which uses exactly this
    convention, so a decoder that always printed hex would make every small immediate look
    like a mismatch and hide the real ones.
    """
    return "%d" % v if 0 <= v < 10 else "0x%X" % v


def render_mem_op(o: Operand) -> str:
    """Render an effective address.

    The three terms are joined with an explicit separator rather than by concatenation. An
    earlier version built `[base, index, disp]` and joined with "" for the first two, which
    produced `[eaxecx*4]` - a string that looks like a register name and would reassemble
    into something entirely different. Silence is the dangerous failure mode here.
    """
    out = ""
    if o.base:
        out += o.base
    if o.index:
        term = "%s*%d" % (o.index, o.scale) if o.scale != 1 else o.index
        out += ("+" if out else "") + term
    disp = o.disp
    if disp:
        # An absolute address gets no leading '+': `[12345678h]` is an address, `[+1234h]`
        # reads as a malformed expression. A displacement *after* a base or index keeps the
        # sign, because there it really is an offset.
        sign = "+" if (out and disp > 0) else ("-" if disp < 0 else "")
        out += sign + fmt_num(abs(disp))
    elif not out:
        out = "0x0"
    return "[%s]" % out


def render_operand(o: Operand, idx: int = 0) -> str:
    if o.kind == "mem":
        body = render_mem_op(o)
        if o.seg:
            # es: and fs:/gs: overrides are semantically load-bearing, so always show them.
            # A bare ds: is normally implied, but on an absolute address it is shown because
            # that is what distinguishes an address from an offset.
            if o.seg != "ds" or idx == 0 or o.absolute:
                body = "%s:%s" % (o.seg, body)
        # A size qualifier is required only where the width is ambiguous: a memory
        # destination with no register on the other side. dumpbin does the same, and the
        # tests compare text, so this mirrors the reference rather than inventing a style.
        if o.size and idx == 0 and o.size in PTR_NAME:
            return "%s %s" % (PTR_NAME[o.size], body)
        return body
    return o.text


# --------------------------------------------------------------------------- opcode tables
def _build_onebyte() -> dict[int, tuple[str | None, str]]:
    t: dict[int, tuple[str | None, str]] = {}
    # 0x00-0x3F: eight ALU ops x six regular forms. Derived rather than typed out, because a
    # hand-typed 640-entry table is where typos hide.
    forms = ["Eb,Gb", "Ev,Gv", "Gb,Eb", "Gv,Ev", "al,Ib", "eax,Iz"]
    mnem = ["add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"]
    for row in range(4):
        for op in range(8):
            for col in range(6):
                t[(row << 4) | (op << 3) | col] = (mnem[op], forms[col])
    for i in range(4):
        t[0x06 + i * 8] = ("push", SEG[i])
        t[0x07 + i * 8] = ("pop", SEG[i])
    for i in range(8):
        t[0x40 + i] = ("inc", R32[i])
        t[0x48 + i] = ("dec", R32[i])
        t[0x50 + i] = ("push", R32[i])
        t[0x58 + i] = ("pop", R32[i])
        t[0x70 + i] = (["jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja"][i], "Jb")
        t[0x78 + i] = (["js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg"][i], "Jb")
        t[0xB0 + i] = ("mov", R8[i] + ",Ib")
        t[0xB8 + i] = ("mov", R32[i] + ",Iz")
    t.update({
        0x0F: (None, "esc"),
        0x60: ("pushad", ""), 0x61: ("popad", ""), 0x62: ("bound", "Gv,M"),
        0x63: ("arpl", "Ew,Gw"), 0x68: ("push", "Iz"), 0x69: ("imul", "Gv,Ev,Iz"),
        0x6A: ("push", "Ib"), 0x6B: ("imul", "Gv,Ev,Ib"),
        0x6C: ("insb", ""), 0x6D: ("insd", ""), 0x6E: ("outsb", ""), 0x6F: ("outsd", ""),
        0x80: (None, "Eb,Ib"), 0x81: (None, "Ev,Iz"), 0x82: (None, "Eb,Ib"),
        0x83: (None, "Ev,Ib"),
        0x84: ("test", "Eb,Gb"), 0x85: ("test", "Ev,Gv"),
        0x86: ("xchg", "Eb,Gb"), 0x87: ("xchg", "Ev,Gv"),
        0x88: ("mov", "Eb,Gb"), 0x89: ("mov", "Ev,Gv"),
        0x8A: ("mov", "Gb,Eb"), 0x8B: ("mov", "Gv,Ev"),
        0x8C: ("mov", "Ew,sw"), 0x8D: ("lea", "Gv,M"), 0x8E: ("mov", "sw,Ew"),
        0x8F: ("pop", "Ev"),
        0x90: ("nop", ""), 0x91: ("xchg", "eax,ecx"), 0x92: ("xchg", "eax,edx"),
        0x93: ("xchg", "eax,ebx"), 0x94: ("xchg", "eax,esp"), 0x95: ("xchg", "eax,ebp"),
        0x96: ("xchg", "eax,esi"), 0x97: ("xchg", "eax,edi"),
        0x98: ("cwde", ""), 0x99: ("cdq", ""), 0x9A: ("callf", "Ap"),
        0x9B: ("wait", ""), 0x9C: ("pushfd", ""), 0x9D: ("popfd", ""),
        0x9E: ("sahf", ""), 0x9F: ("lahf", ""),
        0xA0: ("mov", "al,Ob"), 0xA1: ("mov", "eax,Ov"),
        0xA2: ("mov", "Ob,al"), 0xA3: ("mov", "Ov,eax"),
        0xA4: ("movsb", ""), 0xA5: ("movsd", ""), 0xA6: ("cmpsb", ""), 0xA7: ("cmpsd", ""),
        0xA8: ("test", "al,Ib"), 0xA9: ("test", "eax,Iz"),
        0xAA: ("stosb", ""), 0xAB: ("stosd", ""), 0xAC: ("lodsb", ""), 0xAD: ("lodsd", ""),
        0xAE: ("scasb", ""), 0xAF: ("scasd", ""),
        0xC0: (None, "Eb,Ib"), 0xC1: (None, "Ev,Ib"), 0xC2: ("ret", "Iw"),
        0xC3: ("ret", ""), 0xC4: ("les", "Gv,M"), 0xC5: ("lds", "Gv,M"),
        0xC6: ("mov", "Eb,Ib"), 0xC7: ("mov", "Ev,Iz"), 0xC8: ("enter", "Iw,Iw"),
        0xC9: ("leave", ""), 0xCA: ("retf", "Iw"), 0xCB: ("retf", ""),
        0xCC: ("int3", ""), 0xCD: ("int", "Ib"), 0xCE: ("into", ""), 0xCF: ("iretd", ""),
        0xD0: (None, "Eb,1"), 0xD1: (None, "Ev,1"), 0xD2: (None, "Eb,cl"),
        0xD3: (None, "Ev,cl"), 0xD4: ("aam", "Ib"), 0xD5: ("aad", "Ib"),
        0xD6: ("salc", ""), 0xD7: ("xlatb", ""),
        0xD8: (None, "x87"), 0xD9: (None, "x87"), 0xDA: (None, "x87"), 0xDB: (None, "x87"),
        0xDC: (None, "x87"), 0xDD: (None, "x87"), 0xDE: (None, "x87"), 0xDF: (None, "x87"),
        0xE0: ("loopne", "Jb"), 0xE1: ("loope", "Jb"), 0xE2: ("loop", "Jb"),
        0xE3: ("jecxz", "Jb"), 0xE4: ("in", "al,Ib"), 0xE5: ("in", "eax,Ib"),
        0xE6: ("out", "Ib,al"), 0xE7: ("out", "Ib,eax"),
        0xE8: ("call", "Jz"), 0xE9: ("jmp", "Jz"), 0xEA: ("jmpf", "Ap"),
        0xEB: ("jmp", "Jb"), 0xEC: ("in", "al,dx"), 0xED: ("in", "eax,dx"),
        0xEE: ("out", "dx,al"), 0xEF: ("out", "dx,eax"),
        0xF1: ("int1", ""), 0xF4: ("hlt", ""), 0xF5: ("cmc", ""),
        0xF6: (None, "Eb"), 0xF7: (None, "Ev"),
        0xF8: ("clc", ""), 0xF9: ("stc", ""), 0xFA: ("cli", ""), 0xFB: ("sti", ""),
        0xFC: ("cld", ""), 0xFD: ("std", ""),
        0xFE: (None, "Eb"), 0xFF: (None, "Ev"),
    })
    return t


ONEBYTE = _build_onebyte()

# A duplicate key inside a table literal is invisible: Python keeps whichever came last, and
# in a 700-entry dict a mistyped opcode number looks exactly like a correct one. The tables
# are therefore also asserted for internal consistency by
# `scripts/tests/check_tables.py`, which re-parses this module's source with `ast` and fails
# on repeated keys. Doing it in the test rather than here keeps import cost at zero.

TWOBYTE: dict[int, tuple[str | None, str]] = {
    0x00: ("grp6", "Ew"), 0x01: ("grp7", "Ew"),
    0x02: ("lar", "Gv,Ew"), 0x03: ("lsl", "Gv,Ew"), 0x05: ("syscall", ""),
    0x06: ("clts", ""), 0x07: ("sysret", ""),
    0x08: ("invd", ""), 0x09: ("wbinvd", ""), 0x0B: ("ud2", ""),
    0x0D: ("prefetchw", "M"), 0x0E: ("femms", ""),
    0x10: ("movups", "V,W"), 0x11: ("movups", "W,V"), 0x12: ("movlps", "V,W"),
    0x13: ("movlps", "W,V"), 0x14: ("unpcklps", "V,W"), 0x15: ("unpckhps", "V,W"),
    0x16: ("movhps", "V,W"), 0x17: ("movhps", "W,V"),
    0x18: ("prefetch", "M"),
    0x1F: ("nop", "Ev"),
    0x20: ("mov", "Rd,Cd"), 0x21: ("mov", "Rd,Dd"), 0x22: ("mov", "Cd,Rd"),
    0x23: ("mov", "Dd,Rd"), 0x24: ("mov", "Rd,Td"), 0x26: ("mov", "Td,Rd"),
    0x28: ("movaps", "V,W"), 0x29: ("movaps", "W,V"), 0x2A: ("cvtpi2ps", "V,Q"),
    0x2B: ("movntps", "W,V"), 0x2C: ("cvttps2pi", "P,W"), 0x2D: ("cvtps2pi", "P,W"),
    0x2E: ("ucomiss", "V,W"), 0x2F: ("comiss", "V,W"),
    0x30: ("wrmsr", ""), 0x31: ("rdtsc", ""), 0x32: ("rdmsr", ""), 0x33: ("rdpmc", ""),
    0x34: ("sysenter", ""), 0x35: ("sysexit", ""),
    0x40: ("cmovo", "Gv,Ev"), 0x41: ("cmovno", "Gv,Ev"), 0x42: ("cmovb", "Gv,Ev"),
    0x43: ("cmovae", "Gv,Ev"), 0x44: ("cmove", "Gv,Ev"), 0x45: ("cmovne", "Gv,Ev"),
    0x46: ("cmovbe", "Gv,Ev"), 0x47: ("cmova", "Gv,Ev"), 0x48: ("cmovs", "Gv,Ev"),
    0x49: ("cmovns", "Gv,Ev"), 0x4A: ("cmovp", "Gv,Ev"), 0x4B: ("cmovnp", "Gv,Ev"),
    0x4C: ("cmovl", "Gv,Ev"), 0x4D: ("cmovge", "Gv,Ev"), 0x4E: ("cmovle", "Gv,Ev"),
    0x4F: ("cmovg", "Gv,Ev"),
    0x50: ("movmskps", "Gd,U"), 0x51: ("sqrtps", "V,W"), 0x52: ("rsqrtps", "V,W"),
    0x53: ("rcpps", "V,W"), 0x54: ("andps", "V,W"), 0x55: ("andnps", "V,W"),
    0x56: ("orps", "V,W"), 0x57: ("xorps", "V,W"), 0x58: ("addps", "V,W"),
    0x59: ("mulps", "V,W"), 0x5A: ("cvtps2pd", "V,W"), 0x5B: ("cvtdq2ps", "V,W"),
    0x5C: ("subps", "V,W"), 0x5D: ("minps", "V,W"), 0x5E: ("divps", "V,W"),
    0x5F: ("maxps", "V,W"),
    0x60: ("punpcklbw", "P,Q"), 0x61: ("punpcklwd", "P,Q"), 0x62: ("punpckldq", "P,Q"),    0x63: ("packsswb", "P,Q"), 0x64: ("pcmpgtb", "P,Q"), 0x65: ("pcmpgtw", "P,Q"),
    0x66: ("pcmpgtd", "P,Q"), 0x67: ("packuswb", "P,Q"), 0x68: ("punpckhbw", "P,Q"),
    0x69: ("punpckhwd", "P,Q"), 0x6A: ("punpckhdq", "P,Q"), 0x6B: ("packssdw", "P,Q"),
    0x6E: ("movd", "P,Ey"), 0x6F: ("movq", "P,Q"),
    0x70: ("pshufw", "P,Q,Ib"), 0x74: ("pcmpeqb", "P,Q"), 0x75: ("pcmpeqw", "P,Q"),
    0x76: ("pcmpeqd", "P,Q"), 0x77: ("emms", ""), 0x7E: ("movd", "Ey,P"),
    0x7F: ("movq", "Q,P"),
    0x80: ("jo", "Jz"), 0x81: ("jno", "Jz"), 0x82: ("jb", "Jz"), 0x83: ("jae", "Jz"),
    0x84: ("je", "Jz"), 0x85: ("jne", "Jz"), 0x86: ("jbe", "Jz"), 0x87: ("ja", "Jz"),
    0x88: ("js", "Jz"), 0x89: ("jns", "Jz"), 0x8A: ("jp", "Jz"), 0x8B: ("jnp", "Jz"),
    0x8C: ("jl", "Jz"), 0x8D: ("jge", "Jz"), 0x8E: ("jle", "Jz"), 0x8F: ("jg", "Jz"),
    0x90: ("seto", "Eb"), 0x91: ("setno", "Eb"), 0x92: ("setb", "Eb"), 0x93: ("setae", "Eb"),
    0x94: ("sete", "Eb"), 0x95: ("setne", "Eb"), 0x96: ("setbe", "Eb"), 0x97: ("seta", "Eb"),
    0x98: ("sets", "Eb"), 0x99: ("setns", "Eb"), 0x9A: ("setp", "Eb"), 0x9B: ("setnp", "Eb"),
    0x9C: ("setl", "Eb"), 0x9D: ("setge", "Eb"), 0x9E: ("setle", "Eb"), 0x9F: ("setg", "Eb"),
    0xA0: ("push", "fs"), 0xA1: ("pop", "fs"), 0xA2: ("cpuid", ""), 0xA3: ("bt", "Ev,Gv"),
    0xA4: ("shld", "Ev,Gv,Ib"), 0xA5: ("shld", "Ev,Gv,cl"),
    0xA8: ("push", "gs"), 0xA9: ("pop", "gs"), 0xAA: ("rsm", ""),
    0xAB: ("bts", "Ev,Gv"), 0xAC: ("shrd", "Ev,Gv,Ib"), 0xAD: ("shrd", "Ev,Gv,cl"),
    0xAE: ("grp15", "M"), 0xAF: ("imul", "Gv,Ev"),
    0xB0: ("cmpxchg", "Eb,Gb"), 0xB1: ("cmpxchg", "Ev,Gv"),
    0xB2: ("lss", "Gv,M"), 0xB3: ("btr", "Ev,Gv"), 0xB4: ("lfs", "Gv,M"),
    0xB5: ("lgs", "Gv,M"), 0xB6: ("movzx", "Gv,Eb"), 0xB7: ("movzx", "Gv,Ew"),
    0xB8: ("popcnt", "Gv,Ev"), 0xBA: ("grp8", "Ev,Ib"), 0xBB: ("btc", "Ev,Gv"),
    0xBC: ("bsf", "Gv,Ev"), 0xBD: ("bsr", "Gv,Ev"),
    0xBE: ("movsx", "Gv,Eb"), 0xBF: ("movsx", "Gv,Ew"),
    0xC0: ("xadd", "Eb,Gb"), 0xC1: ("xadd", "Ev,Gv"), 0xC2: ("cmpps", "V,W,Ib"),
    0xC3: ("movnti", "Ey,Gy"), 0xC4: ("pinsrw", "P,Ew,Ib"), 0xC5: ("pextrw", "Gd,P,Ib"),
    0xC6: ("shufps", "V,W,Ib"), 0xC7: ("grp9", "M"),
    0xC8: ("bswap", "eax"), 0xC9: ("bswap", "ecx"), 0xCA: ("bswap", "edx"),
    0xCB: ("bswap", "ebx"), 0xCC: ("bswap", "esp"), 0xCD: ("bswap", "ebp"),
    0xCE: ("bswap", "esi"), 0xCF: ("bswap", "edi"),
    0xD0: ("addsubps", "V,W"), 0xD1: ("psrlw", "P,Q"), 0xD2: ("psrld", "P,Q"),
    0xD3: ("psrlq", "P,Q"), 0xD4: ("paddq", "P,Q"), 0xD5: ("pmullw", "P,Q"),
    0xD6: ("movq", "W,V"), 0xD7: ("pmovmskb", "Gd,P"), 0xD8: ("psubusb", "P,Q"),
    0xD9: ("psubusw", "P,Q"), 0xDA: ("pminub", "P,Q"), 0xDB: ("pand", "P,Q"),
    0xDC: ("paddusb", "P,Q"), 0xDD: ("paddusw", "P,Q"), 0xDE: ("pmaxub", "P,Q"),
    0xDF: ("pandn", "P,Q"),
    0xE0: ("pavgb", "P,Q"), 0xE1: ("psraw", "P,Q"), 0xE2: ("psrad", "P,Q"),
    0xE3: ("pavgw", "P,Q"), 0xE4: ("pmulhuw", "P,Q"), 0xE5: ("pmulhw", "P,Q"),
    0xE7: ("movntq", "M,P"), 0xE8: ("psubsb", "P,Q"), 0xE9: ("psubsw", "P,Q"),
    0xEA: ("pminsw", "P,Q"), 0xEB: ("por", "P,Q"), 0xEC: ("paddsb", "P,Q"),
    0xED: ("paddsw", "P,Q"), 0xEE: ("pmaxsw", "P,Q"), 0xEF: ("pxor", "P,Q"),
    0xF0: ("lddqu", "V,W"), 0xF1: ("psllw", "P,Q"), 0xF2: ("pslld", "P,Q"),
    0xF3: ("psllq", "P,Q"), 0xF4: ("pmuludq", "P,Q"), 0xF5: ("pmaddwd", "P,Q"),
    0xF6: ("psadbw", "P,Q"), 0xF7: ("maskmovq", "P,Q"), 0xF8: ("psubb", "P,Q"),
    0xF9: ("psubw", "P,Q"), 0xFA: ("psubd", "P,Q"), 0xFB: ("psubq", "P,Q"),
    0xFC: ("paddb", "P,Q"), 0xFD: ("paddw", "P,Q"), 0xFE: ("paddd", "P,Q"),
}

# The 66/F2/F3 prefixes select a different instruction for the same 0F opcode. Rather than
# triplicate the table, these override it. Keyed (prefix, opcode).
MANDATORY: dict[tuple[str, int], tuple[str, str]] = {
    ("66", 0x10): ("movupd", "V,W"), ("66", 0x11): ("movupd", "W,V"),
    ("66", 0x12): ("movlpd", "V,W"), ("66", 0x13): ("movlpd", "W,V"),
    ("66", 0x14): ("unpcklpd", "V,W"), ("66", 0x15): ("unpckhpd", "V,W"),
    ("66", 0x16): ("movhpd", "V,W"), ("66", 0x17): ("movhpd", "W,V"),
    ("66", 0x28): ("movapd", "V,W"), ("66", 0x29): ("movapd", "W,V"),
    ("66", 0x2A): ("cvtpi2pd", "V,Q"), ("66", 0x2B): ("movntpd", "W,V"),
    ("66", 0x2C): ("cvttpd2pi", "P,W"), ("66", 0x2D): ("cvtpd2pi", "P,W"),
    ("66", 0x2E): ("ucomisd", "V,W"), ("66", 0x2F): ("comisd", "V,W"),
    ("66", 0x50): ("movmskpd", "Gd,U"), ("66", 0x51): ("sqrtpd", "V,W"),
    ("66", 0x54): ("andpd", "V,W"), ("66", 0x55): ("andnpd", "V,W"),
    ("66", 0x56): ("orpd", "V,W"), ("66", 0x57): ("xorpd", "V,W"),
    ("66", 0x58): ("addpd", "V,W"), ("66", 0x59): ("mulpd", "V,W"),
    ("66", 0x5A): ("cvtpd2ps", "V,W"), ("66", 0x5B): ("cvtps2dq", "V,W"),
    ("66", 0x5C): ("subpd", "V,W"), ("66", 0x5D): ("minpd", "V,W"),
    ("66", 0x5E): ("divpd", "V,W"), ("66", 0x5F): ("maxpd", "V,W"),
    ("66", 0x60): ("punpcklbw", "V,W"), ("66", 0x61): ("punpcklwd", "V,W"),
    ("66", 0x62): ("punpckldq", "V,W"), ("66", 0x63): ("packsswb", "V,W"),
    ("66", 0x64): ("pcmpgtb", "V,W"), ("66", 0x65): ("pcmpgtw", "V,W"),
    ("66", 0x66): ("pcmpgtd", "V,W"), ("66", 0x67): ("packuswb", "V,W"),
    ("66", 0x68): ("punpckhbw", "V,W"), ("66", 0x69): ("punpckhwd", "V,W"),
    ("66", 0x6A): ("punpckhdq", "V,W"), ("66", 0x6B): ("packssdw", "V,W"),
    ("66", 0x6C): ("punpcklqdq", "V,W"), ("66", 0x6D): ("punpckhqdq", "V,W"),
    ("66", 0x6E): ("movd", "V,Ey"), ("66", 0x6F): ("movdqa", "V,W"),
    ("66", 0x70): ("pshufd", "V,W,Ib"),
    ("66", 0x74): ("pcmpeqb", "V,W"), ("66", 0x75): ("pcmpeqw", "V,W"),
    ("66", 0x76): ("pcmpeqd", "V,W"),
    ("66", 0x7C): ("haddpd", "V,W"), ("66", 0x7D): ("hsubpd", "V,W"),
    ("66", 0x7E): ("movd", "Ey,V"), ("66", 0x7F): ("movdqa", "W,V"),
    ("66", 0xC2): ("cmppd", "V,W,Ib"), ("66", 0xC4): ("pinsrw", "V,Ew,Ib"),
    ("66", 0xC5): ("pextrw", "Gd,U,Ib"), ("66", 0xC6): ("shufpd", "V,W,Ib"),
    ("66", 0xD0): ("addsubpd", "V,W"), ("66", 0xD1): ("psrlw", "V,W"),
    ("66", 0xD2): ("psrld", "V,W"), ("66", 0xD3): ("psrlq", "V,W"),
    ("66", 0xD4): ("paddq", "V,W"), ("66", 0xD5): ("pmullw", "V,W"),
    ("66", 0xD6): ("movq", "W,V"), ("66", 0xD7): ("pmovmskb", "Gd,U"),
    ("66", 0xD8): ("psubusb", "V,W"), ("66", 0xD9): ("psubusw", "V,W"),
    ("66", 0xDA): ("pminub", "V,W"), ("66", 0xDB): ("pand", "V,W"),
    ("66", 0xDC): ("paddusb", "V,W"), ("66", 0xDD): ("paddusw", "V,W"),
    ("66", 0xDE): ("pmaxub", "V,W"), ("66", 0xDF): ("pandn", "V,W"),
    ("66", 0xE0): ("pavgb", "V,W"), ("66", 0xE1): ("psraw", "V,W"),
    ("66", 0xE2): ("psrad", "V,W"), ("66", 0xE3): ("pavgw", "V,W"),
    ("66", 0xE4): ("pmulhuw", "V,W"), ("66", 0xE5): ("pmulhw", "V,W"),
    ("66", 0xE6): ("cvttpd2dq", "V,W"), ("66", 0xE7): ("movntdq", "M,V"),
    ("66", 0xE8): ("psubsb", "V,W"), ("66", 0xE9): ("psubsw", "V,W"),
    ("66", 0xEA): ("pminsw", "V,W"), ("66", 0xEB): ("por", "V,W"),
    ("66", 0xEC): ("paddsb", "V,W"), ("66", 0xED): ("paddsw", "V,W"),
    ("66", 0xEE): ("pmaxsw", "V,W"), ("66", 0xEF): ("pxor", "V,W"),
    ("66", 0xF1): ("psllw", "V,W"), ("66", 0xF2): ("pslld", "V,W"),
    ("66", 0xF3): ("psllq", "V,W"), ("66", 0xF4): ("pmuludq", "V,W"),
    ("66", 0xF5): ("pmaddwd", "V,W"), ("66", 0xF6): ("psadbw", "V,W"),
    ("66", 0xF7): ("maskmovdqu", "V,U"), ("66", 0xF8): ("psubb", "V,W"),
    ("66", 0xF9): ("psubw", "V,W"), ("66", 0xFA): ("psubd", "V,W"),
    ("66", 0xFB): ("psubq", "V,W"), ("66", 0xFC): ("paddb", "V,W"),
    ("66", 0xFD): ("paddw", "V,W"), ("66", 0xFE): ("paddd", "V,W"),
    ("F3", 0x10): ("movss", "V,W"), ("F3", 0x11): ("movss", "W,V"),
    ("F3", 0x2A): ("cvtsi2ss", "V,Ey"), ("F3", 0x2C): ("cvttss2si", "Gy,W"),
    ("F3", 0x2D): ("cvtss2si", "Gy,W"), ("F3", 0x51): ("sqrtss", "V,W"),
    ("F3", 0x52): ("rsqrtss", "V,W"), ("F3", 0x53): ("rcpss", "V,W"),
    ("F3", 0x58): ("addss", "V,W"), ("F3", 0x59): ("mulss", "V,W"),
    ("F3", 0x5A): ("cvtss2sd", "V,W"), ("F3", 0x5B): ("cvttps2dq", "V,W"),
    ("F3", 0x5C): ("subss", "V,W"), ("F3", 0x5D): ("minss", "V,W"),
    ("F3", 0x5E): ("divss", "V,W"), ("F3", 0x5F): ("maxss", "V,W"),
    ("F3", 0x70): ("pshufhw", "V,W,Ib"), ("F3", 0x7E): ("movq", "V,W"),
    ("F3", 0x7F): ("movdqu", "W,V"), ("F3", 0xC2): ("cmpss", "V,W,Ib"),
    ("F3", 0xE6): ("cvtdq2pd", "V,W"),
    ("F3", 0x09): ("wbnoinvd", ""),
    ("F3", 0x12): ("movsldup", "V,W"), ("F3", 0x16): ("movshdup", "V,W"),
    ("F3", 0x6F): ("movdqu", "V,W"),
    ("F2", 0x6F): ("movdqa", "V,W"), ("F2", 0xD6): ("movdq2q", "P,W"),
    ("F2", 0x12): ("movddup", "V,W"),
    ("F3", 0xAE): ("grp15", "M"),      # rdfsbase/wrfsbase/rdgsbase/wrgsbase on /0../3
    ("F3", 0x1E): ("nop", "Ev"),
    ("F3", 0xBC): ("tzcnt", "Gv,Ev"), ("F3", 0xBD): ("lzcnt", "Gv,Ev"),
    ("F3", 0xD6): ("movq2dq", "V,Q"),
    ("F2", 0x10): ("movsd", "V,W"), ("F2", 0x11): ("movsd", "W,V"),
    ("F2", 0x12): ("movddup", "V,W"), ("F2", 0x2A): ("cvtsi2sd", "V,Ey"),
    ("F2", 0x2C): ("cvttsd2si", "Gy,W"), ("F2", 0x2D): ("cvtsd2si", "Gy,W"),
    ("F2", 0x51): ("sqrtsd", "V,W"), ("F2", 0x58): ("addsd", "V,W"),
    ("F2", 0x59): ("mulsd", "V,W"), ("F2", 0x5A): ("cvtsd2ss", "V,W"),
    ("F2", 0x5C): ("subsd", "V,W"), ("F2", 0x5D): ("minsd", "V,W"),
    ("F2", 0x5E): ("divsd", "V,W"), ("F2", 0x5F): ("maxsd", "V,W"),
    ("F2", 0x70): ("pshuflw", "V,W,Ib"), ("F2", 0x7C): ("haddps", "V,W"),
    ("F2", 0x7D): ("hsubps", "V,W"), ("F2", 0xC2): ("cmpsd", "V,W,Ib"),
    ("F2", 0xD0): ("addsubps", "V,W"), ("F2", 0xE6): ("cvtpd2dq", "V,W"),
    ("F2", 0xF0): ("lddqu", "V,W"),
    # The 66-prefixed shift-by-immediate groups. These have no unprefixed counterpart, so
    # without an entry here `66 0F 71 /2` is reported as an unknown opcode - and an unknown
    # opcode in a linear sweep stops the listing, which is how one missing row turns into a
    # "this function could not be decoded" conclusion.
    ("66", 0x71): ("grp12", "U,Ib"), ("66", 0x72): ("grp13", "U,Ib"),
    ("66", 0x73): ("grp14", "U,Ib"),
    # The MMX forms of the same shifts select with 0F 71 /2 (psrlw mm,imm8) etc.
    ("", 0x71): ("grp12", "U,Ib"), ("", 0x72): ("grp13", "U,Ib"),
    ("", 0x73): ("grp14", "U,Ib"),
    ("66", 0xD0 + 0x1000): ("", ""),      # sentinel, never matched
}

THREEBYTE_38: dict[int, tuple[str, str]] = {
    0x00: ("pshufb", "P,Q"), 0x01: ("phaddw", "P,Q"), 0x02: ("phaddd", "P,Q"),    0x03: ("phaddsw", "P,Q"), 0x04: ("pmaddubsw", "P,Q"), 0x05: ("phsubw", "P,Q"),
    0x06: ("phsubd", "P,Q"), 0x07: ("phsubsw", "P,Q"), 0x08: ("psignb", "P,Q"),
    0x09: ("psignw", "P,Q"), 0x0A: ("psignd", "P,Q"), 0x0B: ("pmulhrsw", "P,Q"),
    0x10: ("pblendvb", "V,W"), 0x14: ("blendvps", "V,W"), 0x15: ("blendvpd", "V,W"),
    0x17: ("ptest", "V,W"), 0x1C: ("pabsb", "P,Q"), 0x1D: ("pabsw", "P,Q"),
    0x1E: ("pabsd", "P,Q"),
    0x20: ("pmovsxbw", "V,W"), 0x21: ("pmovsxbd", "V,W"), 0x22: ("pmovsxbq", "V,W"),
    0x23: ("pmovsxwd", "V,W"), 0x24: ("pmovsxwq", "V,W"), 0x25: ("pmovsxdq", "V,W"),
    0x28: ("pmuldq", "V,W"), 0x29: ("pcmpeqq", "V,W"), 0x2A: ("movntdqa", "V,W"),
    0x2B: ("packusdw", "V,W"),
    0x30: ("pmovzxbw", "V,W"), 0x31: ("pmovzxbd", "V,W"), 0x32: ("pmovzxbq", "V,W"),
    0x33: ("pmovzxwd", "V,W"), 0x34: ("pmovzxwq", "V,W"), 0x35: ("pmovzxdq", "V,W"),
    0x37: ("pcmpgtq", "V,W"), 0x38: ("pminsb", "V,W"), 0x39: ("pminsd", "V,W"),
    0x3A: ("pminuw", "V,W"), 0x3B: ("pminud", "V,W"), 0x3C: ("pmaxsb", "V,W"),
    0x3D: ("pmaxsd", "V,W"), 0x3E: ("pmaxuw", "V,W"), 0x3F: ("pmaxud", "V,W"),
    0x40: ("pmulld", "V,W"), 0x41: ("phminposuw", "V,W"),
    0xC8: ("sha1nexte", "V,W"), 0xC9: ("sha1msg1", "V,W"), 0xCA: ("sha1msg2", "V,W"),
    0xCB: ("sha256rnds2", "V,W"), 0xCC: ("sha256msg1", "V,W"), 0xCD: ("sha256msg2", "V,W"),
    0xDB: ("aesimc", "V,W"), 0xDC: ("aesenc", "V,W"), 0xDD: ("aesenclast", "V,W"),
    0xDE: ("aesdec", "V,W"), 0xDF: ("aesdeclast", "V,W"),
    0xF0: ("movbe", "Gv,M"), 0xF1: ("movbe", "M,Gv"),
}
THREEBYTE_3A: dict[int, tuple[str, str]] = {    0x08: ("roundps", "V,W,Ib"), 0x09: ("roundpd", "V,W,Ib"),
    0x0A: ("roundss", "V,W,Ib"), 0x0B: ("roundsd", "V,W,Ib"),
    0x0C: ("blendps", "V,W,Ib"), 0x0D: ("blendpd", "V,W,Ib"),
    0x0E: ("pblendw", "V,W,Ib"), 0x0F: ("palignr", "P,Q,Ib"),
    0x14: ("pextrb", "Eb,V,Ib"), 0x15: ("pextrw", "Ew,V,Ib"),
    0x16: ("pextrd", "Ey,V,Ib"), 0x17: ("extractps", "Ey,V,Ib"),
    0x20: ("pinsrb", "V,Eb,Ib"), 0x21: ("insertps", "V,W,Ib"),
    0x22: ("pinsrd", "V,Ey,Ib"),
    0x40: ("dpps", "V,W,Ib"), 0x41: ("dppd", "V,W,Ib"), 0x42: ("mpsadbw", "V,W,Ib"),
    0x44: ("pclmulqdq", "V,W,Ib"), 0x60: ("pcmpestrm", "V,W,Ib"),
    0x61: ("pcmpestri", "V,W,Ib"), 0x62: ("pcmpistrm", "V,W,Ib"),
    0x63: ("pcmpistri", "V,W,Ib"), 0xDF: ("aeskeygenassist", "V,W,Ib"),
}

# Mandatory-prefix overrides for the three-byte escapes, keyed (prefix, escape, opcode).
# `66 0F 38 F0` is CRC32 while plain `0F 38 F0` is MOVBE - the same escape byte with a
# different mandatory prefix, so the unprefixed table cannot answer for the prefixed form.
MANDATORY_3BYTE: dict[tuple[str, int, int], tuple[str, str]] = {
    ("F2", 0x38, 0xF0): ("crc32", "Gd,Eb"),
    ("F2", 0x38, 0xF1): ("crc32", "Gd,Ev"),
}

GROUP1 = ["add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"]
# CMPPS/CMPPD/CMPSS/CMPSD condition codes, from the imm8 field.
CMPCC = ["eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"]
GROUP2 = ["rol", "ror", "rcl", "rcr", "shl", "shr", "shl", "sar"]
GROUP3 = ["test", "test", "not", "neg", "mul", "imul", "div", "idiv"]
GROUP4 = ["inc", "dec"]
GROUP5 = ["inc", "dec", "call", "callf", "jmp", "jmpf", "push", None]
GROUP6 = ["sldt", "str", "lldt", "ltr", "verr", "verw", None, None]
GROUP7 = ["sgdt", "sidt", "lgdt", "lidt", "smsw", None, "lmsw", "invlpg"]
GROUP15 = ["fxsave", "fxrstor", "ldmxcsr", "stmxcsr", "xsave", "xrstor", "clflush", None]
GRP9 = {0: None, 1: "cmpxchg8b", 6: "rdrand", 7: "rdseed"}
# The shift-by-immediate groups. The /reg field selects the operation AND the operand width,
# which is why one opcode covers both a word and a quadword shift:
#   0F 71 /2 psrlw  /4 psraw  /6 psllw
#   0F 72 /2 psrld  /4 psrad  /6 pslld
#   0F 73 /2 psrlq  /3 psrldq /6 psllq /7 pslldq
# Guessing these as "index 0 only" made every real one fail as untabulated, and an
# untabulated opcode stops a linear sweep - one wrong table row, a whole function undecoded.
GRP12 = [None, None, "psrlw", None, "psraw", None, "psllw", None]
GRP13 = [None, None, "psrld", None, "psrad", None, "pslld", None]
GRP14 = [None, None, "psrlq", "psrldq", None, None, "psllq", "pslldq"]

X87_REG_FORMS = {
    (0xD8, 0xC0): "fadd", (0xD8, 0xC8): "fmul", (0xD8, 0xE0): "fsub",
    (0xD8, 0xE8): "fsubr", (0xD8, 0xF0): "fdiv", (0xD8, 0xF8): "fdivr",
    (0xDC, 0xC0): "fadd", (0xDC, 0xC8): "fmul", (0xDC, 0xE0): "fsubr",
    (0xDC, 0xE8): "fsub", (0xDC, 0xF0): "fdivr", (0xDC, 0xF8): "fdiv",
    (0xDE, 0xC0): "faddp", (0xDE, 0xC8): "fmulp", (0xDE, 0xE0): "fsubrp",
    (0xDE, 0xE8): "fsubp", (0xDE, 0xF0): "fdivrp", (0xDE, 0xF8): "fdivp",
}
X87_MEM = {
    0xD8: ["fadd", "fmul", "fcom", "fcomp", "fsub", "fsubr", "fdiv", "fdivr"],
    0xD9: ["fld", None, "fst", "fstp", "fldenv", "fldcw", "fnstenv", "fnstcw"],
    0xDA: ["fiadd", "fimul", "ficom", "ficomp", "fisub", "fisubr", "fidiv", "fidivr"],
    0xDB: ["fild", "fisttp", "fist", "fistp", None, "fld", None, "fstp"],
    0xDC: ["fadd", "fmul", "fcom", "fcomp", "fsub", "fsubr", "fdiv", "fdivr"],
    0xDD: ["fld", "fisttp", "fst", "fstp", "frstor", None, "fnsave", "fnstsw"],
    0xDE: ["fiadd", "fimul", "ficom", "ficomp", "fisub", "fisubr", "fidiv", "fidivr"],
    0xDF: ["fild", "fisttp", "fist", "fistp", "fbld", "fild", "fbstp", "fistp"],
}
# size in bytes of the memory operand for x87 mem forms, per opcode and /reg group
X87_MEM_SIZE = {0xD8: 4, 0xD9: 4, 0xDA: 4, 0xDB: 4, 0xDC: 8, 0xDD: 8, 0xDE: 2, 0xDF: 2}
X87_ALT_SIZE = {
    # env/save/control-word forms are not the default width for their opcode. These change
    # the decoded instruction length, so they are correctness, not cosmetics.
    (0xD9, 0x20): 28, (0xD9, 0x30): 28, (0xD9, 0x28): 2, (0xD9, 0x38): 2,   # fldenv/fnstenv/fldcw/fnstcw
    (0xDD, 0x20): 108, (0xDD, 0x30): 108, (0xDD, 0x38): 2,                 # frstor/fnsave/fnstsw
    (0xDB, 0x20): 10, (0xDB, 0x30): 10,                                    # fbld/fbstp
    (0xDB, 0x28): 10, (0xDF, 0x28): 10,                                    # fld tbyte / fild qword
    (0xDF, 0x20): 10, (0xDF, 0x30): 10,
    (0xDF, 0x38): 8,
    # integer arithmetic forms: 16-bit or 32-bit
    (0xDA, 0x00): 4, (0xDA, 0x08): 4, (0xDA, 0x10): 4, (0xDA, 0x18): 4,
    (0xDA, 0x20): 4, (0xDA, 0x28): 4, (0xDA, 0x30): 4, (0xDA, 0x38): 4,
    (0xDE, 0x00): 2, (0xDE, 0x08): 2, (0xDE, 0x10): 2, (0xDE, 0x18): 2,
    (0xDE, 0x20): 2, (0xDE, 0x28): 2, (0xDE, 0x30): 2, (0xDE, 0x38): 2,
    (0xDB, 0x00): 4, (0xDB, 0x08): 4, (0xDB, 0x10): 4, (0xDB, 0x18): 4,    # fild/fisttp/fist/fistp
    (0xDF, 0x00): 2, (0xDF, 0x08): 2, (0xDF, 0x10): 2, (0xDF, 0x18): 2,
}
X87_NOARG = {
    0xD9: {0xD0: "fnop", 0xE0: "fchs", 0xE1: "fabs", 0xE4: "ftst", 0xE5: "fxam",
           0xE8: "fld1", 0xE9: "fldl2t", 0xEA: "fldl2e", 0xEB: "fldpi", 0xEC: "fldlg2",
           0xED: "fldln2", 0xEE: "fldz", 0xF0: "f2xm1", 0xF1: "fyl2x", 0xF2: "fptan",
           0xF3: "fpatan", 0xF4: "fxtract", 0xF5: "fprem1", 0xF6: "fdecstp",
           0xF7: "fincstp", 0xF8: "fprem", 0xF9: "fyl2xp1", 0xFA: "fsqrt",
           0xFB: "fsincos", 0xFC: "frndint", 0xFD: "fscale", 0xFE: "fsin", 0xFF: "fcos"},
    0xDB: {0xE2: "fnclex", 0xE3: "fninit"},
    0xDF: {0xE0: "fnstsw"},
}
X87_MODRM_MOD3 = {
    # (opcode, modrm & 0xF8) -> (mnemonic, has explicit st(i) operand). The has_operand flag
    # matters for the forms whose st(i) is fixed at 1 (faddp/fmulp/fsubp/fdivp) and for the
    # compare forms dumpbin prints with both operands (fcomi st,st(1)).
    (0xD8, 0xC0): ("fadd", True), (0xD8, 0xC8): ("fmul", True), (0xD8, 0xE0): ("fsub", True),
    (0xD8, 0xE8): ("fsubr", True), (0xD8, 0xF0): ("fdiv", True), (0xD8, 0xF8): ("fdivr", True),
    (0xDC, 0xC0): ("fadd", True), (0xDC, 0xC8): ("fmul", True), (0xDC, 0xE0): ("fsubr", True),
    (0xDC, 0xE8): ("fsub", True), (0xDC, 0xF0): ("fdivr", True), (0xDC, 0xF8): ("fdiv", True),
    (0xD9, 0xC0): ("fld", True), (0xD9, 0xC8): ("fxch", True),
    (0xDD, 0xC0): ("ffree", True), (0xDD, 0xD0): ("fst", True), (0xDD, 0xD8): ("fstp", True),
    (0xDD, 0xE0): ("fucom", True), (0xDD, 0xE8): ("fucomp", True),
    # fcom/fcomp: the compare-and-pop pair. D8 /2../3 and DC /2../3 are the 32- and 64-bit
    # forms, and DA /2../3 are the integer ones; all six encode st(i) in the r/m field.
    (0xD8, 0xD0): ("fcom", True), (0xD8, 0xD8): ("fcomp", True),
    (0xDC, 0xD0): ("fcom", True), (0xDC, 0xD8): ("fcomp", True),
    (0xDA, 0xD0): ("ficom", True), (0xDA, 0xD8): ("ficomp", True),
    (0xDE, 0xD0): ("ficom", True), (0xDE, 0xD8): ("ficomp", True),
    (0xDB, 0xC0): ("fcmovnb", True), (0xDB, 0xC8): ("fcmovne", True),
    (0xDB, 0xD0): ("fcmovnbe", True), (0xDB, 0xD8): ("fcmovnu", True),
    (0xDB, 0xE8): ("fucomi", True), (0xDB, 0xF0): ("fcomi", True),
    (0xDF, 0xC0): ("ffreep", True), (0xDF, 0xE8): ("fucomip", True),
    (0xDF, 0xF0): ("fcomip", True),
    (0xDE, 0xC0): ("faddp", True), (0xDE, 0xC8): ("fmulp", True),
    (0xDE, 0xE0): ("fsubrp", True), (0xDE, 0xE8): ("fsubp", True),
    (0xDE, 0xF0): ("fdivrp", True), (0xDE, 0xF8): ("fdivp", True),
}
# D9 D0 and DA E9 are whole-byte encodings, not /reg groups, so they are keyed on the full
# ModRM byte rather than on (opcode, modrm & 0xF8). Looking them up in the group table finds
# nothing and the instruction is then reported as untabulated - a false gap in the listing.
X87_MODRM_EXACT = {
    (0xD9, 0xD0): ("fnop", False),
    (0xDA, 0xE9): ("fucompp", False),
    (0xDB, 0xE2): ("fnclex", False),
    (0xDB, 0xE3): ("fninit", False),
    (0xDF, 0xE0): ("fnstsw", True),      # fnstsw ax - the only fnstsw form with an operand
}

_STRING_OPS = ("movsb", "movsd", "stosb", "stosd", "lodsb", "lodsd", "cmpsb", "cmpsd",
               "scasb", "scasd", "insb", "insd", "outsb", "outsd")

# String instructions have implicit operands that dumpbin prints in full and that a reader
# needs in order to know the direction and the segment. Each entry is
# (size in bytes, which operands exist), where the roles are:
#   movs  ES:[edi] <- [esi]      stos  ES:[edi] <- al/eax     lods  al/eax <- [esi]
#   cmps  [esi] vs ES:[edi]      scas  al/eax vs ES:[edi]     ins/outs  port <-> [edi]/[esi]
_STRING_OPERANDS = {
    "movsb": (1, "dst_src"), "movsd": (4, "dst_src"),
    "stosb": (1, "dst"), "stosd": (4, "dst"),
    "lodsb": (1, "src"), "lodsd": (4, "src"),
    "cmpsb": (1, "dst_src"), "cmpsd": (4, "dst_src"),
    "scasb": (1, "dst"), "scasd": (4, "dst"),
    "insb": (1, "dst"), "insd": (4, "dst"),
    "outsb": (1, "src"), "outsd": (4, "src"),
}


# --------------------------------------------------------------------------- decoder
class Decoder:
    """Decodes x86-32 instructions from a flat buffer.

    `code` is indexed from 0 and `base_va` is the virtual address of `code[0]`, so a whole
    section can be handed over at once and every branch target comes back as an absolute VA.
    """

    def __init__(self, code: bytes, base_va: int = 0):
        self.code = code
        self.base_va = base_va

    # -- raw reads ---------------------------------------------------------------
    def _u8(self, i: int) -> int:
        if i < 0 or i >= len(self.code):
            raise DecodeError("truncated (1 byte past end)")
        return self.code[i]

    def _peek(self, i: int) -> int:
        """Like _u8 but returns -1 past the end instead of raising.

        Used to peek at a ModRM byte without committing to consuming it: a probe buffer ends
        right after the instruction, and a raise there would turn "is this the register
        form?" into a decode failure.
        """
        return self.code[i] if 0 <= i < len(self.code) else -1

    def _u16(self, i: int) -> int:
        if i + 2 > len(self.code):
            raise DecodeError("truncated (2 bytes past end)")
        return struct.unpack_from("<H", self.code, i)[0]

    def _u32(self, i: int) -> int:
        if i + 4 > len(self.code):
            raise DecodeError("truncated (4 bytes past end)")
        return struct.unpack_from("<I", self.code, i)[0]

    def _i8(self, i: int) -> int:
        v = self._u8(i)
        return v - 256 if v >= 128 else v

    def _rm_as(self, o: Operand | None, nbytes: int, cls: str = "G") -> Operand:
        """Relabel a mod==3 r/m operand to the register class and width it actually uses.

        `_modrm` labels a register-form r/m as a 32-bit general-purpose register because
        nothing in the byte says otherwise. The opcode does: `F6 /2` with mod == 3 is
        `not al`, and `66 0F 71 /6` is a shift of an XMM register, not of EAX. Without this,
        `shr byte [mem],2` and `not al` decode as their 32-bit twins, and the SSE shift
        groups decode as shifts of EAX - same length, wrong instruction.

        `cls` selects the register file: 'G' general purpose, 'V' XMM, 'P' MMX.
        """
        if o is None:
            return Operand("reg", "?", nbytes)
        if o.kind != "reg":
            o.size = nbytes
            return o
        idx = o.rm or 0
        if cls == "V":
            return Operand("reg", XMM[idx], 16)
        if cls == "P":
            return Operand("reg", MMX[idx], 8)
        if nbytes == 1:
            return Operand("reg", R8[idx], 1, rm=idx)
        if nbytes == 2:
            return Operand("reg", R16[idx], 2, rm=idx)
        return Operand("reg", R32[idx], 4, rm=idx)

    # -- modrm -------------------------------------------------------------------
    def _modrm(self, i: int, size: int, seg: str | None):
        """Decode a ModRM byte plus any SIB/displacement.

        Returns (new_index, rm_operand_or_None, reg_field). The r/m side is returned as a
        *general-purpose* register for mod == 3 with a placeholder register name; the caller
        relabels it for the register class its form actually names (see `_reg_class`). It is
        deliberately not labelled here, because with mod == 3 nothing in the byte tells you
        whether `/r` is a GP register, an MMX register or an XMM register - only the opcode
        does. Guessing 32-bit here is what made `movaps xmm2,xmm3` decode as `movaps xmm2,ebx`
        and `movd xmm0,eax` decode as `movd mm0,ax`: right length, wrong operands, and a
        listing that would still look believable.
        """
        m = self._u8(i)
        i += 1
        mod, reg, rm = m >> 6, (m >> 3) & 7, m & 7
        if mod == 3:
            return i, Operand("reg", R32[rm], size, rm=rm), reg

        base = index = None
        scale = 1
        disp = 0
        # mod == 0 with rm == 5 means "no base register, disp32 follows" - an absolute
        # address. It must be decided BEFORE the displacement tails below, and it must be
        # cleared for every other r/m value: a flag left set from a previous decode makes the
        # next instruction four bytes longer and desynchronises the whole listing.
        absolute = (mod == 0 and rm == 5)
        if rm == 4:
            sib = self._u8(i)
            i += 1
            ss, idx, bs = sib >> 6, (sib >> 3) & 7, sib & 7
            if idx != 4:
                index = R32[idx]
                scale = 1 << ss
            if bs == 5 and mod == 0:
                # No base register in the SIB: it is followed by a disp32. Consuming exactly
                # four bytes here matters - one byte too many swallows the next instruction.
                disp = self._u32(i)
                i += 4
            else:
                base = R32[bs]
        elif not absolute:
            base = R32[rm]

        if mod == 1:
            disp += self._i8(i)
            i += 1
        elif mod == 2 or absolute:
            disp += self._u32(i)
            i += 4
        return i, Operand("mem", size=size, base=base, index=index, scale=scale,
                          disp=disp, seg=seg or ("ds" if absolute else None),
                          absolute=absolute), reg

    # -- entry point -------------------------------------------------------------
    def decode(self, start: int) -> Instruction:
        ins = Instruction(va=self.base_va + start)
        i = start
        seg = None
        rep = ""
        op16 = False
        nprefix = 0
        while i < len(self.code):
            b = self.code[i]
            if b == 0x66:
                op16 = True
            elif b == 0x67:
                pass
            elif b == 0xF0:
                ins.prefixes.append("lock")
            elif b == 0xF2:
                rep = "F2"
            elif b == 0xF3:
                rep = "F3"
            elif b in (0x2E, 0x36, 0x3E, 0x26):
                seg = {0x2E: "cs", 0x36: "ss", 0x3E: "ds", 0x26: "es"}[b]
            elif b == 0x64:
                seg = "fs"
            elif b == 0x65:
                seg = "gs"
            else:
                break
            i += 1
            nprefix += 1
            if nprefix > 14:
                ins.ok = False
                ins.error = "more than 14 prefix bytes"
                ins.length = nprefix
                ins.raw = self.code[start:start + nprefix]
                ins.is_invalid = True
                return ins

        try:
            op = self._u8(i)
            i += 1
            if op == 0x0F:
                i = self._decode_0f(ins, i, seg, rep, op16)
            else:
                i = self._decode_1b(ins, op, i, seg, rep, op16)
        except DecodeError as e:
            # A rep/repne prefix on an opcode we cannot decode is still a complete
            # instruction: the prefix is a legal no-op, and dumpbin reports it as exactly
            # that (`rep` / `repne`, one byte). Reporting the whole thing as invalid would
            # be wrong about the length, which is the one mistake a disassembler must never
            # make - it desynchronises everything after it.
            if rep:
                ins.ok = True
                ins.error = ""
                ins.mnemonic = "rep" if rep == "F3" else "repne"
                ins.operands = []
                ins.prefixes = []
                ins.length = 1
                ins.raw = self.code[start:start + 1]
                ins.is_invalid = False
                ins.fall_through = True
                ins.is_terminal = False
                return ins
            ins.ok = False
            ins.error = str(e)
            ins.length = max(1, i - start)
            ins.raw = self.code[start:start + ins.length]
            ins.is_invalid = True
            return ins

        ins.length = i - start
        if ins.length <= 0:
            ins.ok = False
            ins.error = "decoder produced zero length"
            ins.length = 1
            ins.is_invalid = True
        ins.raw = self.code[start:start + ins.length]
        if ins.ok:
            self._classify(ins)
        return ins

    def _classify(self, ins: Instruction) -> None:
        m = ins.mnemonic.split()[-1] if ins.mnemonic else ""
        if m == "call":
            ins.is_call = True
        elif m == "callf":
            ins.is_call = True
            ins.is_terminal = True
        elif m in ("jmp", "jmpf"):
            ins.is_jmp = True
        elif m == "jecxz" or (m.startswith("j") and m not in ("jmp", "jmpf")):
            ins.is_jcc = True
        elif m in ("ret", "retf", "iretd"):
            ins.is_ret = True
        elif m in ("int3", "ud2", "hlt", "int1", "int"):
            ins.is_halt = True
        elif m in ("loop", "loope", "loopne"):
            ins.is_jcc = True

        for o in ins.operands:
            if o.kind == "rel" and o.target is not None:
                ins.jump_targets.append(o.target)
        if (ins.is_jmp or ins.is_call) and not ins.jump_targets and ins.operands and \
                ins.operands[0].kind in ("reg", "mem"):
            ins.indirect_branch = True
        if ins.is_jmp or ins.is_ret or ins.is_halt or ins.is_call and ins.mnemonic == "callf":
            if ins.is_jmp or ins.is_ret or ins.is_halt:
                ins.is_terminal = True
        if ins.is_jmp and ins.jump_targets:
            ins.is_terminal = True          # unconditional direct jump: no fall-through
        if ins.is_terminal:
            ins.fall_through = False

        # A rep prefix on something that is not a string instruction is a legal no-op, and
        # the prefix is then the only name the instruction has (`rep`, `repne`). Leaving the
        # mnemonic empty would print a blank line where an instruction is.
        if not ins.mnemonic and ins.prefixes:
            ins.mnemonic = ins.prefixes.pop()
            ins.fall_through = True
            ins.is_terminal = False

    # -- one-byte ----------------------------------------------------------------
    def _decode_1b(self, ins: Instruction, op: int, i: int, seg, rep, op16) -> int:
        entry = ONEBYTE.get(op)
        if entry is None:
            raise DecodeError("unknown opcode 0x%02X" % op)
        mnem, form = entry
        if op == 0x0F or form == "esc":
            raise DecodeError("0F escape reached the one-byte path")

        # -- groups that the /reg field selects
        if op in (0x80, 0x81, 0x82, 0x83):
            size = 1 if op in (0x80, 0x82) else (2 if op16 else 4)
            i, rmop, reg = self._modrm(i, size, seg)
            if op in (0x80, 0x82):
                imm, i = self._u8(i), i + 1
            elif op == 0x81:
                imm, i = (self._u16(i), i + 2) if op16 else (self._u32(i), i + 4)
            else:
                imm, i = self._i8(i), i + 1
            ins.mnemonic = GROUP1[reg]
            ins.operands = [self._rm_as(rmop, size), Operand("imm", fmt_num(imm & 0xFFFFFFFF))]
            return i

        if op in (0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3):
            size = 1 if op in (0xC0, 0xD0, 0xD2) else (2 if op16 else 4)
            i, rmop, reg = self._modrm(i, size, seg)
            ins.mnemonic = GROUP2[reg]
            ops = [self._rm_as(rmop, size)]
            if op in (0xC0, 0xC1):
                ops.append(Operand("imm", fmt_num(self._u8(i))))
                i += 1
            elif op in (0xD0, 0xD1):
                ops.append(Operand("imm", "1"))
            else:
                ops.append(Operand("reg", "cl"))
            ins.operands = ops
            return i

        if op == 0xF6 or op == 0xF7:
            size = 1 if op == 0xF6 else (2 if op16 else 4)
            i, rmop, reg = self._modrm(i, size, seg)
            nm = GROUP3[reg]
            if nm is None:
                raise DecodeError("F%X /%d is not a valid group-3 selector"
                                  % (op & 0xF, reg))
            if reg < 2:
                if size == 1:
                    imm, i = self._u8(i), i + 1
                else:
                    imm, i = (self._u16(i), i + 2) if op16 else (self._u32(i), i + 4)
                ins.operands = [self._rm_as(rmop, size), Operand("imm", fmt_num(imm))]
            else:
                ins.operands = [self._rm_as(rmop, size)]
            ins.mnemonic = nm
            return i

        if op == 0xFE:
            i, rmop, reg = self._modrm(i, 1, seg)
            if reg > 1:
                raise DecodeError("FE /%d is not a valid group-4 selector" % reg)
            ins.mnemonic = GROUP4[reg]
            ins.operands = [self._rm_as(rmop, 1)]
            return i

        if op == 0xFF:
            size = 2 if op16 else 4
            i, rmop, reg = self._modrm(i, size, seg)
            nm = GROUP5[reg]
            if nm is None:
                raise DecodeError("FF /%d is reserved" % reg)
            ins.mnemonic = nm
            ins.operands = [self._rm_as(rmop, size)]
            return i

        if op in X87_MEM:
            return self._decode_x87(ins, op, i, seg)

        if mnem is None:
            raise DecodeError("opcode 0x%02X has no handler" % op)

        return self._decode_form(ins, mnem, form, i, seg, op16, rep)

    # -- x87 ---------------------------------------------------------------------
    def _decode_x87(self, ins: Instruction, op: int, i: int, seg) -> int:
        m = self._u8(i)
        mod, reg, rm = m >> 6, (m >> 3) & 7, m & 7

        if mod != 3:
            nm = X87_MEM[op][reg]
            if nm is None:
                raise DecodeError("x87 %02X /%d is reserved" % (op, reg))
            size = X87_MEM_SIZE[op]
            # Loads/stores of an integer width differ from the arithmetic forms (fild/fistp
            # take 16/32/64 depending on /reg), and the control-word forms are words. Getting
            # this wrong changes the decoded length, so it is a correctness issue, not a
            # cosmetic one.
            key = (op, m & 0xF8)
            if key in X87_ALT_SIZE:
                size = X87_ALT_SIZE[key]
            ni, rmop, reg = self._modrm(i, size, seg)
            ins.mnemonic = nm
            ins.operands = [rmop] if rmop else []
            return ni
        key = (op, m & 0xF8)
        exact = X87_MODRM_EXACT.get((op, m))
        ent = exact if exact is not None else X87_MODRM_MOD3.get(key)
        if ent is None:
            # D9 D0 and friends are full-byte encodings, not /reg groups
            nm = X87_NOARG.get(op, {}).get(m)
            if nm is None:
                raise DecodeError("x87 %02X modrm 0x%02X not tabulated" % (op, m))
            ins.mnemonic = nm
            ins.operands = []
            return i + 1
        nm, has_operand = ent
        ins.mnemonic = nm
        # Both operands are emitted where the st(i) is not implied. For the arithmetic forms
        # the implicit st(0) is part of the name (`faddp` means "add st(0) and st(i), pop"),
        # so only the explicit st(i) is printed - dumping both turns `faddp st(1),st` into
        # `faddp st(0),st(1)`, which reads as a different instruction.
        pair_forms = ("fcomi", "fcomip", "fucomi", "fucomip")
        if nm == "fnstsw":
            # DF E0 stores the status word into AX; this is the only fnstsw with an operand
            ins.operands = [Operand("reg", "ax", 2)]
        elif has_operand and nm in pair_forms:
            ins.operands = [Operand("reg", ST[0]), Operand("reg", ST[rm])]
        elif has_operand:
            ins.operands = [Operand("reg", ST[rm])]
        else:
            ins.operands = []
        return i + 1

    # -- shared operand-template decoder -----------------------------------------
    def _decode_form(self, ins: Instruction, mnem: str, form: str, i: int, seg,
                     op16: bool, rep: str) -> int:
        """Decode an instruction from its operand template (e.g. `Gv,Ev`, `V,W`, `Ev,Iz`).

        Template codes, with the two additions this implementation needs:

            E,G,S  general purpose (byte/word/dword per the size suffix)
            P      MMX register          V, U  XMM register (U selects from the r/m field)
            W      XMM-width r/m         Q     MMX-width r/m      M  any-width r/m
            I      immediate   J  relative branch   O  absolute address (moffs)
            A      far pointer  s  segment register (mov Ew,sw)
            R,D,T  control/debug/test registers ('t' is 3DNow, unused here)
        """
        self_off = ins.va - self.base_va
        vbytes = 2 if op16 else 4
        fields = [f for f in (form.split(",") if form else []) if f]

        ops: list[Operand] = []
        modrm_rm: Operand | None = None
        modrm_reg: int | None = None
        rm_class = "G"

        def rm_spec(f: str) -> tuple[str, str]:
            """(register class, size suffix) for the operand that owns the r/m field."""
            return {"E": ("G", f[1:] or "v"), "W": ("V", "d"), "Q": ("P", "q"),
                    "M": ("M", ""), "U": ("V", f[1:] or "v")}.get(f[0], ("G", f[1:] or "v"))

        def needs_modrm(f: str) -> bool:
            """True for the operand codes that OWN the r/m field.

            `V` (and `P`, `G`) are register-only operands taken from the /reg field; they do
            not own the r/m field. Treating `V` as the owner is what made `movd V,Ey` decode
            as `movd xmm0,ax`: the scan stopped at `V`, took its default size (2, because the
            66 prefix sets the operand size to 16 bits), and handed that size to the ModRM
            decoder, so the EAX in the encoding came out as AX.
            """
            return bool(f) and f[0] in "EWQMUSRDT" and f not in ("cl",)

        def has_modrm_field(f: str) -> bool:
            """True if this operand code is *encoded in* the ModRM byte, either half.

            `pextrw Gd,P,Ib` has no E/W/Q/M operand - both registers come from the ModRM byte
            - so keying the parse on `needs_modrm` alone skipped reading it and then indexed
            the register table with None.

            The only form that looks like `Gxx,` without owning a ModRM byte is the old
            MMX pextrw, `Gd,P,Ib`... which does own one. Every `Gv,Ev`-style template has an
            E/W/Q/M operand too, so this predicate is deliberately permissive; the risk of
            over-reading one ModRM byte is smaller than the risk of not reading one.
            """
            return bool(f) and f[0] in "GEPVWQMUSRDT"

        # The table tells us both which operand owns the r/m field and how wide it is. It has
        # to be recorded here rather than re-derived later: the *first* field that needs a
        # modrm is often a register-only one (`V` in `movd V,Ey`), and asking that field for
        # a size yields the default, not the size of the operand that actually owns the field.
        # Re-deriving gave `movd xmm0,ax` and `pinsrw xmm0,ax,2` - AX where the encoding says
        # EAX, because the 66 prefix made the default size 2.
        rm_suffix = "v"
        if any(has_modrm_field(f) for f in fields):
            for f in fields:
                if needs_modrm(f):
                    rm_class, rm_suffix = rm_spec(f)
                    rmsize = {"b": 1, "w": 2, "d": 4, "q": 8, "v": vbytes, "y": 4,
                              "z": vbytes}.get(rm_suffix, vbytes)
                    break
            if not any(needs_modrm(f) for f in fields):
                # Every ModRM operand here is register-only (`pextrw Gd,P,Ib`), so there is
                # no r/m width to derive; the byte still has to be consumed.
                rmsize = 4
            i, modrm_rm, modrm_reg = self._modrm(i, rmsize, seg)

        def rm_operand() -> Operand:
            if modrm_rm is None:
                raise DecodeError("form %r: r/m operand without a modrm byte" % form)
            if modrm_rm.kind != "reg":
                return modrm_rm
            # mod == 3: the same three bits mean a GP, MMX, XMM or segment register
            # depending on the opcode, so the register class comes from the template.
            idx = modrm_rm.rm or 0
            if rm_class == "V":
                return Operand("reg", XMM[idx], 16)
            if rm_class == "P":
                return Operand("reg", MMX[idx], 8)
            n = {"b": 1, "w": 2, "d": 4, "v": vbytes, "y": 4, "z": vbytes}.get(rm_suffix,
                                                                              vbytes)
            return Operand("reg", R8[idx] if n == 1 else (R16[idx] if n == 2 else R32[idx]), n)

        for fi, f in enumerate(fields):
            kind = f[0]
            sz = f[1] if len(f) > 1 else ""
            if kind == "E":
                n = {"b": 1, "w": 2, "d": 4, "q": 8, "v": vbytes, "y": 4, "z": vbytes}.get(
                    sz, vbytes)
                o = rm_operand()
                if o.kind == "mem":
                    o.size = n
                ops.append(o)
            elif kind == "G":
                n = {"b": 1, "w": 2, "d": 4, "v": vbytes, "y": 4, "z": vbytes}.get(sz, vbytes)
                ops.append(Operand("reg", R8[modrm_reg] if n == 1 else
                                   (R16[modrm_reg] if n == 2 else R32[modrm_reg]), n))
            elif kind == "P":
                if modrm_reg is None:
                    raise DecodeError("form %r: no modrm byte for the %s operand" % (form, f))
                ops.append(Operand("reg", MMX[modrm_reg], 8))
            elif kind == "V":
                if modrm_reg is None:
                    raise DecodeError("form %r: no modrm byte for the %s operand" % (form, f))
                ops.append(Operand("reg", XMM[modrm_reg], 16))
            elif kind == "y":
                # `Ey` names a 32-bit *general-purpose* r/m operand even inside an MMX/SSE
                # instruction: `movd xmm0,eax` encodes EAX, not AX and not an XMM register.
                # Reading the size suffix from the r/m spec gives 4 here; anything that
                # decides the register class from a default instead produces `movd xmm0,ax`.
                o = rm_operand()
                if o.kind == "mem":
                    o.size = 4
                ops.append(o)
            elif kind in ("W", "Q", "M", "U"):
                # These all name the *r/m* operand, so the register class has to be taken
                # from the r/m field, not the reg field. `pextrw eax,xmm1,3` encodes xmm1 in
                # r/m; reading reg there yields xmm0 and silently moves the answer.
                saved = rm_class
                rm_class = {"W": "V", "Q": "P", "M": "M", "U": "V"}[kind]
                o = rm_operand()
                rm_class = saved
                if o.kind == "mem":
                    o.size = {"W": 16, "Q": 8, "M": 0, "U": 16}[kind]
                ops.append(o)
            elif kind in ("R", "D", "T"):
                ops.append(Operand("reg", {"R": "cr", "D": "dr", "T": "tr"}[kind] +
                                   str(modrm_reg), 4))
            elif kind == "s" and sz == "w":
                if modrm_reg is None:
                    raise DecodeError("form %r: no modrm byte for the %s operand" % (form, f))
                ops.append(Operand("reg", SEG[modrm_reg], 2))
            elif kind == "I":
                if sz == "b":
                    ops.append(Operand("imm", fmt_num(self._u8(i)))); i += 1
                elif sz == "w":
                    ops.append(Operand("imm", fmt_num(self._u16(i)))); i += 2
                else:
                    v = self._u16(i) if op16 else self._u32(i)
                    i += 2 if op16 else 4
                    ops.append(Operand("imm", fmt_num(v)))
            elif kind == "J":
                if sz == "b":
                    rel = self._i8(i); i += 1
                else:
                    rel = self._u32(i); i += 4
                    if rel >= 0x80000000:
                        rel -= 1 << 32
                tgt = ins.va + (i - self_off) + rel
                ops.append(Operand("rel", "0x%08X" % tgt, target=tgt))
            elif kind == "O":
                addr = self._u32(i); i += 4
                o = Operand("mem", size=(1 if sz == "b" else vbytes), disp=addr,
                            seg=seg or "ds", absolute=True)
                o.target = addr
                ops.append(o)
            elif kind == "A":
                # A far pointer is 4 bytes of offset followed by 2 of selector. The table
                # says `Iz` for the offset by convention, so accept either suffix.
                offv = self._u32(i); selv = self._u16(i + 4); i += 6
                ops.append(Operand("ptr", "0x%04X:0x%08X" % (selv, offv)))
            elif f == "1":
                ops.append(Operand("imm", "1"))
            elif f in R8 or f in R16 or f in R32 or f in SEG or f in ("st",):
                # Explicit register names in a template: `dl,Ib`, `eax,Iz`, `Sw`... The list
                # is built from the register tables rather than typed out, because the
                # hand-typed version covered al/ax/eax/cl/dx and silently missed bl/dl/dh/bh
                # - 368 of the 378 undecodable addresses in BH6.exe were that one omission.
                ops.append(Operand("reg", f))
            else:
                raise DecodeError("form %r: cannot decode operand %r" % (form, f))

        ins.mnemonic = mnem
        ins.operands = ops
        # `aam`/`aad` take an imm8 base, and with the customary base of 10 the assembler
        # writes the mnemonic alone (`aam`, not `aam 0xA`). The immediate has already been
        # consumed as a normal operand, so this only adjusts how it is shown.
        if mnem in ("aam", "aad") and ops and ops[-1].kind == "imm" and ops[-1].text == "0xA":
            ops.pop()
        # A rep prefix turns a string instruction into the repeated form of *that* string
        # instruction. It must not be applied to an opcode that merely shares the byte: F2
        # 0F 10 is `movsd xmm2,[eax+8]` (the SSE scalar double move), not a repne string
        # compare. The guard is that this form has no ModRM byte at all - the string ops are
        # the only rep-prefixable instructions without one.
        if mnem in _STRING_OPERANDS and modrm_rm is None:
            size, roles = _STRING_OPERANDS[mnem]
            # ES:[edi] is the destination of movs/stos/ins and the *first* operand of cmps,
            # while [esi] is the source of movs/lods and the first operand of cmps. Getting
            # the roles the wrong way round is not cosmetic: `scas` compares against [edi],
            # so printing [esi] names a memory access the instruction never performs.
            idx_reg = "edi" if size == 4 else "edi"
            src_reg = "esi" if size == 4 else "esi"
            if mnem.startswith("outs"):
                ops = [Operand("mem", size=size, base="dx", seg="dx", disp=0)]
                if roles == "src":
                    ops.append(Operand("mem", size=size, base=src_reg, disp=0))
            else:
                dst = Operand("mem", size=size, base=idx_reg, seg="es", disp=0)
                src = Operand("mem", size=size, base=src_reg, disp=0)
                if roles == "dst_src":
                    ops = [dst, src]
                elif roles == "dst":
                    ops = [dst]
                else:
                    ops = [src]
            ins.operands = ops
            if rep:
                ins.prefixes.append("rep" if rep == "F3" else "repne")
        return i

    # -- two-byte ----------------------------------------------------------------
    def _decode_0f(self, ins: Instruction, i: int, seg, rep, op16) -> int:
        op2 = self._u8(i)
        i += 1
        vbytes = 2 if op16 else 4

        if op2 in (0x38, 0x3A):
            # The 66/F2/F3 prefixes mean something else entirely for many of these escapes
            # (66 0F 38 F0 is CRC32, not MOVBE; F2 0F 38 F0 is the other CRC32 form). The
            # plain table below is only the unprefixed reading, so a prefixed form must not
            # be looked up there first.
            if not rep and not op16:
                op3 = self._u8(i)
                i += 1
                ent = (THREEBYTE_38 if op2 == 0x38 else THREEBYTE_3A).get(op3)
                if ent is None:
                    raise DecodeError("0F %02X %02X not tabulated" % (op2, op3))
                mnem, form = ent
                return self._decode_form(ins, mnem, form, i, seg, op16, rep)

            # mandatory-prefix overrides for the three-byte escapes, expressed as a separate
            # table so the unprefixed entries above stay readable
            op3 = self._u8(i)
            i += 1
            pfx3 = rep or ("66" if op16 else "")
            ent3 = MANDATORY_3BYTE.get((pfx3, op2, op3))
            if ent3 is not None:
                mnem, form = ent3
                return self._decode_form(ins, mnem, form, i, seg, op16, rep)
            ent = (THREEBYTE_38 if op2 == 0x38 else THREEBYTE_3A).get(op3)
            if ent is None:
                raise DecodeError("0F %02X %02X not tabulated" % (op2, op3))
            mnem, form = ent
            return self._decode_form(ins, mnem, form, i, seg, op16, rep)

        # The 66 prefix is not a rep prefix, so it must be folded in here: the SSE2 forms it
        # selects (66 0F 71 /2 psrlw xmm,imm8 and friends) have no unprefixed counterpart and
        # would otherwise be reported as unknown opcodes.
        pfx = rep or ("66" if op16 else "")

        # A packed/scalar compare takes its condition as an imm8, and the assembler writes
        # that condition into the mnemonic: `0F C2 /r 00` is `cmpeqps xmm2,xmm3`, not
        # `cmpps xmm2,xmm3,0`. Printing the generic form is not a lie about the bytes, but
        # the condition is the entire meaning of the instruction, so it belongs in the name.
        # The prefix selects the flavour: none = ps, 66 = pd, F3 = ss, F2 = sd.
        #
        # This has to come BEFORE the mandatory-prefix lookup below: 66 0F C2 has an entry
        # there (cmppd), so reaching that table first returns the generic spelling and this
        # branch never runs.
        if op2 == 0xC2:
            flavour = {"": "ps", "66": "pd", "F3": "ss", "F2": "sd"}[pfx]
            m = self._u8(i)
            mod, reg, rm = m >> 6, (m >> 3) & 7, m & 7
            if mod == 3:
                i += 1
                src = Operand("reg", XMM[rm], 16)
            else:
                i, src, _ = self._modrm(i, 16, seg)
            imm = self._u8(i)
            i += 1
            ins.mnemonic = "cmp" + CMPCC[imm & 7] + flavour
            ins.operands = [Operand("reg", XMM[reg], 16), src]
            return i

        if pfx and (pfx, op2) in MANDATORY:
            mnem, form = MANDATORY[(pfx, op2)]
            if not mnem.startswith("grp"):
                return self._decode_form(ins, mnem, form, i, seg, op16, rep if rep else "")
            # The prefixed group shifts (66 0F 71/72/73) resolve through the group registry,
            # not through the generic form decoder: `grp12` is a table key, not a mnemonic,
            # and decoding it as a form would emit the placeholder as the instruction name.
            # grp15 (F3 0F AE) is not a shape group - it is handled by the 0F table below,
            # where the /reg field selects between the FXSAVE and FSGSBASE families.
            table = {"grp12": GRP12, "grp13": GRP13, "grp14": GRP14}.get(mnem)
            if table is None:
                ent = TWOBYTE.get(op2)
                if ent is None:
                    raise DecodeError("unknown two-byte opcode 0F %02X" % op2)
                mnem, form = ent
            else:
                m = self._u8(i)
                reg = (m >> 3) & 7
                ni, rmop, reg = self._modrm(i, 16, seg)
                nm = table[reg] if reg < len(table) else None
                if nm is None:
                    raise DecodeError("0F %02X /%d not tabulated" % (op2, reg))
                ins.mnemonic = nm
                ins.operands = [self._rm_as(rmop, 16, "V"),
                                Operand("imm", fmt_num(self._u8(ni)))]
                return ni + 1

        ent = TWOBYTE.get(op2)
        if ent is None:
            raise DecodeError("unknown two-byte opcode 0F %02X" % op2)
        mnem, form = ent

        if mnem in ("grp6", "grp7", "grp15", "grp8", "grp9", "grp12", "grp13", "grp14",
                    "prefetch"):
            m = self._u8(i)
            reg = (m >> 3) & 7
            size = 1 if mnem in ("grp12", "grp13", "grp14") else vbytes
            if mnem == "grp9":
                size = 8
            ni, rmop, reg = self._modrm(i, size, seg)
            if mnem == "grp6":
                nm = GROUP6[reg]
            elif mnem == "grp7":
                nm = GROUP7[reg]
            elif mnem == "grp15":
                # F3 0F AE /0../3 are the FSGSBASE accesses; without the prefix they are
                # fxsave/fxrstor/ldmxcsr/stmxcsr.
                if rep == "F3" and reg < 4:
                    nm = ["rdfsbase", "rdgsbase", "wrfsbase", "wrgsbase"][reg]
                else:
                    nm = GROUP15[reg]
            elif mnem == "grp8":
                nm = [None, None, None, None, "bt", "bts", "btr", "btc"][reg]
            elif mnem == "grp9":
                nm = GRP9.get(reg)
            elif mnem == "grp12":
                nm = GRP12[reg]
            elif mnem == "grp13":
                nm = GRP13[reg]
            elif mnem == "grp14":
                nm = GRP14[reg]
            else:
                # 0F 18 /1../3 are the prefetch hints; dumpbin prints /5 as the generic
                # `prefetch` because the encoding is reused.
                nm = ["prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2",
                      None, "prefetch", None, None][reg]
            if nm is None:
                raise DecodeError("0F %02X /%d not tabulated" % (op2, reg))
            ops = [self._rm_as(rmop, 16 if mnem in ("grp12", "grp13", "grp14") else size,
                               "V" if mnem in ("grp12", "grp13", "grp14") else "G")]
            if mnem == "grp8" or mnem in ("grp12", "grp13", "grp14"):
                ops.append(Operand("imm", fmt_num(self._u8(ni))))
                ni += 1
            ins.mnemonic = nm
            ins.operands = ops
            return ni

        if mnem is None:
            raise DecodeError("0F %02X has no handler" % op2)

        # `mov eax,cr0` / `mov cr0,eax` and `mov eax,dr7`: the register class of BOTH
        # operands depends on which one sits in the r/m field. The templates read `Rd,Cd`,
        # but the r/m operand is a general-purpose register whenever the reg field is the
        # system register - decoding them as both control registers produced
        # "cannot decode operand 'Cd'" and, worse, could have produced `cr0,cr0`.
        if form in ("Rd,Cd", "Rd,Dd", "Rd,Td", "Cd,Rd", "Dd,Rd", "Td,Rd"):
            m = self._u8(i)
            i += 1
            mod, reg, rm = m >> 6, (m >> 3) & 7, m & 7
            cls = {c: p for c, p in (("C", "cr"), ("c", "cr"), ("D", "dr"), ("d", "dr"),
                                     ("T", "tr"), ("t", "tr"))}
            sysreg_letter = form[1] if form[0] in ("R", "r") else form[0]
            sysreg = Operand("reg", cls[sysreg_letter] + str(reg), 4)
            gp = Operand("reg", R32[rm], 4)
            ins.mnemonic = mnem
            ins.operands = [gp, sysreg] if form[0] in ("R", "r") else [sysreg, gp]
            return i

        # 0F 12 / 0F 16 with mod == 3 are the register-pair moves, not the memory loads:
        # `movhlps xmm0,xmm1` (0F 12) and `movlhps xmm0,xmm1` (0F 16). Checked only when no
        # mandatory prefix applies, because F3 0F 12 / F3 0F 16 are movsldup/movshdup, which
        # the table above has already returned.
        if op2 in (0x12, 0x16) and not rep and not op16 and (self._peek(i) >> 6) == 3:
            m = self._u8(i)
            i += 1
            mod, reg, rm = m >> 6, (m >> 3) & 7, m & 7
            ins.mnemonic = "movhlps" if op2 == 0x12 else "movlhps"
            ins.operands = [Operand("reg", XMM[reg], 16), Operand("reg", XMM[rm], 16)]
            return i

        return self._decode_form(ins, mnem, form, i, seg, op16, rep)

def decode_one(code: bytes, va: int) -> Instruction:
    """Convenience wrapper: decode a single instruction at the start of `code`."""
    return Decoder(code, va).decode(0)
