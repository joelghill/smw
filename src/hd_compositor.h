#pragma once
#include <stddef.h>
#include "types.h"

// Forward declaration — avoids pulling in hd_scene.h into every translation unit.
struct HdScene;

extern bool  g_hd_enabled;
extern uint8 g_hd_scale;
extern bool  g_hd_skip_sprites;

void HdCompositor_Init(void);
void HdCompositor_ApplyConfig(void);
void HdCompositor_Shutdown(void);
bool HdCompositor_Toggle(void);   // flips g_hd_enabled; returns new value
void HdCompositor_Draw(uint8 *dst, size_t pitch,
                       const uint8 *sd_pixels,
                       int sd_width, int sd_height);

// Returns a pointer to the per-frame scene built by the last HdCompositor_Draw call.
const struct HdScene *HdCompositor_GetScene(void);
