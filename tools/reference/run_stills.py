"""Runs the reference comparison over every test still, for every mode.

    python run_stills.py --build BUILD_DIR [--obs OBS_DIR] [--out OUT_DIR] [--only NAME]
                         [--renderer d3d11|opengl]

For each case it runs tools/harness (the plugin in a headless libobs, from a
build configured with -DGLYPHNOTIC_DEBUG_TOOLS=ON) on a still, then
compare.py on the grid dump and the output frame, and prints a Markdown
table. Exit status 0 when every case passes.

Every case runs with motion off (Soften 0, Hold 0, Settle 0, Boil 0) unless
it sets a motion setting itself; the Soften cases check the blur against the
reference's.

Windows: BUILD_DIR is e.g. build_dbg/RelWithDebInfo (glyphnotic.dll and
glyphnotic-harness.exe), OBS_DIR defaults to C:\\Program Files\\obs-studio.
Linux: BUILD_DIR holds glyphnotic.so and glyphnotic-harness, OBS_DIR defaults
to /usr.
"""

import argparse
import json
import os
import subprocess
import sys
import time

import compare
import reference as ref

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
STILLS = os.path.join(ROOT, "tests", "stills")
CUSTOM = "@#%*+=-:."

# A custom Console palette: eight dark-to-light grays (slots 0-7, usable as
# backgrounds) and eight saturated colors.
CUSTOM_PALETTE = [(v, v, v) for v in (0, 30, 60, 90, 120, 160, 200, 240)] + \
                 [(230, 40, 40), (40, 200, 60), (50, 80, 230), (240, 220, 40),
                  (230, 120, 20), (150, 50, 200), (30, 200, 210), (255, 140, 180)]


def obs_color(rgb):
    r, g, b = rgb
    return 0xFF000000 | (b << 16) | (g << 8) | r


PALETTE_SETTINGS = dict({"palette": "custom"},
                        **{"palette_%d" % k: obs_color(c) for k, c in enumerate(CUSTOM_PALETTE)})


MOTION_OFF = {"soften": 0, "hold": 0, "settle": 0, "boil": 0}


def case(name, still, mode, settings=None, on_scene=False, press=None):
    return dict(name=name, still=still, mode=mode, settings=dict(MOTION_OFF, **(settings or {})),
                on_scene=on_scene, press=press)


CASES = [
    # Mosaic (Phase 1)
    case("patches-blocks-1", "patches.png", "mosaic", {"charset": "blocks"}),
    case("patches-ascii-2-topleft-clear", "patches.png", "mosaic",
         {"charset": "ascii", "cell_scale": 2, "alignment": "top_left", "edges": "transparent"}),
    case("patches-lines-3", "patches.png", "mosaic", {"charset": "lines", "cell_scale": 3}),
    case("patches-every-4", "patches.png", "mosaic", {"charset": "every", "cell_scale": 4}),
    case("patches-blocks-1-scene", "patches.png", "mosaic", {"charset": "blocks"}, on_scene=True),
    case("victor-blocks-1", "victor.png", "mosaic", {"charset": "blocks"}),
    case("victor-lines-1", "victor.png", "mosaic", {"charset": "lines"}),
    case("victor-ascii-1", "victor.png", "mosaic", {"charset": "ascii"}),
    case("victor-every-1", "victor.png", "mosaic", {"charset": "every"}),
    case("victor-custom-1", "victor.png", "mosaic", {"charset": "custom", "custom_chars": CUSTOM}),
    case("victor-ascii-2", "victor.png", "mosaic", {"charset": "ascii", "cell_scale": 2}),
    case("spring-blocks-1", "spring.png", "mosaic", {"charset": "blocks"}),
    case("spring-ascii-1", "spring.png", "mosaic", {"charset": "ascii"}),
    case("spring-every-1", "spring.png", "mosaic", {"charset": "every"}),
    case("spring-lines-3-topleft", "spring.png", "mosaic",
         {"charset": "lines", "cell_scale": 3, "alignment": "top_left"}),
    case("sintel-blocks-1", "sintel.png", "mosaic", {"charset": "blocks"}),
    case("sintel-lines-1", "sintel.png", "mosaic", {"charset": "lines"}),
    case("sintel-ascii-1", "sintel.png", "mosaic", {"charset": "ascii"}),
    case("sintel-every-1", "sintel.png", "mosaic", {"charset": "every"}),
    case("sintel-ascii-4", "sintel.png", "mosaic", {"charset": "ascii", "cell_scale": 4}),
    case("bbb-alpha-blocks-1", "bbb-alpha.png", "mosaic", {"charset": "blocks"}),
    case("bbb-alpha-ascii-1-opaque", "bbb-alpha.png", "mosaic", {"charset": "ascii", "keep_transparency": False}),
    case("bbb-alpha-lines-2-topleft-clear", "bbb-alpha.png", "mosaic",
         {"charset": "lines", "cell_scale": 2, "alignment": "top_left", "edges": "transparent"}),
    case("bbb-alpha-every-1", "bbb-alpha.png", "mosaic", {"charset": "every"}),
    case("bbb-alpha-ascii-1-scene", "bbb-alpha.png", "mosaic", {"charset": "ascii"}, on_scene=True),

    # Ink: every character set, each ink and background, Auto and Manual
    # levels, the slider extremes, transparency and the scene path.
    case("ink-victor-blocks", "victor.png", "ink", {"charset": "blocks"}),
    case("ink-victor-lines", "victor.png", "ink", {"charset": "lines"}),
    case("ink-victor-ascii", "victor.png", "ink", {"charset": "ascii"}),
    case("ink-victor-every", "victor.png", "ink", {"charset": "every"}),
    case("ink-victor-custom", "victor.png", "ink", {"charset": "custom", "custom_chars": CUSTOM}),
    case("ink-victor-ascii-high", "victor.png", "ink",
         {"charset": "ascii", "gamma": 1.5, "shape": 1.0, "saturation": 2.0, "ink_lightness": 0.5}),
    case("ink-victor-ascii-low", "victor.png", "ink",
         {"charset": "ascii", "gamma": 0.5, "shape": 0.0, "saturation": 0.5, "ink_lightness": 1.0}),
    case("ink-spring-ascii-tint", "spring.png", "ink", {"charset": "ascii", "ink_background": "tint"}),
    case("ink-spring-every", "spring.png", "ink", {"charset": "every"}),
    case("ink-spring-lines-3-topleft-amber", "spring.png", "ink",
         {"charset": "lines", "cell_scale": 3, "alignment": "top_left", "ink_color": "amber"}),
    case("ink-sintel-ascii-2-green", "sintel.png", "ink", {"charset": "ascii", "cell_scale": 2, "ink_color": "green"}),
    case("ink-sintel-ascii-manual", "sintel.png", "ink",
         {"charset": "ascii", "ink_levels": "manual", "black_point": 0.1, "white_point": 0.6}),
    case("ink-sintel-ascii-manual-narrow", "sintel.png", "ink",
         {"charset": "ascii", "ink_levels": "manual", "black_point": 0.5, "white_point": 0.4}),
    case("ink-patches-ascii-white", "patches.png", "ink", {"charset": "ascii", "ink_color": "white"}),
    case("ink-patches-blocks-4-custom-ink", "patches.png", "ink",
         {"charset": "blocks", "cell_scale": 4, "ink_color": "custom", "ink_custom_color": obs_color((255, 60, 200))}),
    case("ink-bbb-alpha-ascii", "bbb-alpha.png", "ink", {"charset": "ascii"}),
    case("ink-bbb-alpha-ascii-opaque", "bbb-alpha.png", "ink", {"charset": "ascii", "keep_transparency": False}),
    case("ink-bbb-alpha-every-scene", "bbb-alpha.png", "ink", {"charset": "every"}, on_scene=True),

    # Poster: every character set, the level extremes and an uneven step (7).
    case("poster-victor-blocks", "victor.png", "poster", {"charset": "blocks"}),
    case("poster-victor-lines", "victor.png", "poster", {"charset": "lines"}),
    case("poster-victor-ascii", "victor.png", "poster", {"charset": "ascii"}),
    case("poster-victor-every", "victor.png", "poster", {"charset": "every"}),
    case("poster-victor-custom", "victor.png", "poster", {"charset": "custom", "custom_chars": CUSTOM}),
    case("poster-spring-lines-2", "spring.png", "poster", {"charset": "lines", "poster_levels": 2}),
    case("poster-spring-lines-16", "spring.png", "poster", {"charset": "lines", "poster_levels": 16}),
    case("poster-sintel-ascii-7", "sintel.png", "poster", {"charset": "ascii", "poster_levels": 7}),
    case("poster-patches-lines-3", "patches.png", "poster", {"charset": "lines", "cell_scale": 3}),
    case("poster-bbb-alpha-lines-2-clear", "bbb-alpha.png", "poster",
         {"charset": "lines", "cell_scale": 2, "edges": "transparent"}),

    # Console: every character set, VGA, a custom palette and a captured one.
    case("console-victor-blocks", "victor.png", "console", {"charset": "blocks"}),
    case("console-victor-lines", "victor.png", "console", {"charset": "lines"}),
    case("console-victor-ascii", "victor.png", "console", {"charset": "ascii"}),
    case("console-victor-every", "victor.png", "console", {"charset": "every"}),
    case("console-victor-custom", "victor.png", "console", {"charset": "custom", "custom_chars": CUSTOM}),
    case("console-spring-every", "spring.png", "console", {"charset": "every"}),
    case("console-sintel-lines-2", "sintel.png", "console", {"charset": "lines", "cell_scale": 2}),
    case("console-patches-lines", "patches.png", "console", {"charset": "lines"}),
    case("console-patches-ascii-custom-palette", "patches.png", "console", dict({"charset": "ascii"}, **PALETTE_SETTINGS)),
    case("console-spring-lines-custom-palette", "spring.png", "console", dict({"charset": "lines"}, **PALETTE_SETTINGS)),
    case("console-sintel-lines-captured", "sintel.png", "console", {"charset": "lines"}, press="capture_palette"),
    case("console-bbb-alpha-lines", "bbb-alpha.png", "console", {"charset": "lines"}),
    case("console-bbb-alpha-ascii-opaque-scene", "bbb-alpha.png", "console",
         {"charset": "ascii", "keep_transparency": False}, on_scene=True),

    # Soften (Phase 3): the default 0.6 px, the 2 px end, a small 0.1 px, with
    # cell scales, alignment, transparency and the scene path, in every mode.
    case("soften-victor-mosaic-ascii-0.6", "victor.png", "mosaic", {"charset": "ascii", "soften": 0.6}),
    case("soften-victor-mosaic-ascii-2.0", "victor.png", "mosaic", {"charset": "ascii", "soften": 2.0}),
    case("soften-patches-mosaic-blocks-0.6", "patches.png", "mosaic", {"charset": "blocks", "soften": 0.6}),
    case("soften-patches-mosaic-lines-2-topleft-2.0", "patches.png", "mosaic",
         {"charset": "lines", "cell_scale": 2, "alignment": "top_left", "soften": 2.0}),
    case("soften-spring-mosaic-every-0.1", "spring.png", "mosaic", {"charset": "every", "soften": 0.1}),
    case("soften-bbb-alpha-mosaic-ascii-1.0", "bbb-alpha.png", "mosaic", {"charset": "ascii", "soften": 1.0}),
    case("soften-bbb-alpha-mosaic-blocks-3-scene-0.6", "bbb-alpha.png", "mosaic",
         {"charset": "blocks", "cell_scale": 3, "soften": 0.6}, on_scene=True),
    case("soften-victor-ink-ascii-0.6", "victor.png", "ink", {"charset": "ascii", "soften": 0.6}),
    case("soften-sintel-ink-every-2.0", "sintel.png", "ink", {"charset": "every", "soften": 2.0}),
    case("soften-bbb-alpha-ink-ascii-4-1.5", "bbb-alpha.png", "ink", {"charset": "ascii", "cell_scale": 4,
                                                                       "soften": 1.5}),
    case("soften-sintel-poster-lines-0.6", "sintel.png", "poster", {"charset": "lines", "soften": 0.6}),
    case("soften-spring-poster-ascii-2-2.0", "spring.png", "poster",
         {"charset": "ascii", "cell_scale": 2, "soften": 2.0}),
    case("soften-victor-console-lines-0.6", "victor.png", "console", {"charset": "lines", "soften": 0.6}),
    case("soften-bbb-alpha-console-lines-2.0", "bbb-alpha.png", "console", {"charset": "lines", "soften": 2.0}),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    win = sys.platform == "win32"
    ap.add_argument("--obs", default=r"C:\Program Files\obs-studio" if win else "/usr")
    ap.add_argument("--out", default=os.path.join(ROOT, "build_stills"))
    ap.add_argument("--only", help="run only cases whose name contains this")
    ap.add_argument("--renderer", help="d3d11 or opengl (Windows); default: the platform's")
    a = ap.parse_args()

    a.out = os.path.abspath(a.out)
    a.build = os.path.abspath(a.build)
    os.makedirs(a.out, exist_ok=True)
    plugin = os.path.join(a.build, "glyphnotic.dll" if win else "glyphnotic.so")
    harness = os.path.join(a.build, "glyphnotic-harness.exe" if win else "glyphnotic-harness")
    env = dict(os.environ)
    if win:
        env["PATH"] = os.path.join(a.obs, "bin", "64bit") + os.pathsep + env["PATH"]

    rows, results, all_pass = [], [], True
    for c in CASES:
        name = c["name"]
        if a.only and a.only not in name:
            continue
        still_path = os.path.join(STILLS, c["still"])
        dump = os.path.join(a.out, name + ".json")
        frame = os.path.join(a.out, name + ".pam")
        cmd = [harness, "--obs", a.obs, "--plugin", plugin, "--plugin-data", os.path.join(ROOT, "data"),
               "--image", still_path, "--filter", "glyphnotic_" + c["mode"], "--settings", json.dumps(c["settings"]),
               "--dump", dump, "--frame", frame]
        if c["on_scene"]:
            cmd.append("--on-scene")
        if c["press"]:
            cmd += ["--press", c["press"], "--wait-frames", "30"]
        if a.renderer:
            cmd += ["--renderer", a.renderer]
        t0 = time.time()
        p = subprocess.run(cmd, env=env, capture_output=True, text=True)
        leaks = [l for l in p.stderr.splitlines() if l.startswith("memory leaks:")]
        if p.returncode != 0:
            print("%s: harness failed\n%s" % (name, p.stderr), file=sys.stderr)
            rows.append("| %s | %s | harness failed | | | | | | |" % (name, c["mode"]))
            all_pass = False
            continue
        custom = c["settings"].get("custom_chars", "") if c["settings"].get("charset") == "custom" else ""
        res = compare.compare(still_path, dump, frame, custom)
        res["case"] = name
        res["leaks"] = leaks[0].split(":")[1].strip() if leaks else "?"
        res["seconds"] = round(time.time() - t0, 1)
        if c["press"] == "capture_palette":
            # The captured palette is in use, isn't VGA, and the look became Custom.
            d = json.load(open(dump))
            res["checks"]["captured"] = (d.get("palette_name") == "custom" and d.get("look") == "custom" and
                                         [tuple(x) for x in d["palette"]] != ref.VGA)
            res["pass"] = res["pass"] and res["checks"]["captured"]
        results.append(res)
        all_pass = all_pass and res["pass"] and res["leaks"] == "0"
        failed = [k for k, v in res["checks"].items() if not v]
        rows.append("| %s | %s | %d | %d | %.2f%% | %s | %.3f | %s | %s |" % (
            name, c["mode"], res.get("cells", 0), res.get("glyphs", 0), 100 * res.get("match", 0),
            "%.2e" % res["max_gap"] if res.get("mismatches") else "-",
            max(res.get("max_color_error", 0), res.get("max_known_color_error", 0)),
            res.get("frame_bad_pixels", "?"), "pass" if not failed and res["leaks"] == "0" else
            "FAIL: " + ", ".join(failed + ([] if res["leaks"] == "0" else ["leaks"]))))
        print(rows[-1], flush=True)

    print()
    print("| Case | Mode | Cells | Glyphs | Match | Max gap | Max color err (/255) | Bad frame px | Result |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for r in rows:
        print(r)
    with open(os.path.join(a.out, "results.json"), "w") as f:
        json.dump(results, f, indent=1, default=float)
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
