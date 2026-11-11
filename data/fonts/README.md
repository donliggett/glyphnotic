# Bundled font

`Uni2-Fixed16.psf` is the 8×16 "Uni2 Fixed" Linux console font by Anton
Zinoviev, from Debian's console-setup package.

## License

Public domain. Debian's copyright file for console-setup states:

> All console fonts are public domain by nature.

## Where this copy came from

| | |
| --- | --- |
| Package | `console-setup-linux_1.242~deb13u1_all.deb` (Debian 13 "trixie") |
| Source | https://deb.debian.org/debian/pool/main/c/console-setup/ |
| Path in package | `/usr/share/consolefonts/Uni2-Fixed16.psf.gz` |
| SHA-256 of the source `.psf.gz` | `7413efb11856a7e1ce437ca9752d3f03000889a93264eca2e2dec2923cbb977e` |
| SHA-256 of `Uni2-Fixed16.psf` here | `d9025175dcf18f8b7442009a1870837f6ae974fa9561e9d8dae5eb145566fee7` |
| Copyright file | https://metadata.ftp-master.debian.org/changelogs/main/c/console-setup/console-setup_1.242~deb13u1_copyright |

The `.psf` is the package's `.psf.gz` decompressed, with no other change.
To check it:

```bash
gzip -dc Uni2-Fixed16.psf.gz | sha256sum
```

## Format

PSF1: mode 3 (512 glyphs plus a Unicode table), 16 bytes per glyph (8×16,
one byte per row, most significant bit leftmost), 791 mapped code points,
10,804 bytes. `src/font.cpp` reads it; `tests/unit/font-test.cpp` checks it.
