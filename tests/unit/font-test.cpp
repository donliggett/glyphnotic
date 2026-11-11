// Unit tests for src/font.cpp: PSF1 parsing, character sets and the atlas.
// Exits 0 when every check passes.

#include "font.hpp"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace glyphnotic;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string &what)
{
	g_checks++;
	if (!ok) {
		g_failures++;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void check_eq(size_t got, size_t want, const std::string &what)
{
	check(got == want, what + ": got " + std::to_string(got) + ", want " + std::to_string(want));
}

void test_font_file(const Font &font)
{
	check_eq(font.glyph_count(), 512, "Uni2-Fixed16 glyph count");
	check(font.glyph_for(U' ') >= 0, "font maps space");
	check(font.glyph_for(U'A') >= 0, "font maps 'A'");
	check(font.glyph_for(0x2588) >= 0, "font maps U+2588 full block");
	check(font.glyph_for(0x10FFFF) < 0, "font does not map U+10FFFF");
	check_eq(static_cast<size_t>(count_lit_pixels(font.bitmap(font.glyph_for(U' ')))), 0,
		 "space has no lit pixels");
	check_eq(static_cast<size_t>(count_lit_pixels(font.bitmap(font.glyph_for(0x2588)))), 128,
		 "full block lights all 128 pixels");
}

void test_set(const Font &font, CharsetId id, const char *name, size_t want)
{
	const Charset set = build_charset(font, id);
	check_eq(set.size(), want, std::string(name) + " unique glyphs");
	check(!set.chars.empty() && set.chars[0] == U' ', std::string(name) + " starts with space");
	std::set<GlyphBitmap> unique(set.bitmaps.begin(), set.bitmaps.end());
	check_eq(unique.size(), set.size(), std::string(name) + " has no two identical bitmaps");
	check(set.glyphs.size() == set.n_on.size() && set.glyphs.size() == set.chars.size(),
	      std::string(name) + " parallel arrays agree");
	for (size_t i = 0; i < set.size(); i++)
		if (count_lit_pixels(set.bitmaps[i]) != set.n_on[i])
			check(false, std::string(name) + " n_on matches bitmap");
}

void test_sets(const Font &font)
{
	test_set(font, CharsetId::Blocks, "Blocks", 5);
	test_set(font, CharsetId::Lines, "Lines", 25);
	test_set(font, CharsetId::Ascii, "ASCII", 95);
	test_set(font, CharsetId::EveryGlyph, "Every glyph", 503);

	// Lines starts with the Blocks set.
	const Charset blocks = build_charset(font, CharsetId::Blocks);
	const Charset lines = build_charset(font, CharsetId::Lines);
	bool prefix = lines.size() >= blocks.size();
	for (size_t i = 0; prefix && i < blocks.size(); i++)
		prefix = lines.chars[i] == blocks.chars[i];
	check(prefix, "Lines begins with Blocks");

	// Every glyph is in code-point order after space and skips U+007F..U+009F.
	const Charset every = build_charset(font, CharsetId::EveryGlyph);
	bool ordered = true, excluded = true;
	for (size_t i = 1; i < every.size(); i++) {
		if (i > 1 && every.chars[i] <= every.chars[i - 1])
			ordered = false;
		if (every.chars[i] <= 0x20 || (every.chars[i] >= 0x7F && every.chars[i] <= 0x9F))
			excluded = false;
	}
	check(ordered, "Every glyph is in code-point order");
	check(excluded, "Every glyph skips control ranges");
}

void test_custom(const Font &font)
{
	// Space first, then the typed characters in order; duplicates and missing characters dropped.
	Charset set = build_charset(font, CharsetId::Custom, "#@#  .");
	check(set.chars == std::u32string(U" #@."), "Custom keeps order and drops duplicates");

	set = build_charset(font, CharsetId::Custom, "\xE2\x96\x88\xE2\x96\x91"); // UTF-8 for U+2588 U+2591
	check(set.chars == std::u32string(U" █░"), "Custom decodes UTF-8");

	set = build_charset(font, CharsetId::Custom, "\xF4\x8F\xBF\xBF"); // U+10FFFF, not in the font
	check_eq(set.size(), 1, "Custom skips characters the font lacks");
	check(!set.usable(), "a set of only space is not usable");

	set = build_charset(font, CharsetId::Custom, "");
	check(!set.usable(), "empty Custom set is not usable");
	set = build_charset(font, CharsetId::Custom, "x");
	check(set.usable(), "space plus one character is usable");
}

void test_atlas(const Font &font)
{
	const Charset set = build_charset(font, CharsetId::Ascii);
	const std::vector<uint8_t> atlas = build_atlas(set);
	check_eq(atlas.size(), set.size() * 128, "atlas is one 128-texel row per glyph");
	bool match = true;
	for (size_t g = 0; g < set.size() && match; g++) {
		int lit = 0;
		for (int i = 0; i < 128; i++) {
			const uint8_t t = atlas[g * 128 + static_cast<size_t>(i)];
			if (t != 0 && t != 255)
				match = false;
			lit += t == 255;
			// Row-major 8x16: texel i is pixel (x = i % 8, y = i / 8); MSB is leftmost.
			const bool bit = (set.bitmaps[g][static_cast<size_t>(i / 8)] >> (7 - i % 8)) & 1;
			if (bit != (t == 255))
				match = false;
		}
		if (lit != set.n_on[g])
			match = false;
	}
	check(match, "atlas texels match the glyph bitmaps");
}

void test_bad_input()
{
	Font font;
	std::string error;
	check(!font.load_psf1({0x00, 0x01, 0x02, 0x03}, &error), "rejects data without PSF1 magic");
	check(!font.load_psf1({0x36, 0x04, 0x03, 0x10, 0x00}, &error), "rejects truncated PSF1");
	check(!font.load_psf1({0x36, 0x04, 0x01, 0x10}, &error), "rejects PSF1 without a Unicode table");
	check(!font.load_file("does-not-exist.psf", &error), "reports a missing file");
}

void test_utf8()
{
	check(decode_utf8("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80") == std::u32string(U"Aé€\U0001F600"),
	      "decodes 1-4 byte UTF-8");
	check(decode_utf8("\xFF"
			  "a") == std::u32string(U"�a"),
	      "replaces an invalid lead byte");
	check(decode_utf8("\xE2\x82") == std::u32string(U"��"), "replaces a truncated sequence");
}

} // namespace

int main()
{
	Font font;
	std::string error;
	if (!font.load_file(GLYPHNOTIC_FONT_PATH, &error)) {
		std::printf("FAIL: cannot load %s: %s\n", GLYPHNOTIC_FONT_PATH, error.c_str());
		return 1;
	}

	test_font_file(font);
	test_sets(font);
	test_custom(font);
	test_atlas(font);
	test_bad_input();
	test_utf8();

	std::printf("%d checks, %d failed\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
