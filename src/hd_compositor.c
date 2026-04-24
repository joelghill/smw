#include "hd_compositor.h"
#include "hd_gfx.h"
#include "hd_scene.h"
#include "hd_vram_map.h"
#include "common_rtl.h"
#include "snes/ppu.h"
#include <stdint.h>
#include <stdio.h>

static HdScene g_hd_scene;

// Forward declaration — defined after HdCompositor_Draw.
static void HdCompositor_DrawSprites(uint8 *hd_buf, size_t hd_pitch,
                                     int hd_width, int hd_height,
                                     const Ppu *ppu);
static void HdCompositor_DebugDump(const HdScene *scene);

bool  g_hd_enabled      = true;
uint8 g_hd_scale        = 1;
bool  g_hd_skip_sprites = false;

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

  // Composite HD sprites on top of the upscaled BG.
  if (g_hd_scale > 1) {
    int hd_width  = sd_width  * S;
    int hd_height = sd_height * S;
    HdCompositor_DrawSprites(dst, pitch, hd_width, hd_height, g_my_ppu);
  }
}

const HdScene *HdCompositor_GetScene(void) {
  return &g_hd_scene;
}

// ---------------------------------------------------------------------------
// Sprite compositor — Step 5
// ---------------------------------------------------------------------------

// Composite all HD sprites from g_hd_scene into hd_buf.
// Iterates layers 0..3 (low→high priority), and within each layer iterates
// sprites from highest OAM index down to lowest so that lower-index (higher
// OAM priority) sprites overwrite — matching the PPU's first-come-first-served
// rule.
static void HdCompositor_DrawSprites(uint8 *hd_buf, size_t hd_pitch,
                                     int hd_width, int hd_height,
                                     const Ppu *ppu) {
  int S = (int)g_hd_scale;
  const uint16 *cgram = ppu->cgram;
  const uint8  *bmult = ppu->brightnessMult;

  for (int layer = 0; layer < 4; layer++) {
    for (int k = (int)g_hd_scene.count - 1; k >= 0; k--) {
      const HdSprite *s = &g_hd_scene.sprites[k];
      if (s->layer != (uint8)layer) continue;

      bool hflip     = (s->flags & 1) != 0;
      bool vflip     = (s->flags & 2) != 0;
      int  num_tiles = s->size >> 3;  // tiles per side: 1, 2, 4, or 8
      int  pal_base  = 0x80 + s->palette * 16;

      // Recover objAdr from tile_vram and tile_num.
      // objAdr is always 0x2000-aligned (PPU_objTileAdr1/2 are multiples of 0x2000).
      uint16 objAdr  = (uint16)((s->tile_vram - (uint16)s->tile_num * 16) & 0x7fff);
      uint8  tile_hi = s->tile_num >> 4;   // high nibble of tile_num
      uint8  tile_lo = s->tile_num & 0xf;  // low  nibble of tile_num

      // Walk output tile grid (matches PPU usedTile formula).
      for (int t_row = 0; t_row < num_tiles; t_row++) {
        // Source tile row: vflip reverses tile order AND pixel order within tile.
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

          // HD pixel origin of this source tile in the sheet.
          int tx       = (tile_in_sheet & 0xf) * 8 * S;
          int ty       = (tile_in_sheet >> 4)  * 8 * S;
          int sh_width = (int)sheet->width;
          const uint8 *idx_buf = sheet->index_buffer;

          // Walk the 8×8 SD pixels within the sub-tile.
          for (int yi = 0; yi < 8; yi++) {
            // vflip reverses pixels within the tile vertically.
            int src_yi     = vflip ? (7 - yi) : yi;
            int out_y_base = (s->y + t_row * 8 + yi) * S;
            if (out_y_base + S <= 0 || out_y_base >= hd_height) continue;

            for (int xi = 0; xi < 8; xi++) {
              int src_xi     = hflip ? (7 - xi) : xi;
              int out_x_base = (s->x + t_col * 8 + xi) * S;
              if (out_x_base + S <= 0 || out_x_base >= hd_width) continue;

              // Top-left HD pixel for this source (src_xi, src_yi) within tile.
              int hd_sx = tx + src_xi * S;
              int hd_sy = ty + src_yi * S;

              // Expand one SD pixel into an S×S HD block.
              for (int py = 0; py < S; py++) {
                int dst_y = out_y_base + py;
                if (dst_y < 0 || dst_y >= hd_height) continue;
                uint32_t *dst_row =
                    (uint32_t *)((uint8 *)hd_buf + (size_t)dst_y * hd_pitch);
                for (int px = 0; px < S; px++) {
                  int dst_x = out_x_base + px;
                  if (dst_x < 0 || dst_x >= hd_width) continue;
                  uint8 index = idx_buf[(hd_sy + py) * sh_width + hd_sx + px];
                  if (index == 0) continue;  // transparent
                  uint16 color = cgram[pal_base + index];
                  // CGRAM format: ..bbbbb ggggg rrrrr (R in low 5 bits).
                  // Output: BGRA little-endian (B=byte0, G=byte1, R=byte2, A=byte3).
                  uint8 r = bmult[(color >>  0) & 0x1f];
                  uint8 g = bmult[(color >>  5) & 0x1f];
                  uint8 b = bmult[(color >> 10) & 0x1f];
                  dst_row[dst_x] = (uint32_t)b | ((uint32_t)g << 8) |
                                   ((uint32_t)r << 16) | 0xff000000u;
                }
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
