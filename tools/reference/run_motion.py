"""Checks the motion settings over time (docs/SPEC.md, Motion; Phase 3's acceptance).

    python run_motion.py --build BUILD_DIR [--obs OBS_DIR] [--out OUT_DIR] [--renderer d3d11|opengl]
                         [--only SUITE[,SUITE...]] [--flicker-frames N]

It runs tools/harness with the plugin's Record grid (debug builds), which
writes every frame's held and shown glyphs, boil frames left and colors, and
with the harness's frame-sequence source, which plays stills in turn and can
add fresh Gaussian noise to every frame. Suites:

  source   The frame-sequence source hands a filter the same picture as OBS's
           image source (same grid and colors, opaque and alpha stills).
  flicker  A still plus fresh Gaussian noise (sigma 2/255) every frame, 600
           frames, every look: the share of cells that change shown glyph per
           frame, with motion off and at the look's settings. Non-boiling
           looks must change <= 3% of cells per frame at their settings.
  still    Boil 0 on a still: consecutive frames are identical (glyphs and
           colors of every recorded frame, and two consecutive output frames),
           for every look (Static and Fever with Boil 0).
  boil     Boil > 0 on a still: the swap rate (swaps started per second by a
           cell not already swapping) is within 20% of the setting at 30 and 60
           fps; a swap shows one glyph for exactly its linger, then the held
           glyph (or a new swap); the held glyph never changes, even at Hold
           100; the boiled glyph is within the spread, and isn't the held one
           when another glyph is clearly within it (the reference's costs).
           The rate is measured as a Poisson rate, -ln(1 - p) * fps, from the
           share p of free cells that start a swap per frame.
  jump     Two different stills (victor and a crop of sintel, 69% of cells
           change lightness by more than 0.12) alternate every 15 frames, every
           look: each switch is a light jump, and the first frame after it
           matches a fresh pick (a new filter on that still) in 100% of cells,
           with no boil and the fresh colors (Settle restarted). A control
           switch that isn't a light jump (victor to 0.8x victor) isn't a
           fresh pick (Hold keeps glyphs, and Ink's levels ease).
  hold     A switch that isn't a light jump (victor to 0.8x victor and back):
           in the first frame after it, each cell keeps its glyph exactly when
           that glyph's cost on the new picture is within Hold's threshold of
           the best (the reference's costs); at Hold 0 every cell takes the best.
  settle   Ink with Settle, switching between victor and 0.8x victor: every
           frame's colors are c_new + (c_prev - c_new) * settle.

Exit status 0 when every check passes.
"""

import argparse
import json
import os
import subprocess
import sys
import time

import numpy as np

import imageio_min
import reference as ref
import run_looks

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
STILLS = os.path.join(ROOT, "tests", "stills")
VICTOR = os.path.join(STILLS, "victor.png")
NOISE = 2.0          # 8-bit units: sigma = 2/255
FLICKER_LIMIT = 0.03
RATE_TOLERANCE = 0.20
MOTION_OFF = {"soften": 0, "hold": 0, "settle": 0, "boil": 0}
SUITES = ["source", "flicker", "still", "boil", "jump", "hold", "settle"]

ALL_LOOKS = [(mode, lid, values) for mode, looks in run_looks.LOOKS.items() for lid, values in looks]


# ---------------------------------------------------------------------------
# Recordings

def read_recording(path):
    """The frames of a recording, stacked: held, shown, boil (T, cy, cx), colors (T, cy, cx, 6) or None."""
    data = open(path, "rb").read()
    pos = 0
    frames = []
    while pos < len(data):
        hdr = np.frombuffer(data, "<u4", 8, pos)
        pos += 32
        if hdr[0] != 0x46524E47:
            raise ValueError("bad recording %s at %d" % (path, pos))
        cx, cy = int(hdr[2]), int(hdr[3])
        n = cx * cy
        f = dict(frame=int(hdr[1]), jump=bool(hdr[4]), share=float(np.frombuffer(hdr[5:6].tobytes(), "<f4")[0]),
                 mean_l=float(np.frombuffer(hdr[7:8].tobytes(), "<f4")[0]))
        f["held"] = np.frombuffer(data, "<u2", n, pos).reshape(cy, cx).astype(int)
        pos += 2 * n
        f["shown"] = np.frombuffer(data, "<u2", n, pos).reshape(cy, cx).astype(int)
        pos += 2 * n
        f["boil"] = np.frombuffer(data, "u1", n, pos).reshape(cy, cx).astype(int)
        pos += n
        f["colors"] = None
        if hdr[6]:
            f["colors"] = np.frombuffer(data, "<f4", n * 6, pos).reshape(cy, cx, 6).astype(np.float64)
            pos += 4 * n * 6
        frames.append(f)
    rec = {k: np.array([f[k] for f in frames]) for k in ("frame", "jump", "share", "mean_l", "held", "shown", "boil")}
    rec["colors"] = np.array([f["colors"] for f in frames]) if frames and frames[0]["colors"] is not None else None
    return rec


# ---------------------------------------------------------------------------

class Runner:
    def __init__(self, a):
        win = sys.platform == "win32"
        self.a = a
        self.plugin = os.path.join(a.build, "glyphnotic.dll" if win else "glyphnotic.so")
        self.harness_bin = os.path.join(a.build, "glyphnotic-harness.exe" if win else "glyphnotic-harness")
        self.env = dict(os.environ)
        if win:
            self.env["PATH"] = os.path.join(a.obs, "bin", "64bit") + os.pathsep + self.env["PATH"]
        self.failures = []
        self.checks = 0
        self.font = ref.Font()

    def check(self, ok, what, detail=""):
        self.checks += 1
        if not ok:
            self.failures.append(what)
            print("FAIL: %s %s" % (what, detail), flush=True)
        return ok

    def path(self, name):
        return os.path.join(self.a.out, name)

    def harness(self, name, mode, settings, image=VICTOR, also=(), period=0, noise=0.0, sequence=False,
                record=0, colors=False, dump=False, frame_pair=False, fps=30):
        """Runs the harness; returns (recording or None, dump or None), or (None, None) on failure."""
        cmd = [self.harness_bin, "--obs", self.a.obs, "--plugin", self.plugin, "--plugin-data",
               os.path.join(ROOT, "data"), "--image", image, "--filter", "glyphnotic_" + mode,
               "--settings", json.dumps(settings), "--fps", str(fps)]
        for f in also:
            cmd += ["--also", f]
        if period:
            cmd += ["--period", str(period)]
        if noise:
            cmd += ["--noise", str(noise)]
        if sequence:
            cmd += ["--frame-sequence"]
        if dump:
            cmd += ["--dump", self.path(name + ".json")]
        if record:
            cmd += ["--record", self.path(name + ".rec"), "--record-frames", str(record)]
            if colors:
                cmd += ["--record-colors"]
        if frame_pair:
            cmd += ["--frame-pair", self.path(name + "-pair")]
        if self.a.renderer:
            cmd += ["--renderer", self.a.renderer]
        p = subprocess.run(cmd, env=self.env, capture_output=True, text=True)
        leaks = [l for l in p.stderr.splitlines() if l.startswith("memory leaks:")]
        ok = self.check(p.returncode == 0, name + ": harness ran", p.stderr[-2000:])
        self.check(bool(leaks) and leaks[0].strip() == "memory leaks: 0", name + ": no leaks", str(leaks))
        if not ok:
            return None, None
        rec = read_recording(self.path(name + ".rec")) if record else None
        d = json.load(open(self.path(name + ".json"))) if dump else None
        return rec, d

    def reference(self, img, d):
        """The reference run for a still with a dump's settings (fresh pick, the dump's Soften)."""
        return ref.run(img, mode=d["mode"], charset=d["charset_name"], scale=d["cell_scale"],
                       alignment=d["alignment"], font=self.font, keep_transparency=d["keep_transparency"],
                       ink=d.get("ink"), palette=[tuple(c) for c in d["palette"]] if "palette" in d else None,
                       soften=d["motion"]["soften"])


def write_variants(out):
    """victor at 0.8x (not a light jump), and a crop of sintel the size of victor (a light jump)."""
    victor = imageio_min.read_png(VICTOR)
    dark = victor.copy()
    dark[..., :3] = (victor[..., :3].astype(np.float64) * 0.8).round().astype(np.uint8)
    dark_path = os.path.join(out, "victor-0.8.png")
    imageio_min.write_png(dark_path, dark)
    sintel = imageio_min.read_png(os.path.join(STILLS, "sintel.png"))
    crop = np.ascontiguousarray(sintel[:victor.shape[0], :victor.shape[1]])
    crop_path = os.path.join(out, "sintel-crop.png")
    imageio_min.write_png(crop_path, crop)
    return dark_path, crop_path


def no_look(values, **changes):
    """A look's settings without naming the look, so the filter keeps any changes."""
    return dict(values, **changes)


# ---------------------------------------------------------------------------
# Suites

def suite_source(r, rows):
    """The frame-sequence source against OBS's image source."""
    for mode, still in (("mosaic", "victor.png"), ("ink", "victor.png"), ("mosaic", "bbb-alpha.png"),
                        ("console", "bbb-alpha.png")):
        image = os.path.join(STILLS, still)
        settings = dict(MOTION_OFF, charset="ascii")
        name = "source-%s-%s" % (mode, still[:-4])
        _, d1 = r.harness(name + "-image", mode, settings, image=image, dump=True)
        _, d2 = r.harness(name + "-sequence", mode, settings, image=image, sequence=True, dump=True)
        if not (d1 and d2):
            continue
        same = all(d1[k] == d2[k] for k in ("glyph", "held", "text_lab", "bg_lab", "opaque", "mean_lab"))
        r.check(same, "%s: the frame sequence source matches the image source" % name)
        rows.append("| %s | %s | %s |" % (mode, still, "identical" if same else "DIFFERENT"))


def suite_flicker(r, rows):
    frames = r.a.flicker_frames
    for mode, lid, values in ALL_LOOKS:
        result = {}
        for variant, settings in (("off", no_look(values, **MOTION_OFF)), ("look", {"look": lid})):
            name = "flicker-%s-%s-%s" % (mode, lid, variant)
            rec, d = r.harness(name, mode, settings, noise=NOISE, record=frames, dump=True)
            if rec is None:
                continue
            r.check(not rec["jump"].any(), name + ": no light jump under noise")
            changed = (rec["shown"][1:] != rec["shown"][:-1]).mean(axis=(1, 2))
            held = (rec["held"][1:] != rec["held"][:-1]).mean(axis=(1, 2))
            result[variant] = (changed.mean(), changed.max(), held.mean())
            os.remove(r.path(name + ".rec"))
        if len(result) < 2:
            continue
        boiling = values.get("boil", 0) > 0
        off, look = result["off"], result["look"]
        if not boiling:
            r.check(look[0] <= FLICKER_LIMIT, "%s %s: <= 3%% of cells change per frame at the look's settings "
                    "(got %.2f%%)" % (mode, lid, 100 * look[0]))
        rows.append("| %s | %s | %.2f%% (max %.2f%%) | %.2f%% (max %.2f%%) | %s | %s |" % (
            mode, lid, 100 * off[0], 100 * off[1], 100 * look[0], 100 * look[1],
            "%.2f%%" % (100 * look[2]) if boiling else "-",
            "boils (no limit)" if boiling else ("pass" if look[0] <= FLICKER_LIMIT else "FAIL")))


def suite_still(r, rows):
    for mode, lid, values in ALL_LOOKS:
        boiling = values.get("boil", 0) > 0
        settings = no_look(values, boil=0) if boiling else {"look": lid}
        name = "still-%s-%s" % (mode, lid)
        rec, _ = r.harness(name, mode, settings, record=20, colors=True, frame_pair=True)
        if rec is None:
            continue
        same_grid = all((rec[k] == rec[k][0]).all() for k in ("held", "shown", "boil"))
        same_colors = bool((rec["colors"] == rec["colors"][0]).all())
        p0 = imageio_min.read_pam(r.path(name + "-pair-0.pam"))
        p1 = imageio_min.read_pam(r.path(name + "-pair-1.pam"))
        same_frames = bool((p0 == p1).all())
        r.check(same_grid and same_colors, "%s: Boil 0, every recorded frame identical" % name)
        r.check(same_frames, "%s: Boil 0, two consecutive output frames identical" % name)
        rows.append("| %s | %s%s | %s | %s | %s |" % (
            mode, lid, " (Boil 0)" if boiling else "", "identical" if same_grid else "DIFFER",
            "identical" if same_colors else "DIFFER", "identical" if same_frames else "DIFFER"))


BOIL_CASES = [
    # name, mode, settings, fps
    ("ink-static-30", "ink", {"look": "static"}, 30),
    ("ink-static-60", "ink", {"look": "static"}, 60),
    ("ink-fever-30", "ink", {"look": "fever"}, 30),
    ("ink-fever-60", "ink", {"look": "fever"}, 60),
    ("ink-boil100-hold100-30", "ink", no_look(run_looks.LOOKS["ink"][0][1], boil=100, boil_spread=50,
                                              boil_linger=1, hold=100), 30),
    ("mosaic-boil30-linger12-hold100-30", "mosaic", no_look(run_looks.LOOKS["mosaic"][0][1], boil=30,
                                                            boil_spread=100, boil_linger=12, hold=100), 30),
    ("mosaic-boil30-linger12-hold100-60", "mosaic", no_look(run_looks.LOOKS["mosaic"][0][1], boil=30,
                                                            boil_spread=100, boil_linger=12, hold=100), 60),
    ("poster-boil10-30", "poster", no_look(run_looks.LOOKS["poster"][0][1], boil=10, boil_linger=2), 30),
    ("console-boil60-linger4-60", "console", no_look(run_looks.LOOKS["console"][0][1], boil=60, boil_spread=20,
                                                      boil_linger=4), 60),
    ("ink-every-boil25-spread0-30", "ink", no_look(run_looks.LOOKS["ink"][4][1], boil=25, boil_spread=0,
                                                   boil_linger=3), 30),
]


def suite_boil(r, rows):
    img = imageio_min.read_png(VICTOR)
    for name, mode, settings, fps in BOIL_CASES:
        frames = 10 * fps
        rec, d = r.harness("boil-" + name, mode, settings, record=frames, dump=True, fps=fps)
        if rec is None:
            continue
        m = d["motion"]
        boil, linger, spread = m["boil"], m["boil_linger"], m["boil_spread"] / 200.0
        z, shown, held = rec["boil"], rec["shown"], rec["held"]

        # The swap rate of a cell that isn't swapping, as a Poisson rate.
        free = z[:-1] <= 1
        starts = (z[1:] == linger) & free
        rate = -np.log(1.0 - starts.sum() / free.sum()) * fps
        want = boil / 100.0 * 4.0
        err = rate / want - 1.0
        r.check(abs(err) <= RATE_TOLERANCE, "boil %s: rate %.3f/s within 20%% of %.3f/s" % (name, rate, want))

        # Each swap: one glyph for exactly `linger` frames, counting down, then
        # the held glyph or a new swap.
        bad_linger = 0
        complete = 0
        t_idx, y_idx, x_idx = np.nonzero(starts)
        for t, y, x in zip(t_idx + 1, y_idx, x_idx):
            if t + linger >= len(z):
                continue
            complete += 1
            run = z[t:t + linger, y, x]
            ok = (run == np.arange(linger, 0, -1)).all() and (shown[t:t + linger, y, x] == shown[t, y, x]).all()
            after = z[t + linger, y, x]
            ok = ok and ((after == 0 and shown[t + linger, y, x] == held[t + linger, y, x]) or after == linger)
            bad_linger += not ok
        r.check(bad_linger == 0, "boil %s: every swap lasts its linger and returns (%d of %d wrong)" % (
            name, bad_linger, complete))
        not_boiling = z == 0
        r.check(bool((shown[not_boiling] == held[not_boiling]).all()), "boil %s: a cell not swapping shows its held glyph" % name)
        r.check(bool((held == held[0]).all()), "boil %s: the held glyphs never change (Hold %d)" % (name, m["hold"]))

        # The boiled glyphs: not the held one, within the spread of the reference's costs.
        rr = r.reference(img, d)
        costs = rr["costs"]
        best = costs.min(-1)
        worst = costs.max(-1)
        limit = best + spread * (worst - best)
        tol = 1e-6 + 1e-6 * np.abs(worst - best)
        tt, yy, xx = np.nonzero(z[1:] == linger)
        tt = tt + 1
        g = shown[tt, yy, xx]
        h = held[tt, yy, xx]
        within = costs[yy, xx, g] <= limit[yy, xx] + tol[yy, xx]
        same = g == h
        # A swap may show the held glyph only when no other glyph is within the
        # spread: flag it only where another glyph is clearly inside.
        margin = np.maximum(1e-7, 0.01 * (limit - best))
        inside = costs[yy, xx] < (limit - margin)[yy, xx, None]
        inside[np.arange(len(h)), h] = False
        same_bad = same & inside.any(-1)
        r.check(bool(within.all()), "boil %s: boiled glyphs within the spread (%d outside)" % (name, (~within).sum()))
        r.check(not same_bad.any(), "boil %s: a swap shows another glyph when there is one (%d not)" % (
            name, same_bad.sum()))
        visible = (~same).mean() if len(same) else 0.0
        rows.append("| %s | %d | %d | %d | %d | %.3f | %.3f | %+.1f%% | %d | %.0f%% | %s |" % (
            name, fps, boil, linger, m["hold"], want, rate, 100 * err, complete, 100 * visible,
            "pass" if not r.failures or all(not f.startswith("boil " + name + ":") for f in r.failures) else "FAIL"))


def suite_jump(r, rows, variants):
    dark, crop = variants
    period = 15
    for mode, lid, values in ALL_LOOKS:
        name = "jump-%s-%s" % (mode, lid)
        rec, _ = r.harness(name, mode, {"look": lid}, also=[crop], period=period, record=6 * period, colors=True)
        fresh = {}
        for key, image in (("A", VICTOR), ("B", crop)):
            fr, _ = r.harness("%s-fresh%s" % (name, key), mode, no_look(values, boil=0), image=image, sequence=True,
                              record=2, colors=True)
            if fr is not None:
                fresh[key] = fr
        if rec is None or len(fresh) < 2:
            continue
        jumps = np.nonzero(rec["jump"])[0]
        r.check(len(jumps) >= 4, "%s: a light jump at every switch (%d)" % (name, len(jumps)))
        r.check(bool((np.diff(jumps) == period).all()), "%s: light jumps exactly %d frames apart" % (name, period))
        matched = []
        worst_color = 0.0
        for t in jumps:
            best_key, best_match = None, -1.0
            for key, fr in fresh.items():
                match = (rec["held"][t] == fr["held"][0]).mean()
                if match > best_match:
                    best_key, best_match = key, match
            fr = fresh[best_key]
            matched.append((best_key, best_match))
            shown_ok = (rec["shown"][t] == fr["held"][0]).all() and (rec["boil"][t] == 0).all()
            cdiff = float(np.abs(rec["colors"][t] - fr["colors"][0]).max())
            worst_color = max(worst_color, cdiff)
            r.check(best_match == 1.0 and shown_ok, "%s: frame after the switch at %d matches a fresh pick of %s "
                    "(%.4f%%)" % (name, t, best_key, 100 * best_match))
        r.check(worst_color <= 1e-6, "%s: colors after each switch are the fresh colors (max diff %.2g)" % (
            name, worst_color))
        keys = [k for k, _ in matched]
        r.check(all(keys[i] != keys[i + 1] for i in range(len(keys) - 1)), "%s: the switches alternate" % name)
        # Between switches every held glyph is one of the two fresh picks.
        between = all(any((rec["held"][t] == fr["held"][0]).all() for fr in fresh.values()) for t in range(len(rec["held"])))
        r.check(between, "%s: every frame's held glyphs are a fresh pick of one still" % name)
        rows.append("| %s | %s | %d | %s | %.2f%% | %.1g |" % (
            mode, lid, len(jumps), "".join(keys), 100 * min(m for _, m in matched) if matched else 0, worst_color))

    # Control: a switch that isn't a light jump keeps held glyphs (Hold 25).
    name = "jump-control-ink-black"
    rec, _ = r.harness(name, "ink", {"look": "black"}, also=[dark], period=period, record=3 * period)
    fr, _ = r.harness(name + "-freshB", "ink", {"look": "black"}, image=dark, sequence=True, record=1)
    if rec is not None and fr is not None:
        r.check(not rec["jump"].any(), name + ": victor to 0.8x victor is not a light jump")
        # The first frame on the darker still, found by the frame's mean lightness.
        dark_frames = np.nonzero(rec["mean_l"] < fr["mean_l"][0] + 1e-4)[0]
        starts = [t for t in dark_frames if t > 0 and rec["mean_l"][t - 1] > fr["mean_l"][0] + 1e-4]
        kept = float((rec["held"][starts[0]] != fr["held"][0]).mean()) if starts else None
        r.check(kept is not None and kept > 0, name + ": without a light jump, the frame isn't a fresh pick")
        rows.append("| control: ink black, victor to 0.8x victor | | 0 | | %.2f%% of cells differ from a fresh pick | |" % (
            100 * (kept or 0)))


HOLD_CASES = [
    ("mosaic-hold100", "mosaic", no_look(run_looks.LOOKS["mosaic"][0][1], hold=100)),
    ("mosaic-hold30", "mosaic", no_look(run_looks.LOOKS["mosaic"][0][1], hold=30)),
    ("mosaic-hold0", "mosaic", no_look(run_looks.LOOKS["mosaic"][0][1], hold=0)),
    ("poster-hold50", "poster", no_look(run_looks.LOOKS["poster"][0][1], hold=50)),
    ("console-hold100", "console", no_look(run_looks.LOOKS["console"][0][1], hold=100)),
    ("ink-manual-hold100", "ink", no_look(run_looks.LOOKS["ink"][0][1], hold=100, ink_levels="manual",
                                          black_point=0.1, white_point=0.8)),
    ("ink-manual-hold25", "ink", no_look(run_looks.LOOKS["ink"][0][1], ink_levels="manual", black_point=0.1,
                                         white_point=0.8)),
]


def suite_hold(r, rows, variants):
    dark, _ = variants
    period = 20
    imgs = {VICTOR: imageio_min.read_png(VICTOR), dark: imageio_min.read_png(dark)}
    for name, mode, settings in HOLD_CASES:
        rec, d = r.harness("hold-" + name, mode, settings, also=[dark], period=period, record=3 * period, dump=True)
        if rec is None:
            continue
        thr = d["motion"]["hold_threshold"]
        refs = {k: r.reference(img, d) for k, img in imgs.items()}
        # Which still each frame shows, from the frame's mean lightness.
        means = {k: rr["mean_lab"][..., 0].mean() for k, rr in refs.items()}
        on = [min(means, key=lambda k: abs(means[k] - m)) for m in rec["mean_l"]]
        r.check(all(min(abs(means[k] - m) for k in means) < 1e-4 for m in rec["mean_l"]),
                "hold %s: every frame shows one of the stills" % name)
        changes = [t for t in range(1, len(on)) if on[t] != on[t - 1]]
        r.check(len(changes) >= 2, "hold %s: the stills switch (%d)" % (name, len(changes)))
        total = wrong = ambiguous = kept = 0
        for t in changes:
            before = rec["held"][t - 1]
            k = on[t]
            costs = refs[k]["costs"]
            best = costs.min(-1)
            gap = np.take_along_axis(costs, before[..., None], -1)[..., 0] - best
            keep = gap <= thr if thr > 0 else (before == refs[k]["best"])
            want = np.where(keep, before, refs[k]["best"])
            got = rec["held"][t]
            near = np.abs(gap - thr) < 1e-6 if thr > 0 else np.zeros_like(keep)
            # A cell whose best glyph is a near-tie in the reference may take the other one.
            alt_ok = np.take_along_axis(costs, got[..., None], -1)[..., 0] - best < 1e-6
            bad = (got != want) & ~near & ~((want == refs[k]["best"]) & alt_ok)
            total += got.size
            wrong += int(bad.sum())
            ambiguous += int(near.sum())
            kept += int((keep & (before != refs[k]["best"])).sum())
            # And the glyphs then stay: the still doesn't change until the next switch.
            stay = rec["held"][t:t + period - 1]
            r.check(bool((stay == got).all()), "hold %s: glyphs stay after the switch at %d" % (name, t))
        r.check(wrong == 0, "hold %s: kept exactly the glyphs within the threshold (%d cells wrong)" % (name, wrong))
        rows.append("| %s | %g | %d | %d | %d | %d | %s |" % (
            name, thr, len(changes), kept, ambiguous, wrong, "pass" if wrong == 0 else "FAIL"))


def suite_settle(r, rows, variants):
    dark, _ = variants
    period = 10
    tint = run_looks.LOOKS["ink"][1][1]
    for settle in (50, 95):
        name = "settle-ink-tint-%d" % settle
        rec, _ = r.harness(name, "ink", no_look(tint, settle=settle), also=[dark], period=period, record=4 * period,
                           colors=True)
        fresh = []
        for i, image in enumerate((VICTOR, dark)):
            fr, _ = r.harness("%s-fresh%d" % (name, i), "ink", no_look(tint, settle=0), image=image, sequence=True,
                              record=1, colors=True)
            if fr is not None:
                fresh.append(fr["colors"][0])
        if rec is None or len(fresh) < 2:
            continue
        s = settle / 100.0
        c = rec["colors"]
        worst = 0.0
        moving = 0
        for t in range(1, len(c)):
            errs = [np.abs(c[t] - (f + (c[t - 1] - f) * s)).max() for f in fresh]
            worst = max(worst, min(errs))
            moving += int(min(np.abs(c[t] - f).max() for f in fresh) > 1e-4)
        r.check(worst <= 1e-5, "%s: colors follow c_new + (c_prev - c_new) * %.2f (max error %.2g)" % (name, s, worst))
        r.check(moving > 0, "%s: colors are still settling after a switch" % name)
        rows.append("| %d%% | %d | %d | %.2g | %s |" % (settle, len(c), moving, worst, "pass" if worst <= 1e-5 else "FAIL"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    win = sys.platform == "win32"
    ap.add_argument("--obs", default=r"C:\Program Files\obs-studio" if win else "/usr")
    ap.add_argument("--out", default=os.path.join(ROOT, "build_motion"))
    ap.add_argument("--renderer")
    ap.add_argument("--only", help="comma-separated suites: " + ", ".join(SUITES))
    ap.add_argument("--flicker-frames", type=int, default=600)
    a = ap.parse_args()
    a.out = os.path.abspath(a.out)
    a.build = os.path.abspath(a.build)
    os.makedirs(a.out, exist_ok=True)
    suites = a.only.split(",") if a.only else SUITES
    r = Runner(a)
    variants = write_variants(a.out)
    t0 = time.time()

    tables = []
    if "source" in suites:
        rows = []
        suite_source(r, rows)
        tables.append(("Frame sequence source against OBS's image source", "| Filter | Still | Grid and colors |",
                       rows))
    if "flicker" in suites:
        rows = []
        suite_flicker(r, rows)
        tables.append(("Flicker: victor + fresh noise (sigma 2/255), %d frames, cells changing shown glyph per frame"
                       % a.flicker_frames,
                       "| Filter | Look | Motion off | Look's settings | Held changes | Result |", rows))
    if "still" in suites:
        rows = []
        suite_still(r, rows)
        tables.append(("Boil 0 on a still: 20 recorded frames and two output frames",
                       "| Filter | Look | Glyphs | Colors | Output frames |", rows))
    if "boil" in suites:
        rows = []
        suite_boil(r, rows)
        tables.append(("Boil on a still (victor), 10 s each",
                       "| Case | fps | Boil | Linger | Hold | Setting (/s) | Measured (/s) | Error | Swaps checked | "
                       "Swaps showing another glyph | Result |",
                       rows))
    if "jump" in suites:
        rows = []
        suite_jump(r, rows, variants)
        tables.append(("Light jump: victor and a sintel crop alternating every 15 frames (A = victor, B = sintel)",
                       "| Filter | Look | Jumps | Stills | Lowest match | Max color diff |", rows))
    if "hold" in suites:
        rows = []
        suite_hold(r, rows, variants)
        tables.append(("Hold: switching victor and 0.8x victor every 20 frames (no light jump)",
                       "| Case | Threshold | Switches | Cells kept | Cells at the threshold | Wrong | Result |", rows))
    if "settle" in suites:
        rows = []
        suite_settle(r, rows, variants)
        tables.append(("Settle: Ink Tint switching victor and 0.8x victor every 10 frames",
                       "| Settle | Frames | Frames still settling | Max error | Result |", rows))

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
    return 0 if not r.failures else 1


if __name__ == "__main__":
    sys.exit(main())
