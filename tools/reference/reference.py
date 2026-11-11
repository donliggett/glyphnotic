"""Glyphnotic reference implementation: Sample, Cell stats and Score.

Plain Python + NumPy, float64, written from docs/SPEC.md (not from the
plugin's shaders). It handles single stills with motion off and outputs the
glyph index per cell plus the cell colors. It exists only to check the
plugin; it isn't shipped.

Phase 1 covers Mosaic.
"""

import os
import struct

import numpy as np

GLYPH_W, GLYPH_H = 8, 16
GLYPH_PIXELS = GLYPH_W * GLYPH_H

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
FONT_PATH = os.path.join(ROOT, "data", "fonts", "Uni2-Fixed16.psf")


# ---------------------------------------------------------------------------
# Font and character sets (spec: Font and character sets)

class Font:
    def __init__(self, path=FONT_PATH):
        data = open(path, "rb").read()
        if data[:2] != b"\x36\x04":
            raise ValueError("not a PSF1 font")
        mode, height = data[2], data[3]
        if height != GLYPH_H or not (mode & 0x02):
            raise ValueError("expected an 8x16 PSF1 font with a Unicode table")
        count = 512 if mode & 0x01 else 256
        self.bitmaps = [data[4 + i * 16:4 + (i + 1) * 16] for i in range(count)]
        self.cmap = {}
        pos = 4 + count * 16
        for glyph in range(count):
            in_sequence = False
            while True:
                (u,) = struct.unpack("<H", data[pos:pos + 2])
                pos += 2
                if u == 0xFFFF:
                    break
                if u == 0xFFFE:
                    in_sequence = True
                    continue
                if not in_sequence and u not in self.cmap:
                    self.cmap[u] = glyph

    def glyph_for(self, cp):
        return self.cmap.get(cp, -1)


BLOCKS = " █░▒■▮"
LINES = BLOCKS + "─│═║┼├┤┬┴┌┐└┘" \
                 "╭╮╰╯╱╲╳/\\_‗¯·.•●"


def charset_candidates(font, name, custom=""):
    if name == "blocks":
        return [ord(c) for c in BLOCKS]
    if name == "lines":
        return [ord(c) for c in LINES]
    if name == "ascii":
        return [0x20] + list(range(0x21, 0x7F))
    if name == "every":
        return [0x20] + [cp for cp in sorted(font.cmap) if cp > 0x20 and not (0x7F <= cp <= 0x9F)]
    if name == "custom":
        return [0x20] + [ord(c) for c in custom]
    raise ValueError("unknown character set %r" % name)


def build_charset(font, name, custom=""):
    """Returns (code points, bitmaps as a (G, 128) 0/1 float array)."""
    chars, seen, rows = [], set(), []
    for cp in charset_candidates(font, name, custom):
        g = font.glyph_for(cp)
        if g < 0:
            continue
        bm = font.bitmaps[g]
        if bm in seen:
            continue
        seen.add(bm)
        chars.append(cp)
        bits = np.unpackbits(np.frombuffer(bm, dtype=np.uint8))  # MSB first = leftmost
        rows.append(bits.astype(np.float64))
    return chars, np.array(rows)


# ---------------------------------------------------------------------------
# Color (spec: Color)

def srgb_decode(u):
    return np.where(u <= 0.04045, u / 12.92, ((u + 0.055) / 1.055) ** 2.4)


def srgb_encode(u):
    u = np.clip(u, 0.0, 1.0)
    return np.where(u <= 0.0031308, 12.92 * u, 1.055 * u ** (1 / 2.4) - 0.055)


M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
               [0.2119034982, 0.6806995451, 0.1073969566],
               [0.0883024619, 0.2817188376, 0.6299787005]])
M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
               [1.9779984951, -2.4285922050, 0.4505937099],
               [0.0259040371, 0.7827717662, -0.8086757660]])
M2_INV = np.array([[1.0, 0.3963377774, 0.2158037573],
                   [1.0, -0.1055613458, -0.0638541728],
                   [1.0, -0.0894841775, -1.2914855480]])
M1_INV = np.array([[4.0767416621, -3.3077115913, 0.2309699292],
                   [-1.2684380046, 2.6097574011, -0.3413193965],
                   [-0.0041960863, -0.7034186147, 1.7076147010]])


def linear_to_oklab(rgb):
    lms = rgb @ M1.T
    return np.cbrt(np.maximum(lms, 0.0)) @ M2.T


def oklab_to_srgb255(lab):
    lms = (lab @ M2_INV.T) ** 3
    return srgb_encode(lms @ M1_INV.T) * 255.0


# ---------------------------------------------------------------------------
# The grid and the passes

def make_grid(width, height, scale, alignment):
    cw, ch = GLYPH_W * scale, GLYPH_H * scale
    cx, cy = width // cw, height // ch
    if alignment == "center":
        ox, oy = (width - cx * cw) // 2, (height - cy * ch) // 2
    else:
        ox = oy = 0
    return dict(scale=scale, cells_x=cx, cells_y=cy, offset_x=ox, offset_y=oy)


def obs_input(img):
    """Straight sRGB-encoded RGBA in [0, 1] as the filter recovers it from OBS.

    OBS hands filters 8-bit color premultiplied by alpha in encoded space
    (libobs's image loader stores round(c * a / 255)); the Sample pass divides
    alpha back out. Colors of nearly transparent pixels are therefore coarse.
    """
    f = img.astype(np.float64)
    a = f[..., 3:4]
    prem = np.floor(f[..., :3] * (a / 255.0) + 0.5)  # gs_premultiply_xyza: (u8)(c*a*255 + 0.5)
    with np.errstate(invalid="ignore", divide="ignore"):
        straight = np.where((a > 0) & (a < 255), np.clip(prem / a, 0.0, 1.0), f[..., :3] / 255.0)
    return np.concatenate([straight, a / 255.0], axis=-1)


def sample_pass(img, grid):
    """img: uint8 (h, w, 4) straight RGBA. Returns lab (cy, cx, 128, 3), alpha (cy, cx, 128)."""
    s, cx, cy = grid["scale"], grid["cells_x"], grid["cells_y"]
    ox, oy = grid["offset_x"], grid["offset_y"]
    crop = obs_input(img[oy:oy + cy * GLYPH_H * s, ox:ox + cx * GLYPH_W * s])
    lin = srgb_decode(crop[..., :3])
    a = crop[..., 3]
    # Alpha-weighted (premultiplied) mean of each s*s block, in linear RGB.
    prem = (lin * a[..., None]).reshape(cy * GLYPH_H, s, cx * GLYPH_W, s, 3).sum(axis=(1, 3))
    asum = a.reshape(cy * GLYPH_H, s, cx * GLYPH_W, s).sum(axis=(1, 3))
    with np.errstate(invalid="ignore", divide="ignore"):
        lin_s = np.where(asum[..., None] > 0, prem / asum[..., None], 0.0)
    lab = linear_to_oklab(lin_s)
    alpha = asum / (s * s)
    # Rearrange to cells: (cy, 16, cx, 8, ...) -> (cy, cx, 16*8, ...), index ly*8+lx.
    lab = lab.reshape(cy, GLYPH_H, cx, GLYPH_W, 3).transpose(0, 2, 1, 3, 4).reshape(cy, cx, GLYPH_PIXELS, 3)
    alpha = alpha.reshape(cy, GLYPH_H, cx, GLYPH_W).transpose(0, 2, 1, 3).reshape(cy, cx, GLYPH_PIXELS)
    return lab, alpha


def cell_weights(alpha):
    """1 for samples counted in color statistics: alpha >= 0.5 when the cell has any, else all."""
    opaque = alpha >= 0.5
    any_opaque = opaque.any(axis=-1, keepdims=True)
    return np.where(any_opaque, opaque, True).astype(np.float64)


def stats_pass(lab, alpha):
    w = cell_weights(alpha)
    n = np.maximum(w.sum(axis=-1), 1.0)
    mean_lab = (lab * w[..., None]).sum(axis=-2) / n[..., None]
    return mean_lab, alpha.mean(axis=-1)


def mosaic_costs(lab, alpha, glyphs):
    """Cost of every glyph for every cell: (cy, cx, G), spec form up to a per-cell constant.

    cost(g) = -( |S_on|^2 / n_on + |S_off|^2 / n_off ) / 128, a term dropped when its
    count is 0, with S and n over the cell's counted samples. Sums are taken
    relative to the cell's first counted sample; this adds the same constant
    to every glyph's cost in a cell, so winners and cost gaps are unchanged.
    """
    w = cell_weights(alpha)
    first = np.argmax(w > 0, axis=-1)
    shift = np.take_along_axis(lab, first[..., None, None], axis=-2)
    y = (lab - shift) * w[..., None]
    tot = y.sum(axis=-2)                       # (cy, cx, 3)
    ntot = w.sum(axis=-1)                      # (cy, cx)
    bt = glyphs.T                              # (128, G)
    s_on = np.stack([y[..., c] @ bt for c in range(3)], axis=-1)  # (cy, cx, G, 3)
    n_on = w @ bt                              # (cy, cx, G)
    s_off = tot[..., None, :] - s_on
    n_off = ntot[..., None] - n_on
    with np.errstate(invalid="ignore", divide="ignore"):
        fit = np.where(n_on > 0, (s_on ** 2).sum(-1) / n_on, 0.0) + \
              np.where(n_off > 0, (s_off ** 2).sum(-1) / n_off, 0.0)
    return -fit / GLYPH_PIXELS


def mosaic_colors(lab, alpha, glyphs, chosen):
    """Text and background OKLab for the given glyph per cell."""
    w = cell_weights(alpha)
    bits = glyphs[chosen]                      # (cy, cx, 128)
    won = w * bits
    woff = w * (1.0 - bits)
    n_on = won.sum(-1)
    n_off = woff.sum(-1)
    with np.errstate(invalid="ignore", divide="ignore"):
        text = (lab * won[..., None]).sum(-2) / n_on[..., None]
        back = (lab * woff[..., None]).sum(-2) / n_off[..., None]
    text = np.where(n_on[..., None] > 0, text, back)
    back = np.where(n_off[..., None] > 0, back, text)
    return text, back


def run(img, mode="mosaic", charset="blocks", custom="", scale=1, alignment="center", font=None):
    if mode != "mosaic":
        raise NotImplementedError("Phase 1 reference covers Mosaic only")
    font = font or Font()
    chars, glyphs = build_charset(font, charset, custom)
    if len(chars) < 2:
        chars, glyphs = build_charset(font, "blocks")
    grid = make_grid(img.shape[1], img.shape[0], scale, alignment)
    lab, alpha = sample_pass(img, grid)
    mean_lab, mean_alpha = stats_pass(lab, alpha)
    costs = mosaic_costs(lab, alpha, glyphs)
    best = np.argmin(costs, axis=-1)           # first minimum: lowest index wins a tie
    text, back = mosaic_colors(lab, alpha, glyphs, best)
    return dict(grid=grid, chars=chars, glyphs=glyphs, lab=lab, alpha=alpha, mean_lab=mean_lab,
                mean_alpha=mean_alpha, costs=costs, best=best, text=text, back=back)
