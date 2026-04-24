"""
Export SMW GFX files as palette-index greyscale spritesheets for HD replacement.

Source sheets go to <export_dir>/source/ as single RGBA images where each
pixel's grey level encodes its original SNES palette index. The game keeps
applying palettes at runtime, so HD replacements in <export_dir>/hd/ only
need to stay faithful to the index structure — the renderer reads the grey
level back to an index, then looks up the live CGRAM color as usual. This
means the HD sheet is resolution-independent with respect to palette: one
greyscale sheet serves every in-game palette variant.

Encoding
  Palette indices are encoded with grey = index * 17 in all three RGB
  channels (i.e. 4bpp spacing: max_idx = 15). Index 0 is fully transparent
  (alpha 0) to mark sprite/BG transparency. Round-trip is
  index = round(grey * 15 / 255).

  The runtime always addresses CGRAM as 4bpp (16-entry palettes) regardless
  of the source sheet's bpp — 3bpp sheets are inflated to 4bpp at VRAM
  upload, and their used indices remain 0..7 in a 16-entry palette slot.
  Encoding at a fixed 4bpp step means a 3bpp source index 1 is grey 17
  (not 36), so the loader decodes it back to index 1 rather than index 2.

Tile formats (per sneslab.net/wiki/Graphics_Format)
  2bpp (16 B/tile): bp1+bp2 intertwined row-by-row.
  3bpp (24 B/tile): bp1+bp2 intertwined, then bp3 as a flat 8-byte block.
  4bpp (32 B/tile): bp1+bp2 then bp3+bp4 intertwined.
  Leftmost pixel of each row is bit 7 of the bitplane byte.

SMW stores most GFX slots natively as 3bpp and inflates to 4bpp at VRAM
upload; the bpp is inferred from the decompressed size (128 tiles per slot).
"""

import os
import sys
import util


_TILE_BYTES = {2: 16, 3: 24, 4: 32}


def _decode_tile(data, offset, bpp):
    """Return 64 palette indices (row-major, left-to-right) for one 8x8 tile."""
    pixels = []
    for y in range(8):
        p0 = data[offset + y * 2]
        p1 = data[offset + y * 2 + 1]
        if bpp == 2:
            p2 = p3 = 0
        elif bpp == 3:
            p2 = data[offset + 16 + y]
            p3 = 0
        else:  # 4
            p2 = data[offset + y * 2 + 16]
            p3 = data[offset + y * 2 + 17]
        for bit in range(7, -1, -1):
            pixels.append(
                ((p0 >> bit) & 1) |
                (((p1 >> bit) & 1) << 1) |
                (((p2 >> bit) & 1) << 2) |
                (((p3 >> bit) & 1) << 3)
            )
    return pixels


def _detect_bpp(size):
    """Infer bpp from decompressed byte count (SMW slots hold 128 tiles)."""
    if size == 4096: return 4
    if size == 3072: return 3
    if size == 2048: return 2
    if size % 24 == 0: return 3
    if size % 32 == 0: return 4
    return 2


def gfx_to_greyscale_image(tile_data, bpp=4, tiles_wide=16):
    """Render tile data as a single RGBA sheet with indices encoded as grey.

    Each tile is 8x8. The output image is tiles_wide tiles across, row-major.
    Pixel index 0 is transparent; indices 1..(2**bpp - 1) map to evenly-spaced
    grey values from (1*255/max) up to 255.
    """
    from PIL import Image

    tile_bytes = _TILE_BYTES[bpp]
    num_tiles = len(tile_data) // tile_bytes
    if num_tiles == 0:
        return None

    # Always encode at 4bpp spacing: the runtime inflates 3bpp VRAM tiles to
    # 4bpp and addresses 16-entry palettes, so a 3bpp source index N must
    # land at grey = N * 17 (not N * 255/7) to round-trip correctly.
    rows = (num_tiles + tiles_wide - 1) // tiles_wide
    img = Image.new('RGBA', (tiles_wide * 8, rows * 8), (0, 0, 0, 0))
    pix = img.load()

    for ti in range(num_tiles):
        tile = _decode_tile(tile_data, ti * tile_bytes, bpp)
        tx, ty = (ti % tiles_wide) * 8, (ti // tiles_wide) * 8
        for py in range(8):
            for px in range(8):
                ci = tile[py * 8 + px]
                if ci == 0:
                    pix[tx + px, ty + py] = (0, 0, 0, 0)
                else:
                    g = ci * 17
                    pix[tx + px, ty + py] = (g, g, g, 255)

    return img


def export_sheets(export_dir):
    """Export all SMW GFX files to <export_dir>/source/ as greyscale PNGs."""
    try:
        from PIL import Image
    except ImportError:
        print('ERROR: Pillow is required.  Run: pip install Pillow', file=sys.stderr)
        sys.exit(1)

    from compile_resources import decomp_data

    source_dir = os.path.join(export_dir, 'source')
    hd_dir     = os.path.join(export_dir, 'hd')
    os.makedirs(source_dir, exist_ok=True)
    os.makedirs(hd_dir,     exist_ok=True)

    tiles_wide = 16
    exported   = 0

    # --- GFX 0x00-0x31: 50 standard files via pointer table at 0xB992 ---
    lo   = util.get_bytes(0xB992, 50)
    hi   = util.get_bytes(0xB9c4, 50)
    bank = util.get_bytes(0xB9f6, 50)

    for i in range(50):
        addr      = bank[i] << 16 | hi[i] << 8 | lo[i]
        tile_data = decomp_data(addr)
        bpp       = _detect_bpp(len(tile_data))
        img       = gfx_to_greyscale_image(tile_data, bpp, tiles_wide)
        if img is None:
            print(f'  gfx{i:02x}: empty, skipped')
            continue
        path = os.path.join(source_dir, f'gfx{i:02x}.png')
        img = img.resize((img.width * 4, img.height * 4), Image.NEAREST)
        img.save(path)
        num_tiles = len(tile_data) // _TILE_BYTES[bpp]
        print(f'  gfx{i:02x}.png  {num_tiles:3d} tiles  {bpp}bpp  {img.width}x{img.height}')
        exported += 1

    # --- GFX 0x32 (Mario body, native 4bpp) ---
    # --- GFX 0x33 (Mario alt / Yoshi, 3bpp in ROM) ---
    # gfx33 is stored 3bpp in the vanilla ROM.  The inflation loop in
    # GraphicsDecompressionRoutines_DecompressGFX32And33 reads the source
    # buffer high→low while the destination pointer also travels high→low,
    # so both pointers move in lockstep.  Tracing a single tile confirms:
    #   - The LAST 3bpp tile (ROM tile N-1) is processed first and lands at
    #     the HIGHEST 4bpp output addresses (g_ram+0xace0..0xacff = tile N-1).
    #   - The FIRST 3bpp tile (ROM tile 0) is processed last and lands at
    #     the LOWEST output addresses (g_ram+0x7d00..0x7d1f = tile 0).
    # Tile order is therefore PRESERVED: inflated tile i == ROM tile i.
    # Within each tile, the row order is also preserved (row 0 at the lowest
    # address) because the reversed-read, reversed-write cancel out.
    # No reordering is needed here; export tile i must equal inflated tile i
    # so that HdVramMap_RecordCopyFromStaging resolves Yoshi uploads correctly.
    for gfx_id, word_addr, label in [
        (0x32, 0xB8D8, 'Mario body'),
        (0x33, 0xB88B, 'Mario alt / Yoshi'),
    ]:
        addr      = 0x80000 | util.get_word(word_addr)
        tile_data = decomp_data(addr)
        bpp       = 4 if gfx_id == 0x32 else _detect_bpp(len(tile_data))
        img = gfx_to_greyscale_image(tile_data, bpp, tiles_wide)
        if img is None:
            print(f'  gfx{gfx_id:02x}: empty, skipped')
            continue
        path = os.path.join(source_dir, f'gfx{gfx_id:02x}.png')
        img = img.resize((img.width * 4, img.height * 4), Image.NEAREST)
        img.save(path)
        num_tiles = len(tile_data) // _TILE_BYTES[bpp]
        print(f'  gfx{gfx_id:02x}.png  {num_tiles:3d} tiles  {bpp}bpp  {img.width}x{img.height}  ({label})')
        exported += 1

    print(f'\nExported {exported} sheets to {source_dir}/')
    print(f'Each pixel\'s grey level encodes its SNES palette index.')
    print(f'HD replacements go in: {hd_dir}/')
    print(f'Any integer scale is supported — scale is detected from image dimensions.')
