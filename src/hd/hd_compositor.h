#pragma once
#include <stddef.h>
#include "types.h"
#include "hd_frame.h"

extern bool   g_hd_enabled;
extern uint8  g_hd_scale;
extern bool   g_hd_skip_sprites;

// v2: per-BG-layer enable: [0]=BG1, [1]=BG2, [2]=BG3.  All false until Step 9b.
extern bool   g_hd_bg_enabled[3];

void HdCompositor_Init(void);
void HdCompositor_ApplyConfig(void);
void HdCompositor_Shutdown(void);
bool HdCompositor_Toggle(void);   // flips g_hd_enabled; returns new value
void HdCompositor_Draw(uint8 *dst, size_t pitch, const HdRenderInput *input);
