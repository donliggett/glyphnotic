"""Checks the looks and the filters' properties (docs/SPEC.md, Looks and Filters and settings).

    python run_looks.py --build BUILD_DIR [--obs OBS_DIR] [--out OUT_DIR] [--renderer d3d11|opengl]

For every filter it checks, through tools/harness:
  - a new filter starts on its first look, with that look's settings;
  - the Look list holds the filter's looks (from the spec, written out here
    independently of the plugin) plus Custom;
  - each look applies, both named in the settings (as a scene file or a
    script would) and chosen the way the properties window chooses it; the
    settings then equal the look's, and the picture matches the reference
    (with the look's Soften; for the boiling looks, the held glyphs);
  - changing a setting the look controls turns the Look to Custom, and a
    Grid setting doesn't;
  - mode-specific settings appear only in their own filter, and the
    conditional ones (Custom characters, custom ink, Levels' sliders, the
    palette colors) only when they apply.
Exit status 0 when every check passes.
"""

import argparse
import json
import os
import subprocess
import sys

import compare

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
STILL = os.path.join(ROOT, "tests", "stills", "victor.png")

# The spec's looks. Settings a look doesn't list keep the defaults: Soften
# 0.6, Settle 0, Boil 0, Boil spread 20, Boil linger 2; Ink's own defaults.
MOTION = {"soften": 0.6, "settle": 0, "boil": 0, "boil_spread": 20, "boil_linger": 2}
INK = {"ink_lightness": 0.92, "saturation": 1.4, "gamma": 0.85, "shape": 0.35, "ink_levels": "auto",
       "adapt_speed": 1.5, "black_point": 0.0, "white_point": 1.0}


def ink_look(charset, color, background, hold, **motion):
    return dict(MOTION, **INK, charset=charset, ink_color=color, ink_background=background, hold=hold, **motion)


LOOKS = {
    "ink": [
        ("black", ink_look("ascii", "cell", "black", 25)),
        ("tint", ink_look("ascii", "cell", "tint", 25)),
        ("phosphor", ink_look("ascii", "green", "black", 25)),
        ("amber", ink_look("ascii", "amber", "black", 25)),
        ("every_glyph", ink_look("every", "cell", "black", 25)),
        ("static", ink_look("ascii", "green", "black", 25, boil=15, boil_spread=20, boil_linger=2)),
        ("fever", ink_look("ascii", "cell", "tint", 10, boil=45, boil_spread=35, boil_linger=1, settle=30)),
    ],
    "mosaic": [("blocks", dict(MOTION, charset="blocks", hold=10))],
    "poster": [("poster", dict(MOTION, charset="lines", poster_levels=6, hold=10))],
    "console": [("console", dict(MOTION, charset="lines", palette="vga", hold=10))],
}

# A setting each filter's looks control, with a value no look uses.
CONTROLLED_CHANGE = {"ink": {"gamma": 1.2}, "mosaic": {"charset": "ascii"}, "poster": {"poster_levels": 3},
                     "console": {"palette": "custom"}}
MOTION_PROPS = {"soften", "hold", "settle", "boil", "boil_spread", "boil_linger"}
MODE_PROPS = {"ink": {"ink_color", "ink_custom_color", "ink_background", "ink_lightness", "saturation", "gamma",
                      "shape", "ink_levels", "adapt_speed", "black_point", "white_point"},
              "poster": {"poster_levels"},
              "console": {"palette", "capture_palette", "palette_note"} | {"palette_%d" % k for k in range(16)}}


def flatten(props):
    out = {}
    for p in props:
        out[p["name"]] = p
        if "content" in p:
            out.update(flatten(p["content"]))
    return out


class Runner:
    def __init__(self, a):
        win = sys.platform == "win32"
        self.a = a
        self.plugin = os.path.join(a.build, "glyphnotic.dll" if win else "glyphnotic.so")
        self.harness = os.path.join(a.build, "glyphnotic-harness.exe" if win else "glyphnotic-harness")
        self.env = dict(os.environ)
        if win:
            self.env["PATH"] = os.path.join(a.obs, "bin", "64bit") + os.pathsep + self.env["PATH"]
        self.failures = []
        self.checks = 0

    def harness_run(self, name, mode, settings=None, ui_set=None, dump=False):
        base = os.path.join(self.a.out, name)
        cmd = [self.harness, "--obs", self.a.obs, "--plugin", self.plugin, "--plugin-data",
               os.path.join(ROOT, "data"), "--image", STILL, "--filter", "glyphnotic_" + mode,
               "--settings", json.dumps(settings or {}), "--settings-out", base + "-settings.json",
               "--properties-out", base + "-props.json"]
        if ui_set:
            # Without the filter's own update, so only the properties callbacks act.
            cmd += ["--ui-set", json.dumps(ui_set), "--ui-no-update"]
        if dump:
            cmd += ["--dump", base + ".json", "--frame", base + ".pam"]
        if self.a.renderer:
            cmd += ["--renderer", self.a.renderer]
        p = subprocess.run(cmd, env=self.env, capture_output=True, text=True)
        leaks = [l for l in p.stderr.splitlines() if l.startswith("memory leaks:")]
        self.check(p.returncode == 0, name + ": harness ran", p.stderr[-2000:])
        self.check(bool(leaks) and leaks[0].strip() == "memory leaks: 0", name + ": no leaks")
        if p.returncode != 0:
            return None, None
        settings_out = json.load(open(base + "-settings.json"))
        props = flatten(json.load(open(base + "-props.json")))
        return settings_out, props

    def check(self, ok, what, detail=""):
        self.checks += 1
        if not ok:
            self.failures.append(what)
            print("FAIL: %s %s" % (what, detail), flush=True)

    def check_look(self, name, settings, look_id, values):
        self.check(settings.get("look") == look_id, "%s: look is %s (got %s)" % (name, look_id, settings.get("look")))
        for k, v in values.items():
            got = settings.get(k)
            same = abs(got - v) < 1e-9 if isinstance(v, float) and isinstance(got, (int, float)) else got == v
            self.check(same, "%s: %s = %r (got %r)" % (name, k, v, got))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    win = sys.platform == "win32"
    ap.add_argument("--obs", default=r"C:\Program Files\obs-studio" if win else "/usr")
    ap.add_argument("--out", default=os.path.join(ROOT, "build_looks"))
    ap.add_argument("--renderer")
    a = ap.parse_args()
    a.out = os.path.abspath(a.out)
    a.build = os.path.abspath(a.build)
    os.makedirs(a.out, exist_ok=True)
    r = Runner(a)
    rows = []

    for mode, looks in LOOKS.items():
        ids = [lid for lid, _ in looks]

        # A new filter: the first look, the Look list, which properties show.
        s, props = r.harness_run("%s-new" % mode, mode)
        if s:
            r.check_look("%s new" % mode, s, ids[0], looks[0][1])
            r.check(props["look"]["items"] == ids + ["custom"],
                    "%s: Look list is %s (got %s)" % (mode, ids + ["custom"], props["look"]["items"]))
            for m, names in MODE_PROPS.items():
                for n in names:
                    r.check((n in props) == (m == mode), "%s: property %s present only in %s" % (mode, n, m))
            r.check(not props["custom_chars"]["visible"], "%s: Custom characters hidden" % mode)
            for n in sorted(MOTION_PROPS):
                r.check(n in props and props[n]["visible"], "%s: Motion's %s shown" % (mode, n))
            if mode == "ink":
                r.check(not props["ink_custom_color"]["visible"], "ink: custom ink hidden for Cell")
                r.check(props["adapt_speed"]["visible"], "ink: Adapt speed shown for Auto")
                r.check(not props["black_point"]["visible"] and not props["white_point"]["visible"],
                        "ink: Black/White point hidden for Auto")
            if mode == "console":
                r.check(not any(props["palette_%d" % k]["visible"] for k in range(16)),
                        "console: palette colors hidden for VGA")

        for lid, values in looks:
            # Named in the settings, as a scene file or a script would.
            name = "%s-%s" % (mode, lid)
            s, _ = r.harness_run(name, mode, settings={"look": lid}, dump=True)
            if s:
                r.check_look(name, s, lid, values)
                res = compare.compare(STILL, os.path.join(a.out, name + ".json"), os.path.join(a.out, name + ".pam"))
                failed = [k for k, v in res["checks"].items() if not v]
                r.check(res["pass"], "%s: picture matches the reference" % name, str(failed))
                rows.append("| %s | %s | %.2f%% | %s | %s |" % (
                    mode, lid, 100 * res.get("match", 0),
                    "%.2e" % res["max_gap"] if res.get("mismatches") else "-", "pass" if res["pass"] else "FAIL"))
            # Chosen in the properties window, starting from a new filter.
            s, _ = r.harness_run(name + "-ui", mode, ui_set={"look": lid})
            if s:
                r.check_look(name + " (chosen in the window)", s, lid, values)

        # Changing a setting the look controls shows Custom; a Grid setting doesn't.
        s, props = r.harness_run("%s-change" % mode, mode, ui_set=CONTROLLED_CHANGE[mode])
        if s:
            r.check(s["look"] == "custom", "%s: changing %s shows Custom" % (mode, list(CONTROLLED_CHANGE[mode])))
            if mode == "console":
                r.check(all(props["palette_%d" % k]["visible"] for k in range(16)),
                        "console: palette colors shown for Custom")
        s, _ = r.harness_run("%s-grid" % mode, mode, ui_set={"cell_scale": 2})
        if s:
            r.check(s["look"] == ids[0], "%s: changing Cell scale keeps the look" % mode)

    # Conditional Ink properties.
    s, props = r.harness_run("ink-visibility", "ink", ui_set={"ink_color": "custom", "ink_levels": "manual"})
    if s:
        r.check(props["ink_custom_color"]["visible"], "ink: custom ink shown for Custom")
        r.check(not props["adapt_speed"]["visible"], "ink: Adapt speed hidden for Manual")
        r.check(props["black_point"]["visible"] and props["white_point"]["visible"],
                "ink: Black/White point shown for Manual")
        r.check(s["look"] == "custom", "ink: Manual levels shows Custom")
    s, props = r.harness_run("mosaic-custom-chars", "mosaic", ui_set={"charset": "custom"})
    if s:
        r.check(props["custom_chars"]["visible"], "mosaic: Custom characters shown for Custom")
        r.check(props["custom_fallback_note"]["visible"], "mosaic: fallback note shown for empty Custom")

    print()
    print("| Filter | Look | Match | Max gap | Result |")
    print("| --- | --- | --- | --- | --- |")
    for row in rows:
        print(row)
    print()
    print("%d checks, %d failures" % (r.checks, len(r.failures)))
    return 0 if not r.failures else 1


if __name__ == "__main__":
    sys.exit(main())
