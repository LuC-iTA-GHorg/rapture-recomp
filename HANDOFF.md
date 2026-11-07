# HANDOFF

Updated: 2026-11-07 (third session: MuShoot playable from the public port).

## State

**MuShoot is playable**, built by the public `mushoot-rapture-port` (`build.bat`, submodule
`recomp/` = this repository) from the user's own game files. Verified unattended (`--autoplay`,
hidden window, SDL dummy audio): intro movies (DILogo/DDLogo), title, Mushu's spoken intro,
New game dialog, levels with Huns/riders/hawks, firing, level-complete bonus, Options menu
and every sub-dialog (Audio, High scores, Credits with fireworks, Save), game over, initials
entry, high-score table (saved to the .ini), "Play again?". A clean clone of the port builds
and runs.

### Recompiler (`recomp/`, Python + pefile + capstone)
* `pe.py` PE32 loader with static rebasing; `cfg.py` function discovery (trusted seeds vs
  candidates, plausibility filter, jump tables incl. negative indexes, exe without relocs);
  `x86c.py` x86 -> C (integer set, x87 as 80-bit `long double` with precision control,
  string ops, flags as locals, guest stack, `fs:` TEB); `__main__.py` driver.
* Calls through the IAT are direct (guest export or `rc_call_import(idx)`).
* Config (`configs/<game>.toml`, addresses of the original files only): module list and guest
  bases, title, settings file, `[replace.<module>]` function replacements, difftest known list.
* MuShoot: ~3,900 functions, ~133 k instructions, **0 unsupported instructions**.

### Runtime (`runtime/`)
4 GiB guest space = one pagefile-backed **section** (so GDI can draw into guest memory),
dispatch by binary search, host thunks for host functions (COM, GetProcAddress), crash handler
(guest registers, ebp chain), optional call trace (`-DRECOMP_CALLTRACE=ON`, `RC_BREAK=addr,...`),
`RC_DUMP=addr:len`, `rc_replace_return0_cdecl`.

### Win32 layer (`win32/`) — see docs/DESIGN.md §3
kernel32 (heap with free quarantine, virtual memory, threads with own CPU/stack/TEB, never-reused
kernel handles, GetVersion = XP), user32 (virtual windows, message queue from SDL, capture with
WM_CAPTURECHANGED, painting, cursors from guest resources, wsprintf), gdi32 (forwarded; DIB
sections in guest memory), winmm (mmio/midiOut/joy forwarded, timeGetTime = engine clock, no
mixer/CD), advapi32 (registry in %APPDATA%\<settings>.ini, RegKey format), version (none),
ddraw (mode change no-op), dsound.c (DirectSound on SDL audio), movie.cpp (MCIWnd on
platform::Movie), window.cpp (platform::Window + `--autoplay` scripts, PNG shots),
module.c (images, DllMain order, LoadLibrary/GetProcAddress, resources).

### Tests
* `test_mushoot_crt` (3275 checks: CRT string/memory functions, x87 helpers) — `RECOMP_BUILD_TESTS`.
* `tools/difftest.py` + `difftest`: every self-contained function vs Unicorn on random states:
  all agree except 6 known x87-flag helpers.

## Ruckus (branch `ruckus`)

**Disney's Rooftop Ruckus is playable**, built by the public `rooftop-ruckus-rapture-port`
(`build.bat`, submodule `recomp/` = this branch) from the user's own game files.

* Game-specific files only: `configs/ruckus.toml` (bases as MuShoot; `__sbh_alloc_block`
  replaced in every module: exe, moses.dll and Aaron.dll at MuShoot's addresses, since the
  exe's code is identical and moses/Aaron are the same files; Ruckus.dll at its own address,
  found by a relocation-masked byte match of MuShoot's function) and
  `tests/autoplay/ruckus_full.txt` (unattended run, coordinates only).
* Recompiler output: exe 680, Ruckus.dll 1133, moses 712, Aaron 1351 functions;
  **0 unsupported instructions**.
* **Changes to common code (`recomp/`, `runtime/`, `win32/`, CMakeLists.txt): none.**
  MuShoot rebuilt from this branch: builds, `test_mushoot_crt` passes.
* Verified with `--autoplay tests/autoplay/ruckus_full.txt --fast 4` and compared with the
  hand port (`gameover.txt` of the dev repository, same screens): intro movies (DILogo,
  DDLogo), title, spoken intro, New game dialog, level 1 with Abu, Jafar, enemies and items,
  losing lives, game over, initials entry, high-score table (saved to the .ini), "Play again?",
  No -> Iago -> credits with fireworks.
* Not verified: music (MIDI), visible window, joystick/mouse play, Options menu and its
  sub-dialogs, Save/Restore, levels after the first, two-player mode. WaveVol/MidiVol are
  saved as 0 (mixer not emulated, as for MuShoot; the hand port writes 100).
* Script timing: waits are game time, movies and speech real time, so the script clicks the
  New game OK repeatedly instead of waiting for the dialog.
* Build: `-DGAME_CONFIG=configs/ruckus.toml -DGAME_EXE=ruckus -DRECOMP_BUILD_TESTS=OFF`
  (the tests are MuShoot-specific).

## How to work on it
```
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ ^
      -DGAME_DIR="D:/Desktop/GAME SHOTS/zzz-ZUGHI/MUSHOOT" -DGAME_EXE=mushoot ^
      -DSDL2_DIR=E:/RE_Tools/SDL2/cmake -DRAPTURE_ENGINE_DIR=../rapture-engine
cmake --build build
build\mushoot.exe --autoplay script.txt --outdir OUT [--fast 4] [--settings OUT\s.ini] [--log|--log2]
```
* Never start it with a visible window without telling the user first; use `--autoplay`.
  Always pass `--settings` (else the real %APPDATA% file is used) and an absolute or relative
  `--outdir` (resolved before the program enters the game folder).
* Script commands: `wait ms`, `move/click/rclick/down/up x y`, `key name`, `shot name`, `quit`.
  With `--fast N` game time runs N times faster but audio (speech, movies) stays real time.
* Debugging a crash: rebuild with `-DRECOMP_CALLTRACE=ON`; the fault report shows the ebp chain;
  `RC_BREAK=<guest addr>` prints the call stack whenever that function is entered. A local,
  private script maps guest addresses to the Ghidra function names (not in this repo).
* `pefile`/`capstone`/`unicorn` are in the user site-packages: do not run Python with `-I`.

## Decisions (this session)
* Win32: forward what is pure or memory-based, emulate pointer/presentation-bound APIs.
* Quarantine + GetVersion XP + `__sbh_alloc_block` replaced: the games read freed objects
  right after deleting them (Aaron's scene message queues in `Window::LoadScene`).
* No change was needed in rapture-engine (only `platform/` is used: Window, Movie, Surface), so
  nothing was merged there and the engine pointers of the dev/port repositories are unchanged.

## Open points / next steps
1. **Not verified**: music (midiOut to the Windows synthesizer: silent in autoplay), the visible
   window (scaling, fullscreen, cursor shapes), joysticks, the effect of the Audio dialog
   toggles, the "No" path of "Play again?" (title, credits) and a full Save/Restore cycle.
2. Mixer API is not emulated (WaveVol/MidiVol are saved as 0; no effect on the sound).
3. The About box (DialogBoxParamA) is not shown.
4. Gameplay pacing vs. the hand port: in our runs the game reached level 5-6 before game over,
   the hand port's run ended at level 3; Hun targets are random (seeded by time), so compare
   several runs (or seed both identically) before calling it a bug.
5. The other three games: add `configs/<game>.toml` (bases, title, settings, the
   `__sbh_alloc_block` address of each module: same moses/Aaron/exe addresses), then a port.
6. Build speed: rapture-engine's whole library is compiled although only `platform/` is used.

## Rules
* Nothing derived from the games in git: check `git ls-files` and grep before every push.
* Repository is **private** until the user reviews it. No LICENSE yet (user's decision).
* Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
