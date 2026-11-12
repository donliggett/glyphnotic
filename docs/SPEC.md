# Glyphnotic — Build Spec

Status: v1.0, built in Phases 0–5 (`docs/build-history/`). Licensed GPL-2.0-or-later (see Licensing); repository private.

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
cmake/windows/resources/installer-Windows.iss   the Windows installer (Inno Setup 6)
src/
  plugin-main.cpp      module load, registers the four filters
  glyph-filter.cpp     shared filter: create/destroy/update/render, palette capture, grid dump
  modes.cpp            per-mode settings, properties, defaults, applying looks
  settings.hpp         modes, setting keys, the settings the render thread uses (no libobs dependency)
  motion.hpp           the motion settings' constants and formulas: K_mode, Boil's chance, Soften's kernel (no libobs dependency)
  font.cpp             PSF1 parser, character sets, glyph atlas (no libobs dependency)
  grid.hpp             grid geometry (no libobs dependency)
  palette.cpp          color conversions, the VGA palette, palette capture's k-means (no libobs dependency)
  presets.cpp          the looks table (no libobs dependency)
  plugin-support.*     from the plugin template (logging helper)
data/
  effects/*.effect     all GPU passes: soften, sample, stats, levels (Reduce and Levels), lightness, score,
                       cell-color, draw, capture; common and cells are shared includes
  fonts/Uni2-Fixed16.psf, fonts/README.md
  locale/en-US.ini
  manifest.json        generated at build time from buildspec.json: OBS 33 Plugin Manager metadata (display name, ID, version, links)
tools/README.md        how to build and run the test tools
tools/harness/         headless libobs runner for a filter on a still (debug builds only)
tools/reference/       Python + NumPy reference implementation, compare and run scripts
tools/linux-check/     end-of-phase Linux check: OBS under xvfb-run in an Ubuntu 26.04 container; the Linux install test
tools/obs-tests/       Phase 5's tests in the OBS window (Windows): a Lua script and its driver
tests/unit/            standalone C++ unit tests (no OBS needed)
tests/stills/          openly licensed test stills (with ATTRIBUTION.md)
docs/
  SPEC.md, BUILDING.md, INSTALL.md, LOOKS.md, PERFORMANCE.md
  BUILDER-NOTES.md     how the builder works: shell, commands, test suites, OBS and Linux checks, standing rules
  build-history/       one report per build phase
  looks/               the stills in LOOKS.md
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

The winning slots give the cell's text and background colors; the lowest slot wins a tie. A side with no counted samples takes the other side's slot (its term is 0 for any slot). When both sides end up with the same slot `k`, the cell is one color whatever the glyph, and its cost is computed from the cell's totals as `( n |P_k|² − 2 S_tot · P_k ) / 128`: every such glyph then ties exactly and the lowest index wins, on any GPU. Sums are taken relative to the cell's first counted sample, as above.

### Ink: brightness first, shape second

Only lightness is scored. Each sample's L is stretched and shaped:

`L' = ink × clamp((L − lo) / (hi − lo), 0, 1) ^ gamma`

Here `lo`/`hi` come from **Levels** (below), `ink` is **Ink lightness**, and `gamma` is **Gamma**. With `m` = the mean of `L'` over the cell's counted samples, and `level(g) = N(g) / max_g N × ink`, where `N(g)` is the glyph's own lit-pixel count (from the atlas, whatever the cell's transparency):

`cost(g) = (level(g) − m)² + shape × ( n_on × ink² − 2 × ink × S_on(L') ) / 128`

`n_on` and `S_on` use counted samples, as above. `S_on` is summed relative to the first counted sample's `L'` (`c`), as `n_on × ink × (ink − 2c) − 2 × ink × S_on(L' − c)`, the same value: a flat cell then sums exact zeros, and glyphs with equal counts tie exactly.

**Shape** (0–1) trades pure density against matching the glyph's form.

Ink colors:

- **Text:** OKLab `(ink, a, b)`. With **Ink color** = Cell, `(a, b)` is the cell's mean `(a, b)` × **Saturation**. Fixed inks: Green `(−0.12, 0.10)`, Amber `(0.034, 0.136)` (the hue of #FFB000), White `(0, 0)`. Custom takes `(a, b)` from a picked color.
- **Background:** Black, or Tint, which is the cell's own mean color at 60% of its raw mean lightness, `(0.6 × L̄, ā, b̄)`.

**Levels** (Ink only):

- **Auto** (default): each frame, take the mean `μ` and standard deviation `σ` (population) of the raw mean lightness of the cells drawn opaque (all cells when Keep transparency is off); the target is `lo = μ − 2σ`, `hi = μ + 2σ`, each clamped to [0, 1]. If then `hi − lo < 0.05`, both move to 0.025 either side of their midpoint, and back inside [0, 1] if that crosses an end (`lo = 0, hi = 0.05` or `lo = 0.95, hi = 1`). With no opaque cells, `μ = 0.5, σ = 0`. The live `lo`/`hi` ease toward the target with time constant **Adapt speed** (0.2–5 s, default 1.5 s), like a camera's auto exposure: `lo += (target − lo) × (1 − e^(−dt / adapt speed))`, with `dt` the time since the last frame. They snap on the first frame, when the grid resets (see Render passes) and on a Light jump.
- **Manual:** **Black point** (0–0.6) and **White point** (0.2–1) sliders (defaults 0 and 1) set `lo = black point` and `hi = max(white point, black point + 0.05)`.

## Motion

These settings shape how the grid changes from frame to frame. They apply to all four filters and are what make the filter usable on a live camera, where sensor noise would otherwise make the characters crawl.

| Setting | Range (UI) | What it does |
| --- | --- | --- |
| **Soften** | 0–2 px | Gaussian blur (σ in source pixels) on the source before sampling. Removes sensor noise at its source without holding back real motion. |
| **Hold** | 0–100 | A cell keeps its previous glyph unless the best glyph's cost is lower by more than the threshold. 0 = no hold; 100 = almost locked. Internally `threshold = Hold / 100 × K_mode`; `K_mode` is set per mode so that 100 means "almost locked" in every mode (values below). |
| **Settle** | 0–95% | Each cell's text and background colors blend with the previous frame's: `c = c_new + (c_prev − c_new) × Settle`, in OKLab. Calms color flicker separately from glyph changes. |
| **Boil** | 0–100 | Deliberate, image-independent glyph swaps. The rate at which a cell that isn't already swapping starts a swap: 0 → never, 100 → 4 swaps per cell per second. It's converted to a per-frame chance `p = 1 − e^(−r·dt)` (`r` the rate, `dt` the frame interval of OBS's configured frame rate), so the look is the same at 30 and 60 fps. |
| **Boil spread** | 0–100 | How far a swap may stray from the best glyph: a swapping cell picks uniformly among the glyphs other than its held glyph whose cost is within `spread × (worst − best)` of the best (UI 100 maps to 0.5); if there are none, it shows its held glyph for the swap. Low spread keeps the image readable; high spread turns into static. |
| **Boil linger** | 1–12 frames | How many frames a swapped glyph is shown before the cell returns to its held glyph (or starts its next swap). 1 is a flicker, longer is a crawl. |

**Soften** blurs the filter's input as OBS hands it over (8-bit, sRGB-encoded, premultiplied by alpha), so transparent pixels don't bleed color; Sample then divides alpha out as for unblurred input. The kernel has taps `−r..r`, `r = ceil(3σ)`, weights `e^(−i²/2σ²)` normalized to sum 1, and a tap beyond the source's edge takes the edge pixel. It's separable: a row pass at source size, then a column pass inside Sample.

**Hold and Boil don't interact.** Each cell stores its *held* glyph, the image-driven choice that Hold compares against, separately from its *shown* glyph. A boiled glyph is shown for its linger and then dropped; it never becomes the held glyph, so boiling can't pile up under a high Hold.

**Hold** keeps the held glyph when `cost(held) − cost(best) ≤ threshold`, else takes the best (the lowest index among ties). At Hold 0 the held glyph is always the best. `K_mode`: Ink 0.003, Mosaic 0.000025, Poster 0.00015, Console 0.0015. Each is 10× the threshold at which fresh noise of σ = 2/255 on `victor.png`, with the mode's first look, changes fewer than 0.1% of cells per frame, so Hold 10 holds against sensor noise (Phase 3); on `victor.png` panning 1 px per frame with that noise, Hold 100 changes 5–14% as many cells per frame as Hold 0 in every mode, and 10–24% on a 4 px per frame pan, so it's almost locked for small movements and still follows big ones (Phase 4). The modes' costs differ by orders of magnitude, and Poster's Lines set flickers more under noise than Mosaic's Blocks.

**Light jump.** If at least 50% of all cells change raw mean lightness by more than 0.12 from the previous frame, the frame counts as a light jump: every cell picks fresh (Hold is ignored for one frame), Settle restarts, Auto levels snap, and active boils end, with no new swap starting that frame. This covers scene switches and lights turning on. The first frame of a grid (a new filter, or after a reset; see Render passes) is fresh in the same way, without counting as a light jump.

Randomness for Boil is a PCG hash of the cell position, a per-instance seed (from the time and the instance's address at creation, not saved) and the instance's frame counter, so it costs no texture memory and no two instances boil in step.

## Render passes

This sequence is a suggestion; the builder may merge or split passes as long as the results and Acceptance hold. "Cell-res" means a texture with one texel per cell.

1. **Soften** (skipped at 0): separable Gaussian. The row pass draws OBS's input into a float target at source resolution; the column pass happens as Sample reads that target, so only one source-size target is needed.
2. **Sample:** source → OKLab plus alpha at sample resolution, `cellsX × 8` by `cellsY × 16`; this is where Cell scale's block averaging happens.
3. **Cell stats** (cell-res, ping-pong): mean OKLab and mean alpha per cell.
4. **Reduce** (every mode): successive downsampling to 1×1, each pass summing 16×16 blocks; the first sums `(1, L, L²)` over the cells drawn opaque, for Ink's Auto levels, and the cells whose raw mean lightness changed by more than 0.12 from the previous frame's Cell stats, for the Light jump. A 2×1 **Levels** pass (ping-pong) turns the sums into the frame's state: the light jump and its share, and Ink's `μ`, `σ` and target, eased against the previous frame's levels, or Manual's `lo`/`hi`. Ink then runs a **Lightness** pass at sample resolution: each sample's `L'` and alpha, which its Score pass reads in place of the samples.
5. **Score** (cell-res, ping-pong): each cell loops over the glyphs and its 128 samples, reading the previous held glyph and boil state, and applies Hold and Boil; a cell that starts a swap loops over the glyphs again to pick it. Output: held glyph, shown glyph, boil frames left (this one included), best cost. Indices are stored as floats, which hold exact integers.
6. **Color** (one pass into a double-width target, `2 × cellsX` by `cellsY`, ping-pong: even texels hold text colors, odd texels background colors): text and background color for the shown glyph in OKLab, blended with the previous colors by Settle; alpha marks the cell opaque or transparent.
7. **Draw** (source resolution): each pixel finds its cell, reads the shown glyph's bit from the atlas, and outputs the text or background color, or transparency. Poster snaps its colors here, after Settle, so the drawn colors are always on its levels.
8. **Capture** (Console, only when Capture palette is pressed): the Sample output averaged down to about 160×90 (alpha-weighted mean OKLab per block, and the block's mean alpha) and read back.

Intermediate targets are 32-bit float RGBA. Every pass reads its inputs with `Load` at integer texel positions derived from the sprite's UVs, so Direct3D and OpenGL index textures the same way.

Ping-pong textures and the frame counter belong to the instance. They are reset when settings change in a way that alters the grid: charset, cell scale, alignment, Keep transparency, mode-specific color settings, or source size. Motion settings don't reset them. A frame whose passes fail part way also starts the history over.

## Filters and settings

All four filters share the groups below; mode-specific settings appear only in their own filter. Choosing a **Look** fills in its settings; changing any of them shows the Look as **Custom**.

**Look:** preset list (see Looks) plus Custom. A new filter starts on its first look. A look sets every setting in Characters (except Custom characters), Motion and the mode's own group, listed or default; it doesn't touch Grid, Custom characters, the custom ink color or the custom palette colors. Settings that don't come through the properties window (a scene file, a script) follow the same rule: a newly named look fills in its settings, and settings that no longer match an unchanged look turn it to Custom. A filter created with settings that name no look keeps them, showing Custom if they differ from the first look.

**Characters:** Set (Blocks / Lines / ASCII / Every glyph / Custom), Custom characters (text, shown only for Custom), and a warning note, shown when the Custom characters give fewer than 2 glyphs and Blocks is used instead.

**Grid:** Cell scale (1×–4×), Alignment (Center / Top-left), Edges (Black / Transparent), Keep transparency (on/off).

**Motion:** Soften, Hold, Settle, Boil, Boil spread, Boil linger.

**Ink only:** Ink color (Cell / Green / Amber / White / Custom + color picker, shown for Custom), Background (Black / Tint), Ink lightness (0.5–1.0, default 0.92), Saturation (0.5–2.0, default 1.4), Gamma (0.7–1.6, default 0.85), Shape (0–0.6, default 0.35), Levels (Auto / Manual), Adapt speed (0.2–5 s, default 1.5 s; shown for Auto), Black point (0–0.6, default 0) and White point (0.2–1.0, default 1) (shown for Manual).

**Poster only:** Levels (2–12, default 6).

**Console only:** Palette (VGA / Custom), **Capture palette** button, a note on what capture does, 16 color pickers (shown for Custom; they start as the VGA colors).

**Capture palette** reads back one downscaled frame (about 160×90, from the Sample pass output, so the grid's area at the cell scale) through a `gs_stagesurface`, and runs k-means on a worker thread over its pixels with alpha ≥ 0.5 (all pixels if none). k-means runs in OKLab: k = 16, k-means++ seeding from a fixed seed (splitmix64), the best of 3 starts by total squared distance, 30 iterations each; an empty cluster takes the point farthest from its center. It orders the colors by pixel count with slots 0–7 the most common, writes them (rounded to 8-bit sRGB) into the 16 color pickers, and sets Palette and the Look to Custom. The result is saved with the scene like any other setting.

The VGA palette is the standard 16-color text-mode palette, ordered so slots 0–7 are the eight normal-intensity colors.

The ranges above are the properties window's; Phase 4 trimmed them to the span where every position looks deliberate (`docs/build-history/phase-4.md` has the measurements). A value set outside them by a scene file or a script is used as before: the filter clamps only the motion settings (to the ranges in Motion) and Poster's Levels (to 2–16).

## Looks

Final values from Phase 4; `docs/LOOKS.md` shows each look on three test stills, with before and after stills for the looks Phase 4 changed. Unlisted settings keep the defaults above. Soften defaults to 0.6 px, Hold to 25, Settle to 0, Boil to 0, Boil spread to 20 and Boil linger to 2 unless noted.

| Filter | Look | Characters | Settings |
| --- | --- | --- | --- |
| Ink | **Black** | ASCII | Cell ink, black background, Hold 25 |
| Ink | **Tint** | ASCII | Cell ink, tint background, Hold 25 |
| Ink | **Phosphor** | ASCII | Green ink, black background, Hold 25 |
| Ink | **Amber** | ASCII | Amber ink, black background, Hold 25 |
| Ink | **Every Glyph** | Every glyph | Cell ink, black background, Hold 25 |
| Ink | **Static** | ASCII | Green ink, black background, Hold 25, Boil 15, spread 20, linger 2 |
| Ink | **Fever** | ASCII | Cell ink, tint background, Hold 10, Boil 45, spread 35, linger 1, Settle 30% |
| Mosaic | **Blocks** | Blocks | Hold 25 |
| Poster | **Poster** | Lines | Levels 6, Hold 25 |
| Console | **Console** | Lines | VGA palette, Hold 25 |

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

CI produces `glyphnotic-<version>-macos-universal.pkg`. It isn't tested; reports are welcome.

### Building from source

`docs/BUILDING.md` covers each platform in its own equal section, following the plugin template's build scripts: Windows (PowerShell, Visual Studio), Linux (bash, CMake + Ninja or Make), macOS (Xcode).

## Reference implementation

`tools/reference/` is a plain Python + NumPy implementation of Soften, Sample, Cell stats and Score for single stills, written from this spec, giving a fresh pick (Hold, Settle and Boil act over time, and are checked over time by `run_motion.py` against the reference's costs). It works in float64 and models OBS's 8-bit premultiplied input (see Color). It outputs the glyph index per cell. `compare.py` diffs it against a grid dump from the plugin, checks the cell colors and opacity, and checks the output frame OBS produced against the picture the dump describes.

A debug build of the plugin (CMake option `GLYPHNOTIC_DEBUG_TOOLS`) adds a **Dump grid** button that writes the shown glyph indices and the cell colors for the current frame to a file, and a **Record grid** button that writes every following frame's held and shown glyphs, boil state and colors. The same build makes `tools/harness`, which runs a filter on a still in a headless libobs (OBS's own image source, or its own frame sequence of stills with fresh noise; real renderer) and saves the dump, a recording and output frames; `run_stills.py` runs it and `compare.py` over every test still; `run_color_paths.py` checks colors along OBS's other color paths (filters before and after, 10-bit and HDR canvases, 16-bit and HDR sources); `run_motion.py` checks the motion settings over time. `tools/README.md` has the commands. None of this is shipped.

## Build phases and acceptance

A builder starting a phase first reads `docs/BUILDER-NOTES.md`, the previous phase's report and this spec's sections for the phase. Each phase ends with a report in `docs/build-history/phase-N.md` covering what was built, how it was tested, measurements, and deviations from this spec with reasons. Then the spec is updated and the phase's boxes are ticked. From Phase 3 on, every phase also ends with the Linux check (`tools/linux-check/`, steps in `tools/README.md`): OBS 33 under `xvfb-run` in an Ubuntu 26.04 container, with each Glyphnotic filter on an image source, must log no errors and 0 memory leaks. Before stopping at the review gate the builder also updates `docs/BUILDER-NOTES.md`. After a phase is approved it is committed and pushed, and CI must be green before the next phase starts.

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
- [x] All four filters with their settings; Auto and Manual levels; VGA and custom palettes; Capture palette
- [x] Reference match as in Phase 1, for each mode and character set (measured: 66 cases over all four modes and all five sets, 442,116 cells; ≥ 99.97% of cells match in every case and 100% in 60 cases; the 6 mismatched cells all have cost gaps under 1e-8; the same on Direct3D 11 and OpenGL)
- [x] All looks from Looks are present and apply (measured: all 10 looks, applied from settings and from the properties window, each picture matching the reference)

**Phase 3: motion**
- [x] Soften, Hold, Settle, Boil, Boil spread, Boil linger, Light jump (measured: Soften matches the reference in ≥ 99.99% of cells in 14 still cases, every mismatch a cost gap under 1e-10; Hold keeps exactly the glyphs the reference's costs predict in 7 cases; Settle follows its formula within 3e-8)
- [x] Flicker test: a still plus fresh Gaussian noise (σ = 2/255) every frame, 600 frames. Report the share of cells that change glyph per frame for each look, with motion off and at the look's settings. At the look's settings, non-boiling looks change ≤ 3% of cells per frame (measured on victor: 2.3–47% with motion off, 0.00–0.11% for the eight non-boiling looks at their settings; Static 3.8%, Fever 11.4%)
- [x] Light jump: alternating between two different stills, the first frame after each switch matches a fresh pick in 100% of cells (measured: victor and a crop of sintel, all 10 looks, 6 switches each: 100%, with the fresh colors)
- [x] Boil 0: two consecutive frames of a still are identical. Boil > 0: the measured swap rate is within ±20% of the setting. A swapped cell returns to its held glyph after its linger, even at Hold 100 (measured: every look's frames identical at Boil 0; 10 cases at 30 and 60 fps within 1.4% of the set rate; every swap lasts exactly its linger, held glyphs unchanged at Hold 10–100; the same on Direct3D 11 and OpenGL. Linux check passed: OBS 33 on Mesa llvmpipe with Ink Static and Fever, no errors, 0 leaks)

**Phase 4: tuning**
- [x] `K_mode` set for each mode, and every slider range trimmed to the span that looks deliberate (measured: Phase 3's `K_mode` confirmed on a panning still with noise, where Hold 100 changes 5–14% as many cells per frame as Hold 0 on a 1 px pan and 10–24% on a 4 px pan in every mode; Gamma, Shape, Black point, White point and Poster's Levels trimmed, the other sliders measured and kept)
- [x] Final look defaults; `docs/LOOKS.md` with a still of each look on at least three test stills (Blocks, Poster and Console at Hold 25: at σ 4/255 noise every look changes ≤ 0.14% of cells per frame; all 10 looks on victor, spring and sintel, drawn by the plugin)
- [x] Ranges, defaults and `K_mode` values written back into this spec (every suite passes on Direct3D 11 and OpenGL with them. Linux check passed: OBS 33 on Mesa llvmpipe, no errors, 0 leaks)

**Phase 5: performance, packaging, release**
- [x] `docs/PERFORMANCE.md`: GPU time per look at 1920×1080, measured with libobs GPU timer queries (or OBS's render-time stat with and without the filter), with the test GPU named (measured on an RTX 4070 Ti, Direct3D 11, as three instances per scene: Ink's ASCII looks 1.25–1.32 ms per frame, Static 1.51, Fever 1.66, Every Glyph 4.01, Blocks, Poster and Console 1.16–1.17)
- [x] On the test machine at 1920×1080, 60 fps: three Ink/ASCII instances on three sources run with no lagged or skipped frames in OBS's stats; one Every Glyph instance does the same (measured: 60 s recordings with NVENC, 0 lagged and 0 skipped frames in both, and with three Fever instances)
- [x] Adding and removing a filter 100 times leaves "Number of memory leaks: 0" in the OBS log at exit (measured: 100 times through OBS's Filters window, and 400 through the API)
- [x] Settings survive an OBS restart; instances on different sources stay independent (measured: three filters' settings, a captured palette included, identical after a restart and each picture pixel-identical; changing one changes only its own source)
- [x] Windows installer and zip, Linux .deb and tarball, macOS .pkg from CI; install and uninstall tested on Windows and Linux by following Install exactly (measured: every package from CI on commit `a755cc5`; CI's installer, zip, .deb and tarball installed, loaded in OBS, uninstalled cleanly, with OBS keeping the filters' settings; the macOS .pkg built, not tested)
- [ ] `README.md` and `docs/INSTALL.md` final; `v1.0.0` tagged with a GitHub Release (README and INSTALL final, version 1.0.0, licensed GPL-2.0-or-later; left: the tag and the release)

## Test assets

Test stills come from openly licensed footage (for example the Blender Foundation's open movies, CC BY). Each still is credited in `tests/stills/ATTRIBUTION.md`. Only stills are committed; no video. The set includes at least one face close-up, one high-contrast scene, one dark scene, one image with an alpha channel, and one synthetic still of known color patches.

## Licensing

- **GPL-2.0-or-later**, decided at the Phase 5 review (2026-11-11), before the `v1.0.0` tag. The root `LICENSE` holds the GPL version 2 text; Glyphnotic's own source files (`src/`, except the template's `plugin-support` files, and `data/effects/`) start with `SPDX-License-Identifier: GPL-2.0-or-later` and a copyright line. `LICENSE` is installed with the plugin's data, so every package carries it, and CI's source tarball goes with each release.
- Why GPL: the plugin links libobs, which is GPL-2.0-or-later, so distributed binaries need a GPL-compatible license. GPL-2.0-or-later matches libobs, the plugin template and most OBS plugins. Packages may be sold or offered as donation-ware; buyers get the source and the GPL's rights with them.
- The plugin template's license terms (reviewed in Phase 0): the template repository ships the GPL-2.0 license text, and its source files carry a "GPL version 2 or (at your option) any later version" notice. Glyphnotic keeps the template's CMake, CI and `plugin-support` files. The template's `LICENSE` file is kept as `docs/licenses/obs-plugintemplate-GPL-2.0.txt` (the same text as the root `LICENSE`).
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
