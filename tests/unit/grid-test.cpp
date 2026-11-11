// Unit tests for src/grid.hpp: grid geometry for each cell scale and alignment.
// Exits 0 when every check passes.

#include "grid.hpp"

#include <cstdio>
#include <string>

using namespace glyphnotic;

namespace {

int g_failures = 0;
int g_checks = 0;

void check_eq(int got, int want, const std::string &what)
{
	g_checks++;
	if (got != want) {
		g_failures++;
		std::printf("FAIL: %s: got %d, want %d\n", what.c_str(), got, want);
	}
}

void check_grid(int w, int h, int scale, Alignment align, int cx, int cy, int ox, int oy)
{
	const Grid g = make_grid(w, h, scale, align);
	const std::string name = std::to_string(w) + "x" + std::to_string(h) + " scale " + std::to_string(scale) +
				 (align == Alignment::Center ? " center" : " top-left");
	check_eq(g.cells_x, cx, name + " cells_x");
	check_eq(g.cells_y, cy, name + " cells_y");
	check_eq(g.offset_x, ox, name + " offset_x");
	check_eq(g.offset_y, oy, name + " offset_y");
	check_eq(g.cell_w, 8 * g.scale, name + " cell_w");
	check_eq(g.cell_h, 16 * g.scale, name + " cell_h");
	check_eq(g.sample_w, cx * 8, name + " sample_w");
	check_eq(g.sample_h, cy * 16, name + " sample_h");
}

} // namespace

int main()
{
	// The spec's example: 240x67 cells, 4 rows of pixels left over at the top and bottom.
	check_grid(1920, 1080, 1, Alignment::Center, 240, 67, 0, 4);
	check_grid(1920, 1080, 1, Alignment::TopLeft, 240, 67, 0, 0);
	check_grid(1920, 1080, 2, Alignment::Center, 120, 33, 0, 12);
	check_grid(1920, 1080, 3, Alignment::Center, 80, 22, 0, 12);
	check_grid(1920, 1080, 4, Alignment::Center, 60, 16, 0, 28);
	check_grid(1280, 720, 1, Alignment::Center, 160, 45, 0, 0);
	// Odd leftovers: the extra pixel goes to the bottom/right.
	check_grid(894, 853, 1, Alignment::Center, 111, 53, 3, 2);
	check_grid(905, 975, 4, Alignment::Center, 28, 15, 4, 7);
	// Smaller than one cell: an empty grid.
	check_grid(7, 100, 1, Alignment::Center, 0, 6, 3, 2);
	check_eq(make_grid(7, 100, 1, Alignment::Center).empty() ? 1 : 0, 1, "7x100 grid is empty");
	check_eq(make_grid(0, 0, 1, Alignment::Center).empty() ? 1 : 0, 1, "0x0 grid is empty");
	// Out-of-range scales are clamped.
	check_eq(make_grid(1920, 1080, 0, Alignment::Center).scale, 1, "scale 0 clamps to 1");
	check_eq(make_grid(1920, 1080, 9, Alignment::Center).scale, 4, "scale 9 clamps to 4");

	std::printf("%d checks, %d failed\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
