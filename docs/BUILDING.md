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
install folder as `release/glyphnotic-<version>-windows-x64.zip` and, with
`-BuildInstaller` (whenever CI packages: pushes to `main`, tags and manual
runs), compiles `cmake/windows/resources/installer-Windows.iss` with
[Inno Setup 6](https://jrsoftware.org/isinfo.php) into
`release/glyphnotic-<version>-windows-x64-installer.exe`. CI installs Inno
Setup with Chocolatey when the runner doesn't have it. The installer needs
administrator rights, installs only to
`C:\ProgramData\obs-studio\plugins\glyphnotic\` and registers an
uninstaller; its `AppId` is fixed so upgrades replace the earlier install.

To build the installer by hand, install Inno Setup 6, install the plugin into
`release\RelWithDebInfo` as above, and run from the repository root (the
defines are what `Package-Windows.ps1` passes):

```powershell
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DProductName=glyphnotic /DDisplayName=Glyphnotic /DProductVersion=1.0.0 "/DAuthor=Don Liggett" /DWebsite=https://github.com/donliggett/glyphnotic "/DSourceDir=$PWD\release\RelWithDebInfo" "/DOutputDir=$PWD\release" /DOutputBaseFilename=glyphnotic-1.0.0-windows-x64-installer cmake\windows\resources\installer-Windows.iss
```

## Linux

Requirements: Ubuntu 26.04 LTS (other distributions work with equivalent
packages), OBS 33 from OBS's PPA, CMake 3.28 or later, Ninja, GCC.

```bash
sudo add-apt-repository ppa:obsproject/obs-studio
```

```bash
sudo apt install obs-studio build-essential cmake ninja-build pkg-config libsimde-dev libgles2-mesa-dev zsh jq
```

Check that `apt policy obs-studio` shows 33.0.0 or later from the PPA.
Ubuntu 26.04's own archive has obs-studio 32.x. On systems using Ubuntu's
amd64v3 package variant (GitHub's ubuntu-26.04 runner is one), apt reads only
the PPA's amd64v3 index, which is empty, and installs 32.x. The fix is to add
`Architectures: amd64` under `Types: deb` in
`/etc/apt/sources.list.d/*obsproject*obs-studio*.sources`, then
`sudo apt update`. The CI script `.github/scripts/utils.zsh/setup_ubuntu`
does this, and stops if the installed OBS is older than 33.0.0.

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
  (`glyphnotic/glyphnotic.so`, stripped like the .deb's, and `glyphnotic/data/`) for `~/.local/share/obs-studio/plugins/`
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

## Making a release

1. Set the new version in `buildspec.json` (`version`), commit and push to
   `main`, and wait for CI to pass. Pushes to `main` build the Windows zip
   and installer and the Linux .deb and tarball; a manual run (**Actions →
   Dispatch → Run workflow**, job `build`) builds macOS's .pkg as well.
2. Try the packages from that run (`gh run download <run id>`): on Windows
   by following [INSTALL.md](INSTALL.md); on Linux with
   `tools/linux-check/install-check.sh` and the packages mounted at `/pkgs`
   ([tools/README.md](../tools/README.md)).
3. Tag the commit `v<version>` (for example `v1.0.0`) and push the tag:

   ```bash
   git tag -a v1.0.0 -m "Glyphnotic 1.0.0"
   ```

   ```bash
   git push origin v1.0.0
   ```

   The tag's run builds every platform in the Release configuration, checks
   that the tag matches `buildspec.json`'s version, and creates a **draft**
   GitHub Release named "Glyphnotic <version>" holding every package and a
   `CHECKSUMS.txt` (SHA-256) as its notes. Tags `v<version>-rc<n>` and
   `-beta<n>` make prereleases.
4. Check the draft's files, add notes, and publish it.

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

## Debug build and reference comparison

A build configured with `-DGLYPHNOTIC_DEBUG_TOOLS=ON` adds the filter's
**Dump grid** button and builds `glyphnotic-harness`, which runs the filter on
a still in a headless libobs. `tools/reference/run_stills.py` then compares
every test still with the Python reference implementation. Never ship a
debug build. [tools/README.md](../tools/README.md) has the commands.

## Formatting

CI checks C/C++ with clang-format 19 and CMake files with gersemi, using
`.clang-format` and `.gersemirc`. The versions are those in OBS's Homebrew
tap: clang-format 19.1.7 and gersemi 0.21.0. Both are pinned in
`tools/reference/requirements.txt` and installed in the repository's `.venv`;
[tools/README.md](../tools/README.md#formatting) has the commands.
