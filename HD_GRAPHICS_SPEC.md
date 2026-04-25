# HD Graphics — Implementation Spec

Per-step implementation plan for replacing sprite rendering with HD replacements loaded from `gfx/hd/`. Each step is self-contained; a fresh session (including a smaller/cheaper model like Sonnet) should be able to read the step in isolation and complete it.

Read this Overview and Architecture before picking up any step.

---

## Overview

**Goal:** display HD sprite replacements (integer scale, greyscale-palette-index-encoded PNGs in `gfx/hd/`) in place of the PPU's SD sprite output. Backgrounds continue to render from the PPU at SD, upscaled nearest. HD sprites composite on top with per-layer visual effects (e.g., drop shadows).

**Scope (v1):** sprites only. ✅ **COMPLETE** (Steps 1–7 done). No background replacement, no mode-7 handling, no Lunar Magic support.

**Non-goals (v1):**
- Preserving BG→sprite priority interleaving. HD sprites render on top of all BG regardless of BG priority bits. Accepted limitation; addressed in v2.
- GPU compositor. CPU-only. Port to GPU is a later project.
- BG replacement, mode 7, color-math accuracy. BG replacement is addressed in v2 (Steps 8–12).

**Verification path:** the reference SNES emulator compares RAM each frame. HD display is pixel-only — no game-state mutation — so verification is unaffected. Do not introduce state mutations into the HD path.

---

## Architecture

### Data flow (per frame, HD on)

1. Game runs; writes OAM/VRAM/CGRAM via PPU register hooks.
2. `g_rtl_game_info->draw_ppu_frame()` runs `ppu_runLine` for each scanline, producing SD output in `g_my_pixels`. When HD is on, `PpuDrawSprites` is **skipped** so `g_my_pixels` contains BG+backdrop only.
3. HD compositor (`HdCompositor_Draw`):
   a. Nearest-upscale `g_my_pixels` into the HD frame buffer (`256·S × 224·S × 4 BGRA`).
   b. Walk OAM → build `HdScene` (one entry per visible OAM sprite, tagged with layer = OAM priority).
   c. For each layer, low → high: composite layer into a temp HD buffer, run effect passes on it, alpha-blit onto main HD buffer.
4. `OpenGLRenderer_BeginDraw` already sizes its backing buffer from `PpuGetCurrentRenderScale`, so the final frame goes through to the GL texture at HD dimensions with no changes.

### Key existing code pointers

- [src/main.c:179](src/main.c#L179) `RtlDrawPpuFrame` — entry point. Runs PPU (via `draw_ppu_frame`) then memcpys SD → destination. **Modified in Step 1.**
- [src/snes/ppu.c:1149](src/snes/ppu.c#L1149) `PpuGetCurrentRenderScale` — hardcoded return 1. **Modified in Step 1.**
- [src/snes/ppu.c:630](src/snes/ppu.c#L630) `PpuDrawBackgrounds` (mode 1 branch) — calls `PpuDrawSprites`. **Modified in Step 1** (gated skip).
- [src/snes/ppu.c:752](src/snes/ppu.c#L752) `ppu_evaluateSprites` — reference implementation of the OAM walk and tile-addressing logic. Mirror its `charnum`/`obsel`/high-OAM decoding in Step 4.
- [src/snes/ppu.c:590](src/snes/ppu.c#L590) `PpuDrawSprites` — reference for sprite→screen composite order.
- [src/snes/ppu.h:67](src/snes/ppu.h#L67) `Ppu` struct (oam[256], highOam[32], cgram[256], vram[32768], obsel, inidisp). Access `g_snes->ppu` or `g_my_ppu`.
- [src/types.h:109](src/types.h#L109) `OamEnt {xpos, ypos, charnum, flags}`. Note: this is the *game-side* 4-byte layout. The *PPU-side* layout is `ppu->oam[128]` uint16 pairs plus `highOam[32]` bits (x-high + size).
- [src/opengl.c:147](src/opengl.c#L147) `OpenGLRenderer_BeginDraw` — allocates `width × height × 4` BGRA. Already scale-agnostic. **No changes needed.**
- [src/smw_rtl.h:18-22](src/smw_rtl.h#L18-L22) `SmwCopyToVram*` family — VRAM upload wrappers. Hook site for Step 3.
- [assets/export_sheets.py](assets/export_sheets.py) — **authoritative spec for the HD PNG format.** Read it. Index↔grey encoding, transparency convention, tile layout.
- [Makefile](Makefile) `SRCS:=$(wildcard src/*.c src/snes/*.c)` — new `.c` files in `src/` are picked up automatically.

### HD PNG format (from export_sheets.py)

- RGBA PNG, tile grid is **16 tiles wide × N tiles tall** (N ≥ 1). Width is fixed at 16 tiles; height is variable per sheet.
- Each source tile is 8×8 SNES pixels. At scale `S`, tile is `8S × 8S` pixels, sheet is `128S × (8S·N)` pixels, sheet holds `16·N` tiles.
- Per pixel: R == G == B == `grey`; alpha 0 means transparent (palette index 0). Otherwise `index = round(grey * max / 255)` where `max = 15` for 4bpp sprites.
- "Scale" = HD PNG width / 128. Must be a positive integer. Height must be a positive multiple of `8S`.
- Sprites in SMW are always 4bpp (OBJ tiles). Indices 0..15. Index 0 = transparent.

### Global state additions (cross-cutting)

These globals are introduced progressively across steps. Declared in `src/hd_compositor.h`, defined in `src/hd_compositor.c`. Other files `#include` the header.

```c
// src/hd_compositor.h
extern bool   g_hd_enabled;        // set by config; when true, HD pipeline active
extern uint8  g_hd_scale;          // 1..N; 1 = SD only; updated at HD asset load
extern bool   g_hd_skip_sprites;   // ppu.c reads this; when true, PpuDrawSprites is skipped
```

`g_hd_skip_sprites` is set equal to `g_hd_enabled` at frame start. Separate variable so tests/dev can toggle independently.

---

## Terminology

- **SD**: 256 × 224 (or 240) native SNES resolution, 4 BGRA bytes per pixel.
- **HD**: `256·S × 224·S`, `S ∈ {2, 3, 4, 6, 8, ...}`.
- **Sheet**: one of the ~52 SMW GFX slots (`gfx00.png` … `gfx33.png`). Always 16 tiles wide; tile-row count is per-sheet.
- **Tile**: 8×8 native pixel block. In an HD sheet, `8S × 8S` pixels.
- **Slot→VRAM mapping**: record of which sheet last uploaded to each VRAM tile address.
- **Scene**: per-frame list of sprite draw calls derived from OAM.
- **Layer**: `0..3`, assigned from OAM priority. Effect passes are per-layer.

---

## Step 1 — HD scale plumbing + stub compositor ✅ DONE

**Goal:** make the pipeline run at an HD scale with the existing SD output nearest-upscaled. No HD assets yet. Proves the plumbing works end-to-end.

**Read first:**
- [src/main.c:179-216](src/main.c#L179-L216)
- [src/snes/ppu.c:1149-1151](src/snes/ppu.c#L1149-L1151)
- [src/snes/ppu.c:614-656](src/snes/ppu.c#L614-L656) (PpuDrawBackgrounds to understand the sprite-skip gate)

**Tasks:**

1. Create `src/hd_compositor.h` with the globals listed above and:
   ```c
   void HdCompositor_Init(void);                                  // called once at startup
   void HdCompositor_Draw(uint8 *dst, size_t pitch,
                          const uint8 *sd_pixels,                  // g_my_pixels (256·4·H bytes)
                          int sd_width, int sd_height);
   ```
2. Create `src/hd_compositor.c`:
   - Define the globals. Default `g_hd_enabled = true`, `g_hd_scale = 4` for now (Step 7 hooks config).
   - `HdCompositor_Init` is a no-op for now.
   - `HdCompositor_Draw` nearest-upscales `sd_pixels` (width `sd_width`, height `sd_height`, pitch `sd_width*4`) into `dst` (pitch `pitch`) by factor `g_hd_scale`.
3. Modify [src/snes/ppu.c:1149](src/snes/ppu.c#L1149) `PpuGetCurrentRenderScale` to return `g_hd_enabled ? g_hd_scale : 1`. Add the forward decl or `#include "../hd_compositor.h"`.
4. Modify [src/snes/ppu.c:630-649](src/snes/ppu.c#L630-L649) `PpuDrawBackgrounds` mode-1 branch:
   - Wrap the `if (ppu->lineHasSprites) PpuDrawSprites(...)` call in `if (!g_hd_skip_sprites) { ... }`.
   - Leave mode 7 branch alone.
5. Modify [src/main.c:179](src/main.c#L179) `RtlDrawPpuFrame`:
   - Before `g_rtl_game_info->draw_ppu_frame()`, set `g_hd_skip_sprites = g_hd_enabled`.
   - If `g_hd_enabled`: call `HdCompositor_Draw(pixel_buffer, pitch, ppu_pixels, g_snes_width, g_snes_height)`.
   - Else: keep the existing memcpy loop.
6. Modify [src/main.c:465-466](src/main.c#L465-L466) `PpuBeginDrawing` calls — confirm `g_pixels` / `g_my_pixels` stay at SD pitch `256*4`. They do; no change needed (PPU always renders SD; compositor handles the scale-up).
7. Call `HdCompositor_Init()` once at startup in `main` near where other renderer init happens.

**Gotchas:**
- `g_my_pixels` is `256 * 4 * 240` static; PPU always writes 256 wide. Don't conflate `g_snes_width` (which may become 256+extraLR if overscan) with the PPU stride.
- `PpuGetCurrentRenderScale` is called from [src/main.c:188](src/main.c#L188) before each frame — value must be stable for the frame; don't toggle mid-frame.
- `g_hd_skip_sprites` in ppu.c: declare `extern bool g_hd_skip_sprites;` near the top (or add an include). The PPU module is C, not C++; simple extern works.
- On-screen fonts ([src/main.c:210-213](src/main.c#L210-L213) `RenderNumber`) write into the HD buffer at SD-pitch positions, so they still work positionally but render tiny. Acceptable for v1.

**Acceptance:**
- ✅ Build clean.
- ✅ Game window opens at HD scale (e.g. 4×). Backgrounds visible, chunky-upscaled. No sprites visible. Game plays (verified by Mario's inputs moving the camera, BG tiles scrolling).
- ✅ Toggle `g_hd_enabled = false` at compile time → SD returns, sprites reappear.

**Post-completion fix:** `SdlRenderer_BeginDraw` heap overflow — SDL texture was created at 256×224 but written at HD dimensions. Fixed by recreating the texture when dimensions change, mirroring `OpenGLRenderer_BeginDraw`.

---

## Step 2 — HD PNG loader ✅ DONE

**Goal:** load all `gfx/hd/gfxNN.png` at startup into palette-index buffers; keep per-sheet metadata. Missing sheets are OK — recorded as "no HD".

**Read first:**
- [assets/export_sheets.py](assets/export_sheets.py) — **authoritative encoding**.
- Step 1 outputs.

**Tasks:**

1. Add `third_party/stb_image.h` (single-header, public domain). Source: https://github.com/nothings/stb — download the pinned file `stb_image.h` and drop in. Add to `.gitignore` if prudent (or commit).
2. Create `src/hd_gfx.h`:
   ```c
   typedef struct HdSheet {
     uint8 *index_buffer;   // width*height bytes; one index per HD pixel; 0 = transparent
     uint16 width;          // scale * 128 (always 16 tiles wide)
     uint16 height;         // scale * 8 * num_tile_rows (variable per sheet)
     uint16 tile_count;     // total 8x8 tiles in the sheet = (width / 8S) * (height / 8S) = 16 * num_tile_rows
     uint8  scale;          // 1..N
     bool   loaded;
   } HdSheet;

   enum { kHdSheetCount = 0x34 };   // 0x00..0x33 (matches export_sheets.py)

   extern HdSheet g_hd_sheets[kHdSheetCount];

   void HdGfx_LoadAll(const char *hd_dir);
   void HdGfx_Free(void);
   ```
3. Create `src/hd_gfx.c`:
   - `HdGfx_LoadAll("gfx/hd")`:
     - For i = 0..0x33: try `gfx/hd/gfx%02x.png`. If missing, `loaded = false`, continue.
     - Use `stb_image.h` to decode as RGBA.
     - Validate: `width % 128 == 0`, `scale = width / 128 ≥ 1`, `height % (8 * scale) == 0`, `height ≥ 8 * scale`. If validation fails, warn via `fprintf(stderr, ...)` and skip.
     - Allocate `index_buffer = malloc(width * height)`; for each pixel convert `(r, a)` → index:
       - If `a == 0`: `index = 0`.
       - Else: `index = (r * 15 + 127) / 255;` (round to nearest; matches `round(grey * 15 / 255)` from the encoder).
     - Populate `HdSheet` (including `tile_count = (width / (8*scale)) * (height / (8*scale)) = 16 * (height / (8*scale))`) and mark `loaded = true`. Log sheet id, dims, scale.
   - Determine effective global scale: set `g_hd_scale` (from Step 1) to the max scale across loaded sheets, or fall back to 1 if none loaded. (Multiple scales coexist at compositor time; the frame buffer is sized to the maximum.)
   - **Or** require a single uniform scale — simpler for v1. Go with uniform: first loaded sheet's scale sets `g_hd_scale`; reject other-scaled sheets with a warning.
   - `HdGfx_Free`: free every non-null `index_buffer`.
4. Call `HdGfx_LoadAll("gfx/hd")` from `HdCompositor_Init` (or after config loads, if that runs earlier). Call `HdGfx_Free` on shutdown.

**Gotchas:**
- `stb_image` is single-header; exactly one `.c` must `#define STB_IMAGE_IMPLEMENTATION` before the include. Do this in `src/hd_gfx.c`.
- Reject non-integer scale (e.g., a 1023×512 PNG). Log and skip.
- Don't crash on PNGs with indexed color mode — stb_image expands to RGBA automatically.
- HD sheets are large: at scale=8, one `16 × N`-tile sheet uses `1024 × 64·N` bytes (e.g. N=8 → 512 KB). A full set of 52 stacked 8-row sheets is ~27 MB; variable-height sheets scale linearly with row count. Acceptable.
- The tile index layout within a sheet matches source: row-major, 16 tiles per row. Store flat (no per-tile chunking); compositor computes tile offsets on the fly.

**Acceptance:**
- ✅ Build clean.
- ✅ With HD PNGs in `gfx/hd/`, startup logs show loaded sheets with correct scale.
- ✅ With `gfx/hd/` empty, `g_hd_scale` stays 1 and game runs at SD.

**Post-completion note:** `glsl_shader.c` already defined `STB_IMAGE_IMPLEMENTATION`, so `hd_gfx.c` omits the define and just includes the header directly.

---

## Step 3 — Slot→VRAM tracking ✅ DONE

**Goal:** at any point during play, given a VRAM word-address, know which source sheet and which tile-within-sheet was last uploaded there.

**Why:** OAM entries reference VRAM tile numbers (`charnum` + OBJ tile base). To pick the right HD tile we need to resolve that back to `(sheet_id, tile_in_sheet)`.

**Read first:**
- [src/smw_00.c:2694-2763](src/smw_00.c#L2694) `UploadGraphicsFiles_UploadGFXFile` — the bulk sprite/BG sheet uploader. Read all of it.
- [src/smw_00.c:2656-2692](src/smw_00.c#L2656) `UploadGraphicsFiles` — the per-level driver that invokes the above.
- [src/smw_00.c:3014-3041](src/smw_00.c#L3014) `GraphicsDecompressionRoutines_DecompressGFX32And33` — Mario sheet staging to `g_ram + 0x2000`.
- [src/smw_00.c:2307-2327](src/smw_00.c#L2307) `UploadPlayerGFX` — per-frame Mario dynamic tile updates via `SmwCopyToVram` from `g_ram + 0x2000`.
- [src/smw_00.c:2359-2364](src/smw_00.c#L2359) `RestoreSP1AfterMarioStart` — more Mario slots from `g_ram + 0xbf6 / 0xcb6`.
- [src/common_rtl.c:821-834](src/common_rtl.c#L821) `SmwCopyToVram` family.
- [src/snes/ppu.c:752-831](src/snes/ppu.c#L752-L831) `ppu_evaluateSprites` — confirms how `objAdr + usedTile * 16` indexes VRAM.

**Context (result of investigation — the original spec's assumption was wrong):**

Sprite/BG graphics reach VRAM via **two** distinct paths; hooking only `SmwCopyToVram` misses the primary one.

- **Path A — bulk per-level sheets (primary for sprites).** `UploadGraphicsFiles_UploadGFXFile(dst_addr, j, index)` calls `GraphicsDecompress(j)` (which decompresses to the transient buffer `g_ram + 0xad00`), then runs an **inline 3bpp→4bpp inflation loop** that writes directly via `uint16 *dst = RtlGetVramAddr() + dst_addr`. It **never** calls `SmwCopyToVram`. Per level, `UploadGraphicsFiles` invokes this 4× for sprite slots and 4× for FG/BG slots, each writing 128 tiles × 32 bytes = 0x800 word-addresses. Sheet id comes straight from `j`.
- **Path B — dynamic per-frame updates (Mario + animated tiles).** `SmwCopyToVram` variants copy from `g_ram + offset` staging areas. Sources:
  - **Mario** (`UploadPlayerGFX`, `RestoreSP1AfterMarioStart`): reads from `g_ram + 0x2000` (sheet 0x32 pre-decompressed there by `DecompressGFX32And33`) and from `g_ram + 0xbf6 / 0xcb6` (also sheet 0x32 regions).
  - **Animated tiles** (`UploadLevelExAnimationData`): reads from `g_ram + graphics_tile_anim_source_address*` — these indices point into variable locations produced by a level-tile-animation subsystem. Typically BG, not sprite; for v1 we can skip resolution and let these come back unmapped.
  - **Tilemap data** (status bar, blocks layers): source does not correspond to any sheet. Skip.

Sheets 0x32 / 0x33 are pre-decompressed 4bpp (Mario). Sheets 0x00-0x31 are 3bpp on disk and get inflated to 4bpp only along Path A.

**Revised strategy:**

Primary mechanism is a **direct hook inside `UploadGraphicsFiles_UploadGFXFile`** that records `(sheet=j, vram_word_addr=dst_addr, tile_count=128, src_tile_offset=0)` once per call. This covers every per-level sprite and BG sheet with no guessing.

Secondary mechanism is a **staging-buffer registry + `SmwCopyToVram` hook** for dynamic Mario tiles. Register `g_ram + 0x2000` as sheet 0x32's staging base at the point `DecompressGFX32And33` runs. `SmwCopyToVram` looks up its `src` pointer in the registry; if it falls inside a staging range, record a region; otherwise skip (not sheet data). Mario's `0x64a0/0x65a0` uploads from `g_ram + 0xbf6/0xcb6` also fall outside the `g_ram + 0x2000` range — either register a second Mario staging range (if that RAM area is a stable mirror) or accept those slots as unmapped in v1 and fall back to SD.

The PNG is laid out in source-tile order, 16 tiles per row. Path A always writes 128 VRAM tiles contiguously, so VRAM-tile N maps to sheet-tile N (for N < `sheet->tile_count`); sheets with fewer than 128 tiles leave the tail unmapped (fall back to SD), and sheets with more than 128 tiles have their extra tiles unreachable from a single bulk region. In both paths `tile_in_sheet = (vram_word_addr - region_base) / 16` — no bpp arithmetic needed (Path A is 4bpp at the VRAM side; Path B is 4bpp on both sides).

**Tasks:**

1. Create `src/hd_vram_map.h`:
   ```c
   // Represents a single contiguous VRAM region whose content was uploaded from a known sheet.
   typedef struct HdVramRegion {
     uint16 vram_word_addr;   // base VRAM word address
     uint16 tile_count;       // number of 16-word VRAM tiles (4bpp)
     uint8  sheet_id;         // 0..0x33
     uint16 src_tile_offset;  // first tile within the sheet covered by this region
   } HdVramRegion;

   void HdVramMap_Reset(void);

   // Path A: called from UploadGraphicsFiles_UploadGFXFile once per bulk upload.
   // dst_word_addr and tile_count are in 4bpp VRAM tile units (16 words each).
   void HdVramMap_RecordSheetUpload(uint16 dst_word_addr, uint8 sheet_id,
                                    uint16 src_tile_offset, uint16 tile_count);

   // Path B: called from SmwCopyToVram* after the copy. Looks src up in the
   // staging registry; if unmapped, the call is a no-op.
   void HdVramMap_RecordCopyFromStaging(uint16 dst_word_addr, const uint8 *src, int byte_count);

   // Staging registry — register fixed RAM regions that hold a known sheet (4bpp, 32 B/tile).
   void HdVramMap_RegisterStaging(uint8 sheet_id, const uint8 *base, size_t size,
                                  uint16 sheet_tile_base);

   // Resolve a VRAM tile word-address to (sheet, tile_in_sheet). false if unmapped.
   bool HdVramMap_ResolveTile(uint16 vram_word_addr, uint8 *sheet_out, uint16 *tile_out);
   ```
2. Create `src/hd_vram_map.c`:
   - Internal state: an array of `HdVramRegion` (cap ~128 — one per sheet slot is plenty) plus a small staging registry array.
   - Regions are written in most-recent-last order. `ResolveTile` scans from the end and returns the first region whose `[vram_word_addr, vram_word_addr + tile_count*16)` contains the query address. No merging needed for correctness; add merge later if the list grows long.
   - `HdVramMap_RecordSheetUpload` just appends a region. If a prior region fully overlaps, mark it stale (or leave it — newer entries win on scan-from-end).
   - `HdVramMap_RecordCopyFromStaging`:
     - Walk the staging registry. If `src` lies in `[base, base+size)`, compute `byte_offset = src - base`. Require 32-byte alignment (skip otherwise — partial-tile or unaligned uploads aren't sheet data).
     - Append a region with `sheet_id` from the registry, `src_tile_offset = sheet_tile_base + byte_offset/32`, `tile_count = byte_count/32`.
     - If `src` is not in any staging range, skip.
   - `HdVramMap_Reset` clears regions (not the staging registry).
3. Hook Path A — edit `UploadGraphicsFiles_UploadGFXFile` in [src/smw_00.c:2694](src/smw_00.c#L2694):
   - At the end of the function (after both tile-loop branches complete), add:
     ```c
     // dst_addr is the VRAM *word* addr; the function always writes 128 4bpp tiles.
     HdVramMap_RecordSheetUpload(dst_addr, j, 0, 128);
     ```
   - This runs for the Lunar-Magic-4bpp branch too; `j` is still the sheet id in that case.
   - Do **not** record when `lunar_magic_upload_hack` is true (LmHook returned a custom buffer that may not correspond to a vanilla sheet) — guard with `if (!lunar_magic_upload_hack) HdVramMap_RecordSheetUpload(...);`. v1 scope is no-LM anyway.
4. Hook Path A — edit `UploadGraphicsFiles_Layer3` in [src/smw_00.c:2648](src/smw_00.c#L2648): the layer-3 uploader calls `SmwCopyToVram(0x4000 + i * 0x400, GraphicsDecompress(40 + i), 0x800)` with `p0 = g_ram + 0xad00` (the transient decomp buffer). For v1 (sprites-only), **do not** try to record these — the transient buffer is reused across sheets and doesn't fit the staging model. Accept layer-3 text as unmapped.
5. Hook Path B — modify the `SmwCopy*` family in [src/common_rtl.c:821](src/common_rtl.c#L821):
   - After each copy completes, call `HdVramMap_RecordCopyFromStaging(vram_addr, src, n)`.
   - For `SmwCopyToVramLow` (low-byte only): skip recording. It mutates existing tile data rather than uploading a new tile.
   - For `SmwCopyToVramPitch32`: skip for v1. It's striped writes used for tilemap/overworld data, never for tile graphics in the sprite OBJ range.
6. Register Mario's staging buffer. In [src/smw_00.c:3014](src/smw_00.c#L3014) `GraphicsDecompressionRoutines_DecompressGFX32And33`, after the `memcpy(g_ram + 0x2000, kGfx32, kGfx32_SIZE);` calls, add:
   ```c
   HdVramMap_RegisterStaging(0x32, g_ram + 0x2000, kGfx32_SIZE, 0);
   ```
   And for the `kGfx33` memcpy to `g_ram + 0x7d00` (LM 4bpp branch): `HdVramMap_RegisterStaging(0x33, g_ram + 0x7d00, kGfx33_SIZE, 0);` — but skip for v1 (LM only).
7. Reset regions on `HdVramMap_Reset` — call from `HdCompositor_Init` at startup. Optionally also on level load, but not required for correctness since newer entries win.

**Gotchas:**
- **3bpp→4bpp inflation is handled by the upload hook, not the map.** Path A's destination is 32 B/tile in VRAM; the HD PNG is indexed by source tile number (0..127), which *is* the VRAM tile number for a 128-tile bulk upload. So `src_tile_offset = 0` and `tile_in_sheet = (vram_word_addr - region.vram_word_addr) / 16` — no bpp arithmetic needed.
- **Path B is already 4bpp on both sides.** Mario's staging (`g_ram + 0x2000`) is the 4bpp `kGfx32` memcpy'd verbatim. VRAM upload is byte-for-byte. 32 B/tile on both sides.
- **Special tile cases in `UploadGraphicsFiles_UploadGFXFile`** (j==8 / j==30 / j==0x32 with tileset ≥0x11): these follow a different inner loop but still produce exactly 128 tiles in VRAM from the 128 source tiles. The recording `(sheet=j, tile_count=128, src_tile_offset=0)` is still correct. The per-tile bit-swizzling differences are invisible at the sheet→VRAM-tile-index level — they affect which palette bits each pixel gets, which HD rendering handles by reading the PNG index directly.
- **`SmwCopyToVram` tilemap calls** (e.g. `kStatusBarTilemap_*`, `blocks_layer*_vramupload_address`): source doesn't lie in any registered staging range → naturally skipped.
- **Empty/placeholder uploads** ([smw_00.c:2311-2324](src/smw_00.c#L2311-L2324) with `t == 0` using `g_ram + 0x2000` as the fallback): these *do* land in sheet 0x32's staging range and would be spuriously recorded. Guard in the map: if `src == g_ram + 0x2000` *and* the original `t` was zero, skip. Simplest: inside `RecordCopyFromStaging`, reject `src_tile_offset == 0` with `tile_count == 2` as a heuristic — *or* better, add a small branch at the call site that skips the record when the pointer is the placeholder. The cleanest fix is to only record when `t != 0` at the `UploadPlayerGFX` call sites (four lines). Prefer that.
- **VRAM geometry.** VRAM is 32K words; OBJ tile base is `(obsel & 7) << 13` (words) — each unit is a 16 KB (0x2000-word) region. See `PPU_objTileAdr1` in [src/snes/ppu.h:168](src/snes/ppu.h#L168). Typical OBJ base for SMW: word 0x6000. The map stores word-addresses, so no unit confusion.
- **Regions overlap on purpose.** A per-frame Mario upload writes into 0x6000-0x60FF *inside* the 0x6000-0x67FF region previously recorded by `UploadGraphicsFiles_UploadGFXFile` for sheet 0x32. Scan-from-end ensures the newer Mario region wins, which is what we want.
- **Scan order.** `ResolveTile` must iterate newest→oldest so the most recent upload for a given address is returned.

**Acceptance:**
- Add a debug dump command (e.g. bind to F10) that prints the current vram→sheet map: one line per region `(sheet_id, vram_base, tile_count, src_tile_offset)`.
- Load level 1-1 (Yoshi's Island 1): dump should show 4 sprite-slot regions at 0x6000/0x6800/0x7000/0x7800 with sheet ids from `kUploadGraphicsFiles_SpriteGFXList` (level 0 = sprite graphics setting 0 → entries 0..3 of the list). Cross-reference with `gfx/source/gfx*.png` to sanity-check.
- After Mario moves, dump should also show small per-frame regions inside 0x6000-0x67FF tagged as sheet 0x32.
- `HdVramMap_ResolveTile(objAdr + oamCharnum*16, ...)` for a visible Mario sprite returns `(sheet=0x32, tile ≈ charnum & 0x7f)`. For a visible enemy sprite it returns one of the four sprite-slot sheets with `tile = charnum - (vram_slot_base / 16)`.

---

## Step 4 — Scene build  ✅ DONE

**Goal:** once per frame, walk OAM and produce a list of `HdSprite` entries describing what to draw. Scene holds the sprite's VRAM top-left tile word-address; the compositor (Step 5) resolves each 8×8 sub-tile against the HD VRAM map so sprites that span multiple recorded regions (e.g. Mario dynamic tiles next to a Path-A slot) render correctly.

**Read first:**
- [src/snes/ppu.c:752-831](src/snes/ppu.c#L752-L831) `ppu_evaluateSprites` — copy the OAM-decoding logic.
- [src/snes/ppu.h:167-172](src/snes/ppu.h#L167-L172) — `PPU_objSize`, `PPU_objTileAdr1/2`, `PPU_objPriority`.
- Step 3 outputs.

**Key fact:** `PPU_objTileAdr1` returns a **word** address (see [ppu.h:168](src/snes/ppu.h#L168): `(obsel & 7) << 13` — `<< 13` words = 16 KB). `ppu->vram` is `uint16_t[0x8000]` and [ppu.c:809](src/snes/ppu.c#L809) indexes it directly with `objAdr + usedTile * 16`. **Do not divide `objAdr` by 2.**

**Tasks:**

1. Create `src/hd_scene.h`:
   ```c
   typedef struct HdSprite {
     int16  x, y;          // SD pixel coords, top-left. May be negative / offscreen; clip in compositor.
     uint8  size;          // sprite size in SD pixels (8, 16, 32, or 64). Square.
     uint8  palette;       // 0..7; CGRAM base = 0x80 + palette*16
     uint8  flags;         // bit0: hflip, bit1: vflip
     uint8  layer;         // 0..3 (OAM priority)
     uint16 tile_vram;     // VRAM word-addr of the sprite's top-left 8×8 tile.
                           // Compositor re-resolves per sub-tile via HdVramMap_ResolveTile.
     uint8  _pad[2];
   } HdSprite;

   enum { kHdSceneMax = 128 };

   typedef struct HdScene {
     HdSprite sprites[kHdSceneMax];
     uint16   count;
   } HdScene;

   void HdScene_Build(HdScene *scene, const Ppu *ppu);
   ```
   Note: we store `tile_vram` (the VRAM word-address of the top-left tile) rather than a resolved `(sheet, tile_in_sheet)` pair. Step 5 walks the sub-tile grid in VRAM space and resolves each one independently. This fixes sprites that straddle two HD-map regions (common for Mario: Path-B dynamic tiles inside a Path-A sprite slot).

2. Copy the `spriteSizes` table from [ppu.c:754-757](src/snes/ppu.c#L754-L757) into `hd_scene.c`:
   ```c
   static const uint8 spriteSizes[8][2] = {
     {8, 16}, {8, 32}, {8, 64}, {16, 32},
     {16, 64}, {32, 64}, {16, 32}, {16, 32}
   };
   ```
   All entries are square sides. Treat sprites as square.

3. Create `src/hd_scene.c`. `HdScene_Build`:
   - `scene->count = 0;`
   - OAM stride matches [ppu.c:762](src/snes/ppu.c#L762): `ppu_evaluateSprites` walks `index = 0..254 step 2` (128 sprites, 2 words each — `ppu->oam[0x100]` total). Sprite *i* lives at `oam[i*2]` (y<<8 | x-low) and `oam[i*2 + 1]` (tile | attrs).
   - High-OAM stride: `index = i*2`, so `highOam[index >> 3] == highOam[i >> 2]`. The 2 bits for sprite *i* sit at bit positions `(index & 7)` (x-high) and `(index & 7) + 1` (size-select) — equivalently `(i & 3)*2` and `(i & 3)*2 + 1`. Extract:
     ```c
     uint8 hi    = (ppu->highOam[i >> 2] >> ((i & 3) * 2)) & 3;
     bool  xhigh = hi & 1;
     bool  large = hi & 2;
     ```
   - For each `i` in `0..127`:
     ```c
     uint16 oam0 = ppu->oam[i*2];
     uint16 oam1 = ppu->oam[i*2 + 1];

     int16 x = oam0 & 0xff;
     x |= xhigh << 8;
     if (x > 255) x -= 512;                     // sign-extend 9-bit
     uint8 y = oam0 >> 8;

     uint8 size = spriteSizes[PPU_objSize(ppu)][large ? 1 : 0];

     uint8  tile_num      = oam1 & 0xff;
     bool   charnum_high  = (oam1 >> 8) & 1;
     uint16 objAdr        = charnum_high ? PPU_objTileAdr2(ppu)   // already in words
                                         : PPU_objTileAdr1(ppu);
     uint16 tile_vram     = (objAdr + tile_num * 16) & 0x7fff;    // 4bpp tile = 16 words

     uint8 palette = (oam1 >> 9) & 7;
     uint8 flags   = (oam1 >> 14) & 3;          // bit 0 = hflip, bit 1 = vflip
     uint8 layer   = (oam1 >> 12) & 3;
     ```
   - Skip hidden sprites: in SMW a sprite with `y == 0xF0` is the conventional "hidden" marker. Also skip anything whose bbox falls entirely outside `[0, 256) × [0, sd_height)`.
   - Append to `scene->sprites[scene->count++]` if `scene->count < kHdSceneMax`.
   - Do **not** resolve `HdVramMap_ResolveTile` here — leave the VRAM address on the struct; the compositor resolves per sub-tile.

4. In `HdCompositor_Draw`: declare a file-static `HdScene g_hd_scene;` and call `HdScene_Build(&g_hd_scene, g_my_ppu)` before the sprite composite pass (Step 5).

**Gotchas:**
- **OAM priority rotation (`PPU_objPriority(ppu)`) is NOT handled.** Real PPU starts OAM iteration at `oamaddl & 0xfe` when bit is set. SMW gameplay rarely uses this; accept as a v1 limitation and document in Known Limitations.
- **`y == 0xF0` hide is a heuristic**, not a match for PPU. PPU renders any sprite whose `row = line - y` falls in `[0, spriteHeight)`. The heuristic is safe for SMW's conventions; a bbox offscreen test provides a backstop.
- **Size table entries are all square** (`{8,16}`, `{8,32}`, …). V1 treats `size` as one number (square side).
- **`objAdr` is in words.** Every mention of "byte address" for OAM tile base is wrong — see Key Fact above.
- **Sub-tile / sheet layout.** Per [ppu.c:807-808](src/snes/ppu.c#L807-L808), tiles within a multi-8×8 sprite wrap on a 16-tile row: `usedTile = ((((tile & 0xff) >> 4) + (row >> 3)) << 4) | (((tile & 0xf) + (col >> 3)) & 0xf)`. Rows do not wrap — a 32×32 sprite at `tile_num=0xf0` references tile indices past 0xff (into the next VRAM page). The compositor uses the same formula operating on VRAM word-addresses.

**Acceptance:**
- Add an `#ifdef HD_DEBUG` dump (e.g. F9) printing `g_hd_scene`: index, `(x, y)`, `size`, `tile_vram`, `palette`, `flags`, `layer`.
- Run Yoshi's Island 1: scene count matches the visibly drawn sprite count (Mario is ~4 sprites, plus any enemies / powerups / HUD sprites). Mario's `tile_vram` should fall inside a region the VRAM map tags as sheet 0x32.

---

## Step 5 — CPU compositor (baseline, no effects) ✅ DONE

**Goal:** HD sprites actually appear on screen. Palette applied via current CGRAM. Sprites render on top of the upscaled BG. Each 8×8 sub-tile is resolved independently against the HD VRAM map so sprites spanning Path-A / Path-B regions still render correctly.

**Read first:**
- Step 1, 2, 4 outputs.
- [src/snes/ppu.c:693-710](src/snes/ppu.c#L693-L710) final composite pass — reference for CGRAM + brightness math.
- [src/snes/ppu.c:787-826](src/snes/ppu.c#L787-L826) — reference for the vflip-row / hflip-col / usedTile logic this step mirrors.

### Iteration order (nail this down first)

PPU behavior ([ppu.c:821](src/snes/ppu.c#L821)): within a priority band, `ppu_evaluateSprites` walks OAM `i = 0..127` and *only writes a pixel if the target is still empty*. Result: **lower-index OAM wins at overlap.**

To reproduce this with an alpha-overwrite model, iterate each layer from **highest OAM index down to lowest** (later-index painted first, then earlier-index painted on top). Between layers, go low layer → high layer so higher OAM-priority lands above.

```c
for (int layer = 0; layer < 4; layer++) {
  for (int k = g_hd_scene.count - 1; k >= 0; k--) {
    HdSprite *s = &g_hd_scene.sprites[k];
    if (s->layer != layer) continue;
    // … composite s into HD buffer …
  }
}
```

### Sprite composite (per sprite, output-space walk)

Walk *output* pixel offsets `(row, col)` in `[0, size)`; map each to a source pixel via the flip rules, then to a VRAM tile, then to `(sheet, tile_in_sheet)` via `HdVramMap_ResolveTile`, then to an HD index, CGRAM color, brightness-mapped BGRA.

```c
int     S            = g_hd_scale;
bool    hflip        = s->flags & 1;
bool    vflip        = s->flags & 2;
uint16  base_vram    = s->tile_vram;              // top-left 8×8 tile, word addr
uint8   tile_hi      = (base_vram / 16) >> 4;     // = (charnum >> 4) of top-left tile
uint8   tile_lo      = (base_vram / 16) & 0xf;    // = (charnum & 0xf)
uint16  obj_page     = base_vram & ~0xff;         // 256-word page containing the top-left tile
                                                  // (used to mask the col-wrap back into VRAM)
int     palette_base = 0x80 + s->palette * 16;
uint16 *cgram        = ppu->cgram;
uint8  *bmult        = ppu->brightnessMult;       // final-line value; see Known Limitations

for (int row = 0; row < s->size; row++) {
  int srcRow = vflip ? (s->size - 1 - row) : row;
  int out_y  = (s->y + row) * S;
  if (out_y < 0 || out_y >= hd_height) continue;       // cheap clip; per-row, not per-HD-line

  // Which sub-tile row & within-tile y-pixel (in SD units) we're sampling:
  int tile_row_src   = srcRow >> 3;
  int y_in_tile_sd   = srcRow & 7;

  for (int col = 0; col < s->size; col++) {
    int srcCol = hflip ? (s->size - 1 - col) : col;
    int out_x  = (s->x + col) * S;
    if (out_x < 0 || out_x >= hd_width) continue;

    int tile_col_src = srcCol >> 3;
    int x_in_tile_sd = srcCol & 7;

    // PPU's usedTile formula, operating on VRAM word-addresses.
    // Column wraps within the 16-tile row of the object page; rows do not wrap
    // (they continue past the end of the page, matching PPU behavior).
    uint8  u_hi = (tile_hi + tile_row_src) & 0xff;
    uint8  u_lo = (tile_lo + tile_col_src) & 0xf;
    uint16 usedTile  = ((uint16)u_hi << 4) | u_lo;
    uint16 tile_vram = (obj_page + usedTile * 16) & 0x7fff;

    uint8  sheet_id;
    uint16 tile_in_sheet;
    if (!HdVramMap_ResolveTile(tile_vram, &sheet_id, &tile_in_sheet))
      continue;                                          // no HD tile — skip this pixel
    HdSheet *sheet = &g_hd_sheets[sheet_id];
    if (!sheet->loaded) continue;
    // Uniform-scale invariant from Step 2.
    assert(sheet->scale == S);

    // HD sheet coords of the tile's top-left. Sheets are always 16 tiles wide
    // (width = 128*S); height is variable per sheet — see sheet->tile_count.
    if (tile_in_sheet >= sheet->tile_count) continue;  // out-of-sheet reference — skip
    int tx = (tile_in_sheet & 0xf) * 8 * S;
    int ty = (tile_in_sheet >> 4)  * 8 * S;

    // HD pixel range for this (row, col) = one SD pixel = S×S HD pixels.
    for (int py = 0; py < S; py++) {
      int dst_y = out_y + py;
      if (dst_y < 0 || dst_y >= hd_height) continue;
      uint32_t *dst_row = (uint32_t *)((uint8 *)hd_buf + dst_y * hd_pitch);
      for (int px = 0; px < S; px++) {
        int dst_x = out_x + px;
        if (dst_x < 0 || dst_x >= hd_width) continue;

        int src_x_hd = tx + x_in_tile_sd * S + px;
        int src_y_hd = ty + y_in_tile_sd * S + py;
        uint8 index = sheet->index_buffer[src_y_hd * sheet->width + src_x_hd];
        if (index == 0) continue;                        // HD-PNG transparent

        uint16 color = cgram[palette_base + index];      // 15-bit ..bbbbb gggggrrrrr (R low)
        uint8  r     = bmult[(color >>  0) & 0x1f];
        uint8  g     = bmult[(color >>  5) & 0x1f];
        uint8  b     = bmult[(color >> 10) & 0x1f];
        dst_row[dst_x] = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | 0xff000000u;
      }
    }
  }
}
```

Notes on the code above:
- **Flip handling.** Both the tile-select (`tile_row_src`, `tile_col_src`) and the within-tile offset (`y_in_tile_sd`, `x_in_tile_sd`) derive from `srcRow`/`srcCol`, which have the flip already applied. This matches ppu.c: [ppu.c:792](src/snes/ppu.c#L792) flips `row` before the sub-tile loop, and [ppu.c:807](src/snes/ppu.c#L807) flips `col` via `usedCol`. Getting either one wrong breaks flipped large sprites (all of Mario's left-facing frames).
- **Per-sub-tile VRAM resolution.** `HdVramMap_ResolveTile` runs once per SD pixel. That's 64 resolves per 8×8 sub-tile — acceptable at v1 scale. If profiling shows it matters, hoist the resolve out to an `(8×8)` sub-tile loop.
- **Mask to sheet.** Sheet tile count is variable (Step 2 accepts any positive number of 8-row tile rows; width is fixed at 16 tiles). `tile_in_sheet >= sheet->tile_count` means the sprite referenced VRAM tiles past the end of a recorded region's sheet — treat as unmapped and skip. Do **not** hard-code 128 here.
- **BGRA byte order.** Matches [ppu.c:709](src/snes/ppu.c#L709): R in bits 16-23, G in 8-15, B in 0-7. Alpha set to 0xff on every sprite pixel (BG upscale leaves alpha = 0; setting alpha here prepares for Step 6's blend).
- **Sheet scale invariant.** Step 2 rejects sheets whose scale doesn't match `g_hd_scale`; the assert is a debug safety net.

### Integrate into `HdCompositor_Draw`

```c
void HdCompositor_Draw(uint8 *dst, size_t pitch,
                       const uint8 *sd_pixels, int sd_width, int sd_height) {
  int S = g_hd_scale;
  int hd_width  = sd_width  * S;
  int hd_height = sd_height * S;

  // 1. Nearest-upscale BG (existing code from Step 1).
  // 2. Build scene from current PPU state.
  HdScene_Build(&g_hd_scene, g_my_ppu);
  // 3. Composite sprites layer-by-layer, high-OAM-first within layer.
  HdCompositor_DrawSprites(dst, pitch, hd_width, hd_height, g_my_ppu);
}
```

`g_my_ppu` is the reimplementation's PPU; `g_snes->ppu` is the reference emulator's. Use `g_my_ppu` so HD output reflects what the game code actually wrote. Verify the symbol exists (declared in [src/main.c](src/main.c) / [src/variables.h](src/variables.h)) before using.

### Performance hoisting

Inside the per-sprite function:
- Hoist `sheet`, `sheet->index_buffer`, `sheet->width`, `sheet->tile_count`, `palette_base`, `cgram`, `bmult` out of inner loops.
- Hoist sub-tile resolve if profiling demands it (resolve once per 8×8 sub-tile; store `sheet_id`, `tile_in_sheet` in locals; loop the 8×8 SD pixels × S² HD pixels).
- Don't attempt SIMD in v1.

**Gotchas:**
- **Large-sprite vflip must flip the sub-tile index, not just the within-tile y.** If only the within-tile y is flipped, a vflipped 32×32 sprite renders each 8×8 flipped individually but in unflipped top-to-bottom order — visibly wrong. The pseudocode above handles this by deriving `srcRow` from output `row` first and then splitting into tile-row + pixel-row from the flipped value.
- **`brightnessMult` is per-scanline** and updated as ppu.c runs each line (see [ppu.c:110-113](src/snes/ppu.c#L110-L113)). At `HdCompositor_Draw` time, only the *last* line's brightness is available. Fade effects that HDMA `$2100` across the frame will apply the final-line brightness uniformly to HD sprites. Accepted v1 limitation — document.
- **Alpha in the BG buffer is 0.** Sprite writes always set A=0xff, which is what Step 6 needs for alpha-blit. Do not assume the BG pass wrote opaque alpha.
- **CGRAM format.** 15-bit `..bbbbb gggggrrrrr` (R in low 5 bits). Match [ppu.c:709-711](src/snes/ppu.c#L709-L711) exactly.
- **Pixel format is BGRA little-endian** (what [opengl.c](src/opengl.c) uploads with `GL_BGRA` + `GL_UNSIGNED_INT_8_8_8_8_REV`). Don't swap R/B.
- **OAM priority rotation is still ignored** (continuing from Step 4). Added to Known Limitations.

**Acceptance:**
- With an HD Mario sheet in `gfx/hd/gfx32.png`: Mario renders crisply on top of the upscaled BG. Invincibility palette flash and level fade-in apply to HD Mario (via CGRAM + final-line brightness).
- Left-facing / crouching Mario (hflip / vflip) render correctly — not mirrored per-sub-tile.
- A sprite that spans Mario's Path-B region and a neighboring Path-A region renders both parts from their correct source sheets (test: a large Mario-adjacent sprite while Mario is mid-animation).
- Sprites with no HD replacement (`HdVramMap_ResolveTile` fails, or `sheet_id`'s sheet isn't loaded) simply don't draw. No crash.

---

## Step 6 — Per-layer effects (drop shadow) ✅ DONE

**Goal:** render each layer's sprites into a temp HD buffer, run an effect pass (drop shadow for v1), alpha-blit the result onto the main HD buffer. Sets up the per-layer effect-pass pipeline for later additions (outer glow, etc.).

**Status of dependencies:** Step 5 complete. Sprites render directly into the main HD buffer from inside `HdCompositor_DrawSprites`, which owns the `layer = 0..3` outer loop. This step refactors that.

### What Step 5 actually did (read before planning changes)

- [src/hd_compositor.c:67](src/hd_compositor.c#L67) `HdCompositor_DrawSprites(hd_buf, hd_pitch, hd_w, hd_h, ppu)` contains the full `for layer in 0..3 { for k = count-1..0 { blit } }` loop and writes straight to the main HD buffer. Step 6 must pull the inner blit out so it can target any buffer and optionally run in silhouette mode.
- [src/hd_scene.h:14-16](src/hd_scene.h#L14-L16) `HdSprite` stores both `tile_vram` and `tile_num`. The compositor recovers `objAdr = (tile_vram - tile_num*16) & 0x7fff` and walks sub-tiles with the PPU `usedTile` formula. Keep this pattern — the silhouette pass uses exactly the same sub-tile walk.
- [src/hd_compositor.c:91-163](src/hd_compositor.c#L91-L163) shows the flip handling that must be preserved in silhouette mode: tile-order flip (`src_t_row`/`src_t_col`), within-tile pixel flip (`src_yi`/`src_xi`), AND S×S sub-pixel flip (`src_py`/`src_px` — the commit 7b3b2fe fix for HD detail on flipped sprites).
- [src/hd_compositor.c:18-20](src/hd_compositor.c#L18-L20) globals: `g_hd_enabled` defaults true; `g_hd_scale` starts 1 and is set by `HdGfx_LoadAll` to the max loaded-sheet scale. Sprite compositing is gated on `g_hd_scale > 1` at [hd_compositor.c:47](src/hd_compositor.c#L47) — so when no HD sheets are present, Step 6 work is a no-op.
- [src/hd_compositor.c:22-25](src/hd_compositor.c#L22-L25) `HdCompositor_Init` runs at startup **before** SDL/GL have chosen dimensions. HD buffer size (`sd_w * S × sd_h * S`) is only known inside `HdCompositor_Draw`. Allocate HD-sized work buffers lazily on first Draw (or when dims change) — not in Init.
- An always-on `HdCompositor_DebugDump` at [hd_compositor.c:172](src/hd_compositor.c#L172) fires every 60 frames. Leave it untouched.

### Read first

- [src/hd_compositor.c](src/hd_compositor.c) — whole file, especially `HdCompositor_Draw` and `HdCompositor_DrawSprites`.
- [src/hd_compositor.h](src/hd_compositor.h) — globals + entry points.
- [src/hd_scene.h](src/hd_scene.h) — `HdSprite` fields (`tile_vram`, `tile_num`, `layer`, `flags`).
- [src/hd_gfx.h](src/hd_gfx.h) — `HdSheet` layout (sheets are always 16 tiles wide; `tile_count` varies).
- [src/hd_vram_map.h](src/hd_vram_map.h) — `HdVramMap_ResolveTile`.

### Tasks

1. **Layer config** — add to `src/hd_compositor.c` (file-static, not exported yet; Step 7 hooks INI to mutate it):
   ```c
   typedef struct HdLayerCfg {
     bool     shadow_enabled;
     int16    shadow_dx, shadow_dy;   // offset in HD pixels (may be negative)
     uint8    shadow_alpha;           // 0..255, the src alpha used when alpha-blitting the shadow buffer
     uint8    shadow_r, shadow_g, shadow_b;  // shadow color (default black)
   } HdLayerCfg;

   static HdLayerCfg g_hd_layer_cfg[4] = {
     [0] = { false },
     [1] = { false },
     [2] = { .shadow_enabled = true, .shadow_dx = 4, .shadow_dy = 4,
             .shadow_alpha = 128, .shadow_r = 0, .shadow_g = 0, .shadow_b = 0 },
     [3] = { false },
   };
   ```
   Rationale for layer 2 default: SMW's Mario, enemies, and most gameplay sprites use OAM priority 2. The user can tweak later.

2. **Lazy work-buffer management.** Add file-statics:
   ```c
   static uint8 *g_hd_layer_buf   = NULL;   // HD-sized, one sprite layer at a time
   static uint8 *g_hd_shadow_buf  = NULL;   // HD-sized, one shadow silhouette at a time
   static int    g_hd_buf_width   = 0;
   static int    g_hd_buf_height  = 0;
   ```
   A helper `HdCompositor_EnsureBuffers(int hd_w, int hd_h)` frees + reallocates both buffers when `hd_w`/`hd_h` differ from the cached dims. Call from `HdCompositor_Draw` after computing `hd_width`/`hd_height`, before compositing. Size: `hd_w * hd_h * 4` bytes each. At `S=4`, `1024×896×4 ≈ 3.5 MB × 2 buffers = 7 MB`. Do not free during play; only on `HdCompositor_Shutdown` (add this symbol if it doesn't exist, or free in an `atexit`).

3. **Refactor the sprite blit.** Rename the Step 5 function and add a mode parameter:
   ```c
   typedef enum {
     kHdBlitColor       = 0,  // CGRAM-indexed color + brightnessMult (Step 5 behavior)
     kHdBlitSilhouette  = 1,  // fixed RGB from cfg, fixed alpha from cfg, only where HD index != 0
   } HdBlitMode;

   static void HdCompositor_BlitSprite(uint8 *dst_buf, size_t dst_pitch,
                                       int dst_w, int dst_h,
                                       const HdSprite *s,
                                       HdBlitMode mode,
                                       int16 offset_x, int16 offset_y,    // in HD pixels
                                       uint8 sil_r, uint8 sil_g, uint8 sil_b, uint8 sil_a,
                                       const Ppu *ppu);
   ```
   The body is the existing Step 5 inner blit (tile-grid walk, `HdVramMap_ResolveTile`, 8×8 sub-tile pixels, S×S expansion with tri-level flip) lifted verbatim, with two changes:
   - Add `offset_x` / `offset_y` to the final HD destination coords (in HD pixel units, not SD). `offset_x = offset_y = 0` for color mode; set to `shadow_dx` / `shadow_dy` for silhouette.
   - The per-pixel color write becomes:
     ```c
     if (mode == kHdBlitColor) {
       uint16 color = cgram[pal_base + index];
       uint8 r = bmult[(color >>  0) & 0x1f];
       uint8 g = bmult[(color >>  5) & 0x1f];
       uint8 b = bmult[(color >> 10) & 0x1f];
       dst_row[dst_x] = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | 0xff000000u;
     } else {  // kHdBlitSilhouette
       // Opaque overwrite; multiple silhouettes overlapping within the same buffer must NOT
       // compound alpha — last-writer-wins is fine because they all share the same color.
       dst_row[dst_x] = (uint32_t)sil_b | ((uint32_t)sil_g << 8) | ((uint32_t)sil_r << 16)
                      | ((uint32_t)sil_a << 24);
     }
     ```
   - Everything else (flip tiers, sheet/tile resolve, `tile_in_sheet >= sheet->tile_count` guard, `sheet->scale != S` guard, `index == 0` skip) is shared between modes.

   Silhouette mode still needs the full flip chain so asymmetric sprites produce correctly-shaped silhouettes.

4. **Alpha-blit helper.** Straight CPU loop over `hd_w × hd_h` pixels:
   ```c
   static void HdCompositor_AlphaBlit(uint8 *dst, size_t dst_pitch,
                                      const uint8 *src, size_t src_pitch,
                                      int w, int h);
   ```
   Per pixel, read src BGRA, extract `a = src[3]`; if `a == 0`, skip. Else:
   ```c
   uint8 inv = 255 - a;
   dst[0] = (uint8)((src[0] * a + dst[0] * inv + 127) / 255);  // B
   dst[1] = (uint8)((src[1] * a + dst[1] * inv + 127) / 255);  // G
   dst[2] = (uint8)((src[2] * a + dst[2] * inv + 127) / 255);  // R
   dst[3] = 0xff;  // keep main buffer opaque
   ```
   `+127` is round-to-nearest on integer divide by 255.

5. **New per-layer composite in `HdCompositor_Draw`.** Replace the single `HdCompositor_DrawSprites(dst, pitch, hd_w, hd_h, ppu)` call (still guarded by `if (g_hd_scale > 1)`) with:
   ```c
   HdCompositor_EnsureBuffers(hd_width, hd_height);
   size_t work_pitch = (size_t)hd_width * 4;

   for (int layer = 0; layer < 4; layer++) {
     const HdLayerCfg *cfg = &g_hd_layer_cfg[layer];

     // --- shadow pre-pass (if enabled) -----------------------------------
     if (cfg->shadow_enabled) {
       memset(g_hd_shadow_buf, 0, work_pitch * hd_height);
       // Highest-index-first within layer, matching Step 5's OAM overwrite order.
       for (int k = (int)g_hd_scene.count - 1; k >= 0; k--) {
         const HdSprite *s = &g_hd_scene.sprites[k];
         if (s->layer != layer) continue;
         HdCompositor_BlitSprite(g_hd_shadow_buf, work_pitch, hd_width, hd_height,
                                 s, kHdBlitSilhouette,
                                 cfg->shadow_dx, cfg->shadow_dy,
                                 cfg->shadow_r, cfg->shadow_g, cfg->shadow_b,
                                 cfg->shadow_alpha,
                                 g_my_ppu);
       }
       HdCompositor_AlphaBlit(dst, pitch, g_hd_shadow_buf, work_pitch, hd_width, hd_height);
     }

     // --- sprite layer pass ----------------------------------------------
     memset(g_hd_layer_buf, 0, work_pitch * hd_height);
     for (int k = (int)g_hd_scene.count - 1; k >= 0; k--) {
       const HdSprite *s = &g_hd_scene.sprites[k];
       if (s->layer != layer) continue;
       HdCompositor_BlitSprite(g_hd_layer_buf, work_pitch, hd_width, hd_height,
                               s, kHdBlitColor, 0, 0, 0, 0, 0, 0xff, g_my_ppu);
     }
     HdCompositor_AlphaBlit(dst, pitch, g_hd_layer_buf, work_pitch, hd_width, hd_height);
   }
   ```
   Delete the old `HdCompositor_DrawSprites` (it's fully subsumed).

6. **Update the header.** No public API change — the existing `HdCompositor_Draw` signature still works; everything else is file-static. `HdCompositor_GetScene` continues to expose the scene.

7. **Makefile** — no change (new code lives in the existing `hd_compositor.c`).

### Gotchas

- **Silhouette alpha is per-buffer, not per-pixel-accumulated.** Writing `sil_a` directly into the shadow buffer with last-writer-wins means overlapping silhouettes inside the same layer do NOT stack darker — that's intentional. One shadow per layer group. Accumulating per-pixel alpha (using alpha-blit while drawing silhouettes) would darken overlap regions; don't do that.
- **Flip chain must be complete in silhouette mode.** Mario crouching + facing left (vflip + hflip) must produce a silhouette that actually matches his shape. Reuse the tri-level flip from Step 5 (tile-order + within-tile + S×S sub-pixel) verbatim — strip only the CGRAM lookup.
- **Shadow pre-pass drops `brightnessMult`.** Silhouettes use raw `shadow_r/g/b`; they don't reflect CGRAM fades. This is deliberate — the shadow is a rendering effect, not game content. Fade-to-black at level end still darkens the sprite layer on top of the (still-black) shadow, producing a reasonable look.
- **Shadow offset is in HD pixels, not SD.** `shadow_dx = 4` at `S=4` is one SD pixel; at `S=8` it's half an SD pixel. If the user wants "one SD pixel", they'll configure `S` in Step 7.
- **BG-only alpha in main HD buffer is undefined.** Step 1's nearest-upscale copies whatever alpha was in `g_my_pixels` (likely 0 or 0xff depending on PPU code paths). `AlphaBlit` only reads **src** alpha; it writes `0xff` to dst alpha after blending, so the main buffer becomes uniformly opaque after the first layer's shadow or sprite pass. Fine for the GL upload (ignores alpha anyway).
- **Buffer sizing edge cases.** When `g_hd_scale == 1`, the sprite composite is gated off and `HdCompositor_EnsureBuffers` never runs — don't call it from the gated `if` or you'll allocate unused memory. Call it inside the `if (g_hd_scale > 1)` block only.
- **Memory footprint at S=8.** `2048×1792×4 × 2 = 28 MB`. Acceptable.
- **Scale change after Step 7 toggle.** `HdCompositor_EnsureBuffers` guards on dim-change and reallocates. Don't cache pitch elsewhere.
- **`offset_x/y` clipping.** Adding `offset_x`, `offset_y` to final HD dst coords can push pixels outside `[0, hd_w) × [0, hd_h)`. The existing per-pixel `if (dst_x < 0 || dst_x >= hd_width) continue;` guard in the Step 5 blit already handles this — keep it.
- **Sprites whose tiles fall outside the HD map still render nothing in both modes.** `HdVramMap_ResolveTile` failing causes `continue`, so silhouette writes nothing. A sprite with zero mapped sub-tiles casts no shadow. That's correct — there's no HD content to silhouette.

### Acceptance

- **Build clean**, no new warnings.
- With HD sheets present and `g_hd_layer_cfg[2].shadow_enabled = true` (default): Mario shows an offset drop shadow on layer 2. Enemies drawn on OAM priority 2 also cast shadows. Items/power-ups sharing priority 2 too. Use the Step 5 debug dump to confirm Mario's OAM entries are on layer 2 — if they're on a different layer, document that and point Step 7's default at that layer instead. (Do not hardcode a "Mario layer" — let config decide.)
- Toggle `g_hd_layer_cfg[2].shadow_enabled = false` at compile time → shadows disappear; sprites render as in Step 5.
- Flipped sprites cast shape-correct silhouettes: left-facing Mario's shadow points the same direction as his body, not mirrored.
- No visible alpha compounding when two layer-2 sprites overlap (their shadows merge cleanly, not darken in the overlap region).
- At `g_hd_scale == 1` (no HD sheets), the compositor is identical to SD — no work buffers allocated, no shadows.
- Frame rate is unchanged or within noise of Step 5. Per-layer memsets on a 3.5 MB buffer are memory-bandwidth-bound but fast; flag for profiling if a regression appears.

---

## Step 7 — Config + toggle ✅ DONE

**Goal:** make HD enable/disable, the gfx dir, and per-layer shadow effect knobs user-configurable from `smw.ini`. Add a runtime hotkey to toggle HD on/off. Fix the runtime guard so an HD-enabled config with no sheets loaded falls back cleanly to SD.

**Status of dependencies:** Steps 1–6 complete. Compositor + per-layer shadow effect work; defaults are hardcoded in `hd_compositor.c`. The work buffers allocated lazily in Step 6 are not freed at shutdown.

### What Step 6 actually shipped (read before planning changes)

- [src/hd_compositor.c:42-55](src/hd_compositor.c#L42-L55) `HdLayerCfg g_hd_layer_cfg[4]` — file-static. Fields: `shadow_enabled` (bool), `shadow_dx/dy` (int16, HD pixels), `shadow_alpha` (uint8), `shadow_r/g/b` (uint8). Default has shadow on layer 2 only: `dx=4, dy=4, alpha=128, color=0x000000`. **No accessor** — Step 7 must either expose one or move the symbol to the header.
- [src/hd_compositor.c:60-74](src/hd_compositor.c#L60-L74) lazy work buffers (`g_hd_layer_buf`, `g_hd_shadow_buf`) reallocated by `HdCompositor_EnsureBuffers` on dim change. Currently nothing frees them at exit.
- [src/hd_compositor.c:76-79](src/hd_compositor.c#L76-L79) `HdCompositor_Init` calls `HdVramMap_Reset` and `HdGfx_LoadAll("gfx/hd")` with the path hardcoded.
- [src/hd_compositor.c:35-37](src/hd_compositor.c#L35-L37) globals: `g_hd_enabled = true`, `g_hd_scale = 1`. `g_hd_scale` is mutated by `HdGfx_LoadAll` to the max loaded-sheet scale; if no sheets load it stays 1.

### Existing config + input infrastructure (use these patterns)

- `Config` struct in [src/config.h:49-84](src/config.h#L49-L84). Existing string-pointer fields (`link_graphics`, `shader`) hold pointers into `g_config.memory_buffer` (the parsed INI buffer) — re-use the same pattern for `hd_gfx_dir`.
- INI sections enumerated in `GetIniSection` ([src/config.c:284-298](src/config.c#L284-L298)). `[Graphics]` is section 1; that's where HD keys go (matches existing `WindowScale`, `LinearFiltering`, etc.).
- Per-section dispatch in `HandleIniConfig` ([src/config.c:330-441](src/config.c#L330-L441)). Section 1 reads boolean/int/string keys with `ParseBool` / `strtol` / direct assignment.
- Command keys live in the `kKeys_*` enum at [src/config.h:5-41](src/config.h#L5-L41). Defaults table at [src/config.c:23-45](src/config.c#L23-L45) (using macros `_(SDLK_x)`, `S(...)`, `C(...)`, `A(...)` for plain / Shift+ / Ctrl+ / Alt+ bindings). Names exposed to the INI `[KeyMap]` section live in `kKeyNameId` at [src/config.c:59-66](src/config.c#L59-L66) (use `S(name)` for single-cmd entries).
- Hotkey dispatch is the `switch` in `HandleCommand` at [src/main.c:694-779](src/main.c#L694-L779) — add a `case kKeys_ToggleHdGfx:` branch.
- Init order: `ParseConfigFile(config_file)` runs at [src/main.c:377](src/main.c#L377), `HdCompositor_Init()` at [src/main.c:491](src/main.c#L491). Init can therefore read `g_config` directly — no plumbing needed.
- Shutdown happens around [src/main.c:636-637](src/main.c#L636-L637) (`g_renderer_funcs.Destroy(); HdGfx_Free();`). Add a sibling `HdCompositor_Shutdown()` call there.

### Latent bug to fix as part of this step

[src/main.c:184](src/main.c#L184) sets `g_hd_skip_sprites = g_hd_enabled` unconditionally, then [src/main.c:188](src/main.c#L188) calls `HdCompositor_Draw` whenever `g_hd_enabled`. But `HdCompositor_Draw` only composites HD sprites when `g_hd_scale > 1` ([hd_compositor.c:101](src/hd_compositor.c#L101)). Result: when HD is enabled by config but no HD sheets loaded (so `g_hd_scale` stayed 1), SD sprites get skipped *and* HD sprites never draw → invisible sprites.

Fix as part of Task 3 below. The cleanest predicate is "HD is actually doing something" = `g_hd_enabled && g_hd_scale > 1`. Either inline that, or introduce a derived `bool g_hd_active` updated whenever `g_hd_enabled` or `g_hd_scale` changes.

### Tasks

1. **Extend the `Config` struct** ([src/config.h](src/config.h)):
   ```c
   // HD graphics
   bool        hd_gfx_enabled;       // default true
   const char *hd_gfx_dir;           // default NULL → fall back to "gfx/hd" at use site
   bool        hd_layer_shadow[4];   // default { false, false, true, false }
   int16       hd_shadow_dx;         // default 4   (HD pixels)
   int16       hd_shadow_dy;         // default 4   (HD pixels)
   uint8       hd_shadow_alpha;      // default 128
   uint8       hd_shadow_r;          // default 0
   uint8       hd_shadow_g;          // default 0
   uint8       hd_shadow_b;          // default 0
   ```
   Set defaults in `ParseConfigFile` ([src/config.c:480-491](src/config.c#L480-L491)) alongside the existing `msuvolume` / `save_playthrough` defaults — set them *before* `ParseOneConfigFile` runs so user values can override.

2. **Parse the new keys** in section 1 of `HandleIniConfig` ([src/config.c:354-396](src/config.c#L354-L396)). Pascal-case to match the existing style:
   - `HdGfxEnabled` → `ParseBool(value, &g_config.hd_gfx_enabled)`.
   - `HdGfxDir` → `g_config.hd_gfx_dir = value;` (the INI buffer outlives runtime, see existing `LinkGraphics` precedent).
   - `HdLayerShadow` → comma-separated 4-bool list (e.g. `0,0,1,0`). Implement inline: walk `NextDelim(&value, ',')` for up to 4 entries, `ParseBool` each, write into `g_config.hd_layer_shadow[i]`. Don't reuse `ParseKeyArray` — that's for SDL keycodes.
   - `HdShadowOffset` → comma-separated `dx,dy` ints. Parse with `strtol`.
   - `HdShadowAlpha` → `(uint8)strtol(value, NULL, 10)`.
   - `HdShadowColor` → either `R,G,B` decimal or `#RRGGBB` hex. Pick one — hex is shorter for users; decimal is simpler to parse. Recommend hex with `strtoul(value+1, NULL, 16)` if `value[0] == '#'`, else decimal triple.

3. **Apply config + fix the runtime guard.** Two sub-changes:

   3a. Add a public function in `hd_compositor.h`:
   ```c
   void HdCompositor_ApplyConfig(void);
   void HdCompositor_Shutdown(void);
   bool HdCompositor_Toggle(void);   // returns the new g_hd_enabled value
   ```
   Implement in `hd_compositor.c`:
   - `ApplyConfig`: read `g_config.hd_gfx_enabled` into `g_hd_enabled`; populate the 4 entries of `g_hd_layer_cfg` from `g_config.hd_layer_shadow[i]` + the shared offset/alpha/color fields. Layer 2's previous hardcoded default is now produced by config defaults.
   - `Shutdown`: free `g_hd_layer_buf`, `g_hd_shadow_buf`, zero the cached dims. Idempotent.
   - `Toggle`: flip `g_hd_enabled`; return new value (caller may want to log it).

   3b. Modify `HdCompositor_Init` ([src/hd_compositor.c:76-79](src/hd_compositor.c#L76-L79)):
   ```c
   void HdCompositor_Init(void) {
     HdCompositor_ApplyConfig();
     HdVramMap_Reset();
     HdGfx_LoadAll(g_config.hd_gfx_dir ? g_config.hd_gfx_dir : "gfx/hd");
     // Runtime guard: HD enabled by config but no sheets loaded → silently SD.
     if (g_hd_enabled && g_hd_scale <= 1) {
       fprintf(stderr, "HD enabled but no HD sheets loaded from '%s' "
                       "(scale=1); falling back to SD.\n",
               g_config.hd_gfx_dir ? g_config.hd_gfx_dir : "gfx/hd");
     }
   }
   ```
   `Init` must `#include "config.h"` (currently does not).

   3c. Fix the gate in `RtlDrawPpuFrame` at [src/main.c:184-193](src/main.c#L184-L193):
   ```c
   bool hd_active = g_hd_enabled && g_hd_scale > 1;
   g_hd_skip_sprites = hd_active;
   g_rtl_game_info->draw_ppu_frame();
   uint8 *ppu_pixels = g_other_image ? g_my_pixels : g_pixels;
   if (hd_active) {
     HdCompositor_Draw(pixel_buffer, pitch, ppu_pixels, g_snes_width, g_snes_height);
   } else {
     for (size_t y = 0, y_end = g_snes_height; y < y_end; y++)
       memcpy(pixel_buffer + y * pitch, ppu_pixels + y * 256 * 4, 256 * 4);
   }
   ```
   Note: `PpuGetCurrentRenderScale` at [src/snes/ppu.c:1151](src/snes/ppu.c#L1151) already returns `g_hd_enabled ? g_hd_scale : 1` — when `g_hd_scale == 1` that's still 1, so the renderer sizing path is already self-consistent. No change needed there.

   3d. Wire `HdCompositor_Shutdown()` into the shutdown path at [src/main.c:637](src/main.c#L637), adjacent to `HdGfx_Free()`.

4. **Add the toggle hotkey.**

   4a. Add `kKeys_ToggleHdGfx` to the `kKeys_*` enum in [src/config.h:5-41](src/config.h#L5-L41) — insert anywhere after `kKeys_ControlsP2_Last` (the controls block has fixed indices, but the single-key block is order-free as long as `kDefaultKbdControls` matches).

   4b. Add the default binding to `kDefaultKbdControls` ([src/config.c:23-45](src/config.c#L23-L45)). Use `C(SDLK_h)` (Ctrl+H). Position the entry to match wherever `kKeys_ToggleHdGfx` lands in the enum — easiest is to append both at the very end (after `VolumeDown`) so existing line offsets don't shift.

   4c. Add `S(ToggleHdGfx)` to the `kKeyNameId[]` array ([src/config.c:59-66](src/config.c#L59-L66)) so users can rebind via the `[KeyMap]` INI section.

   4d. Add a `case kKeys_ToggleHdGfx:` arm to the `switch` in `HandleCommand` ([src/main.c:728-773](src/main.c#L728-L773)):
   ```c
   case kKeys_ToggleHdGfx: {
     bool now = HdCompositor_Toggle();
     fprintf(stderr, "HD graphics: %s\n", now ? "ON" : "OFF");
     break;
   }
   ```

5. **Document the new INI keys.** Add to [smw.ini](smw.ini) under `[Graphics]`, after the existing entries:
   ```ini
   # --- HD graphics (sprites only, v1) ---
   # Enable HD sprite rendering (requires PNGs in HdGfxDir).
   HdGfxEnabled = 1

   # Directory for HD sprite sheets (gfx00.png .. gfx33.png).
   #HdGfxDir = gfx/hd

   # Per-OAM-priority drop shadow toggles (4 booleans, layers 0..3).
   HdLayerShadow = 0,0,1,0

   # Drop shadow offset in HD pixels (dx,dy) and alpha (0..255).
   HdShadowOffset = 4,4
   HdShadowAlpha  = 128

   # Drop shadow color, hex or comma-separated decimal RGB.
   HdShadowColor  = #000000
   ```

### Gotchas

- **Init order is already correct.** `ParseConfigFile` runs at main.c:377; `HdCompositor_Init` at main.c:491. Reading `g_config` from `ApplyConfig` is safe.
- **`hd_gfx_dir` lifetime.** The INI parser stores `g_config.memory_buffer = filedata` (config.c:450) and never frees it during runtime, so pointers into the buffer stay valid for the program's lifetime. Mirror `link_graphics`/`shader` — assign the parser's `value` pointer directly, do *not* `strdup`. Default path `"gfx/hd"` is a string literal — also safe.
- **Toggling HD at runtime changes `PpuGetCurrentRenderScale`'s return.** Existing renderers (`OpenGLRenderer_BeginDraw`, `SdlRenderer_BeginDraw` post Step-1 fix) already reallocate on dim change. Validate visually that the toggle doesn't flicker for more than one frame — if it does, document and accept; do not introduce a debounce in v1.
- **Toggle does not reload HD sheets.** A user dropping new PNGs into `gfx/hd` and pressing Ctrl+H expects them to appear. They won't — sheets are loaded once at Init. Document this; offer "restart to apply" guidance. Reload-on-toggle is out of scope for v1.
- **Config `hd_layer_shadow` ↔ `g_hd_layer_cfg.shadow_enabled` mapping** is per-layer; the offset/alpha/color knobs are *shared* across all 4 layers (matching the simplified user-facing surface). The struct still has per-layer color/offset internally — `ApplyConfig` writes the same shared values into all 4 entries. Per-layer customization stays available to code but isn't exposed via INI in v1.
- **`HdCompositor_Toggle` is the only way to mutate `g_hd_enabled` post-Init.** Don't have other code reach into the global directly — going through the function gives us a single place to add side effects later (e.g. log, flush, force-reapply).
- **Existing "extra" file at root** [smooth_sprites.py](smooth_sprites.py) is unrelated to this step; the IDE may have it open. Ignore.
- **Shutdown ordering.** `HdCompositor_Shutdown` frees the layer/shadow work buffers; `HdGfx_Free` frees the sheet index buffers. They're independent — call order doesn't matter, but keep them adjacent so a future reader can find both.
- **Don't move `g_hd_layer_cfg` to the header.** Keep it file-static; mutate via `HdCompositor_ApplyConfig`. Future steps will add more effects and a getter is cheaper than fixing a leaky abstraction later.
- **Don't add an `--hd` CLI flag.** Config + hotkey covers the v1 surface.

### Acceptance

- **Build clean.**
- **Fresh checkout, no `gfx/hd/` directory, default INI:** game runs at SD, prints the "no HD sheets loaded ... falling back to SD" warning once at startup, no other HD output. SD sprites visible.
- **`gfx/hd/` populated with sheets at scale 4, default INI:** game runs at HD scale 4, Mario shows the configured drop shadow on layer 2.
- **`HdGfxEnabled = 0` in INI:** game runs at SD even when sheets are present. No warning.
- **Ctrl+H at runtime:** flips between HD and SD on the next frame. Window resizes; one-frame transient is acceptable but not multi-frame flicker. stderr logs `HD graphics: ON` / `OFF`.
- **`HdShadowAlpha = 0` in INI:** shadows render fully transparent (i.e., invisible) without crashing.
- **`HdShadowOffset = 0,0`:** shadows render directly under sprites (visible only via the alpha tint, since they have no offset).
- **`HdLayerShadow = 1,1,1,1`:** every OAM priority casts its own shadow; verify no per-pixel alpha compounding inside a single layer (overlapping silhouettes don't darken at the overlap).
- **Rebinding Ctrl+H** via `[KeyMap]` section `ToggleHdGfx = Shift+H` (for example) takes effect on next launch.
- **Quit cleanly:** no leaks reported by ASan/Valgrind from the HD path. (Spot check: rerun an existing valgrind/ASan flow if you have one; otherwise confirm the new free path is reachable.)

---

## Known limitations (document in README or code comment after v1)

- HD sprites composite on top of all BG tiles regardless of BG priority bits — a sprite that should render *behind* a high-prio BG tile (e.g., bit 3 of `$2105` set with BG3 prio 1) will appear in front. Rare in SMW gameplay.
- Mode 7 scenes fall back to upscaled SD (no HD path). Bowser battle, map mode 7 rotations.
- Color math (add/subtract subscreen), direct-color mode, color window math — not reproduced in the HD path. Most SMW gameplay doesn't use these for sprites; some effects (halo around Yoshi coins?) may look plainer.
- Verification mismatch snapshots compare RAM, not pixels — HD output does not affect verification.
- One uniform scale across all sheets in v1. Mixed-scale support is future work.
- **OAM priority rotation** (`PPU_objPriority(ppu)` — `$2103` bit 7) is ignored. HD scene walks OAM `0..127` unconditionally. SMW gameplay rarely uses rotation; if a cutscene/title screen relies on it, HD layering will be wrong for that scene.
- **Per-scanline brightness.** `ppu->brightnessMult` changes per scanline (HDMA on `$2100`). HD sprites all use the final-line value, so frame-wide fade effects are faithful but mid-frame brightness ramps are not. Typically cosmetic.
- **BG-priority interleaving (restated).** See first bullet.

---

## Build/test workflow per session

- `make -j$(nproc)` after each step. Fix warnings.
- `./smw` to run.
- Keep a level save available for quick iteration (e.g., Yoshi's Island 1 with a power-up visible).
- If you add a debug print (F9/F10 dumps), leave them behind `#ifdef HD_DEBUG` so they don't clutter runtime output.

## Out-of-scope (v1 — deferred to v2 or later)

- **BG tile replacement** (FG/BG/BG3 layers) — addressed in v2 (Steps 8–12).
- GPU compositor (textured quads, fragment-shader effects, palette LUT uniforms).
- Mode 7 HD.
- Per-sheet variable scale.
- Lunar Magic hack GFX.
- Mix of SD fallback for unmapped sprites (v1 just skips them).

---

## Version 2 — Background Tile Replacement + Proper Priority Layering

### Overview (v2)

**Goal:** display HD replacements for BG1, BG2, and BG3 background tiles, interleaved with HD sprites at the correct PPU priority levels, so that sprites correctly appear in front of or behind background tiles according to their OAM priority and BG tile priority bits.

**Scope (v2):** BG1 (4bpp FG tileset), BG2 (4bpp BG tileset), BG3 (2bpp UI/text layer), and overworld map tiles. All rendered through the same BG tile compositor once their graphics uploads are tracked.

**Non-goals (v2):**
- Mode 7 scene replacement (Bowser fight, overworld rotation/zoom). SD upscale + HD sprites only.
- Color math (add/subtract subscreen) reproduced in HD — HD tiles use CGRAM directly, no subscreen blending.
- Window clipping on HD BG tiles — HD tiles that are partially window-masked show their full HD area.
- Subscreen (secondary screen) HD replacement.
- Lunar Magic BG GFX routing.

**Why priority map capture is necessary:**

v1 upscales the fully composited SD frame (BG + sprites, all layers already merged) and then blits HD sprites on top. For BG tile replacement we must know, for each SD output pixel, *which PPU layer won that pixel*. Without this, an HD BG tile blitted onto the SD upscale would cover SD sprite pixels that should appear in front of it (e.g., an enemy standing in front of a bush would be erased by the HD bush tile). The solution is to capture the PPU's per-pixel priority z-buffer (`bgBuffers[0].data`) immediately after BG+sprite rendering on each scanline, before the CGRAM colour lookup. Then the BG tile compositor uses this buffer to gate each blit: an HD BG tile pixel is only written where the priority map confirms that tile's layer actually won.

---

### Architecture Changes (v2)

**New data flow (per frame, HD+BG on):**

1. Game runs, writes OAM/VRAM/CGRAM/tilemap regs as before.
2. `draw_ppu_frame()` runs `ppu_runLine` for each scanline:
   - Inside `PpuDrawWholeLine`, after `PpuDrawBackgrounds` has filled `bgBuffers[0]`, **capture** `bgBuffers[0].data[0..255]` into `g_hd_prio_map[(y-1)*256 .. (y-1)*256 + 255]` (new Step 8 hook).
   - Then the existing CGRAM lookup + pixel-buffer write runs unchanged.
3. `HdCompositor_Draw` runs:
   a. Nearest-upscale SD output into HD frame buffer (baseline fallback for all unmapped pixels).
   b. Build `HdScene` from OAM (unchanged from v1).
   c. Execute the **full 11-pass priority-ordered composite loop** (new Step 9), replacing the v1 four-pass sprite loop:
      - BG3 lo, Spr prio 0 (+ shadow), BG3 hi (!bg3prio), Spr prio 1 (+ shadow), BG2 lo, BG1 lo, Spr prio 2 (+ shadow), BG2 hi, BG1 hi, Spr prio 3 (+ shadow), BG3 hi (bg3prio).
      - Each BG pass calls `HdCompositor_BlitBgLayer`; each sprite pass uses the existing `BlitSprite` mechanism.

**Priority z-buffer encoding (mode 1):**

`bgBuffers[0].data[x]` is a `uint16` whose upper byte encodes the priority of the winning layer. The following non-overlapping ranges are stable across all mode-1 frames:

| Layer               | Upper-byte range | Notes                                    |
|---------------------|-----------------|------------------------------------------|
| Backdrop            | 0x05            | `ClearBackdrop` writes `0x0500`          |
| BG3 lo              | 0x12            | `zlo=0x1200`, 2bpp, palette+pixel ≤ 0x0A |
| Sprite prio 0       | 0x24            | `SPRITE_PRIO_TO_PRIO(0,…)=0x24`         |
| BG3 hi (!bg3prio)   | 0x32            | `zhi=0x3200` when `bgmode & 8 == 0`     |
| Sprite prio 1       | 0x64            |                                          |
| BG2 lo              | 0x71–0x77       | `zlo=0x7100`, palette*16 spread         |
| BG1 lo              | 0x80–0x87       | `zlo=0x8000`                             |
| Sprite prio 2       | 0xA4            |                                          |
| BG2 hi              | 0xB1–0xB7       | `zhi=0xB100`                             |
| BG1 hi              | 0xC0–0xC7       | `zhi=0xC000`                             |
| Sprite prio 3       | 0xE4            |                                          |
| BG3 hi (bg3prio)    | 0xF2            | `zhi=0xF200` when `bgmode & 8 != 0`     |

No two ranges overlap; a simple cascade of upper-byte threshold checks decodes any z-value to its source layer unambiguously.

**New symbols (add to `src/hd_compositor.h`):**

```c
// Per-frame priority map: 256 * 240 uint16 entries, one per SD pixel.
// Captured from bgBuffers[0].data each scanline in ppu.c.
// Allocated in HdCompositor_Init, freed in HdCompositor_Shutdown.
extern uint16 *g_hd_prio_map;

// Source-layer enum decoded from a priority map entry.
typedef enum {
  kHdBgLayer_Backdrop      = 0,
  kHdBgLayer_BG3lo         = 1,
  kHdBgLayer_Spr0          = 2,
  kHdBgLayer_BG3hi_noprio  = 3,  // BG3 hi when PPU_bg3priority(ppu) == 0
  kHdBgLayer_Spr1          = 4,
  kHdBgLayer_BG2lo         = 5,
  kHdBgLayer_BG1lo         = 6,
  kHdBgLayer_Spr2          = 7,
  kHdBgLayer_BG2hi         = 8,
  kHdBgLayer_BG1hi         = 9,
  kHdBgLayer_Spr3          = 10,
  kHdBgLayer_BG3hi_prio    = 11, // BG3 hi when PPU_bg3priority(ppu) != 0
} HdBgLayerID;

HdBgLayerID HdDecodeZbuf(uint16 z);

// Config flag for BG tile replacement (per-layer).
// g_hd_bg_enabled[i]: true → replace BG(i+1) tiles with HD; false → SD upscale only.
extern bool g_hd_bg_enabled[3];  // [0]=BG1, [1]=BG2, [2]=BG3
```

---

### Step 8 — Per-scanline priority map capture

**Goal:** After each scanline is rendered, copy the PPU's raw priority z-buffer (`bgBuffers[0].data`) into a CPU-side frame buffer `g_hd_prio_map`. Provide `HdDecodeZbuf` to classify any z-value to its source layer.

**Read first:**
- [src/snes/ppu.c: `PpuDrawWholeLine`](src/snes/ppu.c) — understand the call sequence: `ClearBackdrop` → `PpuDrawBackgrounds` → optional subscreen → final CGRAM loop.
- [src/snes/ppu.h: `PpuPixelPrioBufs`](src/snes/ppu.h) — `bgBuffers[0].data` is `uint16_t[kPpuXPixels]` = `uint16[256]`. `kPpuExtraLeftRight == 0` so `data[0..255]` maps directly to screen pixels 0..255.
- Step 7 outputs — `HdCompositor_Init`, `HdCompositor_Shutdown`, and the `g_hd_enabled` guard.

**Tasks:**

1. **Allocate `g_hd_prio_map`.** In `hd_compositor.c`, add:
   ```c
   uint16 *g_hd_prio_map = NULL;
   bool    g_hd_bg_enabled[3] = { true, true, false };  // BG3 off until Step 10
   ```
   In `HdCompositor_Init`, after existing init work:
   ```c
   if (!g_hd_prio_map)
     g_hd_prio_map = (uint16 *)malloc(256 * 240 * sizeof(uint16));
   ```
   In `HdCompositor_Shutdown`:
   ```c
   free(g_hd_prio_map); g_hd_prio_map = NULL;
   ```
   Declare extern in `src/hd_compositor.h`.

2. **Capture hook in `ppu.c`.** In `PpuDrawWholeLine` ([src/snes/ppu.c](src/snes/ppu.c)), immediately after the `PpuDrawBackgrounds(ppu, y, false)` call and *before* the subscreen / CGRAM composite block, insert:
   ```c
   // Capture per-pixel priority for HD BG compositor.
   extern uint16 *g_hd_prio_map;
   extern bool    g_hd_enabled;
   if (g_hd_enabled && g_hd_prio_map) {
     int sy = (int)y - 1;  // y is 1-based; sy is 0-based
     if ((unsigned)sy < 240)
       memcpy(g_hd_prio_map + sy * 256, ppu->bgBuffers[0].data, 256 * sizeof(uint16));
   }
   ```
   Do **not** add an `#include`; use `extern` declarations directly (ppu.c is a C translation unit that already has direct `extern bool g_hd_skip_sprites` as a precedent from Step 1).

3. **Implement `HdDecodeZbuf`.** Add to `hd_compositor.c` and declare in `hd_compositor.h`:
   ```c
   HdBgLayerID HdDecodeZbuf(uint16 z) {
     uint8 hi = (uint8)(z >> 8);
     if (hi >= 0xF2) return kHdBgLayer_BG3hi_prio;
     if (hi >= 0xE4) return kHdBgLayer_Spr3;
     if (hi >= 0xC0) return kHdBgLayer_BG1hi;
     if (hi >= 0xB1) return kHdBgLayer_BG2hi;
     if (hi >= 0xA4) return kHdBgLayer_Spr2;
     if (hi >= 0x80) return kHdBgLayer_BG1lo;
     if (hi >= 0x71) return kHdBgLayer_BG2lo;
     if (hi >= 0x64) return kHdBgLayer_Spr1;
     if (hi >= 0x32) return kHdBgLayer_BG3hi_noprio;
     if (hi >= 0x24) return kHdBgLayer_Spr0;
     if (hi >= 0x12) return kHdBgLayer_BG3lo;
     return kHdBgLayer_Backdrop;
   }
   ```
   The thresholds derive from the encoding table in the Architecture section above. Values in the gap ranges (e.g. 0x25..0x31) do not occur in practice.

4. **Extend the always-on debug dump.** In `HdCompositor_DebugDump` (runs ~1 Hz), add a single line printing the decoded layer for a fixed SD pixel (e.g. `(128, 112)`) to validate capture:
   ```c
   if (g_hd_prio_map) {
     uint16 z = g_hd_prio_map[112 * 256 + 128];
     fprintf(stderr, "  prio_map[112,128] = 0x%04x → layer %d\n", z, (int)HdDecodeZbuf(z));
   }
   ```

**Gotchas:**
- **Forced-blank lines.** When `PPU_forcedBlank(ppu)`, `PpuDrawWholeLine` returns early before reaching the capture point — `bgBuffers[0]` is never filled for that line. Guard `if (!PPU_forcedBlank(ppu))` before the capture, or accept that those rows stay at their previous value (harmless — the compositor skips forced-blank pixels anyway).
- **Overscan.** If the game uses 240-line mode (`ppu->frameOverscan`), lines 225–240 are valid and `g_hd_prio_map` must be 240 lines deep. The 240-line allocation already covers this.
- **`kPpuExtraLeftRight == 0`.** The `bgBuffers[0].data` array starts at index 0 for screen pixel 0 with no extra margin. If this constant ever changes, the `memcpy` offset must be updated.
- **Subscreen.** `PpuDrawBackgrounds(ppu, y, true)` (subscreen) also writes `bgBuffers[1]`, which we do not capture. HD compositing ignores the subscreen; v2 limitation noted below.
- **`g_new_ppu == false`.** When the old PPU renderer is active (`PpuDrawWholeLineOldPpu`), `bgBuffers[0]` is not used. The old PPU path does not produce useful priority data. Guard: if `!g_new_ppu` set `g_hd_bg_enabled[0..2] = false` (fall back to SD upscale + HD sprites only). Check `g_new_ppu` in `HdCompositor_Init`; print a warning if old PPU and BG HD is enabled.

**Acceptance:**
- Build clean.
- Run Yoshi's Island 1. The debug dump shows `prio_map[112,128]` decoding to `kHdBgLayer_BG1lo` or `kHdBgLayer_BG2lo` (a background tile), or a sprite layer if Mario is centred at screen-centre. Manually verify against what is visually at that pixel.
- Enter a forced-blank frame (save/load state): no crash. Capture guard prevents writing garbage.

---

### Step 9 — HD BG1/BG2 tile compositor

**Goal:** Walk BG1 (PPU layer 0) and BG2 (PPU layer 1) tilemaps from VRAM each frame; for each visible 8×8 tile that resolves to a loaded HD sheet, blit it at the correct HD-scale screen position, gated by the priority map so tiles only overwrite the SD upscale where they actually won the priority battle. Refactor `HdCompositor_Draw` to execute all 11 passes in the correct PPU priority order.

**Read first:**
- [src/snes/ppu.c: `PpuDrawBackground_4bpp`](src/snes/ppu.c) — the reference for tilemap addressing (scroll, `PPU_bgTilemapAdr`, tilemap entry decoding, `PPU_bgTileAdr`, tile VRAM address formula). Mirror its tile-walk logic exactly.
- [src/snes/ppu.h: `PPU_bgTileAdr`, `PPU_bgTilemapAdr`, `PPU_bgTilemapWider`, `PPU_bgTilemapHigher`, `PPU_bg3priority`, `PPU_mode`](src/snes/ppu.h) — macros used in the tile walk.
- [src/hd_vram_map.h](src/hd_vram_map.h) — `HdVramMap_ResolveTile(vram_word_addr, &sheet_id, &tile_in_sheet)`. Same function used for both sprites and BG tiles; the 4bpp 16-word/tile stride applies identically.
- Step 8 output — `g_hd_prio_map`, `HdDecodeZbuf`, `HdBgLayerID`.
- Step 6 output — `HdCompositor_BlitSprite`, `HdCompositor_AlphaBlit`, `g_hd_layer_buf`, `g_hd_shadow_buf`, `HdLayerCfg`.

**Key facts:**

- **Tilemap entry format (16-bit word):**
  - Bits 9–0: tile number (0..1023), index into graphics tileset.
  - Bits 12–10: palette (0..7); BG1/BG2 CGRAM base = `palette * 16` (not `0x80 + palette*16` like sprites).
  - Bit 13: tile priority (0 = lo, 1 = hi).
  - Bit 14: hflip (if set, tile is horizontally flipped).
  - Bit 15: vflip.
- **VRAM tile word-address for 4bpp BG:** `(PPU_bgTileAdr(ppu, layer) + tile_num * 16) & 0x7fff`. Identical formula to `objAdr + usedTile * 16` for sprites.
- **Tilemap addressing with scroll:** `PPU_bgTilemapAdr(ppu, layer)` gives the base. Scrolled tilemap row = `((screen_y + vScroll[layer]) >> 3) & 0x1f`; scrolled col = `((screen_x + hScroll[layer]) >> 3) & 0x1f`. For wide (64-pixel) or tall (64-tile) tilemaps, an additional `0x400`-word offset selects the second horizontal page, and `0x400` (narrow) or `0x800` (wide) selects the second vertical page — mirror `PpuDrawBackground_4bpp` exactly.
- **SD pixel → screen tile position:** the SD pixel at `(sd_x, sd_y)` falls in tile column `((sd_x + hScroll) >> 3) & 0x1f` and tile row `((sd_y + vScroll) >> 3) & 0x1f` of the tilemap. Within the tile: `x_in_tile = (sd_x + hScroll) & 7`, `y_in_tile = (sd_y + vScroll) & 7`.
- **Tri-level flip in the HD blit** (identical to sprites): tile-order flip (through `src_xi`/`src_yi`), within-tile pixel flip (through `src_xi * S + src_px`), and sub-pixel flip (S×S block flip via `src_px`, `src_py`). All three tiers must be applied or flipped tiles render incorrectly.

**Tasks:**

1. **New `HdCompositor_BlitBgLayer` function.** Add to `hd_compositor.c` (file-static):
   ```c
   static void HdCompositor_BlitBgLayer(
       uint8     *hd_buf,         // destination HD frame buffer
       size_t     hd_pitch,
       int        hd_w, int hd_h,
       int        bg_layer,       // 0 = BG1, 1 = BG2
       bool       prio_hi,        // true = only render high-priority tiles
       HdBgLayerID expected_layer, // kHdBgLayer_BG1hi, kHdBgLayer_BG2lo, etc.
       const Ppu *ppu);
   ```
   Algorithm:
   - Early exit if `!g_hd_bg_enabled[bg_layer]` or `PPU_mode(ppu) != 1`.
   - Compute `S = g_hd_scale`, `sd_w = hd_w / S`, `sd_h = hd_h / S`.
   - Read `tileadr = PPU_bgTileAdr(ppu, bg_layer)`, `tilemap_base = PPU_bgTilemapAdr(ppu, bg_layer)`.
   - Read `hs = (int)ppu->hScroll[bg_layer]`, `vs = (int)ppu->vScroll[bg_layer]`.
   - Iterate tile rows and columns covering the visible screen (32 cols × 29 rows maximum to handle partial tiles at edges):
     ```c
     // First tile partially visible at top-left:
     // Its top-left SD pixel is at screen position (pix_x_start, pix_y_start).
     int pix_x_start = -(hs & 7);
     int pix_y_start = -(vs & 7);
     for (int tr = 0; tr <= (sd_h + (vs & 7) + 7) / 8; tr++) {
       int screen_tile_y = pix_y_start + tr * 8;  // SD y of tile top row
       if (screen_tile_y >= sd_h) break;
       uint scrolled_y = (uint)(vs + tr * 8 - (vs & 7));
       int tilemap_row = (scrolled_y >> 3) & 0x1f;
       bool use_y_hi = (scrolled_y & 0x100) && PPU_bgTilemapHigher(ppu, bg_layer);

       for (int tc = 0; tc <= (sd_w + (hs & 7) + 7) / 8; tc++) {
         int screen_tile_x = pix_x_start + tc * 8;
         if (screen_tile_x >= sd_w) break;
         uint scrolled_x = (uint)(hs + tc * 8 - (hs & 7));
         int tilemap_col = (scrolled_x >> 3) & 0x1f;
         bool use_x_hi = (scrolled_x & 0x100) && PPU_bgTilemapWider(ppu, bg_layer);

         // Build tilemap word-address (matches PpuDrawBackground_4bpp's sc_offs logic):
         int sc_offs = (int)tilemap_base;
         if (use_y_hi) sc_offs += PPU_bgTilemapWider(ppu, bg_layer) ? 0x800 : 0x400;
         if (use_x_hi) sc_offs += 0x400;
         uint16 te = ppu->vram[(sc_offs + tilemap_row * 32 + tilemap_col) & 0x7fff];

         int tile_num  = te & 0x3ff;
         bool tile_hi  = (te >> 13) & 1;
         int  palette  = (te >> 10) & 7;
         bool hflip    = (te >> 14) & 1;
         bool vflip    = (te >> 15) & 1;

         if ((bool)tile_hi != prio_hi) continue;  // skip wrong priority tier

         uint16 tile_vram = (uint16)((tileadr + tile_num * 16) & 0x7fff);
         uint8  sheet_id;
         uint16 tile_in_sheet;
         if (!HdVramMap_ResolveTile(tile_vram, &sheet_id, &tile_in_sheet)) continue;
         const HdSheet *sheet = &g_hd_sheets[sheet_id];
         if (!sheet->loaded || sheet->scale != (uint8)S) continue;
         if (tile_in_sheet >= sheet->tile_count) continue;

         int tx = (tile_in_sheet & 0xf) * 8 * S;  // HD x of tile top-left in sheet
         int ty = (tile_in_sheet >> 4)  * 8 * S;  // HD y of tile top-left in sheet
         int pal_base = palette * 16;              // BG CGRAM base (no 0x80 offset)
         const uint8  *idx_buf = sheet->index_buffer;
         int sh_width = (int)sheet->width;

         // Blit the 8×8 SD-pixel tile (expanding each SD pixel to S×S HD pixels):
         for (int yi = 0; yi < 8; yi++) {
           int src_yi  = vflip ? (7 - yi) : yi;
           int sd_y    = screen_tile_y + yi;
           if (sd_y < 0 || sd_y >= sd_h) continue;

           for (int xi = 0; xi < 8; xi++) {
             int src_xi  = hflip ? (7 - xi) : xi;
             int sd_x    = screen_tile_x + xi;
             if (sd_x < 0 || sd_x >= sd_w) continue;

             // Priority gate: only blit where this layer won in the SD frame.
             uint16 z = g_hd_prio_map[sd_y * 256 + sd_x];
             if (HdDecodeZbuf(z) != expected_layer) continue;

             int out_x_base = sd_x * S;
             int out_y_base = sd_y * S;
             int hd_sx = tx + src_xi * S;
             int hd_sy = ty + src_yi * S;

             for (int py = 0; py < S; py++) {
               int src_py = vflip ? (S - 1 - py) : py;
               int dst_y  = out_y_base + py;
               if (dst_y < 0 || dst_y >= hd_h) continue;
               uint32_t *dst_row = (uint32_t *)(hd_buf + (size_t)dst_y * hd_pitch);
               for (int px = 0; px < S; px++) {
                 int src_px = hflip ? (S - 1 - px) : px;
                 int dst_x  = out_x_base + px;
                 if (dst_x < 0 || dst_x >= hd_w) continue;
                 uint8 index = idx_buf[(hd_sy + src_py) * sh_width + hd_sx + src_px];
                 if (index == 0) continue;   // transparent in HD sheet
                 uint16 color = ppu->cgram[pal_base + index];
                 uint8 r = ppu->brightnessMult[(color >>  0) & 0x1f];
                 uint8 g = ppu->brightnessMult[(color >>  5) & 0x1f];
                 uint8 b = ppu->brightnessMult[(color >> 10) & 0x1f];
                 dst_row[dst_x] = (uint32_t)b | ((uint32_t)g << 8) |
                                  ((uint32_t)r << 16) | 0xff000000u;
               }
             }
           }
         }
       }
     }
     ```

2. **Refactor `HdCompositor_Draw` to the 11-pass loop.** Replace the existing `for (int layer = 0..3)` sprite loop with the full priority-ordered sequence. Wrap the entire block in `if (g_hd_scale > 1 && PPU_mode(g_my_ppu) == 1)` (mode 7 falls through to SD upscale + sprites-on-top as before).

   The composite function writes directly to `dst` (the main HD buffer) for BG tile passes. Sprite passes continue to use the `g_hd_layer_buf` / `g_hd_shadow_buf` → `AlphaBlit` mechanism from Step 6.

   ```c
   bool bg3prio = PPU_bg3priority(g_my_ppu) != 0;

   // Pass 1: BG3 lo  (Step 10 adds this)
   // HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 2, false, kHdBgLayer_BG3lo, g_my_ppu);

   // Pass 2: Sprite priority 0 (with shadow)
   HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, 0, g_my_ppu);

   // Pass 3: BG3 hi when !bg3prio  (Step 10 adds this)
   // if (!bg3prio) HdCompositor_BlitBgLayer(..., kHdBgLayer_BG3hi_noprio, ...);

   // Pass 4: Sprite priority 1
   HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, 1, g_my_ppu);

   // Pass 5: BG2 lo
   HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 1, false, kHdBgLayer_BG2lo, g_my_ppu);

   // Pass 6: BG1 lo
   HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 0, false, kHdBgLayer_BG1lo, g_my_ppu);

   // Pass 7: Sprite priority 2
   HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, 2, g_my_ppu);

   // Pass 8: BG2 hi
   HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 1, true,  kHdBgLayer_BG2hi, g_my_ppu);

   // Pass 9: BG1 hi
   HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 0, true,  kHdBgLayer_BG1hi, g_my_ppu);

   // Pass 10: Sprite priority 3
   HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, 3, g_my_ppu);

   // Pass 11: BG3 hi when bg3prio  (Step 10 adds this)
   // if (bg3prio) HdCompositor_BlitBgLayer(..., kHdBgLayer_BG3hi_prio, ...);
   ```

   Extract the v1 shadow+sprite per-layer logic into a helper `HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, int oam_prio, const Ppu *ppu)`. Its body is the shadow pre-pass + sprite color pass from Step 6, filtered to `s->layer == oam_prio`.

3. **No changes to `HdVramMap`.** BG tile resolution uses the same `HdVramMap_ResolveTile` as sprites. BG sheet uploads already go through `UploadGraphicsFiles_UploadGFXFile`, which records them via `HdVramMap_RecordSheetUpload`. No new tracking code is needed for BG1/BG2.

**Gotchas:**

- **Tilemap page selection with large scrolls.** `scrolled_x` or `scrolled_y` can exceed 255 when the scroll is large (e.g. `hs = 400`). The `& 0x100` check for the second page must operate on the *per-tile* scrolled position `(uint)(hs + tc * 8 - (hs & 7))`, not on a screen-pixel coordinate. Use `uint` arithmetic to avoid sign-extension bugs; wrap `scrolled_x >> 3 & 0x1f` for the row/col index within the page.
- **`pix_x_start` / `pix_y_start` are negative.** A tile that is partially scrolled off the left/top has `screen_tile_x < 0`; individual SD pixels with `sd_x < 0 || sd_y < 0` are skipped by the guards inside the pixel loop. Do not special-case the tile — skip at the *pixel* level.
- **Priority gate false negative.** If a BG tile has index 0 (transparent) in the HD sheet but the SD version had a non-zero colour, the SD pixel shows through — the priority gate passes but `index == 0 → continue`, leaving the SD upscale pixel intact. Correct behaviour: the SD upscale shows the SD tile where the HD tile is transparent.
- **Priority gate and sprite overlap.** A sprite at prio 2 standing in front of a BG1 lo tile: the SD pixel at the sprite's location has a *sprite* priority (0xA4xx), not BG1 lo (0x80xx). The gate blocks the BG1 lo tile from overwriting it. Then the sprite compositor (pass 7) blits the HD sprite on top. Correct.
- **BG tile blitting into `dst` directly (not through the work buffer).** Unlike sprites, BG tile blits write directly into the main HD buffer (equivalent to `alpha=255` immediate overwrite). They don't go through `g_hd_layer_buf` → `AlphaBlit`. Rationale: HD BG tiles fully replace the SD upscale at their locations; no per-layer alpha effects are planned for BG tiles in v2.
- **`brightnessMult` is the final-scanline value.** Same limitation as sprites (v1 known limitation). Acceptable.
- **Mode 7 gate.** `PPU_mode(g_my_ppu) == 1` is mode 1. For mode 7, skip all BG passes and use v1 sprite-on-top. Mode 7 frames have no BG1/BG2 in the normal sense.
- **`HdCompositor_BlitBgLayer` is called directly for each priority tier, not through a work buffer.** Shadow effects on BG tiles are out of scope for v2.

**Acceptance:**
- Build clean, no new warnings.
- Yoshi's Island 1 with `gfx/hd/gfx00.png` loaded (the first FG tile sheet): FG tiles that map to sheet 0x00 render in HD. Tiles without HD sheets show the nearest-upscaled SD fallback.
- A tile with prio=1 and an enemy (prio-2 sprite) in front of it: the HD tile does not occlude the SD enemy — the enemy appears correctly in front of the tile (confirmed by the priority gate).
- A tile with prio=1 and Mario (prio-2 sprite, if HD Mario sheet is loaded) walking in front: HD Mario renders on top of the HD tile. Correct.
- Scroll: walk Mario so the camera moves; HD BG tiles scroll smoothly with the background.
- Disable `g_hd_bg_enabled[0] = false` at compile time: BG1 shows as SD upscale (same as v1). Sprites still HD.

---

### Step 10 — 2bpp tile support + BG3 compositor

**Goal:** Extend `HdVramMap` to handle 2bpp tile addresses (BG3 in mode 1 uses 8 VRAM words per tile rather than 16). Hook `UploadGraphicsFiles_Layer3` to record BG3 sheet uploads. Add BG3 to the priority-ordered compositor pass.

**Read first:**
- [src/snes/ppu.c: `PpuDrawBackground_2bpp`](src/snes/ppu.c) — 2bpp tile address formula: `(ta + tile_num * 8) & 0x7fff`. kPaletteShift = 8; palette contribution to z-value is `(tile & 0x1c00) >> 8` (0..7 directly, not ×16). Pixel range 1..3.
- [src/smw_00.c: `UploadGraphicsFiles_Layer3`](src/smw_00.c) — calls `SmwCopyToVram(0x4000 + i * 0x400, GraphicsDecompress(40 + i), 0x800)` for i=0..3. Sheets 0x28..0x2B (decimal 40..43), VRAM word addresses 0x4000, 0x4400, 0x4800, 0x4C00. Each upload is 0x800 bytes = 0x400 VRAM words = 64 tiles at 8 words/tile (2bpp).
- [src/hd_vram_map.h](src/hd_vram_map.h) — current `HdVramRegion` struct; `ResolveTile` assumes 16-word stride.
- Step 9 output — `HdCompositor_BlitBgLayer` and the 11-pass loop with the BG3 passes commented out.

**Key facts:**

- BG3 is 2bpp. Each tile occupies 8 VRAM words (not 16). The VRAM address of tile N is `(PPU_bgTileAdr(ppu, 2) + N * 8) & 0x7fff`.
- The existing `HdVramMap_ResolveTile` finds a region and computes `tile_in_sheet = (vram_word_addr - region.vram_word_addr) / 16`. This division by 16 is wrong for 2bpp tiles; it would halve the tile index.
- Sheet gfx28..gfx2B are 2bpp *source* tiles. When loaded as HD PNGs they are encoded the same way as 4bpp sheets (one index byte per pixel, index 0 = transparent). Max index = 3 (2bpp).
- BG3 palettes: `palette * 4` CGRAM base (4 entries per 2bpp tile), not `palette * 16`. The palette index in the tilemap entry is bits 12–10 (same bits as 4bpp), but each palette has only 4 colours.
- `UploadGraphicsFiles_Layer3` uses `SmwCopyToVram` (Path B), not `UploadGraphicsFiles_UploadGFXFile` (Path A). The current VRAM map has no staging entry for sheets 0x28..0x2B.

**Tasks:**

1. **Extend `HdVramRegion` with a `tile_stride` field.** In [src/hd_vram_map.h](src/hd_vram_map.h):
   ```c
   typedef struct HdVramRegion {
     uint16 vram_word_addr;
     uint16 tile_count;
     uint8  sheet_id;
     uint16 src_tile_offset;
     uint8  tile_stride;   // NEW: VRAM words per tile: 8 (2bpp) or 16 (4bpp)
   } HdVramRegion;
   ```
   Existing callers of `HdVramMap_RecordSheetUpload` do not pass a stride; add a `tile_stride` parameter (default 16). Update all call sites — currently only `UploadGraphicsFiles_UploadGFXFile`.

2. **Update `HdVramMap_ResolveTile` to use the stride.** Replace the hard-coded `/ 16` with `/ region->tile_stride`. No change to the resolution algorithm otherwise.

3. **Add `HdVramMap_RecordSheetUpload_2bpp` (or add a stride parameter to the existing function).** Prefer a single function with an explicit `tile_stride` parameter. Update the existing call in [src/smw_00.c](src/smw_00.c):
   ```c
   HdVramMap_RecordSheetUpload(dst_addr, j, 0, 128, 16);   // 4bpp as before
   ```

4. **Hook `UploadGraphicsFiles_Layer3`.** At the end of the function body, after the four `SmwCopyToVram` calls, add:
   ```c
   // Record BG3 (2bpp) sheet uploads in the HD VRAM map.
   for (int i = 0; i < 4; i++)
     HdVramMap_RecordSheetUpload((uint16)(0x4000 + i * 0x400), (uint8)(0x28 + i),
                                 0, 64, 8);  // 64 tiles, 8 words/tile (2bpp)
   ```
   This covers sheets gfx28..gfx2B at VRAM word addresses 0x4000..0x4C00.

5. **`HdCompositor_BlitBgLayer` for BG3.** Add a `tile_stride` parameter (or detect BG3 via `bg_layer == 2`). The only changes from the BG1/BG2 path are:
   - `tile_vram = (PPU_bgTileAdr(ppu, 2) + tile_num * 8) & 0x7fff` (stride 8, not 16).
   - `pal_base = palette * 4` (4 entries per 2bpp palette).
   - Max valid index in the HD sheet is 3 (2bpp), but index 0 is always transparent — same `if (index == 0) continue;` guard applies.

6. **Enable BG3 in the 11-pass loop.** Uncomment the three BG3 pass calls added in Step 9 (passes 1, 3, and 11). Set `g_hd_bg_enabled[2] = true` default in `hd_compositor.c`.

7. **Update `HdCompositor_ApplyConfig`** to read a new config field `g_config.hd_bg_enabled[3]` (added in Step 12) into `g_hd_bg_enabled[0..2]`.

**Gotchas:**
- **Stride mismatch is silent.** If you forget to update a `HdVramMap_ResolveTile` call site, tiles silently misidentify (wrong tile number returned). Cross-check by dumping tile resolutions for a known BG3 tile at the Yoshi's Island 1 title box.
- **`UploadGraphicsFiles_Layer3` first uploads four Layer 3 text/number sheets, then calls `UploadGraphicsFiles_UploadGFXFile(0x6000, 0, 0)`.** The `UploadGraphicsFiles_UploadGFXFile` call is for the sprite slot at 0x6000, not BG3. Do not confuse the two. Only record the four 0x4000-series VRAM words as 2bpp BG3 uploads.
- **BG3 in SMW is primarily used for Layer 3 foreground overlays (star world text, message boxes) and some title-screen elements.** It is rarely the primary visual interest. Disable with `g_hd_bg_enabled[2] = false` in the config default if BG3 HD sheets are not available.
- **`PPU_bgTileAdr(ppu, 2)`** macro: `(ppu->bgTileAdr >> 8 & 0xf) << 12`. Standard macro from ppu.h; do not recompute manually.

**Acceptance:**
- Build clean.
- With gfx28.png present: Layer 3 text tiles (e.g. "MARIO" HUD letters, if BG3 carries them in the current level) render in HD with correct 2bpp palette colours.
- Confirm `HdVramMap_ResolveTile` returns the correct `tile_in_sheet` for a known 2bpp tile (add a temporary debug print keyed to a specific BG3 VRAM address from the `HdVramMap_Dump` output).
- 4bpp resolution is unaffected: sprite and BG1/BG2 tile mapping continues to return the same results as after Step 9.

---

### Step 11 — Overworld map tile tracking

**Goal:** Identify and hook the graphics upload paths used by the overworld map screen so that `g_hd_prio_map` and the BG tile VRAM map cover overworld tile graphics. The BG tile compositor introduced in Steps 9–10 then works for overworld without further changes.

**Read first:**
- The overworld map likely loads its own set of FG/BG sheets via a code path separate from `UploadGraphicsFiles`. Search for calls to `UploadGraphicsFiles_UploadGFXFile` and `SmwCopyToVram` that execute only during the overworld screen transition:
  ```
  grep -n "UploadGraphicsFiles\|SmwCopyToVram\|overworld\|ow_" src/smw_0*.c | head -80
  ```
- [src/smw_00.c: near `UploadGraphicsFiles`](src/smw_00.c) — look for the overworld-specific graphics upload routine (often named after "overworld" or "OW" in the ROM labels). It may call `UploadGraphicsFiles_UploadGFXFile` with a different GFX list, or bypass it entirely.
- [src/variables.h](src/variables.h) — look for `ow_` prefixed globals related to the overworld tile graphics setting.

**Context (to be confirmed by investigation):**

The overworld uses mode 1 (same as levels) with BG1 and BG2 carrying the overworld map tiles, and BG3 carrying path/icon overlays. The graphics sheets loaded for the overworld are different from the in-level sheets (different entries from the FG/BG GFX lists, or dedicated overworld sheets). If `UploadGraphicsFiles_UploadGFXFile` is already called with the overworld sheet IDs and the correct VRAM destinations, no additional hooking is needed — the VRAM map already records them.

If overworld uploads go through a *different* code path (e.g. a direct memcpy to `RtlGetVramAddr()`), those uploads are invisible to the VRAM map and must be hooked individually.

**Tasks:**

1. **Identify the overworld graphics upload function.** Use the grep above; trace calls from the main overworld loop. Likely candidates: `UploadGraphicsFiles_Overworld` or similar. Check whether it ultimately calls `UploadGraphicsFiles_UploadGFXFile`.

2. **If it calls `UploadGraphicsFiles_UploadGFXFile`:** No additional tracking code needed. Verify by checking the debug dump (`HdVramMap_Dump`) while on the overworld screen — it should show regions for the overworld sheets.

3. **If it writes to VRAM directly (bypassing the hook):** Add a `HdVramMap_RecordSheetUpload(dst_addr, sheet_id, 0, tile_count, 16)` call at the end of the direct-write function, mirroring the hook already added to `UploadGraphicsFiles_UploadGFXFile`.

4. **Verify BG tile compositor on the overworld.** Navigate to the world map; confirm that HD overworld tiles render where sheets are loaded and the priority map shows the expected layer IDs.

**Acceptance:**
- With overworld HD sheets present: overworld map tiles render in HD.
- Entering a level and returning to the overworld does not corrupt tile resolution — `HdVramMap_Reset` (called at level load, or via `HdCompositor_Init`) clears stale regions; the overworld upload re-records fresh regions.
- No regression in level BG tile resolution after the overworld path is hooked.

---

### Step 12 — Config + INI extensions (v2)

**Goal:** Expose BG layer HD enable/disable toggles in `smw.ini`. Add a runtime hotkey to toggle BG HD rendering. Extend `HdCompositor_ApplyConfig` to read the new keys.

**Read first:**
- Step 7 output — `HandleIniConfig` section-1 dispatch, `ParseBool`, `kDefaultKbdControls`, `kKeyNameId`, `HandleCommand`.
- [src/config.h](src/config.h) — add new fields adjacent to the existing HD block.

**Tasks:**

1. **Extend `Config` struct** ([src/config.h](src/config.h)):
   ```c
   // v2 HD BG tile replacement
   bool hd_bg_enabled[3];   // [0]=BG1, [1]=BG2, [2]=BG3. Default: true, true, false.
   ```
   Set defaults in `ParseConfigFile` before `ParseOneConfigFile` runs:
   ```c
   g_config.hd_bg_enabled[0] = true;
   g_config.hd_bg_enabled[1] = true;
   g_config.hd_bg_enabled[2] = false;
   ```

2. **Parse `HdBgLayers` in `HandleIniConfig` section 1.** Comma-separated 3-bool list (e.g. `1,1,0`):
   ```c
   // "HdBgLayers = 1,1,0"
   for (int i = 0; i < 3 && value; i++) {
     ParseBool(value, &g_config.hd_bg_enabled[i]);
     value = NextDelim(&value, ',');
   }
   ```

3. **Apply in `HdCompositor_ApplyConfig`:**
   ```c
   for (int i = 0; i < 3; i++)
     g_hd_bg_enabled[i] = g_config.hd_bg_enabled[i];
   ```

4. **Add `kKeys_ToggleHdBg` hotkey** (Ctrl+B by default). Follow the Step 7 pattern: enum entry, default binding, `kKeyNameId` entry, `HandleCommand` case:
   ```c
   case kKeys_ToggleHdBg: {
     // Cycle through: all-on → sprites-only → all-off → all-on
     bool any = g_hd_bg_enabled[0] || g_hd_bg_enabled[1] || g_hd_bg_enabled[2];
     bool spr_only = !any;
     if (any && (g_hd_bg_enabled[0] || g_hd_bg_enabled[1])) {
       g_hd_bg_enabled[0] = g_hd_bg_enabled[1] = g_hd_bg_enabled[2] = false;
       fprintf(stderr, "HD BG tiles: OFF (sprites only)\n");
     } else {
       g_hd_bg_enabled[0] = g_config.hd_bg_enabled[0];
       g_hd_bg_enabled[1] = g_config.hd_bg_enabled[1];
       g_hd_bg_enabled[2] = g_config.hd_bg_enabled[2];
       fprintf(stderr, "HD BG tiles: ON\n");
     }
     break;
   }
   ```

5. **Document in `smw.ini`** under `[Graphics]`, after the existing HD sprite keys:
   ```ini
   # HD background tile replacement (requires matching PNGs in HdGfxDir).
   # Comma-separated per-layer toggles: BG1, BG2, BG3.
   HdBgLayers = 1,1,0
   ```

**Acceptance:**
- `HdBgLayers = 0,0,0` in INI: game runs with HD sprites only (v1 behaviour). BG passes all early-exit.
- `HdBgLayers = 1,1,0`: BG1 and BG2 tiles render in HD where sheets are present; BG3 stays SD.
- Ctrl+B at runtime: toggles BG tile rendering on/off without restarting.
- Build clean.

---

## Known Limitations (v2 additions)

- **Mode 7 BG rendering is never replaced** with HD content. Bowser fight and overworld rotation/zoom scenes show nearest-upscaled SD BG + HD sprites (sprites-on-top, same as v1).
- **Window masking is not applied to HD BG tiles.** A BG tile partially hidden by a colour window renders its full 8×8 area in HD regardless.
- **Subscreen / colour math.** HD BG tiles do not participate in add/subtract colour math with the subscreen. Pixels that would be colour-blended in SD appear as plain CGRAM colours in HD.
- **Per-scanline brightness ramp on BG tiles.** Same limitation as HD sprites: `brightnessMult` reflects only the last rendered scanline. HDMA-driven mid-frame brightness changes affect BG tiles imprecisely.
- **Transparent HD BG tile pixels expose the SD upscale.** Where an HD sheet has index 0 at a given tile position, the SD nearest-upscale pixel shows. This is intentional: it allows partial HD coverage without visual gaps, at the cost of potential SD/HD mismatch at transparent-pixel boundaries.
- **BG3 (2bpp) HD replacement requires explicit opt-in** (`HdBgLayers = 1,1,1`) because BG3 sheets are rarely the primary visual content. Disable by default to avoid user-visible issues when gfx28..gfx2B PNGs are absent.
- Sub-pixel smooth scrolling for HD BG.
