#pragma once
#include "types.h"

typedef struct HdSheet {
  uint8  *index_buffer;  // width*height bytes; one palette index per HD pixel; 0 = transparent
  uint16  width;         // scale * 128 (always 16 tiles wide)
  uint16  height;        // scale * (8 * num_tile_rows); variable
  uint16  tile_count;    // total number of 8x8 tiles in the sheet (width/8S * height/8S)
  uint8   scale;         // integer scale factor (1..N)
  bool    loaded;
} HdSheet;

enum { kHdSheetCount = 0x34 };  // gfx00..gfx33

extern HdSheet g_hd_sheets[kHdSheetCount];

void HdGfx_LoadAll(const char *hd_dir);
void HdGfx_Free(void);
