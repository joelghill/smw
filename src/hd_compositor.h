#pragma once
#include <stddef.h>
#include "types.h"

extern bool  g_hd_enabled;
extern uint8 g_hd_scale;
extern bool  g_hd_skip_sprites;

void HdCompositor_Init(void);
void HdCompositor_Draw(uint8 *dst, size_t pitch,
                       const uint8 *sd_pixels,
                       int sd_width, int sd_height);
