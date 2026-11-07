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


THUNK_BASE = 0xFFF00000


class Unsupported(Exception):
    pass


def fname(addr: int) -> str:
    return f"f_{addr:08x}"


class Translator:
    def __init__(self, func: Function, known: set[int], iat: dict[int, int] | None = None):
        self.iat = iat or {}         # IAT slot -> resolved target (guest export or thunk)
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

        if mn.startswith("lock "):
            mn = mn[5:]   # single-threaded guest: the lock prefix changes nothing
        if mn[0] == "f" or mn == "wait":
            return self.x87(i, mn)
        if mn == "xlatb":
            return [self.reg_write("al", "rd8(c->ebx + (uint8_t)c->eax)")]

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
        if mn == "sahf":
            return ["{ uint32_t h = c->eax >> 8; cf = h & 1; pf = (h >> 2) & 1; zf = (h >> 6) & 1; sf = (h >> 7) & 1; }"]
        if mn == "lahf":
            return [self.reg_write("ah", "(sf << 7) | (zf << 6) | (pf << 2) | 2u | cf")]
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
            direct = self.iat_call(ops[0])
            if direct:
                return [push + " " + direct]
            return ["{ uint32_t t = " + self.rd(i, ops[0]) + "; " + push + " rc_call(c, t); }"]
        if mn == "ret":
            n = 4 + (ops[0].imm if ops else 0)
            return [f"c->esp += {n}; return;"]
        raise Unsupported("mnemonic")

    def iat_call(self, op) -> str | None:
        """`call/jmp dword ptr [slot]` through an IAT slot resolved at recompile time."""
        if op.type != X.X86_OP_MEM or op.mem.base or op.mem.index or op.mem.segment:
            return None
        t = self.iat.get(op.mem.disp & 0xFFFFFFFF)
        if t is None:
            return None
        if t in self.known:
            return f"{fname(t)}(c);"
        if t >= THUNK_BASE:
            return f"rc_call_import(c, {(t - THUNK_BASE) // 16}u);"
        return None

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
        direct = self.iat_call(op)
        if direct:
            return [direct + " return;"]
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
            kk = f"(k % {N}u)"
            if mn == "rol":
                body = (f"uint32_t q = {kk}; r = q ? ((a << q) | (a >> ({N}u - q))) & {M} : a; cf = r & 1; "
                        f"of = ((r >> {N - 1}) ^ cf) & 1;")
            else:
                body = (f"uint32_t q = {kk}; r = q ? ((a >> q) | (a << ({N}u - q))) & {M} : a; cf = (r >> {N - 1}) & 1; "
                        f"of = ((r >> {N - 1}) ^ (r >> {N - 2})) & 1;")
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

    # -- x87 -------------------------------------------------------------
    @staticmethod
    def _sti(i) -> int | None:
        """Index i of st(i) for a register-form x87 instruction, from the ModRM
        byte. capstone 5 reports wrong x87 register operands (`fxch st(1)` has
        operand st(0)), so the operand list is never used for st(i)."""
        b = i.bytes
        if len(b) >= 2 and 0xD8 <= b[-2] <= 0xDF and b[-1] >= 0xC0:
            return b[-1] & 7
        return None

    def _st(self, i, op) -> int:
        k = self._sti(i)
        if k is None:
            raise Unsupported("x87 register operand without ModRM")
        return k

    def _fmem_rd(self, i, op, integer: bool) -> str:
        e = self.ea(i, op)
        if integer:
            t = {2: "(int16_t)rd16", 4: "(int32_t)rd32", 8: "(int64_t)rd64"}.get(op.size)
            if not t:
                raise Unsupported("x87 integer size")
            return f"(f80){t}({e})"
        if op.size == 4:
            return f"(f80)rdf32({e})"
        if op.size == 8:
            return f"(f80)rdf64({e})"
        if op.size == 10:
            return f"rdf80({e})"
        raise Unsupported("x87 memory operand size")

    def x87(self, i, mn) -> list[str]:
        ops = i.operands
        if mn in ("wait", "fwait", "fnop", "fninit", "finit"):
            return [] if mn not in ("fninit", "finit") else ["c->cw = 0x37F; c->sw = 0; c->top = 0;"]
        if mn == "fnclex":
            return ["c->sw &= 0x7F00u;"]
        if mn == "fldcw":
            return [f"c->cw = rd16({self.ea(i, ops[0])});"]
        if mn == "fnstcw":
            return [f"wr16({self.ea(i, ops[0])}, c->cw);"]
        if mn == "fnstsw":
            if ops[0].type == X.X86_OP_REG:
                return [self.reg_write("ax", "fstsw(c)")]
            return [f"wr16({self.ea(i, ops[0])}, fstsw(c));"]
        consts = {"fldz": "0.0L", "fld1": "1.0L", "fldpi": "3.14159265358979323846264338327950288L",
                  "fldl2e": "1.44269504088896340735992468100189214L", "fldl2t": "3.32192809488736234787031942948939018L",
                  "fldlg2": "0.301029995663981195213738894724493027L", "fldln2": "0.693147180559945309417232121458176568L"}
        if mn in consts:
            return [f"fpush(c, {consts[mn]});"]
        if mn in ("fld", "fild"):
            op = ops[0]
            if op.type == X.X86_OP_REG:
                return [f"{{ f80 v = ST({self._st(i, op)}); fpush(c, v); }}"]
            return [f"fpush(c, {self._fmem_rd(i, op, mn == 'fild')});"]
        if mn in ("fst", "fstp"):
            op = ops[0]
            pop = " fpop(c);" if mn == "fstp" else ""
            if op.type == X.X86_OP_REG:
                return [f"ST({self._st(i, op)}) = ST(0);{pop}"]
            e = self.ea(i, op)
            if op.size == 4:
                return [f"wrf32({e}, (float)ST(0));{pop}"]
            if op.size == 8:
                return [f"wrf64({e}, (double)ST(0));{pop}"]
            if op.size == 10:
                return [f"wrf80({e}, ST(0));{pop}"]
            raise Unsupported("x87 store size")
        if mn in ("fist", "fistp"):
            op = ops[0]
            pop = " fpop(c);" if mn == "fistp" else ""
            b = op.size * 8
            w = {16: "wr16", 32: "wr32", 64: "wr64"}[b]
            return [f"{w}({self.ea(i, op)}, ({UTYPE.get(op.size, 'uint64_t')})rc_fist(c, ST(0), {b}));{pop}"]
        if mn == "fxch":
            k = self._st(i, None)
            return [f"{{ f80 t = ST(0); ST(0) = ST({k}); ST({k}) = t; }}"]
        if mn in ("fchs", "fabs", "fsqrt", "frndint"):
            f = {"fchs": "-ST(0)", "fabs": "fabsl(ST(0))", "fsqrt": "FPREC(sqrtl(ST(0)))", "frndint": "rc_fround(c, ST(0))"}[mn]
            return [f"ST(0) = {f};"]
        if mn == "fxam":
            return ["rc_fxam(c);"]
        if mn == "ftst":
            return ["fcmp(c, ST(0), 0.0);"]
        if mn == "fscale":
            return ["ST(0) = ldexpl(ST(0), (int)truncl(ST(1)));"]
        if mn == "fpatan":
            return ["ST(1) = atan2l(ST(1), ST(0)); fpop(c);"]
        if mn in ("fsin", "fcos"):
            return [f"ST(0) = {mn[1:]}l(ST(0));"]
        if mn in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp", "ficom", "ficomp"):
            npop = 2 if mn.endswith("pp") else 1 if mn.endswith("p") else 0
            if ops and ops[0].type == X.X86_OP_MEM:
                b = self._fmem_rd(i, ops[0], mn.startswith("fi"))
            else:
                b = f"ST({self._st(i, None)})"
            return [f"fcmp(c, ST(0), {b});" + " fpop(c);" * npop]
        base = mn.rstrip("p") if mn not in ("fsubp", "fsubrp", "fdivp", "fdivrp", "faddp", "fmulp") else mn[:-1]
        if base.startswith("fi") and base[2:] in ("add", "sub", "subr", "mul", "div", "divr"):
            integer, opname = True, base[2:]
        elif base[1:] in ("add", "sub", "subr", "mul", "div", "divr"):
            integer, opname = False, base[1:]
        else:
            raise Unsupported("x87 mnemonic")
        pop = mn.endswith("p") and not integer
        expr = {"add": "{d} + {s}", "sub": "{d} - {s}", "subr": "{s} - {d}",
                "mul": "{d} * {s}", "div": "{d} / {s}", "divr": "{s} / {d}"}[opname]
        if len(ops) == 1 and ops[0].type == X.X86_OP_MEM:
            s_ = self._fmem_rd(i, ops[0], integer)
            return [f"ST(0) = FPREC({expr.format(d='ST(0)', s=s_)});"]
        k = self._sti(i)
        if k is None:
            raise Unsupported("x87 arithmetic form")
        # D8: st(0) = st(0) op st(i); DC/DE: st(i) = st(i) op st(0) (DE pops).
        di, si = (0, k) if i.bytes[-2] == 0xD8 else (k, 0)
        out = f"ST({di}) = FPREC({expr.format(d=f'ST({di})', s=f'ST({si})')});"
        return [out + (" fpop(c);" if pop else "")]
