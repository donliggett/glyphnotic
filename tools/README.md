# Tools

Test tools for Glyphnotic. None of this ships with the plugin.

| Path | What it is |
| --- | --- |
| `harness/harness.cpp` | `glyphnotic-harness`: runs Glyphnotic filters on a still image (or a sequence of stills, with fresh noise on every frame) in a headless libobs (no OBS window); writes the grid dump, a per-frame recording, output frames or OBS's canvas texture, and lag and GPU-time figures |
| `reference/reference.py` | The reference implementation (Python + NumPy, float64) of Soften, Sample, Cell stats, Ink's levels and Score for all four modes (a fresh pick: Hold, Settle and Boil act over time), written from `docs/SPEC.md` |
| `reference/compare.py` | Compares one grid dump (and output frame) with the reference |
| `reference/run_stills.py` | Runs the harness and `compare.py` over every test still, for every mode, and prints a table |
| `reference/run_looks.py` | Checks the looks (present, applied from settings and from the properties window, Custom on change) and which properties each filter shows |
| `reference/run_levels.py` | Checks Ink's Auto levels over time (easing, snapping on a size change and a light jump) and Manual levels |
| `reference/run_motion.py` | Checks the motion settings over time: flicker under noise for every look, Boil 0 on a still, Boil's rate, linger and spread, Light jump, Hold against the reference's costs, Settle |
| `reference/run_tuning.py` | Phase 4's tuning measurements: Hold on a still and a panning still under noise, the looks under heavier noise, Boil spread's cost ranges, the static sliders' sweeps; and draws the stills in `docs/LOOKS.md` |
| `reference/tuning.py` | Helpers for `run_tuning.py`: a fresh pick drawn from the reference, crops, contact sheets |
| `reference/run_performance.py` | Phase 5's GPU-time measurements at 1920×1080, 60 fps: every look, cell scales, character sets, and how the time grows with instances (`docs/PERFORMANCE.md`) |
| `obs-tests/run_obs_tests.py`, `obs-tests/glyphnotic-tests.lua`, `obs-tests/filters-ui.ps1` | Phase 5's tests in the OBS window (Windows): three Ink/ASCII instances and one Every Glyph instance at 1080p60 with no lagged or skipped frames, adding and removing filters 100 times in the Filters window (and 400 through the API) with 0 leaks, settings surviving a restart with instances staying independent |
| `linux-check/obs-xvfb-check.sh` | The end-of-phase Linux check: OBS 33 under `xvfb-run` in an Ubuntu 26.04 container with every filter |
| `linux-check/install-check.sh` | Phase 5's Linux install test: the .deb and the tarball installed and removed by following `docs/INSTALL.md` |
| `reference/run_color_paths.py` | Checks colors along OBS's other color paths: Color Correction before and after, 10-bit and HDR canvases, 16-bit and HDR sources |
| `reference/make_stills.py` | Rebuilds `tests/stills/` (see `tests/stills/ATTRIBUTION.md`) |
| `reference/imageio_min.py` | PNG/PAM/BMP reading and PNG writing with only NumPy |
| `reference/requirements.txt` | Glyphnotic's Python packages, pinned: NumPy for the scripts, and clang-format and gersemi for formatting |

The harness needs a debug build of the plugin and an installed OBS of the
version in `buildspec.json` (33.0.1). The Python scripts need Python 3.12 or
later and the packages in `requirements.txt`, in a virtual environment of
Glyphnotic's own. The same environment holds the formatters (see
[Formatting](#formatting)). The commands below run from the repository root unless
they say otherwise. `.venv/` and the `build_*` folders there are ignored by
git.

## Windows

Python environment (once):

```powershell
py -m venv .venv
```

```powershell
.venv\Scripts\python -m pip install -r tools\reference\requirements.txt
```

Debug build, in its own folder so the release build stays as it is:

```powershell
cmake --preset windows-x64 -B build_dbg -DGLYPHNOTIC_DEBUG_TOOLS=ON
```

```powershell
cmake --build build_dbg --config RelWithDebInfo
```

This gives `build_dbg\RelWithDebInfo\glyphnotic.dll` and
`glyphnotic-harness.exe`. The harness links libobs from `.deps` and runs
against the OBS installed in `C:\Program Files\obs-studio` (`--obs` to change
it). It needs OBS's `bin\64bit` folder on `PATH` for `obs.dll`; the scripts
set that.

Reference comparison, from `tools\reference`:

```powershell
..\..\.venv\Scripts\python run_stills.py --build ..\..\build_dbg\RelWithDebInfo
```

```powershell
..\..\.venv\Scripts\python run_stills.py --build ..\..\build_dbg\RelWithDebInfo --renderer opengl --out ..\..\build_stills_gl
```

```powershell
..\..\.venv\Scripts\python run_looks.py --build ..\..\build_dbg\RelWithDebInfo
```

```powershell
..\..\.venv\Scripts\python run_levels.py --build ..\..\build_dbg\RelWithDebInfo
```

```powershell
..\..\.venv\Scripts\python run_color_paths.py --build ..\..\build_dbg\RelWithDebInfo
```

```powershell
..\..\.venv\Scripts\python run_motion.py --build ..\..\build_dbg\RelWithDebInfo
```

The second command runs OBS's OpenGL renderer on Windows, which checks the
effects' GLSL translation without a Linux machine. `run_looks.py`,
`run_levels.py` and `run_motion.py` take `--renderer opengl` too (with
`--out` set to a folder of their own). `run_tuning.py` measures for tuning rather than checks (its suites:
`hold` ~8 min, `noise` ~3 min, `boil` and `ranges` ~3 min without OBS,
`looks` ~1 min, which writes `docs/looks/`); `--only looks` after a change
to a look. `run_motion.py` takes about 11
minutes, most of it the flicker suite (20 runs of 600 frames at 30 fps);
`--only boil,jump` runs some of its suites, and `--flicker-frames 150` a
shorter flicker test.

## Linux

Python environment (once; on Ubuntu, `sudo apt install python3-venv` first):

```bash
python3 -m venv .venv
```

```bash
.venv/bin/pip install -r tools/reference/requirements.txt
```

Debug build, with OBS 33 and the build dependencies from
[BUILDING.md](../docs/BUILDING.md#linux) installed:

```bash
cmake --preset ubuntu-x86_64 -B build_dbg -DGLYPHNOTIC_DEBUG_TOOLS=ON
```

```bash
cmake --build build_dbg
```

This gives `build_dbg/glyphnotic.so` and `build_dbg/glyphnotic-harness`,
which runs against the OBS installed under `/usr` (`--obs` to change it). It
needs an X display; on a machine without one, run the scripts under
`xvfb-run -a`.

Reference comparison, from `tools/reference`:

```bash
../../.venv/bin/python run_stills.py --build ../../build_dbg
```

```bash
../../.venv/bin/python run_looks.py --build ../../build_dbg
```

```bash
../../.venv/bin/python run_levels.py --build ../../build_dbg
```

```bash
../../.venv/bin/python run_color_paths.py --build ../../build_dbg
```

```bash
../../.venv/bin/python run_motion.py --build ../../build_dbg
```

The Linux steps follow the Windows ones and the harness has Linux code
paths, but they haven't been run yet (see `docs/build-history/phase-1.md`).

## Linux check in OBS (end of every phase)

From Phase 3 on, every phase ends with this check (`docs/SPEC.md`, Build
phases). `linux-check/obs-xvfb-check.sh` runs in a fresh `ubuntu:26.04`
container: it installs OBS 33 from OBS's PPA, builds the plugin as CI does
(warnings as errors) and runs the unit tests, installs it to `/usr`, runs
OBS under `xvfb-run` as an ordinary user on a test collection with one
image source per Glyphnotic filter (Ink, Mosaic, Poster, Console, each on
its first look, plus Ink's boiling looks Static and Fever), stops OBS with
SIGINT after 40 s so it shuts down cleanly,
and checks OBS's log: the plugin loaded, all four filters were created, no
Glyphnotic errors, no failed draws, "Number of memory leaks: 0".

On Windows with Docker Desktop, from the repository root (Git Bash; in
PowerShell drop `MSYS_NO_PATHCONV=1`):

```bash
mkdir -p build_linux_check
```

```bash
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD:/src:ro" -v "$PWD/build_linux_check:/out" ubuntu:26.04 bash /src/tools/linux-check/obs-xvfb-check.sh
```

It takes about 5 minutes and exits with 0 when every check passes.
`build_linux_check/` then holds `result.txt` (the checks), `obs.log` and
`screen.xwd`, the X screen with OBS's window, which
`linux-check/xwd_to_png.py` turns into a PNG:

```bash
.venv/Scripts/python tools/linux-check/xwd_to_png.py build_linux_check/screen.xwd build_linux_check/screen.png
```

The container is removed afterwards (`--rm`); the `ubuntu:26.04` image
(163 MB) stays cached for the next phase. On a Linux machine, the same
`docker run` works with `$PWD` as is.

## Linux install test (Phase 5, and before a release)

`linux-check/install-check.sh` installs and removes the packages exactly as
`docs/INSTALL.md` says, as an ordinary user with sudo in a fresh
`ubuntu:26.04` container: `sudo apt install ./<deb>`, OBS under `xvfb-run`
on a collection with Glyphnotic Ink on an image source, `sudo apt remove
glyphnotic`, OBS again; then the tarball extracted into
`~/.local/share/obs-studio/plugins`, OBS, the folder deleted, OBS again.
With the plugin installed it checks that OBS loaded it (with the package's
version), created the filter, logged no Glyphnotic errors and 0 leaks;
after removing it, that OBS didn't load it and no files are left. It builds
the packages with CI's scripts (`build-ubuntu`, `package-ubuntu --package`),
or tests the ones in a folder mounted at `/pkgs` (CI's, downloaded with
`gh run download`):

```bash
mkdir -p build_install_check
```

```bash
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD:/src:ro" -v "$PWD/build_install_check:/out" ubuntu:26.04 bash /src/tools/linux-check/install-check.sh
```

```bash
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD:/src:ro" -v "$PWD/build_install_check:/out" -v "$PWD/build_ci_packages:/pkgs:ro" ubuntu:26.04 bash /src/tools/linux-check/install-check.sh
```

About 6 minutes. `build_install_check/` then holds `install-result.txt`,
OBS's four logs and (when it built them) `packages/`.

## Performance (Phase 5)

`reference/run_performance.py` times the filter's GPU work with OBS's
source profiler (libobs GPU timer queries) through the harness, on 1080p
stills at 60 fps; `docs/PERFORMANCE.md` explains the method and has the
figures. From `tools\reference`, with the GPU otherwise idle (about 13
minutes per renderer):

```powershell
..\..\.venv\Scripts\python run_performance.py --build ..\..\build_dbg\RelWithDebInfo
```

```powershell
..\..\.venv\Scripts\python run_performance.py --build ..\..\build_dbg\RelWithDebInfo --renderer opengl --out ..\..\build_perf_gl
```

`--only looks,instances` runs some of its suites, `--frames` changes the
600 frames per case.

## Tests in the OBS window (Phase 5, Windows)

`obs-tests/run_obs_tests.py` runs OBS itself, with the "Glyphnotic Test"
scene collection and profile only, once per test, and reads its log. Close
OBS and install the release build in
`C:\ProgramData\obs-studio\plugins\glyphnotic\` first. It backs up the
collection and the profile's `basic.ini` into `--backup-dir` (once per day:
`<date> before Phase 5 tests - <file>`), then sets the profile to 1920×1080
at 60 fps with recordings in `build_obs_tests\recordings\`, adds its scenes
("Phase 5 ...") and `obs-tests/glyphnotic-tests.lua` to the collection, and
starts OBS with `GLYPHNOTIC_TEST` set so the script runs the test (the
script does nothing without it). From the repository root (about 12
minutes):

```powershell
.venv\Scripts\python tools\obs-tests\run_obs_tests.py --backup-dir <a folder outside the repository>
```

Tests: `perf-ascii` (Ink Black, Tint and Phosphor on three 1080p images),
`perf-every` (one Every Glyph), `perf-fever` (three Fever: heavier than the
spec asks), `perf-none` (the three images without a filter), each recording
for 60 s (`--seconds`) and passing with no lagged or skipped frames;
`leak-ui` (a filter added and removed 100 times in OBS's Filters window
by `filters-ui.ps1`, with Windows UI Automation and real mouse clicks, no
script loaded; "Number of memory leaks: 0" at exit); `leak` (each filter
added and removed 100 times through the API from the script); `persist` (settings set on three sources as the properties
window sets them, Capture palette included; OBS restarted; the same
settings and pictures after the restart; changing one filter's look changes
only that source). `--only leak,persist` runs some. Results go to
`build_obs_tests/results.json`; OBS's windows open and close by themselves,
so leave the mouse and keyboard alone while it runs. It doesn't start OBS
with `--disable-missing-files-check`: with that flag OBS 33.0.1 logs one
leak of its own.

## Formatting

CI checks C/C++ under `src/` with clang-format and the CMake files with
gersemi, at the versions pinned in `requirements.txt` (clang-format 19.1.7,
gersemi 0.21.0, the versions in OBS's Homebrew tap). They're installed in
`.venv` with the rest of the packages. Format the files you changed (the
harness and the unit tests use the same style), for example on Windows:

```powershell
.venv\Scripts\clang-format -i src\glyph-filter.cpp src\modes.cpp
```

```powershell
.venv\Scripts\gersemi -i CMakeLists.txt tests\unit\CMakeLists.txt
```

Check everything CI checks, without changing files:

```powershell
.venv\Scripts\clang-format --dry-run --Werror (Get-ChildItem src -Recurse -Include *.c,*.cpp,*.h,*.hpp).FullName
```

```powershell
.venv\Scripts\gersemi --check CMakeLists.txt cmake
```

On Linux the same commands run from `.venv/bin/`.

## What the scripts report

Results go to `build_stills/`, `build_looks/`, `build_levels/`,
`build_motion/` (or `--out`), `build_color_paths/` and `build_perf/` at the repository
root: grid dumps, recordings (`.rec`), frames as `.pam`, settings and
properties as JSON, `results.json`. Each script
prints a Markdown table and exits with 0 when every case passes. One case
alone: `run_stills.py --only victor-ascii` (or `--only ink-` for one mode). One dump by hand:

```powershell
..\..\.venv\Scripts\python compare.py ..\..\tests\stills\victor.png ..\..\build_stills\victor-ascii-1.json --frame ..\..\build_stills\victor-ascii-1.pam
```

In OBS itself, a debug build's filter shows **Dump grid (debug)** and
**Record grid (debug)** buttons (Record grid needs the environment variables
below, so it's meant for the harness).
It writes `grid-dump.json` to OBS's plugin config folder
(`%APPDATA%\obs-studio\plugin_config\glyphnotic\` on Windows,
`~/.config/obs-studio/plugin_config/glyphnotic/` on Linux), or to the path in
the `GLYPHNOTIC_DUMP_PATH` environment variable.

## The harness

```
glyphnotic-harness --obs <dir> --plugin <module> --plugin-data <dir>
         (--image <png> | --hdr-image <png> [--hdr-scale <x>])
         [--filter <id> [--settings <json>]]...  [--instances <n>] [--on-scene]
         [--renderer d3d11|opengl] [--format bgra|i010|p010] [--colorspace srgb|709|2100pq|2100hlg]
         [--range full|partial] [--fps <n>] [--frames <n>] [--profile] [--verbose]
         [--dump <json>] [--frame <pam>] [--main-texture <raw>] [--check-visible <property>]
         [--ui-set <json> [--ui-no-update]] [--press <button>]... [--wait-frames <n>]
         [--settings-out <json>] [--properties-out <json>]
         [--then-image <png> --then-frames <n> --dump2 <json>]
         [--also <png>]... [--period <frames>] [--noise <sigma>] [--noise-seed <n>] [--frame-sequence]
         [--record <bin> --record-frames <n> [--record-colors]] [--frame-pair <prefix>]
```

It starts libobs with the still's size as the canvas (rounded up to a
multiple of 4; OBS's output crashes on odd sizes) and loads OBS's image
source, OBS's own filters when a `--filter` isn't Glyphnotic's, and the
plugin. `--plugin-data` can point at the repository's `data/` folder, so
effect changes need no rebuild.

- `--filter` (repeatable, each optionally followed by `--settings`) builds a
  filter chain in that order on the image, or on a scene holding it with
  `--on-scene`. `--instances n` makes n images, each with its own chain, in
  one scene.
- `--hdr-image` uses a test-only HDR source instead of OBS's image source:
  the PNG decoded to linear light, times `--hdr-scale`, as a half-float
  texture in Rec. 709 extended (1.0 = SDR white).
- `--format`, `--colorspace` and `--range` set the canvas like OBS's
  Advanced video settings.
- `--dump` presses Dump grid on the first Glyphnotic filter; `--frame` saves
  one output frame (8-bit canvas only); `--main-texture` saves OBS's canvas
  texture: a line `GHRAW <w> <h> <bgra8|rgba8|rgba16f|rgba32f>` and the rows.
  SDR canvases hold sRGB-encoded values, also in float formats; Rec. 2100
  canvases hold linear Rec. 709 values.
- `--fps` and `--frames` run that many frames and print the lagged ones;
  `--profile` prints OBS's per-source GPU times (OBS's source profiler then
  shows leaks of its own, so check leaks without it).
- `--check-visible` prints whether a property of the first Glyphnotic
  filter is visible after OBS applies the settings, as the properties
  window would.
- `--ui-set` changes settings of the first Glyphnotic filter the way the
  properties window does: it opens the properties (OBS runs every modified
  callback), sets the values, runs each changed property's modified
  callback, then updates the filter. `--ui-no-update` leaves out that update,
  so only the callbacks act. `--press` clicks a button (e.g.
  `capture_palette`). Either then waits `--wait-frames` frames (default 10).
- `--settings-out` writes the filter's settings, defaults included, as
  JSON; `--properties-out` writes its properties (name, type, visibility,
  list items, sliders' `min`, `max` and `step`, groups nested).
- `--then-image` switches the image source to another PNG after the first
  dump, runs `--then-frames` frames and presses Dump grid again into
  `--dump2` (`run_levels.py` uses it).
- `--also`, `--period`, `--noise` (or `--frame-sequence` alone) use the
  harness's frame-sequence source instead of OBS's image source: it shows
  `--image` and then each `--also` (same size) for `--period` frames each
  (0: the first only), with fresh Gaussian noise of standard deviation
  `--noise` (8-bit units: 2 is 2/255) added to every color channel of every
  frame and rounded, seeded by `--noise-seed`. It premultiplies by alpha as
  OBS's image loader does, and hands a filter the same bytes as OBS's image
  source (`run_motion.py`'s source suite checks this).
- `--record` presses Record grid on the first Glyphnotic filter: the plugin
  writes `--record-frames` consecutive frames (with `--record-colors`, the
  cell colors too; format below) and the harness waits for the file.
- `--frame-pair` saves two consecutive output frames as `<prefix>-0.pam` and
  `<prefix>-1.pam`.

The harness points the plugin's dump at `--dump` itself (through
`GLYPHNOTIC_DUMP_PATH`), and the recording at `--record` (through
`GLYPHNOTIC_RECORD_PATH`, `GLYPHNOTIC_RECORD_FRAMES` and
`GLYPHNOTIC_RECORD_COLORS`).

It prints OBS's memory-leak count at exit.

### Grid dump format

JSON (`version` 3) with the settings (`mode`, `look`, `charset_name`,
`cell_scale`, `alignment`, `edges`, `keep_transparency`), the instance's
frame counter (`frame`), the character set as code points (`charset`), the
geometry (`source_width`, `source_height`, `cells_x`, `cells_y`, `offset_x`,
`offset_y`), the motion settings (`motion`: `soften`, `hold`, `settle`,
`boil`, `boil_spread`, `boil_linger`, plus Hold's `hold_threshold` and the
instance's Boil `seed`), this frame's `light_jump` and `jump_share` (the
share of cells whose lightness changed by more than 0.12), per mode Ink's settings (`ink`: `color`, `a`, `b` (the fixed
or custom ink's OKLab a, b), `background`, `lightness`, `saturation`,
`gamma`, `shape`, `levels`, `adapt_speed`, `black_point`, `white_point`)
and this frame's `levels` (`lo`, `hi`, `μ`, `σ`), Poster's `poster_levels`,
Console's `palette_name` and `palette` (16 sRGB triples), and per-cell
arrays in row-major order:
`glyph` (shown glyph index), `held`, `boil` (frames the boiled glyph still
shows, this one included; 0 = not swapping), `cost` (best cost), `mean_lab`,
`mean_alpha` (Cell stats), `opaque`, `text_lab`, `bg_lab` (OKLab) and
`text_srgb`, `bg_srgb` (0–255, unrounded; Poster's before snapping).

### Recording format

Binary, little endian, one record per frame from the frame Record grid was
pressed: a header of 8 uint32 (`GNRF` as 0x46524E47, the instance's frame
counter, `cells_x`, `cells_y`, light jump 0/1, the jump share as float32
bits, colors 0/1, the mean of the cells' raw mean lightness as float32
bits), then `held` uint16 per cell, `shown` uint16 per cell, `boil` uint8
per cell, and with colors float32 × 6 per cell (text OKLab, background
OKLab), cells in row-major order. `run_motion.py`'s `read_recording` reads
it.
