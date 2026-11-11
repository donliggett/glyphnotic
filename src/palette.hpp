// Glyphnotic: color conversions, the built-in palette and palette capture
// (k-means in OKLab).
//
// This file has no libobs dependency, so the unit tests in tests/unit build
// and run without OBS.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace glyphnotic {

constexpr int kPaletteSize = 16;
constexpr int kBackgroundSlots = 8; // the background may use slots 0..7

struct Lab {
	double L = 0.0, a = 0.0, b = 0.0;
};

struct Rgb8 {
	uint8_t r = 0, g = 0, b = 0;
	bool operator==(const Rgb8 &o) const { return r == o.r && g == o.g && b == o.b; }
};

using Palette = std::array<Rgb8, kPaletteSize>;

// The standard 16-color text-mode (VGA) palette: slots 0-7 are the eight
// normal-intensity colors, 8-15 the bright ones.
const Palette &vga_palette();

double srgb_decode(double u);
double srgb_encode(double u); // clips to [0, 1]
Lab linear_to_oklab(double r, double g, double b);
void oklab_to_linear(const Lab &c, double out[3]); // not clipped
Lab srgb8_to_oklab(Rgb8 c);
// OKLab to sRGB 0-255, clipped, unrounded.
void oklab_to_srgb255(const Lab &c, double out[3]);
// OKLab to 8-bit sRGB, clipped and rounded.
Rgb8 oklab_to_srgb8(const Lab &c);

// OBS color properties store colors as 0xAABBGGRR.
Rgb8 rgb8_from_obs(long long color);
long long rgb8_to_obs(Rgb8 c);

// Palette capture: k-means in OKLab with k = 16, a fixed seed, the best of 3
// starts (k-means++ seeding) and 30 iterations each. The result is ordered by
// cluster size, largest first, so slots 0-7 are the most common colors.
// With fewer than 16 distinct colors, some slots repeat a color and count 0.
struct KMeansResult {
	std::array<Lab, kPaletteSize> centers;
	std::array<size_t, kPaletteSize> counts{};
	double sse = 0.0; // sum of squared distances of the chosen start
};

KMeansResult kmeans_palette(const std::vector<Lab> &points);

} // namespace glyphnotic
