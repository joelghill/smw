#include "hd_compositor.h"
#include "hd_gfx.h"
#include "hd_scene.h"
#include "hd_vram_map.h"
#include "common_rtl.h"
#include "config.h"
#include "snes/ppu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern bool g_new_ppu;  // defined in main.c; true = new PPU with bgBuffers populated

static HdScene g_hd_scene;

// ---------------------------------------------------------------------------
// Blit mode: color (Step 5 behaviour) or silhouette (shadow pre-pass).
// ---------------------------------------------------------------------------
typedef enum {
  kHdBlitColor      = 0,  // CGRAM-indexed color + brightnessMult
  kHdBlitSilhouette = 1,  // fixed RGB/alpha, only where HD index != 0
} HdBlitMode;

// Forward declarations — defined after HdCompositor_Draw.
static void HdCompositor_DebugDump(const HdScene *scene);
// gate_layer: if >= 0, only blit pixels where HdDecodeZbuf(prio_map) == gate_layer.
// Pass -1 to blit unconditionally (no gate).
static void HdCompositor_AlphaBlit(uint8 *dst, size_t dst_pitch,
                                   const uint8 *src, size_t src_pitch,
                                   int w, int h, int gate_layer);
static void HdCompositor_BlitSprite(uint8 *dst_buf, size_t dst_pitch,
                                    int dst_w, int dst_h,
                                    const HdSprite *s,
                                    HdBlitMode mode,
                                    int16 offset_x, int16 offset_y,
                                    uint8 sil_r, uint8 sil_g, uint8 sil_b, uint8 sil_a,
                                    const Ppu *ppu);
static void HdCompositor_CompositeSpriteLayer(uint8 *dst, size_t pitch,
                                              int hd_w, int hd_h,
                                              int oam_prio, const Ppu *ppu);
static void HdCompositor_BlitBgLayer(uint8 *dst, size_t pitch,
                                     int hd_w, int hd_h,
                                     int bg_layer, bool prio_hi,
                                     HdBgLayerID expected_layer,
                                     const Ppu *ppu);

bool  g_hd_enabled      = true;
uint8 g_hd_scale        = 1;
bool  g_hd_skip_sprites = false;

// v2: per-pixel priority z-buffer.
uint16 *g_hd_prio_map  = NULL;
bool    g_hd_bg_enabled[3] = { true, true, false };   // BG3 stays off until Step 10

// Map OAM priority 0..3 to the corresponding HdBgLayerID for the sprite priority gate.
static const HdBgLayerID kHdSpritePrioToLayer[4] = {
  kHdBgLayer_Spr0, kHdBgLayer_Spr1, kHdBgLayer_Spr2, kHdBgLayer_Spr3,
};

// ---------------------------------------------------------------------------
// Per-layer effect configuration (file-static; Step 7 hooks INI to mutate).
// ---------------------------------------------------------------------------
typedef struct HdLayerCfg {
  bool    shadow_enabled;
  int16   shadow_dx, shadow_dy;  // offset in HD pixels
  uint8   shadow_alpha;          // 0..255
  uint8   shadow_r, shadow_g, shadow_b;  // shadow color
} HdLayerCfg;

static HdLayerCfg g_hd_layer_cfg[4] = {
  [0] = { false },
  [1] = { false },
  [2] = { .shadow_enabled = true, .shadow_dx = 4, .shadow_dy = 4,
          .shadow_alpha = 128, .shadow_r = 0, .shadow_g = 0, .shadow_b = 0 },
  [3] = { false },
};

// ---------------------------------------------------------------------------
// Lazy work buffers — allocated/reallocated on first Draw or dim-change.
// ---------------------------------------------------------------------------
static uint8 *g_hd_layer_buf  = NULL;  // one sprite layer at a time
static uint8 *g_hd_shadow_buf = NULL;  // one shadow silhouette at a time
static int    g_hd_buf_width  = 0;
static int    g_hd_buf_height = 0;

static void HdCompositor_EnsureBuffers(int hd_w, int hd_h) {
  if (hd_w == g_hd_buf_width && hd_h == g_hd_buf_height) return;
  free(g_hd_layer_buf);
  free(g_hd_shadow_buf);
  size_t sz = (size_t)hd_w * (size_t)hd_h * 4;
  g_hd_layer_buf  = (uint8 *)malloc(sz);
  g_hd_shadow_buf = (uint8 *)malloc(sz);
  g_hd_buf_width  = hd_w;
  g_hd_buf_height = hd_h;
}

// ---------------------------------------------------------------------------
// Priority z-buffer decoder (v2 Step 8).
// Uses cascaded >= thresholds; handles the 0x_6 level6 sprite variant.
// ---------------------------------------------------------------------------
HdBgLayerID HdDecodeZbuf(uint16 z) {
  uint8 hi = (uint8)(z >> 8);
  if (hi >= 0xF2) return kHdBgLayer_BG3hi_prio;
  if (hi >= 0xE4) return kHdBgLayer_Spr3;        // 0xE4 or 0xE6 (level6)
  if (hi >= 0xC0) return kHdBgLayer_BG1hi;
  if (hi >= 0xB1) return kHdBgLayer_BG2hi;
  if (hi >= 0xA4) return kHdBgLayer_Spr2;        // 0xA4 or 0xA6
  if (hi >= 0x80) return kHdBgLayer_BG1lo;
  if (hi >= 0x71) return kHdBgLayer_BG2lo;
  if (hi >= 0x64) return kHdBgLayer_Spr1;        // 0x64 or 0x66
  if (hi >= 0x32) return kHdBgLayer_BG3hi_noprio;
  if (hi >= 0x24) return kHdBgLayer_Spr0;        // 0x24 or 0x26
  if (hi >= 0x12) return kHdBgLayer_BG3lo;
  return kHdBgLayer_Backdrop;                     // 0x00 (pre-clear) or 0x05 (ClearBackdrop)
}

void HdCompositor_ApplyConfig(void) {
  g_hd_enabled = g_config.hd_gfx_enabled;
  for (int i = 0; i < 4; i++) {
    g_hd_layer_cfg[i].shadow_enabled = g_config.hd_layer_shadow[i];
    g_hd_layer_cfg[i].shadow_dx    = g_config.hd_shadow_dx;
    g_hd_layer_cfg[i].shadow_dy    = g_config.hd_shadow_dy;
    g_hd_layer_cfg[i].shadow_alpha = g_config.hd_shadow_alpha;
    g_hd_layer_cfg[i].shadow_r     = g_config.hd_shadow_r;
    g_hd_layer_cfg[i].shadow_g     = g_config.hd_shadow_g;
    g_hd_layer_cfg[i].shadow_b     = g_config.hd_shadow_b;
  }
}

void HdCompositor_Shutdown(void) {
  free(g_hd_layer_buf);
  free(g_hd_shadow_buf);
  g_hd_layer_buf  = NULL;
  g_hd_shadow_buf = NULL;
  g_hd_buf_width  = 0;
  g_hd_buf_height = 0;
  free(g_hd_prio_map);
  g_hd_prio_map = NULL;
}

bool HdCompositor_Toggle(void) {
  g_hd_enabled = !g_hd_enabled;
  return g_hd_enabled;
}

void HdCompositor_Init(void) {
  HdCompositor_ApplyConfig();
  HdVramMap_Reset();
  const char *hd_dir = g_config.hd_gfx_dir ? g_config.hd_gfx_dir : "gfx/hd";
  HdGfx_LoadAll(hd_dir);
  if (g_hd_enabled && g_hd_scale <= 1) {
    fprintf(stderr, "HD enabled but no HD sheets loaded from '%s' "
                    "(scale=1); falling back to SD.\n", hd_dir);
  }
  // v2 Step 8: allocate per-scanline priority z-buffer.
  if (!g_hd_prio_map)
    g_hd_prio_map = (uint16 *)malloc(256 * 240 * sizeof(uint16));
}

void HdCompositor_Draw(uint8 *dst, size_t pitch,
                       const uint8 *sd_pixels,
                       int sd_width, int sd_height) {
  int S = g_hd_scale;
  int hd_width  = sd_width  * S;
  int hd_height = sd_height * S;

  // 1. Nearest-upscale SD pixels into HD buffer (always — SD is the fallback).
  int src_pitch = sd_width * 4;
  for (int y = 0; y < hd_height; y++) {
    const uint8 *src_row = sd_pixels + (y / S) * src_pitch;
    uint32_t *dst_row = (uint32_t *)((uint8 *)dst + y * pitch);
    const uint32_t *src32 = (const uint32_t *)src_row;
    for (int x = 0; x < hd_width; x++)
      dst_row[x] = src32[x / S];
  }

  // 2. Build the HD sprite scene from current PPU OAM state.
  HdScene_Build(&g_hd_scene, g_my_ppu);

  // Periodic diagnostic dump (~once per second at 60fps).
  HdCompositor_DebugDump(&g_hd_scene);

  // 3. HD composite passes.
  if (g_hd_scale > 1) {
    HdCompositor_EnsureBuffers(hd_width, hd_height);

    if (PPU_mode(g_my_ppu) == 1) {
      // 11-pass priority-ordered composite (v2 Step 9a scaffolding).
      // BG passes are no-op stubs until Step 9b/10; sprite passes are fully functional.
      bool bg3prio = PPU_bg3priority(g_my_ppu) != 0;

      HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 2, false, kHdBgLayer_BG3lo,        g_my_ppu);
      HdCompositor_CompositeSpriteLayer(dst, pitch, hd_width, hd_height, 0, g_my_ppu);
      if (!bg3prio)
        HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 2, true,  kHdBgLayer_BG3hi_noprio, g_my_ppu);
      HdCompositor_CompositeSpriteLayer(dst, pitch, hd_width, hd_height, 1, g_my_ppu);
      HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 1, false, kHdBgLayer_BG2lo,        g_my_ppu);
      HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 0, false, kHdBgLayer_BG1lo,        g_my_ppu);
      HdCompositor_CompositeSpriteLayer(dst, pitch, hd_width, hd_height, 2, g_my_ppu);
      HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 1, true,  kHdBgLayer_BG2hi,        g_my_ppu);
      HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 0, true,  kHdBgLayer_BG1hi,        g_my_ppu);
      HdCompositor_CompositeSpriteLayer(dst, pitch, hd_width, hd_height, 3, g_my_ppu);
      if (bg3prio)
        HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 2, true,  kHdBgLayer_BG3hi_prio,  g_my_ppu);
    } else {
      // Mode 7 or unknown mode: v1 sprite-on-top fallback (sprites already in SD baseline).
      for (int p = 0; p < 4; p++)
        HdCompositor_CompositeSpriteLayer(dst, pitch, hd_width, hd_height, p, g_my_ppu);
    }
  }
}

const HdScene *HdCompositor_GetScene(void) {
  return &g_hd_scene;
}

// ---------------------------------------------------------------------------
// Step 9a: per-OAM-priority sprite layer composite (shadow + color passes).
// Extracted from the v1 per-layer loop so the 11-pass loop can call it.
// ---------------------------------------------------------------------------
static void HdCompositor_CompositeSpriteLayer(uint8 *dst, size_t pitch,
                                              int hd_w, int hd_h,
                                              int oam_prio, const Ppu *ppu) {
  const HdLayerCfg *cfg = &g_hd_layer_cfg[oam_prio];
  size_t work_pitch = (size_t)hd_w * 4;

  // --- shadow pre-pass (if enabled) ---
  if (cfg->shadow_enabled) {
    memset(g_hd_shadow_buf, 0, work_pitch * (size_t)hd_h);
    for (int k = (int)g_hd_scene.count - 1; k >= 0; k--) {
      const HdSprite *s = &g_hd_scene.sprites[k];
      if (s->layer != (uint8)oam_prio) continue;
      HdCompositor_BlitSprite(g_hd_shadow_buf, work_pitch, hd_w, hd_h,
                              s, kHdBlitSilhouette,
                              cfg->shadow_dx, cfg->shadow_dy,
                              cfg->shadow_r, cfg->shadow_g, cfg->shadow_b,
                              cfg->shadow_alpha,
                              ppu);
    }
    HdCompositor_AlphaBlit(dst, pitch, g_hd_shadow_buf, work_pitch, hd_w, hd_h,
                            (int)kHdSpritePrioToLayer[oam_prio]);
  }

  // --- sprite color pass ---
  memset(g_hd_layer_buf, 0, work_pitch * (size_t)hd_h);
  for (int k = (int)g_hd_scene.count - 1; k >= 0; k--) {
    const HdSprite *s = &g_hd_scene.sprites[k];
    if (s->layer != (uint8)oam_prio) continue;
    HdCompositor_BlitSprite(g_hd_layer_buf, work_pitch, hd_w, hd_h,
                            s, kHdBlitColor, 0, 0, 0, 0, 0, 0xff, ppu);
  }
  HdCompositor_AlphaBlit(dst, pitch, g_hd_layer_buf, work_pitch, hd_w, hd_h,
                         (int)kHdSpritePrioToLayer[oam_prio]);
}

// ---------------------------------------------------------------------------
// Step 9b: 4bpp BG1/BG2 tile compositor.
// Iterates over every SD pixel, checks the priority gate, resolves the HD
// tile from the VRAM map, and expands each SD pixel into S×S HD pixels.
// BG3 (2bpp) is added in Step 10.
// ---------------------------------------------------------------------------
static void HdCompositor_BlitBgLayer(uint8 *dst, size_t pitch,
                                     int hd_w, int hd_h,
                                     int bg_layer, bool prio_hi,
                                     HdBgLayerID expected_layer,
                                     const Ppu *ppu) {
  if (!g_hd_bg_enabled[bg_layer]) return;
  if (PPU_mode(ppu) != 1) return;
  if (!g_new_ppu) return;                         // old PPU: bgBuffers not populated
  if (!g_hd_prio_map) return;
  // Mosaic: fall back to SD upscale to avoid out-of-sync HD rendering.
  if (PPU_mosaicSize(ppu) > 1 && PPU_mosaicEnabled(ppu, bg_layer)) return;

  int S    = (int)g_hd_scale;
  int sd_h = hd_h / S;
  int sd_w = hd_w / S;
  int tileadr = PPU_bgTileAdr(ppu, bg_layer);

  for (int sy = 0; sy < sd_h; sy++) {
    // Per-row scrolled tilemap pointers — mirror PpuDrawBackground_4bpp lines.
    uint y_scrolled = (uint)(sy + ppu->vScroll[bg_layer]);
    int sc_offs = (int)PPU_bgTilemapAdr(ppu, bg_layer)
                  + (int)(((y_scrolled >> 3) & 0x1f) << 5);
    if ((y_scrolled & 0x100) && PPU_bgTilemapHigher(ppu, bg_layer))
      sc_offs += PPU_bgTilemapWider(ppu, bg_layer) ? 0x800 : 0x400;
    const uint16 *tps[2] = {
      &ppu->vram[sc_offs & 0x7fff],
      &ppu->vram[(sc_offs + (PPU_bgTilemapWider(ppu, bg_layer) ? 0x400 : 0)) & 0x7fff],
    };
    int y_in_tile = (int)(y_scrolled & 7);

    uint x_scrolled = (uint)ppu->hScroll[bg_layer];
    for (int sx = 0; sx < sd_w; sx++, x_scrolled++) {
      // Priority gate: only paint where this layer won at this SD pixel.
      if (HdDecodeZbuf(g_hd_prio_map[sy * 256 + sx]) != expected_layer) continue;

      const uint16 *tp = tps[(x_scrolled >> 8) & 1];
      uint16 te = tp[(x_scrolled >> 3) & 0x1f];

      // Skip if the tile's prio bit doesn't match what we're rendering.
      if (((te >> 13) & 1) != (uint)(prio_hi ? 1 : 0)) continue;

      int  tile_num = (int)(te & 0x3ff);
      int  palette  = (int)((te >> 10) & 7);
      bool hflip    = (te >> 14) & 1;
      bool vflip    = (te >> 15) & 1;

      uint16 tile_vram = (uint16)((tileadr + tile_num * 16) & 0x7fff);
      uint8  sheet_id;
      uint16 tile_in_sheet;
      if (!HdVramMap_ResolveTile(tile_vram, &sheet_id, &tile_in_sheet)) continue;
      const HdSheet *sheet = &g_hd_sheets[sheet_id];
      if (!sheet->loaded || sheet->scale != (uint8)S) continue;
      if (tile_in_sheet >= sheet->tile_count) continue;

      // Within-tile SD pixel position, with flip applied.
      int xi_sd = hflip ? (7 - (int)(x_scrolled & 7)) : (int)(x_scrolled & 7);
      int yi_sd = vflip ? (7 - y_in_tile)             : y_in_tile;

      // Top-left of this SD pixel's S×S block in the HD sheet.
      int tx_px = (tile_in_sheet & 0xf) * 8 * S + xi_sd * S;
      int ty_px = (tile_in_sheet >> 4)  * 8 * S + yi_sd * S;
      int sh_w  = (int)sheet->width;
      const uint8 *idx = sheet->index_buffer;
      const uint16 *cgram = ppu->cgram;
      const uint8  *bmult = ppu->brightnessMult;
      int pal_base = palette * 16;  // BG CGRAM: no 0x80 offset (sprites only)

      int out_x = sx * S, out_y = sy * S;
      for (int py = 0; py < S; py++) {
        int src_py = vflip ? (S - 1 - py) : py;
        uint32_t *dst_row = (uint32_t *)(dst + (size_t)(out_y + py) * pitch);
        for (int px = 0; px < S; px++) {
          int src_px = hflip ? (S - 1 - px) : px;
          uint8 index = idx[(ty_px + src_py) * sh_w + tx_px + src_px];
          if (index == 0) continue;  // transparent: leave SD baseline
          uint16 color = cgram[pal_base + index];
          uint8 r = bmult[(color >>  0) & 0x1f];
          uint8 g = bmult[(color >>  5) & 0x1f];
          uint8 b = bmult[(color >> 10) & 0x1f];
          dst_row[out_x + px] = (uint32_t)b | ((uint32_t)g << 8)
                               | ((uint32_t)r << 16) | 0xff000000u;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Alpha-blit helper.  Composites src (BGRA) onto dst using src alpha.
// gate_layer >= 0: sprite gate — only blit where decoded prio layer <= gate_layer.
//   Sprites are skipped from the PPU pixel render when HD is active, so only
//   BG z-values appear in g_hd_prio_map.  "<= gate_layer" means: draw the HD
//   sprite pixel unless a BG layer with strictly higher priority already won it.
// gate_layer == -1: no gate (unconditional blit).
// ---------------------------------------------------------------------------
static void HdCompositor_AlphaBlit(uint8 *dst, size_t dst_pitch,
                                   const uint8 *src, size_t src_pitch,
                                   int w, int h, int gate_layer) {
  bool gated = (gate_layer >= 0) && (g_hd_prio_map != NULL);
  int  S     = (int)g_hd_scale;
  for (int y = 0; y < h; y++) {
    uint8       *dst_row = dst + y * dst_pitch;
    const uint8 *src_row = src + y * src_pitch;
    int sd_y = (gated) ? (y / S) : 0;
    for (int x = 0; x < w; x++) {
      if (gated) {
        int sd_x = x / S;
        if ((int)HdDecodeZbuf(g_hd_prio_map[sd_y * 256 + sd_x]) > gate_layer) continue;
      }
      uint8 a = src_row[x * 4 + 3];
      if (a == 0) continue;
      uint8 inv = (uint8)(255 - a);
      dst_row[x * 4 + 0] = (uint8)((src_row[x * 4 + 0] * a + dst_row[x * 4 + 0] * inv + 127) / 255);  // B
      dst_row[x * 4 + 1] = (uint8)((src_row[x * 4 + 1] * a + dst_row[x * 4 + 1] * inv + 127) / 255);  // G
      dst_row[x * 4 + 2] = (uint8)((src_row[x * 4 + 2] * a + dst_row[x * 4 + 2] * inv + 127) / 255);  // R
      dst_row[x * 4 + 3] = 0xff;
    }
  }
}

// ---------------------------------------------------------------------------
// Sprite compositor — Step 5 / Step 6
// ---------------------------------------------------------------------------

// Blit one HdSprite into dst_buf, either in full color (kHdBlitColor) or as a
// fixed-color silhouette (kHdBlitSilhouette) for the shadow pre-pass.
// offset_x / offset_y are in HD pixel units and are added to the final dst coords.
static void HdCompositor_BlitSprite(uint8 *dst_buf, size_t dst_pitch,
                                    int dst_w, int dst_h,
                                    const HdSprite *s,
                                    HdBlitMode mode,
                                    int16 offset_x, int16 offset_y,
                                    uint8 sil_r, uint8 sil_g, uint8 sil_b, uint8 sil_a,
                                    const Ppu *ppu) {
  int S = (int)g_hd_scale;
  const uint16 *cgram = ppu->cgram;
  const uint8  *bmult = ppu->brightnessMult;

  bool hflip     = (s->flags & 1) != 0;
  bool vflip     = (s->flags & 2) != 0;
  int  num_tiles = s->size >> 3;  // tiles per side: 1, 2, 4, or 8
  int  pal_base  = 0x80 + s->palette * 16;

  uint16 objAdr  = (uint16)((s->tile_vram - (uint16)s->tile_num * 16) & 0x7fff);
  uint8  tile_hi = s->tile_num >> 4;
  uint8  tile_lo = s->tile_num & 0xf;

  for (int t_row = 0; t_row < num_tiles; t_row++) {
    int   src_t_row = vflip ? (num_tiles - 1 - t_row) : t_row;
    uint8 u_hi      = (uint8)((tile_hi + src_t_row) & 0xff);

    for (int t_col = 0; t_col < num_tiles; t_col++) {
      int   src_t_col  = hflip ? (num_tiles - 1 - t_col) : t_col;
      uint8 u_lo       = (uint8)((tile_lo + src_t_col) & 0xf);
      uint16 used_tile = ((uint16)u_hi << 4) | u_lo;
      uint16 tv        = (uint16)((objAdr + used_tile * 16) & 0x7fff);

      uint8  sheet_id;
      uint16 tile_in_sheet;
      if (!HdVramMap_ResolveTile(tv, &sheet_id, &tile_in_sheet))
        continue;
      const HdSheet *sheet = &g_hd_sheets[sheet_id];
      if (!sheet->loaded || sheet->scale != (uint8)S ||
          tile_in_sheet >= sheet->tile_count)
        continue;

      int tx       = (tile_in_sheet & 0xf) * 8 * S;
      int ty       = (tile_in_sheet >> 4)  * 8 * S;
      int sh_width = (int)sheet->width;
      const uint8 *idx_buf = sheet->index_buffer;

      for (int yi = 0; yi < 8; yi++) {
        int src_yi     = vflip ? (7 - yi) : yi;
        int out_y_base = (s->y + t_row * 8 + yi) * S + (int)offset_y;
        if (out_y_base + S <= 0 || out_y_base >= dst_h) continue;

        for (int xi = 0; xi < 8; xi++) {
          int src_xi     = hflip ? (7 - xi) : xi;
          int out_x_base = (s->x + t_col * 8 + xi) * S + (int)offset_x;
          if (out_x_base + S <= 0 || out_x_base >= dst_w) continue;

          int hd_sx = tx + src_xi * S;
          int hd_sy = ty + src_yi * S;

          for (int py = 0; py < S; py++) {
            int dst_y  = out_y_base + py;
            int src_py = vflip ? (S - 1 - py) : py;
            if (dst_y < 0 || dst_y >= dst_h) continue;
            uint32_t *dst_row =
                (uint32_t *)(dst_buf + (size_t)dst_y * dst_pitch);
            for (int px = 0; px < S; px++) {
              int dst_x  = out_x_base + px;
              int src_px = hflip ? (S - 1 - px) : px;
              if (dst_x < 0 || dst_x >= dst_w) continue;
              uint8 index = idx_buf[(hd_sy + src_py) * sh_width + hd_sx + src_px];
              if (index == 0) continue;  // transparent

              if (mode == kHdBlitColor) {
                uint16 color = cgram[pal_base + index];
                uint8 r = bmult[(color >>  0) & 0x1f];
                uint8 g = bmult[(color >>  5) & 0x1f];
                uint8 b = bmult[(color >> 10) & 0x1f];
                dst_row[dst_x] = (uint32_t)b | ((uint32_t)g << 8) |
                                 ((uint32_t)r << 16) | 0xff000000u;
              } else {
                dst_row[dst_x] = (uint32_t)sil_b | ((uint32_t)sil_g << 8) |
                                 ((uint32_t)sil_r << 16) | ((uint32_t)sil_a << 24);
              }
            }
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Always-on diagnostic dump.  Runs ~once per second.  Prints the VRAM map
// and, for each visible HD scene sprite, how many of its sub-tiles resolved
// against the map and what (sheet_id, tile_in_sheet) the first resolved tile
// landed on.  No build-type gating — fprintf to stderr.
// ---------------------------------------------------------------------------
static void HdCompositor_DebugDump(const HdScene *scene) {
  static unsigned frame_counter = 0;
  if (++frame_counter % 60 != 0) return;  // ~1 Hz at 60fps

  fprintf(stderr, "\n----- HD Debug @ frame %u  (scene count=%u, hd_scale=%u) -----\n",
          frame_counter, scene->count, g_hd_scale);
  HdVramMap_Dump();

  // v2 Step 8: sample the priority map at screen centre as a quick sanity check.
  if (g_hd_prio_map) {
    uint16 z = g_hd_prio_map[112 * 256 + 128];
    fprintf(stderr, "  prio_map[112,128] = 0x%04x -> layer %d\n", z, (int)HdDecodeZbuf(z));
  }

  fprintf(stderr, "----- Scene -----\n");
  for (int i = 0; i < scene->count; i++) {
    const HdSprite *s = &scene->sprites[i];
    int    num_tiles  = s->size >> 3;
    uint16 objAdr     = (uint16)((s->tile_vram - (uint16)s->tile_num * 16) & 0x7fff);
    uint8  tile_hi    = s->tile_num >> 4;
    uint8  tile_lo    = s->tile_num & 0xf;

    int    resolved   = 0;
    int    unresolved = 0;
    bool   sample_set = false;
    uint8  s_sheet    = 0;
    uint16 s_tile     = 0;
    uint16 s_vram     = 0;
    uint16 first_unresolved_vram = 0;
    bool   any_unresolved = false;

    for (int t_row = 0; t_row < num_tiles; t_row++) {
      uint8 u_hi = (uint8)((tile_hi + t_row) & 0xff);
      for (int t_col = 0; t_col < num_tiles; t_col++) {
        uint8  u_lo      = (uint8)((tile_lo + t_col) & 0xf);
        uint16 used_tile = ((uint16)u_hi << 4) | u_lo;
        uint16 tv        = (uint16)((objAdr + used_tile * 16) & 0x7fff);

        uint8  sheet_id;
        uint16 tile_in_sheet;
        if (HdVramMap_ResolveTile(tv, &sheet_id, &tile_in_sheet)) {
          resolved++;
          if (!sample_set) {
            sample_set = true;
            s_sheet    = sheet_id;
            s_tile     = tile_in_sheet;
            s_vram     = tv;
          }
        } else {
          unresolved++;
          if (!any_unresolved) {
            any_unresolved        = true;
            first_unresolved_vram = tv;
          }
        }
      }
    }

    fprintf(stderr,
            "  oam[%3d] xy=(%4d,%4d) sz=%2d pal=%u lay=%u flags=0x%x  "
            "tile_num=0x%02x objAdr=0x%04x tile_vram=0x%04x  R=%d U=%d",
            i, s->x, s->y, s->size, s->palette, s->layer, s->flags,
            s->tile_num, objAdr, s->tile_vram, resolved, unresolved);

    if (sample_set) {
      const HdSheet *sh = &g_hd_sheets[s_sheet];
      fprintf(stderr, "  first_R: vram=0x%04x -> gfx%02x tile=%u (loaded=%d, scale=%u, tiles=%u)",
              s_vram, s_sheet, s_tile, sh->loaded, sh->scale, sh->tile_count);
    }
    if (any_unresolved) {
      fprintf(stderr, "  first_U: vram=0x%04x", first_unresolved_vram);
    }
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "----- End HD Debug -----\n");
  fflush(stderr);
}

// ---------------------------------------------------------------------------
// Priority map diagnostic accessor (Step 8).
// ---------------------------------------------------------------------------
HdBgLayerID HdGetPrioMapSample(int sd_x, int sd_y) {
  if (!g_hd_prio_map) return kHdBgLayer_Backdrop;
  if ((unsigned)sd_x >= 256 || (unsigned)sd_y >= 240) return kHdBgLayer_Backdrop;
  return HdDecodeZbuf(g_hd_prio_map[sd_y * 256 + sd_x]);
}
