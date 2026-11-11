"""Checks Glyphnotic's color handling along OBS's other color paths.

    python run_color_paths.py --build BUILD_DIR [--obs OBS_DIR] [--out OUT_DIR]

The filter isn't OBS_SOURCE_SRGB (docs/SPEC.md, Color), so OBS converts
whatever is upstream to 8-bit sRGB before the filter and converts the
filter's sRGB output to whatever is downstream. These cases check that with
OBS's own conversions in the loop:

  cc-before    Color Correction, then Glyphnotic
  cc-after     Glyphnotic, then Color Correction
  i010-709     10-bit canvas (I010), Rec. 709, limited range
  p010-pq      HDR canvas (P010, Rec. 2100 PQ)
  png16        a 16-bit PNG source
  hdr-sdr      an HDR source (linear, highlights to 2x SDR white) on an SDR canvas
  hdr-pq       the same HDR source on an HDR canvas

What the filter should see is what OBS draws for the same upstream on an
8-bit sRGB canvas: the harness captures that frame first, and the reference
runs on it. The filter's output is checked in OBS's main canvas texture
(any format) against the picture the grid dump describes, or, after Color
Correction, against Color Correction applied to Glyphnotic's own output.
"""

import argparse
import json
import os
import subprocess
import sys

import numpy as np

import compare
import imageio_min
import reference as ref

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
STILLS = os.path.join(ROOT, "tests", "stills")
# Motion off (Soften blurs; the rest act over time), as in Phase 1.
MOSAIC = ["--filter", "glyphnotic_mosaic", "--settings",
          json.dumps({"charset": "ascii", "soften": 0, "hold": 0, "settle": 0, "boil": 0})]
CC = ["--filter", "color_filter_v2", "--settings",
      json.dumps({"gamma": 0.4, "saturation": 0.6, "hue_shift": 40.0, "contrast": 0.2})]


class Runner:
    def __init__(self, a):
        win = sys.platform == "win32"
        self.out = os.path.abspath(a.out)
        os.makedirs(self.out, exist_ok=True)
        build = os.path.abspath(a.build)
        self.base = [os.path.join(build, "glyphnotic-harness.exe" if win else "glyphnotic-harness"),
                     "--obs", a.obs, "--plugin", os.path.join(build, "glyphnotic.dll" if win else "glyphnotic.so"),
                     "--plugin-data", os.path.join(ROOT, "data")]
        self.env = dict(os.environ)
        if win:
            self.env["PATH"] = os.path.join(a.obs, "bin", "64bit") + os.pathsep + self.env["PATH"]

    def path(self, name):
        return os.path.join(self.out, name)

    def run(self, args, dump=None):
        env = dict(self.env)
        if dump:
            env["GLYPHNOTIC_DUMP_PATH"] = dump
            args = args + ["--dump", dump]
        p = subprocess.run(self.base + args, env=env, capture_output=True, text=True)
        leaks = [l for l in p.stderr.splitlines() if l.startswith("memory leaks:")]
        if p.returncode != 0:
            raise RuntimeError("harness failed: %s\n%s" % (" ".join(args), p.stderr))
        return leaks[0].split(":")[1].strip() if leaks else "?"

    def capture(self, name, args):
        """Runs args on an 8-bit sRGB canvas and saves the frame as PNG."""
        pam = self.path(name + ".pam")
        self.run(args + ["--frame", pam])
        png = self.path(name + ".png")
        imageio_min.write_png(png, imageio_min.read_pam(pam))
        return png


def read_main_texture(path, width, height, linear):
    """OBS's main canvas texture as sRGB-encoded 0-255 floats (h, w, 4), cropped to the still.

    SDR canvases hold sRGB-encoded values even in float formats; HDR canvases
    (Rec. 2100) hold linear Rec. 709 values with 1.0 = SDR white.
    """
    data = open(path, "rb").read()
    nl = data.index(b"\n")
    _, w, h, fmt = data[:nl].decode().split()
    w, h = int(w), int(h)
    body = data[nl + 1:]
    if fmt in ("bgra8", "rgba8"):
        px = np.frombuffer(body, dtype=np.uint8).reshape(h, w, 4).astype(np.float64)
        if fmt == "bgra8":
            px = px[..., [2, 1, 0, 3]]
    else:
        dtype = np.float16 if fmt == "rgba16f" else np.float32
        v = np.frombuffer(body, dtype=dtype).reshape(h, w, 4).astype(np.float64)
        rgb = ref.srgb_encode(v[..., :3]) if linear else v[..., :3]
        px = np.concatenate([rgb * 255.0, v[..., 3:] * 255.0], axis=-1)
    return px[:height, :width], fmt


def frame_diff(actual, expected):
    d = np.abs(np.asarray(actual, dtype=np.float64) - np.asarray(expected, dtype=np.float64))
    return int((d.max(-1) > 1.0).sum()), float(d.max())


def check_dump(still, dump, frame=None):
    res = compare.compare(still, dump, frame)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--obs", default=r"C:\Program Files\obs-studio" if sys.platform == "win32" else "/usr")
    ap.add_argument("--out", default=os.path.join(ROOT, "build_color_paths"))
    a = ap.parse_args()
    r = Runner(a)
    victor = os.path.join(STILLS, "victor.png")
    rows, ok_all = [], True

    def report(name, res, extra="", leaks="0", ok=None):
        nonlocal ok_all
        ok = res["pass"] if ok is None else ok
        ok = ok and leaks == "0"
        ok_all = ok_all and ok
        rows.append("| %s | %.2f%% | %.3f | %s | %s | %s |" % (
            name, 100 * res.get("match", 0), res.get("max_color_error", 0), extra, leaks,
            "pass" if ok else "FAIL: " + ", ".join(k for k, v in res["checks"].items() if not v)))
        print(rows[-1], flush=True)

    # Color Correction before Glyphnotic: the filter must see Color Correction's output.
    seen = r.capture("cc-before-input", ["--image", victor] + CC)
    dump, pam = r.path("cc-before.json"), r.path("cc-before.pam")
    leaks = r.run(["--image", victor] + CC + MOSAIC + ["--frame", pam], dump)
    res = compare.compare(seen, dump, pam)
    report("cc-before", res, "frame bad px %d" % res["frame_bad_pixels"], leaks)

    # Glyphnotic before Color Correction: Color Correction must see Glyphnotic's output.
    own = r.capture("cc-after-glyphnotic", ["--image", victor] + MOSAIC)
    expected = imageio_min.read_pam(r.path("cc-after-glyphnotic.pam"))
    expected_cc = r.capture("cc-after-expected", ["--image", own] + CC)
    dump, pam = r.path("cc-after.json"), r.path("cc-after.pam")
    leaks = r.run(["--image", victor] + MOSAIC + CC + ["--frame", pam], dump)
    res = compare.compare(victor, dump)
    bad, worst = frame_diff(imageio_min.read_pam(pam), imageio_min.read_png(expected_cc))
    report("cc-after", res, "frame vs CC(Glyphnotic) bad px %d, max %.0f" % (bad, worst), leaks,
           res["pass"] and bad == 0)
    del expected

    # 10-bit and HDR canvases: the input path is unchanged; the output lands in a float canvas.
    # Control: the image alone on the HDR canvas shows the error of OBS's own sRGB-to-linear
    # conversion (the GPU's sRGB hardware) and half floats. Glyphnotic's output may be off by
    # that plus the 1/255 every frame check allows for rounding its colors to 8 bits.
    pq = ["--format", "p010", "--colorspace", "2100pq"]
    raw = r.path("pq-control.raw")
    r.run(["--image", victor] + pq + ["--main-texture", raw])
    img = imageio_min.read_png(victor)
    canvas, _ = read_main_texture(raw, img.shape[1], img.shape[0], True)
    _, pq_control = frame_diff(canvas, img)
    rows.append("| pq-control (image alone on the HDR canvas) | - | - | canvas vs still max %.2f | - | - |" % pq_control)
    print(rows[-1], flush=True)
    # OBS's GPU Delay filter isn't OBS_SOURCE_SRGB either: it shows that route adds nothing.
    raw = r.path("pq-gpu-delay.raw")
    r.run(["--image", victor] + pq + ["--filter", "gpu_delay", "--settings", '{"delay_ms": 100}',
                                      "--main-texture", raw])
    canvas, _ = read_main_texture(raw, img.shape[1], img.shape[0], True)
    rows.append("| pq-control (GPU Delay, not sRGB-aware, on the HDR canvas) | - | - | canvas vs still max %.2f | - | - |"
                % frame_diff(canvas, img)[1])
    print(rows[-1], flush=True)
    tolerance = {"i010-709": 1.0, "p010-pq": 1.0 + pq_control}
    for name, video in (("i010-709", ["--format", "i010", "--colorspace", "709", "--range", "partial"]),
                        ("p010-pq", pq)):
        dump, raw = r.path(name + ".json"), r.path(name + ".raw")
        leaks = r.run(["--image", victor] + video + MOSAIC + ["--main-texture", raw], dump)
        res = compare.compare(victor, dump)
        img = imageio_min.read_png(victor)
        d = json.load(open(dump))
        exp = compare.expected_frame(d, compare._charset_bits(ref.Font(), d), img.shape[1], img.shape[0])
        canvas, fmt = read_main_texture(raw, img.shape[1], img.shape[0], name == "p010-pq")
        _, worst = frame_diff(canvas, exp)
        report(name, res, "canvas %s max %.2f (limit %.2f)" % (fmt, worst, tolerance[name]), leaks,
               res["pass"] and worst <= tolerance[name])

    # A 16-bit PNG: smooth gradients whose values aren't multiples of 257.
    h, w = 720, 1280
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    g16 = np.stack([xx / (w - 1), yy / (h - 1), 0.5 + 0.5 * np.sin(xx / 97.0 + yy / 61.0)], -1)
    png16 = r.path("gradient16.png")
    imageio_min.write_png(png16, np.concatenate(
        [np.rint(g16 * 65535), np.full((h, w, 1), 65535.0)], -1).astype(np.uint16))
    seen = r.capture("png16-input", ["--image", png16])
    naive = np.rint(g16 * 255.0)
    conv_err = float(np.abs(imageio_min.read_png(seen)[..., :3] - naive).max())
    dump, pam = r.path("png16.json"), r.path("png16.pam")
    leaks = r.run(["--image", png16] + MOSAIC + ["--frame", pam], dump)
    res = compare.compare(seen, dump, pam)
    report("png16", res, "OBS's 16-to-8-bit input vs rounding: max %.0f/255" % conv_err, leaks)

    # An HDR source: what the filter sees is OBS's HDR-to-SDR conversion (tone mapped).
    hdr = ["--hdr-image", victor, "--hdr-scale", "2.0"]
    seen = r.capture("hdr-input", hdr)
    dump, pam = r.path("hdr-sdr.json"), r.path("hdr-sdr.pam")
    leaks = r.run(hdr + MOSAIC + ["--frame", pam], dump)
    res = compare.compare(seen, dump, pam)
    report("hdr-sdr", res, "frame bad px %d" % res["frame_bad_pixels"], leaks)

    dump, raw = r.path("hdr-pq.json"), r.path("hdr-pq.raw")
    leaks = r.run(hdr + pq + MOSAIC + ["--main-texture", raw], dump)
    res = compare.compare(seen, dump)
    img = imageio_min.read_png(seen)
    d = json.load(open(dump))
    exp = compare.expected_frame(d, compare._charset_bits(ref.Font(), d), img.shape[1], img.shape[0])
    canvas, fmt = read_main_texture(raw, img.shape[1], img.shape[0], True)
    _, worst = frame_diff(canvas, exp)
    report("hdr-pq", res, "canvas %s max %.2f (limit %.2f)" % (fmt, worst, tolerance["p010-pq"]), leaks,
           res["pass"] and worst <= tolerance["p010-pq"])

    print()
    print("| Case | Match | Max color err (/255) | Output check | Leaks | Result |")
    print("| --- | --- | --- | --- | --- | --- |")
    for row in rows:
        print(row)
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
