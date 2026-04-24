#include "hd_vram_map.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Region list — written newest-last so ResolveTile can scan from the end and
// return the most-recent upload for a given VRAM address.
// ---------------------------------------------------------------------------
#define kMaxRegions 128

static HdVramRegion g_regions[kMaxRegions];
static int          g_region_count;

// ---------------------------------------------------------------------------
// Staging registry — maps a RAM buffer to a known sheet.  Persists across
// HdVramMap_Reset() calls; only grows, never shrinks.
// ---------------------------------------------------------------------------
#define kMaxStaging 8

typedef struct {
  uint8        sheet_id;
  const uint8 *base;
  size_t       size;
  uint16       sheet_tile_base;  // index of the first tile in the sheet at base
} StagingEntry;

static StagingEntry g_staging[kMaxStaging];
static int          g_staging_count;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void HdVramMap_Reset(void) {
  g_region_count = 0;
  // Staging registry is intentionally NOT cleared here.
}

void HdVramMap_RegisterStaging(uint8 sheet_id, const uint8 *base, size_t size,
                               uint16 sheet_tile_base) {
  // Update an existing entry for this sheet_id if present.
  for (int i = 0; i < g_staging_count; i++) {
    if (g_staging[i].sheet_id == sheet_id) {
      g_staging[i].base            = base;
      g_staging[i].size            = size;
      g_staging[i].sheet_tile_base = sheet_tile_base;
      return;
    }
  }
  if (g_staging_count < kMaxStaging) {
    g_staging[g_staging_count].sheet_id        = sheet_id;
    g_staging[g_staging_count].base            = base;
    g_staging[g_staging_count].size            = size;
    g_staging[g_staging_count].sheet_tile_base = sheet_tile_base;
    g_staging_count++;
  }
}

void HdVramMap_RecordSheetUpload(uint16 dst_word_addr, uint8 sheet_id,
                                 uint16 src_tile_offset, uint16 tile_count) {
  if (sheet_id == 0xFF || tile_count == 0)
    return;
  if (g_region_count < kMaxRegions) {
    g_regions[g_region_count].vram_word_addr  = dst_word_addr;
    g_regions[g_region_count].tile_count      = tile_count;
    g_regions[g_region_count].sheet_id        = sheet_id;
    g_regions[g_region_count].src_tile_offset = src_tile_offset;
    g_region_count++;
  }
}

void HdVramMap_RecordCopyFromStaging(uint16 dst_word_addr, const uint8 *src,
                                     int byte_count) {
  if (byte_count <= 0)
    return;

  uint8  rec_sheet_id    = 0xFF;  // 0xFF sentinel = unmapped
  uint16 rec_tile_offset = 0;

  for (int i = 0; i < g_staging_count; i++) {
    const StagingEntry *e = &g_staging[i];
    if (src < e->base || src >= e->base + e->size)
      continue;

    size_t byte_offset = (size_t)(src - e->base);
    // Each 4bpp 8×8 tile is 32 bytes.  Require alignment.
    if (byte_offset % 32 != 0)
      return;

    rec_sheet_id    = e->sheet_id;
    rec_tile_offset = e->sheet_tile_base + (uint16)(byte_offset / 32);
    break;
  }

  // Source not in any registered staging buffer: don't record a region.
  // ResolveTile will fall back to the most-recent Path-A bulk upload covering
  // this VRAM address, which is the level's current sprite GFX sheet — the
  // right answer for Yoshi dynamic-tile uploads (g_ram + 0x8500+), RestoreSP1
  // (g_ram + 0xbf6/0xcb6), and any other unregistered source writing into an
  // already-populated sprite page.  Hardcoding sheet 0x32 here (as earlier
  // versions did) mapped every SP1 write onto Mario's sheet, which is only
  // correct for Mario himself.
  if (rec_sheet_id == 0xFF)
    return;

  uint16 tile_count = (uint16)(byte_count / 32);
  if (tile_count == 0)
    return;

  // Per-frame uploads always hit the same small set of VRAM addresses.
  // Update an existing entry in-place when address and size match so the
  // region list doesn't grow without bound.
  for (int j = g_region_count - 1; j >= 0; j--) {
    if (g_regions[j].vram_word_addr == dst_word_addr &&
        g_regions[j].tile_count == tile_count) {
      g_regions[j].sheet_id        = rec_sheet_id;
      g_regions[j].src_tile_offset = rec_tile_offset;
      return;
    }
  }

  if (g_region_count < kMaxRegions) {
    g_regions[g_region_count].vram_word_addr  = dst_word_addr;
    g_regions[g_region_count].tile_count      = tile_count;
    g_regions[g_region_count].sheet_id        = rec_sheet_id;
    g_regions[g_region_count].src_tile_offset = rec_tile_offset;
    g_region_count++;
  }
}

bool HdVramMap_ResolveTile(uint16 vram_word_addr, uint8 *sheet_out,
                           uint16 *tile_out) {
  // Pick the most specific overlapping region (smallest tile_count wins).
  // This is ordering-independent: Path-B per-frame uploads (tile_count=2) beat
  // Path-A bulk uploads (tile_count=128) for the same VRAM address regardless
  // of which was appended first, which matters because Path-B regions often
  // exist before Path-A for a slot (title-screen Mario writes before the
  // level's UploadGraphicsFiles_UploadGFXFile runs) and the update-in-place
  // scan leaves Path-B at its original array position.
  // Ties are broken by recency — newer (higher index) wins — so stale
  // same-specificity regions from a prior level don't shadow current ones.
  int    best            = -1;
  uint16 best_tile_count = 0xFFFF;
  for (int i = g_region_count - 1; i >= 0; i--) {
    const HdVramRegion *r = &g_regions[i];
    if (vram_word_addr >= r->vram_word_addr &&
        vram_word_addr < (uint16)(r->vram_word_addr + r->tile_count * 16u)) {
      if (r->tile_count < best_tile_count) {
        best            = i;
        best_tile_count = r->tile_count;
      }
    }
  }
  if (best < 0)
    return false;
  const HdVramRegion *r = &g_regions[best];
  // 0xFF = explicit unmapped marker: return false so the compositor skips.
  if (r->sheet_id == 0xFF)
    return false;
  uint16 tile_offset_in_region = (vram_word_addr - r->vram_word_addr) / 16;
  *sheet_out = r->sheet_id;
  *tile_out  = r->src_tile_offset + tile_offset_in_region;
  return true;
}

void HdVramMap_Dump(void) {
  fprintf(stderr, "=== HdVramMap Dump (%d regions) ===\n", g_region_count);
  for (int i = g_region_count - 1; i >= 0; i--) {
    const HdVramRegion *r = &g_regions[i];
    fprintf(stderr, "  [%d] sheet=gfx%02x  vram=0x%04x  tiles=%u  src_tile=%u\n",
            i, r->sheet_id, r->vram_word_addr, r->tile_count,
            r->src_tile_offset);
  }
  fprintf(stderr, "=== Staging registry (%d entries) ===\n", g_staging_count);
  for (int i = 0; i < g_staging_count; i++) {
    const StagingEntry *e = &g_staging[i];
    fprintf(stderr, "  [%d] sheet=gfx%02x  base=%p  size=%zu  tile_base=%u\n",
            i, e->sheet_id, (const void *)e->base, e->size,
            e->sheet_tile_base);
  }
}
