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

- **BG tile replacement** (FG/BG/BG3 layers) — addressed in v2.
- **Mode 7 HD** — addressed in v2 (Step 11′).
- GPU compositor (textured quads, fragment-shader effects, palette LUT uniforms).
- Per-sheet variable scale.
- Lunar Magic hack GFX.
- Mix of SD fallback for unmapped sprites (v1 just skips them; v2 makes them placeholder).

---

## Version 2 — Pure-HD Coverage with Placeholder Fallback

### Overview (v2)

**Goal:** when HD is on, render the entire frame through the HD pipeline — backdrop, BG1/BG2/BG3, sprites, and Mode 7 — with no SD upscale and no per-pixel priority gate. Tiles and sprites without authored HD content render through a procedurally-generated magenta placeholder, so coverage is always 100% and authoring gaps are immediately visible. When HD is off (Ctrl+H), the SD path runs unchanged.

**Scope (v2):**
- Mode 1 BG1/BG2/BG3 tile replacement.
- Mode 7 (Bowser fight, ending) — full HD path with per-HD-pixel matrix transform.
- Overworld map tiles.
- Procedural magenta placeholder coverage for any unmapped tile or sprite.

**Non-goals (v2):**
- Color math (subscreen add/subtract). HD content samples CGRAM directly.
- Window clipping on HD tiles.
- Subscreen HD replacement.
- Lunar Magic BG GFX routing.
- Per-scanline brightness ramps.
- Mosaic reproduction (HD path ignores mosaic — placeholder/HD render at full HD resolution).

### Why this rewrite (vs. the previous v2 plan)

The original v2 plan layered HD content on top of an always-on SD upscale, gated by a per-scanline priority z-buffer captured from the PPU. In practice this:
1. Produced a visible "double image" on every tile whose HD authoring did not exactly match the SD silhouette.
2. Required ~600 lines of mutually-dependent gate plumbing (capture hook, decoder, layer enum, two distinct gate predicates) to keep HD and SD aligned.
3. Made Mode 7 a permanent SD island.

Pure-HD coverage with magenta placeholders sidesteps every one of these. The complexity of the priority gate is replaced by the simple discipline that "every sheet ID must resolve to *something*."

### Architecture (data flow per frame, HD on)

1. Game runs as before; writes OAM/VRAM/CGRAM.
2. `g_hd_skip_sprites = true` whenever `hd_active` — PPU skips sprite rendering. The SD frame `g_my_pixels` is not consumed by the HD path; it is built only because the verification harness still needs it.
3. `HdCompositor_Draw` runs:
   1. **Backdrop fill.** Read `cgram[0]`, apply `brightnessMult`, fill the entire HD buffer with that BGRA color.
   2. **Build `HdScene`** from OAM (unchanged from v1 Step 4).
   3. **If `PPU_mode(ppu) == 7`**, run `HdCompositor_DrawMode7` and return (Step 9′ stubs this to magenta; Step 11′ implements the real path).
   4. **11-pass composite.** Each pass writes unconditionally — no priority gate. BG passes walk their tilemap directly; sprite passes walk `HdScene` filtered by OAM priority. Within a pass, HD-sheet index 0 is transparent. Later passes overwrite earlier passes, exactly mirroring SNES PPU layer ordering.

### Composite order (Mode 1)

| # | Pass                              |
|---|-----------------------------------|
| 1 | BG3 lo                            |
| 2 | Sprite OAM-prio 0 (shadow + color) |
| 3 | BG3 hi (only if !bg3prio)         |
| 4 | Sprite OAM-prio 1 (shadow + color) |
| 5 | BG2 lo                            |
| 6 | BG1 lo                            |
| 7 | Sprite OAM-prio 2 (shadow + color) |
| 8 | BG2 hi                            |
| 9 | BG1 hi                            |
|10 | Sprite OAM-prio 3 (shadow + color) |
|11 | BG3 hi (only if  bg3prio)         |

### What stays from v1 / earlier-v2 work

- Ctrl+H toggle (`g_hd_enabled`, `kKeys_ToggleHdGfx`).
- HD asset loader ([src/hd_gfx.c](src/hd_gfx.c), [src/hd_gfx.h](src/hd_gfx.h)) — extended in Step 8′ to synthesize placeholders.
- VRAM map ([src/hd_vram_map.c](src/hd_vram_map.c)) — extended with a placeholder catch-all so `HdVramMap_ResolveTile` always returns true.
- `HdScene` OAM walk ([src/hd_scene.c](src/hd_scene.c)).
- `HdCompositor_BlitSprite` — sprite blit with tri-level flip (v1 Step 5).
- `HdCompositor_CompositeSpriteLayer` — shadow + color two-pass per OAM-priority layer (v1 Step 6).
- All `HdLayerCfg` per-OAM-priority effects (drop shadow with offset, alpha, color).
- INI config parsing for HD-sprite + shadow keys (v1 Step 7).

### What gets deleted (relative to current `experiment/hd` branch)

- **SD upscale** at start of `HdCompositor_Draw` ([hd_compositor.c:168-176](src/hd_compositor.c#L168-L176)).
- **Priority z-buffer machinery:** `g_hd_prio_map` allocation, capture hook in [ppu.c:673-683](src/snes/ppu.c#L673-L683), per-frame memset in [main.c:188-189](src/main.c#L188-L189), `HdDecodeZbuf`, `HdBgLayerID` enum, `kHdSpritePrioToLayer` table, debug sample.
- **`gate_layer` parameter on `HdCompositor_AlphaBlit`** ([hd_compositor.c:30-31](src/hd_compositor.c#L30-L31)) and the gate logic at [hd_compositor.c:368-371](src/hd_compositor.c#L368-L371).
- **`expected_layer` parameter** and gate at [hd_compositor.c:296](src/hd_compositor.c#L296) on `HdCompositor_BlitBgLayer`.
- **The `else { for p = 0..3 } CompositeSpriteLayer ...` Mode 7 fallback** in `HdCompositor_Draw` — replaced by the dedicated Mode 7 path.

---

### Step 8′ — Magenta placeholder + tear out priority machinery

**Goal:** every sheet slot has content (real or placeholder). `HdVramMap_ResolveTile` always returns true. Drop the priority z-buffer, capture hook, decoder, gate parameters, and gate predicates. After this step, the compositor still draws what it draws today, but nothing depends on `g_hd_prio_map` and unmapped tiles render as magenta with bit-encoded sheet/tile IDs.

**Read first:**
- [src/hd_gfx.c](src/hd_gfx.c) and [src/hd_gfx.h](src/hd_gfx.h) — current PNG loader and `HdSheet` struct.
- [src/hd_vram_map.c](src/hd_vram_map.c) — region list, resolution, staging registry.
- [src/hd_compositor.c](src/hd_compositor.c) — every `g_hd_prio_map` / `HdDecodeZbuf` / `HdBgLayerID` reference.
- [src/snes/ppu.c:673-683](src/snes/ppu.c#L673-L683) — capture hook to remove.

**Tasks:**

1. **Synthesize a single global placeholder sheet.** Allocate one global `g_hd_placeholder` of type `HdSheet` (separate from the `g_hd_sheets[0x34]` array). It is populated once in `HdGfx_LoadAll` after the per-slot PNG load loop, at the configured `g_hd_scale`. Every blit site routes to it when `sheet_id == 0xFE`.

   **Struct shape (reuses existing `HdSheet`).** Same layout as a real sheet so blit code can use it interchangeably:
   - `width  = 128 * S` (16 tiles wide × 8 SD pixels × S).
   - `height =  64 * S` (8 tile rows × 8 SD pixels × S — total 128 tiles).
   - `tile_count = 128`.
   - `scale = S`.
   - `loaded = true`.
   - `index_buffer` — present but unused at runtime; can be left NULL or filled with `1`s (the blit code for placeholder reads from a pre-decoded BGRA buffer instead — see below).

   **Pre-decoded BGRA buffer.** Add a `bgra_buffer` field to `HdSheet` (or a parallel allocation `g_hd_placeholder_bgra` of size `width * height * 4` bytes) holding the placeholder's final BGRA pixels. The blit code branches on `sheet_id == 0xFE` to read directly from this BGRA buffer instead of doing the per-pixel CGRAM lookup. This avoids needing magenta to live in CGRAM.

   **Pixel content per tile (8·S × 8·S HD pixels):**
   - Default fill: opaque magenta `BGRA = 0xFF FF 00 FF` (i.e. red + blue, no green, alpha 1).
   - Top row of the tile (HD pixels y ∈ `[0, S)`): render the **sheet ID** as 8 bits, MSB at left, each bit an `S × S` block. `1` = white (`0xFFFFFFFF`), `0` = black (`0xFF000000`). Width of this row: 8·S HD pixels (full tile width).
   - Second row (HD pixels y ∈ `[S, 2·S)`): render the **tile-in-sheet index** low 8 bits the same way.
   - Remaining rows (`y ∈ [2·S, 8·S)`): magenta.

   At small scales (S=2) this leaves only 4·S=8 HD rows of magenta below the bit pattern — readable. At S=4 there's 24 rows of magenta — clearly identifiable.

   **Routing.** In `HdCompositor_BlitSprite` and `HdCompositor_BlitBgLayer`, immediately after `HdVramMap_ResolveTile`, branch on sheet ID:
   ```c
   const HdSheet *sheet = (sheet_id == 0xFE) ? &g_hd_placeholder : &g_hd_sheets[sheet_id];
   bool use_bgra_direct = (sheet_id == 0xFE);
   ```
   Inner pixel loop: when `use_bgra_direct`, read `dst_pixel = ((uint32_t*)sheet->bgra_buffer)[(ty + src_py) * sheet->width + (tx + src_px)]`; skip the cgram/brightnessMult path. Otherwise unchanged from today.
2. **Pre-register a placeholder catch-all in `HdVramMap`.** In `HdVramMap_Reset`, after clearing `g_region_count`, insert one region at index 0:
   ```c
   g_regions[0].vram_word_addr  = 0x0000;
   g_regions[0].tile_count      = 0x0800;     // covers the full 0x0000..0x7FFF VRAM range
   g_regions[0].sheet_id        = 0xFE;       // reserved: "placeholder"
   g_regions[0].src_tile_offset = 0;
   g_region_count = 1;
   ```
   In `HdVramMap_ResolveTile`, when the best-matching region's `sheet_id == 0xFE`, return a placeholder result: `*sheet_out = 0xFE; *tile_out = (vram_word_addr >> 4) & 0x7F;` (4bpp stride; Step 10′ will adjust for 2bpp). The compositor's blit code detects `sheet_id == 0xFE` and reads from the corresponding placeholder sheet. This means **`HdVramMap_ResolveTile` never returns false in v2** — the existing skip path becomes dead code.
3. **Delete the priority z-buffer.** Remove from [hd_compositor.h](src/hd_compositor.h):
   ```c
   extern uint16 *g_hd_prio_map;
   typedef enum { kHdBgLayer_Backdrop = 0, ... } HdBgLayerID;
   HdBgLayerID HdDecodeZbuf(uint16 z);
   ```
   Remove from [hd_compositor.c](src/hd_compositor.c): the global, the alloc in `HdCompositor_Init`, the free in `HdCompositor_Shutdown`, the decoder, the `kHdSpritePrioToLayer` table, the debug print at [hd_compositor.c:493-497](src/hd_compositor.c#L493-L497), and **the entire `HdGetPrioMapSample` function at [hd_compositor.c:566-570](src/hd_compositor.c#L566-L570)** (also remove its declaration from `hd_compositor.h` if exported). Remove from [main.c:188-189](src/main.c#L188-L189): the per-frame memset. Remove from [ppu.c:673-683](src/snes/ppu.c#L673-L683): the entire capture block.
4. **Drop the `gate_layer` parameter from `HdCompositor_AlphaBlit`.** Remove the gate logic. Update both call sites in `HdCompositor_CompositeSpriteLayer` (shadow blit + color blit) to drop the trailing argument. The function signature becomes `(uint8 *dst, size_t dst_pitch, const uint8 *src, size_t src_pitch, int w, int h)`.
5. **Drop the `expected_layer` parameter from `HdCompositor_BlitBgLayer`.** Remove the gate at [hd_compositor.c:296](src/hd_compositor.c#L296). The rest of the function (per-row tilemap pointer setup, hi/lo prio bit check, `HdVramMap_ResolveTile`, sheet validation, S×S expansion with tri-flip, index-0 transparency) stays. Update the 11-pass call sites in `HdCompositor_Draw` to pass only `(dst, pitch, hd_w, hd_h, bg_layer, prio_hi, ppu)`.

**Gotchas:**
- The placeholder catch-all must always sit at array index 0 (oldest). Other regions are appended newer, and `ResolveTile`'s newest-first scan with smallest-tile-count tiebreak naturally prefers any explicit upload over the catch-all.
- Sheet ID `0xFE` must NOT collide with any real sheet ID. Real sheets are `0..0x33`; sheet `0xFF` was already used as the unmapped sentinel in `HdVramMap_RecordCopyFromStaging`. Use `0xFE` for placeholder, keep `0xFF` as the explicit-unmapped sentinel (it now becomes unreachable but leave the constant for code clarity).
- Don't synthesize placeholders into the index `0xFE` slot of `g_hd_sheets` — keep it in a dedicated `g_hd_placeholder` global (single sheet, used by every unmapped resolution). Saves memory and keeps the loop simpler.

**Acceptance:**
- Build clean. `HdBgLayerID`, `HdDecodeZbuf`, `g_hd_prio_map` are no longer referenced anywhere.
- With **no** HD sheets present and HD on: every tile and every sprite renders as magenta with bit-encoded sheet/tile IDs.
- With only `gfx00.png` present: tiles whose VRAM uploads identify them as sheet 0x00 render correctly; everything else is magenta-with-ID.
- v1 sprite rendering for tracked sprites: visually identical to post-Step-7 v1.

---

### Step 9′ — Pure-HD compositor (Mode 1)

**Goal:** rewrite `HdCompositor_Draw` so HD frames are built from scratch — backdrop fill + 11-pass composite — with no SD upscale anywhere. Mode 7 short-circuits to a magenta-fill stub (replaced by Step 11′). Drop shadows and per-layer effects continue to work.

**Read first:**
- [src/hd_compositor.c:161-212](src/hd_compositor.c#L161-L212) — current `HdCompositor_Draw`.
- [src/hd_compositor.c:222-255](src/hd_compositor.c#L222-L255) — `HdCompositor_CompositeSpriteLayer` (shadow + color two-pass).
- [src/main.c:183-198](src/main.c#L183-L198) — `RtlDrawPpuFrame` and the HD entry point.

**Tasks:**

1. **Replace the SD upscale with backdrop fill.** First section of `HdCompositor_Draw`:
   ```c
   uint16 bd = g_my_ppu->cgram[0];
   uint8  br = g_my_ppu->brightnessMult[(bd >>  0) & 0x1f];
   uint8  bg = g_my_ppu->brightnessMult[(bd >>  5) & 0x1f];
   uint8  bb = g_my_ppu->brightnessMult[(bd >> 10) & 0x1f];
   uint32_t backdrop = (uint32_t)bb | ((uint32_t)bg << 8) | ((uint32_t)br << 16) | 0xff000000u;
   for (int y = 0; y < hd_height; y++) {
     uint32_t *row = (uint32_t *)((uint8 *)dst + (size_t)y * pitch);
     for (int x = 0; x < hd_width; x++) row[x] = backdrop;
   }
   ```
2. **Drop the `sd_pixels`/`sd_width`/`sd_height` params from `HdCompositor_Draw`.** Use `g_my_ppu` and the `g_hd_scale × 256/224` derived dimensions directly. Update the call site at [main.c:194](src/main.c#L194). The call in the SD-only branch (`!hd_active`) is unchanged — it still memcpys `g_my_pixels` into the framebuffer.
3. **Mode 7 short-circuit.** After `HdScene_Build`:
   ```c
   if (PPU_mode(g_my_ppu) == 7) {
     HdCompositor_DrawMode7Placeholder(dst, pitch, hd_width, hd_height);
     return;  // Sprites in Mode 7 are deferred to Step 11′.
   }
   ```

   **`HdCompositor_DrawMode7Placeholder` definition.** New file-static function in `hd_compositor.c`:
   ```c
   static void HdCompositor_DrawMode7Placeholder(uint8 *dst, size_t pitch,
                                                 int hd_w, int hd_h) {
     uint32_t magenta = 0x00FF00FFu | 0xFF000000u;  // BGRA: B=0xFF, G=0x00, R=0xFF, A=0xFF
     for (int y = 0; y < hd_h; y++) {
       uint32_t *row = (uint32_t *)(dst + (size_t)y * pitch);
       for (int x = 0; x < hd_w; x++) row[x] = magenta;
     }
     // Reuse the placeholder's bit-encoded font: in the top-left of the buffer,
     // render the constant byte 0xM7 = 0xB7 (chosen so 'M7' reads as a recognizable
     // bit pattern: 10110111). Two rows of 8 bits each, S×S blocks per bit.
     int S = (int)g_hd_scale;
     uint8 marker[2] = { 0xB7, 0xB7 };  // arbitrary "MODE 7" sentinel
     for (int row_i = 0; row_i < 2; row_i++) {
       uint8 byte = marker[row_i];
       for (int bit = 0; bit < 8; bit++) {
         uint32_t color = (byte & (0x80 >> bit)) ? 0xFFFFFFFFu : 0xFF000000u;
         int bx = bit * S, by = row_i * S;
         for (int py = 0; py < S; py++) {
           uint32_t *r = (uint32_t *)(dst + (size_t)(by + py) * pitch);
           for (int px = 0; px < S; px++) r[bx + px] = color;
         }
       }
     }
   }
   ```
   The two-byte marker is intentionally distinct from any sheet-ID + tile-index pair generated by the regular placeholder, so a Mode 7 placeholder is identifiable on sight.
4. **11-pass composite.** Same call sequence as today, but each call drops its `expected_layer` argument:
   ```c
   bool bg3prio = PPU_bg3priority(g_my_ppu) != 0;
   HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 2, false, g_my_ppu);          // BG3 lo  (Step 10′)
   HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, 0, g_my_ppu);
   if (!bg3prio)
     HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 2, true, g_my_ppu);          // BG3 hi noprio (Step 10′)
   HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, 1, g_my_ppu);
   HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 1, false, g_my_ppu);
   HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 0, false, g_my_ppu);
   HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, 2, g_my_ppu);
   HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 1, true, g_my_ppu);
   HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 0, true, g_my_ppu);
   HdCompositor_CompositeSpriteLayer(dst, pitch, hd_w, hd_h, 3, g_my_ppu);
   if (bg3prio)
     HdCompositor_BlitBgLayer(dst, pitch, hd_w, hd_h, 2, true, g_my_ppu);          // BG3 hi prio (Step 10′)
   ```
   (BG3 calls are no-ops until Step 10′ enables `g_hd_bg_enabled[2]`.)
5. **`HdCompositor_BlitBgLayer` body.** Drop the priority gate at [hd_compositor.c:296](src/hd_compositor.c#L296). The BG3-unsupported guard (`if (bg_layer == 2) return;` if present) goes away once Step 10′ wires BG3. `HdVramMap_ResolveTile` always succeeds after Step 8′, so **delete the now-dead `if (!HdVramMap_ResolveTile(...)) continue;` line** at [hd_compositor.c:312](src/hd_compositor.c#L312) — replace it with the assert-free unconditional resolve plus the placeholder branch from Step 8′ Task 1:
   ```c
   uint8 sheet_id; uint16 tile_in_sheet;
   HdVramMap_ResolveTile(tile_vram, &sheet_id, &tile_in_sheet);  // always true now
   const HdSheet *sheet = (sheet_id == 0xFE) ? &g_hd_placeholder : &g_hd_sheets[sheet_id];
   bool use_bgra_direct = (sheet_id == 0xFE);
   ```
   Keep the existing `if (!sheet->loaded || sheet->scale != S || tile_in_sheet >= sheet->tile_count) continue;` guard — the placeholder is `loaded=true, scale=S, tile_count=128`, so it always passes; real sheets that fail the guard are skipped (rare misconfig). Inside the inner pixel loop, when `use_bgra_direct`, copy the BGRA word directly:
   ```c
   uint32_t pix = ((const uint32_t *)sheet->bgra_buffer)
                    [(ty_px + src_py) * sh_w + tx_px + src_px];
   if ((pix >> 24) == 0) continue;            // alpha-0 = transparent (placeholder is fully opaque)
   dst_row[out_x + px] = pix;
   ```
   When `!use_bgra_direct`, the existing index/cgram/brightnessMult path is unchanged.

6. **`HdCompositor_BlitSprite` placeholder branch.** Same pattern at the existing `HdVramMap_ResolveTile` call site. Add the same two-line `sheet`/`use_bgra_direct` branch immediately after resolve. Inner pixel loop: when `use_bgra_direct`, write the BGRA pixel directly (skipping `cgram[pal_base + index]` and `brightnessMult`); the silhouette path (`mode == kHdBlitSilhouette`) reads the placeholder's BGRA alpha as the silhouette mask — alpha-non-zero pixels become silhouette, alpha-0 stay transparent. Placeholder is fully opaque so silhouettes for unmapped sprites cover their full bounding box (visible but clearly wrong shape — that's the point).

**Gotchas:**
- **Shadow buffer alignment.** The shadow pre-pass renders silhouettes into `g_hd_shadow_buf` with the same blit; placeholder sprites generate placeholder silhouettes. Cosmetic only — once authored, real sprites take over.
- **`g_hd_skip_sprites` stays `= hd_active`.** This is intentional now: SD sprites would only confuse the verification harness, and the HD path no longer consumes the SD frame.
- **`g_new_ppu == false`.** Old PPU does not honor `g_hd_skip_sprites`. The HD path still works because it reads VRAM/CGRAM directly from `g_my_ppu` regardless of which PPU the verification side runs. Document that old-PPU mode produces SD sprites in the verification window only; the HD viewport is unaffected.

**Acceptance:**
- Build clean.
- Yoshi's Island 1 with HD on and **no** sheets: full magenta with bit-encoded IDs everywhere.
- Yoshi's Island 1 with current `gfx32.png` (Mario): Mario renders in HD; everything else is placeholder. No SD pixels visible at any point.
- Ctrl+H toggle: instant SD ↔ HD switch with no transitional artifacts.
- Bowser fight: solid magenta with "MODE 7" marker; no SD upscale.
- Drop shadow on Mario (OAM prio 2): unchanged from v1.
- Performance: SD upscale was O(hd_w × hd_h) per frame; backdrop fill is the same cost. No regression.

---

### Step 10′ — BG3 (2bpp) tile support

**Goal:** wire BG3 tiles through the same compositor with the correct 8-word/tile stride. Identical to the original Step 10, minus all priority-gate plumbing.

**Read first:**
- [src/snes/ppu.c: `PpuDrawBackground_2bpp`](src/snes/ppu.c#L300) — 2bpp address formula and palette base.
- [src/smw_00.c: `UploadGraphicsFiles_Layer3`](src/smw_00.c#L2685) — loads sheets gfx28..gfx2B at VRAM 0x4000..0x4C00.
- [src/hd_vram_map.h](src/hd_vram_map.h) — current region struct (no stride).

**Tasks:**

1. **Add `tile_stride` to `HdVramRegion`.**
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
   For `HdVramMap_RecordCopyFromStaging`: all staging-registered sheets in SMW are 4bpp, so a constant 16 there is acceptable. Document this in a comment.
   For the placeholder catch-all (Step 8′): set `tile_stride = 16` (will be overridden by real uploads with real strides).
3. **Use per-region stride in `HdVramMap_ResolveTile`.**
   ```c
   uint16 tile_offset_in_region = (vram_word_addr - region->vram_word_addr) / region->tile_stride;
   *tile_out = region->src_tile_offset + tile_offset_in_region;
   ```
   For the placeholder fallback (sheet_id 0xFE), use `tile_stride = 16` for the placeholder lookup as well, OR carry the stride through the placeholder branch. Either works since the placeholder is a debug visual.
4. **Hook `UploadGraphicsFiles_Layer3` (Path A direct).** At the end of the function, after the four `SmwCopyToVram` calls and before the unrelated sprite-slot upload:
   ```c
   for (int i = 0; i < 4; i++)
     HdVramMap_RecordSheetUpload((uint16)(0x4000 + i * 0x400),
                                 (uint8)(0x28 + i), 0, /*tile_count=*/64,
                                 /*tile_stride=*/8);
   ```
   Comment briefly that we hook here directly because BG3 source pointers are not registered in the staging registry.
5. **Split `HdCompositor_BlitBgLayer` into `_4bpp` and `_2bpp` variants.** Do NOT parameterize a single function — the inner loops diverge enough on stride, palette base, pixel decode, and sheet `tile_count` that fusing them produces an uglier hot loop than two cleanly-named copies. Rename the existing function to `HdCompositor_BlitBgLayer_4bpp`. Copy it to `HdCompositor_BlitBgLayer_2bpp` with these substitutions:
   - `tile_vram = (PPU_bgTileAdr(ppu, 2) + tile_num * 8) & 0x7fff` (stride 8, not 16).
   - `int pal_base = palette * 4;` (was `palette * 16`).
   - Authored 2bpp sheet `tile_count == 64` (one sheet = 4 tile rows × 16 = 64 tiles). Placeholder's `tile_count` is 128 in both cases — guard `tile_in_sheet >= sheet->tile_count` handles both.
   - Pixel decode is unchanged from the 4bpp path *for the placeholder branch* (BGRA passthrough). For real sheets, the `index_buffer` is still pre-decoded to per-pixel indices in `[0..3]`; index 0 transparent. Confirm sheet encoding from `assets/export_sheets.py`.
   - Hi/lo prio bit check, tri-flip, S×S expansion: identical logic.
   The 11-pass loop in `HdCompositor_Draw` calls `_4bpp` for `bg_layer < 2` and `_2bpp` for `bg_layer == 2`.
6. **Default-enable BG3.**
   ```c
   bool g_hd_bg_enabled[3] = { true, true, true };
   ```

**Gotchas:**
- Forgetting to update one `RecordSheetUpload` site = silent off-by-2 tile misidentification. Add a one-time `HdVramMap_Dump` for BG3 VRAM addresses (0x4000–0x4FFF) to verify resolution.
- BG3 palette overlap with BG1/BG2 in CGRAM 0..31 is normal SNES behavior; `cgram[palette*4 + index]` gives the correct color.
- 2bpp pixel range is 1..3. Indices 4..15 in a 2bpp authored sheet (sheet IDs 0x28..0x2B) are an authoring bug — emit a one-time stderr warning at sheet load time.

**Acceptance:**
- Build clean.
- With `gfx28.png` present: BG3 message-box / status-bar text renders at HD scale.
- Without it: BG3 area renders as magenta-with-ID (placeholder catch-all caught by Step 8′).
- 4bpp regression: BG1/BG2 and sprite rendering unchanged.
- `HdVramMap_Dump` shows entries for VRAM addresses 0x4000, 0x4400, 0x4800, 0x4C00 with `tile_stride=8` and sheet IDs 0x28..0x2B.

---

### Step 11′ — HD Mode 7

**Goal:** replace the Step 9′ Mode 7 magenta stub with a real per-HD-pixel matrix-transform path that samples a dedicated HD Mode 7 sheet. Until the sheet is authored, the placeholder catch-all naturally produces a magenta result (no special-case stub needed).

**Read first:**
- [src/snes/ppu.c: `PpuDrawBackground_mode7`](src/snes/ppu.c) — canonical Mode 7 implementation. Mirror its matrix math exactly.
- PPU Mode 7 register state: `m7matrix[6]` (a, b, c, d, x, y), `hScroll[0]`, `vScroll[0]`. All can change per scanline via HDMA.

**Architecture:**

Mode 7 is fundamentally different from Mode 1's tilemap walk. BG1 in Mode 7 is a single 128×128-tile fixed sheet; per output pixel the PPU computes:
```
u = ((m7a * (x - cx) + m7b * (y - cy)) >> 8) + cx + hScroll
v = ((m7c * (x - cx) + m7d * (y - cy)) >> 8) + cy + vScroll
```
…then samples the tilemap entry at `(u/8, v/8)` and tile pixel at `(u%8, v%8)`. To render at HD resolution along the transformed axes, the entire matrix must be evaluated per HD pixel, and the source must be HD-resolution (not nearest-upscaled SD).

**Tasks:**

1. **Capture per-scanline Mode 7 state.** Allocate `g_hd_mode7_lines` (8 × `uint16` per scanline × 240 scanlines ≈ 3.5 KB). In `PpuDrawWholeLine`, when `PPU_mode == 7`, copy `m7matrix[0..5]`, `hScroll[0]`, `vScroll[0]` into the slot for line `y - 1`. Cleared in `RtlDrawPpuFrame` before `draw_ppu_frame()`.
2. **Add `g_hd_mode7_sheet`.** A separate sheet slot (not in `g_hd_sheets[0x34]`). Loader looks for `gfx_mode7.png` in `HdGfxDir`. Format: a single non-tiled image of `1024·S × 1024·S` pixels (covers the full Mode 7 source space at HD resolution). Pre-decoded to BGRA at load time so the per-pixel sampler is a single memory read.
3. **Implement `HdCompositor_DrawMode7`.** Replaces the Step 9′ stub. **Mirror [`PpuDrawBackground_mode7`](src/snes/ppu.c#L520-L589) line-for-line** for the matrix math — do NOT re-derive. Reference reproduced here so the implementer can transliterate without context-switching:

   ```c
   // From PpuDrawBackground_mode7 (ppu.c lines 528-543, 547-555, 575-587):
   //
   //   int hScroll = ((int16_t)(ppu->m7matrix[6] << 3)) >> 3;     // sign-extend 13-bit
   //   int vScroll = ((int16_t)(ppu->m7matrix[7] << 3)) >> 3;
   //   int xCenter = ((int16_t)(ppu->m7matrix[4] << 3)) >> 3;
   //   int yCenter = ((int16_t)(ppu->m7matrix[5] << 3)) >> 3;
   //   int clippedH = hScroll - xCenter;
   //   int clippedV = vScroll - yCenter;
   //   clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
   //   clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
   //   uint32 ry = PPU_m7yFlip(ppu) ? 255 - y : y;
   //   uint32 m7startX = (ppu->m7matrix[0] * clippedH & ~63)
   //                   + (ppu->m7matrix[1] * ry       & ~63)
   //                   + (ppu->m7matrix[1] * clippedV & ~63) + (xCenter << 8);
   //   uint32 m7startY = (ppu->m7matrix[2] * clippedH & ~63)
   //                   + (ppu->m7matrix[3] * ry       & ~63)
   //                   + (ppu->m7matrix[3] * clippedV & ~63) + (yCenter << 8);
   //   uint32 rx  = PPU_m7xFlip(ppu) ? 255 - x : x;
   //   uint32 xpos = m7startX + ppu->m7matrix[0] * rx;
   //   uint32 ypos = m7startY + ppu->m7matrix[2] * rx;
   //   uint32 dx  = PPU_m7xFlip(ppu) ? -ppu->m7matrix[0] : ppu->m7matrix[0];
   //   uint32 dy  = PPU_m7xFlip(ppu) ? -ppu->m7matrix[2] : ppu->m7matrix[2];
   //   uint32 outside_value = PPU_m7largeField(ppu) ? 0x3ffff : 0xffffffff;
   //   bool char_fill = PPU_m7charFill(ppu);
   //   // per-SD-pixel inner step:
   //   if ((uint32)(xpos | ypos) > outside_value) {
   //     if (!char_fill) continue;            // transparent (screen_over = transparent)
   //     tile = 0;                            // char_fill = repeat tile 0
   //   } else {
   //     tile = ppu->vram[(ypos >> 11 & 0x7f) * 128 + (xpos >> 11 & 0x7f)] & 0xff;
   //   }
   //   uint8 pixel = ppu->vram[tile * 64 + (ypos >> 8 & 7) * 8 + (xpos >> 8 & 7)] >> 8;
   //   // ...

   // HD adaptation:
   int S = (int)g_hd_scale;
   for (int sy = 0; sy < 224; sy++) {
     // Decode this scanline's captured Mode 7 state
     const uint16 *m7 = &g_hd_mode7_lines[sy * 8];
     int hScroll = ((int16_t)(m7[6] << 3)) >> 3;
     int vScroll = ((int16_t)(m7[7] << 3)) >> 3;
     int xCenter = ((int16_t)(m7[4] << 3)) >> 3;
     int yCenter = ((int16_t)(m7[5] << 3)) >> 3;
     int clippedH = hScroll - xCenter;
     int clippedV = vScroll - yCenter;
     clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
     clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
     int16 m7a = (int16)m7[0], m7b = (int16)m7[1], m7c = (int16)m7[2], m7d = (int16)m7[3];
     // y_flip / x_flip / large_field / char_fill captured into a flags byte at m7[?] —
     // add a slot for them in the per-line buffer; OR snapshot them once per frame at
     // line 0 if SMW never animates them mid-frame (verify before relying on that).

     uint32 ry = /* yFlip ? 255-sy : sy */;
     uint32 m7startX = (m7a * clippedH & ~63) + (m7b * ry & ~63)
                     + (m7b * clippedV & ~63) + (xCenter << 8);
     uint32 m7startY = (m7c * clippedH & ~63) + (m7d * ry & ~63)
                     + (m7d * clippedV & ~63) + (yCenter << 8);

     for (int sx = 0; sx < 256; sx++) {
       uint32 rx = /* xFlip ? 255-sx : sx */;
       uint32 xpos = m7startX + m7a * rx;
       uint32 ypos = m7startY + m7c * rx;
       uint32 outside_value = /* largeField ? 0x3ffff : 0xffffffff */;
       bool transparent = false;
       int tile;
       if ((uint32)(xpos | ypos) > outside_value) {
         if (/* !char_fill */) { transparent = true; }
         else                  { tile = 0; }
       } else {
         tile = ppu->vram[(ypos >> 11 & 0x7f) * 128 + (xpos >> 11 & 0x7f)] & 0xff;
       }
       int u_in_tile = (xpos >> 8) & 7;   // 0..7 — SD pixel within the tile
       int v_in_tile = (ypos >> 8) & 7;

       // Now expand to S × S HD pixels for this SD output pixel.
       // Source HD coords:
       //   tile_x_in_sheet = (tile & 0x0F) * 8,  tile_y_in_sheet = (tile >> 4) * 8
       //   hu = (tile_x_in_sheet + u_in_tile) * S
       //   hv = (tile_y_in_sheet + v_in_tile) * S
       // For sub-pixel hx/hy in [0..S):
       //   sample g_hd_mode7_sheet at (hu + hx, hv + hy)
       int tile_x_sd = (tile & 0x0F) * 8 + u_in_tile;
       int tile_y_sd = (tile >> 4)  * 8 + v_in_tile;
       int hu0 = tile_x_sd * S;
       int hv0 = tile_y_sd * S;
       for (int dy = 0; dy < S; dy++) {
         uint32_t *row = (uint32_t *)(dst + (size_t)(sy * S + dy) * pitch);
         for (int dx = 0; dx < S; dx++) {
           uint32_t pix;
           if (transparent) {
             pix = /* backdrop color computed at top of frame */;
           } else if (g_hd_mode7_sheet.loaded) {
             pix = g_hd_mode7_sheet.bgra[(hv0 + dy) * g_hd_mode7_sheet.width + (hu0 + dx)];
           } else {
             // Fall through to the placeholder catch-all so unauthored Mode 7 looks
             // like any other unauthored sheet (magenta + bit-encoded tile id).
             pix = HdMode7_PlaceholderSample(tile, u_in_tile, v_in_tile, dx, dy);
           }
           row[sx * S + dx] = pix;
         }
       }
     }
   }
   ```

   Notes:
   - `m7a..m7d` are signed 16-bit fixed-point (8.8 format). `xpos`/`ypos` accumulate as `uint32` — relying on two's-complement wrap is intentional and matches the PPU.
   - `~63` masking matches the PPU's per-component truncation.
   - `xpos >> 11 & 0x7f` indexes the 128×128 tilemap; `xpos >> 8 & 7` indexes the 8 SD pixels within the tile. These shifts correspond to fixed-point bits, not pixel bits — do not adapt them for HD scale.
   - **Per-line state to capture (Task 1):** `m7matrix[0..5]`, `hScroll`, `vScroll` — at minimum 8 uint16s. If `PPU_m7xFlip` / `m7yFlip` / `m7largeField` / `m7charFill` are confirmed to never change mid-frame in SMW, snapshot them once per frame instead. Verify by adding a one-frame sentinel that errors if any of those flags differ between scanline 0 and scanline 223.
   - `HdMode7_PlaceholderSample` reuses the global `g_hd_placeholder.bgra_buffer` from Step 8′, indexing by `tile` (sheet-tile-index) so each Mode 7 source tile produces its own bit-encoded marker.
4. **Sprites in Mode 7.** OAM sprites still render in Mode 7 (Bowser projectiles, ending text). After `HdCompositor_DrawMode7`, run the existing four sprite-priority passes via `HdCompositor_CompositeSpriteLayer` so sprites composite on top.
5. **Replace the Step 9′ stub.** Delete `HdCompositor_DrawMode7Placeholder`.

**Gotchas:**
- **Per-scanline matrix capture is allowed to be a no-op when `PPU_mode != 7` for that scanline.** Mode can change mid-frame in some hacks; the line buffer should be a per-line snapshot independent of the frame's dominant mode.
- **`screen_over` (wrap / clamp / transparent / repeat-tile) modes** must mirror PPU behavior. See the existing PPU Mode 7 implementation for the correct branches.
- **Memory.** At `S = 4`, `g_hd_mode7_sheet` is `4096 × 4096 × 4` = 64 MB. Acceptable on desktop; flag it as a known cost in Known Limitations. The placeholder is small (load-time generation of the same 1024-px image at scale, but the placeholder buffer can be the standard `128·S × 64·S` tile sheet — Mode 7 sampler just resolves any HD coord to a placeholder tile).

**Acceptance:**
- Build clean.
- Bowser fight with `gfx_mode7.png` authored: arena renders in HD, Bowser sprite composites on top.
- Bowser fight without it: full magenta + bit-encoded IDs (via placeholder catch-all), Bowser sprite still composites in HD on top.
- Ending sequence: zoom-out works; per-scanline capture covers HDMA-driven matrix changes.
- No SD pixels appear in Mode 7 at any time.

---

### Step 12′ — Overworld tile tracking + INI extensions

**Goal:** verify overworld tile uploads register with `HdVramMap`, expose per-BG enable in INI, add a runtime hotkey for cycling BG HD on/off.

**Read first:** Step 7 outputs (config infrastructure, key binding pattern).

**Tasks:**

1. **Trace overworld graphics upload.** Same investigation as the original Step 11:
   ```sh
   grep -n "Overworld\|overworld\|ow_" src/smw_0*.c | grep -iE "upload|gfx|vram"
   grep -n "UploadGraphicsFiles\|SmwCopyToVram" src/smw_0*.c | head -80
   ```
   Identify the overworld-only GFX init path. Cross-reference with `ow_`-prefixed globals and game-mode variables.
2. **Classify the upload path.**
   - **Path A:** if it ends in `UploadGraphicsFiles_UploadGFXFile`, the existing hook records it.
   - **Path B:** if it uses `SmwCopyToVram` with sources already in the staging registry, the existing hook records it.
   - **New path:** if it writes to VRAM via `RtlGetVramAddr()` or a unique helper, add `HdVramMap_RecordSheetUpload` at the end with the appropriate stride.
3. **Verify on the overworld.** Walk to a node, enter and exit a level, return to the overworld. Confirm `HdVramMap_Dump` shows the overworld sheet regions. Confirm HD overworld tiles render where sheets are present (or as placeholders where not).
4. **Add `HdBgLayers` INI key.** Three-bool comma list (e.g. `1,1,1`) parsed in `HandleIniConfig`'s section 1, applied in `HdCompositor_ApplyConfig` to `g_hd_bg_enabled[]`. Default `1,1,1`.
5. **Add `kKeys_ToggleHdBg` hotkey (Ctrl+B default).** Two-state cycle: all-on (per INI) ↔ all-off (BG passes early-exit, exposing backdrop + sprites only). Mirror the v1 Step 7 toggle pattern.
6. **Document in `smw.ini`.** Under `[Graphics]`, after the existing HD-sprite keys.

**Acceptance:**
- Overworld map renders pure HD where sheets are loaded, placeholder otherwise.
- Round-trip overworld → level → overworld: no stale VRAM regions.
- `HdBgLayers = 0,0,0`: BG passes skipped → backdrop + sprites only.
- Ctrl+B at runtime: BG-layer toggle works without restart.
- Build clean.

---

## Known limitations (v2)

- **Color math (subscreen add/subtract) is not reproduced for HD content.** Underwater overlays, half-color smoke, fade transitions: HD content shows pure CGRAM colors. Visible discontinuity is reduced compared to the previous v2 plan because there is no SD-vs-HD boundary, but transition frames still look "cleaner than the SNES."
- **Window clipping not applied to HD content.**
- **Subscreen HD replacement is out of scope.** Main-screen only.
- **Per-scanline brightness ramp** (HDMA on `$2100`) reflects only the last scanline of the frame. HD content uses the final-line `brightnessMult`.
- **Mosaic is not reproduced.** When the SNES mosaics a layer, HD renders that layer at full HD resolution.
- **Animated tile graphics** (water/lava/coins/blocks via `UploadLevelExAnimationData`) need their VRAM uploads tracked or they render as placeholder. Adding hooks per animation source is straightforward but not in scope of these steps; document each animated source as it is encountered.
- **Mode 7 HD memory footprint.** `gfx_mode7.png` at scale 4 is 4096×4096 BGRA ≈ 64 MB resident.
- **Magenta placeholder is intentionally loud.** Final user-facing builds need authored sheets for every active gameplay area; ship-blocking placeholders are the explicit signal for that.
- **OAM priority rotation** (`$2103` bit 7) is ignored — `HdScene_Build` walks OAM 0..127 unconditionally. Same as v1.

