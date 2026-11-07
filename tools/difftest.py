"""Differential test: recompiled code vs. Unicorn, on random states.

    python tools/difftest.py CONFIG --game-dir DIR --gen GEN_DIR --runner build/difftest.exe
                             [--cases N] [--seed S] [--max-funcs M] [--func ADDR ...]

Picks every *self-contained* guest function (its call closure has no system
import and no indirect call other than jump tables), builds random entry
states (registers, stack arguments, a scratch area full of plausible pointers
and small integers), runs each state in Unicorn, keeps the states Unicorn
completes cleanly (returns, touches only mapped memory, no CPU exception),
runs the same states in the recompiled code (tests/difftest.c) and compares
the eight registers, the scratch area, the stack window and every module image.

Needs `unicorn` (pip install unicorn) in addition to pefile and capstone.
"""
from __future__ import annotations

import argparse
import os
import random
import struct
import subprocess
import sys
import tomllib
import zlib

from capstone import x86 as X
import unicorn as U
from unicorn import x86_const as UX

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from recomp import cfg, pe  # noqa: E402

ARGS = 8
SCRATCH, SCRATCH_SIZE = 0x00200000, 0x4000
TEB = 0x00300000
GDT = 0x00310000
STACK_BASE, STACK_SIZE = 0x00100000, 0x10000
STACK_TOP = STACK_BASE + STACK_SIZE - 16
STACK_WINDOW = 0x1000
SENTINEL = 0xFFFFFFF0
THUNK_BASE = 0xFFF00000


def load_modules(conf, game_dir):
    mods = [pe.load(os.path.join(game_dir, m["file"]), int(m["base"], 0)) for m in conf["module"]]
    by_name = {m.name: m for m in mods}
    iat = {}
    for m in mods:
        for im in m.imports:
            t = by_name.get(im.dll)
            if t is None:
                iat[im.slot] = None            # system import
            else:
                iat[im.slot] = t.exports.get(im.name) if im.name else t.exports_ord.get(im.ordinal)
    return mods, iat


def self_contained(funcs, iat):
    memo: dict[int, bool] = {}

    def ok(e, stack=()):
        if e in memo:
            return memo[e]
        if e in stack:
            return True
        f = funcs.get(e)
        if f is None or f.bad:
            memo[e] = False
            return False
        res = True
        callees = set(f.calls) | set(f.tail_calls)
        for a, i in f.insns.items():
            if i.mnemonic in ("int3", "hlt", "ud2"):
                res = False
            if i.mnemonic in ("call", "jmp") and i.operands[0].type != X.X86_OP_IMM:
                op = i.operands[0]
                if a in f.jump_tables:
                    continue
                if op.type == X.X86_OP_MEM and not op.mem.base and not op.mem.index:
                    t = iat.get(op.mem.disp & 0xFFFFFFFF, None)
                    if t is not None and t in funcs:
                        callees.add(t)
                        continue
                res = False
        if res:
            for t in callees:
                if not ok(t, stack + (e,)):
                    res = False
                    break
        memo[e] = res
        return res

    return sorted(e for e in funcs if ok(e))


class Emu:
    def __init__(self, gen, mods):
        self.uc = U.Uc(U.UC_ARCH_X86, U.UC_MODE_32)
        self.images = []
        for m in mods:
            data = open(os.path.join(gen, m.name + ".bin"), "rb").read()
            size = (m.size + 0xFFF) & ~0xFFF
            self.uc.mem_map(m.base, size)
            self.images.append((m.base, data))
        for base, size in ((STACK_BASE, STACK_SIZE), (SCRATCH, SCRATCH_SIZE), (TEB, 0x1000),
                           (GDT, 0x1000), (SENTINEL & ~0xFFF, 0x1000)):
            self.uc.mem_map(base, size)
        # Flat code/data segments plus fs -> TEB, through a GDT (all DPL 0).
        gdt = bytes(8)
        gdt += self._descriptor(0, 0xFFFFF, 0x9B, gran=True)    # 0x08 code
        gdt += self._descriptor(0, 0xFFFFF, 0x93, gran=True)    # 0x10 data
        gdt += self._descriptor(TEB, 0xFFF, 0x93)               # 0x18 fs
        self.uc.mem_write(GDT, gdt)
        self.uc.reg_write(UX.UC_X86_REG_GDTR, (0, GDT, len(gdt) - 1, 0))
        self.uc.reg_write(UX.UC_X86_REG_CS, 0x08)
        for r in (UX.UC_X86_REG_SS, UX.UC_X86_REG_DS, UX.UC_X86_REG_ES, UX.UC_X86_REG_GS):
            self.uc.reg_write(r, 0x10)
        self.uc.reg_write(UX.UC_X86_REG_FS, 0x18)
        self.bad = False
        self.uc.hook_add(U.UC_HOOK_MEM_UNMAPPED, self._unmapped)

    @staticmethod
    def _descriptor(base, limit, access, gran=False):
        d = limit & 0xFFFF
        d |= (base & 0xFFFFFF) << 16
        d |= access << 40
        d |= ((limit >> 16) & 0xF) << 48
        d |= (0xC if gran else 0x4) << 52     # 32-bit segment, 4 KiB or byte granularity
        d |= ((base >> 24) & 0xFF) << 56
        return struct.pack("<Q", d)

    def _unmapped(self, uc, access, address, size, value, data):
        self.bad = True
        return False

    def run(self, func, regs, args, scratch):
        uc = self.uc
        for base, data in self.images:
            uc.mem_write(base, data)
        uc.mem_write(SCRATCH, scratch)
        uc.mem_write(STACK_BASE, b"\0" * STACK_SIZE)
        teb = bytearray(0x1000)
        struct.pack_into("<I", teb, 0, 0xFFFFFFFF)
        uc.mem_write(TEB, bytes(teb))
        sp = STACK_TOP
        for a in reversed(args):
            sp -= 4
            uc.mem_write(sp, struct.pack("<I", a))
        sp -= 4
        uc.mem_write(sp, struct.pack("<I", SENTINEL))
        names = [UX.UC_X86_REG_EAX, UX.UC_X86_REG_ECX, UX.UC_X86_REG_EDX, UX.UC_X86_REG_EBX,
                 UX.UC_X86_REG_ESP, UX.UC_X86_REG_EBP, UX.UC_X86_REG_ESI, UX.UC_X86_REG_EDI]
        for n, v in zip(names, regs):
            uc.reg_write(n, v)
        uc.reg_write(UX.UC_X86_REG_ESP, sp)
        uc.reg_write(UX.UC_X86_REG_EFLAGS, 0x202)
        uc.reg_write(UX.UC_X86_REG_FPCW, 0x027F)
        self.bad = False
        try:
            uc.emu_start(func, SENTINEL, count=2_000_000)
        except U.UcError:
            return None
        if self.bad or uc.reg_read(UX.UC_X86_REG_EIP) != SENTINEL:
            return None
        out = [uc.reg_read(n) for n in names]
        out.append(zlib.crc32(bytes(uc.mem_read(SCRATCH, SCRATCH_SIZE))))
        lo = STACK_TOP - STACK_WINDOW + 16
        out.append(zlib.crc32(bytes(uc.mem_read(lo, STACK_WINDOW))))
        for base, data in self.images:
            out.append(zlib.crc32(bytes(uc.mem_read(base, len(data)))))
        return out


def random_state(rng):
    def value():
        r = rng.random()
        if r < 0.45:
            return SCRATCH + 4 * rng.randrange(0, (SCRATCH_SIZE - 0x400) // 4)
        if r < 0.85:
            return rng.randrange(0, 17)
        return rng.getrandbits(32)

    regs = [value() for _ in range(8)]
    args = [value() for _ in range(ARGS)]
    words = [value() if rng.random() < 0.8 else 0 for _ in range(SCRATCH_SIZE // 4)]
    scratch = struct.pack(f"<{len(words)}I", *words)
    return regs, args, scratch


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("config")
    ap.add_argument("--game-dir", required=True)
    ap.add_argument("--gen", required=True)
    ap.add_argument("--runner", required=True)
    ap.add_argument("--cases", type=int, default=12, help="random states tried per function")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--max-funcs", type=int, default=0)
    ap.add_argument("--func", action="append", default=[])
    a = ap.parse_args(argv)
    with open(a.config, "rb") as fh:
        conf = tomllib.load(fh)
    mods, iat = load_modules(conf, a.game_dir)
    funcs = {}
    for m in mods:
        funcs.update(cfg.discover(m))
    if a.func:
        targets = [int(x, 16) for x in a.func]
    else:
        targets = self_contained(funcs, iat)
    rng = random.Random(a.seed)
    if a.max_funcs and len(targets) > a.max_funcs:
        targets = sorted(rng.sample(targets, a.max_funcs))
    print(f"{len(targets)} self-contained functions selected")
    emu = Emu(a.gen, mods)
    cases, expect = [], []
    tested = set()
    for f in targets:
        for _ in range(a.cases):
            regs, args, scratch = random_state(rng)
            res = emu.run(f, regs, args, scratch)
            if res is None:
                continue
            cases.append(struct.pack(f"<{1 + 8 + ARGS}I", f, *regs, *args) + scratch)
            expect.append((f, res))
            tested.add(f)
    print(f"{len(cases)} clean states over {len(tested)} functions")
    cpath = os.path.join(a.gen, "difftest_cases.bin")
    rpath = os.path.join(a.gen, "difftest_results.bin")
    with open(cpath, "wb") as fh:
        fh.write(b"".join(cases))
    p = subprocess.run([os.path.abspath(a.runner), a.gen, cpath, rpath])
    got = open(rpath, "rb").read() if os.path.exists(rpath) else b""
    rec = 4 * (10 + len(mods))
    labels = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "scratch", "stack"] + [m.name for m in mods]
    bad_funcs = {}
    for k, (f, want) in enumerate(expect):
        if (k + 1) * rec > len(got):
            print(f"runner stopped at case {k} (function {f:#x}), exit code {p.returncode}")
            bad_funcs.setdefault(f, "crash")
            break
        have = struct.unpack_from(f"<{10 + len(mods)}I", got, k * rec)
        diff = [labels[j] for j in range(len(want)) if want[j] != have[j]]
        if diff and f not in bad_funcs:
            bad_funcs[f] = ", ".join(diff)
    known = {int(x, 16) for x in conf.get("difftest", {}).get("known_differences", [])}
    for f in sorted(set(bad_funcs) & known):
        print(f"known     {f:#010x}: {bad_funcs.pop(f)}")
    for f, d in sorted(bad_funcs.items()):
        print(f"MISMATCH {f:#010x}: {d}")
    print(f"{len(tested) - len(bad_funcs)} / {len(tested)} functions agree")
    return 1 if bad_funcs else 0


if __name__ == "__main__":
    sys.exit(main())
