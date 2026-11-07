# HANDOFF

Updated: 2026-11-07 (all four games playable; guest thread stacks reused).

## State

All four games are playable, each built by its public port (`build.bat`, submodule
`recomp/` = this repository) from the user's own game files: `mushoot-rapture-port`,
`flying-frenzy-rapture-port`, `flounder-quest-rapture-port`, `rooftop-ruckus-rapture-port`.
Game-specific files: `configs/<game>.toml` (addresses only) and `tests/autoplay/<game>_full.txt`.

### MuShoot
**MuShoot is playable**, built by the public `mushoot-rapture-port` (`build.bat`, submodule
`recomp/` = this repository) from the user's own game files. Verified unattended (`--autoplay`,
hidden window, SDL dummy audio): intro movies (DILogo/DDLogo), title, Mushu's spoken intro,
New game dialog, levels with Huns/riders/hawks, firing, level-complete bonus, Options menu
and every sub-dialog (Audio, High scores, Credits with fireworks, Save), game over, initials
entry, high-score table (saved to the .ini), "Play again?". A clean clone of the port builds
and runs.

### Flying Frenzy

**Playable**, built by the public `flying-frenzy-rapture-port` (`build.bat`, submodule
`recomp/` = this repository) from the user's own files (`Flying.exe`, `Frenzy.dll`, `moses.dll`,
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
### Flounder Quest

**Disney's Flounder Quest is playable**, built by the public `flounder-quest-rapture-port`
(`build.bat`, submodule `recomp/` = this repository). Config: `configs/flounder.toml`
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

### Rooftop Ruckus

**Disney's Rooftop Ruckus is playable**, built by the public `rooftop-ruckus-rapture-port`
(`build.bat`, submodule `recomp/` = this repository) from the user's own game files.

* Game-specific files only: `configs/ruckus.toml` (bases as MuShoot; `__sbh_alloc_block`
  replaced in every module: exe, moses.dll and Aaron.dll at MuShoot's addresses, since the
  exe's code is identical and moses/Aaron are the same files; Ruckus.dll at its own address,
  found by a relocation-masked byte match of MuShoot's function) and
  `tests/autoplay/ruckus_full.txt` (unattended run, coordinates only).
* Recompiler output: exe 680, Ruckus.dll 1133, moses 712, Aaron 1351 functions;
  **0 unsupported instructions**.
* **Changes to common code (`recomp/`, `runtime/`, `win32/`, CMakeLists.txt): none.**
  MuShoot rebuilt with it: builds, `test_mushoot_crt` passes.
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
* `test_threads` (guest threads through the Win32 layer, no game code).
* `test_mushoot_crt` (3275 checks: CRT string/memory functions, x87 helpers) — `RECOMP_BUILD_TESTS`.
* `tools/difftest.py` + `difftest`: every self-contained function vs Unicorn on random states:
  all agree except 6 known x87-flag helpers.


### Guest threads
Every guest thread has a slot (`win32/threads.c`): a TEB and a 256 KiB guest stack, given back
when the thread returns, calls ExitThread or is terminated. 55 threads can run at once, any
number over the program's life (before, stacks were never freed and the 9th thread aborted
with "out of guest stack space": MuShoot starts one per sound/movie and hit it at the credits
in some runs). `test_threads` (built with the game, `RECOMP_BUILD_TESTS`) creates and ends
thousands of threads through the layer's own CreateThread/ExitThread/TerminateThread.

## Autoplay scripts (`tests/autoplay/`)
Unattended full runs, one per game: hidden window, SDL dummy audio, PNG shots in `OUT`.
Always use a scratch settings file (the game rewrites it; never the real %APPDATA% file).

| game | script | what it covers | about |
|---|---|---|---|
| MuShoot | `mushoot_full.txt` | intro, New game, idle until game over, initials, high scores, "Play again?" No, credits | 2 min |
| Flying Frenzy | `flying_full.txt` | intro, New game, idle until game over, initials, high scores, No, credits | 3 min |
| Flounder Quest | `flounder_full.txt` | intro, New game, F4 cheat, level 1 complete, level 2, game over, initials, high scores | 3 min |
| Rooftop Ruckus | `ruckus_full.txt` | intro, New game, level 1, game over, initials, high scores, No, credits | 3 min |

```
build\mushoot.exe  --autoplay tests\autoplay\mushoot_full.txt  --outdir OUT --settings OUT\s.ini --fast 4
build\flying.exe   --autoplay tests\autoplay\flying_full.txt   --outdir OUT --settings OUT\s.ini --fast 4
copy tests\autoplay\flounder_settings.ini OUT\s.ini
build\flounder.exe --autoplay tests\autoplay\flounder_full.txt --outdir OUT --settings OUT\s.ini --fast 4
build\ruckus.exe   --autoplay tests\autoplay\ruckus_full.txt   --outdir OUT --settings OUT\s.ini --fast 4
```
From a port, the executable is `build\<game>.exe` of the port and the scripts are in
`recomp\tests\autoplay\`. A run passed when every shot of the script exists, the program
exits with 0, and the last shots show the expected screen (credits; Flounder: "Play again?").
`flounder_settings.ini` sets `FKeys=1` (the F-key cheats) in the `[*]` section, which
`win32/advapi32.c` uses as defaults for every registry key.
`flounder_full.txt` steers Sebastian by holding keys, so it depends on the frame rate: run it
alone (with other games or a build running at the same time Sebastian missed Flounder once and
the run ended on level 1 with 0 points). A good run saves 1500 points, level 2.

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

## Decisions
* Win32: forward what is pure or memory-based, emulate pointer/presentation-bound APIs.
* Quarantine + GetVersion XP + `__sbh_alloc_block` replaced: the games read freed objects
  right after deleting them (Aaron's scene message queues in `Window::LoadScene`).
* No change was needed in rapture-engine (only `platform/` is used: Window, Movie, Surface), so
  nothing was merged there and the engine pointers of the dev/port repositories are unchanged.

## Open points / next steps (common to all games)
1. **Not verified**: music (midiOut to the Windows synthesizer: silent in autoplay), the visible
   window (scaling, fullscreen, cursor shapes), joysticks, the effect of the Audio dialog
   toggles and a full Save/Restore cycle.
2. Mixer API is not emulated (WaveVol/MidiVol are saved as 0; no effect on the sound).
3. The About box (DialogBoxParamA) is not shown.
4. Gameplay pacing vs. the hand port: in our runs the game reached level 5-6 before game over,
   the hand port's run ended at level 3; Hun targets are random (seeded by time), so compare
   several runs (or seed both identically) before calling it a bug.
5. Build speed: rapture-engine's whole library is compiled although only `platform/` is used.

## Rules
* Nothing derived from the games in git: check `git ls-files` and grep before every push.
* Repository is **public** (since 2026-11-07): everything in it (code, comments, docs, commit
  messages) is in English. No LICENSE yet (user's decision): all rights reserved until then.
* Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

