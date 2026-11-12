"""Phase 4 tuning measurements (docs/SPEC.md, Build phases: Phase 4).

    python run_tuning.py --build BUILD_DIR [--obs OBS_DIR] [--out OUT_DIR] [--renderer d3d11|opengl]
                         [--only SUITE[,SUITE...]]

Suites:

  hold     Hold over time, per mode (each mode's first look, Hold varied):
           victor plus fresh noise (sigma 2/255) as a still, and panning 1 px
           per frame (a slow sway) and 4 px per frame (a quick move). For each
           it reports the share of cells that change shown glyph per frame,
           and how stale the held glyphs are: the mean of
           (cost(held) - best) / (worst - best) over cells, with the
           reference's costs on the clean (noise-free) frame, so the figure
           means the same in every mode.
  noise    The looks' Hold under heavier noise: every non-boiling look's
           first-look settings at Hold 10 and 25, victor plus fresh noise of
           sigma 2/255 and 4/255 (a noisier camera), 150 frames: the share of
           cells that change glyph per frame.
  boil     Where the next-best glyph sits in a cell's cost range, per mode
           (the reference on victor, spring and sintel): how much of Boil
           spread's range shows another glyph.
  ranges   The static settings' sweeps from the reference (no OBS): Ink's
           Gamma (cells at space or the densest glyphs), Shape (glyph fit
           against tone error), Saturation (drawn chroma), Black and White
           point; Poster's Levels (color change per step); Soften (glyphs
           changed against Soften 0).
  looks    Every look on victor, spring and sintel, drawn by the plugin (one
           output frame each, after 30 frames), cropped for docs/LOOKS.md.

Results print as Markdown tables; the hold suite also writes results.json.
"""

import argparse
import json
import os
import sys
import time

import numpy as np

import imageio_min
import reference as ref
import run_looks
import run_motion
import tuning as T

ROOT = run_motion.ROOT
STILLS = run_motion.STILLS
SUITES = ["hold", "noise", "boil", "ranges", "looks"]
NOISE = 2.0
PAN_WIDTH = 760  # victor is 894 wide: room for 120 px of pan
HOLDS = [0, 10, 25, 50, 75, 100]
MODES = ["ink", "mosaic", "poster", "console"]


def load(name):
    img = imageio_min.read_png(os.path.join(STILLS, name + ".png"))
    return img


def write_pan(out, step, count):
    """count crops of victor, each `step` px right of the last; returns the paths."""
    victor = load("victor")
    paths = []
    folder = os.path.join(out, "pan%d" % step)
    os.makedirs(folder, exist_ok=True)
    for k in range(count):
        p = os.path.join(folder, "%03d.png" % k)
        if not os.path.exists(p):
            imageio_min.write_png(p, np.ascontiguousarray(victor[:, k * step:k * step + PAN_WIDTH]))
        paths.append(p)
    return paths


def first_look(mode):
    return dict(run_looks.LOOKS[mode][0][1])


def staleness(costs, held):
    """Mean of (cost(held) - best) / (worst - best) over cells with a cost range."""
    best = costs.min(-1)
    worst = costs.max(-1)
    rng = worst - best
    h = np.take_along_axis(costs, held[..., None], -1)[..., 0]
    ok = rng > 1e-12
    return float(((h - best)[ok] / rng[ok]).mean())


def suite_hold(r, rows, results):
    pans = {1: write_pan(r.a.out, 1, 60), 4: write_pan(r.a.out, 4, 30)}
    frames = 90
    for mode in MODES:
        base = first_look(mode)
        ref_costs = {}
        for hold in HOLDS:
            s = dict(base, hold=hold)
            row = {"mode": mode, "hold": hold, "threshold": None}
            # Still + noise.
            name = "hold-%s-%d-still" % (mode, hold)
            rec, d = r.harness(name, mode, s, image=pans[1][0], noise=NOISE, record=frames, dump=True)
            if rec is None:
                continue
            row["threshold"] = d["motion"]["hold_threshold"]
            row["still_change"] = float((rec["shown"][1:] != rec["shown"][:-1]).mean())
            key = ("still", 0)
            if key not in ref_costs:
                ref_costs[key] = r.reference(load_frame(pans[1][0]), d)["costs"]
            row["still_stale"] = float(np.mean([staleness(ref_costs[key], h) for h in rec["held"][10:]]))
            os.remove(r.path(name + ".rec"))
            # Pans.
            for step, paths in pans.items():
                name = "hold-%s-%d-pan%d" % (mode, hold, step)
                rec, d = r.harness(name, mode, s, image=paths[0], also=paths[1:], period=1, noise=NOISE,
                                   record=frames, dump=True)
                if rec is None:
                    continue
                idx = match_frames(rec, paths, d, r, ref_costs, step)
                # Leave out the frame after the pan's wrap-around (a jump back).
                keep = [t for t in range(1, frames) if idx[t] != 0]
                row["pan%d_change" % step] = float(np.mean([(rec["shown"][t] != rec["shown"][t - 1]).mean()
                                                           for t in keep]))
                row["pan%d_stale" % step] = float(np.mean([staleness(ref_costs[(step, idx[t])], rec["held"][t])
                                                          for t in keep]))
                os.remove(r.path(name + ".rec"))
            results.append(row)
            rows.append("| %s | %d | %.3g | %.2f%% | %.4f | %.2f%% | %.4f | %.2f%% | %.4f |" % (
                mode, hold, row["threshold"], 100 * row["still_change"], row["still_stale"],
                100 * row.get("pan1_change", np.nan), row.get("pan1_stale", np.nan),
                100 * row.get("pan4_change", np.nan), row.get("pan4_stale", np.nan)))
            print(rows[-1], flush=True)


_FRAMES = {}


def load_frame(path):
    if path not in _FRAMES:
        _FRAMES[path] = imageio_min.read_png(path)
    return _FRAMES[path]


def match_frames(rec, paths, d, r, ref_costs, step):
    """Which pan image each recorded frame shows: the recording's frame counter
    minus a constant offset, found from the frames' mean lightness."""
    n = len(paths)
    means = []
    for k, p in enumerate(paths):
        key = (step, k)
        if key not in ref_costs:
            rr = r.reference(load_frame(p), d)
            ref_costs[key] = rr["costs"]
            ref_costs[("mean", step, k)] = float(rr["mean_lab"][..., 0].mean())
        means.append(ref_costs[("mean", step, k)])
    means = np.array(means)
    counter = rec["frame"]
    best, best_err = 0, np.inf
    for off in range(n):
        err = np.abs(means[(counter - off) % n] - rec["mean_l"]).sum()
        if err < best_err:
            best, best_err = off, err
    return (counter - best) % n


def suite_noise(r, rows):
    for mode, lid, values in run_motion.ALL_LOOKS:
        if values.get("boil", 0):
            continue
        cells = []
        for hold in (10, 25):
            for sigma in (2.0, 4.0):
                name = "noise-%s-%s-%d-%g" % (mode, lid, hold, sigma)
                rec, _ = r.harness(name, mode, dict(values, hold=hold), noise=sigma, record=150)
                if rec is None:
                    cells.append("-")
                    continue
                cells.append("%.2f%%" % (100 * (rec["shown"][1:] != rec["shown"][:-1]).mean()))
                os.remove(r.path(name + ".rec"))
        rows.append("| %s | %s | %d | %s |" % (mode, lid, values["hold"], " | ".join(cells)))
        print(rows[-1], flush=True)


# ---------------------------------------------------------------------------

def suite_boil(r, rows):
    for mode in MODES:
        base = first_look(mode)
        gaps = []
        for still in ("victor", "spring", "sintel"):
            rr = T.run(load(still), mode, base)
            g = T.second_gap(rr["costs"])
            gaps.append(g[np.isfinite(g)])
        g = np.concatenate(gaps)
        q = np.percentile(g, [10, 25, 50, 75, 90])
        within = [float((g <= s).mean()) for s in (0.1, 0.25, 0.5, 0.75, 1.0)]
        rows.append("| %s | %s | %s |" % (mode, " / ".join("%.3f" % x for x in q),
                                          " / ".join("%.0f%%" % (100 * x) for x in within)))
        print(rows[-1], flush=True)


# ---------------------------------------------------------------------------

def suite_ranges(r, tables):
    stills = {k: load(k) for k in ("victor", "spring", "sintel")}
    ink = first_look("ink")

    def ink_tone(s):
        out = []
        for img in stills.values():
            rr = T.run(img, "ink", s)
            n = rr["glyphs"].sum(1)
            lvl = n / n.max()
            b = rr["best"]
            out.append((float((n[b] == 0).mean()), float((lvl[b] >= 0.85).mean())))
        return out

    rows = []
    for v in np.round(np.arange(0.4, 1.81, 0.1), 2):
        t = ink_tone(dict(ink, gamma=float(v)))
        rows.append("| %.1f | %s | %s |" % (v, " / ".join("%.1f%%" % (100 * x[0]) for x in t),
                                            " / ".join("%.1f%%" % (100 * x[1]) for x in t)))
    tables.append(("Ink Gamma: cells drawn as space, and with the densest glyphs (>= 85% of the most lit), "
                   "victor / spring / sintel", "| Gamma | Space | Densest |", rows))

    rows = []
    for v in (0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0):
        fit, tone = [], []
        for img in stills.values():
            rr = T.run(img, "ink", dict(ink, shape=v))
            b = rr["best"]
            bits = rr["glyphs"][b]
            lp = rr["lp"]
            x = bits - bits.mean(-1, keepdims=True)
            y = lp - lp.mean(-1, keepdims=True)
            den = np.sqrt((x * x).sum(-1) * (y * y).sum(-1))
            ok = (y.std(-1) > 0.08) & (den > 0)
            fit.append(float(((x * y).sum(-1)[ok] / den[ok]).mean()))
            n = rr["glyphs"].sum(1)
            tone.append(float(np.abs(n[b] / n.max() * ink["ink_lightness"] - lp.mean(-1)).mean()))
        rows.append("| %.1f | %s | %s |" % (v, " / ".join("%.3f" % x for x in fit),
                                            " / ".join("%.3f" % x for x in tone)))
    tables.append(("Ink Shape: glyph fit (correlation of the glyph with the cell's L', cells with texture) and "
                   "tone error (|level - m|), victor / spring / sintel", "| Shape | Fit | Tone error |", rows))

    rows = []
    prev = None
    for v in (0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5):
        chroma = []
        cur = []
        for img in stills.values():
            rr = T.run(img, "ink", dict(ink, saturation=v))
            text, _ = ref.colors_for(rr, rr["best"])
            srgb = np.clip(ref.oklab_to_srgb255(text), 0, 255) / 255.0
            lab = ref.linear_to_oklab(ref.srgb_decode(srgb))
            cur.append(lab)
            chroma.append(float(np.hypot(lab[..., 1], lab[..., 2]).mean()))
        step = ([float(np.linalg.norm(c - p, axis=-1).mean()) for c, p in zip(cur, prev)] if prev else None)
        prev = cur
        rows.append("| %.2f | %s | %s |" % (v, " / ".join("%.3f" % x for x in chroma),
                                            " / ".join("%.4f" % x for x in step) if step else "-"))
    tables.append(("Ink Saturation: mean OKLab chroma of the drawn ink (after clipping to sRGB) and its change "
                   "from the row above, victor / spring / sintel", "| Saturation | Chroma | Change |", rows))

    rows = []
    for v in (0.0, 0.1, 0.2, 0.3, 0.5, 0.7, 0.8, 0.9, 1.0):
        out = []
        for img in stills.values():
            rr = T.run(img, "ink", dict(ink, ink_levels="manual", black_point=float(v), white_point=1.0))
            b = rr["best"]
            n = rr["glyphs"].sum(1)
            out.append(float((n[b] == 0).mean()))
        out2 = []
        for img in stills.values():
            rr = T.run(img, "ink", dict(ink, ink_levels="manual", black_point=0.0, white_point=float(v)))
            b = rr["best"]
            n = rr["glyphs"].sum(1)
            out2.append(float((n[b] / n.max() >= 0.85).mean()))
        rows.append("| %.1f | %s | %s |" % (v, " / ".join("%.0f%%" % (100 * x) for x in out),
                                            " / ".join("%.0f%%" % (100 * x) for x in out2)))
    tables.append(("Ink Manual levels: cells drawn as space with that Black point (White point 1), and with the "
                   "densest glyphs with that White point (Black point 0), victor / spring / sintel",
                   "| Value | Space (Black point) | Densest (White point) |", rows))

    rows = []
    poster = first_look("poster")
    runs = {k: T.run(img, "poster", poster) for k, img in stills.items()}
    mosaic = {k: T.picture(rr, dict(poster, poster_levels=256)).astype(float) for k, rr in runs.items()}
    prev = None
    for lv in range(2, 17):
        pics = {k: T.picture(rr, dict(poster, poster_levels=lv)).astype(float) for k, rr in runs.items()}
        dist = [float(np.abs(pics[k] - mosaic[k]).mean()) for k in pics]
        step = [float(np.abs(pics[k] - prev[k]).mean()) for k in pics] if prev else None
        ncol = [len(np.unique(pics[k].reshape(-1, 3), axis=0)) for k in pics]
        prev = pics
        rows.append("| %d | %s | %s | %s |" % (lv, " / ".join("%.1f" % x for x in dist),
                                               " / ".join("%.1f" % x for x in step) if step else "-",
                                               " / ".join(str(x) for x in ncol)))
    tables.append(("Poster Levels: mean difference from unsnapped colors and from the row above (8-bit units "
                   "per channel), and colors used, victor / spring / sintel",
                   "| Levels | From unsnapped | Step | Colors |", rows))

    rows = []
    for mode in MODES:
        base = first_look(mode)
        off = {k: T.run(img, mode, dict(base, soften=0.0)) for k, img in stills.items()}
        cells = []
        for v in (0.3, 0.6, 1.0, 1.5, 2.0, 3.0):
            ch = []
            for k, img in stills.items():
                rr = T.run(img, mode, dict(base, soften=v))
                ch.append(float((rr["best"] != off[k]["best"]).mean()))
            cells.append("%.0f%%" % (100 * np.mean(ch)))
        rows.append("| %s | %s |" % (mode, " | ".join(cells)))
    tables.append(("Soften: cells whose glyph differs from Soften 0 (mean over victor, spring, sintel)",
                   "| Filter | 0.3 | 0.6 | 1.0 | 1.5 | 2.0 | 3.0 |", rows))


# ---------------------------------------------------------------------------

CROPS = {"victor": (180, 200, 448, 336), "spring": (500, 40, 448, 336), "sintel": (230, 250, 448, 336)}


def suite_looks(r, rows, dest):
    os.makedirs(dest, exist_ok=True)
    for mode, looks in run_looks.LOOKS.items():
        for lid, _ in looks:
            for still, box in CROPS.items():
                name = "look-%s-%s-%s" % (mode, lid, still)
                frame = r.path(name + ".pam")
                ok = run_frame(r, name, mode, {"look": lid}, os.path.join(STILLS, still + ".png"), frame)
                if not ok:
                    continue
                img = imageio_min.read_pam(frame)
                imageio_min.write_png(os.path.join(dest, "%s-%s-%s.png" % (mode, lid, still)),
                                      np.ascontiguousarray(T.crop(img, box)[..., :3]))
                os.remove(frame)
            rows.append("| %s | %s |" % (mode, lid))
            print(rows[-1], flush=True)


def run_frame(r, name, mode, settings, image, frame):
    import subprocess
    cmd = [r.harness_bin, "--obs", r.a.obs, "--plugin", r.plugin, "--plugin-data", os.path.join(ROOT, "data"),
           "--image", image, "--filter", "glyphnotic_" + mode, "--settings", json.dumps(settings),
           "--fps", "30", "--frames", "30", "--frame", frame]
    if r.a.renderer:
        cmd += ["--renderer", r.a.renderer]
    p = subprocess.run(cmd, env=r.env, capture_output=True, text=True)
    return r.check(p.returncode == 0 and os.path.exists(frame), name + ": harness ran", p.stderr[-2000:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    win = sys.platform == "win32"
    ap.add_argument("--obs", default=r"C:\Program Files\obs-studio" if win else "/usr")
    ap.add_argument("--out", default=os.path.join(ROOT, "build_tuning"))
    ap.add_argument("--renderer")
    ap.add_argument("--only", help="comma-separated suites: " + ", ".join(SUITES))
    ap.add_argument("--looks-dest", default=os.path.join(ROOT, "docs", "looks"))
    a = ap.parse_args()
    a.out = os.path.abspath(a.out)
    a.build = os.path.abspath(a.build)
    os.makedirs(a.out, exist_ok=True)
    suites = a.only.split(",") if a.only else SUITES
    r = run_motion.Runner(a)
    t0 = time.time()
    tables = []
    if "hold" in suites:
        rows, results = [], []
        suite_hold(r, rows, results)
        json.dump(results, open(os.path.join(a.out, "hold.json"), "w"), indent=1)
        tables.append(("Hold: victor + fresh noise (sigma 2/255), still and panning; each mode's first look",
                       "| Filter | Hold | Threshold | Still: changes | Still: staleness | Pan 1 px: changes | "
                       "Pan 1 px: staleness | Pan 4 px: changes | Pan 4 px: staleness |", rows))
    if "noise" in suites:
        rows = []
        suite_noise(r, rows)
        tables.append(("Looks under noise: cells changing glyph per frame (victor, 150 frames)",
                       "| Filter | Look | Look's Hold | Hold 10, sigma 2 | Hold 10, sigma 4 | Hold 25, sigma 2 | "
                       "Hold 25, sigma 4 |", rows))
    if "boil" in suites:
        rows = []
        suite_boil(r, rows)
        tables.append(("Boil spread: the next-best glyph's place in the cell's cost range, (second - best) / "
                       "(worst - best), victor + spring + sintel, each mode's first look",
                       "| Filter | Percentiles 10 / 25 / 50 / 75 / 90 | Cells with another glyph within 0.1 / "
                       "0.25 / 0.5 / 0.75 / 1 of the range |", rows))
    if "ranges" in suites:
        suite_ranges(r, tables)
    if "looks" in suites:
        rows = []
        suite_looks(r, rows, a.looks_dest)
        tables.append(("Looks drawn for docs/LOOKS.md", "| Filter | Look |", rows))

    for title, header, rows in tables:
        print()
        print("### " + title)
        print()
        print(header)
        print("|" + " --- |" * (header.count("|") - 1))
        for row in rows:
            print(row)
    print()
    print("%d checks, %d failures, %.0f s" % (r.checks, len(r.failures), time.time() - t0))
    return 1 if r.failures else 0


if __name__ == "__main__":
    sys.exit(main())
