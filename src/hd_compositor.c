#include "hd_compositor.h"
#include "hd_gfx.h"
#include "hd_scene.h"
#include "hd_vram_map.h"
#include "common_rtl.h"
#include <stdint.h>

static HdScene g_hd_scene;

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
  // (Sprite compositing will be added in Step 5.)
  HdScene_Build(&g_hd_scene, g_my_ppu);
}

const HdScene *HdCompositor_GetScene(void) {
  return &g_hd_scene;
}
