# HD Graphics — Refactored V2 Spec

This document is the source of truth for HD pipeline work on the
`experiment/hd` branch. It replaces the older step-by-step history-oriented
plan because the implementation is now split into focused modules under
`src/hd/`.

Scope of this spec: bring HD rendering to V2 ("done") quality, with placeholder
safety net, on a refactor-aware foundation. Each step below is written so that
a Sonnet-class implementer can execute it without external context.

---

## 1) Scope and Goal

**Goal:** When HD is active, render the frame through the HD compositor pipeline
with authored HD assets where available and deterministic placeholder fallback
where not.

**V2 target behavior:**
- Pure HD composition path for Mode 1 and Mode 7.
- No game-state mutation from HD code paths.
- Sprite and BG (incl. BG3 2bpp) replacement driven by VRAM-to-sheet mapping.
- Unmapped content rendered as visible placeholder, never silently dropped.
- Effects (drop shadow) remain additive and configurable.

**Non-goals for this track:**
- Color math parity (subscreen add/subtract/window math).
- Lunar Magic custom GFX routing (`src/lm.c` paths). V2.3 audit is stock-ROM
  upload paths only.
- Mixed-scale HD sheets in one session.
- HUD / overlay text rendering. The `HdTextItem` and `HdGameState` fields in
  `HdRenderInput` are reserved surface; do not extend or consume in V2.

---

## 2) Current Implemented Baseline

### 2.1 Refactored module layout

HD pipeline code lives in `src/hd/`:
- [hd_frame.h](src/hd/hd_frame.h) / [hd_frame.c](src/hd/hd_frame.c) — builds
  `HdRenderInput` (`HdFrameSnapshot` + `HdGameState`).
- [hd_scene.h](src/hd/hd_scene.h) / [hd_scene.c](src/hd/hd_scene.c) — OAM walk
  to `HdScene`.
- [hd_vram_map.h](src/hd/hd_vram_map.h) / [hd_vram_map.c](src/hd/hd_vram_map.c)
  — VRAM region tracking, staging registry, placeholder catch-all.
- [hd_gfx.h](src/hd/hd_gfx.h) / [hd_gfx.c](src/hd/hd_gfx.c) — HD sheet loading,
  global placeholder synthesis.
- [hd_blit_sprite.h](src/hd/hd_blit_sprite.h) /
  [hd_blit_sprite.c](src/hd/hd_blit_sprite.c) — sprite blit (color/silhouette).
- [hd_blit_bg.h](src/hd/hd_blit_bg.h) / [hd_blit_bg.c](src/hd/hd_blit_bg.c) —
  BG layer blit + Mode 7 placeholder helper.
- [hd_alpha.h](src/hd/hd_alpha.h) / [hd_alpha.c](src/hd/hd_alpha.c) — alpha
  compositing helper.
- [hd_compositor.h](src/hd/hd_compositor.h) /
  [hd_compositor.c](src/hd/hd_compositor.c) — pass orchestrator + layer shadow
  config.

### 2.2 Runtime integration already present

- HD frame build and draw are called from `RtlDrawPpuFrame` in
  [src/main.c](src/main.c).
- `g_hd_skip_sprites` is set only when `hd_active = g_hd_enabled && g_hd_scale > 1`
  ([src/main.c:186-187](src/main.c#L186-L187)).
- PPU sprite draw is gated in mode 1 when `g_hd_skip_sprites` is true.
- Config and toggle (`Ctrl+H`) are wired through
  [src/config.c](src/config.c), [src/config.h](src/config.h), and `src/main.c`.
- BG1/BG2 blit already consumes authored sheets via
  `HdVramMap_ResolveTile` + `g_hd_sheets`
  ([hd_blit_bg.c:65-72](src/hd/hd_blit_bg.c#L65-L72)).

### 2.3 Current compositor behavior

- Backdrop fill from decoded palette
  ([hd_compositor.c:137-140](src/hd/hd_compositor.c#L137-L140)).
- Mode 1 uses 11-pass priority-ordered composite
  ([hd_compositor.c:152-165](src/hd/hd_compositor.c#L152-L165)).
- Per-layer shadow pre-pass + color pass for sprites.
- `g_hd_bg_enabled = { true, true, false }` — BG3 disabled by default.
- Mode 7 emits a magenta placeholder
  ([hd_blit_bg.c:7-26](src/hd/hd_blit_bg.c#L7-L26)).

### 2.4 Current tests

- Only [tests/test_hd_vram_map.c](tests/test_hd_vram_map.c) exists.
- Built via Unity in [third_party/unity/](third_party/unity/).
- The Makefile `test` target globs `tests/test_*.c` but the **only** explicit
  link rule is for `test_hd_vram_map.elf`. Adding new tests today requires
  adding new explicit Makefile rules — V2.0 fixes this.

### 2.5 Known stale artifact (must be removed)

- [src/hd_compositor.c](src/hd_compositor.c) is the **pre-refactor monolithic
  compositor** and is still picked up by the Makefile glob `src/*.c`. It
  duplicates non-static symbols already defined in
  `src/hd/hd_compositor.c` (`HdCompositor_Draw`, `_Init`, `_Shutdown`,
  `_Toggle`, `_ApplyConfig`). The current `smw` binary on disk predates the
  refactor; a fresh `make clean all` will fail to link. **This is a build
  blocker, not hygiene.** Removed in V2.0.

---

## 3) End-State Definition (V2 Done)

V2 is complete when **all** of the following are true:

1. Fresh `make clean -j$(nproc) all` succeeds with no duplicate-symbol errors
   and no warnings under existing `-Werror`.
2. Mode 1 HD path renders BG1/BG2/BG3 + sprites with deterministic placeholder
   fallback for any missing mapping.
3. Mode 7 uses a real HD renderer path (not magenta stub), with placeholder
   fallback if dedicated Mode 7 assets are missing.
4. VRAM mapping handles all stock-ROM gameplay and overworld upload paths.
5. HD toggles and config settings are stable and do not produce
   invisible-content failure modes.
6. Every HD module with branching logic has unit-test line coverage ≥80%
   (≥85% for `hd_vram_map.c`), measured by the coverage harness defined in
   V2.0.
7. `make test` passes from a clean tree.
8. [CLAUDE.md](CLAUDE.md) "HD Graphics" section reflects the final V2 paths,
   filenames, and behaviors.

---

## 4) Contracts To Keep Stable

These architectural contracts must be preserved by every step below.

### 4.1 Module contract

- `HdRenderInput` ([hd_frame.h:54-61](src/hd/hd_frame.h#L54-L61)) is the only
  per-frame compositor input contract.
- `HdFrame_Build(HdRenderInput*, const struct Ppu*)` is the **single boundary**
  where HD code reads live PPU/global state. `HdScene_Build` is called from
  inside `HdFrame_Build`; no other HD code (compositor, blitters, alpha) may
  read live game globals or PPU internals.
- Blitters consume `HdFrameSnapshot` and `HdSprite` data only.
- HD code is **display-only**. It does not write to non-HD globals, OAM, VRAM,
  CGRAM, PPU registers, or any field in `g_ram` outside HD-owned regions.
- Placeholder fallback is deterministic and prefers "visibly wrong" over
  silent omission. The placeholder sheet id is `0xFE`; the unresolved id is
  `0xFF`. Resolve behavior is documented in
  [hd_vram_map.h:5-13](src/hd/hd_vram_map.h#L5-L13).

### 4.2 Canonical pass order (Mode 1)

The sequence below is the contract; no step below may reorder or drop passes
without a corresponding update to this section.

```
backdrop fill                      (palette[0])
BG3-lo
sprite layer 0    (OAM priority 0)
BG3-hi if !bg3prio
sprite layer 1    (OAM priority 1)
BG2-lo
BG1-lo
sprite layer 2    (OAM priority 2)
BG2-hi
BG1-hi
sprite layer 3    (OAM priority 3)
BG3-hi if  bg3prio
```

Each `BG*` pass is gated by `g_hd_bg_enabled[layer]` and the snapshot mosaic
flag. Each sprite layer runs an optional silhouette pre-pass (controlled by
`g_hd_layer_cfg[oam_prio].shadow_enabled`) followed by the color pass.

### 4.3 Coverage / display-only invariants are testable

V2.5 includes a display-only invariant test (see Step V2.5).

---

## 5) Revised V2 Implementation Plan

> **Convention for every step below:** "Files touched" lists every file the
> implementer is expected to modify. "Unit tests" lists new or extended test
> files and the named cases they must contain. "Acceptance" is binary —
> implementer must verify each bullet before marking the step done.

---

## Step V2.0 — Build hygiene, test infrastructure, coverage harness

**Goal:** Make the refactored `src/hd/` tree the only HD implementation in
the build, give the test target a generic rule so V2.1+ can add tests
without Makefile churn, define the coverage tool, and resync `CLAUDE.md`.

### Work item V2.0-A: Remove stale top-level `hd_compositor.c`

1. Delete [src/hd_compositor.c](src/hd_compositor.c).
2. Run `make clean && make -j$(nproc)` and confirm no link errors and no
   warnings.
3. Confirm by `nm`: only `src/hd/hd_compositor.o` defines
   `HdCompositor_Draw`, `_Init`, `_Shutdown`, `_Toggle`, `_ApplyConfig`.
4. There is no top-level `src/hd_compositor.h`; no include path edits needed.

### Work item V2.0-B: Generalize the test Makefile rule

Current rule in [Makefile:42-46](Makefile#L42-L46) is hand-written for
`test_hd_vram_map.elf`. Replace with a static pattern rule plus a
per-test source list, e.g.:

```make
TEST_CFLAGS := -O0 -g -Werror -I. -Isrc -Ithird_party/unity --coverage
TEST_LDFLAGS := --coverage

# Each test depends on its own .c plus the src/hd/*.c modules it exercises.
# Sources are declared per-test below, then all tests share one link rule.
test_hd_vram_map_SRCS    := tests/test_hd_vram_map.c    src/hd/hd_vram_map.c
test_hd_scene_SRCS       := tests/test_hd_scene.c       src/hd/hd_scene.c       src/hd/hd_vram_map.c
test_hd_frame_SRCS       := tests/test_hd_frame.c       src/hd/hd_frame.c       src/hd/hd_scene.c src/hd/hd_vram_map.c
test_hd_alpha_SRCS       := tests/test_hd_alpha.c       src/hd/hd_alpha.c
test_hd_blit_sprite_SRCS := tests/test_hd_blit_sprite.c src/hd/hd_blit_sprite.c src/hd/hd_vram_map.c src/hd/hd_gfx.c
test_hd_blit_bg_SRCS     := tests/test_hd_blit_bg.c     src/hd/hd_blit_bg.c     src/hd/hd_vram_map.c src/hd/hd_gfx.c
test_hd_compositor_SRCS  := tests/test_hd_compositor.c  src/hd/hd_compositor.c  $(test_hd_blit_bg_SRCS) $(test_hd_blit_sprite_SRCS) src/hd/hd_alpha.c src/hd/hd_frame.c src/hd/hd_scene.c

tests/%.elf:
	$(CC) $(TEST_CFLAGS) -o $@ $($*_SRCS) third_party/unity/unity.c $(TEST_LDFLAGS)
```

Implementer note: `--coverage` adds gcov instrumentation; if existing test
target should not require coverage, gate behind `COVERAGE=1`:
`TEST_CFLAGS += $(if $(COVERAGE),--coverage,)`.

### Work item V2.0-C: Coverage harness

1. Add `make coverage` target that:
   - Builds all `tests/*.elf` with `COVERAGE=1`.
   - Runs each test (so `*.gcda` files are produced).
   - Runs `gcov` over `src/hd/*.c` and writes a one-line summary per file:
     `FILE: <pct>%`.
   - Exits non-zero if any module under measurement is below its target
     (80% default; 85% for `hd_vram_map.c`).
2. Targets are encoded in the Makefile, e.g.
   `HD_COV_TARGETS := src/hd/hd_alpha.c:80 src/hd/hd_blit_bg.c:80 ... src/hd/hd_vram_map.c:85`.
3. `gcov` is the required tool. `lcov` is optional and not required to merge.

Acceptance for V2.0-C: `make coverage` runs end-to-end on a clean tree (after
V2.5) and prints per-file percentages. Until then, the target may report
"insufficient coverage" — that is expected and not a V2.0 failure.

### Work item V2.0-D: Resync `CLAUDE.md`

Edit the "HD Graphics (branch: experiment/hd)" section of
[CLAUDE.md](CLAUDE.md) so that:

- "New Source Files" lists `src/hd/*.h/.c` paths (NOT top-level `src/hd_*`).
- "Known Limitations (v1) — Mode 7 scenes (...) fall back to upscaled SD."
  is corrected to: "Mode 7 scenes render a magenta placeholder (real Mode 7
  HD path is V2.2)."
- The pass-order block matches Section 4.2 of this spec verbatim.
- "v2 BG Tile Priority Architecture (planned)" is rewritten as "current"
  rather than "planned" since the 11-pass orchestrator is now live.

### Files touched

- Delete: [src/hd_compositor.c](src/hd_compositor.c)
- Modify: [Makefile](Makefile), [CLAUDE.md](CLAUDE.md)

### Unit tests

- No new test files.
- Existing `tests/test_hd_vram_map.elf` must still pass.

### Acceptance

- `make clean && make -j$(nproc)` succeeds with no warnings.
- `nm src/hd/hd_compositor.o` and `nm` over the final `smw` binary show
  exactly one definition of each `HdCompositor_*` global symbol.
- `make test` passes from a clean tree.
- `make coverage` runs without error on `test_hd_vram_map` (other modules
  may report below target — expected until V2.5).
- `CLAUDE.md` HD section accurately reflects refactored layout and current
  Mode 7 behavior.

---

## Step V2.1 — BG3 (2bpp) completion in refactored blitter

**Goal:** fully support BG3 authored HD tiles in the refactored BG path.
BG1/BG2 already consume authored sheets via `HdVramMap_ResolveTile` +
`g_hd_sheets` ([hd_blit_bg.c:65-72](src/hd/hd_blit_bg.c#L65-L72)). BG3
is structurally different because tiles are 2bpp (8 VRAM words / 16 bytes
per tile) instead of 4bpp (16 VRAM words / 32 bytes per tile).

### Work item V2.1-A: Add `tile_stride` to `HdVramRegion`

Edit [src/hd/hd_vram_map.h](src/hd/hd_vram_map.h):

1. Add field to `HdVramRegion`:
   ```c
   uint8 tile_stride_words;  // 16 for 4bpp tiles, 8 for 2bpp tiles
   ```
2. Update `HdVramMap_RecordSheetUpload` signature:
   ```c
   void HdVramMap_RecordSheetUpload(uint16 dst_word_addr, uint8 sheet_id,
                                    uint16 src_tile_offset, uint16 tile_count,
                                    uint8 tile_stride_words);
   ```
3. Update `HdVramMap_RecordCopyFromStaging` signature similarly (default 16
   for current callers; explicit 8 for BG3).

Edit [src/hd/hd_vram_map.c](src/hd/hd_vram_map.c):

1. Store `tile_stride_words` in each `HdVramRegion` on record.
2. In `HdVramMap_ResolveTile`, when checking whether `vram_word_addr` falls
   inside a region, compute the per-tile offset using the region's stride:
   ```c
   uint16 offset_words = vram_word_addr - region->vram_word_addr;
   if (offset_words >= region->tile_count * region->tile_stride_words) continue;
   if (offset_words % region->tile_stride_words != 0) continue;
   uint16 tile_in_region = offset_words / region->tile_stride_words;
   ```
3. Return `region->src_tile_offset + tile_in_region`.

Existing call sites that pass tile-stride must be updated:
- `UploadGraphicsFiles_UploadGFXFile` hook in [src/smw_00.c](src/smw_00.c) —
  pass 16 for sprite/BG1/BG2 sheets, 8 for BG3 sheets. Implementer must
  inspect which sheet-id ranges correspond to BG3 (typically sheet IDs
  used as the BG3 source by the level loader; verify by reading
  `UploadGraphicsFiles_UploadGFXFile` and surrounding logic).
- `HdVramMap_RecordCopyFromStaging` callers in
  [src/common_rtl.c](src/common_rtl.c) — pass 16 (Mario dynamic tiles are
  4bpp).

### Work item V2.1-B: Branch BG blit on stride

Edit [src/hd/hd_blit_bg.c](src/hd/hd_blit_bg.c):

1. The current line 65 hardcodes 4bpp:
   ```c
   uint16 tile_vram = (uint16)((tileadr + tile_num * 16) & 0x7fff);
   ```
   Replace with a stride based on the BG layer:
   ```c
   int   bpp_words = (bg_layer == 2) ? 8 : 16;  // BG3 is 2bpp
   uint16 tile_vram = (uint16)((tileadr + tile_num * bpp_words) & 0x7fff);
   ```
2. The blit body (lines 80-99) reads `sheet->index_buffer` with values 0..15
   for 4bpp. For 2bpp the valid index range is 0..3. Author the BG3 sheet
   loader (V2.1-C) to clamp to 4 colors so the existing code path "just works"
   — i.e. no per-pixel branch is needed in the blit.
3. BG palette base for BG3 is still `palette * 16` from the tilemap entry; no
   change.

### Work item V2.1-C: BG3 sheet loader handles 2bpp index range

Edit [src/hd/hd_gfx.c](src/hd/hd_gfx.c) sheet loader:

- The PNG decode currently produces indices 0..15 (4bpp).
  Add a per-sheet `bpp` field to the sheet manifest (or infer from sheet id
  mapping). For BG3 sheets, change index quantization from
  `index = round(grey * 15 / 255)` to `index = round(grey * 3 / 255)` and
  cap at 3.
- Implementer note: the authoritative encoder is
  [assets/export_sheets.py](assets/export_sheets.py); update it in lockstep
  if encoding changes are needed.

### Work item V2.1-D: Enable BG3 by default

Edit [src/hd/hd_compositor.c:16](src/hd/hd_compositor.c#L16):
```c
bool g_hd_bg_enabled[3] = { true, true, true };
```

### Files touched

- [src/hd/hd_vram_map.h](src/hd/hd_vram_map.h)
- [src/hd/hd_vram_map.c](src/hd/hd_vram_map.c)
- [src/hd/hd_blit_bg.c](src/hd/hd_blit_bg.c)
- [src/hd/hd_gfx.c](src/hd/hd_gfx.c) (and possibly `hd_gfx.h` for sheet bpp
  metadata)
- [src/hd/hd_compositor.c](src/hd/hd_compositor.c) (BG3 default-on)
- [src/smw_00.c](src/smw_00.c) (upload hook stride argument)
- [src/common_rtl.c](src/common_rtl.c) (upload hook stride argument)
- [assets/export_sheets.py](assets/export_sheets.py) if encoding changes

### Unit tests (required)

- New: `tests/test_hd_blit_bg.c`. Cases:
  - `test_bg_blit_uses_resolved_sheet`: synthetic snapshot with one tilemap
    entry, one registered sheet, no flip. Assert one pixel at expected
    output location matches palette decode.
  - `test_bg_blit_hflip_vflip`: assert flip flags map correctly.
  - `test_bg_blit_unmapped_yields_placeholder`: tile not in any region;
    assert placeholder is drawn (sheet id 0xFE).
  - `test_bg3_uses_8_word_stride`: register one BG3 region with
    `tile_stride_words=8`, resolve tile 1 at base+8 words.
  - `test_bg_blit_skips_when_layer_disabled`: clear `g_hd_bg_enabled[1]`,
    assert no writes.
  - `test_bg_blit_skips_when_mode_not_1`: `f->mode = 7`, assert no writes.
- Extend `tests/test_hd_vram_map.c`:
  - `test_resolve_mixed_strides`: register one 4bpp region and one 2bpp region
    in adjacent VRAM ranges; resolve a few addresses in each and verify
    correct `(sheet_id, tile_in_sheet)`.
  - `test_resolve_misaligned_returns_unresolved`: address that falls between
    tile boundaries (e.g., +12 words inside a 16-word stride) returns 0xFE.
- Coverage targets:
  - `src/hd/hd_blit_bg.c` ≥80%
  - `src/hd/hd_vram_map.c` ≥85%

### Acceptance

- `make clean all && make test && make coverage` succeed.
- BG3 HD content renders when authored.
- BG3 placeholder appears when missing.
- No regression in BG1/BG2 or sprite rendering on a level previously
  validated.

---

## Step V2.2 — Mode 7 real HD renderer

**Goal:** replace the magenta Mode 7 placeholder with a true HD Mode 7
compositor, with placeholder fallback when Mode 7 assets are missing.

The current placeholder is in [hd_blit_bg.c:7-26](src/hd/hd_blit_bg.c#L7-L26)
and is selected at [hd_compositor.c:147-150](src/hd/hd_compositor.c#L147-L150).

### Work item V2.2-A: Per-scanline matrix capture

The PPU latches Mode 7 matrix registers (M7A/M7B/M7C/M7D, M7X/M7Y, M7HOFS/M7VOFS)
per scanline. The HD compositor runs once per frame, so it must observe these
on a per-scanline basis to reproduce on-the-fly transforms (Bowser fight,
title screen, overworld globe).

Implementer task:
1. Identify the existing per-scanline path in
   [src/snes/ppu.c](src/snes/ppu.c). The function that latches Mode 7 state
   is part of `ppu_handleVblank` / per-line setup. (Concretely: look for
   `m7matrix`, `m7startX`, `m7startY` reads.)
2. Add an HD hook called once per scanline when `g_hd_skip_sprites && f->mode == 7`,
   storing into a new struct:
   ```c
   // src/hd/hd_frame.h
   typedef struct HdMode7Line {
     int16  m7a, m7b, m7c, m7d;     // 8.8 signed fixed-point
     int16  m7x, m7y;               // 13-bit signed center
     int16  m7hofs, m7vofs;         // 13-bit signed scroll
     uint8  control;                // PPU $211a (screen-over, flip)
   } HdMode7Line;
   ```
3. Add a 224-entry array `HdMode7Line m7_lines[224];` to `HdFrameSnapshot`.
4. Populate from PPU during the line-render pass when in Mode 7.

This crosses the "HD code does not read PPU directly" boundary; the rule is
that the **PPU writes into the snapshot via a dedicated capture function
called by the PPU**, not the other way around. The capture function lives
in `hd_frame.c` and is called from `ppu.c` only when HD is active. This
mirrors how `g_hd_skip_sprites` is read in the PPU path today.

### Work item V2.2-B: Mode 7 sampler

New file [src/hd/hd_mode7.c](src/hd/hd_mode7.c) (and `.h`):

```c
void HdMode7_DrawScanline(uint8 *dst, size_t pitch, int hd_w,
                          int sd_y, const HdMode7Line *line,
                          const HdFrameSnapshot *f);
void HdMode7_DrawFrame(uint8 *dst, size_t pitch, int hd_w, int hd_h,
                       const HdFrameSnapshot *f);
```

Per-pixel screen-to-tilemap math (per output pixel `(sx, sy)`):
```
ox = sx + line->m7hofs - line->m7x
oy = sy + line->m7vofs - line->m7y
fx = (line->m7a * ox + line->m7b * oy) >> 8 + line->m7x  // 8.8 multiply
fy = (line->m7c * ox + line->m7d * oy) >> 8 + line->m7y
tile_x = (fx >> 3) & 0x7f
tile_y = (fy >> 3) & 0x7f
in_x   = fx & 7
in_y   = fy & 7
tilemap_index = tile_y * 128 + tile_x       // 128×128 fixed
tile_num      = vram[tilemap_index] & 0xff  // Mode 7 tilemap is 1 byte / entry
```
The byte at `vram[tile_num * 64 + in_y * 8 + in_x]` is the 8bpp palette
index. Apply screen-over behavior per `line->control` bits 6-7
(transparent / wrap / fill / fill-with-tile-zero).

The HD path replaces the per-pixel index lookup with: resolve `tile_num`
through the Mode 7 VRAM map (a separate region list, since Mode 7 tiles
are 8bpp / 64 bytes/tile — `tile_stride_words = 32`). On hit, sample the
HD sheet at the correct sub-pixel within the `S×S` expansion. On miss,
draw the placeholder for that pixel (single-pixel Mode 7 placeholder
texture, see V2.2-D).

### Work item V2.2-C: Sprite layering in Mode 7

Mode 7 frames still go through the OAM walk; sprites must composite on top
of the Mode 7 background. Implementer task: in `HdCompositor_Draw`, when
`f->mode == 7`:

1. Call `HdMode7_DrawFrame` to fill BG.
2. Then call `CompositeSpriteLayer` for OAM priorities 0..3 in order
   (Mode 7 has no BG2/BG3 to interleave; OAM layers are stacked over the
   Mode 7 BG in their natural order).

Replace the `HdDrawMode7Placeholder` short-circuit at
[hd_compositor.c:147-150](src/hd/hd_compositor.c#L147-L150) with the new
flow.

### Work item V2.2-D: Mode 7 sheet lifecycle and placeholder

Edit [src/hd/hd_gfx.c](src/hd/hd_gfx.c):

- Mode 7 sheets are 8bpp. Loader must accept indices 0..255.
- Sheet width remains 16 tiles; height = `8 * S * (tile_count / 16)`.
- If no Mode 7 sheets are present at startup, generate a deterministic
  placeholder Mode 7 sheet (e.g. a 16-color palette ramp tile applied to
  every tile slot). Draw operations must always produce visible pixels —
  never transparent — for Mode 7.

### Files touched

- New: `src/hd/hd_mode7.h`, `src/hd/hd_mode7.c`
- [src/hd/hd_frame.h](src/hd/hd_frame.h) (add `HdMode7Line`, `m7_lines[224]`)
- [src/hd/hd_frame.c](src/hd/hd_frame.c) (per-scanline capture function)
- [src/snes/ppu.c](src/snes/ppu.c) (per-scanline hook call when HD active)
- [src/hd/hd_compositor.c](src/hd/hd_compositor.c) (Mode 7 dispatch)
- [src/hd/hd_blit_bg.c](src/hd/hd_blit_bg.c) (delete `HdDrawMode7Placeholder`
  once `hd_mode7.c` owns the fallback)
- [src/hd/hd_gfx.c](src/hd/hd_gfx.c) (Mode 7 sheet handling)
- [src/hd/hd_vram_map.c](src/hd/hd_vram_map.c) (Mode 7 region list — may be
  a separate parallel list to keep stride math simple)
- [Makefile](Makefile) auto-globs `src/hd/*.c`, no edit needed for the new
  source file.

### Unit tests (required)

- New: `tests/test_hd_mode7.c`. Cases:
  - `test_m7_identity_transform`: M7A=M7D=0x100, M7B=M7C=0, M7X=M7Y=0.
    For input `(sx, sy)`, expect tile lookup at `(sx>>3, sy>>3)`.
  - `test_m7_translation_only`: M7HOFS/M7VOFS shift; assert sampling shifted.
  - `test_m7_rotation_90`: M7B=0x100, M7C=-0x100, M7A=M7D=0; assert rotated
    sample.
  - `test_m7_screen_over_transparent`: control bits select "transparent
    outside"; assert pixel left as backdrop outside the 1024×1024 plane.
  - `test_m7_screen_over_wrap`: control bits select wrap; assert the same
    tile index repeats past the edge.
  - `test_m7_unmapped_tile_uses_placeholder`: VRAM map empty for the tile
    range; assert placeholder pixel.
- Coverage target: `src/hd/hd_mode7.c` ≥80%.

### Acceptance

- Bowser fight and overworld globe scenes render via HD path, not magenta.
- Missing authored Mode 7 assets fall back to placeholder visuals; output is
  never invisible.
- `make test` passes with new test file.

---

## Step V2.3 — Upload coverage for stock-ROM overworld and dynamic paths

**Goal:** ensure all stock-ROM gameplay and overworld VRAM upload paths
resolve to correct sheet/tile mapping. Lunar Magic-specific paths in
[src/lm.c](src/lm.c) are explicitly out of scope (per Section 1).

### Work item V2.3-A: Audit stock-ROM upload sites

1. List all callers of `RtlGetVramAddr`, `SmwCopyToVram`, and direct
   `g_zram` VRAM-write sites in `smw_*.c` and `common_rtl.c`. The full
   list lives in the existing memory note "SMW GFX Upload Paths" — extend
   that list during the audit.
2. For each call site, classify:
   - Path A (bulk) → must call `HdVramMap_RecordSheetUpload`.
   - Path B (dynamic, from staging) → must call
     `HdVramMap_RecordCopyFromStaging`.
   - Direct VRAM write that is *not* sourced from a known sheet → must call
     `HdVramMap_ForgetRegion` to mark the range as unresolved (so it falls
     through to placeholder cleanly).
3. Confirm overworld-specific upload sites have hooks. The overworld has
   distinct status-bar, map-tile, and event-tile uploads; expect at least 2-3
   sites that may currently be unhooked.

### Work item V2.3-B: Resolve precedence and recency

Confirm and document in the test suite:
- Newest region wins on overlap (existing behavior — see
  [hd_vram_map.c:ResolveTile](src/hd/hd_vram_map.c)).
- `HdVramMap_ForgetRegion` removes a Path-B region and exposes the Path-A
  region beneath it.
- A staging buffer write that maps to no registered staging is a no-op (does
  NOT clear the underlying Path-A mapping).

### Files touched

- [src/hd/hd_vram_map.c](src/hd/hd_vram_map.c) (only if precedence tests
  reveal a bug; otherwise no edits)
- Upload-site hooks in [src/smw_00.c](src/smw_00.c),
  [src/common_rtl.c](src/common_rtl.c), and any overworld bank used
  (`smw_04.c`, `smw_05.c` are common candidates — verify by code search).

### Unit tests (required)

Extend `tests/test_hd_vram_map.c`:
- `test_newest_region_wins_on_overlap`.
- `test_forget_region_exposes_underlying_bulk`.
- `test_staging_copy_with_unregistered_src_is_noop`.
- `test_overworld_to_level_to_overworld_sequence`: sequence record-reset
  patterns mimicking transitions; assert mapping after each phase.
- Coverage target: `src/hd/hd_vram_map.c` ≥85%.

### Acceptance

- Overworld → level → overworld transitions maintain correct mapping
  (validated in runtime sanity, not just unit tests).
- Placeholder appears only where content is genuinely unmapped (no sprite
  or BG tile silently disappears in a stock-ROM playthrough of the first
  two worlds).

---

## Step V2.4 — Config surface finalization and runtime controls

**Goal:** lock down the V2 config surface in `smw.ini` and ensure all keys
have parser coverage.

### Work item V2.4-A: Existing keys (must remain stable)

The following keys are already wired
([config.c:396-424](src/config.c#L396-L424)):
- `HdGfxEnabled` (bool) → `g_config.hd_gfx_enabled`
- `HdGfxDir` (string) → `g_config.hd_gfx_dir`
- `HdLayerShadow` (4 bools, comma-separated) → `g_config.hd_layer_shadow[0..3]`
- `HdShadowOffset` (dx,dy) → `g_config.hd_shadow_dx`, `hd_shadow_dy`
- `HdShadowAlpha` (uint8) → `g_config.hd_shadow_alpha`
- `HdShadowColor` (`#RRGGBB` or `r,g,b`) → `g_config.hd_shadow_r/g/b`

Keymap entry (already present): `ToggleHdGfx`
([config.c:65](src/config.c#L65), [config.h:40](src/config.h#L40)).

### Work item V2.4-B: New keys for V2

Add to [src/config.h](src/config.h):
```c
bool hd_bg_enabled[3];   // default { true, true, true }
```

Add parser branch in [src/config.c](src/config.c):
- `HdBgEnabled` (3 bools, comma-separated) → `g_config.hd_bg_enabled[0..2]`.
- Default in `Config_Defaults`: all three `true`.

Wire from config to compositor — extend
[hd_compositor.c HdCompositor_ApplyConfig](src/hd/hd_compositor.c#L55-L66):
```c
for (int i = 0; i < 3; i++) g_hd_bg_enabled[i] = g_config.hd_bg_enabled[i];
```

### Work item V2.4-C: `smw.ini` documentation

Update the shipped `smw.ini` with a commented `[HD]` section listing
each key, valid range, and default. Format must match existing INI style
(case-insensitive keys, `;` for comments).

### Work item V2.4-D: Toggle is frame-safe

`HdCompositor_Toggle` already flips `g_hd_enabled` only. Verify (by code
inspection — no new code) that toggling mid-frame cannot cause the
PPU to skip sprites for half a frame and the compositor to also skip them.
The check at [src/main.c:186](src/main.c#L186) sets `g_hd_skip_sprites`
once per frame from `g_hd_enabled`, which is sampled once. This is already
correct; document in `HdCompositor_Toggle` comment for future readers.

### Files touched

- [src/config.h](src/config.h)
- [src/config.c](src/config.c)
- [src/hd/hd_compositor.c](src/hd/hd_compositor.c) (`ApplyConfig`)
- `smw.ini` (project root)
- [src/hd/hd_compositor.h](src/hd/hd_compositor.h) (one-line comment)

### Unit tests (required)

New: `tests/test_hd_config.c`.

Test the parser without bringing in SDL or the rest of the game. Pattern:
include `src/config.c` directly into the test (or a compatible parser
harness), feed lines, and assert `g_config` fields. If the existing parser
is too tangled to isolate, add a small new function
`Config_ParseLine(const char *key, const char *value)` extracted from the
current parser body and call it from both production code and the test.

Cases:
- `test_hd_gfx_enabled_parses_true_false`.
- `test_hd_layer_shadow_parses_4_bools`.
- `test_hd_layer_shadow_too_few_values_leaves_remainder_unchanged`.
- `test_hd_shadow_offset_parses_dx_dy`.
- `test_hd_shadow_color_hex_form`.
- `test_hd_shadow_color_rgb_form`.
- `test_hd_bg_enabled_parses_3_bools` (new V2 key).
- `test_hd_bg_enabled_defaults_all_true`.

Coverage target: HD-specific parser branches ≥80% of lines in the
HD-key block of `Config_ParseLine`.

### Acceptance

- All keys above are parsed and applied.
- Toggling Ctrl+H mid-frame does not produce missing-sprite frames.
- `make test` passes.

---

## Step V2.5 — Performance, correctness, and hardening pass

**Goal:** stabilize frame-time, lock in display-only invariant, finish the
test matrix, hit coverage targets, declare V2 complete.

### Work item V2.5-A: Hot-path profiling

1. Profile a representative 60-second gameplay session under HD on
   (e.g. with `perf record ./smw` or `gprof` if available).
2. Identify the top 3 self-time functions in `src/hd/`.
3. For each, apply at most one of: lift loop-invariant work, replace
   per-pixel function call with inline body, switch alpha blend to a
   precomputed table. Do not refactor module structure.

Implementer note: do not optimize speculatively. Only act on functions
that show up in the top 3.

### Work item V2.5-B: Display-only invariant test

New: `tests/test_hd_display_only.c`.

Build a synthetic `Ppu` and snapshot non-HD globals into a byte
buffer. Run `HdFrame_Build` then `HdCompositor_Draw` on a heap output
buffer. Snapshot the same non-HD globals again. `memcmp` them; assert
zero diff.

The non-HD globals to capture: `g_ram` (the entire emulated RAM array
referenced by the RTL — see `common_rtl.h`), the Ppu struct fields
*excluding* anything HD-introduced (the new Mode 7 capture struct is
HD-introduced and may legitimately change). The test compiles the
specific source files needed (no SDL).

This test is the only enforcement of the "HD code is display-only"
contract; V2 is not done without it.

### Work item V2.5-C: Fill out remaining test files

Add the following test files at minimum (Cases listed are mandatory; add
more as needed to reach coverage):

- `tests/test_hd_alpha.c`:
  - `test_alpha_zero_dst_passthrough`.
  - `test_alpha_full_overwrites`.
  - `test_alpha_blends_proportionally` (assert formula
    `out = (src*a + dst*(255-a) + 127) / 255` to ±1 LSB).
- `tests/test_hd_scene.c`:
  - `test_scene_empty_oam_yields_zero_count`.
  - `test_scene_priority_field_copied`.
  - `test_scene_offscreen_sprite_filtered_out`.
  - `test_scene_size_select_bit`.
- `tests/test_hd_frame.c`:
  - `test_frame_palette_decoded_with_brightness`.
  - `test_frame_backdrop_is_palette_zero`.
  - `test_frame_mosaic_flags_propagate`.
  - `test_frame_mode_field_copied`.
- `tests/test_hd_blit_sprite.c`:
  - `test_sprite_blit_no_flip`.
  - `test_sprite_blit_hflip`.
  - `test_sprite_blit_vflip`.
  - `test_sprite_blit_hvflip` (verify all three flip tiers per
    [CLAUDE.md HD section](CLAUDE.md): tile-order, within-tile,
    sub-pixel).
  - `test_sprite_blit_silhouette_writes_fixed_color`.
  - `test_sprite_blit_unmapped_uses_placeholder`.
- `tests/test_hd_compositor.c`:
  - Use a mock for `HdBlitBgLayer`, `HdBlitSprite`, `HdAlpha_Blit` (link
    a stub `.c` file in the test that records calls). Assert the
    canonical pass order from Section 4.2 across both
    `bg3prio = false` and `bg3prio = true`.
  - `test_mode7_dispatches_to_mode7_renderer` (after V2.2 lands).
  - `test_layer_shadow_runs_when_enabled_only`.

### Work item V2.5-D: Hit coverage gates

Run `make coverage`. For any module under target, add focused tests until
the gate passes. Do not lower the gates.

### Work item V2.5-E: Strip debug instrumentation

- `HdScene_Dump`, `HdVramMap_Dump`: keep functions but ensure no caller
  invokes them in the default build. Wrap any debug call with
  `#if HD_DEBUG_DUMP` (compile flag, default off).
- Remove `printf`/`fprintf` calls from hot paths (`HdCompositor_Draw`,
  `HdBlitBgLayer`, `HdBlitSprite`, alpha). The `fprintf(stderr, "HD enabled
  but no HD sheets...")` warning at
  [hd_compositor.c:88](src/hd/hd_compositor.c#L88) stays — startup only.

### Files touched

- New tests: `tests/test_hd_display_only.c`, `tests/test_hd_alpha.c`,
  `tests/test_hd_scene.c`, `tests/test_hd_frame.c`,
  `tests/test_hd_blit_sprite.c`, `tests/test_hd_compositor.c` (and any
  stub helpers under `tests/stubs/`).
- Possibly minor edits in any HD module flagged by profiling (V2.5-A) or
  for `HD_DEBUG_DUMP` gating (V2.5-E).
- [Makefile](Makefile): add new test SRCS lines for each new test
  per the V2.0-B convention.

### Unit tests (required)

All listed above.

### Acceptance

- `make clean && make -j$(nproc) all` succeeds clean.
- `make test` passes all suites.
- `make coverage` passes all gates (≥80% per HD module, ≥85% for
  `hd_vram_map.c`).
- 60-second gameplay session shows no regressions vs. pre-V2.5 in
  representative levels (1-1, 1-2, Bowser fight, overworld map).
- No `printf`/`fprintf` in hot paths.

---

## 6) Unit testing requirements (mandatory)

These rules are part of the implementation contract for HD work.

1. Any new module added under `src/hd/` must ship with corresponding tests
   under `tests/`.
2. Any module under `src/hd/` with conditional branches must maintain ≥80%
   line coverage measured by `make coverage`.
3. Critical routing modules: `src/hd/hd_vram_map.c` ≥85%.
4. Tests run headless. They must not require SDL window creation, audio
   init, GL init, or ROM assets.
5. Coverage tool: `gcov`, invoked by the `make coverage` target added in
   V2.0-C. `lcov` HTML output is optional, not required to merge.
6. PRs for HD changes are incomplete without updated tests, and `make
   coverage` output stating each touched module's percentage.

Test file mapping:
- `src/hd/hd_vram_map.c` → `tests/test_hd_vram_map.c`
- `src/hd/hd_scene.c` → `tests/test_hd_scene.c`
- `src/hd/hd_frame.c` → `tests/test_hd_frame.c`
- `src/hd/hd_alpha.c` → `tests/test_hd_alpha.c`
- `src/hd/hd_blit_sprite.c` → `tests/test_hd_blit_sprite.c`
- `src/hd/hd_blit_bg.c` → `tests/test_hd_blit_bg.c`
- `src/hd/hd_mode7.c` → `tests/test_hd_mode7.c` (V2.2)
- `src/hd/hd_compositor.c` → `tests/test_hd_compositor.c`
- HD config parser → `tests/test_hd_config.c`
- Display-only invariant → `tests/test_hd_display_only.c`

---

## 7) Validation workflow per step

Per step, in order:

1. `make clean && make -j$(nproc) all` — must succeed with no warnings.
2. `make test` — all suites pass.
3. `make coverage` (after V2.0) — module gates pass.
4. Runtime sanity:
   - One stock gameplay level with active sprites (e.g. 1-1).
   - One overworld transition (level → map → level).
   - One Mode 7 scene (V2.2 onward).

If a step changes mapping logic, gate any new debug dump behind
`HD_DEBUG_DUMP` and leave it off in the merged build.

---

## 8) Known remaining risks

- Mode 7 parity depends on matching PPU fixed-point behavior exactly.
  Test vectors in V2.2 must be derived from the SD PPU output, not
  reimplemented from scratch.
- Stock-ROM upload-path coverage may still miss niche scenes (intro
  cutscene, credits). V2.3 audit covers gameplay + overworld; cutscene
  coverage is opportunistic.
- Lunar Magic ROM hacks are out of scope and may show placeholder content
  in unexpected places. Document in user-facing release notes when V2 ships.
- Coverage gates can mask logic bugs if tests happen to drive the same
  paths the production code uses. Display-only invariant test (V2.5-B)
  is the structural backstop.

---

## 9) Definition of done for this spec

This spec is fulfilled when:
- Steps V2.0 through V2.5 are completed in order.
- HD path covers Mode 1 + Mode 7 with placeholder safety net.
- Unit-test and coverage gates are met for all touched HD modules.
- `smw.ini`, `CLAUDE.md`, and developer docs reflect final V2 behavior.
- `make clean && make -j$(nproc) all && make test && make coverage`
  succeeds from a fresh checkout.
