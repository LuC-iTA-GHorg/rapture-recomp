# rapture-recomp

Static x86-32 recompiler for the Disney "Hot Shot" games (1999) built on the Rapture
engine (`moses.dll`, `Aaron.dll`). At build time it translates **your own copy** of the
game's executables and DLLs into C, which is compiled with a small runtime.

This repository contains no game code or data: you must own the original game. Generated
files live only in your build directory.

**Status: early prototype.** The recompiler loads and rebases all four MuShoot modules,
discovers ~3,300 functions, translates them to C that compiles, and a test checks a
first set of recompiled `Shoot.dll` functions against host libc. The Win32 layer does not
exist yet, so nothing is playable. See [docs/DESIGN.md](docs/DESIGN.md) and
[HANDOFF.md](HANDOFF.md).

## Requirements

* Windows, MinGW-w64 GCC (64-bit), CMake >= 3.20, Ninja
* Python >= 3.11 with `pefile` and `capstone` (`pip install pefile capstone`)

## Build and test (MuShoot)

```bat
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DGAME_DIR="C:/path/to/MUSHOOT"
cmake --build build
build\test_mushoot_crt build\gen
```

`GAME_DIR` is the folder with `Shoot.exe`, `Shoot.dll`, `moses.dll`, `Aaron.dll`.
The recompiler alone: `python -m recomp configs/mushoot.toml --game-dir DIR --out OUT`.

## Layout

| path | what |
|---|---|
| `recomp/` | the recompiler (Python): PE loader, function discovery, x86 -> C |
| `runtime/` | guest memory, dispatch, faults (C) |
| `configs/` | per-game configs: module list and guest bases (no game content) |
| `tests/` | tests of recompiled code against known semantics |
| `docs/DESIGN.md` | architecture and its rationale |
