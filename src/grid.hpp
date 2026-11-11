// Glyphnotic: grid geometry (no libobs dependency, unit-tested).

#pragma once

#include "font.hpp"

namespace glyphnotic {

enum class Alignment { Center = 0, TopLeft };

struct Grid {
	int scale = 1;  // cell scale s, 1..4
	int cell_w = 0; // 8*s source pixels
	int cell_h = 0; // 16*s source pixels
	int cells_x = 0;
	int cells_y = 0;
	int offset_x = 0; // source pixel of the grid's top-left corner
	int offset_y = 0;
	int sample_w = 0; // Sample pass size: cells_x*8 by cells_y*16
	int sample_h = 0;

	bool empty() const { return cells_x <= 0 || cells_y <= 0; }
	int width_px() const { return cells_x * cell_w; }
	int height_px() const { return cells_y * cell_h; }
};

constexpr int kMinCellScale = 1;
constexpr int kMaxCellScale = 4;

// Lays the grid over a source. Leftover pixels are split evenly (the extra
// pixel, if odd, goes to the bottom/right) or all at the bottom/right.
inline Grid make_grid(int source_w, int source_h, int scale, Alignment align)
{
	Grid g;
	if (scale < kMinCellScale)
		scale = kMinCellScale;
	if (scale > kMaxCellScale)
		scale = kMaxCellScale;
	g.scale = scale;
	g.cell_w = kGlyphWidth * scale;
	g.cell_h = kGlyphHeight * scale;
	if (source_w <= 0 || source_h <= 0)
		return g;
	g.cells_x = source_w / g.cell_w;
	g.cells_y = source_h / g.cell_h;
	if (align == Alignment::Center) {
		g.offset_x = (source_w - g.width_px()) / 2;
		g.offset_y = (source_h - g.height_px()) / 2;
	}
	g.sample_w = g.cells_x * kGlyphWidth;
	g.sample_h = g.cells_y * kGlyphHeight;
	return g;
}

} // namespace glyphnotic
