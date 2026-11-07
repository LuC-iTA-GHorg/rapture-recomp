# HANDOFF

Updated: 2026-11-07 (second session: steps 1 and 2 done).

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
  (binary search), `rc_invoke`, faults, x87 helpers; `rc_trace` (test: env `RC_TRACE=1`)
  logs every indirect call.
* `configs/mushoot.toml`: bases exe 0x400000, Shoot.dll 0x10000000, moses 0x10100000,
  Aaron 0x10200000.
* MuShoot numbers (session 2): shoot.exe 680 functions / 41 jump tables, shoot.dll 1174 / 40,
  moses.dll 712 / 41, aaron.dll 1351 / 47; ~133 k instructions; 219 system imports; 250
  unsupported instructions, almost all x87. All generated C compiles (gcc -O1).
* Discovery check against the private Ghidra function lists (counts only, done locally):
  the remaining differences are SEH filter/handler blocks that we find and Ghidra does not
  (reached only through scope tables), dead code with no reference at all (23 in the exe,
  ~25 in Shoot.dll), and bodies reached by a tail `jmp` that we merge into the caller as a
  label (duplicated, not lost).
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

1. ~~Discovery quality~~ (done, session 2): trusted seeds (entry, exports, extras, their
   callees) vs candidates (code pointers in data, code-pointing immediates of accepted
   functions), candidates rejected if their body has instructions MSVC never emits or they
   start inside an accepted function; the exe (no relocations) scans aligned data dwords
   and bounds jump tables by distance.
2. ~~x87 subset~~ (done, session 2): 8 `double` registers + TOP in `CPU`, status word
   condition codes (fcom/fxam/ftst, fnstsw, sahf/lahf), control word (fldcw/fnstcw,
   rounding for fist/frndint), m32/m64/m80 loads and stores (80-bit converted in
   software), arithmetic in all forms, fpatan/fscale/fsqrt. Also xlatb, narrow rotates,
   `lock` prefix. **0 unsupported instructions** left in the 4 MuShoot modules.
   Tests: Aaron.dll `_ftol` (rounding mode switch, edx:eax, cw restored), `_CIsqrt` and
   `_CIatan2` (all quadrants, x = 0) pass: **3275 checks, 0 failures**.
   **capstone 5 pitfall**: its x87 register operands are wrong (`fxch st(1)` reports
   operand `st(0)`); `x86c.py` takes st(i) from the ModRM byte and the destination from
   the opcode (D8: st(0); DC/DE: st(i)). Mnemonics (fsub vs fsubr) are correct.
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
