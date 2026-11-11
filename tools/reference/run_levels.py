"""Checks Ink's levels over time (docs/SPEC.md, Ink, Levels).

    python run_levels.py --build BUILD_DIR [--obs OBS_DIR] [--out OUT_DIR] [--renderer d3d11|opengl]

The harness runs Ink on victor.png, dumps the grid, switches the image to
another still, runs a number of frames at 30 fps and dumps again. Cases:

  ease-1.5s   Auto, Adapt speed 1.5 s, switch to a darkened victor (same size):
              the first dump's levels are victor's target; the second lies on
              the easing curve lo = T2 + (T1 - T2) exp(-n dt / 1.5 s), for the
              number of frames n between the two dumps (allowing up to 8 frames
              for the switch to reach the filter), and strictly between T1 and T2
  ease-0.2s   the same at 0.2 s: after 30 frames the levels are within 1% of T2
  resize      Auto, switch to spring.png (a different size): the levels snap
              to spring's target at once (a source size change resets the grid)
  manual      Manual levels ignore the picture: both dumps use the black and
              white points

In every case the second dump's glyphs match the reference run with the
dump's own lo/hi (>= 99%, gap < 1e-4). Exit status 0 when all pass.
"""

import argparse
import json
import math
import os
import subprocess
import sys

import numpy as np

import imageio_min
import reference as ref

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
STILLS = os.path.join(ROOT, "tests", "stills")
FPS = 30
FRAMES = 30


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    win = sys.platform == "win32"
    ap.add_argument("--obs", default=r"C:\Program Files\obs-studio" if win else "/usr")
    ap.add_argument("--out", default=os.path.join(ROOT, "build_levels"))
    ap.add_argument("--renderer")
    a = ap.parse_args()
    a.out = os.path.abspath(a.out)
    a.build = os.path.abspath(a.build)
    os.makedirs(a.out, exist_ok=True)
    plugin = os.path.join(a.build, "glyphnotic.dll" if win else "glyphnotic.so")
    harness = os.path.join(a.build, "glyphnotic-harness.exe" if win else "glyphnotic-harness")
    env = dict(os.environ)
    if win:
        env["PATH"] = os.path.join(a.obs, "bin", "64bit") + os.pathsep + env["PATH"]

    victor = os.path.join(STILLS, "victor.png")
    img1 = imageio_min.read_png(victor)
    dark = os.path.join(a.out, "victor-dark.png")
    img_dark = img1.copy()
    img_dark[..., :3] = (img1[..., :3].astype(np.float64) * 0.5).round().astype(np.uint8)
    imageio_min.write_png(dark, img_dark)
    spring = os.path.join(STILLS, "spring.png")

    manual = {"ink_levels": "manual", "black_point": 0.2, "white_point": 0.7}
    cases = [("ease-1.5s", dark, {"adapt_speed": 1.5}), ("ease-0.2s", dark, {"adapt_speed": 0.2}),
             ("resize", spring, {}), ("manual", dark, manual)]

    font = ref.Font()
    all_pass = True
    print("| Case | Frames | lo, hi first | lo, hi second | Expected second | Glyph match | Result |")
    print("| --- | --- | --- | --- | --- | --- | --- |")
    for name, second, settings in cases:
        settings = dict({"charset": "ascii"}, **settings)
        d1p, d2p = os.path.join(a.out, name + "-1.json"), os.path.join(a.out, name + "-2.json")
        cmd = [harness, "--obs", a.obs, "--plugin", plugin, "--plugin-data", os.path.join(ROOT, "data"),
               "--image", victor, "--filter", "glyphnotic_ink", "--settings", json.dumps(settings),
               "--fps", str(FPS), "--dump", d1p, "--then-image", second, "--then-frames", str(FRAMES),
               "--dump2", d2p]
        if a.renderer:
            cmd += ["--renderer", a.renderer]
        p = subprocess.run(cmd, env=env, capture_output=True, text=True)
        if p.returncode != 0:
            print("%s: harness failed\n%s" % (name, p.stderr), file=sys.stderr)
            all_pass = False
            continue
        d1, d2 = json.load(open(d1p)), json.load(open(d2p))
        img2 = imageio_min.read_png(second)
        r1 = ref.run(img1, mode="ink", charset="ascii", font=font, ink=d1["ink"])
        r2 = ref.run(img2, mode="ink", charset="ascii", font=font, ink=d2["ink"])
        t1, t2 = np.array(r1["levels"][:2]), np.array(r2["levels"][:2])
        l1, l2 = np.array(d1["levels"][:2]), np.array(d2["levels"][:2])
        n = d2["frame"] - d1["frame"]
        checks = {"first": bool(np.abs(l1 - t1).max() < 1e-5)}
        tau = d1["ink"]["adapt_speed"]
        if name.startswith("ease"):
            # The switch reaches the filter within a few frames of the first dump.
            hi_n, lo_n = n, max(n - 8, 0)
            f_lo, f_hi = math.exp(-hi_n / FPS / tau), math.exp(-lo_n / FPS / tau)
            e1, e2 = t2 + (t1 - t2) * f_lo, t2 + (t1 - t2) * f_hi
            lower, upper = np.minimum(e1, e2) - 1e-5, np.maximum(e1, e2) + 1e-5
            checks["on_curve"] = bool(((l2 >= lower) & (l2 <= upper)).all())
            checks["between"] = bool((np.abs(l2 - t2) < np.abs(t1 - t2)).all())
            expected = "%.4f..%.4f, %.4f..%.4f" % (lower[0], upper[0], lower[1], upper[1])
            if tau < 0.5:
                checks["converged"] = bool((np.abs(l2 - t2) <= 0.01 * np.abs(t1 - t2)).all())
        elif name == "resize":
            checks["snapped"] = bool(np.abs(l2 - t2).max() < 1e-5)
            expected = "%.4f, %.4f" % tuple(t2)
        else:
            want = np.array(ref.manual_levels(0.2, 0.7))
            checks["manual"] = bool(np.abs(l1 - want).max() < 1e-6 and np.abs(l2 - want).max() < 1e-6)
            expected = "%.4f, %.4f" % tuple(want)
        # The second picture, scored with the plugin's own levels.
        rr = ref.run(img2, mode="ink", charset="ascii", font=font, ink=d2["ink"], levels=l2)
        glyph = np.array(d2["glyph"]).reshape(rr["best"].shape).astype(int)
        match = glyph == rr["best"]
        gap = (np.take_along_axis(rr["costs"], glyph[..., None], -1) -
               np.take_along_axis(rr["costs"], rr["best"][..., None], -1))[..., 0]
        checks["glyphs"] = bool(match.mean() >= 0.99 and (gap[~match].max() if (~match).any() else 0) < 1e-4)
        ok = all(checks.values())
        all_pass = all_pass and ok
        print("| %s | %d | %.4f, %.4f | %.4f, %.4f | %s | %.2f%% | %s |" % (
            name, n, l1[0], l1[1], l2[0], l2[1], expected, 100 * match.mean(),
            "pass" if ok else "FAIL: " + ", ".join(k for k, v in checks.items() if not v)))
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
