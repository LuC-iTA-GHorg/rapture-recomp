"""PE32 image loading and static rebasing.

The recompiler maps every module of a game into one flat 32-bit guest address
space. Several modules share the same preferred ImageBase (0x10000000), so each
module is assigned its own base by the game config and its base relocations are
applied here, at recompile time. The translator then sees final addresses in
every instruction and never has to think about relocation again.

Nothing read here is ever written to the repository: the user's own files are
read during the build and only the generated C (in the build directory) holds
anything derived from them.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

import pefile


@dataclass
class Import:
    dll: str            # lower-case DLL name, e.g. "kernel32.dll"
    name: str | None    # symbol name, or None for an ordinal import
    ordinal: int | None
    slot: int           # guest address of the IAT slot


@dataclass
class Section:
    name: str
    va: int             # guest address (rebased)
    vsize: int
    executable: bool


@dataclass
class Module:
    path: str
    name: str           # lower-case file name, e.g. "shoot.dll"
    base: int           # guest base actually used
    pref_base: int
    size: int
    entry: int          # guest address of the entry point (0 if none)
    image: bytearray    # memory image, already rebased
    sections: list[Section] = field(default_factory=list)
    imports: list[Import] = field(default_factory=list)
    exports: dict[str, int] = field(default_factory=dict)       # name -> address
    exports_ord: dict[int, int] = field(default_factory=dict)   # ordinal -> address
    relocs: set[int] = field(default_factory=set)               # guest addrs of relocated dwords

    # -- memory access -------------------------------------------------
    def contains(self, addr: int) -> bool:
        return self.base <= addr < self.base + self.size

    def u8(self, addr: int) -> int:
        return self.image[addr - self.base]

    def u32(self, addr: int) -> int:
        o = addr - self.base
        return struct.unpack_from("<I", self.image, o)[0]

    def bytes_at(self, addr: int, n: int) -> bytes:
        o = addr - self.base
        return bytes(self.image[o:o + n])

    def is_code(self, addr: int) -> bool:
        return any(s.executable and s.va <= addr < s.va + s.vsize for s in self.sections)


def load(path: str, base: int | None = None) -> Module:
    pe = pefile.PE(path, fast_load=False)
    oh = pe.OPTIONAL_HEADER
    pref = oh.ImageBase
    if base is None:
        base = pref
    image = bytearray(pe.get_memory_mapped_image(ImageBase=pref))
    image.extend(b"\0" * max(0, oh.SizeOfImage - len(image)))
    delta = (base - pref) & 0xFFFFFFFF

    relocs: set[int] = set()
    for blk in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for e in blk.entries:
            if e.type == 0:          # IMAGE_REL_BASED_ABSOLUTE (padding)
                continue
            if e.type != 3:          # IMAGE_REL_BASED_HIGHLOW
                raise ValueError(f"{path}: unsupported relocation type {e.type}")
            rva = e.rva
            v = struct.unpack_from("<I", image, rva)[0]
            struct.pack_into("<I", image, rva, (v + delta) & 0xFFFFFFFF)
            relocs.add(base + rva)
    if delta and not relocs:
        raise ValueError(f"{path}: cannot rebase a module without relocations")

    name = path.replace("\\", "/").rsplit("/", 1)[-1].lower()
    m = Module(path=path, name=name, base=base, pref_base=pref, size=oh.SizeOfImage,
               entry=(base + oh.AddressOfEntryPoint) if oh.AddressOfEntryPoint else 0,
               image=image, relocs=relocs)
    for s in pe.sections:
        m.sections.append(Section(
            name=s.Name.rstrip(b"\0").decode("ascii", "replace"),
            va=base + s.VirtualAddress, vsize=s.Misc_VirtualSize,
            executable=bool(s.Characteristics & 0x20000000)))
    for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for imp in d.imports:
            m.imports.append(Import(
                dll=d.dll.decode("ascii").lower(),
                name=imp.name.decode("ascii") if imp.name else None,
                ordinal=imp.ordinal if not imp.name else None,
                slot=(imp.address - pref + base) & 0xFFFFFFFF))
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            if s.forwarder:
                continue
            a = base + s.address
            m.exports_ord[s.ordinal] = a
            if s.name:
                m.exports[s.name.decode("ascii")] = a
    return m
