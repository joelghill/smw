#pragma once
#include "types.h"
#include "hd_scene.h"
#include <stddef.h>

// An overlay text item queued for HD rendering.
typedef struct HdTextItem {
  int16       x, y;        // HD pixel position, top-left
  const char *text;
  uint32_t    color_bgra;
  uint8       font_size;
} HdTextItem;

// Semantic game-state values, decoded from the raw SNES globals once per frame.
// Fields that are not yet wired up remain zero.
typedef struct HdGameState {
  uint8    lives;
  uint8    coins;
  uint32_t score;        // decoded from BCD, 0..999999
  uint16   level_timer;  // decoded from BCD, 0..999
  uint8    world;
  uint8    level;
} HdGameState;

// Snapshot of all PPU state the compositor needs for one frame.
// Built once by HdFrame_Build; passed as const to every blit function.
typedef struct HdFrameSnapshot {
  // BG configuration (three layers; layer index = 0..2)
  int   mode;
  bool  bg3prio;
  int   tilemap_adr[3];
  int   tile_adr[3];
  bool  tilemap_wider[3];
  bool  tilemap_higher[3];
  int   hscroll[3];
  int   vscroll[3];
  uint8 mosaic_size;
  bool  mosaic_enabled[3];

  // Full 256-entry palette pre-decoded to BGRA with brightness applied.
  // BG palette base = palette * 16; sprite palette base = 0x80 + palette * 16.
  uint32_t palette_bgra[256];
  uint32_t backdrop_bgra;  // palette_bgra[0], copied out for clarity

  // Pointer into the PPU's live VRAM buffer.  Valid for the compositor pass
  // (VRAM is not written during rendering).
  const uint16 *vram;

  // Sprite scene built from current OAM state.
  HdScene sprites;
} HdFrameSnapshot;

// The full input to HdCompositor_Draw for one frame.
typedef struct HdRenderInput {
  HdFrameSnapshot frame;
  HdGameState     game;

  // Optional overlay text.  May be NULL when text_count == 0.
  HdTextItem *text_items;
  int         text_count;
} HdRenderInput;

// Forward declaration — avoids pulling snes/ppu.h into callers.
struct Ppu;

// Populate *input from the current PPU registers and game-state globals.
// Call once per frame, before HdCompositor_Draw.
void HdFrame_Build(HdRenderInput *input, const struct Ppu *ppu);
