// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Don Liggett

// Glyphnotic: per-mode settings, properties, defaults and looks (the libobs
// side of settings.hpp and presets.hpp).

#pragma once

#include "settings.hpp"

#include <obs-module.h>

#include <string>

namespace glyphnotic {

struct ModeInfo {
	const char *id;       // OBS source id; saved in scene files, never change it
	const char *name_key; // locale key for the display name
	const char *name;     // short name for logs and dumps
};

const ModeInfo &mode_info(Mode mode);

// type_data of each registration.
struct TypeData {
	Mode mode;
	const Font *font;
};

CharsetId charset_from_value(const char *value);
const char *charset_value(CharsetId id);

void mode_defaults(Mode mode, obs_data_t *settings);
Settings read_settings(Mode mode, obs_data_t *settings);

// Keeps the Look and the settings it controls consistent, for settings that
// didn't come through the properties window (scene files, scripts, the
// harness). `last_look` is the look of the previous update (at create: ""
// if the settings name a look, else the default look): a new look fills in
// its values; an unchanged look whose values no longer match becomes Custom.
// Returns true if it changed the settings.
bool reconcile_look(Mode mode, obs_data_t *settings, const std::string &last_look);

// Property names that aren't settings.
constexpr const char *kPropCustomFallback = "custom_fallback_note";
constexpr const char *kPropCapturePalette = "capture_palette";
constexpr const char *kPropPaletteNote = "palette_note";
constexpr const char *kPropDumpGrid = "dump_grid";
constexpr const char *kPropRecordGrid = "record_grid";

// Adds the Look, Characters, Grid and mode groups. capture_clicked is the
// Capture palette button's callback (Console only), called with `data`.
void add_properties(obs_properties_t *props, TypeData *td, obs_property_clicked_t capture_clicked, void *data);

} // namespace glyphnotic
