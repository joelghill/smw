#pragma once
#include "types.h"

// Forward-declare Ppu so callers only need hd_scene.h.
struct Ppu;

// One visible OAM sprite entry for the HD compositor.
typedef struct HdSprite {
  int16  x, y;       // SD pixel coords, top-left. May be negative / off-screen.
  uint8  size;       // sprite size in SD pixels (8, 16, 32, or 64). Square.
  uint8  palette;    // 0..7; CGRAM base = 0x80 + palette * 16
  uint8  flags;      // bit0 = hflip, bit1 = vflip
  uint8  layer;      // 0..3 (OAM priority field)
  uint16 tile_vram;  // VRAM word-addr of the sprite's top-left 8×8 tile.
                     // The compositor resolves per sub-tile via HdVramMap_ResolveTile.
  uint8  _pad[2];
} HdSprite;

enum { kHdSceneMax = 128 };

typedef struct HdScene {
  HdSprite sprites[kHdSceneMax];
  uint16   count;
} HdScene;

// Build the per-frame scene from the current PPU OAM state.
// Walks OAM entries 0..127 and populates *scene.
// Skips hidden sprites (y == 0xF0) and sprites that are fully off-screen.
void HdScene_Build(HdScene *scene, const struct Ppu *ppu);

// Dump the scene to stderr for debugging.
void HdScene_Dump(const HdScene *scene);
