#pragma once
#include "types.h"
#include "hd_frame.h"
#include <stddef.h>

// Blit one BG layer (lo or hi priority tiles) into dst at HD resolution.
void HdBlitBgLayer(uint8 *dst, size_t pitch,
                   int hd_w, int hd_h,
                   int bg_layer, bool prio_hi,
                   const HdFrameSnapshot *f);

// Mode 7 placeholder — magenta fill with 0xB7 sentinel bits.
// Replaced by the real Mode 7 path in Step 11.
void HdDrawMode7Placeholder(uint8 *dst, size_t pitch, int hd_w, int hd_h);
