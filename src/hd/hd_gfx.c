#include "third_party/stb/stb_image.h"

#include "hd_gfx.h"
#include "hd_compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HdSheet g_hd_sheets[kHdSheetCount];
HdSheet g_hd_placeholder;

void HdGfx_LoadAll(const char *hd_dir) {
  int effective_scale = 0;

  for (int i = 0; i < kHdSheetCount; i++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/gfx%02x.png", hd_dir, i);

    int w, h, channels;
    unsigned char *rgba = stbi_load(path, &w, &h, &channels, 4);
    if (!rgba) {
      g_hd_sheets[i].loaded = false;
      continue;
    }

    // Validate: width must be a positive multiple of 128 (16 tiles × scale),
    // height must be a positive multiple of (8 * scale) (any number of tile rows).
    if (w % 128 != 0 || w / 128 < 1 || h % (8 * (w / 128)) != 0 || h < (8 * (w / 128))) {
      fprintf(stderr, "HD: gfx%02x.png invalid dimensions %dx%d, skipping\n", i, w, h);
      stbi_image_free(rgba);
      g_hd_sheets[i].loaded = false;
      continue;
    }

    int scale = w / 128;

    // Uniform scale enforcement
    if (effective_scale == 0) {
      effective_scale = scale;
    } else if (scale != effective_scale) {
      fprintf(stderr, "HD: gfx%02x.png scale %d differs from established scale %d, skipping\n",
              i, scale, effective_scale);
      stbi_image_free(rgba);
      g_hd_sheets[i].loaded = false;
      continue;
    }

    uint8 *index_buffer = (uint8 *)malloc((size_t)w * h);
    if (!index_buffer) {
      fprintf(stderr, "HD: out of memory loading gfx%02x.png\n", i);
      stbi_image_free(rgba);
      g_hd_sheets[i].loaded = false;
      continue;
    }

    // Convert RGBA pixels to palette indices
    // Encoding: grey level encodes index; alpha==0 means transparent (index 0)
    // index = round(grey * 15 / 255) = (r * 15 + 127) / 255
    const unsigned char *src = rgba;
    uint8 *dst = index_buffer;
    int num_pixels = w * h;
    for (int p = 0; p < num_pixels; p++, src += 4, dst++) {
      uint8 a = src[3];
      uint8 r = src[0];
      *dst = (a == 0) ? 0 : (uint8)((r * 15 + 127) / 255);
    }

    stbi_image_free(rgba);

    g_hd_sheets[i].index_buffer = index_buffer;
    g_hd_sheets[i].width        = (uint16)w;
    g_hd_sheets[i].height       = (uint16)h;
    g_hd_sheets[i].tile_count   = (uint16)((w / (8 * scale)) * (h / (8 * scale)));
    g_hd_sheets[i].scale        = (uint8)scale;
    g_hd_sheets[i].loaded       = true;

    fprintf(stderr, "HD: loaded gfx%02x %dx%d scale=%d\n", i, w, h, scale);
  }

  if (effective_scale > 0) {
    g_hd_scale = (uint8)effective_scale;
  } else {
    fprintf(stderr, "HD: no sheets loaded, falling back to SD\n");
    g_hd_enabled = false;
    g_hd_scale   = 1;
  }

  // Synthesize the global magenta placeholder sheet.
  // Tiles encode: row 0 = sheet_id (0xFE) bits, row 1 = tile_in_sheet bits.
  {
    int S   = (int)g_hd_scale;
    int ph_w = 128 * S;
    int ph_h =  64 * S;  // 8 tile rows × 8 SD pixels × S
    g_hd_placeholder.width        = (uint16)ph_w;
    g_hd_placeholder.height       = (uint16)ph_h;
    g_hd_placeholder.tile_count   = 128;
    g_hd_placeholder.scale        = (uint8)S;
    g_hd_placeholder.loaded       = true;
    g_hd_placeholder.index_buffer = NULL;  // not used; blit reads bgra_buffer

    size_t sz = (size_t)ph_w * (size_t)ph_h * 4;
    g_hd_placeholder.bgra_buffer = (uint8 *)malloc(sz);
    if (!g_hd_placeholder.bgra_buffer) {
      fprintf(stderr, "HD: out of memory synthesizing placeholder\n");
      return;
    }

    uint32_t *bgra    = (uint32_t *)g_hd_placeholder.bgra_buffer;
    // BGRA: B=0xFF, G=0x00, R=0xFF, A=0xFF  ->  0xFF FF 00 FF
    uint32_t  magenta = 0xFF | (0x00u << 8) | (0xFFu << 16) | (0xFFu << 24);
    uint32_t  white   = 0xFFFFFFFFu;
    uint32_t  black   = 0xFF000000u;

    for (int p = 0; p < ph_w * ph_h; p++) bgra[p] = magenta;

    uint8 sheet_byte = 0xFE;
    for (int tile = 0; tile < 128; tile++) {
      int tc     = tile & 0xf;
      int tr     = tile >> 4;
      int tile_x = tc * 8 * S;
      int tile_y = tr * 8 * S;
      uint8 tile_byte = (uint8)tile;

      // Row 0: encode sheet ID (0xFE), MSB first, each bit = S×S block.
      for (int bit = 0; bit < 8; bit++) {
        uint32_t col = (sheet_byte & (0x80u >> bit)) ? white : black;
        int bx = tile_x + bit * S;
        int by = tile_y;
        for (int py = 0; py < S; py++)
          for (int px = 0; px < S; px++)
            bgra[(by + py) * ph_w + bx + px] = col;
      }
      // Row 1: encode tile index (low 8 bits).
      for (int bit = 0; bit < 8; bit++) {
        uint32_t col = (tile_byte & (0x80u >> bit)) ? white : black;
        int bx = tile_x + bit * S;
        int by = tile_y + S;
        for (int py = 0; py < S; py++)
          for (int px = 0; px < S; px++)
            bgra[(by + py) * ph_w + bx + px] = col;
      }
    }
  }
}

void HdGfx_Free(void) {
  for (int i = 0; i < kHdSheetCount; i++) {
    if (g_hd_sheets[i].index_buffer) {
      free(g_hd_sheets[i].index_buffer);
      memset(&g_hd_sheets[i], 0, sizeof(g_hd_sheets[i]));
    }
  }
  if (g_hd_placeholder.bgra_buffer) {
    free(g_hd_placeholder.bgra_buffer);
    memset(&g_hd_placeholder, 0, sizeof(g_hd_placeholder));
  }
}
