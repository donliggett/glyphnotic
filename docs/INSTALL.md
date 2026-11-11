# Installing Glyphnotic

Glyphnotic needs **OBS Studio 33.0.0 or later**. It installs only to the plugin
locations OBS 33 introduced; it never uses the legacy locations, which OBS
flags as "Legacy" in **Tools → Plugin Manager** and stops loading in OBS 34.

Packages are attached to GitHub Releases. Until the repository is public,
only signed-in collaborators can download them.

> Status (Phase 0): the Windows zip and Linux .deb and tarball are produced by CI on every push to main; the macOS .pkg only for release tags and manual runs.
> The Windows installer arrives in Phase 5. Install and
> uninstall are tested end to end in Phase 5.

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
2. Unzip `glyphnotic-<version>-windows-x64.zip` into
   `C:\ProgramData\obs-studio\plugins\` so that it contains a `glyphnotic`
   folder. (Once it exists, run `glyphnotic-<version>-windows-x64-installer.exe` instead.)
3. Start OBS. Right-click a source → **Filters** → **+** and pick a Glyphnotic filter.

Uninstall: delete `C:\ProgramData\obs-studio\plugins\glyphnotic`.

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

Uninstall: `sudo apt remove glyphnotic`, or delete
`~/.local/share/obs-studio/plugins/glyphnotic`.

## macOS (may work)

CI produces `glyphnotic-<version>-macos-universal.pkg`, which installs for the
current user into `~/Library/Application Support/obs-studio/plugins/`. It isn't
tested; reports are welcome.
