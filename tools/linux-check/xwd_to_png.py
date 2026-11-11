"""Converts the X screenshot from obs-xvfb-check.sh (XWD, 24- or 32-bit TrueColor) to PNG.

    python xwd_to_png.py screen.xwd screen.png
"""

import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "reference"))
import imageio_min  # noqa: E402


def read_xwd(path):
    data = open(path, "rb").read()
    fields = struct.unpack(">25I", data[:100])
    (header_size, _version, _fmt, _depth, width, height, _xoff, byte_order, _unit, _bit_order, _pad,
     bpp, bytes_per_line, _visual, red_mask, green_mask, blue_mask, _bits_rgb, _cmap, ncolors) = fields[:20]
    if bpp not in (24, 32):
        raise ValueError("expected 24 or 32 bits per pixel, got %d" % bpp)
    start = header_size + ncolors * 12
    raw = np.frombuffer(data, dtype=np.uint8, count=height * bytes_per_line, offset=start)
    px = raw.reshape(height, bytes_per_line)[:, :width * (bpp // 8)].reshape(height, width, bpp // 8)
    if byte_order != 0:  # MSB first
        px = px[..., ::-1]
    rows = np.zeros((height, width), dtype=np.uint32)
    for i in range(bpp // 8):
        rows |= px[..., i].astype(np.uint32) << (8 * i)

    def channel(mask):
        shift = (mask & -mask).bit_length() - 1
        return ((rows & mask) >> shift).astype(np.uint8)

    rgba = np.stack([channel(red_mask), channel(green_mask), channel(blue_mask),
                     np.full(rows.shape, 255, dtype=np.uint8)], axis=-1)
    return rgba


if __name__ == "__main__":
    imageio_min.write_png(sys.argv[2], read_xwd(sys.argv[1]))
