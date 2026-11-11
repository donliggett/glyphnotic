// Glyphnotic: drawing modes, setting keys and the settings the render thread
// uses. No libobs dependency.

#pragma once

#include "font.hpp"
#include "grid.hpp"
#include "palette.hpp"

namespace glyphnotic {

enum class Mode { Ink = 0, Mosaic, Poster, Console };
constexpr int kModeCount = 4;

// Setting keys and their values are saved in scene files; never change them.
namespace key {
constexpr const char *kLook = "look";
constexpr const char *kCharset = "charset";
constexpr const char *kCustomChars = "custom_chars";
constexpr const char *kCellScale = "cell_scale";
constexpr const char *kAlignment = "alignment";
constexpr const char *kEdges = "edges";
constexpr const char *kKeepTransparency = "keep_transparency";
// Motion
constexpr const char *kSoften = "soften";
constexpr const char *kHold = "hold";
constexpr const char *kSettle = "settle";
constexpr const char *kBoil = "boil";
constexpr const char *kBoilSpread = "boil_spread";
constexpr const char *kBoilLinger = "boil_linger";
// Ink
constexpr const char *kInkColor = "ink_color";
constexpr const char *kInkCustomColor = "ink_custom_color";
constexpr const char *kInkBackground = "ink_background";
constexpr const char *kInkLightness = "ink_lightness";
constexpr const char *kSaturation = "saturation";
constexpr const char *kGamma = "gamma";
constexpr const char *kShape = "shape";
constexpr const char *kInkLevels = "ink_levels";
constexpr const char *kAdaptSpeed = "adapt_speed";
constexpr const char *kBlackPoint = "black_point";
constexpr const char *kWhitePoint = "white_point";
// Poster
constexpr const char *kPosterLevels = "poster_levels";
// Console
constexpr const char *kPalette = "palette";
constexpr const char *kPaletteColorPrefix = "palette_"; // palette_0 .. palette_15
} // namespace key

constexpr const char *kLookCustom = "custom";

constexpr double kMaxSoften = 2.0; // px

enum class InkColor { Cell = 0, Green, Amber, White, Custom };

// Settings the render thread reads, resolved from the source settings.
struct Settings {
	CharsetId charset = CharsetId::Blocks;
	int cell_scale = 1;
	Alignment alignment = Alignment::Center;
	bool edges_transparent = false;
	bool keep_transparency = true;

	// Motion (the defaults are the first looks')
	double soften = 0.6;  // sigma in source pixels; 0 = off
	int hold = 25;        // 0-100
	int settle = 0;       // percent, 0-95
	int boil = 0;         // 0-100
	int boil_spread = 20; // 0-100
	int boil_linger = 2;  // frames, 1-12

	// Ink
	InkColor ink_color = InkColor::Cell;
	double ink_a = 0.0, ink_b = 0.0; // the fixed or custom ink's (a, b); unused for Cell
	Rgb8 ink_custom{0xFF, 0xB0, 0x00};
	bool background_tint = false;
	double ink_lightness = 0.92;
	double saturation = 1.4;
	double gamma = 0.85;
	double shape = 0.35;
	bool levels_auto = true;
	double adapt_speed = 1.5; // seconds
	double black_point = 0.0;
	double white_point = 1.0;

	// Poster
	int poster_levels = 6;

	// Console
	bool palette_custom = false;
	Palette palette{}; // the palette in use (VGA or the custom colors)
};

} // namespace glyphnotic
