"""Runs the Phase 1 reference comparison over every test still.

    python run_stills.py --build BUILD_DIR [--obs OBS_DIR] [--out OUT_DIR] [--only NAME]
                         [--renderer d3d11|opengl]

For each case it runs tools/harness (the plugin in a headless libobs, from a
build configured with -DGLYPHNOTIC_DEBUG_TOOLS=ON) on a still, then
compare.py on the grid dump and the output frame, and prints a Markdown
table. Exit status 0 when every case passes.

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

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
STILLS = os.path.join(ROOT, "tests", "stills")
CUSTOM = "@#%*+=-:."

# (case name, still, filter settings, on scene)
CASES = [
    ("patches-blocks-1", "patches.png", {"charset": "blocks"}, False),
    ("patches-ascii-2-topleft-clear", "patches.png",
     {"charset": "ascii", "cell_scale": 2, "alignment": "top_left", "edges": "transparent"}, False),
    ("patches-lines-3", "patches.png", {"charset": "lines", "cell_scale": 3}, False),
    ("patches-every-4", "patches.png", {"charset": "every", "cell_scale": 4}, False),
    ("patches-blocks-1-scene", "patches.png", {"charset": "blocks"}, True),
    ("victor-blocks-1", "victor.png", {"charset": "blocks"}, False),
    ("victor-lines-1", "victor.png", {"charset": "lines"}, False),
    ("victor-ascii-1", "victor.png", {"charset": "ascii"}, False),
    ("victor-every-1", "victor.png", {"charset": "every"}, False),
    ("victor-custom-1", "victor.png", {"charset": "custom", "custom_chars": CUSTOM}, False),
    ("victor-ascii-2", "victor.png", {"charset": "ascii", "cell_scale": 2}, False),
    ("spring-blocks-1", "spring.png", {"charset": "blocks"}, False),
    ("spring-ascii-1", "spring.png", {"charset": "ascii"}, False),
    ("spring-every-1", "spring.png", {"charset": "every"}, False),
    ("spring-lines-3-topleft", "spring.png", {"charset": "lines", "cell_scale": 3, "alignment": "top_left"}, False),
    ("sintel-blocks-1", "sintel.png", {"charset": "blocks"}, False),
    ("sintel-lines-1", "sintel.png", {"charset": "lines"}, False),
    ("sintel-ascii-1", "sintel.png", {"charset": "ascii"}, False),
    ("sintel-every-1", "sintel.png", {"charset": "every"}, False),
    ("sintel-ascii-4", "sintel.png", {"charset": "ascii", "cell_scale": 4}, False),
    ("bbb-alpha-blocks-1", "bbb-alpha.png", {"charset": "blocks"}, False),
    ("bbb-alpha-ascii-1-opaque", "bbb-alpha.png", {"charset": "ascii", "keep_transparency": False}, False),
    ("bbb-alpha-lines-2-topleft-clear", "bbb-alpha.png",
     {"charset": "lines", "cell_scale": 2, "alignment": "top_left", "edges": "transparent"}, False),
    ("bbb-alpha-every-1", "bbb-alpha.png", {"charset": "every"}, False),
    ("bbb-alpha-ascii-1-scene", "bbb-alpha.png", {"charset": "ascii"}, True),
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
    for name, still, settings, on_scene in CASES:
        if a.only and a.only not in name:
            continue
        still_path = os.path.join(STILLS, still)
        dump = os.path.join(a.out, name + ".json")
        frame = os.path.join(a.out, name + ".pam")
        env["GLYPHNOTIC_DUMP_PATH"] = dump
        cmd = [harness, "--obs", a.obs, "--plugin", plugin, "--plugin-data", os.path.join(ROOT, "data"),
               "--image", still_path, "--filter", "glyphnotic_mosaic", "--settings", json.dumps(settings),
               "--dump", dump, "--frame", frame]
        if on_scene:
            cmd.append("--on-scene")
        if a.renderer:
            cmd += ["--renderer", a.renderer]
        t0 = time.time()
        p = subprocess.run(cmd, env=env, capture_output=True, text=True)
        leaks = [l for l in p.stderr.splitlines() if l.startswith("memory leaks:")]
        if p.returncode != 0:
            print("%s: harness failed\n%s" % (name, p.stderr), file=sys.stderr)
            rows.append("| %s | harness failed | | | | | | |" % name)
            all_pass = False
            continue
        res = compare.compare(still_path, dump, frame, CUSTOM if settings.get("charset") == "custom" else "")
        res["case"] = name
        res["leaks"] = leaks[0].split(":")[1].strip() if leaks else "?"
        res["seconds"] = round(time.time() - t0, 1)
        results.append(res)
        all_pass = all_pass and res["pass"] and res["leaks"] == "0"
        failed = [k for k, v in res["checks"].items() if not v]
        rows.append("| %s | %d | %d | %.2f%% | %s | %.3f | %s | %s |" % (
            name, res.get("cells", 0), res.get("glyphs", 0), 100 * res.get("match", 0),
            "%.2e" % res["max_gap"] if res.get("mismatches") else "-",
            max(res.get("max_color_error", 0), res.get("max_known_color_error", 0)),
            res.get("frame_bad_pixels", "?"), "pass" if not failed and res["leaks"] == "0" else
            "FAIL: " + ", ".join(failed + ([] if res["leaks"] == "0" else ["leaks"]))))
        print(rows[-1], flush=True)

    print()
    print("| Case | Cells | Glyphs | Match | Max gap | Max color err (/255) | Bad frame px | Result |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- |")
    for r in rows:
        print(r)
    with open(os.path.join(a.out, "results.json"), "w") as f:
        json.dump(results, f, indent=1, default=float)
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
