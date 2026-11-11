// Glyphnotic: the looks. Starting values from docs/SPEC.md, Looks; Phase 4
// tunes them.

#include "presets.hpp"

#include <cstring>

namespace glyphnotic {

namespace {

using Kind = LookValue::Kind;

LookValue str(const char *k, const char *v)
{
	return {k, Kind::String, v};
}

LookValue num(const char *k, long long v)
{
	return {k, Kind::Int, {}, v};
}

LookValue dbl(const char *k, double v)
{
	return {k, Kind::Double, {}, 0, v};
}

// The defaults of every setting a look controls, per mode.
std::vector<LookValue> base_values(Mode mode)
{
	std::vector<LookValue> v = {
		str(key::kCharset, "ascii"), dbl(key::kSoften, 0.6),    num(key::kHold, 25),      num(key::kSettle, 0),
		num(key::kBoil, 0),          num(key::kBoilSpread, 20), num(key::kBoilLinger, 2),
	};
	switch (mode) {
	case Mode::Ink:
		v.push_back(str(key::kInkColor, "cell"));
		v.push_back(str(key::kInkBackground, "black"));
		v.push_back(dbl(key::kInkLightness, 0.92));
		v.push_back(dbl(key::kSaturation, 1.4));
		v.push_back(dbl(key::kGamma, 0.85));
		v.push_back(dbl(key::kShape, 0.35));
		v.push_back(str(key::kInkLevels, "auto"));
		v.push_back(dbl(key::kAdaptSpeed, 1.5));
		v.push_back(dbl(key::kBlackPoint, 0.0));
		v.push_back(dbl(key::kWhitePoint, 1.0));
		break;
	case Mode::Mosaic:
		break;
	case Mode::Poster:
		v.push_back(num(key::kPosterLevels, 6));
		break;
	case Mode::Console:
		v.push_back(str(key::kPalette, "vga"));
		break;
	}
	return v;
}

Look make(Mode mode, const char *id, const char *label_key, const std::vector<LookValue> &overrides)
{
	Look look{id, label_key, base_values(mode)};
	for (const LookValue &o : overrides)
		for (LookValue &v : look.values)
			if (strcmp(v.key, o.key) == 0)
				v = o;
	return look;
}

std::vector<Look> build(Mode mode)
{
	switch (mode) {
	case Mode::Ink:
		return {
			make(mode, "black", "Glyphnotic.Look.Black", {}),
			make(mode, "tint", "Glyphnotic.Look.Tint", {str(key::kInkBackground, "tint")}),
			make(mode, "phosphor", "Glyphnotic.Look.Phosphor", {str(key::kInkColor, "green")}),
			make(mode, "amber", "Glyphnotic.Look.Amber", {str(key::kInkColor, "amber")}),
			make(mode, "every_glyph", "Glyphnotic.Look.EveryGlyph", {str(key::kCharset, "every")}),
			make(mode, "static", "Glyphnotic.Look.Static",
			     {str(key::kInkColor, "green"), num(key::kBoil, 15), num(key::kBoilSpread, 20),
			      num(key::kBoilLinger, 2)}),
			make(mode, "fever", "Glyphnotic.Look.Fever",
			     {str(key::kInkBackground, "tint"), num(key::kHold, 10), num(key::kBoil, 45),
			      num(key::kBoilSpread, 35), num(key::kBoilLinger, 1), num(key::kSettle, 30)}),
		};
	case Mode::Mosaic:
		return {make(mode, "blocks", "Glyphnotic.Look.Blocks",
			     {str(key::kCharset, "blocks"), num(key::kHold, 10)})};
	case Mode::Poster:
		return {make(mode, "poster", "Glyphnotic.Look.Poster",
			     {str(key::kCharset, "lines"), num(key::kPosterLevels, 6), num(key::kHold, 10)})};
	case Mode::Console:
		return {make(mode, "console", "Glyphnotic.Look.Console",
			     {str(key::kCharset, "lines"), str(key::kPalette, "vga"), num(key::kHold, 10)})};
	}
	return {};
}

} // namespace

const std::vector<Look> &looks_for(Mode mode)
{
	static const std::vector<Look> kLooks[kModeCount] = {build(Mode::Ink), build(Mode::Mosaic), build(Mode::Poster),
							     build(Mode::Console)};
	return kLooks[static_cast<int>(mode)];
}

const Look *find_look(Mode mode, const char *id)
{
	if (!id)
		return nullptr;
	for (const Look &look : looks_for(mode))
		if (strcmp(look.id, id) == 0)
			return &look;
	return nullptr;
}

bool look_controls(Mode mode, const char *key_name)
{
	for (const LookValue &v : looks_for(mode).front().values)
		if (strcmp(v.key, key_name) == 0)
			return true;
	return false;
}

} // namespace glyphnotic
