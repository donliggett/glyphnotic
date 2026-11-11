# Builder notes

How Glyphnotic is built and tested in practice, from Phases 0–3. Commands
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

One case: `run_stills.py ... --only ink-victor`; some motion suites:
`run_motion.py ... --only boil,jump` (suites: source, flicker, still, boil,
jump, hold, settle; `--flicker-frames 150` for a quick flicker run). Every
suite exits 0 when all pass and must pass on both renderers. The two
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
