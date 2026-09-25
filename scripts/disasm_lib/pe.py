"""Read-only PE32/PE32+ image loader.

The existing one-off scripts (`pe_info.py`, `pe_imports.py`, `xrefs.py`) each re-parse the
headers inline with `struct.unpack_from`. That was fine while the only job was printing a
header dump; it is not fine as the base of a disassembler, where every address -> offset
conversion decides whether a listing is real or garbage.

This module is the single place that knows how the file maps to memory. It is deliberately
read-only: nothing here writes to the image or to disk.

Conventions used everywhere downstream:
  * an "rva" is an address relative to the image base
  * a "va"  is a runtime address (image base + rva)
  * addresses are ints, never hex strings, until they are rendered
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field


class PeError(Exception):
    pass


@dataclass
class Section:
    name: str
    va: int              # RVA of the section
    vsize: int           # virtual size (may exceed raw size)
    raw: int             # file offset of the raw data
    rawsize: int
    chars: int

    @property
    def is_code(self) -> bool:
        return bool(self.chars & 0x00000020)          # IMAGE_SCN_CNT_CODE

    @property
    def is_executable(self) -> bool:
        return bool(self.chars & 0x20000000)          # IMAGE_SCN_MEM_EXECUTE

    @property
    def is_discardable(self) -> bool:
        return bool(self.chars & 0x02000000)          # IMAGE_SCN_MEM_DISCARDABLE

    @property
    def span(self) -> int:
        """Size used for address arithmetic: the larger of virtual and raw size.

        Using max() rather than vsize is deliberate: a section may have rawsize > vsize
        (padding), and treating the padding as unmapped creates holes that a decoder would
        report as "outside image" for addresses that genuinely exist on disk.
        """
        return max(self.vsize, self.rawsize)

    def __str__(self) -> str:
        return "%s rva=0x%08X vsize=0x%X raw=0x%X rawsize=0x%X%s%s" % (
            self.name, self.va, self.vsize, self.raw, self.rawsize,
            " CODE" if self.is_code else "", " EXEC" if self.is_executable else "")


@dataclass
class Import:
    dll: str
    name: str | None          # None when imported by ordinal
    ordinal: int | None
    iat_rva: int              # RVA of the IAT slot that holds the resolved address

    def __str__(self) -> str:
        if self.name:
            return "%s!%s" % (self.dll, self.name)
        return "%s!#%d" % (self.dll, self.ordinal or 0)


@dataclass
class Export:
    name: str | None
    ordinal: int
    rva: int
    forwarder: str | None = None


@dataclass
class Reloc:
    rva: int                  # where the fixup lives
    type: int                 # IMAGE_REL_BASED_*


@dataclass
class Image:
    path: str
    data: bytes
    is_64: bool
    image_base: int
    entry_rva: int
    sections: list[Section] = field(default_factory=list)
    imports: list[Import] = field(default_factory=list)
    exports: list[Export] = field(default_factory=list)
    tls_callbacks: list[int] = field(default_factory=list)      # RVAs
    relocs: list[Reloc] = field(default_factory=list)
    dll_name: str = ""
    timestamp: int = 0
    subsystem: int = 0
    # rva -> import, filled from the IAT so a `call [0x...]` can be annotated
    iat: dict[int, Import] = field(default_factory=dict)
    # anything surprising found while parsing headers; surfaced by summary() rather than
    # swallowed, because a quietly-wrong image produces a confidently-wrong listing
    header_notes: list[str] = field(default_factory=list)

    # ---------------------------------------------------------------- construction
    @classmethod
    def load(cls, path: str) -> "Image":
        with open(path, "rb") as fh:
            return cls.from_bytes(fh.read(), path)

    @classmethod
    def from_bytes(cls, data: bytes, path: str = "<memory>") -> "Image":
        if len(data) < 0x40 or data[:2] != b"MZ":
            raise PeError("not an MZ image: %s" % path)
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
            raise PeError("no PE signature at 0x%X in %s" % (pe, path))
        machine, nsec, timestamp = struct.unpack_from("<HHI", data, pe + 4)
        optsz, chars = struct.unpack_from("<HH", data, pe + 20)
        opt = pe + 24

        is_64 = machine == 0x8664
        if machine == 0x14C:
            is_64 = False
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic == 0x20B:
            is_64 = True
        elif magic == 0x10B:
            is_64 = False
        else:
            raise PeError("unknown optional header magic 0x%04X" % magic)

        # Field offsets inside IMAGE_OPTIONAL_HEADER are NOT the same for PE32 and PE32+,
        # because PE32+ drops BaseOfData and widens the four size fields. In PE32 the
        # sequence is ...AddressOfEntryPoint(+16), BaseOfCode(+20), BaseOfData(+24),
        # ImageBase(+28); in PE32+ there is no BaseOfData and ImageBase is a 64-bit field
        # at +24.
        #
        # Reading ImageBase from +24 for both - which this loader did at first - yields
        # BaseOfData on a PE32 image. Here that is 0x1102000 rather than the real 0x400000,
        # so every virtual address the tool computed was off by 0xD02000: plausible-looking,
        # self-consistent, and completely wrong. Hence the sanity check below rather than a
        # bare unpack.
        image_base = struct.unpack_from("<Q" if is_64 else "<I", data,
                                        opt + (24 if is_64 else 28))[0]
        entry_rva = struct.unpack_from("<I", data, opt + 16)[0]
        subsystem = struct.unpack_from("<H", data, opt + 68)[0]

        if not (0x10000 <= image_base <= 0xFFFFFFFF) or (image_base & 0xFFFF):
            raise PeError("implausible ImageBase 0x%X in %s - the optional header layout "
                          "changed, so every address downstream would be wrong"
                          % (image_base, path))

        img = cls(path=path, data=data, is_64=is_64, image_base=image_base,
                  entry_rva=entry_rva, timestamp=timestamp, subsystem=subsystem)

        sec_off = opt + optsz
        for i in range(nsec):
            o = sec_off + i * 40
            if o + 40 > len(data):
                break
            name = data[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, o + 8)
            schars = struct.unpack_from("<I", data, o + 36)[0]
            img.sections.append(Section(name, va, vsize, raw, rawsize, schars))

        img._read_data_directories(opt, optsz, magic)
        for imp in img.imports:
            img.iat[imp.iat_rva] = imp
        return img

    def _read_data_directories(self, opt: int, optsz: int, magic: int) -> None:
        """Locate the 16 IMAGE_DATA_DIRECTORY entries.

        Do NOT trust the offset everyone quotes for NumberOfRvaAndSizes. The widely-copied
        layout (and the one this file used first) says opt+96 for PE32 / opt+112 for PE32+,
        but those figures are four bytes late: the real field is at opt+92 / opt+108, with
        the directory array at opt+96 / opt+112.

        Getting it wrong is silent, not loud. Reading at opt+96 lands on the first
        directory's own Rva field, so NumberOfRvaAndSizes comes back 0, the array is read
        one slot late (directory N gets directory N+1's values), and a loader cheerfully
        reports "0 imports, 0 relocations, no TLS" for an exe that has all three. That is
        precisely how the earlier one-off PE scripts in this directory managed to look
        authoritative while reporting an import table that does not exist.

        Rather than pick a constant, derive the array position from SizeOfOptionalHeader,
        which is authoritative by definition, and cross-check against the magic-derived
        offset. Any disagreement is reported instead of ignored.
        """
        d = self.data
        is64 = self.is_64
        dirsize = 8
        ndirs = 16

        derived = opt + optsz - ndirs * dirsize
        magic_off = opt + (108 if is64 else 92)
        expected = magic_off + 4

        notes = []
        if derived != expected:
            notes.append("SizeOfOptionalHeader implies data dirs at opt+0x%X, "
                         "magic implies opt+0x%X" % (derived - opt, expected - opt))
        # SizeOfOptionalHeader is occasionally stripped by protectors; fall back to magic.
        dd_off = derived if 0 < optsz <= 0x1000 else expected
        if dd_off < opt or dd_off + ndirs * dirsize > len(d):
            dd_off = expected
            notes.append("fell back to the magic-derived data directory offset")
        self.header_notes = notes

        ndd_off = dd_off - 4
        ndd = struct.unpack_from("<I", d, ndd_off)[0]
        if not (0 <= ndd <= 16):
            notes.append("NumberOfRvaAndSizes = %d is out of range; clamping" % ndd)
            ndd = min(max(ndd, 0), 16)

        dirs = []
        for i in range(min(ndd, ndirs)):
            o = dd_off + i * dirsize
            if o + dirsize > len(d):
                break
            dirs.append(struct.unpack_from("<II", d, o))
        while len(dirs) < ndirs:
            dirs.append((0, 0))

        self._read_exports(dirs[0])
        self._read_imports(dirs[1])
        self._read_relocs(dirs[5])
        self._read_tls(dirs[9], is64)

    # ---------------------------------------------------------------- directories
    def _read_exports(self, dd) -> None:
        rva, size = dd
        if not rva:
            return
        d = self.data
        o = self.rva_to_off(rva)
        if o is None or o + 40 > len(d):
            return
        (chars, ts, major, minor, name_rva, base, nfunc, nname,
         afunc, aname, aord) = struct.unpack_from("<IIHHIIIIIII", d, o)
        self.dll_name = self.read_string(name_rva) if name_rva else ""
        ords = self._dwords(aord, nname)
        names = self._dwords(aname, nname)
        funcs = self._dwords(afunc, nfunc)
        # RVA of the export directory and of its name table give a cheap forwarder test
        edata = (rva, rva + size)
        for i in range(nname):
            if i >= len(names) or names[i] == 0:
                continue
            nm = self.read_string(names[i])
            idx = ords[i] if i < len(ords) else 0
            if idx >= len(funcs):
                continue
            frva = funcs[idx]
            fwd = None
            if edata[0] <= frva < edata[1]:
                fwd = self.read_string(frva)
            self.exports.append(Export(nm, base + idx, frva, fwd))

    def _read_imports(self, dd) -> None:
        rva, size = dd
        if not rva:
            return
        d = self.data
        o = self.rva_to_off(rva)
        if o is None:
            return
        idx = 0
        while True:
            ent = o + idx * 20
            if ent + 20 > len(d):
                break
            oft, ts, fwd, name_rva, first = struct.unpack_from("<IIIII", d, ent)
            if name_rva == 0 and first == 0:
                break
            dll = self.read_string(name_rva) or "<noname>"
            thunk_rva = oft or first
            iat_rva = first
            t = self.rva_to_off(thunk_rva)
            if t is not None:
                k = 0
                while True:
                    off = t + k * (8 if self.is_64 else 4)
                    if off + (8 if self.is_64 else 4) > len(d):
                        break
                    val = struct.unpack_from("<Q" if self.is_64 else "<I", d, off)[0]
                    if val == 0:
                        break
                    if self.is_64:
                        by_ord = bool(val & (1 << 63))
                    else:
                        by_ord = bool(val & (1 << 31))
                    if by_ord:
                        self.imports.append(Import(dll, None, val & 0xFFFF,
                                                   iat_rva + k * (8 if self.is_64 else 4)))
                    else:
                        hint_name_rva = val & 0x7FFFFFFF
                        nm = self.read_string(hint_name_rva + 2)
                        self.imports.append(Import(dll, nm, None,
                                                   iat_rva + k * (8 if self.is_64 else 4)))
                    k += 1
                    if k > 0x4000:
                        break
            idx += 1
            if idx > 0x1000:
                break

    def _read_relocs(self, dd) -> None:
        rva, size = dd
        if not rva or size < 8:
            return
        d = self.data
        o = self.rva_to_off(rva)
        if o is None:
            return
        end = o + size
        while o + 8 <= min(end, len(d)):
            page, blk = struct.unpack_from("<II", d, o)
            if blk < 8:
                break
            n = (blk - 8) // 2
            for i in range(n):
                w = struct.unpack_from("<H", d, o + 8 + i * 2)[0]
                self.relocs.append(Reloc(page + (w & 0xFFF), w >> 12))
            o += blk

    def _read_tls(self, dd, is64: bool) -> None:
        rva, size = dd
        if not rva:
            return
        d = self.data
        o = self.rva_to_off(rva)
        if o is None:
            return
        # IMAGE_TLS_DIRECTORY32/64: StartAddressOfRawData, EndAddressOfRawData,
        # AddressOfIndex, AddressOfCallBacks, SizeOfZeroFill, Characteristics
        addr_callbacks = struct.unpack_from("<Q" if is64 else "<I", d, o + (24 if is64 else 12))[0]
        if not addr_callbacks:
            return
        off = self.rva_to_off(addr_callbacks - self.image_base)
        if off is None:
            return
        for i in range(64):
            p = off + i * (8 if is64 else 4)
            if p + (8 if is64 else 4) > len(d):
                break
            v = struct.unpack_from("<Q" if is64 else "<I", d, p)[0]
            if v == 0:
                break
            self.tls_callbacks.append(v - self.image_base)

    def _dwords(self, rva: int, count: int) -> list[int]:
        o = self.rva_to_off(rva)
        if o is None:
            return []
        count = max(0, min(count, 0x20000))
        return list(struct.unpack_from("<%dI" % count, self.data, o)) if count else []

    # ---------------------------------------------------------------- addressing
    def _section_span(self, s: Section) -> tuple[int, int]:
        """(start, end) in RVAs for a section, as sizes actually appear on disk.

        `span` here is min(vsize, rawsize) rather than the usual max. That is deliberate and
        it was found the hard way on BH6.exe: `.text` declares vsize 0x110002B with rawsize
        0x1100200, and `.rdata` begins at exactly 0x1102000 - inside `.text`'s virtual
        extent. Using max() therefore swallowed the first 0x1D5 bytes of `.rdata`, and every
        address in that window resolved to `.text`, i.e. to an executable section. The
        analyser then treated data pointers as function pointers and produced 87 000 bogus
        code roots. The disk size is the honest bound for bytes that exist.
        """
        return s.va, s.va + min(s.vsize, s.rawsize) if s.rawsize else s.va + s.vsize

    def section_of_rva(self, rva: int) -> Section | None:
        """The section containing `rva`, preferring the section that *declares* it.

        Overlap between sections is real, not hypothetical: on BH6.exe `.rdata`'s rva
        (0x1102000) falls inside `.text`'s declared virtual extent, so a first-match or
        smallest-match rule labels a megabyte of read-only data as executable code - which is
        what made the analyser treat 87 000 data pointers as function pointers.

        The rule that works is: a section wins if the rva is inside its *declared* virtual
        size; the disk size (rawsize) is only a fallback for the tail, and only when no
        section declares that rva. `.text` declares 0x110002B bytes, which stops short of
        `.rdata`'s start, so `.rdata` is resolved correctly even though `.text`'s raw bytes
        physically extend further.
        """
        for s in self.sections:
            if s.va <= rva < s.va + s.vsize:
                return s
        for s in self.sections:
            if s.va <= rva < s.va + s.rawsize:
                return s
        return None

    def overlapping_data_sections(self) -> list[int]:
        """RVAs of sections whose declared start another section's raw extent covers.

        A scanner hunting for data (pointer tables, string runs) should skip these, because
        the bytes there belong to the earlier section and are already accounted for.
        """
        out = []
        for s in self.sections:
            for t in self.sections:
                if t is s or not t.is_executable:
                    continue
                if t.va <= s.va < t.va + t.rawsize and not (t.va <= s.va < t.va + t.vsize):
                    out.append(s.va)
                    break
        return out

    def off_to_rva(self, off: int) -> int | None:
        for s in self.sections:
            if s.raw <= off < s.raw + s.span:
                return s.va + (off - s.raw)
        return None

    def rva_to_off(self, rva: int) -> int | None:
        """File offset holding the byte at `rva`, or None if the byte is not on disk.

        `.data` normally declares more virtual size than raw size, and the difference is
        zero-initialised BSS: those bytes *have no file content at all*. This function used
        to return `raw + (rva - va)` regardless, which for BSS addresses lands somewhere else
        in the file entirely - reading 0x17C3164 (the DTI object BH6's registration code
        writes to) returned bytes belonging to another section and looked like real data.
        Reporting None is the honest answer, and callers that need to handle BSS can.
        """
        if rva < 0:
            return None
        for s in self.sections:
            if not (s.va <= rva < s.va + s.vsize):
                continue
            within = rva - s.va
            if within >= s.rawsize:
                return None                      # BSS: no bytes on disk
            o = s.raw + within
            return o if o < len(self.data) else None
        for s in self.sections:
            if s.va <= rva < s.va + s.rawsize:
                o = s.raw + (rva - s.va)
                return o if o < len(self.data) else None
        # headers are addressable as rva < first section
        if rva < (self.sections[0].va if self.sections else 0x1000):
            return rva if rva < len(self.data) else None
        return None

    def is_bss_rva(self, rva: int) -> bool:
        """True when the rva is inside a section's virtual-only tail."""
        for s in self.sections:
            if s.va <= rva < s.va + s.vsize and (rva - s.va) >= s.rawsize:
                return True
        return False

    def is_mapped_rva(self, rva: int) -> bool:
        return self.rva_to_off(rva) is not None

    def read_rva(self, rva: int, n: int) -> bytes:
        o = self.rva_to_off(rva)
        if o is None:
            return b""
        return self.data[o:o + n]

    def read_u32_rva(self, rva: int) -> int | None:
        b = self.read_rva(rva, 4)
        return struct.unpack("<I", b)[0] if len(b) == 4 else None

    def read_string(self, rva: int, maxlen: int = 256) -> str:
        o = self.rva_to_off(rva)
        if o is None:
            return ""
        raw = self.data[o:o + maxlen].split(b"\0")[0]
        return raw.decode("ascii", "replace")

    def is_printable_string_at(self, rva: int, minlen: int = 4) -> str:
        o = self.rva_to_off(rva)
        if o is None:
            return ""
        raw = self.data[o:o + 128].split(b"\0")[0]
        if len(raw) < minlen:
            return ""
        try:
            s = raw.decode("ascii")
        except UnicodeDecodeError:
            return ""
        return s if all(32 <= ord(c) < 127 for c in s) else ""

    # ---------------------------------------------------------------- facts
    @property
    def entry_va(self) -> int:
        return self.image_base + self.entry_rva

    def code_sections(self) -> list[Section]:
        return [s for s in self.sections if s.is_executable and not s.is_discardable]

    def summary(self) -> str:
        out = ["%s  (%s)" % (self.path, "PE32+" if self.is_64 else "PE32"),
               "image base  0x%08X   entry rva 0x%08X   subsystem %d   timestamp 0x%08X"
               % (self.image_base, self.entry_rva, self.subsystem, self.timestamp)]
        for s in self.sections:
            out.append("  " + str(s))
        out.append("imports: %d from %d dll(s); exports: %d; tls callbacks: %d; relocs: %d"
                   % (len(self.imports), len({i.dll for i in self.imports}),
                      len(self.exports), len(self.tls_callbacks), len(self.relocs)))
        for n in self.header_notes:
            out.append("  ! " + n)
        return "\n".join(out)


def find_pattern(img: Image, pat: bytes, sections: list[Section] | None = None):
    """Yield RVAs of every occurrence of `pat` inside the given sections."""
    for s in (sections if sections is not None else img.sections):
        blob = img.data[s.raw:s.raw + min(s.rawsize, len(img.data) - s.raw)]
        start = 0
        while True:
            j = blob.find(pat, start)
            if j < 0:
                break
            yield s.va + j
            start = j + 1
