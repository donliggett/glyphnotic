// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Don Liggett

// Glyphnotic: the shared filter core, registered once per drawing mode.

#pragma once

#include "settings.hpp"

namespace glyphnotic {

class Font;

// Registers the filter for one mode. The font must outlive the module.
void register_glyph_filter(Mode mode, const Font *font);

} // namespace glyphnotic
