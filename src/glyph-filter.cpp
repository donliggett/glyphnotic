// Glyphnotic: the shared filter core.
//
// Phase 0: the filter is a pass-through. It exists so the module, its
// registration and its install location can be tested in OBS before any GPU
// work lands (Phase 1).

#include "glyph-filter.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <new>

namespace glyphnotic {

namespace {

struct ModeInfo {
	const char *id;       // OBS source id; saved in scene files, never change it
	const char *name_key; // locale key for the display name
};

const ModeInfo kModes[] = {
	{"glyphnotic_ink", "Glyphnotic.Ink"},
	{"glyphnotic_mosaic", "Glyphnotic.Mosaic"},
	{"glyphnotic_poster", "Glyphnotic.Poster"},
	{"glyphnotic_console", "Glyphnotic.Console"},
};

struct GlyphFilter {
	obs_source_t *context = nullptr;
	Mode mode = Mode::Ink;
	const Font *font = nullptr;
};

// type_data carries the mode, so one implementation serves all four registrations.
struct TypeData {
	Mode mode;
	const Font *font;
};

const char *filter_get_name(void *type_data)
{
	const auto *td = static_cast<const TypeData *>(type_data);
	return obs_module_text(kModes[static_cast<int>(td->mode)].name_key);
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	const auto *td = static_cast<const TypeData *>(obs_source_get_type_data(source));
	auto *filter = new (std::nothrow) GlyphFilter();
	if (!filter)
		return nullptr;
	filter->context = source;
	filter->mode = td->mode;
	filter->font = td->font;
	return filter;
}

void filter_destroy(void *data)
{
	delete static_cast<GlyphFilter *>(data);
}

void filter_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	auto *filter = static_cast<GlyphFilter *>(data);
	obs_source_skip_video_filter(filter->context);
}

obs_properties_t *filter_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_properties_create();
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
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = filter_get_name;
	info.create = filter_create;
	info.destroy = filter_destroy;
	info.video_render = filter_video_render;
	info.get_properties = filter_get_properties;
	info.type_data = new TypeData{mode, font};
	info.free_type_data = free_type_data;
	obs_register_source(&info);
}

} // namespace glyphnotic
