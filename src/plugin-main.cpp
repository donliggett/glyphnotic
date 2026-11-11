// Glyphnotic: module load. Loads the bundled font and registers the filters.

#include <obs-module.h>
#include <plugin-support.h>

#include "font.hpp"
#include "glyph-filter.hpp"

#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {
glyphnotic::Font g_font;
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Glyphnotic.Description");
}

bool obs_module_load(void)
{
	using namespace glyphnotic;

	char *font_path = obs_module_file("fonts/Uni2-Fixed16.psf");
	if (!font_path) {
		obs_log(LOG_ERROR, "font file fonts/Uni2-Fixed16.psf not found in the module data folder");
		return false;
	}
	std::string error;
	const bool loaded = g_font.load_file(font_path, &error);
	bfree(font_path);
	if (!loaded) {
		obs_log(LOG_ERROR, "failed to load font: %s", error.c_str());
		return false;
	}

	obs_log(LOG_INFO, "font loaded: %zu glyphs; sets Blocks %zu, Lines %zu, ASCII %zu, Every glyph %zu",
		g_font.glyph_count(), build_charset(g_font, CharsetId::Blocks).size(),
		build_charset(g_font, CharsetId::Lines).size(), build_charset(g_font, CharsetId::Ascii).size(),
		build_charset(g_font, CharsetId::EveryGlyph).size());

	// Phase 0 registers only Ink, as a pass-through. Mosaic, Poster and Console follow in Phases 1-2.
	register_glyph_filter(Mode::Ink, &g_font);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
