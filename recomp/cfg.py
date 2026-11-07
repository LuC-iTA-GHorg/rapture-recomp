"""Function discovery and control-flow recovery (recursive descent).

Seeds: module entry point, exports, extra addresses from the game config, and
every relocated dword (in code or data) that points into an executable section
and decodes as a plausible instruction start (vtables, callbacks, `push offset
func`). Each function is the set of instructions reachable from its entry
without following calls. Jump tables (`jmp [reg*4 + table]`) are resolved by
reading consecutive relocated dwords that point back into code.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import capstone
from capstone import x86 as X

from .pe import Module

_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
_md.detail = True

COND_JUMPS = {"je", "jne", "jb", "jae", "jbe", "ja", "jl", "jge", "jle", "jg",
              "js", "jns", "jo", "jno", "jp", "jnp", "jecxz", "loop", "loope", "loopne"}


@dataclass
class Function:
    entry: int
    insns: dict[int, object] = field(default_factory=dict)   # addr -> capstone insn
    labels: set[int] = field(default_factory=set)             # branch targets inside the function
    jump_tables: dict[int, list[int]] = field(default_factory=dict)  # insn addr -> targets
    tail_calls: set[int] = field(default_factory=set)         # jmp targets that are other functions
    calls: set[int] = field(default_factory=set)              # direct call targets
    bad: list[str] = field(default_factory=list)              # decode problems


def decode(mod: Module, addr: int):
    if not mod.is_code(addr):
        return None
    code = mod.bytes_at(addr, 16)
    for i in _md.disasm(code, addr, 1):
        return i
    return None


def _jump_table(mod: Module, insn) -> list[int] | None:
    """`jmp dword ptr [reg*4 + table]` -> list of targets, or None."""
    if len(insn.operands) != 1 or insn.operands[0].type != X.X86_OP_MEM:
        return None
    m = insn.operands[0].mem
    if m.base != 0 or m.index == 0 or m.scale != 4:
        return None
    table = m.disp & 0xFFFFFFFF
    out = []
    a = table
    # A module without relocations (the exe) has no reloc set to bound the
    # table: any slot holding a code address near the jump counts.
    def slot(x: int) -> bool:
        if mod.relocs:
            return x in mod.relocs
        return mod.contains(x) and mod.is_code(mod.u32(x)) and abs(mod.u32(x) - insn.address) < 0x4000
    # The first entries may be unused (index known to be >= 1, e.g. `n & 3`
    # dispatch in memcpy): skip up to 3 non-relocated leading slots.
    for _ in range(3):
        if slot(a):
            break
        a += 4
    while slot(a) and mod.contains(a):
        t = mod.u32(a)
        if not mod.is_code(t):
            break
        out.append(t)
        a += 4
    # Negative indexes (`neg ecx; jmp [ecx*4 + end_of_table]` in memmove):
    # also take the relocated code pointers just before the displacement.
    a = table - 4
    while slot(a) and mod.is_code(mod.u32(a)) and table - a <= 64:
        out.append(mod.u32(a))
        a -= 4
    return out or None


def recover(mod: Module, entry: int, known_entries: set[int]) -> Function:
    f = Function(entry)
    work = [entry]
    f.labels.add(entry)
    while work:
        a = work.pop()
        while a not in f.insns:
            i = decode(mod, a)
            if i is None:
                f.bad.append(f"cannot decode at {a:#x}")
                break
            f.insns[a] = i
            nxt = a + i.size
            mn = i.mnemonic
            if mn in ("ret", "retf", "int3", "hlt", "ud2"):
                break
            if mn == "jmp":
                op = i.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xFFFFFFFF
                    if t in known_entries and t != entry:
                        f.tail_calls.add(t)
                    else:
                        f.labels.add(t)
                        work.append(t)
                else:
                    jt = _jump_table(mod, i)
                    if jt:
                        f.jump_tables[a] = jt
                        for t in jt:
                            f.labels.add(t)
                            work.append(t)
                break
            if mn in COND_JUMPS:
                t = i.operands[0].imm & 0xFFFFFFFF
                f.labels.add(t)
                work.append(t)
                f.labels.add(nxt)
            elif mn == "call":
                op = i.operands[0]
                if op.type == X.X86_OP_IMM:
                    f.calls.add(op.imm & 0xFFFFFFFF)
            a = nxt
    return f


# Instructions that never occur in MSVC 6 output: a candidate entry whose
# recovered body contains one of them is data that looked like a code pointer.
GARBAGE = {"in", "out", "insb", "insd", "insw", "outsb", "outsd", "outsw", "into", "das", "daa",
           "aaa", "aas", "aam", "aad", "lds", "les", "lfs", "lgs", "bound", "arpl", "hlt", "retf",
           "iretd", "cli", "sti", "salc", "ljmp", "lcall", "sldt", "str",
           "lgdt", "lidt", "popal", "icebp", "int1"}
_SEG_OVERRIDES = {0x2E, 0x36, 0x26, 0x65, 0x3E}


def plausible(f: Function) -> bool:
    if f.bad:
        return False
    for i in f.insns.values():
        if i.mnemonic in GARBAGE or i.mnemonic.startswith(("rep in", "rep out")):
            return False
        if i.bytes[:2] == bytes(2):                  # add byte ptr [eax], al: zero fill
            return False
        if i.prefix[1] in _SEG_OVERRIDES:
            return False
        for op in i.operands:
            if op.type == X.X86_OP_REG and i.reg_name(op.reg) in ("es", "cs", "ss", "ds", "gs"):
                return False
    return True


def _code_immediates(mod: Module, f: Function) -> set[int]:
    """Immediate operands (`push offset f`, `mov [x], offset f`) that point into code."""
    out = set()
    for i in f.insns.values():
        if i.mnemonic in COND_JUMPS or i.mnemonic in ("jmp", "call"):
            continue
        for op in i.operands:
            if op.type == X.X86_OP_IMM and op.size == 4:
                v = op.imm & 0xFFFFFFFF
                if mod.is_code(v):
                    out.add(v)
    return out


def _data_pointers(mod: Module) -> list[int]:
    """Code pointers stored in data sections (vtables, callback tables)."""
    out = []
    if mod.relocs:
        for r in mod.relocs:
            if not mod.is_code(r) and mod.is_code(mod.u32(r)):
                out.append(mod.u32(r))
        return out
    # No relocations (exe at a fixed base): scan aligned dwords of the data sections.
    for s in mod.sections:
        if s.executable:
            continue
        for a in range(s.va, s.va + s.vsize - 3, 4):
            v = mod.u32(a)
            if mod.is_code(v):
                out.append(v)
    return out


def discover(mod: Module, extra: list[int] = ()) -> dict[int, Function]:
    """Phase 1: trusted seeds (entry, exports, config extras) and their direct
    callees, always accepted. Phase 2: candidates from code pointers in data and
    from code-pointing immediates of accepted functions; a candidate is accepted
    only if its body is plausible and it does not start inside an accepted
    function (a branch label or the middle of an instruction)."""
    strong: set[int] = set(extra)
    if mod.entry:
        strong.add(mod.entry)
    strong.update(mod.exports.values())
    strong.update(mod.exports_ord.values())

    funcs: dict[int, Function] = {}
    covered: set[int] = set()     # every byte of every accepted instruction, entries excluded
    known: set[int] = set(strong)

    def accept(f: Function) -> None:
        funcs[f.entry] = f
        for a, i in f.insns.items():
            covered.update(range(a + (1 if a == f.entry else 0), a + i.size))

    def grow(seeds: list[int], trusted: bool) -> list[int]:
        """Recover seeds and their direct callees; return new candidate pointers."""
        found: list[int] = []
        pending = list(seeds)
        while pending:
            e = pending.pop()
            if e in funcs or not mod.is_code(e):
                continue
            if not trusted and e in covered:
                continue
            f = recover(mod, e, known | {e})
            if not f.insns:
                continue
            if not trusted and not plausible(f):
                continue
            accept(f)
            known.add(e)
            for t in f.calls | f.tail_calls:
                if t not in funcs:
                    known.add(t)
                    pending.append(t)
            found.extend(_code_immediates(mod, f))
        return found

    cand = grow(sorted(strong), trusted=True)
    cand.extend(_data_pointers(mod))
    seen: set[int] = set()
    while True:
        todo = sorted(set(cand) - seen - set(funcs))
        if not todo:
            break
        seen.update(todo)
        known.update(todo)
        cand = grow(todo, trusted=False)
    # Callees of a trusted function are trusted too, but a function first
    # recovered with an incomplete `known` set may have treated a later entry
    # as a local label: harmless (the code is duplicated, not lost).
    return funcs
