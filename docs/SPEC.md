# Glyphnotic — Build Spec

Status: v1.0 spec, ready to build. Repository private; license to be decided (see Licensing).

## Overview

Glyphnotic is a native OBS Studio plugin that redraws any video source as a live grid of characters. It adds four video filters, one per drawing mode:

| Filter | Look in one line |
| --- | --- |
| Glyphnotic Ink | Characters chosen by brightness and shape, lit in the picture's own colors or a fixed ink |
| Glyphnotic Mosaic | Block characters with free text and background colors |
| Glyphnotic Poster | Mosaic with colors snapped to a few levels per channel |
| Glyphnotic Console | Characters and colors limited to a 16-color palette |

Each filter is an ordinary OBS filter: it goes on any source, scene or group, stacks with other filters, and every instance keeps its own settings. Every filter ships with named looks (presets) and a set of motion controls, from a locked, still grid to a deliberately boiling one.

Out of scope for v1.0: rendering or exporting video files, a separate settings window, HDR output, fonts other than the bundled one.

## Platforms

| Platform | v1.0 status |
| --- | --- |
| Windows 10/11, x64 | Supported: built in CI, tested, installer and zip |
| Linux x86_64 (Ubuntu 26.04 LTS and current Debian; other distros from source) | Supported: built in CI, tested, .deb and tarball |
| macOS | May work: built in CI on release tags and manual runs, not tested |

- OBS Studio 33.0.0 or later. The plugin is built against OBS 33.0.x headers (33.0.1 in `buildspec.json`); libobs ignores the patch number when checking a module, so 33.0.0 loads it. `buildspec.json` stays on 33.0.x so that this minimum holds, and the .deb depends on `obs-studio (>= 33.0.0)`. The plugin installs only to OBS 33's third-party plugin location, never to the legacy locations OBS 34 drops.
- Renderers: Direct3D 11 (Windows) and OpenGL (Linux). All GPU code is libobs effect files, so no renderer-specific code exists. Metal on macOS falls under "may work".
- Any GPU that runs OBS 33 should load the plugin; performance targets are in Acceptance.

## Architecture

- **Language and build.** C++17 on the libobs C API, started from the current `obsproject/obs-plugintemplate` (CMake, its CI workflows and packaging). Toolchain minimums follow OBS 33's own build: Visual Studio 2026 with Windows SDK 10.0.26100 (so CMake 4.2 or later on Windows), Xcode 26 with a macOS 13 deployment target, and Ubuntu 26.04 with OBS from OBS's PPA. CI runs on the `windows-2025-vs2026`, `macos-26` and `ubuntu-26.04` runners, as OBS 33's CI does. Windows and Linux build on every push to `main` and on pull requests; macOS builds only for tags and manual (`workflow_dispatch`) runs. The template (last updated December 2025) targeted OBS 31.1.1; Phase 0 moved it to OBS 33.0.1 (see `docs/build-history/phase-0.md`).
- **No Qt.** All UI is the OBS properties API (sliders, lists, color pickers, a text field, a button), so the plugin has no Qt dependency and isn't tied to OBS's Qt version.
- **One shared filter core, four registrations.** A single implementation is registered four times with a fixed mode each. Modes differ only in their scoring and color functions.
- **GPU only for per-frame work.** Each frame runs as a short chain of libobs render passes on `gs_texrender` targets: no compute shaders and no multiple render targets, since libobs offers neither portably. The CPU only does setup, and palette capture on request.
- **State per instance.** Each filter instance owns its textures, including the previous frame's grid, so instances never share state.

### Repository layout

```
CMakeLists.txt, CMakePresets.json, buildspec.json, cmake/, build-aux/, .github/   from the plugin template
cmake/common/manifest.json.in   template for data/manifest.json
src/
  plugin-main.cpp      module load, registers the four filters
  glyph-filter.cpp     shared filter: create/destroy/update/properties/render
  modes.cpp            per-mode settings, presets and effect selection
  font.cpp             PSF1 parser, character sets, glyph atlas (no libobs dependency)
  grid.hpp             grid geometry (no libobs dependency)
  palette.cpp          built-in palettes, palette capture (k-means)
  presets.cpp          the looks
  plugin-support.*     from the plugin template (logging helper)
data/
  effects/*.effect     all GPU passes: sample, stats, score, cell-color, draw; common and cells are shared includes
  fonts/Uni2-Fixed16.psf, fonts/README.md
  locale/en-US.ini
  manifest.json        generated at build time from buildspec.json: OBS 33 Plugin Manager metadata (display name, ID, version, links)
tools/README.md        how to build and run the test tools
tools/harness/         headless libobs runner for a filter on a still (debug builds only)
tools/reference/       Python + NumPy reference implementation, compare and run scripts
tests/unit/            standalone C++ unit tests (no OBS needed)
tests/stills/          openly licensed test stills (with ATTRIBUTION.md)
docs/
  SPEC.md, BUILDING.md, INSTALL.md, LOOKS.md, PERFORMANCE.md
  build-history/       one report per build phase
  licenses/            license texts of bundled third-party material
```

## Font and character sets

**Font.** `Uni2-Fixed16.psf`, the 8×16 Uni2 Fixed console font from Debian's console-setup package (Anton Zinoviev). console-setup's copyright statement: "All console fonts are public domain by nature." It ships in `data/fonts/` with a README stating this. It ships uncompressed, decompressed once from Debian's `Uni2-Fixed16.psf.gz` (the README records both files' SHA-256). It's a PSF1 file: mode 3, 512 glyphs, 16 rows of one byte each, followed by a Unicode table (per glyph: UCS-2 code points ending in 0xFFFF; 0xFFFE starts a sequence, which is skipped). A code point maps to the first glyph that lists it.

**Character sets.** For each set, characters the font lacks are skipped, and a character whose bitmap matches an earlier one in the set is dropped, so no two glyphs in a set are identical. Space is always first.

| Set | Characters | Unique glyphs |
| --- | --- | --- |
| Blocks | `␠█░▒■▮` | 5 |
| Lines | Blocks plus `─│═║┼├┤┬┴┌┐└┘╭╮╰╯╱╲╳/\_‗¯·.•●` | 25 |
| ASCII | Space and the 94 printable characters U+0021–U+007E | 95 |
| Every glyph | Space, then every mapped code point above U+0020 in code-point order, excluding U+007F–U+009F | 503 |
| Custom | Space, then the characters typed in the Custom characters field, in order | Varies, minimum 2; fewer falls back to Blocks |

**Glyph atlas.** On load and on a charset change, the active set's bitmaps go into a small texture with one row per glyph and 128 texels per row (row-major 8×16). The scoring pass and the draw pass both read it. Per glyph, `n_on` is the number of lit pixels and `n_off = 128 − n_on`.

## The grid

- A cell is 8×16 source pixels at scale 1. **Cell scale** (1×–4×) multiplies both, so cells become 16×32, 24×48 or 32×64; at scale `s` each 8×16 sample is the mean of an `s×s` pixel block (alpha-weighted, in linear RGB; the sample's alpha is the block's mean alpha), and glyphs are drawn nearest-neighbor at `s×`.
- The output is the source's size. The grid is centered (or top-left, a setting); when the leftover is odd, the extra pixel goes to the bottom or right. Leftover edge pixels are black or transparent (a setting). A source smaller than one cell passes through unchanged.
- A 1920×1080 source at scale 1 is 240×67 cells, with 4 rows of pixels left over at the top and bottom.

## Color

- Source pixels are sRGB-encoded. Each sample is converted to linear RGB, then to OKLab (Ottosson, 2020); all scoring and color math is in OKLab. Output colors are converted back to sRGB, clipped to [0, 1].
- OBS may hand a filter linear or sRGB textures depending on the source and the `gs_framebuffer_srgb` state. Glyphnotic settles this by not declaring `OBS_SOURCE_SRGB`: OBS then always hands it the 8-bit input as stored sRGB-encoded values (bound without the sRGB view, framebuffer sRGB off) and takes back sRGB-encoded output. The shaders decode and encode with the sRGB formulas, so each value is decoded exactly once and the GPU's sRGB hardware isn't used; on the Phase 1 test GPU it was up to 0.43 of an 8-bit step off. The patch still confirms it (Acceptance).
- OBS hands filters their input premultiplied by alpha, in encoded space (OBS's image loader stores `round(c × a / 255)` in 8 bits). The Sample pass divides alpha back out, so the colors of nearly transparent pixels are coarse.
- v1.0 works in SDR. With an HDR source or canvas, the filter requests and outputs SDR.
- **Transparency** (setting, default on): a cell whose mean source alpha is below 0.5 is drawn fully transparent; all other cells are opaque. Color statistics use only pixels with alpha ≥ 0.5 when the cell has any. A keyed camera stays keyed, cut along cell edges.

## Scoring

For each cell and each glyph `g` in the set, the filter computes a cost; the lowest cost wins, and the lowest glyph index wins a tie. Sums are over the cell's 128 samples: `S_on(g)` is the sum over pixels the glyph lights, `S_tot` the sum over all 128, `S_off = S_tot − S_on`. Vectors are OKLab (L, a, b).

Sums and counts include only the cell's counted samples (alpha ≥ 0.5 when the cell has any; see Transparency), so in a partly transparent cell `n_on` and `n_off` count counted samples; when all 128 count they're the glyph's lit and unlit pixel counts.

Sums may be taken relative to any per-cell constant, which shifts every glyph's cost in the cell equally. Glyphnotic and the reference sum relative to the cell's first counted sample, so a flat cell sums exact zeros, every glyph ties, and space (index 0) wins on any GPU.

### Mosaic and Poster: free colors

Text color = mean of the lit pixels; background = mean of the unlit pixels.

`cost(g) = −( |S_on|² / n_on + |S_off|² / n_off ) / 128`

A term is dropped when its count is 0; that glyph's missing color takes the other side's color. Poster then snaps each output sRGB channel to the nearest of **Levels** evenly spaced values from 0 to 255: `v' = round(v / step) × step`, where `step = 255 / (levels − 1)`.

### Console: palette colors

The palette has 16 OKLab colors `P[0..15]`, ordered so slots 0–7 are the most common colors. Text takes any of the 16; the background takes only slots 0–7.

`cost(g) = ( min_k∈0..15 [ n_on |P_k|² − 2 S_on · P_k ] + min_k∈0..7 [ n_off |P_k|² − 2 S_off · P_k ] ) / 128`

The winning slots give the cell's text and background colors; the lowest slot wins a tie.

### Ink: brightness first, shape second

Only lightness is scored. Each sample's L is stretched and shaped:

`L' = ink × clamp((L − lo) / (hi − lo), 0, 1) ^ gamma`

Here `lo`/`hi` come from **Levels** (below), `ink` is **Ink lightness**, and `gamma` is **Gamma**. With `m` = the mean of `L'` over the cell, and `level(g) = n_on(g) / max_g n_on × ink`:

`cost(g) = (level(g) − m)² + shape × ( n_on × ink² − 2 × ink × S_on(L') ) / 128`

**Shape** (0–1) trades pure density against matching the glyph's form.

Ink colors:

- **Text:** OKLab `(ink, a, b)`. With **Ink color** = Cell, `(a, b)` is the cell's mean `(a, b)` × **Saturation**. Fixed inks: Green `(−0.12, 0.10)`, Amber `(0.034, 0.136)` (the hue of #FFB000), White `(0, 0)`. Custom takes `(a, b)` from a picked color.
- **Background:** Black, or Tint, which is the cell's own mean color at 60% of its raw mean lightness, `(0.6 × L̄, ā, b̄)`.

**Levels** (Ink only):

- **Auto** (default): each frame, take the mean `μ` and standard deviation `σ` of the cells' raw mean lightness; the target is `lo = μ − 2σ`, `hi = μ + 2σ`, clamped to [0, 1] with `hi − lo ≥ 0.05`. The live `lo`/`hi` ease toward the target with time constant **Adapt speed** (0.2–5 s, default 1.5 s), like a camera's auto exposure. They snap on the first frame and on a Light jump.
- **Manual:** **Black point** and **White point** sliders set `lo` and `hi` directly.

## Motion

These settings shape how the grid changes from frame to frame. They apply to all four filters and are what make the filter usable on a live camera, where sensor noise would otherwise make the characters crawl.

| Setting | Range (UI) | What it does |
| --- | --- | --- |
| **Soften** | 0–2 px | Gaussian blur (σ in source pixels) on the source before sampling. Removes sensor noise at its source without holding back real motion. |
| **Hold** | 0–100 | A cell keeps its previous glyph unless the best glyph's cost is lower by more than the threshold. 0 = no hold; 100 = almost locked. Internally `threshold = Hold / 100 × K_mode`; `K_mode` is set per mode during tuning (Phase 4) so that 100 means "almost locked" in every mode. |
| **Settle** | 0–95% | Each cell's text and background colors blend with the previous frame's: `c = mix(c_new, c_prev, Settle)`, in OKLab. Calms color flicker separately from glyph changes. |
| **Boil** | 0–100 | Deliberate, image-independent glyph swaps. The chance that a cell starts a swap in a given second: 0 → never, 100 → 4 swaps per cell per second on average. It's converted to a per-frame chance using OBS's actual frame rate, so the look is the same at 30 and 60 fps. |
| **Boil spread** | 0–100 | How far a swap may stray from the best glyph: a swapping cell picks uniformly among glyphs whose cost is within `spread × (worst − best)` of the best (UI 100 maps to 0.5). Low spread keeps the image readable; high spread turns into static. |
| **Boil linger** | 1–12 frames | How long a swapped glyph stays before the cell returns to its held glyph. 1 is a flicker, longer is a crawl. |

**Hold and Boil don't interact.** Each cell stores its *held* glyph, the image-driven choice that Hold compares against, separately from its *shown* glyph. A boiled glyph is shown for its linger and then dropped; it never becomes the held glyph, so boiling can't pile up under a high Hold.

**Light jump.** If at least 50% of cells change raw mean lightness by more than 0.12 from the previous frame, the frame counts as a light jump: every cell picks fresh (Hold is ignored for one frame), Settle restarts, Auto levels snap, and active boils end. This covers scene switches and lights turning on.

Randomness for Boil is a hash (e.g. PCG) of the cell position, a per-instance seed and a frame counter, so it costs no texture memory and no two instances boil in step.

## Render passes

This sequence is a suggestion; the builder may merge or split passes as long as the results and Acceptance hold. "Cell-res" means a texture with one texel per cell.

1. **Soften** (skipped at 0): separable Gaussian, two passes at source resolution.
2. **Sample:** source → OKLab plus alpha at sample resolution, `cellsX × 8` by `cellsY × 16`; this is where Cell scale's block averaging happens.
3. **Cell stats** (cell-res): mean OKLab and mean alpha per cell, plus a flag for Light jump comparing against the previous frame's stats (ping-pong).
4. **Reduce:** successive downsampling to 1×1 for the Light jump share, and for Ink's Auto levels (`μ`, `σ`), which are then eased against the previous frame's levels (ping-pong 1×1).
5. **Score** (cell-res): each cell loops over the glyphs and its 128 samples, reading the previous held glyph and boil state. Output: held glyph, shown glyph, boil frames left, best cost. Indices are stored as floats, which hold exact integers.
6. **Color** (one pass into a double-width target, `2 × cellsX` by `cellsY`: even texels hold text colors, odd texels background colors): text and background color for the shown glyph in OKLab, blended with the previous colors by Settle (ping-pong); alpha marks the cell opaque or transparent.
7. **Draw** (source resolution): each pixel finds its cell, reads the shown glyph's bit from the atlas, and outputs the text or background color, or transparency.

Intermediate targets are 32-bit float RGBA. Every pass reads its inputs with `Load` at integer texel positions derived from the sprite's UVs, so Direct3D and OpenGL index textures the same way.

Ping-pong textures and the frame counter belong to the instance. They are reset when settings change in a way that alters the grid: charset, cell scale, mode-specific color settings, or source size.

## Filters and settings

All four filters share the groups below; mode-specific settings appear only in their own filter. Choosing a **Look** fills in its settings; changing any of them shows the Look as **Custom**.

**Look:** preset list (see Looks) plus Custom.

**Characters:** Set (Blocks / Lines / ASCII / Every glyph / Custom), Custom characters (text, shown only for Custom), and a warning note, shown when the Custom characters give fewer than 2 glyphs and Blocks is used instead.

**Grid:** Cell scale (1×–4×), Alignment (Center / Top-left), Edges (Black / Transparent), Keep transparency (on/off).

**Motion:** Soften, Hold, Settle, Boil, Boil spread, Boil linger.

**Ink only:** Ink color (Cell / Green / Amber / White / Custom + color picker), Background (Black / Tint), Ink lightness (0.5–1.0, default 0.92), Saturation (0.5–2.0, default 1.4), Gamma (0.5–1.5, default 0.85), Shape (0–1, default 0.35), Levels (Auto / Manual), Adapt speed, Black point, White point.

**Poster only:** Levels (2–16, default 6).

**Console only:** Palette (VGA / Custom), 16 color pickers (shown for Custom), **Capture palette** button.

**Capture palette** reads back one downscaled frame (about 160×90) from the filter's input through a `gs_stagesurface` and runs on a worker thread. It runs k-means in OKLab (k = 16, fixed seed, best of 3 starts, 30 iterations), orders the colors by pixel count with slots 0–7 the most common, writes them into the 16 color pickers, and sets Palette to Custom. The result is saved with the scene like any other setting.

The VGA palette is the standard 16-color text-mode palette, ordered so slots 0–7 are the eight normal-intensity colors.

The ranges above are starting points. Phase 4 trims each one to the span where every position looks deliberate.

## Looks

Starting values; Phase 4 tunes the defaults and records before/after stills in `docs/LOOKS.md`. Unlisted settings keep the defaults above. Soften defaults to 0.6 px and Settle to 0 unless noted.

| Filter | Look | Characters | Settings |
| --- | --- | --- | --- |
| Ink | **Black** | ASCII | Cell ink, black background, Hold 25 |
| Ink | **Tint** | ASCII | Cell ink, tint background, Hold 25 |
| Ink | **Phosphor** | ASCII | Green ink, black background, Hold 25 |
| Ink | **Amber** | ASCII | Amber ink, black background, Hold 25 |
| Ink | **Every Glyph** | Every glyph | Cell ink, black background, Hold 25 |
| Ink | **Static** | ASCII | Green ink, black background, Hold 25, Boil 15, spread 20, linger 2 |
| Ink | **Fever** | ASCII | Cell ink, tint background, Hold 10, Boil 45, spread 35, linger 1, Settle 30% |
| Mosaic | **Blocks** | Blocks | Hold 10 |
| Poster | **Poster** | Lines | Levels 6, Hold 10 |
| Console | **Console** | Lines | VGA palette, Hold 10 |

## Install

Packages are attached to GitHub Releases. Until the repository is public, they're downloaded by signed-in collaborators.

OBS 33 moved third-party plugins to a new per-plugin location with a new folder layout. The plugin is never installed to a legacy location; OBS flags those as "Legacy" in its Plugin Manager and stops loading them in OBS 34. Confirmed in Phase 0 against OBS 33.0.1's source (`frontend/plugin-manager/PluginModuleLoader_*.cpp`) and OBS's [Legacy Plugin Developer Info](https://obsproject.com/kb/legacy-plugin-developer-info); the Windows location was tested with OBS 33.0.1. `docs/INSTALL.md` has the details.

| Platform | Plugin binary | Plugin data |
| --- | --- | --- |
| Windows | `C:\ProgramData\obs-studio\plugins\glyphnotic\glyphnotic.dll` | `C:\ProgramData\obs-studio\plugins\glyphnotic\data\` |
| Windows, portable OBS | `<OBS folder>\plugins\glyphnotic\glyphnotic.dll` | `<OBS folder>\plugins\glyphnotic\data\` |
| Linux, system (.deb) | `/usr/lib/x86_64-linux-gnu/obs-modules/plugins/glyphnotic.so` | `/usr/share/obs/obs-modules/plugins/glyphnotic/` |
| Linux, per user (tarball) | `~/.local/share/obs-studio/plugins/glyphnotic/glyphnotic.so` | `~/.local/share/obs-studio/plugins/glyphnotic/data/` |
| macOS | `~/Library/Application Support/obs-studio/plugins/glyphnotic.plugin` | inside the bundle |

On Linux the per-user folder is `$XDG_DATA_HOME/obs-studio/plugins/` (default `~/.local/share/...`), and the system folder is `<prefix>/<libdir>/obs-modules/plugins`, fixed when OBS is built; `/usr/lib/x86_64-linux-gnu` is what OBS's own Ubuntu packages use. OBS 33 also reads `data/manifest.json` for the Plugin Manager's display name, ID and version; Glyphnotic generates it from `buildspec.json`.

### Windows

1. Close OBS.
2. Run `glyphnotic-<version>-windows-x64-installer.exe`, or unzip `glyphnotic-<version>-windows-x64.zip` into OBS's third-party plugin folder (`C:\ProgramData\obs-studio\plugins\`) so that it contains a `glyphnotic` folder.
3. Start OBS. Right-click a source → **Filters** → **+** and pick a Glyphnotic filter.

Uninstall: use **Apps → Installed apps** for the installer, or delete the `glyphnotic` plugin folder.

### Linux

1. Close OBS.
2. On Ubuntu 26.04 LTS or current Debian, run `sudo apt install ./glyphnotic-<version>-linux-x86_64.deb`. Otherwise, extract `glyphnotic-<version>-linux-x86_64.tar.xz` into OBS's per-user plugin folder (`~/.local/share/obs-studio/plugins/`) so that it contains a `glyphnotic` folder.
3. Start OBS. Right-click a source → **Filters** → **+** and pick a Glyphnotic filter.

Uninstall: `sudo apt remove glyphnotic`, or delete the `glyphnotic` plugin folder.

Flatpak builds of OBS aren't covered in v1.0 (see Roadmap).

### macOS (may work)

CI produces `glyphnotic-<version>-macos.pkg`. It isn't tested; reports are welcome.

### Building from source

`docs/BUILDING.md` covers each platform in its own equal section, following the plugin template's build scripts: Windows (PowerShell, Visual Studio), Linux (bash, CMake + Ninja or Make), macOS (Xcode).

## Reference implementation

`tools/reference/` is a plain Python + NumPy implementation of Sample, Cell stats and Score for single stills, written from this spec, with motion settings off. It works in float64 and models OBS's 8-bit premultiplied input (see Color). It outputs the glyph index per cell. `compare.py` diffs it against a grid dump from the plugin, checks the cell colors and opacity, and checks the output frame OBS produced against the picture the dump describes.

A debug build of the plugin (CMake option `GLYPHNOTIC_DEBUG_TOOLS`) adds a **Dump grid** button that writes the shown glyph indices and the cell colors for the current frame to a file. The same build makes `tools/harness`, which runs a filter on a still in a headless libobs (OBS's own image source, real renderer) and saves the dump and one output frame; `run_stills.py` runs it and `compare.py` over every test still; `run_color_paths.py` checks colors along OBS's other color paths (filters before and after, 10-bit and HDR canvases, 16-bit and HDR sources). `tools/README.md` has the commands. None of this is shipped.

## Build phases and acceptance

Each phase ends with a report in `docs/build-history/phase-N.md` covering what was built, how it was tested, measurements, and deviations from this spec with reasons. Then the spec is updated and the phase's boxes are ticked.

**Phase 0: scaffold**
- [x] Repository created from `obsproject/obs-plugintemplate`; CI builds Windows, Linux and macOS packages (Windows and Linux green on every push to `main`; macOS builds only on tags and manual runs, by decision at the Phase 0 review)
- [x] A pass-through "Glyphnotic Ink" filter loads in OBS 33 from the new plugin location, with no "Legacy" badge in the Plugin Manager
- [x] PSF1 parser and character sets, with unit tests confirming the unique counts 5 / 25 / 95 / 503
- [x] Install paths confirmed and written into Install and `docs/INSTALL.md`

**Phase 1: grid and Mosaic**
- [x] Sample, Cell stats, Score and Draw passes; Mosaic filter; cell scale; alignment; edges; transparency
- [x] sRGB handling verified: a test still with known patch colors yields cell colors within 1/255 of expected (measured: within 0.003/255)
- [x] Reference match: on the test stills with motion off, ≥ 99% of cells match the reference; every mismatch has a cost gap under 1e-4 (measured: 100% of cells in all 25 cases, on Direct3D 11 and OpenGL)

**Phase 2: Ink, Poster, Console**
- [ ] All four filters with their settings; Auto and Manual levels; VGA and custom palettes; Capture palette
- [ ] Reference match as in Phase 1, for each mode and character set
- [ ] All looks from Looks are present and apply

**Phase 3: motion**
- [ ] Soften, Hold, Settle, Boil, Boil spread, Boil linger, Light jump
- [ ] Flicker test: a still plus fresh Gaussian noise (σ = 2/255) every frame, 600 frames. Report the share of cells that change glyph per frame for each look, with motion off and at the look's settings. At the look's settings, non-boiling looks change ≤ 3% of cells per frame
- [ ] Light jump: alternating between two different stills, the first frame after each switch matches a fresh pick in 100% of cells
- [ ] Boil 0: two consecutive frames of a still are identical. Boil > 0: the measured swap rate is within ±20% of the setting. A swapped cell returns to its held glyph after its linger, even at Hold 100

**Phase 4: tuning**
- [ ] `K_mode` set for each mode, and every slider range trimmed to the span that looks deliberate
- [ ] Final look defaults; `docs/LOOKS.md` with a still of each look on at least three test stills
- [ ] Ranges, defaults and `K_mode` values written back into this spec

**Phase 5: performance, packaging, release**
- [ ] `docs/PERFORMANCE.md`: GPU time per look at 1920×1080, measured with libobs GPU timer queries (or OBS's render-time stat with and without the filter), with the test GPU named
- [ ] On the test machine at 1920×1080, 60 fps: three Ink/ASCII instances on three sources run with no lagged or skipped frames in OBS's stats; one Every Glyph instance does the same
- [ ] Adding and removing a filter 100 times leaves "Number of memory leaks: 0" in the OBS log at exit
- [ ] Settings survive an OBS restart; instances on different sources stay independent
- [ ] Windows installer and zip, Linux .deb and tarball, macOS .pkg from CI; install and uninstall tested on Windows and Linux by following Install exactly
- [ ] `README.md` and `docs/INSTALL.md` final; `v1.0.0` tagged with a GitHub Release

## Test assets

Test stills come from openly licensed footage (for example the Blender Foundation's open movies, CC BY). Each still is credited in `tests/stills/ATTRIBUTION.md`. Only stills are committed; no video. The set includes at least one face close-up, one high-contrast scene, one dark scene, one image with an alpha channel, and one synthetic still of known color patches.

## Licensing

- To be decided before any public release; the repository stays private until then.
- The plugin links libobs, which is GPL-2.0-or-later. Distributing plugin binaries therefore requires a GPL-compatible license, and the eventual choice must be one.
- The plugin template's license terms (reviewed in Phase 0): the template repository ships the GPL-2.0 license text, and its source files carry a "GPL version 2 or (at your option) any later version" notice. Glyphnotic keeps the template's CMake, CI and `plugin-support` files, so the eventual license must also be compatible with GPL-2.0-or-later, which the libobs requirement already implies. The template's `LICENSE` file was moved to `docs/licenses/obs-plugintemplate-GPL-2.0.txt` so that a root `LICENSE` isn't read as the decision; Glyphnotic's own new files carry no license header until it's made.
- The bundled font is public domain (see Font).

## Roadmap

Only releases that have been cleared get a version number. Everything else is an idea: listed so it isn't lost, in no order and with no commitment.

| Release | Scope | Status |
| --- | --- | --- |
| v1.0 | Four filters, ten looks, motion controls, Windows and Linux packages | Next |

**Ideas, not scheduled**

- Audio-reactive motion: Boil, Hold or Ink lightness driven by an audio source's level
- Flatpak extension for Flatpak builds of OBS, and a Flathub listing once licensed
- A listing on the OBS forums' plugin directory once licensed
- More fonts: other PSF or BDF fonts, including taller or wider cells
- A Glyphnotic scene transition that dissolves one scene into the next through glyphs
- HDR in and out
- Grid export: the live character grid as text, for terminals or overlays
