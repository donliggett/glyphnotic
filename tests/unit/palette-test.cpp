// Unit tests for src/palette.cpp (color conversions, the VGA palette and
// palette capture's k-means) and src/presets.cpp (the looks table).
// Exits 0 when every check passes.

#include "palette.hpp"
#include "presets.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
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

void check_near(double got, double want, double tol, const std::string &what)
{
	check(std::fabs(got - want) <= tol, what + ": got " + std::to_string(got) + ", want " + std::to_string(want));
}

void test_conversions()
{
	const Lab white = srgb8_to_oklab({255, 255, 255});
	check_near(white.L, 1.0, 1e-6, "white L");
	check_near(white.a, 0.0, 1e-6, "white a");
	check_near(white.b, 0.0, 1e-6, "white b");
	const Lab black = srgb8_to_oklab({0, 0, 0});
	check_near(black.L, 0.0, 1e-12, "black L");

	bool all_round_trip = true;
	for (int v = 0; v < 256; v++) {
		const Rgb8 c{static_cast<uint8_t>(v), static_cast<uint8_t>(255 - v), static_cast<uint8_t>(v / 2)};
		if (!(oklab_to_srgb8(srgb8_to_oklab(c)) == c))
			all_round_trip = false;
	}
	check(all_round_trip, "sRGB8 -> OKLab -> sRGB8 round trip");

	for (const Rgb8 &c : vga_palette())
		check(oklab_to_srgb8(srgb8_to_oklab(c)) == c, "VGA color round trip");

	const Rgb8 c{0x12, 0x34, 0x56};
	check(rgb8_to_obs(c) == 0xFF563412LL, "OBS color packing is 0xAABBGGRR");
	check(rgb8_from_obs(0xFF563412LL) == c, "OBS color unpacking");
}

void test_vga()
{
	const Palette &p = vga_palette();
	check(p[0] == Rgb8{0, 0, 0}, "VGA 0 is black");
	check(p[6] == Rgb8{0xAA, 0x55, 0x00}, "VGA 6 is brown");
	check(p[7] == Rgb8{0xAA, 0xAA, 0xAA}, "VGA 7 is light gray");
	check(p[8] == Rgb8{0x55, 0x55, 0x55}, "VGA 8 is dark gray");
	check(p[15] == Rgb8{0xFF, 0xFF, 0xFF}, "VGA 15 is white");
	// Slots 0-7 are the normal-intensity colors: no channel above 0xAA.
	bool normal = true;
	for (int k = 0; k < kBackgroundSlots; k++)
		if (p[k].r > 0xAA || p[k].g > 0xAA || p[k].b > 0xAA)
			normal = false;
	check(normal, "VGA slots 0-7 are normal intensity");
}

void test_kmeans()
{
	// 16 well-separated colors, color i with 40 + 10*i points, slightly jittered.
	std::vector<Lab> truth;
	for (int i = 0; i < kPaletteSize; i++)
		truth.push_back({0.1 + 0.05 * i, (i % 4) * 0.08 - 0.12, (i / 4) * 0.08 - 0.12});
	std::vector<Lab> pts;
	for (int i = 0; i < kPaletteSize; i++)
		for (int j = 0; j < 40 + 10 * i; j++) {
			const double e = 0.002 * ((j % 5) - 2);
			pts.push_back({truth[static_cast<size_t>(i)].L + e, truth[static_cast<size_t>(i)].a - e,
				       truth[static_cast<size_t>(i)].b + e * 0.5});
		}

	const KMeansResult r = kmeans_palette(pts);
	// Ordered by count, largest first: truth index 15 first.
	bool ordered = true, found = true;
	for (int k = 0; k < kPaletteSize; k++) {
		const size_t want_count = static_cast<size_t>(40 + 10 * (15 - k));
		if (r.counts[static_cast<size_t>(k)] != want_count)
			ordered = false;
		const Lab &t = truth[static_cast<size_t>(15 - k)];
		const Lab &c = r.centers[static_cast<size_t>(k)];
		if (std::fabs(c.L - t.L) > 1e-9 || std::fabs(c.a - t.a) > 1e-9 || std::fabs(c.b - t.b) > 1e-9)
			found = false;
	}
	check(ordered, "k-means: slots ordered by count, largest first");
	check(found, "k-means: finds the 16 cluster means");

	const KMeansResult again = kmeans_palette(pts);
	bool same = again.sse == r.sse;
	for (size_t k = 0; k < kPaletteSize; k++)
		same = same && again.centers[k].L == r.centers[k].L && again.centers[k].a == r.centers[k].a &&
		       again.centers[k].b == r.centers[k].b;
	check(same, "k-means: repeatable (fixed seed)");

	// Three distinct colors only: three slots hold them, the rest count 0.
	std::vector<Lab> few;
	for (int j = 0; j < 30; j++)
		few.push_back({0.2 + 0.3 * (j % 3), 0.0, 0.0});
	const KMeansResult f = kmeans_palette(few);
	size_t total = 0, used = 0;
	for (size_t k = 0; k < kPaletteSize; k++) {
		total += f.counts[k];
		used += f.counts[k] > 0 ? 1 : 0;
	}
	check(total == 30 && used == 3, "k-means: three colors give three used slots");
	check_near(f.sse, 0.0, 1e-12, "k-means: three colors fit exactly");

	const KMeansResult none = kmeans_palette({});
	check(none.counts[0] == 0, "k-means: no points");
}

void test_looks()
{
	check(looks_for(Mode::Ink).size() == 7, "Ink has 7 looks");
	check(looks_for(Mode::Mosaic).size() == 1, "Mosaic has 1 look");
	check(looks_for(Mode::Poster).size() == 1, "Poster has 1 look");
	check(looks_for(Mode::Console).size() == 1, "Console has 1 look");
	check(strcmp(looks_for(Mode::Ink).front().id, "black") == 0, "Ink's default look is Black");

	std::set<std::string> ids;
	for (int m = 0; m < kModeCount; m++) {
		const auto mode = static_cast<Mode>(m);
		const auto &base = looks_for(mode).front().values;
		for (const Look &look : looks_for(mode)) {
			ids.insert(look.id);
			bool same_keys = look.values.size() == base.size();
			for (size_t i = 0; same_keys && i < base.size(); i++)
				same_keys = strcmp(look.values[i].key, base[i].key) == 0;
			check(same_keys, std::string("look ") + look.id + " sets every controlled setting");
			check(find_look(mode, look.id) == &look, std::string("find_look ") + look.id);
		}
		check(find_look(mode, kLookCustom) == nullptr, "custom isn't a look");
		check(look_controls(mode, key::kCharset), "looks control the character set");
		check(look_controls(mode, key::kHold), "looks control Hold");
		check(!look_controls(mode, key::kCellScale), "looks don't control the cell scale");
		check(!look_controls(mode, key::kCustomChars), "looks don't control custom characters");
	}
	check(ids.size() == 10, "10 distinct look ids");
	check(look_controls(Mode::Ink, key::kGamma) && !look_controls(Mode::Mosaic, key::kGamma),
	      "Ink settings belong to Ink's looks only");
	check(!look_controls(Mode::Console, "palette_0"), "custom palette colors aren't part of a look");

	const Look *fever = find_look(Mode::Ink, "fever");
	bool fever_ok = fever != nullptr;
	for (size_t i = 0; fever && i < fever->values.size(); i++) {
		const LookValue &v = fever->values[i];
		if (strcmp(v.key, key::kBoil) == 0)
			fever_ok = fever_ok && v.i == 45;
		if (strcmp(v.key, key::kSettle) == 0)
			fever_ok = fever_ok && v.i == 30;
		if (strcmp(v.key, key::kInkBackground) == 0)
			fever_ok = fever_ok && v.s == "tint";
		if (strcmp(v.key, key::kGamma) == 0)
			fever_ok = fever_ok && v.d == 0.85;
	}
	check(fever_ok, "Fever's values");
}

} // namespace

int main()
{
	test_conversions();
	test_vga();
	test_kmeans();
	test_looks();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
