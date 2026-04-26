#pragma once
#include "types.h"
#include <stddef.h>

// Alpha-composite src (BGRA) onto dst in-place using per-pixel src alpha.
void HdAlpha_Blit(uint8 *dst, size_t dst_pitch,
                  const uint8 *src, size_t src_pitch,
                  int w, int h);
