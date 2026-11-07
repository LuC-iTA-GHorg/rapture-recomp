# rapture-recomp: design

A static recompiler for x86-32 Windows games built on the 1999 "Rapture" engine
(`moses.dll` + `Aaron.dll`, plus one scene DLL and one host exe per game). It runs
**on the user's machine, at build time**, on the user's own copy of the game, and
produces C that is compiled and linked with a small runtime. The repository holds the
recompiler, the runtime, per-game configs (file names, addresses, patches) and tests:
**never** bytes, strings or decompiled code of any game.

Windows (64-bit, MinGW-w64 GCC) is the only target platform.

## 1. Architecture: recompile the whole stack, emulate Win32 (option B)

Three options were considered:

| | A: scene DLL only, bridge to `rapture-engine` | **B: whole stack + Win32 layer** | C: 32-bit native build |
|---|---|---|---|
| translated | scene DLL (~40 k insns) | exe + scene DLL + moses + Aaron (~125 k insns; ~85 k shared by the 4 games) | as A or B |
| boundary | ~150-230 C++ engine entry points with object identity, vtables, inline fields, two-way lifetime | ~220 flat Win32 functions + a few DirectDraw/DirectSound methods | as A or B |
| fidelity | bounded by the hand port + the bridge | the original binaries | as A or B |

**Chosen: B.** Reasons:

1. **The engine boundary is a C++ ABI, not an API.** Scene classes derive from ~25 engine
   classes, construct the engine bases in place, install their own vptrs into engine
   subobjects, depend on the engine bases' sizes and subobject offsets, and are called back
   by the engine through hundreds of overridden virtual slots. `rapture-engine` is a
   faithful port *by name*, not by memory layout (64-bit, deliberate layout changes), so A
   needs a two-way object bridge for every class pairing: about as much code as the Win32
   layer of B and far harder to debug.
2. **The Win32 surface is small and flat.** Imports of all modules together: ~220 Win32
   functions (KERNEL32, USER32, GDI32, WINMM, ADVAPI32, VERSION). Rendering is GDI only
   (DIB sections + blits; DirectDraw is used only to change the display mode), file I/O
   goes through `mmio*`, there are no timers. The hard back ends already exist in
   `rapture-engine` (`platform/`: SDL2 window, audio, MIDI, AVI, joystick; software GDI,
   text and EMF helpers in `moses/`), so the Win32 layer reuses them.
3. **The machine code is friendly.** MSVC 6, static single-threaded CRT in every module,
   no MMX/SSE, x87 only in a handful of functions and in the CRT, no self-modifying code,
   C++ EH frames but no `throw`, jump tables inline in `.text` and relocated.
4. **One engine translation serves four games**: `moses.dll`, `Aaron.dll` and the host exe
   are byte-identical (the exes differ by 3 immediates) across the family.
5. C (32-bit real pointers) brings nothing: the project toolchain is 64-bit, C+A keeps the
   ABI problem, and calling the real Win32/DirectX reproduces the original's problems on
   modern Windows.

**Role of `rapture-engine` under B.** The hand-written moses/Aaron/Arcade/host code of
`rapture-engine` **no longer runs the games**: in the recompiled build the games run on the
recompiled original `moses.dll` and `Aaron.dll`. The hand port becomes the **reference**
(documentation of what each engine function does) and the **test oracle** (the same autoplay
scripts run on the hand port and on the recompiled build, and their results are compared).
What is still linked and executed is `rapture-engine`'s `platform/` layer (SDL2 window,
audio, MIDI, AVI, joystick) and its Win32-replacement helpers (software GDI, text, EMF,
ini/registry), used as back ends of the Win32 emulation. A function-replacement hook
(per-game config: "guest address -> host function") lets hot loops, the moses audio threads
or CRT routines be replaced by native code where that is simpler.

## 2. Pipeline

```
user's game folder                    build directory (never committed)
  Shoot.exe Shoot.dll           ┌──>  <module>.bin   rebased images, IAT filled
  moses.dll Aaron.dll  ──recomp─┼──>  code_NNN.c     one C function per guest function
                      (Python)  ├──>  dispatch.c     address -> function table, import table
  configs/<game>.toml ──────────┘──>  report.txt     statistics, unsupported instructions
                                             │
                    runtime/ (C) + Win32 layer + generated C ──gcc──> game executable
```

### 2.1 Loading and rebasing (`recomp/pe.py`)

All modules share one flat 32-bit guest address space. Three of them prefer base
0x10000000, so the config gives every module its own base and the loader applies the base
relocations **at recompile time**. The translator only ever sees final addresses.
Imports between guest modules are resolved statically (IAT slots receive the export's
guest address); every other import gets a thunk address (0xFFF00000 + 16·n) that the
dispatcher maps to a host implementation.

### 2.2 Function discovery (`recomp/cfg.py`)

Seeds: entry point, exports, config extras, and every relocated dword that points into
code (vtables, callbacks, `push offset func`). Each function is recovered by recursive
descent without following calls. Jump tables (`jmp [reg*4 + table]`) are read from the
relocation set, including tables whose first slots are unused and negative indexes
(`neg ecx; jmp [ecx*4 + end]`, as in the CRT's `memmove`). Direct `jmp` to another known
entry is a tail call. The exe has no relocations: its seeds come from the entry point,
`CreateScene` and the code pointers found in its data (to do: vtable scan).

### 2.3 Translation (`recomp/x86c.py`)

* One C function per guest function: `void f_XXXXXXXX(CPU *c)`. Branches inside it are
  `goto` labels; jump tables become `switch`.
* Registers live in the `CPU` struct; flags are **function-local** C variables computed
  eagerly per instruction. MSVC code never keeps flags live across a call or a return, so
  the C compiler deletes every flag computation no branch reads.
* The guest stack is real guest memory. `call` pushes the guest return address and calls
  the C function; `ret n` pops it and returns. Arguments, locals, `this` adjustments and
  SEH frames therefore behave exactly as in the original.
* Indirect calls/jumps go through `rc_call()`, a binary search in the generated table of
  function entries, falling back to import thunks.
* `fs:` accesses use `c->fs_base` (a fake TIB; the SEH chain is never walked, since no
  exception is ever thrown).
* Unsupported instructions become `rc_unimpl()` calls: the build never stops, the report
  lists them, and they abort with the address only if executed.

### 2.4 Runtime (`runtime/`)

* `rc_mem`: 4 GiB reserved with `VirtualAlloc`, guest address = offset. Accessors use
  `memcpy`, so unaligned accesses are fine.
* Stack, fake TIB, scratch memory, image loading from the generated `.bin` files.
* `rc_invoke()` calls a guest function with arguments (used by tests and by host code
  calling back into the guest: WndProcs, thread entry points, callbacks).

## 3. The Win32 layer (`win32/`)

Windows is the only target, so the layer **forwards to the host Windows** whatever only
computes or works on memory, and **emulates** whatever depends on 32-bit pointers, on the
process layout or on the presentation:

| forwarded to the host | emulated |
|---|---|
| strings and locale (`MultiByteToWideChar`, `LCMapString`, `lstr*`, `GetStringType`...) | guest heap (`HeapAlloc`...) and virtual memory, in guest memory |
| GDI drawing: DCs, pens, brushes, fonts, palettes, blits, EMF, text | DIB sections: pixels in guest memory (below) |
| `mmio*` (the `.rrs` archives), `midiOut*` (the host MIDI synthesizer), `joy*` | modules, `LoadLibrary`/`GetProcAddress`, resources (read from the guest images) |
| files (`FindFirstFile`, `GetFullPathName`...), time and date formatting | threads (one guest CPU, stack and TEB per host thread), kernel handles |
| | windows, message queue and input (SDL2 window of rapture-engine) |
| | DirectSound (SDL2 audio mixer), DirectDraw (no-op mode change), MCIWnd (`platform::Movie`) |
| | registry (an `.ini` file, same format as rapture-engine's `RegKey`) |

* **Guest memory is a section.** The 4 GiB guest space is a pagefile-backed section mapped
  once; `CreateDIBSection` is called with that section and `offset = guest address`, so
  GDI draws straight into guest memory and the guest's own pixel code (moses blits RLE
  sprites itself) works on the same bytes. Every GDI call is followed by `GdiFlush`.
* **Handles.** GDI and USER handles are 32-bit significant on 64-bit Windows and pass
  through; kernel handles get never-reused guest numbers (the games wait on and close
  handles of objects they already closed); pointer-sized handles (`HMMIO`, `HMIDIOUT`) go
  through tables.
* **The screen** is a host memory DC on a 640x480 32-bit DIB; drawing on it marks it dirty
  and the message pump copies it to `platform::Window`'s framebuffer (scaled, F11 / Alt+Enter
  fullscreen).
* **Faithful to Windows, not to the hand port.** Where the games depend on undocumented
  Windows behaviour the layer reproduces it: freed heap blocks stay intact for a while
  (quarantine), closed handles fail, `GetVersion` reports Windows XP. One function of each
  module's static CRT is replaced through the config (`[replace.<module>]`): the VC6
  small-block allocator, so that every allocation reaches that heap (Aaron dispatches the
  message queue of a scene it has just deleted).
* **Unattended runs**: `--autoplay SCRIPT` (hidden window, scripted input, PNG shots, SDL
  dummy audio), `W32_PCMDUMP` (audio mix to a file), `--log`, and in the runtime a crash
  report with the guest registers and the `ebp` chain, an optional call trace
  (`-DRECOMP_CALLTRACE=ON`, `RC_BREAK=addr`) and `RC_DUMP=addr:len`.

## 4. Legal boundary

* The repository: recompiler, runtime, Win32 layer, tests, configs with addresses,
  names and patches. Tests check recompiled code against known semantics (host libc,
  documented Win32 behaviour) and refer to guest functions only by address.
* Everything derived from the game (rebased images, generated C, objects, executables) is
  produced in the build directory on the user's machine and ignored by git.

## 5. Status and next steps

See `HANDOFF.md`.
