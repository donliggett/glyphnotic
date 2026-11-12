"""Phase 5 performance measurements (docs/SPEC.md, Build phases: Phase 5).

    python run_performance.py --build BUILD_DIR [--obs OBS_DIR] [--out OUT_DIR] [--renderer d3d11|opengl]
                              [--frames N] [--only SUITE[,SUITE...]]

Each case runs the harness on 1920x1080 stills at 60 fps for --frames
frames (default 600, 10 s) with OBS's source profiler on, which times every
source's rendering with libobs GPU timer queries (gs_timer).

A filter alone on an otherwise idle GPU times anywhere from 1.3x to 2x its
cost from run to run (likely a short burst of work running below the GPU's
full clocks), so each case is timed as three instances, each on its own 1080p
image in one scene: the GPU time per instance is the scene's GPU time / 3.
The time of one instance alone (the same filter by itself) is reported
too. Every figure includes drawing the image under the filter, which the
image's own time shows is negligible (about 0.02 ms).

Suites:

  looks      every look
  scale      Ink Black and Every Glyph at cell scales 1x-4x
  charsets   Ink Black's settings with each character set
  instances  how the scene's GPU time grows with 1, 2, 3, 4 and 8 instances
             of Ink Black and Every Glyph, with the frames OBS lagged

The still is spring.png scaled up to 1080 rows and cropped to 1920 columns,
written to OUT_DIR/spring-1080.png. Results print as Markdown tables and go
to OUT_DIR/results.json.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

import numpy as np

import imageio_min
import run_looks

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
STILLS = os.path.join(ROOT, "tests", "stills")
SUITES = ["looks", "scale", "charsets", "instances"]
LOOK_NAMES = {"black": "Black", "tint": "Tint", "phosphor": "Phosphor", "amber": "Amber",
              "every_glyph": "Every Glyph", "static": "Static", "fever": "Fever", "blocks": "Blocks",
              "poster": "Poster", "console": "Console"}

GPU_RE = re.compile(r"gpu: (\w+) '([^']*)': avg ([0-9.]+) ms, max ([0-9.]+) ms \(render CPU avg ([0-9.]+) ms\)")
RUN_RE = re.compile(r"run: (\d+) frames in ([0-9.]+) s at (\d+) fps, (\d+) lagged")


def make_still(out, name="spring"):
    """A test still scaled (bilinear) to cover 1920x1080 and cropped to it, centered: OUT/<name>-1080.png."""
    path = os.path.join(out, name + "-1080.png")
    if os.path.exists(path):
        return path
    img = imageio_min.read_png(os.path.join(STILLS, name + ".png")).astype(np.float64)
    h, w = img.shape[:2]
    scale = max(1080 / h, 1920 / w)
    nh, nw = max(1080, int(round(h * scale))), max(1920, int(round(w * scale)))
    ys = (np.arange(nh) + 0.5) / scale - 0.5
    xs = (np.arange(nw) + 0.5) / scale - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, h - 1)
    x0 = np.clip(np.floor(xs).astype(int), 0, w - 1)
    y1 = np.clip(y0 + 1, 0, h - 1)
    x1 = np.clip(x0 + 1, 0, w - 1)
    fy = np.clip(ys - y0, 0, 1)[:, None, None]
    fx = np.clip(xs - x0, 0, 1)[None, :, None]
    top = img[y0][:, x0] * (1 - fx) + img[y0][:, x1] * fx
    bottom = img[y1][:, x0] * (1 - fx) + img[y1][:, x1] * fx
    big = top * (1 - fy) + bottom * fy
    top_row, left = (nh - 1080) // 2, (nw - 1920) // 2
    crop = np.clip(np.round(big[top_row:top_row + 1080, left:left + 1920]), 0, 255).astype(np.uint8)
    imageio_min.write_png(path, np.ascontiguousarray(crop))
    return path


class Runner:
    def __init__(self, a):
        win = sys.platform == "win32"
        self.a = a
        self.plugin = os.path.join(a.build, "glyphnotic.dll" if win else "glyphnotic.so")
        self.harness = os.path.join(a.build, "glyphnotic-harness.exe" if win else "glyphnotic-harness")
        self.env = dict(os.environ)
        if win:
            self.env["PATH"] = os.path.join(a.obs, "bin", "64bit") + os.pathsep + self.env["PATH"]
        self.still = make_still(a.out)
        self.failures = []

    def run(self, name, chain, instances=1):
        """chain: [(mode, settings)]; returns {'filters': [...], 'sources': [...], 'frames', 'lagged', 'secs'}."""
        cmd = [self.harness, "--obs", self.a.obs, "--plugin", self.plugin, "--plugin-data",
               os.path.join(ROOT, "data"), "--image", self.still, "--fps", "60", "--frames", str(self.a.frames),
               "--profile"]
        for mode, settings in chain:
            cmd += ["--filter", "glyphnotic_" + mode, "--settings", json.dumps(settings)]
        if instances > 1:
            cmd += ["--instances", str(instances)]
        if self.a.renderer:
            cmd += ["--renderer", self.a.renderer]
        t0 = time.time()
        p = subprocess.run(cmd, env=self.env, capture_output=True, text=True)
        res = {"name": name, "filters": [], "sources": [], "scenes": []}
        for m in GPU_RE.finditer(p.stderr):
            kind, label, avg, mx, cpu = m.groups()
            res[kind + "s"].append({"name": label, "gpu_avg": float(avg), "gpu_max": float(mx),
                                    "cpu_avg": float(cpu)})
        m = RUN_RE.search(p.stderr)
        if p.returncode != 0 or not m or not res["filters"]:
            self.failures.append(name)
            print("FAIL: %s: harness exit %d\n%s" % (name, p.returncode, p.stderr[-2000:]), flush=True)
            return None
        res.update(frames=int(m.group(1)), secs=float(m.group(2)), lagged=int(m.group(4)))
        print("  %s: %.3f ms GPU (max %.3f), %d/%d lagged (%.0f s)" % (
            name, res["filters"][0]["gpu_avg"], res["filters"][0]["gpu_max"], res["lagged"], res["frames"],
            time.time() - t0), flush=True)
        return res


INSTANCES = 3


def per_instance(res, n):
    """GPU ms per instance: the scene's time / n (n > 1), or the filter's own time alone."""
    if n > 1 and res["scenes"]:
        return res["scenes"][0]["gpu_avg"] / n
    return res["filters"][0]["gpu_avg"]


def measure(r, results, name, chain):
    """(per instance of INSTANCES, alone, lagged frames with INSTANCES) or None."""
    many = r.run(name + "-x%d" % INSTANCES, chain, instances=INSTANCES)
    alone = r.run(name + "-x1", chain)
    if not many or not alone:
        return None
    results += [many, alone]
    return per_instance(many, INSTANCES), per_instance(alone, 1), many["lagged"], many["frames"]


def black_values(**changes):
    """Ink Black's settings without naming the look, so a changed set stays changed."""
    v = dict(run_looks.LOOKS["ink"][0][1])
    v.update(changes)
    return v


def suite_looks(r, results):
    rows = []
    for mode, looks in run_looks.LOOKS.items():
        for lid, _ in looks:
            m = measure(r, results, "looks-%s-%s" % (mode, lid), [(mode, {"look": lid})])
            if m:
                rows.append((mode, lid) + m)
    print("\n| Filter | Look | GPU per instance (ms) | One instance alone (ms) | Lagged frames, 3 instances |")
    print("| --- | --- | --- | --- | --- |")
    for mode, lid, per, alone, lagged, frames in rows:
        print("| %s | %s | %.2f | %.2f | %d / %d |" % (mode.capitalize(), LOOK_NAMES[lid], per, alone, lagged, frames))


def suite_scale(r, results):
    rows = []
    for lid in ("black", "every_glyph"):
        for scale in (1, 2, 3, 4):
            m = measure(r, results, "scale-%s-%d" % (lid, scale), [("ink", {"look": lid, "cell_scale": scale})])
            if m:
                rows.append((lid, scale) + m)
    print("\n| Look | Cell scale | GPU per instance (ms) | One instance alone (ms) |")
    print("| --- | --- | --- | --- |")
    for lid, scale, per, alone, _, _ in rows:
        print("| Ink %s | %dx | %.2f | %.2f |" % (LOOK_NAMES[lid], scale, per, alone))


def suite_charsets(r, results):
    rows = []
    for cs in ("blocks", "lines", "ascii", "every"):
        m = measure(r, results, "charset-%s" % cs, [("ink", black_values(charset=cs))])
        if m:
            rows.append((cs,) + m)
    print("\n| Set (Ink, Black's other settings) | GPU per instance (ms) | One instance alone (ms) |")
    print("| --- | --- | --- |")
    for cs, per, alone, _, _ in rows:
        print("| %s | %.2f | %.2f |" % (cs, per, alone))


def suite_instances(r, results):
    rows = []
    for lid in ("black", "every_glyph"):
        for n in (1, 2, 3, 4, 8):
            res = r.run("instances-%s-x%d" % (lid, n), [("ink", {"look": lid})], instances=n)
            if res:
                results.append(res)
                total = res["scenes"][0]["gpu_avg"] if n > 1 and res["scenes"] else res["filters"][0]["gpu_avg"]
                rows.append((lid, n, total, res["lagged"], res["frames"]))
    print("\n| Look | Instances | Scene GPU (ms) | Per instance (ms) | Lagged frames |")
    print("| --- | --- | --- | --- | --- |")
    for lid, n, total, lagged, frames in rows:
        print("| Ink %s | %d | %.2f | %.2f | %d / %d |" % (LOOK_NAMES[lid], n, total, total / n, lagged, frames))


def main():
    win = sys.platform == "win32"
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--obs", default=r"C:\Program Files\obs-studio" if win else "/usr")
    ap.add_argument("--out", default=os.path.join(ROOT, "build_perf"))
    ap.add_argument("--renderer")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--only")
    a = ap.parse_args()
    a.build = os.path.abspath(a.build)
    a.out = os.path.abspath(a.out)
    os.makedirs(a.out, exist_ok=True)
    suites = a.only.split(",") if a.only else SUITES
    r = Runner(a)
    results = []
    for s in suites:
        print("== %s" % s, flush=True)
        globals()["suite_" + s](r, results)
    with open(os.path.join(a.out, "results.json"), "w") as f:
        json.dump(results, f, indent=1)
    if r.failures:
        print("\n%d failures: %s" % (len(r.failures), ", ".join(r.failures)))
        return 1
    print("\nall cases ran")
    return 0


if __name__ == "__main__":
    sys.exit(main())
