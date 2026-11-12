// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Don Liggett

// Glyphnotic: the looks (named presets). No libobs dependency.
//
// A look sets every setting it controls: the character set, the motion
// settings and the mode's own settings (docs/SPEC.md, Looks). Settings a
// look doesn't list keep the filter's defaults. Grid settings, Custom
// characters, the custom ink color and the custom palette colors aren't
// part of any look.

#pragma once

#include "settings.hpp"

#include <string>
#include <vector>

namespace glyphnotic {

struct LookValue {
	enum class Kind { String, Int, Double };
	const char *key;
	Kind kind;
	std::string s;
	long long i = 0;
	double d = 0.0;
};

struct Look {
	const char *id;        // saved in scene files; never change it
	const char *label_key; // locale key
	std::vector<LookValue> values;
};

// The looks of one filter, in menu order. The first is the filter's default.
const std::vector<Look> &looks_for(Mode mode);

// The look with this id in this mode, or nullptr.
const Look *find_look(Mode mode, const char *id);

// True if `key` is a setting the looks of this mode control.
bool look_controls(Mode mode, const char *key);

} // namespace glyphnotic
