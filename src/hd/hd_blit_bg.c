#include "hd_blit_bg.h"
#include "hd_compositor.h"
#include "hd_gfx.h"
#include "hd_vram_map.h"
#include <stdint.h>

void HdDrawMode7Placeholder(uint8 *dst, size_t pitch, int hd_w, int hd_h) {
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

void HdBlitBgLayer(uint8 *dst, size_t pitch,
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
      const HdSheet *sheet    = (sheet_id == 0xFE) ? &g_hd_placeholder : &g_hd_sheets[sheet_id];
      bool           use_bgra = (sheet_id == 0xFE);
      if (!sheet->loaded || sheet->scale != (uint8)S) continue;
      if (tile_in_sheet >= sheet->tile_count) continue;

      int xi_sd = hflip ? (7 - (int)(x_scrolled & 7)) : (int)(x_scrolled & 7);
      int yi_sd = vflip ? (7 - y_in_tile)              : y_in_tile;
      int tx_px = (tile_in_sheet & 0xf) * 8 * S + xi_sd * S;
      int ty_px = (tile_in_sheet >> 4)  * 8 * S + yi_sd * S;
      int sh_w  = (int)sheet->width;
      int pal_base             = palette * 16;  // BG palette: no 0x80 offset
      const uint8    *idx      = use_bgra ? NULL : sheet->index_buffer;
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
