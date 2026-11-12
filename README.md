# Glyphnotic

An OBS Studio plugin that redraws any video source as a live grid of
characters. It adds four video filters, one per drawing mode:

| Filter | Look |
| --- | --- |
| Glyphnotic Ink | Characters chosen by brightness and shape, lit in the picture's own colors or a fixed ink |
| Glyphnotic Mosaic | Block characters with free text and background colors |
| Glyphnotic Poster | Mosaic with colors snapped to a few levels per channel |
| Glyphnotic Console | Characters and colors limited to a 16-color palette, VGA or captured from the picture |

Each is an ordinary OBS filter: it goes on any source, scene or group,
stacks with other filters, and every instance keeps its own settings. Each
comes with named looks (ten in all; [docs/LOOKS.md](docs/LOOKS.md) shows
them) and motion controls, from a locked, still grid that holds steady on a
noisy camera to a deliberately boiling one.

**Version 1.0.0.** Requirements: OBS Studio 33.0.0 or later, on Windows 10/11
x64 or Linux x86_64 (Ubuntu 26.04 LTS and current Debian get a .deb; other
distributions use the tarball or build from source). macOS packages are
built but not tested.

- Install: [docs/INSTALL.md](docs/INSTALL.md)
- The looks: [docs/LOOKS.md](docs/LOOKS.md)
- GPU cost: [docs/PERFORMANCE.md](docs/PERFORMANCE.md) (on a 1080p source
  and an RTX 4070 Ti, about 1.3 ms of GPU time per frame for an ASCII Ink
  look and 4 ms for Every Glyph)
- Build from source: [docs/BUILDING.md](docs/BUILDING.md)
- Design and acceptance tests: [docs/SPEC.md](docs/SPEC.md), with a report
  on each build phase in [docs/build-history/](docs/build-history/)

## Using it

Right-click a source → **Filters** → **+** → a Glyphnotic filter. Pick a
**Look**, then adjust:

- **Characters**: the set (Blocks, Lines, ASCII, Every glyph, or your own
  characters).
- **Grid**: cell scale (1×–4×; larger cells are cheaper and chunkier),
  alignment, the edges' color, and whether transparent areas stay
  transparent.
- **Motion**: Soften and Hold calm a live camera's noise; Settle smooths
  color changes; Boil, Boil spread and Boil linger swap glyphs on purpose.
- The filter's own settings: Ink's color, background, levels and shape;
  Poster's levels; Console's palette (**Capture palette** takes 16 colors
  from the current picture).

Changing a setting turns the look to **Custom**; picking a look again
restores its values.

## Credits

- Built from the [OBS plugin template](https://github.com/obsproject/obs-plugintemplate)
  (GPL-2.0; see [docs/licenses/](docs/licenses/)).
- Font: Uni2 Fixed 8×16 by Anton Zinoviev, from Debian's console-setup
  (public domain; see [data/fonts/README.md](data/fonts/README.md)).
- Test stills (not shipped) from the Blender Foundation's open movies, CC BY;
  see [tests/stills/ATTRIBUTION.md](tests/stills/ATTRIBUTION.md).

## License

Glyphnotic is free software, licensed under the **GNU General Public License,
version 2 or (at your option) any later version** (GPL-2.0-or-later); see
[LICENSE](LICENSE). It's distributed in the hope that it will be useful, but
without any warranty. Every package includes the license, and each release
has the matching source (`glyphnotic-<version>-source.tar.xz`).

Copyright (C) 2026 Don Liggett. Glyphnotic links libobs (GPL-2.0-or-later)
and keeps the OBS plugin template's build files (GPL-2.0-or-later). The
bundled font is public domain.
