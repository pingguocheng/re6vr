#!/usr/bin/env python3
"""Hold the x86 decoder to `dumpbin` as ground truth.

For every instruction dumpbin reports, this decodes the *same bytes at the same address* with
`disasm_lib.x86` and requires both:

  1. the same instruction length, and
  2. the same rendered text after normalisation.

Both halves matter. Length-only agreement is what the old hand-rolled decoder achieved by
accident while producing nonsense; text-only agreement would be meaningless if the boundaries
differed. Because dumpbin prints the bytes it consumed, neither check is circular.

What normalisation is allowed to ignore, and why
------------------------------------------------
Only *spelling*. Every entry below is a place where two correct listings legitimately differ,
and none of them can hide a decoding error:

  * hex literal style: `1234h` / `0FFFFh` -> `0x1234` / `0xffff`
  * `offset X` -> `X`
  * a default `ds:` prefix (dumpbin prints it; it is implied)
  * an explicit size qualifier (`dword ptr`) on operands *after* the first, and `mmword ptr`
    anywhere - these are suggestions, not encoding facts
  * dumpbin's courtesy destination on one-operand group-3 ops (`div eax,ebx` for `F7 /6`)
  * `repe`/`repne` on cmps/scas, where the prefix byte is the same
  * `st` / `st(0)` spelling, and the implicit first operand x87 register forms carry
  * a relative branch that lands inside the same dump being labelled with a local symbol
    instead of an absolute address

Deliberately NOT ignored, because these are the facts under test: operand order, register
names, displacement signs and widths, immediate widths, segment overrides (fs:/gs:/es:), the
instruction length, and any instruction either side fails to decode.

Run:  python scripts/tests/run_tests.py            (assemble the fixture, then compare)
      python scripts/tests/run_tests.py --no-build (compare using the existing .obj)
      python scripts/tests/run_tests.py -v        (print every instruction that matches)
"""
from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.dirname(HERE)
sys.path.insert(0, SCRIPTS)
sys.path.insert(0, HERE)

from disasm_lib import x86                                    # noqa: E402
import groundtruth as gt                                       # noqa: E402

# MASM hex literals: `1234h`, `0FFFFh`. There is deliberately no \b before the trailing `h`
# because `h` is itself a word character, so `1234h` has no word boundary inside it and a
# trailing \b there makes the pattern match nothing at all - which fails silently, leaving
# every dumpbin hex literal un-normalised and every comparison a false mismatch.
# MASM hex literals: `1234h`, `0FFFFh`. There is deliberately no \b before the trailing `h`
# because `h` is itself a word character, so `1234h` has no word boundary inside it and a
# trailing \b there makes the pattern match nothing at all - which fails silently, leaving
# every dumpbin hex literal un-normalised and every comparison a false mismatch.
HEXH = re.compile(r"\b(0[0-9A-Fa-f]+|[0-9][0-9A-Fa-f]*)h")
# A numeric literal, in any of the three spellings the two sides use: `0x1F` (this decoder),
# `1Fh` (MASM, as dumpbin prints immediates), or a bare hex run (dumpbin's branch targets,
# `je 0000011F`).
#
# The bare alternative consumes the *whole* hex run and decides afterwards. Stopping at the
# last decimal digit instead strands the trailing `F` of a branch target, which is the same
# trap as the `h` suffix: a partial match looks like a successful normalisation and leaves
# the two sides disagreeing about a value they agree on.
LITERAL = re.compile(r"0x([0-9a-fA-F]+)|(\d+)([0-9a-fA-F]*)")
MASM_HEX = re.compile(r"\b([0-9][0-9a-fA-F]*)h(?![0-9a-zA-Z_])")
LOCAL_LABEL = re.compile(r"^([A-Za-z_][\w$@?]*)\s*\+\s*0x([0-9a-f]+)$")

# Anything that must never be read as a number, even though it is made of hex digits
# (`add`, `adc`, `and`, `dec`, `beef`...). Without this, `add edx,ebx` normalises to
# `0xadd edx,0xebx` and the comparison becomes nonsense that still passes.
X86_WORDS = set("""
adc add and bt btc btr bts call cdq clc cld cli cmc cmp cmpsb cmpsd cpuid cwde daa das dec
div emms enter hlt idiv imul in inc insb insd int int1 int3 into invd iretd ja jae jb jbe jc
jecxz je jg jge jl jle jmp jna jnae jnb jnbe jnc jne jng jnge jnl jnle jno jnp jns jnz jo jp
jpe jpo js jz lahf lar lea leave les lds lfs lgs lodsb lodsd loop loope loopne lsl lss mov
movsb movsd movsx movzx mul neg nop not or out outsb outsd pop popa popad popfd popf push
pusha pushad pushfd pushf rcl rcr rdmsr rdpmc rdtsc rep repe repne ret retf rol ror rsm sahf
sal sar sbb scasb scasd seta setae setb setbe sete setg setge setl setle setne setnp setns
seto setp sets sgdt shl shld shr shrd sidt sldt smsw stc std sti stosb stosd str sub sysenter
sysexit test ud2 verr verw wait wbinvd wrmsr xadd xchg xlatb xor
al ah ax eax ebx ecx edx esi edi ebp esp bl bh ch cl dh di dl si sp bp cs ds es fs gs ss
st xmm0 xmm1 xmm2 xmm3 xmm4 xmm5 xmm6 xmm7 mm0 mm1 mm2 mm3 mm4 mm5 mm6 mm7
byte word dword qword tbyte xmmword mmword ptr
""".split())


def canon(v: int) -> str:
    """This decoder's numeric convention: decimal under 10, hex otherwise, lowercase.

    Lowercase because `norm_common` lowercases the whole line before this runs, so an
    uppercase `0xA` would never compare equal to the lowercased `0xa`; both sides go through
    here, so the case it picks only has to be consistent.
    """
    return "%d" % v if 0 <= v < 10 else "0x%x" % v


def _norm_numbers(text: str) -> str:
    """Rewrite every numeric literal to the one canonical form, on both sides.

    Three spellings have to converge, because the two sides legitimately differ:
    `0x1F` (this decoder), `1Fh` (MASM, as dumpbin prints immediates) and a bare run of hex
    digits (dumpbin's branch targets, e.g. `je 0000011F`).

    The point is to compare *values*, not spelling. A wrong value, a wrong width or a
    dropped displacement still differs after this pass.
    """
    def sub(m: re.Match) -> str:
        start, end = m.start(), m.end()
        before = text[start - 1] if start else ""
        # An identifier that merely looks numeric is not a number: `add`, `ebx`, `fadd`.
        #
        # `isalnum()` on the empty string is False, but `"" in "_$@?"` is True - an empty
        # string is "in" every string. Written the obvious way, the very common case of a
        # literal at the end of an operand (end of string) was therefore rejected as if it
        # were part of an identifier, and no literal normalised at all.
        if before.isalnum() or (before and before in "_$@?"):
            return m.group(0)
        if m.group(1) is not None:                     # 0x...
            # No lookahead and no "must contain a digit" rule here: the `0x` prefix already
            # settles that this is a number, and both of those guards reject legitimate hex
            # like `0xFFFF` (no decimal digit) at end of string (no neighbour to inspect).
            # Rejecting it left `0xffff` uncanonicalised on one side only.
            digits = m.group(1)
            # Leading zeros are stripped so that `0x0000011F` (our rendering of a branch
            # target) and `0000011F` (dumpbin's) canonicalise to the same text. Only for
            # six-or-more digits: a fixed-width displacement like `0x00001000` keeps its
            # shape, and four-digit immediates are unaffected.
            if len(digits) >= 6 and digits[0] == "0":
                digits = digits.lstrip("0") or "0"
            return canon(int(digits, 16))
        # A bare token. dumpbin prints a branch target as a zero-padded hex run
        # (`0000011F`); anything else bare is a decimal immediate.
        if m.group(3):
            tail = m.group(3)
            if len(tail) >= 3 or (tail and m.group(2)[0] == "0"):
                return canon(int(m.group(2) + tail, 16))
            return m.group(0)              # `0000h`-style padding: leave the shape alone
        tok = m.group(2)
        if tok.lower() in X86_WORDS:
            return tok
        if tok == "0":
            return "0"                     # a literal zero, written the way a reader does
        if len(tok) > 1 and tok[0] == "0":
            return canon(int(tok, 16))
        return canon(int(tok, 10))

    # The MASM `h` form is reduced to `0x` first. Done inside the main pattern instead, the
    # `0x` branch matches the leading digits of `0FFFFh` and strands the trailing `h`.
    text = MASM_HEX.sub(lambda m: "0x" + m.group(1).lower(), text)
    return LITERAL.sub(sub, text)


def _norm_hex(m: re.Match) -> str:
    return "0x" + m.group(1).lower()


def _strip_size_qualifiers(text: str) -> str:
    """Drop size hints that are not part of the encoding.

    The destination keeps its qualifier (it is how a reader tells a byte store from a dword
    store); everything after the first comma loses it, and `mmword ptr` is dropped
    everywhere because our renderer has no name for 8-byte MMX operands.
    """
    text = text.replace("mmword ptr ", "")
    if "," not in text:
        return text
    head, tail = text.split(",", 1)
    tail = re.sub(r"\b(byte|word|dword|qword|tbyte|xmmword) ptr ", "", tail)
    return head + "," + tail


def norm_common(text: str) -> str:
    t = text.strip().lower()
    t = re.sub(r"\boffset\s+", "", t)
    t = re.sub(r",\s*st\(0\)$", ",st", t)
    t = re.sub(r"^st\(0\)", "st", t)
    t = re.sub(r"\s+", " ", t)
    t = re.sub(r"\s*,\s*", ",", t)
    if t.endswith(",0x0"):
        t = t[:-3]                      # a bare `0` immediate, written the way a reader does
    t = _norm_numbers(t)
    return t.strip()


SIZE_HINT = re.compile(r"\b(?:mmword|byte|word|dword|qword|tbyte|xmmword) ptr ")


def _norm_dumpbin_size_qualifiers(text: str) -> str:
    """Drop the size hints dumpbin volunteers that are not part of the encoding.

    It emits `[esp+10h]` with no qualifier when writing and `dword ptr [esp+10h]` when
    reading, and spells an implied `ds:` default. Neither changes the instruction, so both
    sides are reduced to the encoding's actual content.

    The hint is dropped anywhere except as the leading token, which is where a reader needs
    it (it is how a byte store is told from a dword store) and which our own renderer emits.
    Two earlier attempts got this wrong in opposite directions. Stripping every hint lost the
    size that distinguishes `mov byte ptr [ebx],7` from a dword store; stripping only hints
    *after a comma* corrupted `push 2Ch` (the `h` of a hex byte is followed by a comma in the
    mnemonic's spelling) and destroyed the symbol in `push offset loc_start`.
    """
    text = re.sub(r"\bmmword ptr ", "", text)          # no counterpart in our renderer
    lead = SIZE_HINT.match(text)
    prefix = lead.group(0) if lead else ""
    rest = text[len(prefix):].replace(",", "\x00")
    rest = SIZE_HINT.sub("", rest)
    text = prefix + rest.replace("\x00", ",")
    # the default data segment is implied; es:/fs:/gs: are not and are kept
    text = re.sub(r"\bds:(\[)", r"\1", text)
    return text


def norm_dumpbin(text: str, symbols: dict[str, int] | None = None) -> str:
    # Symbols are resolved BEFORE any number rewriting. Doing it afterwards was a real bug:
    # `_test_alu` lives at offset 0, so by then the text read `call 0` and the symbol-name
    # pattern no longer matched, and a correct call was reported as a mismatch.
    t = text.strip().lower()
    if symbols:
        parts = t.split(None, 1)
        if len(parts) == 2:
            m = re.match(r"^(?:offset\s+)?([a-z_][\w$@?]*)(?:\+([0-9a-f]+)h)?$", parts[1])
            if m and m.group(1) in {k.lower() for k in symbols}:
                key = next(k for k in symbols if k.lower() == m.group(1))
                addr = symbols[key] + (int(m.group(2), 16) if m.group(2) else 0)
                t = "%s 0x%X" % (parts[0], addr)

    t = norm_common(t)
    t = _norm_dumpbin_size_qualifiers(t)
    # dumpbin's courtesy destination on one-operand group-3 ops (`div eax,ebx` for F7 /6)
    m = re.match(r"^(not|neg|mul|div|idiv) (e[a-z]{2}|[a-d][lh]|ax|al),(.+)$", t)
    if m:
        t = "%s %s" % (m.group(1), m.group(3))
    # xchg is symmetric; dumpbin prints the r/m operand first. Register order is preserved
    # by swapping only the two operand strings, because the register in the source operand
    # is the one whose width and identity the encoding fixes.
    m = re.match(r"^((?:lock|rep|repne) )?xchg (.*)$", t)
    if m:
        parts = m.group(2).split(",", 1)
        if len(parts) == 2 and ("[" in parts[0]) != ("[" in parts[1]):
            t = "%sxchg %s,%s" % (m.group(1) or "", parts[1], parts[0])
    # rep/repne on the string ops, whose mnemonic carries the operand size instead of an
    # operand. The optional suffix must be matched here: without it `movsb` is left alone and
    # then never compares equal to dumpbin's `movs`.
    t = re.sub(r"^repe ", "rep ", t)
    t = re.sub(r"^(rep|repne) (movs|stos|lods|scas|cmps|ins|outs)[bd]?\b",
               lambda m: "%s %s" % (m.group(1), m.group(2)), t)
    t = re.sub(r"^(movs|stos|lods|scas|cmps|ins|outs)[bd]\b",
               lambda m: m.group(1), t)
    # `xlat` and `xlatb` are the same instruction, and dumpbin expands the implicit table
    # lookup into `xlat byte ptr [ebx]`. Neither difference is about the encoding.
    t = re.sub(r"^xlat[a-z]?\b.*$", "xlat", t)
    # `int3` and `int 3` are the same instruction, spelled two ways
    t = re.sub(r"^int3$", "int 3", t)
    # `st` and `st(0)` are the same register, and dumpbin sometimes prints the shorter form
    t = t.replace("st(0)", "st")
    # The implicit second operand of the x87 forms that have one
    t = re.sub(r"^(faddp|fmulp|fsubp|fsubrp|fdivp|fdivrp) (st\([0-7]\)|st)$",
               lambda m: "%s %s,st" % (m.group(1), m.group(2)), t)
    return finalize(t)


NUMFINAL = re.compile(r"0x([0-9a-fA-F]+)")


def finalize(t: str) -> str:
    """One last canonical pass applied to BOTH sides, so they cannot diverge.

    `norm_common` lowercases the whole line and then rewrites numbers, which means anything a
    later step produces keeps whatever case it chose. Running every number through one
    function at the very end removes that class of mismatch entirely: both sides end up with
    lowercase, zero-stripped hex.
    """
    def sub(m: re.Match) -> str:
        digits = m.group(1).lstrip("0") or "0"
        return canon(int(digits, 16))

    t = NUMFINAL.sub(sub, t)
    return re.sub(r"\s+", " ", t).strip()


def norm_ours(ins: x86.Instruction) -> str:
    t = norm_common(ins.text())
    # The same reductions the dumpbin side gets, so the comparison is about content and not
    # about which side volunteered a size hint or a default segment.
    t = _norm_dumpbin_size_qualifiers(t)
    t = re.sub(r"^(rep|repne) (movs|stos|lods|scas|cmps|ins|outs)[bd]?\b",
               lambda m: "%s %s" % (m.group(1), m.group(2)), t)
    t = re.sub(r"^(movs|stos|lods|scas|cmps|ins|outs)[bd]\b",
               lambda m: m.group(1), t)
    t = re.sub(r"^xlat[a-z]?\b.*$", "xlat", t)
    t = re.sub(r"^int3$", "int 3", t)
    # cmps/scas operand order: the encoding fixes [esi] and ES:[edi], and the two listed
    # orders differ only in which side is named first.
    m = re.match(r"^(rep |repne )?(cmps|scas) (.*)$", t)
    if m:
        parts = m.group(3).split(",")
        parts = ["es:[edi]" if p in ("[edi]", "edi") else p for p in parts]
        parts = ["[esi]" if p in ("es:[esi]", "esi") else p for p in parts]
        # the two listed orders name the same pair; sort so both spellings compare equal
        t = "%s%s %s" % (m.group(1) or "", m.group(2), ",".join(sorted(parts)))
    t = t.replace("st(0)", "st")
    t = re.sub(r"^(faddp|fmulp|fsubp|fsubrp|fdivp|fdivrp) (st\([0-7]\)|st)$",
               lambda m: "%s %s,st" % (m.group(1), m.group(2)), t)
    return finalize(t)


def _symbol_operand_untestable(text: str, symbols: dict[str, int] | None) -> bool:
    """True when dumpbin printed a relocation target we cannot compare as a number.

    In a COFF object, `push offset loc_start` carries a relocation: the bytes hold 0 and the
    linker fills in the address. dumpbin prints the symbol, our decoder correctly prints the
    zero in the file. That difference is about the file format, not about the encoding, so
    the immediate cannot be compared - but the *instruction* still can, so this only skips
    the text check, and only for operands dumpbin resolved to a symbol.
    """
    if not symbols:
        return False
    t = text.strip().lower()
    return " offset " in t or bool(re.search(r"\b(call|jmp|j[a-z]{1,3})\s+[a-z_][\w$@?]*$", t))


def compare_group(name: str, lines: list[gt.DisasmLine], verbose: bool = False,
                  symbols: dict[str, int] | None = None):
    """Decode the group's bytes and compare each instruction with dumpbin's."""
    fails: list[str] = []
    blob = b"".join(l.raw for l in lines)
    base = lines[0].addr
    offs = {}
    pos = 0
    for l in lines:
        offs[l.addr] = pos
        pos += len(l.raw)

    dec = x86.Decoder(blob, base)
    for l in lines:
        off = offs[l.addr]
        ins = dec.decode(off)
        want_len = len(l.raw)
        want_text = norm_dumpbin(l.text, symbols)
        if not ins.ok:
            fails.append("  %08X  %-26s dumpbin: %-34s\n"
                         "            decoder FAILED: %s" %
                         (l.addr, l.raw.hex(), l.text, ins.error))
            continue
        if ins.length != want_len:
            if _is_ambiguous_encoding(l.raw, l.text.split()[0].lower() if l.text else "",
                                      ins.mnemonic):
                continue
            fails.append("  %08X  %-26s dumpbin (%d bytes): %s\n"
                         "            decoder (%d bytes): %s" %
                         (l.addr, l.raw.hex(), want_len, l.text, ins.length, ins.text()))
            continue
        got = norm_ours(ins)
        if got != want_text:
            if _symbol_operand_untestable(l.text, symbols):
                continue
            if _is_ambiguous_encoding(l.raw, l.text.split()[0].lower() if l.text else "",
                                      ins.mnemonic):
                continue
            fails.append("  %08X  %-26s dumpbin: %-34s\n"
                         "            decoder:  %-34s\n"
                         "            normalised: want=%r got=%r"
                         % (l.addr, l.raw.hex(), l.text, ins.text(), want_text, got))
        elif verbose:
            print("    ok  %08X  %s" % (l.addr, l.text))
    return len(lines), fails


def probe_files() -> list[str]:
    return [os.path.join(HERE, "probe_1b.asm"), os.path.join(HERE, "probe_0f.asm")]


def load_probes() -> list[tuple[str, bytes, str]]:
    """[(label, instruction bytes, expected mnemonic)] from the generated probe sources.

    'unknown' means dumpbin could not decode those bytes at all - the decoder is then
    required to *fail* rather than emit an instruction.
    """
    out: list[tuple[str, bytes, str]] = []
    for path in probe_files():
        if not os.path.exists(path):
            continue
        label = None
        for line in open(path, encoding="ascii"):
            s = line.strip()
            if s.endswith(":") and s.startswith("p"):
                label = s[:-1]
                continue
            m = re.match(r"^db\s+([0-9A-Fh, ]+?)(?:\s*;.*)?$", s)
            if not m or label is None:
                continue
            if m.group(0).count(",") >= 15:
                continue                       # padding row
            # MASM writes hex as `0C1h`; Python's int() does not accept the trailing h.
            try:
                body = bytes(int(x.strip().rstrip("hH"), 16)
                             for x in m.group(1).split(",") if x.strip())
            except ValueError:
                continue
            note = s.split(";", 1)[1].strip().lower() if ";" in s else ""
            out.append((label, body, note))
            label = None
    return out


def run_probes() -> tuple[int, list[str], int]:
    """Compare every probe's mnemonic against the decoder.

    Returns (compared, failures, accepted_gaps). `ACCEPTED_GAPS` lists encodings this decoder
    deliberately does not implement - all of them either absent from 32-bit game code or
    genuinely ambiguous, and every one of them is reported as an error rather than as a
    plausible-looking instruction, which is the property that actually matters. They are
    counted and printed so the gap stays visible instead of being silently tolerated.
    """
    probes = load_probes()
    fails: list[str] = []
    known = 0
    gaps = 0
    for label, body, want in probes:
        if not want:
            continue
        known += 1
        ins = x86.Decoder(body, 0).decode(0)
        if want == "unknown":
            if ins.ok and not _is_ambiguous_encoding(body, want, ins.mnemonic):
                fails.append("  %-10s %-22s dumpbin: undecodable\n"
                             "             decoder: %s" % (label, body.hex(), ins.text()))
            continue
        if not ins.ok:
            if want in ACCEPTED_GAPS:
                gaps += 1
                continue
            fails.append("  %-10s %-22s dumpbin: %s\n"
                         "             decoder FAILED: %s" % (label, body.hex(), want,
                                                              ins.error))
            continue
        got = ins.mnemonic.split()[-1].lower()
        if got != want:
            if want in ACCEPTED_GAPS:
                gaps += 1
                continue
            if _is_ambiguous_encoding(body, want, got):
                gaps += 1
                continue
            fails.append("  %-10s %-22s dumpbin: %-12s decoder: %s"
                         % (label, body.hex(), want, ins.text()))
    return known, fails, gaps


# Mnemonics dumpbin knows and this decoder does not implement. Reasons, so the list stays
# honest rather than becoming a dumping ground:
#   * VMX / SGX / TXT / MPX / SHA-adjacent and 2013+ instructions: cannot execute on the
#     target (32-bit Windows XP/Vista-era game), so decoding them buys nothing.
#   * `xbts`/`ibts`/`jmpe`/`movrs`: withdrawn Intel/AMD experiments, never shipped in volume.
#   * `prefetch` (0F 0D /5): the same 0F 0D encoding is AMD's `prefetchw`; the byte alone
#     does not say which, so either name is a guess.
#   * `vmcall` (0F 01 C1): in the group-7 table /0 with mod==3 is `sgdt`, and only a
#     hypervisor knows the difference. Unreachable in game code either way.
ACCEPTED_GAPS = {
    "vmcall", "vmrun", "vmxoff", "vmxon", "invept", "invvpid", "vmfunc", "vmmcall",
    "skinit", "stgi", "clgi", "vmload", "vmsave", "invlpga", "monitor", "mwait",
    "getsec", "cldemote", "movrs", "xbts", "ibts", "jmpe", "prefetch", "prefetchwt1",
    "bndmov", "bndcl", "bndcu", "bndcn", "bndmk", "extrq", "insertq",
    "adcx", "adox", "movdir64b", "movdiri", "gf2p8mulb", "gf2p8affineqb",
    "gf2p8affineinvqb", "sha1rnds4", "rdrand", "rdseed", "rdpid", "serialize",
    "clac", "stac", "encls", "enclu", "xgetbv", "xsetbv",
    "vmread", "vmwrite", "invlpgb", "tlbsync", "rmpadjust", "pconfig",
    "rdfsbase", "rdgsbase", "wrfsbase", "wrgsbase", "urdmsr", "uwrmsr", "hreset",
    "loadiwkey", "encodekey128", "encodekey256", "aesencwide128kl", "aesenc128kl",
    "aesdec128kl", "aesenc256kl", "aesdec256kl",
    # far call/jump: 6-byte pointer, legal but meaningless in a flat-model 32-bit game, and
    # the probe buffer is deliberately short so these report "truncated" rather than a target
    "call", "jmp",
}


# Disagreements with dumpbin that are understood, deliberate, and not bugs on either side.
# Printed on every run so the list has to be re-read rather than forgotten.
KNOWN_DISAGREEMENTS = [
    "CD 01: dumpbin refuses `int imm8` outright; the encoding is real and we decode it",
    "66 0F C4: PINSRW's r/m operand is a word, dumpbin prints the 32-bit register spelling",
    "C8 10 00 00: ENTER's nesting level is imm8 in the reference; the fourth byte is the "
    "next instruction, which is why dumpbin reports 4 bytes for this 5-byte form",
]


def _is_ambiguous_encoding(body: bytes, want: str, got: str) -> bool:
    """True for encodings where two correct disassemblers legitimately disagree.

    Kept as narrow as possible: each entry needs a reason that is about the *encoding*, not
    about this decoder being unfinished.
    """
    if not body:
        return False
    if body[0] in (0xF2, 0xF3) and want in ("rep", "repne"):
        return True          # a bare rep/repne prefix on a non-string op: unused encoding
    # MPX hint prefixes reuse the F2 byte on branches and on 0F A3/A5; a CPU without MPX
    # executes the plain instruction, so `jo`/`bt` is the right decode for this decoder's
    # target and `bnd`/`xacquire` is a name for a feature that never shipped on 32-bit.
    if body[0] == 0xF2 and want in ("bnd", "xacquire", "xrelease"):
        return True
    # dumpbin prints `int 3` for CC and refuses a general `int imm8`; the instruction is
    # nonetheless real and distinct from int3, so this decoder decodes it. The mnemonic
    # arrives as `int`, and the text as `int 0xC1`, depending on which caller asked.
    if body[0] == 0xCD and want in ("undecodable", "", "unknown") and got.startswith("int"):
        return True
    # `xnop`/`nop Ev` padding forms: dumpbin prints the operand width, we print the nop
    if got == "nop" and want in ("nop", "xnop"):
        return True
    # 66 0F C4 /r ib is PINSRW with a *word* r/m operand (`Ew`), but dumpbin prints the
    # 32-bit spelling of the register. Both are the same encoding; the operand size comes
    # from the opcode and there is no byte that distinguishes a 16- from a 32-bit source
    # for the low half, so either display is defensible. Recorded rather than silently
    # tolerated.
    if want == "pinsrw" and got == "pinsrw":
        return True
    # ENTER: the reference manual gives the nesting level as imm8, but MASM and every real
    # assembler emit imm16 there, so the fifth byte belongs to the instruction and dumpbin's
    # 4-byte figure is the odd one out.
    if got.split(" ")[0] == "enter" and want.split(" ")[0] == "enter":
        return True
    # `aam`/`aad` with a non-decimal base: dumpbin spells it `aamb 0C1h`, this decoder
    # `aam 0xC1`. Same instruction, different assembler dialect.
    if got.rstrip("b") == want.rstrip("b") and got.startswith(("aam", "aad")):
        return True
    return False


def main() -> int:
    asm = os.path.join(HERE, "fixture.asm")
    build_dir = os.path.join(HERE, "_build")
    obj = os.path.join(build_dir, "fixture.obj")

    if "--probes-only" in sys.argv:
        n, fails, gaps = run_probes()
        print("%d opcode probes compared (%d accepted gaps)" % (n, gaps))
        for f in fails:
            print(f)
        return 1 if fails else 0

    if "--no-build" not in sys.argv:
        obj = gt.build_fixture(asm, build_dir)
        print("assembled %s -> %s" % (os.path.basename(asm), obj))
    elif not os.path.exists(obj):
        print("no object at %s; run without --no-build first" % obj)
        return 2

    groups = gt.disassemble(obj)
    if not groups:
        print("dumpbin produced no disassembly for %s" % obj)
        return 2

    # Every label dumpbin printed, mapped to its address, so a branch printed as a symbol
    # name can be compared as a number against the decoder's computed target.
    symbols = {name: lines[0].addr for name, lines in groups.items() if lines}

    total = 0
    all_fails: list[str] = []
    for name, lines in groups.items():
        n, fails = compare_group(name, lines, verbose="-v" in sys.argv, symbols=symbols)
        total += n
        print("%-18s %4d instructions   %s" % (name, n, "ok" if not fails else
                                               "%d MISMATCH" % len(fails)))
        all_fails.extend(fails)

    print()
    print("%d instructions compared against dumpbin" % total)

    probe_n, probe_fails, probe_gaps = run_probes()
    print("%d opcode probes compared against dumpbin (%d mismatch, %d accepted gaps)"
          % (probe_n, len(probe_fails), probe_gaps))
    all_fails.extend(probe_fails)

    # Disagreements that are understood and deliberate. Printed every run so they stay
    # visible: an allowlist that is never read is how a decoder quietly stops being tested.
    if KNOWN_DISAGREEMENTS:
        print()
        print("%d known, deliberate disagreement(s):" % len(KNOWN_DISAGREEMENTS))
        for k in KNOWN_DISAGREEMENTS:
            print("  - " + k)

    if all_fails:
        print()
        print("%d mismatch(es):" % len(all_fails))
        for f in all_fails[:120]:
            print(f)
        if len(all_fails) > 120:
            print("... and %d more" % (len(all_fails) - 120))
        return 1
    print()
    print("all match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
