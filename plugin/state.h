#pragma once
// The plugin's state text, shared by projects (effGetChunk / effSetChunk) and preset files.
//
// "polyforce 4": key=value lines of REAL values (Hz, seconds, voice counts, option index) for
// every sound parameter, plus the tables and the tuning by key (and, in a project, the preset
// it came from). Survives parameters being added or reordered AND ranges changing (a 0..1
// value would silently move: unison 4 of 1..16 reads back as 2 of 1..8). Version 1 stored 0..1
// values; version 2 chose a built-in table by index (o1_wave, now the oscillator's wave mode);
// version 3 had no per-oscillator routes.
#include "surface.h"

#include <string>

namespace pf {

constexpr int kStateVersion = 4;

std::string saveState(const Surface& s, bool asPreset);
// A preset starts from the defaults (what it doesn't say is the default); a project's state
// only overrides what it lists. False if it isn't PolyForce state.
bool loadState(Surface& s, const std::string& text, bool asPreset);

} // namespace pf
