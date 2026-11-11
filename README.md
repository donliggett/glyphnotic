# Glyphnotic

An OBS Studio plugin that redraws any video source as a live grid of
characters. It adds four video filters, one per drawing mode: Ink, Mosaic,
Poster and Console.

**Status:** in development. Phase 1 is done: the "Glyphnotic Mosaic" filter
draws the character grid ("Glyphnotic Ink" still passes the picture through
until Phase 2). See
[docs/SPEC.md](docs/SPEC.md) for the full design and build phases, and
[docs/build-history/](docs/build-history/) for a report on each phase.

- Requirements: OBS Studio 33.0.0 or later; Windows 10/11 x64 or Linux x86_64
  (macOS may work).
- Install: [docs/INSTALL.md](docs/INSTALL.md)
- Build from source: [docs/BUILDING.md](docs/BUILDING.md)

## Credits

- Built from the [OBS plugin template](https://github.com/obsproject/obs-plugintemplate)
  (GPL-2.0; see [docs/licenses/](docs/licenses/)).
- Font: Uni2 Fixed 8×16 by Anton Zinoviev, from Debian's console-setup
  (public domain; see [data/fonts/README.md](data/fonts/README.md)).
- Test stills (not shipped) from the Blender Foundation's open movies, CC BY;
  see [tests/stills/ATTRIBUTION.md](tests/stills/ATTRIBUTION.md).

## License

Contains GPL-2.0 code from the OBS plugin template; Glyphnotic's own license is to be decided (the repository stays private until then).
