#include "hd_compositor.h"
#include "hd_frame.h"
#include "hd_gfx.h"
#include "hd_vram_map.h"
#include "config.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Blit mode: full color or fixed-color silhouette for the shadow pre-pass.
// ---------------------------------------------------------------------------
typedef enum {
  kHdBlitColor      = 0,
  kHdBlitSilhouette = 1,
} HdBlitMode;

// Forward declarations
static void HdCompositor_DrawMode7Placeholder(uint8 *dst, size_t pitch, int hd_w, int hd_h);
static void HdCompositor_AlphaBlit(uint8 *dst, size_t dst_pitch,
                                   const uint8 *src, size_t src_pitch,
                                   int w, int h);
static void HdCompositor_BlitSprite(uint8 *dst_buf, size_t dst_pitch,
                                    int dst_w, int dst_h,
                                    const HdSprite *s,
                                    HdBlitMode mode,
                                    int16 offset_x, int16 offset_y,
                                    uint8 sil_r, uint8 sil_g, uint8 sil_b, uint8 sil_a,
                                    const HdFrameSnapshot *f);
static void HdCompositor_CompositeSpriteLayer(uint8 *dst, size_t pitch,
                                              int hd_w, int hd_h,
                                              int oam_prio,
                                              const HdFrameSnapshot *f);
static void HdCompositor_BlitBgLayer(uint8 *dst, size_t pitch,
                                     int hd_w, int hd_h,
                                     int bg_layer, bool prio_hi,
                                     const HdFrameSnapshot *f);

bool  g_hd_enabled      = true;
uint8 g_hd_scale        = 1;
bool  g_hd_skip_sprites = false;

bool g_hd_bg_enabled[3] = { true, true, false };  // BG3 stays off until Step 10

// ---------------------------------------------------------------------------
// Per-layer shadow effect configuration.
// ---------------------------------------------------------------------------
typedef struct HdLayerCfg {
  bool  shadow_enabled;
  int16 shadow_dx, shadow_dy;  // offset in HD pixels
  uint8 shadow_alpha;          // 0..255
  uint8 shadow_r, shadow_g, shadow_b;
} HdLayerCfg;

static HdLayerCfg g_hd_layer_cfg[4] = {
  [0] = { false },
  [1] = { false },
  [2] = { .shadow_enabled = true, .shadow_dx = 4, .shadow_dy = 4,
          .shadow_alpha = 128, .shadow_r = 0, .shadow_g = 0, .shadow_b = 0 },
  [3] = { false },
};

// ---------------------------------------------------------------------------
// Lazy work buffers — allocated/reallocated when the HD frame size changes.
// ---------------------------------------------------------------------------
static uint8 *g_hd_layer_buf  = NULL;
static uint8 *g_hd_shadow_buf = NULL;
static int    g_hd_buf_width  = 0;
static int    g_hd_buf_height = 0;

static void HdCompositor_EnsureBuffers(int hd_w, int hd_h) {
  if (hd_w == g_hd_buf_width && hd_h == g_hd_buf_height) return;
  free(g_hd_layer_buf);
  free(g_hd_shadow_buf);
  size_t sz      = (size_t)hd_w * (size_t)hd_h * 4;
  g_hd_layer_buf  = (uint8 *)malloc(sz);
  g_hd_shadow_buf = (uint8 *)malloc(sz);
  g_hd_buf_width  = hd_w;
  g_hd_buf_height = hd_h;
}

void HdCompositor_ApplyConfig(void) {
  g_hd_enabled = g_config.hd_gfx_enabled;
  for (int i = 0; i < 4; i++) {
    g_hd_layer_cfg[i].shadow_enabled = g_config.hd_layer_shadow[i];
    g_hd_layer_cfg[i].shadow_dx      = g_config.hd_shadow_dx;
    g_hd_layer_cfg[i].shadow_dy      = g_config.hd_shadow_dy;
    g_hd_layer_cfg[i].shadow_alpha   = g_config.hd_shadow_alpha;
    g_hd_layer_cfg[i].shadow_r       = g_config.hd_shadow_r;
    g_hd_layer_cfg[i].shadow_g       = g_config.hd_shadow_g;
    g_hd_layer_cfg[i].shadow_b       = g_config.hd_shadow_b;
  }
}

void HdCompositor_Shutdown(void) {
  free(g_hd_layer_buf);
  free(g_hd_shadow_buf);
  g_hd_layer_buf  = NULL;
  g_hd_shadow_buf = NULL;
  g_hd_buf_width  = 0;
  g_hd_buf_height = 0;
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
}

// ---------------------------------------------------------------------------
// Mode 7 placeholder — magenta fill with 0xB7 sentinel bits.
// Replaced by the real Mode 7 path in Step 11.
// ---------------------------------------------------------------------------
static void HdCompositor_DrawMode7Placeholder(uint8 *dst, size_t pitch,
                                              int hd_w, int hd_h) {
  uint32_t magenta = 0x00FF00FFu | 0xFF000000u;
  for (int y = 0; y < hd_h; y++) {
    uint32_t *row = (uint32_t *)(dst + (size_t)y * pitch);
    for (int x = 0; x < hd_w; x++) row[x] = magenta;
  }
  int S = (int)g_hd_scale;
  uint8 marker[2] = { 0xB7, 0xB7 };
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

// ---------------------------------------------------------------------------
// Main entry point — called once per frame from RtlDrawPpuFrame.
// ---------------------------------------------------------------------------
void HdCompositor_Draw(uint8 *dst, size_t pitch, const HdRenderInput *input) {
  const HdFrameSnapshot *f = &input->frame;
  int S         = g_hd_scale;
  int hd_width  = 256 * S;
  int hd_height = 224 * S;

  // 1. Backdrop fill from pre-decoded CGRAM[0].
  for (int y = 0; y < hd_height; y++) {
    uint32_t *row = (uint32_t *)((uint8 *)dst + (size_t)y * pitch);
    for (int x = 0; x < hd_width; x++) row[x] = f->backdrop_bgra;
  }

  if (g_hd_scale <= 1) return;

  HdCompositor_EnsureBuffers(hd_width, hd_height);

  // Mode 7 short-circuit — real implementation deferred to Step 11.
  if (f->mode == 7) {
    HdCompositor_DrawMode7Placeholder(dst, pitch, hd_width, hd_height);
    return;
  }

  // 2. 11-pass priority-ordered composite (Mode 1).
  HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 2, false, f);
  HdCompositor_CompositeSpriteLayer(dst, pitch, hd_width, hd_height, 0, f);
  if (!f->bg3prio)
    HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 2, true, f);
  HdCompositor_CompositeSpriteLayer(dst, pitch, hd_width, hd_height, 1, f);
  HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 1, false, f);
  HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 0, false, f);
  HdCompositor_CompositeSpriteLayer(dst, pitch, hd_width, hd_height, 2, f);
  HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 1, true,  f);
  HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 0, true,  f);
  HdCompositor_CompositeSpriteLayer(dst, pitch, hd_width, hd_height, 3, f);
  if (f->bg3prio)
    HdCompositor_BlitBgLayer(dst, pitch, hd_width, hd_height, 2, true, f);
}

// ---------------------------------------------------------------------------
// Per-OAM-priority sprite layer: optional shadow pre-pass + color pass.
// ---------------------------------------------------------------------------
static void HdCompositor_CompositeSpriteLayer(uint8 *dst, size_t pitch,
                                              int hd_w, int hd_h,
                                              int oam_prio,
                                              const HdFrameSnapshot *f) {
  const HdLayerCfg *cfg      = &g_hd_layer_cfg[oam_prio];
  size_t            work_pitch = (size_t)hd_w * 4;

  if (cfg->shadow_enabled) {
    memset(g_hd_shadow_buf, 0, work_pitch * (size_t)hd_h);
    for (int k = (int)f->sprites.count - 1; k >= 0; k--) {
      const HdSprite *s = &f->sprites.sprites[k];
      if (s->layer != (uint8)oam_prio) continue;
      HdCompositor_BlitSprite(g_hd_shadow_buf, work_pitch, hd_w, hd_h,
                              s, kHdBlitSilhouette,
                              cfg->shadow_dx, cfg->shadow_dy,
                              cfg->shadow_r, cfg->shadow_g, cfg->shadow_b,
                              cfg->shadow_alpha, f);
    }
    HdCompositor_AlphaBlit(dst, pitch, g_hd_shadow_buf, work_pitch, hd_w, hd_h);
  }

  memset(g_hd_layer_buf, 0, work_pitch * (size_t)hd_h);
  for (int k = (int)f->sprites.count - 1; k >= 0; k--) {
    const HdSprite *s = &f->sprites.sprites[k];
    if (s->layer != (uint8)oam_prio) continue;
    HdCompositor_BlitSprite(g_hd_layer_buf, work_pitch, hd_w, hd_h,
                            s, kHdBlitColor, 0, 0, 0, 0, 0, 0xff, f);
  }
  HdCompositor_AlphaBlit(dst, pitch, g_hd_layer_buf, work_pitch, hd_w, hd_h);
}

// ---------------------------------------------------------------------------
// 4bpp BG tile compositor (BG1/BG2; BG3 added in Step 10).
// Iterates over SD pixels, gates on prio bit, resolves HD tile, expands S×S.
// ---------------------------------------------------------------------------
static void HdCompositor_BlitBgLayer(uint8 *dst, size_t pitch,
                                     int hd_w, int hd_h,
                                     int bg_layer, bool prio_hi,
                                     const HdFrameSnapshot *f) {
  if (!g_hd_bg_enabled[bg_layer]) return;
  if (f->mode != 1) return;
  if (f->mosaic_size > 1 && f->mosaic_enabled[bg_layer]) return;

  int S       = (int)g_hd_scale;
  int sd_h    = hd_h / S;
  int sd_w    = hd_w / S;
  int tileadr = f->tile_adr[bg_layer];

  for (int sy = 0; sy < sd_h; sy++) {
    uint y_scrolled = (uint)(sy + f->vscroll[bg_layer]);
    int sc_offs = f->tilemap_adr[bg_layer]
                + (int)(((y_scrolled >> 3) & 0x1f) << 5);
    if ((y_scrolled & 0x100) && f->tilemap_higher[bg_layer])
      sc_offs += f->tilemap_wider[bg_layer] ? 0x800 : 0x400;
    const uint16 *tps[2] = {
      &f->vram[sc_offs & 0x7fff],
      &f->vram[(sc_offs + (f->tilemap_wider[bg_layer] ? 0x400 : 0)) & 0x7fff],
    };
    int y_in_tile = (int)(y_scrolled & 7);

    uint x_scrolled = (uint)f->hscroll[bg_layer];
    for (int sx = 0; sx < sd_w; sx++, x_scrolled++) {
      const uint16 *tp = tps[(x_scrolled >> 8) & 1];
      uint16 te = tp[(x_scrolled >> 3) & 0x1f];

      if (((te >> 13) & 1) != (uint)(prio_hi ? 1 : 0)) continue;

      int  tile_num = (int)(te & 0x3ff);
      int  palette  = (int)((te >> 10) & 7);
      bool hflip    = (te >> 14) & 1;
      bool vflip    = (te >> 15) & 1;

      uint16 tile_vram = (uint16)((tileadr + tile_num * 16) & 0x7fff);
      uint8  sheet_id;
      uint16 tile_in_sheet;
      HdVramMap_ResolveTile(tile_vram, &sheet_id, &tile_in_sheet);
      const HdSheet *sheet       = (sheet_id == 0xFE) ? &g_hd_placeholder : &g_hd_sheets[sheet_id];
      bool           use_bgra    = (sheet_id == 0xFE);
      if (!sheet->loaded || sheet->scale != (uint8)S) continue;
      if (tile_in_sheet >= sheet->tile_count) continue;

      int xi_sd = hflip ? (7 - (int)(x_scrolled & 7)) : (int)(x_scrolled & 7);
      int yi_sd = vflip ? (7 - y_in_tile)              : y_in_tile;
      int tx_px = (tile_in_sheet & 0xf) * 8 * S + xi_sd * S;
      int ty_px = (tile_in_sheet >> 4)  * 8 * S + yi_sd * S;
      int sh_w  = (int)sheet->width;
      int pal_base            = palette * 16;  // BG palette: no 0x80 offset
      const uint8    *idx     = use_bgra ? NULL : sheet->index_buffer;
      const uint32_t *bgra_buf = use_bgra ? (const uint32_t *)sheet->bgra_buffer : NULL;

      int out_x = sx * S, out_y = sy * S;
      for (int py = 0; py < S; py++) {
        int src_py    = vflip ? (S - 1 - py) : py;
        uint32_t *dst_row = (uint32_t *)(dst + (size_t)(out_y + py) * pitch);
        for (int px = 0; px < S; px++) {
          int src_px = hflip ? (S - 1 - px) : px;
          if (use_bgra) {
            uint32_t pix = bgra_buf[(ty_px + src_py) * sh_w + tx_px + src_px];
            if ((pix >> 24) == 0) continue;
            dst_row[out_x + px] = pix;
          } else {
            uint8 index = idx[(ty_px + src_py) * sh_w + tx_px + src_px];
            if (index == 0) continue;
            dst_row[out_x + px] = f->palette_bgra[pal_base + index];
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Alpha-blit: composite src BGRA onto dst using src alpha.
// ---------------------------------------------------------------------------
static void HdCompositor_AlphaBlit(uint8 *dst, size_t dst_pitch,
                                   const uint8 *src, size_t src_pitch,
                                   int w, int h) {
  for (int y = 0; y < h; y++) {
    uint8       *dst_row = dst + y * dst_pitch;
    const uint8 *src_row = src + y * src_pitch;
    for (int x = 0; x < w; x++) {
      uint8 a = src_row[x * 4 + 3];
      if (a == 0) continue;
      uint8 inv        = (uint8)(255 - a);
      dst_row[x*4 + 0] = (uint8)((src_row[x*4+0] * a + dst_row[x*4+0] * inv + 127) / 255);
      dst_row[x*4 + 1] = (uint8)((src_row[x*4+1] * a + dst_row[x*4+1] * inv + 127) / 255);
      dst_row[x*4 + 2] = (uint8)((src_row[x*4+2] * a + dst_row[x*4+2] * inv + 127) / 255);
      dst_row[x*4 + 3] = 0xff;
    }
  }
}

// ---------------------------------------------------------------------------
// Single-sprite blit: color pass or silhouette for shadow.
// offset_x / offset_y are in HD pixels (used for shadow displacement).
// ---------------------------------------------------------------------------
static void HdCompositor_BlitSprite(uint8 *dst_buf, size_t dst_pitch,
                                    int dst_w, int dst_h,
                                    const HdSprite *s,
                                    HdBlitMode mode,
                                    int16 offset_x, int16 offset_y,
                                    uint8 sil_r, uint8 sil_g, uint8 sil_b, uint8 sil_a,
                                    const HdFrameSnapshot *f) {
  int S          = (int)g_hd_scale;
  bool hflip     = (s->flags & 1) != 0;
  bool vflip     = (s->flags & 2) != 0;
  int  num_tiles = s->size >> 3;
  int  pal_base  = 0x80 + s->palette * 16;  // sprite palette: 0x80 offset

  uint16 objAdr  = (uint16)((s->tile_vram - (uint16)s->tile_num * 16) & 0x7fff);
  uint8  tile_hi = s->tile_num >> 4;
  uint8  tile_lo = s->tile_num & 0xf;

  for (int t_row = 0; t_row < num_tiles; t_row++) {
    int   src_t_row = vflip ? (num_tiles - 1 - t_row) : t_row;
    uint8 u_hi      = (uint8)((tile_hi + src_t_row) & 0xff);

    for (int t_col = 0; t_col < num_tiles; t_col++) {
      int    src_t_col = hflip ? (num_tiles - 1 - t_col) : t_col;
      uint8  u_lo      = (uint8)((tile_lo + src_t_col) & 0xf);
      uint16 used_tile = ((uint16)u_hi << 4) | u_lo;
      uint16 tv        = (uint16)((objAdr + used_tile * 16) & 0x7fff);

      uint8  sheet_id;
      uint16 tile_in_sheet;
      HdVramMap_ResolveTile(tv, &sheet_id, &tile_in_sheet);
      const HdSheet *sheet    = (sheet_id == 0xFE) ? &g_hd_placeholder : &g_hd_sheets[sheet_id];
      bool           use_bgra = (sheet_id == 0xFE);
      if (!sheet->loaded || sheet->scale != (uint8)S ||
          tile_in_sheet >= sheet->tile_count)
        continue;

      int tx       = (tile_in_sheet & 0xf) * 8 * S;
      int ty       = (tile_in_sheet >> 4)  * 8 * S;
      int sh_width = (int)sheet->width;
      const uint8    *idx_buf  = use_bgra ? NULL : sheet->index_buffer;
      const uint32_t *bgra_buf = use_bgra ? (const uint32_t *)sheet->bgra_buffer : NULL;

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
            uint32_t *dst_row = (uint32_t *)(dst_buf + (size_t)dst_y * dst_pitch);

            for (int px = 0; px < S; px++) {
              int dst_x  = out_x_base + px;
              int src_px = hflip ? (S - 1 - px) : px;
              if (dst_x < 0 || dst_x >= dst_w) continue;

              if (use_bgra) {
                uint32_t pix = bgra_buf[(hd_sy + src_py) * sh_width + hd_sx + src_px];
                if ((pix >> 24) == 0) continue;
                dst_row[dst_x] = (mode == kHdBlitColor)
                    ? pix
                    : ((uint32_t)sil_b | ((uint32_t)sil_g << 8)
                       | ((uint32_t)sil_r << 16) | ((uint32_t)sil_a << 24));
              } else {
                uint8 index = idx_buf[(hd_sy + src_py) * sh_width + hd_sx + src_px];
                if (index == 0) continue;
                dst_row[dst_x] = (mode == kHdBlitColor)
                    ? f->palette_bgra[pal_base + index]
                    : ((uint32_t)sil_b | ((uint32_t)sil_g << 8)
                       | ((uint32_t)sil_r << 16) | ((uint32_t)sil_a << 24));
              }
            }
          }
        }
      }
    }
  }
}
