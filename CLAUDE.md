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

---

## HD / Upscaled Feature Guidelines

These rules apply to all new work on the upscaled rendering pipeline.

### Where new code lives

All new code related to the upscaled and enhanced version of the game must live in **`src/hd/`**. Do not add HD-related logic to any file outside that directory. The only permitted exceptions are the minimal hook points already present in `main.c`, `common_rtl.c`, and `smw_00.c` that bridge the 1:1 implementation to the HD layer.

### Separation of concerns

The SMW 1:1 reimplementation (`smw_*.c`, `common_rtl.c`, `smw_rtl.c`, `snes/`) is treated as **read-only** from the HD layer's perspective. The HD code observes game state; it never mutates it. New features are always **additive** — appending rendering passes or decorators on top of the existing pipeline, not modifying original game logic.

### Primary rendering interface

`HdRenderInput` (defined in `src/hd/hd_frame.h`) is the canonical input to the compositor for every frame. The structs `HdFrameSnapshot`, `HdGameState`, `HdScene`, and `HdTextItem` are the contract between the game-state reader (`HdFrame_Build`) and every downstream blit or effect function. New rendering features must consume `HdRenderInput`; they must not reach back into raw game globals or PPU internals directly.

### Unit testing requirement

Every new module added under `src/hd/` must have a corresponding test file under `tests/`. New code is not considered done until unit-test coverage for that module is **≥ 80%**. Pure rendering helpers that have no branching logic are exempt, but any function with conditional paths must be covered. Tests must be runnable in isolation without a live SDL2 window or the game ROM.

---

## HD Graphics (branch: `experiment/hd`)

The `experiment/hd` branch adds an HD sprite and background rendering pipeline on top of the base game. The full implementation plan is in `HD_GRAPHICS_SPEC.md`. Summary below.

### Scope

- **v1 (Steps 1–7, complete):** HD sprite replacement only. Backgrounds render at SD, nearest-upscaled. Per-layer drop shadow effects.
- **v2 (Steps 8–12, planned):** HD BG1/BG2/BG3 tile replacement with correct PPU priority interleaving.
- HD graphics are **pixel-only** — no game-state mutations. Verification (RAM compare) is unaffected.

### New Source Files

```
src/hd_compositor.h/.c  # Master HD compositor: per-frame entry point, layer effects, globals
src/hd_gfx.h/.c         # PNG sheet loader (stb_image); maintains g_hd_sheets[0x34]
src/hd_scene.h/.c       # OAM walk → HdScene (one HdSprite per visible OAM entry)
src/hd_vram_map.h/.c    # Slot→VRAM tracking: maps VRAM word-addresses back to (sheet, tile)
assets/export_sheets.py # Authoritative encoder for the HD PNG format (reference for decoding)
```

### Data Flow (per frame, HD on)

1. Game writes OAM/VRAM/CGRAM via PPU register hooks.
2. `draw_ppu_frame()` runs `ppu_runLine` per scanline, producing SD output in `g_my_pixels`. When HD is on, `PpuDrawSprites` is **skipped** (`g_hd_skip_sprites = true`) so `g_my_pixels` is BG+backdrop only.
3. `HdCompositor_Draw` (called from `RtlDrawPpuFrame` in `main.c`):
   - Nearest-upscales `g_my_pixels` into the HD frame buffer (`256·S × 224·S × 4 BGRA`).
   - Calls `HdScene_Build` to walk OAM → `HdScene` (one entry per visible OAM sprite, tagged with OAM priority 0–3).
   - For each OAM priority layer (0→3): optionally renders a drop shadow silhouette into a temp buffer, alpha-blits it; then renders the sprite color pass into a temp buffer, alpha-blits it onto the main HD buffer.
4. The GL renderer receives the HD-sized frame via `PpuGetCurrentRenderScale` (returns `g_hd_scale`).

### Key Globals (defined in `hd_compositor.c`, declared in `hd_compositor.h`)

```c
bool   g_hd_enabled;       // Master toggle; set from smw.ini HdGfxEnabled
uint8  g_hd_scale;         // 1..N; set by HdGfx_LoadAll to the scale of loaded sheets
bool   g_hd_skip_sprites;  // Read by ppu.c; skips PpuDrawSprites when true
uint16 *g_hd_prio_map;     // (v2) 256×240 priority z-buffer captured per scanline
bool   g_hd_bg_enabled[3]; // (v2) per-layer BG tile replacement enable
```

Runtime guard: HD is only active when `g_hd_enabled && g_hd_scale > 1`. If HD is enabled in config but no sheets loaded, the game silently falls back to SD with a stderr warning.

### HD PNG Format

- RGBA PNG, always **16 tiles wide** (`width = 128·S`), variable height (`height = 8·S·N` for N ≥ 1 tile rows).
- Per pixel: `R == G == B == grey`; `alpha 0` → transparent (index 0). Otherwise `index = round(grey × 15 / 255)` (4bpp, max index 15). For 2bpp BG3 sheets, max index 3.
- Scale `S = width / 128` must be a positive integer. All loaded sheets must share the same scale.
- Authoritative encoding is in `assets/export_sheets.py`. Sheets live in `gfx/hd/gfx00.png`…`gfx33.png`.

### VRAM Mapping (slot→tile resolution)

Two upload paths must be tracked to resolve VRAM tile addresses back to HD sheets:

- **Path A (bulk per-level sheets):** `UploadGraphicsFiles_UploadGFXFile` in `smw_00.c` writes 128 tiles per call directly via `RtlGetVramAddr()` (bypasses `SmwCopyToVram`). Hooked to call `HdVramMap_RecordSheetUpload` after each call.
- **Path B (dynamic Mario tiles):** `SmwCopyToVram` family in `common_rtl.c`. After each copy, calls `HdVramMap_RecordCopyFromStaging`, which looks up the source pointer in a registered staging-buffer registry (sheet 0x32 staged at `g_ram + 0x2000`).

`HdVramMap_ResolveTile(vram_word_addr, &sheet_id, &tile_in_sheet)` scans recorded regions newest-first and returns the first match.

### Compositor Sprite Blit (key implementation detail)

Per sprite, the compositor walks output pixel offsets `(row, col)` in `[0, size)`, applying **tri-level flip**:
1. **Tile-order flip** — which 8×8 sub-tile within the sprite (`tile_row_src`, `tile_col_src` derived from flipped `srcRow`/`srcCol`).
2. **Within-tile pixel flip** — pixel offset inside the 8×8 tile (`x_in_tile_sd`, `y_in_tile_sd`).
3. **Sub-pixel flip** — within the `S×S` HD expansion block (`src_px`, `src_py`).

All three tiers must be applied; missing any one breaks flipped large sprites. The sub-tile VRAM address uses the PPU's `usedTile` formula:
```c
uint16 usedTile = ((uint16)(tile_hi + tile_row_src) << 4) | ((tile_lo + tile_col_src) & 0xf);
uint16 tile_vram = (obj_page + usedTile * 16) & 0x7fff;
```
This matches `ppu_evaluateSprites` in `ppu.c` exactly.

### Per-Layer Drop Shadow

Configured via `smw.ini` (`HdLayerShadow`, `HdShadowOffset`, `HdShadowAlpha`, `HdShadowColor`). Default: shadow on OAM priority layer 2 only (Mario + most enemies). Algorithm:
1. Render all sprites in the layer as a monochrome silhouette into `g_hd_shadow_buf`, offset by `(shadow_dx, shadow_dy)` HD pixels.
2. Alpha-blit `g_hd_shadow_buf` onto main HD buffer.
3. Render all sprites color into `g_hd_layer_buf`.
4. Alpha-blit `g_hd_layer_buf` onto main HD buffer.

Overlapping silhouettes within a layer do **not** compound (last-writer-wins in the shadow buffer). This is intentional.

### Runtime Hotkey

- **Ctrl+H** — toggle HD on/off (`kKeys_ToggleHdGfx`). Rebindable via `[KeyMap]` section `ToggleHdGfx`.
- Toggling does not reload sheets (restart required to pick up new PNGs).

### v2 BG Tile Priority Architecture (planned)

v2 replaces the v1 "sprites on top of everything" model with a full 11-pass priority-ordered composite:

> BG3-lo → Spr-0 → BG3-hi(!prio) → Spr-1 → BG2-lo → BG1-lo → Spr-2 → BG2-hi → BG1-hi → Spr-3 → BG3-hi(prio)

This requires capturing the PPU's per-pixel z-buffer (`bgBuffers[0].data`) into `g_hd_prio_map` each scanline (Step 8), so BG tile blits can be gated on "did this layer actually win this pixel?". BG3 is 2bpp (8 VRAM words/tile vs. 16 for 4bpp); `HdVramRegion` gets a `tile_stride` field to handle both.

### Known Limitations (v1)

- HD sprites composite on top of all BG tiles regardless of BG priority bits (fixed in v2).
- Mode 7 scenes (Bowser fight, overworld rotation) fall back to upscaled SD.
- `brightnessMult` uses the final scanline's value; mid-frame HDMA brightness ramps are not reproduced per-scanline.
- OAM priority rotation (`$2103` bit 7) is ignored; OAM is always walked 0→127.
- One uniform scale across all sheets; mixed-scale sheets are rejected with a warning.
- Sprites with no HD replacement are skipped entirely (no SD fallback for individual unmapped sprites).
