"""Builds tests/stills/*.png.

    python make_stills.py [BMP_DIR]

patches.png and the alpha channel of bbb-alpha.png are generated here. The
photographic stills start as downloads from Wikimedia Commons (see
tests/stills/ATTRIBUTION.md), cropped or resized to 24-bit BMP once with
Windows' System.Drawing (the commands are in ATTRIBUTION.md), and are
re-encoded here as plain 8-bit PNG with no color chunks. Without BMP_DIR only
patches.png is rebuilt.
"""

import os
import sys

import numpy as np

import imageio_min

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "tests", "stills")


def patches():
    """1920x1080 grid of 12x9 flat patches (160x120 px) with known sRGB values."""
    values = []
    for g in (0, 1, 2, 3, 5, 8, 10, 11, 12, 16, 24, 32, 48, 64, 96, 128, 160, 188, 200, 224, 240, 250, 254, 255):
        values.append((g, g, g))
    for c in ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255),
              (128, 0, 0), (0, 128, 0), (0, 0, 128), (255, 176, 0), (64, 32, 16), (16, 32, 64)):
        values.append(c)
    rng = np.random.default_rng(20261111)
    while len(values) < 108:
        values.append(tuple(int(v) for v in rng.integers(0, 256, 3)))
    img = np.zeros((1080, 1920, 3), dtype=np.uint8)
    for i, rgb in enumerate(values):
        r, c = divmod(i, 12)
        img[r * 120:(r + 1) * 120, c * 160:(c + 1) * 160] = rgb
    return img


def alpha_mask(h, w):
    """Opaque disc with a soft edge, a fully transparent notch, and partial-alpha bars."""
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    d = np.hypot(xx - w * 0.45, yy - h * 0.5)
    r = h * 0.42
    a = np.clip((r + 40 - d) / 80.0, 0.0, 1.0)            # 1 inside, ramps to 0 over 80 px
    a[int(h * 0.15):int(h * 0.30), int(w * 0.40):int(w * 0.52)] = 0.0   # a hole
    for i, v in enumerate((0.25, 0.5, 0.75)):             # partial bars on the right
        x0 = int(w * 0.80) + i * 70
        a[int(h * 0.2):int(h * 0.8), x0:x0 + 60] = v
    return np.rint(a * 255).astype(np.uint8)


def main():
    os.makedirs(OUT, exist_ok=True)
    imageio_min.write_png(os.path.join(OUT, "patches.png"), patches())
    if len(sys.argv) > 1:
        src = sys.argv[1]
        for name in ("victor", "spring", "sintel"):
            img = imageio_min.read_bmp(os.path.join(src, name + ".bmp"))
            imageio_min.write_png(os.path.join(OUT, name + ".png"), img[..., :3])
        img = imageio_min.read_bmp(os.path.join(src, "bbb.bmp"))
        img[..., 3] = alpha_mask(img.shape[0], img.shape[1])
        imageio_min.write_png(os.path.join(OUT, "bbb-alpha.png"), img)
    for f in sorted(os.listdir(OUT)):
        if f.endswith(".png"):
            print(f, os.path.getsize(os.path.join(OUT, f)))


if __name__ == "__main__":
    main()
