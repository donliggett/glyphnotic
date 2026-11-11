// Glyphnotic: PSF1 font loading, character sets and the glyph atlas.

#include "font.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

namespace glyphnotic {

namespace {

void set_error(std::string *error, const std::string &message)
{
	if (error)
		*error = message;
}

} // namespace

std::u32string decode_utf8(const std::string &text)
{
	std::u32string out;
	const auto *s = reinterpret_cast<const unsigned char *>(text.data());
	const size_t n = text.size();
	size_t i = 0;
	while (i < n) {
		unsigned char c = s[i];
		int extra;
		char32_t cp;
		if (c < 0x80) {
			cp = c;
			extra = 0;
		} else if ((c & 0xE0) == 0xC0) {
			cp = c & 0x1F;
			extra = 1;
		} else if ((c & 0xF0) == 0xE0) {
			cp = c & 0x0F;
			extra = 2;
		} else if ((c & 0xF8) == 0xF0) {
			cp = c & 0x07;
			extra = 3;
		} else {
			out.push_back(0xFFFD);
			i++;
			continue;
		}
		bool ok = true;
		for (int k = 1; k <= extra; k++) {
			if (i + k >= n || (s[i + k] & 0xC0) != 0x80) {
				ok = false;
				break;
			}
			cp = (cp << 6) | (s[i + k] & 0x3F);
		}
		if (!ok) {
			out.push_back(0xFFFD);
			i++;
			continue;
		}
		out.push_back(cp);
		i += extra + 1;
	}
	return out;
}

bool Font::load_psf1(const std::vector<uint8_t> &data, std::string *error)
{
	enum : uint8_t { MODE512 = 0x01, MODEHASTAB = 0x02 };

	glyphs_.clear();
	cmap_.clear();

	if (data.size() < 4 || data[0] != 0x36 || data[1] != 0x04)
		return set_error(error, "not a PSF1 font"), false;
	const uint8_t mode = data[2];
	const uint8_t charsize = data[3];
	if (charsize != kGlyphHeight)
		return set_error(error, "PSF1 font is not 8x16"), false;
	if (!(mode & MODEHASTAB))
		return set_error(error, "PSF1 font has no Unicode table"), false;

	const size_t count = (mode & MODE512) ? 512 : 256;
	size_t pos = 4;
	if (pos + count * charsize > data.size())
		return set_error(error, "PSF1 font is truncated"), false;

	glyphs_.resize(count);
	for (size_t g = 0; g < count; g++) {
		std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(pos), charsize, glyphs_[g].begin());
		pos += charsize;
	}

	// Unicode table: per glyph, UCS-2 little-endian entries ending in 0xFFFF.
	// 0xFFFE starts a sequence, which runs to the 0xFFFF and is skipped.
	for (size_t g = 0; g < count; g++) {
		bool in_sequence = false;
		for (;;) {
			if (pos + 2 > data.size()) {
				glyphs_.clear();
				cmap_.clear();
				return set_error(error, "PSF1 Unicode table is truncated"), false;
			}
			const uint16_t u = uint16_t(data[pos] | data[pos + 1] << 8);
			pos += 2;
			if (u == 0xFFFF)
				break;
			if (u == 0xFFFE) {
				in_sequence = true;
				continue;
			}
			if (!in_sequence)
				cmap_.emplace(char32_t(u), int(g)); // keeps the first glyph listing it
		}
	}
	return true;
}

bool Font::load_file(const std::string &path, std::string *error)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return set_error(error, "cannot open " + path), false;
	std::vector<uint8_t> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	return load_psf1(raw, error);
}

int Font::glyph_for(char32_t cp) const
{
	auto it = cmap_.find(cp);
	return it == cmap_.end() ? -1 : it->second;
}

std::vector<char32_t> Font::mapped_code_points() const
{
	std::vector<char32_t> out;
	out.reserve(cmap_.size());
	for (const auto &entry : cmap_)
		out.push_back(entry.first);
	return out;
}

int count_lit_pixels(const GlyphBitmap &bitmap)
{
	int n = 0;
	for (uint8_t row : bitmap)
		for (int bit = 0; bit < 8; bit++)
			n += (row >> bit) & 1;
	return n;
}

std::u32string charset_candidates(const Font &font, CharsetId id, const std::string &custom_utf8)
{
	// Blocks: space, full block, light shade, medium shade, black square, black vertical rectangle.
	static const char32_t kBlocks[] = U" █░▒■▮";
	// Lines adds box drawing, diagonals and dots.
	static const char32_t kLines[] = U"─│═║┼├┤┬┴┌┐└"
					 U"┘╭╮╰╯╱╲╳/\\_‗¯·.•"
					 U"●";

	std::u32string out;
	switch (id) {
	case CharsetId::Blocks:
		out = kBlocks;
		break;
	case CharsetId::Lines:
		out = std::u32string(kBlocks) + kLines;
		break;
	case CharsetId::Ascii:
		out.push_back(U' ');
		for (char32_t c = 0x21; c <= 0x7E; c++)
			out.push_back(c);
		break;
	case CharsetId::EveryGlyph:
		out.push_back(U' ');
		for (char32_t cp : font.mapped_code_points())
			if (cp > 0x20 && !(cp >= 0x7F && cp <= 0x9F))
				out.push_back(cp);
		break;
	case CharsetId::Custom:
		out.push_back(U' ');
		out += decode_utf8(custom_utf8);
		break;
	}
	return out;
}

Charset build_charset(const Font &font, CharsetId id, const std::string &custom_utf8)
{
	Charset set;
	std::set<GlyphBitmap> seen;
	for (char32_t cp : charset_candidates(font, id, custom_utf8)) {
		const int g = font.glyph_for(cp);
		if (g < 0)
			continue; // the font lacks it
		const GlyphBitmap &bitmap = font.bitmap(g);
		if (!seen.insert(bitmap).second)
			continue; // same bitmap as an earlier entry
		set.chars.push_back(cp);
		set.glyphs.push_back(g);
		set.n_on.push_back(count_lit_pixels(bitmap));
		set.bitmaps.push_back(bitmap);
	}
	return set;
}

std::vector<uint8_t> build_atlas(const Charset &set)
{
	std::vector<uint8_t> texels(set.size() * kGlyphPixels, 0);
	for (size_t g = 0; g < set.size(); g++) {
		uint8_t *row = texels.data() + g * kGlyphPixels;
		for (int y = 0; y < kGlyphHeight; y++)
			for (int x = 0; x < kGlyphWidth; x++)
				row[y * kGlyphWidth + x] = ((set.bitmaps[g][y] >> (7 - x)) & 1) ? 255 : 0;
	}
	return texels;
}

} // namespace glyphnotic
