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
    # The first entries may be unused (index known to be >= 1, e.g. `n & 3`
    # dispatch in memcpy): skip up to 3 non-relocated leading slots.
    for _ in range(3):
        if a in mod.relocs:
            break
        a += 4
    while a in mod.relocs and mod.contains(a):
        t = mod.u32(a)
        if not mod.is_code(t):
            break
        out.append(t)
        a += 4
    # Negative indexes (`neg ecx; jmp [ecx*4 + end_of_table]` in memmove):
    # also take the relocated code pointers just before the displacement.
    a = table - 4
    while a in mod.relocs and mod.is_code(mod.u32(a)) and table - a <= 64:
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


def discover(mod: Module, extra: list[int] = ()) -> dict[int, Function]:
    seeds: set[int] = set()
    if mod.entry:
        seeds.add(mod.entry)
    seeds.update(mod.exports.values())
    seeds.update(mod.exports_ord.values())
    seeds.update(extra)
    for r in mod.relocs:
        v = mod.u32(r)
        if mod.is_code(v) and not (mod.is_code(r) and _inside_jump_table(mod, r)):
            seeds.add(v)
    funcs: dict[int, Function] = {}
    pending = sorted(seeds)
    known = set(seeds)
    while pending:
        e = pending.pop()
        if e in funcs:
            continue
        f = recover(mod, e, known)
        if not f.insns:
            continue
        if f.bad and e not in mod.exports.values() and e != mod.entry and len(f.insns) < 2:
            continue  # data that merely looked like a code pointer
        funcs[e] = f
        for t in f.calls | f.tail_calls:
            if mod.is_code(t) and t not in known:
                known.add(t)
                pending.append(t)
    return funcs


def _inside_jump_table(mod: Module, r: int) -> bool:
    # A relocated dword in .text that is followed or preceded by another
    # relocated dword is most likely a jump table entry, not an operand.
    return (r + 4) in mod.relocs or (r - 4) in mod.relocs
