// Unit tests for src/motion.hpp (Hold's threshold, Boil's chance per frame
// and spread, Soften's kernel). Exits 0 when every check passes.

#include "motion.hpp"

#include <cmath>
#include <cstdio>
#include <string>

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

void test_hold()
{
	for (int m = 0; m < kModeCount; m++) {
		const Mode mode = static_cast<Mode>(m);
		check(hold_threshold(mode, 0) == 0.0, "Hold 0 is no hold");
		check_near(hold_threshold(mode, 100), kHoldScale[m], 1e-15, "Hold 100 is K_mode");
		check_near(hold_threshold(mode, 25), 0.25 * kHoldScale[m], 1e-15, "Hold 25 is K_mode / 4");
		check(kHoldScale[m] > 0.0, "K_mode is positive");
	}
}

void test_boil()
{
	check(boil_chance(0, 1.0 / 30) == 0.0, "Boil 0 never swaps");
	// The expected number of swaps per second is the same at 30 and 60 fps:
	// a free cell's chance per frame p gives a rate of -ln(1 - p) / dt.
	for (int boil : {1, 15, 45, 100}) {
		const double want = boil / 100.0 * kBoilMaxRate;
		for (double fps : {24.0, 30.0, 60.0, 144.0}) {
			const double p = boil_chance(boil, 1.0 / fps);
			check(p > 0.0 && p < 1.0, "chance in (0, 1)");
			check_near(-std::log(1.0 - p) * fps, want, 1e-9,
				   "Boil " + std::to_string(boil) + " rate at " + std::to_string(fps) + " fps");
		}
	}
	check_near(boil_chance(100, 1.0 / 30), 1.0 - std::exp(-4.0 / 30), 1e-15, "Boil 100 at 30 fps");
	check(boil_spread_share(0) == 0.0, "spread 0");
	check_near(boil_spread_share(100), 0.5, 1e-15, "spread 100 is half the cost range");
	check_near(boil_spread_share(20), 0.1, 1e-15, "spread 20");
}

void test_soften()
{
	check(soften_radius(0.0) == 0, "Soften 0 is off");
	check(soften_radius(0.6) == 2, "radius at 0.6 px");
	check(soften_radius(1.0) == 3, "radius at 1 px");
	check(soften_radius(2.0) == 6, "radius at 2 px");
	check(soften_radius(0.1) == 1, "radius at 0.1 px");
	for (double sigma : {0.1, 0.6, 1.0, 1.5, 2.0}) {
		const std::vector<double> w = soften_weights(sigma);
		const int r = soften_radius(sigma);
		check(w.size() == static_cast<size_t>(2 * r + 1), "kernel size");
		double sum = 0.0;
		for (double x : w)
			sum += x;
		check_near(sum, 1.0, 1e-12, "kernel sums to 1");
		for (int i = 0; i < r; i++) {
			check_near(w[static_cast<size_t>(i)], w[static_cast<size_t>(2 * r - i)], 1e-15,
				   "kernel symmetric");
			check(w[static_cast<size_t>(i)] < w[static_cast<size_t>(i + 1)], "kernel peaks at the center");
		}
	}
	// A flat region stays flat: the weights sum to 1.
	const std::vector<double> w = soften_weights(0.6);
	check_near(w[2], 1.0 / (1.0 + 2.0 * std::exp(-0.5 / 0.36) + 2.0 * std::exp(-2.0 / 0.36)), 1e-12,
		   "center weight at 0.6 px");
}

} // namespace

int main()
{
	test_hold();
	test_boil();
	test_soften();
	std::printf("motion-test: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
