"""Glyphnotic reference implementation: Soften, Sample, Cell stats, Levels and Score.

Plain Python + NumPy, float64, written from docs/SPEC.md (not from the
plugin's shaders). It handles single stills with the other motion settings
off (Hold, Settle and Boil act over time) and outputs the glyph index per
cell plus the cell colors. It exists only to check the plugin; it isn't
shipped.

It covers all four modes: Mosaic, Poster (Mosaic, colors snapped when
drawn), Ink (Auto and Manual levels) and Console.
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


def soften_radius(sigma):
    """Soften's kernel radius: ceil(3 sigma), 0 when off."""
    return int(np.ceil(3.0 * sigma - 1e-9)) if sigma > 0 else 0


def soften_weights(sigma):
    r = soften_radius(sigma)
    i = np.arange(-r, r + 1, dtype=np.float64)
    w = np.exp(-0.5 * i * i / (sigma * sigma))
    return w / w.sum()


def gaussian_blur(x, sigma):
    """Separable Gaussian blur of an (h, w, c) array, taps beyond an edge taking the edge pixel."""
    r = soften_radius(sigma)
    if r == 0:
        return x
    w = soften_weights(sigma)
    for axis in (1, 0):
        pad = [(0, 0)] * x.ndim
        pad[axis] = (r, r)
        xp = np.pad(x, pad, mode="edge")
        n = x.shape[axis]
        x = sum(w[k] * np.take(xp, np.arange(k, k + n), axis=axis) for k in range(2 * r + 1))
    return x


def obs_input(img, soften=0.0):
    """Straight sRGB-encoded RGBA in [0, 1] as the filter recovers it from OBS.

    OBS hands filters 8-bit color premultiplied by alpha in encoded space
    (libobs's image loader stores round(c * a / 255)); the Sample pass divides
    alpha back out. Colors of nearly transparent pixels are therefore coarse.
    Soften blurs those premultiplied values (and alpha) before alpha is
    divided out.
    """
    f = img.astype(np.float64)
    a = f[..., 3:4]
    prem = np.floor(f[..., :3] * (a / 255.0) + 0.5)  # gs_premultiply_xyza: (u8)(c*a*255 + 0.5)
    if soften > 0:
        b = gaussian_blur(np.concatenate([prem, a], axis=-1) / 255.0, soften)
        prem, a = b[..., :3], b[..., 3:4]
        with np.errstate(invalid="ignore", divide="ignore"):
            straight = np.where((a > 0) & (a < 1), np.clip(prem / a, 0.0, 1.0), prem)
        return np.concatenate([straight, a], axis=-1)
    with np.errstate(invalid="ignore", divide="ignore"):
        straight = np.where((a > 0) & (a < 255), np.clip(prem / a, 0.0, 1.0), f[..., :3] / 255.0)
    return np.concatenate([straight, a / 255.0], axis=-1)


def sample_pass(img, grid, soften=0.0):
    """img: uint8 (h, w, 4) straight RGBA. Returns lab (cy, cx, 128, 3), alpha (cy, cx, 128).

    Soften blurs the whole source, so the cells near the grid's edge see the
    pixels beyond it."""
    s, cx, cy = grid["scale"], grid["cells_x"], grid["cells_y"]
    ox, oy = grid["offset_x"], grid["offset_y"]
    crop = obs_input(img, soften)[oy:oy + cy * GLYPH_H * s, ox:ox + cx * GLYPH_W * s]
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


def poster_snap(srgb255, levels):
    """Poster: each sRGB channel (0-255) to the nearest of `levels` evenly spaced values."""
    step = 255.0 / (levels - 1)
    return np.round(srgb255 / step) * step


# ---------------------------------------------------------------------------
# Ink (spec: Ink: brightness first, shape second)

FIXED_INKS = {"green": (-0.12, 0.10), "amber": (0.034, 0.136), "white": (0.0, 0.0)}

INK_DEFAULTS = dict(color="cell", a=0.0, b=0.0, background="black", lightness=0.92, saturation=1.4,
                    gamma=0.85, shape=0.35, levels="auto", black_point=0.0, white_point=1.0)


def drawn_opaque(mean_alpha, keep_transparency):
    return ~(keep_transparency & (mean_alpha < 0.5))


def auto_levels(mean_lab, mean_alpha, keep_transparency):
    """Auto levels' target: (lo, hi, mu, sigma) over the cells drawn opaque."""
    L = mean_lab[..., 0][drawn_opaque(mean_alpha, keep_transparency)]
    if L.size:
        mu = L.mean()
        sigma = np.sqrt(max((L * L).mean() - mu * mu, 0.0))
    else:
        mu, sigma = 0.5, 0.0
    lo = min(max(mu - 2 * sigma, 0.0), 1.0)
    hi = min(max(mu + 2 * sigma, 0.0), 1.0)
    if hi - lo < 0.05:
        c = 0.5 * (lo + hi)
        lo, hi = c - 0.025, c + 0.025
        if lo < 0.0:
            lo, hi = 0.0, 0.05
        if hi > 1.0:
            lo, hi = 0.95, 1.0
    return lo, hi, mu, sigma


def manual_levels(black_point, white_point):
    return black_point, max(white_point, black_point + 0.05)


def ink_lightness(lab, lo, hi, ink, gamma):
    """L' = ink * clamp((L - lo) / (hi - lo), 0, 1) ^ gamma, per sample."""
    return ink * np.clip((lab[..., 0] - lo) / (hi - lo), 0.0, 1.0) ** gamma


def ink_costs(lp, alpha, glyphs, ink, shape):
    """cost(g) = (level(g) - m)^2 + shape * (n_on ink^2 - 2 ink S_on(L')) / 128.

    m, n_on and S_on use the cell's counted samples; level(g) uses the glyph's
    own lit-pixel count relative to the set's most lit glyph. S_on is summed
    relative to the first counted sample's L' (c), as
    n_on ink (ink - 2c) - 2 ink S_on(L' - c), the same value: a flat cell sums
    exact zeros, so glyphs with equal counts tie exactly.
    """
    w = cell_weights(alpha)
    m = (lp * w).sum(-1) / np.maximum(w.sum(-1), 1.0)
    first = np.argmax(w > 0, axis=-1)
    c = np.take_along_axis(lp, first[..., None], axis=-1)  # (cy, cx, 1)
    n_glyph = glyphs.sum(axis=1)
    level = n_glyph / n_glyph.max() * ink                 # (G,)
    bt = glyphs.T
    s_on = ((lp - c) * w) @ bt                            # (cy, cx, G)
    n_on = w @ bt
    k = ink * (ink - 2.0 * c)
    return (level - m[..., None]) ** 2 + shape * (n_on * k - 2.0 * ink * s_on) / GLYPH_PIXELS


def ink_colors(mean_lab, o):
    """Text (ink, a, b) and background (black or tint) per cell, from the cell's mean OKLab."""
    text = np.empty_like(mean_lab)
    text[..., 0] = o["lightness"]
    if o["color"] == "cell":
        text[..., 1:] = mean_lab[..., 1:] * o["saturation"]
    else:
        a, b = (o["a"], o["b"]) if o["color"] == "custom" else FIXED_INKS[o["color"]]
        text[..., 1] = a
        text[..., 2] = b
    if o["background"] == "tint":
        back = mean_lab.copy()
        back[..., 0] *= 0.6
    else:
        back = np.zeros_like(mean_lab)
    return text, back


# ---------------------------------------------------------------------------
# Console (spec: Console: palette colors)

VGA = [(0x00, 0x00, 0x00), (0x00, 0x00, 0xAA), (0x00, 0xAA, 0x00), (0x00, 0xAA, 0xAA),
       (0xAA, 0x00, 0x00), (0xAA, 0x00, 0xAA), (0xAA, 0x55, 0x00), (0xAA, 0xAA, 0xAA),
       (0x55, 0x55, 0x55), (0x55, 0x55, 0xFF), (0x55, 0xFF, 0x55), (0x55, 0xFF, 0xFF),
       (0xFF, 0x55, 0x55), (0xFF, 0x55, 0xFF), (0xFF, 0xFF, 0x55), (0xFF, 0xFF, 0xFF)]
BACKGROUND_SLOTS = 8


def palette_lab(palette_rgb):
    rgb = np.array(palette_rgb, dtype=np.float64) / 255.0
    return linear_to_oklab(srgb_decode(rgb))


def _shifted(lab, alpha):
    """Counted samples relative to the cell's first counted sample, the weights and that sample."""
    w = cell_weights(alpha)
    first = np.argmax(w > 0, axis=-1)
    shift = np.take_along_axis(lab, first[..., None, None], axis=-2)[..., 0, :]
    return (lab - shift[..., None, :]) * w[..., None], w, shift


def _console_terms(y, w, glyphs, pal, shift):
    """Per glyph: the best text slot, best background slot and the cost, for the cells in y.

    The spec's cost, min_k [n_on |P_k|^2 - 2 S_on.P_k] + min_k<8 [...], computed
    relative to the cell's first counted sample c (P' = P - c, Q = |P'|^2),
    which changes every glyph's cost in the cell by the same amount. A side
    with no counted samples takes the other side's slot. When both sides get
    the same slot k the cost is n Q_k - 2 S.P'_k from the cell's totals, so
    all such glyphs (they draw the same picture) tie exactly.
    """
    bt = glyphs.T
    tot = y.sum(axis=-2)                                   # (..., 3)
    ntot = w.sum(axis=-1)
    s_on = np.stack([y[..., c] @ bt for c in range(3)], axis=-1)   # (..., G, 3)
    n_on = w @ bt                                          # (..., G)
    s_off = tot[..., None, :] - s_on
    n_off = ntot[..., None] - n_on
    pp = pal - shift[..., None, :]                         # (..., 16, 3)
    q = (pp ** 2).sum(-1)                                  # (..., 16)
    vt = n_on[..., None] * q[..., None, :] - 2.0 * np.einsum("...gc,...kc->...gk", s_on, pp)
    vb = (n_off[..., None] * q[..., None, :BACKGROUND_SLOTS] -
          2.0 * np.einsum("...gc,...kc->...gk", s_off, pp[..., :BACKGROUND_SLOTS, :]))
    kt = np.argmin(vt, axis=-1)                            # lowest slot wins a tie
    kb = np.argmin(vb, axis=-1)
    kt = np.where(n_on > 0, kt, kb)                        # a side with no samples takes the other's slot
    kb = np.where(n_off > 0, kb, kt)
    qt = np.take_along_axis(q[..., None, :], kt[..., None], -1)[..., 0]
    qb = np.take_along_axis(q[..., None, :], kb[..., None], -1)[..., 0]
    pt = np.take_along_axis(pp[..., None, :, :], kt[..., None, None], -2)[..., 0, :]
    pb = np.take_along_axis(pp[..., None, :, :], kb[..., None, None], -2)[..., 0, :]
    split = (n_on * qt + n_off * qb - 2.0 * ((s_on * pt).sum(-1) + (s_off * pb).sum(-1))) / GLYPH_PIXELS
    whole = (ntot[..., None] * qb - 2.0 * (tot[..., None, :] * pb).sum(-1)) / GLYPH_PIXELS
    cost = np.where(kt == kb, whole, split)
    return kt, kb, cost, n_on, n_off


def console_costs(lab, alpha, glyphs, pal):
    """Cost of every glyph for every cell: (cy, cx, G). Row by row to bound memory."""
    y, w, shift = _shifted(lab, alpha)
    out = np.empty(lab.shape[:2] + (glyphs.shape[0],))
    for r in range(lab.shape[0]):
        out[r] = _console_terms(y[r], w[r], glyphs, pal, shift[r])[2]
    return out


def console_colors(lab, alpha, glyphs, pal, chosen):
    """Text and background OKLab (palette colors) and slots for the given glyph per cell."""
    y, w, shift = _shifted(lab, alpha)
    g = glyphs[chosen]                                     # (cy, cx, 128): one glyph per cell
    tot = y.sum(axis=-2)
    ntot = w.sum(axis=-1)
    s_on = (y * g[..., None]).sum(axis=-2)
    n_on = (w * g).sum(axis=-1)
    s_off = tot - s_on
    n_off = ntot - n_on
    pp = pal - shift[..., None, :]                         # (cy, cx, 16, 3)
    q = (pp ** 2).sum(-1)
    vt = n_on[..., None] * q - 2.0 * np.einsum("...c,...kc->...k", s_on, pp)
    vb = n_off[..., None] * q[..., :BACKGROUND_SLOTS] - \
        2.0 * np.einsum("...c,...kc->...k", s_off, pp[..., :BACKGROUND_SLOTS, :])
    kt = np.argmin(vt, axis=-1)
    kb = np.argmin(vb, axis=-1)
    kt = np.where(n_on > 0, kt, kb)                        # a side with no samples takes the other's slot
    kb = np.where(n_off > 0, kb, kt)
    return pal[kt], pal[kb], kt, kb


# ---------------------------------------------------------------------------

def run(img, mode="mosaic", charset="blocks", custom="", scale=1, alignment="center", font=None,
        keep_transparency=True, ink=None, palette=None, levels=None, soften=0.0):
    """Runs the reference on a still with Hold, Settle and Boil off (a fresh pick).

    ink: Ink settings (keys of INK_DEFAULTS); palette: Console's 16 sRGB triples (default VGA);
    levels: Ink's (lo, hi) to use instead of the still's own (to check eased levels).
    """
    font = font or Font()
    chars, glyphs = build_charset(font, charset, custom)
    if len(chars) < 2:
        chars, glyphs = build_charset(font, "blocks")
    grid = make_grid(img.shape[1], img.shape[0], scale, alignment)
    lab, alpha = sample_pass(img, grid, soften)
    mean_lab, mean_alpha = stats_pass(lab, alpha)
    r = dict(grid=grid, chars=chars, glyphs=glyphs, lab=lab, alpha=alpha, mean_lab=mean_lab,
             mean_alpha=mean_alpha, mode=mode)
    if mode in ("mosaic", "poster"):
        costs = mosaic_costs(lab, alpha, glyphs)
    elif mode == "ink":
        o = dict(INK_DEFAULTS, **(ink or {}))
        if levels is not None:
            levels = tuple(levels[:2]) + (None, None)
        elif o["levels"] == "auto":
            levels = auto_levels(mean_lab, mean_alpha, keep_transparency)
        else:
            levels = manual_levels(o["black_point"], o["white_point"]) + (None, None)
        r.update(ink=o, levels=levels)
        r["lp"] = ink_lightness(lab, levels[0], levels[1], o["lightness"], o["gamma"])
        costs = ink_costs(r["lp"], alpha, glyphs, o["lightness"], o["shape"])
    elif mode == "console":
        r["palette_rgb"] = palette or VGA
        r["pal"] = palette_lab(r["palette_rgb"])
        costs = console_costs(lab, alpha, glyphs, r["pal"])
    else:
        raise ValueError("unknown mode %r" % mode)
    best = np.argmin(costs, axis=-1)           # first minimum: lowest index wins a tie
    r.update(costs=costs, best=best)
    r["text"], r["back"] = colors_for(r, best)
    return r


def colors_for(r, chosen):
    """Text and background OKLab per cell for the given glyphs (Poster: before snapping)."""
    if r["mode"] in ("mosaic", "poster"):
        return mosaic_colors(r["lab"], r["alpha"], r["glyphs"], chosen)
    if r["mode"] == "ink":
        return ink_colors(r["mean_lab"], r["ink"])
    text, back, _, _ = console_colors(r["lab"], r["alpha"], r["glyphs"], r["pal"], chosen)
    return text, back
