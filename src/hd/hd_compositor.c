#include "hd_compositor.h"
#include "hd_frame.h"
#include "hd_gfx.h"
#include "hd_vram_map.h"
#include "hd_alpha.h"
#include "hd_blit_sprite.h"
#include "hd_blit_bg.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool  g_hd_enabled      = true;
uint8 g_hd_scale        = 1;
bool  g_hd_skip_sprites = false;
bool  g_hd_bg_enabled[3] = { true, true, false };  // BG3 stays off until Step 10

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
  size_t sz       = (size_t)hd_w * (size_t)hd_h * 4;
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
// Per-OAM-priority sprite layer: optional shadow pre-pass then color pass.
// ---------------------------------------------------------------------------
static void CompositeSpriteLayer(uint8 *dst, size_t pitch,
                                 int hd_w, int hd_h,
                                 int oam_prio,
                                 const HdFrameSnapshot *f) {
  const HdLayerCfg *cfg       = &g_hd_layer_cfg[oam_prio];
  size_t            work_pitch = (size_t)hd_w * 4;

  if (cfg->shadow_enabled) {
    memset(g_hd_shadow_buf, 0, work_pitch * (size_t)hd_h);
    for (int k = (int)f->sprites.count - 1; k >= 0; k--) {
      const HdSprite *s = &f->sprites.sprites[k];
      if (s->layer != (uint8)oam_prio) continue;
      HdBlitSprite(g_hd_shadow_buf, work_pitch, hd_w, hd_h,
                   s, kHdBlitSilhouette,
                   cfg->shadow_dx, cfg->shadow_dy,
                   cfg->shadow_r, cfg->shadow_g, cfg->shadow_b,
                   cfg->shadow_alpha, f);
    }
    HdAlpha_Blit(dst, pitch, g_hd_shadow_buf, work_pitch, hd_w, hd_h);
  }

  memset(g_hd_layer_buf, 0, work_pitch * (size_t)hd_h);
  for (int k = (int)f->sprites.count - 1; k >= 0; k--) {
    const HdSprite *s = &f->sprites.sprites[k];
    if (s->layer != (uint8)oam_prio) continue;
    HdBlitSprite(g_hd_layer_buf, work_pitch, hd_w, hd_h,
                 s, kHdBlitColor, 0, 0, 0, 0, 0, 0xff, f);
  }
  HdAlpha_Blit(dst, pitch, g_hd_layer_buf, work_pitch, hd_w, hd_h);
}

// ---------------------------------------------------------------------------
// Main entry point — called once per frame from RtlDrawPpuFrame.
// ---------------------------------------------------------------------------
void HdCompositor_Draw(uint8 *dst, size_t pitch, const HdRenderInput *input) {
  const HdFrameSnapshot *f = &input->frame;
  int S         = g_hd_scale;
  int hd_width  = 256 * S;
  int hd_height = 224 * S;

  // Backdrop fill from pre-decoded CGRAM[0].
  for (int y = 0; y < hd_height; y++) {
    uint32_t *row = (uint32_t *)((uint8 *)dst + (size_t)y * pitch);
    for (int x = 0; x < hd_width; x++) row[x] = f->backdrop_bgra;
  }

  if (g_hd_scale <= 1) return;

  HdCompositor_EnsureBuffers(hd_width, hd_height);

  // Mode 7 short-circuit — real implementation deferred to Step 11.
  if (f->mode == 7) {
    HdDrawMode7Placeholder(dst, pitch, hd_width, hd_height);
    return;
  }

  // 11-pass priority-ordered composite (Mode 1).
  HdBlitBgLayer(dst, pitch, hd_width, hd_height, 2, false, f);
  CompositeSpriteLayer(dst, pitch, hd_width, hd_height, 0, f);
  if (!f->bg3prio)
    HdBlitBgLayer(dst, pitch, hd_width, hd_height, 2, true, f);
  CompositeSpriteLayer(dst, pitch, hd_width, hd_height, 1, f);
  HdBlitBgLayer(dst, pitch, hd_width, hd_height, 1, false, f);
  HdBlitBgLayer(dst, pitch, hd_width, hd_height, 0, false, f);
  CompositeSpriteLayer(dst, pitch, hd_width, hd_height, 2, f);
  HdBlitBgLayer(dst, pitch, hd_width, hd_height, 1, true,  f);
  HdBlitBgLayer(dst, pitch, hd_width, hd_height, 0, true,  f);
  CompositeSpriteLayer(dst, pitch, hd_width, hd_height, 3, f);
  if (f->bg3prio)
    HdBlitBgLayer(dst, pitch, hd_width, hd_height, 2, true, f);
}
