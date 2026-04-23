#include "../third_party/stb/stb_image.h"

#include "hd_gfx.h"
#include "hd_compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HdSheet g_hd_sheets[kHdSheetCount];

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

    // Validate dimensions: must be multiples of 128/64 with matching scale
    if (w % 128 != 0 || h % 64 != 0 || w / 128 != h / 64 || w / 128 < 1) {
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
}

void HdGfx_Free(void) {
  for (int i = 0; i < kHdSheetCount; i++) {
    if (g_hd_sheets[i].index_buffer) {
      free(g_hd_sheets[i].index_buffer);
      memset(&g_hd_sheets[i], 0, sizeof(g_hd_sheets[i]));
    }
  }
}
