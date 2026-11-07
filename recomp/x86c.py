"""x86-32 -> C translator.

Every guest function becomes one C function `void f_XXXXXXXX(CPU *c)`.
Guest registers live in the CPU struct; flags are C locals of the function
(MSVC code never keeps flags live across a call or a return), so the C
compiler deletes every flag computation that no branch reads. The guest stack
is real guest memory: `call` pushes the guest return address, `ret` pops it,
so stack-relative code (arguments, locals, `this` adjustments, SEH frames)
behaves exactly as in the original.

Unsupported instructions do not stop the build: they become a call to
`rc_unimpl()`, which aborts with the address when executed, and they are
counted in the build report.
"""
from __future__ import annotations

from capstone import x86 as X

from .cfg import Function

R32 = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"}
R16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
R8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
R8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}

MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}
SIGN = {1: "0x80u", 2: "0x8000u", 4: "0x80000000u"}
BITS = {1: 8, 2: 16, 4: 32}
STYPE = {1: "int8_t", 2: "int16_t", 4: "int32_t"}
UTYPE = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}

CC = {
    "e": "zf", "ne": "!zf", "b": "cf", "ae": "!cf", "be": "(cf|zf)", "a": "!(cf|zf)",
    "l": "(sf!=of)", "ge": "(sf==of)", "le": "(zf|(sf!=of))", "g": "(!zf&&(sf==of))",
    "s": "sf", "ns": "!sf", "o": "of", "no": "!of", "p": "pf", "np": "!pf",
}


class Unsupported(Exception):
    pass


def fname(addr: int) -> str:
    return f"f_{addr:08x}"


class Translator:
    def __init__(self, func: Function, known: set[int]):
        self.f = func
        self.known = known           # every function entry of every module
        self.out: list[str] = []
        self.unimpl: list[str] = []
        self.label_used: set[int] = set(func.labels)

    # -- operands ------------------------------------------------------
    def reg_read(self, name: str) -> str:
        if name in R32:
            return f"c->{name}"
        if name in R16:
            return f"(uint16_t)c->{R16[name]}"
        if name in R8L:
            return f"(uint8_t)c->{R8L[name]}"
        if name in R8H:
            return f"(uint8_t)(c->{R8H[name]} >> 8)"
        raise Unsupported(f"register {name}")

    def reg_write(self, name: str, v: str) -> str:
        if name in R32:
            return f"c->{name} = (uint32_t)({v});"
        if name in R16:
            r = R16[name]
            return f"c->{r} = (c->{r} & 0xFFFF0000u) | (uint16_t)({v});"
        if name in R8L:
            r = R8L[name]
            return f"c->{r} = (c->{r} & 0xFFFFFF00u) | (uint8_t)({v});"
        if name in R8H:
            r = R8H[name]
            return f"c->{r} = (c->{r} & 0xFFFF00FFu) | ((uint32_t)(uint8_t)({v}) << 8);"
        raise Unsupported(f"register {name}")

    def ea(self, i, op) -> str:
        m = op.mem
        parts = []
        if m.base:
            parts.append(f"c->{self._r32(i.reg_name(m.base))}")
        if m.index:
            idx = f"c->{self._r32(i.reg_name(m.index))}"
            parts.append(idx if m.scale == 1 else f"{idx}*{m.scale}u")
        if m.disp or not parts:
            parts.append(f"0x{m.disp & 0xFFFFFFFF:x}u")
        e = " + ".join(parts)
        if m.segment:
            seg = i.reg_name(m.segment)
            if seg == "fs":
                e = f"c->fs_base + {e}"
            elif seg not in ("ds", "ss", "es", "cs"):
                raise Unsupported(f"segment {seg}")
        return f"(uint32_t)({e})"

    def _r32(self, name: str) -> str:
        if name not in R32:
            raise Unsupported(f"address register {name}")
        return name

    def rd(self, i, op, ea: str | None = None) -> str:
        if op.type == X.X86_OP_REG:
            return self.reg_read(i.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return f"0x{op.imm & ((1 << (8 * op.size)) - 1):x}u"
        if op.type == X.X86_OP_MEM:
            return f"rd{BITS[op.size]}({ea or self.ea(i, op)})"
        raise Unsupported("operand type")

    def wr(self, i, op, v: str, ea: str | None = None) -> str:
        if op.type == X.X86_OP_REG:
            return self.reg_write(i.reg_name(op.reg), v)
        if op.type == X.X86_OP_MEM:
            return f"wr{BITS[op.size]}({ea or self.ea(i, op)}, ({UTYPE[op.size]})({v}));"
        raise Unsupported("write to non-lvalue")

    @staticmethod
    def szp(n: int) -> str:
        return f"zf = (r == 0); sf = (r >> {BITS[n] - 1}) & 1; pf = rc_parity(r);"

    # -- main ----------------------------------------------------------
    def translate(self) -> str:
        f = self.f
        addrs = sorted(f.insns)
        body: list[str] = []
        need_labels: set[int] = set(f.labels)
        for k, a in enumerate(addrs):
            i = f.insns[a]
            try:
                code = self.insn(i)
            except Unsupported as e:
                self.unimpl.append(f"{a:#x} {i.mnemonic} {i.op_str} ({e})")
                code = [f'rc_unimpl(c, 0x{a:x}u, "{i.mnemonic} {i.op_str}");']
            body.append((a, i, code))
            nxt = a + i.size
            falls = not self._ends_block(i)
            if falls and (k + 1 == len(addrs) or addrs[k + 1] != nxt):
                body[-1][2].append(f"goto L_{nxt:08x};")
                need_labels.add(nxt)
        missing = [t for t in need_labels if t not in f.insns]
        lines = [f"void {fname(f.entry)}(CPU *c) {{",
                 "    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0;",
                 "    (void)cf; (void)zf; (void)sf; (void)of; (void)pf;"]
        if f.entry != addrs[0]:
            lines.append(f"    goto L_{f.entry:08x};")
        for a, i, code in body:
            if a in need_labels:
                lines.append(f"L_{a:08x}:;")
            lines.append(f"    /* {a:08x}: {i.mnemonic} {i.op_str} */")
            for ln in code:
                lines.append("    " + ln)
        for t in missing:
            lines.append(f"L_{t:08x}:; rc_fault(c, 0x{t:x}u, \"branch target not decoded\");")
        lines.append("}")
        return "\n".join(lines)

    @staticmethod
    def _ends_block(i) -> bool:
        return i.mnemonic in ("ret", "retf", "jmp", "int3", "hlt", "ud2")

    def insn(self, i) -> list[str]:
        mn = i.mnemonic
        ops = i.operands
        a = i.address
        nxt = a + i.size
        prefix = ""
        if mn.startswith(("rep ", "repe ", "repne ", "repz ", "repnz ")):
            prefix, mn = mn.split(" ", 1)

        if prefix or mn in ("stosb", "stosw", "stosd", "movsb", "movsw", "movsd",
                            "scasb", "scasd", "cmpsb", "cmpsd", "lodsb", "lodsd"):
            return self.string_op(i, prefix, mn)

        if mn == "nop":
            return []
        if mn == "mov":
            return [self.wr(i, ops[0], self.rd(i, ops[1]))]
        if mn in ("movzx", "movsx"):
            s = ops[1].size
            v = self.rd(i, ops[1])
            if mn == "movsx":
                v = f"(uint32_t)(int32_t)({STYPE[s]})({v})"
            return [self.wr(i, ops[0], v)]
        if mn == "lea":
            m = ops[1]
            if m.mem.segment:
                raise Unsupported("lea with segment")
            return [self.wr(i, ops[0], self.ea(i, m))]
        if mn == "push":
            v = self.rd(i, ops[0])
            if ops[0].type == X.X86_OP_IMM:
                v = f"0x{ops[0].imm & 0xFFFFFFFF:x}u"
            if ops[0].size == 2:
                raise Unsupported("16-bit push")
            return [f"{{ uint32_t v = {v}; c->esp -= 4; wr32(c->esp, v); }}"]
        if mn == "pop":
            return ["{ uint32_t v = rd32(c->esp); c->esp += 4; " + self.wr(i, ops[0], "v") + " }"]
        if mn == "leave":
            return ["c->esp = c->ebp; c->ebp = rd32(c->esp); c->esp += 4;"]
        if mn == "enter":
            if ops[1].imm != 0:
                raise Unsupported("enter with nesting level")
            return [f"c->esp -= 4; wr32(c->esp, c->ebp); c->ebp = c->esp; c->esp -= 0x{ops[0].imm:x}u;"]
        if mn == "xchg":
            ea0 = self.ea(i, ops[0]) if ops[0].type == X.X86_OP_MEM else None
            ea1 = self.ea(i, ops[1]) if ops[1].type == X.X86_OP_MEM else None
            return ["{ uint32_t t0 = " + self.rd(i, ops[0], ea0) + "; uint32_t t1 = " + self.rd(i, ops[1], ea1) + "; "
                    + self.wr(i, ops[0], "t1", ea0) + " " + self.wr(i, ops[1], "t0", ea1) + " }"]
        if mn in ("add", "adc", "sub", "sbb", "cmp", "and", "or", "xor", "test"):
            return self.alu2(i, mn)
        if mn in ("inc", "dec", "neg", "not"):
            return self.alu1(i, mn)
        if mn in ("shl", "sal", "shr", "sar", "rol", "ror"):
            return self.shift(i, mn)
        if mn == "cdq":
            return ["c->edx = (uint32_t)((int32_t)c->eax >> 31);"]
        if mn == "cwde":
            return ["c->eax = (uint32_t)(int32_t)(int16_t)c->eax;"]
        if mn == "imul":
            return self.imul(i)
        if mn == "mul":
            if ops[0].size != 4:
                raise Unsupported("narrow mul")
            return ["{ uint64_t w = (uint64_t)c->eax * " + self.rd(i, ops[0]) + "; c->eax = (uint32_t)w; "
                    "c->edx = (uint32_t)(w >> 32); cf = of = (c->edx != 0); }"]
        if mn in ("div", "idiv"):
            if ops[0].size != 4:
                raise Unsupported("narrow div")
            d = self.rd(i, ops[0])
            if mn == "div":
                return ["{ uint32_t d = " + d + "; uint64_t n = ((uint64_t)c->edx << 32) | c->eax; "
                        f"if (!d) rc_fault(c, 0x{a:x}u, \"divide by zero\"); "
                        "c->eax = (uint32_t)(n / d); c->edx = (uint32_t)(n % d); }"]
            return ["{ int32_t d = (int32_t)" + d + "; int64_t n = (int64_t)(((uint64_t)c->edx << 32) | c->eax); "
                    f"if (!d) rc_fault(c, 0x{a:x}u, \"divide by zero\"); "
                    "c->eax = (uint32_t)(int32_t)(n / d); c->edx = (uint32_t)(int32_t)(n % d); }"]
        if mn.startswith("set") and mn[3:] in CC:
            return [self.wr(i, ops[0], f"({CC[mn[3:]]}) ? 1 : 0")]
        if mn in ("cld", "std"):
            return [f"c->df = {1 if mn == 'std' else 0};"]
        if mn in ("clc", "stc", "cmc"):
            return [{"clc": "cf = 0;", "stc": "cf = 1;", "cmc": "cf ^= 1;"}[mn]]
        if mn in ("int3", "hlt", "ud2"):
            return [f'rc_fault(c, 0x{a:x}u, "{mn}");']

        # -- control flow ---------------------------------------------
        if mn == "jmp":
            return self.jmp(i)
        if mn[0] == "j" and mn[1:] in CC:
            return [f"if ({CC[mn[1:]]}) goto L_{ops[0].imm & 0xFFFFFFFF:08x};"]
        if mn == "jecxz":
            return [f"if (c->ecx == 0) goto L_{ops[0].imm & 0xFFFFFFFF:08x};"]
        if mn in ("loop", "loope", "loopne"):
            cond = {"loop": "", "loope": " && zf", "loopne": " && !zf"}[mn]
            return [f"if (--c->ecx != 0{cond}) goto L_{ops[0].imm & 0xFFFFFFFF:08x};"]
        if mn == "call":
            push = f"c->esp -= 4; wr32(c->esp, 0x{nxt:x}u);"
            if ops[0].type == X.X86_OP_IMM:
                t = ops[0].imm & 0xFFFFFFFF
                if t in self.known:
                    return [push + f" {fname(t)}(c);"]
                return [push + f" rc_call(c, 0x{t:x}u);"]
            return ["{ uint32_t t = " + self.rd(i, ops[0]) + "; " + push + " rc_call(c, t); }"]
        if mn == "ret":
            n = 4 + (ops[0].imm if ops else 0)
            return [f"c->esp += {n}; return;"]
        raise Unsupported("mnemonic")

    def jmp(self, i) -> list[str]:
        op = i.operands[0]
        if op.type == X.X86_OP_IMM:
            t = op.imm & 0xFFFFFFFF
            if t in self.f.tail_calls:
                return [f"{fname(t)}(c); return;"]
            return [f"goto L_{t:08x};"]
        jt = self.f.jump_tables.get(i.address)
        if jt:
            out = ["switch (" + self.rd(i, op) + ") {"]
            for t in sorted(set(jt)):
                out.append(f"case 0x{t:x}u: goto L_{t:08x};")
            out.append(f'default: rc_fault(c, 0x{i.address:x}u, "jump table miss");')
            out.append("}")
            return out
        # Indirect jump that is not a table: a tail call (import thunks, `jmp eax`).
        return ["{ uint32_t t = " + self.rd(i, op) + "; rc_call(c, t); return; }"]

    def alu2(self, i, mn) -> list[str]:
        d, s = i.operands
        n = d.size
        M, SB = MASK[n], SIGN[n]
        ea = self.ea(i, d) if d.type == X.X86_OP_MEM else None
        sv = self.rd(i, s)
        if s.type == X.X86_OP_IMM:
            sv = f"0x{s.imm & ((1 << (8 * n)) - 1):x}u"
        pre = f"{{ uint32_t ea_ = {ea}; " if ea else "{ "
        ea_ref = "ea_" if ea else None
        av = self.rd(i, d, ea_ref)
        head = pre + f"uint32_t a = {av}; uint32_t b = {sv}; uint32_t r; "
        if mn in ("add", "adc"):
            carry = " + cf" if mn == "adc" else ""
            body = (f"uint64_t w = (uint64_t)a + b{carry}; r = (uint32_t)w & {M}; cf = (uint32_t)(w >> {BITS[n]}) & 1; "
                    f"of = ((~(a ^ b)) & (a ^ r) & {SB}) != 0; ")
        elif mn in ("sub", "cmp", "sbb"):
            borrow = " - cf" if mn == "sbb" else ""
            cfx = "((uint64_t)a < (uint64_t)b + cf)" if mn == "sbb" else "(a < b)"
            body = (f"r = (uint32_t)(a - b{borrow}) & {M}; uint32_t nc = {cfx}; "
                    f"of = ((a ^ b) & (a ^ r) & {SB}) != 0; cf = nc; ")
        else:
            opch = {"and": "&", "test": "&", "or": "|", "xor": "^"}[mn]
            body = f"r = a {opch} b; cf = 0; of = 0; "
        body += self.szp(n) + " "
        wb = "" if mn in ("cmp", "test") else self.wr(i, d, "r", ea_ref)
        return [head + body + wb + " }"]

    def alu1(self, i, mn) -> list[str]:
        d = i.operands[0]
        n = d.size
        M, SB = MASK[n], SIGN[n]
        ea = self.ea(i, d) if d.type == X.X86_OP_MEM else None
        pre = f"{{ uint32_t ea_ = {ea}; " if ea else "{ "
        ea_ref = "ea_" if ea else None
        head = pre + f"uint32_t a = {self.rd(i, d, ea_ref)}; uint32_t r; "
        if mn == "inc":
            body = f"r = (a + 1) & {M}; of = (r == {SB}); " + self.szp(n)
        elif mn == "dec":
            body = f"r = (a - 1) & {M}; of = (a == {SB}); " + self.szp(n)
        elif mn == "neg":
            body = f"r = (0u - a) & {M}; cf = (a != 0); of = (a == {SB}); " + self.szp(n)
        else:
            body = f"r = (~a) & {M};"
        return [head + body + " " + self.wr(i, d, "r", ea_ref) + " }"]

    def shift(self, i, mn) -> list[str]:
        d = i.operands[0]
        n = d.size
        N = BITS[n]
        M, SB = MASK[n], SIGN[n]
        if len(i.operands) == 1:
            cnt = "1u"
        else:
            cnt = self.rd(i, i.operands[1])
        ea = self.ea(i, d) if d.type == X.X86_OP_MEM else None
        pre = f"{{ uint32_t ea_ = {ea}; " if ea else "{ "
        ea_ref = "ea_" if ea else None
        head = pre + f"uint32_t a = {self.rd(i, d, ea_ref)}; uint32_t k = ({cnt}) & 31u; if (k) {{ uint32_t r; "
        if mn in ("shl", "sal"):
            body = (f"r = (uint32_t)((uint64_t)a << k) & {M}; cf = (uint32_t)(((uint64_t)a << k) >> {N}) & 1; "
                    f"of = ((r & {SB}) != 0) ^ cf; " + self.szp(n))
        elif mn == "shr":
            body = f"r = (uint32_t)((uint64_t)a >> k); cf = (uint32_t)((uint64_t)a >> (k - 1)) & 1; of = (a & {SB}) != 0; " + self.szp(n)
        elif mn == "sar":
            body = (f"int64_t sa = ({STYPE[n]})a; r = (uint32_t)(sa >> k) & {M}; cf = (uint32_t)(sa >> (k - 1)) & 1; of = 0; "
                    + self.szp(n))
        else:
            if n != 4:
                raise Unsupported("narrow rotate")
            if mn == "rol":
                body = "r = (a << k) | (a >> ((32 - k) & 31)); cf = r & 1; of = ((r >> 31) ^ cf) & 1;"
            else:
                body = "r = (a >> k) | (a << ((32 - k) & 31)); cf = r >> 31; of = ((r >> 31) ^ (r >> 30)) & 1;"
        return [head + body + " " + self.wr(i, d, "r", ea_ref) + " } }"]

    def imul(self, i) -> list[str]:
        ops = i.operands
        if len(ops) == 1:
            if ops[0].size != 4:
                raise Unsupported("narrow imul")
            return ["{ int64_t w = (int64_t)(int32_t)c->eax * (int32_t)" + self.rd(i, ops[0]) + "; "
                    "c->eax = (uint32_t)w; c->edx = (uint32_t)((uint64_t)w >> 32); cf = of = (w != (int32_t)w); }"]
        if ops[0].size != 4:
            raise Unsupported("narrow imul")
        if len(ops) == 2:
            x, y = self.rd(i, ops[0]), self.rd(i, ops[1])
        else:
            x = self.rd(i, ops[1])
            y = f"0x{ops[2].imm & 0xFFFFFFFF:x}u"
        return ["{ int64_t w = (int64_t)(int32_t)" + x + " * (int32_t)" + y + "; cf = of = (w != (int32_t)w); "
                + self.wr(i, ops[0], "(uint32_t)w") + " }"]

    def string_op(self, i, prefix, mn) -> list[str]:
        sz = {"b": 1, "w": 2, "d": 4}[mn[-1]]
        base = mn[:-1]
        B = BITS[sz]
        step = f"(c->df ? -{sz} : {sz})"
        if base == "stos":
            one = f"wr{B}(c->edi, ({UTYPE[sz]})c->eax); c->edi += {step};"
        elif base == "movs":
            one = f"wr{B}(c->edi, rd{B}(c->esi)); c->esi += {step}; c->edi += {step};"
        elif base == "lods":
            one = self.reg_write({1: "al", 2: "ax", 4: "eax"}[sz], f"rd{B}(c->esi)") + f" c->esi += {step};"
        elif base == "scas":
            one = (f"{{ uint32_t a = c->eax & {MASK[sz]}; uint32_t b = rd{B}(c->edi); uint32_t r = (a - b) & {MASK[sz]}; "
                   f"cf = a < b; of = ((a ^ b) & (a ^ r) & {SIGN[sz]}) != 0; {self.szp(sz)} }} c->edi += {step};")
        elif base == "cmps":
            one = (f"{{ uint32_t a = rd{B}(c->esi); uint32_t b = rd{B}(c->edi); uint32_t r = (a - b) & {MASK[sz]}; "
                   f"cf = a < b; of = ((a ^ b) & (a ^ r) & {SIGN[sz]}) != 0; {self.szp(sz)} }} c->esi += {step}; c->edi += {step};")
        else:
            raise Unsupported("string op")
        if not prefix:
            return [one]
        if base in ("stos", "movs", "lods"):
            return [f"while (c->ecx) {{ {one} c->ecx--; }}"]
        stop = "if (!zf) break;" if prefix in ("repe", "repz", "rep") else "if (zf) break;"
        return [f"while (c->ecx) {{ {one} c->ecx--; {stop} }}"]
