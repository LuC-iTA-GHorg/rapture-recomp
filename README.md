# rapture-recomp

Static x86-32 recompiler for the Disney "Hot Shot" games (1999) built on the Rapture
engine (`moses.dll`, `Aaron.dll`). At build time it translates **your own copy** of the
game's executable and DLLs into C, which is compiled with a small runtime and a Win32
replacement layer (SDL2, rapture-engine's platform layer) into a native 64-bit Windows
program.

This repository contains no game code or data: you must own the original game. Generated
files live only in your build directory.

**Status:** all four games are playable: Disney's MuShoot, Flying Frenzy, Flounder Quest and
Rooftop Ruckus (public ports `mushoot-rapture-port`, `flying-frenzy-rapture-port`,
`flounder-quest-rapture-port`, `rooftop-ruckus-rapture-port`, which use this repository as a
submodule; per-game settings in `configs/<game>.toml`, e.g. `-DGAME_CONFIG=configs/flounder.toml
-DGAME_EXE=flounder`). Design: [docs/DESIGN.md](docs/DESIGN.md); state and next
steps: [HANDOFF.md](HANDOFF.md).

## Requirements

* Windows, MinGW-w64 GCC (64-bit), CMake >= 3.20, SDL2 (MinGW development files)
* Python >= 3.11 with `pefile` and `capstone` (`pip install pefile capstone`);
  `unicorn` too for the differential test
* a checkout of [rapture-engine](https://github.com/LuC-iTA-GHorg/rapture-engine)

## Build (MuShoot)

```bat
cmake -S . -B build -G Ninja -DGAME_DIR="C:/path/to/MUSHOOT" -DGAME_EXE=mushoot ^
      -DRAPTURE_ENGINE_DIR=path/to/rapture-engine -DSDL2_DIR=path/to/SDL2/cmake
cmake --build build
build\mushoot.exe
```

`GAME_DIR` is the folder with `Shoot.exe`, `Shoot.dll`, `moses.dll`, `Aaron.dll` and the data
files. The executable runs in that folder (it goes there by itself, or `--game-dir DIR`).
Options: `--fullscreen`, `--settings FILE.ini`, `--autoplay SCRIPT --outdir DIR [--fast N]`
(unattended run with a hidden window, see `win32/window.cpp`), `--log`.

Tests: `build\test_threads` (guest threads through the Win32 layer), `build\test_mushoot_crt build\gen`;
`python tools/difftest.py configs/mushoot.toml --game-dir DIR --gen build/gen --runner build/difftest.exe`.
Unattended full runs of each game: `tests/autoplay/` (see HANDOFF.md).
The recompiler alone: `python -m recomp configs/mushoot.toml --game-dir DIR --out OUT`.

## Layout

| path | what |
|---|---|
| `recomp/` | the recompiler (Python): PE loader, function discovery, x86 -> C |
| `runtime/` | guest memory, dispatch, imports, faults, x87 helpers (C) |
| `win32/` | the Win32 layer: kernel32, user32, gdi32, winmm, registry, DirectSound, MCIWnd, window |
| `configs/` | per-game configs: modules, guest bases, function replacements (addresses only) |
| `tests/`, `tools/` | tests of recompiled code, differential test against Unicorn |
| `docs/DESIGN.md` | architecture and its rationale |
