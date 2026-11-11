"""Compare a plugin grid dump with the reference implementation.

    python compare.py STILL.png DUMP.json [--frame FRAME.pam] [--custom TEXT] [--json]

Works for every mode; the dump carries the mode and its settings.

Checks (docs/SPEC.md, Phases 1 and 2):
  - the character set and grid geometry match;
  - Ink with Auto levels: the plugin's lo/hi match the reference's within 1e-5;
  - glyphs: >= 99% of cells match the reference, and every mismatch has a
    cost gap (reference cost of the plugin's glyph minus the reference's best)
    under 1e-4;
  - colors: the dump's text/background colors against the reference's colors
    for the same glyph, in sRGB 0-255 units;
  - known colors (Mosaic and Poster): every cell whose source pixels are all
    one color (a flat cell) must come out as that color within 1/255;
  - opacity per cell (either way for a cell whose mean alpha is within 1e-6
    of the 0.5 threshold);
  - with --frame, the output frame OBS produced matches the picture the dump
    describes (glyph bitmaps, colors, edges; Poster's snapped colors)
    within 1/255 per channel.

Soften comes from the dump (the reference blurs as the plugin does). With
Boil on, the shown glyph of a boiling cell differs from the image-driven
pick, so the held glyphs are compared instead, the colors are those of the
shown glyphs, and the frame (taken a few frames after the dump, when other
cells boil) isn't checked.

Exit status 0 when every check passes.
"""

import argparse
import json
import sys

import numpy as np

import imageio_min
import reference as ref

GAP_LIMIT = 1e-4
MATCH_MIN = 0.99
COLOR_LIMIT = 1.0  # sRGB, 0-255 units
LEVELS_LIMIT = 1e-5
POSTER_AMBIGUOUS = 1e-3  # a snap this close to a rounding boundary may go either way


def cell_colors_8bit(dump, key):
    """A cell color as drawn, in 8-bit units, and an alternative where Poster's
    snap is too close to a rounding boundary to call (else the same value)."""
    v = np.array(dump[key])
    levels = dump.get("poster_levels")
    if not levels:
        r = np.rint(v)
        return r, r
    step = 255.0 / (levels - 1)
    x = v / step
    snapped = np.round(x) * step
    frac = x - np.floor(x)
    near = np.abs(frac - 0.5) < POSTER_AMBIGUOUS
    other = np.where(np.round(x) > x, np.floor(x), np.ceil(x)) * step
    return np.rint(snapped), np.rint(np.where(near, other, snapped))


def expected_frame(dump, glyph_bits, width, height, alt=False):
    """The picture the dump describes, as uint8 RGBA. alt: Poster's other snap where ambiguous."""
    cx, cy, s = dump["cells_x"], dump["cells_y"], dump["cell_scale"]
    ox, oy = dump["offset_x"], dump["offset_y"]
    edge = (0, 0, 0, 0) if dump["edges"] == "transparent" else (0, 0, 0, 255)
    out = np.empty((height, width, 4), dtype=np.uint8)
    out[...] = edge
    glyph = np.array(dump["glyph"]).reshape(cy, cx).astype(int)
    text = cell_colors_8bit(dump, "text_srgb")[1 if alt else 0].reshape(cy, cx, 3)
    back = cell_colors_8bit(dump, "bg_srgb")[1 if alt else 0].reshape(cy, cx, 3)
    opaque = np.array(dump["opaque"]).reshape(cy, cx) > 0.5
    bits = glyph_bits[glyph].reshape(cy, cx, 16, 8)            # per cell bitmap
    bits = bits.repeat(s, axis=2).repeat(s, axis=3)           # nearest-neighbor at scale s
    color = np.where(bits[..., None] > 0.5, text[:, :, None, None, :], back[:, :, None, None, :])
    rgba = np.concatenate([color, np.full(color.shape[:-1] + (1,), 255.0)], axis=-1)
    rgba = np.where(opaque[:, :, None, None, None], rgba, 0.0)
    block = rgba.transpose(0, 2, 1, 3, 4).reshape(cy * 16 * s, cx * 8 * s, 4)
    out[oy:oy + block.shape[0], ox:ox + block.shape[1]] = block.astype(np.uint8)
    return out


def flat_cells(img, grid, margin=0):
    """For each cell, (is_flat, rgb) over its source pixels and `margin` pixels
    around them (Soften's radius: what the blur reads), within the image."""
    s, cx, cy = grid["scale"], grid["cells_x"], grid["cells_y"]
    ox, oy = grid["offset_x"], grid["offset_y"]
    im = img.astype(np.int32)
    if margin:
        im = np.pad(im, ((margin, margin), (margin, margin), (0, 0)), mode="edge")
    flat = np.zeros((cy, cx), dtype=bool)
    rgb = np.zeros((cy, cx, 3))
    for j in range(cy):
        for i in range(cx):
            y0, x0 = oy + j * 16 * s, ox + i * 8 * s
            block = im[y0:y0 + 16 * s + 2 * margin, x0:x0 + 8 * s + 2 * margin].reshape(-1, 4)
            first = block[0]
            flat[j, i] = (block == first).all() and first[3] == 255
            rgb[j, i] = first[:3]
    return flat, rgb


def compare(still_path, dump_path, frame_path=None, custom=""):
    img = imageio_min.read_png(still_path)
    dump = json.load(open(dump_path))
    font = ref.Font()
    charset = dump["charset_name"]
    mode = dump.get("mode", "mosaic")
    motion = dump.get("motion", {})
    soften = motion.get("soften", 0.0)
    boiling = motion.get("boil", 0) > 0
    r = ref.run(img, mode=mode, charset=charset, custom=custom, scale=dump["cell_scale"],
                alignment=dump["alignment"], font=font, keep_transparency=dump["keep_transparency"],
                ink=dump.get("ink"), palette=[tuple(c) for c in dump["palette"]] if "palette" in dump else None,
                soften=soften)
    grid = r["grid"]
    res = {"still": still_path, "dump": dump_path, "mode": mode, "checks": {}}
    checks = res["checks"]

    checks["charset"] = dump["charset"] == r["chars"]
    checks["geometry"] = all(dump[k] == grid[k] for k in ("cells_x", "cells_y", "offset_x", "offset_y"))
    if not (checks["charset"] and checks["geometry"]):
        res["error"] = "charset or geometry differs: dump %s, reference %s" % (
            {k: dump[k] for k in ("cells_x", "cells_y", "offset_x", "offset_y")}, grid)
        res["pass"] = False
        return res

    if mode == "ink":
        lo, hi = r["levels"][0], r["levels"][1]
        res["levels"] = [float(lo), float(hi)]
        res["levels_error"] = float(max(abs(dump["levels"][0] - lo), abs(dump["levels"][1] - hi)))
        checks["levels"] = res["levels_error"] < LEVELS_LIMIT

    cy, cx = grid["cells_y"], grid["cells_x"]
    shown = np.array(dump["glyph"]).reshape(cy, cx).astype(int)
    plugin = np.array(dump["held"]).reshape(cy, cx).astype(int) if boiling else shown
    best = r["best"]
    costs = r["costs"]
    match = plugin == best
    gap = np.take_along_axis(costs, plugin[..., None], -1)[..., 0] - np.take_along_axis(costs, best[..., None], -1)[..., 0]
    res["cells"] = int(cx * cy)
    res["glyphs"] = len(r["chars"])
    res["match"] = float(match.mean())
    res["mismatches"] = int((~match).sum())
    res["max_gap"] = float(gap[~match].max()) if (~match).any() else 0.0
    checks["glyph_match"] = res["match"] >= MATCH_MIN
    checks["gap"] = res["max_gap"] < GAP_LIMIT

    # Colors for the plugin's own glyph, so color errors aren't mixed up with glyph mismatches.
    text, back = ref.colors_for(r, shown)
    d_text = np.array(dump["text_srgb"]).reshape(cy, cx, 3)
    d_back = np.array(dump["bg_srgb"]).reshape(cy, cx, 3)
    color_err = max(np.abs(d_text - ref.oklab_to_srgb255(text)).max(),
                    np.abs(d_back - ref.oklab_to_srgb255(back)).max())
    res["max_color_error"] = float(color_err)
    checks["colors"] = color_err <= COLOR_LIMIT

    flat, flat_rgb = flat_cells(img, grid, ref.soften_radius(soften))
    res["flat_cells"] = int(flat.sum())
    if flat.any() and mode in ("mosaic", "poster"):
        known_err = max(np.abs(d_text - flat_rgb)[flat].max(), np.abs(d_back - flat_rgb)[flat].max())
        res["max_known_color_error"] = float(known_err)
        checks["known_colors"] = known_err <= COLOR_LIMIT
        checks["flat_glyph_is_space"] = bool((shown[flat] == 0).all())

    keep = dump["keep_transparency"]
    ref_opaque = ~(keep & (r["mean_alpha"] < 0.5))
    d_opaque = np.array(dump["opaque"]).reshape(cy, cx) > 0.5
    # A mean alpha this close to 0.5 is the threshold itself in exact
    # arithmetic (e.g. a straight edge through the cell's middle under a
    # symmetric blur); float rounding may put it on either side.
    at_threshold = keep & (np.abs(r["mean_alpha"] - 0.5) < 1e-6)
    res["opacity_at_threshold"] = int(at_threshold.sum())
    res["opacity_mismatches"] = int(((ref_opaque != d_opaque) & ~at_threshold).sum())
    checks["opacity"] = res["opacity_mismatches"] == 0

    if frame_path and not boiling:
        frame = imageio_min.read_pam(frame_path)
        bits = _charset_bits(font, dump)
        exp = expected_frame(dump, bits, img.shape[1], img.shape[0])
        alt = expected_frame(dump, bits, img.shape[1], img.shape[0], alt=True)
        diff = np.minimum(np.abs(frame.astype(int) - exp.astype(int)).max(axis=-1),
                          np.abs(frame.astype(int) - alt.astype(int)).max(axis=-1))
        res["frame_bad_pixels"] = int((diff > 1).sum())
        res["frame_max_diff"] = int(diff.max())
        checks["frame"] = res["frame_bad_pixels"] == 0

    res["pass"] = all(checks.values())
    return res


def _charset_bits(font, dump):
    rows = []
    for cp in dump["charset"]:
        bm = font.bitmaps[font.glyph_for(cp)]
        rows.append(np.unpackbits(np.frombuffer(bm, dtype=np.uint8)))
    return np.array(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("still")
    ap.add_argument("dump")
    ap.add_argument("--frame")
    ap.add_argument("--custom", default="")
    ap.add_argument("--json", action="store_true", help="print the result as JSON")
    a = ap.parse_args()
    res = compare(a.still, a.dump, a.frame, a.custom)
    if a.json:
        print(json.dumps(res))
    else:
        for k, v in res.items():
            print("%s: %s" % (k, v))
    return 0 if res["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
