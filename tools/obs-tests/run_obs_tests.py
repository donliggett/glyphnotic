"""Phase 5 tests in the OBS window (Windows; docs/SPEC.md, Build phases: Phase 5).

    python run_obs_tests.py --backup-dir DIR [--obs OBS_DIR] [--out OUT_DIR] [--seconds N] [--only TEST[,TEST...]]

Runs OBS itself, with the "Glyphnotic Test" scene collection and profile
(never the real ones), once per test, and checks its log. OBS must be closed
and the release plugin installed (tools/README.md). Before changing
anything it copies the collection and the profile's basic.ini into
--backup-dir as "<date> before Phase 5 tests - <file>" (once; an existing
backup of that name is kept). It then:

  - sets the profile to 1920x1080 at 60 fps (base and output), with
    recordings and screenshots going to OUT_DIR/recordings;
  - adds glyphnotic-tests.lua to the collection's scripts, and the scenes
    "Phase 5 perf ASCII" (three 1080p images with Ink Black, Tint and
    Phosphor), "Phase 5 perf Every Glyph" (one with Every Glyph), "Phase 5
    perf Fever x3" (three with Fever: a heavier case than the spec's),
    "Phase 5 perf none" (the three images with no filter), "Phase 5 leaks"
    and "Phase 5 settings" (three images for the settings test).

Tests (each starts OBS with GLYPHNOTIC_TEST set; see glyphnotic-tests.lua):

  perf-ascii, perf-every, perf-fever, perf-none
           record for --seconds (default 60) and report the frames drawn,
           lagged and skipped (OBS's output log) and the average render
           time. Passes with no lagged or skipped frames.
  leak-ui  add and remove a Glyphnotic filter 100 times in OBS's own Filters
           window (filters-ui.ps1: + , the filter, OK; select it, - , Yes;
           the four filters in turn), with no test script loaded. Passes with
           100 adds and removes in OBS's log and "Number of memory leaks: 0"
           at exit. It moves the mouse.
  leak     the same through the API from the Lua script, faster: each
           Glyphnotic filter added and removed 100 times (400 filters).
  persist  set filters and settings on three sources, quit OBS, start it
           again: passes when every filter's settings and each source's
           screenshot are the same after the restart, and after changing
           one filter's look only that source's picture and settings change.

Every run must also log no Glyphnotic errors or warnings and 0 leaks. The
1080p stills are written to OUT_DIR (spring, victor and sintel, scaled to
cover 1920x1080). Exits 0 when every test passes.
"""

import argparse
import datetime
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "reference"))
import imageio_min  # noqa: E402
import run_performance  # noqa: E402

APPDATA = os.environ.get("APPDATA", "")
OBS_CFG = os.path.join(APPDATA, "obs-studio")
COLLECTION = os.path.join(OBS_CFG, "basic", "scenes", "Glyphnotic_Test.json")
PROFILE = os.path.join(OBS_CFG, "basic", "profiles", "Glyphnotic_Test", "basic.ini")
LOGS = os.path.join(OBS_CFG, "logs")
SCRIPT = os.path.join(HERE, "glyphnotic-tests.lua")
TESTS = ["perf-ascii", "perf-every", "perf-fever", "perf-none", "leak-ui", "leak", "persist"]
PERF_SCENES = {"perf-ascii": "Phase 5 perf ASCII", "perf-every": "Phase 5 perf Every Glyph",
               "perf-fever": "Phase 5 perf Fever x3", "perf-none": "Phase 5 perf none"}
STILLS = os.path.join(ROOT, "tests", "stills")
TAG = "glyphnotic-tests.lua] P5 "


def uuid_for(name):
    """A fixed UUID per test source, so reruns replace rather than add."""
    import hashlib
    h = hashlib.md5(name.encode()).hexdigest()
    return "%s-%s-4%s-8%s-%s" % (h[:8], h[8:12], h[13:16], h[17:20], h[20:32])


def image_source(name, path, filters=()):
    return {"name": name, "uuid": uuid_for(name), "id": "image_source", "versioned_id": "image_source",
            "settings": {"file": path.replace("\\", "/")}, "enabled": True,
            "filters": [{"name": fname, "uuid": uuid_for(name + "/" + fname), "id": fid, "versioned_id": fid,
                         "settings": {"look": look}, "enabled": True} for fid, fname, look in filters]}


def scene(name, items):
    """items: [(source name, x, y, scale)]"""
    return {"name": name, "uuid": uuid_for(name), "id": "scene", "versioned_id": "scene",
            "settings": {"id_counter": len(items), "custom_size": False, "items": [
                {"name": n, "source_uuid": uuid_for(n), "id": i + 1, "visible": True, "locked": False, "rot": 0.0,
                 "align": 5, "pos": {"x": x, "y": y}, "scale": {"x": s, "y": s}, "bounds_type": 0,
                 "bounds": {"x": 0.0, "y": 0.0}} for i, (n, x, y, s) in enumerate(items)]},
            "enabled": True}


def test_sources(out):
    stills = [run_performance.make_still(out, n) for n in ("spring", "victor", "sintel")]
    layout = [(0.0, 0.0), (960.0, 0.0), (480.0, 540.0)]
    sources, scenes = [], []

    def three(scene_name, prefix, looks):
        items = []
        for k, (still, (x, y)) in enumerate(zip(stills, layout)):
            look = looks[k] if looks else None
            name = "%s %d%s" % (prefix, k + 1, " (Ink %s)" % look[1] if look else "")
            sources.append(image_source(name, still, [("glyphnotic_ink", "Glyphnotic Ink", look[0])] if look else []))
            items.append((name, x, y, 0.5))
        scenes.append(scene(scene_name, items))

    three("Phase 5 perf ASCII", "P5 perf", [("black", "Black"), ("tint", "Tint"), ("phosphor", "Phosphor")])
    three("Phase 5 perf Fever x3", "P5 fever", [("fever", "Fever")] * 3)
    three("Phase 5 perf none", "P5 plain", None)
    sources.append(image_source("P5 every (Ink Every Glyph)", stills[0],
                                [("glyphnotic_ink", "Glyphnotic Ink", "every_glyph")]))
    scenes.append(scene("Phase 5 perf Every Glyph", [("P5 every (Ink Every Glyph)", 0.0, 0.0, 1.0)]))
    sources.append(image_source("P5 leak", os.path.join(STILLS, "victor.png")))
    scenes.append(scene("Phase 5 leaks", [("P5 leak", 0.0, 0.0, 1.0)]))
    for name, still, x in (("P5 A", "victor.png", 0.0), ("P5 B", "sintel.png", 640.0), ("P5 C", "spring.png", 0.0)):
        sources.append(image_source(name, os.path.join(STILLS, still)))
    scenes.append(scene("Phase 5 settings", [("P5 A", 0.0, 0.0, 0.7), ("P5 B", 660.0, 0.0, 0.7),
                                             ("P5 C", 0.0, 620.0, 0.5)]))
    return sources + scenes


def backup(backup_dir):
    date = datetime.date.today().isoformat()
    for src, name in ((COLLECTION, "Glyphnotic_Test.json"), (PROFILE, "profile basic.ini")):
        dst = os.path.join(backup_dir, "%s before Phase 5 tests - %s" % (date, name))
        if not os.path.exists(dst):
            shutil.copy2(src, dst)
            print("backed up %s to %s" % (src, dst))


def set_up_profile(out):
    rec = os.path.join(out, "recordings")
    os.makedirs(rec, exist_ok=True)
    raw = open(PROFILE, "rb").read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    lines = raw.decode("utf-8-sig").splitlines()
    want = {"Video": {"BaseCX": "1920", "BaseCY": "1080", "OutputCX": "1920", "OutputCY": "1080", "FPSType": "0",
                      "FPSCommon": "60"},
            "SimpleOutput": {"FilePath": rec.replace("\\", "\\\\")}}
    section, done, result = None, set(), []

    def flush(sec):
        for k, v in want.get(sec, {}).items():
            if (sec, k) not in done:
                result.append("%s=%s" % (k, v))
                done.add((sec, k))

    for line in lines:
        m = re.match(r"\[(.+)\]$", line.strip())
        if m:
            if section is not None:
                while result and result[-1] == "":
                    result.pop()
                flush(section)
                result.append("")
            section = m.group(1)
            result.append(line)
            continue
        k = line.split("=", 1)[0]
        if section in want and k in want[section]:
            result.append("%s=%s" % (k, want[section][k]))
            done.add((section, k))
        else:
            result.append(line)
    flush(section)
    data = "\r\n".join(result) + "\r\n"
    open(PROFILE, "wb").write((b"\xef\xbb\xbf" if bom else b"") + data.encode("utf-8"))


def set_script(d, on):
    """Adds the test script to the collection's scripts (on) or takes it out."""
    scripts = [s for s in d.setdefault("modules", {}).get("scripts-tool", [])
               if os.path.normcase(os.path.normpath(s.get("path", ""))) != os.path.normcase(SCRIPT)]
    d["modules"]["scripts-tool"] = scripts + ([{"path": SCRIPT.replace("\\", "/"), "settings": {}}] if on else [])


def set_up_collection(out):
    d = json.load(open(COLLECTION, encoding="utf-8"))
    new = test_sources(out)
    names = {s["name"] for s in new}
    d["sources"] = [s for s in d["sources"] if s["name"] not in names] + new
    order = [o for o in d.get("scene_order", []) if o["name"] not in names]
    d["scene_order"] = order + [{"name": s["name"]} for s in new if s["id"] == "scene"]
    set_script(d, True)
    with open(COLLECTION, "w", encoding="utf-8") as f:
        json.dump(d, f, indent=4)


def collection_script(on):
    """With OBS closed: the test script in the collection or not (OBS saves the collection on exit)."""
    d = json.load(open(COLLECTION, encoding="utf-8"))
    set_script(d, on)
    with open(COLLECTION, "w", encoding="utf-8") as f:
        json.dump(d, f, indent=4)


def obs_running():
    p = subprocess.run(["tasklist", "/FI", "IMAGENAME eq obs64.exe", "/NH"], capture_output=True, text=True)
    return "obs64.exe" in p.stdout


def close_obs():
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-Process obs64 -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() }"],
                   capture_output=True)


def run_obs(a, test_value, timeout, scene=None, during=None):
    """Starts OBS with GLYPHNOTIC_TEST, waits for "P5 done" (or, with `during`, for the scene to load and
    then runs during()), closes it; returns the log text."""
    before = set(glob.glob(os.path.join(LOGS, "*.txt")))
    env = dict(os.environ, GLYPHNOTIC_TEST=test_value)
    bin_dir = os.path.join(a.obs, "bin", "64bit")
    # Not --disable-missing-files-check: with it, OBS 33.0.1 logs "Number of memory leaks: 1" by itself.
    args = [os.path.join(bin_dir, "obs64.exe"), "--collection", "Glyphnotic Test", "--profile", "Glyphnotic Test",
            "--disable-updater"]
    if scene:
        args += ["--scene", scene]
    proc = subprocess.Popen(args, cwd=bin_dir, env=env)
    log_path, text, t0 = None, "", time.time()
    while time.time() - t0 < timeout:
        time.sleep(1)
        if log_path is None:
            new = sorted(set(glob.glob(os.path.join(LOGS, "*.txt"))) - before, key=os.path.getmtime)
            log_path = new[-1] if new else None
            continue
        text = open(log_path, encoding="utf-8", errors="replace").read()
        if during and "Switched to scene" in text:
            time.sleep(3)
            during()
            break
        if TAG + "done" in text or TAG + "ERROR" in text or proc.poll() is not None:
            break
    else:
        print("  timed out after %d s" % timeout)
    # Close the window as a user would, so OBS saves and logs its leak count; OBS ignores
    # the request while it's still loading, so ask again until it exits.
    for _ in range(9):
        close_obs()
        try:
            proc.wait(10)
            break
        except subprocess.TimeoutExpired:
            pass
    else:
        print("  OBS didn't exit; killing it")
        proc.kill()
        proc.wait()
    time.sleep(1)
    text = open(log_path, encoding="utf-8", errors="replace").read() if log_path else ""
    print("  log: %s" % log_path)
    return text


class Checks:
    def __init__(self):
        self.failures = []

    def check(self, ok, what):
        print("  %s: %s" % ("pass" if ok else "FAIL", what), flush=True)
        if not ok:
            self.failures.append(what)
        return ok

    def common(self, name, text):
        bad = [l for l in text.splitlines() if re.search(r"\[glyphnotic\].*(error|failed|warning|cannot)", l, re.I)]
        self.check(not bad, "%s: no Glyphnotic errors or warnings %s" % (name, bad[:3]))
        self.check(TAG + "ERROR" not in text, "%s: the test script found everything it needed" % name)
        self.check(TAG + "done" in text, "%s: the test script finished" % name)
        self.check("Number of memory leaks: 0" in text, "%s: Number of memory leaks: 0" % name)


def perf(a, c, test, results):
    scene_name = PERF_SCENES[test]
    print("== %s (%s, %d s)" % (test, scene_name, a.seconds), flush=True)
    text = run_obs(a, "perf:%s:%d" % (scene_name, a.seconds), a.seconds + 120)
    c.common(test, text)
    r = {"test": test, "scene": scene_name}
    m = re.search(re.escape(TAG) + r"perf result: (\d+) frames, (\d+) lagged, average frame render time ([0-9.]+) ms"
                  r" \(sampled\), active fps ([0-9.]+)", text)
    if m:
        r.update(frames=int(m.group(1)), lagged=int(m.group(2)), render_ms=float(m.group(3)), fps=float(m.group(4)))
    m = re.search(r"Output '(\w+)': Total drawn frames: (\d+)(?: \((\d+) attempted\))?", text)
    if m:
        r["drawn"] = int(m.group(2))
        r["drawn_attempted"] = int(m.group(3) or m.group(2))
    m = re.search(r"Output '\w+': Total frames output: (\d+)", text)
    if m:
        r["output_frames"] = int(m.group(1))
    m = re.search(r"Output '\w+': Number of lagged frames due to rendering lag/stalls: (\d+)", text)
    r["output_lagged"] = int(m.group(1)) if m else 0
    m = re.search(r"number of skipped frames due to encoding lag: (\d+)/(\d+)", text)
    r["skipped"] = int(m.group(1)) if m else 0
    m = re.search(r"Loading up D3D11 on adapter (.+?) \(", text)
    r["adapter"] = m.group(1) if m else "?"
    results.append(r)
    print("  %s" % json.dumps(r))
    c.check("frames" in r and "drawn" in r, "%s: frame figures logged" % test)
    c.check(r.get("lagged", 1) == 0 and r["output_lagged"] == 0, "%s: no lagged frames" % test)
    c.check(r["skipped"] == 0, "%s: no skipped frames" % test)


def leak(a, c, results):
    print("== leak", flush=True)
    text = run_obs(a, "leak:100", 300)
    c.common("leak", text)
    m = re.search(re.escape(TAG) + r"leak test: added and removed (\d+) filters", text)
    n = int(m.group(1)) if m else 0
    c.check(n == 400, "leak: added and removed 400 filters (%d)" % n)
    results.append({"test": "leak", "filters": n, "leaks_0": "Number of memory leaks: 0" in text})


def leak_ui(a, c, results, cycles=100):
    """Filters added and removed in OBS's Filters window (filters-ui.ps1), with no test script loaded."""
    print("== leak-ui (%d filters through the Filters window)" % cycles, flush=True)
    collection_script(False)
    out = {}

    def drive():
        p = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                            os.path.join(HERE, "filters-ui.ps1"), "-Source", "P5 leak", "-Cycles", str(cycles)],
                           capture_output=True, text=True, timeout=cycles * 30 + 60)
        out["stdout"], out["stderr"], out["code"] = p.stdout, p.stderr, p.returncode

    try:
        text = run_obs(a, "", cycles * 30 + 120, scene="Phase 5 leaks", during=drive)
    finally:
        collection_script(True)
    c.check(out.get("code") == 0 and "filters-ui: done %d" % cycles in out.get("stdout", ""),
            "leak-ui: the UI script added and removed %d filters %s" % (cycles, out.get("stderr", "")[-500:]))
    added = len(re.findall(r"User added filter '.*' \(glyphnotic_\w+\) to source 'P5 leak'", text))
    removed = len(re.findall(r"User removed filter '.*' \(glyphnotic_\w+\) from source 'P5 leak'", text))
    c.check(added == cycles and removed == cycles, "leak-ui: OBS logged %d adds and %d removes" % (added, removed))
    bad = [l for l in text.splitlines() if re.search(r"\[glyphnotic\].*(error|failed|warning|cannot)", l, re.I)]
    c.check(not bad, "leak-ui: no Glyphnotic errors or warnings %s" % bad[:3])
    c.check("Number of memory leaks: 0" in text, "leak-ui: Number of memory leaks: 0")
    results.append({"test": "leak-ui", "added": added, "removed": removed,
                    "leaks_0": "Number of memory leaks: 0" in text})


def parse_persist(text):
    """{tag: {'settings': {(source, filter): dict}, 'shots': {source: png path}}}"""
    out, settings, pending = {}, {}, []
    for line in text.splitlines():
        m = re.search(re.escape(TAG) + r"settings (.+?) / (.+?): (\{.*\})\s*$", line)
        if m:
            settings[(m.group(1), m.group(2))] = json.loads(m.group(3))
            continue
        m = re.search(re.escape(TAG) + r"settings logged (.+)$", line)
        if m:
            out.setdefault(m.group(1).strip(), {})["settings"] = settings
            settings = {}
            continue
        m = re.search(re.escape(TAG) + r"screenshot (\w+) (.+)$", line)
        if m:
            pending.append((m.group(1), m.group(2).strip()))
            continue
        m = re.search(r"Saved screenshot to '(.+)'", line)
        if m and pending:
            tag, source = pending.pop(0)
            out.setdefault(tag, {}).setdefault("shots", {})[source] = m.group(1)
    return out


def same_png(p, q):
    a, b = imageio_min.read_png(p), imageio_min.read_png(q)
    return a.shape == b.shape and bool((a == b).all())


def persist(a, c, results):
    print("== persist (set, then restart)", flush=True)
    t1 = run_obs(a, "persist-set", 180)
    c.common("persist-set", t1)
    t2 = run_obs(a, "persist-check", 180)
    c.common("persist-check", t2)
    s1, s2 = parse_persist(t1), parse_persist(t2)
    before = s1.get("after set", {}).get("settings", {})
    restart = s2.get("after restart", {}).get("settings", {})
    changed = s2.get("after changing A", {}).get("settings", {})
    shots = {"set": s1.get("set", {}).get("shots", {}), "restart": s2.get("restart", {}).get("shots", {}),
             "changed": s2.get("changed", {}).get("shots", {})}
    c.check(len(before) == 3 and len(restart) == 3 and len(changed) == 3, "persist: three filters' settings logged")
    for key, v in before.items():
        c.check(restart.get(key) == v, "persist: %s / %s settings the same after the restart" % key)
    ink = before.get(("P5 A", "P5 Ink"), {})
    con = before.get(("P5 B", "P5 Console"), {})
    c.check(ink.get("custom_chars") == " .:-=+*#%@" and ink.get("cell_scale") == 2 and ink.get("look") == "custom",
            "persist: A's custom settings were applied (custom characters, cell scale 2, look Custom)")
    c.check(con.get("palette") == "custom" and "palette_0" in con, "persist: B's captured palette is saved")
    for src in ("P5 A", "P5 B", "P5 C"):
        ok = src in shots["set"] and src in shots["restart"] and same_png(shots["set"][src], shots["restart"][src])
        c.check(ok, "persist: %s draws the same after the restart" % src)
    a_changed = changed.get(("P5 A", "P5 Ink"), {})
    c.check(a_changed.get("look") == "phosphor" and a_changed.get("ink_color") == "green",
            "persist: A's look changed to Phosphor")
    for key in (("P5 B", "P5 Console"), ("P5 C", "P5 Poster")):
        c.check(changed.get(key) == restart.get(key), "persist: %s / %s settings unchanged by A's change" % key)
    for src, same in (("P5 A", False), ("P5 B", True), ("P5 C", True)):
        ok = src in shots["changed"] and src in shots["restart"] and \
            same_png(shots["changed"][src], shots["restart"][src]) == same
        c.check(ok, "persist: %s's picture %s after A's change" % (src, "unchanged" if same else "changed"))
    results.append({"test": "persist", "settings_before": {"%s / %s" % k: v for k, v in before.items()},
                    "shots": shots})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--backup-dir", required=True)
    ap.add_argument("--obs", default=r"C:\Program Files\obs-studio")
    ap.add_argument("--out", default=os.path.join(ROOT, "build_obs_tests"))
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--only")
    a = ap.parse_args()
    a.out = os.path.abspath(a.out)
    os.makedirs(a.out, exist_ok=True)
    if sys.platform != "win32":
        print("Windows only")
        return 2
    if obs_running():
        print("close OBS first")
        return 2
    if not os.path.isdir(a.backup_dir):
        print("no backup folder %s" % a.backup_dir)
        return 2
    backup(a.backup_dir)
    set_up_profile(a.out)
    set_up_collection(a.out)
    c, results = Checks(), []
    for t in (a.only.split(",") if a.only else TESTS):
        if t in PERF_SCENES:
            perf(a, c, t, results)
        elif t == "leak":
            leak(a, c, results)
        elif t == "leak-ui":
            leak_ui(a, c, results)
        elif t == "persist":
            persist(a, c, results)
        elif t.split(":")[0] == "idle":
            # A baseline: OBS shows a scene (default "Phase 5 leaks") and closes.
            print("== %s" % t, flush=True)
            c.common(t, run_obs(a, t if ":" in t else "idle:Phase 5 leaks:10", 120))
        else:
            print("unknown test %s" % t)
            return 2
    with open(os.path.join(a.out, "results.json"), "w") as f:
        json.dump(results, f, indent=1)
    if c.failures:
        print("\n%d failures" % len(c.failures))
        return 1
    print("\nall tests pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
