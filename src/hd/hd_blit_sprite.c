#include "hd_blit_sprite.h"
#include "hd_compositor.h"
#include "hd_gfx.h"
#include "hd_vram_map.h"

void HdBlitSprite(uint8 *dst_buf, size_t dst_pitch,
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
