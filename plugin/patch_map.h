#pragma once
// MPC's 0..1 parameter values <-> real values, display text, and the engine Patch.
// Ranges, curves, names and options come from surface/surface.py via build/param_ids.h.
#include "param_ids.h"
#include "../dsp/synth.h"

#include <string>

namespace pf {

float paramValue(int id, float norm);          // real value (Hz, seconds, semitones, option index...)
float paramNorm(int id, float value);          // inverse, for state text, tests and the bench
std::string paramDisplay(int id, float norm);  // what the knob's value label shows
Patch patchFromParams(const float* norm);      // norm[P_COUNT]; tables are set by the caller

} // namespace pf
