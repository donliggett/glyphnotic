"""Phase 4 tuning helpers: render a still the way the plugin draws it (a fresh
pick, from the reference), crop it, lay crops out in contact sheets, and
measure how much a picture changes between two settings.

The reference matches the plugin in 100% of cells on a fresh pick (Phases
1-3), so sweeps of the static settings render here, without OBS. Motion
over time (Hold, Boil, Settle) needs the plugin: see run_tuning.py.
"""

import numpy as np

import reference as ref

INK_KEYS = {"ink_color": "color", "ink_background": "background", "ink_lightness": "lightness",
            "saturation": "saturation", "gamma": "gamma", "shape": "shape", "ink_levels": "levels",
            "black_point": "black_point", "white_point": "white_point"}

_FONT = None


def font():
    global _FONT
    if _FONT is None:
        _FONT = ref.Font()
    return _FONT


def run(img, mode, settings):
    """The reference's fresh pick for a filter's settings (setting keys as in scene files)."""
    ink = {INK_KEYS[k]: v for k, v in settings.items() if k in INK_KEYS}
    palette = settings.get("palette_rgb")
    return ref.run(img, mode=mode, charset=settings.get("charset", "ascii"), scale=settings.get("cell_scale", 1),
                   font=font(), ink=ink, palette=palette, soften=settings.get("soften", 0.0))


def picture(r, settings, glyph=None):
    """The output frame the plugin draws for a reference run, as uint8 RGB (edges black)."""
    g = r["best"] if glyph is None else glyph
    text, back = ref.colors_for(r, g)
    t = ref.oklab_to_srgb255(text)
    b = ref.oklab_to_srgb255(back)
    if r["mode"] == "poster":
        levels = settings.get("poster_levels", 6)
        t = ref.poster_snap(t, levels)
        b = ref.poster_snap(b, levels)
    t, b = np.rint(t), np.rint(b)
    grid = r["grid"]
    cy, cx, s = grid["cells_y"], grid["cells_x"], grid["scale"]
    bits = r["glyphs"][g].reshape(cy, cx, 16, 8).repeat(s, 2).repeat(s, 3)
    col = np.where(bits[..., None] > 0.5, t[:, :, None, None, :], b[:, :, None, None, :])
    opaque = ref.drawn_opaque(r["mean_alpha"], True)
    col = np.where(opaque[:, :, None, None, None], col, 0.0)
    return col.transpose(0, 2, 1, 3, 4).reshape(cy * 16 * s, cx * 8 * s, 3).astype(np.uint8)


def crop(img, box):
    x, y, w, h = box
    return img[y:y + h, x:x + w]


def sheet(rows, labels=None, gap=6, bg=40):
    """Lays out a list of rows of equally sized RGB images."""
    h, w = rows[0][0].shape[:2]
    ncol = max(len(r) for r in rows)
    out = np.full((len(rows) * (h + gap) + gap, ncol * (w + gap) + gap, 3), bg, np.uint8)
    for j, row in enumerate(rows):
        for i, im in enumerate(row):
            y, x = gap + j * (h + gap), gap + i * (w + gap)
            out[y:y + h, x:x + w] = im[..., :3]
    return out


def change(r1, r2):
    """How much two pictures of the same still differ: (share of cells whose glyph differs,
    mean OKLab distance of the cell's mean drawn color)."""
    glyph = float((r1["best"] != r2["best"]).mean())
    return glyph, float(np.linalg.norm(drawn_mean(r1) - drawn_mean(r2), axis=-1).mean())


def drawn_mean(r):
    """The mean OKLab color each cell draws (text and background weighted by the glyph's pixels)."""
    text, back = ref.colors_for(r, r["best"])
    lit = r["glyphs"][r["best"]].mean(-1)[..., None]
    return lit * text + (1 - lit) * back


def second_gap(costs):
    """Per cell, (second best - best) / (worst - best): where the next glyph sits in the cost range."""
    s = np.sort(costs, axis=-1)
    rng = s[..., -1] - s[..., 0]
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(rng > 0, (s[..., 1] - s[..., 0]) / rng, np.nan)
