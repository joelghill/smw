# HD Graphics — Implementation Spec

Per-step implementation plan for replacing sprite rendering with HD replacements loaded from `gfx/hd/`. Each step is self-contained; a fresh session (including a smaller/cheaper model like Sonnet) should be able to read the step in isolation and complete it.

Read this Overview and Architecture before picking up any step.

---

## Overview

**Goal:** display HD sprite replacements (integer scale, greyscale-palette-index-encoded PNGs in `gfx/hd/`) in place of the PPU's SD sprite output. Backgrounds continue to render from the PPU at SD, upscaled nearest. HD sprites composite on top with per-layer visual effects (e.g., drop shadows).

**Scope (v1):** sprites only. No background replacement, no mode-7 handling, no Lunar Magic support.

**Non-goals:**
- Preserving BG→sprite priority interleaving. HD sprites render on top of all BG regardless of BG priority bits. Accepted limitation.
- GPU compositor. CPU-only. Port to GPU is a later project.
- BG replacement, mode 7, color-math accuracy.

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

- RGBA PNG, tile grid is **16 tiles wide × 8 tiles tall** = 128 tiles per sheet.
- Each source tile is 8×8 SNES pixels. At scale `S`, tile is `8S × 8S` pixels, sheet is `128S × 64S`.
- Per pixel: R == G == B == `grey`; alpha 0 means transparent (palette index 0). Otherwise `index = round(grey * max / 255)` where `max = 15` for 4bpp sprites.
- "Scale" = HD PNG width / 128. Must be a positive integer.
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
- **Sheet**: one of the ~52 SMW GFX slots (`gfx00.png` … `gfx33.png`), each 128 tiles.
- **Tile**: 8×8 native pixel block. In an HD sheet, `8S × 8S` pixels.
- **Slot→VRAM mapping**: record of which sheet last uploaded to each VRAM tile address.
- **Scene**: per-frame list of sprite draw calls derived from OAM.
- **Layer**: `0..3`, assigned from OAM priority. Effect passes are per-layer.

---

## Step 1 — HD scale plumbing + stub compositor

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
- Build clean.
- Game window opens at HD scale (e.g. 4×). Backgrounds visible, chunky-upscaled. No sprites visible. Game plays (verified by Mario's inputs moving the camera, BG tiles scrolling).
- Toggle `g_hd_enabled = false` at compile time → SD returns, sprites reappear.

---

## Step 2 — HD PNG loader

**Goal:** load all `gfx/hd/gfxNN.png` at startup into palette-index buffers; keep per-sheet metadata. Missing sheets are OK — recorded as "no HD".

**Read first:**
- [assets/export_sheets.py](assets/export_sheets.py) — **authoritative encoding**.
- Step 1 outputs.

**Tasks:**

1. Add `third_party/stb_image.h` (single-header, public domain). Source: https://github.com/nothings/stb — download the pinned file `stb_image.h` and drop in. Add to `.gitignore` if prudent (or commit).
2. Create `src/hd_gfx.h`:
   ```c
   typedef struct HdSheet {
     uint8 *index_buffer;   // scale*128 * scale*64 bytes; one index per HD pixel; 0 = transparent
     uint16 width;          // scale * 128
     uint16 height;         // scale * 64
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
     - Validate: `width % 128 == 0`, `height % 64 == 0`, `width/128 == height/64`, scale ≥ 1. If validation fails, warn via `fprintf(stderr, ...)` and skip.
     - Allocate `index_buffer = malloc(width * height)`; for each pixel convert `(r, a)` → index:
       - If `a == 0`: `index = 0`.
       - Else: `index = (r * 15 + 127) / 255;` (round to nearest; matches `round(grey * 15 / 255)` from the encoder).
     - Populate `HdSheet` and mark `loaded = true`. Log sheet id, dims, scale.
   - Determine effective global scale: set `g_hd_scale` (from Step 1) to the max scale across loaded sheets, or fall back to 1 if none loaded. (Multiple scales coexist at compositor time; the frame buffer is sized to the maximum.)
   - **Or** require a single uniform scale — simpler for v1. Go with uniform: first loaded sheet's scale sets `g_hd_scale`; reject other-scaled sheets with a warning.
   - `HdGfx_Free`: free every non-null `index_buffer`.
4. Call `HdGfx_LoadAll("gfx/hd")` from `HdCompositor_Init` (or after config loads, if that runs earlier). Call `HdGfx_Free` on shutdown.

**Gotchas:**
- `stb_image` is single-header; exactly one `.c` must `#define STB_IMAGE_IMPLEMENTATION` before the include. Do this in `src/hd_gfx.c`.
- Reject non-integer scale (e.g., a 1023×512 PNG). Log and skip.
- Don't crash on PNGs with indexed color mode — stb_image expands to RGBA automatically.
- HD sheets are large: scale=8 → 1024×512 bytes = 512KB × 52 = ~27MB RAM. Acceptable.
- The tile index layout within a sheet matches source: row-major, 16 tiles per row. Store flat (no per-tile chunking); compositor computes tile offsets on the fly.

**Acceptance:**
- Build clean.
- With some test HD PNGs in `gfx/hd/`, startup logs show loaded sheets with correct scale.
- With `gfx/hd/` empty, startup logs warnings or "no HD sheets", `g_hd_scale` stays 1 (game runs as if HD were off).

---

## Step 3 — Slot→VRAM tracking

**Goal:** at any point during play, given a VRAM word-address, know which source sheet and which tile-within-sheet was last uploaded there.

**Why:** OAM entries reference VRAM tile numbers (`charnum` + OBJ tile base). To pick the right HD tile we need to resolve that back to `(sheet_id, tile_in_sheet)`.

**Read first:**
- [src/smw_rtl.h:18-22](src/smw_rtl.h#L18-L22)
- [src/smw_rtl.c:67+](src/smw_rtl.c#L67) (SmwDrawPpuFrame and surrounding helpers — locate `SmwCopyToVram*` implementations).
- [src/smw_00.c:607-2600](src/smw_00.c) example call sites (GFX uploads, mostly from `g_ram + offset`).
- [src/snes/ppu.c:752-831](src/snes/ppu.c#L752-L831) `ppu_evaluateSprites` — confirms how `objAdr + usedTile * 16` indexes VRAM.

**Strategy:**

GFX uploads to VRAM come from staging RAM (`g_ram + offset`) after decompression. The decompressor writes a known sheet into a known staging location. If we tag staging *buffers* with their sheet ID at decompression time, then `SmwCopyToVram*` can look up the source pointer in the tag map.

**Investigation first (spend ≤30 min):**
- Grep for `decomp_data`, `DecompressGfx`, `DecompGfx`, `decomp_` in `src/smw_*.c` and `assets/compile_resources.*` to find where GFX is decompressed at runtime. Note that decompressed data from the *Python* side (`decomp_data`) is baked into `smw_assets.dat`; the C runtime likely reads pre-decompressed sheets directly.
- If the C runtime loads sheets directly from `smw_assets.dat` via `assets/smw_assets.h`, identify the loader and the mapping from sheet index to RAM offset.

**Tasks:**

1. Create `src/hd_vram_map.h`:
   ```c
   // Represents a single contiguous VRAM region whose content was uploaded from a known sheet.
   typedef struct HdVramRegion {
     uint16 vram_word_addr;   // base VRAM word address
     uint16 tile_count;       // number of 16-word tiles (4bpp); 0 if not tile data
     uint8  sheet_id;         // 0..0x33
     uint16 src_tile_offset;  // first tile within sheet
   } HdVramRegion;

   void HdVramMap_Reset(void);
   void HdVramMap_RecordUpload(uint16 vram_word_addr, const uint8 *src, int byte_count);
   // Resolves a VRAM tile word-address to (sheet, tile_in_sheet). Returns false if unmapped.
   bool HdVramMap_ResolveTile(uint16 vram_word_addr, uint8 *sheet_out, uint16 *tile_out);
   ```
2. Create `src/hd_vram_map.c`:
   - Internally: keep a sorted list of `HdVramRegion`. Small (<256 entries typical).
   - `RecordUpload`: look up `src` in the sheet-staging-buffer registry (see step 3 below). If the source range falls within a tagged sheet buffer, compute `sheet_id` and `src_tile_offset` (byte offset in staging / 32 for 4bpp, /24 for 3bpp — but inflation to 4bpp happens at VRAM, so the *source* bytes per tile and the *dest* bytes per tile may differ; see gotcha).
   - Merge adjacent regions from the same sheet when possible to keep the list short.
   - `ResolveTile`: binary search for the region containing `vram_word_addr`, compute `tile_in_sheet = src_tile_offset + (vram_word_addr - region.vram_word_addr) / 16`.
3. Add a *sheet staging registry*:
   ```c
   // Called at the point the game decompresses sheet `id` into RAM at [base, base+size).
   void HdVramMap_RegisterStagingBuffer(uint8 sheet_id, const uint8 *base, size_t size);
   ```
   Implementation: a small array of `{sheet_id, base, size}` tuples. Look up `src` by pointer-range test.
4. Hook `SmwCopyToVram`, `SmwCopyToVramLow`, `SmwCopyToVramPitch32` (in `src/smw_rtl.c`) to call `HdVramMap_RecordUpload` after performing the copy.
5. Hook the decompression call site to register staging buffers. If the C runtime pre-decompresses all sheets to fixed RAM locations at boot, register them once at boot. If it decompresses on demand, register at each decompression.

**Gotchas:**
- **3bpp→4bpp inflation:** sprite sheets are stored natively 3bpp but uploaded to VRAM as 4bpp. That means a single source tile in the staging buffer is 24 bytes; in VRAM it occupies 32 bytes. `src_tile_offset` must be computed from *source* tile stride. The HD PNG's tile count matches the source, not the VRAM layout — so the mapping math is: `tile_in_sheet = src_tile_offset + (src_byte_offset_within_region / src_tile_bytes)`. Record `src_tile_bytes` in the region struct if needed.
- Some `SmwCopyToVram` calls upload *tilemap data* (e.g. `kStatusBarTilemap_*`), not tile graphics. These don't correspond to a sheet. Detect by: source pointer doesn't fall in any registered staging buffer → skip recording.
- Uploads from `g_ram + 0x2000` with `t == 0` in [smw_00.c:2311-2324](src/smw_00.c#L2311-L2324) are placeholder/empty uploads. Skip or record with a sentinel "no-sheet" marker.
- VRAM is 32K words; OBJ tile base is `(obsel & 7) << 13` (bytes) = `(obsel & 7) << 12` (words). Typical OBJ range: `0x4000`–`0x7FFF` words.
- Don't assume regions are non-overlapping. Later uploads override earlier ones — `RecordUpload` must splice the list correctly (or simply: push a new region; `ResolveTile` returns the *most recent* region containing the address).

**Acceptance:**
- Add a debug dump command (e.g. bind to F10) that prints the current vram→sheet map.
- Load level 1-1: top of VRAM has Mario's sheet(s), fire-flower sheet, etc. Cross-reference with `gfx/source/` to sanity-check.
- `HdVramMap_ResolveTile(objAdr + oamCharnum*16, ...)` for a known sprite returns a plausible (sheet, tile) pair.

---

## Step 4 — Scene build

**Goal:** once per frame, walk OAM and produce a list of `HdSprite` entries describing what to draw.

**Read first:**
- [src/snes/ppu.c:752-831](src/snes/ppu.c#L752-L831) `ppu_evaluateSprites` — copy the OAM-decoding logic.
- [src/snes/ppu.h:167-172](src/snes/ppu.h#L167-L172) — `PPU_objSize`, `PPU_objTileAdr1/2`, `PPU_objPriority`.
- Step 3 outputs.

**Tasks:**

1. Create `src/hd_scene.h`:
   ```c
   typedef struct HdSprite {
     int16  x, y;          // SD pixel coords, top-left. May be negative / offscreen; clip in compositor.
     uint8  size;          // sprite size in SD pixels (8, 16, 32, or 64). Square.
     uint8  sheet;         // source sheet id; 0xFF if unresolved (skip draw)
     uint16 tile;          // first tile within the sheet (top-left tile of the sprite)
     uint8  palette;       // 0..7; CGRAM base = 0x80 + palette*16
     uint8  flags;         // bit0: hflip, bit1: vflip; room for more
     uint8  layer;         // 0..3 (OAM priority)
     uint8  _pad;
   } HdSprite;

   enum { kHdSceneMax = 128 };

   typedef struct HdScene {
     HdSprite sprites[kHdSceneMax];
     uint16   count;
   } HdScene;

   void HdScene_Build(HdScene *scene, const Ppu *ppu);
   ```
2. Create `src/hd_scene.c`:
   - `HdScene_Build`: set `count = 0`. Iterate `for (int i = 0; i < 128; i++)`:
     - Read `oam0 = ppu->oam[i*2]`, `oam1 = ppu->oam[i*2 + 1]`, high-OAM bits via `(ppu->highOam[i >> 2] >> ((i & 3) * 2))`. (Double-check stride: `highOam` packs 2 bits per sprite — bit0 = x-high, bit1 = size-select.)
     - Decode: `x = oam0 & 0xff; x |= (highbits & 1) << 8; if (x > 255) x -= 512;` `y = oam0 >> 8;`
     - `size = spriteSizes[PPU_objSize(ppu)][(highbits >> 1) & 1];` (use `ppu_evaluateSprites`'s `spriteSizes` table — copy it into `hd_scene.c`).
     - `tile_num = oam1 & 0xff; charnum_high = (oam1 >> 8) & 1;`
     - `objAdr = charnum_high ? PPU_objTileAdr2(ppu) : PPU_objTileAdr1(ppu);`
     - VRAM word addr of the sprite's top-left tile: `objAdr_words = objAdr >> 1; tile_vram = objAdr_words + tile_num * 16;` (4bpp tile = 16 words; note `objAdr` from `PPU_objTileAdr*` may already be in bytes; confirm and normalize).
     - Resolve via `HdVramMap_ResolveTile(tile_vram, &sheet, &tile_in_sheet)`. If unresolved, set `sheet = 0xFF`.
     - `palette = (oam1 >> 9) & 7;`
     - `flags = ((oam1 >> 14) & 1) | ((oam1 >> 14) & 2);` — oam1 bit 14 = hflip, bit 15 = vflip. Pack into `flags` bits 0,1.
     - `layer = (oam1 >> 12) & 3;`
   - Skip entries fully offscreen (all pixels clipped). Cheap bbox test.
   - Skip hidden sprites (y == 0xF0 is the traditional "hidden" y in SMW; check).
3. In `HdCompositor_Draw`: allocate a static `HdScene g_hd_scene;` and call `HdScene_Build(&g_hd_scene, ppu)` before compositing sprites. (Step 5 uses it.)

**Gotchas:**
- OAM packing is tricky. Read `ppu->oam` as uint16 (2 entries per 32-bit logical sprite is wrong — `oam[i*2]` is word 0, `oam[i*2 + 1]` is word 1; 128 sprites × 2 words = 256 uint16 total, matches `ppu->oam[0x100]`).
- highOam: 32 bytes total = 256 bits = 2 bits/sprite × 128. Layout: sprite i's bits live at `highOam[i >> 2]`, positions `(i & 3) * 2` (low) and `+1` (high). Confirm by comparing with `ppu_evaluateSprites`.
- Sprite y=0xF0 in SMW hides the sprite (off-bottom). Skip if outside the visible range.
- `spriteSizes[PPU_objSize(ppu)]` returns `{small, large}` sides. If non-square (rare), clarify; v1 can assume square.
- Sprites larger than 8×8 reference a grid of tiles inside the sheet. Per ppu.c:807, tile arrangement within a sprite uses a "16 tiles wide" wrap: `usedTile = (((tile_num >> 4) + (row >> 3)) << 4) | (((tile_num & 0xf) + (col >> 3)) & 0xf)`. The compositor (Step 5) replicates this walk; scene only stores the top-left `tile_num`.

**Acceptance:**
- Add a debug print (e.g. F9) dumping `g_hd_scene` for one frame.
- Run Yoshi's Island 1: scene should contain Mario (layer 2 typically), bushes/clouds (none — those are BG), maybe powerup items. Counts match the visibly drawn sprites.

---

## Step 5 — CPU compositor (baseline, no effects)

**Goal:** HD sprites actually appear on screen. Palette applied via current CGRAM. Sprites render on top of the upscaled BG.

**Read first:**
- Step 1, 2, 4 outputs.
- [src/snes/ppu.c:693-710](src/snes/ppu.c#L693-L710) final composite pass — reference for CGRAM + brightness math.
- [src/snes/ppu.c:797-826](src/snes/ppu.c#L797-L826) — reference for per-tile sub-tile iteration inside a large sprite.

**Tasks:**

1. Extend `src/hd_compositor.c`:
   - After the BG upscale pass, walk `g_hd_scene` sorted by `layer` low→high. Within the same layer, iterate in OAM order *reversed* (matches PPU convention — later OAM entries draw over earlier at equal priority? Actually PPU: lower-index OAM has higher priority with OAM priority rotation disabled. Validate against `ppu_evaluateSprites` behavior; it iterates i=0..127 and writes pixels only if the target is empty, so *earlier* OAM wins. Mirror that by iterating earlier-first and skipping already-written pixels, *or* iterate in reverse for an alpha overwrite model. For v1, alpha-overwrite in reverse order is simpler and near-identical for non-overlapping sprites.)
2. For each `HdSprite s` with `s.sheet != 0xFF`:
   - `HdSheet *sheet = &g_hd_sheets[s.sheet]; if (!sheet->loaded) continue;`
   - `int S = g_hd_scale;` (must equal `sheet->scale` — assert in debug).
   - `int palette_base = 0x80 + s.palette * 16;`
   - For each 8×8 sub-tile within the sprite (walk col=0..s.size step 8, row=0..s.size step 8):
     - Compute `usedTile` via the formula from ppu.c:807 (honoring hflip/vflip).
     - Tile's top-left in sheet pixels: `(tx, ty) = ((usedTile & 0xf) * 8*S, (usedTile >> 4) * 8*S)`. Note `usedTile` may wrap; mask to `0..127`.
     - Destination top-left in HD buffer: `(dx, dy) = ((s.x + col) * S, (s.y + row) * S)`.
     - For each HD pixel `(px, py)` in `[0, 8S)²`:
       - Apply hflip/vflip to source coords: `sx = hflip ? (8*S - 1 - px) : px; sy = vflip ? (8*S - 1 - py) : py;`
       - `index = sheet->index_buffer[(ty + sy) * sheet->width + (tx + sx)];`
       - If `index == 0`: skip (transparent).
       - Clip destination: `out_x = dx + px; out_y = dy + py; if out of HD bounds, skip.`
       - `color = ppu->cgram[palette_base + index];` — 15-bit BGR5.
       - Apply brightness: `r = ppu->brightnessMult[(color >> 0) & 0x1f]; g = ...[(color >> 5) & 0x1f]; b = ...[(color >> 10) & 0x1f];`
       - Write BGRA: `dst_pixel = (b << 0) | (g << 8) | (r << 16) | (0xff << 24);`
3. Performance: hoist invariants (tile pointer, palette pointer) outside inner loops. Don't worry about SIMD yet.

**Gotchas:**
- Flip within a large sprite is tricky: flipping also *reorders* the sub-tiles (see ppu.c:806 `usedCol`). Mirror the same formula.
- `usedTile` math needs mask-to-128: the PPU uses `oam1 & 0xff` + offsets with `& 0xf` on the low nibble, which wraps columns within the 16-tile row but not rows; verify by re-reading ppu.c:807 and matching exactly.
- Brightness: `ppu->brightnessMult` is populated per-scanline in ppu.c's render path; it's valid after `draw_ppu_frame()` runs. Compose *after* the PPU scan (which is what happens in `RtlDrawPpuFrame`).
- CGRAM colors are 15-bit: `bbbbb gggggrrrr r` with R low. Match the existing `PpuDrawWholeLine` mapping exactly to preserve color fidelity.
- Don't swap R/B — SDL's expected format is BGRA little-endian; `opengl.c` uploads with `GL_BGRA` + `GL_UNSIGNED_INT_8_8_8_8_REV`. Matches what ppu.c writes. Follow that convention.

**Acceptance:**
- Game runs with HD sprites visible: Mario is chunky-BG + crisp-HD-sprite (if you've painted a Mario HD sheet). Palette changes (invincibility flash, fade-in) apply to HD sprites correctly.
- No crash when loading a level whose sprites have no HD replacement: `sheet == 0xFF` → sprite simply doesn't render. (Acceptable failure mode for v1; Step 7 can fall back to upscaled SD for unmapped sprites if desired.)

---

## Step 6 — Per-layer effects (drop shadow)

**Goal:** render each layer into a temp HD buffer, run an effect pass, alpha-blit into the main HD buffer. Implement drop shadow as the first effect.

**Read first:** Step 5 output.

**Tasks:**

1. Define layer config in `src/hd_compositor.c`:
   ```c
   typedef struct HdLayerCfg {
     bool     shadow_enabled;
     int8     shadow_dx, shadow_dy;   // offset in HD pixels
     uint8    shadow_alpha;           // 0..255
     uint32   shadow_bgra;            // fixed shadow color, default 0xff000000 (black, full alpha as source)
   } HdLayerCfg;

   static HdLayerCfg g_hd_layer_cfg[4] = {
     { false }, { false }, { true, 4, 4, 128, 0xff000000 }, { false },
   };
   ```
2. Change Step 5's compositor to per-layer:
   - Allocate one persistent `uint8 *g_hd_layer_buffer` of size `HD_W * HD_H * 4`.
   - For layer in 0..3:
     - `memset(g_hd_layer_buffer, 0, HD_W * HD_H * 4);`
     - Composite all `HdSprite`s with `s.layer == layer` into `g_hd_layer_buffer` (same blit as Step 5).
     - If `g_hd_layer_cfg[layer].shadow_enabled`: **shadow pre-pass** — run *before* the main blit. Two-buffer scheme cleanest: allocate a shadow buffer, draw silhouettes there, alpha-blit it into main HD buffer, *then* alpha-blit the sprite buffer.
     - Alpha-blit `g_hd_layer_buffer` onto the main HD destination.
3. Silhouette draw for the shadow pass: same blit logic as Step 5 but output pixel is `shadow_bgra` with `shadow_alpha` scaling A. Offset dest by `(shadow_dx, shadow_dy)`.
4. Alpha-blit helper: `dst.rgb = src.a*src.rgb + (1 - src.a)*dst.rgb; dst.a = 0xff;`

**Gotchas:**
- Drop shadow for *all sprites in the layer combined* looks different from per-sprite shadows (sprites inside the same layer won't cast shadows on each other). That's the user's stated intent: one shadow per layer group.
- Shadow extending past the sprite silhouette is correct; don't try to clip to sprite bbox.
- Memory: 4 layer buffers × 1024×896×4 = 14 MB peak. Reuse one temp buffer — loop over layers sequentially.
- Ordering: shadow pass should render *before* the sprites of that layer, so sprites in the same layer appear *above* their own shadow.

**Acceptance:**
- With `g_hd_layer_cfg[2].shadow_enabled = true`, Mario has a crisp offset shadow on the ground. Enemies on the same priority also cast shadows (expected). Power-ups too, if they share layer 2.
- Shadow fades correctly near transparent sprite pixels (alpha respect).

---

## Step 7 — Config + toggle

**Goal:** user-controllable enable/disable, hotkey toggle, effects configurable from `smw.ini`.

**Read first:**
- [src/config.h](src/config.h), [src/config.c](src/config.c) — existing INI parser + defaults.
- [src/main.c:~400-600](src/main.c) input handling — `HandleCommand` for hotkey.

**Tasks:**

1. Add to `Config` struct (in `config.h`) and parser (`config.c`):
   - `bool hd_gfx_enabled;` default `true` if HD sheets exist, else `false`.
   - `char hd_gfx_dir[256];` default `"gfx/hd"`.
   - Per-layer shadow config:
     - `bool hd_layer_shadow[4];` default `{false, false, true, false}`.
     - `int hd_shadow_dx, hd_shadow_dy;` default `4, 4`.
     - `int hd_shadow_alpha;` default `128`.
2. At startup, after `ConfigLoad`, set `g_hd_enabled = config.hd_gfx_enabled`; pass `config.hd_gfx_dir` to `HdGfx_LoadAll`; apply shadow config to `g_hd_layer_cfg`.
3. Hotkey: add a command (e.g. `kKey_ToggleHdGfx`) in the command enum, register a default binding (e.g. Ctrl+H), handle in `HandleCommand` by flipping `g_hd_enabled`. (If enabling dynamically changes scale, `OpenGLRenderer_BeginDraw` will resize the buffer on next frame — works automatically.)
4. Runtime guard: if `g_hd_scale > 1` but no HD sheets loaded, log a warning and clamp to 1.

**Gotchas:**
- Toggling HD on/off mid-game resizes the GL texture — validate it doesn't flicker badly. If it does, debounce or require a one-frame blank.
- Config INI keys: match existing naming convention in the project (snake_case vs camelCase — read config.c to confirm).

**Acceptance:**
- Fresh checkout, no HD PNGs: game plays at SD, no warnings.
- HD PNGs present: game plays at HD, Ctrl+H toggles back to SD and back.
- Changing `smw.ini` values for shadow offset/alpha takes effect on next launch.

---

## Known limitations (document in README or code comment after v1)

- HD sprites composite on top of all BG tiles regardless of BG priority bits — a sprite that should render *behind* a high-prio BG tile (e.g., bit 3 of `$2105` set with BG3 prio 1) will appear in front. Rare in SMW gameplay.
- Mode 7 scenes fall back to upscaled SD (no HD path). Bowser battle, map mode 7 rotations.
- Color math (add/subtract subscreen), direct-color mode, color window math — not reproduced in the HD path. Most SMW gameplay doesn't use these for sprites; some effects (halo around Yoshi coins?) may look plainer.
- Verification mismatch snapshots compare RAM, not pixels — HD output does not affect verification.
- One uniform scale across all sheets in v1. Mixed-scale support is future work.

---

## Build/test workflow per session

- `make -j$(nproc)` after each step. Fix warnings.
- `./smw` to run.
- Keep a level save available for quick iteration (e.g., Yoshi's Island 1 with a power-up visible).
- If you add a debug print (F9/F10 dumps), leave them behind `#ifdef HD_DEBUG` so they don't clutter runtime output.

## Out-of-scope (explicitly for later)

- GPU compositor (textured quads, fragment-shader effects, palette LUT uniforms).
- BG tile replacement.
- Mode 7 HD.
- Per-sheet variable scale.
- Lunar Magic hack GFX.
- Mix of SD fallback for unmapped sprites (v1 just skips them).
- Sub-pixel smooth scrolling for HD BG.
