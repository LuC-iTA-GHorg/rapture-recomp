# HANDOFF

Updated: 2026-11-07 (first session).

## State

* Architecture chosen: **B**, recompile exe + scene DLL + moses.dll + Aaron.dll into one
  program over a flat guest address space, emulate Win32 on top of `rapture-engine`'s
  platform layer. Rationale in `docs/DESIGN.md` (the feasibility survey that backs it is a
  private working note outside this repo).
* `recomp/` (Python, `pefile` + `capstone`):
  * `pe.py`: loads PE32, static rebase with base relocations, imports/exports.
  * `cfg.py`: seeds (entry, exports, relocated code pointers), recursive descent, jump
    tables (unused leading slots, negative indexes), tail calls.
  * `x86c.py`: x86 -> C for the integer subset (mov/alu/shift/mul/div/setcc/string ops/
    control flow), function-local flags, guest stack in guest memory, `fs:` via fake TIB.
  * `__main__.py`: driver; writes `<module>.bin`, `funcs.h`, `code_NNN.c`, `dispatch.c`,
    `report.txt` to the build directory. Cross-guest imports resolved statically, system
    imports get thunk addresses 0xFFF00000 + 16n.
* `runtime/`: guest memory (4 GiB reserve), stack, image loading, `rc_call` dispatch
  (binary search), `rc_invoke`, faults.
* `configs/mushoot.toml`: bases exe 0x400000, Shoot.dll 0x10000000, moses 0x10100000,
  Aaron 0x10200000.
* MuShoot numbers: shoot.exe 53 functions (no relocs: discovery incomplete), shoot.dll
  1202, moses.dll 736, aaron.dll 1371; ~116 k instructions; 219 system imports; 578
  "unsupported" lines in the report, mostly data decoded as code from bogus seeds
  (`pop es`, `in`, `das`...) plus the real x87 subset (fld/fstp/fnstsw/sahf/fldcw...).
  All generated C compiles (gcc -O1, ~8 s with Ninja).
* `tests/test_mushoot_crt.c`: 3194 checks pass: Shoot.dll's strlen, strcpy, strcat,
  two memcpy/memmove variants (jump tables, overlap both directions), memset, and the
  code-page -> LCID lookup, compared against host libc; also checks callee-saved registers.

Build:
```
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DGAME_DIR="D:/Desktop/GAME SHOTS/zzz-ZUGHI/MUSHOOT"
cmake --build build && build\test_mushoot_crt build\gen
```
(`pefile` is in the user site-packages: do not run Python with `-I`.)

## Decisions

* Recompiler in Python (fastest to write; the build needs Python anyway). A C decoder
  (Zydis) can replace capstone later if dropping the pip dependency matters.
* No 32-bit toolchain on the machine: 64-bit host, guest = `rc_mem + addr`.
* Flags eager but local: relies on gcc dead-store elimination. Revisit only if code size
  or compile time becomes a problem.
* Static rebasing at recompile time, so no runtime relocation.

## Next steps (in order)

1. **Discovery quality**: drop seeds that decode to garbage (privileged/segment
   instructions, `in/out`, `das`, `aam`...), compare function counts with the private
   Ghidra lists (counts only), add the exe's vtable scan (`mov [reg], imm` with imm in
   `.rdata` -> slots in `.text`).
2. **x87 subset** (`fld/fst/fstp/fild/fistp/fadd/fsub/fmul/fdiv/fcomp/fnstsw/sahf/fldcw/
   fchs/fabs/fxch`): an 8-entry `double` stack in `CPU`, control/status word.
3. **Direct import calls**: `call [IAT]` where the slot is a guest export -> direct C call
   (now goes through `rc_call`).
4. **Differential testing with Unicorn** (`pip install unicorn`): run the same guest function
   in Unicorn and in the recompiled code on random inputs, compare registers + memory.
   This scales tests beyond the CRT without hand-written oracles.
5. **Win32 layer** (C, in `runtime/win32/`), order: KERNEL32 heap/env/locale for the CRT
   startup (`_DllMainCRTStartup` of each module) -> USER32 window + message loop -> GDI
   DIB sections + BitBlt -> WINMM mmio/time -> registry/ini -> DirectSound/MIDI (or HLE of
   moses' audio engines) -> joystick -> AVI -> About dialog. Back ends from
   `rapture-engine/platform` (clone it into `_recomp/` on a `recomp` branch if changes are
   needed; never push its master).
6. Boot `Shoot.exe` -> title -> `Shoot.dll`; then compare with the hand port via autoplay.
7. `build.bat` in the public `mushoot-rapture-port` calling this recompiler.

## Rules

* Nothing derived from the games in git: check `git ls-files` before every push.
* Repository is **private** until the user reviews it.
* Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
