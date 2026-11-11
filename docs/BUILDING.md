# Building Glyphnotic from source

Glyphnotic is built with CMake from the OBS plugin template
(`obsproject/obs-plugintemplate`). The template's build scripts live in
`.github/scripts/`; CI runs them, and so can you.

All platforms build against **OBS Studio 33.0.1**, set in `buildspec.json`.
On Windows and macOS the first configure downloads OBS's sources and prebuilt
dependencies into `.deps/` and builds libobs from them, which takes a few
minutes; later configures reuse it. On Linux, libobs comes from OBS's PPA.

The unit tests need only a C++ compiler and CMake 3.28 or later; see
[Unit tests](#unit-tests).

## Windows

Requirements:

- Windows 10 or 11, x64
- Visual Studio 2026 (version 18), or its Build Tools, with the "Desktop
  development with C++" workload and Windows SDK 10.0.26100
- CMake 4.2 or later (Visual Studio's bundled CMake is fine). Older CMake
  can't generate Visual Studio 2026 projects.
- Git

In PowerShell, from the repository root:

```powershell
cmake --preset windows-x64
```

```powershell
cmake --build --preset windows-x64
```

```powershell
cmake --install build_x64 --prefix release/RelWithDebInfo --config RelWithDebInfo
```

`release\RelWithDebInfo\glyphnotic\` then holds `glyphnotic.dll` and `data\`.
To try it, close OBS and copy that `glyphnotic` folder into
`C:\ProgramData\obs-studio\plugins\`.

If `cmake` isn't on your PATH, use Visual Studio's copy at
`C:\Program Files\Microsoft Visual Studio\18\<edition>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
(Build Tools install under `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\`).

CI builds with `.github/scripts/Build-Windows.ps1` and packages with
`.github/scripts/Package-Windows.ps1` (PowerShell 7, CI only), which zips the
install folder as `release/glyphnotic-<version>-windows-x64.zip`.

## Linux

Requirements: Ubuntu 26.04 LTS (other distributions work with equivalent
packages), OBS 33 from OBS's PPA, CMake 3.28 or later, Ninja, GCC.

```bash
sudo add-apt-repository ppa:obsproject/obs-studio
```

```bash
sudo apt install obs-studio build-essential cmake ninja-build pkg-config libsimde-dev libgles2-mesa-dev zsh jq
```

Check that `apt policy obs-studio` shows 33.0.0 or later from the PPA. Ubuntu
26.04's own archive has obs-studio 32.x, and on amd64v3 systems apt can pick
it over the PPA's build. The CI script `.github/scripts/utils.zsh/setup_ubuntu`
avoids this by pinning the PPA (`/etc/apt/preferences.d/obsproject-obs-studio`,
`Pin: release o=LP-PPA-obsproject-obs-studio`, priority 1001), and it stops
if the installed OBS is older than 33.0.0.

Then, from the repository root, either run the template's scripts (as CI does):

```bash
./.github/scripts/build-ubuntu --target ubuntu-x86_64 --config RelWithDebInfo
```

```bash
./.github/scripts/package-ubuntu --target ubuntu-x86_64 --config RelWithDebInfo --package
```

or use CMake directly:

```bash
cmake --preset ubuntu-x86_64 -DCMAKE_INSTALL_PREFIX=/usr
```

```bash
cmake --build --preset ubuntu-x86_64
```

```bash
cmake --install build_x86_64 --prefix release/RelWithDebInfo
```

The scripts install into `release/RelWithDebInfo/` in the system layout
(`lib/x86_64-linux-gnu/obs-modules/plugins/glyphnotic.so`,
`share/obs/obs-modules/plugins/glyphnotic/`) and package:

- `release/glyphnotic-<version>-linux-x86_64.deb` (with `--package`), plus its `-dbgsym.ddeb`
- `release/glyphnotic-<version>-linux-x86_64.tar.xz`, the per-user layout
  (`glyphnotic/glyphnotic.so`, `glyphnotic/data/`) for `~/.local/share/obs-studio/plugins/`
- `release/glyphnotic-<version>-source.tar.xz`

## macOS

Requirements: macOS 13 or later, Xcode 26, CMake 3.28 or later. CI builds macOS
only for release tags and manual runs, and macOS builds are not tested.

```bash
cmake --preset macos
```

```bash
cmake --build --preset macos
```

CI uses `.github/scripts/build-macos` and `.github/scripts/package-macos`,
which produce `release/glyphnotic-<version>-macos-universal.pkg`. Without
signing secrets the package is unsigned.

## Unit tests

`tests/unit` is a standalone CMake project with no OBS dependency. From the
repository root, on any platform:

```bash
cmake -S tests/unit -B build_tests
```

```bash
cmake --build build_tests --config Release
```

```bash
ctest --test-dir build_tests -C Release --output-on-failure
```

CI runs these on every platform it builds.

## Formatting

CI checks C/C++ with clang-format 19 and CMake files with gersemi, using
`.clang-format` and `.gersemirc`. The versions are those in OBS's Homebrew
tap: clang-format 19 and gersemi 0.21.0.
