#pragma once
#include "types.h"
#include "hd_frame.h"
#include "hd_scene.h"
#include <stddef.h>

typedef enum {
  kHdBlitColor      = 0,  // CGRAM-indexed color, brightness applied
  kHdBlitSilhouette = 1,  // fixed RGBA, only where HD index != 0
} HdBlitMode;

// Blit one sprite into dst_buf in color or silhouette mode.
// offset_x / offset_y are in HD pixels (used for shadow displacement).
void HdBlitSprite(uint8 *dst_buf, size_t dst_pitch,
                  int dst_w, int dst_h,
                  const HdSprite *s,
                  HdBlitMode mode,
                  int16 offset_x, int16 offset_y,
                  uint8 sil_r, uint8 sil_g, uint8 sil_b, uint8 sil_a,
                  const HdFrameSnapshot *f);
