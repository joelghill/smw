#pragma once
#include "types.h"
#include <stddef.h>

// Represents a single contiguous VRAM region whose content was uploaded from
// a known source sheet.  Regions are stored newest-last; ResolveTile scans
// from the end so the most-recent upload for a given address wins.
typedef struct HdVramRegion {
  uint16 vram_word_addr;   // base VRAM word address of the region
  uint16 tile_count;       // number of 4bpp 8×8 tiles (16 VRAM words each)
  uint8  sheet_id;         // 0x00..0x33; 0xFF means unresolved/empty
  uint16 src_tile_offset;  // index of the first tile within the source sheet
} HdVramRegion;

// Reset the region list.  Does NOT clear the staging registry — call once at
// startup (or on level load) to discard stale per-level regions.
void HdVramMap_Reset(void);

// Path A — called from UploadGraphicsFiles_UploadGFXFile after the tile loops
// complete.  Records that 'tile_count' tiles of sheet 'sheet_id' (starting at
// source tile 'src_tile_offset') now occupy VRAM starting at dst_word_addr.
void HdVramMap_RecordSheetUpload(uint16 dst_word_addr, uint8 sheet_id,
                                 uint16 src_tile_offset, uint16 tile_count);

// Path B — called from SmwCopyToVram after the copy.  Looks 'src' up in the
// staging registry; if it falls inside a registered buffer, records the region.
// If 'src' is not in any registered buffer the call is a no-op.
void HdVramMap_RecordCopyFromStaging(uint16 dst_word_addr, const uint8 *src,
                                     int byte_count);

// Register a fixed RAM buffer as holding a known sheet (4bpp, 32 bytes/tile).
// 'sheet_tile_base' is the index within the sheet of the first tile in the
// buffer (usually 0).  Calling again with the same sheet_id updates the entry.
void HdVramMap_RegisterStaging(uint8 sheet_id, const uint8 *base, size_t size,
                               uint16 sheet_tile_base);

// Resolve a VRAM tile word-address to (sheet_id, tile_in_sheet).
// Returns false if the address has no recorded upload.
bool HdVramMap_ResolveTile(uint16 vram_word_addr, uint8 *sheet_out,
                           uint16 *tile_out);

// Dump current state to stderr for debugging.
void HdVramMap_Dump(void);
