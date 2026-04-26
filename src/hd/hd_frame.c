#include "hd_frame.h"
#include "hd_scene.h"
#include "snes/ppu.h"
#include "variables.h"
#include "common_rtl.h"
#include <string.h>

void HdFrame_Build(HdRenderInput *input, const Ppu *ppu) {
  HdFrameSnapshot *f = &input->frame;

  // --- BG configuration ---
  f->mode    = PPU_mode(ppu);
  f->bg3prio = PPU_bg3priority(ppu) != 0;
  f->mosaic_size = (uint8)PPU_mosaicSize(ppu);

  for (int i = 0; i < 3; i++) {
    f->tilemap_adr[i]    = PPU_bgTilemapAdr(ppu, i);
    f->tile_adr[i]       = PPU_bgTileAdr(ppu, i);
    f->tilemap_wider[i]  = PPU_bgTilemapWider(ppu, i) != 0;
    f->tilemap_higher[i] = PPU_bgTilemapHigher(ppu, i) != 0;
    f->hscroll[i]        = (int)ppu->hScroll[i];
    f->vscroll[i]        = (int)ppu->vScroll[i];
    f->mosaic_enabled[i] = PPU_mosaicEnabled(ppu, i) != 0;
  }

  // --- Palette: decode all 256 CGRAM entries to BGRA once ---
  for (int i = 0; i < 256; i++) {
    uint16_t c = ppu->cgram[i];
    uint8 r = ppu->brightnessMult[(c >>  0) & 0x1f];
    uint8 g = ppu->brightnessMult[(c >>  5) & 0x1f];
    uint8 b = ppu->brightnessMult[(c >> 10) & 0x1f];
    f->palette_bgra[i] = (uint32_t)b | ((uint32_t)g << 8)
                       | ((uint32_t)r << 16) | 0xff000000u;
  }
  f->backdrop_bgra = f->palette_bgra[0];

  // --- VRAM pointer (not a copy — VRAM is stable during the compositor pass) ---
  f->vram = ppu->vram;

  // --- Sprite scene ---
  HdScene_Build(&f->sprites, ppu);

  // --- Game state ---
  HdGameState *gs = &input->game;

  gs->lives = players_lives[0];
  gs->coins = player_current_coin_count;

  // Score is three BCD bytes: hi encodes digits 5-4, mid 3-2, lo 1-0.
  {
    uint8 sh = player_mario_score_hi;
    uint8 sm = player_mario_score_mid;
    uint8 sl = player_mario_score_lo;
    gs->score = (uint32_t)(sh >> 4) * 100000u + (uint32_t)(sh & 0xf) * 10000u
              + (uint32_t)(sm >> 4) *   1000u  + (uint32_t)(sm & 0xf) *   100u
              + (uint32_t)(sl >> 4) *     10u  + (uint32_t)(sl & 0xf);
  }

  // Timer is three separate digit bytes (each 0..9).
  gs->level_timer = (uint16)counter_timer_hundreds * 100u
                  + (uint16)counter_timer_tens      *  10u
                  + (uint16)counter_timer_ones;

  // world / level: wired up when HUD rendering is implemented.
  gs->world = 0;
  gs->level = 0;

  // --- Overlay text: none populated yet ---
  input->text_items = NULL;
  input->text_count = 0;
}
