# Installing Glyphnotic

Glyphnotic needs **OBS Studio 33.0.0 or later**. It installs only to the plugin
locations OBS 33 introduced; it never uses the legacy locations, which OBS
flags as "Legacy" in **Tools → Plugin Manager** and stops loading in OBS 34.

Packages are attached to GitHub Releases. Until the repository is public,
only signed-in collaborators can download them.

Each release has:

| File | For |
| --- | --- |
| `glyphnotic-<version>-windows-x64-installer.exe` | Windows: installs for all users and adds an uninstaller |
| `glyphnotic-<version>-windows-x64.zip` | Windows by hand, and portable OBS |
| `glyphnotic-<version>-linux-x86_64.deb` | Ubuntu 26.04 LTS and current Debian with OBS 33 from OBS's PPA |
| `glyphnotic-<version>-linux-x86_64.tar.xz` | Other Linux distributions: a per-user install |
| `glyphnotic-<version>-macos-universal.pkg` | macOS (may work; not tested) |

`CHECKSUMS.txt` in the release notes lists each file's SHA-256.

## Where OBS 33 looks for plugins

These paths were confirmed against OBS 33.0.1's source
(`frontend/plugin-manager/PluginModuleLoader_*.cpp`) and OBS's
[Legacy Plugin Developer Info](https://obsproject.com/kb/legacy-plugin-developer-info)
page, and the Windows path was tested with OBS 33.0.1 (see
`docs/build-history/phase-0.md`).

### Windows

| | Path |
| --- | --- |
| Plugin | `C:\ProgramData\obs-studio\plugins\glyphnotic\glyphnotic.dll` |
| Data | `C:\ProgramData\obs-studio\plugins\glyphnotic\data\` |
| Portable OBS | `<OBS folder>\plugins\glyphnotic\glyphnotic.dll` and `<OBS folder>\plugins\glyphnotic\data\` (portable OBS ignores ProgramData) |

The plugin folder's name must match the DLL's name. Legacy locations, not
used: `C:\ProgramData\obs-studio\plugins\<plugin>\bin\64bit\` and
`C:\Program Files\obs-studio\obs-plugins\64bit\`.

### Linux

| | Path |
| --- | --- |
| System (the .deb) | `/usr/lib/x86_64-linux-gnu/obs-modules/plugins/glyphnotic.so` and `/usr/share/obs/obs-modules/plugins/glyphnotic/` |
| Per user (the tarball) | `~/.local/share/obs-studio/plugins/glyphnotic/glyphnotic.so` and `~/.local/share/obs-studio/plugins/glyphnotic/data/` |

The per-user folder is `$XDG_DATA_HOME/obs-studio/plugins/`, which is
`~/.local/share/obs-studio/plugins/` when `XDG_DATA_HOME` isn't set. The system
folder is fixed when OBS is built (`<prefix>/<libdir>/obs-modules/plugins`);
`/usr/lib/x86_64-linux-gnu` is what OBS's own Ubuntu packages use. OBS
builds from other distributions may use a different libdir, so on those use
the tarball. Legacy locations, not used: `~/.config/obs-studio/plugins/<plugin>/bin/64bit/`
and `/usr/lib/<arch>/obs-plugins/`.

Flatpak builds of OBS aren't covered in v1.0.

### macOS (may work)

| | Path |
| --- | --- |
| Plugin bundle | `~/Library/Application Support/obs-studio/plugins/glyphnotic.plugin` |

OBS 33 didn't change the macOS location for bundles. The legacy macOS
layout (`.../obs-studio/plugins/<plugin>/bin/<plugin>.so`) is not used.

## Windows

1. Close OBS.
2. Run `glyphnotic-<version>-windows-x64-installer.exe` and accept Windows'
   prompt to let it make changes (it installs into `C:\ProgramData`, for
   all users). It installs to `C:\ProgramData\obs-studio\plugins\glyphnotic\`;
   there's no folder to choose. Or, without the installer, unzip
   `glyphnotic-<version>-windows-x64.zip` into `C:\ProgramData\obs-studio\plugins\`
   so that it contains a `glyphnotic` folder.
3. Start OBS. Right-click a source → **Filters** → **+** and pick a Glyphnotic filter.

**Tools → Plugin Manager** lists Glyphnotic with its version.

The installer isn't code-signed, so Windows SmartScreen may say "Windows
protected your PC" when you run a downloaded copy: choose **More info →
Run anyway**, or use the zip.

Portable OBS ignores `C:\ProgramData`: unzip the zip into the `plugins`
folder inside OBS's own folder (create it if needed), so that it contains
`plugins\glyphnotic\glyphnotic.dll`.

Uninstall: close OBS, then **Settings → Apps → Installed apps** →
**Glyphnotic (OBS Studio plugin)** → **Uninstall** if you used the
installer, or delete `C:\ProgramData\obs-studio\plugins\glyphnotic` if you
unzipped it. Settings saved in your scene collections stay there; a filter
whose plugin is missing is kept by OBS and works again once the plugin is back.

To upgrade, close OBS and run the newer installer (or unzip the newer zip
over the old folder).

## Linux

1. Close OBS.
2. On Ubuntu 26.04 LTS or current Debian with OBS 33 from OBS's PPA, run:

   ```bash
   sudo apt install ./glyphnotic-<version>-linux-x86_64.deb
   ```

   Otherwise, extract the tarball into the per-user folder:

   ```bash
   mkdir -p ~/.local/share/obs-studio/plugins
   ```

   ```bash
   tar -xJf glyphnotic-<version>-linux-x86_64.tar.xz -C ~/.local/share/obs-studio/plugins
   ```

3. Start OBS. Right-click a source → **Filters** → **+** and pick a Glyphnotic filter.

Uninstall: close OBS, then `sudo apt remove glyphnotic`, or delete
`~/.local/share/obs-studio/plugins/glyphnotic` for the tarball. To upgrade,
install the newer .deb the same way, or extract the newer tarball over the
old folder.

## macOS (may work)

CI produces `glyphnotic-<version>-macos-universal.pkg`, which installs for the
current user into `~/Library/Application Support/obs-studio/plugins/`. It isn't
tested; reports are welcome.
