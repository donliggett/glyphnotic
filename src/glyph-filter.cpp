// Glyphnotic: the shared filter core.
//
// One implementation, registered once per drawing mode (type_data carries
// the mode). Each frame runs a short chain of libobs render passes (see
// docs/SPEC.md, Render passes):
//
//   Soften      (Soften > 0) the input blurred along rows, at source size
//   Sample      filter input -> OKLab + alpha, cellsX*8 x cellsY*16 (with
//               Soften, blurred along columns as it reads)
//   Cell stats  mean OKLab and alpha per cell
//   Reduce      sums of the cells' lightness and of the cells whose lightness
//               jumped since the last frame, down to 1x1
//   Levels      (2x1) the light jump, and Ink's lo/hi eased against the last
//   Lightness   (Ink) each sample's stretched and shaped L'
//   Score       held and shown glyph per cell: Hold and Boil
//   Color       text and background color per cell (double-width target),
//               blended with the last frame's by Settle
//   Draw        the character grid at source size, into OBS's target
//
// Cell stats, Levels, Score and Color keep the previous frame (ping-pong
// pairs, `cur` holds the latest). Settings, properties and looks live in
// modes.cpp; the motion formulas in motion.hpp; palettes and capture's
// k-means in palette.cpp.

#include "glyph-filter.hpp"

#include "font.hpp"
#include "grid.hpp"
#include "modes.hpp"
#include "motion.hpp"
#include "palette.hpp"
#include "presets.hpp"

#include <graphics/vec2.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace glyphnotic {

namespace {

constexpr int kReduceBlock = 16;   // Reduce sums 16x16 blocks per pass
constexpr int kCaptureWidth = 160; // Capture palette reads back about 160x90
// The properties window refreshes this long after the last change of a drag.
constexpr uint64_t kRefreshDelayNs = 600'000'000;

struct Effects {
	gs_effect_t *soften = nullptr;
	gs_effect_t *sample = nullptr;
	gs_effect_t *stats = nullptr;
	gs_effect_t *levels = nullptr;
	gs_effect_t *lightness = nullptr;
	gs_effect_t *score = nullptr;
	gs_effect_t *color = nullptr;
	gs_effect_t *draw = nullptr;
	gs_effect_t *capture = nullptr;

	bool ok() const
	{
		return soften && sample && stats && levels && lightness && score && color && draw && capture;
	}
};

struct GlyphFilter {
	obs_source_t *context = nullptr;
	TypeData *td = nullptr;
	Mode mode = Mode::Ink;
	const Font *font = nullptr;

	// Graphics resources, touched only with the graphics context entered.
	Effects fx;
	gs_texrender_t *tr_soften = nullptr;
	gs_texrender_t *tr_sample = nullptr;
	gs_texrender_t *tr_lightness = nullptr;
	gs_texrender_t *tr_capture = nullptr;
	// Ping-pong pairs: [cur] holds the latest frame, [1 - cur] the one before.
	gs_texrender_t *tr_stats[2] = {nullptr, nullptr};
	gs_texrender_t *tr_levels[2] = {nullptr, nullptr}; // 2x1: Ink's levels, the light jump
	gs_texrender_t *tr_score[2] = {nullptr, nullptr};
	gs_texrender_t *tr_color[2] = {nullptr, nullptr};
	std::vector<gs_texrender_t *> tr_reduce;
	gs_texture_t *atlas = nullptr;
	Charset atlas_charset; // the set uploaded to `atlas`
	float atlas_max_n_on = 1.0f;
	gs_texture_t *palette_tex = nullptr;
	Palette palette_uploaded{};

	// Written by update() on the UI or video thread, read by the render thread.
	std::mutex mutex;
	Settings settings;
	Charset pending_charset;
	bool charset_dirty = false;
	bool reset_pending = true; // the grid's history must start over
	std::string last_look;     // the look at the previous update
	bool created = false;
	std::atomic<uint64_t> refresh_at_ns{0}; // when to refresh the properties window; 0 = not waiting

	// Render-thread state.
	int cur = 0;          // which texrender of each pair holds the latest frame
	bool history = false; // false: the next frame starts the grid's history over
	int last_cells_x = -1, last_cells_y = -1, last_scale = -1;
	uint64_t frame = 0;
	uint32_t seed = 0; // Boil's per-instance seed
	std::atomic<float> tick_seconds{0.0f};

	std::atomic<bool> dump_requested{false};
	std::atomic<bool> record_requested{false};
	FILE *record_file = nullptr; // debug builds: per-frame grid recording
	std::string record_path;
	int record_left = 0;
	bool record_colors = false;
	std::atomic<bool> capture_requested{false};
	std::atomic<bool> capture_running{false};
	std::thread capture_thread;
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
	for (gs_effect_t *e : {f->fx.soften, f->fx.sample, f->fx.stats, f->fx.levels, f->fx.lightness, f->fx.score,
			       f->fx.color, f->fx.draw, f->fx.capture})
		gs_effect_destroy(e);
	f->fx = Effects{};
	for (gs_texrender_t *t : {f->tr_soften, f->tr_sample, f->tr_lightness, f->tr_capture})
		gs_texrender_destroy(t);
	for (int i = 0; i < 2; i++)
		for (gs_texrender_t *t : {f->tr_stats[i], f->tr_levels[i], f->tr_score[i], f->tr_color[i]})
			gs_texrender_destroy(t);
	for (gs_texrender_t *t : f->tr_reduce)
		gs_texrender_destroy(t);
	f->tr_reduce.clear();
	gs_texture_destroy(f->atlas);
	gs_texture_destroy(f->palette_tex);
	f->atlas = f->palette_tex = nullptr;
}

gs_texrender_t *float_target()
{
	return gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
}

// ---------------------------------------------------------------------------
// Settings

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

// True if a change from a to b alters the grid's history (docs/SPEC.md,
// Render passes): the character set, the cell scale or a mode color setting.
bool grid_changed(const Settings &a, const Settings &b)
{
	return a.charset != b.charset || a.cell_scale != b.cell_scale || a.alignment != b.alignment ||
	       a.keep_transparency != b.keep_transparency || a.ink_color != b.ink_color || a.ink_a != b.ink_a ||
	       a.ink_b != b.ink_b || a.background_tint != b.background_tint || a.ink_lightness != b.ink_lightness ||
	       a.saturation != b.saturation || a.gamma != b.gamma || a.shape != b.shape ||
	       a.levels_auto != b.levels_auto || a.black_point != b.black_point || a.white_point != b.white_point ||
	       a.poster_levels != b.poster_levels || a.palette_custom != b.palette_custom || !(a.palette == b.palette);
}

void filter_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<GlyphFilter *>(data);
	const bool changed = reconcile_look(f->mode, settings, f->last_look);
	const std::string look = obs_data_get_string(settings, key::kLook);
	if (f->created && changed)
		obs_source_update_properties(f->context); // the properties window shows the new values
	// The window's own callback turns the Look Custom without refreshing, so a
	// slider drag isn't dropped. OBS's Filters window updates the filter on
	// every change of a drag, so the refresh waits until the changes stop.
	if (f->created && ((look == kLookCustom && f->last_look != kLookCustom) || f->refresh_at_ns != 0))
		f->refresh_at_ns = os_gettime_ns() + kRefreshDelayNs;
	f->last_look = look;

	Settings s = read_settings(f->mode, settings);
	Charset set = make_charset(*f->font, s.charset, obs_data_get_string(settings, key::kCustomChars));

	std::lock_guard<std::mutex> lock(f->mutex);
	if (grid_changed(f->settings, s))
		f->reset_pending = true;
	f->settings = s;
	if (set.chars != f->pending_charset.chars) {
		f->pending_charset = std::move(set);
		f->charset_dirty = true;
		f->reset_pending = true;
	}
}

void filter_defaults(void *type_data, obs_data_t *settings)
{
	mode_defaults(static_cast<const TypeData *>(type_data)->mode, settings);
}

#ifdef GLYPHNOTIC_DEBUG_TOOLS
bool dump_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	if (data)
		static_cast<GlyphFilter *>(data)->dump_requested = true;
	return false;
}

bool record_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	if (data)
		static_cast<GlyphFilter *>(data)->record_requested = true;
	return false;
}
#endif

bool capture_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	if (data)
		static_cast<GlyphFilter *>(data)->capture_requested = true;
	return false;
}

obs_properties_t *filter_get_properties(void *data, void *type_data)
{
	auto *td = static_cast<TypeData *>(type_data);
	obs_properties_t *props = obs_properties_create();
	add_properties(props, td, capture_clicked, data);
#ifdef GLYPHNOTIC_DEBUG_TOOLS
	obs_properties_add_button2(props, kPropDumpGrid, obs_module_text("Glyphnotic.DumpGrid"), dump_clicked, data);
	obs_properties_add_button2(props, kPropRecordGrid, obs_module_text("Glyphnotic.RecordGrid"), record_clicked,
				   data);
#endif
	return props;
}

// ---------------------------------------------------------------------------
// Create / destroy

const char *filter_get_name(void *type_data)
{
	return obs_module_text(mode_info(static_cast<const TypeData *>(type_data)->mode).name_key);
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *td = static_cast<TypeData *>(obs_source_get_type_data(source));
	auto *f = new (std::nothrow) GlyphFilter();
	if (!f)
		return nullptr;
	f->context = source;
	f->td = td;
	f->mode = td->mode;
	f->font = td->font;

	// Boil's seed: different for every instance, so no two boil in step.
	uint64_t z = os_gettime_ns() ^ reinterpret_cast<uintptr_t>(f);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	f->seed = static_cast<uint32_t>((z ^ (z >> 31)) & 0x7FFFFFFF);

	obs_enter_graphics();
	f->fx.soften = load_effect("soften.effect");
	f->fx.sample = load_effect("sample.effect");
	f->fx.stats = load_effect("stats.effect");
	f->fx.levels = load_effect("levels.effect");
	f->fx.lightness = load_effect("lightness.effect");
	f->fx.score = load_effect("score.effect");
	f->fx.color = load_effect("cell-color.effect");
	f->fx.draw = load_effect("draw.effect");
	f->fx.capture = load_effect("capture.effect");
	f->tr_soften = float_target();
	f->tr_sample = float_target();
	f->tr_lightness = float_target();
	f->tr_capture = float_target();
	for (int i = 0; i < 2; i++) {
		f->tr_stats[i] = float_target();
		f->tr_levels[i] = float_target();
		f->tr_score[i] = float_target();
		f->tr_color[i] = float_target();
	}
	obs_leave_graphics();

	if (!f->fx.ok())
		obs_log(LOG_ERROR, "'%s': effects failed to load; the filter passes the picture through",
			obs_source_get_name(source));

	// A look named in the settings is applied; without one, the settings
	// stand, and the Look shows Custom if they differ from the default look's.
	if (!obs_data_has_user_value(settings, key::kLook))
		f->last_look = obs_data_get_string(settings, key::kLook);
	filter_update(f, settings);
	f->created = true;
	return f;
}

void filter_destroy(void *data)
{
	auto *f = static_cast<GlyphFilter *>(data);
	if (f->capture_thread.joinable())
		f->capture_thread.join();
	if (f->record_file)
		fclose(f->record_file);
	obs_enter_graphics();
	destroy_graphics(f);
	obs_leave_graphics();
	delete f;
}

void filter_tick(void *data, float seconds)
{
	auto *f = static_cast<GlyphFilter *>(data);
	f->tick_seconds = seconds;
	// A properties refresh waiting for a drag to end (see filter_update).
	uint64_t at = f->refresh_at_ns;
	if (at != 0 && os_gettime_ns() >= at && f->refresh_at_ns.compare_exchange_strong(at, 0))
		obs_source_update_properties(f->context);
}

// ---------------------------------------------------------------------------
// Render helpers

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

void set_flag(gs_effect_t *fx, const char *name, bool value)
{
	set_float(fx, name, value ? 1.0f : 0.0f);
}

void set_tex(gs_effect_t *fx, const char *name, gs_texture_t *tex)
{
	gs_effect_set_texture(gs_effect_get_param_by_name(fx, name), tex);
}

void draw_technique(gs_effect_t *fx, const char *technique, uint32_t w, uint32_t h)
{
	gs_technique_t *tech = gs_effect_get_technique(fx, technique);
	const size_t passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; i++) {
		gs_technique_begin_pass(tech, i);
		gs_draw_sprite(nullptr, 0, w, h);
		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);
}

// Runs one full-target pass into a float texrender. Blending must be off.
template<typename SetParams>
bool run_pass(gs_texrender_t *tr, gs_effect_t *fx, const char *technique, int w, int h, SetParams set_params)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, static_cast<uint32_t>(w), static_cast<uint32_t>(h)))
		return false;
	gs_ortho(0.0f, static_cast<float>(w), 0.0f, static_cast<float>(h), -100.0f, 100.0f);
	set_params();
	draw_technique(fx, technique, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
	gs_texrender_end(tr);
	return true;
}

// Score's score_mode: 0 Mosaic (and Poster), 1 Ink, 2 Console.
int score_mode(Mode mode)
{
	switch (mode) {
	case Mode::Ink:
		return 1;
	case Mode::Console:
		return 2;
	default:
		return 0;
	}
}

// Color's technique for a mode: Poster colors like Mosaic.
const char *mode_technique(Mode mode)
{
	switch (mode) {
	case Mode::Ink:
		return "Ink";
	case Mode::Console:
		return "Console";
	default:
		return "Mosaic";
	}
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
	int max_n = 1;
	for (int n : set.n_on)
		max_n = std::max(max_n, n);
	f->atlas_max_n_on = static_cast<float>(max_n);
	f->atlas_charset = std::move(set);
}

// The palette as a 16x1 float texture of OKLab colors.
void upload_palette(GlyphFilter *f, const Palette &palette)
{
	if (f->palette_tex && palette == f->palette_uploaded)
		return;
	float texels[kPaletteSize * 4];
	for (int k = 0; k < kPaletteSize; k++) {
		const Lab c = srgb8_to_oklab(palette[static_cast<size_t>(k)]);
		texels[k * 4 + 0] = static_cast<float>(c.L);
		texels[k * 4 + 1] = static_cast<float>(c.a);
		texels[k * 4 + 2] = static_cast<float>(c.b);
		texels[k * 4 + 3] = 1.0f;
	}
	gs_texture_destroy(f->palette_tex);
	const uint8_t *levels[] = {reinterpret_cast<const uint8_t *>(texels)};
	f->palette_tex = gs_texture_create(kPaletteSize, 1, GS_RGBA32F, 1, levels, 0);
	f->palette_uploaded = palette;
}

gs_texrender_t *reduce_target(GlyphFilter *f, size_t i)
{
	while (f->tr_reduce.size() <= i)
		f->tr_reduce.push_back(float_target());
	return f->tr_reduce[i];
}

// Reduce and Levels: this frame's state into tr_levels[next] (2x1): the
// light jump, and Ink's lo/hi.
bool run_frame_state(GlyphFilter *f, const Grid &grid, const Settings &s, int next)
{
	const int prev = 1 - next;
	gs_texture_t *src = gs_texrender_get_texture(f->tr_stats[next]);
	gs_texture_t *prev_stats = gs_texrender_get_texture(f->tr_stats[prev]);
	int sw = grid.cells_x, sh = grid.cells_y;
	const char *tech = "ReduceStats";
	size_t i = 0;
	bool ok = true;
	do {
		const int dw = (sw + kReduceBlock - 1) / kReduceBlock;
		const int dh = (sh + kReduceBlock - 1) / kReduceBlock;
		gs_texrender_t *tr = reduce_target(f, i++);
		ok = ok && run_pass(tr, f->fx.levels, tech, dw, dh, [&] {
			     set_tex(f->fx.levels, "src", src);
			     set_vec2(f->fx.levels, "src_size", static_cast<float>(sw), static_cast<float>(sh));
			     set_vec2(f->fx.levels, "dst_size", static_cast<float>(dw), static_cast<float>(dh));
			     set_int(f->fx.levels, "block", kReduceBlock);
			     set_flag(f->fx.levels, "keep_transparency", s.keep_transparency);
			     set_tex(f->fx.levels, "prev_stats", prev_stats);
			     set_flag(f->fx.levels, "have_prev", f->history);
			     set_float(f->fx.levels, "jump_delta", static_cast<float>(kLightJumpDelta));
		     });
		src = gs_texrender_get_texture(tr);
		sw = dw;
		sh = dh;
		tech = "ReduceSum";
	} while (ok && (sw > 1 || sh > 1));

	// Ease toward this frame's target: 1 - exp(-dt / adapt speed); 1 snaps
	// (and so does a light jump, in the shader).
	const float dt = f->tick_seconds;
	const float ease = f->history ? 1.0f - std::exp(-dt / static_cast<float>(s.adapt_speed)) : 1.0f;
	gs_texture_t *prev_levels = gs_texrender_get_texture(f->tr_levels[prev]);
	ok = ok && run_pass(f->tr_levels[next], f->fx.levels, "Levels", 2, 1, [&] {
		     set_tex(f->fx.levels, "sums", src);
		     set_tex(f->fx.levels, "prev", prev_levels);
		     set_flag(f->fx.levels, "levels_auto", s.levels_auto);
		     set_float(f->fx.levels, "black_point", static_cast<float>(s.black_point));
		     set_float(f->fx.levels, "white_point", static_cast<float>(s.white_point));
		     set_float(f->fx.levels, "ease", ease);
		     set_flag(f->fx.levels, "have_prev", f->history);
		     set_float(f->fx.levels, "cell_total", static_cast<float>(grid.cells_x * grid.cells_y));
		     set_float(f->fx.levels, "jump_share", static_cast<float>(kLightJumpShare));
	     });
	return ok;
}

// Boil's chance per frame, from OBS's frame rate.
float frame_boil_chance(int boil)
{
	const double frame_seconds = static_cast<double>(obs_get_frame_interval_ns()) / 1e9;
	return static_cast<float>(boil_chance(boil, frame_seconds));
}

void start_capture(GlyphFilter *f, const Grid &grid);
void dump_grid(GlyphFilter *f, const Grid &grid, const Settings &s, uint32_t source_w, uint32_t source_h);
void record_frame(GlyphFilter *f, const Grid &grid);

// ---------------------------------------------------------------------------
// Render

void filter_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	auto *f = static_cast<GlyphFilter *>(data);

	obs_source_t *target = obs_filter_get_target(f->context);
	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);

	Settings s;
	bool reset = false;
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		s = f->settings;
		reset = f->reset_pending;
		f->reset_pending = false;
	}
	const Grid grid = make_grid(static_cast<int>(w), static_cast<int>(h), s.cell_scale, s.alignment);

	// A settings change or a new grid size starts the grid's history over.
	if (reset || grid.cells_x != f->last_cells_x || grid.cells_y != f->last_cells_y ||
	    grid.scale != f->last_scale) {
		f->history = false;
		f->last_cells_x = grid.cells_x;
		f->last_cells_y = grid.cells_y;
		f->last_scale = grid.scale;
	}

	// A source smaller than one cell passes through, as does a filter whose
	// effects failed to load.
	if (!f->fx.ok() || grid.empty()) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	upload_atlas(f);
	if (f->mode == Mode::Console)
		upload_palette(f, s.palette);
	if (!f->atlas || (f->mode == Mode::Console && !f->palette_tex)) {
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

	// OBS draws its input texture with one of our effects: Sample's, or with
	// Soften on, Soften's row blur. The filter doesn't declare
	// OBS_SOURCE_SRGB, so OBS binds the 8-bit texture without its sRGB view and
	// with framebuffer sRGB off: the shaders read the stored sRGB-encoded
	// values, and Sample decodes them itself, exactly once.
	const int radius = soften_radius(s.soften);
	const auto sw = static_cast<uint32_t>(grid.sample_w), sh = static_cast<uint32_t>(grid.sample_h);
	auto set_sample_params = [&] {
		set_vec2(f->fx.sample, "sample_size", static_cast<float>(grid.sample_w),
			 static_cast<float>(grid.sample_h));
		set_vec2(f->fx.sample, "grid_offset", static_cast<float>(grid.offset_x),
			 static_cast<float>(grid.offset_y));
		set_int(f->fx.sample, "cell_scale", grid.scale);
		// Direct3D wants every parameter a technique's shaders reference set.
		set_vec2(f->fx.sample, "source_size", static_cast<float>(w), static_cast<float>(h));
		set_float(f->fx.sample, "sigma", static_cast<float>(s.soften));
		set_int(f->fx.sample, "radius", radius);
	};
	bool ok = false;
	if (radius == 0) {
		gs_texrender_reset(f->tr_sample);
		if (gs_texrender_begin(f->tr_sample, sw, sh)) {
			gs_ortho(0.0f, static_cast<float>(sw), 0.0f, static_cast<float>(sh), -100.0f, 100.0f);
			set_sample_params();
			obs_source_process_filter_tech_end(f->context, f->fx.sample, sw, sh, "Draw");
			gs_texrender_end(f->tr_sample);
			ok = true;
		}
		gs_enable_framebuffer_srgb(false);
	} else {
		gs_texrender_reset(f->tr_soften);
		if (gs_texrender_begin(f->tr_soften, w, h)) {
			gs_ortho(0.0f, static_cast<float>(w), 0.0f, static_cast<float>(h), -100.0f, 100.0f);
			set_vec2(f->fx.soften, "source_size", static_cast<float>(w), static_cast<float>(h));
			set_float(f->fx.soften, "sigma", static_cast<float>(s.soften));
			set_int(f->fx.soften, "radius", radius);
			obs_source_process_filter_tech_end(f->context, f->fx.soften, w, h, "BlurH");
			gs_texrender_end(f->tr_soften);
			ok = true;
		}
		gs_enable_framebuffer_srgb(false);
		ok = ok && run_pass(f->tr_sample, f->fx.sample, "DrawSoft", grid.sample_w, grid.sample_h, [&] {
			     set_sample_params();
			     set_tex(f->fx.sample, "image", gs_texrender_get_texture(f->tr_soften));
		     });
	}

	// Each pass of a ping-pong pair writes [next] and reads the last frame from [cur].
	const int next = 1 - f->cur;
	gs_texture_t *samples = gs_texrender_get_texture(f->tr_sample);
	const auto cells_x = static_cast<float>(grid.cells_x);
	const auto cells_y = static_cast<float>(grid.cells_y);

	ok = ok && run_pass(f->tr_stats[next], f->fx.stats, "Draw", grid.cells_x, grid.cells_y, [&] {
		     set_tex(f->fx.stats, "samples", samples);
		     set_vec2(f->fx.stats, "cell_count", cells_x, cells_y);
	     });
	ok = ok && run_frame_state(f, grid, s, next);
	gs_texture_t *frame_state = gs_texrender_get_texture(f->tr_levels[next]);

	// Ink scores lightness only: its Score pass reads L' instead of the samples.
	gs_texture_t *score_input = samples;
	if (f->mode == Mode::Ink) {
		ok = ok && run_pass(f->tr_lightness, f->fx.lightness, "Draw", grid.sample_w, grid.sample_h, [&] {
			     set_tex(f->fx.lightness, "samples", samples);
			     set_tex(f->fx.lightness, "levels", frame_state);
			     set_vec2(f->fx.lightness, "sample_size", static_cast<float>(grid.sample_w),
				      static_cast<float>(grid.sample_h));
			     set_float(f->fx.lightness, "ink_lightness", static_cast<float>(s.ink_lightness));
			     set_float(f->fx.lightness, "gamma", static_cast<float>(s.gamma));
		     });
		score_input = gs_texrender_get_texture(f->tr_lightness);
	}

	ok = ok && run_pass(f->tr_score[next], f->fx.score, "Draw", grid.cells_x, grid.cells_y, [&] {
		     set_tex(f->fx.score, "samples", score_input);
		     set_tex(f->fx.score, "atlas", f->atlas);
		     set_tex(f->fx.score, "palette", f->palette_tex);
		     set_vec2(f->fx.score, "cell_count", cells_x, cells_y);
		     set_int(f->fx.score, "glyph_count", static_cast<int>(f->atlas_charset.size()));
		     set_int(f->fx.score, "score_mode", score_mode(f->mode));
		     set_float(f->fx.score, "ink_lightness", static_cast<float>(s.ink_lightness));
		     set_float(f->fx.score, "shape", static_cast<float>(s.shape));
		     set_float(f->fx.score, "max_n_on", f->atlas_max_n_on);
		     set_tex(f->fx.score, "prev_scores", gs_texrender_get_texture(f->tr_score[f->cur]));
		     set_tex(f->fx.score, "frame_state", frame_state);
		     set_flag(f->fx.score, "history", f->history);
		     set_float(f->fx.score, "hold_threshold", static_cast<float>(hold_threshold(f->mode, s.hold)));
		     set_float(f->fx.score, "boil_chance", frame_boil_chance(s.boil));
		     set_float(f->fx.score, "boil_spread", static_cast<float>(boil_spread_share(s.boil_spread)));
		     set_int(f->fx.score, "boil_linger", s.boil_linger);
		     set_int(f->fx.score, "seed", static_cast<int>(f->seed));
		     set_int(f->fx.score, "frame_index", static_cast<int>(f->frame & 0x7FFFFFFF));
	     });
	gs_texture_t *scores = gs_texrender_get_texture(f->tr_score[next]);
	ok = ok &&
	     run_pass(f->tr_color[next], f->fx.color, mode_technique(f->mode), grid.cells_x * 2, grid.cells_y, [&] {
		     set_tex(f->fx.color, "samples", samples);
		     set_tex(f->fx.color, "atlas", f->atlas);
		     set_tex(f->fx.color, "palette", f->palette_tex);
		     set_tex(f->fx.color, "scores", scores);
		     set_tex(f->fx.color, "stats", gs_texrender_get_texture(f->tr_stats[next]));
		     set_vec2(f->fx.color, "cell_count", cells_x, cells_y);
		     set_flag(f->fx.color, "keep_transparency", s.keep_transparency);
		     set_float(f->fx.color, "ink_lightness", static_cast<float>(s.ink_lightness));
		     set_flag(f->fx.color, "ink_cell", s.ink_color == InkColor::Cell);
		     set_vec2(f->fx.color, "ink_ab", static_cast<float>(s.ink_a), static_cast<float>(s.ink_b));
		     set_float(f->fx.color, "saturation", static_cast<float>(s.saturation));
		     set_flag(f->fx.color, "background_tint", s.background_tint);
		     set_tex(f->fx.color, "prev_colors", gs_texrender_get_texture(f->tr_color[f->cur]));
		     set_tex(f->fx.color, "frame_state", frame_state);
		     set_flag(f->fx.color, "history", f->history);
		     set_float(f->fx.color, "settle", static_cast<float>(s.settle / 100.0));
	     });

	if (ok && f->mode == Mode::Console && f->capture_requested.exchange(false))
		start_capture(f, grid);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_srgb);

	// A frame that failed part way leaves the pairs out of step: start over.
	f->history = ok;
	if (!ok)
		return;
	f->cur = next;
	f->frame++;

	if (f->dump_requested.exchange(false))
		dump_grid(f, grid, s, w, h);
	record_frame(f, grid);

	// Draw into OBS's target. OBS turns linear sRGB off for filters that
	// aren't OBS_SOURCE_SRGB, so the shader writes sRGB-encoded values; the
	// linear branch only follows OBS's convention should that state be on.
	const bool linear_srgb = gs_get_linear_srgb();
	gs_enable_framebuffer_srgb(linear_srgb);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_effect_t *fx = f->fx.draw;
	set_tex(fx, "atlas", f->atlas);
	set_tex(fx, "scores", scores);
	set_tex(fx, "colors", gs_texrender_get_texture(f->tr_color[f->cur]));
	set_vec2(fx, "source_size", static_cast<float>(w), static_cast<float>(h));
	set_vec2(fx, "grid_offset", static_cast<float>(grid.offset_x), static_cast<float>(grid.offset_y));
	set_vec2(fx, "grid_size", static_cast<float>(grid.width_px()), static_cast<float>(grid.height_px()));
	set_int(fx, "cell_scale", grid.scale);
	set_flag(fx, "edges_transparent", s.edges_transparent);
	set_flag(fx, "output_linear", linear_srgb);
	set_float(fx, "poster_levels", f->mode == Mode::Poster ? static_cast<float>(s.poster_levels) : 0.0f);
	draw_technique(fx, "Draw", w, h);

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

// ---------------------------------------------------------------------------
// Capture palette (Console): downscale the samples on the GPU, read them back
// once, and run k-means on a worker thread, which writes the colors into the
// settings and sets Palette (and the Look) to Custom.

void capture_worker(obs_weak_source_t *weak, std::vector<Lab> points)
{
	const KMeansResult r = kmeans_palette(points);
	obs_source_t *source = obs_weak_source_get_source(weak);
	obs_weak_source_release(weak);
	if (!source)
		return; // the filter went away meanwhile

	obs_data_t *patch = obs_data_create();
	for (int k = 0; k < kPaletteSize; k++) {
		const std::string name = std::string(key::kPaletteColorPrefix) + std::to_string(k);
		obs_data_set_int(patch, name.c_str(), rgb8_to_obs(oklab_to_srgb8(r.centers[static_cast<size_t>(k)])));
	}
	obs_data_set_string(patch, key::kPalette, "custom");
	obs_data_set_string(patch, key::kLook, kLookCustom);
	obs_source_update(source, patch);
	obs_data_release(patch);
	obs_source_update_properties(source);
	obs_log(LOG_INFO, "'%s': palette captured from %zu pixels", obs_source_get_name(source), points.size());
	obs_source_release(source);
}

void start_capture(GlyphFilter *f, const Grid &grid)
{
	if (f->capture_running)
		return;
	const int cw = std::min(kCaptureWidth, grid.sample_w);
	const int ch =
		std::max(1, static_cast<int>(std::lround(static_cast<double>(grid.sample_h) * cw / grid.sample_w)));
	const bool ok = run_pass(f->tr_capture, f->fx.capture, "Draw", cw, ch, [&] {
		set_tex(f->fx.capture, "samples", gs_texrender_get_texture(f->tr_sample));
		set_vec2(f->fx.capture, "sample_size", static_cast<float>(grid.sample_w),
			 static_cast<float>(grid.sample_h));
		set_vec2(f->fx.capture, "capture_size", static_cast<float>(cw), static_cast<float>(ch));
	});
	std::vector<float> texels;
	if (!ok || !read_texture(gs_texrender_get_texture(f->tr_capture), static_cast<uint32_t>(cw),
				 static_cast<uint32_t>(ch), texels)) {
		obs_log(LOG_WARNING, "capture palette: readback failed");
		return;
	}

	// Pixels with alpha >= 0.5 when there are any, else all of them.
	std::vector<Lab> opaque, all;
	for (size_t i = 0; i + 3 < texels.size(); i += 4) {
		const Lab c{texels[i], texels[i + 1], texels[i + 2]};
		all.push_back(c);
		if (texels[i + 3] >= 0.5f)
			opaque.push_back(c);
	}
	if (f->capture_thread.joinable())
		f->capture_thread.join();
	f->capture_running = true;
	obs_weak_source_t *weak = obs_source_get_weak_source(f->context);
	f->capture_thread = std::thread([f, weak, pts = opaque.empty() ? all : opaque]() mutable {
		capture_worker(weak, std::move(pts));
		f->capture_running = false;
	});
}

// ---------------------------------------------------------------------------
// Grid dump (debug builds): the shown glyph and colors of every cell, the
// settings and Ink's levels, for tools/reference/compare.py.

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
	std::vector<float> scores, colors, stats, levels;
	if (!read_texture(gs_texrender_get_texture(f->tr_score[f->cur]), cx, cy, scores) ||
	    !read_texture(gs_texrender_get_texture(f->tr_color[f->cur]), cx * 2, cy, colors) ||
	    !read_texture(gs_texrender_get_texture(f->tr_stats[f->cur]), cx, cy, stats) ||
	    !read_texture(gs_texrender_get_texture(f->tr_levels[f->cur]), 2, 1, levels)) {
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
	auto list = [&](const char *key_name, auto value_of, bool last = false) {
		fprintf(file, "\"%s\":[", key_name);
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
		oklab_to_srgb255(Lab{p[0], p[1], p[2]}, c);
		return "[" + num(c[0]) + "," + num(c[1]) + "," + num(c[2]) + "]";
	};
	auto rgb8 = [&](Rgb8 c) {
		return "[" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "]";
	};
	const float *text = colors.data();
	auto text_of = [&](size_t i) {
		return text + ((i / cx) * cx * 2 + (i % cx) * 2) * 4;
	};
	auto back_of = [&](size_t i) {
		return text_of(i) + 4;
	};
	auto b = [](bool v) {
		return v ? "true" : "false";
	};
	static const char *kInkNames[] = {"cell", "green", "amber", "white", "custom"};

	obs_data_t *source_settings = obs_source_get_settings(f->context);
	fprintf(file, "{\"version\":3,\"mode\":\"%s\",\"look\":\"%s\",\"charset_name\":\"%s\",\"frame\":%llu,\n",
		mode_info(f->mode).name, obs_data_get_string(source_settings, key::kLook), charset_value(s.charset),
		static_cast<unsigned long long>(f->frame));
	obs_data_release(source_settings);
	fprintf(file, "\"charset\":[");
	for (size_t i = 0; i < f->atlas_charset.size(); i++)
		fprintf(file, i ? ",%u" : "%u", static_cast<unsigned>(f->atlas_charset.chars[i]));
	fprintf(file, "],\n");
	fprintf(file,
		"\"source_width\":%u,\"source_height\":%u,\"cell_scale\":%d,\"alignment\":\"%s\",\"edges\":\"%s\","
		"\"keep_transparency\":%s,\n",
		source_w, source_h, grid.scale, s.alignment == Alignment::Center ? "center" : "top_left",
		s.edges_transparent ? "transparent" : "black", b(s.keep_transparency));
	fprintf(file, "\"cells_x\":%u,\"cells_y\":%u,\"offset_x\":%d,\"offset_y\":%d,\n", cx, cy, grid.offset_x,
		grid.offset_y);
	fprintf(file,
		"\"motion\":{\"soften\":%s,\"hold\":%d,\"settle\":%d,\"boil\":%d,\"boil_spread\":%d,"
		"\"boil_linger\":%d,\"hold_threshold\":%s,\"seed\":%u},\n",
		num(s.soften).c_str(), s.hold, s.settle, s.boil, s.boil_spread, s.boil_linger,
		num(hold_threshold(f->mode, s.hold)).c_str(), f->seed);
	fprintf(file, "\"light_jump\":%s,\"jump_share\":%s,\n", b(levels[4] > 0.5f), num(levels[5]).c_str());
	if (f->mode == Mode::Ink) {
		fprintf(file,
			"\"ink\":{\"color\":\"%s\",\"a\":%s,\"b\":%s,\"background\":\"%s\",\"lightness\":%s,"
			"\"saturation\":%s,\"gamma\":%s,\"shape\":%s,\"levels\":\"%s\",\"adapt_speed\":%s,"
			"\"black_point\":%s,\"white_point\":%s},\n",
			kInkNames[static_cast<int>(s.ink_color)], num(s.ink_a).c_str(), num(s.ink_b).c_str(),
			s.background_tint ? "tint" : "black", num(s.ink_lightness).c_str(), num(s.saturation).c_str(),
			num(s.gamma).c_str(), num(s.shape).c_str(), s.levels_auto ? "auto" : "manual",
			num(s.adapt_speed).c_str(), num(s.black_point).c_str(), num(s.white_point).c_str());
		fprintf(file, "\"levels\":[%s,%s,%s,%s],\n", num(levels[0]).c_str(), num(levels[1]).c_str(),
			num(levels[2]).c_str(), num(levels[3]).c_str());
	}
	if (f->mode == Mode::Poster)
		fprintf(file, "\"poster_levels\":%d,\n", s.poster_levels);
	if (f->mode == Mode::Console) {
		fprintf(file, "\"palette_name\":\"%s\",\"palette\":[", s.palette_custom ? "custom" : "vga");
		for (int k = 0; k < kPaletteSize; k++)
			fprintf(file, k ? ",%s" : "%s", rgb8(s.palette[static_cast<size_t>(k)]).c_str());
		fprintf(file, "],\n");
	}
	list("glyph", [&](size_t i) { return num(scores[i * 4 + 1]); });
	list("held", [&](size_t i) { return num(scores[i * 4 + 0]); });
	list("boil", [&](size_t i) { return num(scores[i * 4 + 2]); });
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

// Per-frame recording (debug builds): Record grid starts it; each frame then
// appends the held and shown glyphs, the boil frames left and optionally the
// colors of every cell, for tools/reference/run_motion.py. Format (little
// endian): per frame, a header of 8 uint32 ("GNRF", frame counter, cells_x,
// cells_y, light jump 0/1, the jump share as float bits, colors 0/1, the mean
// of the cells' raw mean lightness as float bits), then
// held uint16[n], shown uint16[n], boil uint8[n], and with colors float32[n][6]
// (text OKLab, background OKLab). The file is written as <path>.part and
// renamed to <path> after the last frame.
void record_frame(GlyphFilter *f, const Grid &grid)
{
#ifdef GLYPHNOTIC_DEBUG_TOOLS
	if (f->record_requested.exchange(false) && !f->record_file) {
		const char *path = getenv("GLYPHNOTIC_RECORD_PATH");
		const char *frames = getenv("GLYPHNOTIC_RECORD_FRAMES");
		const char *colors = getenv("GLYPHNOTIC_RECORD_COLORS");
		f->record_path = path ? path : "";
		f->record_left = frames ? atoi(frames) : 0;
		f->record_colors = colors && *colors == '1';
		const std::string part = f->record_path + ".part";
		f->record_file = (f->record_path.empty() || f->record_left <= 0) ? nullptr
										 : os_fopen(part.c_str(), "wb");
		if (!f->record_file)
			obs_log(LOG_WARNING, "record: cannot start (GLYPHNOTIC_RECORD_PATH, GLYPHNOTIC_RECORD_FRAMES)");
	}
	if (!f->record_file)
		return;

	const auto cx = static_cast<uint32_t>(grid.cells_x);
	const auto cy = static_cast<uint32_t>(grid.cells_y);
	const size_t n = static_cast<size_t>(cx) * cy;
	std::vector<float> scores, colors, state, stats;
	if (!read_texture(gs_texrender_get_texture(f->tr_score[f->cur]), cx, cy, scores) ||
	    !read_texture(gs_texrender_get_texture(f->tr_levels[f->cur]), 2, 1, state) ||
	    !read_texture(gs_texrender_get_texture(f->tr_stats[f->cur]), cx, cy, stats) ||
	    (f->record_colors && !read_texture(gs_texrender_get_texture(f->tr_color[f->cur]), cx * 2, cy, colors))) {
		obs_log(LOG_WARNING, "record: texture readback failed");
		return;
	}
	double sum_l = 0.0;
	for (size_t i = 0; i < n; i++)
		sum_l += stats[i * 4];
	const float mean_l = static_cast<float>(sum_l / static_cast<double>(n));
	uint32_t share_bits, mean_bits;
	memcpy(&share_bits, &state[5], 4);
	memcpy(&mean_bits, &mean_l, 4);
	const uint32_t header[8] = {0x46524E47u, // "GNRF"
				    static_cast<uint32_t>(f->frame),
				    cx,
				    cy,
				    state[4] > 0.5f ? 1u : 0u,
				    share_bits,
				    f->record_colors ? 1u : 0u,
				    mean_bits};
	std::vector<uint16_t> held(n), shown(n);
	std::vector<uint8_t> boil(n);
	for (size_t i = 0; i < n; i++) {
		held[i] = static_cast<uint16_t>(scores[i * 4 + 0] + 0.5f);
		shown[i] = static_cast<uint16_t>(scores[i * 4 + 1] + 0.5f);
		boil[i] = static_cast<uint8_t>(scores[i * 4 + 2] + 0.5f);
	}
	fwrite(header, sizeof(header), 1, f->record_file);
	fwrite(held.data(), sizeof(uint16_t), n, f->record_file);
	fwrite(shown.data(), sizeof(uint16_t), n, f->record_file);
	fwrite(boil.data(), 1, n, f->record_file);
	if (f->record_colors) {
		std::vector<float> lab(n * 6);
		for (size_t i = 0; i < n; i++) {
			const float *t = colors.data() + ((i / cx) * cx * 2 + (i % cx) * 2) * 4;
			for (int k = 0; k < 3; k++) {
				lab[i * 6 + static_cast<size_t>(k)] = t[k];
				lab[i * 6 + 3 + static_cast<size_t>(k)] = t[4 + k];
			}
		}
		fwrite(lab.data(), sizeof(float), lab.size(), f->record_file);
	}
	if (--f->record_left <= 0) {
		fclose(f->record_file);
		f->record_file = nullptr;
		const std::string part = f->record_path + ".part";
		os_unlink(f->record_path.c_str());
		os_rename(part.c_str(), f->record_path.c_str());
		obs_log(LOG_INFO, "recording written to %s", f->record_path.c_str());
	}
#else
	UNUSED_PARAMETER(f);
	UNUSED_PARAMETER(grid);
#endif
}

void free_type_data(void *type_data)
{
	delete static_cast<TypeData *>(type_data);
}

} // namespace

void register_glyph_filter(Mode mode, const Font *font)
{
	obs_source_info info = {};
	info.id = mode_info(mode).id;
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
	info.video_tick = filter_tick;
	info.video_render = filter_video_render;
	info.video_get_color_space = filter_get_color_space;
	info.type_data = new TypeData{mode, font};
	info.free_type_data = free_type_data;
	obs_register_source(&info);
}

} // namespace glyphnotic
