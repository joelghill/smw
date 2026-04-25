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

**Goal:** display HD replacements for BG1, BG2, and BG3 background tiles, correctly interleaved with HD sprites in PPU priority order, so that BG tiles obscure sprites that the SNES would have placed behind them and vice versa.

**Scope (v2):** BG1 (4bpp FG tileset), BG2 (4bpp BG tileset), BG3 (2bpp UI/text layer), and overworld map tiles. All flow through the same BG tile compositor once their VRAM uploads are tracked.

**Non-goals (v2):**
- Mode 7 scene replacement (Bowser fight, overworld rotation/zoom). SD upscale + HD sprites only.
- Color math (add/subtract subscreen) reproduced in HD — HD tiles use CGRAM directly, no subscreen blending. **See "Color-math caveat" below — this is more visible than v1's color-math limitation.**
- Window clipping on HD BG tiles — HD tiles that are partially window-masked render their full HD area.
- Subscreen (secondary screen) HD replacement.
- Lunar Magic BG GFX routing.
- HD replacement of mosaic-affected layers — falls back to SD for the duration of mosaic.

### Core architectural principle: gate every HD pass against the SD priority map

v1 renders sprites only and assumes "HD sprites on top of everything." v2 renders both BG tiles and sprites in HD with arbitrary z-ordering, so it adopts a different correctness model:

1. **The SD frame (BG + sprites, fully rendered) is the always-on fallback.** It is nearest-upscaled into the HD buffer at the start of each frame. Anything not painted by an HD pass shows the SD pixel — never invisible, only pixelated.

2. **The PPU's per-pixel priority z-buffer (`bgBuffers[0].data`) is captured after each scanline.** This z-buffer reflects which layer (BG1lo/hi, BG2lo/hi, BG3lo/hi, Spr0..3, backdrop) won at each pixel. v2 reads from a CPU-side copy `g_hd_prio_map`.

3. **Every HD pass — BG *and sprite* — is gated against the priority map.** A pass for layer L only writes a pixel if `HdDecodeZbuf(g_hd_prio_map[y*256+x]) == L`. The 11-pass execution order is bookkeeping for shadow effects on sprites; correctness comes from the gate, not the order.

   This is the key correctness fix vs. a "render in priority order without gates" approach. Without sprite gating, a sprite at prio 0 standing behind a BG1hi tile would paint its HD pixels on top of the SD upscale; a later BG1hi pass would only cover them where the HD BG sheet is opaque, leaking the sprite through any transparent HD tile pixels. Gating sprites symmetrically with BG tiles eliminates this class of bug regardless of HD sheet alpha shape.

4. **Untracked content (unmapped sprites, animated tiles, mode-7 frames) shows from the SD baseline.** No content is ever missing.

### Architecture changes (data flow per frame, HD+BG on)

1. Game runs, writes OAM/VRAM/CGRAM/tilemap regs as before.
2. `g_hd_skip_sprites` is **permanently `false` in v2.** Sprites render in SD into `bgBuffers[0]`/`g_my_pixels` so that (a) sprite z-values appear in the priority map and (b) unmapped sprites have SD content as fallback. The v1 sprite-skip mechanism is superseded but left in place for `g_new_ppu == false` (old PPU) fallback.
3. `RtlDrawPpuFrame` clears `g_hd_prio_map` to zero before `draw_ppu_frame()` runs. A zero entry (z=0x0000) decodes as `kHdBgLayer_Backdrop` and blocks every BG/sprite gate, so any line skipped by forced-blank stays as the upscaled SD pixel (which is also backdrop on those lines).
4. `draw_ppu_frame()` runs `ppu_runLine` per scanline:
   - `PpuDrawBackgrounds(ppu, y, false)` renders BG tiles and sprites into `bgBuffers[0]`.
   - Immediately after, the new capture hook copies `bgBuffers[0].data[0..255]` into `g_hd_prio_map[(y-1)*256 ..]`.
   - The CGRAM lookup runs unchanged, producing the full SD frame in `g_my_pixels`.
5. `HdCompositor_Draw` runs:
   a. Nearest-upscale `g_my_pixels` into the HD buffer.
   b. Build `HdScene` from OAM (unchanged from v1).
   c. Execute the 11-pass priority-ordered composite loop. **All passes are gated by `g_hd_prio_map`.** Passes write directly into the main HD buffer; sprite passes still use `g_hd_layer_buf` / `g_hd_shadow_buf` for shadow compositing, but the alpha-blit step from work buffer to main buffer applies the priority gate.

### Priority z-buffer encoding

The PPU stores `dstz[i] = z_base + (palette << palette_shift) + pixel` per pixel. The upper byte of the encoded value identifies the source layer. Pixel and palette bits stay in the lower byte and never carry into the upper byte (max contribution: BG 4bpp `(7<<4) + 15 = 0x7F`; BG 2bpp `(7<<2) + 3 = 0x1F`; sprite contributions stay below 0x10).

| Layer               | Upper byte | Source                                    |
|---------------------|-----------|-------------------------------------------|
| Backdrop            | 0x00 or 0x05 | Pre-clear (0) / `ClearBackdrop` (0x05) |
| BG3 lo              | 0x12      | `zlo=0x1200`                              |
| Sprite prio 0       | 0x24 *or* 0x26 | `SPRITE_PRIO_TO_PRIO(0,level6)`      |
| BG3 hi (no bg3prio) | 0x32      | `zhi=0x3200` when `bgmode & 8 == 0`       |
| Sprite prio 1       | 0x64 *or* 0x66 |                                      |
| BG2 lo              | 0x71      | `zlo=0x7100`                              |
| BG1 lo              | 0x80      | `zlo=0x8000`                              |
| Sprite prio 2       | 0xA4 *or* 0xA6 |                                      |
| BG2 hi              | 0xB1      | `zhi=0xB100`                              |
| BG1 hi              | 0xC0      | `zhi=0xC000`                              |
| Sprite prio 3       | 0xE4 *or* 0xE6 |                                      |
| BG3 hi (bg3prio)    | 0xF2      | `zhi=0xF200` when `bgmode & 8 != 0`       |

Notes:
- Sprite upper bytes have a `+2` variant due to the `level6` parameter of `SPRITE_PRIO_TO_PRIO((prio*4+2)*16 + 4 + (level6?2:0))`. The decoder must handle both 0x24 *and* 0x26 (etc.) — use `>=` thresholds, never exact equality.
- The cascade decoder in `HdDecodeZbuf` works because no two layers' upper-byte ranges overlap. The full mapping for any value is determined entirely by the first matching upper-byte threshold from highest to lowest.

### New symbols (`src/hd_compositor.h`)

```c
extern uint16 *g_hd_prio_map;      // 256 * 240 uint16, alloc in Init / free in Shutdown
extern bool    g_hd_bg_enabled[3]; // [0]=BG1, [1]=BG2, [2]=BG3 — runtime gate per layer

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
```

### 11-pass composite order (executed by `HdCompositor_Draw`)

| # | Pass                       | Expected layer (gate)        |
|---|----------------------------|------------------------------|
| 1 | BG3 lo                     | `kHdBgLayer_BG3lo`           |
| 2 | Sprite prio 0 (+ shadow)   | `kHdBgLayer_Spr0`            |
| 3 | BG3 hi (only if !bg3prio)  | `kHdBgLayer_BG3hi_noprio`    |
| 4 | Sprite prio 1 (+ shadow)   | `kHdBgLayer_Spr1`            |
| 5 | BG2 lo                     | `kHdBgLayer_BG2lo`           |
| 6 | BG1 lo                     | `kHdBgLayer_BG1lo`           |
| 7 | Sprite prio 2 (+ shadow)   | `kHdBgLayer_Spr2`            |
| 8 | BG2 hi                     | `kHdBgLayer_BG2hi`           |
| 9 | BG1 hi                     | `kHdBgLayer_BG1hi`           |
|10 | Sprite prio 3 (+ shadow)   | `kHdBgLayer_Spr3`            |
|11 | BG3 hi (only if  bg3prio)  | `kHdBgLayer_BG3hi_prio`      |

The order is for shadow-stacking determinism only. Every pass gates on its expected layer; a pixel is overwritten iff `HdDecodeZbuf(g_hd_prio_map[y*256+x]) == expected`.

---

### Color-math caveat (read before Step 9)

SMW uses subscreen color math for several visible effects:
- Underwater color overlay (Vanilla Dome, Yoshi's Island 2, etc.)
- Smoke / cloud sprites with half-color blending
- Fade-to-black/white transitions on level entry, death, P-switch

HD BG tiles in v2 sample CGRAM directly and skip color math. Where SD tiles around the HD area are color-blended, HD tiles will look "pure" — visibly different from their surroundings. This is more noticeable than the v1 sprite-only color-math limitation because BG tiles cover much larger screen areas. **Document this prominently in user-facing notes; do not present v2 as "looks like SD but crisper."**

---

### Step 8 — Per-scanline priority map capture

**Goal:** allocate `g_hd_prio_map`, capture `bgBuffers[0].data` after each scanline's `PpuDrawBackgrounds`, expose `HdDecodeZbuf`. No visible behavior change yet — sprites still render in SD, no HD BG path exists.

**Read first:**
- [src/snes/ppu.c: `PpuDrawWholeLine`](src/snes/ppu.c#L659) — sequence: `ClearBackdrop` → `PpuDrawBackgrounds(ppu, y, false)` → optional subscreen → CGRAM lookup loop.
- [src/snes/ppu.h: `PpuPixelPrioBufs`, `kPpuExtraLeftRight`](src/snes/ppu.h) — confirm `bgBuffers[0].data` is `uint16[256 + 2*kPpuExtraLeftRight]`. With `kPpuExtraLeftRight == 0`, indices `0..255` map directly to screen pixels.
- Step 7 outputs in [src/hd_compositor.c](src/hd_compositor.c) — `HdCompositor_Init`, `HdCompositor_Shutdown`, `g_hd_enabled`.

**Tasks:**

1. **Allocate `g_hd_prio_map`.** In `hd_compositor.c`:
   ```c
   uint16 *g_hd_prio_map = NULL;
   bool    g_hd_bg_enabled[3] = { false, false, false };  // off until Step 9b/10
   ```
   In `HdCompositor_Init`:
   ```c
   if (!g_hd_prio_map)
     g_hd_prio_map = (uint16 *)malloc(256 * 240 * sizeof(uint16));
   ```
   In `HdCompositor_Shutdown`: `free(g_hd_prio_map); g_hd_prio_map = NULL;`
   Declare `extern` in `hd_compositor.h`.

2. **Capture hook in `ppu.c`.** Inside `PpuDrawWholeLine`, immediately after `PpuDrawBackgrounds(ppu, y, false)` and before any subscreen / CGRAM code:
   ```c
   // HD compositor: capture per-pixel priority. extern decls used to avoid
   // pulling hd_compositor.h into ppu.c (ppu.c has no SMW deps by design).
   extern uint16 *g_hd_prio_map;
   extern bool    g_hd_enabled;
   if (g_hd_enabled && g_hd_prio_map) {
     int sy = (int)y - 1;  // y is 1-based
     if ((unsigned)sy < 240)
       memcpy(g_hd_prio_map + (size_t)sy * 256, ppu->bgBuffers[0].data, 256 * sizeof(uint16));
   }
   ```
   Forced-blank lines return early before this point — they leave the prior frame's pre-clear (Task 4) untouched.

3. **Implement `HdDecodeZbuf`.** Cascade decoder using `>=` thresholds:
   ```c
   HdBgLayerID HdDecodeZbuf(uint16 z) {
     uint8 hi = (uint8)(z >> 8);
     if (hi >= 0xF2) return kHdBgLayer_BG3hi_prio;
     if (hi >= 0xE4) return kHdBgLayer_Spr3;       // 0xE4 or 0xE6 (level6 bit)
     if (hi >= 0xC0) return kHdBgLayer_BG1hi;
     if (hi >= 0xB1) return kHdBgLayer_BG2hi;
     if (hi >= 0xA4) return kHdBgLayer_Spr2;       // 0xA4 or 0xA6
     if (hi >= 0x80) return kHdBgLayer_BG1lo;
     if (hi >= 0x71) return kHdBgLayer_BG2lo;
     if (hi >= 0x64) return kHdBgLayer_Spr1;       // 0x64 or 0x66
     if (hi >= 0x32) return kHdBgLayer_BG3hi_noprio;
     if (hi >= 0x24) return kHdBgLayer_Spr0;       // 0x24 or 0x26
     if (hi >= 0x12) return kHdBgLayer_BG3lo;
     return kHdBgLayer_Backdrop;                    // covers 0x00 (pre-clear) and 0x05 (ClearBackdrop)
   }
   ```
   Use `>=` (not equality). The level6 bit produces 0x_6 variants for sprite priorities; the cascade subsumes them.

4. **Pre-clear `g_hd_prio_map` per frame.** In `RtlDrawPpuFrame` ([src/main.c:183](src/main.c#L183)), inside the `if (hd_active)` block, before `g_rtl_game_info->draw_ppu_frame()`:
   ```c
   if (g_hd_prio_map)
     memset(g_hd_prio_map, 0, 256 * 240 * sizeof(uint16));
   ```
   Zero ⇒ `kHdBgLayer_Backdrop` ⇒ no gate ever passes ⇒ uncaptured lines stay as the SD upscale (which is also backdrop on those lines).

5. **Debug hook.** In `HdCompositor_DebugDump` (~1 Hz), add:
   ```c
   if (g_hd_prio_map) {
     uint16 z = g_hd_prio_map[112 * 256 + 128];
     fprintf(stderr, "  prio_map[112,128] = 0x%04x → layer %d\n", z, (int)HdDecodeZbuf(z));
   }
   ```

6. **Do not yet flip `g_hd_skip_sprites`.** Leave the v1 sprite-skip behavior as-is for this step. The flip happens in Step 9a together with the pass-loop refactor, so any regression bisects cleanly.

**Gotchas:**
- `kPpuExtraLeftRight` — currently 0. If it ever becomes nonzero, the capture base must shift by `kPpuExtraLeftRight`.
- `g_new_ppu == false` (old PPU): `bgBuffers[0]` is unused. The capture hook is a no-op (data stays as zero). Step 9b/10 must additionally guard BG enable on `g_new_ppu == true`.
- Subscreen pass writes `bgBuffers[1]` — ignored.
- 240-line overscan: `frameOverscan` lines fit because the buffer is allocated at 240 lines.

**Acceptance:**
- Build clean, no warnings.
- Run Yoshi's Island 1; `prio_map[112,128]` decodes to a plausible layer (BG1lo / BG2lo / Spr2 if Mario is centered).
- Save/load (forced-blank during transition) does not crash.
- v1 sprite rendering visually unchanged.

---

### Step 9a — Refactor `HdCompositor_Draw` into the 11-pass loop (BG passes are no-ops)

**Goal:** replace the v1 four-iteration sprite loop with the full 11-pass scaffolding. BG slots are explicit no-op stubs; sprite slots match v1 behavior 1:1. Flip `g_hd_skip_sprites = false` so SD sprites become the always-on baseline. The user-visible result of this step **must be visually identical to post-Step-7 v1.**

**Read first:**
- Current `HdCompositor_Draw` at [src/hd_compositor.c:115](src/hd_compositor.c#L115) — the four-layer loop with shadow then color blits via `g_hd_layer_buf` / `g_hd_shadow_buf` / `HdCompositor_AlphaBlit`.
- `RtlDrawPpuFrame` setting `g_hd_skip_sprites = hd_active` in [src/main.c:183](src/main.c#L183).
- Step 8 outputs.

**Tasks:**

1. **Extract `HdCompositor_CompositeSpriteLayer`.** New file-static helper:
   ```c
   static void HdCompositor_CompositeSpriteLayer(
       uint8 *dst, size_t pitch, int hd_w, int hd_h,
       int oam_prio, const Ppu *ppu);
   ```
   Body is the existing per-layer block from `HdCompositor_Draw`: shadow pre-pass into `g_hd_shadow_buf`, alpha-blit; color pass into `g_hd_layer_buf`, alpha-blit. Filter on `s->layer == oam_prio` exactly as v1 does. **Do not yet add the priority gate** — Step 9b adds it once a real BG layer exists to test the gate against.

2. **Add `HdCompositor_BlitBgLayer` stub.** New file-static helper:
   ```c
   static void HdCompositor_BlitBgLayer(
       uint8 *dst, size_t pitch, int hd_w, int hd_h,
       int bg_layer,                  // 0=BG1, 1=BG2, 2=BG3
       bool prio_hi,
       HdBgLayerID expected_layer,
       const Ppu *ppu) {
     (void)dst; (void)pitch; (void)hd_w; (void)hd_h;
     (void)bg_layer; (void)prio_hi; (void)expected_layer; (void)ppu;
     // Implemented in Step 9b (BG1/BG2 4bpp) and Step 10 (BG3 2bpp).
   }
   ```

3. **Rewrite `HdCompositor_Draw` body** (after the upscale + `HdScene_Build`):
   ```c
   if (g_hd_scale > 1 && PPU_mode(g_my_ppu) == 1) {
     bool bg3prio = PPU_bg3priority(g_my_ppu) != 0;

     HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 2, false, kHdBgLayer_BG3lo,        g_my_ppu);
     HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, 0, g_my_ppu);
     if (!bg3prio)
       HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 2, true,  kHdBgLayer_BG3hi_noprio, g_my_ppu);
     HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, 1, g_my_ppu);
     HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 1, false, kHdBgLayer_BG2lo,        g_my_ppu);
     HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 0, false, kHdBgLayer_BG1lo,        g_my_ppu);
     HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, 2, g_my_ppu);
     HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 1, true,  kHdBgLayer_BG2hi,        g_my_ppu);
     HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 0, true,  kHdBgLayer_BG1hi,        g_my_ppu);
     HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, 3, g_my_ppu);
     if (bg3prio)
       HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 2, true,  kHdBgLayer_BG3hi_prio,  g_my_ppu);
   } else {
     // Mode 7 (or HD off): keep v1 sprite-on-top behavior — sprites render in SD,
     // and no HD passes execute (BG stubs no-op, sprite passes execute and look correct
     // because there's nothing for them to incorrectly cover).
     for (int p = 0; p < 4; p++)
       HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, p, g_my_ppu);
   }
   ```
   Note: `g_my_ppu` is the symbol used elsewhere in [src/hd_compositor.c](src/hd_compositor.c) (e.g. line 129); confirm it resolves before relying on it.

4. **Flip `g_hd_skip_sprites = false`.** In [src/main.c:183](src/main.c#L183) `RtlDrawPpuFrame`:
   ```c
   g_hd_skip_sprites = false;  // v2: sprites always render in SD as baseline
   ```
   Sprites now appear in `g_my_pixels` (the SD frame) AND in `bgBuffers[0]` (the priority map). The HD sprite passes overwrite the SD sprite pixels with HD versions. Because `HdCompositor_BlitSprite` writes opaque HD sprite pixels (not alpha-blended on top of SD), the visual result for tracked sprites is identical to v1; for untracked sprites the SD version now shows through where v1 had blank.

   **Visual diff vs. v1:** previously, a sprite without an HD replacement was invisible. Now it renders as nearest-upscaled SD. This is the only intended visible delta of Step 9a. If the user wants strict v1 parity for unmapped sprites, leave Step 9a's flip out and have Step 9b enable it together with BG rendering.

**Gotchas:**
- `g_new_ppu == false` (old PPU): the priority map will be all zeros and BG passes (when implemented in 9b) will all gate-fail correctly. Sprite passes still work.
- The mode-7 fallback path duplicates v1 exactly. Test by entering the Bowser fight or the world map rotation.

**Acceptance:**
- Build clean.
- Yoshi's Island 1: visually identical to post-Step-7. Frame-by-frame comparison.
- Mode 7 (Bowser fight, OW rotation): visually identical to post-Step-7.
- A level with an enemy whose HD sheet is *not* loaded: enemy now appears as upscaled SD instead of vanishing (this is the intended delta).
- `HdCompositor_DebugDump` continues printing prio map sample.

---

### Step 9b — BG1/BG2 4bpp tile compositor + sprite priority gate

**Goal:** Implement `HdCompositor_BlitBgLayer` for 4bpp BG1/BG2. Add the priority gate to sprite passes too. After this step, BG1/BG2 tiles render in HD where sheets are loaded; HD sprites only paint where SD sprites won at that pixel; transparent HD pixels safely fall through to SD baseline regardless of layer order.

**Read first:**
- [src/snes/ppu.c: `PpuDrawBackground_4bpp`](src/snes/ppu.c#L203) — the canonical tilemap-walk algorithm. **Mirror its structure exactly.** The v1 sprite tri-flip bug came from inventing geometry from scratch instead of mirroring `ppu_evaluateSprites`; do not repeat that mistake here.
- `PPU_bgTileAdr`, `PPU_bgTilemapAdr`, `PPU_bgTilemapWider`, `PPU_bgTilemapHigher`, `PPU_mode`, `PPU_bg3priority`, `PPU_mosaicSize`, `PPU_mosaicEnabled` macros in [src/snes/ppu.h](src/snes/ppu.h).
- [src/hd_vram_map.h](src/hd_vram_map.h) — `HdVramMap_ResolveTile`. The 4bpp 16-words/tile stride is fine for BG1/BG2. (BG3 needs the stride parameter added in Step 10.)
- [src/hd_compositor.c](src/hd_compositor.c) — `HdCompositor_AlphaBlit` (line 183), `HdCompositor_BlitSprite` (line 208), `HdLayerCfg`.

**Key tilemap facts:**

- **Tilemap entry (16-bit word):** bits 0–9 tile_num; bits 10–12 palette; bit 13 prio (0=lo, 1=hi); bit 14 hflip; bit 15 vflip.
- **Tile VRAM word-address (4bpp):** `(PPU_bgTileAdr(ppu, layer) + tile_num * 16) & 0x7fff`.
- **CGRAM base for BG1/BG2 4bpp:** `palette * 16` (not `0x80 + palette*16` — that 0x80 offset is sprite-only).
- **Tilemap addressing with scroll:** mirror lines 217–235 of `PpuDrawBackground_4bpp`. Scrolled `y = screen_y + ppu->vScroll[layer]`; `sc_offs = PPU_bgTilemapAdr + ((y >> 3) & 0x1f) << 5)`; `(y & 0x100) && PPU_bgTilemapHigher` selects the second vertical page; `tps[2]` array gives the two horizontal pages and `(x >> 8) & 1` selects between them.
- **Tri-level flip in the HD blit** (identical to sprites): tile-order flip via `src_xi/src_yi`, within-tile pixel flip via `xi/yi → src_xi/src_yi`, sub-pixel flip via `S×S` block. **All three must be applied.**

**Tasks:**

1. **Implement `HdCompositor_BlitBgLayer` for 4bpp.** Recommended structure: iterate scanline-style, mirroring `PpuDrawBackground_4bpp`'s outer loop.

   Pseudo-code (column-major iteration is fine too; this is the version closest to the PPU reference):
   ```c
   if (!g_hd_bg_enabled[bg_layer]) return;
   if (PPU_mode(ppu) != 1) return;
   if (!g_new_ppu) return;
   if (PPU_mosaicSize(ppu) > 1 && PPU_mosaicEnabled(ppu, bg_layer)) return;  // see gotcha

   int S      = g_hd_scale;
   int sd_h   = hd_h / S;
   int sd_w   = hd_w / S;
   int tileadr = PPU_bgTileAdr(ppu, bg_layer);
   uint8 *idx_buf;  // resolved per-tile

   for (int sy = 0; sy < sd_h; sy++) {
     // Per-row scrolled tilemap pointers (mirror lines 216-223 of PpuDrawBackground_4bpp).
     uint y_scrolled = (uint)(sy + ppu->vScroll[bg_layer]);
     int sc_offs = PPU_bgTilemapAdr(ppu, bg_layer) + (((y_scrolled >> 3) & 0x1f) << 5);
     if ((y_scrolled & 0x100) && PPU_bgTilemapHigher(ppu, bg_layer))
       sc_offs += PPU_bgTilemapWider(ppu, bg_layer) ? 0x800 : 0x400;
     const uint16 *tps[2] = {
       &ppu->vram[sc_offs & 0x7fff],
       &ppu->vram[(sc_offs + (PPU_bgTilemapWider(ppu, bg_layer) ? 0x400 : 0)) & 0x7fff],
     };
     int y_in_tile = y_scrolled & 7;

     // For each SD pixel in the row.
     uint x_scrolled = (uint)ppu->hScroll[bg_layer];
     for (int sx = 0; sx < sd_w; sx++, x_scrolled++) {
       // Priority gate.
       if (HdDecodeZbuf(g_hd_prio_map[sy * 256 + sx]) != expected_layer) continue;

       const uint16 *tp = tps[(x_scrolled >> 8) & 1];
       uint16 te = tp[(x_scrolled >> 3) & 0x1f];

       if (((te >> 13) & 1) != (uint)prio_hi) continue;

       int  tile_num = te & 0x3ff;
       int  palette  = (te >> 10) & 7;
       bool hflip    = (te >> 14) & 1;
       bool vflip    = (te >> 15) & 1;

       uint16 tile_vram = (uint16)((tileadr + tile_num * 16) & 0x7fff);
       uint8  sheet_id;
       uint16 tile_in_sheet;
       if (!HdVramMap_ResolveTile(tile_vram, &sheet_id, &tile_in_sheet)) continue;
       const HdSheet *sheet = &g_hd_sheets[sheet_id];
       if (!sheet->loaded || sheet->scale != (uint8)S) continue;
       if (tile_in_sheet >= sheet->tile_count) continue;

       // Source 8x8 SD pixel within the tile, with within-tile flip.
       int x_in_tile_sd = vflip ? (7 - (int)(x_scrolled & 7)) : (int)(x_scrolled & 7);
       int y_in_tile_sd = vflip ? (7 - y_in_tile)             : y_in_tile;
       // Actually: hflip flips x, vflip flips y. Recompute:
       int xi_sd = hflip ? (7 - (int)(x_scrolled & 7)) : (int)(x_scrolled & 7);
       int yi_sd = vflip ? (7 - y_in_tile)             : y_in_tile;

       int tx_sheet_px = (tile_in_sheet & 0xf) * 8 * S + xi_sd * S;  // top-left of SD pixel in HD sheet
       int ty_sheet_px = (tile_in_sheet >> 4)  * 8 * S + yi_sd * S;
       int sh_w = (int)sheet->width;

       // Expand the 1 SD pixel into S×S HD pixels with sub-pixel flip.
       int out_x = sx * S, out_y = sy * S;
       for (int py = 0; py < S; py++) {
         int src_py = vflip ? (S - 1 - py) : py;
         uint32_t *dst_row = (uint32_t *)(dst + (size_t)(out_y + py) * pitch);
         for (int px = 0; px < S; px++) {
           int src_px = hflip ? (S - 1 - px) : px;
           uint8 index = sheet->index_buffer[(ty_sheet_px + src_py) * sh_w + tx_sheet_px + src_px];
           if (index == 0) continue;  // transparent → leave SD baseline (or whatever lower pass painted)
           uint16 color = ppu->cgram[palette * 16 + index];
           uint8 r = ppu->brightnessMult[(color >>  0) & 0x1f];
           uint8 g = ppu->brightnessMult[(color >>  5) & 0x1f];
           uint8 b = ppu->brightnessMult[(color >> 10) & 0x1f];
           dst_row[out_x + px] = (uint32_t)b | ((uint32_t)g << 8) |
                                 ((uint32_t)r << 16) | 0xff000000u;
         }
       }
     }
   }
   ```

   Notes on this structure:
   - One prio map check per SD pixel. The inner `S×S` block is unconditional once the SD pixel passes the gate. This keeps the hot loop tight.
   - Per-row tilemap pointer setup amortizes across 256 SD pixels.
   - **No tile-row early-exit on prio mismatch.** Within an 8-pixel tile, *some* SD pixels may pass the gate (where this layer won) and some may not (where a higher layer occluded). Per-pixel gating is mandatory.
   - **Performance check:** at S=4, this is `256*240 = 61,440` gate reads × 8 BG passes = ~492K reads/frame. Tile resolution caches well via row-pointer reuse. Acceptable.

2. **Add the priority gate to sprite passes.** Modify `HdCompositor_CompositeSpriteLayer` so that the alpha-blit step from work buffer to main buffer skips pixels where `HdDecodeZbuf(g_hd_prio_map[sd_y * 256 + sd_x]) != kHdBgLayer_Spr<oam_prio>`. The cleanest implementation:

   - Add a parameter to `HdCompositor_AlphaBlit`: `HdBgLayerID gate_layer` (or pass `-1` to disable).
   - When gating is active, for each output HD pixel `(hx, hy)`, compute `sd_x = hx / S`, `sd_y = hy / S`, look up the prio map, and skip the alpha blend if the decoded layer != `gate_layer`.

   Apply to both the shadow alpha-blit and the color alpha-blit. The shadow inherits the same gate as its sprite: a shadow should only render where the sprite itself would render.

   Map `oam_prio` → `HdBgLayerID`:
   ```c
   static const HdBgLayerID kHdSpritePrioToLayer[4] = {
     kHdBgLayer_Spr0, kHdBgLayer_Spr1, kHdBgLayer_Spr2, kHdBgLayer_Spr3,
   };
   ```

3. **Default-enable BG1/BG2.** In `hd_compositor.c`:
   ```c
   bool g_hd_bg_enabled[3] = { true, true, false };  // BG3 stays off until Step 10
   ```

4. **No `HdVramMap` changes.** BG1/BG2 use the existing 4bpp 16-word stride. `UploadGraphicsFiles_UploadGFXFile` already records uploads via the v1 hook.

**Gotchas:**

- **Mosaic.** When `PPU_mosaicEnabled(layer)` and `PPU_mosaicSize > 1`, the SD frame uses block-replicated pixels. HD tile blits ignore mosaic and would render at full HD resolution out of sync with the SD pixel block they replace. Cleanest v2 behavior: **fall back to SD upscale for that layer this frame** (the early-exit guard above). This matches the v2 known limitation.
- **`g_new_ppu == false`.** Old PPU does not populate `bgBuffers[0]`. The early-exit on `!g_new_ppu` keeps the SD upscale visible; print a one-time warning at startup.
- **Sprite gate, level6 bit.** Sprite z values include the 0x_6 variant (palette ≥ 4). The cascade decoder maps both 0x_4 and 0x_6 to the same `Spr<n>`, so the gate works without special handling.
- **Sprites that span priority levels per-pixel.** An OAM sprite is a single priority level — all its pixels get the same z when drawn. The gate treats them uniformly.
- **Per-pixel gate cost in `HdCompositor_AlphaBlit`.** At HD scale 4, sprite work buffers are 1024×896. The gate division `hx / S` is constant-strength on common scales (S=2,4,8 are bit shifts). Hoist `S` to a local; inline the division as a shift if `S` is a known power of two, else compute `sd_x` once per HD column via outer loop.
- **Gate failures and HD sprite occlusion by HD BG.** A sprite occluded by a BG tile will have its prio map entry stomped to BGNlo/hi. The sprite gate then fails at those pixels and the BG pass paints. This is correct and *replaces* the post-hoc "BG covers HD sprite" mechanism that would have failed when HD BG sheets had transparent pixels. Verify by placing an HD-mapped sprite at prio 0 behind an HD-mapped BG1hi tile with a partially transparent HD shape: result should show the BG tile shape, no sprite leakage through the BG transparent gaps. If sprite leakage appears, the sprite gate is broken or off.
- **Index 0 transparency in HD BG sheets.** Pixels with index 0 leave the underlying buffer untouched. Since the buffer started as the SD upscale, transparent HD pixels reveal SD content — correct fall-through.

**Acceptance:**

- Build clean, no new warnings.
- Yoshi's Island 1 with at least `gfx00.png` present (FG tiles for that level): tiles backed by sheet 0x00 render at HD scale; tiles without HD show as upscaled SD; visible boundary between HD and SD tiles is sharp but content-correct.
- Sprite-behind-tile test: HD-mapped sprite prio 0 standing behind HD-mapped BG1hi tile → tile occludes sprite; no transparent-pixel sprite leakage.
- Sprite-in-front-of-tile test: HD-mapped Mario (prio 2) walking over HD BG1lo tile → Mario fully on top.
- Scroll test: walk Mario; HD tiles scroll smoothly with the camera.
- Mosaic test (P-switch end animation, level intro mosaic): mosaic layers fall back to SD upscale; no artifacts.
- Compile-time `g_hd_bg_enabled[0] = false`: BG1 returns to SD upscale; sprites unaffected.

---

### Step 10 — 2bpp tile support + BG3 compositor

**Goal:** track BG3 (2bpp) sheets in `HdVramMap` with the correct 8-word/tile stride, hook `UploadGraphicsFiles_Layer3` to record sheet uploads, and add the BG3 variant of `HdCompositor_BlitBgLayer`. Wire the BG3 passes (1, 3, 11) in the loop.

**Read first:**
- [src/snes/ppu.c: `PpuDrawBackground_2bpp`](src/snes/ppu.c#L300) — 2bpp address formula `(ta + tile * 8) & 0x7fff`; `kPaletteShift = 8` ⇒ `(tile & 0x1c00) >> 8` = `palette << 2` ⇒ palette base `palette * 4` in CGRAM.
- [src/smw_00.c: `UploadGraphicsFiles_Layer3`](src/smw_00.c) — calls `SmwCopyToVram(0x4000 + i*0x400, GraphicsDecompress(40 + i), 0x800)` for i=0..3. Sheets gfx28..gfx2B.
- [src/hd_vram_map.h](src/hd_vram_map.h) — current `HdVramRegion` (no stride), `HdVramMap_ResolveTile` (hard-codes `/16`).
- Step 9b output.

**Why we hook Layer3 directly (Path A) rather than via the existing Path B staging-buffer mechanism:** `UploadGraphicsFiles_Layer3` uses `SmwCopyToVram` (Path B), but BG3 source pointers (output of `GraphicsDecompress(40+i)`) are not registered in the staging-buffer registry — only sheet 0x32 is. We could register four more staging buffers, but the sources for BG3 are dynamically allocated/decompressed, complicating lifetime tracking. A direct `HdVramMap_RecordSheetUpload` call at the end of `UploadGraphicsFiles_Layer3` is the simpler, more robust choice. Document this in the function with a one-line comment so the next reader knows why both paths exist for similar uploads.

**Tasks:**

1. **Add `tile_stride` to `HdVramRegion`.** [src/hd_vram_map.h](src/hd_vram_map.h):
   ```c
   typedef struct HdVramRegion {
     uint16 vram_word_addr;
     uint16 tile_count;
     uint8  sheet_id;
     uint16 src_tile_offset;
     uint8  tile_stride;   // VRAM words/tile: 16 (4bpp) or 8 (2bpp)
   } HdVramRegion;
   ```

2. **Add `tile_stride` parameter to `HdVramMap_RecordSheetUpload`.** Update the existing call in `UploadGraphicsFiles_UploadGFXFile`:
   ```c
   HdVramMap_RecordSheetUpload(dst_addr, j, 0, 128, /*tile_stride=*/16);
   ```
   Plus the Path B variant `HdVramMap_RecordCopyFromStaging` — verify whether it also needs a stride argument. (For SMW, all staging-registered sheets are 4bpp, so a constant 16 there is acceptable.)

3. **Update `HdVramMap_ResolveTile`** to use the per-region stride:
   ```c
   tile_in_sheet = (vram_word_addr - region->vram_word_addr) / region->tile_stride + region->src_tile_offset;
   ```
   No other change to the resolution algorithm.

4. **Hook `UploadGraphicsFiles_Layer3`.** In [src/smw_00.c](src/smw_00.c), at the end of the function (after the four `SmwCopyToVram` calls and before any unrelated `UploadGraphicsFiles_UploadGFXFile(0x6000, …)` sprite call):
   ```c
   // HD compositor: record BG3 (2bpp) sheets. Hooked here directly rather than via
   // the SmwCopyToVram path because BG3 source pointers are not registered in the
   // staging registry (only sheet 0x32 is).
   for (int i = 0; i < 4; i++)
     HdVramMap_RecordSheetUpload((uint16)(0x4000 + i * 0x400),
                                 (uint8)(0x28 + i), 0, /*tile_count=*/64,
                                 /*tile_stride=*/8);
   ```

5. **Add a BG3 variant of `HdCompositor_BlitBgLayer`.** Easiest approach: parameterize the existing function by `tile_stride` and `palette_size` (16 for 4bpp, 4 for 2bpp). Internal differences:
   - `tile_vram = (PPU_bgTileAdr(ppu, 2) + tile_num * tile_stride) & 0x7fff`.
   - `cgram_base = palette * palette_size`.
   - HD sheet tile_count for 2bpp sheets: 64 (one full sheet).
   - Index 0 always transparent — same guard.

   Pass `tile_stride` and `palette_size` from the caller; or split into `HdCompositor_BlitBgLayer_4bpp` and `HdCompositor_BlitBgLayer_2bpp` if the parameterization gets messy. Either is fine — pick whichever keeps the inner loop readable.

6. **Wire BG3 passes (1, 3, 11) in `HdCompositor_Draw`** (uncomment the stubs from Step 9a). Also default-enable BG3:
   ```c
   bool g_hd_bg_enabled[3] = { true, true, true };
   ```

**Gotchas:**

- **Stride mismatch is silent.** Forget to update one `RecordSheetUpload` call site and tiles silently misidentify (off by a factor of 2). Add a one-time `HdVramMap_Dump` line for known BG3 VRAM addresses (0x4000–0x4FFF) to verify resolution after this step.
- **`UploadGraphicsFiles_Layer3` may end with `UploadGraphicsFiles_UploadGFXFile(0x6000, 0, 0)`** for the *sprite* slot at 0x6000 — that is unrelated to BG3 and is already hooked by the v1 mechanism. Do not record it as 2bpp.
- **BG3 palette overlap.** SNES BG3 palettes (8 × 4 entries = 32 colors) live in CGRAM 0..31, overlapping BG1/BG2 palettes 0–1. This is normal SNES behavior; the HD path simply uses `cgram[palette*4 + index]`, which produces the correct color even with overlap.
- **2bpp pixel range is 1..3.** `if (index == 0) continue;` covers transparent. Indices 4..15 in a 2bpp sheet PNG would be a sheet-encoding bug — emit a stderr warning at sheet load time if any pixel ≥ 4 appears in a 2bpp sheet (sheet IDs 0x28..0x2B).
- **SMW BG3 usage:** message boxes, status/HUD numbers, some title-screen text. Often empty (z = backdrop) on most pixels. The compositor will gate-fail on most pixels and exit fast.

**Acceptance:**

- Build clean.
- With gfx28.png present and HD scale > 1: BG3 message-box / "SUPER MARIO WORLD" letters render at HD scale.
- 4bpp resolution unchanged: sprites and BG1/BG2 still render correctly (regression test).
- `HdVramMap_Dump` shows entries for VRAM addresses 0x4000, 0x4400, 0x4800, 0x4C00 with `tile_stride=8` and `sheet_id` 0x28..0x2B respectively.

---

### Step 11 — Overworld map tile tracking

**Goal:** identify how the overworld map screen uploads its tile graphics, ensure those uploads register with `HdVramMap`, and verify the BG compositor renders overworld tiles in HD when sheets are present.

**Why this is investigative, not prescriptive:** the overworld may use the same `UploadGraphicsFiles_*` helpers (Path A or B), in which case it is already hooked, or it may use a dedicated path that needs a new hook. Until we trace the real call graph, the prescriptive plan would be guesswork.

**Tasks:**

1. **Trace overworld graphics upload.** Suggested investigation:
   ```sh
   grep -n "Overworld\|overworld\|ow_" src/smw_0*.c | grep -iE "upload|gfx|vram"
   grep -n "UploadGraphicsFiles\|SmwCopyToVram" src/smw_0*.c | head -80
   ```
   Identify any function that runs only during the overworld screen's GFX init or transitions to/from it. Cross-reference with [src/variables.h](src/variables.h) for `ow_`-prefixed globals and game-mode variables (typically `gameMode == 0x0E` or similar for overworld).

2. **Classify the upload path:**
   - **Path A (no new code):** if the function ultimately calls `UploadGraphicsFiles_UploadGFXFile`, the existing hook records it. Verify by checking `HdVramMap_Dump` while on the world map.
   - **Path B (existing hook):** if it uses `SmwCopyToVram` with sources already in the staging registry, the existing hook records it.
   - **New path:** if it writes to VRAM directly via `RtlGetVramAddr()` or a unique helper, add `HdVramMap_RecordSheetUpload` at the end of that function with the appropriate stride.

3. **Verify rendering on the overworld.** Walk to a node, enter and exit a level, return to the overworld. Confirm `HdVramMap_Dump` shows the overworld sheet regions. Confirm HD overworld tiles render where sheets are present.

4. **Document the finding.** In a comment near the hook (or in the function itself), record which upload mechanism the overworld uses, so future readers don't repeat the investigation.

**Acceptance:**

- With overworld HD sheets present: overworld map tiles render in HD at the correct positions.
- Round-trip (overworld → level → overworld): no stale VRAM map regions cause incorrect resolution. `HdVramMap_Reset` (called per-level-load if it exists, or on `HdCompositor_Init`) ensures a clean slate.
- If the path turns out to need a new hook, that hook is added and documented; if not, the investigation is recorded in a comment so the next person knows it was checked.
- If hooking is not feasible in this step, document overworld as "SD baseline only — HD overworld pending" in the v2 known limitations and proceed.

---

### Step 12 — Config + INI extensions (v2)

**Goal:** expose BG layer enables in `smw.ini`, apply them in `HdCompositor_ApplyConfig`, add a runtime hotkey for cycling BG HD on/off.

**Read first:** Step 7 outputs — `HandleIniConfig` section-1 dispatch, `ParseBool`, `kDefaultKbdControls`, `kKeyNameId`, `HandleCommand`.

**Tasks:**

1. **Extend `Config`** ([src/config.h](src/config.h)):
   ```c
   bool hd_bg_enabled[3];   // [0]=BG1, [1]=BG2, [2]=BG3
   ```
   Defaults in `ParseConfigFile` (before `ParseOneConfigFile`):
   ```c
   g_config.hd_bg_enabled[0] = true;
   g_config.hd_bg_enabled[1] = true;
   g_config.hd_bg_enabled[2] = true;
   ```

2. **Parse `HdBgLayers` in `HandleIniConfig` section 1.** Comma-separated 3-bool list (e.g. `1,1,0`). Follow the existing comma-list parsing pattern used elsewhere in section 1.

3. **Apply in `HdCompositor_ApplyConfig`:**
   ```c
   for (int i = 0; i < 3; i++)
     g_hd_bg_enabled[i] = g_config.hd_bg_enabled[i];
   ```

4. **Add `kKeys_ToggleHdBg` hotkey (Ctrl+B by default).** Mirror the Step 7 pattern: enum entry, default binding in `kDefaultKbdControls`, name in `kKeyNameId`, `HandleCommand` case. **The simpler two-state cycle:**
   ```c
   case kKeys_ToggleHdBg: {
     bool any_on = g_hd_bg_enabled[0] || g_hd_bg_enabled[1] || g_hd_bg_enabled[2];
     if (any_on) {
       g_hd_bg_enabled[0] = g_hd_bg_enabled[1] = g_hd_bg_enabled[2] = false;
       fprintf(stderr, "HD BG tiles: OFF\n");
     } else {
       g_hd_bg_enabled[0] = g_config.hd_bg_enabled[0];
       g_hd_bg_enabled[1] = g_config.hd_bg_enabled[1];
       g_hd_bg_enabled[2] = g_config.hd_bg_enabled[2];
       fprintf(stderr, "HD BG tiles: ON (per INI)\n");
     }
     break;
   }
   ```
   (The original three-state "all-on → sprites-only → all-off" cycle was specified but the example code only flipped between two states. Two states is simpler and matches what the example actually did.)

5. **Document in `smw.ini`** under `[Graphics]`, after the existing HD sprite keys:
   ```ini
   # HD background tile replacement (requires matching PNGs in HdGfxDir).
   # Comma-separated per-layer toggles: BG1, BG2, BG3.
   HdBgLayers = 1,1,1
   ```

**Acceptance:**

- `HdBgLayers = 0,0,0`: BG passes all early-exit; HD sprites only (v1-equivalent visual modulo the SD-sprite-baseline change from Step 9a).
- `HdBgLayers = 1,1,1` (default): BG layers render in HD where sheets present; SD upscale where not.
- Ctrl+B at runtime: toggles BG tile rendering between INI-defined state and OFF without restarting.
- Build clean.

---

## Known Limitations (v2 additions)

- **Color math is not reproduced for HD tiles.** Underwater overlays, fade transitions, half-color smoke, and other subscreen-blended effects show pure CGRAM colors on HD tiles while surrounding SD content blends correctly. Visible discontinuity at the boundary. (See "Color-math caveat" above the steps.)
- **Mosaic-affected layers fall back to SD upscale for the duration of mosaic.** P-switch end animation and level-intro fades briefly drop HD on the mosaiced layer.
- **Mode 7 BG rendering is never replaced.** Bowser fight, OW rotation/zoom: full SD frame nearest-upscaled; HD sprites still composite on top using the v1 sprite-on-top fallback path.
- **Animated tile graphics are not tracked in the VRAM map.** Water/lava/coin/animated-block VRAM is uploaded by `UploadLevelExAnimationData` and `UploadOverworldExAnimationData` and not currently registered. Those tiles always show as upscaled SD. Content is never missing.
- **Window masking is not applied to HD BG tiles.** A BG tile partially hidden by a color window renders its full 8×8 area at HD.
- **Subscreen HD replacement is out of scope.** Only main-screen content gets HD treatment.
- **Per-scanline brightness ramp.** `brightnessMult` reflects only the last scanline of the frame; HDMA-driven mid-frame brightness ramps are not applied per-scanline to HD content.
- **Old PPU (`g_new_ppu == false`) disables HD BG.** Falls back to v1 sprite-only on top of upscaled SD. Warning printed at startup.
- **Sub-pixel smooth scrolling for HD BG is not implemented.** HD BG tiles snap to SD-pixel-aligned positions; smooth sub-pixel motion across an HD tile would require interpolation in the blit, deferred.
- **Transparent HD tile pixels reveal SD content underneath.** This is the correct fall-through behavior, but visible as resolution mismatch at sheet alpha boundaries. Authors should keep HD sheet alpha shapes ≥ SD tile shapes if they want pure HD coverage.

