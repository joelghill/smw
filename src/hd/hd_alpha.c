#include "hd_alpha.h"

void HdAlpha_Blit(uint8 *dst, size_t dst_pitch,
                  const uint8 *src, size_t src_pitch,
                  int w, int h) {
  for (int y = 0; y < h; y++) {
    uint8       *dst_row = dst + y * dst_pitch;
    const uint8 *src_row = src + y * src_pitch;
    for (int x = 0; x < w; x++) {
      uint8 a = src_row[x * 4 + 3];
      if (a == 0) continue;
      uint8 inv        = (uint8)(255 - a);
      dst_row[x*4 + 0] = (uint8)((src_row[x*4+0] * a + dst_row[x*4+0] * inv + 127) / 255);
      dst_row[x*4 + 1] = (uint8)((src_row[x*4+1] * a + dst_row[x*4+1] * inv + 127) / 255);
      dst_row[x*4 + 2] = (uint8)((src_row[x*4+2] * a + dst_row[x*4+2] * inv + 127) / 255);
      dst_row[x*4 + 3] = 0xff;
    }
  }
}
