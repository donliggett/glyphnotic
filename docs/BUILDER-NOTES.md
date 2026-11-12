# Builder notes

How Glyphnotic is built and tested in practice, from Phases 0–5. Commands
over prose; the why is in `docs/build-history/`.

## Standing rules

- **Before starting a phase, read:** this file, the previous phase's report
  (`docs/build-history/phase-N.md`), and the spec sections the current
  phase's boxes name (`docs/SPEC.md`, Build phases and acceptance).
- **Before stopping at the review gate:** write the phase report, update the
  spec and tick the boxes, run the Linux check (below), and **update this
  file** with anything the phase changed or taught.
- **After approval:** commit, push, and confirm CI is green before the next
  phase.
- Never use the real OBS scene collection or profile; back up the test ones
  before changing them (below).

## Shell

Build and run tests from **PowerShell** with Visual Studio's CMake first on
`PATH`. Git Bash finds an older CMake (`C:\Strawberry\c\bin\cmake.exe`) that
defaults to Ninja and can't generate Visual Studio 2026 projects, and the
build folders were configured with the VS 18 generator.

```powershell
$env:PATH = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$env:PATH"
```

Bash is fine for git, `gh`, `docker` and the Python scripts. For multi-line
edit scripts, write a `.py` file (in the session scratchpad) and run it;
heredocs in the agent's Bash tool have mangled backslashes, and one holding
Python's `'''` strings failed to parse at all.

## Build (Windows, repository root)

| Folder | What | Configure | Build |
| --- | --- | --- | --- |
| `build_x64` | release plugin | `cmake --preset windows-x64` | `cmake --build build_x64 --config RelWithDebInfo` |
| `build_dbg` | debug tools: Dump grid button, `glyphnotic-harness` | `cmake --preset windows-x64 -B build_dbg -DGLYPHNOTIC_DEBUG_TOOLS=ON` | `cmake --build build_dbg --config RelWithDebInfo` |
| `build_tests` | unit tests (no OBS) | `cmake -S tests/unit -B build_tests` | `cmake --build build_tests --config Release` |

```powershell
ctest --test-dir build_tests -C Release --output-on-failure
```

Release install, then into the test plugin folder (OBS closed):

```powershell
cmake --install build_x64 --prefix release/RelWithDebInfo --config RelWithDebInfo
```

```powershell
Copy-Item -Recurse -Force release\RelWithDebInfo\glyphnotic\* C:\ProgramData\obs-studio\plugins\glyphnotic\
```

## Python environment

`.venv` at the repository root (git-ignored), from
`tools/reference/requirements.txt`: numpy 2.5.4, clang-format 19.1.7,
gersemi 0.21.0.

```powershell
py -m venv .venv
```

```powershell
.venv\Scripts\python -m pip install -r tools\reference\requirements.txt
```

Format, then check as CI does:

```powershell
.venv\Scripts\clang-format -i src\glyph-filter.cpp
```

```powershell
.venv\Scripts\clang-format --dry-run --Werror (Get-ChildItem src -Recurse -Include *.c,*.cpp,*.h,*.hpp).FullName
```

```powershell
.venv\Scripts\gersemi --check CMakeLists.txt cmake tests\unit\CMakeLists.txt
```

## Harness and reference suites

From `tools\reference`, against the debug build. Effects load from the
repository's `data\`, so shader edits need no rebuild; don't rebuild
`build_dbg` while a suite runs (the harness loads its DLL).

| Suite | Command | Time |
| --- | --- | --- |
| Stills, D3D11 | `..\..\.venv\Scripts\python run_stills.py --build ..\..\build_dbg\RelWithDebInfo` | ~4 min |
| Stills, OpenGL | `... run_stills.py --build ..\..\build_dbg\RelWithDebInfo --renderer opengl --out ..\..\build_stills_gl` | ~4 min |
| Looks and properties | `... run_looks.py --build ..\..\build_dbg\RelWithDebInfo` (add `--renderer opengl --out ..\..\build_looks_gl`) | ~2 min |
| Ink levels over time | `... run_levels.py --build ..\..\build_dbg\RelWithDebInfo` (same for OpenGL) | <1 min |
| Color paths | `... run_color_paths.py --build ..\..\build_dbg\RelWithDebInfo` | ~1 min |
| Motion over time | `... run_motion.py --build ..\..\build_dbg\RelWithDebInfo` (add `--renderer opengl --out ..\..\build_motion_gl`) | ~11 min |
| Tuning measurements (Phase 4; not pass/fail) | `... run_tuning.py --build ..\..\build_dbg\RelWithDebInfo` (suites hold, noise, boil, ranges, looks) | ~15 min |
| GPU time at 1080p60 (Phase 5; not pass/fail) | `... run_performance.py --build ..\..\build_dbg\RelWithDebInfo` (add `--renderer opengl --out ..\..\build_perf_gl`; suites looks, scale, charsets, instances) | ~13 min |

One case: `run_stills.py ... --only ink-victor`; some motion suites:
`run_motion.py ... --only boil,jump` (suites: source, flicker, still, boil,
jump, hold, settle; `--flicker-frames 150` for a quick flicker run). Every
suite exits 0 when all pass and must pass on both renderers
(`run_tuning.py` only measures; after changing a look, redraw
`docs/looks/` with `run_tuning.py ... --only looks`, 1 min). The two
renderers' chains can run at the same time with separate `--out` folders
(about 40 minutes each, all suites). Harness options, the dump and the
recording format: `tools/README.md`.

Every `run_stills.py` case runs with motion off unless it sets a motion
setting: a filter created without a look takes the first look's values as
defaults, including Soften 0.6. Scripts that compare with the reference must
set `soften`, `hold`, `settle`, `boil` themselves.

## OBS by hand

Back up the test collection and profile first, to `D:\Repos\Glyphnotic_backups\`
(outside the repository), named `<date> before <what> - <file>`:

```powershell
Copy-Item "$env:APPDATA\obs-studio\basic\scenes\Glyphnotic_Test.json" "D:\Repos\Glyphnotic_backups\2026-11-11 before Phase 3 tests - Glyphnotic_Test.json"
```

Launch (the test collection and profile, never the real ones):

```powershell
Start-Process -FilePath "C:\Program Files\obs-studio\bin\64bit\obs64.exe" -WorkingDirectory "C:\Program Files\obs-studio\bin\64bit" -ArgumentList '--collection "Glyphnotic Test" --profile "Glyphnotic Test"'
```

Close cleanly and check the newest log for errors and leaks:

```powershell
(Get-Process obs64).CloseMainWindow()
```

```powershell
Select-String -Path (Get-ChildItem "$env:APPDATA\obs-studio\logs" | Sort-Object LastWriteTime | Select-Object -Last 1).FullName -Pattern "\[glyphnotic\]|memory leaks"
```

Scene files can be edited only with OBS closed. Without a mouse, Windows UI
Automation plus `SetCursorPos`/`mouse_event` can drive the Filters window
(Phase 2 did, to test a slider drag); make the window tall first, since
controls scrolled out of view report off-window rectangles.

## OBS-window tests (Phase 5)

`tools/obs-tests/run_obs_tests.py` drives OBS by itself (no mouse needed):
it backs up the test collection and profile, sets the profile to 1080p60,
adds its scenes and `glyphnotic-tests.lua` to the collection, and starts
OBS once per test with `GLYPHNOTIC_TEST` set; the Lua script runs the test
and logs `P5 ...` lines, the driver closes OBS and reads the log. Install
the release build first (above), OBS closed, GPU otherwise idle:

```powershell
.venv\Scripts\python tools\obs-tests\run_obs_tests.py --backup-dir D:\Repos\Glyphnotic_backups
```

About 12 minutes (`--only leak-ui,persist` for some); `leak-ui` clicks
through the Filters window with the real mouse for about 3 minutes.
`--only idle` starts and closes OBS with the script loaded (a baseline). The profile stays at
1920×1080, 60 fps afterwards (it was 1280×720 output at 30 fps before
Phase 5; the backup has it).

## Linux check (end of every phase)

Docker Desktop, from the repository root in Git Bash:

```bash
mkdir -p build_linux_check
```

```bash
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD:/src:ro" -v "$PWD/build_linux_check:/out" ubuntu:26.04 bash /src/tools/linux-check/obs-xvfb-check.sh
```

```bash
.venv/Scripts/python tools/linux-check/xwd_to_png.py build_linux_check/screen.xwd build_linux_check/screen.png
```

About 5 minutes. It builds with GCC (warnings as errors), runs the unit
tests, runs OBS 33 under `xvfb-run` with each filter on an image source (and
Ink's boiling looks Static and Fever, from Phase 3), and checks the log;
`build_linux_check/result.txt` lists the checks. Exit 0 = pass. Add any new
filter or code path worth a GLSL check to the script's test collection. If
it stops in "Installing OBS 33" with an SSL error from Launchpad, that's the
network: run it again.

### Linux install test (Phase 5, and before a release)

Same Docker setup; it installs and removes the .deb and the tarball by
following `docs/INSTALL.md` (built here, or CI's mounted at `/pkgs`):

```bash
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD:/src:ro" -v "$PWD/build_install_check:/out" ubuntu:26.04 bash /src/tools/linux-check/install-check.sh
```

About 6 minutes; `build_install_check/install-result.txt` lists the checks.

## CI

Windows and Ubuntu build on every push to `main` (macOS on tags and manual
runs only). After pushing:

```bash
gh run list --branch main --limit 3
```

```bash
gh run watch <run id> --exit-status
```

`gh run list --commit` needs the full SHA; with a short one it finds nothing.

Pushes to `main` and manual runs (`gh workflow run dispatch.yaml --ref main
-f job=build`, which adds macOS, about 5 minutes) package everything,
including the Windows installer (Inno Setup, installed on the runner with
Chocolatey). Releases: `docs/BUILDING.md`, Making a release. A tag
`v<version>` must match `buildspec.json`'s version or the release job
fails. New executable scripts need `git add --chmod=+x` (the checkout is on
Windows).

## Lessons that cost time

- **OpenGL:** every pixel shader must read its `uv`. A pass that doesn't
  loses the attribute in GLSL and libobs's draw fails (`device_draw (GL)
  failed`) while Direct3D works. Always run the OpenGL suites.
- **Exact ties:** wherever many glyphs draw the same picture, compute the
  cost so the tie is exact on both GPU and reference (sums relative to the
  cell's first counted sample; glyph-independent forms), or matches drop.
- **Properties window:** a modified callback that returns true rebuilds the
  window and drops a slider drag; OBS's Filters window updates the filter on
  every drag step. Defer refreshes (see `filter_update`/`filter_tick`).
- **Harness `--ui-set`** with `--ui-no-update` tests the properties callbacks
  alone; without it the filter's own update can hide a broken callback.
- **Settings keys and values are saved in scene files**: fixed once shipped
  in a phase (lists in each phase report).
- **Direct3D: "Not all shader parameters were set"** fails the draw when a
  uniform that a technique's shaders reference was never set, even on a
  path that doesn't use it (a shared helper counts). OpenGL doesn't
  complain, so set every uniform of an effect on every path.
- **Integer hashing works in effects** on both renderers: `uint`, `u`
  literals, `>>`, `^`. GLSL 3.30 doesn't mix `int` and `uint` in
  arithmetic, so convert explicitly (`uint(x)`).
- **Cost scales differ by orders of magnitude between modes** (median cost
  range on victor: Ink 0.3, Console 0.014, Poster 9e-6, Mosaic 3e-6). Test
  tolerances on costs must be relative to the cell's range; an absolute
  1e-6 hid real behavior in Mosaic.
- **Hold can keep every glyph through a change**, so don't find a switch
  frame from glyph changes: the recording's header has the frame's mean
  lightness, which identifies the still.
- **Light jump needs a big change:** halving victor's encoded values changes
  only 46% of cells by more than 0.12 (no jump); 0.3× or a different still
  (a crop of sintel: 69%) is one.
- **Sweeping a static setting needs no OBS**: the reference matches the
  plugin on a fresh pick, so `tuning.py` draws its pictures (contact
  sheets, crops) in seconds. Only motion (Hold, Boil, Settle) needs the
  harness.
- **Glyph-change counts mislead in Ink**: ASCII has so many near-ties that
  a Gamma step of 0.1 changes 80% of glyphs while the tone barely moves.
  Measure tone (space and densest-glyph shares), fit or drawn color.
- **Cost-range shares don't compare across modes** (Ink's ranges are set
  by the extreme glyphs, Mosaic's next-best glyph sits near the worst):
  compare modes by glyph changes per frame or by the picture.
- **σ 2/255 noise is a quiet camera.** At σ 4/255 Hold 10 lets 0.4–5% of
  cells change per frame; the looks use Hold 25 (Phase 4).
- **A lone filter's GPU time is noisy**: on an otherwise idle GPU one
  instance timed 1.8–3.0 ms from run to run where the same filter costs
  1.2–1.4 ms per instance under load (likely short bursts of work running
  below full clocks; `nvidia-smi` sampling once a second can't show it). Time three
  instances in one scene and divide the scene's time (`run_performance.py`
  does).
- **A look named in `--settings` overrides the settings beside it** (a
  newly named look fills in its settings): `{"look": "black", "charset":
  "every"}` runs ASCII. To vary a look's setting, pass the look's values
  without `look`.
- **Heredocs in the agent's Bash tool eat backslashes**, also in quoted
  (`<<'EOF'`) heredocs: `\n` and Windows paths came out as newlines,
  carriage returns and backspaces. Write edit scripts with the Write tool
  and run them.
- **`--disable-missing-files-check` makes OBS 33.0.1 leak** one allocation
  by itself ("Number of memory leaks: 1" with no Glyphnotic filter in
  sight). Don't start OBS with it for a leak check. `--disable-shutdown-check`
  doesn't exist on Windows; after OBS is killed it asks about Safe Mode on
  the next start: choose to run normally.
- **Lua test scripts: no frontend event callback while ticking.** Steps run
  in `script_tick` on the graphics thread; `obs_frontend_set_current_scene`
  there waits for the UI thread, which waits for the script's lock to
  deliver the scene-change event: OBS hangs. Poll state instead.
- **UI Automation on OBS (Qt)**: handles go stale as Qt rebuilds its
  accessibility tree and clicks can land mid-layout; look controls up on
  every use and retry a click whose result doesn't appear
  (`filters-ui.ps1`). Invoking a button that opens a menu or dialog blocks;
  click with the mouse instead.
- **Slider ranges are UI only** (`src/settings.hpp`): scene files and
  scripts may set wider values; only motion settings and Poster's Levels
  are clamped. `run_looks.py` checks the ranges against the spec.
