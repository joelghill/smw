# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Super Mario World (SMW) is a reverse-engineered C reimplementation of the classic 1990 SNES game. It includes the complete game logic decompiled from the original ROM, a full SNES hardware emulator, and support for Lunar Magic ROM hacks. A `smw.sfc` ROM file is required only for initial asset extraction.

## Build Commands

```sh
# Standard build
make

# Parallel build (recommended)
make -j$(nproc)

# Clean rebuild
make clean all

# Clean object files only
make clean_obj

# Clean generated files
make clean_gen
```

**Dependencies (Linux):** `libsdl2-dev`, Python 3, `zstandard` pip package, GCC or Clang.

**Asset extraction (one-time):** Place `smw.sfc` in the repo root, then:
```sh
python3 assets/restool.py
```
This produces `smw_assets.dat`. The ROM is not needed after extraction.

**Run the game:**
```sh
./smw
```

See `BUILDING.md` for Windows (TCC/MSYS2/MSVC) and Nintendo Switch build instructions.

## Code Style

Clang-format is configured (`.clang-format`, Google style base). Format changed files with:
```sh
clang-format -i <file>
```

## Testing

There is no automated test suite. Correctness is verified at runtime:

- The game runs a reference SNES emulator in parallel in the background.
- Each frame, RAM state is compared between the reimplementation and the reference.
- On mismatch, a snapshot is saved to `saves/`. Snapshots can be replayed to reproduce and verify fixes.

To test a change, build and play through the affected game areas. Check `saves/` for any mismatch snapshots generated during play.

## Architecture

### Source Layout

```
src/
  main.c                  # Entry point: SDL2 window, input, audio callback
  snes/                   # SNES hardware emulation (CPU, PPU, APU, DMA, Cart)
  smw_00.c – smw_0d.c    # Game logic reversed from the ROM, organized by bank
  smw_rtl.h/.c            # SMW runtime library — hardware abstraction layer
  smw_spc_player.h/.c     # SPC700 music player
  common_rtl.h/.c         # Shared CPU/RTL infrastructure
  common_cpu_infra.h/.c   # CPU infrastructure shared across game modules
  smb1/, smbll/           # Super Mario Bros 1 & Lost Levels implementations
  config.h/.c             # INI-based config (smw.ini): keys, graphics, audio
  types.h                 # Type aliases (uint8, uint16, int32, …) and macros
  consts.h                # Game constants
  funcs.h                 # Function declarations (auto-generated from ROM)
  variables.h             # Global game variables (from reversed ROM)
  glsl_shader.h/.c        # OpenGL GLSL shader renderer
  opengl.c                # OpenGL rendering backend
  lm.c                    # Lunar Magic ROM hack compatibility
  util.h/.c               # File I/O, string parsing, BPS patch application
  tracing.h/.c            # Debug tracing
  platform/win32/         # Windows-specific (volume, resources)
  platform/switch/        # Nintendo Switch port
assets/
  restool.py              # Asset extraction tool (ROM → smw_assets.dat)
```

### Key Architectural Patterns

**SNES hardware emulation (`src/snes/`):** Full 65816 CPU, PPU, APU, DMA, and cartridge emulation derived from LakeSnes. This layer runs alongside (or as the reference for) the reimplemented game logic.

**Reversed game logic (`smw_*.c`):** The largest part of the codebase. Each file corresponds to a ROM bank. Functions are named and organized to match the original assembly. `variables.h` and `funcs.h` expose the global state and function table.

**Runtime library abstraction (`smw_rtl.h`, `common_rtl.h`):** Sits between game logic and the hardware emulator. Provides register reads/writes, DMA triggers, IRQ hooks, and rendering calls so game code doesn't call SDL2 or OpenGL directly.

**Renderer:** SDL2 software or OpenGL (switchable). GLSL shaders live in `src/glsl_shader.h/.c`. The renderer receives the PPU framebuffer each frame.

**Configuration (`smw.ini` / `config.h`):** All user-facing settings (keybindings, display, audio, gameplay flags) are read at startup from `smw.ini` and exposed via `config.h` structs.

**BPS patching (`util.c`):** ROM hacks distributed as `.bps` patch files can be applied at runtime via `--patch` CLI flag. The patcher is in `util.c`.

**Parallel verification:** When built in verification mode, the SNES emulator runs in lockstep with the reimplementation. RAM divergences trigger snapshot saves used for bug reports.
