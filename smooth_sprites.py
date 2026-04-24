#!/usr/bin/env python3
"""
Scale4x pixel-art upscaler for SMW sprite sheets.

Reads 1x source sheets from gfx/source/ and writes 4x HD sheets to gfx/hd/
using Scale4x (Scale2x applied twice).

Scale2x (by Andrea Mazzoleni, https://www.scale2x.it/) is a palette-safe
pixel-art algorithm: it never blends or interpolates colours. It only
rearranges existing palette entries to smooth diagonal edges, turning a
staircase like:

    1 1 0 0          1 1 0 0 0 0 0 0
    1 1 0 0    →     1 1 1 0 0 0 0 0   (and so on at 4x via a second pass)
    1 1 1 1          1 1 1 1 1 0 0 0
    1 1 1 1          1 1 1 1 1 1 1 1

Because Scale2x never produces new colours, the output palette index encoding
(grey = index * 17) is always preserved exactly.
"""

import os
import numpy as np
from PIL import Image

SOURCE_DIR = "gfx/source"
OUTPUT_DIR = "gfx/hd"
SCALE      = 4   # must be a power of 2


def scale2x(indices: np.ndarray) -> np.ndarray:
    """Scale2x on a 2-D array of palette indices.  Returns a 2x-sized array.

    Only existing values are ever written — no blending, no new colours.
    Algorithm: Andrea Mazzoleni's EPX / Scale2x.
    """
    p = np.pad(indices, 1, mode="edge")
    B = p[:-2, 1:-1]   # above
    D = p[1:-1, :-2]   # left
    E = p[1:-1, 1:-1]  # centre (source pixel)
    F = p[1:-1, 2:]    # right
    H = p[2:,  1:-1]   # below

    E0 = np.where((D == B) & (D != H) & (B != F), D, E)  # top-left output
    E1 = np.where((B == F) & (D != B) & (F != H), F, E)  # top-right
    E2 = np.where((D == H) & (D != B) & (H != F), D, E)  # bottom-left
    E3 = np.where((H == F) & (D != H) & (B != F), F, E)  # bottom-right

    h, w = indices.shape
    out = np.empty((h * 2, w * 2), dtype=indices.dtype)
    out[0::2, 0::2] = E0
    out[0::2, 1::2] = E1
    out[1::2, 0::2] = E2
    out[1::2, 1::2] = E3
    return out


def scale4x(indices: np.ndarray) -> np.ndarray:
    """Apply Scale2x twice to get a 4x result."""
    return scale2x(scale2x(indices))


def upscale_sheet(src_path: str, dst_path: str) -> dict:
    img = Image.open(src_path)
    arr = np.array(img, dtype=np.uint8)   # (H, W, 4) RGBA

    # Encode each pixel as a palette index (0 = transparent).
    # All opaque pixels have R == G == B == index * 17.
    # Index 0 transparent and index 0 opaque both encode as 0 — the alpha
    # channel records which is which.
    opaque = (arr[:, :, 3] > 127)
    grey   = arr[:, :, 0]
    # Treat transparent pixels as a distinct "colour" by using 255+1 = 256.
    # This prevents Scale2x from propagating an opaque edge colour into a
    # transparent region.
    indices = np.where(opaque, grey.astype(np.uint16), np.uint16(256))

    # Apply Scale4x.
    up = scale4x(indices)

    # Reconstruct RGBA: transparent wherever index == 256, grey otherwise.
    out_opaque = (up != 256).astype(np.uint8)
    out_grey   = np.where(out_opaque, up.astype(np.uint8), np.uint8(0))

    result = np.stack([out_grey, out_grey, out_grey, out_opaque * 255], axis=2)
    Image.fromarray(result, mode="RGBA").save(dst_path)

    # Stats for the report.
    unique_idx = np.unique(indices[opaque])
    palette    = sorted(int(v) for v in unique_idx)
    return {"palette": palette}


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    sheets = sorted(f for f in os.listdir(SOURCE_DIR) if f.endswith(".png"))
    print(f"Scale4x upscaling {len(sheets)} sprite sheets")
    print(f"  Source : {SOURCE_DIR}/  (1x)")
    print(f"  Output : {OUTPUT_DIR}/  ({SCALE}x)")
    print()

    for fname in sheets:
        src  = os.path.join(SOURCE_DIR, fname)
        dst  = os.path.join(OUTPUT_DIR,  fname)
        info = upscale_sheet(src, dst)
        pal  = ", ".join(str(v) for v in info["palette"])
        print(f"  {fname}  —  {len(info['palette'])} palette entries [{pal}]")

    print()
    print(f"Done. HD sheets written to {OUTPUT_DIR}/")


if __name__ == "__main__":
    main()
