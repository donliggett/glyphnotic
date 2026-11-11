// Glyphnotic: PSF1 font loading, character sets and the glyph atlas.
//
// This file has no libobs dependency, so the unit tests in tests/unit build
// and run without OBS.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace glyphnotic {

constexpr int kGlyphWidth = 8;
constexpr int kGlyphHeight = 16;
constexpr int kGlyphPixels = kGlyphWidth * kGlyphHeight; // 128

// One 8x16 glyph: 16 rows of one byte, most significant bit is the leftmost pixel.
using GlyphBitmap = std::array<uint8_t, kGlyphHeight>;

// Decodes UTF-8 into code points. Malformed bytes become U+FFFD.
std::u32string decode_utf8(const std::string &text);

class Font {
public:
	// Parses an uncompressed PSF1 font. Only 8x16 fonts with a Unicode table are accepted.
	bool load_psf1(const std::vector<uint8_t> &data, std::string *error);
	// Reads a file and parses it as PSF1.
	bool load_file(const std::string &path, std::string *error);

	size_t glyph_count() const { return glyphs_.size(); }
	const GlyphBitmap &bitmap(int glyph) const { return glyphs_[static_cast<size_t>(glyph)]; }
	// Index of the first glyph whose Unicode entry lists cp, or -1 if the font lacks it.
	int glyph_for(char32_t cp) const;
	// All mapped code points in ascending order.
	std::vector<char32_t> mapped_code_points() const;

private:
	std::vector<GlyphBitmap> glyphs_;
	std::map<char32_t, int> cmap_;
};

enum class CharsetId { Blocks = 0, Lines, Ascii, EveryGlyph, Custom };

// Minimum number of glyphs (including space) a usable set has.
constexpr size_t kMinCharsetSize = 2;

struct Charset {
	std::u32string chars;    // the character shown for each entry
	std::vector<int> glyphs; // font glyph index for each entry
	std::vector<int> n_on;   // lit pixels per entry
	std::vector<GlyphBitmap> bitmaps;

	size_t size() const { return glyphs.size(); }
	bool usable() const { return size() >= kMinCharsetSize; }
};

// The characters a built-in set asks for, before font filtering and de-duplication.
// Space comes first. For EveryGlyph the list comes from the font.
std::u32string charset_candidates(const Font &font, CharsetId id, const std::string &custom_utf8);

// Builds a set: space first, characters the font lacks skipped, and any
// character whose bitmap matches an earlier entry's dropped.
Charset build_charset(const Font &font, CharsetId id, const std::string &custom_utf8 = {});

// Atlas texels for a set: one row per glyph, 128 texels per row (row-major
// 8x16), 255 for a lit pixel and 0 for an unlit one. Size is set.size() * 128.
std::vector<uint8_t> build_atlas(const Charset &set);

int count_lit_pixels(const GlyphBitmap &bitmap);

} // namespace glyphnotic
