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

The PNG always has 128 tiles matching *source* layout. Since Path A writes one full sheet (128 tiles) contiguously, `tile_in_sheet = (vram_word_addr - region_base) / 16`. For Path B, both source and destination are 4bpp (32 B/tile), so the same formula works (just computed on the RAM staging side).

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
- **VRAM geometry.** VRAM is 32K words; OBJ tile base is `(obsel & 7) << 12` (words). Typical OBJ range: `0x4000`–`0x7FFF` words. The map stores word-addresses, so no unit confusion.
- **Regions overlap on purpose.** A per-frame Mario upload writes into 0x6000-0x60FF *inside* the 0x6000-0x67FF region previously recorded by `UploadGraphicsFiles_UploadGFXFile` for sheet 0x32. Scan-from-end ensures the newer Mario region wins, which is what we want.
- **Scan order.** `ResolveTile` must iterate newest→oldest so the most recent upload for a given address is returned.

**Acceptance:**
- Add a debug dump command (e.g. bind to F10) that prints the current vram→sheet map: one line per region `(sheet_id, vram_base, tile_count, src_tile_offset)`.
- Load level 1-1 (Yoshi's Island 1): dump should show 4 sprite-slot regions at 0x6000/0x6800/0x7000/0x7800 with sheet ids from `kUploadGraphicsFiles_SpriteGFXList` (level 0 = sprite graphics setting 0 → entries 0..3 of the list). Cross-reference with `gfx/source/gfx*.png` to sanity-check.
- After Mario moves, dump should also show small per-frame regions inside 0x6000-0x67FF tagged as sheet 0x32.
- `HdVramMap_ResolveTile(objAdr + oamCharnum*16, ...)` for a visible Mario sprite returns `(sheet=0x32, tile ≈ charnum & 0x7f)`. For a visible enemy sprite it returns one of the four sprite-slot sheets with `tile = charnum - (vram_slot_base / 16)`.

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
