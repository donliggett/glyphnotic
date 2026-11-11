// Glyphnotic: the motion settings' constants and formulas (docs/SPEC.md,
// Motion). No libobs dependency; the shaders hold the per-cell logic.

#pragma once

#include "settings.hpp"

#include <cmath>
#include <vector>

namespace glyphnotic {

// Hold's K_mode: threshold = Hold / 100 * K_mode, in the mode's cost units,
// which differ by orders of magnitude between modes. Provisional values from
// Phase 3's flicker test: 10x the threshold at which fresh noise (sigma
// 2/255) on victor.png, with the mode's first look, changes fewer than 0.1% of
// cells per frame, so Hold 10 holds against sensor noise and Hold 100 is
// almost locked. Phase 4 tunes them.
constexpr double kHoldScale[kModeCount] = {
	0.003,    // Ink (ASCII)
	0.000025, // Mosaic (Blocks)
	0.00015,  // Poster (Lines; scores like Mosaic, but finer glyphs flicker more)
	0.0015,   // Console (Lines)
};

constexpr double kBoilMaxRate = 4.0;   // swaps per cell per second at Boil 100
constexpr double kBoilSpreadMax = 0.5; // Boil spread 100
constexpr double kLightJumpDelta = 0.12;
constexpr double kLightJumpShare = 0.5;
constexpr double kSoftenSigmas = 3.0; // the blur kernel's radius, in sigmas

inline double hold_threshold(Mode mode, int hold)
{
	return hold <= 0 ? 0.0 : hold / 100.0 * kHoldScale[static_cast<int>(mode)];
}

// The chance that a cell not already swapping starts a swap in one frame of
// frame_seconds: Boil sets a rate r (swaps per second), and the chance per
// frame is 1 - e^(-r dt), so the rate is the same at any frame rate.
inline double boil_chance(int boil, double frame_seconds)
{
	if (boil <= 0 || frame_seconds <= 0.0)
		return 0.0;
	const double rate = boil / 100.0 * kBoilMaxRate;
	return 1.0 - std::exp(-rate * frame_seconds);
}

// Boil spread 0-100 as a share of the cell's cost range (worst - best).
inline double boil_spread_share(int spread)
{
	return spread <= 0 ? 0.0 : spread / 100.0 * kBoilSpreadMax;
}

// Soften's kernel radius in pixels: ceil(3 sigma), 0 when off.
inline int soften_radius(double sigma)
{
	return sigma <= 0.0 ? 0 : static_cast<int>(std::ceil(kSoftenSigmas * sigma - 1e-9));
}

// Soften's normalized kernel, taps -radius..radius (what soften.effect computes).
inline std::vector<double> soften_weights(double sigma)
{
	const int r = soften_radius(sigma);
	std::vector<double> w(static_cast<size_t>(2 * r + 1), 1.0);
	if (r == 0)
		return w;
	double sum = 0.0;
	for (int i = -r; i <= r; i++) {
		w[static_cast<size_t>(i + r)] = std::exp(-0.5 * i * i / (sigma * sigma));
		sum += w[static_cast<size_t>(i + r)];
	}
	for (double &x : w)
		x /= sum;
	return w;
}

} // namespace glyphnotic
