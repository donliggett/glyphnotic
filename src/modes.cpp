// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Don Liggett

// Glyphnotic: per-mode settings, properties, defaults and looks.

#include "modes.hpp"

#include "presets.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace glyphnotic {

namespace {

const ModeInfo kModes[kModeCount] = {
	{"glyphnotic_ink", "Glyphnotic.Ink", "ink"},
	{"glyphnotic_mosaic", "Glyphnotic.Mosaic", "mosaic"},
	{"glyphnotic_poster", "Glyphnotic.Poster", "poster"},
	{"glyphnotic_console", "Glyphnotic.Console", "console"},
};

struct Choice {
	const char *value;
	const char *label_key;
};

struct CharsetChoice {
	const char *value;
	const char *label_key;
	CharsetId id;
};

const CharsetChoice kCharsets[] = {
	{"blocks", "Glyphnotic.Charset.Blocks", CharsetId::Blocks},
	{"lines", "Glyphnotic.Charset.Lines", CharsetId::Lines},
	{"ascii", "Glyphnotic.Charset.Ascii", CharsetId::Ascii},
	{"every", "Glyphnotic.Charset.Every", CharsetId::EveryGlyph},
	{"custom", "Glyphnotic.Charset.Custom", CharsetId::Custom},
};

const Choice kInkColors[] = {
	{"cell", "Glyphnotic.InkColor.Cell"},     {"green", "Glyphnotic.InkColor.Green"},
	{"amber", "Glyphnotic.InkColor.Amber"},   {"white", "Glyphnotic.InkColor.White"},
	{"custom", "Glyphnotic.InkColor.Custom"},
};

// Fixed inks' OKLab (a, b) (docs/SPEC.md, Ink colors).
struct FixedInk {
	InkColor id;
	double a, b;
};
const FixedInk kFixedInks[] = {
	{InkColor::Green, -0.12, 0.10},
	{InkColor::Amber, 0.034, 0.136}, // the hue of #FFB000
	{InkColor::White, 0.0, 0.0},
};

constexpr Rgb8 kDefaultInkCustom{0x40, 0xC0, 0xFF};

std::string palette_key(int k)
{
	return std::string(key::kPaletteColorPrefix) + std::to_string(k);
}

bool is(const char *a, const char *b)
{
	return a && b && strcmp(a, b) == 0;
}

// ---------------------------------------------------------------------------
// Looks against obs_data

bool value_matches(obs_data_t *s, const LookValue &v)
{
	switch (v.kind) {
	case LookValue::Kind::String:
		return v.s == obs_data_get_string(s, v.key);
	case LookValue::Kind::Int:
		return obs_data_get_int(s, v.key) == v.i;
	case LookValue::Kind::Double:
		return std::fabs(obs_data_get_double(s, v.key) - v.d) < 1e-6;
	}
	return false;
}

void set_value(obs_data_t *s, const LookValue &v)
{
	switch (v.kind) {
	case LookValue::Kind::String:
		obs_data_set_string(s, v.key, v.s.c_str());
		break;
	case LookValue::Kind::Int:
		obs_data_set_int(s, v.key, v.i);
		break;
	case LookValue::Kind::Double:
		obs_data_set_double(s, v.key, v.d);
		break;
	}
}

void set_default(obs_data_t *s, const LookValue &v)
{
	switch (v.kind) {
	case LookValue::Kind::String:
		obs_data_set_default_string(s, v.key, v.s.c_str());
		break;
	case LookValue::Kind::Int:
		obs_data_set_default_int(s, v.key, v.i);
		break;
	case LookValue::Kind::Double:
		obs_data_set_default_double(s, v.key, v.d);
		break;
	}
}

bool look_matches(obs_data_t *s, const Look &look)
{
	for (const LookValue &v : look.values)
		if (!value_matches(s, v))
			return false;
	return true;
}

// Writes the look's values; returns true if any changed.
bool apply_look(obs_data_t *s, const Look &look)
{
	bool changed = false;
	for (const LookValue &v : look.values) {
		if (!value_matches(s, v)) {
			set_value(s, v);
			changed = true;
		}
	}
	return changed;
}

// ---------------------------------------------------------------------------
// Properties: visibility and the Look

bool set_visible(obs_properties_t *props, const char *name, bool visible)
{
	obs_property_t *p = obs_properties_get(props, name);
	if (!p || obs_property_visible(p) == visible)
		return false;
	obs_property_set_visible(p, visible);
	return true;
}

// Shows each property only when it applies. Returns true if any changed.
bool update_visibility(const TypeData *td, obs_properties_t *props, obs_data_t *s)
{
	bool changed = false;
	const bool custom = charset_from_value(obs_data_get_string(s, key::kCharset)) == CharsetId::Custom;
	const bool fallback =
		custom &&
		!build_charset(*td->font, CharsetId::Custom, obs_data_get_string(s, key::kCustomChars)).usable();
	changed |= set_visible(props, key::kCustomChars, custom);
	changed |= set_visible(props, kPropCustomFallback, fallback);

	if (td->mode == Mode::Ink) {
		const bool auto_levels = !is(obs_data_get_string(s, key::kInkLevels), "manual");
		changed |=
			set_visible(props, key::kInkCustomColor, is(obs_data_get_string(s, key::kInkColor), "custom"));
		changed |= set_visible(props, key::kAdaptSpeed, auto_levels);
		changed |= set_visible(props, key::kBlackPoint, !auto_levels);
		changed |= set_visible(props, key::kWhitePoint, !auto_levels);
	}
	if (td->mode == Mode::Console) {
		const bool custom_palette = is(obs_data_get_string(s, key::kPalette), "custom");
		for (int k = 0; k < kPaletteSize; k++)
			changed |= set_visible(props, palette_key(k).c_str(), custom_palette);
	}
	return changed;
}

// The one modified callback of every property: applies a chosen Look, turns
// the Look to Custom when a setting it controls changes, and updates which
// properties are shown. Returns true only when the window must refresh at
// once (a chosen look, a property shown or hidden), so dragging a slider
// isn't interrupted.
bool settings_modified(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *s)
{
	const auto *td = static_cast<const TypeData *>(priv);
	const char *name = obs_property_name(property);
	const char *look_id = obs_data_get_string(s, key::kLook);
	const Look *look = find_look(td->mode, look_id);
	bool refresh = false;

	if (is(name, key::kLook)) {
		if (look)
			apply_look(s, *look);
		refresh = true;
	} else if (look && look_controls(td->mode, name) && !look_matches(s, *look)) {
		// No refresh here: rebuilding the window mid-drag drops the slider
		// drag. The filter refreshes the window to show Custom once the
		// changes stop (see filter_update).
		obs_data_set_string(s, key::kLook, kLookCustom);
	}
	refresh |= update_visibility(td, props, s);
	return refresh;
}

obs_property_t *add_list(obs_properties_t *props, const char *name, const char *label_key)
{
	return obs_properties_add_list(props, name, obs_module_text(label_key), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
}

obs_property_t *add_slider(obs_properties_t *props, const char *name, const char *label_key, double min, double max,
			   double step)
{
	return obs_properties_add_float_slider(props, name, obs_module_text(label_key), min, max, step);
}

} // namespace

const ModeInfo &mode_info(Mode mode)
{
	return kModes[static_cast<int>(mode)];
}

CharsetId charset_from_value(const char *value)
{
	for (const auto &c : kCharsets)
		if (is(value, c.value))
			return c.id;
	return CharsetId::Blocks;
}

const char *charset_value(CharsetId id)
{
	for (const auto &c : kCharsets)
		if (c.id == id)
			return c.value;
	return "blocks";
}

void mode_defaults(Mode mode, obs_data_t *s)
{
	const Look &first = looks_for(mode).front();
	obs_data_set_default_string(s, key::kLook, first.id);
	for (const LookValue &v : first.values)
		set_default(s, v);

	obs_data_set_default_string(s, key::kCustomChars, "");
	obs_data_set_default_int(s, key::kCellScale, 1);
	obs_data_set_default_string(s, key::kAlignment, "center");
	obs_data_set_default_string(s, key::kEdges, "black");
	obs_data_set_default_bool(s, key::kKeepTransparency, true);
	if (mode == Mode::Ink)
		obs_data_set_default_int(s, key::kInkCustomColor, rgb8_to_obs(kDefaultInkCustom));
	if (mode == Mode::Console)
		for (int k = 0; k < kPaletteSize; k++)
			obs_data_set_default_int(s, palette_key(k).c_str(),
						 rgb8_to_obs(vga_palette()[static_cast<size_t>(k)]));
}

Settings read_settings(Mode mode, obs_data_t *s)
{
	Settings r;
	r.charset = charset_from_value(obs_data_get_string(s, key::kCharset));
	r.cell_scale = static_cast<int>(obs_data_get_int(s, key::kCellScale));
	if (r.cell_scale < kMinCellScale || r.cell_scale > kMaxCellScale)
		r.cell_scale = 1;
	r.alignment = is(obs_data_get_string(s, key::kAlignment), "top_left") ? Alignment::TopLeft : Alignment::Center;
	r.edges_transparent = is(obs_data_get_string(s, key::kEdges), "transparent");
	r.keep_transparency = obs_data_get_bool(s, key::kKeepTransparency);

	auto clamp_int = [&](const char *k, int lo, int hi) {
		const long long v = obs_data_get_int(s, k);
		return static_cast<int>(v < lo ? lo : v > hi ? hi : v);
	};
	r.soften = obs_data_get_double(s, key::kSoften);
	r.soften = r.soften < 0.0 ? 0.0 : r.soften > kMaxSoften ? kMaxSoften : r.soften;
	r.hold = clamp_int(key::kHold, 0, 100);
	r.settle = clamp_int(key::kSettle, 0, 95);
	r.boil = clamp_int(key::kBoil, 0, 100);
	r.boil_spread = clamp_int(key::kBoilSpread, 0, 100);
	r.boil_linger = clamp_int(key::kBoilLinger, 1, 12);

	if (mode == Mode::Ink) {
		const char *ink = obs_data_get_string(s, key::kInkColor);
		r.ink_color = InkColor::Cell;
		for (size_t i = 0; i < sizeof(kInkColors) / sizeof(kInkColors[0]); i++)
			if (is(ink, kInkColors[i].value))
				r.ink_color = static_cast<InkColor>(i);
		r.ink_custom = rgb8_from_obs(obs_data_get_int(s, key::kInkCustomColor));
		for (const FixedInk &f : kFixedInks)
			if (f.id == r.ink_color) {
				r.ink_a = f.a;
				r.ink_b = f.b;
			}
		if (r.ink_color == InkColor::Custom) {
			const Lab c = srgb8_to_oklab(r.ink_custom);
			r.ink_a = c.a;
			r.ink_b = c.b;
		}
		r.background_tint = is(obs_data_get_string(s, key::kInkBackground), "tint");
		r.ink_lightness = obs_data_get_double(s, key::kInkLightness);
		r.saturation = obs_data_get_double(s, key::kSaturation);
		r.gamma = obs_data_get_double(s, key::kGamma);
		r.shape = obs_data_get_double(s, key::kShape);
		r.levels_auto = !is(obs_data_get_string(s, key::kInkLevels), "manual");
		r.adapt_speed = obs_data_get_double(s, key::kAdaptSpeed);
		if (r.adapt_speed < 0.01)
			r.adapt_speed = 0.01;
		r.black_point = obs_data_get_double(s, key::kBlackPoint);
		r.white_point = obs_data_get_double(s, key::kWhitePoint);
	}
	if (mode == Mode::Poster) {
		r.poster_levels = static_cast<int>(obs_data_get_int(s, key::kPosterLevels));
		if (r.poster_levels < 2)
			r.poster_levels = 2;
		if (r.poster_levels > 16)
			r.poster_levels = 16;
	}
	if (mode == Mode::Console) {
		r.palette_custom = is(obs_data_get_string(s, key::kPalette), "custom");
		r.palette = vga_palette();
		if (r.palette_custom)
			for (int k = 0; k < kPaletteSize; k++)
				r.palette[static_cast<size_t>(k)] =
					rgb8_from_obs(obs_data_get_int(s, palette_key(k).c_str()));
	}
	return r;
}

bool reconcile_look(Mode mode, obs_data_t *s, const std::string &last_look)
{
	const std::string current = obs_data_get_string(s, key::kLook);
	const Look *look = find_look(mode, current.c_str());
	if (!look)
		return false; // Custom (or an unknown id): the settings stand as they are
	if (current != last_look)
		return apply_look(s, *look);
	if (!look_matches(s, *look)) {
		obs_data_set_string(s, key::kLook, kLookCustom);
		return true;
	}
	return false;
}

void add_properties(obs_properties_t *props, TypeData *td, obs_property_clicked_t capture_clicked, void *data)
{
	auto watch = [td](obs_property_t *p) {
		obs_property_set_modified_callback2(p, settings_modified, td);
		return p;
	};

	// Look
	obs_property_t *look = watch(add_list(props, key::kLook, "Glyphnotic.Look"));
	for (const Look &l : looks_for(td->mode))
		obs_property_list_add_string(look, obs_module_text(l.label_key), l.id);
	obs_property_list_add_string(look, obs_module_text("Glyphnotic.Look.Custom"), kLookCustom);

	// Characters
	obs_properties_t *chars = obs_properties_create();
	obs_property_t *list = watch(add_list(chars, key::kCharset, "Glyphnotic.Charset"));
	for (const auto &c : kCharsets)
		obs_property_list_add_string(list, obs_module_text(c.label_key), c.value);
	watch(obs_properties_add_text(chars, key::kCustomChars, obs_module_text("Glyphnotic.CustomChars"),
				      OBS_TEXT_DEFAULT));
	obs_property_t *note = obs_properties_add_text(chars, kPropCustomFallback,
						       obs_module_text("Glyphnotic.CustomFallback"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(note, OBS_TEXT_INFO_WARNING);
	obs_property_set_visible(note, false);
	obs_properties_add_group(props, "group_characters", obs_module_text("Glyphnotic.Group.Characters"),
				 OBS_GROUP_NORMAL, chars);

	// Grid
	obs_properties_t *grid = obs_properties_create();
	obs_property_t *scale = obs_properties_add_list(grid, key::kCellScale, obs_module_text("Glyphnotic.CellScale"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int sc = kMinCellScale; sc <= kMaxCellScale; sc++) {
		char label[32];
		snprintf(label, sizeof(label), "%d\xC3\x97 (%dx%d)", sc, kGlyphWidth * sc, kGlyphHeight * sc);
		obs_property_list_add_int(scale, label, sc);
	}
	obs_property_t *align = add_list(grid, key::kAlignment, "Glyphnotic.Alignment");
	obs_property_list_add_string(align, obs_module_text("Glyphnotic.Alignment.Center"), "center");
	obs_property_list_add_string(align, obs_module_text("Glyphnotic.Alignment.TopLeft"), "top_left");
	obs_property_t *edges = add_list(grid, key::kEdges, "Glyphnotic.Edges");
	obs_property_list_add_string(edges, obs_module_text("Glyphnotic.Edges.Black"), "black");
	obs_property_list_add_string(edges, obs_module_text("Glyphnotic.Edges.Transparent"), "transparent");
	obs_properties_add_bool(grid, key::kKeepTransparency, obs_module_text("Glyphnotic.KeepTransparency"));
	obs_properties_add_group(props, "group_grid", obs_module_text("Glyphnotic.Group.Grid"), OBS_GROUP_NORMAL, grid);

	// Motion
	obs_properties_t *motion = obs_properties_create();
	obs_property_t *soften = watch(add_slider(motion, key::kSoften, "Glyphnotic.Soften", 0.0, kMaxSoften, 0.1));
	obs_property_float_set_suffix(soften, " px");
	watch(obs_properties_add_int_slider(motion, key::kHold, obs_module_text("Glyphnotic.Hold"), 0, 100, 1));
	obs_property_t *settle = watch(
		obs_properties_add_int_slider(motion, key::kSettle, obs_module_text("Glyphnotic.Settle"), 0, 95, 1));
	obs_property_int_set_suffix(settle, "%");
	watch(obs_properties_add_int_slider(motion, key::kBoil, obs_module_text("Glyphnotic.Boil"), 0, 100, 1));
	watch(obs_properties_add_int_slider(motion, key::kBoilSpread, obs_module_text("Glyphnotic.BoilSpread"), 0, 100,
					    1));
	obs_property_t *linger = watch(obs_properties_add_int_slider(
		motion, key::kBoilLinger, obs_module_text("Glyphnotic.BoilLinger"), 1, 12, 1));
	obs_property_int_set_suffix(linger, obs_module_text("Glyphnotic.FramesSuffix"));
	obs_properties_add_group(props, "group_motion", obs_module_text("Glyphnotic.Group.Motion"), OBS_GROUP_NORMAL,
				 motion);

	if (td->mode == Mode::Ink) {
		obs_properties_t *ink = obs_properties_create();
		obs_property_t *color = watch(add_list(ink, key::kInkColor, "Glyphnotic.InkColor"));
		for (const Choice &c : kInkColors)
			obs_property_list_add_string(color, obs_module_text(c.label_key), c.value);
		obs_properties_add_color(ink, key::kInkCustomColor, obs_module_text("Glyphnotic.InkCustomColor"));
		obs_property_t *bg = watch(add_list(ink, key::kInkBackground, "Glyphnotic.InkBackground"));
		obs_property_list_add_string(bg, obs_module_text("Glyphnotic.InkBackground.Black"), "black");
		obs_property_list_add_string(bg, obs_module_text("Glyphnotic.InkBackground.Tint"), "tint");
		watch(add_slider(ink, key::kInkLightness, "Glyphnotic.InkLightness", 0.5, 1.0, 0.01));
		watch(add_slider(ink, key::kSaturation, "Glyphnotic.Saturation", 0.5, 2.0, 0.05));
		watch(add_slider(ink, key::kGamma, "Glyphnotic.Gamma", kGammaMin, kGammaMax, 0.01));
		watch(add_slider(ink, key::kShape, "Glyphnotic.Shape", 0.0, kShapeMax, 0.01));
		obs_property_t *levels = watch(add_list(ink, key::kInkLevels, "Glyphnotic.InkLevels"));
		obs_property_list_add_string(levels, obs_module_text("Glyphnotic.InkLevels.Auto"), "auto");
		obs_property_list_add_string(levels, obs_module_text("Glyphnotic.InkLevels.Manual"), "manual");
		obs_property_t *adapt =
			watch(add_slider(ink, key::kAdaptSpeed, "Glyphnotic.AdaptSpeed", 0.2, 5.0, 0.1));
		obs_property_float_set_suffix(adapt, " s");
		watch(add_slider(ink, key::kBlackPoint, "Glyphnotic.BlackPoint", 0.0, kBlackPointMax, 0.01));
		watch(add_slider(ink, key::kWhitePoint, "Glyphnotic.WhitePoint", kWhitePointMin, 1.0, 0.01));
		obs_properties_add_group(props, "group_ink", obs_module_text("Glyphnotic.Group.Ink"), OBS_GROUP_NORMAL,
					 ink);
	}

	if (td->mode == Mode::Poster) {
		obs_properties_t *poster = obs_properties_create();
		watch(obs_properties_add_int_slider(poster, key::kPosterLevels,
						    obs_module_text("Glyphnotic.PosterLevels"), 2, kPosterLevelsMax,
						    1));
		obs_properties_add_group(props, "group_poster", obs_module_text("Glyphnotic.Group.Poster"),
					 OBS_GROUP_NORMAL, poster);
	}

	if (td->mode == Mode::Console) {
		obs_properties_t *pal = obs_properties_create();
		obs_property_t *p = watch(add_list(pal, key::kPalette, "Glyphnotic.Palette"));
		obs_property_list_add_string(p, obs_module_text("Glyphnotic.Palette.Vga"), "vga");
		obs_property_list_add_string(p, obs_module_text("Glyphnotic.Palette.Custom"), "custom");
		obs_properties_add_button2(pal, kPropCapturePalette, obs_module_text("Glyphnotic.CapturePalette"),
					   capture_clicked, data);
		obs_properties_add_text(pal, kPropPaletteNote, obs_module_text("Glyphnotic.PaletteNote"),
					OBS_TEXT_INFO);
		for (int k = 0; k < kPaletteSize; k++) {
			const std::string label =
				std::string(obs_module_text("Glyphnotic.PaletteColor")) + " " + std::to_string(k + 1);
			obs_properties_add_color(pal, palette_key(k).c_str(), label.c_str());
		}
		obs_properties_add_group(props, "group_palette", obs_module_text("Glyphnotic.Group.Palette"),
					 OBS_GROUP_NORMAL, pal);
	}
}

} // namespace glyphnotic
