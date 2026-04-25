#pragma once
#include <stddef.h>
#include "types.h"

// Forward declaration — avoids pulling in hd_scene.h into every translation unit.
struct HdScene;

extern bool    g_hd_enabled;
extern uint8   g_hd_scale;
extern bool    g_hd_skip_sprites;

// v2: per-pixel priority z-buffer captured each scanline from bgBuffers[0].data.
// 256 * 240 uint16 entries; cleared to zero at the start of each frame.
extern uint16 *g_hd_prio_map;

// v2: per-BG-layer enable: [0]=BG1, [1]=BG2, [2]=BG3.  All false until Step 9b.
extern bool    g_hd_bg_enabled[3];

// ---------------------------------------------------------------------------
// Priority-layer IDs decoded from the PPU z-buffer upper byte.
// ---------------------------------------------------------------------------
typedef enum {
  kHdBgLayer_Backdrop      = 0,
  kHdBgLayer_BG3lo         = 1,
  kHdBgLayer_Spr0          = 2,
  kHdBgLayer_BG3hi_noprio  = 3,
  kHdBgLayer_Spr1          = 4,
  kHdBgLayer_BG2lo         = 5,
  kHdBgLayer_BG1lo         = 6,
  kHdBgLayer_Spr2          = 7,
  kHdBgLayer_BG2hi         = 8,
  kHdBgLayer_BG1hi         = 9,
  kHdBgLayer_Spr3          = 10,
  kHdBgLayer_BG3hi_prio    = 11,
} HdBgLayerID;

// Decode a raw z-buffer entry to its layer ID using cascaded >= thresholds.
HdBgLayerID HdDecodeZbuf(uint16 z);

void HdCompositor_Init(void);
void HdCompositor_ApplyConfig(void);
void HdCompositor_Shutdown(void);
bool HdCompositor_Toggle(void);   // flips g_hd_enabled; returns new value
void HdCompositor_Draw(uint8 *dst, size_t pitch,
                       const uint8 *sd_pixels,
                       int sd_width, int sd_height);

// Returns a pointer to the per-frame scene built by the last HdCompositor_Draw call.
const struct HdScene *HdCompositor_GetScene(void);
