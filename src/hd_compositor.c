#include "hd_compositor.h"
#include "hd_gfx.h"
#include "hd_scene.h"
#include "hd_vram_map.h"
#include "common_rtl.h"
#include "snes/ppu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static void HdCompositor_AlphaBlit(uint8 *dst, size_t dst_pitch,
                                   const uint8 *src, size_t src_pitch,
                                   int w, int h);
static void HdCompositor_BlitSprite(uint8 *dst_buf, size_t dst_pitch,
                                    int dst_w, int dst_h,
                                    const HdSprite *s,
                                    HdBlitMode mode,
                                    int16 offset_x, int16 offset_y,
                                    uint8 sil_r, uint8 sil_g, uint8 sil_b, uint8 sil_a,
                                    const Ppu *ppu);

bool  g_hd_enabled      = true;
uint8 g_hd_scale        = 1;
bool  g_hd_skip_sprites = false;

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

void HdCompositor_Init(void) {
  HdVramMap_Reset();
  HdGfx_LoadAll("gfx/hd");
}

void HdCompositor_Draw(uint8 *dst, size_t pitch,
                       const uint8 *sd_pixels,
                       int sd_width, int sd_height) {
  int S = g_hd_scale;
  int src_pitch = sd_width * 4;
  for (int y = 0; y < sd_height * S; y++) {
    const uint8 *src_row = sd_pixels + (y / S) * src_pitch;
    uint32_t *dst_row = (uint32_t *)((uint8 *)dst + y * pitch);
    const uint32_t *src32 = (const uint32_t *)src_row;
    for (int x = 0; x < sd_width * S; x++)
      dst_row[x] = src32[x / S];
  }

  // Build the HD sprite scene from current PPU OAM state.
  HdScene_Build(&g_hd_scene, g_my_ppu);

  // Periodic always-on diagnostic dump (~once per second at 60fps).
  HdCompositor_DebugDump(&g_hd_scene);

  // Per-layer composite: shadow pre-pass then color pass, alpha-blitted into dst.
  if (g_hd_scale > 1) {
    int hd_width  = sd_width  * S;
    int hd_height = sd_height * S;
    HdCompositor_EnsureBuffers(hd_width, hd_height);
    size_t work_pitch = (size_t)hd_width * 4;

    for (int layer = 0; layer < 4; layer++) {
      const HdLayerCfg *cfg = &g_hd_layer_cfg[layer];

      // --- shadow pre-pass (if enabled) -----------------------------------
      if (cfg->shadow_enabled) {
        memset(g_hd_shadow_buf, 0, work_pitch * (size_t)hd_height);
        for (int k = (int)g_hd_scene.count - 1; k >= 0; k--) {
          const HdSprite *s = &g_hd_scene.sprites[k];
          if (s->layer != (uint8)layer) continue;
          HdCompositor_BlitSprite(g_hd_shadow_buf, work_pitch, hd_width, hd_height,
                                  s, kHdBlitSilhouette,
                                  cfg->shadow_dx, cfg->shadow_dy,
                                  cfg->shadow_r, cfg->shadow_g, cfg->shadow_b,
                                  cfg->shadow_alpha,
                                  g_my_ppu);
        }
        HdCompositor_AlphaBlit(dst, pitch, g_hd_shadow_buf, work_pitch,
                               hd_width, hd_height);
      }

      // --- sprite color pass ----------------------------------------------
      memset(g_hd_layer_buf, 0, work_pitch * (size_t)hd_height);
      for (int k = (int)g_hd_scene.count - 1; k >= 0; k--) {
        const HdSprite *s = &g_hd_scene.sprites[k];
        if (s->layer != (uint8)layer) continue;
        HdCompositor_BlitSprite(g_hd_layer_buf, work_pitch, hd_width, hd_height,
                                s, kHdBlitColor, 0, 0, 0, 0, 0, 0xff,
                                g_my_ppu);
      }
      HdCompositor_AlphaBlit(dst, pitch, g_hd_layer_buf, work_pitch,
                             hd_width, hd_height);
    }
  }
}

const HdScene *HdCompositor_GetScene(void) {
  return &g_hd_scene;
}

// ---------------------------------------------------------------------------
// Alpha-blit helper.  Composites src (BGRA) onto dst using src alpha.
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
