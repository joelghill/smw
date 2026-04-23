#include "hd_scene.h"
#include "snes/ppu.h"
#include <stdio.h>

// Matches spriteSizes in ppu.c:ppu_evaluateSprites.
static const uint8 spriteSizes[8][2] = {
  {8, 16}, {8, 32}, {8, 64}, {16, 32},
  {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

void HdScene_Build(HdScene *scene, const Ppu *ppu) {
  scene->count = 0;

  for (int i = 0; i < 128; i++) {
    uint16 oam0 = ppu->oam[i * 2];
    uint16 oam1 = ppu->oam[i * 2 + 1];

    // y is in the high byte of oam0; SMW uses y==0xF0 as the "hidden" sentinel.
    uint8 y_raw = (uint8)(oam0 >> 8);
    if (y_raw == 0xF0)
      continue;

    // High-OAM byte for sprite i holds 2 bits per sprite, packed 4 sprites per byte.
    // Bits: (i & 3)*2 = x-high, (i & 3)*2+1 = size-select (large).
    uint8 hi    = (ppu->highOam[i >> 2] >> ((i & 3) * 2)) & 3;
    bool  xhigh = (hi & 1) != 0;
    bool  large = (hi & 2) != 0;

    int16 x = (int16)(oam0 & 0xff) | ((int16)xhigh << 8);
    if (x > 255) x -= 512;  // sign-extend 9-bit two's complement

    uint8 size = spriteSizes[PPU_objSize(ppu)][large ? 1 : 0];

    // Skip sprites entirely off-screen.  The compositor handles per-pixel
    // clipping, but rejecting completely invisible entries saves work.
    if (x + (int16)size <= 0 || x >= 256)
      continue;
    // y is unsigned 8-bit; sprites above y=240 wrap to top of screen.
    // Skip only the clear case where the sprite is entirely below the frame.
    if ((int)y_raw >= 240)
      continue;

    uint8  tile_num     = (uint8)(oam1 & 0xff);
    bool   charnum_high = ((oam1 >> 8) & 1) != 0;
    uint16 objAdr       = charnum_high ? PPU_objTileAdr2(ppu) : PPU_objTileAdr1(ppu);
    // Each 4bpp 8×8 tile occupies 16 VRAM words.
    uint16 tile_vram    = (uint16)((objAdr + (uint16)tile_num * 16) & 0x7fff);

    uint8 palette = (uint8)((oam1 >> 9) & 7);
    // bit 14 = hflip (bit0 of flags), bit 15 = vflip (bit1 of flags)
    uint8 flags   = (uint8)((oam1 >> 14) & 3);
    uint8 layer   = (uint8)((oam1 >> 12) & 3);

    if (scene->count >= kHdSceneMax)
      break;

    HdSprite *s = &scene->sprites[scene->count++];
    s->x         = x;
    s->y         = (int16)y_raw;
    s->size      = size;
    s->palette   = palette;
    s->flags     = flags;
    s->layer     = layer;
    s->tile_vram = tile_vram;
    s->_pad[0]   = 0;
    s->_pad[1]   = 0;
  }
}

void HdScene_Dump(const HdScene *scene) {
  fprintf(stderr, "=== HdScene Dump (%u sprites) ===\n", scene->count);
  for (int i = 0; i < (int)scene->count; i++) {
    const HdSprite *s = &scene->sprites[i];
    fprintf(stderr,
            "  [%3d] xy=(%4d,%4d) size=%2u tile_vram=0x%04x "
            "palette=%u flags=%c%c layer=%u\n",
            i, s->x, s->y, s->size, s->tile_vram,
            s->palette,
            (s->flags & 1) ? 'H' : '-',
            (s->flags & 2) ? 'V' : '-',
            s->layer);
  }
}
