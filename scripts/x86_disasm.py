#!/usr/bin/env python3
"""Disassemble a small region of BH6.exe with a hand-rolled x86 length decoder.

Why not a real disassembler: none is installed on this machine and the analysis only needs
instruction boundaries, a handful of opcode semantics, and call/jump targets. A full decoder
would be a liability; a length-only decoder plus the few forms this code actually uses is
enough to read a function's structure.

Usage: python x86_disasm.py <rva> [<count>]
"""
import struct
import sys

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
IMAGE_BASE = 0x400000

data = open(EXE, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsz = struct.unpack_from("<H", data, pe + 20)[0]
sections = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = data[o:o + 8].rstrip(b"\0").decode(errors="replace")
    vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, o + 8)
    sections.append((name, va, vsize, raw, rawsize))


def rva_to_off(rva):
    for name, va, vsize, raw, rawsize in sections:
        if va <= rva < va + max(vsize, rawsize):
            return raw + (rva - va)
    return None


def read(rva, n):
    off = rva_to_off(rva)
    return data[off:off + n] if off is not None else b""


# --- a length-only x86 decoder -------------------------------------------------------------
MODRM = {
    0x00: ("add", "Eb,Gb"), 0x01: ("add", "Ev,Gv"), 0x02: ("add", "Gb,Eb"), 0x03: ("add", "Gv,Ev"),
    0x08: ("or", "Eb,Gb"), 0x09: ("or", "Ev,Gv"), 0x0B: ("or", "Gv,Ev"),
    0x10: ("adc", "Eb,Gb"), 0x11: ("adc", "Ev,Gv"),
    0x18: ("sbb", "Eb,Gb"), 0x19: ("sbb", "Ev,Gv"),
    0x20: ("and", "Eb,Gb"), 0x21: ("and", "Ev,Gv"), 0x23: ("and", "Gv,Ev"),
    0x28: ("sub", "Eb,Gb"), 0x29: ("sub", "Ev,Gv"), 0x2B: ("sub", "Gv,Ev"),
    0x30: ("xor", "Eb,Gb"), 0x31: ("xor", "Ev,Gv"), 0x33: ("xor", "Gv,Ev"),
    0x38: ("cmp", "Eb,Gb"), 0x39: ("cmp", "Ev,Gv"), 0x3B: ("cmp", "Gv,Ev"),
    0x50: ("push", "Zv"), 0x51: ("push", "Zv"), 0x52: ("push", "Zv"), 0x53: ("push", "Zv"),
    0x68: ("push", "Iz"), 0x6A: ("push", "Ib"),
    0x70: ("jo", "Jb"), 0x71: ("jno", "Jb"), 0x72: ("jb", "Jb"), 0x73: ("jae", "Jb"),
    0x74: ("je", "Jb"), 0x75: ("jne", "Jb"), 0x76: ("jbe", "Jb"), 0x77: ("ja", "Jb"),
    0x78: ("js", "Jb"), 0x79: ("jns", "Jb"), 0x7A: ("jp", "Jb"), 0x7B: ("jnp", "Jb"),
    0x7C: ("jl", "Jb"), 0x7D: ("jge", "Jb"), 0x7E: ("jle", "Jb"), 0x7F: ("jg", "Jb"),
    0x80: ("grp1", "Eb,Ib"), 0x81: ("grp1", "Ev,Iz"), 0x83: ("grp1", "Ev,Ib"),
    0x84: ("test", "Eb,Gb"), 0x85: ("test", "Ev,Gv"),
    0x86: ("xchg", "Eb,Gb"), 0x87: ("xchg", "Ev,Gv"),
    0x88: ("mov", "Eb,Gb"), 0x89: ("mov", "Ev,Gv"), 0x8A: ("mov", "Gb,Eb"), 0x8B: ("mov", "Gv,Ev"),
    0x8D: ("lea", "Gv,M"), 0x8F: ("pop", "Ev"),
    0x90: ("nop", ""), 0x98: ("cwde", ""), 0x99: ("cdq", ""),
    0x9C: ("pushfd", ""), 0x9D: ("popfd", ""),
    0xA0: ("mov", "al,Ob"), 0xA1: ("mov", "eax,Ov"), 0xA2: ("mov", "Ob,al"), 0xA3: ("mov", "Ov,eax"),
    0xA4: ("movsb", ""), 0xA5: ("movsd", ""), 0xA8: ("test", "al,Ib"), 0xA9: ("test", "eax,Iz"),
    0xB0: ("mov", "r8,Ib"), 0xB1: ("mov", "r8,Ib"), 0xB2: ("mov", "r8,Ib"), 0xB3: ("mov", "r8,Ib"),
    0xB4: ("mov", "r8,Ib"), 0xB5: ("mov", "r8,Ib"), 0xB6: ("mov", "r8,Ib"), 0xB7: ("mov", "r8,Ib"),
    0xB8: ("mov", "r32,Iz"), 0xB9: ("mov", "r32,Iz"), 0xBA: ("mov", "r32,Iz"), 0xBB: ("mov", "r32,Iz"),
    0xBC: ("mov", "r32,Iz"), 0xBD: ("mov", "r32,Iz"), 0xBE: ("mov", "r32,Iz"), 0xBF: ("mov", "r32,Iz"),
    0xC0: ("grp2", "Eb,Ib"), 0xC1: ("grp2", "Ev,Ib"),
    0xC2: ("ret", "Iw"), 0xC3: ("ret", ""), 0xC6: ("mov", "Eb,Ib"), 0xC7: ("mov", "Ev,Iz"),
    0xC9: ("leave", ""), 0xCC: ("int3", ""),
    0xD0: ("grp2", "Eb,1"), 0xD1: ("grp2", "Ev,1"), 0xD2: ("grp2", "Eb,cl"), 0xD3: ("grp2", "Ev,cl"),
    0xE8: ("call", "Jz"), 0xE9: ("jmp", "Jz"), 0xEB: ("jmp", "Jb"),
    0xF6: ("grp3", "Eb"), 0xF7: ("grp3", "Ev"),
    0xFE: ("grp4", ""), 0xFF: ("grp5", "Ev"),
}
REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]


def modrm_len(b, i):
    """Return (length, is_memory, disp, reg, rm, sib_present)."""
    m = b[i]
    mod, reg, rm = m >> 6, (m >> 3) & 7, m & 7
    n = 1
    disp = 0
    sib = False
    if mod != 3 and rm == 4:
        sib = True
        n += 1
        base = b[i + n - 1] & 7
        if (b[i + n - 1] >> 6) == 0 and base == 5:
            disp = struct.unpack_from("<i", b, i + n)[0]
            n += 4
    if mod == 0 and rm == 5:
        disp = struct.unpack_from("<i", b, i + n)[0]
        n += 4
    elif mod == 1:
        disp = struct.unpack_from("<b", b, i + n)[0]
        n += 1
    elif mod == 2:
        disp = struct.unpack_from("<i", b, i + n)[0]
        n += 4
    return n, (mod != 3), disp, reg, rm, sib


def decode_one(b, off, va):
    """Return (length, text, target_or_None)."""
    i = 0
    prefixes = ""
    while i < len(b) and b[i] in (0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65):
        prefixes += {0x66: "op16 ", 0x67: "ad32 ", 0xF2: "repne ", 0xF3: "rep "}.get(b[i], "")
        i += 1
    if i >= len(b):
        return 1, "db ?", None
    op = b[i]
    start = i
    i += 1

    if op == 0x0F:                       # two-byte opcodes
        op2 = b[i]
        i += 1
        name = {0x0B: "ud2", 0x1F: "nop", 0x28: "movaps", 0x29: "movaps", 0x10: "movups",
                0x11: "movups", 0x2A: "cvtpi2ps", 0x2C: "cvttps2pi", 0x2D: "cvtps2pi",
                0x2E: "ucomiss", 0x2F: "comiss", 0x40: "cmovo", 0x41: "cmovno", 0x42: "cmovb",
                0x43: "cmovae", 0x44: "cmove", 0x45: "cmovne", 0x46: "cmovbe", 0x47: "cmova",
                0x4C: "cmovl", 0x4D: "cmovge", 0x4E: "cmovle", 0x4F: "cmovg", 0x54: "andps",
                0x57: "xorps", 0x58: "addps", 0x59: "mulps", 0x5A: "cvtps2pd", 0x5C: "subps",
                0x6E: "movd", 0x6F: "movq", 0x7E: "movd", 0x7F: "movq", 0x80: "jo", 0x81: "jno",
                0x82: "jb", 0x83: "jae", 0x84: "je", 0x85: "jne", 0x8C: "jl", 0x8D: "jge",
                0x8E: "jle", 0x8F: "jg", 0x90: "seto", 0x94: "sete", 0x95: "setne",
                0xB6: "movzx", 0xB7: "movzx", 0xBE: "movsx", 0xBF: "movsx", 0xAF: "imul",
                0xA2: "cpuid", 0xA3: "bt", 0xAB: "bts", 0xB0: "cmpxchg", 0xB1: "cmpxchg",
                0xC0: "xadd", 0xC1: "xadd", 0xC8: "bswap", 0xC9: "bswap", 0xCA: "bswap",
                0xCB: "bswap", 0xEF: "pxor"}.get(op2, "0F%02X" % op2)
        tgt = None
        if 0x80 <= op2 <= 0x8F:
            ln, mem, disp, reg, rm2, sib = modrm_len(b, i)
            i += ln
            rel = struct.unpack_from("<i", b, i)[0]
            i += 4
            tgt = va + i + rel
            return i - 0, "%s 0x%08X" % (name, tgt), tgt
        # most 0F forms carry a modrm
        ln, mem, disp, reg, rm2, sib = modrm_len(b, i)
        i += ln
        if op2 in (0x6E, 0x7E, 0x6F, 0x7F, 0x28, 0x29, 0x10, 0x11, 0x54, 0x57, 0x58, 0x59, 0x5C, 0x2E, 0x2F, 0xAF, 0xB6, 0xB7, 0xBE, 0xBF, 0xB0, 0xB1, 0xC0, 0xC1):
            if mem:
                return i, "%s %s,[mem]" % (name, REG32[reg]), None
            return i, "%s %s,%s" % (name, REG32[reg], REG32[rm2]), None
        return i, name, None

    if op in (0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57):
        return i, "push %s" % REG32[op - 0x50], None
    if op in (0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F):
        return i, "pop %s" % REG32[op - 0x58], None
    if op == 0x68:
        v = struct.unpack_from("<I", b, i)[0]
        return i + 4, "push 0x%08X   ; %s" % (v, string_at(v)), None
    if op == 0x6A:
        v = struct.unpack_from("<b", b, i)[0]
        return i + 1, "push %d" % v, None
    if op in (0xE8, 0xE9):
        rel = struct.unpack_from("<i", b, i)[0]
        tgt = va + i + 4 + rel
        return i + 4, "%s 0x%08X%s" % ("call" if op == 0xE8 else "jmp", tgt,
                                       "   ; -> " + string_at(tgt - IMAGE_BASE + IMAGE_BASE)
                                       if op == 0xE8 else ""), tgt
    if op == 0xEB:
        rel = struct.unpack_from("<b", b, i)[0]
        tgt = va + i + 1 + rel
        return i + 1, "jmp 0x%08X" % tgt, tgt
    if 0x70 <= op <= 0x7F:
        rel = struct.unpack_from("<b", b, i)[0]
        tgt = va + i + 1 + rel
        return i + 1, "%s 0x%08X" % (MODRM[op][0], tgt), tgt
    if op in (0xE3,):
        rel = struct.unpack_from("<b", b, i)[0]
        return i + 1, "jecxz 0x%08X" % (va + i + 1 + rel), None
    if op == 0xC3:
        return i, "ret", None
    if op == 0xC2:
        return i + 2, "ret %d" % struct.unpack_from("<H", b, i)[0], None
    if op in (0x90, 0x98, 0x99, 0x9C, 0x9D, 0xA4, 0xA5, 0xC9, 0xCC, 0xF4, 0xF5, 0xF8, 0xF9, 0xFC, 0xFD, 0xCE, 0xCF):
        return i, {0x90: "nop", 0x98: "cwde", 0x99: "cdq", 0x9C: "pushfd", 0x9D: "popfd",
                   0xA4: "movsb", 0xA5: "movsd", 0xC9: "leave", 0xCC: "int3", 0xF4: "hlt",
                   0xFC: "cld", 0xFD: "std"}.get(op, "op%02X" % op), None
    if op in (0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7):
        return i + 1, "mov %s,0x%02X" % (["al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"][op - 0xB0], b[i]), None
    if 0xB8 <= op <= 0xBF:
        v = struct.unpack_from("<I", b, i)[0]
        return i + 4, "mov %s,0x%08X%s" % (REG32[op - 0xB8], v,
                                           "   ; " + string_at(v) if string_at(v) else ""), None
    if 0xB8 <= op <= 0xBF:
        # mov r32, imm32. This form must be handled HERE, before the MODRM table: B8-BF are
        # not in that table, so a handler placed there is unreachable and the opcode falls
        # through to "unknown", which consumed 5 bytes each time. That kept the boundaries
        # aligned by luck and produced a listing that looked like disassembly and was not.
        v = struct.unpack_from("<I", b, i)[0]
        note = string_at(v)
        return i + 4, "mov %s,0x%08X%s" % (REG32[op - 0xB8], v, "   ; " + note if note else ""), None
    if op in (0xA0, 0xA1, 0xA2, 0xA3):
        v = struct.unpack_from("<I", b, i)[0]
        return i + 4, "mov %s,0x%08X" % ("eax" if op in (0xA1, 0xA3) else "al", v), None
    if op in (0x04, 0x0C, 0x14, 0x1C, 0x24, 0x2C, 0x34, 0x3C, 0xA8):
        return i + 1, "alu al,0x%02X" % b[i], None
    if op in (0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D, 0xA9):
        return i + 4, "alu eax,0x%08X" % struct.unpack_from("<I", b, i)[0], None
    if op in (0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47):
        return i, "inc %s" % REG32[op - 0x40], None
    if op in (0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F):
        return i, "dec %s" % REG32[op - 0x48], None
    if op in (0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97):
        return i, "xchg eax,%s" % REG32[op - 0x90], None
    if op in (0x9B,):
        return i, "fwait", None
    if op in (0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF):
        ln, mem, disp, reg, rm2, sib = modrm_len(b, i)
        return i + ln, "x87 op%02X" % op, None

    if op in MODRM:
        name, form = MODRM[op]
        if form == "":                                     # no operand
            return i, name, None
        if form == "Jb":
            rel = struct.unpack_from("<b", b, i)[0]
            return i + 1, "%s 0x%08X" % (name, va + i + 1 + rel), None
        if form == "Jz":
            rel = struct.unpack_from("<i", b, i)[0]
            tgt = va + i + 4 + rel
            return i + 4, "%s 0x%08X" % (name, tgt), tgt
        if form == "Iz":
            v = struct.unpack_from("<I", b, i)[0]
            return i + 4, "%s 0x%08X%s" % (name, v, "   ; " + string_at(v) if string_at(v) else ""), None
        if form == "Ib":
            return i + 1, "%s 0x%02X" % (name, b[i]), None
        if form == "Iw":
            return i + 2, "%s 0x%04X" % (name, struct.unpack_from("<H", b, i)[0]), None
        if form == "Zv":
            return i, name, None
        if form == "r32,Iz":
            # mov r32, imm32. This form was missing, so the decoder fell through to its
            # "unknown opcode" arm and consumed 5 bytes for every one of them - which kept
            # the instruction boundaries aligned by luck and produced a listing that looked
            # like disassembly while being nonsense.
            v = struct.unpack_from("<I", b, i)[0]
            note = string_at(v)
            return i + 4, "%s %s,0x%08X%s" % (name, REG32[op - 0xB8], v,
                                              "   ; " + note if note else ""), None
        # modrm forms
        ln, mem, disp, reg, rm2, sib = modrm_len(b, i)
        i += ln
        extra = 0
        if form.endswith(",Iz") or form == "Ev,Iz":
            v = struct.unpack_from("<I", b, i)[0]
            extra = 4
            tail = ",0x%08X" % v
        elif form.endswith(",Ib"):
            tail = ",0x%02X" % b[i]
            extra = 1
        else:
            tail = ""
        if mem:
            who = "[mem%s]" % ("%+d" % disp if disp else "")
        else:
            who = REG32[rm2]
        if form.startswith("Gv") or form.startswith("Gb"):
            txt = "%s %s,%s" % (name, REG32[reg], who)
        elif form.startswith("Ob") or form.startswith("Ov"):
            txt = "%s [mem],%s" % (name, REG32[reg])
        else:
            txt = "%s %s%s" % (name, who, tail)
        return i + extra, txt, None

    return i, "db 0x%02X" % op, None


STR_CACHE = {}


def string_at(va):
    """Read an ASCII string at a VIRTUAL address (as it appears in an instruction)."""
    if va in STR_CACHE:
        return STR_CACHE[va]
    off = rva_to_off(va - IMAGE_BASE)
    if off is None:
        STR_CACHE[va] = ""
        return ""
    raw = data[off:off + 64].split(b"\0")[0]
    try:
        s = raw.decode("ascii")
    except Exception:
        s = ""
    s = s if all(32 <= ord(c) < 127 for c in s) and len(s) >= 3 else ""
    STR_CACHE[va] = s
    return s


def main():
    start_rva = int(sys.argv[1], 0)
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    b = read(start_rva, 16 * count + 64)
    off = 0
    for _ in range(count):
        # The decoder works in VIRTUAL addresses: its immediates and branches are VAs, and
        # string_at() expects a VA. Passing an RVA here made every annotation wrong while the
        # instruction lengths stayed right, which is the worst kind of wrong - it looks like
        # disassembly.
        va = IMAGE_BASE + start_rva + off
        ln, txt, tgt = decode_one(b, off, va)
        raw = b[off:off + ln].hex()
        print("0x%08X  %-30s  %s" % (va, raw[:34], txt))
        off += max(1, ln)


if __name__ == "__main__":
    main()
