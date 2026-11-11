# Tools

Test tools for Glyphnotic. None of this ships with the plugin.

| Path | What it is |
| --- | --- |
| `harness/harness.cpp` | `glyphnotic-harness`: runs Glyphnotic filters on a still image in a headless libobs (no OBS window); writes the grid dump, an output frame or OBS's canvas texture, and lag and GPU-time figures |
| `reference/reference.py` | The reference implementation (Python + NumPy, float64) of Sample, Cell stats and Score, written from `docs/SPEC.md` |
| `reference/compare.py` | Compares one grid dump (and output frame) with the reference |
| `reference/run_stills.py` | Runs the harness and `compare.py` over every test still and prints a table |
| `reference/run_color_paths.py` | Checks colors along OBS's other color paths: Color Correction before and after, 10-bit and HDR canvases, 16-bit and HDR sources |
| `reference/make_stills.py` | Rebuilds `tests/stills/` (see `tests/stills/ATTRIBUTION.md`) |
| `reference/imageio_min.py` | PNG/PAM/BMP reading and PNG writing with only NumPy |
| `reference/requirements.txt` | The Python packages, pinned (NumPy only) |

The harness needs a debug build of the plugin and an installed OBS of the
version in `buildspec.json` (33.0.1). The Python scripts need Python 3.12 or
later and the packages in `requirements.txt`, in a virtual environment of
Glyphnotic's own. The commands below run from the repository root unless
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
..\..\.venv\Scripts\python run_color_paths.py --build ..\..\build_dbg\RelWithDebInfo
```

The second command runs OBS's OpenGL renderer on Windows, which checks the
effects' GLSL translation without a Linux machine.

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
../../.venv/bin/python run_color_paths.py --build ../../build_dbg
```

The Linux steps follow the Windows ones and the harness has Linux code
paths, but they haven't been run yet (see `docs/build-history/phase-1.md`).

## What the scripts report

Results go to `build_stills/` (or `--out`) and `build_color_paths/` at the
repository root: grid dumps, frames as `.pam`, `results.json`. Each script
prints a Markdown table and exits with 0 when every case passes. One case
alone: `run_stills.py --only victor-ascii`. One dump by hand:

```powershell
..\..\.venv\Scripts\python compare.py ..\..\tests\stills\victor.png ..\..\build_stills\victor-ascii-1.json --frame ..\..\build_stills\victor-ascii-1.pam
```

In OBS itself, a debug build's filter shows a **Dump grid (debug)** button.
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

It prints OBS's memory-leak count at exit.

### Grid dump format

JSON with the settings (`mode`, `charset_name`, `cell_scale`, `alignment`,
`edges`, `keep_transparency`), the character set as code points
(`charset`), the geometry (`source_width`, `source_height`, `cells_x`,
`cells_y`, `offset_x`, `offset_y`) and per-cell arrays in row-major order:
`glyph` (shown glyph index), `held`, `cost` (best cost), `mean_lab`,
`mean_alpha` (Cell stats), `opaque`, `text_lab`, `bg_lab` (OKLab) and
`text_srgb`, `bg_srgb` (0–255, unrounded).
