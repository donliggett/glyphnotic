// Glyphnotic: color conversions, the built-in palette and palette capture.

#include "palette.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace glyphnotic {

const Palette &vga_palette()
{
	static const Palette kVga = {{
		{0x00, 0x00, 0x00}, // black
		{0x00, 0x00, 0xAA}, // blue
		{0x00, 0xAA, 0x00}, // green
		{0x00, 0xAA, 0xAA}, // cyan
		{0xAA, 0x00, 0x00}, // red
		{0xAA, 0x00, 0xAA}, // magenta
		{0xAA, 0x55, 0x00}, // brown
		{0xAA, 0xAA, 0xAA}, // light gray
		{0x55, 0x55, 0x55}, // dark gray
		{0x55, 0x55, 0xFF}, // light blue
		{0x55, 0xFF, 0x55}, // light green
		{0x55, 0xFF, 0xFF}, // light cyan
		{0xFF, 0x55, 0x55}, // light red
		{0xFF, 0x55, 0xFF}, // light magenta
		{0xFF, 0xFF, 0x55}, // yellow
		{0xFF, 0xFF, 0xFF}, // white
	}};
	return kVga;
}

double srgb_decode(double u)
{
	return u <= 0.04045 ? u / 12.92 : std::pow((u + 0.055) / 1.055, 2.4);
}

double srgb_encode(double u)
{
	u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
	return u <= 0.0031308 ? 12.92 * u : 1.055 * std::pow(u, 1.0 / 2.4) - 0.055;
}

Lab linear_to_oklab(double r, double g, double b)
{
	double l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
	double m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
	double s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;
	l = std::cbrt(std::max(l, 0.0));
	m = std::cbrt(std::max(m, 0.0));
	s = std::cbrt(std::max(s, 0.0));
	return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
		1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
		0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

void oklab_to_linear(const Lab &c, double out[3])
{
	double l = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
	double m = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
	double s = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
	l = l * l * l;
	m = m * m * m;
	s = s * s * s;
	out[0] = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
	out[1] = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
	out[2] = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

Lab srgb8_to_oklab(Rgb8 c)
{
	return linear_to_oklab(srgb_decode(c.r / 255.0), srgb_decode(c.g / 255.0), srgb_decode(c.b / 255.0));
}

void oklab_to_srgb255(const Lab &c, double out[3])
{
	double lin[3];
	oklab_to_linear(c, lin);
	for (int i = 0; i < 3; i++)
		out[i] = srgb_encode(lin[i]) * 255.0;
}

Rgb8 oklab_to_srgb8(const Lab &c)
{
	double v[3];
	oklab_to_srgb255(c, v);
	auto u8 = [](double x) {
		return static_cast<uint8_t>(std::lround(std::min(255.0, std::max(0.0, x))));
	};
	return {u8(v[0]), u8(v[1]), u8(v[2])};
}

Rgb8 rgb8_from_obs(long long color)
{
	return {static_cast<uint8_t>(color & 0xFF), static_cast<uint8_t>((color >> 8) & 0xFF),
		static_cast<uint8_t>((color >> 16) & 0xFF)};
}

long long rgb8_to_obs(Rgb8 c)
{
	return 0xFF000000LL | (static_cast<long long>(c.b) << 16) | (static_cast<long long>(c.g) << 8) | c.r;
}

// ---------------------------------------------------------------------------
// k-means

namespace {

constexpr int kStarts = 3;
constexpr int kIterations = 30;
constexpr uint64_t kSeed = 0x476C797068756E31ULL; // "Glyphun1"; fixed so captures repeat

// splitmix64: small, fast and the same on every platform and compiler.
struct Rng {
	uint64_t state;
	uint64_t next()
	{
		uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return z ^ (z >> 31);
	}
	double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); } // [0, 1)
	size_t index(size_t n) { return static_cast<size_t>(unit() * static_cast<double>(n)) % n; }
};

double dist2(const Lab &p, const Lab &q)
{
	const double dl = p.L - q.L, da = p.a - q.a, db = p.b - q.b;
	return dl * dl + da * da + db * db;
}

// Nearest center; the lowest index wins a tie.
size_t nearest(const Lab &p, const std::array<Lab, kPaletteSize> &centers, double *d2_out)
{
	size_t best = 0;
	double best_d = dist2(p, centers[0]);
	for (size_t k = 1; k < centers.size(); k++) {
		const double d = dist2(p, centers[k]);
		if (d < best_d) {
			best_d = d;
			best = k;
		}
	}
	if (d2_out)
		*d2_out = best_d;
	return best;
}

KMeansResult one_start(const std::vector<Lab> &pts, Rng &rng)
{
	const size_t n = pts.size();
	KMeansResult r;
	std::vector<double> d2(n);

	// k-means++ seeding.
	r.centers[0] = pts[rng.index(n)];
	for (size_t i = 0; i < n; i++)
		d2[i] = dist2(pts[i], r.centers[0]);
	for (int k = 1; k < kPaletteSize; k++) {
		const double total = std::accumulate(d2.begin(), d2.end(), 0.0);
		size_t pick = 0;
		if (total > 0.0) {
			const double target = rng.unit() * total;
			double cum = 0.0;
			pick = n; // fall back to the last point with any weight
			for (size_t i = 0; i < n; i++) {
				cum += d2[i];
				if (cum > target) {
					pick = i;
					break;
				}
			}
			if (pick == n)
				for (size_t i = n; i-- > 0;)
					if (d2[i] > 0.0) {
						pick = i;
						break;
					}
		} else {
			pick = rng.index(n);
		}
		r.centers[static_cast<size_t>(k)] = pts[pick];
		for (size_t i = 0; i < n; i++)
			d2[i] = std::min(d2[i], dist2(pts[i], r.centers[static_cast<size_t>(k)]));
	}

	// Lloyd iterations.
	std::vector<size_t> assign(n, 0);
	for (int it = 0; it < kIterations; it++) {
		for (size_t i = 0; i < n; i++)
			assign[i] = nearest(pts[i], r.centers, &d2[i]);
		std::array<Lab, kPaletteSize> sum{};
		std::array<size_t, kPaletteSize> count{};
		for (size_t i = 0; i < n; i++) {
			Lab &s = sum[assign[i]];
			s.L += pts[i].L;
			s.a += pts[i].a;
			s.b += pts[i].b;
			count[assign[i]]++;
		}
		for (size_t k = 0; k < kPaletteSize; k++) {
			if (count[k] == 0)
				continue;
			const double inv = 1.0 / static_cast<double>(count[k]);
			r.centers[k] = {sum[k].L * inv, sum[k].a * inv, sum[k].b * inv};
		}
		// An empty cluster takes the point farthest from its center.
		for (size_t k = 0; k < kPaletteSize; k++) {
			if (count[k] != 0)
				continue;
			size_t far = 0;
			double far_d = -1.0;
			for (size_t i = 0; i < n; i++)
				if (d2[i] > far_d) {
					far_d = d2[i];
					far = i;
				}
			if (far_d <= 0.0)
				continue; // every point sits on a center: nothing to move
			r.centers[k] = pts[far];
			d2[far] = 0.0;
			assign[far] = k;
		}
	}

	r.counts.fill(0);
	r.sse = 0.0;
	for (size_t i = 0; i < n; i++) {
		double d = 0.0;
		r.counts[nearest(pts[i], r.centers, &d)]++;
		r.sse += d;
	}
	return r;
}

} // namespace

KMeansResult kmeans_palette(const std::vector<Lab> &points)
{
	KMeansResult best;
	if (points.empty())
		return best;
	Rng rng{kSeed};
	for (int s = 0; s < kStarts; s++) {
		KMeansResult r = one_start(points, rng);
		if (s == 0 || r.sse < best.sse)
			best = r;
	}

	// Largest cluster first; equal sizes keep their order.
	std::array<size_t, kPaletteSize> order;
	std::iota(order.begin(), order.end(), size_t{0});
	std::stable_sort(order.begin(), order.end(),
			 [&](size_t x, size_t y) { return best.counts[x] > best.counts[y]; });
	KMeansResult sorted = best;
	for (size_t k = 0; k < kPaletteSize; k++) {
		sorted.centers[k] = best.centers[order[k]];
		sorted.counts[k] = best.counts[order[k]];
	}
	return sorted;
}

} // namespace glyphnotic
