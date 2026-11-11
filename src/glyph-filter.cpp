// Glyphnotic: the shared filter core.
//
// One implementation, registered once per drawing mode (type_data carries
// the mode). Each frame runs a short chain of libobs render passes (see
// docs/SPEC.md, Render passes):
//
//   Sample      filter input -> OKLab + alpha, cellsX*8 x cellsY*16
//   Cell stats  mean OKLab and alpha per cell
//   Score       best glyph per cell
//   Color       text and background color per cell (double-width target)
//   Draw        the character grid at source size, into OBS's target
//
// Phase 1 implements Mosaic. Ink is registered but still passes the picture
// through; Poster and Console are registered in Phase 2.

#include "glyph-filter.hpp"

#include "font.hpp"
#include "grid.hpp"

#include <graphics/vec2.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace glyphnotic {

namespace {

struct ModeInfo {
	const char *id;       // OBS source id; saved in scene files, never change it
	const char *name_key; // locale key for the display name
	const char *name;     // short name for logs and dumps
};

const ModeInfo kModes[] = {
	{"glyphnotic_ink", "Glyphnotic.Ink", "ink"},
	{"glyphnotic_mosaic", "Glyphnotic.Mosaic", "mosaic"},
	{"glyphnotic_poster", "Glyphnotic.Poster", "poster"},
	{"glyphnotic_console", "Glyphnotic.Console", "console"},
};

// Setting keys and values are saved in scene files; never change them.
constexpr const char *kSetCharset = "charset";
constexpr const char *kSetCustomChars = "custom_chars";
constexpr const char *kSetCellScale = "cell_scale";
constexpr const char *kSetAlignment = "alignment";
constexpr const char *kSetEdges = "edges";
constexpr const char *kSetKeepTransparency = "keep_transparency";
constexpr const char *kPropCustomFallback = "custom_fallback_note"; // a note, not a setting
#ifdef GLYPHNOTIC_DEBUG_TOOLS
constexpr const char *kPropDumpGrid = "dump_grid";
#endif

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

CharsetId charset_from_value(const char *value)
{
	for (const auto &c : kCharsets)
		if (value && strcmp(value, c.value) == 0)
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

// Settings the render thread reads, copied under the mutex.
struct Settings {
	CharsetId charset = CharsetId::Blocks;
	int cell_scale = 1;
	Alignment alignment = Alignment::Center;
	bool edges_transparent = false;
	bool keep_transparency = true;
};

struct Effects {
	gs_effect_t *sample = nullptr;
	gs_effect_t *stats = nullptr;
	gs_effect_t *score = nullptr;
	gs_effect_t *color = nullptr;
	gs_effect_t *draw = nullptr;

	bool ok() const { return sample && stats && score && color && draw; }
};

struct GlyphFilter {
	obs_source_t *context = nullptr;
	Mode mode = Mode::Ink;
	const Font *font = nullptr;

	// Graphics resources, touched only with the graphics context entered.
	Effects fx;
	gs_texrender_t *tr_sample = nullptr;
	gs_texrender_t *tr_stats = nullptr;
	gs_texrender_t *tr_score = nullptr;
	gs_texrender_t *tr_color = nullptr;
	gs_texture_t *atlas = nullptr;
	Charset atlas_charset; // the set uploaded to `atlas`

	// Written by update() on the UI thread, read by the render thread.
	std::mutex mutex;
	Settings settings;
	Charset pending_charset;
	bool charset_dirty = false;

	std::atomic<bool> dump_requested{false};
};

// type_data carries the mode, so one implementation serves all four registrations.
struct TypeData {
	Mode mode;
	const Font *font;
};

gs_effect_t *load_effect(const char *name)
{
	std::string rel = std::string("effects/") + name;
	char *path = obs_module_file(rel.c_str());
	if (!path) {
		obs_log(LOG_ERROR, "effect file %s not found", rel.c_str());
		return nullptr;
	}
	char *errors = nullptr;
	gs_effect_t *effect = gs_effect_create_from_file(path, &errors);
	if (!effect)
		obs_log(LOG_ERROR, "failed to load %s: %s", rel.c_str(), errors ? errors : "(no details)");
	bfree(errors);
	bfree(path);
	return effect;
}

void destroy_graphics(GlyphFilter *f)
{
	gs_effect_destroy(f->fx.sample);
	gs_effect_destroy(f->fx.stats);
	gs_effect_destroy(f->fx.score);
	gs_effect_destroy(f->fx.color);
	gs_effect_destroy(f->fx.draw);
	f->fx = Effects{};
	gs_texrender_destroy(f->tr_sample);
	gs_texrender_destroy(f->tr_stats);
	gs_texrender_destroy(f->tr_score);
	gs_texrender_destroy(f->tr_color);
	f->tr_sample = f->tr_stats = f->tr_score = f->tr_color = nullptr;
	gs_texture_destroy(f->atlas);
	f->atlas = nullptr;
}

// ---------------------------------------------------------------------------
// Settings

Settings read_settings(obs_data_t *data)
{
	Settings s;
	s.charset = charset_from_value(obs_data_get_string(data, kSetCharset));
	s.cell_scale = static_cast<int>(obs_data_get_int(data, kSetCellScale));
	if (s.cell_scale < kMinCellScale || s.cell_scale > kMaxCellScale)
		s.cell_scale = 1;
	s.alignment = strcmp(obs_data_get_string(data, kSetAlignment), "top_left") == 0 ? Alignment::TopLeft
											: Alignment::Center;
	s.edges_transparent = strcmp(obs_data_get_string(data, kSetEdges), "transparent") == 0;
	s.keep_transparency = obs_data_get_bool(data, kSetKeepTransparency);
	return s;
}

Charset make_charset(const Font &font, CharsetId id, const char *custom)
{
	Charset set = build_charset(font, id, custom ? custom : "");
	if (!set.usable()) {
		// Only possible for Custom: fewer than 2 distinct glyphs. Fall back to Blocks.
		obs_log(LOG_WARNING, "custom character set has fewer than %zu glyphs; using Blocks", kMinCharsetSize);
		set = build_charset(font, CharsetId::Blocks);
	}
	return set;
}

void filter_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<GlyphFilter *>(data);
	Settings s = read_settings(settings);
	Charset set = make_charset(*f->font, s.charset, obs_data_get_string(settings, kSetCustomChars));

	std::lock_guard<std::mutex> lock(f->mutex);
	f->settings = s;
	if (set.chars != f->pending_charset.chars) {
		f->pending_charset = std::move(set);
		f->charset_dirty = true;
	}
}

void filter_defaults(void *type_data, obs_data_t *settings)
{
	UNUSED_PARAMETER(type_data);
	obs_data_set_default_string(settings, kSetCharset, charset_value(CharsetId::Blocks));
	obs_data_set_default_string(settings, kSetCustomChars, "");
	obs_data_set_default_int(settings, kSetCellScale, 1);
	obs_data_set_default_string(settings, kSetAlignment, "center");
	obs_data_set_default_string(settings, kSetEdges, "black");
	obs_data_set_default_bool(settings, kSetKeepTransparency, true);
}

// Shows Custom characters only for Custom, and the fallback note only when
// the typed characters give fewer than 2 distinct glyphs.
bool charset_modified(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	const auto *font = static_cast<const Font *>(priv);
	const bool custom = charset_from_value(obs_data_get_string(settings, kSetCharset)) == CharsetId::Custom;
	const bool fallback =
		custom &&
		!build_charset(*font, CharsetId::Custom, obs_data_get_string(settings, kSetCustomChars)).usable();
	obs_property_set_visible(obs_properties_get(props, kSetCustomChars), custom);
	obs_property_set_visible(obs_properties_get(props, kPropCustomFallback), fallback);
	return true;
}

#ifdef GLYPHNOTIC_DEBUG_TOOLS
bool dump_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	static_cast<GlyphFilter *>(data)->dump_requested = true;
	return false;
}
#endif

obs_properties_t *filter_get_properties(void *data, void *type_data)
{
#ifndef GLYPHNOTIC_DEBUG_TOOLS
	UNUSED_PARAMETER(data);
#endif
	auto *font = const_cast<Font *>(static_cast<const TypeData *>(type_data)->font);
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *chars = obs_properties_create();
	obs_property_t *list = obs_properties_add_list(chars, kSetCharset, obs_module_text("Glyphnotic.Charset"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (const auto &c : kCharsets)
		obs_property_list_add_string(list, obs_module_text(c.label_key), c.value);
	obs_property_set_modified_callback2(list, charset_modified, font);
	obs_property_t *custom = obs_properties_add_text(chars, kSetCustomChars,
							 obs_module_text("Glyphnotic.CustomChars"), OBS_TEXT_DEFAULT);
	obs_property_set_modified_callback2(custom, charset_modified, font);
	obs_property_t *note = obs_properties_add_text(chars, kPropCustomFallback,
						       obs_module_text("Glyphnotic.CustomFallback"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(note, OBS_TEXT_INFO_WARNING);
	obs_property_set_visible(note, false);
	obs_properties_add_group(props, "group_characters", obs_module_text("Glyphnotic.Group.Characters"),
				 OBS_GROUP_NORMAL, chars);

	obs_properties_t *grid = obs_properties_create();
	obs_property_t *scale = obs_properties_add_list(grid, kSetCellScale, obs_module_text("Glyphnotic.CellScale"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int s = kMinCellScale; s <= kMaxCellScale; s++) {
		char label[32];
		snprintf(label, sizeof(label), "%d\xC3\x97 (%dx%d)", s, kGlyphWidth * s, kGlyphHeight * s);
		obs_property_list_add_int(scale, label, s);
	}
	obs_property_t *align = obs_properties_add_list(grid, kSetAlignment, obs_module_text("Glyphnotic.Alignment"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(align, obs_module_text("Glyphnotic.Alignment.Center"), "center");
	obs_property_list_add_string(align, obs_module_text("Glyphnotic.Alignment.TopLeft"), "top_left");
	obs_property_t *edges = obs_properties_add_list(grid, kSetEdges, obs_module_text("Glyphnotic.Edges"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(edges, obs_module_text("Glyphnotic.Edges.Black"), "black");
	obs_property_list_add_string(edges, obs_module_text("Glyphnotic.Edges.Transparent"), "transparent");
	obs_properties_add_bool(grid, kSetKeepTransparency, obs_module_text("Glyphnotic.KeepTransparency"));
	obs_properties_add_group(props, "group_grid", obs_module_text("Glyphnotic.Group.Grid"), OBS_GROUP_NORMAL, grid);

#ifdef GLYPHNOTIC_DEBUG_TOOLS
	obs_properties_add_button2(props, kPropDumpGrid, obs_module_text("Glyphnotic.DumpGrid"), dump_clicked, data);
#endif
	return props;
}

// ---------------------------------------------------------------------------
// Create / destroy

const char *filter_get_name(void *type_data)
{
	const auto *td = static_cast<const TypeData *>(type_data);
	return obs_module_text(kModes[static_cast<int>(td->mode)].name_key);
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	const auto *td = static_cast<const TypeData *>(obs_source_get_type_data(source));
	auto *f = new (std::nothrow) GlyphFilter();
	if (!f)
		return nullptr;
	f->context = source;
	f->mode = td->mode;
	f->font = td->font;

	obs_enter_graphics();
	f->fx.sample = load_effect("sample.effect");
	f->fx.stats = load_effect("stats.effect");
	f->fx.score = load_effect("score.effect");
	f->fx.color = load_effect("cell-color.effect");
	f->fx.draw = load_effect("draw.effect");
	f->tr_sample = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
	f->tr_stats = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
	f->tr_score = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
	f->tr_color = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
	obs_leave_graphics();

	if (!f->fx.ok())
		obs_log(LOG_ERROR, "'%s': effects failed to load; the filter passes the picture through",
			obs_source_get_name(source));

	filter_update(f, settings);
	return f;
}

void filter_destroy(void *data)
{
	auto *f = static_cast<GlyphFilter *>(data);
	obs_enter_graphics();
	destroy_graphics(f);
	obs_leave_graphics();
	delete f;
}

// ---------------------------------------------------------------------------
// Render

void set_vec2(gs_effect_t *fx, const char *name, float x, float y)
{
	struct vec2 v;
	vec2_set(&v, x, y);
	gs_effect_set_vec2(gs_effect_get_param_by_name(fx, name), &v);
}

void set_int(gs_effect_t *fx, const char *name, int value)
{
	gs_effect_set_int(gs_effect_get_param_by_name(fx, name), value);
}

void set_float(gs_effect_t *fx, const char *name, float value)
{
	gs_effect_set_float(gs_effect_get_param_by_name(fx, name), value);
}

void set_tex(gs_effect_t *fx, const char *name, gs_texture_t *tex)
{
	gs_effect_set_texture(gs_effect_get_param_by_name(fx, name), tex);
}

void draw_technique(gs_effect_t *fx, uint32_t w, uint32_t h)
{
	gs_technique_t *tech = gs_effect_get_technique(fx, "Draw");
	const size_t passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; i++) {
		gs_technique_begin_pass(tech, i);
		gs_draw_sprite(nullptr, 0, w, h);
		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);
}

// Runs one full-target pass into a float texrender. Blending must be off.
template<typename SetParams> bool run_pass(gs_texrender_t *tr, gs_effect_t *fx, int w, int h, SetParams set_params)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, static_cast<uint32_t>(w), static_cast<uint32_t>(h)))
		return false;
	gs_ortho(0.0f, static_cast<float>(w), 0.0f, static_cast<float>(h), -100.0f, 100.0f);
	set_params();
	draw_technique(fx, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
	gs_texrender_end(tr);
	return true;
}

void upload_atlas(GlyphFilter *f)
{
	Charset set;
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		if (!f->charset_dirty && f->atlas)
			return;
		set = f->pending_charset;
		f->charset_dirty = false;
	}
	gs_texture_destroy(f->atlas);
	f->atlas = nullptr;
	std::vector<uint8_t> texels = build_atlas(set);
	const uint8_t *levels[] = {texels.data()};
	f->atlas = gs_texture_create(kGlyphPixels, static_cast<uint32_t>(set.size()), GS_R8, 1, levels, 0);
	f->atlas_charset = std::move(set);
}

void dump_grid(GlyphFilter *f, const Grid &grid, const Settings &s, uint32_t source_w, uint32_t source_h);

void filter_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	auto *f = static_cast<GlyphFilter *>(data);

	obs_source_t *target = obs_filter_get_target(f->context);
	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);

	Settings s;
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		s = f->settings;
	}
	const Grid grid = make_grid(static_cast<int>(w), static_cast<int>(h), s.cell_scale, s.alignment);

	// Ink is Phase 2: it still passes the picture through. So does a source
	// smaller than one cell, or a filter whose effects failed to load.
	if (f->mode == Mode::Ink || !f->fx.ok() || grid.empty()) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	upload_atlas(f);
	if (!f->atlas) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	// Ask for SDR sRGB input; OBS converts HDR sources down. The input arrives
	// premultiplied, as OBS renders sources.
	if (!obs_source_process_filter_begin_with_color_space(f->context, GS_RGBA, GS_CS_SRGB, OBS_NO_DIRECT_RENDERING))
		return;

	gs_blend_state_push();
	gs_enable_blending(false);
	const bool prev_srgb = gs_framebuffer_srgb_enabled();

	// Sample: OBS draws its input texture with our effect. The filter doesn't
	// declare OBS_SOURCE_SRGB, so OBS binds the 8-bit texture without its sRGB
	// view and with framebuffer sRGB off: the shader reads the stored
	// sRGB-encoded values and decodes them itself, exactly once.
	bool ok = false;
	gs_texrender_reset(f->tr_sample);
	if (gs_texrender_begin(f->tr_sample, static_cast<uint32_t>(grid.sample_w),
			       static_cast<uint32_t>(grid.sample_h))) {
		gs_ortho(0.0f, static_cast<float>(grid.sample_w), 0.0f, static_cast<float>(grid.sample_h), -100.0f,
			 100.0f);
		set_vec2(f->fx.sample, "sample_size", static_cast<float>(grid.sample_w),
			 static_cast<float>(grid.sample_h));
		set_vec2(f->fx.sample, "grid_offset", static_cast<float>(grid.offset_x),
			 static_cast<float>(grid.offset_y));
		set_int(f->fx.sample, "cell_scale", grid.scale);
		obs_source_process_filter_tech_end(f->context, f->fx.sample, static_cast<uint32_t>(grid.sample_w),
						   static_cast<uint32_t>(grid.sample_h), "Draw");
		gs_texrender_end(f->tr_sample);
		ok = true;
	}
	gs_enable_framebuffer_srgb(false);

	gs_texture_t *samples = gs_texrender_get_texture(f->tr_sample);
	const auto cells_x = static_cast<float>(grid.cells_x);
	const auto cells_y = static_cast<float>(grid.cells_y);

	ok = ok && run_pass(f->tr_stats, f->fx.stats, grid.cells_x, grid.cells_y, [&] {
		     set_tex(f->fx.stats, "samples", samples);
		     set_vec2(f->fx.stats, "cell_count", cells_x, cells_y);
	     });
	ok = ok && run_pass(f->tr_score, f->fx.score, grid.cells_x, grid.cells_y, [&] {
		     set_tex(f->fx.score, "samples", samples);
		     set_tex(f->fx.score, "atlas", f->atlas);
		     set_vec2(f->fx.score, "cell_count", cells_x, cells_y);
		     set_int(f->fx.score, "glyph_count", static_cast<int>(f->atlas_charset.size()));
	     });
	ok = ok && run_pass(f->tr_color, f->fx.color, grid.cells_x * 2, grid.cells_y, [&] {
		     set_tex(f->fx.color, "samples", samples);
		     set_tex(f->fx.color, "atlas", f->atlas);
		     set_tex(f->fx.color, "scores", gs_texrender_get_texture(f->tr_score));
		     set_tex(f->fx.color, "stats", gs_texrender_get_texture(f->tr_stats));
		     set_vec2(f->fx.color, "cell_count", cells_x, cells_y);
		     set_float(f->fx.color, "keep_transparency", s.keep_transparency ? 1.0f : 0.0f);
	     });

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_srgb);

	if (!ok)
		return;

	if (f->dump_requested.exchange(false))
		dump_grid(f, grid, s, w, h);

	// Draw into OBS's target. OBS turns linear sRGB off for filters that
	// aren't OBS_SOURCE_SRGB, so the shader writes sRGB-encoded values; the
	// linear branch only follows OBS's convention should that state be on.
	const bool linear_srgb = gs_get_linear_srgb();
	gs_enable_framebuffer_srgb(linear_srgb);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_effect_t *fx = f->fx.draw;
	set_tex(fx, "atlas", f->atlas);
	set_tex(fx, "scores", gs_texrender_get_texture(f->tr_score));
	set_tex(fx, "colors", gs_texrender_get_texture(f->tr_color));
	set_vec2(fx, "source_size", static_cast<float>(w), static_cast<float>(h));
	set_vec2(fx, "grid_offset", static_cast<float>(grid.offset_x), static_cast<float>(grid.offset_y));
	set_vec2(fx, "grid_size", static_cast<float>(grid.width_px()), static_cast<float>(grid.height_px()));
	set_int(fx, "cell_scale", grid.scale);
	set_float(fx, "edges_transparent", s.edges_transparent ? 1.0f : 0.0f);
	set_float(fx, "output_linear", linear_srgb ? 1.0f : 0.0f);
	draw_technique(fx, w, h);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_srgb);
}

enum gs_color_space filter_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);
	return GS_CS_SRGB; // v1.0 outputs SDR
}

// ---------------------------------------------------------------------------
// Grid dump (debug builds): the shown glyph and colors of every cell, for
// tools/reference/compare.py.

double srgb_encode(double u)
{
	u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
	return u <= 0.0031308 ? 12.92 * u : 1.055 * std::pow(u, 1.0 / 2.4) - 0.055;
}

void oklab_to_srgb255(const float *lab, double out[3])
{
	const double L = lab[0], a = lab[1], b = lab[2];
	double l = L + 0.3963377774 * a + 0.2158037573 * b;
	double m = L - 0.1055613458 * a - 0.0638541728 * b;
	double s = L - 0.0894841775 * a - 1.2914855480 * b;
	l = l * l * l;
	m = m * m * m;
	s = s * s * s;
	const double r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
	const double g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
	const double bl = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
	out[0] = srgb_encode(r) * 255.0;
	out[1] = srgb_encode(g) * 255.0;
	out[2] = srgb_encode(bl) * 255.0;
}

// Reads a float texture back to the CPU (4 floats per texel, row-major).
bool read_texture(gs_texture_t *tex, uint32_t w, uint32_t h, std::vector<float> &out)
{
	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA32F);
	if (!stage)
		return false;
	gs_stage_texture(stage, tex);
	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	bool ok = gs_stagesurface_map(stage, &ptr, &linesize);
	if (ok) {
		out.resize(static_cast<size_t>(w) * h * 4);
		for (uint32_t y = 0; y < h; y++)
			memcpy(out.data() + static_cast<size_t>(y) * w * 4, ptr + static_cast<size_t>(y) * linesize,
			       static_cast<size_t>(w) * 4 * sizeof(float));
		gs_stagesurface_unmap(stage);
	}
	gs_stagesurface_destroy(stage);
	return ok;
}

std::string dump_path()
{
	const char *env = getenv("GLYPHNOTIC_DUMP_PATH");
	if (env && *env)
		return env;
	char *dir = obs_module_config_path("");
	std::string path;
	if (dir) {
		os_mkdirs(dir);
		path = std::string(dir) + "/grid-dump.json";
		bfree(dir);
	}
	return path;
}

void dump_grid(GlyphFilter *f, const Grid &grid, const Settings &s, uint32_t source_w, uint32_t source_h)
{
	const auto cx = static_cast<uint32_t>(grid.cells_x);
	const auto cy = static_cast<uint32_t>(grid.cells_y);
	std::vector<float> scores, colors, stats;
	if (!read_texture(gs_texrender_get_texture(f->tr_score), cx, cy, scores) ||
	    !read_texture(gs_texrender_get_texture(f->tr_color), cx * 2, cy, colors) ||
	    !read_texture(gs_texrender_get_texture(f->tr_stats), cx, cy, stats)) {
		obs_log(LOG_WARNING, "grid dump: texture readback failed");
		return;
	}

	const std::string path = dump_path();
	FILE *file = path.empty() ? nullptr : os_fopen(path.c_str(), "wb");
	if (!file) {
		obs_log(LOG_WARNING, "grid dump: cannot write '%s'", path.c_str());
		return;
	}

	const size_t n = static_cast<size_t>(cx) * cy;
	auto list = [&](const char *key, auto value_of, bool last = false) {
		fprintf(file, "\"%s\":[", key);
		for (size_t i = 0; i < n; i++)
			fprintf(file, i ? ",%s" : "%s", value_of(i).c_str());
		fprintf(file, "]%s\n", last ? "" : ",");
	};
	auto num = [](double v) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%.9g", v);
		return std::string(buf);
	};
	auto vec3 = [&](const float *p) {
		return "[" + num(p[0]) + "," + num(p[1]) + "," + num(p[2]) + "]";
	};
	auto srgb3 = [&](const float *p) {
		double c[3];
		oklab_to_srgb255(p, c);
		return "[" + num(c[0]) + "," + num(c[1]) + "," + num(c[2]) + "]";
	};
	const float *text = colors.data();
	auto text_of = [&](size_t i) {
		return text + ((i / cx) * cx * 2 + (i % cx) * 2) * 4;
	};
	auto back_of = [&](size_t i) {
		return text_of(i) + 4;
	};

	fprintf(file, "{\"version\":1,\"mode\":\"%s\",\"charset_name\":\"%s\",\n",
		kModes[static_cast<int>(f->mode)].name, charset_value(s.charset));
	fprintf(file, "\"charset\":[");
	for (size_t i = 0; i < f->atlas_charset.size(); i++)
		fprintf(file, i ? ",%u" : "%u", static_cast<unsigned>(f->atlas_charset.chars[i]));
	fprintf(file, "],\n");
	fprintf(file,
		"\"source_width\":%u,\"source_height\":%u,\"cell_scale\":%d,\"alignment\":\"%s\",\"edges\":\"%s\","
		"\"keep_transparency\":%s,\n",
		source_w, source_h, grid.scale, s.alignment == Alignment::Center ? "center" : "top_left",
		s.edges_transparent ? "transparent" : "black", s.keep_transparency ? "true" : "false");
	fprintf(file, "\"cells_x\":%u,\"cells_y\":%u,\"offset_x\":%d,\"offset_y\":%d,\n", cx, cy, grid.offset_x,
		grid.offset_y);
	list("glyph", [&](size_t i) { return num(scores[i * 4 + 1]); });
	list("held", [&](size_t i) { return num(scores[i * 4 + 0]); });
	list("cost", [&](size_t i) { return num(scores[i * 4 + 3]); });
	list("mean_lab", [&](size_t i) { return vec3(&stats[i * 4]); });
	list("mean_alpha", [&](size_t i) { return num(stats[i * 4 + 3]); });
	list("opaque", [&](size_t i) { return num(text_of(i)[3]); });
	list("text_lab", [&](size_t i) { return vec3(text_of(i)); });
	list("bg_lab", [&](size_t i) { return vec3(back_of(i)); });
	list("text_srgb", [&](size_t i) { return srgb3(text_of(i)); });
	list("bg_srgb", [&](size_t i) { return srgb3(back_of(i)); }, true);
	fprintf(file, "}\n");
	fclose(file);
	obs_log(LOG_INFO, "grid dump written to %s (%ux%u cells)", path.c_str(), cx, cy);
}

void free_type_data(void *type_data)
{
	delete static_cast<TypeData *>(type_data);
}

} // namespace

void register_glyph_filter(Mode mode, const Font *font)
{
	obs_source_info info = {};
	info.id = kModes[static_cast<int>(mode)].id;
	info.type = OBS_SOURCE_TYPE_FILTER;
	// Deliberately not OBS_SOURCE_SRGB: OBS then hands over and takes back
	// sRGB-encoded values untouched, and the shaders do the conversions with
	// the exact formulas instead of the GPU's approximate sRGB hardware.
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = filter_get_name;
	info.create = filter_create;
	info.destroy = filter_destroy;
	info.update = filter_update;
	info.get_defaults2 = filter_defaults;
	info.get_properties2 = filter_get_properties;
	info.video_render = filter_video_render;
	info.video_get_color_space = filter_get_color_space;
	info.type_data = new TypeData{mode, font};
	info.free_type_data = free_type_data;
	obs_register_source(&info);
}

} // namespace glyphnotic
