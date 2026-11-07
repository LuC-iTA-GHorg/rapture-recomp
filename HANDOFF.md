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

## Flounder (branch `flounder`, 2026-11-07)

**Disney's Flounder Quest is playable**, built by the public `flounder-quest-rapture-port`
(`build.bat`, submodule `recomp/` = this branch). Config: `configs/flounder.toml`
(`Quest.exe` + `quest.dll` + moses + Aaron, same guest bases as MuShoot; `__sbh_alloc_block`
of `quest.dll` found by a relocation-masked byte match of MuShoot's; the exe, moses and Aaron
addresses are the same as in MuShoot). Recompiled: 3,993 functions, ~137 k instructions,
0 unsupported instructions; no game-specific code anywhere else.

Verified unattended (`--autoplay`, `--fast 4`, settings in a scratch .ini):
intro movies, title, Sebastian's spoken intro, New game dialog (keyboard and mouse), level 1
(tiles, scrolling, HUD, timer, jellyfish, clam, swimming in every direction), the Options menu
(Esc), the `FKeys` cheats (F3 +1 life, F4 +30 s), rescuing Flounder (+1000, "level complete",
time bonus 500; no no-damage bonus since a jellyfish hit Sebastian: matches the hand port's
formula), level 2, lives lost to timeouts, game over, initials entry, high-score table (saved
to the .ini: 1500, level 2, easy), "Play again?". The game-over path with score 0 gives the
same screens as the hand port (`flounder-quest-rapture-dev`, its `gameover.txt` script).
Not verified: music/sound (dummy audio), save/restore, long play (eels, sharks, chests,
breakable blocks), levels above 2, the visible window.

Changes to common code (generic, MuShoot rebuilt: `test_mushoot_crt` 3275/0, difftest
601/601 agree, autoplay boot to the New game dialog):
* `win32/window.cpp`: autoplay commands `keydown NAME` / `keyup NAME` (hold a key: games that
  steer with `GetKeyState`) and the key name `alt`.

## Rules
* Nothing derived from the games in git: check `git ls-files` and grep before every push.
* Repository is **private** until the user reviews it. No LICENSE yet (user's decision).
* Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Flying (branch `flying`, Disney's Flying Frenzy)

**Playable**, built by the public `flying-frenzy-rapture-port` (`build.bat`, submodule
`recomp/` = this branch) from the user's own files (`Flying.exe`, `Frenzy.dll`, `moses.dll`,
`Aaron.dll`; moses/Aaron are the same files as MuShoot's, the exe has the same code layout).

* Config: `configs/flying.toml` (same guest bases as MuShoot; `__sbh_alloc_block` of
  `Frenzy.dll` at 0x1001eced, of `Flying.exe` at the same address as `Shoot.exe`'s).
  Recompiled: 682 + 1153 + 712 + 1353 functions, 0 unsupported instructions.
* Autoplay script `tests/autoplay/flying_full.txt` (`--fast 4`, about 3 minutes):
  intro movies, title, New game -> OK, a level with the hero idle until game over, initials,
  high-score table (saved to the .ini), "Play again?" -> No, Phil, credits.
* Compared with the hand port (`gameover` autoplay of `flying-frenzy-rapture-dev`): same
  screens, same score/level/difficulty row in the table; name entry and "Play again?"
  screens identical within 16 levels per channel.

**Changes to common code** (generic, MuShoot rebuilt: `test_mushoot_crt` 3275 checks, 0
failures; MuShoot autoplay to the New game dialog and into a level):
* `recomp/cfg.py`: a direct call / tail jump target of an accepted function is always an
  entry, even when it lies inside an already accepted body. Before, a function that falls
  through `call _exit` (no return) into the next function hid that next function from the
  candidate pass, and the direct call to it went through `rc_call` and faulted
  (`Frenzy.dll`'s `_controlfp`, called by the CRT start-up). Adds 2 functions to the exe and
  2 to Aaron.dll for MuShoot too (duplicated code, harmless).

**Not verified**: music (MIDI), visible window, 2 players, joystick, the Options menu and its
dialogs, Save/Restore, levels beyond 2, the "Yes" path of "Play again?".
